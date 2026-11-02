"""Exercise developer and runtime-script paths from a different caller directory."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--qt', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
args.output.mkdir(parents=True, exist_ok=False)
output = args.output.resolve()
records = []

def run(name, command, cwd=output):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, encoding='utf-8', timeout=600)
    (output / (name + '.stdout.log')).write_text(result.stdout, encoding='utf-8')
    (output / (name + '.stderr.log')).write_text(result.stderr, encoding='utf-8')
    records.append(dict(name=name, exit_code=result.returncode))
    print('PASS' if result.returncode == 0 else 'FAIL', name, result.returncode)

run('dev-relative-qt', ['pwsh', '-NoProfile', '-File', str(repo / 'tools/dev.ps1'),
    'Build', '-QtRoot', os.path.relpath(args.qt.resolve(), args.qt.resolve().parent.parent)],
    cwd=args.qt.resolve().parent.parent)
(output / 'runtime').mkdir()
(output / 'dotnet-cwd').mkdir()
shutil.copyfile(Path(os.environ['WINDIR']) / 'System32/where.exe', output / 'runtime/formaview.exe')
# Execute the actual initialization and Run-Clean function independently of
# manifest/import auditing. where.exe is a local process-launch fixture only.
script = r'''
$ErrorActionPreference = 'Stop'
$text = Get-Content -LiteralPath $env:PATH_TEST_SCRIPT -Raw
$prefix = [scriptblock]::Create($text.Substring(0, $text.IndexOf('$report =')))
$ast = [System.Management.Automation.Language.Parser]::ParseInput($text, [ref]$null, [ref]$null)
$function = $ast.Find({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Run-Clean'}, $true)
[Environment]::CurrentDirectory = Join-Path (Get-Location).Path 'dotnet-cwd'
$runtime = if ($env:PATH_TEST_RELATIVE -eq '1') { 'runtime' } else { Join-Path (Get-Location).Path 'runtime' }
. $prefix -RunDir $runtime -EvidenceDir $env:PATH_TEST_EVIDENCE
Invoke-Expression $function.Extent.Text
$minimalPath = "$env:WINDIR\System32;$env:WINDIR"
$result = Run-Clean @('/?') 'launch'
if ($result.Exit -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $env:PATH_TEST_EVIDENCE 'launch.stdout.txt'))) { throw 'Process evidence missing' }
'''
os.environ['PATH_TEST_SCRIPT'] = str(repo / 'tools/verify_runtime.ps1')
for relative in ('0', '1'):
    os.environ['PATH_TEST_RELATIVE'] = relative
    os.environ['PATH_TEST_EVIDENCE'] = 'evidence-' + relative
    run('runtime-relative-' + relative, ['pwsh', '-NoProfile', '-Command', script])
(output / 'results.json').write_text(json.dumps(records, indent=2))
raise SystemExit(1 if any(row['exit_code'] for row in records) else 0)
