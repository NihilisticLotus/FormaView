"""Build the branded Inno installer directly from the verified runtime."""
import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import struct
from ctypes import wintypes as wt

from PIL import Image, ImageDraw, ImageFont

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
BASE = HERE / "templates/0.1.0"
LOCK = HERE / "compiler-lock.json"
DEV = HISTORY = ISCC = DEST = EVIDENCE = None
IDENTITY = "{618397E7-C70C-4D14-B601-D6FB2F562BA1}"


def configure_staging_root(root=None):
    """Bind release inputs/outputs to an explicit external staging directory."""
    global DEV, HISTORY, ISCC, DEST, EVIDENCE
    value = root or os.environ.get("FORMAVIEW_RELEASE_STAGING_ROOT")
    if not value:
        raise ValueError("Pass --staging-root or set FORMAVIEW_RELEASE_STAGING_ROOT")
    DEV = Path(value).resolve()
    HISTORY = DEV / "verification/M13-task-history-20261110"
    ISCC = DEV / "toolchain/M13/InnoSetup-6.7.3/ISCC.exe"
    DEST = DEV / "deploy/FormaView-0.1.0-Windows-x64-upgrade-policy-20261111"
    EVIDENCE = DEV / "verification/M13-upgrade-policy-20261111"
    return DEV


def read(path):
    return path.read_text(encoding="utf-8-sig")


def sha(path):
    with path.open("rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def write(path, value):
    path.write_text(value, encoding="utf-8-sig")


def save(path, data):
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f"Expected exactly one template marker: {old!r}")
    return text.replace(old, new, 1)

def product_version(exe):
    """Use the fixed Win32 version resource, independent of its localized strings."""
    api = ct.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [wt.LPCWSTR, ct.POINTER(wt.DWORD)]
    api.GetFileVersionInfoSizeW.restype = wt.DWORD
    api.GetFileVersionInfoW.argtypes = [wt.LPCWSTR, wt.DWORD, wt.DWORD, ct.c_void_p]
    api.GetFileVersionInfoW.restype = wt.BOOL
    api.VerQueryValueW.argtypes = [ct.c_void_p, wt.LPCWSTR, ct.POINTER(ct.c_void_p), ct.POINTER(wt.UINT)]
    api.VerQueryValueW.restype = wt.BOOL
    size = api.GetFileVersionInfoSizeW(str(exe), None)
    if not size:
        raise ValueError(f"Missing version resource: {exe}")
    buffer = ct.create_string_buffer(size)
    fixed = ct.c_void_p()
    count = wt.UINT()
    if not api.GetFileVersionInfoW(str(exe), 0, size, buffer) or not api.VerQueryValueW(buffer, "\\", ct.byref(fixed), ct.byref(count)) or count.value < 52:
        raise ValueError(f"Cannot read version resource: {exe}")
    sig, _, fms, fls, pms, pls = struct.unpack("<6I", ct.string_at(fixed, 24))
    if sig != 0xFEEF04BD or (fms, fls) != (pms, pls) or (pls & 0xffff):
        raise ValueError(f"Expected consistent major.minor.patch.0 file and product versions: {exe}")
    return f"{pms >> 16}.{pms & 0xffff}.{pls >> 16}"


def render_art(directory, icon_path):
    source = Image.open(icon_path).convert("RGBA")
    panel = Image.new("RGB", (328, 628), "#202126")
    draw = ImageDraw.Draw(panel)
    draw.rectangle((0, 0, 7, 628), fill="#F28132")
    # Crop marks and grid echo the asset preview viewport.
    for x in range(32, 328, 32):
        draw.line((x, 382, x, 628), fill="#2B2C32", width=1)
    for y in range(400, 628, 32):
        draw.line((8, y, 328, y), fill="#2B2C32", width=1)
    for x, y, sx, sy in [(54, 132, 1, 1), (274, 132, -1, 1),
                          (54, 352, 1, -1), (274, 352, -1, -1)]:
        draw.line([(x, y + 24 * sy), (x, y), (x + 24 * sx, y)], fill="#66666F", width=2)
    mark = source.resize((128, 128), Image.Resampling.LANCZOS)
    panel.paste(mark, (100, 178), mark)
    brand = ImageFont.truetype("C:/Windows/Fonts/segoeuib.ttf", 37)
    caption = ImageFont.truetype("C:/Windows/Fonts/msyh.ttc", 20)
    draw.text((164, 439), "FormaView", anchor="mm", font=brand, fill="#F9F6F2")
    draw.text((164, 486), "资产预览与转换", anchor="mm", font=caption, fill="#DFDDE3")
    draw.rectangle((145, 537, 182, 540), fill="#F28132")
    panel.save(directory / "wizard-panel.bmp")
    header = Image.new("RGB", (112, 112), "#FFFFFF")
    mark = source.resize((96, 96), Image.Resampling.LANCZOS)
    header.paste(mark, (8, 8), mark)
    header.save(directory / "wizard-mark.bmp")


