"""Deterministic, original model fixtures. Output must be outside the source tree."""
from pathlib import Path
import base64,json,struct,sys,zlib
from urllib.parse import unquote

root=Path(sys.argv[1]).resolve()
repo=Path(__file__).resolve().parents[1]
if root==repo or repo in root.parents:raise SystemExit('Use an external fixture directory')
root.mkdir(parents=True,exist_ok=False)
def png():
    def chunk(kind,payload):return struct.pack('>I',len(payload))+kind+payload+struct.pack('>I',zlib.crc32(kind+payload))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',2,2,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b'\0'+bytes([255,0,0,255,0,255,0,255])+b'\0'+bytes([0,0,255,255,255,255,255,255])))+chunk(b'IEND',b'')
def write(name,doc):
    def buffer_bytes(index):
        uri=doc['buffers'][index]['uri']
        return base64.b64decode(uri.split(',',1)[1]) if uri.startswith('data:') else (root/unquote(uri)).read_bytes()
    def position_values(index):
        accessor=doc['accessors'][index];values=[[0.,0.,0.] for _ in range(accessor['count'])]
        if 'bufferView' in accessor:
            view=doc['bufferViews'][accessor['bufferView']];data=buffer_bytes(view['buffer']);offset=view.get('byteOffset',0)+accessor.get('byteOffset',0);stride=view.get('byteStride',12)
            values=[list(struct.unpack_from('<3f',data,offset+i*stride)) for i in range(accessor['count'])]
        if 'sparse' in accessor:
            sparse=accessor['sparse'];indices=sparse['indices'];view=doc['bufferViews'][indices['bufferView']];index_data=buffer_bytes(view['buffer']);offset=view.get('byteOffset',0)+indices.get('byteOffset',0);kind={5121:'B',5123:'H',5125:'I'}[indices['componentType']]
            encoded=sparse['values'];view=doc['bufferViews'][encoded['bufferView']];data=buffer_bytes(view['buffer']);value_offset=view.get('byteOffset',0)+encoded.get('byteOffset',0)
            for i in range(sparse['count']):
                target=struct.unpack_from('<'+kind,index_data,offset+i*struct.calcsize(kind))[0]
                values[target]=list(struct.unpack_from('<3f',data,value_offset+i*12))
        return values
    for mesh in doc.get('meshes',[]):
        for primitive in mesh.get('primitives',[]):
            for attributes in [primitive['attributes']]+primitive.get('targets',[]):
                if 'POSITION' not in attributes:continue
                position=doc['accessors'][attributes['POSITION']]
                values=position_values(attributes['POSITION'])
                position['min']=[min(v[c] for v in values) for c in range(3)]
                position['max']=[max(v[c] for v in values) for c in range(3)]
    (root/name).write_text(json.dumps(doc,ensure_ascii=False),encoding='utf-8')
