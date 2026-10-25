"""Every conversion-loss code emitted by the native sources has a trilingual
user explanation in src/host/Explain.qml, and every stable service error code
has a headline. Run: python -I tests/explain_coverage_tests.py [repo-root]"""
import pathlib
import re
import sys

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1])
explain = (root / "src/host/Explain.qml").read_text(encoding="utf-8")
explained = set(re.findall(r'^\s{8}"([a-z0-9_]+)":\s*\[', explain, re.M))

emitted = set()
for source in (root / "src").rglob("*.cpp"):
    text = source.read_text(encoding="utf-8", errors="replace")
    emitted.update(re.findall(r'\bloss\(\s*(?:out\s*,\s*|losses\s*,\s*)?"([a-z0-9_]+)"', text))
    emitted.update(re.findall(r's\.loss\(\s*"([a-z0-9_]+)"', text))
emitted.discard("")

errors = set()
for name in ("image_requests.cpp",):
    block = (root / "src/formats" / name).read_text(encoding="utf-8")
    start = block.index("QString error_name")
    errors.update(re.findall(r'return "([a-z_]+)";', block[start:start + 900]))

missing_losses = sorted(emitted - explained)
missing_errors = sorted(errors - explained)
unused = sorted(c for c in explained - emitted - errors if not c.startswith("_"))
print(f"loss codes emitted: {len(emitted)}; error codes: {len(errors)}; explained keys: {len(explained)}")
if missing_losses or missing_errors:
    print("MISSING loss explanations:", missing_losses)
    print("MISSING error headlines:", missing_errors)
    sys.exit(1)
if unused:
    print("explained but no longer emitted (informational):", unused)
print("explain coverage OK")
