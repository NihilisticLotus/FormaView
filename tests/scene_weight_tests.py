"""Duplicate joint slots must become one FBX cluster entry per point."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

from scene_export_tests import fixtures


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.output / 'fixtures'
    fixtures(source)
    pmx = (source / 'source.pmx').read_bytes()
    old = struct.pack('<B4b4f', 2, 0, 1, 2, 3, .1, .2, .3, .4)
    new = struct.pack('<B4b4f', 2, 0, 0, 1, 2, .25, .25, .5, 0)
    assert pmx.count(old) == 1
    (source / 'duplicate.pmx').write_bytes(pmx.replace(old, new))
    document = json.loads((source / 'source.gltf').read_text(encoding='utf-8'))
    attributes = document['meshes'][0]['primitives'][0]['attributes']
    for cross_set in (False, True):
        binary = bytearray((source / 'source.bin').read_bytes())
        values = {'JOINTS_0': ('4B', [0, 0, 1, 0]),
                  'WEIGHTS_0': ('4f', [.25, 0 if cross_set else .25, .5, 0]),
                  'JOINTS_1': ('4B', [0, 0, 0, 0]),
                  'WEIGHTS_1': ('4f', [.25 if cross_set else 0, 0, 0, 0])}
        for name, (layout, row) in values.items():
            accessor = document['accessors'][attributes[name]]
            view = document['bufferViews'][accessor['bufferView']]
            struct.pack_into('<' + layout, binary, view.get('byteOffset', 0) + accessor.get('byteOffset', 0), *row)
        name = 'cross-set' if cross_set else 'duplicate'
        document['buffers'][0]['uri'] = name + '.bin'
        (source / (name + '.bin')).write_bytes(binary)
        (source / (name + '.gltf')).write_text(json.dumps(document), encoding='utf-8')
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],
                                    output_roots=[str(args.output.resolve())],
                                    policy_revision='duplicate-weights')), encoding='utf-8')
    records = []
    def call(operation, **values):
        request = dict(version=1, operation=operation, **values)
        process = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(scope)],
                                 input=json.dumps(request) + '\n', text=True, encoding='utf-8',
                                 capture_output=True, timeout=60)
        response = json.loads(process.stdout)
        records.append(dict(request=request, response=response, exit_code=process.returncode))
        (args.output / 'requests.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
        assert process.returncode == 0 and 'error' not in response, response
        return response
    passed = True
    for filename in ('duplicate.pmx', 'duplicate.gltf', 'cross-set.gltf'):
        input_path = source / filename
        inspected = call('inspect', input=str(input_path))
        options = dict(target='fbx')
        if inspected['clips']:
            options['clip_ids'] = [clip['id'] for clip in inspected['clips']]
        target = args.output / (filename + '.fbx')
        request = dict(input=str(input_path), output=str(target), options=options)
        preflight = call('preflight', **request)
        call('convert', **request, approval_digest=preflight['approval_digest'], acknowledge_losses=True)
        report = target.with_suffix('.json')
        subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), str(target), str(report)],
                       check=True, timeout=30)
        mesh = json.loads(report.read_text(encoding='utf-8'))['meshes'][0]
        expected = {'骨骼0': .5, '骨骼1': .5} if filename.endswith('.pmx') else {'Joint0': .5, 'Joint1': .5}
        for name, weight in expected.items():
            cluster = next(c for c in mesh['clusters'] if c['bone'] == name)
            entries = [w for i, w in zip(cluster['indices'], cluster['weights']) if i == 0]
            valid = len(entries) == 1 and abs(entries[0] - weight) < 1e-6
            print(filename, name, entries, 'valid=', valid)
            passed &= valid
    assert passed, 'Duplicate point indices or incorrect merged weights in FBX clusters'


if __name__ == '__main__':
    main()
