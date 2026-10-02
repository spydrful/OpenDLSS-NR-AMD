"""Convert one game's one swapchain from PresentMon 2.3.1 CSV into frame intervals.

Keep dropped/unshown presents and outliers. Only explicit known generated frame
types are removed. Real-rendered FPS requires an external FG-off assertion.
Official schema: GameTechDev/PresentMon v2.3.1, README-ConsoleApplication.md and
PresentMon/CsvOutput.cpp. No runtime-job/frame-ID association is inferred.
"""
from __future__ import annotations
import argparse
import csv
from decimal import Decimal, InvalidOperation
import hashlib
import json
import math
from pathlib import Path
import statistics

GENERATED_TYPES = {"intel xess-fg", "amd afmf"}


def number(value, label):
    try:
        result = Decimal(value)
        if not result.is_finite() or result < 0:
            raise ValueError()
        return result
    except (InvalidOperation, TypeError, ValueError) as error:
        raise ValueError(f"invalid nonnegative {label}: {value!r}") from error


def stats(values):
    values = sorted(values)
    def percentile(q):
        at = (len(values) - 1) * q
        lo = int(at)
        return values[lo] + (values[min(lo + 1, len(values) - 1)] - values[lo]) * (at - lo)
    return {"mean": statistics.fmean(values), "median": statistics.median(values),
            "p95": percentile(.95), "p99": percentile(.99), "min": values[0], "max": values[-1]}


