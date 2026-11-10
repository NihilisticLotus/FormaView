"""Verify the native wizard on a private Windows desktop and exercise disposable installs."""
import ctypes as ct
from ctypes import wintypes as wt
from pathlib import Path
import re
import subprocess
import time
from uuid import uuid4
import winreg

from PIL import Image, ImageDraw, ImageFont
import build
from build import read, save, sha, write

u, g, k = ct.windll.user32, ct.windll.gdi32, ct.windll.kernel32
H = ct.c_void_p
CALLBACK = ct.WINFUNCTYPE(wt.BOOL, H, wt.LPARAM)


def api(lib, name, args, result):
    method = getattr(lib, name)
    method.argtypes, method.restype = args, result
    return method


create_desktop = api(u, "CreateDesktopW", [wt.LPCWSTR, H, H, wt.DWORD, wt.DWORD, H], H)
close_desktop = api(u, "CloseDesktop", [H], wt.BOOL)
enum_desktop = api(u, "EnumDesktopWindows", [H, CALLBACK, wt.LPARAM], wt.BOOL)
enum_children = api(u, "EnumChildWindows", [H, CALLBACK, wt.LPARAM], wt.BOOL)
is_visible = api(u, "IsWindowVisible", [H], wt.BOOL)
is_enabled = api(u, "IsWindowEnabled", [H], wt.BOOL)
send = api(u, "SendMessageTimeoutW", [H, wt.UINT, wt.WPARAM, wt.LPARAM, wt.UINT, wt.UINT,
                                     ct.POINTER(ct.c_size_t)], H)
get_class = api(u, "GetClassNameW", [H, wt.LPWSTR, ct.c_int], ct.c_int)
get_title = api(u, "GetWindowTextW", [H, wt.LPWSTR, ct.c_int], ct.c_int)
rect = api(u, "GetWindowRect", [H, ct.POINTER(wt.RECT)], wt.BOOL)
print_window = api(u, "PrintWindow", [H, H, wt.UINT], wt.BOOL)
create_dc = api(g, "CreateCompatibleDC", [H], H)
create_dib = api(g, "CreateDIBSection", [H, H, wt.UINT, ct.POINTER(H), H, wt.DWORD], H)
select = api(g, "SelectObject", [H, H], H)
delete_object = api(g, "DeleteObject", [H], wt.BOOL)
delete_dc = api(g, "DeleteDC", [H], wt.BOOL)
api(u, "SetProcessDpiAwarenessContext", [H], wt.BOOL)(-4)

class StartupInfo(ct.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR),
                ("dwX", wt.DWORD), ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD),
                ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD), ("dwFillAttribute", wt.DWORD),
                ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", H), ("hStdInput", H), ("hStdOutput", H), ("hStdError", H)]


