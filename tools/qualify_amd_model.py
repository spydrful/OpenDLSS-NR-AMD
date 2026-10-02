"""Qualify preserving AMD kernels from modelcheck's real binary artifacts.

No model execution or GPU access occurs here. Baseline and candidate fixtures
must have execution identities exported by modelcheck and bound to their own
bench/profile records. The 320 suite checks every graph boundary. --target-only
checks the two head schedules at 1707x960 without reading all target boundaries.
These deterministic, reset-history inputs establish neither game quality nor
NVIDIA runtime parity, and modelcheck timings do not qualify performance.
"""
from __future__ import annotations

import argparse
import array
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys


_spec = importlib.util.spec_from_file_location("_amd_qualification_protocol", Path(__file__).with_name("tune_amd.py"))
_protocol = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_protocol)

TRANSITIONS = (0, 4, 8, 14, 22)
CHECKPOINTS = {f"block-{block}" for block in range(70)} | {
    f"transition-{block}-{block + 1}" for block in TRANSITIONS}
SELECTION_KEYS = _protocol.SELECTION_KEYS
CHUNK_BYTES = 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(value, label: str) -> str:
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-fA-F]{64}", value) is not None,
            f"{label} must be a SHA-256 digest")
    return value.lower()


def selection(value, label: str) -> dict:
    if isinstance(value, dict): value = {"window_queries": 64, **value}
    require(isinstance(value, dict), f"{label} lacks actual selected kernel policy")
    require(value.get("kernels") in ("baseline", "optimized") and value.get("arithmetic") == "k16",
            f"{label} must use preserving k16 arithmetic")
    for key in ("tile_n", "stage_k"):
        require(type(value.get(key)) is int and value[key] in (16, 32, 64), f"{label}: invalid {key}")
    require(type(value.get("window_queries")) is int and value["window_queries"] in (16, 32, 64), f"{label}: invalid window query count")
    try:
        return _protocol.selected_policy(value, explicit_flags=True)
    except ValueError as error:
        raise ValueError(f"{label}: {error}") from error


def identity(value, label: str) -> dict:
    result = _protocol.identity(value)
    require(re.fullmatch(r"[0-9a-f]{4}:[0-9a-f]{4}", result["device_id"]) is not None,
            f"{label}: device_id must be lowercase vendor:device hexadecimal")
    return result


def dimensions(value, label: str) -> tuple[int, int]:
    require(isinstance(value, list) and len(value) == 2 and all(type(v) is int and 1 <= v <= 32768 for v in value),
            f"{label} must contain two positive bounded dimensions")
    return tuple(value)


