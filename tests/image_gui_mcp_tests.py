"""Real MCP client against the GUI-owned service; the GUI event loop stays live."""
import argparse
import asyncio
import json
import os
from pathlib import Path
import sys
import uuid

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from mcp.types import Implementation


async def main():
    parser = argparse.ArgumentParser()
    for name in ('endpoint', 'adapter', 'source', 'output', 'existing_job', 'report'):
        parser.add_argument('--' + name.replace('_', '-'), required=True)
    args = parser.parse_args()
    env = dict(os.environ, FORMAVIEW_IMAGE_ENDPOINT=args.endpoint)
    records = []
    async with stdio_client(StdioServerParameters(command=sys.executable, args=[args.adapter], env=env)) as (reader, writer):
        async with ClientSession(reader, writer, client_info=Implementation(name='FormaView GUI integration', version='1')) as session:
            initialized = await session.initialize()
            tools = await session.list_tools()
            assert len(tools.tools) == 13, sorted(t.name for t in tools.tools)  # 11 image tools + inspect_model + create_model_export_plan

            async def call(name, arguments):
                result = await session.call_tool(name, arguments)
                assert not result.isError, result
                data = result.structuredContent
                if data is None:
                    data = json.loads(next(c.text for c in result.content if c.type == 'text'))
                records.append({'tool': name, 'arguments': arguments, 'result': data})
                return data

            capabilities = await call('get_image_capabilities', {})
            assert len(capabilities['read_formats']) == 11
            assert len(capabilities['write_targets']) == 9
            jobs = await call('list_image_jobs', {})
            assert any(j['id'] == args.existing_job for j in jobs['items'])
            old = await call('get_image_job', {'job_id': args.existing_job})
            assert old['state'] == 'succeeded'
            image = await call('inspect_image', {'path': args.source})
            assert [c['name'] for c in image['selected_channels']] == ['R', 'G', 'B', 'A']
            planned = await call('create_image_plan', {'items': [{'input': args.source, 'output': args.output, 'conflict': 'skip',
                'options': {'target': 'png', 'channels': [{'source': c, 'destination': c} for c in 'RGBA'],
                            'source_alpha': 'straight', 'alpha_channel': 'A'}}]})
            for _ in range(200):
                plan = await call('get_image_plan', {'plan_id': planned['plan_id']})
                if plan['state'] != 'planning':
                    break
                await asyncio.sleep(.025)
            assert plan['state'] == 'ready', plan
            submitted = await call('submit_image_job', {'plan_id': plan['id'], 'approval_digest': plan['approval_digest'],
                'idempotency_key': 'gui-mcp-' + uuid.uuid4().hex, 'acknowledge_losses': True})
            for _ in range(200):
                job = await call('get_image_job', {'job_id': submitted['job_id']})
                if job['state'] not in ('queued', 'running'):
                    break
                await asyncio.sleep(.025)
            assert job['state'] == 'succeeded', job
            assert Path(args.output).is_file()
            Path(args.report).write_text(json.dumps({'protocol': initialized.protocolVersion,
                'job_id': job['id'], 'capabilities': capabilities, 'records': records}, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    asyncio.run(main())