NOTICE = """安装说明

一、安装范围
推荐为本机所有用户安装。程序放在 Windows 的 Program Files 目录，需要管理员权限。仅为当前账户安装时，可以选择该账户有写入权限的本机目录。覆盖升级沿用原有安装范围和目录。

二、程序和快捷方式
安装包包含 FormaView 主程序及所需的运行组件。你可以选择是否添加开始菜单和桌面快捷方式。安装前请保存工作并关闭正在运行的 FormaView。

三、文件关联
可以选择希望双击后用 FormaView 打开的预览格式。安装后请在 Windows 的默认应用设置中确认；每个账户可以有不同的选择。未选择的格式不新增关联注册。

四、用户数据
各账户的设置和任务历史分别保存在自己的 Windows 用户资料中。卸载时可分别选择是否保留当前账户的用户设置和任务历史，两项默认保留。原始素材和导出文件始终保留。

五、第三方许可与分发
第三方许可文本和声明随程序安装，可在安装目录中查阅。此安装包尚未签名，未授予公开再分发权限。
"""

GUIDE = r"""FormaView 0.1.0 · Windows x64

为本机所有用户安装：%ProgramFiles%\FormaView。
仅为当前账户安装：默认 %LOCALAPPDATA%\Programs\FormaView，可选择其他本机目录。
升级、重新安装或覆盖为较早版本时沿用原目录和范围，并保留用户数据。

从开始菜单或安装目录打开 FormaView。如需通过双击打开所选格式，请在 Windows 设置 → 应用 → 默认应用中确认。
卸载入口：Windows 设置 → 应用 → 已安装的应用 → FormaView。
卸载时可分别选择是否保留当前账户的用户设置和任务历史，默认保留。原始素材和导出文件始终保留。第三方许可与声明随程序安装。
"""


