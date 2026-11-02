"""FBX take directories must not claim stacks absent from the imported graph."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
for name in ('build', 'source', 'output'):
    parser.add_argument('--' + name, type=Path, required=True)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=False)
original = args.source.read_bytes()
assert b'AnimStack::Take 001' in original
policy = args.output / 'scope.json'
policy.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],
    output_roots=[str(args.output.resolve())], policy_revision='stack-count')))
records = []

def call(operation, **values):
    request = dict(version=1, operation=operation, **values)
    run = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(policy)],
        input=json.dumps(request), capture_output=True, text=True, encoding='utf-8', timeout=60)
    response = json.loads(run.stdout)
    records.append(dict(request=request, response=response, exit_code=run.returncode))
    (args.output / 'requests.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
    return response

for name in ('normal', 'mismatch'):
    source = args.output / (name + '.fbx')
    source.write_bytes(original if name == 'normal' else original.replace(b'AnimStack::Take 001', b'AnimStack::Different'))
    inspected = call('inspect', input=str(source))
    assert len(inspected['clips']) == 1, inspected
    output = args.output / (name + '-output.fbx')
    request = dict(input=str(source), output=str(output),
        options=dict(target='fbx', clip_ids=[inspected['clips'][0]['id']]))
    preflight = call('preflight', **request)
    if name == 'mismatch' and 'error' in preflight:
        assert preflight['error']['code'] == 'invalid_argument', preflight
        assert 'stack count' in preflight['error']['message'], preflight
        assert not output.exists()
        print('PASS mismatch rejected before output')
        continue
    assert 'error' not in preflight, preflight
    written = call('convert', **request, approval_digest=preflight['approval_digest'], acknowledge_losses=True)
    assert 'error' not in written, written
    probe = output.with_suffix('.json')
    subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), str(output), str(probe)], check=True)
    actual = json.loads(probe.read_text(encoding='utf-8'))
    assert name != 'mismatch', f"Mismatch silently exported: claimed={len(written['clips'])}, actual={len(actual['clips'])}"
    assert len(actual['clips']) == 1
    print('PASS normal stack retained and read back')
