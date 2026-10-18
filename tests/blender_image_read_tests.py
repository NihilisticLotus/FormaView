"""Compare externally generated Blender images to actual Blender reopen pixels."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--raster-probe',type=Path,required=True)
    parser.add_argument('--professional-probe',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    manifest=json.loads(args.manifest.read_text(encoding='utf-8'))
    assert manifest['user_scene_unchanged']
    root=args.manifest.parent
    results=[]
    for i,row in enumerate(manifest['records']):
        path=root/row['file'];out=args.output/f'case-{i:02d}'
        assert hashlib.sha256(path.read_bytes()).hexdigest()==row['sha256']
        professional=path.suffix in ['.exr','.hdr','.tga']
        command=[str(args.professional_probe),'image',str(path),str(out)] if professional else [str(args.raster_probe),str(path),str(out),'0']
        run=subprocess.run(command,capture_output=True,text=True,timeout=60)
        result={'file':row['file'],'command':command,'exit':run.returncode,'stderr':run.stderr}
        try:
            assert run.returncode==0,run.stderr
            metadata=json.loads((out/'result.json').read_text())
            info=metadata['subresources'][0] if professional else metadata
            reference=np.fromfile(root/row['reopened_raw'],dtype='<f4').reshape(row['height'],row['width'],4)[::-1]
            errors={}
            for c in info['channels']:
                dtype={0:'u1',1:'<u2',2:'<u4',3:'<i2',4:'<i4',5:'<f2',6:'<f4',7:'<f8'}[c['scalar']]
                values=np.fromfile(out/c['file'],dtype=dtype).reshape(row['height'],row['width']).astype(float)
                if c['scalar'] in [0,1]:values/=255 if c['scalar']==0 else 65535
                expected=reference[:,:,['R','G','B','A'].index(c['name'])]
                error=float(np.abs(values-expected).max())
                errors[c['name']]=error
                tolerance=1/128 if path.suffix=='.hdr' else 2/255 if path.suffix=='.jpg' else 2e-7
                assert error<=tolerance,f'{c["name"]} error {error} exceeds {tolerance}'
            assert len(info['channels'])==(4 if row['mode']=='RGBA' else 3)
            result.update(passed=True,max_errors=errors)
        except Exception as error:result.update(passed=False,error=str(error))
        results.append(result)
    report={'blender':manifest['blender'],'transport':manifest['transport'],
            'generated_source':'self-authored DCC exports, not independent third-party assets',
            'unavailable_exports':manifest.get('unavailable_exports',[]),
            'total':len(results),'passed':sum(r['passed'] for r in results),'results':results}
    (args.output/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='results'}))
    for row in results:
        if not row['passed']:print(row['file'],row['error'])
    return int(report['total']!=report['passed'])


if __name__=='__main__':raise SystemExit(main())
