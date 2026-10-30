"""Regression: a consistent manifest must still reject missing MCP payloads."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--pwsh', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    results = []
    for manifest_name in ('sha256.json', 'payload-manifest.json'):
        for scenario in ('complete', 'omitted-mcp', 'wrong-source'):
            root = args.output / (manifest_name + '-' + scenario)
            payload = root / 'payload'
            payload.mkdir(parents=True)
            for name in ('formaview.exe', 'formaview_image_service.exe', 'formaview_image_cli.exe'):
                (payload / name).write_bytes(b'not-executed-manifest-only-fixture')
            shutil.copyfile(args.source / 'tools/runtime/README-run.txt', payload / 'README-run.txt')
            if scenario != 'omitted-mcp':
                (payload / 'mcp').mkdir()
                for name in ('image_stdio.py', 'requirements.lock', 'requirements.txt'):
                    shutil.copyfile(args.source / 'tools/mcp' / name, payload / 'mcp' / name)
                if scenario == 'wrong-source':
                    with (payload / 'mcp/image_stdio.py').open('ab') as stream:
                        stream.write(b'\n# mismatched-source-fixture\n')
            rows = [dict(path=p.relative_to(payload).as_posix(), bytes=p.stat().st_size,
                         sha256=hashlib.sha256(p.read_bytes()).hexdigest())
                    for p in sorted(payload.rglob('*')) if p.is_file()]
            document = rows if manifest_name == 'sha256.json' else dict(files=rows)
            (payload / manifest_name).write_text(json.dumps(document), encoding='utf-8')
            command = [str(args.pwsh), '-NoProfile', '-File', str(args.source / 'tools/verify_runtime.ps1'),
                       '-RunDir', str(payload), '-EvidenceDir', str(root / 'evidence'),
                       '-SourceRoot', str(args.source), '-ManifestOnly']
            run = subprocess.run(command, capture_output=True, timeout=30)
            (root / 'stdout.txt').write_bytes(run.stdout)
            (root / 'stderr.txt').write_bytes(run.stderr)
            report = json.loads((root / 'evidence/verify-runtime.json').read_text(encoding='utf-8-sig'))
            checks = {check['name']: check['ok'] for check in report['checks']}
            assert checks['manifest'], report
            assert (run.returncode == 0) == (scenario == 'complete'), report
            assert checks['required-runtime-files'] == (scenario != 'omitted-mcp'), report
            assert checks['packaged-source-files'] == (scenario == 'complete'), report
            results.append(dict(manifest=manifest_name, scenario=scenario, exitCode=run.returncode, checks=checks))
    (args.output / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
    print(f'{len(results)} payload contract regressions passed')


if __name__ == '__main__':
    main()
