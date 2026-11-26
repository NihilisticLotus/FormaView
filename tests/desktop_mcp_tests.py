"""Exercise the packaged desktop through the official stdio MCP client (no UI input)."""
import argparse
import asyncio
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import time

from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client
import win32api
import win32con
import win32gui
import win32process


def window_count(pid):
    windows = []
    def inspect(hwnd, _):
        if win32process.GetWindowThreadProcessId(hwnd)[1] == pid and win32gui.IsWindowVisible(hwnd):
            windows.append(hwnd)
    win32gui.EnumWindows(inspect, None)
    return len(windows)


def gray(path, seconds=None):
    args = ['ffmpeg', '-v', 'error']
    if seconds is not None:
        args += ['-ss', str(seconds)]
    return subprocess.check_output(args + ['-i', str(path), '-map', '0:v:0', '-frames:v', '1',
        '-vf', 'scale=64:36', '-pix_fmt', 'gray', '-f', 'rawvideo', '-'])


async def exercise(args, env, endpoint=None):
    run = args.output
    if endpoint:
        env.update(FORMAVIEW_IMAGE_ENDPOINT=endpoint, FORMAVIEW_DESKTOP='1')
    else:
        env.update(FORMAVIEW_DESKTOP_SCOPE=str(run / 'scope.json'))
    params = StdioServerParameters(command=str(args.runtime / 'mcp/formaview-mcp.exe'), env=env)
    report = {'mode': args.mode, 'exe_sha256': hashlib.file_digest((args.runtime / 'formaview.exe').open('rb'), 'sha256').hexdigest(),
              'protocol': None, 'tools': [], 'calls': []}
    try:
        with (run / 'adapter.log').open('w', encoding='utf8') as log:
            async with stdio_client(params, errlog=log) as streams, ClientSession(*streams) as client:
                report['protocol'] = (await client.initialize()).protocolVersion
                report['tools'] = sorted(t.name for t in (await client.list_tools()).tools)
                expected = {'open_asset', 'get_playback_state', 'control_video', 'step_video_frame',
                            'capture_video_frame', 'read_texture_pixel', 'plan_image_batch', 'plan_fbx_batch'}
                assert expected <= set(report['tools']), expected.difference(report['tools'])

                async def call(name, error=None, **kwargs):
                    start = time.perf_counter()
                    reply = await client.call_tool(name, kwargs)
                    result = reply.structuredContent or json.loads(reply.content[0].text)
                    report['calls'].append(dict(tool=name, arguments=kwargs, result=result,
                                                ms=round(1000 * (time.perf_counter()-start), 2)))
                    assert not reply.isError and result.get('error', {}).get('code') == error, (name, result)
                    return result

                caps = await call('get_image_capabilities')
                assert 'png' in caps['write_targets']
                clock = await call('open_asset', path=str(args.clock))
                asset = clock['asset_id']
                pid = clock['host_pid']
                host = win32api.OpenProcess(win32con.PROCESS_QUERY_INFORMATION | win32con.PROCESS_VM_READ, False, pid)
                try:
                    exe = Path(win32process.GetModuleFileNameEx(host, 0))
                finally:
                    host.Close()
                assert exe.samefile(args.runtime / 'formaview.exe')
                report.update(executable=str(exe), visible_windows=window_count(pid), host_pid=pid)
                assert (report['visible_windows'] == 0) == (args.mode == 'background')

                async def control(action, value=None):
                    return await call('control_video', asset_id=asset, action=action, value=value)

                await control('pause')
                if args.game_texture:
                    sampled = await call('read_texture_pixel', path=str(args.game_texture), x=480, y=23)
                    raw = {c['name']: c['raw'] for c in sampled['channels']}
                    reference = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(args.game_texture),
                        '-vf', 'crop=1:1:480:23', '-frames:v', '1', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'])
                    # This TGA declares zero attribute bits; its fourth byte is padding.
                    assert list(raw) == list('RGB') and list(reference) == [raw[n] for n in 'RGB'], (raw, reference)
                    assert (await call('get_playback_state', asset_id=asset))['asset_id'] == asset
                assert (await control('volume', 32))['volume'] == 32
                assert abs((await control('speed', 1.1))['speed'] - 1.1) < 1e-3
                await control('speed', 1)
                assert abs((await control('seek', 10))['position'] - 10) < .01
                for index, (direction, target) in enumerate([('next', 10.5), ('previous', 10)]):
                    s = await call('step_video_frame', asset_id=asset, direction=direction)
                    assert abs(s['position'] - target) < .01, s
                    png = run / 'frames' / f'clock-{index}.png'
                    shot = await call('capture_video_frame', asset_id=asset, output=str(png), subtitles=False)
                    assert png.stat().st_size == shot['bytes']
                    data = gray(png)
                    diffs = {str(t): sum(abs(a-b) for a, b in zip(data, gray(args.clock, t), strict=True)) / len(data)
                             for t in [target-.5, target, target+.5]}
                    report.setdefault('clock_image_oracle', []).append(diffs)
                    assert min(diffs, key=diffs.get) == str(target), diffs
                await call('capture_video_frame', error='conflict', asset_id=asset, output=str(png), subtitles=True)
                await call('capture_video_frame', error='out_of_scope', asset_id=asset,
                           output=str(run / 'outside.png'), subtitles=False)
                assert not (run / 'outside.png').exists()
                assert not (await control('play'))['paused']
                await asyncio.sleep(.65)
                assert (await control('pause'))['position'] > 10

                texture = await call('open_asset', path=str(args.texture))
                assert texture['kind'] == 'image'
                await call('get_playback_state', error='input_changed', asset_id=asset)
                pixel = await call('read_texture_pixel', path=str(args.texture), x=4, y=2)
                # Independent RGB565 unpack of an uncompressed legacy DDS test fixture.
                src = args.texture.read_bytes()
                assert src[:4] == b'DDS ' and struct.unpack_from('<I', src, 88)[0] == 16
                width = struct.unpack_from('<I', src, 16)[0]
                word = struct.unpack_from('<H', src, 128 + 2 * (2 * width + 4))[0]
                expected = [(word >> 11)/31, ((word >> 5) & 63)/63, (word & 31)/31]
                assert all(abs(c['interpreted']-v) < 1e-6 for c,v in zip(pixel['channels'], expected, strict=True))
                await call('read_texture_pixel', error='invalid_argument', path=str(args.texture), x=width, y=0)
                await call('open_asset', error='out_of_scope', path=str(args.runtime / 'README-run.txt'))

                if args.real:
                    scene = await call('open_asset', path=str(args.real))
                    asset = scene['asset_id']
                    await control('pause')
                    pts = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-read_intervals', '58%+16',
                        '-select_streams', 'v:0', '-show_packets', '-show_entries', 'packet=pts_time', '-of', 'json', str(args.real)]))
                    times = sorted(float(p['pts_time']) for p in pts['packets'])
                    anchor = min(range(len(times)), key=lambda i: abs(times[i]-60))
                    assert anchor >= 24 and abs(times[anchor]-60) < .0001
                    for steps in [10, 24]:
                        assert abs((await control('seek', 60))['position'] - 60) < .0001
                        for n in range(1, steps+1):
                            s = await call('step_video_frame', asset_id=asset, direction='previous')
                            assert abs(s['position']-times[anchor-n]) < .0001, (n, s, times[anchor-n])
                        s = await call('step_video_frame', asset_id=asset, direction='next')
                        assert abs(s['position']-times[anchor-steps+1]) < .0001
                        png = run / 'frames' / f'real-{steps}.png'
                        await call('capture_video_frame', asset_id=asset, output=str(png), subtitles=False)
                        assert struct.unpack_from('>II', png.read_bytes(), 16) == (3840, 1632)
                        actual = gray(png)
                        reference = gray(args.real, s['position'] - .00001)
                        mae = sum(abs(a-b) for a,b in zip(actual, reference, strict=True)) / len(actual)
                        report.setdefault('real_image_oracle', []).append(dict(steps=steps, position=s['position'], mae=mae))
                        assert mae < 5, (steps, mae)
                        assert not (await control('play'))['paused']
                        await asyncio.sleep(.2)
                        assert (await control('pause'))['position'] > s['position']
                    if args.mode == 'background' and scene['indexed'] and scene['duration'] > 1852:
                        # A long-GOP seek can publish time-pos while its old picture
                        # is still in the VO. Capture immediately after the MCP reply.
                        target = 1851.25
                        async def capture(label):
                            png = run / 'frames' / f'indexed-{label}.png'
                            await call('capture_video_frame', asset_id=asset, output=str(png), subtitles=False)
                            return hashlib.sha256(png.read_bytes()).hexdigest()
                        await control('seek', value=target)
                        expected = await capture('exact')
                        await control('seek', value=target + 1/60)
                        adjacent = await capture('adjacent')
                        state = await call('step_video_frame', asset_id=asset, direction='previous')
                        observed = await capture('step')
                        assert abs(state['position'] - target) < .0001
                        assert expected != adjacent and observed == expected, (expected, adjacent, observed)
                report['passed'] = True
    finally:
        (run / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')


async def main():
    p = argparse.ArgumentParser()
    for name in ['runtime', 'output', 'clock', 'texture', 'real']:
        p.add_argument('--'+name, type=lambda x: Path(x).resolve(), required=name != 'real')
    p.add_argument('--mode', choices=['background', 'window'], default='background')
    p.add_argument('--game-texture', type=lambda x: Path(x).resolve())
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    frames = args.output / 'frames'
    frames.mkdir()
    roots = [p.parent for p in [args.clock, args.texture, args.real, args.game_texture] if p]
    policy = dict(input_roots=[str(p) for p in roots], output_roots=[str(frames)], allow_overwrite=False)
    scope = args.output / 'scope.json'
    scope.write_text(json.dumps(policy, ensure_ascii=False), encoding='utf8')
    env = {k:v for k,v in os.environ.items() if not k.startswith(('FORMAVIEW_', 'QT_', 'QML'))}
    sysroot = os.environ['SystemRoot']
    env.update(PATH=os.pathsep.join([str(Path(sysroot)/'System32'), sysroot]),
               FORMAVIEW_IMAGE_STATE_ROOT=str(args.output / 'journal'), PYTHONUTF8='1')
    if args.mode == 'background':
        await exercise(args, env)
    else:
        with (args.output / 'host.log').open('wb') as log:
            proc = await asyncio.create_subprocess_exec(str(args.runtime/'formaview.exe'), '--mcp-scope', str(scope),
                '--trace', str(args.output/'trace.json'), env=env, stdout=asyncio.subprocess.PIPE, stderr=log)
            try:
                while line := await asyncio.wait_for(proc.stdout.readline(), 30):
                    if line.startswith(b'{'):
                        await exercise(args, env, json.loads(line)['endpoint'])
                        break
                else:
                    raise AssertionError('Window endpoint did not start')
            finally:
                if proc.returncode is None:
                    proc.terminate()
                await proc.wait()


if __name__ == '__main__':
    asyncio.run(main())