class ProcessInfo(ct.Structure):
    _fields_ = [("hProcess", H), ("hThread", H), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


create_process = api(k, "CreateProcessW", [wt.LPCWSTR, wt.LPWSTR, H, H, wt.BOOL, wt.DWORD, H, wt.LPCWSTR,
                                         ct.POINTER(StartupInfo), ct.POINTER(ProcessInfo)], wt.BOOL)
wait_process = api(k, "WaitForSingleObject", [H, wt.DWORD], wt.DWORD)
exit_code = api(k, "GetExitCodeProcess", [H, ct.POINTER(wt.DWORD)], wt.BOOL)
terminate_process = api(k, "TerminateProcess", [H, wt.UINT], wt.BOOL)
close_handle = api(k, "CloseHandle", [H], wt.BOOL)


class Process:
    def __init__(self, handle):
        self.handle = handle

    def poll(self):
        if wait_process(self.handle, 0) != 0:
            return None
        result = wt.DWORD()
        assert exit_code(self.handle, ct.byref(result))
        return result.value

    def wait(self, timeout):
        if wait_process(self.handle, int(timeout * 1000)) != 0:
            raise TimeoutError("Setup did not exit")
        return self.poll()

    def terminate(self):
        terminate_process(self.handle, 1)


def enum_windows(fn, owner):
    items = []

    @CALLBACK
    def collect(hwnd, _):
        if is_visible(hwnd):
            items.append(hwnd)
        return 1

    fn(owner, collect, 0)
    return items


def wtext(hwnd, child=True):
    value = ct.create_unicode_buffer(4096)
    if child:
        send(hwnd, 0x000D, len(value), ct.addressof(value), 0x2, 500, ct.byref(ct.c_size_t()))
    else:
        get_title(hwnd, value, len(value))
    return value.value


def wclass(hwnd):
    value = ct.create_unicode_buffer(128)
    get_class(hwnd, value, len(value))
    return value.value


def capture(hwnd, target):
    box = wt.RECT()
    assert rect(hwnd, ct.byref(box))
    w, h = box.right - box.left, box.bottom - box.top
    assert 50 < w < 3000 and 50 < h < 2000, (w, h)

    class BitmapInfo(ct.Structure):
        _fields_ = [("size", wt.DWORD), ("width", wt.LONG), ("height", wt.LONG),
                    ("planes", wt.WORD), ("bits", wt.WORD), ("compression", wt.DWORD),
                    ("image_size", wt.DWORD), ("x", wt.LONG), ("y", wt.LONG),
                    ("used", wt.DWORD), ("important", wt.DWORD)]

    dc, data = create_dc(None), H()
    info = BitmapInfo(40, w, -h, 1, 32)
    bmp = create_dib(dc, ct.byref(info), 0, ct.byref(data), None, 0)
    assert bmp and data.value
    old = select(dc, bmp)
    try:
        for attempt in range(4):
            assert print_window(hwnd, dc, 2 if attempt % 2 == 0 else 0)
            im = Image.frombytes("RGB", (w, h), ct.string_at(data, w * h * 4), "raw", "BGRX")
            if any(max(im.getpixel((w * n // 8, h // 2))) > 96 for n in range(2, 7)):
                break
            time.sleep(.08)
        else:
            raise AssertionError(f"Window client area is blank: {target}")
        assert max(hi - lo for lo, hi in im.getextrema()) > 100, target
        im.save(target)
    finally:
        select(dc, old)
        delete_object(bmp)
        delete_dc(dc)


class Desktop:
    def __init__(self):
        self.name = "FormaViewQA-" + uuid4().hex
        self.handle = create_desktop(self.name, None, None, 0, 0x01FF, None)
        assert self.handle, ct.WinError()

    def launch(self, exe, *args):
        info, process = StartupInfo(), ProcessInfo()
        info.cb = ct.sizeof(info)
        info.lpDesktop = "WinSta0\\" + self.name
        cmd = ct.create_unicode_buffer(subprocess.list2cmdline([str(exe), *map(str, args)]))
        if not create_process(str(exe), cmd, None, None, False, 0, None, str(exe.parent),
                              ct.byref(info), ct.byref(process)):
            raise ct.WinError()
        close_handle(process.hThread)
        return Process(process.hProcess)

    def wait_window(self, cls, title=None, timeout=18):
        end = time.monotonic() + timeout
        seen = []
        while time.monotonic() < end:
            seen = [(hwnd, wclass(hwnd), wtext(hwnd, False))
                    for hwnd in enum_windows(enum_desktop, self.handle)]
            for hwnd, kind, name in seen:
                if (cls is None or kind == cls) and (title is None or title in name):
                    return hwnd
            time.sleep(.12)
        raise TimeoutError((cls, title, seen))

    def close(self):
        pid_for = api(u, "GetWindowThreadProcessId", [H, ct.POINTER(wt.DWORD)], wt.DWORD)
        open_process = api(k, "OpenProcess", [wt.DWORD, wt.BOOL, wt.DWORD], H)
        for hwnd in enum_windows(enum_desktop, self.handle):
            if wclass(hwnd) not in ("#32770", "TSetupForm", "TWizardForm", "TUninstallProgressForm"):
                continue
            pid = wt.DWORD()
            pid_for(hwnd, ct.byref(pid))
            process = open_process(1, False, pid.value)
            if process:
                terminate_process(process, 1)
                close_handle(process)
        close_desktop(self.handle)


def children(window):
    return [(hwnd, wclass(hwnd), wtext(hwnd)) for hwnd in enum_windows(enum_children, window)]


def click(window, text=None, command_id=None, delay=.55, control="button"):
    get_id = api(u, "GetDlgCtrlID", [H], ct.c_int)
    matches = [(hwnd, kind, value) for hwnd, kind, value in children(window)
               if ((command_id is not None and get_id(hwnd) == command_id) or
                   (text is not None and control in kind.lower() and text in value)) and is_enabled(hwnd)]
    assert len(matches) == 1, (text, command_id, matches, children(window))
    hwnd = matches[0][0]
    # Post to this test window only. BM_CLICK needs focus on some dialog controls;
    # a notification to the real parent works on an inactive private desktop.
    get_parent = api(u, "GetParent", [H], H)
    post = api(u, "PostMessageW", [H, wt.UINT, wt.WPARAM, wt.LPARAM], wt.BOOL)
    assert post(get_parent(hwnd), 0x0111, get_id(hwnd) & 0xFFFF, hwnd)
    time.sleep(delay)


def wait_text(window, expected):
    end = time.monotonic() + 6
    while time.monotonic() < end:
        if any(expected in value for _, _, value in children(window)):
            return
        time.sleep(.12)
    raise AssertionError((expected, children(window)))

def wait_dialog(desktop, title, content, timeout=12):
    end = time.monotonic() + timeout
    seen = []
    while time.monotonic() < end:
        for hwnd in enum_windows(enum_desktop, desktop.handle):
            if wclass(hwnd) != "#32770" or title not in wtext(hwnd, False):
                continue
            seen = [value for _, _, value in children(hwnd)]
            if any(content in value for value in seen):
                return hwnd
        time.sleep(.08)
    raise TimeoutError((title, content, seen))


def record(window, folder, name, expected=None):
    if expected:
        wait_text(window, expected)
    if wclass(window) == "#32770":
        time.sleep(.18)
    target = folder / (name + ".png")
    capture(window, target)
    print(name, flush=True)
    return target


def registry(root, key, name):
    try:
        with winreg.OpenKey(root, key, 0, winreg.KEY_READ | winreg.KEY_WOW64_64KEY) as handle:
            return winreg.QueryValueEx(handle, name)[0]
    except FileNotFoundError:
        return None


def make_fixture(fixture, app_id, profile, namespace):
    engine, meta = build.DEST / "installer", build.DEST / "payload-metadata"
    source = read(engine / "FormaView.iss").replace(build.IDENTITY, app_id)
    source = source.replace(r"Software\FormaView\Installer", profile)
    # Keep the real scopes and UI; confine installed files and all app-specific
    # registry state to unique test-only locations. No shortcuts or app launch.
    app = fixture / "installed"
    source = source.replace(r"{commonpf64}\FormaView", str(app))
    source = re.sub(r"(?ms)^\[Icons\].*?(?=^\[Registry\])", "", source)
    source = source.replace("OutputBaseFilename=FormaView-{#PackageVersion}-Windows-x64-Setup", "OutputBaseFilename=wizard-fixture")
    assoc = read(engine / "associations.iss").replace(r"Software\FormaView", "Software\\" + namespace)
    assoc = assoc.replace("FormaView.Local", namespace)
    data = read(engine / "uninstall-data.iss")
    data = data.replace(r"Software\FormaView\Installer", profile)
    data = data.replace(r"Software\FormaView\FormaView", "Software\\" + namespace + r"\Settings")
    data = data.replace(r"{localappdata}\FormaView\FormaView", str(fixture / "account-state"))
    for name in ("wizard-mark.bmp", "wizard-panel.bmp", "install-notice.txt"):
        (fixture / name).write_bytes((engine / name).read_bytes())
    (fixture / "upgrade-policy.iss").write_bytes((engine / "upgrade-policy.iss").read_bytes())
    (fixture / "formaview.ico").write_bytes((meta / "formaview.ico").read_bytes())
    (fixture / "installation.ini").write_bytes(f"[Install]\r\nAppId={app_id}\r\nVersion=0.1.0\r\n".encode())
    (fixture / "owned-files.txt").write_bytes(b"\xef\xbb\xbfinstallation.ini\r\nowned-files.txt\r\n")
    write(fixture / "associations.iss", assoc)
    write(fixture / "uninstall-data.iss", data)
    write(fixture / "files.generated.iss", "\n".join(f'Source: "{fixture/n}"; DestDir: "{{app}}"; Flags: ignoreversion'
                                                   for n in ("installation.ini", "owned-files.txt")))
    current = fixture / "current.iss"
    write(current, source)
    old = re.sub(r"(?ms)^\[Registry\].*?(?=^\[Code\])", "", source)
    old = old.replace("PrivilegesRequired=admin", "PrivilegesRequired=lowest")
    old = old.replace("PrivilegesRequiredOverridesAllowed=dialog\n", "")
    old = old.replace("OutputBaseFilename=wizard-fixture", "OutputBaseFilename=wizard-legacy")
    legacy = fixture / "legacy.iss"
    write(legacy, old)
    for script in (current, legacy):
        log = subprocess.run([str(build.ISCC), "/Q", f"/DPayload={fixture}", f"/DOutputPath={fixture}",
                              "/DPayloadMiB=109", "/DCandidateId=fixture", str(script)], capture_output=True)
        (fixture / f"{script.stem}-compile.log").write_bytes(log.stdout + log.stderr)
        assert log.returncode == 0, log.stdout + log.stderr
    return fixture / "wizard-fixture.exe", fixture / "wizard-legacy.exe", app


def silent(exe, log, *args):
    run = subprocess.run([str(exe), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={log}", *args],
                         timeout=25, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return run.returncode


def make_sheet(paths, out):
    font = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 23)
    title = ImageFont.truetype("C:/Windows/Fonts/msyhbd.ttc", 34)
    sheet = Image.new("RGB", (1440, 98 + ((len(paths) + 1) // 2) * 530), "#EDEDF0")
    draw = ImageDraw.Draw(sheet)
    draw.text((44, 30), "FormaView   /   安装与卸载", font=title, fill="#242429")
    for i, (label, path) in enumerate(paths):
        x, y = 44 + (i % 2) * 694, 98 + (i // 2) * 530
        draw.text((x, y), label, font=font, fill="#35353D")
        im = Image.open(path).convert("RGB")
        w, h = im.size
        trim_left = next((i for i in range(20) if im.getpixel((i, h // 2)) != (0, 0, 0)), 0)
        trim_right = next((i for i in range(20) if im.getpixel((w - 1 - i, h // 2)) != (0, 0, 0)), 0)
        trim_bottom = next((i for i in range(20) if im.getpixel((w // 2, h - 1 - i)) != (0, 0, 0)), 0)
        im = im.crop((trim_left, 0, w - trim_right, h - trim_bottom))
        im.thumbnail((656, 466), Image.Resampling.LANCZOS)
        sheet.paste(im, (x, y + 40))
    sheet.save(out)


def main():
    build.configure_staging_root()
    token = uuid4().hex[:12]
    fixture = build.EVIDENCE / ("fixture-" + token)
    screenshots = build.EVIDENCE / "screenshots"
    fixture.mkdir()
    screenshots.mkdir(exist_ok=True)
    app_id = "{" + str(uuid4()).upper() + "}"
    namespace = "FormaViewWizardQA-" + token
    profile = "Software\\" + namespace + r"\Profile"
    settings = "Software\\" + namespace + r"\Settings"
    key = r"Software\Microsoft\Windows\CurrentVersion\Uninstall" + "\\" + app_id + "_is1"
    cu, lm = winreg.HKEY_CURRENT_USER, winreg.HKEY_LOCAL_MACHINE
    new, legacy, app = make_fixture(fixture, app_id, profile, namespace)
    desk = Desktop()
    images = []
    release = build.DEST / "output/FormaView-0.1.0-Windows-x64-Setup.exe"
    prod_profile = r"Software\FormaView\Installer"
    profile_before = registry(cu, prod_profile, "TaskHistoryLayout")
    prod_settings = r"Software\FormaView\FormaView"
    settings_before = {name: registry(cu, prod_settings, name) for name in ("language", "preferences", "recent")}
    processes = []
    try:
        # Real delivery: read-only navigation through the machine install wizard.
        prod = desk.launch(release, f"/LOG={fixture/'release-preview.log'}")
        processes.append(prod)
        scope = desk.wait_window("#32770", "安装范围")
        images.append(("安装范围", record(scope, screenshots, "01-scope")))
        click(scope, "为本机所有用户安装")
        wizard = desk.wait_window("TWizardForm", "FormaView")
        images.append(("欢迎", record(wizard, screenshots, "02-welcome")))
        for name, label, expected in [
            ("03-notice", "安装说明", None),
            ("04-location", "安装位置", r"C:\Program Files\FormaView"),
            ("05-options", "安装选项", None),
            ("06-association", "文件关联", "不关联"),
        ]:
            click(wizard, "下一步")
            images.append((label, record(wizard, screenshots, name, expected)))
        click(wizard, "常用格式")
        click(wizard, "下一步")
        images.append(("准备安装", record(wizard, screenshots, "07-ready", "文件关联（13 个格式）")))
        click(wizard, "取消")
        exit_dialog = desk.wait_window("#32770", "退出")
        click(exit_dialog, command_id=6)
        assert prod.wait(timeout=12) in (0, 2)
        assert not app.exists()

        # Real Inno lifecycle in the disposable machine scope.
        test = desk.launch(new, "/ALLUSERS", f"/DIR={app}", "/ASSOC=.fbx", "/TASKS=", f"/LOG={fixture/'machine.log'}")
        processes.append(test)
        wizard = desk.wait_window("TWizardForm", "FormaView")
        for _ in range(5):
            click(wizard, "下一步")
        click(wizard, "安装")
        images.append(("确认默认应用", record(wizard, screenshots, "08-default-apps", "打开默认应用设置")))
        click(wizard, "下一步")
        images.append(("安装完成", record(wizard, screenshots, "09-finished")))
        click(wizard, "完成")
        assert test.wait(timeout=10) == 0
        assert Path(registry(lm, key, "InstallLocation")).resolve() == app.resolve()
        assert registry(cu, key, "InstallLocation") is None and registry(cu, profile, "TaskHistoryLayout") is None
        assert registry(lm, r"Software\Classes\\" + namespace + r".fbx\shell\open\command", "") == f'"{app}\\formaview.exe" --open "%1"'
        originals = app / "original-source.txt"
        exports = app / "export-output.txt"
        for p in (originals, exports):
            p.write_bytes(b"keep original bytes")
        state = fixture / "account-state"
        installed = state / "installed/image-journal"
        legacy_state = state / "image-journal"
        dev_state = state / "development/image-journal"
        policy = "a" * 64
        guid = "a628b692-4825-480b-8390-37506f69cfab"
        active = [
            "desktop-history.json",
            f"{policy}/{guid}.json",
            f"{policy}/{guid}.json.ABC123",
            f"{policy}-groups/{guid}/index.json",
            f"{policy}-groups/{guid}/protected-0.json",
            f"{policy}-groups/{guid}/source-guard-1.json",
            f"{policy}-groups/{guid}/0/requests.json",
            f"{policy}-groups/{guid}/0/direct-result.json",
            f"{policy}-groups/{guid}/0/records/{guid}.json",
        ]
        bystanders = [
            installed / "assets/original.fbx",
            installed / f"{policy}-groups/{guid}/0/export.glb",
            installed / f"{policy}/different.json",
            dev_state / "desktop-history.json",
            dev_state / f"{policy}/{guid}.json",
            legacy_state / "desktop-history.json",
            originals, exports,
        ]

        def seed():
            with winreg.CreateKeyEx(cu, settings, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as handle:
                for name in ("language", "preferences", "recent", "unknown"):
                    winreg.SetValueEx(handle, name, 0, winreg.REG_SZ, "test-" + name)
            for target in [installed / name for name in active] + bystanders:
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(b"keep original bytes")

        def assert_state(delete_settings=False, delete_history=False, delete_legacy=False):
            for name in ("language", "preferences", "recent", "unknown"):
                assert registry(cu, settings, name) == (None if delete_settings and name != "unknown" else "test-" + name), name
            for name in active:
                target = installed / name
                assert not target.exists() if delete_history else target.read_bytes() == b"keep original bytes", target
            for target in bystanders:
                assert not target.exists() if delete_legacy and target.parent == legacy_state else target.read_bytes() == b"keep original bytes", target

        def ui_uninstall(folder, name, remove_settings=False, remove_history=False, mode="confirm", show=False,
                         expected_partial=False):
            uninstaller = folder / "unins000.exe"
            process = desk.launch(uninstaller, f"/LOG={fixture/(name+'.log')}")
            processes.append(process)
            options = desk.wait_window(None, "用户数据")
            wait_text(options, "保留任务历史")
            controls = {value: hwnd for hwnd, kind, value in children(options) if "checkbox" in kind.lower()}
            for label, remove in (("保留用户设置", remove_settings), ("保留任务历史", remove_history)):
                hwnd = controls[label]
                assert is_enabled(hwnd), children(options)
                checked = ct.c_size_t()
                send(hwnd, 0xF0, 0, 0, 0x2, 500, ct.byref(checked))  # BM_GETCHECK
                assert checked.value == 1, label
                if remove:
                    click(options, label, control="checkbox", delay=.1)
                    send(hwnd, 0xF0, 0, 0, 0x2, 500, ct.byref(checked))
                    assert checked.value == 0, label
            if show:
                images.append(("卸载时保留的数据", record(options, screenshots, "10-uninstall-options")))
            if mode == "cancel-options":
                click(options, "取消")
                process.wait(12)
                assert uninstaller.exists()
                return
            click(options, "继续")
            confirm = wait_dialog(desk, "卸载", "是否确认卸载")
            if show:
                images.append(("卸载确认", record(confirm, screenshots, "11-uninstall-confirm", "是否确认卸载")))
            if mode == "cancel-confirm":
                click(confirm, command_id=7)
                process.wait(12)
                assert uninstaller.exists()
                return
            click(confirm, command_id=6, delay=0)
            if show:
                progress = desk.wait_window("TUninstallProgressForm", "卸载")
                images.append(("卸载进度", record(progress, screenshots, "12-uninstall-progress", "用户设置、任务历史")))
            if expected_partial:
                finish = wait_dialog(desk, "卸载", "部分选定数据未能删除")
                click(finish, "确定")
            finish = wait_dialog(desk, "卸载", "已卸载")
            if show:
                images.append(("卸载完成", record(finish, screenshots, "13-uninstall-finished")))
            click(finish, "确定")
            for _ in range(100):
                if not uninstaller.exists():
                    break
                time.sleep(.1)
            assert not uninstaller.exists()

        seed()
        ui_uninstall(app, "machine-uninstall", show=True)
        assert_state()
        uninstaller = app / "unins000.exe"
        assert registry(lm, key, "InstallLocation") is None and not uninstaller.exists()
        assert registry(lm, r"Software\Classes\\" + namespace + r".fbx\shell\open\command", "") is None
        assert all(p.read_bytes() == b"keep original bytes" for p in (originals, exports))

        # Current-user install, normal upgrade (without forcing a scope),
        # uninstall, and in-place upgrade of an older registered install.
        per = fixture / "per-user"
        def setup(exe, name, *args):
            assert silent(exe, fixture / f"{name}.log", f"/DIR={per}", "/TASKS=", *args) == 0, name
            assert Path(registry(cu, key, "InstallLocation")).resolve() == per.resolve()
            assert registry(lm, key, "InstallLocation") is None
        def uninstall(name):
            assert silent(per / "unins000.exe", fixture / f"{name}.log") == 0
            for _ in range(100):
                if not (per / "unins000.exe").exists():
                    break
                time.sleep(.1)
            assert not (per / "unins000.exe").exists()
            assert registry(cu, key, "InstallLocation") is None
        setup(new, "per-user-fresh", "/CURRENTUSER")
        assert registry(cu, profile, "TaskHistoryLayout") == "installed"
        setup(new, "per-user-upgrade")
        assert registry(cu, profile, "TaskHistoryLayout") == "installed"
        seed()
        ui_uninstall(per, "cancel-options", remove_settings=True, remove_history=True, mode="cancel-options")
        assert_state()
        ui_uninstall(per, "cancel-confirm", remove_settings=True, remove_history=True, mode="cancel-confirm")
        assert_state()
        uninstall("per-user-uninstall")
        assert_state()  # Silent uninstall always retains both.
        assert registry(cu, profile, "TaskHistoryLayout") == "installed"
        for name, rs, rh in [("settings-only", True, False), ("history-only", False, True), ("both", True, True)]:
            setup(new, name + "-install", "/CURRENTUSER")
            seed()
            ui_uninstall(per, name, remove_settings=rs, remove_history=rh)
            assert_state(delete_settings=rs, delete_history=rh)
            assert registry(cu, profile, "TaskHistoryLayout") == "installed"
        setup(new, "junction-guard-install", "/CURRENTUSER")
        seed()
        external = fixture / "untouched-external"
        external.mkdir()
        (external / f"{guid}.json").write_bytes(b"outside journal")
        alias = installed / ("b" * 64)
        alias.symlink_to(external, target_is_directory=True)
        ui_uninstall(per, "junction-guard", remove_history=True, expected_partial=True)
        assert_state(delete_history=True)
        assert alias.is_symlink() and (external / f"{guid}.json").read_bytes() == b"outside journal"
        alias.rmdir()  # Remove the test symlink itself; do not traverse it.
        winreg.DeleteKeyEx(cu, profile, winreg.KEY_WOW64_64KEY)
        setup(legacy, "legacy-install")
        assert registry(cu, profile, "TaskHistoryLayout") is None
        setup(new, "upgrade-from-legacy")
        assert registry(cu, profile, "TaskHistoryLayout") == "legacy"
        seed()
        ui_uninstall(per, "legacy-uninstall", remove_history=True)
        assert_state(delete_legacy=True)
        assert registry(cu, profile, "TaskHistoryLayout") == "legacy"
        winreg.DeleteKeyEx(cu, profile, winreg.KEY_WOW64_64KEY)
        winreg.DeleteKeyEx(cu, settings, winreg.KEY_WOW64_64KEY)
        winreg.DeleteKeyEx(cu, "Software\\" + namespace, winreg.KEY_WOW64_64KEY)
        assert registry(cu, prod_profile, "TaskHistoryLayout") == profile_before
        assert {name: registry(cu, prod_settings, name) for name in settings_before} == settings_before
        make_sheet(images, build.EVIDENCE / "安装与卸载预览.png")
        make_sheet([images[i] for i in (9, 10, 11, 12)], build.EVIDENCE / "新版界面一览.png")
        save(build.EVIDENCE / "verification.json", dict(passed=True, releaseSha256=sha(release),
             realDeliveryBeforeInstall=True, isolatedDesktop=True, machineScopeRegistryVerified=True,
             uninstallerNativeDialogVerified=True, originalAndExportPreserved=True,
             freshLayout="installed", legacyUpgradeLayout="legacy",
             layoutSurvivedUninstall=True, nativeDefaultAppChangeTested=False,
             optionalRemovalCases=["keep both", "delete settings only", "delete history only", "delete both"],
             silentUninstallKeptData=True, bothCancelPathsKeptData=True,
             devHistoryAndUnrelatedFilesKept=True, reparseTargetKept=True, legacyActiveHistoryDeleted=True,
             screenshotPaths=[str(p) for _, p in images], fixture=str(fixture)))
        print("PASS wizard, both scopes, upgrade, all uninstall choices, cancellation, and safe history cleanup", flush=True)
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            close_handle(process.handle)
        desk.close()


if __name__ == "__main__":
    main()
