"""OBJ MTL discovery retains complete names, tolerates loss and deduplicates libraries."""
import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    for name in ('build', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    for name, material in (('red.mtl', 'Red'), ('red  paint.mtl', 'Red'), ('blue.mtl', 'Blue'), ('duplicate.mtl', 'Red')):
        (args.output / name).write_text(f'newmtl {material}\nKd 1 0 0\n', encoding='utf-8')
    policy = args.output / 'scope.json'
    policy.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())], output_roots=[str(args.output.resolve())],
                                     policy_revision='obj-materials')), encoding='utf-8')
    records, failed = [], []

    def call(operation, **values):
        request = dict(version=1, operation=operation, **values)
        run = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(policy)],
            input=json.dumps(request) + '\n', text=True, encoding='utf-8', capture_output=True, timeout=60)
        result = json.loads(run.stdout)
        records.append(dict(request=request, response=result, exit_code=run.returncode))
        (args.output / 'requests.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
        return result

    cases = {'missing': 'mtllib missing.mtl', 'spaces': 'mtllib red  paint.mtl',
             'repeat': 'mtllib red.mtl\nmtllib RED.MTL', 'multiple': 'mtllib red.mtl blue.mtl',
             'conflict': 'mtllib red.mtl duplicate.mtl'}
    for name, declaration in cases.items():
        source = args.output / (name + '.obj')
        source.write_text(declaration + '\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Red\nf 1 2 3\n', encoding='utf-8')
        output = args.output / (name + '.fbx')
        request = dict(input=str(source), output=str(output), options=dict(target='fbx'))
        result = call('preflight', **request)
        try:
            if name == 'conflict':
                assert result['error']['code'] == 'invalid_argument', result
                continue
            assert 'error' not in result, result
            losses = [row['code'] for row in result['losses']]
            assert ('obj_missing_mtl' in losses) == (name == 'missing'), losses
            denied = call('convert', **request, approval_digest=result['approval_digest'], acknowledge_losses=False)
            assert 'error' in denied and not output.exists(), denied
            written = call('convert', **request, approval_digest=result['approval_digest'], acknowledge_losses=True)
            assert 'error' not in written, written
            report = output.with_suffix('.json')
            subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), str(output), str(report)], check=True)
            mesh = json.loads(report.read_text(encoding='utf-8'))['meshes'][0]
            if name != 'missing':
                assert mesh['materials'][0]['diffuse'] == [1, 0, 0], mesh['materials']
            print('PASS', name)
        except (AssertionError, KeyError) as error:
            print('FAIL', name, error)
            failed.append(name)
    assert not failed, failed


if __name__ == '__main__':
    main()
