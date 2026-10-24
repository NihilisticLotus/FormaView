"""External FBX media relocation, independent readback and source protection."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

from scene_export_tests import fixtures


def external_fbx(path, absolute, relative):
    # ASCII FBX keeps the two producer paths explicit and independent of the
    # exporter under test. Every field starts on its own line for the SDK reader.
    path.write_text('''; FBX 7.4.0 project file
FBXHeaderExtension: {
 FBXHeaderVersion: 1003
 FBXVersion: 7400
}
Definitions: {
 Version: 100
 Count: 5
 ObjectType: "Model" { Count: 1 }
 ObjectType: "Geometry" { Count: 1 }
 ObjectType: "Material" { Count: 1 }
 ObjectType: "Texture" { Count: 1 }
 ObjectType: "Video" { Count: 1 }
}
Objects: {
 Geometry: 1, "Geometry::Triangle", "Mesh" {
  Vertices: *9 { a: 0,0,0,2,0,0,0,3,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
  LayerElementMaterial: 0 {
   Version: 101
   Name: ""
   MappingInformationType: "AllSame"
   ReferenceInformationType: "IndexToDirect"
   Materials: *1 { a: 0 }
  }
  Layer: 0 {
   Version: 100
   LayerElement: {
    Type: "LayerElementMaterial"
    TypedIndex: 0
   }
  }
 }
 Model: 2, "Model::Triangle", "Mesh" { Version: 232 }
 Material: 3, "Material::Paint", "" {
  Version: 102
  ShadingModel: "phong"
  MultiLayer: 0
  Properties70: { P: "DiffuseColor", "Color", "", "A",1,1,1 }
 }
 Texture: 4, "Texture::Color", "" {
  Type: "TextureVideoClip"
  Version: 202
  TextureName: "Texture::Color"
  Media: "Video::Color"
  FileName: "ABSOLUTE"
  RelativeFilename: "RELATIVE"
  ModelUVTranslation: 0,0
  ModelUVScaling: 1,1
  Texture_Alpha_Source: "None"
  Cropping: 0,0,0,0
 }
 Video: 5, "Video::Color", "Clip" {
  Type: "Clip"
  Filename: "ABSOLUTE"
  RelativeFilename: "RELATIVE"
 }
}
Connections: {
 C: "OO",1,2
 C: "OO",2,0
 C: "OO",3,2
 C: "OP",4,3,"DiffuseColor"
 C: "OO",5,4
}
'''.replace('ABSOLUTE', absolute.as_posix()).replace('RELATIVE', relative)
        .replace('{ ', '{\n ').replace(' }', '\n }'), encoding='utf-8')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = args.output / 'fixtures'
    fixtures(source)
    original = (source / 'pixel.png').read_bytes()
    source_hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir()}
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(source)], output_roots=[str(args.output)],
                                    allow_overwrite=True, policy_revision='fbx-media-tests')))
    records, results = [], []

    def call(operation, **kwargs):
        request = dict(version=1, operation=operation, **kwargs)
        process = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(scope)],
                                 input=json.dumps(request) + '\n', capture_output=True, text=True,
                                 encoding='utf-8', timeout=60)
        records.append(dict(request=request, returncode=process.returncode,
                            stdout=process.stdout, stderr=process.stderr))
        assert process.returncode in (0, 1), (process.returncode, process.stderr)
        return json.loads(process.stdout)

    def request(model, name):
        return dict(input=str(model), output=str(args.output / (name + '.fbx')), options=dict(target='fbx'))

    def exported(req):
        preflight = call('preflight', **req)
        assert 'error' not in preflight, preflight
        assert not any(x['code'] == 'external_texture_unavailable' for x in preflight['losses']), preflight
        result = call('convert', **req, approval_digest=preflight['approval_digest'], acknowledge_losses=True)
        assert 'error' not in result, result
        report = Path(req['output']).with_suffix('.json')
        subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'),
                        req['output'], str(report)], check=True, timeout=30)
        actual = json.loads(report.read_text(encoding='utf-8'))
        assert len(actual['meshes']) == 1 and len(actual['meshes'][0]['points']) == 3, actual
        assert len(actual['media']) == 1, actual
        assert actual['media'][0]['sha256'] == hashlib.sha256(original).hexdigest(), actual['media']
        return preflight

    moved = source / 'relocated.fbx'
    stale = source / 'previous-location/pixel.png'
    external_fbx(moved, stale, 'pixel.png')

    def relative_fallback():
        result = exported(request(moved, 'relocated-output'))
        assert any(Path(s['path']) == source / 'pixel.png' for s in result['sources']), result
        assert str(stale.resolve()).lower() in [str(Path(s).resolve()).lower() for s in result['referenced_sources']]

    def primary_precedence():
        model = source / 'primary.fbx'
        external_fbx(model, source / 'pixel.png', 'missing-relative.png')
        exported(request(model, 'primary-output'))

    def literal_percent_path():
        folder = source / 'media%20literal'
        folder.mkdir()
        (folder / 'pixel.png').write_bytes(original)
        model = source / 'literal-percent.fbx'
        external_fbx(model, stale, 'media%20literal/pixel.png')
        exported(request(model, 'literal-percent-output'))

    def changed_dependency():
        req = request(moved, 'changed-output')
        plan = call('preflight', **req)
        assert 'error' not in plan, plan
        texture = source / 'pixel.png'
        texture.write_bytes(original + b'changed')
        try:
            result = call('convert', **req, approval_digest=plan['approval_digest'], acknowledge_losses=True)
            assert 'error' in result, result
            assert not Path(req['output']).exists()
        finally:
            texture.write_bytes(original)

    def source_protection():
        texture = source / 'reserved.fbx'
        texture.write_bytes(original)
        model = source / 'protected.fbx'
        external_fbx(model, stale, texture.name)
        result = call('preflight', input=str(model), output=str(texture), conflict='overwrite', options=dict(target='fbx'))
        assert 'error' in result, result
        assert texture.read_bytes() == original

    def out_of_scope():
        texture = args.output / 'outside.fbx'
        texture.write_bytes(original)
        model = source / 'out-of-scope.fbx'
        external_fbx(model, stale, '../outside.fbx')
        result = call('inspect', input=str(model))
        assert 'error' not in result and result['has_mesh'], result
        assert any(x['code'] == 'external_texture_unavailable' for x in result['losses']), result
        assert not any(Path(s['path']) == texture for s in result['sources']), result
        result = call('preflight', input=str(model), output=str(texture), conflict='overwrite', options=dict(target='fbx'))
        assert 'error' in result, result
        assert texture.read_bytes() == original

    try:
        for name, test in [('relative_fallback', relative_fallback), ('primary_precedence', primary_precedence),
                           ('literal_percent_path', literal_percent_path), ('changed_dependency', changed_dependency),
                           ('source_protection', source_protection), ('out_of_scope', out_of_scope)]:
            try:
                test()
                results.append(dict(name=name, passed=True))
            except Exception as error:
                results.append(dict(name=name, passed=False, error=str(error)))
            print(name, 'PASS' if results[-1]['passed'] else 'FAIL', flush=True)
        assert all(r['passed'] for r in results), results
        assert source_hashes == {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in source_hashes}
    finally:
        (args.output / 'results.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
        (args.output / 'requests.json').write_text(json.dumps(records, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
