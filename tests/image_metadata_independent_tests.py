"""Independent Pillow/LittleCMS ICC and pixel checks through native requests."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path
from PIL import Image, ImageCms


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    profile = ImageCms.ImageCmsProfile(ImageCms.createProfile('sRGB')).tobytes()
    image = Image.new('RGB', (16, 16))
    image.putdata([(x * 13, y * 11, (x + y) * 7) for y in range(16) for x in range(16)])
    source = args.output / 'source.png'; image.save(source, icc_profile=profile)
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps({'input_roots': [str(args.output.resolve())], 'output_roots': [str(args.output.resolve())],
        'allow_overwrite': False, 'policy_revision': 'icc-independent-v1'}), encoding='utf-8')
    records = []

    def call(request):
        result = subprocess.run([str(args.native), '--scope', str(scope)], input=json.dumps(dict(request, version=1)) + '\n',
            text=True, encoding='utf-8', capture_output=True, timeout=30)
        response = json.loads(result.stdout)
        records.append({'request': request, 'result': response, 'stderr': result.stderr})
        with (args.output / 'calls.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(json.dumps(records[-1], ensure_ascii=False) + '\n')
        assert result.returncode == (1 if 'error' in response else 0), result.stderr
        return response

    def convert(source, target, suffix, **options):
        request = {'input': str(source.resolve()), 'output': str((args.output / suffix).resolve()),
            'options': dict(target=target, channels=[{'source': c, 'destination': c} for c in 'RGB'], **options)}
        preview = call(dict(request, operation='preflight'))
        assert 'error' not in preview, preview
        result = call(dict(request, operation='convert', approval_digest=preview['approval_digest'], acknowledge_losses=True))
        assert result['status'] == 'succeeded', result
        return Path(result['committed_output']), preview

    exact = []
    for target in ('png', 'jpeg', 'tiff', 'webp'):
        path, preview = convert(source, target, 'preserve.' + target, **({'allow_lossy': True} if target == 'jpeg' else {}))
        with Image.open(path) as decoded:
            assert decoded.info.get('icc_profile') == profile, (target, 'ICC changed')
            if target != 'jpeg': assert decoded.convert('RGB').tobytes() == image.tobytes(), target
        assert not any(loss['code'] == 'icc_profile' for loss in preview['losses']), preview
        exact.append(target)
        # Use each real output as an input and preserve its profile back to PNG.
        copied, _ = convert(path, 'png', 'from-' + target + '.png')
        with Image.open(copied) as decoded: assert decoded.info.get('icc_profile') == profile, ('input', target)
    dropped, drop_plan = convert(source, 'png', 'drop.png', metadata='drop')
    with Image.open(dropped) as decoded:
        assert not decoded.info.get('icc_profile') and decoded.convert('RGB').tobytes() == image.tobytes()
    assert any(loss['code'] == 'icc_profile' for loss in drop_plan['losses'])
    bmp, bmp_plan = convert(source, 'bmp', 'unsupported-profile.bmp')
    assert any(loss['code'] == 'icc_profile' for loss in bmp_plan['losses'])
    with Image.open(bmp) as decoded: assert decoded.convert('RGB').tobytes() == image.tobytes()
    changed = {'operation': 'preflight', 'input': str(source.resolve()), 'output': str((args.output / 'transformed.png').resolve()),
        'options': {'target': 'png', 'channels': [{'source': c, 'destination': c} for c in 'RGB'],
                    'meaning': 'colour', 'transfer': 'srgb_to_linear', 'output_scalar': 'uint8', 'quantization': 'clamp_round'}}
    rejection = call(changed)
    assert rejection['error']['code'] == 'invalid_argument' and 'profile' in rejection['error']['message'], rejection
    changed['options']['metadata'] = 'drop'
    allowed = call(changed)
    assert 'error' not in allowed and {'icc_profile', 'colour_transform'}.issubset({x['code'] for x in allowed['losses']})
    for container in ('png', 'jpeg'):
        unknown = args.output / ('unknown-profile.' + container)
        image.save(unknown, icc_profile=b'not-an-interpretable-ICC-profile', format=container)
        unknown_request = {'operation': 'preflight', 'input': str(unknown.resolve()),
            'output': str((args.output / ('unknown-copy-' + container + '.png')).resolve()),
            'options': {'target': 'png', 'channels': [{'source': c, 'destination': c} for c in 'RGB']}}
        rejected = call(unknown_request)
        assert rejected['error']['code'] == 'unsupported', rejected
        unknown_request['options']['metadata'] = 'drop'
        dropped = call(unknown_request)
        assert 'error' not in dropped and any(x['code'] == 'icc_profile' for x in dropped['losses']), dropped
    report = {'passed': True, 'calls': len(records), 'icc_bytes': len(profile),
        'icc_sha256': hashlib.sha256(profile).hexdigest(), 'exact_profile_targets': exact,
        'reader_roundtrips': exact, 'results': records}
    (args.output / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({k: report[k] for k in ('passed', 'calls', 'icc_bytes', 'exact_profile_targets')}))


if __name__ == '__main__': main()
