"""Install/restore the local Qt 6.12.0 MSVC x64 Preview workaround.

Never stops another process. A locked destination fails without replacing it.
The backup directory must be new and keeps the original bytes and identities.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def amd64(path):
    data = path.read_bytes()
    offset = struct.unpack_from("<I", data, 0x3C)[0]
    if data[:2] != b"MZ" or data[offset:offset + 4] != b"PE\0\0":
        raise RuntimeError(f"Not a PE image: {path}")
    if struct.unpack_from("<H", data, offset + 4)[0] != 0x8664:
        raise RuntimeError(f"Not an x64 image: {path}")


def replace(source, destination):
    fd, name = tempfile.mkstemp(prefix="formaview-preview-", suffix=".tmp",
                                dir=destination.parent)
    os.close(fd)
    temporary = Path(name)
    try:
        shutil.copy2(source, temporary)
        if digest(temporary) != digest(source):
            raise RuntimeError("Temporary DLL hash mismatch")
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("action", choices=("install", "restore"))
parser.add_argument("--qt", type=Path, required=True)
parser.add_argument("--backup", type=Path, required=True)
parser.add_argument("--build", type=Path)
parser.add_argument("--expected-current", help="Required original SHA-256 when installing")
args = parser.parse_args()
qt = args.qt.resolve(strict=True)
target = qt / "plugins/qmltooling/qmldbg_preview.dll"
version = subprocess.check_output([str(qt / "bin/qmake.exe"), "-query", "QT_VERSION"],
                                  text=True).strip()
spec = subprocess.check_output([str(qt / "bin/qmake.exe"), "-query", "QMAKE_XSPEC"],
                               text=True).strip()
if version != "6.12.0" or spec != "win32-msvc":
    raise RuntimeError(f"Unsupported Qt ABI: {version}, {spec}")
amd64(qt / "bin/Qt6Core.dll")
amd64(target)
backup = args.backup.resolve()
if args.action == "install":
    if not args.build or not args.expected_current:
        parser.error("install requires --build and --expected-current")
    manifest = json.loads((args.build / "plugin-RelWithDebInfo.json").read_text())
    source = Path(manifest["deployed"]).resolve(strict=True)
    amd64(source)
    if digest(source) != digest(Path(manifest["built"])):
        raise RuntimeError("Built and deployed plugin differ")
    if digest(target) != args.expected_current.lower():
        raise RuntimeError("Installed plugin changed; refusing an unknown replacement")
    if digest(source) == digest(target):
        raise RuntimeError("Plugin already installed")
    backup.mkdir(parents=True, exist_ok=False)
    original = backup / "qmldbg_preview.dll"
    shutil.copy2(target, original)
    record = dict(target=str(target.resolve()), qt_version=version, qt_spec=spec,
                  source=str(source), original_sha256=digest(original),
                  installed_sha256=digest(source), status="backed_up")
    (backup / "manifest.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
    if digest(original) != digest(target):
        raise RuntimeError("Backup verification failed")
    replace(source, target)
    if digest(target) != record["installed_sha256"]:
        replace(original, target)
        raise RuntimeError("Installation verification failed; original restored")
    record["status"] = "installed"
else:
    record = json.loads((backup / "manifest.json").read_text(encoding="utf-8"))
    if Path(record["target"]).resolve() != target.resolve():
        raise RuntimeError("Backup belongs to another Qt installation")
    original = backup / "qmldbg_preview.dll"
    if digest(original) != record["original_sha256"]:
        raise RuntimeError("Original backup changed")
    if digest(target) != record["installed_sha256"]:
        raise RuntimeError("Installed plugin changed; refusing to restore over it")
    replace(original, target)
    if digest(target) != record["original_sha256"]:
        raise RuntimeError("Restore verification failed")
    record["status"] = "restored"
(backup / "manifest.json").write_text(json.dumps(record, indent=2), encoding="utf-8")
print(json.dumps(record, indent=2))
