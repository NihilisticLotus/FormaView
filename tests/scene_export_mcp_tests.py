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


async def main():
    parser=argparse.ArgumentParser()
    for key in ('build','fixtures','adapter','output'):parser.add_argument('--'+key,type=Path,required=True)
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
        async with stdio_client(StdioServerParameters(command=sys.executable,args=[str(args.adapter)],env=env)) as (read,write):
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
                # Native cancellation uses the same plan ID exposed over MCP.
                pending=[dict(items[2],output=str(args.output/('cancel-'+str(i)+'.fbx'))) for i in range(32)]
                p=await call('create_model_export_plan',items=pending)
                await call('cancel_image_work',kind='plan',work_id=p['plan_id'])
                cancelled=await terminal('plan',p['plan_id']);assert cancelled['state']=='cancelled',cancelled
                assert hashes=={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in hashes}
                (args.output/'passed.json').write_text(json.dumps(dict(passed=True,outputs=len(job['items']),changed_buffer=True,batch_source_protection=True,cancellation=True)),encoding='utf-8')
                print('Generic MCP exports passed:',len(job['items']))
    finally:
        (args.output/'mcp-records.json').write_text(json.dumps(records,ensure_ascii=False,indent=2),encoding='utf-8')
        service.terminate();await service.wait()
        (args.output/'service-stderr.log').write_bytes(await service.stderr.read())


if __name__=='__main__':asyncio.run(main())
