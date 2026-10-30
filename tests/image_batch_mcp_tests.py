"""Actual MCP client, native named-pipe service, disk journals and image outputs."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time
import uuid
import zlib
import win32api
import win32con
import win32security
import win32process

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def png(path, size):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    row = bytes((32, 96, 160, 224)) * size
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress((b'\0' + row) * size)) + chunk(b'IEND', b''))


async def main():
    parser = argparse.ArgumentParser()
    for field in ('native', 'service', 'server', 'fixture', 'output', 'decoder_python'):
        parser.add_argument('--' + field.replace('_', '-'), type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    inputs = args.output / 'input'; outputs = args.output / 'output'; journal = args.output / 'journal'
    for directory in (inputs, outputs, journal): directory.mkdir()
    sources = [inputs / '输入一.png', inputs / '输入二.png']
    for source in sources: shutil.copyfile(args.fixture, source)
    large = inputs / 'large.png'; png(large, 1024)
    policy = args.output / 'scope.json'
    policy.write_text(json.dumps({'input_roots': [str(inputs.resolve())],
        'output_roots': [str(outputs.resolve()), str(inputs.resolve())],
        'allow_overwrite': True, 'policy_revision': 'batch-mcp-test-1'}), encoding='utf-8')
    endpoint = 'formaview-test-' + uuid.uuid4().hex
    env = os.environ.copy(); env.update(FORMAVIEW_IMAGE_CLI=str(args.native.resolve()), FORMAVIEW_IMAGE_ENDPOINT=endpoint)
    parameters = StdioServerParameters(command=sys.executable, args=[str(args.server.resolve())], env=env)
    records = []; latencies = []; processes = []; service_logs = []; memory = {}

    def sample_memory():
        if process.returncode is not None: return
        handle = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, False, process.pid)
        try:
            current = win32process.GetProcessMemoryInfo(handle)
            memory[process.pid] = {'peak_working_set_bytes': current['PeakWorkingSetSize'],
                'last_working_set_bytes': current['WorkingSetSize'], 'peak_pagefile_bytes': current['PeakPagefileUsage']}
        finally: handle.Close()

    async def start_service():
        log = open(args.output / f'service-{len(processes)}.stderr.log', 'wb'); service_logs.append(log)
        process = await asyncio.create_subprocess_exec(str(args.service.resolve()), '--scope', str(policy.resolve()),
            '--journal', str(journal.resolve()), '--endpoint', endpoint, stdout=asyncio.subprocess.PIPE, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        processes.append(process)
        ready = await asyncio.wait_for(process.stdout.readline(), 15)
        assert ready and json.loads(ready)['ready'], (ready, process.returncode)
        return process

    def item(source, name, conflict='skip'):
        return {'input': str(source.resolve()), 'output': str((outputs / name).resolve()), 'conflict': conflict,
            'options': {'target': 'tiff', 'channels': [{'source': c, 'destination': c} for c in 'RGBA'],
                        'source_alpha': 'straight', 'alpha_channel': 'A'}}

    async def call(session, tool, arguments):
        start = time.perf_counter()
        result = await session.call_tool(tool, arguments)
        elapsed = (time.perf_counter() - start) * 1000
        sample_memory()
        assert not result.isError, result
        payload = result.structuredContent
        if payload is None: payload = json.loads(next(c.text for c in result.content if c.type == 'text'))
        records.append({'tool': tool, 'arguments': arguments, 'result': payload, 'milliseconds': elapsed})
        if tool in ('get_image_job', 'get_image_plan', 'cancel_image_work', 'list_image_jobs'): latencies.append(elapsed)
        return payload

    async def terminal(session, kind, key, limit=100):
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            result = await call(session, 'get_image_' + kind, {kind + '_id': key, 'limit': limit})
            assert 'error' not in result, result
            if result['state'] not in ('planning', 'queued', 'running'): return result
            await asyncio.sleep(0.02)
        raise AssertionError('Batch did not reach terminal state')

    async def plan(session, items):
        response = await call(session, 'create_image_plan', {'items': items})
        assert 'plan_id' in response, response
        result = await terminal(session, 'plan', response['plan_id'])
        assert result['state'] == 'ready', result
        return result

    def submission(plan, key):
        return {'plan_id': plan['id'], 'approval_digest': plan['approval_digest'],
                'idempotency_key': key, 'acknowledge_losses': True}

    process = await start_service()
    security = win32security.GetFileSecurity(str(journal), win32security.DACL_SECURITY_INFORMATION)
    assert security.GetSecurityDescriptorControl()[0] & win32security.SE_DACL_PROTECTED
    token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
    current_sid = win32security.GetTokenInformation(token, win32security.TokenUser)[0]
    acl = security.GetSecurityDescriptorDacl()
    assert acl.GetAceCount() == 1 and acl.GetAce(0)[2] == current_sid, 'Journal ACL grants another identity'
    try:
        async with stdio_client(parameters) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                initialized = await session.initialize()
                tools = await session.list_tools()
                assert {t.name for t in tools.tools} == {'get_image_capabilities', 'inspect_image', 'discover_images', 'get_image_discovery', 'create_image_plan', 'get_image_plan',
                    'submit_image_job', 'get_image_job', 'list_image_jobs', 'cancel_image_work', 'retry_image_plan',
                    'inspect_model', 'create_model_export_plan'}
                capabilities = await call(session, 'get_image_capabilities', {})
                assert len(capabilities['read_formats']) == 11 and len(capabilities['write_targets']) == 9
                discovery = await call(session, 'discover_images', {'directory': str(inputs.resolve())})
                assert discovery['total'] == 3, discovery
                selected = await call(session, 'get_image_discovery', {'discovery_id': discovery['discovery_id'], 'limit': 1})
                assert selected['total'] == 3 and selected['next_offset'] == 1 and selected['membership_only']
                nested = inputs / 'nested'; nested.mkdir(); shutil.copyfile(args.fixture, nested / 'nested.png')
                (inputs / 'ignored.txt').write_text('not an image', encoding='utf-8')
                recursive = await call(session, 'discover_images', {'directory': str(inputs.resolve()), 'recursive': True})
                assert recursive['total'] == 4
                stable = await call(session, 'get_image_discovery', {'discovery_id': discovery['discovery_id']})
                assert stable['total'] == 3, 'Discovery membership changed after creation'
                outside = await call(session, 'discover_images', {'directory': str(args.output.resolve())})
                assert outside['error']['code'] == 'out_of_scope'
                preview_source = inputs / 'preview-4096.png'; png(preview_source, 4096)
                info = await call(session, 'inspect_image', {'path': str(sources[0].resolve())})
                assert len(info['selected_channels']) == 4
                matrix_items = []
                for target in ('png', 'jpeg', 'bmp', 'tga', 'tiff', 'webp', 'dds', 'hdr', 'exr'):
                    request = item(sources[0], 'converted.' + target)
                    options = request['options']; options['target'] = target
                    if target in ('jpeg', 'bmp', 'hdr'):
                        options['channels'] = [{'source': c, 'destination': c} for c in 'RGB']
                        options['alpha_action'] = 'drop'
                    if target in ('jpeg', 'hdr'): options['allow_lossy'] = True
                    if target in ('hdr', 'exr'): options.update(output_scalar='float32', quantization='clamp_round')
                    matrix_items.append(request)
                first = await plan(session, matrix_items)
                assert not list(outputs.iterdir()), 'Preflight wrote a destination'
                page = await call(session, 'get_image_plan', {'plan_id': first['id'], 'limit': 1})
                assert page['total'] == 9 and page['next_offset'] == 1 and len(page['items']) == 1
                submitted_args = submission(first, 'stable-key')
                submitted = await call(session, 'submit_image_job', submitted_args)
                job_id = submitted['job_id']
                repeated = await call(session, 'submit_image_job', submitted_args)
                assert repeated['job_id'] == job_id and repeated['reused']
                conflicting = await call(session, 'submit_image_job', dict(submitted_args, acknowledge_losses=False))
                assert conflicting['error']['code'] == 'conflict'
                done = await terminal(session, 'job', job_id)
                assert done['state'] == 'succeeded' and all(i['status'] == 'succeeded' for i in done['items'])
                for entry in done['items']:
                    result = entry['result']; target = Path(result['committed_output'])
                    assert hashlib.sha256(target.read_bytes()).hexdigest() == result['output_sha256']
                # An independent Pillow consumer validates pixels, not merely presence/hash.
                decode = subprocess.run([str(args.decoder_python), '-c',
                    'from PIL import Image; import sys; a=Image.open(sys.argv[1]).convert("RGBA"); b=Image.open(sys.argv[2]).convert("RGBA"); assert a.size==b.size and a.tobytes()==b.tobytes(); print("pixel-exact")',
                    str(sources[0]), str(outputs / 'converted.tiff')], capture_output=True, text=True)
                assert decode.returncode == 0, decode.stderr
                read_formats = []
                pair_sources = []
                for request in matrix_items:
                    original = Path(request['output']); copied = inputs / original.name
                    shutil.copyfile(original, copied)
                    inspected = await call(session, 'inspect_image', {'path': str(copied.resolve())})
                    assert 'error' not in inspected and inspected['total'] >= 1, inspected
                    assert inspected['input']['sha256'] == hashlib.sha256(copied.read_bytes()).hexdigest()
                    read_formats.append(request['options']['target'])
                    pair_sources.append((copied, inspected))
                extras = subprocess.run([str(args.decoder_python), '-c',
                    'from PIL import Image; from pathlib import Path; import sys; a=Image.open(sys.argv[1]); p=Path(sys.argv[2]); a.save(p/"extra.gif"); a.save(p/"extra.ico",sizes=[(8,8)])',
                    str(sources[0]), str(inputs)], capture_output=True, text=True)
                assert extras.returncode == 0, extras.stderr
                for container in ('gif', 'ico'):
                    inspected = await call(session, 'inspect_image', {'path': str((inputs / ('extra.' + container)).resolve())})
                    assert 'error' not in inspected and inspected['total'] >= 1, inspected
                    read_formats.append(container)
                    pair_sources.append((inputs / ('extra.' + container), inspected))
                pair_directory = inputs / 'pairs'; pair_directory.mkdir()
                pair_requests = []; pair_manifest = []
                for source_path, source_info in pair_sources:
                    channel_names = [c['name'] for c in source_info['selected_channels']]
                    for target in capabilities['write_targets']:
                        drop_alpha = target in ('jpeg', 'bmp', 'hdr')
                        channel_map = [c for c in 'RGBA' if c in channel_names and not (drop_alpha and c == 'A')]
                        options = {'target': target, 'channels': [{'source': c, 'destination': c} for c in channel_map],
                            # These generated fixtures retain straight source RGB/alpha.
                            # EXR has no alpha-mode tag; declare the known fixture semantics.
                            'source_alpha': 'straight' if 'A' in channel_names else 'absent',
                            'alpha_action': 'drop' if drop_alpha else 'preserve',
                            'output_scalar': 'float32' if target in ('hdr', 'exr') else 'uint8',
                            'quantization': 'clamp_round', 'allow_lossy': target in ('jpeg', 'hdr'), 'metadata': 'drop'}
                        if 'A' in channel_names: options['alpha_channel'] = 'A'
                        destination = pair_directory / (source_path.suffix[1:] + '-to-' + target + '.' + target)
                        pair_requests.append({'input': str(source_path.resolve()), 'output': str(destination.resolve()),
                            'conflict': 'skip', 'options': options})
                        pair_manifest.append({'input': str(source_path.resolve()), 'output': str(destination.resolve()),
                            'target': target, 'drop_alpha': drop_alpha})
                assert len(pair_requests) == 99
                for offset in range(0, len(pair_requests), 64):
                    paired = await plan(session, pair_requests[offset:offset + 64])
                    paired_submit = await call(session, 'submit_image_job', submission(paired, f'pairs-{offset}'))
                    paired_done = await terminal(session, 'job', paired_submit['job_id'])
                    assert paired_done['state'] == 'succeeded', paired_done
                    for result_item in paired_done['items']:
                        result = result_item['result']
                        assert hashlib.sha256(Path(result['committed_output']).read_bytes()).hexdigest() == result['output_sha256']
                manifest_path = args.output / 'pair-manifest.json'
                manifest_path.write_text(json.dumps(pair_manifest, ensure_ascii=False, indent=2), encoding='utf-8')
                independent_pairs = subprocess.run([str(args.decoder_python),
                    str(Path(__file__).with_name('image_pairs_independent_tests.py')), str(manifest_path),
                    str(args.output / 'pair-independent.json')], capture_output=True, text=True)
                assert independent_pairs.returncode == 0, independent_pairs.stderr
                partial = await plan(session, [item(sources[0], 'partial-good.tiff'), item(sources[1], 'partial-changed.tiff')])
                with sources[1].open('ab') as stream: stream.write(b'changed-after-preflight')
                partial_submit = await call(session, 'submit_image_job', submission(partial, 'partial-key'))
                partial_done = await terminal(session, 'job', partial_submit['job_id'])
                assert partial_done['state'] == 'partially_succeeded', partial_done
                assert partial_done['items'][1]['error']['code'] == 'input_changed'
                shutil.copyfile(args.fixture, sources[1])
                retry = await call(session, 'retry_image_plan', {'job_id': partial_submit['job_id']})
                retry_ready = await terminal(session, 'plan', retry['plan_id'])
                assert retry_ready['state'] == 'ready' and retry_ready['total'] == 1
                retry_submit = await call(session, 'submit_image_job', submission(retry_ready, 'retry-key'))
                retry_done = await terminal(session, 'job', retry_submit['job_id'])
                assert retry_done['state'] == 'succeeded'
                # Another item's source and its hard link may never be overwritten.
                linked = inputs / 'linked.png'; os.link(sources[1], linked)
                bad = item(sources[0], 'unused.tiff', 'overwrite'); bad['output'] = str(linked.resolve())
                unsafe = await call(session, 'create_image_plan', {'items': [bad, item(sources[1], 'safe.tiff')]})
                unsafe_done = await terminal(session, 'plan', unsafe['plan_id'])
                assert unsafe_done['items'][0]['error']['code'] == 'conflict', unsafe_done
                assert hashlib.sha256(linked.read_bytes()).hexdigest() == hashlib.sha256(args.fixture.read_bytes()).hexdigest()
                cancel_plan = await plan(session, [item(large, f'cancel-{i}.tiff') for i in range(16)])
                cancel_submit = await call(session, 'submit_image_job', submission(cancel_plan, 'cancel-key'))
                foreground = asyncio.create_task(call(session, 'inspect_image', {'path': str(preview_source.resolve())}))
                for _ in range(10):
                    await call(session, 'get_image_job', {'job_id': cancel_submit['job_id'], 'limit': 1})
                cancel = await call(session, 'cancel_image_work', {'work_id': cancel_submit['job_id']})
                assert cancel['cancellation_requested']
                preview = await foreground
                assert 'error' not in preview and preview['subresources'][0]['data_window']['width'] == 4096, preview
                cancelled = await terminal(session, 'job', cancel_submit['job_id'])
                assert cancelled['state'] == 'cancelled' and any(i['status'] == 'cancelled' for i in cancelled['items'])
                assert not list(outputs.glob('.formaview-*.tmp')), 'Cooperative cancel leaked temporary file'
                crash_plan = await plan(session, [item(large, f'crash-{i}.tiff') for i in range(24)])
                crash_args = submission(crash_plan, 'crash-key')
                crash_submit = await call(session, 'submit_image_job', crash_args)
                crash_id = crash_submit['job_id']
                for attempt in range(100):
                    before_crash = await call(session, 'get_image_job', {'job_id': crash_id})
                    if any(i['status'] == 'succeeded' for i in before_crash['items']): break
                    await asyncio.sleep(0.01)
                assert before_crash['state'] == 'running', before_crash
                process.kill(); await process.wait()
                process = await start_service()
                recovered = await call(session, 'get_image_job', {'job_id': crash_id})
                assert recovered['state'] == 'interrupted', recovered
                committed = [i for i in recovered['items'] if i['status'] == 'succeeded']
                assert committed and any(i['status'] == 'interrupted' for i in recovered['items'])
                for entry in committed:
                    result = entry['result']
                    assert hashlib.sha256(Path(result['committed_output']).read_bytes()).hexdigest() == result['output_sha256']
                repeat_after_restart = await call(session, 'submit_image_job', crash_args)
                assert repeat_after_restart['job_id'] == crash_id and repeat_after_restart['reused']
                original_after_restart = await call(session, 'submit_image_job', submitted_args)
                assert original_after_restart['job_id'] == job_id
                output_count = len(list(outputs.iterdir())); await asyncio.sleep(0.2)
                assert len(list(outputs.iterdir())) == output_count, 'Recovery unexpectedly reran outputs'
                # Exercise actual native queue admission under simultaneous MCP
                # submissions; cancellation/query use a separate adapter lane.
                saturated = await asyncio.gather(*(call(session, 'create_image_plan', {'items': [
                    item(large, f'queue-{n}-{i}.tiff') for i in range(64)]}) for n in range(32)))
                admitted = [r['plan_id'] for r in saturated if 'plan_id' in r]
                rejected = [r for r in saturated if 'error' in r]
                assert admitted and rejected and all(r['error']['code'] == 'queue_full' for r in rejected), saturated
                await asyncio.gather(*(call(session, 'cancel_image_work', {'work_id': key, 'kind': 'plan'}) for key in admitted))
                for key in admitted:
                    state = await terminal(session, 'plan', key, limit=1)
                    assert state['state'] in ('ready', 'cancelled'), state
                assert len(list(outputs.iterdir())) == output_count, 'Queued preflight wrote output'
        # New MCP session reconnects to the still-live native host and finds durable jobs.
        async with stdio_client(parameters) as (reader, writer):
            async with ClientSession(reader, writer) as session:
                await session.initialize()
                listing = await call(session, 'list_image_jobs', {'limit': 1})
                assert listing['total'] >= 5 and listing['next_offset'] == 1
                found = await call(session, 'get_image_job', {'job_id': job_id})
                assert found['state'] == 'succeeded'
                discovery_after_restart = await call(session, 'get_image_discovery', {'discovery_id': discovery['discovery_id']})
                assert discovery_after_restart['total'] == 3
        sorted_latency = sorted(latencies)
        report = {'passed': True, 'calls': len(records), 'protocol': initialized.protocolVersion,
            'independent_pixels': decode.stdout.strip(), 'results': records,
            'query_cancel_ms': {'count': len(latencies), 'p95': sorted_latency[int((len(latencies)-1)*.95)],
                                'p99': sorted_latency[int((len(latencies)-1)*.99)], 'max': max(latencies)},
            'crash_temp_files': [str(p) for p in outputs.glob('.formaview-*.tmp')],
            'journal_acl': 'protected; one allow ACE for current TokenUser',
            'queue_pressure': {'submissions': 32, 'admitted': len(admitted), 'queue_full': len(rejected)},
            'service_memory_by_pid': memory,
            'foreground_inspection_during_batch': '4096x4096 RGBA; shared image admission, state/cancel remain independent',
            'actual_mcp_write_targets': [r['options']['target'] for r in matrix_items],
            'actual_mcp_read_formats': read_formats,
            'conversion_pairs': len(pair_manifest),
            'conversion_pairs_independent': 'pair-independent.json: Pillow, OpenEXR Python consumer and independent DDS/RGBE parsing',
            'separate_native_test': 'image_batch covers commit/journal gap identity+hash reconciliation and real killed-child temporary cleanup',
            'not_covered': ['GUI equivalence', 'ordinary configuration performance',
                'full resource peak accounting across all codecs', 'power-loss/filesystem-failure recovery', 'other filesystem implementations']}
        (args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: report[k] for k in ('passed', 'calls', 'protocol', 'query_cancel_ms', 'crash_temp_files')}))
    finally:
        for running in processes:
            if running.returncode is None: running.kill(); await running.wait()
        for log in service_logs: log.close()
        if not (args.output / 'report.json').exists():
            (args.output / 'failed-records.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__': asyncio.run(main())
