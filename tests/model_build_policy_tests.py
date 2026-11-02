"""Verify generated MSVC projects apply first-party diagnostics to the glTF parser."""
import argparse
from pathlib import Path
import xml.etree.ElementTree as ET

parser = argparse.ArgumentParser()
parser.add_argument('--build', type=Path, required=True)
args = parser.parse_args()
namespace = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}
owners = []
for project in args.build.rglob('*.vcxproj'):
    tree = ET.parse(project)
    sources = [item.attrib.get('Include', '').replace('\\', '/') for item in tree.findall('.//m:ClCompile', namespace)]
    if not any(source.endswith('/gltf_anim.cpp') for source in sources):
        continue
    owners.append(project)
    assert not any(source.endswith('/cgltf_impl.cpp') for source in sources), 'First-party glTF animation is compiled inside the vendor target'
    release = next(group for group in tree.findall('.//m:ItemDefinitionGroup', namespace) if 'Release|x64' in group.attrib.get('Condition', ''))
    for setting, expected in [('WarningLevel', 'Level4'), ('TreatWarningAsError', 'true'), ('ConformanceMode', 'true')]:
        actual = release.find('m:ClCompile/m:' + setting, namespace)
        assert actual is not None and actual.text == expected, (project.name, setting, None if actual is None else actual.text)
    print('PASS', project.name, '/W4 /WX /permissive-')
assert len(owners) == 1, owners
