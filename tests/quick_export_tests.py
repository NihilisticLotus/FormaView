"""Deterministic asynchronous UI boundary tests with a controlled service double."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("qmltestrunner", type=Path)
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
    log = work / "result.txt"
    result = subprocess.run([str(args.qmltestrunner), "-input", str(test), "-o", str(log) + ",txt"],
                            env={**os.environ, "QT_QPA_PLATFORM": "offscreen", "QT_QUICK_CONTROLS_STYLE": "Basic"})
    if log.exists():
        output = log.read_text(encoding="utf-8")
        print(output)
        if any(error in output for error in ("TypeError:", "ReferenceError:", "Binding loop", "Unable to assign")):
            raise SystemExit(1)
    raise SystemExit(result.returncode)
