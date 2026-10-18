"""Independent raster values and export reopen tests. All artifacts are external.

Requires Pillow and numpy. No private inputs. Fixtures are deterministic handmade
PNG chunks or Pillow exports, identified separately in the output report.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import zlib

import numpy as np
from PIL import Image, __version__ as pillow_version


def png_bytes(array, colour_type, bits):
    height, width = array.shape[:2]
    dtype = '>u2' if bits == 16 else 'u1'
    payload = b''.join(b'\0' + row.astype(dtype).tobytes() for row in array)
    def chunk(name, data):
        return struct.pack('>I', len(data)) + name + data + struct.pack('>I', zlib.crc32(name + data))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, bits, colour_type, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(payload)) + chunk(b'IEND', b'')


def read_png_native(path):
    """Independent unfiltering for exported 8/16-bit PNG, no Qt/libpng decode."""
    data = path.read_bytes()
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    pos, compressed = 8, b''
    while pos < len(data):
        size = struct.unpack_from('>I', data, pos)[0]
        kind, chunk = data[pos+4:pos+8], data[pos+8:pos+8+size]
        assert zlib.crc32(kind + chunk) == struct.unpack_from('>I', data, pos+8+size)[0]
        if kind == b'IHDR':
            width, height, bits, colour, compression, filtering, interlace = struct.unpack('>IIBBBBB', chunk)
            assert bits in (8, 16) and (compression, filtering, interlace) == (0, 0, 0)
        if kind == b'IDAT': compressed += chunk
        pos += size + 12
    components = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
    bpp, row_size = components * bits // 8, width * components * bits // 8
    decoded, previous, rows = zlib.decompress(compressed), bytearray(row_size), []
    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
        return a if pa <= pb and pa <= pc else b if pb <= pc else c
    for y in range(height):
        f = decoded[y*(row_size+1)]
        row = bytearray(decoded[y*(row_size+1)+1:(y+1)*(row_size+1)])
        for x in range(row_size):
            a, b, c = row[x-bpp] if x >= bpp else 0, previous[x], previous[x-bpp] if x >= bpp else 0
            prediction = [0, a, b, (a+b)//2, paeth(a,b,c)][f]
            row[x] = (row[x] + prediction) & 255
        rows.append(bytes(row)); previous = row
    return np.frombuffer(b''.join(rows), dtype='>u2' if bits == 16 else 'u1').reshape(height, width, components)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cases = []
    def add(name, content, expected, names, export='png', source='handmade PNG chunks', entry=0):
        path = args.output / name
        path.write_bytes(content)
        cases.append((path, expected, names, export, source, entry))
    base16 = np.array([[[0, 1, 257, 0], [1023, 32768, 65535, 32768], [65535, 50000, 12345, 65535]],
                       [[23, 42, 99, 1234], [40000, 123, 999, 0], [1, 2, 3, 4]]], dtype=np.uint16)
    for bits in (8, 16):
        base = (base16 % 256).astype('u1') if bits == 8 else base16
        for colour, names, indices in [(0,['Y'],[0]),(2,['R','G','B'],[0,1,2]),(4,['Y','A'],[0,3]),(6,['R','G','B','A'],[0,1,2,3])]:
            values = base[:,:,indices]
            add(f'png-{bits}-{colour}.png', png_bytes(values, colour, bits), values, names, 'png')
    palette = Image.new('P', (3,2))
    palette.putpalette([255,34,56, 12,240,90, 80,33,200]+[0]*759)
    palette.putdata([0,1,2,2,1,0]); palette.info['transparency'] = 0
    for fmt in ['PNG','GIF']:
        buf=io.BytesIO();palette.copy().save(buf,format=fmt,optimize=False)
        add('palette.'+fmt.lower(),buf.getvalue(),np.array(palette.copy().convert('RGBA')),['R','G','B','A'],'png','Pillow palette export')
    rgb = np.full((16,16,3), [73,121,208], dtype='u1')
    for fmt in ['JPEG','BMP']:
        buf=io.BytesIO();Image.fromarray(rgb).save(buf,format=fmt,quality=100,subsampling=0)
        content=buf.getvalue();expected=np.array(Image.open(io.BytesIO(content)).convert('RGB'))
        add('rgb.'+fmt.lower(),content,expected,['R','G','B'],fmt.lower(),'Pillow export and independent reopen')
        if fmt == 'BMP':add('bare.dib',content[14:],expected,['R','G','B'],'bmp','Pillow BMP with file header removed')
        if fmt == 'JPEG':
            for ext in ['jpe','jfif']:add('alias.'+ext,content,expected,['R','G','B'],'jpeg','JPEG identical content alias')
    # Two ICO PNG entries with distinct dimensions and exact values, no resampling.
    entries=[]
    for size,rgba in [(16,[30,90,180,128]),(32,[200,70,9,255])]:
        values=np.full((size,size,4),rgba,dtype='u1');entries.append((size,png_bytes(values,6,8),values))
    offset=6+16*len(entries);directory=b'';payload=b''
    for size,data,values in entries:
        directory+=struct.pack('<BBBBHHII',size,size,0,0,1,32,len(data),offset);payload+=data;offset+=len(data)
    ico=struct.pack('<HHH',0,1,len(entries))+directory+payload
    for i,(_,_,values) in enumerate(entries):add(f'icon-entry-{i}.ico',ico,values,['R','G','B','A'],'png','Handmade ICO directory with PNG entries',i)
    reports=[]
    for index,(path,expected,names,export,source,entry) in enumerate(cases):
        out=args.output/f'read-{index:02d}'
        command=[str(args.probe),str(path),str(out),str(entry)]+([export] if export else [])
        run=subprocess.run(command,capture_output=True,text=True,timeout=60)
        record={'input':str(path),'source':source,'sha256':hashlib.sha256(path.read_bytes()).hexdigest(),'entry':entry,'command':command,'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr}
        try:
            assert run.returncode == 0, run.stderr
            result=json.loads((out/'result.json').read_text())
            assert [c['name'] for c in result['channels']] == names
            actual=np.stack([np.fromfile(out/c['file'],dtype={0:'u1',1:'<u2'}[c['scalar']]).reshape(expected.shape[:2]) for c in result['channels']],axis=2)
            assert np.array_equal(actual,expected), f'decode maximum error {np.max(np.abs(actual.astype(float)-expected))}'
            record['decoded_samples']=actual.size
            if export:
                exported=out/('export.'+export)
                reopened=read_png_native(exported) if export=='png' else np.array(Image.open(exported).convert('RGB'))
                error=float(np.max(np.abs(reopened.astype(float)-expected)))
                assert error <= (2 if export=='jpeg' else 0), f'export error {error}'
                record['reopen_max_error']=error;record['export_sha256']=hashlib.sha256(exported.read_bytes()).hexdigest()
            record['passed']=True
        except Exception as e:
            record['passed']=False;record['failure']=str(e)
        reports.append(record)
    report={'pillow':pillow_version,'numpy':np.__version__,'count':len(reports),'passed':sum(x['passed'] for x in reports),'excluded':0,'results':reports}
    (args.output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({k:v for k,v in report.items() if k!='results'}))
    for row in reports:
        if not row['passed']:print(row['input'],row['failure'])
    return int(report['passed']!=len(reports))


if __name__=='__main__':raise SystemExit(main())
