"""Check file-category mapping and the unchanged association ownership boundary."""
import hashlib
import importlib.util
from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[1]


def load_build():
    spec = importlib.util.spec_from_file_location("installer_build", REPO / "tools/installer-ui/build.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def icon_map(source, extensions):
    catalog = source.split("function FormatIconFilename", 1)[1].split("function ExtensionList", 1)[0]
    cases = dict((int(i), name) for i, name in re.findall(r"(\d+): Result := '([^']+)'", catalog))
    assert len(cases) == len(extensions)
    assert "Result := 'formaview.ico'" in catalog  # Unknown indices use the brand fallback.
    return {extension: cases[i] for i, extension in enumerate(extensions)}


def main():
    build = load_build()
    names = build.file_icon_names()
    assert len(set(names.values())) == 3
    for kind, path in build.file_icon_sources().items():
        assert names[kind] == f"formaview-file-{kind}-icon-{hashlib.sha256(path.read_bytes()).hexdigest()}.ico"
    for video in (False, True):
        generated, exts = build.association_source(names, has_video=video)
        mapping = icon_map(generated, exts)
        expected = {}
        for group, formats in build.FORMAT_GROUPS.items():
            if group == "视频" and not video:
                continue
            expected.update({"." + ext: names[build.FILE_ICON_KINDS[group]] for ext, _ in formats})
        assert mapping == expected
        assert (".flv" in mapping) == video
        assert len(mapping) == (36 if video else 25)
        assert mapping[".psa"] == names["model"]
        assert "UserChoice" not in generated
        assert "SHChangeNotify($08000000, 0, 0, 0)" in generated
        assert "FormatIconFilename(I)" in generated
        assert "not SameValue(Key, Name, PreviousData) then exit" in generated
        assert "RegQueryStringValue(InstallRootKey, RecordKey, 'Data', PreviousData)" in generated
        assert "RegWriteStringValue(InstallRootKey, RecordKey, 'Data', Data)" in generated
        changed = dict(names, image="formaview-file-image-icon-" + "a" * 64 + ".ico")
        next_source, next_exts = build.association_source(changed, video)
        next_map = icon_map(next_source, next_exts)
        for ext in mapping:
            assert next_map[ext] == (changed["image"] if mapping[ext] == names["image"] else mapping[ext])
    try:
        build.association_source(dict(names, image="../unsafe.ico"), True)
    except ValueError:
        pass
    else:
        raise AssertionError("Unsafe filename accepted")
    verify = (REPO / "tools/installer-ui/verify.py").read_text(encoding="utf-8-sig")
    assert "for name in file_icon_names:" in verify
    assert 'fixture_files = ("installation.ini", *file_icon_names, "owned-files.txt")' in verify
    print("PASS 36/25 extension mappings, three hashes, scoped updates, ownership guards and Shell notification")


if __name__ == "__main__":
    main()
