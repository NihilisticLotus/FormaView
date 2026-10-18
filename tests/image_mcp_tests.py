"""Real official MCP SDK client -> stdio adapter -> native C++ host test."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--native',type=Path,required=True)
    parser.add_argument('--server',type=Path,required=True)
    parser.add_argument('--fixture',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=False)
    inputs=args.output/'input';outputs=args.output/'output';inputs.mkdir();outputs.mkdir()
    source=inputs/'图像.png';shutil.copyfile(args.fixture,source)
    policy=args.output/'scope.json'
    policy.write_text(json.dumps({'input_roots':[str(inputs.resolve())],'output_roots':[str(outputs.resolve())],
        'allow_overwrite':False,'policy_revision':'mcp-image-test-1'}),encoding='utf-8')
    env=os.environ.copy();env.update(FORMAVIEW_IMAGE_CLI=str(args.native.resolve()),FORMAVIEW_IMAGE_SCOPE=str(policy.resolve()))
    parameters=StdioServerParameters(command=sys.executable,args=[str(args.server.resolve())],env=env)
    records=[]
    async with stdio_client(parameters) as (reader,writer):
        async with ClientSession(reader,writer) as session:
            init=await session.initialize()
            tools=await session.list_tools()
            names={t.name for t in tools.tools}
            assert names=={'get_image_capabilities','inspect_image','preflight_image','convert_image'},names
            async def call(name,arguments):
                result=await session.call_tool(name,arguments)
                assert not result.isError,result
                payload=result.structuredContent
                if payload is None:payload=json.loads(next(c.text for c in result.content if c.type=='text'))
                records.append({'tool':name,'arguments':arguments,'result':payload})
                return payload
            info=await call('inspect_image',{'path':str(source.resolve()),'limit':1})
            assert [c['name'] for c in info['selected_channels']]==['R','G','B','A']
            assert info['total']==1 and info['next_offset'] is None
            assert info['input']['sha256']==hashlib.sha256(source.read_bytes()).hexdigest()
            request={'input':str(source.resolve()),'output':str((outputs/'已转换.tiff').resolve()),'conflict':'skip',
                'options':{'target':'tiff','channels':[{'source':c,'destination':c} for c in 'RGBA'],
                           'alpha_channel':'A','source_alpha':'straight'}}
            before=list(outputs.iterdir())
            plan=await call('preflight_image',{'request':request})
            assert 'error' not in plan,plan
            assert list(outputs.iterdir())==before,'Preflight wrote output'
            wrong=await call('convert_image',{'request':request,'approval_digest':'wrong','acknowledge_losses':True})
            assert wrong['error']['code']=='input_changed'
            done=await call('convert_image',{'request':request,'approval_digest':plan['approval_digest'],'acknowledge_losses':True})
            assert done['status']=='succeeded',done
            result_path=Path(done['committed_output'])
            assert result_path.is_file() and hashlib.sha256(result_path.read_bytes()).hexdigest()==done['output_sha256']
            skipped=await call('convert_image',{'request':request,'approval_digest':plan['approval_digest'],'acknowledge_losses':True})
            assert skipped['status']=='skipped',skipped
            out_scope=await call('inspect_image',{'path':str(args.fixture.resolve())})
            assert out_scope['error']['code']=='out_of_scope'
            changed=dict(request,options=dict(request['options'],quality=55))
            mismatch=await call('convert_image',{'request':changed,'approval_digest':plan['approval_digest'],'acknowledge_losses':True})
            assert mismatch['error']['code']=='input_changed'
            lossy=dict(request,output=str((outputs/'lossy.jpg').resolve()),options={
                'target':'jpeg','channels':[{'source':c,'destination':c} for c in 'RGB'],
                'alpha_channel':'A','source_alpha':'straight','alpha_action':'drop','allow_lossy':True})
            loss_plan=await call('preflight_image',{'request':lossy})
            assert {'lossy_codec','alpha_removed'}.issubset({v['code'] for v in loss_plan['losses']})
            no_ack=await call('convert_image',{'request':lossy,'approval_digest':loss_plan['approval_digest']})
            assert no_ack['error']['code']=='invalid_argument'
            overwritten=dict(request,conflict='overwrite')
            blocked=await call('preflight_image',{'request':overwritten})
            assert blocked['error']['code']=='out_of_scope'
    report={'initialized':init.model_dump(mode='json'),'tools':[t.model_dump(mode='json') for t in tools.tools],
            'calls':len(records),'passed':True,'native':str(args.native),'results':records,
            'not_covered':['durable jobs','idempotency','disconnect recovery','explicit cancellation','GUI integration','performance']}
    (args.output/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'calls':len(records),'passed':True,'protocol':init.protocolVersion}))


if __name__=='__main__':asyncio.run(main())
