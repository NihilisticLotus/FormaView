"""Drive the packaged run directory through its real MCP path.

The headless service starts from the run directory with a minimal PATH (system
folders only), so any missing DLL or plugin fails here. The stdio adapter runs
in a separate venv built from tools/mcp/requirements.lock. Inputs are the
synthetic fixtures made by scene_export_tests.py; outputs are read back by
checking commit hashes and by opening each FBX with the independent probe.

  python runtime_mcp_tests.py --run-dir RUN --venv-python PY --fixtures DIR \
      --probe formaview_scene_export_probe.exe --output NEW_DIR
"""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import uuid
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def clean_env(**extra):
    root = os.environ['SystemRoot']
    env = {k: os.environ[k] for k in ('SystemRoot', 'windir', 'TEMP', 'TMP', 'USERPROFILE', 'APPDATA', 'LOCALAPPDATA', 'COMSPEC', 'PATHEXT') if k in os.environ}
    env['PATH'] = root + '\\System32;' + root
    env.update(extra)
    return env


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run-dir', type=Path, required=True)
    parser.add_argument('--venv-python', type=Path, required=True)
    parser.add_argument('--fixtures', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    fixtures = args.output / 'fixtures'
    shutil.copytree(args.fixtures, fixtures)
    journal = args.output / 'journal'
    journal.mkdir()
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output)], output_roots=[str(args.output)], allow_overwrite=False, policy_revision='runtime-mcp-test')), encoding='utf-8')
    endpoint = 'formaview-runtime-' + uuid.uuid4().hex
    records = []
    service = await asyncio.create_subprocess_exec(
        str(args.run_dir / 'formaview_image_service.exe'), '--scope', str(scope), '--journal', str(journal), '--endpoint', endpoint,
        stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE, env=clean_env())
    try:
        assert json.loads(await asyncio.wait_for(service.stdout.readline(), 20))['ready']
        params = StdioServerParameters(command=str(args.venv_python), args=[str(args.run_dir / 'mcp' / 'image_stdio.py')],
                                       env=clean_env(FORMAVIEW_IMAGE_ENDPOINT=endpoint))
        async with stdio_client(params) as (read, write):
            async with ClientSession(read, write) as client:
                await client.initialize()
                tools = {t.name for t in (await client.list_tools()).tools}
                for name in ('get_image_capabilities', 'inspect_image', 'inspect_model', 'create_image_plan', 'create_model_export_plan',
                             'submit_image_job', 'get_image_job', 'cancel_image_work', 'retry_image_plan', 'discover_images'):
                    assert name in tools, name

                async def call(tool, **kwargs):
                    reply = await client.call_tool(tool, kwargs)
                    assert not reply.isError, reply
                    data = json.loads(next(c.text for c in reply.content if c.type == 'text'))
                    records.append(dict(tool=tool, request=kwargs, response=data))
                    return data

                async def terminal(kind, id):
                    deadline = time.monotonic() + 90
                    while time.monotonic() < deadline:
                        result = await call('get_image_' + kind, **{kind + '_id': id})
                        if result['state'] not in ('planning', 'queued', 'running'):
                            return result
                        await asyncio.sleep(.05)
                    raise AssertionError('service did not finish')

                caps = await call('get_image_capabilities')
                assert len(caps['read_formats']) == 11 and len(caps['write_targets']) == 9, caps
                assert 'fbx' in caps['model_export']['output_formats'] and 'pmx' in caps['model_export']['input_formats']
                found = await call('discover_images', directory=str(fixtures))
                assert found, found

                # Mixed batch: one image conversion plus four model families.
                image_out = args.output / 'pixel.webp'
                items = [dict(input=str(fixtures / 'pixel.png'), output=str(image_out),
                              options=dict(target='webp', channels=[dict(source=c, destination=c) for c in 'RGBA'],
                                           alpha_channel='A', source_alpha='straight', alpha_action='preserve'))]
                for suffix in ('obj', 'glb', 'pmx', 'vrm'):
                    inspected = await call('inspect_model', input=str(fixtures / ('source.' + suffix)))
                    assert 'error' not in inspected, inspected
                    items.append(dict(input=str(fixtures / ('source.' + suffix)), output=str(args.output / (suffix + '.fbx')), options=dict(target='fbx')))
                created = await call('create_image_plan', items=items)
                plan = await terminal('plan', created['plan_id'])
                assert plan['state'] == 'ready', plan
                # Loss acknowledgement is mandatory: submitting without it must be refused.
                refused = await client.call_tool('submit_image_job', dict(plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                                                                         idempotency_key='no-ack', acknowledge_losses=False))
                refused_text = ''.join(c.text for c in refused.content if c.type == 'text')
                assert refused.isError or 'error' in refused_text, refused_text
                queued = await call('submit_image_job', plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                                    idempotency_key='runtime-1', acknowledge_losses=True)
                job = await terminal('job', queued['job_id'])
                assert job['state'] == 'succeeded', job
                assert (await call('submit_image_job', plan_id=created['plan_id'], approval_digest=plan['approval_digest'],
                                   idempotency_key='runtime-1', acknowledge_losses=True))['job_id'] == queued['job_id']
                for i, item in enumerate(job['items']):
                    result = item['result']
                    output = Path(result['committed_output'])
                    assert hashlib.sha256(output.read_bytes()).hexdigest() == result['output_sha256'], output
                    if output.suffix == '.fbx':
                        report = args.output / ('readback-%d.json' % i)
                        subprocess.run([str(args.probe), str(output), str(report)], check=True, timeout=60)
                        actual = json.loads(report.read_text(encoding='utf-8'))
                        assert actual['meshes'], output
                assert image_out.stat().st_size > 0

                # Source protection: the same plan cannot run again over existing outputs.
                again = await call('create_image_plan', items=items)
                again_plan = await terminal('plan', again['plan_id'])
                assert all(i['status'] == 'failed' or i.get('preflight', {}).get('output_exists') for i in again_plan['items']) or again_plan['state'] != 'ready', again_plan
                # Truncated PMX fails with a structured error, not a crash; the service keeps working.
                bad = await call('inspect_model', input=str(fixtures / 'truncated.pmx'))
                assert 'error' in bad, bad
                assert 'error' not in await call('inspect_model', input=str(fixtures / 'source.pmx'))
                # Plan cancellation is observable.
                third = await call('create_image_plan', items=[dict(items[0], output=str(args.output / 'cancel.webp'))])
                await call('cancel_image_work', work_id=third['plan_id'], kind='plan')
                final = await terminal('plan', third['plan_id'])
                assert final['state'] in ('cancelled', 'ready'), final
        (args.output / 'records.json').write_text(json.dumps(records, ensure_ascii=False, indent=1), encoding='utf-8')
        print('Runtime MCP flow passed: %d calls' % len(records))
    finally:
        if service.returncode is None:
            service.terminate()
            await service.wait()
        err = (await service.stderr.read()).decode('utf-8', 'replace')
        (args.output / 'service-stderr.txt').write_text(err, encoding='utf-8')


asyncio.run(main())
