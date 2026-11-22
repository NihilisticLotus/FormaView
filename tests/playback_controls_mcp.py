"""Packaged localhost walkthrough. No desktop input or window captures."""
import asyncio
import json
import os
from pathlib import Path
import subprocess
import sys
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
from desktop_mcp_tests import window_count


async def run(runtime, clip, out, window=False):
    out.mkdir(parents=True, exist_ok=False)
    fixtures = out / 'fixtures'
    fixtures.mkdir()
    frames = out / 'frames'
    frames.mkdir()
    subtitle = fixtures / '外挂字幕.srt'
    subtitle.write_text('1\n00:00:50,000 --> 00:01:20,000\nFormaView MCP subtitle test\n', 'utf8')
    dual = fixtures / 'dual.mkv'
    chapters = fixtures / 'chapters.txt'
    chapters.write_text(';FFMETADATA1\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=0\nEND=2000\ntitle=One\n'
                        '[CHAPTER]\nTIMEBASE=1/1000\nSTART=2000\nEND=4000\ntitle=Two\n', 'utf8')
    subprocess.run(['ffmpeg','-v','error','-i',str(clip),'-i',str(chapters),'-map_chapters','1','-t','4','-map','0:v:0','-map','0:a:0',
        '-map','0:a:0','-c','copy',str(dual)],check=True)
    scope = out/'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(clip.parent),str(fixtures)],
        output_roots=[str(frames)],allow_overwrite=False)), 'utf8')
    env = {k:v for k,v in os.environ.items() if not k.startswith('FORMAVIEW_')}
    env['FORMAVIEW_IMAGE_STATE_ROOT'] = str(out/'journal')
    records = []
    report = dict(runtime=str(runtime),records=records,mode='window' if window else 'background')
    host = None
    try:
        if window:
            host = await asyncio.create_subprocess_exec(str(runtime/'formaview.exe'),'--mcp-scope',str(scope),
                '--trace',str(out/'trace.json'),env=env,stdout=asyncio.subprocess.PIPE)
            while line := await asyncio.wait_for(host.stdout.readline(),30):
                if line.startswith(b'{'):
                    env.update(FORMAVIEW_IMAGE_ENDPOINT=json.loads(line)['endpoint'],FORMAVIEW_DESKTOP='1')
                    break
            else:
                raise RuntimeError('Window endpoint failed to start')
        else:
            env['FORMAVIEW_DESKTOP_SCOPE'] = str(scope)
        params = StdioServerParameters(command=sys.executable,args=[str(runtime/'mcp/image_stdio.py')],env=env)
        async with stdio_client(params) as io, ClientSession(*io) as c:
            report['protocol'] = (await c.initialize()).protocolVersion
            report['tools'] = sorted(t.name for t in (await c.list_tools()).tools)
            asset = None
            async def call(tool, error=None, **args):
                if tool != 'open_asset':
                    args.setdefault('asset_id',asset)
                response = await c.call_tool(tool,args)
                res = response.structuredContent or json.loads(response.content[0].text)
                records.append(dict(tool=tool,args=args,result=res))
                assert not response.isError and res.get('error',{}).get('code') == error,(tool,res)
                return res
            first = await call('open_asset',path=str(clip))
            asset = first['asset_id']
            report['windows'] = window_count(first['host_pid'])
            assert (report['windows'] > 0) == window
            await call('control_video',action='play')
            ls = await call('video_playlist')
            assert Path(ls['items'][ls['current']]['path']).samefile(clip)
            for action in ('next','previous','select'):
                entry = await call('video_playlist',action=action,index=ls['current'])
                assert Path(entry['path']).name == ('02-4K.mp4' if action=='next' else clip.name)
                asset = entry['asset_id']
                assert not entry['paused']
            await call('control_video',action='pause')
            original = await call('get_playback_state')
            for name, field in [('mute','muted'), ('sub-visibility','subtitles_visible'), ('loop-file','repeat_file')]:
                for enabled in [not original[field], original[field]]:
                    changed = await call('set_video_toggle', name=name, enabled=enabled)
                    assert changed[field] == enabled and changed['subtitle_id'] == original['subtitle_id']
                    assert changed['volume'] == original['volume']
            for delay in (-350, 1250, 0):
                changed = await call('control_video', action='subtitle_delay_ms', value=delay)
                assert changed['subtitle_delay_ms'] == delay
            await call('control_video', action='subtitle_delay_ms', value=60001, error='invalid_argument')
            await call('control_video', action='seek', value=6)
            for delta, expected in [(4,10),(-6,4)]:
                changed = await call('control_video', action='seek_relative', value=delta)
                assert abs(changed['position'] - expected) < .03
            await call('control_video', action='seek_relative', value=-20, error='invalid_argument')
            for rate in (8, 1):
                assert (await call('control_video',action='speed',value=rate))['speed'] == rate
            await call('video_chapters')
            await call('video_playlist',action='previous',error='invalid_argument')
            tracks = (await call('list_video_tracks'))['tracks']
            for kind in ('audio','sub'):
                track = next(t for t in tracks if t['type']==kind)
                await call('select_video_track',type=kind,id=0)
                await call('select_video_track',type=kind,id=track['id'])
            await call('select_video_track',type='audio',id=4000,error='invalid_argument')
            await call('load_video_subtitle',path=str(out/'outside.srt'),error='out_of_scope')
            await call('control_video',action='seek',value=60)
            sid = (await call('load_video_subtitle',path=str(subtitle)))['subtitle_id']
            assert sid and sid==(await call('load_video_subtitle',path=str(subtitle)))['subtitle_id']
            assert sum(t.get('external',False) for t in (await call('list_video_tracks'))['tracks'])==1
            async def shot(name,sub=False):
                dest = frames/(name+'.png')
                await call('capture_video_frame',output=str(dest),subtitles=sub)
                return subprocess.check_output(['ffmpeg','-v','error','-i',str(dest),'-frames:v','1',
                    '-vf','scale=320:144','-pix_fmt','rgb24','-f','rawvideo','-'])
            plain = await shot('original')
            caption = await shot('subtitles',True)
            report['subtitle_changed_pixels'] = sum(plain[i:i+3]!=caption[i:i+3] for i in range(0,len(plain),3))
            assert report['subtitle_changed_pixels'] > 15
            embedded = next(t['id'] for t in tracks if t['type']=='sub')
            assert (await call('select_video_track',type='sub',id=embedded))['subtitle_id']==embedded
            assert (await call('select_video_track',type='sub',id=sid))['subtitle_id']==sid
            def chroma(data):
                return sum(max(data[i:i+3])-min(data[i:i+3]) for i in range(0,len(data),3))/(len(data)/3)
            for name,val in [('saturation',-100),('brightness',15),('contrast',20),('sharpen',.8)]:
                filters = (await call('set_video_filter',name=name,value=val))['filters']
                assert abs(filters[name]-val)<1e-6, filters
                # Window-mode filters use mpv's presentation shader; source
                # screenshots exclude that shader. Check decoded pixels in background mode.
                if not window and name=='saturation':
                    grey = await shot('grayscale')
                    report['chroma'] = [chroma(plain),chroma(grey)]
                    assert chroma(grey)<chroma(plain)*.2,report['chroma']
                elif not window:
                    changed = await shot(name)
                    assert changed != plain, name
                await call('set_video_filter',name=name,value=0)
            assert plain==await shot('restored')
            await call('video_ab_loop',action='set',a=61,b=60,error='invalid_argument')
            await call('video_ab_loop',action='set',a=60,b=60.6)
            await call('control_video',action='seek',value=60)
            await call('control_video',action='play')
            positions = []
            for _ in range(23):
                await asyncio.sleep(.09)
                positions.append((await call('get_playback_state'))['position'])
            report['loop_positions'] = positions
            assert sum(a-b>.25 for a,b in zip(positions,positions[1:]))>=2,positions
            await call('video_ab_loop',action='clear')
            await asyncio.sleep(.85)
            assert (await call('control_video',action='pause'))['position']>60.6
            asset=(await call('open_asset',path=str(dual)))['asset_id']
            await call('control_video',action='pause')
            entries = await call('video_chapters')
            assert [ch['title'] for ch in entries['chapters']] == ['One','Two']
            for index in (1,0):
                changed = await call('video_chapters',action='select',index=index)
                assert changed['chapter'] == index and abs(changed['position'] - entries['chapters'][index]['time']) < .05
            await call('video_chapters',action='select',index=2,error='invalid_argument')
            await call('set_video_toggle',name='loop-file',enabled=True)
            await call('control_video',action='seek',value=3.6)
            await call('control_video',action='play')
            for _ in range(30):
                await asyncio.sleep(.05)
                if (await call('get_playback_state'))['position'] < 1:
                    break
            else:
                raise AssertionError('File repeat did not cross EOF')
            await call('control_video',action='pause')
            await call('set_video_toggle',name='loop-file',enabled=False)
            ids=[t['id'] for t in (await call('list_video_tracks'))['tracks'] if t['type']=='audio']
            assert len(ids)==2
            for i in reversed(ids):
                assert (await call('select_video_track',type='audio',id=i))['audio_id']==i
            report['passed']=True
    finally:
        if host and host.returncode is None:
            host.terminate()
            await host.wait()
        (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),'utf8')

if __name__=='__main__':
    asyncio.run(run(*(Path(p).resolve() for p in sys.argv[1:4]), window='--window' in sys.argv[4:]))