def geometry(width: int, height: int) -> tuple[tuple[int, int], list[tuple[int, int]]]:
    """The documented Geometry::fromValid padding rule, independent of fixtures."""
    def align(value, amount):
        return ((value + amount - 1) // amount) * amount
    def alignment(valid):
        reductions, size = 0, valid
        for level in range(6):
            half = align((size + 1) // 2, 4)
            reductions += int(half < size)
            reductions += int(level == 0 and half % 8 != 0)
            size = half
        return 1 << reductions
    aw, ah = alignment(width), alignment(height)
    full_width, full_height = max(320, align(width, aw)), max(320, align(height, ah))
    if full_width % (4 * aw) == 0 and full_height % (4 * ah) == 0:
        full_width += aw
    current_width, current_height = full_width, full_height
    levels = []
    for _ in range(6):
        current_width, current_height = align((current_width + 1) // 2, 4), align((current_height + 1) // 2, 4)
        levels.append((current_width, current_height))
    require(levels[0][0] % 8 == 0 and levels[0][1] % 8 == 0, "unsupported graph geometry")
    return (full_width, full_height), levels


def expected_shapes(full: tuple[int, int], levels: list[tuple[int, int]]) -> dict:
    result = {"block-0": (*full, 32)}
    stages = ((1, 4, 0, 32), (5, 8, 1, 64), (9, 14, 2, 128), (15, 22, 3, 256),
              (23, 30, 4, 512), (31, 38, 5, 1024), (39, 47, 4, 512),
              (48, 55, 3, 256), (56, 61, 2, 128), (62, 65, 1, 64), (66, 69, 0, 32))
    for first, last, level, channels in stages:
        for block in range(first, last + 1):
            result[f"block-{block}"] = (*levels[level], channels)
    for level, block in enumerate(TRANSITIONS):
        result[f"transition-{block}-{block + 1}"] = (*levels[level], 32 << level)
    return result


def artifact(root: Path, filename, label: str, size: int) -> dict:
    require(isinstance(filename, str) and filename.strip(), f"{label} lacks a filename")
    relative = Path(filename)
    require(not relative.is_absolute() and ".." not in relative.parts, f"{label}: filename must stay inside its fixture")
    path = (root / relative).resolve(strict=True)
    require(path.is_relative_to(root) and path.is_file(), f"{label}: artifact escapes its fixture or is not a file")
    require(path.stat().st_size == size and size > 0, f"{label}: expected {size} bytes, got {path.stat().st_size}")
    return {"path": path, "bytes": size, "sha256": _protocol.sha256(path)}


def finite_f32(path: Path, label: str) -> None:
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(CHUNK_BYTES), b""):
            require(len(chunk) % 4 == 0, f"{label}: truncated F32 input")
            values = array.array("f")
            values.frombytes(chunk)
            if sys.byteorder != "little":
                values.byteswap()
            require(all(math.isfinite(value) for value in values), f"{label}: actual buffer contains nonfinite F32 values")


def different_bytes(first: Path, second: Path) -> int:
    count = 0
    with first.open("rb") as a, second.open("rb") as b:
        while True:
            left, right = a.read(CHUNK_BYTES), b.read(CHUNK_BYTES)
            if not left and not right:
                return count
            count += abs(len(left) - len(right))
            if left != right:
                count += sum(x != y for x, y in zip(left, right))


def pair(name: str, baseline: dict, candidate: dict) -> dict:
    different = different_bytes(baseline["path"], candidate["path"])
    return {"name": name, "baseline": str(baseline["path"]), "candidate": str(candidate["path"]),
            "baseline_sha256": baseline["sha256"], "candidate_sha256": candidate["sha256"],
            "baseline_bytes": baseline["bytes"], "candidate_bytes": candidate["bytes"],
            "differentBytes": different, "exact": baseline["bytes"] == candidate["bytes"] and different == 0}


def fixture(path: Path, benchmark_path: Path, role: str, target_only: bool, comparison_anchor="legacy") -> dict:
    root = path.resolve(strict=True)
    require(root.is_dir(), f"{role}: fixture must be a directory")
    manifest_path, report_path = root / "manifest.json", root / "modelcheck-report.json"
    manifest, report = _protocol.read_json(manifest_path), _protocol.read_json(report_path)
    benchmark = _protocol.benchmark(benchmark_path, explicit_policy=comparison_anchor != "legacy")
    for recorded in (manifest, report, benchmark):
        if "comparison_anchor" in recorded:
            _protocol.require_anchor(recorded, comparison_anchor)
        if comparison_anchor != "legacy":
            require(isinstance(recorded.get("selected"), dict) and "window_queries" in recorded["selected"],
                    f"{role}: optimized anchors require explicitly recorded window_queries")
    ident, selected = identity(manifest.get("identity"), role), selection(manifest.get("selected"), role)
    require(ident == benchmark["identity"], f"{role}: fixture execution identity differs from benchmark")
    require(selected == selection(benchmark.get("selected"), f"{role} benchmark"),
            f"{role}: fixture selected policy differs from benchmark")
    require(identity(report.get("identity"), f"{role} report") == ident and selection(report.get("selected"), f"{role} report") == selected,
            f"{role}: manifest/report execution provenance disagrees")
    if role == "baseline":
        query_count = 32 if comparison_anchor == "qualified32" else 64
        require(_protocol.preserving_baseline(selected, comparison_anchor),
                f"baseline must use the explicit {comparison_anchor} baseline N16/K16/Q{query_count} policy without overrides")
    else:
        require(selected["kernels"] == "optimized", "candidate must report optimized kernels")
    require(manifest.get("producer") == "OpenNR Vulkan amd; local validation, not NVIDIA capture"
            and report.get("format") == "OpenNR-local-modelcheck-v1" and report.get("backend") == "amd",
            f"{role}: expected a native AMD modelcheck fixture, not a NVIDIA/reference/game capture")
    require(manifest.get("inputGenerator") == "fixedpoint-proxy-clt-noise-v1", f"{role}: unrecognized input generator")
    require(report.get("nvidiaParityEstablished") is False and report.get("qualityThresholdApplied") is False,
            f"{role}: incompatible diagnostic provenance")
    require(manifest.get("captureImplementation") == "decomposed" and report.get("captureImplementation") == "decomposed",
            f"{role}: captured head must come from --intermediates decomposed execution")
    require(report.get("productionRepeatable") is True and report.get("captureHeadIdentical") is True
            and type(report.get("nonfiniteHead")) is int and report["nonfiniteHead"] == 0,
            f"{role}: modelcheck reports a failed/incomplete execution")
    valid = dimensions(manifest.get("sourceDimensions"), f"{role} sourceDimensions")
    require(valid == ((1707, 960) if target_only else (320, 320)), f"{role}: incorrect suite resolution")
    full, levels = geometry(*valid)
    require(dimensions(manifest.get("fullDimensions"), f"{role} fullDimensions") == full,
            f"{role}: padded geometry differs from the graph rule")
    model_hash = digest(manifest.get("modelManifestSha256"), f"{role} modelManifestSha256")
    require(model_hash == ident["model_sha256"] == digest(report.get("modelManifestSha256"), f"{role} report model hash"),
            f"{role}: model manifest identity disagrees")
    checks = manifest.get("checks")
    require(isinstance(checks, list) and "head" in checks and (target_only or "boundaries" in checks),
            f"{role}: fixture does not export the required checks")
    require(isinstance(manifest.get("inputFeatures"), dict), f"{role}: missing inputFeatures")
    inputs = artifact(root, manifest["inputFeatures"].get("file"), f"{role} features", full[0] * full[1] * 16 * 4)
    require(inputs["sha256"] == digest(manifest.get("inputFeaturesSha256"), f"{role} feature hash")
            == digest(report.get("identicalFeaturesSha256"), f"{role} report feature hash"),
            f"{role}: actual features SHA-256 differs from recorded provenance")
    finite_f32(inputs["path"], f"{role} features")
    heads = {}
    for key, exported in (("head", "referenceHead"), ("captured-head", "capturedHead")):
        require(isinstance(manifest.get(exported), dict), f"{role}: missing {exported} artifact")
        heads[key] = artifact(root, manifest[exported].get("file"), f"{role} {key}", full[0] * full[1] * 4 * 4)
        finite_f32(heads[key]["path"], f"{role} {key}")
    require(manifest["capturedHead"].get("productionIdentical") is True,
            f"{role}: fixture does not report successful capture/production execution")
    boundaries = {}
    if not target_only:
        expected = expected_shapes(full, levels)
        for group, key, prefix in (("blocks", "block", "block-"), ("transitions", "id", "transition-")):
            entries = manifest.get(group)
            require(isinstance(entries, list), f"{role}: missing {group}")
            for entry in entries:
                require(isinstance(entry, dict), f"{role}: invalid boundary record")
                number = entry.get(key)
                require((type(number) is int and 0 <= number < 70) if key == "block" else isinstance(number, str),
                        f"{role}: invalid boundary name")
                name = prefix + str(number)
                require(name in expected and name not in boundaries, f"{role}: unexpected/duplicate boundary {name}")
                shape = (entry.get("width"), entry.get("height"), entry.get("channels"))
                require(all(type(value) is int for value in shape) and shape == expected[name],
                        f"{role}: incorrect graph shape for {name}")
                boundaries[name] = artifact(root, entry.get("file"), f"{role} {name}", math.prod(shape))
        require(set(boundaries) == CHECKPOINTS, f"{role}: all 75 graph boundaries are required")
    return {"root": root, "identity": ident, "selected": selected, "valid": valid, "full": full,
            "inputs": inputs, "heads": heads, "boundaries": boundaries,
            "provenance": {"fixture": str(root), "manifest_sha256": _protocol.sha256(manifest_path),
                           "report_sha256": _protocol.sha256(report_path), "benchmark": str(benchmark_path.resolve()),
                           "benchmark_sha256": _protocol.sha256(benchmark_path), "input_features_sha256": inputs["sha256"],
                           "head_sha256": heads["head"]["sha256"], "captured_head_sha256": heads["captured-head"]["sha256"],
                           "benchmark_resolution": [benchmark["width"], benchmark["height"]]}}


def qualify(baseline_path: Path, candidate_path: Path, baseline_benchmark: Path,
            candidate_benchmark: Path, output: Path, *, target_only: bool = False,
            comparison_anchor="legacy") -> dict:
    anchor = _protocol.comparison_anchor(comparison_anchor)
    output = output.absolute()
    require(not output.exists(), f"output already exists: {output}")
    baseline = fixture(baseline_path, baseline_benchmark, "baseline", target_only, anchor)
    candidate = fixture(candidate_path, candidate_benchmark, "candidate", target_only, anchor)
    require(baseline["root"] != candidate["root"], "baseline and candidate fixtures must be separate")
    for key in ("device_id", "driver_id", "model_sha256", "baseline_shader_sha256"):
        require(baseline["identity"][key] == candidate["identity"][key], f"baseline/candidate identity differs: {key}")
    require(baseline["inputs"]["sha256"] == candidate["inputs"]["sha256"]
            and different_bytes(baseline["inputs"]["path"], candidate["inputs"]["path"]) == 0,
            "baseline and candidate actual feature bytes differ")
    pairs = [pair(name, baseline["boundaries"][name], candidate["boundaries"][name])
             for name in sorted(baseline["boundaries"])]
    pairs.append(pair("head", baseline["heads"]["head"], candidate["heads"]["head"]))
    # One pair must prove both backends' capture schedules. Concatenating the
    # two equal-length heads preserves both proofs for tune_amd's binary gate,
    # which independently compares actual artifacts rather than these booleans.
    bundle_paths = [output.with_name(output.stem + "-production-heads.bin"),
                    output.with_name(output.stem + "-captured-heads.bin")]
    require(all(not path.exists() for path in bundle_paths), "capture/production bundle output already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    created = []
    try:
        bundles = []
        for destination, head_name in zip(bundle_paths, ("head", "captured-head")):
            sha, length = hashlib.sha256(), 0
            with destination.open("xb") as sink:
                created.append(destination)
                for source in (baseline, candidate):
                    with source["heads"][head_name]["path"].open("rb") as head:
                        for chunk in iter(lambda: head.read(CHUNK_BYTES), b""):
                            sink.write(chunk)
                            sha.update(chunk)
                            length += len(chunk)
            bundles.append({"path": destination, "bytes": length, "sha256": sha.hexdigest()})
        capture_pair = pair("capture-production", *bundles)
        capture_pair["component_order"] = ["baseline", "candidate"]
        capture_pair["baseline_schedule"] = "production"
        capture_pair["candidate_schedule"] = "capture"
        pairs.append(capture_pair)
        mismatches = sum(not item["exact"] for item in pairs)
        result = {"format": "OpenNR-amd-exact-manifest-v1", "suite": "target" if target_only else "model320",
                  "comparison_anchor": anchor,
                  "identity": candidate["identity"], "baseline_identity": baseline["identity"],
                  "selected": candidate["selected"], "baseline_selected": baseline["selected"],
                  "resolution": list(candidate["valid"]), "padded_resolution": list(candidate["full"]),
                  "fixtureRoot": str(output.parent), "fixture_sha256": candidate["inputs"]["sha256"],
                  "inputGenerator": "fixedpoint-proxy-clt-noise-v1", "syntheticScene": True,
                  "capturedGameFrames": False, "nvidiaParityEstablished": False,
                  "qualityThresholdApplied": False, "performanceQualified": False,
                  "repeatabilityEvidence": "modelcheck execution report; this tool independently compares exported buffers",
                  "createdUtc": datetime.now(timezone.utc).isoformat(), "passed": mismatches == 0,
                  "checks": len(pairs), "mismatchedChecks": mismatches,
                  "coverage": ["head", "capture-production"] if target_only else ["all-75-boundaries", "head", "capture-production"],
                  "captureProductionComponents": {"baseline": baseline["provenance"], "candidate": candidate["provenance"]},
                  "pairs": pairs}
        with output.open("x", encoding="utf-8") as sink:
            created.append(output)
            json.dump(result, sink, indent=2, allow_nan=False)
            sink.write("\n")
        return result
    except Exception:
        # Only remove the exact files this invocation exclusively created.
        for path in created:
            path.unlink(missing_ok=True)
        raise


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--baseline", type=Path, required=True, help="frozen baseline AMD modelcheck fixture directory")
    parser.add_argument("--candidate", type=Path, required=True, help="optimized AMD modelcheck fixture directory")
    parser.add_argument("--baseline-benchmark", type=Path, required=True, help="actual baseline bench/profile JSON")
    parser.add_argument("--candidate-benchmark", type=Path, required=True, help="actual candidate bench/profile JSON")
    parser.add_argument("--output", type=Path, required=True, help="new exact-manifest path under ignored build/ (also writes two proof bundles)")
    parser.add_argument("--target-only", action="store_true", help="head/capture-production only at valid 1707x960; default is all 75 boundaries at 320x320")
    parser.add_argument("--comparison-anchor", choices=_protocol.COMPARISON_ANCHORS, default="legacy",
                        help="explicit baseline role: legacy kernels, compact64 optimized Q64, or qualified32 optimized Q32")
    args = parser.parse_args(argv)
    try:
        result = qualify(args.baseline, args.candidate, args.baseline_benchmark, args.candidate_benchmark,
                         args.output, target_only=args.target_only, comparison_anchor=args.comparison_anchor)
    except (OSError, ValueError) as error:
        print(f"qualification rejected: {error}", file=sys.stderr)
        return 2
    print(f"{result['suite']}: {result['checks']} actual binary checks, {result['mismatchedChecks']} mismatched; "
          f"{'PASS' if result['passed'] else 'FAIL'}; {args.output.resolve()}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
