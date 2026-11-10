"""Exercise installer transitions with independent, small, real Inno packages."""
from pathlib import Path
import re
import subprocess
import time
from uuid import uuid4
import winreg

import build
from build import read, save, sha, write
from verify import Desktop, click, make_fixture, record, registry, silent, wait_dialog, wait_text


def variant(fixture, app_id, version, component, abort=False):
    label = "abort" if abort else version
    source = read(fixture / "current.iss").replace("OutputBaseFilename=wizard-fixture", f"OutputBaseFilename=version-{label}")
    payload = fixture / ("payload-" + label)
    payload.mkdir()
    files = {
        "installation.ini": f"[Install]\r\nAppId={app_id}\r\nVersion={version}\r\n".encode(),
        "common.txt": ("program file from " + version).encode(),
        "plugins\\" + component: ("component from " + version).encode(),
    }
    files["owned-files.txt"] = ("\ufeff" + "\r\n".join([*files, "owned-files.txt"]) + "\r\n").encode()
    entries = []
    for rel, content in files.items():
        src = payload / rel
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_bytes(content)
        dest = "" if src.parent == payload else "\\" + str(src.parent.relative_to(payload))
        entries.append(f'Source: "{src}"; DestDir: "{{app}}{dest}"; Flags: ignoreversion')
    include = "files-" + label + ".iss"
    write(fixture / include, "\n".join(entries) + "\n")
    source = source.replace('"files.generated.iss"', f'"{include}"')
    if abort:
        assoc = read(fixture / "associations.iss")
        assert assoc.count("if CurStep = ssInstall then RetirePriorFiles;") == 1
        assoc = assoc.replace("if CurStep = ssInstall then RetirePriorFiles;",
                              "if CurStep = ssInstall then begin RetirePriorFiles; Abort; end;")
        write(fixture / "abort-associations.iss", assoc)
        source = source.replace('"associations.iss"', '"abort-associations.iss"')
    script = fixture / f"variant-{label}.iss"
    write(script, source)
    result = subprocess.run([str(build.ISCC), "/Q", f"/DPayload={fixture}", f"/DOutputPath={fixture}",
                             "/DPayloadMiB=1", "/DCandidateId=fixture", f"/DPackageVersion={version}", str(script)],
                            capture_output=True)
    (fixture / f"compile-{label}.log").write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, result.stdout + result.stderr
    return fixture / f"version-{label}.exe"


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
    _, _, machine = make_fixture(fixture, app_id, profile, namespace)
    components = {"0.1.0": "legacy.dll", "0.1.5": "mid.dll", "0.3.0": "new.dll"}
    packages = {v: variant(fixture, app_id, v, dll) for v, dll in components.items()}
    abort_exe = variant(fixture, app_id, "0.3.0", components["0.3.0"], abort=True)
    per = fixture / "custom location" / "FormaView"
    session = Desktop()
    processes = []
    exercised = []
    current_location = None

    def check(version, root, path, with_asset=False):
        assert registry(root, key, "DisplayVersion") == version
        assert Path(registry(root, key, "InstallLocation")).resolve() == path.resolve()
        assert registry(lm if root == cu else cu, key, "DisplayVersion") is None
        assert f"Version={version}" in read(path / "installation.ini")
        assert (path / "common.txt").read_bytes() == ("program file from " + version).encode()
        assert sorted(p.name for p in (path / "plugins").glob("*.dll")) == [components[version]]
        assert (path / "plugins" / components[version]).read_bytes() == ("component from " + version).encode()
        if with_asset:
            assert (path / "user.glb").read_bytes() == b"user asset"
            assert registry(cu, settings, "preferences") == "settings-sentinel"
            assert user_history.read_bytes() == b"task-history-sentinel"
        assoc = r"Software\Classes\\" + namespace + r".fbx\shell\open\command"
        assert registry(root, assoc, "") == f'"{path}\\formaview.exe" --open "%1"'

    def setup(version, name, *args):
        assert silent(packages[version], fixture / (name + ".log"), *args) == 0, name

    def uninstall(path, name):
        exe = path / "unins000.exe"
        assert silent(exe, fixture / (name + ".log")) == 0
        for _ in range(100):
            if not exe.exists():
                break
            time.sleep(.1)
        assert not exe.exists()

    def change(previous, incoming, root, path, name, with_asset=False):
        setup(incoming, name)  # No /DIR or scope: infer both from the installed version.
        check(incoming, root, path, with_asset)
        if previous != incoming:
            records = list(path.parent.glob(".formaview-retired-*/plugins/" + components[previous] + ".retired"))
            assert any(p.read_bytes() == ("component from " + previous).encode() for p in records)
        exercised.append({"scope": "user" if root == cu else "machine", "from": previous, "to": incoming})

    def set_qa_value(subkey, name, value):
        with winreg.CreateKeyEx(cu, subkey, 0, winreg.KEY_SET_VALUE | winreg.KEY_WOW64_64KEY) as handle:
            winreg.SetValueEx(handle, name, 0, winreg.REG_SZ, value)

    def preview(prior, incoming, action, number):
        proc = session.launch(packages[incoming], f"/LOG={fixture/f'preview-{number}.log'}")
        processes.append(proc)
        form = session.wait_window("TWizardForm", "FormaView")
        record(form, screenshots, f"{number}-version-welcome", action + " FormaView")
        wait_text(form, f"已安装：{prior}   →   将安装：{incoming}")
        for _ in range(5):
            click(form, "下一步")
        record(form, screenshots, f"{number}-version-ready", action)
        wait_text(form, f"{prior}  →  {incoming}")
        click(form, "取消")
        dlg = wait_dialog(session, "退出", "是否退出安装")
        click(dlg, command_id=6)
        assert proc.wait(12) in (0, 2)

    try:
        setup("0.1.0", "fresh-per-user", "/CURRENTUSER", f"/DIR={per}", "/ASSOC=.fbx")
        current_location = per
        check("0.1.0", cu, per)
        set_qa_value(settings, "preferences", "settings-sentinel")
        user_history = fixture / "account-state/installed/image-journal/desktop-history.json"
        user_history.parent.mkdir(parents=True)
        user_history.write_bytes(b"task-history-sentinel")
        (per / "user.glb").write_bytes(b"user asset")
        change("0.1.0", "0.1.5", cu, per, "user-upgrade-015", True)
        change("0.1.5", "0.3.0", cu, per, "user-upgrade-030", True)
        preview("0.3.0", "0.1.0", "覆盖安装", "15")
        change("0.3.0", "0.1.0", cu, per, "user-cover-010", True)
        change("0.1.0", "0.1.0", cu, per, "user-reinstall", True)

        fail = session.launch(abort_exe, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={fixture/'interrupt.log'}")
        processes.append(fail)
        assert fail.wait(20) != 0
        check("0.1.0", cu, per, True)
        assert "FormaView retired previous file" in read(fixture / "interrupt.log")
        change("0.1.0", "0.3.0", cu, per, "user-direct-upgrade-030", True)
        assert silent(packages["0.1.5"], fixture / "reject-cross-scope.log", "/ALLUSERS") != 0
        assert silent(packages["0.1.5"], fixture / "reject-different-dir.log", f"/DIR={fixture/'wrong-dir'}") != 0
        assert not (fixture / "wrong-dir").exists()
        check("0.3.0", cu, per, True)
        uninstall(per, "uninstall-per-user")
        current_location = None
        assert (per / "user.glb").read_bytes() == b"user asset" and user_history.read_bytes() == b"task-history-sentinel"

        setup("0.1.0", "fresh-machine", "/ALLUSERS", f"/DIR={machine}", "/ASSOC=.fbx")
        current_location = machine
        check("0.1.0", lm, machine)
        change("0.1.0", "0.3.0", lm, machine, "machine-direct-upgrade-030")
        change("0.3.0", "0.1.5", lm, machine, "machine-cover-015")
        uninstall(machine, "uninstall-machine")
        current_location = None
        setup("0.1.0", "fresh-machine-after-uninstall", "/ALLUSERS", f"/DIR={machine}", "/ASSOC=.fbx")
        current_location = machine
        check("0.1.0", lm, machine)
        uninstall(machine, "final-uninstall-machine")
        current_location = None
        assert registry(cu, key, "DisplayVersion") is None and registry(lm, key, "DisplayVersion") is None
        assert registry(cu, settings, "preferences") == "settings-sentinel"
        assert registry(cu, profile, "TaskHistoryLayout") == "installed"
        save(build.EVIDENCE / "version-verification.json", dict(
            passed=True, releaseSha256=sha(build.DEST / "output/FormaView-0.1.0-Windows-x64-Setup.exe"),
            fixture=str(fixture), transitions=exercised, isolatedRegistryAndData=True,
            syntheticInstallerPayloads=True, differentScopeAndDirRejected=True,
            retiredFilesRecoverable=True, interruptedRetirementRestored=True, cleanReinstallAfterUninstall=True,
            originalAndSettingsAndTaskHistoryUnchanged=True, associationsPreserved=True,
            real015And030BuildsTested=False,
        ))
        print("PASS in-place upgrades, earlier-version overwrite, reinstall, interrupted retirement, both scopes and preserved data")
    finally:
        if current_location and (current_location / "unins000.exe").exists():
            uninstall(current_location, "cleanup-uninstall")
        for process in processes:
            if process.poll() is None:
                process.terminate()
        session.close()
        for subkey in (profile, settings, "Software\\" + namespace):
            try:
                winreg.DeleteKeyEx(cu, subkey, winreg.KEY_WOW64_64KEY)
            except FileNotFoundError:
                pass


if __name__ == "__main__":
    main()
