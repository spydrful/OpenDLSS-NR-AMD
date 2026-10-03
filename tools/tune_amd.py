"""Collect and assess interleaved AMD network benchmarks without claiming game FPS.

The runner gives each child its own environment and saves raw timestamp samples.
Performance eligibility is separate from numerical/scene quality qualification.
Only qualified preserving-arithmetic records enter the generated tuning file.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import statistics
import subprocess


IDENTITY_KEYS = ("device_id", "driver_id", "model_sha256", "shader_sha256", "baseline_shader_sha256")
COMMON_KEYS = ("device_id", "driver_id", "model_sha256", "baseline_shader_sha256")
EXACT_COVERAGE = {"tails", "shifted-windows", "channel-families", "broadcasts", "split-k",
                  "residuals", "activation", "padding", "conversion-edge-cases"}
VIT_COVERAGE = "vit-k1024-k4096-partitions-v1"
# Bounded CLI operator contract: (K, N, rows, flags, partition, batches).
# Historical reports lack this extension; new init/epilogue proofs require it.
VIT_CASES = ((1024,16,1,0,256,1), (1024,48,63,0,512,1), (1024,128,73,24,0,1),
             (1024,48,73,83,256,1), (1024,16,63,99,512,1), (1024,128,1,19,256,1),
             (1024,48,73,35,512,1), (1024,128,63,16,0,1), (4096,16,1,83,1024,1),
             (4096,48,63,99,1024,1), (4096,128,73,0,1024,1), (4096,48,73,24,1024,1),
             (1024,48,63,152,256,2), (4096,16,73,163,1024,2))
PAIRED_COVERAGE = "paired-preload-k160-k544-batch8-v1"
PAIRED_CASES = ((160,16,1,0,0,1),(160,48,73,24,0,1),(160,48,63,83,32,2),
                (160,48,129,99,160,2),(160,48,73,35,32,3),(160,16,73,152,0,8),
                (192,48,63,0,96,2),(192,48,73,163,32,8),(256,64,73,16,0,8),
                (256,128,73,152,0,8),(256,48,129,83,32,8),(512,48,73,24,0,8),
                (544,48,1,0,32,1),(544,48,73,99,544,2))
WINDOW_PADDING_COVERAGE = "window-thin-padding-v1"
WINDOW_PADDING_CASES = tuple((width,height,heads,sx,sy)
                            for width,height in ((1,1),(1,9),(9,1))
                            for heads in (1,2,4) for sx in (0,4) for sy in (0,4))
SCENE_COVERAGE = {"sdr", "hdr-highlights", "motion", "faces", "moving-objects",
                  "disocclusion", "exposure-changes", "camera-cuts"}
MODEL_CHECKPOINTS = {f"block-{i}" for i in range(70)} | {
    "transition-0-1", "transition-4-5", "transition-8-9", "transition-14-15", "transition-22-23"}
LEGACY_FUSION_KEYS = ("fusion", "expert_fusion", "block_fusion", "hardware_publication")
FUSION_KEYS = (*LEGACY_FUSION_KEYS, "ffn32_fusion", "qkv32_fusion")
SELECTION_KEYS = ("kernels", "arithmetic", "gemm", "tile_n", "stage_k", "window_queries", "window_layout", *FUSION_KEYS)
GEMM_VARIANTS = {"shared": "amd_gemm_optimized", "packed": "amd_gemm_packed", "direct": "amd_gemm_direct",
                 "direct-rte": "amd_gemm_direct_rte", "direct-rte-init": "amd_gemm_direct_rte_init",
                 "direct-rte-epilogue": "amd_gemm_direct_rte_epilogue",
                 "direct-rte-pair": "amd_gemm_direct_rte_pair"}
DIRECT_GEMMS = {"direct", "direct-rte", "direct-rte-init", "direct-rte-epilogue", "direct-rte-pair"}
SCALAR_RTE_GEMMS = DIRECT_GEMMS - {"direct"}
EXTENDED_GEMMS = {"direct-rte-init", "direct-rte-epilogue", "direct-rte-pair"}
WINDOW_LAYOUTS = {"staged": None, "register": "amd_window_register",
                  "register-rte": "amd_window_register_rte", "arena-rte": "amd_window_arena_rte"}
COMPARISON_ANCHORS = ("legacy", "compact64", "qualified32", "direct32", "rte32")
ACCELERATED_VARIANTS = {
    "fp8_gemm": set(GEMM_VARIANTS.values()),
    "window_attention": {"amd_window_optimized", "amd_window_small", *filter(None, WINDOW_LAYOUTS.values())},
    "ffn": {"amd_ffn32"}, "qkv_attention": {"amd_qkv32"},
    "expert_ffn": {"amd_expert_ffn"}, "c32_block": {"amd_block32"},
}


def read_json(path: Path) -> dict:
    if path.stat().st_size > 64 * 1024 * 1024:
        raise ValueError(f"JSON exceeds 64 MiB: {path}")
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError(f"duplicate JSON key: {key}")
            value[key] = item
        return value
    result = json.loads(path.read_text(encoding="utf-8-sig"), object_pairs_hook=unique,
                        parse_constant=lambda value: (_ for _ in ()).throw(ValueError(f"nonfinite JSON: {value}")))
    if not isinstance(result, dict):
        raise ValueError(f"JSON must be an object: {path}")
    return result


def write_json(path: Path, value: dict) -> None:
    with path.open("x", encoding="utf-8") as destination:
        json.dump(value, destination, indent=2, allow_nan=False)
        destination.write("\n")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def integer(value, label: str, minimum: int = 0) -> int:
    if type(value) is not int or value < minimum:
        raise ValueError(f"{label} must be an integer >= {minimum}")
    return value


def identity(value, model_required=True) -> dict:
    if not isinstance(value, dict):
        raise ValueError("identity must be an object")
    for key in IDENTITY_KEYS:
        if key == "model_sha256" and not model_required and key not in value:
            continue
        item = value.get(key)
        if not isinstance(item, str) or not item.strip():
            raise ValueError(f"identity lacks {key}")
        if key.endswith("sha256") and (len(item) != 64 or any(c not in "0123456789abcdef" for c in item)):
            raise ValueError(f"invalid lowercase SHA-256: {key}")
    return {key: value[key] for key in IDENTITY_KEYS if key in value}


def selected_policy(value, *, explicit_flags=False) -> dict:
    """Bind SPIR-V specialization constants as well as shader-file hashes."""
    if not isinstance(value, dict) or value.get("kernels") not in ("baseline", "optimized") or value.get("arithmetic") not in ("k16", "k32", "final"):
        raise ValueError("missing actual selected AMD kernel/arithmetic policy")
    result = {"window_queries": 64, "window_layout": "staged", "gemm": "shared", **value}
    if type(result["gemm"]) is not str or result["gemm"] not in GEMM_VARIANTS:
        raise ValueError("selected.gemm must be " + ", ".join(GEMM_VARIANTS))
    for key in ("tile_n", "stage_k", "window_queries"):
        if type(result.get(key)) is not int or result[key] not in (16, 32, 64):
            raise ValueError(f"selected.{key} must be 16, 32 or 64")
    if result["gemm"] in DIRECT_GEMMS and result["stage_k"] != 16:
        raise ValueError("direct GEMM requires stage_k=16")
    if result["gemm"] == "direct-rte-pair" and result["tile_n"] != 16:
        raise ValueError("direct-rte-pair currently requires tile_n=16")
    if type(result["window_layout"]) is not str or result["window_layout"] not in WINDOW_LAYOUTS:
        raise ValueError("selected.window_layout must be " + ", ".join(WINDOW_LAYOUTS))
    if result["window_layout"] != "staged" and (result["window_queries"] == 64 or result["kernels"] == "baseline"):
        raise ValueError("register attention requires optimized Q16/Q32")
    for key in LEGACY_FUSION_KEYS:
        if key not in result and not explicit_flags:
            result[key] = False
        if type(result.get(key)) is not bool:
            raise ValueError(f"selected.{key} must be explicitly Boolean")
    routes = ("ffn32_fusion", "qkv32_fusion")
    present = [key in result for key in routes]
    if any(present) != all(present):
        raise ValueError("selected independent fusion policy requires both routes")
    if not any(present):
        result.update({key: result["fusion"] for key in routes})
    if any(type(result[key]) is not bool for key in routes) or result["fusion"] != all(result[key] for key in routes):
        raise ValueError("selected independent fusion policy contradicts legacy summary")
    if result["gemm"] in SCALAR_RTE_GEMMS and result["hardware_publication"]:
        raise ValueError("scalar RTE GEMM requires packed hardware publication off")
    return {key: result[key] for key in SELECTION_KEYS}


def window_variant(policy: dict) -> str:
    """Name the actual module; new layouts never inherit a predecessor's name."""
    return WINDOW_LAYOUTS[policy["window_layout"]] or ("amd_window_optimized" if policy["window_queries"] == 64 else "amd_window_small")


