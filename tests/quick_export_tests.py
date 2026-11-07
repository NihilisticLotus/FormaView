"""Deterministic asynchronous UI boundary tests with a controlled service double."""
import argparse
import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("qmltestrunner", type=Path)
parser.add_argument("--artifacts", type=Path)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="formaview-quick-") as folder:
    work = Path(folder)
    for source in (repo / "src/host").glob("*.qml"):
        shutil.copy2(source, work / source.name)
    # qmltestrunner has no application resource bundle; use identical source icons.
    icon = work / "AppIcon.qml"
    icon.write_text(icon.read_text(encoding="utf-8").replace(
        "qrc:/formaview/icons/", (repo / "src/host/icons").as_uri() + "/"), encoding="utf-8")
    (work / "qmldir").write_text("singleton Theme 1.0 Theme.qml\nsingleton Explain 1.0 Explain.qml\n", encoding="utf-8")
    test = work / "tst_quick_export.qml"
    shutil.copy2(repo / "tests/tst_quick_export.qml", test)
    if args.artifacts:
        args.artifacts.mkdir(parents=True, exist_ok=True)
        test.write_text(test.read_text(encoding="utf-8").replace('property string artifactRoot: ""',
            'property string artifactRoot: ' + json.dumps(args.artifacts.resolve().as_posix())), encoding="utf-8")
    log = work / "result.txt"
    result = subprocess.run([str(args.qmltestrunner), "-input", str(test), "-o", str(log) + ",txt"],
                            env={**os.environ, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_CONTROLS_STYLE": "Basic",
                                 "QT_QPA_FONTDIR": os.environ.get("WINDIR", "C:/Windows") + "/Fonts"})
    if log.exists():
        output = log.read_text(encoding="utf-8")
        if args.artifacts:
            (args.artifacts / "qml-tests.txt").write_text(output, encoding="utf-8")
        print(output)
        if any(error in output for error in ("TypeError:", "ReferenceError:", "Binding loop", "Unable to assign")):
            raise SystemExit(1)
    raise SystemExit(result.returncode)