def analyze(path: Path, *, process_name=None, process_id=None, swap_chain=None,
            start_seconds=None, end_seconds=None, qpc_start=None, qpc_end=None,
            qpc_frequency=None, qpc_unit=None, cpu_start_unit="seconds", warmup=0,
            frame_generation_off=False):
    if warmup < 0 or cpu_start_unit not in ("seconds", "milliseconds"):
        raise ValueError("invalid warmup or CPUStartTime unit")
    if (start_seconds is not None or end_seconds is not None) and (qpc_start is not None or qpc_end is not None):
        raise ValueError("choose a seconds range or a QPC range")
    start = number(start_seconds, "start seconds") if start_seconds is not None else None
    end = number(end_seconds, "end seconds") if end_seconds is not None else None
    qstart = number(qpc_start, "QPC start") if qpc_start is not None else None
    qend = number(qpc_end, "QPC end") if qpc_end is not None else None
    if (start is not None and end is not None and start >= end) or (qstart is not None and qend is not None and qstart >= qend):
        raise ValueError("range start must be less than end")
    frequency = number(qpc_frequency, "QPC frequency") if qpc_frequency is not None else None
    if frequency is not None and (frequency <= 0 or frequency != frequency.to_integral_value()):
        raise ValueError("QPC frequency must be a positive integer ticks/second")
    if qpc_unit not in (None, "ticks", "milliseconds"):
        raise ValueError("invalid QPC unit")
    with path.open(newline="", encoding="utf-8-sig") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames:
            raise ValueError("empty CSV")
        names = {x.casefold(): x for x in reader.fieldnames}
        if len(names) != len(reader.fieldnames):
            raise ValueError("duplicate CSV columns")
        def column(*aliases):
            return next((names[x.casefold()] for x in aliases if x.casefold() in names), None)
        app, pid, chain = column("Application"), column("ProcessID"), column("SwapChainAddress")
        if not all((app, pid, chain)):
            raise ValueError("CSV requires Application, ProcessID and SwapChainAddress")
        raw = list(reader)
    selected = []
    identities = set()
    for index, row in enumerate(raw, 2):
        try:
            identity = (row[app].strip().casefold(), int(row[pid]), row[chain].strip().casefold())
        except (TypeError, ValueError, AttributeError) as error:
            raise ValueError(f"line {index}: invalid process/swapchain identity") from error
        if identity[1] < 0 or not identity[0] or not identity[2]:
            raise ValueError(f"line {index}: invalid process/swapchain identity")
        if process_name is not None and identity[0] != process_name.casefold():
            continue
        if process_id is not None and identity[1] != process_id:
            continue
        if swap_chain is not None and identity[2] != swap_chain.casefold():
            continue
        identities.add(identity)
        selected.append((index, row))
    if len(identities) != 1:
        raise ValueError(f"select exactly one process/PID/swapchain; matched {len(identities)}: {sorted(identities)}")

    direct = column("MsBetweenPresents")
    frame_type = column("FrameType")
    dropped = column("Dropped")
    # PresentMon 2.3.1's default hybrid schema contains both present and CPU
    # clocks. MsBetweenPresents measures consecutive PresentStartTime values,
    # whereas CPUStartQPC is the preceding application's present-end. Projecting
    # a present delta backwards from a CPU timestamp creates false endpoints.
    # QPCTime is ambiguous between --qpc_time and --qpc_time_ms in v1.
    qclock = (column("TimeInQPC", "QPCTime") if direct else
              column("CPUStartQPC", "CPUStartQPCTime", "CPUStartQPCTimeInMs", "TimeInQPC", "QPCTime"))
    qunit = None
    if qclock:
        folded = qclock.casefold()
        qunit = "milliseconds" if "qpctime" in folded and folded != "qpctime" else "ticks"
        if folded == "qpctime":
            qunit = qpc_unit
    relative = (column("TimeInSeconds", "TimeInMs") if direct else
                column("CPUStartTimeInSeconds", "CPUStartTime", "TimeInSeconds", "TimeInMs"))
    relative_unit = "milliseconds" if relative and relative.casefold() == "timeinms" else "seconds"
    if relative and relative.casefold() == "cpustarttime":
        relative_unit = cpu_start_unit
    use_qpc = relative is None and qclock is not None
    clock = qclock if use_qpc else relative
    clock_unit = qunit if use_qpc else relative_unit
    needs_qpc = use_qpc or qstart is not None or qend is not None
    if needs_qpc and not qclock:
        raise ValueError("matching present QPC clock required for bounded MsBetweenPresents" if direct else
                         "QPC clock required but absent")
    if needs_qpc and qunit is None:
        raise ValueError("v1 QPCTime requires explicit --qpc-unit ticks or milliseconds")
    if needs_qpc and qunit == "ticks" and frequency is None:
        raise ValueError("raw QPC timestamps require --qpc-frequency ticks/second")
    if not clock and not direct:
        raise ValueError("CSV lacks MsBetweenPresents or CPU/present timestamps")
    if (start is not None or end is not None) and relative is None:
        raise ValueError("seconds range requires a relative timestamp column; use --qpc-start/--qpc-end for absolute QPC")
    def seconds(value, unit):
        if unit == "ticks":
            if value != value.to_integral_value():
                raise ValueError("raw QPC timestamps must be integer ticks")
            return value / frequency
        return value / 1000 if unit == "milliseconds" else value
    events = []
    for line, row in selected:
        event = {"source_row": line, "frame_type": row.get(frame_type, "") if frame_type else "", "dropped": row.get(dropped, "") if dropped else ""}
        if dropped and event["dropped"] not in ("0", "1"):
            raise ValueError(f"line {line}: Dropped must be 0 or 1")
        event["generated"] = event["frame_type"].strip().casefold() in GENERATED_TYPES
        event["time"] = seconds(number(row[clock], f"line {line} {clock}"), clock_unit) if clock else None
        event["qpc"] = number(row[qclock], f"line {line} {qclock}") if needs_qpc else None
        if needs_qpc:
            seconds(event["qpc"], qunit)  # Validate tick integer even for range-only use.
        event["direct_ms"] = number(row[direct], f"line {line} {direct}") if direct else None
        events.append(event)
    for previous, current in zip(events, events[1:]):
        if clock and current["time"] < previous["time"]:
            raise ValueError("selected timestamps regress; choose one capture/run")
        if needs_qpc and current["qpc"] < previous["qpc"]:
            raise ValueError("selected QPC timestamps regress; choose one capture/run")
    generated = sum(e["generated"] for e in events)
    if frame_generation_off and generated:
        raise ValueError("FG-off assertion conflicts with explicitly generated frames in capture")
    if generated and not clock:
        raise ValueError("removing generated frames requires timestamps to bridge application intervals")
    derived = direct is None or generated > 0
    retained = [e for e in events if not e["generated"]]
    intervals = []
    boundary = out_of_range = 0
    previous = None
    for event in retained:
        if derived:
            if previous is None:
                boundary += 1
                previous = event
                continue
            duration = (event["time"] - previous["time"]) * 1000
            begin = previous["time"]
            qbegin = previous["qpc"]
        else:
            duration = event["direct_ms"]
            # A zero first v1 delta represents the missing prior capture event.
            if previous is None and duration == 0:
                boundary += 1
                previous = event
                continue
            begin = event["time"] - duration / 1000 if clock else None
            qbegin = event["qpc"] - (duration * frequency / 1000 if qunit == "ticks" else duration) if needs_qpc else None
        in_range = (start is None or begin >= start) and (end is None or event["time"] <= end)
        in_range = in_range and (qstart is None or qbegin >= qstart) and (qend is None or event["qpc"] <= qend)
        if not in_range:
            out_of_range += 1
        else:
            value = float(duration)
            if not math.isfinite(value) or value < 0:
                raise ValueError("invalid derived frame interval")
            intervals.append({"frame_ms": value, "source_row": event["source_row"],
                "end_time_seconds": float(event["time"]) if clock else None,
                "frame_type": event["frame_type"], "dropped": event["dropped"]})
        previous = event
    count_before_warmup = len(intervals)
    intervals = intervals[warmup:]
    if not intervals or sum(x["frame_ms"] for x in intervals) <= 0:
        raise ValueError("no positive-duration measurement remains after selection/warmup")
    timing = stats([x["frame_ms"] for x in intervals])
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    identity = next(iter(identities))
    report = {"source": str(path), "source_sha256": digest.hexdigest(), "source_rows": len(raw),
        "selection": {"application": identity[0], "process_id": identity[1], "swap_chain": identity[2],
            "start_seconds": str(start) if start is not None else None, "end_seconds": str(end) if end is not None else None,
            "qpc_start": str(qstart) if qstart is not None else None, "qpc_end": str(qend) if qend is not None else None,
            "qpc_column": qclock if needs_qpc else None, "qpc_unit": qunit if needs_qpc else None,
            "qpc_frequency": int(frequency) if frequency is not None else None},
        "clock_column": clock, "clock_unit": clock_unit,
        "interval_method": "difference between retained application timestamps" if derived else "MsBetweenPresents",
        "field_availability": {"frame_type_column": frame_type, "dropped_column": dropped},
        "observed_explicit_generated_rows": generated if frame_type else None,
        "observed_dropped_presents_retained": sum(x["dropped"] == "1" for x in intervals) if dropped else None,
        "selected_present_rows": len(events), "explicit_generated_rows_removed": generated,
        "retained_application_present_rows": len(retained), "initial_intervals_unavailable": boundary,
        "range_excluded_intervals": out_of_range, "range_intervals_before_warmup": count_before_warmup,
        "warmup_intervals": warmup, "measured_intervals": len(intervals),
        "dropped_presents_retained": sum(x["dropped"] == "1" for x in intervals),
        "zero_intervals_retained": sum(x["frame_ms"] == 0 for x in intervals),
        "frame_generation_off_asserted": frame_generation_off,
        "frame_ms": timing, "application_present_fps": 1000 / timing["mean"],
        "real_rendered_fps": 1000 / timing["mean"] if frame_generation_off else None,
        "p95_frame_budget_met_60fps": timing["p95"] <= 1000 / 60 if frame_generation_off else None,
        "limitations": ["FG-off is an external capture-setting assertion; CSV cannot prove no uninstrumented generation.",
            "Absent FrameType/Dropped columns mean observations are unavailable; zero filtering/count defaults do not prove zero generation/drops.",
            "Unknown frame types, dropped/unshown presents and timing outliers are retained.",
            "Range selection requires both interval endpoints inside the inclusive range; no FPS is inferred from runtime jobs.",
            "No alignment with OpenNR runtime trace frame IDs is attempted."]}
    return report, intervals


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", type=Path)
    parser.add_argument("--process-name")
    parser.add_argument("--process-id", type=int)
    parser.add_argument("--swap-chain")
    parser.add_argument("--start-seconds")
    parser.add_argument("--end-seconds")
    parser.add_argument("--qpc-start", help="absolute bounds in the selected QPC column's units")
    parser.add_argument("--qpc-end")
    parser.add_argument("--qpc-frequency", type=int)
    parser.add_argument("--qpc-unit", choices=("ticks", "milliseconds"), help="required when using ambiguous v1 QPCTime")
    parser.add_argument("--cpu-start-unit", choices=("seconds", "milliseconds"), default="seconds")
    parser.add_argument("--warmup", type=int, default=0)
    parser.add_argument("--frame-generation-off", action="store_true", help="assert that application and driver FG were disabled during this capture")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--frames-csv", type=Path)
    args = parser.parse_args()
    try:
        outputs = [p for p in (args.output, args.frames_csv) if p is not None]
        if len({p.resolve() for p in outputs}) != len(outputs) or any(p.exists() for p in outputs):
            raise ValueError("output paths must be distinct and must not exist")
        report, intervals = analyze(args.csv, process_name=args.process_name, process_id=args.process_id,
            swap_chain=args.swap_chain, start_seconds=args.start_seconds, end_seconds=args.end_seconds,
            qpc_start=args.qpc_start, qpc_end=args.qpc_end, qpc_frequency=args.qpc_frequency,
            qpc_unit=args.qpc_unit, cpu_start_unit=args.cpu_start_unit, warmup=args.warmup,
            frame_generation_off=args.frame_generation_off)
        result = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if args.frames_csv:
            with args.frames_csv.open("x", newline="", encoding="utf-8") as dest:
                writer = csv.DictWriter(dest, fieldnames=list(intervals[0]))
                writer.writeheader()
                writer.writerows(intervals)
        if args.output:
            with args.output.open("x", encoding="utf-8") as dest:
                dest.write(result)
        print(result, end="")
        return 0
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    raise SystemExit(main())
