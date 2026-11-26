"""Exercise generic FBX inputs through the real MCP/service durable job path."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from scene_export_media_tests import external_fbx


async def main():
    parser=argparse.ArgumentParser()
    for key in ('build','fixtures','adapter','output'):parser.add_argument('--'+key,type=Path,required=True)
    parser.add_argument('--abc-fixture', type=Path, help='Optional polygon cache with at least three samples')
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    fixtures=args.output/'fixtures';shutil.copytree(args.fixtures/'fixtures',fixtures)
    shutil.copyfile(args.fixtures/'glb.fbx',fixtures/'source.fbx')
    journal=args.output/'journal';journal.mkdir()
    scope=args.output/'scope.json';scope.write_text(json.dumps(dict(input_roots=[str(args.output)],output_roots=[str(args.output)],allow_overwrite=True,policy_revision='scene-mcp-test')),encoding='utf-8')
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in fixtures.iterdir()}
    endpoint='formaview-scenes-'+uuid.uuid4().hex
    env=dict(os.environ,FORMAVIEW_IMAGE_ENDPOINT=endpoint)
    records=[]
    service=await asyncio.create_subprocess_exec(str(args.build/'Release/formaview_image_service.exe'),'--scope',str(scope),'--journal',str(journal),'--endpoint',endpoint,stdout=asyncio.subprocess.PIPE,stderr=asyncio.subprocess.PIPE,creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        assert json.loads(await asyncio.wait_for(service.stdout.readline(),15))['ready']
        command = [str(args.adapter)] if args.adapter.suffix.lower() == '.exe' else [sys.executable, str(args.adapter)]
        async with stdio_client(StdioServerParameters(command=command[0],args=command[1:],env=env)) as (read,write):
            async with ClientSession(read,write) as client:
                await client.initialize()
                async def call(tool,**kwargs):
                    reply=await client.call_tool(tool,kwargs)
                    assert not reply.isError,reply
                    data=json.loads(next(c.text for c in reply.content if c.type=='text'))
                    records.append(dict(tool=tool,request=kwargs,response=data))
                    return data
                async def terminal(kind,id):
                    deadline=time.monotonic()+60
                    while time.monotonic()<deadline:
                        result=await call('get_image_'+kind,**{kind+'_id':id})
                        if result['state'] not in ('planning','queued','running'):return result
                        await asyncio.sleep(.05)
                    raise AssertionError('Export service did not finish')
                jpeg = await call('get_image_capabilities', source_extension='.JpG')
                assert jpeg['kind'] == 'image' and 'jpeg' in jpeg['output_formats']
                abc = await call('get_image_capabilities', source_extension='abc')
                assert abc['output_formats'] == ['fbx'] and 'abc_sample' in abc['frame_export']['options']
                unsupported = await call('get_image_capabilities', source_extension='mp4')
                assert unsupported['output_formats'] == []

                async def export(tool, input, filename, **options):
                    output = args.output / filename
                    queued = await call(tool, input=str(input), output=str(output), **options)
                    plan = await terminal('plan', queued['plan_id'])
                    assert plan['state'] == 'ready' and not output.exists(), plan
                    job = await call('submit_image_job', plan_id=plan['id'], approval_digest=plan['approval_digest'],
                                     acknowledge_losses=True, idempotency_key=filename)
                    done = await terminal('job', job['job_id'])
                    assert done['state'] == 'succeeded', done
                    result = done['items'][0]['result']
                    assert hashlib.sha256(output.read_bytes()).hexdigest() == result['output_sha256']
                    return result

                png = fixtures / 'pixel.png'
                await export('plan_image_conversion', png, 'preserved.TIF')
                source_pixel = await call('read_texture_pixel', path=str(png), x=0, y=0)
                output_pixel = await call('read_texture_pixel', path=str(args.output/'preserved.TIF'), x=0, y=0)
                assert output_pixel['channels'] == source_pixel['channels']
                rejected = await call('plan_image_conversion', input=str(png), output=str(args.output/'unapproved.jpg'))
                rejected = await terminal('plan', rejected['plan_id'])
                assert rejected['items'][0]['status'] == 'failed' and not (args.output/'unapproved.jpg').exists()
                await export('plan_image_conversion', png, 'explicit.JPG', alpha_action='drop', allow_lossy=True)
                assert (args.output/'explicit.JPG').read_bytes().startswith(b'\xff\xd8')

                glb = fixtures / 'source.glb'
                wrong_sample = await client.call_tool('plan_fbx_export', dict(
                    input=str(glb), output=str(args.output/'wrong-sample.fbx'), abc_sample=1))
                assert wrong_sample.isError and not (args.output/'wrong-sample.fbx').exists()
                clips = (await call('inspect_model', input=str(glb)))['clips']
                result = await export('plan_fbx_export', glb, 'unreal-cm.fbx', output_axes='unreal',
                                      output_unit='cm', clip_ids=[clip['id'] for clip in clips])
                assert result['output_coordinates']['preset'] == 'unreal'
                report = args.output / 'unreal-readback.json'
                subprocess.run([str(args.build/'tests/Release/formaview_scene_export_probe.exe'),
                                str(args.output/'unreal-cm.fbx'), str(report)], check=True, timeout=30)
                back = json.loads(report.read_text())
                assert abs(back['unit'] - .01) < 1e-8 and len(back['clips']) == len(clips)

                split_dir = args.output / 'split'
                batch_dir = args.output / 'batch'
                batch_dir.mkdir()
                mesh = fixtures / '静态.obj'
                shutil.copyfile(fixtures / 'source.obj', mesh)
                batch_args = dict(inputs=[str(mesh), str(glb)], output_directory=str(batch_dir),
                                  output_axes='unreal', output_unit='cm')
                ambiguous = await call('plan_fbx_batch', **batch_args, name_template='{name}-{clip}.{ext}')
                assert ambiguous['error']['code'] == 'invalid_argument' and 'plan_id' not in ambiguous
                for animations in (True, False):
                    batch = await call('plan_fbx_batch', **batch_args, include_animations=animations,
                                       conflict='skip' if animations else 'rename',
                                       name_template=None if animations else '{index}-{name}-{clip}.{ext}')
                    plan = await terminal('plan', batch['plan_id'])
                    assert plan['state'] == 'ready' and plan['total'] == 2, plan
                    if animations:
                        assert not list(batch_dir.iterdir())
                    queued = await call('submit_image_job', plan_id=plan['id'], approval_digest=plan['approval_digest'],
                                        acknowledge_losses=True, idempotency_key='batch-fbx-' + str(animations))
                    written = await terminal('job', queued['job_id'])
                    assert written['state'] == 'succeeded', written
                    for ordinal, (row, count) in enumerate(zip(written['items'], (0, len(clips) if animations else 0), strict=True), 1):
                        result = row['result']
                        output = Path(result['committed_output'])
                        if not animations:
                            assert output.name == f'{ordinal}-{Path(row["request"]["input"]).stem}-model.fbx'
                        assert output.parent == batch_dir and result['output_coordinates']['preset'] == 'unreal'
                        assert hashlib.sha256(output.read_bytes()).hexdigest() == result['output_sha256']
                        report = args.output / 'batch-readback.json'
                        subprocess.run([str(args.build/'tests/Release/formaview_scene_export_probe.exe'),
                                        str(output), str(report)], check=True, timeout=30)
                        back = json.loads(report.read_text(encoding='utf-8'))
                        assert back['meshes'] and len(back['clips']) == count and abs(back['unit'] - .01) < 1e-8
                assert len(list(batch_dir.glob('*.fbx'))) == 4
                if args.abc_fixture:
                    cache = fixtures / '采样.abc'
                    shutil.copyfile(args.abc_fixture, cache)
                    args_abc = dict(inputs=[str(cache)], output_directory=str(batch_dir))
                    assert (await client.call_tool('plan_fbx_batch', args_abc)).isError
                    batch = await call('plan_fbx_batch', **args_abc, abc_sample=2)
                    plan = await terminal('plan', batch['plan_id'])
                    assert plan['state'] == 'ready' and not (batch_dir / '采样.fbx').exists(), plan
                    queued = await call('submit_image_job', plan_id=plan['id'], approval_digest=plan['approval_digest'],
                                        acknowledge_losses=True, idempotency_key='batch-abc')
                    written = await terminal('job', queued['job_id'])
                    assert written['state'] == 'succeeded', written
                    result = written['items'][0]['result']
                    assert result['source_metadata']['abc']['sample'] == 2, result
                    report = args.output / 'abc-batch-readback.json'
                    subprocess.run([str(args.build/'tests/Release/formaview_scene_export_probe.exe'),
                                    result['committed_output'], str(report)], check=True, timeout=30)
                    back = json.loads(report.read_text(encoding='utf-8'))
                    assert back['meshes'] and not back['clips'], back
                split_dir.mkdir()
                arguments = dict(input=str(glb), output_directory=str(split_dir), output_unit='cm')
                for invalid_ids in ([], ['missing'], [clips[0]['id']] * 2):
                    assert (await client.call_tool('plan_fbx_clips', dict(arguments, clip_ids=invalid_ids))).isError
                split = await call('plan_fbx_clips', **arguments)
                plan = await terminal('plan', split['plan_id'])
                assert plan['state'] == 'ready' and plan['total'] == len(clips) and not list(split_dir.iterdir()), plan
                queued = await call('submit_image_job', plan_id=plan['id'], approval_digest=plan['approval_digest'],
                                    acknowledge_losses=True, idempotency_key='split-clips')
                job = await terminal('job', queued['job_id'])
                assert job['state'] == 'succeeded', job
                for index, clip in enumerate(clips, 1):
                    output = split_dir / f"source-{index:02d}-{clip['name']}.fbx"
                    inspected = await call('inspect_model', input=str(output))
                    assert [c['name'] for c in inspected['clips']] == [clip['name']]

                last_clip = clips[-1]
                selected = await call('plan_fbx_clips', **arguments, clip_ids=[last_clip['id']],
                                      name_template='{index}-{name}-{clip}.{ext}')
                plan = await terminal('plan', selected['plan_id'])
                output = split_dir / f"1-source-{last_clip['name']}.fbx"
                assert plan['state'] == 'ready' and Path(plan['items'][0]['preflight']['output']) == output
                assert not output.exists()
                queued = await call('submit_image_job', plan_id=plan['id'], approval_digest=plan['approval_digest'],
                                    acknowledge_losses=True, idempotency_key='named-clip')
                assert (await terminal('job', queued['job_id']))['state'] == 'succeeded'
                back = await call('inspect_model', input=str(output))
                assert [c['name'] for c in back['clips']] == [last_clip['name']]

                items=[]
                for suffix in ('obj','gltf','glb','pmx','vrm','fbx'):
                    source=str(fixtures/('source.'+suffix))
                    inspected=await call('inspect_model',input=source)
                    options=dict(target='fbx')
                    if inspected['clips']:options['clip_ids']=[c['id'] for c in inspected['clips']]
                    items.append(dict(input=source,output=str(args.output/(suffix+'.fbx')),options=options))
                    if suffix=='fbx':
                        for c in inspected['clips']:items.append(dict(input=source,output=str(args.output/(c['name']+'.fbx')),options=dict(target='fbx',clip_id=c['id'])))
                        items.append(dict(input=source,output=str(args.output/'animation-only.fbx'),options=dict(options,include_mesh=False)))
                created=await call('create_model_export_plan',items=items)
                plan=await terminal('plan',created['plan_id']);assert plan['state']=='ready',plan
                submit=dict(plan_id=created['plan_id'],approval_digest=plan['approval_digest'],acknowledge_losses=True,idempotency_key='generic-first')
                queued=await call('submit_image_job',**submit)
                job=await terminal('job',queued['job_id']);assert job['state']=='succeeded',job
                assert (await call('submit_image_job',**submit))['job_id']==queued['job_id']
                for i,item in enumerate(job['items']):
                    result=item['result'];output=Path(result['committed_output'])
                    assert hashlib.sha256(output.read_bytes()).hexdigest()==result['output_sha256']
                    report=args.output/('readback-'+str(i)+'.json')
                    subprocess.run([str(args.build/'tests/Release/formaview_scene_export_probe.exe'),str(output),str(report)],check=True,timeout=30)
                    actual=json.loads(report.read_text(encoding='utf-8'))
                    assert len(actual['clips'])==len(result['clips'])
                    assert bool(actual['meshes'])==(item['request']['options'].get('include_mesh',True))
                # Malformed sparse storage must produce a normal error without
                # terminating the shared process or preventing the next read.
                document=json.loads((fixtures/'source.gltf').read_text())
                document['bufferViews'].append(dict(buffer=0,byteOffset=1024*1024,byteLength=1))
                document['accessors'][0]['sparse']=dict(count=1,indices=dict(bufferView=len(document['bufferViews'])-1,componentType=5121),values=dict(bufferView=0))
                malformed=fixtures/'malformed.gltf';malformed.write_text(json.dumps(document),encoding='utf-8')
                assert 'error' in await call('inspect_model',input=str(malformed))
                assert 'error' not in await call('inspect_model',input=str(fixtures/'source.gltf'))
                # A changed external binary invalidates the exact preflight approval.
                changed=dict(items[1],output=str(args.output/'changed.fbx'))
                p=await call('create_model_export_plan',items=[changed]);p=await terminal('plan',p['plan_id'])
                buffer=fixtures/'source.bin';original=buffer.read_bytes();data=bytearray(original);data[0:4]=b'\0\0\x80?';buffer.write_bytes(data)
                try:
                    q=await call('submit_image_job',plan_id=p['id'],approval_digest=p['approval_digest'],acknowledge_losses=True,idempotency_key='changed-buffer')
                    failed=await terminal('job',q['job_id']);assert failed['state']=='failed',failed
                    assert not Path(changed['output']).exists()
                finally:buffer.write_bytes(original)
                # Every batch output is checked against other inputs' dependencies.
                auxiliary=fixtures/'protected.fbx';auxiliary.write_bytes(original)
                document=json.loads((fixtures/'source.gltf').read_text());document['buffers'][0]['uri']='protected.fbx'
                owner=fixtures/'protected.gltf';owner.write_text(json.dumps(document),encoding='utf-8')
                protected=[dict(items[0],output=str(auxiliary),conflict='overwrite'),dict(items[1],input=str(owner),output=str(args.output/'protected-owner.fbx'))]
                p=await call('create_model_export_plan',items=protected);p=await terminal('plan',p['plan_id'])
                assert p['items'][0]['status']=='failed',p
                assert auxiliary.read_bytes()==original
                # A missing texture still reserves its referenced path against
                # other outputs: a tolerated read loss is not write permission.
                missing=fixtures/'missing-texture.fbx'
                document=json.loads((fixtures/'source.gltf').read_text())
                document['images'][0]['uri']=missing.name
                owner=fixtures/'missing-texture-owner.gltf';owner.write_text(json.dumps(document),encoding='utf-8')
                protected=[dict(items[0],output=str(missing)),dict(items[1],input=str(owner),output=str(args.output/'missing-owner.fbx'))]
                p=await call('create_model_export_plan',items=protected);p=await terminal('plan',p['plan_id'])
                assert p['items'][0]['status']=='failed',p
                assert not missing.exists()
                # Relocated FBX media participates in cross-item protection,
                # even when its original absolute filename no longer exists.
                relative_texture=fixtures/'relative-texture.fbx'
                relative_texture.write_bytes((fixtures/'pixel.png').read_bytes())
                relative_owner=fixtures/'relative-owner.fbx'
                external_fbx(relative_owner, fixtures/'old-location/pixel.png', relative_texture.name)
                protected=[dict(items[0],output=str(relative_texture),conflict='overwrite'),
                           dict(input=str(relative_owner),output=str(args.output/'relative-owner.fbx'),options=dict(target='fbx'))]
                p=await call('create_model_export_plan',items=protected);p=await terminal('plan',p['plan_id'])
                assert p['items'][0]['status']=='failed',p
                assert relative_texture.read_bytes()==(fixtures/'pixel.png').read_bytes()
                # Images and models share one durable job, result pagination and
                # retry semantics. A changed model dependency must not undo the
                # successful image or publish an unapproved model.
                image_output=args.output/'mixed.png'
                model_output=args.output/'mixed.fbx'
                mixed=[dict(input=str(fixtures/'pixel.png'),output=str(image_output),
                            options=dict(target='png',channels=[dict(source=c,destination=c) for c in 'RGBA'],
                                         alpha_channel='A',source_alpha='straight',alpha_action='preserve')),
                       dict(input=str(relative_owner),output=str(model_output),options=dict(target='fbx'))]
                p=await call('create_image_plan',items=mixed);p=await terminal('plan',p['plan_id'])
                assert p['state']=='ready',p
                page=await call('get_image_plan',plan_id=p['id'],offset=0,limit=1)
                assert page['total']==2 and page['next_offset']==1 and len(page['items'])==1,page
                texture_bytes=relative_texture.read_bytes()
                relative_texture.write_bytes(texture_bytes+b'changed after preflight')
                try:
                    q=await call('submit_image_job',plan_id=p['id'],approval_digest=p['approval_digest'],
                                 acknowledge_losses=True,idempotency_key='mixed-partial')
                    partial=await terminal('job',q['job_id'])
                finally:
                    relative_texture.write_bytes(texture_bytes)
                assert partial['state']=='partially_succeeded',partial
                assert [x['status'] for x in partial['items']]==['succeeded','failed'],partial
                assert partial['items'][1]['error']['code']=='input_changed',partial
                assert not model_output.exists()
                image_hash=hashlib.sha256(image_output.read_bytes()).hexdigest()
                assert image_hash==partial['items'][0]['result']['output_sha256']
                info=await call('inspect_image',path=str(image_output))
                assert 'error' not in info and info['total']==1,info
                page=await call('get_image_job',job_id=q['job_id'],offset=1,limit=1)
                assert page['total']==2 and page['next_offset'] is None and page['items'][0]['status']=='failed',page
                retry=await call('retry_image_plan',job_id=q['job_id']);retry=await terminal('plan',retry['plan_id'])
                assert retry['state']=='ready' and retry['total']==1,retry
                assert retry['items'][0]['request']['input']==str(relative_owner),retry
                q=await call('submit_image_job',plan_id=retry['id'],approval_digest=retry['approval_digest'],
                             acknowledge_losses=True,idempotency_key='mixed-retry')
                retried=await terminal('job',q['job_id']);assert retried['state']=='succeeded',retried
                assert image_hash==hashlib.sha256(image_output.read_bytes()).hexdigest()
                report=args.output/'mixed-readback.json'
                subprocess.run([str(args.build/'tests/Release/formaview_scene_export_probe.exe'),str(model_output),str(report)],check=True,timeout=30)
                actual=json.loads(report.read_text(encoding='utf-8'))
                assert len(actual['meshes'])==1 and len(actual['media'])==1,actual
                assert actual['media'][0]['sha256']==hashlib.sha256(relative_texture.read_bytes()).hexdigest(),actual
                # Native cancellation uses the same plan ID exposed over MCP.
                pending=[dict(items[2],output=str(args.output/('cancel-'+str(i)+'.fbx'))) for i in range(32)]
                p=await call('create_model_export_plan',items=pending)
                await call('cancel_image_work',kind='plan',work_id=p['plan_id'])
                cancelled=await terminal('plan',p['plan_id']);assert cancelled['state']=='cancelled',cancelled
                assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in hashes}
                (args.output/'passed.json').write_text(json.dumps(dict(passed=True,outputs=len(job['items']),malformed_input_survival=True,changed_buffer=True,batch_source_protection=True,missing_texture_protection=True,relative_texture_protection=True,mixed_partial_retry=True,pagination=True,cancellation=True)),encoding='utf-8')
                print('Generic MCP exports passed:',len(job['items']))
    finally:
        (args.output/'mcp-records.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
        service.terminate();await service.wait()
        (args.output/'service-stderr.log').write_bytes(await service.stderr.read())


if __name__=='__main__':asyncio.run(main())
