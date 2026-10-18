"""Real MCP/CLI regressions for approved output names and selected semantics.

Run with the pinned MCP environment; --decoder-python supplies Pillow/numpy/
tifffile for independently generated fixtures and output verification.
All generated data and reports must live in a fresh external --output directory.
"""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import uuid

sys.dont_write_bytecode = True


def fixtures(root):
    from PIL import Image, ImageCms
    import numpy as np
    import tifffile
    root.mkdir()
    for name, rgb in [('red', (255, 0, 0)), ('blue', (0, 0, 255)), ('green', (0, 255, 0))]:
        Image.new('RGB', (2, 2), rgb).save(root / (name + '.png'))
    for name in ['red', 'blue']:
        (root / name).mkdir()
        (root / name / 'asset.png').write_bytes((root / (name + '.png')).read_bytes())
    profile = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
    with tifffile.TiffWriter(root / 'mixed.tiff') as writer:
        writer.write(np.full((2, 2, 4), 128, dtype=np.uint8), photometric='rgb',
                     extrasamples='unassalpha', extratags=[(34675, 'B', len(profile), profile, False)])
        writer.write(np.full((2, 2, 4), 64, dtype=np.uint8), photometric='rgb', extrasamples='assocalpha')
    payloads = []
    import io
    for mode, size, colour, tagged in [('RGBA', 8, (120, 30, 20, 128), True), ('RGB', 4, (4, 6, 8), False)]:
        out = io.BytesIO()
        Image.new(mode, (size, size), colour).save(out, format='PNG', **({'icc_profile': profile} if tagged else {}))
        payloads.append((size, len(mode) * 8, out.getvalue()))
    offset = 6 + 16 * len(payloads)
    data = struct.pack('<HHH', 0, 1, len(payloads))
    for size, bits, payload in payloads:
        data += struct.pack('<BBBBHHII', size, size, 0, 0, 1, bits, len(payload), offset)
        offset += len(payload)
    (root / 'mixed.ico').write_bytes(data + b''.join(p[2] for p in payloads))


def verify_pixels(manifest):
    from PIL import Image
    import numpy as np
    import tifffile
    checks = json.loads(manifest.read_text())
    for check in checks:
        path = Path(check['path'])
        assert hashlib.sha256(path.read_bytes()).hexdigest() == check['sha256'], check
        if path.suffix == '.tiff':
            with tifffile.TiffFile(path) as tif:
                assert int(tif.pages[0].extrasamples[0]) == check['alpha_tag'], check
                assert np.all(tif.pages[0].asarray() == check['value']), check
                assert bool(tif.pages[0].tags.get(34675)) == check['icc'], check
        else:
            with Image.open(path) as im:
                assert tuple(im.getpixel((0, 0))) == tuple(check['pixel']), check
                if 'icc' in check:
                    assert bool(im.info.get('icc_profile')) == check['icc'], check
    print(json.dumps({'passed': True, 'independent_outputs': len(checks)}))


