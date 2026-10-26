"""Independent 8/16-bit PNG/TIFF ICC, alpha and quantization matrix.

Requires numpy, tifffile and Pillow/LittleCMS. Pass real RGB and Gray profiles
and a fresh external output directory. No colour-management accuracy claim.
"""
import argparse
import hashlib
import io
import json
import struct
import subprocess
import zlib
from pathlib import Path

import numpy as np
import tifffile
from PIL import ImageCms, __version__ as pillow_version
from raster_independent_tests import png_bytes, read_png_native


def sha(data):
    return hashlib.sha256(data).hexdigest()


def png_profile(data):
    profiles = []
    position = 8
    while position < len(data):
        size = struct.unpack_from('>I', data, position)[0]
        kind, payload = data[position+4:position+8], data[position+8:position+8+size]
        assert zlib.crc32(kind+payload) == struct.unpack_from('>I', data, position+8+size)[0]
        if kind == b'iCCP':
            name, compressed = payload.split(b'\0', 1)
            assert name and compressed[0] == 0
            profiles.append(zlib.decompress(compressed[1:]))
        position += size+12
    assert len(profiles) == 1
    return profiles[0]


def tagged_png(values, colour_type, bits, profile):
    data = png_bytes(values, colour_type, bits)
    payload = b'Independent\0\0'+zlib.compress(profile)
    chunk = struct.pack('>I', len(payload))+b'iCCP'+payload
    chunk += struct.pack('>I', zlib.crc32(b'iCCP'+payload))
    return data[:33]+chunk+data[33:]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--gray-profile', type=Path, required=True)
    parser.add_argument('--rgb-profile', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    profiles = {True: args.gray_profile.read_bytes(), False: args.rgb_profile.read_bytes()}
    for gray, profile in profiles.items():
        assert profile[16:20] == (b'GRAY' if gray else b'RGB ')
        ImageCms.ImageCmsProfile(io.BytesIO(profile))
        (args.output/('gray.icc' if gray else 'rgb.icc')).write_bytes(profile)
    scope = args.output/'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],
        output_roots=[str(args.output.resolve())], allow_overwrite=False,
        policy_revision='icc-precision-independent-v1')), encoding='utf-8')
    calls, outputs, sources = [], [], {}

    def call(request, error=None):
        process = subprocess.run([str(args.native), '--scope', str(scope)],
            input=json.dumps(dict(request, version=1)), encoding='utf-8', capture_output=True, timeout=60)
        response = json.loads(process.stdout)
        calls.append(dict(request=request, response=response, exit=process.returncode, stderr=process.stderr))
        with (args.output/'calls.jsonl').open('a', encoding='utf-8') as log:
            log.write(json.dumps(calls[-1])+'\n')
        assert not process.stderr and process.returncode == (1 if error else 0), calls[-1]
        if error:
            assert response['error']['code'] == 'invalid_argument' and error in response['error']['message'], response
            assert not Path(request['output']).exists()
        else:
            assert 'error' not in response, response
        return response

    def inspect(path, names, bits, alpha, profile):
        result = call(dict(operation='inspect', input=str(path)))
        assert result['alpha'] == alpha and result['icc_profile_sha256'] == sha(profile), result
        assert result['selected_channels'] == [dict(name=c, scalar=f'uint{bits}', encoding='unorm') for c in names], result

    def convert(path, names, bits, alpha, profile, expected, target, suffix, quantize=False):
        output = args.output/(path.stem+'-'+suffix+'.'+target)
        options = dict(target=target, channels=[dict(source=c, destination=c) for c in names],
            source_alpha=alpha, alpha_channel='A' if 'A' in names else '')
        request = dict(input=str(path), output=str(output), options=options)
        if quantize:
            options['output_scalar'] = 'uint8'
            call(dict(request, operation='preflight'), 'Precision loss requires explicit quantization policy')
            options['quantization'] = 'clamp_round'
        preview = call(dict(request, operation='preflight'))
        expected_codes = ['precision_changed'] if quantize else []
        assert [loss['code'] for loss in preview['losses']] == expected_codes, preview
        if quantize:
            original = read_png_native(path)
            assert int(preview['losses'][0]['detail']) == int(np.count_nonzero(original % 257)), preview
            call(dict(request, operation='convert', approval_digest=preview['approval_digest'],
                      acknowledge_losses=False), 'Preflight losses require explicit acknowledgement')
        done = call(dict(request, operation='convert', approval_digest=preview['approval_digest'], acknowledge_losses=True))
        assert done['status'] == 'succeeded' and done['losses'] == preview['losses'], done
        if target == 'png':
            actual = read_png_native(output)
            actual_profile = png_profile(output.read_bytes())
        else:
            with tifffile.TiffFile(output) as image:
                page = image.pages[0]
                actual = page.asarray()
                if actual.ndim == 2:
                    actual = actual[:, :, None]
                actual_profile = page.tags[34675].value
                assert page.photometric == (1 if names[0] == 'Y' else 2)
                assert list(page.extrasamples) == ([1 if alpha == 'premultiplied' else 2] if 'A' in names else [])
        assert actual.dtype.kind == 'u' and actual.dtype.itemsize == bits//8
        assert np.array_equal(actual, expected) and actual_profile == profile, output
        inspect(output, names, bits, alpha, profile)
        outputs.append(dict(path=str(output), sha256=sha(output.read_bytes()), bits=bits,
            channels=names, alpha=alpha, icc_sha256=sha(profile), samples=int(actual.size), quantized=quantize))

    for bits in [8, 16]:
        for gray, has_alpha, colour_type in [(True, False, 0), (True, True, 4), (False, False, 2), (False, True, 6)]:
            names = (['Y'] if gray else ['R', 'G', 'B'])+(['A'] if has_alpha else [])
            alpha = 'straight' if has_alpha else 'absent'
            maximum = 2**bits-1
            values = ((np.arange(17*19*len(names), dtype='uint64')*25173+5003) % (maximum+1)).astype(f'uint{bits}').reshape(17, 19, len(names))
            landmarks = [0, 1, 255, 256, 257, 32767, 32768, 65534, 65535]
            values.flat[:len(landmarks)] = np.array(landmarks, dtype='uint64') % (maximum+1)
            if has_alpha:
                values[0, :, -1] = 0
                values[1, :, -1] = maximum
            profile = profiles[gray]
            for container in ['png', 'tiff-le', 'tiff-be']:
                path = args.output/f'source-{bits}-{colour_type}-{container}.{container.split("-")[0]}'
                if container == 'png':
                    path.write_bytes(tagged_png(values, colour_type, bits, profile))
                    assert np.array_equal(read_png_native(path), values) and png_profile(path.read_bytes()) == profile
                else:
                    tifffile.imwrite(path, values[:, :, 0] if len(names) == 1 else values,
                        photometric='minisblack' if gray else 'rgb', metadata=None,
                        extrasamples=['unassalpha'] if has_alpha else None,
                        byteorder='<' if container.endswith('le') else '>',
                        extratags=[(34675, 'B', len(profile), profile, False)])
                sources[str(path)] = sha(path.read_bytes())
                inspect(path, names, bits, alpha, profile)
                for target in ['png', 'tiff']:
                    convert(path, names, bits, alpha, profile, values, target, 'exact')
                    if bits == 16 and container == 'png':
                        rounded = np.floor(values.astype('float64')/257+.5).astype('uint8')
                        convert(path, names, 8, alpha, profile, rounded, target, 'rounded', quantize=True)
    for bits in [8, 16]:
        for gray in [True, False]:
            names = ['Y', 'A'] if gray else ['R', 'G', 'B', 'A']
            values = np.full((17, 19, len(names)), 32 if bits == 8 else 8192, dtype=f'uint{bits}')
            values[:, :, -1] = 128 if bits == 8 else 32768
            profile = profiles[gray]
            path = args.output/f'premultiplied-{bits}-{gray}.tiff'
            tifffile.imwrite(path, values, photometric='minisblack' if gray else 'rgb',
                extrasamples=['assocalpha'], metadata=None, extratags=[(34675, 'B', len(profile), profile, False)])
            sources[str(path)] = sha(path.read_bytes())
            inspect(path, names, bits, 'premultiplied', profile)
            convert(path, names, bits, 'premultiplied', profile, values, 'tiff', 'exact')
            for metadata in ['preserve_supported', 'drop']:
                call(dict(operation='preflight', input=str(path), output=str(args.output/(path.stem+'-blocked.png')),
                    options=dict(target='png', metadata=metadata, source_alpha='premultiplied', alpha_channel='A',
                        channels=[dict(source=c, destination=c) for c in names])), 'PNG requires explicit straight alpha')
    assert all(sha(Path(path).read_bytes()) == digest for path, digest in sources.items())
    assert args.gray_profile.read_bytes() == profiles[True] and args.rgb_profile.read_bytes() == profiles[False]
    report = dict(passed=True, sources=sources, outputs=outputs, calls=len(calls),
        expected_rejections=sum('error' in row['response'] for row in calls),
        versions=dict(numpy=np.__version__, tifffile=tifffile.__version__, pillow=pillow_version,
                      littlecms=ImageCms.core.littlecms_version),
        profiles={str(path): sha(path.read_bytes()) for path in [args.gray_profile, args.rgb_profile]})
    (args.output/'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=True, sources=len(sources), outputs=len(outputs), calls=len(calls),
                         expected_rejections=report['expected_rejections'])))


if __name__ == '__main__':
    main()
