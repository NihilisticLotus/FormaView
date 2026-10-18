"""TIFF/WebP values from independent Python consumers; external artifacts only."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np
import tifffile
from PIL import Image, __version__ as pillow_version


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--probe', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []
    rng = np.random.default_rng(505)
    def add_tiff(name, values, names, **kwargs):
        path = args.output/(name+'.tiff')
        stored = values[:, :, 0] if len(names) == 1 else values
        if kwargs.get('planarconfig') == 'separate':
            stored = np.moveaxis(values, 2, 0)
        tifffile.imwrite(path, stored, photometric='rgb' if names[0] == 'R' else 'minisblack',
                         extrasamples=['unassalpha'] if names[-1] == 'A' else None,
                         metadata=None, **kwargs)
        cases.append((path, values, names, 0, 'tiff', 'tifffile native samples'))
    for dtype in ['uint8', 'uint16', 'uint32', 'int16', 'int32', 'float16', 'float32', 'float64']:
        for names in [['Y'], ['Y','A'], ['R','G','B'], ['R','G','B','A']]:
            size=(17,19,len(names))
            if dtype.startswith('float'):
                values=rng.normal(0,2,size).astype(dtype)
                values[0,0,0]=np.nan;values[0,1,0]=np.inf;values[0,2,0]=-np.inf;values[0,3,0]=-0.
            else:
                bounds=np.iinfo(dtype)
                values=rng.integers(bounds.min,bounds.max,size=size,dtype=dtype)
                values[0,0,0]=bounds.max;values[0,1,0]=bounds.min
            add_tiff(f'{dtype}-{len(names)}',values,names,compression='deflate')
    rgba=rng.integers(0,65535,(17,19,4),dtype=np.uint16)
    for planar in ['contig','separate']:
        for tile in [None,(16,16)]:
            for endian in ['<','>']:
                add_tiff(f'layout-{planar}-{bool(tile)}-{ord(endian)}',rgba,['R','G','B','A'],
                         planarconfig=planar,tile=tile,byteorder=endian)
    add_tiff('bigtiff',rgba,['R','G','B','A'],bigtiff=True)
    add_tiff('orientation-bottom',rgba,['R','G','B','A'],extratags=[(274,'H',1,4,False)])
    multi=args.output/'multipage.tif'
    with tifffile.TiffWriter(multi) as writer:
        for i in range(3):
            value=np.full((5+i,7+i),i*1001,dtype=np.uint16)
            writer.write(value,photometric='minisblack',metadata=None)
            cases.append((multi,value[:,:,None],['Y'],i,'tiff','tifffile multi-page'))
    rgb8=rng.integers(0,255,(17,19,3),dtype=np.uint8)
    for compression in ['tiff_lzw','packbits','raw']:
        path=args.output/(compression+'.tiff');Image.fromarray(rgb8).save(path,compression=compression)
        cases.append((path,rgb8,['R','G','B'],0,'tiff','Pillow TIFF compression'))
    for lossless in [True,False]:
        for alpha in [True,False]:
            a=rng.integers(0,255,(17,19,4 if alpha else 3),dtype=np.uint8)
            if alpha:a[:3,:,3]=0
            path=args.output/f'webp-{lossless}-{alpha}.webp'
            Image.fromarray(a).save(path,lossless=lossless,quality=90,exact=True)
            expected=np.array(Image.open(path).convert('RGBA' if alpha else 'RGB'))
            if lossless:assert np.array_equal(expected,a)
            cases.append((path,expected,['R','G','B','A'] if alpha else ['R','G','B'],0,'webp','Pillow WebP export and separate consumer'))
    results=[]
    for index,(path,expected,names,entry,target,source) in enumerate(cases):
        out=args.output/f'case-{index:02d}'
        command=[str(args.probe),str(path),str(out),str(entry),target]
        run=subprocess.run(command,capture_output=True,text=True,timeout=60)
        record={'input':str(path),'entry':entry,'source':source,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),
                'command':command,'exit':run.returncode,'stderr':run.stderr}
        try:
            assert run.returncode==0,run.stderr
            desc=json.loads((out/'result.json').read_text())
            assert [c['name'] for c in desc['channels']]==names
            actual=np.stack([np.fromfile(out/c['file'],dtype={0:'u1',1:'<u2',2:'<u4',3:'<i2',4:'<i4',5:'<f2',6:'<f4',7:'<f8'}[c['scalar']]).reshape(expected.shape[:2]) for c in desc['channels']],axis=2)
            assert actual.dtype==expected.dtype and actual.tobytes()==expected.tobytes(),'Native decoded samples changed'
            exported=out/('export.'+target)
            reopened=tifffile.imread(exported) if target=='tiff' else np.array(Image.open(exported))
            if reopened.ndim==2:reopened=reopened[:,:,None]
            assert reopened.dtype==expected.dtype and reopened.tobytes()==expected.tobytes(),'Export native samples changed'
            record.update(passed=True,samples=actual.size,output_sha256=hashlib.sha256(exported.read_bytes()).hexdigest())
        except Exception as e:record.update(passed=False,error=str(e))
        results.append(record)
    report={'total':len(results),'passed':sum(r['passed'] for r in results),'tifffile':tifffile.__version__,
            'pillow':pillow_version,'numpy':np.__version__,'results':results}
    (args.output/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='results'}))
    for row in results:
        if not row['passed']:print(row['input'],row['error'])
    return int(report['passed']!=report['total'])


if __name__=='__main__':raise SystemExit(main())
