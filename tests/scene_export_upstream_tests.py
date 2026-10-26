"""Read-only upstream GLB -> FBX checks against an independent NumPy skinning oracle.

Supply external GLBs and an empty output directory; no samples are downloaded.
The animation oracle intentionally accepts single-primitive, linear/step skinned
assets only. Unsupported oracle features fail explicitly instead of being skipped.
Requires NumPy and the existing ufbx scene_export_probe build target.
"""
import argparse
import bisect
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess

import numpy as np


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_glb(path):
    raw = path.read_bytes()
    assert struct.unpack_from('<III', raw) == (0x46546C67, 2, len(raw))
    chunks, offset = {}, 12
    while offset < len(raw):
        size, kind = struct.unpack_from('<II', raw, offset)
        assert kind not in chunks and offset + 8 + size <= len(raw)
        chunks[kind] = raw[offset + 8:offset + 8 + size]
        offset += size + 8
    return json.loads(chunks[0x4E4F534A]), chunks[0x004E4942]


def accessor(document, binary, index):
    item = document['accessors'][index]
    dtype = {5126: '<f4', 5125: '<u4', 5123: '<u2', 5121: 'u1', 5122: '<i2', 5120: 'i1'}[item['componentType']]
    count = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[item['type']]
    width = np.dtype(dtype).itemsize
    if 'bufferView' in item:
        view = document['bufferViews'][item['bufferView']]
        assert view['buffer'] == 0
        result = np.ndarray((item['count'], count), dtype=dtype, buffer=binary,
                            offset=view.get('byteOffset', 0) + item.get('byteOffset', 0),
                            strides=(view.get('byteStride', count * width), width)).astype(float)
    else:
        result = np.zeros((item['count'], count))
    if 'sparse' in item:
        sparse = item['sparse']
        index_info, value_info = sparse['indices'], sparse['values']
        index_view = document['bufferViews'][index_info['bufferView']]
        value_view = document['bufferViews'][value_info['bufferView']]
        assert index_view['buffer'] == value_view['buffer'] == 0
        index_type = {5121: 'u1', 5123: '<u2', 5125: '<u4'}[index_info['componentType']]
        indices = np.frombuffer(binary, dtype=index_type, count=sparse['count'],
                                offset=index_view.get('byteOffset', 0)+index_info.get('byteOffset', 0)).astype(int)
        assert len(indices) and (np.diff(indices) > 0).all() and indices[-1] < item['count']
        values = np.frombuffer(binary, dtype=dtype, count=sparse['count']*count,
                               offset=value_view.get('byteOffset', 0)+value_info.get('byteOffset', 0)).reshape(-1, count)
        result[indices] = values
    if item.get('normalized'):
        assert item['componentType'] != 5126
        result = np.maximum(-1, result / np.iinfo(np.dtype(dtype)).max)
    assert np.isfinite(result).all()
    return result


def trs(translation, rotation, scale):
    x, y, z, w = rotation / np.linalg.norm(rotation)
    result = np.eye(4)
    result[:3, :3] = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)]]) @ np.diag(scale)
    result[:3, 3] = translation
    return result


def worlds(document, binary, animation, time):
    nodes = document['nodes']
    values = [dict(translation=np.array(n.get('translation', [0, 0, 0]), float),
                   rotation=np.array(n.get('rotation', [0, 0, 0, 1]), float),
                   scale=np.array(n.get('scale', [1, 1, 1]), float)) for n in nodes]
    for channel in animation['channels']:
        sampler = animation['samplers'][channel['sampler']]
        times = accessor(document, binary, sampler['input'])[:, 0]
        keys = accessor(document, binary, sampler['output'])
        mode = sampler.get('interpolation', 'LINEAR')
        path = channel['target']['path']
        assert mode in ('LINEAR', 'STEP') and path in ('translation', 'rotation', 'scale')
        i = max(0, min(len(times)-1, bisect.bisect_right(times, time)-1))
        j = min(i+1, len(times)-1)
        fraction = 0 if i == j else np.clip((time-times[i])/(times[j]-times[i]), 0, 1)
        a, b = keys[i].copy(), keys[j].copy()
        if mode == 'STEP':
            value = a
        elif path == 'rotation':
            a /= np.linalg.norm(a)
            b /= np.linalg.norm(b)
            dot = float(a @ b)
            if dot < 0:
                b, dot = -b, -dot
            angle = math.acos(min(1, dot))
            value = ((1-fraction)*a + fraction*b if angle < 1e-6 else
                     (math.sin((1-fraction)*angle)*a + math.sin(fraction*angle)*b)/math.sin(angle))
        else:
            value = (1-fraction)*a + fraction*b
        values[channel['target']['node']][path] = value
    parents = {child: i for i, n in enumerate(nodes) for child in n.get('children', [])}
    cache = {}

    def world(index):
        if index not in cache:
            node, value = nodes[index], values[index]
            local = (np.array(node['matrix']).reshape(4, 4).T if 'matrix' in node else
                     trs(value['translation'], value['rotation'], value['scale']))
            cache[index] = world(parents[index]) @ local if index in parents else local
        return cache[index]

    return np.array([world(i) for i in range(len(nodes))])


