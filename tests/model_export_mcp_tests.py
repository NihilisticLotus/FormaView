"""M08 ActorX exports through the real MCP adapter and existing native job service.

Inputs are the explicitly synthetic fixtures emitted by skeletal_tests fbx.
All logs, journal state and output files stay in the supplied external directory.
"""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main():
    parser = argparse.ArgumentParser()
    for name in ('build', 'fixtures', 'adapter', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    journal = args.output / 'journal'
    journal.mkdir()
    policy = args.output / 'scope.json'
    policy.write_text(json.dumps({'input_roots': [str(args.fixtures.resolve())],
        'output_roots': [str(args.output.resolve()), str(args.fixtures.resolve())],
        'allow_overwrite': True, 'policy_revision': 'm08-test-v1'}), encoding='utf-8')
    source_hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                     for p in args.fixtures.glob('source.*')}
    env = os.environ.copy()
    endpoint = 'formaview-m08-' + uuid.uuid4().hex
    env.update(FORMAVIEW_IMAGE_ENDPOINT=endpoint)
    records = []
    service = await asyncio.create_subprocess_exec(str(args.build / 'Release/formaview_image_service.exe'),
        '--scope', str(policy), '--journal', str(journal), '--endpoint', endpoint,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
        creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        ready = await asyncio.wait_for(service.stdout.readline(), 15)
        assert json.loads(ready)['ready'], ready
        params = StdioServerParameters(command=sys.executable, args=[str(args.adapter)], env=env)
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as client:
                await client.initialize()

                async def call(name, **kwargs):
                    result = await client.call_tool(name, kwargs)
                    assert not result.isError, result
                    data = json.loads(next(c.text for c in result.content if c.type == 'text'))
                    records.append({'tool': name, 'request': kwargs, 'response': data})
                    return data

                async def terminal(kind, identity):
                    deadline = time.monotonic() + 60
                    while time.monotonic() < deadline:
                        data = await call('get_image_' + kind, **{kind + '_id': identity})
                        if data['state'] not in ('planning', 'queued', 'running'):
                            return data
                        await asyncio.sleep(.05)
                    raise AssertionError('Nonterminal export work')

                animation = await call('inspect_model', input=str(args.fixtures / 'source.psa'))
                assert len(animation['clips']) == 2
                options = {'target': 'fbx', 'coordinates': 'actorx-z-up-x-forward-rh-cm'}
                items = []

                def item(source, name, extra=None, conflict='skip'):
                    return {'input': str(args.fixtures / source), 'output': str(args.output / name),
                            'options': dict(options, **(extra or {})), 'conflict': conflict}

                for suffix in ('psk', 'pskx'):
                    items.append(item('source.' + suffix, '纯模型-' + suffix + '.fbx'))
                    for clip in animation['clips']:
                        items.append(item('source.' + suffix, suffix + '-' + clip['name'] + '.fbx',
                            {'animation': str(args.fixtures / 'source.psa'), 'clip_id': clip['id']}))
                for clip in animation['clips']:
                    items.append(item('source.psa', 'psa-' + clip['name'] + '.fbx', {'clip_id': clip['id']}))
                items.append(item('source.psa', 'psa-multiple-stacks.fbx', {
                    'clip_ids': [c['id'] for c in animation['clips']],
                    'output_axes': 'unreal', 'output_unit': 'm', 'include_mesh': False}))
                created = await call('create_model_export_plan', items=items)
                plan = await terminal('plan', created['plan_id'])
                assert plan['state'] == 'ready', plan
                assert not any(Path(i['output']).exists() for i in items)
                submit = dict(plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                              idempotency_key='m08-first', acknowledge_losses=True)
                job = await call('submit_image_job', **submit)
                result = await terminal('job', job['job_id'])
                assert result['state'] == 'succeeded', result
                assert (await call('submit_image_job', **submit))['job_id'] == job['job_id']
                assert len(result['items']) == 9
                for index, entry in enumerate(result['items']):
                    output = Path(entry['result']['committed_output'])
                    assert hashlib.sha256(output.read_bytes()).hexdigest() == entry['result']['output_sha256']
                    report = args.output / ('readback-' + str(index) + '.json')
                    proc = await asyncio.create_subprocess_exec(str(args.build / 'tests/Release/formaview_skeletal_probe.exe'),
                        str(output), '-', '-', str(report), creationflags=subprocess.CREATE_NO_WINDOW)
                    assert await proc.wait() == 0
                    back = json.loads(report.read_text())
                    multiple = 'clip_ids' in items[index]['options']
                    assert len(back['clips']) == (0 if '纯模型' in output.name else 2 if multiple else 1), back
                    assert back['meshes'] == (0 if output.name.startswith('psa-') else 1), back
                    if multiple:
                        assert entry['result']['output_coordinates']['metres_per_unit'] == 1
                        assert entry['result']['output_coordinates']['preset'] == 'unreal'
                        for actual, expected in zip(back['clips'], animation['clips']):
                            assert actual['name'] == expected['name']
                            assert abs(actual['start'] - expected['start']) < 1e-8
                            assert abs(actual['end'] - expected['end']) < 1e-8
                    elif back['clips']:
                        expected = next(c for c in animation['clips'] if c['id'] == items[index]['options']['clip_id'])
                        assert back['clips'][0]['name'] == expected['name']
                        assert abs(back['clips'][0]['end'] - expected['end']) < 1e-8

                # Existing output must be skipped, rename must reserve a new name.
                for conflict in ('skip', 'rename'):
                    repeat = dict(items[0], conflict=conflict)
                    created = await call('create_model_export_plan', items=[repeat])
                    plan = await terminal('plan', created['plan_id'])
                    assert plan['state'] == 'ready', plan
                    job = await call('submit_image_job', plan_id=created['plan_id'],
                        approval_digest=plan['approval_digest'], idempotency_key=conflict, acknowledge_losses=True)
                    done = await terminal('job', job['job_id'])
                    assert done['items'][0]['status'] == ('skipped' if conflict == 'skip' else 'succeeded'), done
                    if conflict == 'rename':
                        assert done['items'][0]['result']['committed_output'] != repeat['output']

                # Input alias and unknown clip IDs must never publish.
                bad_items = [dict(items[0], output=str(args.fixtures / 'source.psk'), conflict='overwrite'),
                             item('source.psa', 'invalid-clip.fbx', {'clip_id': 'not-a-source-clip'})]
                created = await call('create_model_export_plan', items=bad_items)
                rejected = await terminal('plan', created['plan_id'])
                assert rejected['state'] == 'failed', rejected

                # A structurally different reference is explicitly rejected.
                incompatible = args.fixtures / 'incompatible.psk'
                incompatible.write_bytes((args.fixtures / 'source.psk').read_bytes().replace(b'bone1\0', b'other\0'))
                mismatched = item('source.psa', 'mismatched.fbx', {'clip_id': animation['clips'][0]['id'], 'reference': str(incompatible)})
                created = await call('create_model_export_plan', items=[mismatched])
                assert (await terminal('plan', created['plan_id']))['state'] == 'failed'

                # A source changed after preflight cannot reuse the old approval.
                changed_source = args.fixtures / 'changed.psk'
                changed_source.write_bytes((args.fixtures / 'source.psk').read_bytes())
                changed_item = item('changed.psk', 'changed.fbx')
                created = await call('create_model_export_plan', items=[changed_item])
                plan = await terminal('plan', created['plan_id'])
                assert plan['state'] == 'ready', plan
                changed_source.write_bytes(changed_source.read_bytes().replace(b'bone1\0', b'other\0'))
                job = await call('submit_image_job', plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                    idempotency_key='changed-source', acknowledge_losses=True)
                rejected = await terminal('job', job['job_id'])
                assert rejected['state'] == 'failed' and not Path(changed_item['output']).exists(), rejected
                retry = await call('retry_image_plan', job_id=job['job_id'])
                retried = await terminal('plan', retry['plan_id'])
                assert retried['state'] == 'ready', retried

                # Auto-discovered config participates in approval and restores source
                # reference channels even for skeleton/animation-only output.
                auto_dir = args.fixtures / ('auto-config-' + uuid.uuid4().hex)
                auto_dir.mkdir()
                auto_psa = auto_dir / 'automatic.psa'
                auto_psa.write_bytes((args.fixtures / 'source.psa').read_bytes())
                auto_config = auto_psa.with_suffix('.config')
                config_text = '[RemoveTracks]\nclip0.0=trans\n'
                auto_config.write_text(config_text, encoding='utf-8')
                auto_item = dict(item('source.psa', 'automatic.fbx', {
                    'clip_id': animation['clips'][0]['id'],
                    'reference': str(args.fixtures / 'source.psk'), 'include_mesh': False}), input=str(auto_psa))
                created = await call('create_model_export_plan', items=[auto_item])
                plan = await terminal('plan', created['plan_id'])
                assert plan['state'] == 'ready', plan
                config_info = plan['items'][0]['preflight']['psa_companion']
                assert config_info['mode'] == 'automatic' and config_info['status'] == 'applied'
                assert config_info['selected_clip_rules'] == [dict(bone_index=0, bone_name='bone0', translation=True, rotation=False)], config_info
                assert config_info['input']['sha256'] == hashlib.sha256(auto_config.read_bytes()).hexdigest()
                job = await call('submit_image_job', plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                    idempotency_key='automatic-config', acknowledge_losses=True)
                assert (await terminal('job', job['job_id']))['state'] == 'succeeded'
                verification = args.output / 'automatic-readback.json'
                proc = await asyncio.create_subprocess_exec(str(args.build / 'tests/Release/formaview_skeletal_probe.exe'),
                    '--verify-actor-clip', str(auto_psa), str(args.fixtures / 'source.psk'), str(auto_config),
                    auto_item['output'], '0', str(verification), creationflags=subprocess.CREATE_NO_WINDOW)
                assert await proc.wait() == 0
                back = json.loads(verification.read_text())
                assert back['passed'] and back['config_effect'] > .1 and back['mesh_count'] == 0, back

                for change in ('changed', 'removed', 'added'):
                    auto_config.write_text(config_text, encoding='utf-8')
                    if change == 'added':
                        auto_config.unlink()
                    changing = dict(auto_item, output=str(args.output / ('config-' + change + '.fbx')))
                    created = await call('create_model_export_plan', items=[changing])
                    plan = await terminal('plan', created['plan_id'])
                    assert plan['state'] == 'ready', plan
                    if change == 'removed':
                        auto_config.unlink()
                    else:
                        auto_config.write_text('[RemoveTracks]\nclip0.0=all\n', encoding='utf-8')
                    job = await call('submit_image_job', plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                        idempotency_key='config-' + change, acknowledge_losses=True)
                    rejected = await terminal('job', job['job_id'])
                    assert rejected['state'] == 'failed' and not Path(changing['output']).exists(), rejected

                # Invalid present config cannot silently fall back to unmasked PSA.
                auto_config.write_text('[RemoveTracks]\nclip0.0=unknown\n', encoding='utf-8')
                created = await call('create_model_export_plan', items=[auto_item])
                assert (await terminal('plan', created['plan_id']))['state'] == 'failed'
                # Explicit override still has precedence over the same-name file.
                override = auto_dir / 'explicit.config'
                override.write_text(config_text, encoding='utf-8')
                explicit = dict(auto_item, options=dict(auto_item['options'], companion=str(override)))
                created = await call('create_model_export_plan', items=[explicit])
                plan = await terminal('plan', created['plan_id'])
                assert plan['state'] == 'ready' and plan['items'][0]['preflight']['psa_companion']['mode'] == 'explicit', plan

                # Cancellation is a terminal result, not merely an accepted request.
                cancel_items = [item('source.psk', f'cancel-{i}.fbx') for i in range(64)]
                created = await call('create_model_export_plan', items=cancel_items)
                await call('cancel_image_work', work_id=created['plan_id'], kind='plan')
                cancelled = await terminal('plan', created['plan_id'])
                assert cancelled['state'] == 'cancelled', cancelled
                assert not any(Path(i['output']).exists() for i in cancel_items)
                assert source_hashes == {p: hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in source_hashes}
                (args.output / 'passed.json').write_text(json.dumps({'passed': True, 'first_batch_outputs': len(items),
                    'multistack': 'independent clips with Unreal axes and metre units',
                    'auto_config': ['applied', 'source-pose-readback', 'changed', 'removed', 'added', 'invalid', 'explicit-override'],
                    'source_hashes': source_hashes, 'source_kind': 'synthetic C++ fixture'}, indent=2), encoding='utf-8')
    finally:
        (args.output / 'mcp-records.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
        if service.returncode is None:
            service.terminate()
        _, stderr = await service.communicate()
        (args.output / 'service.stderr.log').write_bytes(stderr)


if __name__ == '__main__':
    asyncio.run(main())