def setup_source():
    source = read(BASE / "FormaView.iss")
    source = replace(source, r"DefaultDirName={localappdata}\Programs\FormaView", r"DefaultDirName={autopf}\FormaView")
    source = replace(source, "WizardSizePercent=145,140", """WizardSizePercent=118,108
WizardResizable=yes
WizardImageFile=wizard-panel.bmp
WizardSmallImageFile=wizard-mark.bmp
WizardImageStretch=yes""")
    source = replace(source, "PrivilegesRequired=lowest", """PrivilegesRequired=admin
PrivilegesRequiredOverridesAllowed=dialog
UsePreviousPrivileges=yes
UsePreviousAppDir=yes""")
    source = replace(source, 'Name: "english"; MessagesFile: "compiler:Default.isl"', 'Name: "zh"; MessagesFile: "compiler:Default.isl"')
    start, end = source.index("[Messages]"), source.index("[Tasks]")
    source = source[:start] + "[Messages]\n" + read(HERE / "messages.isl").rstrip() + "\n\n" + source[end:]
    source = source.replace(r'"{userdesktop}\FormaView"', r'"{autodesktop}\FormaView"')
    source = source.replace('Description: "创建开始菜单入口"', 'Description: "在开始菜单中添加 FormaView 和卸载入口"')
    source = source.replace('Description: "创建桌面快捷方式"', 'Description: "在桌面添加 FormaView 快捷方式"')
    source = source.replace('Description: "运行 FormaView"', 'Description: "打开 FormaView"')
    source = replace(source, "Flags: createvalueifdoesntexist", "Flags: createvalueifdoesntexist; Check: IsPerUserInstall")
    source = replace(source, "function InitialTaskHistoryLayout(", r"""function IsPerUserInstall: Boolean;
begin Result := not IsAdminInstallMode; end;

function InstallRootKey: Integer;
begin
  if IsAdminInstallMode then Result := HKLM64 else Result := HKCU64;
end;

function OtherInstallRootKey: Integer;
begin
  if IsAdminInstallMode then Result := HKCU64 else Result := HKLM64;
end;

function InstallScopeLabel: string;
begin
  if IsAdminInstallMode then Result := '本机所有用户'
  else Result := '仅当前账户';
end;

function InitialTaskHistoryLayout(""")
    source = replace(source, "function PrepareToInstall(var NeedsRestart: Boolean): string;",
                     '#include "upgrade-policy.iss"\n\nfunction PrepareToInstall(var NeedsRestart: Boolean): string;')
    source = replace(source, "var Root, PreviousPath, Marker, Probe, SpacePath, Parent: string; Existing: Boolean;",
                     "var Root, PreviousPath, PreviousVersion, Marker, Probe, SpacePath, Parent: string; Existing: Boolean;\n    VersionOrder: Integer;")
    source = replace(source, "  if RegQueryStringValue(HKCU64, UninstallKey, 'InstallLocation', PreviousPath) then",
                     r"""  Result := ReadUpgradeState(PreviousPath, PreviousVersion, VersionOrder);
  if Result <> '' then exit;
  if (CompareText(PreviousPath, PreviousInstallDir) <> 0) or (PreviousVersion <> PreviousInstallVersion) then begin
    Result := '安装状态已变化。请关闭并重新运行此安装程序。'; exit;
  end;
  if IsAdminInstallMode and
     (CompareText(Root, ExpandConstant('{commonpf64}\FormaView')) <> 0) then begin
    Result := '为本机所有用户安装时，请使用标准 Program Files 目录。'; exit;
  end;
  if PreviousPath <> '' then""")
    source = replace(source, "  LegacyTaskHistoryUpgrade := Existing and (PreviousPath <> '');",
                     """  if Existing and (PreviousPath = '') then begin
    Result := '此目录已有 FormaView 文件，但未找到对应的安装登记。请选择空目录或先修复旧版安装。'; exit;
  end;
  LegacyTaskHistoryUpgrade := Existing and (PreviousPath <> '') and IsPerUserInstall;""")
    replacements = {
        "Cannot read owned-file manifest. Reinstall to repair. / 无法读取安装清单，请重装修复。": "无法读取安装文件清单，请重新运行安装程序修复。",
        "Missing previous ownership manifest. / 缺少原安装文件清单。": "缺少原安装文件清单，无法继续覆盖安装。",
        "Invalid ownership manifest.": "安装文件清单有误，操作已停止。",
        "Junction/symbolic link detected; no files changed. / 路径含目录联接或符号链接，已停止。": "路径含目录联接或符号链接，未更改任何文件。",
        "A directory conflicts with an installed file: ": "下列目录与程序文件同名：",
        "Existing user file would be overwritten: ": "下列已有文件不属于 FormaView，安装已停止：",
        "File is in use or not writable. Close FormaView and its MCP service, then retry. / 文件占用或无写权限，请关闭程序及 MCP 服务后重试。": "程序文件仍在使用或无法访问。请关闭 FormaView 及其服务后重试。",
        "Choose a local dedicated folder without junctions. / 请选择不含目录联接的本机专用目录。": "请选择本机磁盘上的专用目录，且路径不能包含目录联接或符号链接。",
        "Upgrade must use the existing folder. To move, uninstall first; settings and tasks are retained. / 覆盖安装须使用原目录；迁移请先卸载，设置和任务会保留。": "升级或重新安装须沿用原目录。请返回原安装目录后重试。",
        "Choose an empty folder. Existing user files will not be overwritten. / 请选择空目录，安装器不会覆盖已有用户文件。": "请选择空目录，安装程序不会覆盖其他已有文件。",
        "Incomplete prior installation; keep user data and use a new empty folder. / 原卸载记录缺失，请保留用户数据并选择新空目录。": "原安装记录不完整，请先修复旧版安装。",
        "Cannot read target disk space. / 无法读取目标卷空间，请检查目录后重试。": "无法读取目标磁盘空间，请检查目录后重试。",
        "Insufficient disk space. / 磁盘空间不足：": "磁盘空间不足：",
        "No permission to create installation folder. / 无权限创建安装目录。": "没有权限创建安装目录。",
        "Write-test path already exists; no existing file will be removed.": "无法验证目录写入权限，现有文件未被删除。",
        "No write permission. Choose a writable folder. / 无写入权限，请选择可写目录。": "没有写入权限，请选择可写入的目录。",
        "Cannot remove installer write probe; installation stopped.": "无法完成目录写入验证，安装已停止。",
    }
    for old, new in replacements.items():
        source = replace(source, old, new)
    source = replace(source, "  ExtractTemporaryFile('owned-files.txt');",
                     r"""  ExtractTemporaryFile('owned-files.txt');
  if Existing then begin
    Result := PlanRetiredPayload(Root, ExpandConstant('{tmp}\owned-files.txt'));
    if Result <> '' then begin Log(Result); exit; end;
  end;""")
    source = replace(source, "function InitializeUninstall: Boolean;",
                     '#include "uninstall-data.iss"\n\nfunction InitializeUninstall: Boolean;')
    source = replace(source, "    SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);\n  end;",
                     "    SuppressibleMsgBox(Problem, mbError, MB_OK, IDOK);\n  end else Result := ChooseUninstallData;")
    return source