def extended_operator_evidence(value: dict, reports: list[dict], field: str, marker: str,
                               cases: dict, sizes: dict, *, required: bool, label: str) -> dict:
    """Require shape metadata and full executed buffers for an extension claim."""
    evidence = value.get(field)
    coverage = value.get("coverage", [])
    if required and (marker not in coverage or not isinstance(evidence, dict)):
        raise ValueError(f"new operator proof requires {label} coverage")
    if evidence is None and marker not in coverage:
        return {}
    if (value["suite"] != "operators" or not isinstance(evidence, dict)
            or evidence.get("coverage_marker") != marker or marker not in coverage
            or type(evidence.get("case_count")) is not int or evidence["case_count"] != len(cases)
            or not isinstance(evidence.get("cases"), list) or len(evidence["cases"]) != len(cases)):
        raise ValueError(f"{label} execution metadata is incomplete")
    actual = {}
    for case in evidence["cases"]:
        if (not isinstance(case, dict) or type(case.get("name")) is not str
                or case["name"] not in cases or case["name"] in actual
                or any(type(case.get(key)) is not type(item) or case[key] != item
                       for key, item in cases[case["name"]].items())):
            raise ValueError(f"{label} case shape/flags/partition/batches mismatch")
        actual[case["name"]] = case
    if actual != cases:
        raise ValueError(f"{label} case coverage is incomplete")
    actual_pairs = {entry["name"]: entry for entry in reports}
    for name, size in sizes.items():
        pair = actual_pairs.get(name)
        if pair is None or pair["baseline_bytes"] != size or pair["candidate_bytes"] != size:
            raise ValueError(f"{label} executed output/dual tail allocation is missing or mismatched")
    return {field: evidence}


def requested_fusion_policy(args, candidate=True) -> dict:
    """Per-route selectors override --fusion; missing values inherit it."""
    legacy = bool(getattr(args, "fusion", False)) and candidate
    result = {key: bool(getattr(args, key, False)) and candidate for key in LEGACY_FUSION_KEYS}
    for key in ("ffn32_fusion", "qkv32_fusion"):
        override = getattr(args, key, None)
        if override is not None and type(override) is not bool:
            raise ValueError(f"{key} override must be Boolean or absent")
        result[key] = (legacy if override is None else override) and candidate
    result["fusion"] = result["ffn32_fusion"] and result["qkv32_fusion"]
    return result


def comparison_anchor(value="legacy") -> str:
    if value not in COMPARISON_ANCHORS:
        raise ValueError("comparison_anchor must be " + ", ".join(COMPARISON_ANCHORS))
    return value


def require_anchor(value: dict, requested: str) -> str:
    requested = comparison_anchor(requested)
    recorded = comparison_anchor(value.get("comparison_anchor", "legacy"))
    if recorded != requested:
        raise ValueError("recorded comparison_anchor differs from explicit --comparison-anchor")
    return recorded


def evidence_equal(first, second) -> bool:
    """Historical unlabeled evidence denotes legacy, never an optimized anchor."""
    def canonical(value):
        if isinstance(value, dict):
            if all(key in value for key in ("kernels", "arithmetic", "tile_n", "stage_k")):
                value = {**value, **selected_policy(value)}
            return {key: canonical(item) for key, item in value.items()
                    if key != "comparison_anchor" or item != "legacy"}
        if isinstance(value, list):
            return [canonical(item) for item in value]
        return value
    return canonical(first) == canonical(second)


def anchor_queries(anchor: str) -> int:
    return 32 if comparison_anchor(anchor) in ("qualified32", "direct32", "rte32") else 64


def anchor_gemm(anchor: str) -> str:
    anchor = comparison_anchor(anchor)
    return "direct-rte" if anchor == "rte32" else "direct" if anchor == "direct32" else "shared"


def anchor_window_layout(anchor: str) -> str:
    return "register-rte" if comparison_anchor(anchor) == "rte32" else "staged"


def preserving_baseline(selected: dict, anchor="legacy") -> bool:
    anchor = comparison_anchor(anchor)
    mode = "baseline" if anchor == "legacy" else "optimized"
    window_queries = anchor_queries(anchor)
    return (selected["kernels"] == mode and selected["arithmetic"] == "k16" and selected.get("gemm", "shared") == anchor_gemm(anchor)
            and selected["tile_n"] == 16 and selected["stage_k"] == 16 and selected["window_queries"] == window_queries
            and selected.get("window_layout", "staged") == anchor_window_layout(anchor)
            and not any(selected[key] for key in FUSION_KEYS))


def samples(value, count: int, label: str, *, allow_zero=False) -> list[float]:
    if not isinstance(value, list) or len(value) != count:
        raise ValueError(f"{label} must contain exactly {count} measured samples")
    if any(type(x) not in (int, float) or not math.isfinite(x) or x < 0 or (x == 0 and not allow_zero) for x in value):
        raise ValueError(f"{label} must contain finite {'nonnegative' if allow_zero else 'positive'} GPU milliseconds")
    return [float(x) for x in value]


def stats(values: list[float]) -> dict:
    ordered = sorted(values)
    if not ordered:
        raise ValueError("empty timing series")
    def percentile(fraction):
        at = (len(ordered) - 1) * fraction
        lo = int(at)
        return ordered[lo] + (ordered[min(lo + 1, len(ordered) - 1)] - ordered[lo]) * (at - lo)
    mean = statistics.fmean(ordered)
    deviation = statistics.pstdev(ordered)
    return {"samples": len(ordered), "mean": mean, "median": statistics.median(ordered),
            "p95": percentile(.95), "p99": percentile(.99), "min": ordered[0], "max": ordered[-1],
            "standard_deviation": deviation, "coefficient_of_variation": deviation / mean if mean else 0}


def shape(value) -> dict:
    if not isinstance(value, dict):
        raise ValueError("dispatch shape must be an object")
    for key in ("rows", "N", "K", "flags"):
        integer(value.get(key), f"shape.{key}")
    integer(value.get("batches"), "shape.batches", 1)
    partition = value.get("partition")
    if not ((type(partition) is int and partition >= 0) or (isinstance(partition, str) and partition.strip())):
        raise ValueError("shape.partition must be a nonnegative integer or nonempty string")
    return {key: value[key] for key in ("rows", "N", "K", "batches", "flags", "partition")}


def benchmark(path: Path, *, explicit_policy=False) -> dict:
    value = read_json(path)
    if value.get("format") != "OpenNR-amd-benchmark-v1" or value.get("command") not in ("bench", "profile"):
        raise ValueError("expected OpenNR-amd-benchmark-v1 bench/profile JSON")
    if value.get("readback") is not False or value.get("instrumented") is not (value["command"] == "profile"):
        raise ValueError("timing record must explicitly exclude image readback and identify profile instrumentation")
    value["identity"] = identity(value.get("identity"))
    for name in ("width", "height", "padded_width", "padded_height", "frames"):
        integer(value.get(name), name, 1)
    integer(value.get("warmup"), "warmup")
    if value["padded_width"] < value["width"] or value["padded_height"] < value["height"]:
        raise ValueError("padded geometry cannot be smaller than valid geometry")
    selected = value.get("selected")
    if not isinstance(selected, dict) or selected.get("kernels") not in ("baseline", "optimized") or selected.get("arithmetic") not in ("k16", "k32", "final"):
        raise ValueError("missing actual selected AMD kernel/arithmetic policy")
    if explicit_policy:
        if "window_queries" not in selected:
            raise ValueError("optimized anchors require explicitly recorded window_queries")
        selected_policy(selected, explicit_flags=True)
    for name in ("tile_n", "stage_k"):
        if type(selected.get(name)) is not int or selected[name] not in (16, 32, 64):
            raise ValueError(f"selected.{name} must be 16, 32 or 64")
    selected.setdefault("window_queries",64)
    if type(selected["window_queries"]) is not int or selected["window_queries"] not in (16,32,64):
        raise ValueError("selected.window_queries must be 16, 32 or 64")
    value["selected"] = selected_policy(selected, explicit_flags=explicit_policy)
    value["frame_ms"] = samples(value.get("frame_ms"), value["frames"], "frame_ms")
    if "dispatches" in value:
        if value["command"] != "profile" or not isinstance(value["dispatches"], list) or not value["dispatches"]:
            raise ValueError("dispatch samples require a nonempty profile dispatches array")
        for entry in value["dispatches"]:
            if not isinstance(entry, dict) or not isinstance(entry.get("family"), str) or not entry["family"].strip():
                raise ValueError("invalid dispatch family")
            entry["shape"] = shape(entry.get("shape"))
            if not isinstance(entry.get("variant"), str) or not entry["variant"].strip():
                raise ValueError("dispatch must name actual variant")
            for name in ("tile_n", "stage_k"):
                integer(entry.get(name), "dispatch." + name)
            compact_window = entry["variant"].startswith(("amd_window_small", "amd_window_register", "amd_window_arena"))
            if entry["family"]=="window_attention" or compact_window:
                geometry=entry.get("geometry",{})
                if not isinstance(geometry,dict):raise ValueError("window attention geometry must be an object")
                if compact_window and "tile_m" not in geometry:
                    raise ValueError("compact-window profile must record actual geometry.tile_m")
                queries=geometry.get("tile_m",64)
                if type(queries) is not int or queries not in (16,32,64):
                    raise ValueError("window attention geometry.tile_m must be 16, 32 or 64")
                if compact_window and queries not in (16,32):
                    raise ValueError("compact-window geometry.tile_m must be 16 or 32")
                if queries != value["selected"]["window_queries"]:
                    raise ValueError("window attention geometry.tile_m differs from selected.window_queries")
                if value["selected"]["window_layout"] == "arena-rte" and (
                        entry["variant"] != window_variant(value["selected"])
                        or any(type(geometry.get(key)) is not int or geometry[key] != item
                               for key, item in {"threads":128,"required_subgroup_size":32}.items())):
                    raise ValueError("arena attention dispatch module/resources differ from selected policy")
            if entry["family"] == "fp8_gemm" and value["selected"]["gemm"] in EXTENDED_GEMMS:
                policy = value["selected"]
                geometry = entry.get("geometry")
                required_geometry = {"threads":128, "required_subgroup_size":32, "tile_m":64}
                if (entry["variant"] != GEMM_VARIANTS[policy["gemm"]]
                        or entry["tile_n"] != policy["tile_n"] or entry["stage_k"] != policy["stage_k"]
                        or not isinstance(geometry, dict)
                        or any(type(geometry.get(key)) is not int or geometry[key] != item
                               for key, item in required_geometry.items())):
                    raise ValueError("new GEMM dispatch module/resources differ from selected policy")
            entry["frame_ms"] = samples(entry.get("frame_ms"), value["frames"], "dispatch.frame_ms", allow_zero=True)
    return value


