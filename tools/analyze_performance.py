"""Summarize measured frame/GPU timings without treating model-only FPS as game FPS.

Input CSV requires frame_ms (real rendered frame interval, frame generation off).
Optional GPU timestamp columns: neural_ms, pack_ms, inference_ms, unpack_ms.
Optional vram_mib, bypassed (0/1), and bypass_reason describe GPU memory and
NR bypasses. Warmup samples are removed explicitly.
"""
from __future__ import annotations
import argparse
import csv
import json
import math
import statistics
from pathlib import Path

TIMING_COLUMNS = ("frame_ms", "neural_ms", "pack_ms", "inference_ms", "unpack_ms")


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    at = (len(ordered) - 1) * fraction
    low = int(at)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (at - low)


def summarize(path: Path, warmup: int = 120) -> dict:
    if warmup < 0:
        raise ValueError("warmup must be nonnegative")
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames or "frame_ms" not in reader.fieldnames:
            raise ValueError("CSV must contain frame_ms for real rendered frame intervals")
        columns = [name for name in TIMING_COLUMNS if name in reader.fieldnames]
        data = {name: [] for name in columns}
        vram, bypasses, bypass_reasons = [], [], {}
        count = 0
        for line, row in enumerate(reader, 2):
            count += 1
            parsed = {}
            for name in columns:
                try:
                    value = float(row[name])
                except (TypeError, ValueError) as error:
                    raise ValueError(f"line {line}: invalid {name}") from error
                if not math.isfinite(value) or value < 0 or (name == "frame_ms" and value == 0):
                    raise ValueError(f"line {line}: {name} must be finite and {'positive' if name == 'frame_ms' else 'nonnegative'}")
                parsed[name] = value
            memory = None
            if "vram_mib" in reader.fieldnames:
                try:
                    memory = float(row["vram_mib"])
                except (TypeError, ValueError) as error:
                    raise ValueError(f"line {line}: invalid vram_mib") from error
                if not math.isfinite(memory) or memory < 0:
                    raise ValueError(f"line {line}: vram_mib must be finite and nonnegative")
            bypassed = None
            if "bypassed" in reader.fieldnames:
                if row["bypassed"] not in ("0", "1"):
                    raise ValueError(f"line {line}: bypassed must be 0 or 1")
                bypassed = row["bypassed"] == "1"
            if "bypass_reason" in reader.fieldnames and bypassed is None:
                raise ValueError("bypass_reason requires a bypassed column")
            if count > warmup:
                for name in columns:
                    data[name].append(parsed[name])
                if memory is not None:
                    vram.append(memory)
                if bypassed is not None:
                    bypasses.append(bypassed)
                    if bypassed:
                        reason = row.get("bypass_reason", "").strip() or "unspecified"
                        bypass_reasons[reason] = bypass_reasons.get(reason, 0) + 1
    if not data["frame_ms"]:
        raise ValueError("no measured samples remain after warmup")
    stats = {}
    for name, values in data.items():
        stats[name] = {
            "mean": statistics.fmean(values), "median": statistics.median(values),
            "p95": percentile(values, 0.95), "p99": percentile(values, 0.99),
            "min": min(values), "max": max(values),
        }
    return {
        "source": str(path), "samples": len(data["frame_ms"]), "warmup_samples": warmup,
        "units": "milliseconds", "timings": stats,
        "real_rendered_fps": 1000 / stats["frame_ms"]["mean"],
        "p95_frame_budget_met_60fps": stats["frame_ms"]["p95"] <= 1000 / 60,
        "vram_mib": {"mean": statistics.fmean(vram), "peak": max(vram)} if vram else None,
        "bypassed_frames": sum(bypasses) if bypasses else None,
        "bypass_reasons": bypass_reasons if bypasses else None,
        "all_measured_frames_executed_nr": not any(bypasses) if bypasses else None,
        "measurement_requirement": "Frame generation must be off. GPU columns require timestamps covering the named work, not CPU enqueue duration.",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--warmup", type=int, default=120)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        report = summarize(args.csv, args.warmup)
        text = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as destination:
                destination.write(text)
        print(text, end="")
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
