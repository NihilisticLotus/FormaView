"""Verify external official VRM samples via source arrays and independent ufbx.

No downloads or source writes. Retained geometry, UV, skin, sparse position
morphs and embedded base-color media are checked; VRM behaviour is an explicit
conversion loss, never asserted to be reproduced by FBX.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

import numpy as np

from scene_export_upstream_tests import accessor, compare, digest, read_glb, worlds


def verify(document, binary, report, losses):
    compare([report['unit']], [1.0], 1e-9, 'metres per unit')
    codes = {loss['code'] for loss in losses}
    assert 'vrm_semantics' in codes, 'Missing mandatory VRM semantic loss'
    extensions = document.get('extensionsUsed', [])
    vrm_extensions = {name for name in extensions if name == 'VRM' or name.startswith('VRMC_')}
    assert vrm_extensions
    for extension in vrm_extensions:
        assert any(loss['code'] == 'vrm_semantics' and extension in loss['detail'] for loss in losses), extension
    matrices = worlds(document, binary, {'channels': []}, 0)
    nodes = {node['name']: np.array(node['world']).reshape(4, 4) for node in report['nodes']}
    names = [node['name'] for node in document['nodes']]
    assert len(set(names)) == len(names), 'Oracle requires unique source node names'
    for index, name in enumerate(names):
        compare(nodes[name], matrices[index], 2e-5, 'node transform '+name)
    meshes = {mesh['name']: mesh for mesh in report['meshes']}
    assert len(meshes) == len(report['meshes'])
    totals = dict(meshes=0, vertices=0, corners=0, influences=0, morphs=0, morph_vertices=0, media=0)
    seen = set()
    for node in document['nodes']:
        if 'mesh' not in node:
            continue
        source_mesh = document['meshes'][node['mesh']]
        for primitive_index, primitive in enumerate(source_mesh['primitives']):
            name = node['name']+'_primitive_'+str(primitive_index)
            mesh = meshes[name]
            seen.add(name)
            assert primitive.get('mode', 4) == 4
            attributes = primitive['attributes']
            position = accessor(document, binary, attributes['POSITION'])
            indices = (accessor(document, binary, primitive['indices'])[:, 0].astype(int)
                       if 'indices' in primitive else np.arange(len(position)))
            compare(mesh['points'], position, 2e-5, name+' positions')
            compare(mesh['corners'], indices, 0, name+' topology')
            if 'TEXCOORD_0' in attributes:
                uv = accessor(document, binary, attributes['TEXCOORD_0'])[indices].copy()
                uv[:, 1] = 1-uv[:, 1]
                compare(mesh['uv'], uv, 2e-6, name+' UV')
            if 'NORMAL' in attributes:
                compare(mesh['normals'], accessor(document, binary, attributes['NORMAL'])[indices], 2e-5, name+' normals')
            if 'skin' in node:
                skin = document['skins'][node['skin']]
                joint_names = [document['nodes'][j]['name'] for j in skin['joints']]
                bindings = (accessor(document, binary, skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
                            if 'inverseBindMatrices' in skin else np.tile(np.eye(4), (len(joint_names), 1, 1)))
                expected = np.zeros((len(position), len(joint_names)))
                for key in sorted(k for k in attributes if k.startswith('JOINTS_')):
                    joints = accessor(document, binary, attributes[key]).astype(int)
                    weights = accessor(document, binary, attributes[key.replace('JOINTS_', 'WEIGHTS_')])
                    for column in range(4):
                        np.add.at(expected, (np.arange(len(position)), joints[:, column]), weights[:, column])
                actual = np.zeros_like(expected)
                cluster_names = set()
                for cluster in mesh['clusters']:
                    joint = joint_names.index(cluster['bone'])
                    assert joint not in cluster_names
                    cluster_names.add(joint)
                    compare(np.array(cluster['geometry_to_bone']).reshape(4, 4), bindings[joint], 2e-5, name+' inverse bind')
                    np.add.at(actual[:, joint], cluster['indices'], cluster['weights'])
                compare(actual, expected, 2e-6, name+' source weights')
                totals['influences'] += int(np.count_nonzero(expected))
            else:
                assert not mesh['clusters'], name
            targets = primitive.get('targets', [])
            assert len(mesh['shapes']) == len(targets), name
            for index, (shape, target) in enumerate(zip(mesh['shapes'], targets)):
                expected = (accessor(document, binary, target['POSITION']) if 'POSITION' in target else np.zeros_like(position))
                actual = np.zeros_like(position)
                assert len(shape['indices']) == len(shape['offsets'])
                assert len(set(shape['indices'])) == len(shape['indices'])
                actual[shape['indices']] = np.asarray(shape['offsets']).reshape(-1, 3)
                compare(actual, expected, 2e-5, name+' morph positions '+str(index))
                expected_normals = (accessor(document, binary, target['NORMAL']) if 'NORMAL' in target else np.zeros_like(position))
                actual_normals = np.zeros_like(position)
                if shape['normal_offsets']:
                    assert len(shape['normal_offsets']) == len(shape['indices'])
                    actual_normals[shape['indices']] = shape['normal_offsets']
                compare(actual_normals, expected_normals, 2e-5, name+' morph normals '+str(index))
                weights = node.get('weights', source_mesh.get('weights', []))
                compare([shape['weight']], [weights[index] if index < len(weights) else 0], 2e-6, 'morph default weight')
                totals['morph_vertices'] += len(position)
            totals['meshes'] += 1
            totals['vertices'] += len(position)
            totals['corners'] += len(indices)
            totals['morphs'] += len(targets)
    assert seen == set(meshes), 'Missing or unexpected mesh primitives'
    expected_media = set()
    for material in document.get('materials', []):
        texture = material.get('pbrMetallicRoughness', {}).get('baseColorTexture')
        if texture is None:
            continue
        image = document['images'][document['textures'][texture['index']]['source']]
        assert 'bufferView' in image, 'Oracle currently requires embedded base-color media'
        view = document['bufferViews'][image['bufferView']]
        payload = binary[view.get('byteOffset', 0):view.get('byteOffset', 0)+view['byteLength']]
        expected_media.add(hashlib.sha256(payload).hexdigest())
    # The current exporter embeds these source payloads without re-encoding.
    # This checks that contract, not general perceptual image equivalence.
    actual_media = {media['sha256'] for media in report['media']}
    assert expected_media <= actual_media, ('Missing base-color media', expected_media-actual_media)
    totals['media'] = len(expected_media)
    totals.update(loss_codes=sorted(codes), vrm_extensions=sorted(vrm_extensions), passed=True)
    return totals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--expect-bind-rejection', action='append', default=[], metavar='STEM',
                        help='Explicit sample expected to be unsupported by the FBX cluster API')
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()
    sources = [p.resolve(strict=True) for p in args.sources]
    assert len({p.stem for p in sources}) == len(sources)
    assert set(args.expect_bind_rejection) <= {p.stem for p in sources}
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=sorted({str(p.parent) for p in sources}),
        output_roots=[str(args.output)], allow_overwrite=False, policy_revision='official-vrm-v1')), encoding='utf-8')
    calls, results = [], []

    def call(operation, expect_rejection=False, **values):
        request = dict(version=1, operation=operation, **values)
        process = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(scope)],
            input=json.dumps(request)+'\n', capture_output=True, text=True, encoding='utf-8', timeout=180)
        calls.append(dict(request=request, returncode=process.returncode, stdout=process.stdout, stderr=process.stderr))
        (args.output / 'calls.json').write_text(json.dumps(calls, ensure_ascii=False, indent=2), encoding='utf-8')
        assert process.returncode == (1 if expect_rejection else 0), calls[-1]
        response = json.loads(process.stdout)
        if expect_rejection:
            assert response.get('error', {}).get('code') == 'unsupported', response
            assert response['error']['message'] == 'glTF inverse bind matrix cannot be retained by FBX cluster API', response
        else:
            assert 'error' not in response, response
        return response

    for source in sources:
        before = digest(source)
        document, binary = read_glb(source)
        request = dict(input=str(source), output=str(args.output / (source.stem+'.fbx')), options=dict(target='fbx'))
        rejected = source.stem in args.expect_bind_rejection
        plan = call('preflight', expect_rejection=rejected, **request)
        if rejected:
            assert not Path(request['output']).exists()
            assert digest(source) == before
            row = dict(source=str(source), source_sha256=before,
                       cli_sha256=digest(args.build / 'Release/formaview_image_cli.exe'),
                       compatibility='unsupported', rejection_verified=True, error=plan['error'])
            results.append(row)
            (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
            print(json.dumps(row, ensure_ascii=False), flush=True)
            continue
        assert plan['losses'], 'VRM conversion must report semantic loss'
        call('convert', **request, approval_digest=plan['approval_digest'], acknowledge_losses=True)
        report = args.output / (source.stem+'-ufbx.json')
        process = subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), request['output'], str(report)],
                                 capture_output=True, text=True, encoding='utf-8', timeout=180)
        (args.output / (source.stem+'-probe.log')).write_text(process.stdout+process.stderr, encoding='utf-8')
        assert process.returncode == 0, process.stderr
        row = verify(document, binary, json.loads(report.read_text(encoding='utf-8')), plan['losses'])
        assert digest(source) == before
        row.update(source=str(source), source_sha256=before, compatibility='supported_with_losses', generator=document['asset'].get('generator', 'unknown'),
                   cli_sha256=digest(args.build / 'Release/formaview_image_cli.exe'))
        results.append(row)
        (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(row, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
