"""Windows Qt 6.12.0 Preview regressions; each run uses a new output directory."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import time

parser = argparse.ArgumentParser()
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--qt", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
parser.add_argument("--plugin", type=Path, help="Verify this installed copy of the built plugin")
parser.add_argument("--sdk-discovery", action="store_true",
                    help="Use only the SDK plugin path; no project plugin override")
args = parser.parse_args()
root = args.output.resolve()
root.mkdir(parents=True, exist_ok=False)
manifest = json.loads((args.build / "plugin-RelWithDebInfo.json").read_text())
plugin = (args.plugin or Path(manifest["deployed"])).resolve(strict=True)
assert plugin.read_bytes() == Path(manifest["built"]).read_bytes()
if args.sdk_discovery:
    assert plugin == (args.qt / "plugins/qmltooling/qmldbg_preview.dll").resolve(strict=True)
driver = Path(json.loads((args.build / "regression-RelWithDebInfo.json").read_text())["executable"])
env = os.environ.copy()
env.update(PATH=str(args.qt / "bin") + ";" + env["PATH"],
           QT_PLUGIN_PATH=(str(args.qt / "plugins") if args.sdk_discovery else
                           str(plugin.parent.parent) + ";" + str(args.qt / "plugins")),
           QML_IMPORT_PATH=str(args.qt / "qml"), QT_QUICK_BACKEND="software",
           QT_QPA_PLATFORM="offscreen")
results = []
for fixture, case in [("text", "text"), ("theme", "theme"), ("structure", "structure"),
                      ("handlers-v2", "handlers"), ("external-update", "handlers"),
                      ("invalid", "invalid")]:
    out = root / fixture
    process = subprocess.Popen([str(driver), str(out),
        str(Path(__file__).parent / "fixtures" / fixture), case],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    loaded = None
    deadline = time.monotonic() + 15
    while process.poll() is None and time.monotonic() < deadline:
        try:
            pid = int(json.loads((out / "state.json").read_text(encoding="utf-8"))["pid"])
            probe = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                f"(Get-Process -Id {pid}).Modules | Where-Object ModuleName -eq 'qmldbg_preview.dll' | Select-Object -ExpandProperty FileName"],
                capture_output=True, text=True, timeout=10)
            if probe.stdout.strip():
                loaded = Path(probe.stdout.strip()).resolve(strict=True)
                break
        except (FileNotFoundError, json.JSONDecodeError):
            pass
        time.sleep(.05)
    try:
        stdout, stderr = process.communicate(timeout=150)
    except subprocess.TimeoutExpired:
        process.kill()
        process.communicate()
        raise
    (root / (fixture + "-driver.log")).write_bytes(stdout + stderr)
    d = json.loads((out / "result.json").read_text(encoding="utf-8"))
    valid = (loaded == plugin and process.returncode == 0 and d["exit_code"] == 0
             and not d["crashed"] and d["confirmed_in_place"] and d["a_restored_bytes"]
             and not d.get("first_unobserved") and not d["hot_reload_failures"])
    stages = d["stages"]
    valid &= len(stages) == (3 if case == "invalid" else 7)
    for index, stage in enumerate(stages):
        state = stage["after"]
        valid &= stage["command_completed"] and state["pid"] == d["target_pid"]
        valid &= state["root"] == d["baseline"]["root"]
        if case != "handlers":
            valid &= stage["navigation"]
        if case == "theme":
            expected = "#1976d2" if index % 2 else "#c74c0a"
            valid &= state["theme_value"] == expected
            valid &= state["properties"]["themeColor"] == expected
            valid &= state["theme_readonly"] and state["external_theme_write_rejected"]
            valid &= state["theme_pointer"] == d["baseline"]["theme_pointer"]
        if case == "handlers":
            for name in ("plainCount", "arrowCount", "connectionsCount",
                         "externalBlockCount", "externalArrowCount"):
                valid &= state["properties"][name] == index + 1
            item = next(x for x in state["items"] if x["name"] == "signal-target")
            valid &= item["internalCount"] == index + 1 and item["keysCount"] == index + 1
            if fixture == "handlers-v2":
                expected = "B" if index % 2 else "A"
                valid &= item["lastKeyEpoch"] == expected and item["lastPulseEpoch"] == expected
                valid &= state["identities"] == d["baseline"]["identities"]
            else:
                for name in ("externalBlockValue", "externalArrowValue", "functionValue"):
                    valid &= state["properties"][name] == (70 if index % 2 else 7)
    if case == "invalid":
        valid &= bool(d["errors"]) and all("Expected token" in e for e in d["errors"])
    else:
        valid &= not d["errors"]
    # The legacy handler cases deliberately exercise the deprecated implicit
    # Connections syntax as well as modern functions. Any other warning is a
    # regression, including transient null-parent bindings during reconstruction.
    warnings = d["final"].get("warnings", [])
    legacy_warning = ("QML Connections: Implicitly defined onFoo properties in Connections are deprecated. "
                      "Use this syntax instead: function onFoo(<arguments>) { ... }")
    expected_warnings = [w for w in warnings if fixture in ("handlers-v2", "external-update")
        and "/ExternalObservers.qml:" in w and w.endswith(legacy_warning)]
    unexpected_warnings = [w for w in warnings if w not in expected_warnings]
    valid &= not unexpected_warnings
    results.append({"case": fixture, "passed": bool(valid), "loaded_plugin": str(loaded),
        "exit_code": d["exit_code"], "stages": len(stages), "restored": d["a_restored_bytes"],
        "expected_compile_error": case == "invalid", "errors": d["errors"],
        "warnings": warnings, "expected_legacy_warnings": expected_warnings,
        "unexpected_warnings": unexpected_warnings})
    print(json.dumps(results[-1], ensure_ascii=False), flush=True)
(root / "result.json").write_text(json.dumps({"plugin": str(plugin),
    "sdk_discovery": args.sdk_discovery,
    "plugin_sha256": hashlib.sha256(plugin.read_bytes()).hexdigest(), "results": results},
    ensure_ascii=False, indent=2), encoding="utf-8")
assert all(r["passed"] for r in results)
