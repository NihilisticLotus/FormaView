"""Synthetic source fixtures and independent ufbx readback for M08 scene exports."""
import argparse
import base64
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import zlib


def fixtures(root):
    root.mkdir(parents=True, exist_ok=True)
    def chunk(kind, data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data))
    png = b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1,1,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0\xff\0\0\xff'))+chunk(b'IEND',b'')
    (root / 'pixel.png').write_bytes(png)
    (root / 'source.mtl').write_text('newmtl Red\nKd 0.8 0.2 0.1\nNs 25\nmap_Kd pixel.png\n', encoding='utf-8')
    (root / 'source.obj').write_text('mtllib source.mtl\nv 0 0 0\nv 2 0 0\nv 0 3 0\nvt 0 0\nvt 1 0\nvt 0 1\nvn 0 0 1\nusemtl Red\ns 1\nf -3/1/1 -2/2/1 -1/3/1\n', encoding='utf-8')
    binary = bytearray()
    views, accessors = [], []

    def array(values, kind, shape):
        while len(binary) % 4:
            binary.append(0)
        flat = [x for row in values for x in row] if isinstance(values[0], (list, tuple)) else values
        payload = struct.pack('<' + {5126: 'f', 5123: 'H', 5121: 'B'}[kind] * len(flat), *flat)
        views.append(dict(buffer=0, byteOffset=len(binary), byteLength=len(payload)))
        binary.extend(payload)
        accessors.append(dict(bufferView=len(views)-1, componentType=kind, type=shape, count=len(values)))
        return len(accessors)-1

    position = array([[0,0,0],[2,0,0],[0,3,0]], 5126, 'VEC3')
    accessors[position].update(min=[0,0,0], max=[2,3,0])
    normal = array([[0,0,1]]*3, 5126, 'VEC3')
    uv = array([[0,0],[1,0],[0,1]], 5126, 'VEC2')
    indices = array([0,1,2], 5123, 'SCALAR')
    joints0 = array([[0,1,2,3]]*3, 5121, 'VEC4')
    joints1 = array([[4,5,0,0]]*3, 5121, 'VEC4')
    weights0 = array([[.1,.15,.2,.25]]*3, 5126, 'VEC4')
    weights1 = array([[.2,.1,0,0]]*3, 5126, 'VEC4')
    matrices = [[1,0,0,0,0,1,0,0,0,0,1,0,0,-i,0,1] for i in range(6)]
    bind = array(matrices, 5126, 'MAT4')
    delta = array([[0,0,1]]*3, 5126, 'VEC3')
    time_a = array([2,3,4], 5126, 'SCALAR')
    translation = array([[0,0,0],[1,0,0],[2,0,0]], 5126, 'VEC3')
    rotation = array([[0,0,0,1],[0,0,math.sin(.35),math.cos(.35)],[0,0,math.sin(.7),math.cos(.7)]], 5126, 'VEC4')
    morph = array([0, .5, 1], 5126, 'SCALAR')
    time_b = array([7,9], 5126, 'SCALAR')
    cubic = array([[0,0,0],[0,0,0],[0,2,0],[0,-2,0],[0,0,0],[0,0,0]], 5126, 'VEC3')
    scale = array([[1,1,1],[2,2,2]], 5126, 'VEC3')
    data = dict(asset=dict(version='2.0', generator='FormaView synthetic test'),
        scene=0, scenes=[dict(nodes=[0,1,2,3,4,5,6])],
        nodes=[dict(name='Mesh',mesh=0,skin=0)] + [dict(name='Joint'+str(i),translation=[0,i,0]) for i in range(6)],
        skins=[dict(joints=list(range(1,7)),inverseBindMatrices=bind)],
        meshes=[dict(name='Triangle',weights=[0],extras=dict(targetNames=['Lift']),primitives=[dict(
            attributes=dict(POSITION=position,NORMAL=normal,TEXCOORD_0=uv,JOINTS_0=joints0,WEIGHTS_0=weights0,JOINTS_1=joints1,WEIGHTS_1=weights1),
            indices=indices,material=0,targets=[dict(POSITION=delta)])])],
        materials=[dict(name='Red',pbrMetallicRoughness=dict(baseColorFactor=[.8,.2,.1,1],metallicFactor=.25,roughnessFactor=.4,baseColorTexture=dict(index=0)))],
        textures=[dict(source=0)],images=[dict(uri='pixel.png')],
        animations=[dict(name='Move',samplers=[dict(input=time_a,output=translation),dict(input=time_a,output=rotation),dict(input=time_a,output=morph)],
            channels=[dict(sampler=0,target=dict(node=1,path='translation')),dict(sampler=1,target=dict(node=1,path='rotation')),dict(sampler=2,target=dict(node=0,path='weights'))]),
            dict(name='Bounce',samplers=[dict(input=time_b,output=cubic,interpolation='CUBICSPLINE'),dict(input=time_b,output=scale,interpolation='STEP')],
            channels=[dict(sampler=0,target=dict(node=1,path='translation')),dict(sampler=1,target=dict(node=2,path='scale'))])],
        buffers=[dict(uri='source.bin',byteLength=len(binary))],bufferViews=views,accessors=accessors)
    (root / 'source.bin').write_bytes(binary)
    (root / 'source.gltf').write_text(json.dumps(data), encoding='utf-8')
    data['buffers'][0].pop('uri')
    data['images'][0]['uri'] = 'data:image/png;base64,' + base64.b64encode(png).decode()
    text = json.dumps(data).encode()
    text += b' ' * (-len(text) % 4)
    binary += b'\0' * (-len(binary) % 4)
    glb = struct.pack('<III', 0x46546C67, 2, 28+len(text)+len(binary)) + struct.pack('<II',len(text),0x4E4F534A)+text+struct.pack('<II',len(binary),0x004E4942)+binary
    (root / 'source.glb').write_bytes(glb)
    vrm = json.loads(json.dumps(data))
    vrm['extensionsUsed'] = ['VRMC_vrm']
    vrm['extensions'] = {'VRMC_vrm': {'specVersion':'1.0','meta':{'name':'Synthetic avatar','authors':['FormaView test'],'licenseUrl':'https://vrm.dev/licenses/1.0/'}}}
    text = json.dumps(vrm).encode(); text += b' ' * (-len(text) % 4)
    (root / 'source.vrm').write_bytes(struct.pack('<III',0x46546C67,2,28+len(text)+len(binary))+struct.pack('<II',len(text),0x4E4F534A)+text+struct.pack('<II',len(binary),0x004E4942)+binary)
    pmx=bytearray(b'PMX '+struct.pack('<fB',2.1,8)+bytes([1,1,1,1,1,1,1,1]))
    def put(fmt,*values): pmx.extend(struct.pack('<'+fmt,*values))
    def string(value):
        content=value.encode('utf-8');put('i',len(content));pmx.extend(content)
    for value in ('测试模型','Test model','Synthetic PMX fixture','Known geometry and bind'):string(value)
    put('i',3)
    for i,point in enumerate(((0,0,0),(2,0,0),(0,3,0))):
        put('3f',*point);put('3f',0,0,1);put('2f',i==1,i==2);put('4f',.1,.2,.3,.4)
        if i==1:
            put('Bbbf',3,0,1,.25);put('9f',*([0]*9))
        else:put('B4b4f',2 if i==0 else 4,0,1,2,3,.1,.2,.3,.4)
        put('f',1)
    put('i3B',3,0,1,2)
    put('i',1);string('pixel.png')
    put('i',1);string('红色');string('Red')
    put('4f',.8,.2,.1,1);put('3f',.3,.3,.3);put('f',25);put('3f',.1,.1,.1)
    put('B4ffbbBB',1,0,0,0,1,1,0,-1,0,1);put('B',0);string('Material comment');put('i',3)
    put('i',4)
    for i in range(4):
        string('骨骼'+str(i));string('Bone'+str(i));put('3f',0,i,0);put('biH',-1,0,0);put('3f',0,1,0)
    put('i',3)
    string('抬起');string('Lift');put('BBi',1,1,3)
    for i in range(3):put('B3f',i,0,0,1)
    string('半抬起');string('Half lift');put('BBibf',1,0,1,0,.5)
    string('UV变化');string('UV shift');put('BBiB4f',1,3,1,0,.1,0,0,0)
    put('i',0) # display frames
    put('i',1);string('刚体');string('Rigid');put('bBHB',0,0,0,0);put('14f',*([1]*14));put('B',0)
    put('ii',0,0) # joints and soft bodies
    (root/'source.pmx').write_bytes(pmx)