def base():return {'asset':{'version':'2.0'},'scenes':[{'nodes':[0]}],'scene':0,'nodes':[{'mesh':0}],'meshes':[{'primitives':[{'attributes':{}}]}],'bufferViews':[],'accessors':[]}
def add(doc,binary,values,fmt,kind,component,normalized=False,stride=None):
    while len(binary)%4:binary.extend(b'\0')
    offset=len(binary);binary.extend(struct.pack('<'+fmt,*values));view=len(doc['bufferViews']);doc['bufferViews'].append({'buffer':0,'byteOffset':offset,'byteLength':len(binary)-offset})
    if stride:doc['bufferViews'][-1]['byteStride']=stride
    components={'SCALAR':1,'VEC2':2,'VEC3':3,'VEC4':4,'MAT4':16}[kind]
    a={'bufferView':view,'componentType':component,'count':len(values)//components,'type':kind}
    if normalized:a['normalized']=True
    doc['accessors'].append(a);return len(doc['accessors'])-1
doc=base();binary=bytearray();attrs=doc['meshes'][0]['primitives'][0]['attributes']
attrs['POSITION']=add(doc,binary,[0,0,0,1,0,0,0,1,0],'9f','VEC3',5126)
attrs['TEXCOORD_0']=add(doc,binary,[0,0,2,0,0,-1],'6f','VEC2',5126)
attrs['TEXCOORD_1']=add(doc,binary,[.25,.25,.75,.25,.25,.75],'6f','VEC2',5126)
doc['nodes']=[{'name':'Parent','translation':[3,4,5],'children':[1]},{'name':'Mesh','mesh':0,'scale':[2,2,2]}]
doc['materials']=[{'name':'Linear factor','pbrMetallicRoughness':{'baseColorFactor':[.25,.5,.75,1],'baseColorTexture':{'index':0,'texCoord':1}},'extensions':{'KHR_materials_unlit':{}}}]
doc['meshes'][0]['primitives'][0]['material']=0;doc['textures']=[{'source':0}];doc['images']=[{'uri':'data:image/png;base64,'+base64.b64encode(png()).decode()}];doc['extensionsUsed']=['KHR_materials_unlit'];doc['extensionsRequired']=['KHR_materials_unlit']
doc['buffers']=[{'uri':'%E9%A1%B6%E7%82%B9%E6%95%B0%E6%8D%AE.bin','byteLength':len(binary)}];(root/'顶点数据.bin').write_bytes(binary);write('external.gltf',doc)
glb=json.loads(json.dumps(doc));glb['buffers'][0].pop('uri');glb_binary=bytearray(binary)
while len(glb_binary)%4:glb_binary.append(0)
glb['images']=[{'bufferView':len(glb['bufferViews']),'mimeType':'image/png'}];glb['bufferViews'].append({'buffer':0,'byteOffset':len(glb_binary),'byteLength':len(png())});glb_binary.extend(png());glb['buffers'][0]['byteLength']=len(glb_binary)
encoded=json.dumps(glb).encode();encoded+=b' '*(3-(len(encoded)-1)%4);blob=bytes(glb_binary)+b'\0'*(-len(glb_binary)%4)
(root/'embedded.glb').write_bytes(struct.pack('<III',0x46546c67,2,12+8+len(encoded)+8+len(blob))+struct.pack('<II',len(encoded),0x4e4f534a)+encoded+struct.pack('<II',len(blob),0x004e4942)+blob)
material=json.loads(json.dumps(doc));extra=bytearray(binary)
material['meshes'][0]['primitives'][0]['attributes']['COLOR_0']=add(material,extra,[128,255,0,128]*3,'12B','VEC4',5121,True)
material['buffers']=[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(extra).decode(),'byteLength':len(extra)}]
material['materials'][0]['alphaMode']='MASK';material['materials'][0]['alphaCutoff']=.3
material['materials'][0]['pbrMetallicRoughness']['baseColorFactor'][3]=.5
material['samplers']=[{'magFilter':9728,'minFilter':9985,'wrapS':33071,'wrapT':33648}];material['textures'][0]['sampler']=0
write('coverage-mask.gltf',material)
material['materials'][0]['alphaMode']='BLEND';material['materials'][0].pop('alphaCutoff');write('coverage-blend.gltf',material)
morph=base();extra=bytearray();primitive=morph['meshes'][0]['primitives'][0]
primitive['attributes']['POSITION']=add(morph,extra,[0,0,0,1,0,0,0,1,0],'9f','VEC3',5126)
primitive['attributes']['NORMAL']=add(morph,extra,[0,0,1]*3,'9f','VEC3',5126)
primitive['targets']=[{'POSITION':add(morph,extra,[0,0,2]*3,'9f','VEC3',5126),'NORMAL':add(morph,extra,[0,1,-1]*3,'9f','VEC3',5126)}]
morph['meshes'][0]['weights']=[.5];morph['buffers']=[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(extra).decode(),'byteLength':len(extra)}];write('morph.gltf',morph)
multi=json.loads(json.dumps(morph));multi_data=bytearray(extra)
multi['meshes'][0]['primitives'].append(json.loads(json.dumps(multi['meshes'][0]['primitives'][0])))
static_mesh=json.loads(json.dumps(morph['meshes'][0]));static_mesh['weights']=[.4]
multi['meshes'].append(static_mesh);multi['nodes'].append({'mesh':1});multi['scenes'][0]['nodes'].append(1)
times=add(multi,multi_data,[0,1],'2f','SCALAR',5126)
weights=add(multi,multi_data,[.25,.75],'2f','SCALAR',5126)
translation=add(multi,multi_data,[0,0,0,1,0,0],'6f','VEC3',5126)
multi['animations']=[{'samplers':[{'input':times,'output':weights,'interpolation':'LINEAR'}],
                      'channels':[{'sampler':0,'target':{'node':0,'path':'weights'}}]},
                     {'samplers':[{'input':times,'output':translation,'interpolation':'LINEAR'}],
                      'channels':[{'sampler':0,'target':{'node':0,'path':'translation'}}]}]
