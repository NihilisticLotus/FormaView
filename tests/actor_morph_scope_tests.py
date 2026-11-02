"""Animation-only ActorX export must not reject unused mesh morph chunks."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser()
    for name in ('build', 'fixtures', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    mesh = args.output / 'morph.psk'
    mesh.write_bytes((args.fixtures / 'source.psk').read_bytes()
                    + struct.pack('<20siii', b'MRPHINFO', 1999801, 1, 1) + b'\0')
    animation = args.output / 'motion.psa'
    animation.write_bytes((args.fixtures / 'source.psa').read_bytes())
    policy = args.output / 'scope.json'
    policy.write_text(json.dumps(dict(input_roots=[str(args.output.resolve())],
        output_roots=[str(args.output.resolve())], policy_revision='actor-morph-scope')), encoding='utf-8')
    records = []

    def call(operation, **values):
        request = dict(version=1, operation=operation, **values)
        process = subprocess.run([str(args.build / 'Release/formaview_image_cli.exe'), '--scope', str(policy)],
            input=json.dumps(request) + '\n', text=True, encoding='utf-8', capture_output=True, timeout=60)
        response = json.loads(process.stdout)
        records.append(dict(request=request, response=response, exit_code=process.returncode))
        (args.output / 'requests.json').write_text(json.dumps(records, indent=2), encoding='utf-8')
        return response

    inspected = call('inspect', input=str(animation))
    clip = inspected['clips'][0]['id']
    for primary_mesh in (False, True):
        options = dict(target='fbx', clip_id=clip, include_mesh=False,
                       coordinates='actorx-z-up-x-forward-rh-cm')
        options['animation' if primary_mesh else 'reference'] = str(animation if primary_mesh else mesh)
        output = args.output / f'animation-{primary_mesh}.fbx'
        request = dict(input=str(mesh if primary_mesh else animation), output=str(output), options=options)
        plan = call('preflight', **request)
        assert 'error' not in plan, ('Unused mesh morph rejected animation-only output', plan)
        assert any(loss['code'] == 'actorx_chunk_omitted' and loss['detail'] == 'MRPHINFO'
                   for loss in plan['losses']), plan
        result = call('convert', **request, approval_digest=plan['approval_digest'], acknowledge_losses=True)
        assert 'error' not in result and output.exists(), result
        probe = output.with_suffix('.json')
        subprocess.run([str(args.build / 'tests/Release/formaview_scene_export_probe.exe'), str(output), str(probe)], check=True)
        readback = json.loads(probe.read_text(encoding='utf-8'))
        assert not readback['meshes'] and len(readback['clips']) == 1, readback
        request['options'] = dict(options, include_mesh=True)
        rejected = call('preflight', **request)
        assert rejected['error']['code'] == 'unsupported', rejected
    print('PASS PSA/reference and PSK/animation export omit mesh, report morph loss, retain mesh refusal')


if __name__ == '__main__':
    main()