def association_source():
    previous = read(BASE / "associations.iss")
    # Preserve the existing extension/ProgID inventory, registration protocol,
    # and ownership ledger. Presentation is defined independently.
    prefix = previous[:previous.index("procedure SelectionChanged")]
    prefix = prefix.replace("HKCU64", "InstallRootKey")
    prefix = prefix.replace("Extensions, Defaults:", "Extensions:")
    prefix = replace(prefix, "  Id := QueryAssociation(Extensions[I], 20);",
                     """  if IsAdminInstallMode then begin
    Result := '需在 Windows 中按账户确认'; exit;
  end;
  Id := QueryAssociation(Extensions[I], 20);""")
    ext = previous[previous.index("function ExtensionList:"):previous.index("procedure InitializeWizard;")]
    return prefix + ext + read(HERE / "wizard-code.iss")


def build(dest, evidence, compile_setup, runtime_delivery=None):
    if DEV is None:
        configure_staging_root()
    if runtime_delivery is None:
        runtime_delivery = HISTORY / "delivery.json"
    for path in (dest, evidence):
        if path.exists() and any(path.iterdir()):
            raise FileExistsError(f"Refusing to overwrite non-empty staging path: {path}")
    engine, metadata, output = (dest / name for name in ("installer", "payload-metadata", "output"))
    for directory in (engine, metadata, output, evidence):
        directory.mkdir(parents=True, exist_ok=True)
    lock = json.loads(read(LOCK))
    assert sha(ISCC) == lock["isccSha256"]
    d = json.loads(read(runtime_delivery))
    assert sha(Path(d["archive"])) == d["archiveSha256"]
    run = Path(d["runDirectory"]).resolve()
    sources = {}
    for row in json.loads(read(runtime_delivery.with_name("delivery-files.json"))):
        name = row["path"]
        path = run / name
        assert path.resolve().is_relative_to(run)
        assert path.stat().st_size == row["bytes"] and sha(path) == row["sha256"]
        if name == "sha256.json":
            (metadata / "runtime-manifest.json").write_bytes(path.read_bytes())
        else:
            sources[name] = path
    assert len(sources) + 1 == d["runtimeFiles"]
    assert all(f"mcp/{name}" in sources for name in ("image_stdio.py", "requirements.txt", "requirements.lock"))
    version = product_version(sources["formaview.exe"])
    if any(product_version(sources[name]) != version for name in ("formaview_image_cli.exe", "formaview_image_service.exe")):
        raise ValueError("Runtime executable versions do not match")
    if version != "0.1.0" and (dest == DEST or evidence == EVIDENCE):
        raise ValueError("Use separate --destination and --evidence for a new product version")
    if version != "0.1.0":
        compatibility = d.get("upgradeCompatibility", {})
        if (compatibility.get("profileContract") != "formaview-user-state-v1" or
            "0.1.0" not in compatibility.get("bidirectionalVerifiedFrom", [])):
            raise ValueError("A newer runtime needs recorded bidirectional data compatibility with 0.1.0")
    icon = REPO / "src/host/icons/formaview.ico"
    (metadata / "formaview.ico").write_bytes(icon.read_bytes())
    (metadata / "installation.ini").write_bytes(f"[Install]\r\nAppId={IDENTITY}\r\nVersion={version}\r\n".encode())
    write(metadata / "安装使用说明.txt", GUIDE.replace("FormaView 0.1.0", f"FormaView {version}", 1))
    for name in ("runtime-manifest.json", "formaview.ico", "installation.ini", "安装使用说明.txt"):
        sources[name] = metadata / name
    names = sorted(set(sources) | {"owned-files.txt", "sha256.json"})
    assert not any("image-journal" in n or n.endswith("desktop-history.json") for n in names)
    assert not any(c in n for n in names for c in '";\n\r{}')
    (metadata / "owned-files.txt").write_bytes(("\r\n".join(n.replace("/", "\\") for n in names) + "\r\n").encode("utf-8-sig"))
    sources["owned-files.txt"] = metadata / "owned-files.txt"
    save(metadata / "sha256.json", [dict(path=n, bytes=sources[n].stat().st_size, sha256=sha(sources[n])) for n in sorted(sources)])
    sources["sha256.json"] = metadata / "sha256.json"
    entries = []
    for name in names:
        parent = Path(name).parent
        suffix = "" if str(parent) == "." else "\\" + str(parent)
        entries.append(f'Source: "{sources[name]}"; DestDir: "{{app}}{suffix}"; Flags: ignoreversion')
    write(engine / "files.generated.iss", "\n".join(entries) + "\n")
    write(engine / "install-notice.txt", NOTICE)
    write(engine / "FormaView.iss", setup_source())
    write(engine / "upgrade-policy.iss", read(HERE / "upgrade-policy.iss"))
    write(engine / "uninstall-data.iss", read(HERE / "uninstall-data.iss"))
    assoc = association_source()
    exts = re.search(r"Text := '([^']+)'", assoc)[1].rstrip(",").split(",")
    assert len(exts) == len(set(exts)) == 25 and not {".pmx", ".vrm"}.intersection(exts)
    assert "UserChoice" not in assoc and '--open "%1"' in assoc
    write(engine / "associations.iss", assoc)
    render_art(engine, icon)
    size = sum(p.stat().st_size for p in sources.values())
    command = [str(ISCC), "/Qp", f"/DPayload={metadata}", f"/DOutputPath={output}",
               f"/DPayloadMiB={(size + 1048575) // 1048576}", f"/DPackageVersion={version}",
               "/DCandidateId=verified-runtime", str(engine / "FormaView.iss")]
    save(evidence / "compile-command.json", command)
    if not compile_setup:
        return
    log = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (evidence / "compile.log").write_bytes(log.stdout)
    if log.returncode:
        raise SystemExit(log.stdout.decode(errors="replace"))
    exe = output / f"FormaView-{version}-Windows-x64-Setup.exe"
    info = dict(installer=str(exe), sha256=sha(exe), bytes=exe.stat().st_size, compiler=lock["version"],
                compilerSha256=sha(ISCC), runtimeZipSha256=d["archiveSha256"], runtimeManifestSha256=d["manifestSha256"],
                runtimeDelivery=str(runtime_delivery), sourceCommit=d["sourceCommit"], sourceIdentity=d["sourceIdentity"],
                payloadFiles=len(names), payloadManifestSha256=sha(sources["sha256.json"]),
                sourceFiles={p.name: sha(p) for p in sorted(engine.iterdir())},
                machineDefaultDirectory=r"%ProgramFiles%\FormaView", sameScopeUpgrade=True, previewExtensions=exts,
                shipsTaskHistory=False, mcpAdapterIncluded=True, stagesDuplicateRuntime=False,
                optionalUninstallSettingsAndHistory=True, uninstallDefaultKeepsBoth=True,
                productVersion=version, versionSource="verified runtime PE resources",
                supportsInPlaceVersionChanges=True, profileContract="formaview-user-state-v1",
                retainsRetiredPayloadFilesOutsideLoadPaths=True)
    save(output / "build-record.json", info)
    save(evidence / "installer-build-record.json", info)
    (output / "SHA256SUMS.txt").write_text(f"{info['sha256']} *{exe.name}\n", encoding="ascii")
    print(f"{exe}\n{info['sha256']} · {info['bytes']} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--staging-root", type=Path)
    parser.add_argument("--runtime-delivery", type=Path)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    configure_staging_root(args.staging_root)
    delivery = args.runtime_delivery.resolve() if args.runtime_delivery else None
    build(args.destination.resolve(), args.evidence.resolve(), not args.prepare_only, delivery)
