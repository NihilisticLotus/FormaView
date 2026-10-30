"""Compare the actual preview tree for a PSK and its three FBX export modes.

Usage: python skeleton_roundtrip_regression.py PROBE REPORT PSK MODEL COMBINED ANIMATION
The caller supplies the same reference skeleton for all three exports.
"""
import json
import os
import subprocess
import sys


def hierarchy(sample):
    rows = sample['skeleton_tree']
    names = {row['id']: row['name'] for row in rows}
    assert len(names) == len(rows), 'Duplicate UI node IDs'
    assert len(set(names.values())) == len(rows), 'Fixture needs unique bone names'
    return {row['name']: names.get(row['parent']) for row in rows}


if __name__ == '__main__':
    probe, report, *inputs = sys.argv[1:]
    assert len(inputs) == 4, 'PSK plus model, combined and animation FBX required'
    env = dict(os.environ, FORMAVIEW_PROBE_SOURCE_DETAILS='1')
    subprocess.run([probe, report, *inputs], env=env, check=True, timeout=180)
    with open(report, encoding='utf-8') as stream:
        samples = json.load(stream)['samples']
    reference = hierarchy(samples[0])
    assert reference and '—' not in reference
    for sample in samples[1:]:
        actual = hierarchy(sample)
        assert actual == reference, (sample['path'], len(actual), len(reference),
                                     actual.keys() - reference.keys())
    print(f'PASS: all three FBX modes preserve {len(reference)} bone names and parents')
