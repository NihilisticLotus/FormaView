"""Verify packaging rejects missing runtime DLLs and accepts a complete build."""
import argparse
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
    print('PASS missing DLL and invalid binary rejected; complete runtime package accepted')


if __name__ == '__main__':
    main()