multi['buffers']=[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(multi_data).decode(),'byteLength':len(multi_data)}]
write('morph-multiple-primitives.gltf',multi)
large_normal=json.loads(json.dumps(morph));large_data=bytearray(extra)
normal_delta=large_normal['meshes'][0]['primitives'][0]['targets'][0]['NORMAL']
normal_view=large_normal['bufferViews'][large_normal['accessors'][normal_delta]['bufferView']]
for i in range(3):struct.pack_into('<3f',large_data,normal_view.get('byteOffset',0)+i*12,0,0,1e20)
large_normal['meshes'][0]['weights']=[1]
large_normal['buffers'][0]['uri']='data:application/octet-stream;base64,'+base64.b64encode(large_data).decode()
write('morph-large-normal.gltf',large_normal)
bad=base();extra=bytearray();position=add(bad,extra,[0,0,0,1,0,0,0,1,0],'9f','VEC3',5126)
good_uv=add(bad,extra,[0,0,1,0,0,1],'6f','VEC2',5126);bad_uv=add(bad,extra,[float('nan'),0,1,0,0,1],'6f','VEC2',5126)
bad['meshes']=[{'primitives':[{'attributes':{'POSITION':position,'TEXCOORD_0':good_uv}}]},{'primitives':[{'attributes':{'POSITION':position,'TEXCOORD_0':bad_uv}}]}]
bad['nodes']=[{'mesh':0},{'mesh':1,'name':'LOD_1_name_only'}];bad['scenes'][0]['nodes']=[0,1];bad['buffers']=[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(extra).decode(),'byteLength':len(extra)}];write('nonfinite-lod.gltf',bad)
sparse=base();data=bytearray(struct.pack('<12f',0,0,0,99,0,0,0,99,0,0,0,99)+bytes([1,2])+b'\0\0'+struct.pack('<6f',2,0,0,0,2,0));sparse['buffers']=[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(data).decode(),'byteLength':len(data)}];sparse['bufferViews']=[{'buffer':0,'byteLength':48,'byteStride':16},{'buffer':0,'byteOffset':48,'byteLength':2},{'buffer':0,'byteOffset':52,'byteLength':24}];sparse['accessors']=[{'bufferView':0,'componentType':5126,'count':3,'type':'VEC3','sparse':{'count':2,'indices':{'bufferView':1,'componentType':5121},'values':{'bufferView':2}}}];sparse['meshes'][0]['primitives'][0]['attributes']={'POSITION':0};write('sparse-interleaved.gltf',sparse)
eight=base();data=bytearray();attrs=eight['meshes'][0]['primitives'][0]['attributes'];attrs['POSITION']=add(eight,data,[0,0,0,1,0,0,0,1,0],'9f','VEC3',5126)
for index,(bones,weights) in enumerate([([0,1,2,3],[32768,16384,8192,4096]),([4,5,6,7],[2048,1024,512,256])]):
    attrs[f'JOINTS_{index}']=add(eight,data,bones*3,'12H','VEC4',5123);attrs[f'WEIGHTS_{index}']=add(eight,data,weights*3,'12H','VEC4',5123,True)