def key_for(entry: dict) -> str:
    return json.dumps({"family": entry["family"], "shape": entry["shape"]}, sort_keys=True, separators=(",", ":"))


def operators(run: dict) -> dict:
    result = {}
    for entry in run.get("dispatches", []):
        key = key_for(entry)
        if key not in result:
            result[key] = {"family": entry["family"], "shape": entry["shape"],
                           "variants": set(), "frame_ms": [0.0] * run["frames"]}
        window=entry["family"]=="window_attention" or entry["variant"].startswith("amd_window_small")
        queries=entry.get("geometry",{}).get("tile_m",64) if window else 0
        result[key]["variants"].add((entry["variant"], entry["tile_n"], entry["stage_k"],queries))
        result[key]["frame_ms"] = [a + b for a, b in zip(result[key]["frame_ms"], entry["frame_ms"])]
    return result


def analyze(path: Path, allow_arithmetic_change=False, network_manifest: Path | None = None,
            comparison_anchor="legacy") -> dict:
    manifest = read_json(path)
    anchor = require_anchor(manifest, comparison_anchor)
    if manifest.get("format") != "OpenNR-amd-interleaved-v1" or not isinstance(manifest.get("runs"), list) or not manifest["runs"] or len(manifest["runs"]) % 2:
        raise ValueError("expected nonempty paired OpenNR-amd-interleaved-v1 runs")
    runs = []
    first = None
    role_identities = {}
    role_selections = {}
    source_hashes = []
    for index, item in enumerate(manifest["runs"]):
        role = "baseline" if index % 2 == 0 else "candidate"
        if not isinstance(item, dict) or item.get("role") != role or item.get("pair") != index // 2 or not isinstance(item.get("file"), str):
            raise ValueError("runs must be ordered baseline/candidate pairs with zero-based pair IDs")
        source = path.parent / item["file"]
        run = benchmark(source, explicit_policy=anchor != "legacy")
        if "comparison_anchor" in item:
            require_anchor(item, anchor)
        if "comparison_anchor" in run:
            require_anchor(run, anchor)
        if role == "baseline" and not preserving_baseline(
                selected_policy(run["selected"], explicit_flags=anchor != "legacy"), anchor):
            query_count = anchor_queries(anchor)
            raise ValueError(f"baseline must use the explicit {anchor} preserving N16/K16/Q{query_count} policy without overrides")
        if run["selected"]["arithmetic"] != "k16" and not allow_arithmetic_change:
            raise ValueError("changed arithmetic requires --allow-arithmetic-change")
        if first is None:
            first = run
        for field in ("command", "width", "height", "padded_width", "padded_height", "frames", "warmup"):
            if run[field] != first[field]:
                raise ValueError(f"benchmark pair mismatch: {field}")
        if any(run["identity"][key] != first["identity"][key] for key in COMMON_KEYS):
            raise ValueError("device, driver, model or frozen baseline shader identity mismatch")
        actual_identity = dict(run["identity"])
        actual_identity["executable_sha256"] = item.get("executable_sha256", run["identity"].get("executable_sha256"))
        selection = {k: run["selected"][k] for k in ("kernels", "arithmetic", "tile_n", "stage_k")}
        selection["gemm"] = run["selected"]["gemm"]
        selection["window_queries"]=run["selected"]["window_queries"]
        selection["window_layout"]=run["selected"]["window_layout"]
        selection.update({k: run["selected"].get(k, False) for k in FUSION_KEYS})
        if role == "baseline" and any(selection[k] for k in FUSION_KEYS):
            raise ValueError("baseline cannot enable fusion or hardware publication overrides")
        if role in role_identities and (actual_identity != role_identities[role] or selection != role_selections[role]):
            raise ValueError("identity or actual selection changed between repetitions")
        role_identities[role] = actual_identity
        role_selections[role] = selection
        source_hashes.append({"path": item["file"], "sha256": sha256(source)})
        runs.append(run)
    pairs = len(runs) // 2
    sufficient = pairs >= 3 and first["frames"] >= 30 and first["warmup"] >= 5
    baseline = stats([x for run in runs[::2] for x in run["frame_ms"]])
    candidate = stats([x for run in runs[1::2] for x in run["frame_ms"]])
    median_ratio, p95_ratio = candidate["median"] / baseline["median"], candidate["p95"] / baseline["p95"]
    per_pair = [{"pair": number, "baseline": stats(runs[2 * number]["frame_ms"]),
                 "candidate": stats(runs[2 * number + 1]["frame_ms"])} for number in range(pairs)]
    # Reject a pooled win concealing a >2% regression in any interleaved pair.
    network_safe = sufficient and not first["instrumented"] and median_ratio <= 1.02 and p95_ratio <= 1.02 and all(
        pair["candidate"]["median"] / pair["baseline"]["median"] <= 1.02 and
        pair["candidate"]["p95"] / pair["baseline"]["p95"] <= 1.02 for pair in per_pair)
    network_evidence = None
    if network_manifest is not None:
        if first["command"] != "profile":
            raise ValueError("--network-manifest supplies ordinary bench evidence for profile records only")
        network_evidence = analyze(network_manifest, allow_arithmetic_change, comparison_anchor=anchor)
        if network_evidence["command"] != "bench":
            raise ValueError("network evidence must use uninstrumented bench records")
        geometry = {k: first[k] for k in ("width", "height", "padded_width", "padded_height")}
        if network_evidence["geometry"] != geometry or network_evidence["identities"] != role_identities or network_evidence["selections"] != role_selections:
            raise ValueError("ordinary network evidence does not match profile identities, geometry and policy")
        median_ratio = network_evidence["network_median_ratio"]
        p95_ratio = network_evidence["network_p95_ratio"]
        network_safe = sufficient and network_evidence["network_regression_gate_passed"]
    grouped = [operators(run) for run in runs]
    all_keys = set().union(*(set(group) for group in grouped))
    operator_reports = []
    for key in sorted(all_keys):
        present = all(key in group for group in grouped)
        template = next(group[key] for group in grouped if key in group)
        entry = {"family": template["family"], "shape": template["shape"], "matched_all_runs": present,
                 "performance_eligible": False}
        if present:
            base_stats = stats([x for group in grouped[::2] for x in group[key]["frame_ms"]])
            cand_stats = stats([x for group in grouped[1::2] for x in group[key]["frame_ms"]])
            baseline_variants = set().union(*(group[key]["variants"] for group in grouped[::2]))
            variants = set().union(*(group[key]["variants"] for group in grouped[1::2]))
            improvement = 1 - cand_stats["median"] / base_stats["median"] if base_stats["median"] else 0
            entry.update(baseline_ms=base_stats, candidate_ms=cand_stats, improvement_fraction=improvement,
                         baseline_variants=[{"variant":v,"tile_n":n,"stage_k":k,**({"window_queries":q} if q else {})} for v,n,k,q in sorted(baseline_variants)],
                         candidate_variants=[{"variant":v,"tile_n":n,"stage_k":k,**({"window_queries":q} if q else {})} for v,n,k,q in sorted(variants)],
                         performance_eligible=network_safe and improvement >= .05 and len(variants) == 1 and variants != baseline_variants)
        operator_reports.append(entry)
    return {"format": "OpenNR-amd-performance-v1", "comparison_anchor": anchor, "source_manifest": str(path.resolve()),
            "source_sha256": sha256(path), "source_records": source_hashes,
            "command": first["command"], "instrumented": first["instrumented"], "pairs": pairs,
            "geometry": {k: first[k] for k in ("width", "height", "padded_width", "padded_height")},
            "identities": role_identities, "selections": role_selections, "baseline_ms": baseline,
            "candidate_ms": candidate, "interleaved_pairs": per_pair,
            "network_source_manifest": str(network_manifest.resolve()) if network_manifest is not None else None,
            "network_evidence": network_evidence,
            "network_median_ratio": median_ratio, "network_p95_ratio": p95_ratio,
            "protocol_complete": sufficient, "network_regression_gate_passed": network_safe,
            "default_performance_eligible": network_safe and median_ratio <= .95,
            "operators": operator_reports,
            "limitations": ["Network timestamps exclude D3D12 bridge, FSR and presentation; no game FPS is inferred.",
                            "Profile instrumentation timings are kept separate from ordinary benchmark timings.",
                            "Performance eligibility alone does not qualify arithmetic, temporal behavior or release readiness."]}


