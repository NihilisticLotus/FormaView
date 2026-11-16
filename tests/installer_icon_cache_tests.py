"""Regression checks for content-addressed Windows file-association icons."""
import hashlib
import importlib.util
from pathlib import Path
import re


REPO = Path(__file__).resolve().parents[1]
BUILD_PATH = REPO / "tools/installer-ui/build.py"
ASSOCIATIONS_PATH = REPO / "tools/installer-ui/templates/0.1.0/associations.iss"
ICON_PATH = REPO / "src/host/icons/formaview.ico"


def load_build():
    spec = importlib.util.spec_from_file_location("formaview_installer_build", BUILD_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    build = load_build()
    digest = hashlib.sha256(ICON_PATH.read_bytes()).hexdigest()
    filename = f"formaview-file-icon-{digest}.ico"
    generated = build.association_source(filename)
    template = ASSOCIATIONS_PATH.read_text(encoding="utf-8-sig")
    build_source = BUILD_PATH.read_text(encoding="utf-8-sig")

    assert "@@FILE_ICON_FILENAME@@" not in generated
    assert f"{{app}}\\{filename}" in generated
    assert "DefaultIcon" in generated
    assert "SHChangeNotify($08000000, 0, 0, 0)" in generated
    assert re.search(r"file_icon_name\s*=\s*f\"formaview-file-icon-\{sha\(icon\)\}\.ico\"", build_source)
    assert 'for name in ("runtime-manifest.json", "formaview.ico", file_icon_name,' in build_source

    # An owned value may move to the new content-addressed path only while the
    # registry still equals the ledger's previous data. External changes win.
    assert "RegQueryStringValue(HKCU64, RecordKey, 'Data', PreviousData)" in template
    assert "not SameValue(Key, Name, PreviousData) then exit" in template
    assert "RegWriteStringValue(HKCU64, RecordKey, 'Data', Data)" in template
    assert "NewIcon := '" in template and "@@FILE_ICON_FILENAME@@" in template

    verify_source = (REPO / "tools/installer-ui/verify.py").read_text(encoding="utf-8-sig")
    assert "generated association source must reference the content-addressed file icon" in verify_source
    assert "(fixture / file_icon_name).write_bytes" in verify_source
    print(f"PASS content-addressed association icon {filename}")


if __name__ == "__main__":
    main()
