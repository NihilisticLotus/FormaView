"""Full affine skin bindings: duplicate names, ASCII/binary input, axes and units."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def multiply(a, b):
    return [[sum(a[r][k]*b[k][c] for k in range(4)) for c in range(4)] for r in range(4)]


def inverse(a):
    rows=[list(row)+[float(r==c) for c in range(4)] for r,row in enumerate(a)]
    for c in range(4):
        pivot=max(range(c,4),key=lambda r:abs(rows[r][c]))
        rows[c],rows[pivot]=rows[pivot],rows[c]
        scale=rows[c][c]
        assert abs(scale)>1e-15
        rows[c]=[v/scale for v in rows[c]]
        for r in range(4):
            if r!=c:
                scale=rows[r][c];rows[r]=[x-scale*y for x,y in zip(rows[r],rows[c])]
    return [row[4:] for row in rows]


IDENTITY=[[float(r==c) for c in range(4)] for r in range(4)]


def fixture(path):
    # Deliberate shear must survive; a TRS reconstruction cannot represent it.
    links=[[[2,.03,0,10],[0,3,.02,-4],[.01,0,4,7],[0,0,0,1]],
           [[1,-.04,.02,-2],[.01,2,0,5],[0,.03,3,11],[0,0,0,1]]]
    array=lambda m:','.join(format(m[r][c],'.17g') for c in range(4) for r in range(4))
    clusters=''
    for i,link in enumerate(links):
        clusters+=f'''Deformer: {8+i}, "SubDeformer::Same", "Cluster" {{
 Version: 100
 Indexes: *3 {{ a: 0,1,2 }}
 Weights: *3 {{ a: .5,.5,.5 }}
 Transform: *16 {{ a: {array(inverse(link))} }}
 TransformLink: *16 {{ a: {array(link)} }}
}}
'''
    text='''; FBX 7.4.0 project file
FBXHeaderExtension: { FBXHeaderVersion: 1003
 FBXVersion: 7400
}
GlobalSettings: { Version: 1000
 Properties70: {
  P: "UpAxis", "int", "Integer", "",1
  P: "UpAxisSign", "int", "Integer", "",1
  P: "FrontAxis", "int", "Integer", "",2
  P: "FrontAxisSign", "int", "Integer", "",1
  P: "CoordAxis", "int", "Integer", "",0
  P: "CoordAxisSign", "int", "Integer", "",1
  P: "UnitScaleFactor", "double", "Number", "",1
 }
}
Definitions: { Version: 100
 Count: 9
 ObjectType: "Model" { Count: 5 }
 ObjectType: "Geometry" { Count: 1 }
 ObjectType: "Deformer" { Count: 3 }
}
Objects: {
 Model: 1, "Model::Mesh", "Mesh" { Version: 232 }
 Geometry: 2, "Geometry::Mesh", "Mesh" {
  Vertices: *9 { a: 0,0,0,2,0,0,0,3,0 }
  PolygonVertexIndex: *3 { a: 0,1,-3 }
 }
 Model: 3, "Model::Same", "LimbNode" { Version: 232 }
 Model: 4, "Model::Same", "LimbNode" { Version: 232 }
 Model: 5, "Model::Left", "Null" { Version: 232 }
 Model: 6, "Model::Right", "Null" { Version: 232 }
 Deformer: 7, "Deformer::Skin", "Skin" { Version: 101 }
CLUSTERS
}
Connections: {
 C: "OO",1,0
 C: "OO",2,1
 C: "OO",5,0
 C: "OO",6,0
 C: "OO",3,5
 C: "OO",4,6
 C: "OO",7,2
 C: "OO",8,7
 C: "OO",9,7
 C: "OO",3,8
 C: "OO",4,9
}
'''.replace('CLUSTERS',clusters).replace('{ ','{\n ').replace(' }','\n }')
    path.write_text(text,encoding='utf-8')


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--cli',type=Path)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    source=args.output/'sheared.fbx';fixture(source)
    before=hashlib.sha256(source.read_bytes()).hexdigest()
    scope=args.output/'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],output_roots=[str(args.output.resolve())],allow_overwrite=False,policy_revision='binding-precision')))
    cli=args.cli or args.build/'Release/formaview_image_cli.exe'
    probe=args.build/'tests/Release/formaview_scene_export_probe.exe'
    def inspect(path):
        report=path.with_suffix('.bind.json')
        subprocess.run([str(probe),str(path),str(report),'--bind-only'],check=True,timeout=60)
        return json.loads(report.read_text())
    def export(path,output,axes='source',unit='source'):
        request=dict(version=1,input=str(path.resolve()),output=str(output.resolve()),options=dict(target='fbx',include_mesh=True,output_axes=axes,output_unit=unit),conflict='skip')
        for operation in ['preflight','convert']:
            request['operation']=operation
            p=subprocess.run([str(cli),'--scope',str(scope)],input=json.dumps(request)+'\n',capture_output=True,text=True,encoding='utf-8',timeout=60)
            result=json.loads(p.stdout)
            (output.with_suffix('.'+operation+'.json')).write_text(json.dumps(result,indent=2))
            assert p.returncode==0 and 'error' not in result,result
            if operation=='preflight':
                assert not result['losses'],result['losses']
                request['approval_digest']=result['approval_digest']
        assert b'__FormaViewBinding_' not in output.read_bytes(),'Temporary cluster names leaked'
    original=inspect(source);assert len(original['meshes'][0]['clusters'])==2
    maxima={}
    def matrix(value):return [value[i:i+4] for i in range(0,16,4)]
    def check(name,actual,expected,tol=1e-10):
        delta=max(abs(x-y) for a,b in zip(actual,expected) for x,y in zip(a,b))
        maxima[name]=max(maxima.get(name,0),delta)
        assert delta<=tol,(name,delta,output.name)
    # These bases are the explicit signed axis permutations for the presets.
    bases={'source':IDENTITY,'maya':IDENTITY,'houdini':IDENTITY,
           'blender':[[1,0,0,0],[0,0,-1,0],[0,1,0,0],[0,0,0,1]],
           '3ds-max':[[1,0,0,0],[0,0,-1,0],[0,1,0,0],[0,0,0,1]],
           'unity':[[1,0,0,0],[0,1,0,0],[0,0,-1,0],[0,0,0,1]],
           'cinema4d':[[1,0,0,0],[0,1,0,0],[0,0,-1,0],[0,0,0,1]],
           'unreal':[[0,0,-1,0],[1,0,0,0],[0,1,0,0],[0,0,0,1]]}
    output=args.output/'source.fbx';export(source,output)
    for axes,c in bases.items():
        for unit,factor in [('source',1),('m',.01),('mm',10),('in',1/2.54)]:
            output=args.output/f'{axes}-{unit}.fbx';export(source,output,axes,unit)
            result=inspect(output)
            assert len(result['meshes'])==1 and len(result['meshes'][0]['clusters'])==2
            scale=[[factor if r==c and r<3 else float(r==c) for c in range(4)] for r in range(4)]
            for a,b in zip(original['meshes'][0]['clusters'],result['meshes'][0]['clusters']):
                assert a['bone']==b['bone'] and a['indices']==b['indices'] and a['weights']==b['weights']
                check('inverse_bind',matrix(b['geometry_to_bone']),multiply(multiply(c,matrix(a['geometry_to_bone'])),inverse(c)))
                check('bind_world',matrix(b['bind_to_world']),multiply(multiply(multiply(scale,c),matrix(a['bind_to_world'])),inverse(c)))
    # A binary input must retain the same full matrices on a second export.
    output=args.output/'binary-roundtrip.fbx';export(args.output/'source.fbx',output)
    actual=inspect(output)
    for a,b in zip(original['meshes'][0]['clusters'],actual['meshes'][0]['clusters']):
        for key in ['bind_to_world','geometry_to_bone']:check('binary_'+key,matrix(b[key]),matrix(a[key]))
    # Reimport a different handedness and unit, then return to the source basis.
    output=args.output/'unreal-back-to-maya.fbx'
    export(args.output/'unreal-in.fbx',output,'maya','cm')
    actual=inspect(output)
    assert len(actual['meshes'][0]['clusters'])==2
    for a,b in zip(original['meshes'][0]['clusters'],actual['meshes'][0]['clusters']):
        for key in ['bind_to_world','geometry_to_bone']:check('axes_roundtrip_'+key,matrix(b[key]),matrix(a[key]))
    assert hashlib.sha256(source.read_bytes()).hexdigest()==before
    (args.output/'passed.json').write_text(json.dumps(dict(passed=True,duplicate_names=True,source_unchanged=True,maximum_errors=maxima),indent=2))
    print('PASS exact bindings, duplicate names, binary reimport and 32 axes/unit combinations',maxima)


if __name__=='__main__':main()
