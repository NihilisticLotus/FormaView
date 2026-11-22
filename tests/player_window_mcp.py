"""Packaged MCP dual-monitor regression. Win32 independently observes Qt and supplies a solid backdrop."""
import asyncio
import ctypes
import hashlib
import json
import os
from pathlib import Path
import sys

import win32api
import win32con as wc
import win32gui as gui
import win32process
from PIL import ImageGrab
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


def host_window(pid):
    hits = []
    def visit(hwnd, _):
        if gui.IsWindowVisible(hwnd) and win32process.GetWindowThreadProcessId(hwnd)[1] == pid:
            hits.append(hwnd)
    gui.EnumWindows(visit, None)
    return max(hits, key=lambda h: gui.GetClientRect(h)[2] * gui.GetClientRect(h)[3])


def native(pid):
    h = host_window(pid)
    area = gui.GetClientRect(h)
    place = gui.GetWindowPlacement(h)
    screen = win32api.GetMonitorInfo(win32api.MonitorFromWindow(h))
    return dict(hwnd=h, outer=gui.GetWindowRect(h),
                client=gui.ClientToScreen(h,(0,0)) + gui.ClientToScreen(h,area[2:]),
                normal_rect=place[4], show=place[1], screen=screen['Device'], bounds=screen['Monitor'],
                topmost=bool(gui.GetWindowLong(h,wc.GWL_EXSTYLE) & wc.WS_EX_TOPMOST))


