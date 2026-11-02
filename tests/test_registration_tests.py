"""Ensure built model/GUI probes are visible in the CTest inventory."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--ctest', required=True)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
result = subprocess.run([args.ctest, '--test-dir', str(args.build), '-C', 'Release',
                         '--show-only=json-v1'], capture_output=True, text=True, check=True)
args.output.write_text(result.stdout, encoding='utf-8')
inventory = json.loads(result.stdout)
tests = {test['name']: test for test in inventory['tests']}
required = {'desktop_integration', 'model_state', 'model_render', 'model_format',
            'model_gpu', 'model_performance', 'animation_loop'}
missing = sorted(required - tests.keys())
print('CTest count:', len(tests), 'missing:', missing)
source = (Path(__file__).resolve().parents[1] / 'tests/desktop_tests.cpp').read_text(encoding='utf-8')
absolute = [line for line in source.splitlines() if 'B:/Model' in line or 'E:/CodexProject' in line]
print('Desktop machine-specific paths:', len(absolute))
assert not missing and not absolute, 'Incomplete test registration or machine-specific desktop fixtures'
cache = (args.build / 'CMakeCache.txt').read_text(encoding='utf-8')
if 'FORMAVIEW_UI_TEST_ROOT:PATH=\n' in cache:
    properties = {p['name']: p['value'] for p in tests['desktop_integration']['properties']}
    assert properties.get('DISABLED') is True, 'Missing GUI root must remain visibly disabled'
print('PASS visible registration and portable desktop fixture paths')