eight['buffers']=[{'uri':'data:application/octet-stream;base64,'+base64.b64encode(data).decode(),'byteLength':len(data)}];eight['nodes']=[{'mesh':0,'skin':0,'translation':[99,0,0]}]+[{'name':'Repeated name','translation':[2,0,0]} for _ in range(8)]+[{'name':'Skeleton auxiliary root','children':list(range(1,9))}];eight['scenes'][0]['nodes']=[0,9];eight['skins']=[{'joints':list(range(1,9)),'skeleton':9}];write('all-eight.gltf',eight)
normalized=json.loads(json.dumps(eight));normalized_data=bytearray(data);offset=normalized['bufferViews'][normalized['accessors'][attrs['WEIGHTS_0']]['bufferView']]['byteOffset']
for vertex in range(3):struct.pack_into('<H',normalized_data,offset+vertex*8,33023)
normalized['buffers'][0]['uri']='data:application/octet-stream;base64,'+base64.b64encode(normalized_data).decode();write('eight-normalized.gltf',normalized)
invalid=json.loads(json.dumps(eight));invalid['meshes'][0]['primitives'][0]['attributes'].pop('WEIGHTS_1');write('invalid-pair.gltf',invalid)
invalid=json.loads(json.dumps(doc));invalid['bufferViews'][0]['byteLength']=4;write('invalid-range.gltf',invalid)
invalid=json.loads(json.dumps(doc));invalid['extensionsRequired']=['KHR_draco_mesh_compression'];write('required-draco.gltf',invalid)
(root/'materials.mtl').write_text('newmtl Colour\nKd 0.25 0.5 0.75\nmap_Kd colour.png\n',encoding='utf-8');(root/'colour.png').write_bytes(png())
obj='mtllib materials.mtl\no Concave\nv 0 0 0\nv 2 0 0\nv 2 2 0\nv 1 1 0\nv 0 2 0\nvt 0 0\nvt 2 0\nvt 2 2\nvt 1 1\nvt 0 2\nvn 0 0 1\nusemtl Colour\nf -5/1/1 -4/2/1 -3/3/1 -2/4/1 -1/5/1\n';(root/'concave.obj').write_text(obj,encoding='utf-8');(root/'missing-mtl.obj').write_text(obj.replace('materials.mtl','absent.mtl'),encoding='utf-8')
(root/'two-materials.mtl').write_text('newmtl Red\nKd 1 0 0\nmap_Kd colour.png\nnewmtl Blue\nKd 0 0 1\nmap_Kd colour.png\n',encoding='utf-8')
(root/'two-materials.obj').write_text('mtllib two-materials.mtl\no TwoColours\nv -1 0 0\nv 0 0 0\nv -1 1 0\nv 0 0 0\nv 1 0 0\nv 1 1 0\nvt 0 0\nvt 0.5 0\nvt 0 1\nvt 0.5 0\nvt 1 0\nvt 1 1\nusemtl Red\nf 1/1 2/2 3/3\nusemtl Blue\nf 4/4 5/5 6/6\n',encoding='utf-8')
# Reused accessor keeps the file small while each target requires its own
# decoded per-vertex array. Loading must reject before allocating over budget.
count=50001
bulk=bytearray(count*12)
struct.pack_into('<3f',bulk,12,1,0,0);struct.pack_into('<3f',bulk,24,0,1,0)
(root/'morph-budget.bin').write_bytes(bulk)
over={'asset':{'version':'2.0'},'buffers':[{'uri':'morph-budget.bin','byteLength':len(bulk)}],
      'bufferViews':[{'buffer':0,'byteLength':len(bulk)}],
      'accessors':[{'bufferView':0,'componentType':5126,'count':count,'type':'VEC3','min':[0,0,0],'max':[1,1,0]}],
      'meshes':[{'primitives':[{'attributes':{'POSITION':0},'targets':[{'POSITION':0} for _ in range(1000)]}]}],
      'nodes':[{'mesh':0}],'scenes':[{'nodes':[0]}],'scene':0}
(root/'morph-budget.gltf').write_text(json.dumps(over),encoding='utf-8')
normal_over=json.loads(json.dumps(over))
normal_over['accessors'][0]['count']=24999
normal_over['meshes'][0]['primitives'][0]['attributes']['NORMAL']=0
normal_over['meshes'][0]['primitives'][0]['targets']=[{'POSITION':0,'NORMAL':0} for _ in range(1000)]
(root/'morph-normal-budget.gltf').write_text(json.dumps(normal_over),encoding='utf-8')
print(json.dumps({'directory':str(root),'files':len(list(root.iterdir()))},ensure_ascii=False))