async def run(runtime, clip, out):
    ctypes.windll.user32.SetThreadDpiAwarenessContext(ctypes.c_void_p(-4))
    out.mkdir(exist_ok=False)
    scope = out / 'scope.json'
    scope.write_text(json.dumps(dict(input_roots=[str(clip.parent)],output_roots=[str(out)],allow_overwrite=False)), 'utf8')
    env = {k:v for k,v in os.environ.items() if not k.startswith('FORMAVIEW_')}
    env.update(FORMAVIEW_DESKTOP_SCOPE=str(scope),FORMAVIEW_IMAGE_STATE_ROOT=str(out/'background-journal'))
    params = StdioServerParameters(command=sys.executable,args=[str(runtime/'mcp/image_stdio.py')],env=env)
    with (runtime/'formaview.exe').open('rb') as exe:
        report = dict(exeSha256=hashlib.file_digest(exe,'sha256').hexdigest(),checks=[],calls=[])
    def save():
        (out/'results.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),'utf8')
    klass = gui.WNDCLASS()
    klass.hInstance = win32api.GetModuleHandle(None)
    klass.lpszClassName = 'FVWindowTestBackdrop'
    klass.lpfnWndProc = gui.DefWindowProc
    brush = gui.CreateSolidBrush(win32api.RGB(255,0,255))
    klass.hbrBackground = brush
    gui.RegisterClass(klass)
    covers = {}
    try:
        async with stdio_client(params) as io, ClientSession(*io) as c:
            await c.initialize()
            report['tools'] = sorted(t.name for t in (await c.list_tools()).tools)
            async def call(tool,error=None,**args):
                reply = await c.call_tool(tool,args)
                val = reply.structuredContent or json.loads(reply.content[0].text)
                report['calls'].append(dict(tool=tool,args=args,result=val))
                assert not reply.isError and val.get('error',{}).get('code') == error, (tool,args,val)
                return val
            empty = await call('get_window_state')
            assert not empty['has_window']
            bg = await call('open_asset',path=str(clip))
            await call('control_player_window',asset_id=bg['asset_id'],action='mini',error='invalid_argument')
            opened = await call('open_asset',path=str(clip),window=True)
            pid, asset = opened['host_pid'], opened['asset_id']
            await call('control_video',asset_id=asset,action='pause')
            async def state():
                return await call('get_window_state',asset_id=asset)
            async def change(action,**kwargs):
                return await call('control_player_window',asset_id=asset,action=action,**kwargs)
            initial = await state()
            screens = [s['id'] for s in initial['screens']]
            assert len(screens)==2 and pid!=bg['host_pid']
            assert not (await call('get_window_state',asset_id=bg['asset_id']))['has_window']
            await change('move', screen_id='absent-monitor', error='invalid_argument')
            await change('resize',width=800,height=520,error='invalid_argument')
            for i, screen in enumerate(screens):
                await change('move', screen_id=screen)
                await asyncio.sleep(.2)
                start = native(pid)
                assert start['screen']==screen, (start,screen,initial)
                x,y,r,b = start['bounds']
                cover = gui.CreateWindowEx(wc.WS_EX_TOOLWINDOW|wc.WS_EX_NOACTIVATE,klass.lpszClassName,'FormaView test',
                    wc.WS_POPUP,x,y,r-x,b-y,0,0,klass.hInstance,None)
                covers[screen] = cover
                gui.ShowWindow(cover,wc.SW_SHOWNOACTIVATE)
                gui.RedrawWindow(cover,None,None,wc.RDW_INVALIDATE|wc.RDW_ERASE|wc.RDW_UPDATENOW)
                # Place the owned app directly above this owned, opaque test window.
                gui.SetWindowPos(start['hwnd'],wc.HWND_TOP,0,0,0,0,wc.SWP_NOMOVE|wc.SWP_NOSIZE)
                gui.SetWindowPos(cover,start['hwnd'],0,0,0,0,wc.SWP_NOMOVE|wc.SWP_NOSIZE|wc.SWP_NOACTIVATE)
                await asyncio.sleep(.2)
                before = ImageGrab.grab(bbox=(r-240,y+2,r-1,y+56),all_screens=True)
                assert before.getpixel((10,8))[:3]==(255,0,255), 'Backdrop not visible before fullscreen'
                for maximized in [False, True]:
                    if maximized:
                        gui.ShowWindow(host_window(pid),wc.SW_MAXIMIZE)
                        await asyncio.sleep(.2)
                    for mode in ['fullscreen','mini']:
                        old = native(pid)
                        entered = await change(mode)
                        await asyncio.sleep(.25)
                        active = native(pid)
                        record = dict(screen=screen,mode=mode,maximized=maximized,before=old,active=active)
                        report['checks'].append(record)
                        if mode=='fullscreen':
                            assert active['client']==active['bounds'] and active['screen']==screen and entered['fullscreen']
                            img = ImageGrab.grab(bbox=(x,y,r,b),all_screens=True).convert('RGB')
                            regions = [(0,0,r-x,12),(0,b-y-12,r-x,b-y),(0,0,12,b-y),
                                       (r-x-12,0,r-x,b-y),(0,0,300,80),(r-x-300,0,r-x,80)]
                            def count(area):
                                return sum(n for n,color in img.crop(area).getcolors((r-x)*(b-y)) if color==(255,0,255))
                            record['magenta_pixels'] = [count(area) for area in regions]
                            img.crop(regions[-1]).save(out/f'screen-{i}-max-{maximized}-upper-right.png')
                            assert not any(record['magenta_pixels'])
                            for point in [(x+1,y+1),(r-2,y+1),(x+1,b-2),(r-2,b-2)]:
                                assert gui.GetAncestor(gui.WindowFromPoint(point),2)==active['hwnd'],point
                        else:
                            assert active['topmost'] and entered['topmost']
                            if active['screen'] != screen:
                                # The GUI remembers the last mini window monitor separately.
                                await change('move',screen_id=screen)
                            await change('resize',width=100,height=80,error='invalid_argument')
                            for width,height in [(640,360),(800,520)]:
                                scaled = await change('resize',width=width,height=height)
                                scale = next(s['scale'] for s in initial['screens'] if s['id']==scaled['screen_id'])
                                a,b1,c1,d = native(pid)['client']
                                assert abs(c1-a-width*scale)<=1 and abs(d-b1-height*scale)<=1
                            record['resized']=native(pid)
                        # Exercise idempotence and cross-screen moves while in each mode.
                        assert (await change(mode))['mode']==mode
                        other = screens[1-screens.index((await state())['screen_id'])]
                        moved = await change('move',screen_id=other)
                        await asyncio.sleep(.2)
                        actual = native(pid)
                        assert actual['screen']==other and moved['mode']==mode
                        assert actual['client']==actual['bounds'] if mode=='fullscreen' else actual['topmost']
                        record['moved']=actual
                        restored = await change('normal')
                        await asyncio.sleep(.2)
                        now = native(pid)
                        record['restored']=now
                        save()
                        assert all(now[k]==old[k] for k in ('outer','client','normal_rect','show','screen','topmost')), (old,now)
                        assert restored['maximized']==maximized and not restored['topmost']
                        px = restored['native_outer_px']
                        assert (px['x'],px['y'],px['x']+px['width'],px['y']+px['height'])==old['outer']
                    if maximized:
                        gui.ShowWindow(host_window(pid),wc.SW_RESTORE)
                        await asyncio.sleep(.2)
                gui.DestroyWindow(covers.pop(screen))
            assert (await call('get_playback_state',asset_id=bg['asset_id']))['paused']
        report['passed']=True
    finally:
        for cover in covers.values():
            gui.DestroyWindow(cover)
        gui.UnregisterClass(klass.lpszClassName,klass.hInstance)
        save()


if __name__=='__main__':
    asyncio.run(run(*(Path(p).resolve() for p in sys.argv[1:])))
