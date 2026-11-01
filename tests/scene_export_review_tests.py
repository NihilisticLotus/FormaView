"""Adversarial inputs and independent readback regressions from the M08 review."""
import argparse
import base64
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import zlib

from scene_export_tests import fixtures, close


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--no-animation-reader', action='store_true',
                        help='build tree has no formaview_gltf_anim_tests; recorded as skipped, not passed')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.output / 'fixtures'
    fixtures(source)
    original = json.loads((source / 'source.gltf').read_text())
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output)], output_roots=[str(args.output)],
                                    allow_overwrite=True, policy_revision='review-regressions')))
    records, results = [], []

    def call(operation, **kwargs):
        request = dict(version=1, operation=operation, **kwargs)
        process = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(scope)],
                                 input=json.dumps(request) + '\n', text=True, capture_output=True,
                                 encoding='utf-8', timeout=60)
        records.append(dict(request=request, returncode=process.returncode,
                            stdout=process.stdout, stderr=process.stderr))
        assert process.returncode in (0, 1), (process.returncode, process.stderr[-3000:])
        return json.loads(process.stdout)

    def write(name, data):
        path = source / (name + '.gltf')
        path.write_text(json.dumps(data), encoding='utf-8')
        return str(path)

    def export(name, data):
        path = write(name, data)
        inspected = call('inspect', input=path)
        assert 'error' not in inspected, inspected
        options = dict(target='fbx')
        if inspected['clips']:
            options['clip_ids'] = [c['id'] for c in inspected['clips']]
        request = dict(input=path, output=str(args.output / (name + '.fbx')), options=options)
        preflight = call('preflight', **request)
        assert 'error' not in preflight, preflight
        result = call('convert', **request, approval_digest=preflight['approval_digest'], acknowledge_losses=True)
        assert 'error' not in result, result
        report = args.output / (name + '.json')
        subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), request['output'], str(report)],
                       check=True, timeout=30)
        return json.loads(report.read_text(encoding='utf-8'))

    def reject(name, data):
        path = write(name, data)
        destination = args.output / (name + '.fbx')
        result = call('preflight', input=path, output=str(destination), options=dict(target='fbx'))
        assert 'error' in result, result
        assert not destination.exists()

    def sparse_outside_buffer():
        data = copy.deepcopy(original)
        data['bufferViews'].append(dict(buffer=0, byteOffset=1024*1024, byteLength=1))
        data['accessors'][0]['sparse'] = dict(count=1, indices=dict(bufferView=len(data['bufferViews'])-1, componentType=5121),
                                             values=dict(bufferView=0))
        reject('sparse-outside-buffer', data)

    def offset_overflow():
        data = copy.deepcopy(original)
        data['accessors'][0]['byteOffset'] = 2**64-4
        reject('offset-overflow', data)

    def hierarchy():
        data = copy.deepcopy(original)
        data.pop('animations'); data.pop('skins')
        q = math.sin(math.pi/8); w = math.cos(math.pi/8)
        data['nodes'] = [dict(name='Parent', scale=[2,3,4], children=[1]),
                         dict(name='Child', rotation=[0,0,q,w], mesh=0)]
        data['scenes'] = [dict(nodes=[0])]
        report = export('scaled-hierarchy', data)
        world = next(n['world'] for n in report['nodes'] if n['name'] == 'Child')
        a = math.sqrt(.5)
        close(world, [2*a,-2*a,0,0,3*a,3*a,0,0,0,0,4,0,0,0,0,1])

    def signed_scale():
        data = copy.deepcopy(original)
        data['nodes'][1]['scale'] = [-1,2,3]
        report = export('signed-scale-animation', data)
        first = report['clips'][0]['samples'][0]
        world = next(n['world'] for n in first['nodes'] if n['name'] == 'Joint0')
        close(world, [-1,0,0,0,0,2,0,0,0,0,3,0,0,0,0,1])

    def media_collision():
        def chunk(kind, data):
            return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
        blue = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\0\0\xff\xff'))+chunk(b'IEND',b'')
        red = (source / 'pixel.png').read_bytes()
        (source / 'image-1.png').write_bytes(red)
        data = copy.deepcopy(original)
        data['images'] = [dict(uri='image-1.png'), dict(uri='data:image/png;base64,'+base64.b64encode(blue).decode())]
        data['textures'] = [dict(source=0), dict(source=1)]
        other = copy.deepcopy(data['materials'][0]); other['name'] = 'Blue'
        other['pbrMetallicRoughness']['baseColorTexture']['index'] = 1
        data['materials'].append(other)
        primitive = copy.deepcopy(data['meshes'][0]['primitives'][0]); primitive['material'] = 1
        data['meshes'][0]['primitives'].append(primitive)
        report = export('media-collision', data)
        assert {hashlib.sha256(b).hexdigest() for b in (red, blue)} <= {m['sha256'] for m in report['media']}, report['media']

    def unreadable_dependency():
        # A file may be writable as an output but outside the allowed read root.
        # An acknowledged texture-read loss must never remove source protection.
        target = args.output / 'outside-texture.fbx'
        texture = (source / 'pixel.png').read_bytes()
        target.write_bytes(texture)
        (source / 'guard.mtl').write_text('newmtl Guard\nmap_Kd ../outside-texture.fbx\n')
        model = source / 'guard.obj'
        model.write_text('mtllib guard.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl Guard\nf 1 2 3\n')
        policy = scope.read_bytes()
        scope.write_text(json.dumps(dict(input_roots=[str(source)], output_roots=[str(args.output)],
                                        allow_overwrite=True, policy_revision='review-split-scope')))
        try:
            result = call('preflight', input=str(model), output=str(target), conflict='overwrite', options=dict(target='fbx'))
            assert 'error' in result, 'Preflight permits overwriting an unreadable source texture'
            assert target.read_bytes() == texture
        finally:
            scope.write_bytes(policy)

    def animation_time_range():
        data = copy.deepcopy(original)
        binary = bytearray((source / 'source.bin').read_bytes())
        accessor = data['accessors'][data['animations'][0]['samplers'][0]['input']]
        offset = data['bufferViews'][accessor['bufferView']]['byteOffset']
        struct.pack_into('<3f', binary, offset, 1e20, 2e20, 3e20)
        (source / 'large-time.bin').write_bytes(binary)
        data['buffers'][0]['uri'] = 'large-time.bin'
        reject('large-time', data)

    def valid_sparse():
        data = copy.deepcopy(original)
        binary = bytearray((source / 'source.bin').read_bytes())
        at = len(binary); binary.extend(b'\0\1\2\0')
        data['bufferViews'].append(dict(buffer=0, byteOffset=at, byteLength=3))
        sparse_indices = len(data['bufferViews'])-1
        accessor = data['accessors'][0]
        values = accessor.pop('bufferView')
        accessor['sparse'] = dict(count=3, indices=dict(bufferView=sparse_indices, componentType=5121),
                                  values=dict(bufferView=values))
        data['buffers'][0].update(uri='sparse.bin', byteLength=len(binary))
        (source / 'sparse.bin').write_bytes(binary)
        report = export('valid-sparse', data)
        close(report['meshes'][0]['points'], [[0,0,0],[2,0,0],[0,3,0]])

    def deep_hierarchy():
        data = copy.deepcopy(original)
        data.pop('animations'); data.pop('skins')
        data['nodes'] = [dict(children=[i+1]) for i in range(300)] + [dict(mesh=0)]
        data['scenes'] = [dict(nodes=[0])]
        reject('deep-hierarchy', data)

    def sub_tick_keys():
        data = copy.deepcopy(original)
        binary = bytearray((source / 'source.bin').read_bytes())
        accessor = data['accessors'][data['animations'][0]['samplers'][0]['input']]
        offset = data['bufferViews'][accessor['bufferView']]['byteOffset']
        struct.pack_into('<3f', binary, offset, 0, 1e-12, 2e-12)
        (source / 'sub-tick.bin').write_bytes(binary)
        data['buffers'][0]['uri'] = 'sub-tick.bin'
        reject('sub-tick', data)

    def meshopt_fallback():
        # EXT_meshopt_compression with a valid uncompressed fallback: export has
        # no decoder and must refuse, while plain animation reading stays usable.
        data = copy.deepcopy(original)
        view = data['bufferViews'][data['accessors'][0]['bufferView']]
        view['extensions'] = {'EXT_meshopt_compression': dict(
            buffer=0, byteOffset=view['byteOffset'], byteLength=view['byteLength'],
            byteStride=12, count=view['byteLength'] // 12, mode='ATTRIBUTES')}
        view['byteStride'] = 12
        data['extensionsUsed'] = ['EXT_meshopt_compression']
        path = write('meshopt-fallback', data)
        destination = args.output / 'meshopt-fallback.fbx'
        result = call('preflight', input=path, output=str(destination), options=dict(target='fbx'))
        assert 'error' in result and 'eshopt' in json.dumps(result), result
        assert not destination.exists()

    def embedded_jpeg_extension():
        # A JPEG data URI without mimeType must not be published as image-N.png.
        jpeg = bytes.fromhex('ffd8ffe000104a46494600010100000100010000ffd9')
        data = copy.deepcopy(original)
        data['images'] = [dict(uri='data:image/jpeg;base64,' + base64.b64encode(jpeg).decode())]
        data['textures'] = [dict(source=0)]
        report = export('embedded-jpeg', data)
        names = [m['name'] for m in report['media']]
        assert any(n.lower().endswith(('.jpg', '.jpeg')) for n in names), names

    def affine_inverse_bind():
        # The SDK cluster API loses shear on matrix readback. Refuse such
        # bindings explicitly; representable rotated/scaled binds must survive.
        data = copy.deepcopy(original)
        expected = [
            [1, .25, 0, .5, 0, 1, .125, -2, 0, 0, 1, .25, 0, 0, 0, 1],
            [-2, .125, 0, 1, 0, .5, 0, 2, 0, 0, 1, -1, 0, 0, 0, 1],
        ] + [[1,0,0,0, 0,1,0,-i, 0,0,1,0, 0,0,0,1] for i in range(2, 6)]
        columns = [matrix[row*4+column] for matrix in expected for column in range(4) for row in range(4)]
        payload = struct.pack('<'+'f'*len(columns), *columns)
        data['buffers'].append(dict(uri='data:application/octet-stream;base64,'+base64.b64encode(payload).decode(), byteLength=len(payload)))
        data['bufferViews'].append(dict(buffer=len(data['buffers'])-1, byteLength=len(payload)))
        data['accessors'].append(dict(bufferView=len(data['bufferViews'])-1, componentType=5126, count=6, type='MAT4'))
        data['skins'][0]['inverseBindMatrices'] = len(data['accessors'])-1
        path = write('affine-inverse-bind', data)
        destination = args.output / 'affine-inverse-bind.fbx'
        result = call('preflight', input=path, output=str(destination), options=dict(target='fbx'))
        assert result.get('error', {}).get('code') == 'unsupported', result
        assert 'inverse bind matrix cannot be retained' in result['error']['message'], result
        assert not destination.exists()

        # Positive control: rotation, non-uniform scale and translation with
        # an orthogonal bind basis (the inverse itself need not have one).
        a = math.sqrt(.5)
        expected[0] = [2*a, 2*a, 0, .5, -3*a, 3*a, 0, -2, 0, 0, 4, .25, 0, 0, 0, 1]
        expected[1] = [-2, 0, 0, 1, 0, .5, 0, 2, 0, 0, 1, -1, 0, 0, 0, 1]
        columns = [matrix[row*4+column] for matrix in expected for column in range(4) for row in range(4)]
        payload = struct.pack('<'+'f'*len(columns), *columns)
        data['buffers'][-1]['uri'] = 'data:application/octet-stream;base64,'+base64.b64encode(payload).decode()
        report = export('representable-inverse-bind', data)
        clusters = {cluster['bone']: cluster for cluster in report['meshes'][0]['clusters']}
        for index, matrix in enumerate(expected):
            close(clusters['Joint'+str(index)]['geometry_to_bone'], matrix, 1e-6)

    def animation_reader_inputs():
        # The animation preview reader shares the storage guard: hostile files
        # produce a normal error JSON (process alive), valid ones still load.
        temporary = args.output / 'anim-temp'
        temporary.mkdir()
        environment = dict(os.environ, TEMP=str(temporary), TMP=str(temporary))
        def read(name):
            report = args.output / ('anim-' + name + '.json')
            process = subprocess.run([str(args.build / 'Release/formaview_gltf_anim_tests.exe'),
                                      str(source / (name + '.gltf')), str(report)],
                                     capture_output=True, text=True, timeout=60, env=environment)
            assert process.returncode == 0, (name, process.returncode, process.stderr[-2000:])
            return json.loads(report.read_text(encoding='utf-8'))
        for name in ('sparse-outside-buffer', 'offset-overflow', 'deep-hierarchy'):
            result = read(name)
            assert result.get('error') and 'clips' not in result, (name, result)
        for name in ('source', 'valid-sparse', 'meshopt-fallback'):
            result = read(name)
            assert 'error' not in result and result['clips'] >= 1, (name, result)
            assert all(c['channels'] > 0 for c in result['detail']), (name, result)

    def obj_homogeneous_overflow():
        path = source / 'homogeneous-overflow.obj'
        path.write_text('v 1e308 0 0 1e-308\nv 1 0 0\nv 0 1 0\nf 1 2 3\n', encoding='utf-8')
        destination = args.output / 'homogeneous-overflow.fbx'
        result = call('preflight', input=str(path), output=str(destination), options=dict(target='fbx'))
        assert 'error' in result, result
        assert not destination.exists()
        path.write_text('v 2 0 0 2\nv 1 0 0\nv 0 1 0\nf 1 2 3\n', encoding='utf-8')
        valid = call('preflight', input=str(path), output=str(destination), options=dict(target='fbx'))
        assert 'error' not in valid, valid

    for name, test in [('obj_homogeneous_overflow', obj_homogeneous_overflow),
                       ('sparse_outside_buffer', sparse_outside_buffer), ('offset_overflow', offset_overflow),
                       ('scaled_hierarchy', hierarchy), ('signed_scale_animation', signed_scale),
                       ('embedded_media_collision', media_collision), ('unreadable_dependency', unreadable_dependency),
                       ('animation_time_range', animation_time_range), ('valid_sparse', valid_sparse),
                       ('deep_hierarchy', deep_hierarchy), ('sub_tick_keys', sub_tick_keys),
                       ('meshopt_fallback', meshopt_fallback), ('embedded_jpeg_extension', embedded_jpeg_extension),
                       ('animation_reader_inputs', animation_reader_inputs), ('affine_inverse_bind', affine_inverse_bind)]:
        try:
            if name == 'animation_reader_inputs' and args.no_animation_reader:
                results.append(dict(name=name, passed=False, skipped=True))
            else:
                test()
                results.append(dict(name=name, passed=True))
        except Exception as error:
            results.append(dict(name=name, passed=False, error=str(error)))
        (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        (args.output / 'requests.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
        print(name, 'SKIPPED' if results[-1].get('skipped') else 'PASS' if results[-1]['passed'] else 'FAIL', flush=True)
    assert all(r['passed'] or r.get('skipped') for r in results), results


if __name__ == '__main__':
    main()
