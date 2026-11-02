"""Keep wall-clock acceptance gates discoverable without dropping default coverage."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('inventory', type=Path)
args = parser.parse_args()
tests = {test['name']: test for test in json.loads(args.inventory.read_text(encoding='utf-8-sig'))['tests']}
for name in ['desktop_integration', 'model_performance']:
    props = {prop['name']: prop['value'] for prop in tests[name]['properties']}
    assert 'performance' in props.get('LABELS', []), f'{name} wall-clock gates lack performance label'
    print('PASS', name, props['LABELS'])
presets = json.loads((Path(__file__).resolve().parents[1] / 'CMakePresets.json').read_text())
for name in ['release', 'release-full']:
    preset = next(p for p in presets['testPresets'] if p['name'] == name)
    assert 'filter' not in preset, f'{name} must retain all acceptance gates'
print('PASS default acceptance presets retain the full suite')
