"""Generate tiny, self-authored playback capability fixtures; no DCC required."""
import base64
import json
from pathlib import Path
import struct
import sys


def generate(root):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    data = struct.pack('<17f', 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0)
    doc = {
        'asset': {'version': '2.0'},
        'buffers': [{'uri': 'data:application/octet-stream;base64,' + base64.b64encode(data).decode(), 'byteLength': len(data)}],
        'bufferViews': [{'buffer': 0, 'byteOffset': 0, 'byteLength': 36},
                        {'buffer': 0, 'byteOffset': 36, 'byteLength': 8},
                        {'buffer': 0, 'byteOffset': 44, 'byteLength': 24}],
        'accessors': [
            {'bufferView': 0, 'componentType': 5126, 'count': 3, 'type': 'VEC3', 'min': [0, 0, 0], 'max': [1, 1, 0]},
            {'bufferView': 1, 'componentType': 5126, 'count': 2, 'type': 'SCALAR', 'min': [0], 'max': [1]},
            {'bufferView': 2, 'componentType': 5126, 'count': 2, 'type': 'VEC3'},
            {'bufferView': 1, 'componentType': 5126, 'count': 1, 'type': 'SCALAR', 'min': [0], 'max': [0]},
            {'bufferView': 2, 'componentType': 5126, 'count': 1, 'type': 'VEC3'}],
        'meshes': [{'primitives': [{'attributes': {'POSITION': 0}}]}],
        'nodes': [{'name': 'Triangle', 'mesh': 0}], 'scenes': [{'nodes': [0]}], 'scene': 0,
    }
    def clip(name, single=False):
        return {'name': name, 'samplers': [{'input': 3 if single else 1, 'output': 4 if single else 2, 'interpolation': 'LINEAR'}],
                'channels': [{'sampler': 0, 'target': {'node': 0, 'path': 'translation'}}]}
    for name, clips in [('static', []), ('single-key', [clip('Pose', True)]),
                        ('animated', [clip('Move')]), ('mixed', [clip('Pose', True), clip('Move')])]:
        value = dict(doc)
        if clips:
            value['animations'] = clips
        (root / (name + '.gltf')).write_text(json.dumps(value), encoding='utf-8')
    for name, end in [('placeholder-zero', 0), ('placeholder-range', 46186158000)]:
        (root / (name + '.fbx')).write_text('''; FBX 7.4.0 project file
FBXHeaderExtension: { FBXVersion: 7400 }
Objects: {
 Geometry: 1, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: 0,0,0,1,0,0,0,1,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
 }
 Model: 2, "Model::Triangle", "Mesh" { }
 AnimationStack: 3, "AnimStack::Unreal Take", "" {
  Properties70: {
   P: "LocalStart", "KTime", "Time", "", 0
   P: "LocalStop", "KTime", "Time", "", END_TIME
  }
 }
 AnimationLayer: 4, "AnimLayer::BaseLayer", "" { }
}
Connections: {
 C: "OO",1,2
 C: "OO",2,0
 C: "OO",4,3
}
'''.replace('END_TIME', str(end)), encoding='ascii')
    animated = (root / 'placeholder-range.fbx').read_text(encoding='ascii')
    animated = animated.replace(' AnimationLayer:', ''' AnimationCurveNode: 5, "AnimCurveNode::T", "" {
  Properties70: { P: "d|X", "Number", "", "A", 0 }
 }
 AnimationCurve: 6, "AnimCurve::TX", "" {
  Default: 0
  KeyVer: 4008
  KeyTime: *2 { a: 0,46186158000 }
  KeyValueFloat: *2 { a: 0,1 }
  KeyAttrFlags: *2 { a: 4,4 }
  KeyAttrDataFloat: *8 { a: 0,0,0,0,0,0,0,0 }
  KeyAttrRefCount: *2 { a: 1,1 }
 }
 AnimationLayer:''')
    animated = animated.replace(' C: "OO",4,3', ''' C: "OO",4,3
 C: "OO",5,4
 C: "OP",5,2,"Lcl Translation"
 C: "OP",6,5,"d|X"''')
    (root / 'animated.fbx').write_text(animated, encoding='ascii')
    def chunk(name, stride, payload=b''):
        return struct.pack('<20sIII', name.encode(), 1999801, stride, len(payload) // stride if stride else 0) + payload
    bone = struct.pack('<64sIII11f', b'Root', 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1)
    for frames in (1, 2):
        info = struct.pack('<64s64s4I3f3I', b'Pose' if frames == 1 else b'Move', b'Test',
                           1, 0, 0, 0, 0, float(frames), 30, 0, 0, frames)
        keys = b''.join(struct.pack('<8f', float(i), 0, 0, 0, 0, 0, 1, 1 / 30) for i in range(frames))
        psa = chunk('ANIMHEAD', 0) + chunk('BONENAMES', 120, bone) + chunk('ANIMINFO', 168, info) + chunk('ANIMKEYS', 32, keys)
        (root / ('single-key.psa' if frames == 1 else 'animated.psa')).write_bytes(psa)
    (root / 'invalid.fbx').write_bytes(b'not an FBX')
    return root


if __name__ == '__main__':
    print(generate(sys.argv[1]))