def collect(args) -> Path:
    anchor = comparison_anchor(getattr(args, "comparison_anchor", "legacy"))
    if anchor in ("direct32", "rte32") and (getattr(args, "baseline_executable", None) is None or
                                 getattr(args, "baseline_shaders", None) is None):
        release = "alpha 4" if anchor == "rte32" else "alpha 3"
        raise ValueError(f"{anchor} collection requires explicit --baseline-executable and --baseline-shaders; "
                         f"it is a preserving policy anchor, not an immutable {release} identity pin")
    if args.arithmetic != "k16" and not args.allow_arithmetic_change:
        raise ValueError("changed arithmetic requires --allow-arithmetic-change")
    requested_gemm = getattr(args, "gemm", "shared")
    if requested_gemm not in GEMM_VARIANTS or (requested_gemm in DIRECT_GEMMS and args.stage_k != 16):
        raise ValueError("GEMM must be " + "|".join(GEMM_VARIANTS) + "; direct requires stage_k=16")
    if requested_gemm == "direct-rte-pair" and args.tile_n != 16:
        raise ValueError("direct-rte-pair currently requires tile_n=16")
    requested_layout = getattr(args, "window_layout", "staged")
    if type(requested_layout) is not str or requested_layout not in WINDOW_LAYOUTS or (requested_layout != "staged" and
            (getattr(args,"window_queries",64) not in (16,32) or args.kernels == "baseline")):
        raise ValueError("register/arena attention requires optimized Q16/Q32 and a valid window layout")
    for name in ("width", "height", "frames", "pairs", "timeout"):
        integer(getattr(args, name), name, 1)
    integer(args.warmup, "warmup")
    executable = args.executable.resolve(strict=True)
    baseline_executable = (getattr(args, "baseline_executable", None) or executable).resolve(strict=True)
    model = args.model.resolve(strict=True)
    if not executable.is_file() or not baseline_executable.is_file() or not model.is_dir():
        raise ValueError("executable must be a file and model must be a directory")
    shaders = args.shaders.resolve(strict=True) if args.shaders else None
    baseline_shaders = getattr(args, "baseline_shaders", None)
    baseline_shaders = baseline_shaders.resolve(strict=True) if baseline_shaders else shaders
    if any(directory is not None and not directory.is_dir() for directory in (shaders, baseline_shaders)):
        raise ValueError("shader paths must be directories")
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    executable_hashes = {"baseline": sha256(baseline_executable), "candidate": sha256(executable)}
    manifest = {"format": "OpenNR-amd-interleaved-v1", "comparison_anchor": anchor, "created_utc": datetime.now(timezone.utc).isoformat(),
                "model_directory": str(model), "command": args.mode, "runs": []}
    if anchor in ("direct32", "rte32"):
        route = "Direct-RTE/K16/N16/stage16/Q32/Register-RTE" if anchor == "rte32" else "Direct/K16/N16/stage16/Q32/staged"
        release = "alpha 4" if anchor == "rte32" else "alpha 3"
        manifest["comparison_anchor_scope"] = (
            f"Preserving {route} policy with explicitly selected baseline artifacts. "
            f"Measured executable and loaded shader hashes bind these artifacts; an immutable {release} "
            "comparison additionally requires independent release-identity proof.")
        print(f"{anchor} is a preserving policy anchor; retain independent immutable release-hash proof "
              f"when claiming an {release} baseline", flush=True)
    child_base = {key: value for key, value in os.environ.items() if not key.startswith("DLSS5VK_")}
    for pair in range(args.pairs):
        for role in ("baseline", "candidate"):
            role_executable = baseline_executable if role == "baseline" else executable
            role_shaders = baseline_shaders if role == "baseline" else shaders
            if sha256(role_executable) != executable_hashes[role]:
                raise ValueError(f"{role} executable changed during collection")
            anchor_mode = "baseline" if anchor == "legacy" else "optimized"
            kernels, arithmetic, tile_n, stage_k = (anchor_mode, "k16", 16, 16) if role == "baseline" else (args.kernels, args.arithmetic, args.tile_n, args.stage_k)
            window_queries=anchor_queries(anchor) if role=="baseline" else getattr(args,"window_queries",64)
            window_layout=anchor_window_layout(anchor) if role=="baseline" else getattr(args,"window_layout","staged")
            gemm = anchor_gemm(anchor) if role == "baseline" else requested_gemm
            stem = f"pair-{pair + 1:02d}-{role}"
            json_path, log_path = output / (stem + ".json"), output / (stem + ".log")
            command = [str(role_executable), args.mode, "--backend", "amd", "--model", str(model),
                       "--width", str(args.width), "--height", str(args.height), "--frames", str(args.frames),
                       "--warmup", str(args.warmup), "--amd-kernels", kernels, "--amd-arithmetic", arithmetic,
                       "--amd-tile-n", str(tile_n), "--amd-stage-k", str(stage_k), "--json", str(json_path)]
            command.extend(["--amd-window-queries",str(window_queries)])
            command.extend(["--amd-window-layout",window_layout])
            command.extend(["--amd-gemm",gemm])
            if role_shaders:
                command.extend(["--shaders", str(role_shaders)])
            overrides = {"DLSS5VK_BACKEND": "amd", "DLSS5VK_CHAIN": "0", "DLSS5VK_VALIDATION": "0",
                         "DLSS5VK_DEBUG": "0", "DLSS5VK_AMD_KERNELS": kernels, "DLSS5VK_AMD_ARITHMETIC": arithmetic,
                         "DLSS5VK_AMD_TILE_N": str(tile_n), "DLSS5VK_AMD_STAGE_K": str(stage_k),
                         "DLSS5VK_AMD_WINDOW_QUERIES":str(window_queries),
                         "DLSS5VK_AMD_WINDOW_LAYOUT":window_layout,
                         "DLSS5VK_AMD_GEMM":gemm,
                         "DLSS5VK_PIPELINE_CACHE": str(output / "pipeline-cache")}
            requested_fusion = requested_fusion_policy(args, role == "candidate")
            overrides.update({"DLSS5VK_AMD_" + key.upper(): "1" if enabled else "0" for key,enabled in requested_fusion.items()})
            print(f"{stem}: {args.mode} {args.width}x{args.height}, anchor={anchor}, {kernels}/{arithmetic}/{gemm}, N{tile_n}/K{stage_k}/Q{window_queries}", flush=True)
            with log_path.open("xb") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, cwd=str(role_executable.parent),
                                        env={**child_base, **overrides}, timeout=args.timeout,
                                        creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            if result.returncode:
                raise ValueError(f"{stem} failed with exit {result.returncode}; see {log_path}")
            if sha256(role_executable) != executable_hashes[role]:
                raise ValueError(f"{role} executable changed during collection")
            run = benchmark(json_path, explicit_policy=anchor != "legacy")
            for field in ("width", "height", "warmup", "frames", "command"):
                expected = args.mode if field == "command" else getattr(args, field)
                if run[field] != expected:
                    raise ValueError(f"child ignored requested {field}")
            expected_policy = {"arithmetic": arithmetic, "tile_n": tile_n, "stage_k": stage_k}
            expected_policy["gemm"] = gemm
            expected_policy["window_queries"]=window_queries
            expected_policy["window_layout"]=window_layout
            expected_policy.update(requested_fusion)
            if kernels != "auto":
                expected_policy["kernels"] = kernels
            if any(run["selected"].get(k,False) != v for k, v in expected_policy.items()):
                raise ValueError("forced candidate selection was ignored or silently fell back")
            manifest["runs"].append({"role": role, "pair": pair, "comparison_anchor": anchor, "file": json_path.name, "log": log_path.name,
                                     "executable_sha256": executable_hashes[role], "argv": command, "environment": overrides})
    path = output / "interleaved.json"
    write_json(path, manifest)
    write_json(output / "performance.json", analyze(path, args.allow_arithmetic_change, comparison_anchor=anchor))
    return path