async def run(args):
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
    root = args.output.resolve()
    root.mkdir(exist_ok=False)
    subprocess.run([args.decoder_python, __file__, '--fixtures', str(root / 'input')], check=True)
    output = root / 'output'
    output.mkdir()
    journal = root / 'journal'
    journal.mkdir()
    scope = root / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(root / 'input')], output_roots=[str(output)],
                                    allow_overwrite=True, policy_revision='boundary-regressions-v1')))
    env = dict(os.environ, FORMAVIEW_IMAGE_ENDPOINT='formaview-boundary-' + uuid.uuid4().hex)
    records, cases, pixels = [], [], []
    def check(condition, message):
        assert condition, message
    async def case(name, work):
        try:
            await work()
            cases.append(dict(name=name, passed=True))
        except Exception as error:
            cases.append(dict(name=name, passed=False, error=str(error)))
    def item(source, target, conflict='rename', options=None):
        return dict(input=str(root / 'input' / source), output=str(target), conflict=conflict,
                    options=options or dict(target='png', channels=[dict(source=c, destination=c) for c in 'RGB']))
    def seed(path):
        path.write_bytes((root / 'input/green.png').read_bytes())
    async def cli(request):
        proc = await asyncio.create_subprocess_exec(args.native, '--scope', str(scope), stdin=asyncio.subprocess.PIPE,
                                                    stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.PIPE,
                                                    creationflags=subprocess.CREATE_NO_WINDOW)
        stdout, stderr = await proc.communicate(json.dumps(dict(version=1, **request)).encode())
        value = json.loads(stdout)
        records.append(dict(cli=request, result=value, exit=proc.returncode, stderr=stderr.decode(errors='replace')))
        return value
    with (root / 'service.log').open('wb') as log:
        process = await asyncio.create_subprocess_exec(args.service, '--scope', str(scope), '--journal', str(journal),
                    '--endpoint', env['FORMAVIEW_IMAGE_ENDPOINT'], stdout=asyncio.subprocess.PIPE, stderr=log,
                    env=env, creationflags=subprocess.CREATE_NO_WINDOW)
        try:
            check(json.loads(await asyncio.wait_for(process.stdout.readline(), 20))['ready'], 'Service not ready')
            async with stdio_client(StdioServerParameters(command=sys.executable, args=[args.server], env=env)) as (reader, writer):
                async with ClientSession(reader, writer) as session:
                    init = await session.initialize()
                    async def call(tool, arguments):
                        value = await session.call_tool(tool, arguments)
                        result = value.structuredContent or json.loads(next(c.text for c in value.content if c.type == 'text'))
                        records.append(dict(tool=tool, arguments=arguments, result=result))
                        check(not value.isError and 'error' not in result, result)
                        return result
                    async def terminal(tool, key, value):
                        for _ in range(600):
                            result = await call(tool, {key: value})
                            if result['state'] not in ('planning', 'queued', 'running'):
                                return result
                            await asyncio.sleep(.02)
                        raise TimeoutError('Terminal state deadline')
                    async def plan(items):
                        created = await call('create_image_plan', dict(items=items))
                        result = await terminal('get_image_plan', 'plan_id', created['plan_id'])
                        check(result['state'] == 'ready', result)
                        return result
                    async def submit(p):
                        submitted = await call('submit_image_job', dict(plan_id=p['id'], approval_digest=p['approval_digest'],
                            idempotency_key='boundary-' + p['id'], acknowledge_losses=True))
                        job = await terminal('get_image_job', 'job_id', submitted['job_id'])
                        repeated = await call('submit_image_job', dict(plan_id=p['id'], approval_digest=p['approval_digest'],
                            idempotency_key='boundary-' + p['id'], acknowledge_losses=True))
                        check(repeated['job_id'] == submitted['job_id'], 'Idempotency changed')
                        return job
                    async def collision(second, same=False):
                        directory = output / (second + ('-same' if same else ''))
                        directory.mkdir()
                        target = directory / 'target.png'
                        seed(target)
                        before = {p.name: p.read_bytes() for p in directory.iterdir()}
                        other = target if same else directory / 'target (1).png'
                        p = await plan([item('red/asset.png' if same else 'red.png', target),
                                        item('blue/asset.png' if same else 'blue.png', other, second)])
                        names = [x['preflight']['output'] for x in p['items']]
                        check({p.name: p.read_bytes() for p in directory.iterdir()} == before, 'Preflight wrote destination')
                        job = await submit(p)
                        successful = [x['result'] for x in job['items'] if x['status'] == 'succeeded']
                        paths = [x['committed_output'] for x in successful]
                        check(len(paths) == len(set(p.casefold() for p in paths)), 'Two successes share output')
                        check(len(successful) == 2, job)
                        check(paths == names, 'Committed names differ from preflight')
                        check(target.read_bytes() == before[target.name], 'Existing source target changed')
                        for result, colour in zip(successful, [(255, 0, 0), (0, 0, 255)]):
                            check(hashlib.sha256(Path(result['committed_output']).read_bytes()).hexdigest() == result['output_sha256'],
                                  'Success hash no longer matches disk')
                            pixels.append(dict(path=result['committed_output'], sha256=result['output_sha256'], pixel=colour))
                    for conflict in ['overwrite', 'rename', 'skip']:
                        await case('batch rename+' + conflict, lambda c=conflict: collision(c))
                    await case('same-stem rename+rename', lambda: collision('rename', True))
                    async def same_fixed(conflict):
                        directory = output / ('same-fixed-' + conflict)
                        directory.mkdir()
                        target = directory / 'asset.png'; seed(target)
                        original = target.read_bytes()
                        p = await plan([item('red/asset.png', target), item('blue/asset.png', target, conflict)])
                        selected = Path(p['items'][0]['preflight']['output'])
                        check(selected != target, 'Rename took fixed destination')
                        job = await submit(p)
                        check(job['state'] == 'succeeded', job)
                        first, second = job['items']
                        check(first['status'] == 'succeeded' and Path(first['result']['committed_output']) == selected, first)
                        check(hashlib.sha256(selected.read_bytes()).hexdigest() == first['result']['output_sha256'], first)
                        pixels.append(dict(path=str(selected), sha256=first['result']['output_sha256'], pixel=(255, 0, 0)))
                        if conflict == 'skip':
                            check(second['status'] == 'skipped' and target.read_bytes() == original, second)
                        else:
                            check(second['status'] == 'succeeded', second)
                            pixels.append(dict(path=str(target), sha256=second['result']['output_sha256'], pixel=(0, 0, 255)))
                    for conflict in ['skip', 'overwrite']:
                        await case('same-stem rename+' + conflict, lambda c=conflict: same_fixed(c))
                    async def race():
                        directory = output / 'race'
                        directory.mkdir()
                        target = directory / 'target.png'
                        seed(target)
                        p = await plan([item('red.png', target)])
                        selected = Path(p['items'][0]['preflight']['output'])
                        check(selected != target and not selected.exists(), 'Preflight did not allocate unused name')
                        seed(selected)
                        occupied = selected.read_bytes()
                        job = await submit(p)
                        check(job['state'] == 'failed' and job['items'][0]['error']['code'] in ('conflict', 'input_changed'), job)
                        check(selected.read_bytes() == occupied and len(list(directory.iterdir())) == 2, 'Race silently changed name/output')
                        retry = await call('retry_image_plan', dict(job_id=job['id']))
                        fresh = await terminal('get_image_plan', 'plan_id', retry['plan_id'])
                        check(fresh['state'] == 'ready' and fresh['items'][0]['preflight']['output'] != str(selected), fresh)
                        check((await submit(fresh))['state'] == 'succeeded', 'Explicit retry failed')
                    await case('batch external candidate claim and retry', race)
                    async def single(race=False):
                        directory = output / ('single-race' if race else 'single')
                        directory.mkdir()
                        target = directory / 'target.png'; seed(target)
                        request = item('red.png', target)
                        p = await cli(dict(operation='preflight', **request))
                        repeated = await cli(dict(operation='preflight', **request))
                        check(p['output'] == repeated['output'] and p['approval_digest'] == repeated['approval_digest'],
                              'Unchanged preflight is not deterministic')
                        selected = Path(p['output'])
                        check(selected != target and not selected.exists(), 'Single preflight hid actual name')
                        if race: seed(selected)
                        done = await cli(dict(operation='convert', approval_digest=p['approval_digest'], acknowledge_losses=True, **request))
                        if race:
                            check('error' in done and len(list(directory.iterdir())) == 2, 'Single convert silently rolled suffix')
                        else:
                            check(done.get('committed_output') == str(selected), done)
                    await case('single preflight visible name', single)
                    await case('single external candidate claim', lambda: single(True))
                    async def semantics(name, index, expected, alpha_tag=None):
                        inspected = await call('inspect_image', dict(path=str(root / 'input' / name), subresource=index))
                        check(inspected['alpha'] == expected, inspected)
                        options = dict(target='tiff' if alpha_tag else 'png', subresource=index,
                                       channels=[dict(source=c['name'], destination=c['name']) for c in inspected['selected_channels']],
                                       source_alpha=inspected['alpha'])
                        if expected != 'absent': options['alpha_channel'] = 'A'
                        target = output / (name + str(index) + '.' + options['target'])
                        p = await plan([item(name, target, 'skip', options)])
                        job = await submit(p)
                        check(job['state'] == 'succeeded', job)
                        result = job['items'][0]['result']
                        check(inspected.get('container_alpha') == 'unknown', 'Container summary missing')
                        check(bool(inspected.get('icc_profile_sha256')) == (index == 0), 'Selected profile leaked between resources')
                        if name.endswith('.ico'):
                            check('sRGB' in inspected['colour_space'] if index == 0 else inspected['colour_space'] == 'unspecified',
                                  'Selected colour-space description differs')
                        if alpha_tag:
                            pixels.append(dict(path=str(target), sha256=result['output_sha256'], alpha_tag=alpha_tag,
                                               value=128 if index == 0 else 64, icc=index == 0))
                        else:
                            pixels.append(dict(path=str(target), sha256=result['output_sha256'],
                                               pixel=(120, 30, 20, 128) if index == 0 else (4, 6, 8), icc=index == 0))
                    for i, alpha in enumerate(['straight', 'premultiplied']):
                        await case('TIFF selected alpha page ' + str(i), lambda n=i, a=alpha: semantics('mixed.tiff', n, a, 2 if n == 0 else 1))
                    for i, alpha in enumerate(['straight', 'absent']):
                        await case('ICO selected alpha entry ' + str(i), lambda n=i, a=alpha: semantics('mixed.ico', n, a))
                    protocol = init.protocolVersion
        finally:
            if process.returncode is None: process.terminate()
            await process.wait()
    manifest = root / 'pixels.json'
    manifest.write_text(json.dumps(pixels, indent=2))
    decoded = subprocess.run([args.decoder_python, __file__, '--verify-pixels', str(manifest)], capture_output=True, text=True)
    cases.append(dict(name='independent pixels/tags/profiles', passed=decoded.returncode == 0, output=decoded.stdout, error=decoded.stderr))
    passed = all(c['passed'] for c in cases)
    (root / 'report.json').write_text(json.dumps(dict(passed=passed, protocol=protocol, cases=cases, records=records), indent=2))
    print(json.dumps(dict(passed=passed, cases=cases), indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--fixtures', type=Path)
    parser.add_argument('--verify-pixels', type=Path)
    for option in ['native', 'service', 'server', 'decoder-python']: parser.add_argument('--' + option)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.fixtures: fixtures(args.fixtures)
    elif args.verify_pixels: verify_pixels(args.verify_pixels)
    else: sys.exit(asyncio.run(run(args)))