def compare(actual, expected, tolerance, label):
    actual, expected = np.asarray(actual), np.asarray(expected)
    assert actual.shape == expected.shape, (label, actual.shape, expected.shape)
    assert actual.size and np.isfinite(actual).all() and np.isfinite(expected).all(), label
    error = float(np.max(np.abs(actual - expected)))
    assert error <= tolerance, (label, error, tolerance)
    return error


def verify(document, binary, report):
    compare([report['unit']], [1.0], 1e-9, 'metres per unit')
    assert len(document['meshes']) == len(report['meshes']) == 1
    assert len(document['meshes'][0]['primitives']) == 1
    primitive = document['meshes'][0]['primitives'][0]
    assert primitive.get('mode', 4) == 4 and not primitive.get('targets')
    attributes = primitive['attributes']
    position = accessor(document, binary, attributes['POSITION'])
    indices = (accessor(document, binary, primitive['indices'])[:, 0].astype(int)
               if 'indices' in primitive else np.arange(len(position)))
    mesh = report['meshes'][0]
    assert not mesh['shapes'], 'Unexpected morphs on a source without targets'
    node_names = [node['name'] for node in document['nodes']]
    assert len(set(node_names)) == len(node_names), 'Oracle requires unique source node names'
    compare(mesh['points'], position, 2e-5, 'source positions')
    compare(mesh['corners'], indices, 0, 'topology')
    uv = accessor(document, binary, attributes['TEXCOORD_0'])[indices].copy()
    uv[:, 1] = 1 - uv[:, 1]
    compare(mesh['uv'], uv, 2e-6, 'UV corners')
    mesh_nodes = [n for n in document['nodes'] if 'mesh' in n]
    assert len(mesh_nodes) == 1 and mesh_nodes[0]['mesh'] == 0
    skin = document['skins'][mesh_nodes[0]['skin']]
    joint_names = [document['nodes'][j]['name'] for j in skin['joints']]
    assert len(set(joint_names)) == len(joint_names)
    bindings = accessor(document, binary, skin['inverseBindMatrices']).reshape(-1, 4, 4).transpose(0, 2, 1)
    joint_sets = sorted(k for k in attributes if k.startswith('JOINTS_'))
    joints = np.concatenate([accessor(document, binary, attributes[k]).astype(int) for k in joint_sets], axis=1)
    weights = np.concatenate([accessor(document, binary, attributes[k.replace('JOINTS_', 'WEIGHTS_')]) for k in joint_sets], axis=1)
    assert (weights >= 0).all() and (weights.sum(axis=1) > 0).all()
    assert (joints >= 0).all() and (joints < len(joint_names)).all()
    expected_weights = np.zeros((len(position), len(joint_names)))
    for column in range(joints.shape[1]):
        np.add.at(expected_weights, (np.arange(len(position)), joints[:, column]), weights[:, column])
    actual_weights = np.zeros_like(expected_weights)
    seen = set()
    for cluster in mesh['clusters']:
        joint = joint_names.index(cluster['bone'])
        assert joint not in seen
        seen.add(joint)
        compare(np.array(cluster['geometry_to_bone']).reshape(4, 4), bindings[joint], 2e-5, 'inverse bind')
        assert len(set(cluster['indices'])) == len(cluster['indices'])
        actual_weights[cluster['indices'], joint] = cluster['weights']
    weight_error = compare(actual_weights, expected_weights, 2e-6, 'all source influences')
    weights /= weights.sum(axis=1, keepdims=True)
    assert len(document['animations']) == len(report['clips']) > 0
    homogeneous = np.column_stack([position, np.ones(len(position))])
    vertex_checks, matrix_checks, max_vertex_error, max_matrix_error = 0, 0, 0., 0.
    for animation, clip in zip(document['animations'], report['clips']):
        if animation.get('name'):
            assert animation['name'] == clip['name']
        times = np.concatenate([accessor(document, binary, s['input'])[:, 0] for s in animation['samplers']])
        compare([clip['start'], clip['end']], [times.min(), times.max()], 1e-6, 'clip time')
        assert len(clip['samples']) == 9
        compare([sample['time'] for sample in clip['samples']], np.linspace(times.min(), times.max(), 9), 1e-6, 'sample time grid')
        for sample in clip['samples']:
            matrices = worlds(document, binary, animation, sample['time'])
            actual_nodes = {n['name']: np.array(n['world']).reshape(4, 4) for n in sample['nodes']}
            for index, node in enumerate(document['nodes']):
                assert node.get('name') in actual_nodes
                max_matrix_error = max(max_matrix_error, compare(actual_nodes[node['name']], matrices[index], .005, 'animated world matrix'))
                matrix_checks += 1
            palette = matrices[skin['joints']] @ bindings
            deformed = np.zeros_like(homogeneous)
            for column in range(joints.shape[1]):
                deformed += np.einsum('nij,nj->ni', palette[joints[:, column]], homogeneous) * weights[:, column, None]
            # ufbx's evaluated skinned_position is in world space. Compare every
            # corner, including repeated indices, without inferring a best-fit transform.
            max_vertex_error = max(max_vertex_error, compare(sample['meshes'][0]['deformed'], deformed[indices, :3], .005, 'animated skinned corners'))
            vertex_checks += len(indices)
    return dict(vertices=len(position), corners=len(indices), joints=len(joint_names),
                clips=len(report['clips']), vertex_checks=vertex_checks, matrix_checks=matrix_checks,
                max_vertex_error=max_vertex_error, max_matrix_error=max_matrix_error,
                max_weight_error=weight_error, position_tolerance=.005, passed=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('sources', type=Path, nargs='+')
    args = parser.parse_args()
    sources = [p.resolve(strict=True) for p in args.sources]
    assert len({p.stem for p in sources}) == len(sources), 'Output names must be distinct'
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=sorted({str(p.parent) for p in sources}),
        output_roots=[str(args.output)], allow_overwrite=False, policy_revision='upstream-skinning-v1')), encoding='utf-8')
    records, results = [], []

    def call(operation, **values):
        request = dict(version=1, operation=operation, **values)
        process = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(scope)],
            input=json.dumps(request)+'\n', capture_output=True, text=True, encoding='utf-8', timeout=180)
        records.append(dict(request=request, returncode=process.returncode, stdout=process.stdout, stderr=process.stderr))
        (args.output / 'calls.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')
        assert process.returncode == 0, records[-1]
        response = json.loads(process.stdout)
        assert 'error' not in response, response
        return response

    for source in sources:
        before = digest(source)
        document, binary = read_glb(source)
        inspected = call('inspect', input=str(source))
        request = dict(input=str(source), output=str(args.output / (source.stem+'.fbx')),
                       options=dict(target='fbx', clip_ids=[c['id'] for c in inspected['clips']]))
        plan = call('preflight', **request)
        call('convert', **request, approval_digest=plan['approval_digest'], acknowledge_losses=True)
        path = args.output / (source.stem+'-ufbx.json')
        process = subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), request['output'], str(path)],
                                 capture_output=True, text=True, encoding='utf-8', timeout=180)
        (args.output / (source.stem+'-probe.log')).write_text(process.stdout+process.stderr, encoding='utf-8')
        assert process.returncode == 0, process.stderr
        result = verify(document, binary, json.loads(path.read_text(encoding='utf-8')))
        assert digest(source) == before, 'Source changed'
        result.update(source=str(source), source_sha256=before, generator=document['asset'].get('generator', 'unknown'),
                      losses=plan.get('losses'), cli_sha256=digest(args.build / 'Release/formaview_image_cli.exe'))
        results.append(result)
        (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False), flush=True)


if __name__ == '__main__':
    main()
