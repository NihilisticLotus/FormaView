"""Independently audit a completed headless GIF soak; never modifies inputs."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import statistics

MIB = 1024 * 1024


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def audit(root: Path) -> dict:
    summary = read_json(root / "summary.json")
    protocol = read_json(root / "protocol.json")
    duration = protocol["seconds"]
    assert summary["status"] == "passed", "The runner has not passed"
    assert summary["requested_seconds"] == duration
    assert summary["elapsed_seconds"] >= duration
    limits = {"private_tail_growth_limit_mib": 16, "private_peak_growth_limit_mib": 64,
              "private_slope_limit_mib_per_minute": .5, "handle_growth_limit": 16,
              "cancel_limit_ms": 2000, "seek_limit_ms": 5000}
    assert all(protocol[k] == v for k, v in limits.items()), "Acceptance gates changed"
    frozen = root / "manifest.json"
    if frozen.exists():
        manifest = read_json(frozen)
        assert manifest["duration_seconds"] == duration
        for entry in manifest["files"]:
            assert digest(Path(entry["path"])).lower() == entry["sha256"].lower(), entry["path"]
    for path, expected in protocol["input_sha256"].items():
        assert digest(Path(path)) == expected == summary["input_sha256"][path], path
    with (root / "samples.csv").open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    for row in rows:
        for key in row.keys() - {"phase"}:
            row[key] = float(row[key])
    assert len(rows) == summary["sample_count"]
    gaps = [b["seconds"] - a["seconds"] for a, b in zip(rows, rows[1:])]
    assert gaps and min(gaps) > 0 and max(gaps) < 5, "Telemetry stalled or was reordered"
    assert rows[0]["seconds"] < 1 and rows[-1]["seconds"] >= duration
    measured = [r for r in rows if protocol["warmup_seconds"] <= r["seconds"] <= duration]
    early = [r for r in measured if r["seconds"] <= protocol["warmup_seconds"] + protocol["comparison_window_seconds"]]
    late = [r for r in measured if r["seconds"] >= duration - protocol["comparison_window_seconds"]]
    baseline = statistics.median(r["private_bytes"] for r in early)
    tail = statistics.median(r["private_bytes"] for r in late)
    peak = max(r["private_bytes"] for r in measured)
    times = [r["seconds"] / 60 for r in measured]
    values = [r["private_bytes"] / MIB for r in measured]
    mean_x, mean_y = statistics.mean(times), statistics.mean(values)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(times, values)) / sum((x - mean_x) ** 2 for x in times)
    handle_growth = max(r["handles"] for r in measured) - statistics.median(r["handles"] for r in early)
    growth = (tail - baseline) / MIB
    assert growth <= 16 and (peak - baseline) / MIB <= 64 and handle_growth <= 16
    if duration >= 600:
        assert slope <= .5, "Private memory has a positive long-term trend"
    assert summary["max_cancel_ms"] <= 2000 and summary["max_seek_ms"] <= 5000
    assert summary["natural_wraps"] >= duration // 2 and summary["presentations"] >= duration * 4
    assert summary["max_live_presentations"] <= 3
    events = [json.loads(line) for line in (root / "events.jsonl").read_text(encoding="utf-8").splitlines()]
    end = next(e for e in events if e["event"] == "measurement-end")
    assert end["ms"] >= duration * 1000 and events[-1]["event"] == "passed"
    terminals = [e for e in events if e["event"] == "export-terminal"]
    assert sum(not e["cancelled"] for e in terminals) == summary["complete_exports"]
    assert sum(e["cancelled"] for e in terminals) == summary["cancelled_exports"]
    assert sum(e["files"] for e in terminals) == summary["verified_pngs"]
    assert sum(e["event"] == "export-verified" for e in events) == len(terminals)
    assert not list(root.glob("batch-*")), "Unverified export directories remain"
    return {"status": "passed", "long_run": duration >= 1800, "requested_seconds": duration,
            "elapsed_seconds": summary["elapsed_seconds"], "samples": len(rows), "max_sample_gap_seconds": max(gaps),
            "baseline_private_mib": baseline / MIB, "tail_private_mib": tail / MIB,
            "tail_growth_mib": growth, "peak_growth_mib": (peak - baseline) / MIB,
            "private_slope_mib_per_minute": slope, "handle_growth": handle_growth,
            "baseline_working_set_mib": statistics.median(r["working_set_bytes"] for r in early) / MIB,
            "tail_working_set_mib": statistics.median(r["working_set_bytes"] for r in late) / MIB,
            "source_and_frozen_hashes_unchanged": True,
            "scope": "CPU process/session stability; no GUI, GPU or visual presentation measured"}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    args = parser.parse_args()
    result = audit(args.evidence.resolve())
    text = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    (args.evidence / "analysis.json").write_text(text, encoding="utf-8")
    print(text, end="")


if __name__ == "__main__":
    main()
