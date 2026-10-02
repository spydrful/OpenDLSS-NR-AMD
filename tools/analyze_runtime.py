"""Summarize completed OpenNR GPU jobs; this trace cannot measure game FPS."""
from __future__ import annotations
import argparse
import csv
import json
import math
import statistics
from pathlib import Path

TIMINGS = ("pack_ms", "preprocess_ms", "inference_ms", "composite_ms", "unpack_ms", "neural_ms")
COUNTERS = ("submitted_frames", "bypassed_frames")


def stats(values):
    values = sorted(values)
    def percentile(q):
        at = (len(values) - 1) * q
        lo = int(at)
        return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (at - lo)
    return {"mean": statistics.fmean(values), "median": statistics.median(values),
            "p95": percentile(.95), "p99": percentile(.99), "min": values[0], "max": values[-1]}


def summarize(path: Path, warmup: int = 0, segment: int | None = None) -> dict:
    if warmup < 0 or (segment is not None and segment < 0):
        raise ValueError("warmup and segment must be nonnegative")
    segments = []
    previous = None
    rows = 0
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        required = {"frame_id", "vram_mib", "allocated_neural_bytes", *TIMINGS, *COUNTERS}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("CSV lacks the OpenNR runtime timing header")
        if len(set(reader.fieldnames)) != len(reader.fieldnames):
            raise ValueError("duplicate CSV columns")
        for line, row in enumerate(reader, 2):
            item = {}
            try:
                for name in ("frame_id", "allocated_neural_bytes", *COUNTERS):
                    item[name] = int(row[name])
                    if item[name] < 0:
                        raise ValueError(f"negative {name}")
                for name in (*TIMINGS, "vram_mib"):
                    item[name] = float(row[name])
                    if not math.isfinite(item[name]) or (item[name] < 0 and not (name == "vram_mib" and item[name] == -1)):
                        raise ValueError(f"invalid {name}")
            except (TypeError, ValueError) as error:
                raise ValueError(f"line {line}: invalid runtime measurement ({error})") from error
            reset = previous is not None and (item["frame_id"] <= previous["frame_id"] or
                any(item[name] < previous[name] for name in COUNTERS))
            if previous is None or reset:
                segments.append([])
            segments[-1].append(item)
            previous = item
            rows += 1
    if not rows:
        raise ValueError("no completed NR jobs in trace")
    if segment is not None and segment >= len(segments):
        raise ValueError(f"segment {segment} absent; found {len(segments)} segments")
    reports = []
    for number, items in enumerate(segments):
        if segment is not None and number != segment:
            continue
        measured = items[warmup:]
        if not measured:
            raise ValueError(f"segment {number}: no jobs remain after warmup")
        memory = [x["vram_mib"] for x in measured if x["vram_mib"] >= 0]
        # Counter deltas start at the last excluded row when available. Without
        # that row, counters before the first observed completed job are unknown.
        baseline = items[warmup - 1] if warmup else measured[0]
        reports.append({
            "segment": number, "completed_jobs": len(measured), "warmup_jobs": warmup,
            "first_frame_id": measured[0]["frame_id"], "last_frame_id": measured[-1]["frame_id"],
            "timings_ms": {name: stats([x[name] for x in measured]) for name in TIMINGS},
            "vram_mib_sampled": {"mean": statistics.fmean(memory), "peak": max(memory)} if memory else None,
            "vram_unknown_jobs": sum(x["vram_mib"] < 0 for x in measured),
            "allocated_neural_bytes_partial_peak": max(x["allocated_neural_bytes"] for x in measured),
            "ending_session_counters": {name: measured[-1][name] for name in COUNTERS},
            "observed_counter_increases": {name: measured[-1][name] - baseline[name] for name in COUNTERS},
            "counter_baseline_frame_id": baseline["frame_id"],
            "first_job_counter_interval_unobserved": warmup == 0,
        })
    return {"source": str(path), "source_completed_jobs": rows, "detected_segments": len(segments),
            "segments": reports,
            "limitations": [
                "Rows are completed NR jobs, not all game frames; no FPS or PresentMon per-frame join is inferred.",
                "Frame-ID/counter regressions split segments; append-only traces may contain multiple runs.",
                "Cumulative counters are snapshots, not per-row events; activity before/after trace coverage is unknown.",
                "VRAM is sampled DXGI process-local usage, refreshed every 60 completed jobs; repeated values are not independent samples.",
                "allocated_neural_bytes excludes weights, game resources and shared buffers; it is not total VRAM.",
                "neural_ms spans the GPU bridge and may include waits; it is not network-only time.",
            ]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--warmup", type=int, default=0, help="completed jobs excluded per detected segment")
    parser.add_argument("--segment", type=int, help="zero-based segment to analyze")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        result = json.dumps(summarize(args.csv, args.warmup, args.segment), indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as dest:
                dest.write(result)
        print(result, end="")
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
