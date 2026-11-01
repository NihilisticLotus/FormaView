"""Verify packaging rejects missing runtime DLLs and accepts a complete build."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'pwsh', 'build', 'qt', 'dlls', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    invalid_build = args.output / 'invalid-build' / 'Release'
    invalid_build.mkdir(parents=True)
    for name in ('formaview.exe', 'formaview_image_cli.exe'):
        shutil.copyfile(args.build / 'Release' / name, invalid_build / name)
    (invalid_build / 'formaview_image_service.exe').write_bytes(b'not-a-pe')
    for scenario, dlls, build in (('missing', args.output / 'absent-dlls', args.build),
                                   ('invalid', args.dlls, invalid_build.parent),
                                   ('complete', args.dlls, args.build)):
        command = [str(args.pwsh), '-NoProfile', '-File',
                   str(args.source / 'tools/package_runtime.ps1'),
                   '-BuildDir', str(build), '-OutDir', str(args.output / scenario),
                   '-QtRoot', str(args.qt), '-ExtraDllDir', str(dlls)]
        run = subprocess.run(command, capture_output=True, timeout=180)
        (args.output / (scenario + '.stdout')).write_bytes(run.stdout)
        (args.output / (scenario + '.stderr')).write_bytes(run.stderr)
        if scenario == 'missing':
            assert run.returncode != 0 and b'Unresolved runtime imports:' in run.stderr, (
                'Fixture requires a build with a non-system dependency; see logs')
        elif scenario == 'invalid':
            assert run.returncode != 0 and b'Cannot inspect runtime imports for' in run.stderr, (
                'Invalid binary was not rejected; see logs')
        else:
            assert run.returncode == 0, 'Complete package failed; see logs'
            assert (args.output / scenario / 'sha256.json').is_file()
    # A manifest can consistently describe corrupt binaries. The verifier must
    # reject failed dumpbin inspections before attempting the GUI smoke test.
    invalid_runtime = args.output / 'invalid-runtime'
    invalid_runtime.mkdir()
    for name in ('formaview.exe', 'formaview_image_cli.exe', 'formaview_image_service.exe'):
        (invalid_runtime / name).write_bytes(b'not-a-pe')
    for name in ('image_stdio.py', 'requirements.lock', 'requirements.txt'):
        destination = invalid_runtime / 'mcp' / name
        destination.parent.mkdir(exist_ok=True)
        shutil.copyfile(args.source / 'tools/mcp' / name, destination)
    shutil.copyfile(args.source / 'tools/runtime/README-run.txt', invalid_runtime / 'README-run.txt')
    manifest = []
    for path in invalid_runtime.rglob('*'):
        if path.is_file():
            data = path.read_bytes()
            manifest.append(dict(path=path.relative_to(invalid_runtime).as_posix(),
                                 bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
    (invalid_runtime / 'sha256.json').write_text(json.dumps(manifest), encoding='utf-8')
    evidence = args.output / 'invalid-runtime-evidence'
    run = subprocess.run([str(args.pwsh), '-NoProfile', '-File',
                          str(args.source / 'tools/verify_runtime.ps1'),
                          '-RunDir', str(invalid_runtime), '-EvidenceDir', str(evidence),
                          '-SourceRoot', str(args.source)], capture_output=True, timeout=180)
    (args.output / 'verify-invalid.stdout').write_bytes(run.stdout)
    (args.output / 'verify-invalid.stderr').write_bytes(run.stderr)
    assert run.returncode != 0 and b'FAIL imports-inspected' in run.stdout, (
        'Verifier did not reject failed import inspection; see logs')
    report = json.loads((evidence / 'verify-runtime.json').read_text(encoding='utf-8-sig'))
    assert any(c['name'] == 'imports-resolved' and not c['ok'] for c in report['checks'])
    print('PASS missing DLL, invalid binary and failed verification rejected; complete package accepted')


if __name__ == '__main__':
    main()
