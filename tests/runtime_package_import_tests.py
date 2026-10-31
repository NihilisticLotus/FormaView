"""Verify packaging rejects missing runtime DLLs and accepts a complete build."""
import argparse
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser()
    for name in ('source', 'pwsh', 'build', 'qt', 'dlls', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    for scenario, dlls in (('missing', args.output / 'absent-dlls'),
                           ('complete', args.dlls)):
        command = [str(args.pwsh), '-NoProfile', '-File',
                   str(args.source / 'tools/package_runtime.ps1'),
                   '-BuildDir', str(args.build), '-OutDir', str(args.output / scenario),
                   '-QtRoot', str(args.qt), '-ExtraDllDir', str(dlls)]
        run = subprocess.run(command, capture_output=True, timeout=180)
        (args.output / (scenario + '.stdout')).write_bytes(run.stdout)
        (args.output / (scenario + '.stderr')).write_bytes(run.stderr)
        if scenario == 'missing':
            assert run.returncode != 0 and b'Unresolved runtime imports:' in run.stderr, (
                'Fixture requires a build with a non-system dependency; see logs')
        else:
            assert run.returncode == 0, 'Complete package failed; see logs'
            assert (args.output / scenario / 'sha256.json').is_file()
    print('PASS missing DLL rejected and complete runtime package accepted')


if __name__ == '__main__':
    main()
