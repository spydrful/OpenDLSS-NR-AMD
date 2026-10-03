"""Screen alpha 3's independent C32 fusion routes against Direct-off.

The default action writes a CPU-only plan. GPU work requires both an explicit
``collect`` or ``qualify`` action and ``--execute-gpu``. Results and generated
model artifacts stay in new private output directories; this script never
changes an installed game, defaults, tuning records, or release assets.

Example (use an exclusive GPU slot and keep the model outside source control)::

  python scripts/screen_amd_fusion.py plan --package <alpha3-directory> \
      --model <local-model> --output <new-plan-directory>
  python scripts/screen_amd_fusion.py collect --execute-gpu \
      --package <alpha3-directory> --model <local-model> --output <new-timings>
  python scripts/screen_amd_fusion.py qualify --execute-gpu \
      --package <alpha3-directory> --model <local-model> --timings <timings> \
      --output <new-fixtures>

Timing uses five warmups and three interleaved pairs of 30 measured frames per
candidate by default, with no image readback. Qualification is separate and
exports all 75 checkpoints at 320 plus target-resolution head/composition.
``--target-boundaries`` additionally exports and byte-compares all 75 target
checkpoints; allow sufficient disk space and GPU memory for those captures.
Neither synthetic fixtures nor network timestamps qualify game quality/FPS.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
PACKAGE_HASH = "15decbb890e99eed00e491465feb3ecbdf8a2629c1145155b09d1baa1964cf68"
EXE_HASH = "e93b4ac685666f63a79d2003475ace747aba36dbdce4caddf99ee143f1763c13"
MODEL_HASH = "163f7fdeaa5b0c2ba39103cf5c46853b18d163847cea67f8c9d85e77f78c655e"
DEVICE = "1002:7550"
DRIVER = "AMD proprietary driver|26.9.1 (LLPC)|8389003"
ANCHOR = "direct32"
ROUTES = {"off": (False, False), "ffn-only": (True, False),
          "qkv-only": (False, True), "both": (True, True)}
BASE_SHADER_NAMES = ("amd_gemm", "portable_f16", "amd_window", "amd_global_matrix",
                     "ops", "preprocess", "amd_window_normalize", "amd_global_normalize")
DIRECT_SHADER_NAMES = ("amd_gemm_direct", "portable_f16", "amd_window_small", "amd_global_matrix",
                       "ops", "preprocess", "amd_window_normalize", "amd_global_normalize")


def load_tool(name: str):
    path = ROOT / "tools" / (name + ".py")
    spec = importlib.util.spec_from_file_location("_fusion_screen_" + name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


protocol = load_tool("tune_amd")
model_qualification = load_tool("qualify_amd_model")


def require(condition, message: str) -> None:
    if not condition:
        raise ValueError(message)


def aggregate(names, hashes) -> str:
    value = "".join(name + ":" + hashes[name + ".spv"] + "\n" for name in sorted(names))
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def policy(route: str) -> dict:
    ffn, qkv = ROUTES[route]
    return {"kernels": "optimized", "arithmetic": "k16", "gemm": "direct", "window_layout": "staged",
            "tile_n": 16, "stage_k": 16, "window_queries": 32,
            "fusion": ffn and qkv, "ffn32_fusion": ffn, "qkv32_fusion": qkv,
            "expert_fusion": False, "block_fusion": False, "hardware_publication": False}


def snapshot(args) -> dict:
    package = args.package.resolve(strict=True)
    package_manifest = package / "package-manifest.json"
    require(protocol.sha256(package_manifest) == PACKAGE_HASH, "package is not the pinned published alpha 3")
    manifest = protocol.read_json(package_manifest)
    executable = (args.executable or package / "tools" / "dlss5vk.exe").resolve(strict=True)
    shaders = (args.shaders or package / "payload" / "open-nr" / "shaders").resolve(strict=True)
    model = args.model.resolve(strict=True)
    require(protocol.sha256(executable) == EXE_HASH, "executable differs from the measured alpha 3 CLI")
    require(protocol.sha256(model / "manifest.json") == MODEL_HASH, "model manifest differs from alpha 3")
    shader_hashes = {}
    for item in manifest["files"]:
        if item.get("role") == "shader":
            filename = Path(item["source"]).name
            require(filename not in shader_hashes and filename.endswith(".spv"), "invalid shader package record")
            shader_hashes[filename] = protocol.sha256(shaders / filename)
            require(shader_hashes[filename] == item["sha256"], "shader differs from alpha 3: " + filename)
    require(len(shader_hashes) == 26 and {path.name for path in shaders.glob("*.spv")} == set(shader_hashes),
            "shader directory must contain exactly the 26 alpha 3 SPIR-V modules")
    stages = {}
    for item in protocol.read_json(model / "manifest.json")["stages"]:
        filename = item["file"]
        require(Path(filename).name == filename, "model stage has an unsafe filename")
        stage = model / "model" / filename
        require(stage.stat().st_size == item["packedByteLength"], "model stage length differs: " + filename)
        stages[filename] = protocol.sha256(stage)
        require(stages[filename] == item["sha256"].lower(), "model stage hash differs: " + filename)
    require(len(stages) == 11, "model must have all eleven native stages")
    identities = {}
    for route, (ffn, qkv) in ROUTES.items():
        names = list(DIRECT_SHADER_NAMES)
        if ffn:
            names.append("amd_ffn32")
        if qkv:
            names.append("amd_qkv32")
        identities[route] = {"device_id": DEVICE, "driver_id": DRIVER, "model_sha256": MODEL_HASH,
                             "shader_sha256": aggregate(names, shader_hashes),
                             "baseline_shader_sha256": aggregate(BASE_SHADER_NAMES, shader_hashes)}
    return {"package_directory": str(package), "package_manifest_sha256": PACKAGE_HASH,
            "executable": str(executable), "executable_sha256": EXE_HASH,
            "shader_directory": str(shaders), "shader_files": shader_hashes,
            "model_directory": str(model), "model_manifest_sha256": MODEL_HASH, "model_stages": stages,
            "expected_identities": identities,
            "runner_sha256": protocol.sha256(Path(__file__).resolve()),
            "analysis_tools": {name: protocol.sha256(ROOT / "tools" / name)
                               for name in ("tune_amd.py", "qualify_amd_model.py")}}


def environment(output: Path, route: str) -> dict:
    selected = policy(route)
    result = {"DLSS5VK_BACKEND": "amd", "DLSS5VK_CHAIN": "0", "DLSS5VK_VALIDATION": "0",
              "DLSS5VK_DEBUG": "0", "DLSS5VK_PIPELINE_CACHE": str(output / "pipeline-cache")}
    result.update({"DLSS5VK_AMD_" + name.upper(): str(int(value)) if type(value) is bool else str(value)
                   for name, value in selected.items()})
    return result


def cli(inputs: dict, route: str, command: str) -> list[str]:
    result = [inputs["executable"], command, "--backend", "amd", "--model", inputs["model_directory"],
              "--shaders", inputs["shader_directory"]]
    for name, value in policy(route).items():
        # The older summary switch is set first, and explicit routes always follow it.
        if name == "fusion":
            result += ["--amd-fusion", "0"]
        elif name in ("ffn32_fusion", "qkv32_fusion"):
            result += ["--amd-" + name.replace("_", "-"), str(int(value))]
        else:
            result += ["--amd-" + name.replace("_", "-"), str(int(value)) if type(value) is bool else str(value)]
    return result


def create_output(path: Path) -> Path:
    path = path.absolute()
    path.mkdir(parents=True, exist_ok=False)
    return path.resolve()


def check_run(run: dict, inputs: dict, route: str, width: int = 1707, height: int = 960) -> None:
    require(run["identity"] == inputs["expected_identities"][route], "actual device/driver/model/shader identity differs")
    require(protocol.selected_policy(run["selected"], explicit_flags=True) == policy(route),
            "forced fusion selection was ignored or silently changed")
    require((run["width"], run["height"], run["padded_width"], run["padded_height"]) == (width, height, 1728, 960),
            "timing geometry differs from the full target")


def describe(args, inputs: dict) -> dict:
    return {"format": "OpenNR-amd-fusion-screen-plan-v1", "created_utc": datetime.now(timezone.utc).isoformat(),
            "comparison_anchor": ANCHOR, "inputs": inputs, "gpu_executed": False,
            "timing": {"command": "bench", "width": 1707, "height": 960,
                       "padded_width": 1728, "padded_height": 960,
                       "warmup": args.warmup, "frames": args.frames, "pairs_per_candidate": args.pairs,
                       "readback": False, "instrumented": False,
                       "order": "three separate candidate protocols; each pair runs Direct-off immediately before its candidate"},
            "routes": {route: {"selected": policy(route), "identity": inputs["expected_identities"][route],
                               "bench_argv_prefix": cli(inputs, route, "bench")}
                       for route in ROUTES},
            "qualification": {"model320": "all 75 checkpoints, F32 head, composed proxy RGB and capture/production byte equality",
                              "target": "F32 head, composed proxy RGB and capture/production byte equality",
                              "target_boundaries": bool(args.target_boundaries),
                              "synthetic_scene": True, "captured_game_quality": False},
            "release_qualified": False,
            "limitations": ["Network timings exclude D3D12 bridge, FSR and game presentation.",
                            "Synthetic modelcheck composition is a clamped proxy, not unclamped scene-linear game quality.",
                            "This screen does not update defaults or publish tuning records.",
                            "Active gameplay, temporal scenes, highlight acceptance and lifecycle tests remain separate."]}


def collect(args, inputs: dict, output: Path) -> dict:
    require(ANCHOR in protocol.COMPARISON_ANCHORS, "tune_amd.py must support the explicit direct32 anchor")
    reports = {}
    for route in ("ffn-only", "qkv-only", "both"):
        ffn, qkv = ROUTES[route]
        print("GPU timing begins: " + route + "; Direct-off baseline, 1707x960 -> 1728x960; no readback", flush=True)
        request = argparse.Namespace(executable=Path(inputs["executable"]),
                                     baseline_executable=Path(inputs["executable"]),
                                     model=Path(inputs["model_directory"]), shaders=Path(inputs["shader_directory"]),
                                     baseline_shaders=Path(inputs["shader_directory"]), output=output / route, mode="bench",
                                     kernels="optimized", arithmetic="k16", gemm="direct", tile_n=16, stage_k=16,
                                     window_queries=32, comparison_anchor=ANCHOR, width=1707, height=960,
                                     warmup=args.warmup, frames=args.frames, pairs=args.pairs, timeout=args.timeout,
                                     allow_arithmetic_change=False, fusion=False, ffn32_fusion=ffn,
                                     qkv32_fusion=qkv, expert_fusion=False, block_fusion=False, hardware_publication=False)
        path = protocol.collect(request)
        manifest = protocol.read_json(path)
        for item in manifest["runs"]:
            run = protocol.benchmark(path.parent / item["file"], explicit_policy=True)
            check_run(run, inputs, "off" if item["role"] == "baseline" else route)
            require(item["executable_sha256"] == EXE_HASH, "child binary identity differs")
        reports[route] = protocol.analyze(path, comparison_anchor=ANCHOR)
        require(snapshot(args) == inputs, "pinned assets or analysis tools changed during timing")
    return {"format": "OpenNR-amd-fusion-screen-timings-v1", "comparison_anchor": ANCHOR,
            "inputs": inputs, "gpu_executed": True, "readback": False, "instrumented": False,
            "candidates": reports, "release_qualified": False,
            "limitation": "Timing eligibility is independent of arithmetic, temporal, bridge and game release gates."}


def launch(command, overrides, log_path: Path, timeout: int) -> None:
    child = {key: value for key, value in os.environ.items() if not key.startswith("DLSS5VK_")}
    print("GPU capture begins: " + " ".join(command), flush=True)
    with log_path.open("xb") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT,
                                cwd=str(Path(command[0]).parent), env={**child, **overrides}, timeout=timeout,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    require(result.returncode == 0, f"capture failed with exit {result.returncode}; see {log_path}")


def exact_pair(first: Path, second: Path, name: str, expected_bytes: int | None = None) -> dict:
    require(first.is_file() and second.is_file(), "missing exact artifact: " + name)
    first_bytes, second_bytes = first.stat().st_size, second.stat().st_size
    require(first_bytes > 0 and first_bytes == second_bytes, "exact artifact sizes differ: " + name)
    if expected_bytes is not None:
        require(first_bytes == expected_bytes, "exact artifact has incorrect geometry: " + name)
    different = model_qualification.different_bytes(first, second)
    return {"name": name, "baseline": str(first), "candidate": str(second), "bytes": first_bytes,
            "baseline_sha256": protocol.sha256(first), "candidate_sha256": protocol.sha256(second),
            "differing_bytes": different, "exact": different == 0}


def target_boundaries(root: Path) -> dict:
    manifest = protocol.read_json(root / "manifest.json")
    full, levels = model_qualification.geometry(1707, 960)
    expected = model_qualification.expected_shapes(full, levels)
    artifacts = {}
    for group, number_key, prefix in (("blocks", "block", "block-"), ("transitions", "id", "transition-")):
        for item in manifest[group]:
            name = prefix + str(item[number_key])
            require(name in expected and name not in artifacts, "unexpected or duplicate target boundary: " + name)
            actual = (item["width"], item["height"], item["channels"])
            require(all(type(value) is int for value in actual) and actual == expected[name],
                    "target boundary shape differs: " + name)
            width, height, channels = actual
            artifacts[name] = model_qualification.artifact(root, item["file"], name, width * height * channels)
    require(set(artifacts) == set(expected), "target must export all 75 model checkpoints")
    return artifacts


def qualify(args, inputs: dict, output: Path) -> dict:
    require(args.timings is not None, "qualify requires the separate --timings directory")
    timings = args.timings.resolve(strict=True)
    timing_summary = protocol.read_json(timings / "summary.json")
    require(timing_summary.get("format") == "OpenNR-amd-fusion-screen-timings-v1"
            and timing_summary["inputs"] == inputs, "timing evidence has different pinned assets or analysis tools")
    benchmarks = {}
    for route in ("ffn-only", "qkv-only", "both"):
        manifest_path = timings / route / "interleaved.json"
        require(protocol.evidence_equal(protocol.analyze(manifest_path, comparison_anchor=ANCHOR),
                                        timing_summary["candidates"][route]), "timing report no longer reproduces")
        manifest = protocol.read_json(manifest_path)
        baseline, candidate = manifest["runs"][:2]
        benchmarks.setdefault("off", manifest_path.parent / baseline["file"])
        benchmarks[route] = manifest_path.parent / candidate["file"]
        for item in manifest["runs"]:
            check_run(protocol.benchmark(manifest_path.parent / item["file"], explicit_policy=True),
                      inputs, "off" if item["role"] == "baseline" else route)
    suites = {}
    for suite, width, height in (("model320", 320, 320), ("target", 1707, 960)):
        fixtures = {}
        for route in ROUTES:
            fixture = output / suite / route
            fixture.parent.mkdir(parents=True, exist_ok=True)
            require(not fixture.exists(), "capture output already exists")
            command = cli(inputs, route, "modelcheck") + ["--fixture", str(fixture), "--width", str(width),
                       "--height", str(height), "--frames", "3", "--intermediates", "--require-exact"]
            if suite == "target" and not args.target_boundaries:
                command.append("--head-only")
            if route != "off":
                command += ["--reference", str(fixtures["off"])]
            overrides = environment(output / suite / route, route)
            launch(command, overrides, fixture.parent / (route + ".log"), args.timeout)
            protocol.write_json(fixture.parent / (route + "-execution.json"),
                                {"argv": command, "environment": overrides, "executable_sha256": EXE_HASH})
            recorded = model_qualification.fixture(fixture, benchmarks[route], "candidate", suite == "target", ANCHOR)
            require(recorded["identity"] == inputs["expected_identities"][route]
                    and recorded["selected"] == policy(route), "capture provenance differs from the requested route")
            fixtures[route] = fixture
            require(snapshot(args) == inputs, "pinned assets or analysis tools changed during qualification")
        candidates = {}
        for route in ("ffn-only", "qkv-only", "both"):
            exact = model_qualification.qualify(fixtures["off"], fixtures[route], benchmarks["off"], benchmarks[route],
                        output / suite / (route + "-exact.json"), target_only=suite == "target", comparison_anchor=ANCHOR)
            extra = [exact_pair(fixtures["off"] / "composed-rgb.f32", fixtures[route] / "composed-rgb.f32",
                                "composed-proxy-rgb", width * height * 3 * 4)]
            model_qualification.finite_f32(fixtures["off"] / "composed-rgb.f32", "baseline composed proxy RGB")
            model_qualification.finite_f32(fixtures[route] / "composed-rgb.f32", "candidate composed proxy RGB")
            if suite == "target" and args.target_boundaries:
                baseline = target_boundaries(fixtures["off"])
                candidate = target_boundaries(fixtures[route])
                extra += [exact_pair(baseline[name]["path"], candidate[name]["path"], name, baseline[name]["bytes"])
                          for name in sorted(baseline)]
            candidates[route] = {"passed": exact["passed"] and all(pair["exact"] for pair in extra),
                                 "exact_manifest": str(output / suite / (route + "-exact.json")),
                                 "exact_manifest_sha256": protocol.sha256(output / suite / (route + "-exact.json")),
                                 "extra_pairs": extra}
        suites[suite] = candidates
    return {"format": "OpenNR-amd-fusion-screen-exact-v1", "comparison_anchor": ANCHOR, "inputs": inputs,
            "gpu_executed": True, "timing_directory": str(timings),
            "passed": all(item["passed"] for suite in suites.values() for item in suite.values()),
            "target_all_75_checkpoints": bool(args.target_boundaries), "suites": suites,
            "synthetic_scene": True, "captured_game_quality_qualified": False,
            "bridge_lifecycle_qualified": False, "release_qualified": False}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("plan", "collect", "qualify"), nargs="?", default="plan")
    parser.add_argument("--package", type=Path, required=True, help="unmodified extracted published alpha 3 package")
    parser.add_argument("--model", type=Path, required=True, help="local imported model; never packaged or uploaded")
    parser.add_argument("--output", type=Path, required=True, help="new absent private output directory")
    parser.add_argument("--executable", type=Path, help="optional frozen CLI with the exact published alpha 3 hash")
    parser.add_argument("--shaders", type=Path, help="optional separate directory containing exactly the pinned alpha 3 SPVs")
    parser.add_argument("--timings", type=Path, help="collect output directory; required for qualify")
    parser.add_argument("--execute-gpu", action="store_true", help="explicitly launch GPU work; absent means no child execution")
    parser.add_argument("--target-boundaries", action="store_true", help="also export/byte-compare all 75 target checkpoints")
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--pairs", type=int, default=3)
    parser.add_argument("--timeout", type=int, default=900, help="per GPU process timeout in seconds")
    args = parser.parse_args()
    output = None
    try:
        require(args.action == "plan" or args.execute_gpu, "GPU actions require explicit --execute-gpu")
        require(not (args.action == "plan" and args.execute_gpu), "plan is CPU-only; omit --execute-gpu")
        for name in ("frames", "pairs", "timeout"):
            protocol.integer(getattr(args, name), name, 1)
        protocol.integer(args.warmup, "warmup")
        inputs = snapshot(args)
        destination = args.output.resolve()
        protected = [Path(inputs[key]) for key in ("package_directory", "model_directory", "shader_directory")]
        protected += [Path(inputs["executable"]).parent]
        if args.timings is not None:
            protected.append(args.timings.resolve())
        require(all(not destination.is_relative_to(path) for path in protected),
                "output must be separate from the package, model, shaders, binary directory and existing timing evidence")
        output = create_output(args.output)
        plan = describe(args, inputs)
        protocol.write_json(output / "plan.json", plan)
        if args.action == "plan":
            print("CPU-only pinned fusion plan: " + str(output / "plan.json"))
            return 0
        result = collect(args, inputs, output) if args.action == "collect" else qualify(args, inputs, output)
        protocol.write_json(output / "summary.json", result)
        print("Fusion " + args.action + " result: " + str(output / "summary.json"))
        return 1 if args.action == "qualify" and not result["passed"] else 0
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        if output is not None and not (output / "failure.json").exists():
            protocol.write_json(output / "failure.json", {"action": args.action, "error": str(error), "release_qualified": False})
        print(str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