def exact_manifest(path: Path, comparison_anchor="legacy") -> dict:
    value = read_json(path)
    anchor = require_anchor(value, comparison_anchor)
    if value.get("format") != "OpenNR-amd-exact-manifest-v1" or value.get("suite") not in ("operators", "model320", "target"):
        raise ValueError("expected exact manifest with operators, model320 or target suite")
    model_free = value["suite"] == "operators" and value.get("model_free") is True
    ident = identity(value.get("identity"), model_required=not model_free)
    baseline_identity = identity(value.get("baseline_identity"), model_required=not model_free)
    # Model manifests export nested policies. The model-free operator runner
    # embeds the same actual execution fields inside its identity objects.
    selected_value = value.get("selected", value.get("identity") if model_free else None)
    baseline_value = value.get("baseline_selected", value.get("baseline_identity") if model_free else None)
    if anchor != "legacy" and any(not isinstance(policy, dict) or "window_queries" not in policy
                                     for policy in (selected_value, baseline_value)):
        raise ValueError("optimized anchors require explicitly recorded window_queries")
    selected = selected_policy(selected_value, explicit_flags=True)
    baseline_selected = selected_policy(baseline_value, explicit_flags=True)
    if not preserving_baseline(baseline_selected, anchor) or selected["kernels"] != "optimized" or selected["arithmetic"] != "k16":
        raise ValueError(f"exact qualification must compare the explicit {anchor} anchor with preserving optimized k16")
    common_keys = tuple(key for key in COMMON_KEYS if key != "model_sha256" or not model_free)
    if any(baseline_identity[key] != ident[key] for key in common_keys):
        raise ValueError("exact baseline/candidate identity differs")
    fixture_hash = None
    if model_free:
        fixture_hash = value.get("fixture_sha256")
        if not isinstance(fixture_hash, str) or len(fixture_hash) != 64 or any(c not in "0123456789abcdef" for c in fixture_hash):
            raise ValueError("model-free operator fixtures require a lowercase fixture_sha256")
        if any(baseline_identity[k] != ident[k] for k in ("device_id", "driver_id", "baseline_shader_sha256")):
            raise ValueError("operator baseline/candidate device, driver or frozen baseline identity differs")
    raw_overdispatch = value.get("raw_gemm_overdispatch")
    requires_raw = value["suite"] == "operators" and selected["gemm"] in EXTENDED_GEMMS
    if requires_raw and not isinstance(raw_overdispatch, dict):
        raise ValueError("new GEMM operator proof requires actual raw_gemm_overdispatch provenance")
    if "raw_gemm_overdispatch" in value:
        expected = {"variant": GEMM_VARIANTS[selected["gemm"]], "tile_n": selected["tile_n"],
                    "stage_k": 16, "publication_interval": 16, "required_subgroup_size": 32,
                    "dispatch_count": 3}
        if (value["suite"] != "operators" or selected["gemm"] not in DIRECT_GEMMS or not isinstance(raw_overdispatch, dict)
                or any(type(raw_overdispatch.get(key)) is not type(item) or raw_overdispatch[key] != item
                       for key, item in expected.items())):
            raise ValueError("raw GEMM overdispatch module/resources differ from selected policy")
    entries = value.get("pairs")
    if not isinstance(entries, list) or not entries:
        raise ValueError("exact manifest must list binary pairs")
    if model_free and (value.get("passed") is not True or value.get("validationErrors") != 0 or
                       value.get("mismatchedChecks") != 0 or value.get("checks") != len(entries) or
                       type(value.get("operators")) is not int or value["operators"] < 1):
        raise ValueError("model-free operator execution or Vulkan validation did not pass completely")
    fixture_root = Path(value["fixtureRoot"]) if isinstance(value.get("fixtureRoot"), str) else path.parent
    if isinstance(value.get("fixtureRoot"), str) and not fixture_root.is_absolute():
        fixture_root = path.parent / fixture_root
    names, reports = set(), []
    for entry in entries:
        if not isinstance(entry, dict) or not isinstance(entry.get("name"), str) or not entry["name"] or entry["name"] in names:
            raise ValueError("exact pairs require unique nonempty names")
        names.add(entry["name"])
        if not all(isinstance(entry.get(key), str) and entry[key] for key in ("baseline", "candidate")):
            raise ValueError("exact pair requires baseline and candidate binary paths")
        baseline, candidate = fixture_root / entry["baseline"], fixture_root / entry["candidate"]
        size_a, size_b = baseline.stat().st_size, candidate.stat().st_size
        if not size_a or not size_b:
            raise ValueError("empty exact-capture buffers cannot qualify")
        hash_a, hash_b = sha256(baseline), sha256(candidate)
        reports.append({"name": entry["name"], "baseline_sha256": hash_a, "candidate_sha256": hash_b,
                        "baseline_bytes": size_a, "candidate_bytes": size_b,
                        "exact": size_a == size_b and hash_a == hash_b})
    coverage = value.get("coverage", [])
    if not isinstance(coverage, list) or any(not isinstance(x, str) for x in coverage):
        raise ValueError("coverage must be an array of strings")
    if value["suite"] == "operators" and not EXACT_COVERAGE.issubset(coverage):
        raise ValueError("operator suite lacks prescribed mode/edge-case coverage")
    extended_vit = value.get("extended_vit")
    requires_vit = value["suite"] == "operators" and selected["gemm"] in EXTENDED_GEMMS
    if requires_vit and (VIT_COVERAGE not in coverage or not isinstance(extended_vit, dict)):
        raise ValueError("new GEMM operator proof requires extended ViT K1024/K4096 partition coverage")
    if value["suite"] == "operators" and (extended_vit is not None or VIT_COVERAGE in coverage):
        if (not isinstance(extended_vit, dict) or extended_vit.get("coverage_marker") != VIT_COVERAGE
                or type(extended_vit.get("case_count")) is not int or extended_vit["case_count"] != len(VIT_CASES)
                or not isinstance(extended_vit.get("cases"), list) or len(extended_vit["cases"]) != len(VIT_CASES)):
            raise ValueError("extended ViT execution metadata is incomplete")
        expected_cases = {}
        for K,N,rows,flags,partition,batches in VIT_CASES:
            name = f"gemm-vit-K{K}-N{N}-R{rows}-F{flags}-P{partition}-B{batches}"
            expected_cases[name] = {"name":name,"K":K,"N":N,"rows":rows,"flags":flags,"partition":partition,"batches":batches}
        actual_cases = {}
        for case in extended_vit["cases"]:
            if (not isinstance(case, dict) or type(case.get("name")) is not str or case["name"] not in expected_cases or case["name"] in actual_cases
                    or any(type(case.get(key)) is not type(item) or case[key] != item
                           for key,item in expected_cases[case["name"]].items())):
                raise ValueError("extended ViT case shape/flags/partition/batches mismatch")
            actual_cases[case["name"]] = case
        if actual_cases != expected_cases:
            raise ValueError("extended ViT case coverage is incomplete")
        actual_pairs = {entry["name"]:entry for entry in reports}
        for name,case in expected_cases.items():
            allocation = ((case["rows"] + 63) // 64) * 64 * (32 + case["batches"] * case["N"])
            sizes = {name:allocation * (1 if case["flags"] & 16 else 2)}
            if case["flags"] & 32: sizes[name+"-dual"] = allocation
            for buffer_name,size in sizes.items():
                pair = actual_pairs.get(buffer_name)
                if pair is None or pair["baseline_bytes"] != size or pair["candidate_bytes"] != size:
                    raise ValueError("extended ViT executed output/dual tail allocation is missing or mismatched")
    if raw_overdispatch is not None:
        actual_pairs = {entry["name"]:entry for entry in reports}
        # Real raw fixtures: 73 valid rows padded to 128, N48; variants 3/5
        # have three output batches. Compare full allocations, including guards.
        for name,size in (("gemm-overdispatch-v0",10240), ("gemm-overdispatch-v3",22528),
                          ("gemm-overdispatch-v5",45056), ("gemm-overdispatch-v5-dual",22528)):
            pair = actual_pairs.get(name)
            if pair is None or pair["baseline_bytes"] != size or pair["candidate_bytes"] != size:
                raise ValueError("raw GEMM overdispatch executed output/dual tail allocation is missing or mismatched")
    if value["suite"] == "model320" and (value.get("resolution") != [320, 320] or names != MODEL_CHECKPOINTS | {"head", "capture-production"}):
        raise ValueError("model320 suite requires 75 checkpoints plus head at 320x320")
    if value["suite"] == "target" and (value.get("resolution") != [1707, 960] or not {"head", "capture-production"}.issubset(names)):
        raise ValueError("target suite requires head and capture/production equality at valid resolution 1707x960")
    raw_evidence = {"raw_gemm_overdispatch": raw_overdispatch} if raw_overdispatch is not None else {}
    if extended_vit is not None: raw_evidence["extended_vit"] = extended_vit
    paired_cases, paired_sizes = {}, {}
    for K,N,rows,flags,partition,batches in PAIRED_CASES:
        name = f"gemm-paired-K{K}-N{N}-R{rows}-F{flags}-P{partition}-B{batches}"
        paired_cases[name] = dict(name=name,K=K,N=N,rows=rows,flags=flags,partition=partition,batches=batches)
        allocation = ((rows+63)//64)*64*(32+batches*N)
        paired_sizes[name] = allocation*(1 if flags & 16 else 2)
        if flags & 32: paired_sizes[name+"-dual"] = allocation
    raw_evidence.update(extended_operator_evidence(value,reports,"extended_paired",PAIRED_COVERAGE,
        paired_cases,paired_sizes,required=value["suite"]=="operators" and selected["gemm"] in EXTENDED_GEMMS,
        label="paired-preload K160/K544/batch8"))
    window_cases, window_sizes = {}, {}
    for width,height,heads,sx,sy in WINDOW_PADDING_CASES:
        name = f"window-{width}x{height}-H{heads}-S{sx}x{sy}"
        window_cases[name] = dict(name=name,width=width,height=height,heads=heads,shiftX=sx,shiftY=sy)
        window_sizes[name] = ((width*height+63)//64)*64*heads*32
    raw_evidence.update(extended_operator_evidence(value,reports,"extended_window_padding",WINDOW_PADDING_COVERAGE,
        window_cases,window_sizes,required=value["suite"]=="operators" and selected["window_layout"]=="arena-rte",
        label="thin-window padding"))
    return {**raw_evidence, "suite": value["suite"], "comparison_anchor": anchor, "source_manifest": str(path.resolve()), "source_sha256": sha256(path), "identity": ident,
            "model_free": model_free, "fixture_sha256": fixture_hash, "baseline_identity": baseline_identity,
            "selected": selected, "baseline_selected": baseline_selected,
            "coverage": sorted(set(coverage)), "pairs": reports, "passed": all(x["exact"] for x in reports)}


def qualify(args) -> dict:
    anchor = comparison_anchor(getattr(args, "comparison_anchor", "legacy"))
    # Import only for quality assessment; benchmark/tuning collection uses stdlib.
    module_spec = importlib.util.spec_from_file_location("compare_images", Path(__file__).with_name("compare_images.py"))
    compare_images = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(compare_images)
    ident = None
    selected = baseline_selected = baseline_ident = None
    exact_reports = [exact_manifest(path, anchor) for path in args.exact]
    if len({x["suite"] for x in exact_reports}) != len(exact_reports):
        raise ValueError("qualification cannot contain duplicate exact suites")
    if args.arithmetic == "k16" and {x["suite"] for x in exact_reports} != {"operators", "model320", "target"}:
        raise ValueError("preserving qualification requires operators, model320 and target exact suites")
    quality_reports = []
    coverage, histories = set(), set()
    for exact in exact_reports:
        if selected is None:
            selected, baseline_selected = exact["selected"], exact["baseline_selected"]
            baseline_ident = exact["baseline_identity"]
        if exact["selected"] != selected or exact["baseline_selected"] != baseline_selected:
            raise ValueError("qualification exact-suite selected policy mismatch")
        if any(exact["baseline_identity"][key] != baseline_ident[key] for key in IDENTITY_KEYS
               if key != "model_sha256"):
            raise ValueError("qualification exact-suite baseline identity mismatch")
        if exact["model_free"]:
            continue
        if ident is None:
            ident = exact["identity"]
        if exact["identity"] != ident:
            raise ValueError("qualification exact-suite identity mismatch")
    for path in args.sequence:
        manifest = read_json(path)
        record_identity = identity(manifest.get("identity"))
        record_selected = selected_policy(manifest.get("selected"), explicit_flags=True)
        if record_selected["arithmetic"] != args.arithmetic:
            raise ValueError("quality sequence actual arithmetic differs from requested qualification")
        if selected is None:
            selected = record_selected
        if record_selected != selected:
            raise ValueError("qualification sequence selected policy mismatch")
        if ident is None:
            ident = record_identity
        if record_identity != ident:
            raise ValueError("qualification sequence identity mismatch")
        if manifest.get("data_range") != 1.0 or manifest.get("history_mode") not in ("identical", "evolved"):
            raise ValueError("quality requires fixed data_range 1.0 and identical/evolved history mode")
        if not isinstance(manifest.get("coverage"), list) or any(not isinstance(x, str) for x in manifest["coverage"]):
            raise ValueError("quality sequence must name scene coverage")
        frames = manifest.get("frames")
        if not isinstance(frames, list) or not frames:
            raise ValueError("quality sequence requires nonempty frames")
        for frame in frames:
            if not isinstance(frame, dict) or not all(isinstance(frame.get(name), str) and frame[name] for name in ("reference", "candidate")):
                raise ValueError("quality frame requires reference and candidate paths")
            metadata = compare_images.verify_metadata(frame.get("metadata"))
            if metadata["model_sha256"] != ident["model_sha256"]:
                raise ValueError("quality capture model does not match qualification identity")
            if metadata["color_space"] != "scene-linear" or metadata["pipeline_point"] != "pre-fsr":
                raise ValueError("quality requires scene-linear pre-FSR composed RGB")
            for name in ("reference", "candidate"):
                with (path.parent / frame[name]).open("rb") as image:
                    if image.readline().strip() != b"PF":
                        raise ValueError("scene-linear qualification requires unclamped RGB float PFM")
        report = compare_images.compare_sequence(path, min_psnr=40, min_ssim=.99)
        quality_reports.append({"source_manifest": str(path.resolve()), "source_sha256": sha256(path), "history_mode": manifest["history_mode"],
                                "coverage": manifest["coverage"], "result": report})
        coverage.update(manifest["coverage"])
        histories.add(manifest["history_mode"])
    complete_scenes = SCENE_COVERAGE.issubset(coverage) and histories == {"identical", "evolved"}
    if ident is None or (args.arithmetic != "k16" and not complete_scenes):
        raise ValueError("experimental qualification requires all prescribed scenes and both history modes")
    if any(any(x["identity"][key] != ident[key] for key in IDENTITY_KEYS if key != "model_sha256" or not x["model_free"]) for x in exact_reports):
        raise ValueError("exact/scene identity mismatch")
    preserving = args.arithmetic == "k16" and all(x["passed"] for x in exact_reports)
    scene_quality = complete_scenes and all(x["result"]["thresholds_passed"] for x in quality_reports)
    return {"format": "OpenNR-amd-qualification-v1", "comparison_anchor": anchor, "identity": ident, "arithmetic": args.arithmetic,
            "selections": {"baseline": baseline_selected, "candidate": selected},
            "exact_suites": exact_reports, "quality_sequences": quality_reports,
            "passed": preserving if args.arithmetic == "k16" else scene_quality,
            "preservationQualified": preserving,
            "releaseGates": {"scene_quality": scene_quality, "motion_review": False, "complete_game_benchmarks": False},
            "limitations": ["Numerical gates do not replace human motion review or complete game benchmark validation."]}


def tuning(performance_path: Path, qualification_path: Path, comparison_anchor="legacy") -> dict:
    report, qualification = read_json(performance_path), read_json(qualification_path)
    anchor = require_anchor(report, comparison_anchor)
    require_anchor(qualification, anchor)
    if report.get("format") != "OpenNR-amd-performance-v1" or qualification.get("format") != "OpenNR-amd-qualification-v1":
        raise ValueError("expected generated performance and qualification reports")
    # Evidence and gates are recomputed from raw files, rather than trusting a
    # manually edited eligibility boolean or a summary of unrelated minima.
    network_source = Path(report["network_source_manifest"]) if report.get("network_source_manifest") else None
    if not isinstance(report.get("source_manifest"), str) or not evidence_equal(
            analyze(Path(report["source_manifest"]), network_manifest=network_source, comparison_anchor=anchor), report):
        raise ValueError("performance report no longer agrees with its raw interleaved samples")
    exact_sources = [Path(x["source_manifest"]) for x in qualification.get("exact_suites", [])]
    quality_sources = [Path(x["source_manifest"]) for x in qualification.get("quality_sequences", [])]
    fresh_qualification = qualify(argparse.Namespace(exact=exact_sources, sequence=quality_sources, arithmetic="k16", comparison_anchor=anchor))
    if not evidence_equal(fresh_qualification, qualification):
        raise ValueError("qualification report no longer agrees with its binary/quality capture artifacts")
    candidate_identity = identity(report.get("identities", {}).get("candidate"))
    if identity(qualification.get("identity")) != candidate_identity:
        raise ValueError("qualification identity does not match measured candidate")
    if not evidence_equal(qualification.get("selections"), report.get("selections")):
        raise ValueError("qualification selected policy does not match measured baseline/candidate")
    baseline_identity = identity(report.get("identities", {}).get("baseline"))
    for exact in qualification.get("exact_suites", []):
        if any(exact["baseline_identity"][key] != baseline_identity[key] for key in IDENTITY_KEYS
               if key != "model_sha256" or not exact["model_free"]):
            raise ValueError("qualification baseline identity does not match measured baseline")
    if report.get("selections", {}).get("candidate", {}).get("arithmetic") != "k16" or qualification.get("arithmetic") != "k16":
        raise ValueError("automatic tuning only accepts preserving k16 arithmetic")
    exact = qualification.get("exact_suites", [])
    qualified = (qualification.get("passed") is True and qualification.get("preservationQualified") is True
                 and {x.get("suite") for x in exact} == {"operators", "model320", "target"}
                 and all(x.get("passed") is True for x in exact))
    records = []
    for operator in report.get("operators", []):
        variants = operator.get("candidate_variants", [])
        if not qualified or operator.get("performance_eligible") is not True or len(variants) != 1:
            continue
        actual_shape = shape(operator.get("shape"))
        # The native session loader uses uint32 graph shape IDs. Named
        # partitions remain useful in diagnostic reports, but cannot be
        # represented by its current tuning contract.
        if any(type(actual_shape[key]) is not int or not 0 <= actual_shape[key] <= 0xffffffff
               for key in ("rows", "N", "K", "batches", "flags", "partition")):
            continue
        variant = variants[0]
        if variant.get("variant") not in ACCELERATED_VARIANTS.get(operator["family"], set()):
            continue
        if operator["family"] == "fp8_gemm" and variant["variant"] != GEMM_VARIANTS[selected_policy(report["selections"]["candidate"])["gemm"]]:
            raise ValueError("GEMM dispatch variant does not match its qualified session policy")
        if operator["family"] == "window_attention":
            policy = selected_policy(report["selections"]["candidate"])
            expected_window = window_variant(policy)
            if variant["variant"] != expected_window:
                raise ValueError("attention dispatch variant does not match its qualified session policy")
            if type(variant.get("window_queries")) is not int or variant["window_queries"] != policy["window_queries"]:
                raise ValueError("attention dispatch query geometry does not match its qualified session policy")
        if type(variant.get("tile_n")) is not int or variant["tile_n"] not in (16, 32, 64) or type(variant.get("stage_k")) is not int or variant["stage_k"] not in (16, 32, 64):
            continue
        records.append({"key": {**candidate_identity, "arithmetic": "k16", "family": operator["family"], "shape": actual_shape},
                        "comparison_anchor": anchor,
                        **variant, "qualified": True, "operator_improvement_fraction": operator["improvement_fraction"],
                        "network_median_ratio": report["network_median_ratio"], "network_p95_ratio": report["network_p95_ratio"],
                        "evidence": {"performance_report_sha256": sha256(performance_path),
                                     "qualification_report_sha256": sha256(qualification_path),
                                     "selected": selected_policy(report["selections"]["candidate"], explicit_flags=True)}})
    return {"format": "OpenNR-amd-tuning-v1", "comparison_anchor": anchor, "records": records,
            "identity": candidate_identity, "geometry": report["geometry"],
            "optimized_default_eligible": bool(records) and qualified and report.get("default_performance_eligible") is True,
            "default_selection": report["selections"]["candidate"],
            "fallback": "qualified preserving baseline; invalid/missing records never select experimental arithmetic"}


def merge_candidates(candidates: list[list[Path]], comparison_anchor="legacy") -> dict:
    """Choose per-shape winners from reverified evidence, never promote a mix."""
    winners = {};common_identity = None;common_geometry = None;mixed_identity=False;mixed_geometry=False
    for performance, qualification in candidates:
        candidate = tuning(performance, qualification, comparison_anchor)
        if common_identity is None:
            common_identity=candidate.get("identity")
            common_geometry=candidate.get("geometry")
        else:
            mixed_identity=mixed_identity or candidate.get("identity")!=common_identity
            mixed_geometry=mixed_geometry or candidate.get("geometry")!=common_geometry
        for record in candidate["records"]:
            key = json.dumps(record["key"], sort_keys=True, separators=(",", ":"))
            rank = (-record["operator_improvement_fraction"], record["network_median_ratio"],
                    record["network_p95_ratio"], record["tile_n"], record["stage_k"],record.get("window_queries",64), record["variant"])
            if key not in winners or rank < winners[key][0]:
                winners[key] = rank, record
    return {"format": "OpenNR-amd-tuning-v1", "comparison_anchor": comparison_anchor, "records": [winners[key][1] for key in sorted(winners)],
            "identity": None if mixed_identity else common_identity,
            "geometry": None if mixed_geometry else common_geometry,
            "optimized_default_eligible": False,
            "default_selection": None,
            "fallback": "qualified preserving baseline; invalid/missing records never select experimental arithmetic",
            "limitations": ["A combination of per-shape winners requires its own complete-network benchmark before default promotion."]}


def captured_sequence(directory: Path, allow_nongame=False) -> list[tuple[Path, dict]]:
    """Inspect only atomically published frames; incomplete staging is ignored."""
    frames=[]
    for frame_directory in directory.iterdir():
        manifest=frame_directory/"manifest.json"
        if frame_directory.is_dir() and not frame_directory.name.startswith(".") and manifest.is_file():
            frame=read_json(manifest)
            if frame.get("format")!="OpenNR-game-capture-v1":
                raise ValueError("sequence includes an unsupported capture manifest")
            if frame.get("gameCapture") is not True and not allow_nongame:
                raise ValueError("game sequence requires genuine gameCapture metadata; synthetic diagnostics need --allow-nongame")
            if not isinstance(frame.get("sequence_id"),str) or not frame["sequence_id"]:
                raise ValueError("sequence replay requires bounded-capture sequence metadata")
            integer(frame.get("frame_id"),"captured frame_id")
            integer(frame.get("capture_ordinal"),"capture_ordinal")
            frames.append((frame_directory,frame))
    if not frames or len(frames)>120:
        raise ValueError("sequence must contain 1 through 120 completed capture frames")
    frames.sort(key=lambda item:item[1]["capture_ordinal"])
    first=frames[0][1]
    requested=integer(first.get("requested_capture_count"),"requested_capture_count",1)
    if requested>120 or len(frames)!=requested:
        raise ValueError("bounded sequence is incomplete; wait for every requested capture or record a new sequence")
    if first["capture_ordinal"]!=0:
        raise ValueError("bounded sequence is missing its first capture")
    for index,(_,frame) in enumerate(frames):
        if frame["sequence_id"]!=first["sequence_id"] or frame["capture_ordinal"]!=index:
            raise ValueError("sequence identity or capture ordinal has a gap")
        if frame.get("requested_capture_count")!=requested:
            raise ValueError("capture request count changed within sequence")
        if (frame.get("width"),frame.get("height"),frame.get("modelManifestSha256"))!=(first.get("width"),first.get("height"),first.get("modelManifestSha256")):
            raise ValueError("capture geometry/model changed within bounded sequence")
        if index and frame["frame_id"]!=frames[index-1][1]["frame_id"]+1 and frame.get("reset") is not True:
            raise ValueError("capture frame gap requires a recorded history reset")
    return frames


def replay_sequence(args) -> dict:
    if args.arithmetic!="k16" and not args.allow_arithmetic_change:
        raise ValueError("experimental replay requires --allow-arithmetic-change")
    requested_gemm=getattr(args,"gemm","shared")
    if requested_gemm not in GEMM_VARIANTS or (requested_gemm in DIRECT_GEMMS and args.stage_k!=16):
        raise ValueError("GEMM must be " + "|".join(GEMM_VARIANTS) + "; direct requires stage_k=16")
    if requested_gemm == "direct-rte-pair" and args.tile_n != 16:
        raise ValueError("direct-rte-pair currently requires tile_n=16")
    requested_layout = getattr(args, "window_layout", "staged")
    if type(requested_layout) is not str or requested_layout not in WINDOW_LAYOUTS or (requested_layout != "staged" and
            (getattr(args,"window_queries",64) not in (16,32) or args.kernels == "baseline")):
        raise ValueError("register/arena attention requires optimized Q16/Q32 and a valid window layout")
    frames=captured_sequence(args.capture_sequence.resolve(strict=True),args.allow_nongame)
    executable=args.executable.resolve(strict=True);model=args.model.resolve(strict=True)
    args.output.mkdir(parents=True,exist_ok=False);output=args.output.resolve()
    module_spec=importlib.util.spec_from_file_location("compare_images",Path(__file__).with_name("compare_images.py"))
    images=importlib.util.module_from_spec(module_spec);module_spec.loader.exec_module(images)
    modes=("identical","evolved") if args.history_mode=="both" else (args.history_mode,)
    parent_environment={key:value for key,value in os.environ.items() if not key.startswith("DLSS5VK_")}
    reports=[]
    for history_mode in modes:
        quality={"format":"OpenNR-quality-sequence-v1","data_range":1.0,"history_mode":history_mode,
                 "coverage":sorted(set(args.coverage)),"frames":[]}
        previous_replay={};candidate_identity=None;candidate_selection=None
        for index,(capture,frame) in enumerate(frames):
            metadata=[];destinations={}
            for role in ("reference","candidate"):
                backend=args.reference_backend if role=="reference" else "amd"
                destination=output/history_mode/role/("frame-"+str(frame["frame_id"]))
                destination.parent.mkdir(parents=True,exist_ok=True)
                command=[str(executable),"compositecheck","--backend",backend,"--model",str(model),
                         "--fixture",str(destination),"--recorded-frame",str(capture),"--history-mode",history_mode]
                if args.shaders:command.extend(["--shaders",str(args.shaders.resolve(strict=True))])
                if args.game_shaders:command.extend(["--game-shaders",str(args.game_shaders.resolve(strict=True))])
                if history_mode=="evolved" and role in previous_replay and (not index or frame["frame_id"]==frames[index-1][1]["frame_id"]+1):
                    command.extend(["--previous-replay",str(previous_replay[role])])
                candidate=role=="candidate"
                overrides={"DLSS5VK_BACKEND":backend,"DLSS5VK_CHAIN":"0","DLSS5VK_VALIDATION":"0",
                           "DLSS5VK_AMD_KERNELS":args.kernels if candidate else "baseline",
                           "DLSS5VK_AMD_ARITHMETIC":args.arithmetic if candidate else "k16",
                           "DLSS5VK_AMD_GEMM":getattr(args,"gemm","shared") if candidate else "shared",
                           "DLSS5VK_AMD_TILE_N":str(args.tile_n if candidate else 16),
                           "DLSS5VK_AMD_STAGE_K":str(args.stage_k if candidate else 16),
                           "DLSS5VK_AMD_WINDOW_QUERIES":str(getattr(args,"window_queries",64) if candidate else 64),
                           "DLSS5VK_AMD_WINDOW_LAYOUT":getattr(args,"window_layout","staged") if candidate else "staged"}
                requested_fusion = requested_fusion_policy(args, candidate)
                for flag,enabled in requested_fusion.items():
                    overrides["DLSS5VK_AMD_"+flag.upper()]="1" if enabled else "0"
                print(f"replay {history_mode} frame {index+1}/{len(frames)} {role}",flush=True)
                log_path=destination.parent/(destination.name+".log")
                with log_path.open("xb") as log:
                    result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env={**parent_environment,**overrides},
                                          cwd=str(executable.parent),timeout=args.timeout,
                                          creationflags=subprocess.CREATE_NO_WINDOW if os.name=="nt" else 0)
                if result.returncode:raise ValueError(f"replay failed with exit {result.returncode}; see {log_path}")
                replay=read_json(destination/"manifest.json")
                if replay.get("sourceFrameId")!=frame["frame_id"] or replay.get("historyMode")!=history_mode:
                    raise ValueError("replay did not report the requested frame/history mode")
                previous_replay[role]=destination;destinations[role]=destination
                actual={"frame_id":frame["frame_id"],"sequence_id":frame["sequence_id"],"seed":replay["seed"],
                        "model_sha256":frame["modelManifestSha256"].lower(),
                        "input_sha256":frame["filesSha256"]["source-packed.f32"].lower(),"controls":frame["controls"],
                        "render_resolution":[frame["width"],frame["height"]],"output_resolution":[frame["width"],frame["height"]],
                        "pipeline_point":"pre-fsr","color_space":"scene-linear","pre_exposure":frame["pre_exposure"],
                        "exposure_scale":frame["exposure_scale"],"jitter":frame["jitter"],"motion_scale":frame["motion_scale"],
                        "reset":replay["reset"],"history_frame_ids":replay["historyFrameIds"]}
                metadata.append(actual)
                if candidate:
                    current_identity=identity(replay["identity"])
                    current_selection=selected_policy(replay.get("selected"), explicit_flags=True)
                    if candidate_identity is not None and current_identity!=candidate_identity:
                        raise ValueError("candidate device/driver/model/shader identity changed during replay")
                    if candidate_selection is not None and current_selection!=candidate_selection:
                        raise ValueError("candidate selected policy changed during replay")
                    if (current_selection["arithmetic"]!=args.arithmetic or current_selection["gemm"]!=getattr(args,"gemm","shared") or current_selection["tile_n"]!=args.tile_n
                            or current_selection["stage_k"]!=args.stage_k or current_selection["window_queries"]!=getattr(args,"window_queries",64)
                            or current_selection["window_layout"]!=getattr(args,"window_layout","staged")
                            or (args.kernels!="auto" and current_selection["kernels"]!=args.kernels)
                            or any(current_selection[flag]!=requested_fusion[flag] for flag in FUSION_KEYS)):
                        raise ValueError("forced candidate replay selection was ignored or silently fell back")
                    candidate_identity=current_identity
                    candidate_selection=current_selection
            images.verify_metadata({"reference":metadata[0],"candidate":metadata[1]})
            quality["frames"].append({"reference":str((destinations["reference"]/"recorded-scene-linear-rgb.pfm").relative_to(output)),
                                      "candidate":str((destinations["candidate"]/"recorded-scene-linear-rgb.pfm").relative_to(output)),
                                      "metadata":{"reference":metadata[0],"candidate":metadata[1]}})
        quality["identity"]=candidate_identity
        quality["selected"]=candidate_selection
        manifest_path=output/(history_mode+"-sequence.json");write_json(manifest_path,quality)
        assessment=images.compare_sequence(manifest_path,min_psnr=40,min_ssim=.99)
        write_json(output/(history_mode+"-quality.json"),assessment)
        reports.append({"history_mode":history_mode,"manifest":manifest_path.name,"quality":assessment})
    result={"format":"OpenNR-amd-sequence-replay-v1","frame_count":len(frames),
            "genuine_game_capture":all(frame.get("gameCapture") is True for _,frame in frames),
            "performance_representative":False,"sequences":reports,
            "thresholds_passed":all(x["quality"]["thresholds_passed"] for x in reports),
            "limitations":["Offline replay includes diagnostic readback and cannot measure game FPS.",
                           "Evolved reference/candidate histories each start from reset; identical mode reuses captured history.",
                           "Scene labels require reviewer verification; numerical passing does not establish motion acceptance."]}
    write_json(output/"replay-report.json",result);return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="action", required=True)
    collect_parser = commands.add_parser("collect", help="launch interleaved GPU network runs")
    collect_parser.add_argument("--executable", type=Path, required=True)
    collect_parser.add_argument("--baseline-executable", type=Path, help="frozen baseline tool; defaults to --executable")
    collect_parser.add_argument("--model", type=Path, required=True)
    collect_parser.add_argument("--shaders", type=Path)
    collect_parser.add_argument("--baseline-shaders", type=Path, help="frozen baseline modules; defaults to --shaders")
    collect_parser.add_argument("--output", type=Path, required=True, help="new, absent output directory")
    collect_parser.add_argument("--mode", choices=("bench", "profile"), default="bench")
    collect_parser.add_argument("--width", type=int, default=1707)
    collect_parser.add_argument("--height", type=int, default=960)
    collect_parser.add_argument("--warmup", type=int, default=5)
    collect_parser.add_argument("--frames", type=int, default=30)
    collect_parser.add_argument("--pairs", type=int, default=3)
    collect_parser.add_argument("--timeout", type=int, default=900, help="per child timeout in seconds")
    collect_parser.add_argument("--kernels", choices=("auto", "baseline", "optimized"), default="optimized")
    collect_parser.add_argument("--arithmetic", choices=("k16", "k32", "final"), default="k16")
    collect_parser.add_argument("--gemm", choices=tuple(GEMM_VARIANTS), default="shared")
    collect_parser.add_argument("--tile-n", type=int, choices=(16, 32, 64), default=16)
    collect_parser.add_argument("--stage-k", type=int, choices=(16, 32, 64), default=16)
    collect_parser.add_argument("--window-queries",type=int,choices=(16,32,64),default=64)
    collect_parser.add_argument("--window-layout",choices=tuple(WINDOW_LAYOUTS),default="staged")
    collect_parser.add_argument("--allow-arithmetic-change", action="store_true")
    for flag in LEGACY_FUSION_KEYS:
        collect_parser.add_argument("--" + flag.replace("_","-"),action="store_true")
    for flag in ("ffn32_fusion", "qkv32_fusion"):
        collect_parser.add_argument("--" + flag.replace("_","-"),action=argparse.BooleanOptionalAction,default=None)
    analyze_parser = commands.add_parser("analyze", help="validate and assess collected paired records")
    analyze_parser.add_argument("manifest", type=Path)
    analyze_parser.add_argument("--output", type=Path, required=True)
    analyze_parser.add_argument("--allow-arithmetic-change", action="store_true")
    analyze_parser.add_argument("--network-manifest", type=Path, help="matching ordinary bench run manifest for profile promotion gates")
    qualify_parser = commands.add_parser("qualify", help="verify actual binary and scene capture artifacts")
    qualify_parser.add_argument("--exact", type=Path, action="append", default=[])
    qualify_parser.add_argument("--sequence", type=Path, action="append", default=[])
    qualify_parser.add_argument("--arithmetic", choices=("k16", "k32", "final"), default="k16")
    qualify_parser.add_argument("--output", type=Path, required=True)
    tuning_parser = commands.add_parser("tuning", help="export qualified preserving per-shape tuning records")
    tuning_parser.add_argument("--performance", type=Path, required=True)
    tuning_parser.add_argument("--qualification", type=Path, required=True)
    tuning_parser.add_argument("--output", type=Path, required=True)
    merge_parser = commands.add_parser("merge", help="choose per-shape winners across qualified tile/staging experiments")
    merge_parser.add_argument("--candidate", type=Path, nargs=2, action="append", required=True,
                              metavar=("PERFORMANCE", "QUALIFICATION"))
    merge_parser.add_argument("--output", type=Path, required=True)
    for anchor_parser in (collect_parser, analyze_parser, qualify_parser, tuning_parser, merge_parser):
        anchor_parser.add_argument("--comparison-anchor", choices=COMPARISON_ANCHORS, default="legacy",
                                   help="preserving policy anchor: legacy, shared compact64/qualified32, direct32, or rte32 "
                                        "(collection needs explicit baseline tool/shaders; immutable release hashes are verified separately)")
    replay_parser=commands.add_parser("replay",help="replay a bounded scene sequence with identical and evolved histories")
    replay_parser.add_argument("--capture-sequence",type=Path,required=True)
    replay_parser.add_argument("--executable",type=Path,required=True)
    replay_parser.add_argument("--model",type=Path,required=True)
    replay_parser.add_argument("--shaders",type=Path)
    replay_parser.add_argument("--game-shaders",type=Path)
    replay_parser.add_argument("--output",type=Path,required=True)
    replay_parser.add_argument("--reference-backend",choices=("reference","amd"),default="reference")
    replay_parser.add_argument("--history-mode",choices=("both","identical","evolved"),default="both")
    replay_parser.add_argument("--kernels",choices=("baseline","optimized","auto"),default="optimized")
    replay_parser.add_argument("--arithmetic",choices=("k16","k32","final"),default="k16")
    replay_parser.add_argument("--gemm",choices=tuple(GEMM_VARIANTS),default="shared")
    replay_parser.add_argument("--tile-n",type=int,choices=(16,32,64),default=16)
    replay_parser.add_argument("--stage-k",type=int,choices=(16,32,64),default=16)
    replay_parser.add_argument("--window-queries",type=int,choices=(16,32,64),default=64)
    replay_parser.add_argument("--window-layout",choices=tuple(WINDOW_LAYOUTS),default="staged")
    replay_parser.add_argument("--timeout",type=int,default=900)
    replay_parser.add_argument("--coverage",choices=sorted(SCENE_COVERAGE),action="append",default=[])
    replay_parser.add_argument("--allow-nongame",action="store_true")
    replay_parser.add_argument("--allow-arithmetic-change",action="store_true")
    for flag in LEGACY_FUSION_KEYS:replay_parser.add_argument("--"+flag.replace("_","-"),action="store_true")
    for flag in ("ffn32_fusion", "qkv32_fusion"):
        replay_parser.add_argument("--"+flag.replace("_","-"),action=argparse.BooleanOptionalAction,default=None)
    args = parser.parse_args()
    try:
        if args.action == "collect":
            print(collect(args))
            return 0
        if args.action == "replay":
            report=replay_sequence(args)
            return 0 if report["thresholds_passed"] else 1
        report = (analyze(args.manifest, args.allow_arithmetic_change, args.network_manifest, args.comparison_anchor) if args.action == "analyze" else
                  qualify(args) if args.action == "qualify" else merge_candidates(args.candidate, args.comparison_anchor) if args.action == "merge" else
                  tuning(args.performance, args.qualification, args.comparison_anchor))
        write_json(args.output, report)
        print(args.output)
        return 1 if args.action == "qualify" and not report["passed"] else 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
