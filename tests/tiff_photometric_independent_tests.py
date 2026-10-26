"""Prove unsupported WhiteIsZero TIFF cannot silently become BlackIsZero.

Independent tifffile fixtures and Pillow brightness oracle. Artifacts must be
outside the source tree; existing supported subtypes have a separate matrix.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

import numpy as np
import tifffile
from PIL import Image, __version__ as pillow_version


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    records = []
    scope = args.output/'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],
        output_roots=[str(args.output.resolve())], allow_overwrite=False,
        policy_revision='tiff-photometric-independent-v1')), encoding='utf-8')
    fixtures = []
    for dtype in ['uint8', 'uint16']:
        for alpha in [False, True]:
            for endian in ['<', '>']:
                for tiled in [False, True]:
                    values = np.arange(17*19*(2 if alpha else 1), dtype=dtype).reshape(
                        (17, 19, 2) if alpha else (17, 19))
                    values.flat[0] = 0
                    values.flat[2] = np.iinfo(dtype).max
                    name = f'{dtype}-{alpha}-{ord(endian)}-{tiled}'
                    path = args.output/(name+'.tiff')
                    stored = np.moveaxis(values, 2, 0) if alpha and tiled else values
                    tifffile.imwrite(path, stored, photometric='miniswhite', metadata=None,
                        extrasamples=['unassalpha'] if alpha else None, byteorder=endian,
                        tile=(16, 16) if tiled else None,
                        planarconfig='separate' if alpha and tiled else 'contig')
                    with tifffile.TiffFile(path) as image:
                        assert image.pages[0].photometric == 0
                        assert np.array_equal(image.asarray(), stored)
                    fixtures.append((path, alpha))
    for dtype in ['uint32', 'int16', 'int32', 'float16', 'float32', 'float64']:
        path = args.output/(dtype+'.tiff')
        tifffile.imwrite(path, np.zeros((5, 7), dtype=dtype), photometric='miniswhite', metadata=None)
        fixtures.append((path, False))
    for white_first in [True, False]:
        path = args.output/f'multipage-white-first-{white_first}.tiff'
        with tifffile.TiffWriter(path) as writer:
            for white in [white_first, not white_first]:
                writer.write(np.zeros((5, 7), dtype='uint8'),
                    photometric='miniswhite' if white else 'minisblack', metadata=None)
        fixtures.append((path, False))
    # This reader interprets the tag instead of merely returning stored samples.
    oracle = args.output/'uint8-False-60-False.tiff'
    with Image.open(oracle) as image:
        assert np.array(image).flat[0] == 255 and np.array(image).flat[2] == 0
    sources = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path, _ in fixtures}
    for index, (source, alpha) in enumerate(fixtures):
        probe_output = args.output/f'probe-{index:02d}'
        command = [str(args.probe), str(source), str(probe_output), '0', 'tiff']
        process = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert process.returncode == 1 and 'TIFF WhiteIsZero grayscale' in process.stderr, process
        assert not list(probe_output.glob('*')), probe_output
        records.append(dict(command=command, exit=process.returncode, stderr=process.stderr))
        for operation, target in [('inspect', None), ('preflight', 'png'), ('convert', 'png'),
                                  ('preflight', 'tiff'), ('convert', 'tiff')]:
            request = dict(version=1, operation=operation, input=str(source.resolve()))
            destination = args.output/f'converted-{index:02d}.{target}'
            if target:
                request.update(output=str(destination.resolve()), options=dict(target=target,
                    channels=[dict(source=c, destination=c) for c in (['Y', 'A'] if alpha else ['Y'])],
                    source_alpha='straight' if alpha else 'absent', alpha_channel='A' if alpha else ''))
            if operation == 'convert':
                # Even a fabricated approval cannot bypass the decoder guard.
                request.update(approval_digest='0'*64, acknowledge_losses=True)
            process = subprocess.run([str(args.native), '--scope', str(scope)],
                input=json.dumps(request), encoding='utf-8', capture_output=True, timeout=60)
            response = json.loads(process.stdout)
            records.append(dict(request=request, response=response, exit=process.returncode, stderr=process.stderr))
            (args.output/'calls.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
            assert process.returncode == 1 and not process.stderr, records[-1]
            assert response['error']['code'] == 'unsupported', response
            assert 'TIFF WhiteIsZero grayscale' in response['error']['message'], response
            assert not destination.exists(), destination
    assert all(hashlib.sha256(Path(p).read_bytes()).hexdigest() == value for p, value in sources.items())
    assert not list(args.output.glob('converted-*'))
    report = dict(passed=True, fixtures=len(fixtures), probe_calls=len(fixtures),
        cli_calls=len(fixtures)*5, source_sha256=sources, tifffile=tifffile.__version__,
        pillow=pillow_version, numpy=np.__version__, brightness_oracle='Pillow WhiteIsZero 0=255, max=0')
    (args.output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