def close(actual, expected, tolerance=3e-5):
    if isinstance(expected, list):
        assert len(actual)==len(expected),(actual,expected)
        for a,b in zip(actual,expected):close(a,b,tolerance)
    else:assert abs(actual-expected)<=tolerance,(actual,expected)


def verify(report, suffix, png_hash):
    assert len(report['meshes'])==1,report
    mesh=report['meshes'][0]
    close(mesh['points'],[[0,0,0],[2,0,0],[0,3,0]])
    assert mesh['corners']==[0,1,2]
    close(mesh['uv'],[[0,0],[1,0],[0,1]] if suffix=='obj' else [[0,1],[1,1],[0,0]])
    close(mesh['normals'],[[0,0,1]]*3)
    close(mesh['materials'][0]['diffuse'],[.8,.2,.1])
    assert any(m['sha256']==png_hash for m in report['media']),report['media']
    if suffix=='obj':assert not report['clips'];return
    if suffix=='pmx':
        assert len(mesh['clusters'])==4
        close(mesh['shapes'][0]['offsets'],[[0,0,1]]*3)
        close(mesh['shapes'][1]['offsets'],[[0,0,.5]]*3)
        for i,c in enumerate(mesh['clusters']):close(c['geometry_to_bone'][7],-i)
        return
    assert len(mesh['clusters'])==6
    for i,c in enumerate(mesh['clusters']):
        assert c['indices']==[0,1,2]
        close(c['weights'],[[.1,.15,.2,.25,.2,.1][i]]*3)
        close(c['geometry_to_bone'][7],-i)
    close(mesh['shapes'][0]['offsets'],[[0,0,1]]*3)
    assert [(c['name'],round(c['start']),round(c['end'])) for c in report['clips']]==[('Move',2,4),('Bounce',7,9)]
    for clip in report['clips']:
        for sample in clip['samples']:
            t=sample['time'];nodes={n['name']:n['world'] for n in sample['nodes']}
            if clip['name']=='Move':
                angle=(t-2)*.7
                close(nodes['Joint0'],[math.cos(angle),-math.sin(angle),0,t-2,math.sin(angle),math.cos(angle),0,0,0,0,1,0,0,0,0,1])
                close(sample['meshes'][0]['shapes'][0]['weight'],(t-2)/2)
                expected=[]
                for x,y,z in mesh['points']:
                    expected.append([.9*x+.1*(x*math.cos(angle)-y*math.sin(angle)+t-2),.9*y+.1*(x*math.sin(angle)+y*math.cos(angle)),z+(t-2)/2])
                close(sample['meshes'][0]['deformed'],expected)
            else:
                u=(t-7)/2
                close(nodes['Joint0'][7],4*u*(1-u))
                close(nodes['Joint1'][0],2 if t==9 else 1)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    source=args.output/'fixtures'; fixtures(source)
    scope=args.output/'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output)],output_roots=[str(args.output)],allow_overwrite=True,policy_revision='scene-tests')),encoding='utf-8')
    records=[]
    def call(operation,expect_error=False,**values):
        request=dict(version=1,operation=operation,**values)
        p=subprocess.run([str(args.build/'Release/formaview_image_cli.exe'),'--scope',str(scope)],input=json.dumps(request)+'\n',text=True,capture_output=True,encoding='utf-8',timeout=60)
        if p.stderr:
            with (args.output/'native-stderr.log').open('a',encoding='utf-8') as log:log.write(p.stderr)
        assert p.returncode in (0,1),(p.returncode,p.stdout,p.stderr)
        result=json.loads(p.stdout)
        records.append(dict(request=request,response=result))
        (args.output/'requests.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
        assert ('error' in result)==expect_error,(p.returncode,result,p.stderr)
        return result
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir()}
    outputs=[]
    detailed={}
    def probe(path):
        report=Path(path).with_suffix('.detail.json')
        subprocess.run([str(args.build/'tests/Release/formaview_scene_export_probe.exe'),str(path),str(report)],check=True,timeout=30)
        return json.loads(report.read_text(encoding='utf-8'))
    png_hash=hashlib.sha256((source/'pixel.png').read_bytes()).hexdigest()
    for ext in ('obj','gltf','glb','vrm','pmx'):
        inspected=call('inspect',input=str(source/('source.'+ext)))
        options=dict(target='fbx')
        if inspected['clips']: options['clip_ids']=[c['id'] for c in inspected['clips']]
        request=dict(input=str(source/('source.'+ext)),output=str(args.output/(ext+'.fbx')),options=options)
        preflight=call('preflight',**request)
        assert not Path(request['output']).exists()
        call('convert',**request,approval_digest=preflight['approval_digest'],acknowledge_losses=True)
        outputs.append(request['output'])
        report=args.output/(ext+'-readback.json')
        subprocess.run([str(args.build/'tests/Release/formaview_skeletal_probe.exe'),request['output'],'-','-',str(report)],check=True,timeout=30)
        readback=json.loads(report.read_text())
        assert readback['meshes']==1,readback
        if ext not in ('obj','pmx'):
            assert [(c['name'],round(c['start']),round(c['end'])) for c in readback['clips']]==[('Move',2,4),('Bounce',7,9)],readback
        detailed[ext]=probe(request['output']);verify(detailed[ext],ext,png_hash)
    inspected=call('inspect',input=str(args.output/'glb.fbx'))
    for clip in inspected['clips']:
        request=dict(input=str(args.output/'glb.fbx'),output=str(args.output/(clip['name']+'.fbx')),options=dict(target='fbx',clip_id=clip['id']))
        preflight=call('preflight',**request)
        call('convert',**request,approval_digest=preflight['approval_digest'],acknowledge_losses=True)
        outputs.append(request['output'])
        result=probe(request['output'])
        expected=next(c for c in detailed['glb']['clips'] if c['name']==clip['name'])
        assert len(result['clips'])==1
        for actual,sample in zip(result['clips'][0]['samples'],expected['samples']):
            close(actual['time'],sample['time'])
            for a,b in zip(actual['nodes'],sample['nodes']):close(a['world'],b['world'])
            close(actual['meshes'][0]['deformed'],sample['meshes'][0]['deformed'])
            close(actual['meshes'][0]['shapes'][0]['weight'],sample['meshes'][0]['shapes'][0]['weight'])
        assert any(m['sha256']==png_hash for m in result['media'])
    assert hashes=={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in source.iterdir()}
    # Physical dimensions and source root motion survive output-axis/unit conversion.
    inspected=call('inspect',input=str(source/'source.glb'))
    request=dict(input=str(source/'source.glb'),output=str(args.output/'unreal-cm.fbx'),options=dict(target='fbx',clip_ids=[c['id'] for c in inspected['clips']],output_axes='unreal',output_unit='cm'))
    preflight=call('preflight',**request);call('convert',**request,approval_digest=preflight['approval_digest'],acknowledge_losses=True)
    converted=probe(request['output']);close(converted['unit'],.01)
    for before,after in zip(detailed['glb']['clips'],converted['clips']):
        for a,b in zip(before['samples'],after['samples']):
            am=a['meshes'][0];bm=b['meshes'][0]
            av=[am['deformed'][am['corners'].index(i)] for i in range(3)]
            bv=[bm['deformed'][bm['corners'].index(i)] for i in range(3)]
            for i in range(3):
                for j in range(3):close(math.dist(av[i],av[j]),math.dist(bv[i],bv[j])*.01,1e-4)
    # Invalid inputs must fail without creating output files.
    bad=source/'truncated.pmx';bad.write_bytes((source/'source.pmx').read_bytes()[:35])
    invalid=dict(input=str(bad),output=str(args.output/'invalid.fbx'),options=dict(target='fbx'))
    call('preflight',expect_error=True,**invalid);assert not Path(invalid['output']).exists()
    invalid['input']=str(source/'source.glb');invalid['options']['clip_id']='nonexistent'
    call('preflight',expect_error=True,**invalid);assert not Path(invalid['output']).exists()
    invalid['options']=dict(target='fbx',include_mesh='false')
    call('preflight',expect_error=True,**invalid)
    (args.output/'passed.json').write_text(json.dumps(dict(passed=True,outputs=outputs)),encoding='utf-8')
    print('Scene smoke exports passed:',len(outputs))


if __name__=='__main__': main()
