"""No-GPU tests of performance protocol, artifact gates and child isolation."""
import argparse
import array
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("tune_amd", Path(__file__).resolve().parents[1] / "tools" / "tune_amd.py")
tune = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tune)


class AmdTuningTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)

    @staticmethod
    def identity(role="candidate"):
        return {"device_id": "1002:7550", "driver_id": "synthetic-test-driver",
                "model_sha256": "a" * 64, "baseline_shader_sha256": "b" * 64,
                "shader_sha256": ("c" if role == "candidate" else "d") * 64}

    def record(self, role="baseline", mode="bench", frame_ms=None):
        candidate = role == "candidate"
        value = {"format": "OpenNR-amd-benchmark-v1", "command": mode, "readback": False,
                 "instrumented": mode == "profile", "width": 1707, "height": 960,
                 "padded_width": 1728, "padded_height": 960, "frames": 30, "warmup": 5,
                 "identity": self.identity(role),
                 "selected": {"kernels": "optimized" if candidate else "baseline", "arithmetic": "k16", "tile_n": 16, "stage_k": 16},
                 "frame_ms": frame_ms or [90 if candidate else 100] * 30}
        if mode == "profile":
            value["dispatches"] = [{"family": "fp8_gemm", "shape": {"rows": 4096, "N": 32, "K": 96,
                                     "batches": 1, "flags": 2, "partition": 0},
                                     "variant": "amd_gemm_optimized" if candidate else "amd_gemm", "tile_n": 16, "stage_k": 16,
                                     "frame_ms": [8 if candidate else 10] * 30}]
        return value

    def write(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def paired(self, mode="bench", transform=None, pairs=3):
        manifest = {"format": "OpenNR-amd-interleaved-v1", "runs": []}
        for pair in range(pairs):
            for role in ("baseline", "candidate"):
                value = self.record(role, mode)
                if transform:
                    transform(value, role, pair)
                name = f"{mode}-pair-{pair}-{role}.json"
                self.write(name, value)
                manifest["runs"].append({"role": role, "pair": pair, "file": name})
        return self.write(mode + "-paired.json", manifest)

    def assessed_profile(self, transform=None):
        network = self.paired()
        profile = self.paired("profile", transform)
        return tune.analyze(profile, network_manifest=network)

    def exact_suites(self, changed=False):
        (self.root / "base.u8").write_bytes(bytes([0, 0x80, 7, 255]))
        (self.root / "cand.u8").write_bytes(bytes([0, 0 if changed else 0x80, 7, 255]))
        paths = []
        for suite in ("operators", "model320", "target"):
            names = (list(sorted(tune.EXACT_COVERAGE)) if suite == "operators" else
                     sorted(tune.MODEL_CHECKPOINTS | {"head", "capture-production"}) if suite == "model320" else
                     ["head", "capture-production"])
            value = {"format": "OpenNR-amd-exact-manifest-v1", "suite": suite, "identity": self.identity(),
                     "baseline_identity":self.identity("baseline"),
                     "selected":tune.selected_policy(self.record("candidate")["selected"]),
                     "baseline_selected":tune.selected_policy(self.record("baseline")["selected"]),
                     "coverage": sorted(tune.EXACT_COVERAGE) if suite == "operators" else [],
                     "resolution": [320, 320] if suite == "model320" else [1707, 960],
                     "pairs": [{"name": name, "baseline": "base.u8", "candidate": "cand.u8"} for name in names]}
            paths.append(self.write(suite + ".json", value))
        return paths

    def test_complete_protocol_and_statistics(self):
        report = tune.analyze(self.paired())
        self.assertTrue(report["protocol_complete"])
        self.assertTrue(report["default_performance_eligible"])
        self.assertEqual(report["candidate_ms"]["samples"], 90)
        self.assertEqual(report["candidate_ms"]["median"], 90)
        self.assertEqual(report["candidate_ms"]["p99"], 90)
        self.assertEqual(report["candidate_ms"]["coefficient_of_variation"], 0)
        self.assertAlmostEqual(tune.stats([1, 2, 5])["p95"], 4.7)
        self.assertNotIn("fps", report)

    def test_legacy_and_independent_fusion_policy_binding(self):
        legacy = {**self.record("candidate")["selected"], "fusion": True}
        both = tune.selected_policy(legacy)
        self.assertTrue(both["ffn32_fusion"] and both["qkv32_fusion"])
        self.assertTrue(tune.evidence_equal(legacy, both))
        independent = {**both, "fusion": False, "ffn32_fusion": True, "qkv32_fusion": False}
        selected = tune.selected_policy(independent, explicit_flags=True)
        self.assertTrue(selected["ffn32_fusion"])
        self.assertFalse(selected["qkv32_fusion"])
        self.assertFalse(tune.evidence_equal(legacy, independent))
        for key in ("ffn32_fusion", "qkv32_fusion"):
            with self.subTest(missing=key):
                missing = dict(independent); missing.pop(key)
                with self.assertRaisesRegex(ValueError, "requires both routes"):
                    tune.selected_policy(missing)
            with self.subTest(type=key):
                wrong = dict(independent); wrong[key] = 1
                with self.assertRaisesRegex(ValueError, "contradicts legacy summary"):
                    tune.selected_policy(wrong)
        with self.assertRaisesRegex(ValueError, "contradicts legacy summary"):
            tune.selected_policy({**independent, "fusion": True})

    def test_independent_fusion_request_overrides_and_anchor_isolation(self):
        for legacy in (False, True):
            for ffn in (None, False, True):
                for qkv in (None, False, True):
                    args = argparse.Namespace(fusion=legacy, ffn32_fusion=ffn, qkv32_fusion=qkv)
                    selected = tune.requested_fusion_policy(args)
                    expected_ffn = legacy if ffn is None else ffn
                    expected_qkv = legacy if qkv is None else qkv
                    self.assertEqual(selected["ffn32_fusion"], expected_ffn)
                    self.assertEqual(selected["qkv32_fusion"], expected_qkv)
                    self.assertEqual(selected["fusion"], expected_ffn and expected_qkv)
                    self.assertFalse(any(tune.requested_fusion_policy(args, False).values()))
        with self.assertRaisesRegex(ValueError, "Boolean or absent"):
            tune.requested_fusion_policy(argparse.Namespace(ffn32_fusion=1))

    def test_gemm_policy_preserves_legacy_shared_and_requires_valid_direct_staging(self):
        legacy = self.record("candidate")["selected"]
        self.assertEqual(tune.selected_policy(legacy)["gemm"], "shared")
        self.assertTrue(tune.evidence_equal(legacy, {**legacy, "gemm": "shared"}))
        for gemm in ("packed", "direct"):
            selected = tune.selected_policy({**legacy, "gemm": gemm})
            self.assertEqual(selected["gemm"], gemm)
            self.assertFalse(tune.evidence_equal(legacy, selected))
        for gemm in ("DIRECT", "", None, 1, []):
            with self.subTest(gemm=gemm), self.assertRaisesRegex(ValueError, "selected.gemm"):
                tune.selected_policy({**legacy, "gemm": gemm})
        for stage in (32, 64):
            with self.subTest(stage=stage), self.assertRaisesRegex(ValueError, "direct GEMM requires stage_k=16"):
                tune.selected_policy({**legacy, "gemm": "direct", "stage_k": stage})
            self.assertEqual(tune.selected_policy({**legacy, "gemm": "packed", "stage_k": stage})["stage_k"], stage)

    def test_collect_gemm_is_explicit_and_anchor_always_shared(self):
        model = self.root / "model"; model.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                  output=self.root / "direct", mode="bench", kernels="optimized", arithmetic="k16", gemm="direct",
                                  tile_n=16, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="qualified32")
        calls = []
        def child(command, **kwargs):
            candidate = len(calls) % 2 == 1
            selected = {"kernels": "optimized", "arithmetic": "k16", "tile_n": 16, "stage_k": 16,
                        "window_queries": 32, "gemm": command[command.index("--amd-gemm") + 1],
                        **{key: kwargs["env"]["DLSS5VK_AMD_" + key.upper()] == "1" for key in tune.FUSION_KEYS}}
            self.assertEqual(selected["gemm"], kwargs["env"]["DLSS5VK_AMD_GEMM"])
            calls.append(selected)
            value = self.record("candidate" if candidate else "baseline"); value["selected"] = selected
            Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.dict(os.environ, {"DLSS5VK_AMD_GEMM": "packed"}), mock.patch.object(tune.subprocess, "run", side_effect=child):
            manifest = tune.collect(args)
            self.assertEqual(os.environ["DLSS5VK_AMD_GEMM"], "packed")
        self.assertTrue(all(value["gemm"] == "shared" for value in calls[::2]))
        self.assertTrue(all(value["gemm"] == "direct" for value in calls[1::2]))
        self.assertTrue(all(run["environment"]["DLSS5VK_AMD_GEMM"] == ("direct" if run["role"] == "candidate" else "shared")
                            for run in tune.read_json(manifest)["runs"]))

    def test_gemm_variant_and_explicit_quality_policy_bind_tuning(self):
        def direct(value, role, pair):
            if role == "candidate":
                value["selected"]["gemm"] = "direct"
                for entry in value.get("dispatches", []): entry["variant"] = "amd_gemm_direct"
        network = self.paired(transform=direct)
        profile = self.paired("profile", transform=direct)
        report = tune.analyze(profile, network_manifest=network)
        performance = self.write("direct-performance.json", report)
        exact = self.exact_suites()
        for path in exact:
            value = tune.read_json(path); value["selected"]["gemm"] = "direct"
            path.write_text(json.dumps(value), encoding="utf-8")
        qualification = tune.qualify(argparse.Namespace(exact=exact, sequence=[], arithmetic="k16"))
        proof = self.write("direct-qualification.json", qualification)
        output = tune.tuning(performance, proof)
        self.assertEqual(output["default_selection"]["gemm"], "direct")
        self.assertEqual(output["records"][0]["variant"], "amd_gemm_direct")
        self.assertEqual(output["records"][0]["evidence"]["selected"]["gemm"], "direct")
        # Omitting the policy denotes shared and cannot supply direct evidence.
        value = tune.read_json(exact[0]); value["selected"].pop("gemm")
        exact[0].write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "selected policy mismatch"):
            tune.qualify(argparse.Namespace(exact=exact, sequence=[], arithmetic="k16"))
        # Legacy policies normalize to shared. Direct dispatch evidence must
        # still name direct explicitly rather than inherit the shared default.
        for manifest in (network, profile):
            for run in tune.read_json(manifest)["runs"]:
                if run["role"] == "candidate":
                    path = manifest.parent / run["file"]
                    value = tune.read_json(path); value["selected"].pop("gemm")
                    path.write_text(json.dumps(value), encoding="utf-8")
        for path in exact:
            value = tune.read_json(path); value["selected"].pop("gemm", None)
            path.write_text(json.dumps(value), encoding="utf-8")
        shared_report = self.write("missing-gemm-performance.json", tune.analyze(profile, network_manifest=network))
        shared_quality = self.write("missing-gemm-qualification.json", tune.qualify(argparse.Namespace(exact=exact, sequence=[], arithmetic="k16")))
        with self.assertRaisesRegex(ValueError, "GEMM dispatch variant"):
            tune.tuning(shared_report, shared_quality)

    def test_collect_independent_route_controls_are_recorded_and_child_only(self):
        model = self.root / "model"; model.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                  output=self.root / "independent", mode="bench", kernels="optimized", arithmetic="k16",
                                  tile_n=16, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="compact64", fusion=True, ffn32_fusion=True, qkv32_fusion=False)
        calls = []
        def child(command, **kwargs):
            candidate = len(calls) % 2 == 1
            selected = {"kernels": "optimized", "arithmetic": "k16", "tile_n": 16, "stage_k": 16,
                        "window_queries": 32 if candidate else 64,
                        **{key: kwargs["env"]["DLSS5VK_AMD_" + key.upper()] == "1" for key in tune.FUSION_KEYS}}
            calls.append(selected)
            value = self.record("candidate" if candidate else "baseline"); value["selected"] = selected
            Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        parent = {"DLSS5VK_AMD_FFN32_FUSION": "0", "DLSS5VK_AMD_QKV32_FUSION": "1"}
        with mock.patch.dict(os.environ, parent), mock.patch.object(tune.subprocess, "run", side_effect=child):
            manifest = tune.collect(args)
            self.assertEqual({key: os.environ[key] for key in parent}, parent)
        self.assertTrue(all(not value["ffn32_fusion"] and not value["qkv32_fusion"] for value in calls[::2]))
        self.assertTrue(all(value["ffn32_fusion"] and not value["qkv32_fusion"] and not value["fusion"] for value in calls[1::2]))
        actual = tune.read_json(manifest)
        self.assertTrue(all(run["environment"]["DLSS5VK_AMD_FFN32_FUSION"] == "1" and
                            run["environment"]["DLSS5VK_AMD_QKV32_FUSION"] == "0"
                            for run in actual["runs"] if run["role"] == "candidate"))

    def test_incomplete_protocol_cannot_promote(self):
        report = tune.analyze(self.paired(pairs=2))
        self.assertFalse(report["protocol_complete"])
        self.assertFalse(report["default_performance_eligible"])

    def test_pooled_improvement_cannot_hide_regressed_pair(self):
        def mutate(value, role, pair):
            if role == "candidate":
                value["frame_ms"] = [103 if pair == 2 else 60] * 30
        report = tune.analyze(self.paired(transform=mutate))
        self.assertLess(report["network_median_ratio"], .95)
        self.assertFalse(report["network_regression_gate_passed"])

    def test_p95_regression_blocks_default(self):
        def mutate(value, role, pair):
            if role == "candidate":
                value["frame_ms"] = [90] * 26 + [110] * 4
        report = tune.analyze(self.paired(transform=mutate))
        self.assertFalse(report["default_performance_eligible"])

    def test_profile_aggregates_actual_dispatch_frames(self):
        def mutate(value, role, pair):
            value["dispatches"].append(copy.deepcopy(value["dispatches"][0]))
        report = self.assessed_profile(mutate)
        operator = report["operators"][0]
        self.assertTrue(report["instrumented"])
        self.assertEqual(operator["baseline_ms"]["median"], 20)
        self.assertEqual(operator["candidate_ms"]["median"], 16)
        self.assertTrue(operator["performance_eligible"])

    def test_missing_partition_cannot_claim_operator_match(self):
        def mutate(value, role, pair):
            if role == "candidate":
                value["dispatches"][0]["shape"]["partition"] = "split"
        report = tune.analyze(self.paired("profile", mutate))
        self.assertEqual(len(report["operators"]), 2)
        self.assertTrue(all(not item["performance_eligible"] for item in report["operators"]))

    def test_profile_only_cannot_promote(self):
        report = tune.analyze(self.paired("profile"))
        self.assertFalse(report["default_performance_eligible"])
        self.assertFalse(report["operators"][0]["performance_eligible"])
        self.assertTrue(self.assessed_profile()["operators"][0]["performance_eligible"])

    def test_unchanged_variant_timing_noise_cannot_qualify(self):
        def changed(value,role,pair):value["dispatches"][0]["variant"]="amd_gemm_optimized"
        report=self.assessed_profile(changed)
        self.assertFalse(report["operators"][0]["performance_eligible"])
        self.assertEqual(report["operators"][0]["baseline_variants"],report["operators"][0]["candidate_variants"])

    def test_window_query_selection_and_profile_geometry_are_distinct(self):
        def policy(value,role,pair):
            value["selected"]["window_queries"]=16 if role=="candidate" else 64
        network=self.paired(transform=policy)
        def dispatches(value,role,pair):
            policy(value,role,pair)
            item=value["dispatches"][0];item["family"]="window_attention"
            item["variant"]="amd_window_small" if role=="candidate" else "amd_window"
            item["geometry"]={"tile_m":16 if role=="candidate" else 64}
        profile=self.paired("profile",dispatches)
        report=tune.analyze(profile,network_manifest=network)
        self.assertEqual(report["selections"]["candidate"]["window_queries"],16)
        self.assertEqual(report["operators"][0]["candidate_variants"][0]["window_queries"],16)
        self.assertTrue(report["operators"][0]["performance_eligible"])
        def bad(value,role,pair):
            value["selected"]["window_queries"]=16
        with self.assertRaisesRegex(ValueError,"Q64"):
            tune.analyze(self.paired(transform=bad))

    @staticmethod
    def compact_policy(value, role, pair):
        value["selected"].update(kernels="optimized", window_queries=64 if role == "baseline" else 32,
                                 **{key: False for key in tune.FUSION_KEYS})
        if "dispatches" in value:
            dispatch = value["dispatches"][0]
            dispatch.update(family="window_attention", variant="amd_window_optimized" if role == "baseline" else "amd_window_small",
                            geometry={"tile_m": value["selected"]["window_queries"]})

    def compact_pairs(self, mode="bench", extra=None):
        def policy(value, role, pair):
            self.compact_policy(value, role, pair)
            if extra: extra(value, role, pair)
        path = self.paired(mode, policy)
        value = tune.read_json(path)
        value["comparison_anchor"] = "compact64"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def compact_exact_suites(self):
        paths = self.exact_suites()
        for path in paths:
            value = tune.read_json(path)
            value["comparison_anchor"] = "compact64"
            value["baseline_selected"].update(kernels="optimized", window_queries=64)
            value["selected"]["window_queries"] = 32
            path.write_text(json.dumps(value), encoding="utf-8")
        return paths

    def test_compact_anchor_requires_explicit_flag_and_actual_preserving_policy(self):
        path = self.compact_pairs()
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            tune.analyze(path)
        report = tune.analyze(path, comparison_anchor="compact64")
        self.assertEqual(report["comparison_anchor"], "compact64")
        self.assertEqual(report["selections"]["baseline"]["kernels"], "optimized")
        self.assertEqual(report["selections"]["baseline"]["window_queries"], 64)
        self.assertTrue(report["default_performance_eligible"])
        for field, invalid in (("kernels", "baseline"), ("arithmetic", "k32"), ("tile_n", 32),
                               ("stage_k", 32), ("window_queries", 32), ("fusion", True),
                               ("expert_fusion", True), ("block_fusion", True), ("hardware_publication", True)):
            def extra(value, role, pair):
                if role == "baseline":
                    value["selected"][field] = invalid
                    if field == "fusion":
                        value["selected"].update(ffn32_fusion=invalid, qkv32_fusion=invalid)
            with self.subTest(field=field), self.assertRaisesRegex(ValueError, "compact64 preserving"):
                tune.analyze(self.compact_pairs(extra=extra), comparison_anchor="compact64")

    def test_compact_anchor_never_relabels_unlabeled_legacy_or_missing_policy(self):
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            tune.analyze(self.paired(), comparison_anchor="compact64")
        def extra(value, role, pair):
            if role == "baseline": value["selected"].pop("hardware_publication")
        with self.assertRaisesRegex(ValueError, "explicitly Boolean"):
            tune.analyze(self.compact_pairs(extra=extra), comparison_anchor="compact64")

    def test_compact_exact_gate_requires_label_policy_and_matching_baseline_identity(self):
        paths = self.compact_exact_suites()
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            tune.exact_manifest(paths[0])
        args = argparse.Namespace(exact=paths, sequence=[], arithmetic="k16", comparison_anchor="compact64")
        result = tune.qualify(args)
        self.assertTrue(result["preservationQualified"])
        self.assertEqual(result["comparison_anchor"], "compact64")
        self.assertTrue(all(item["comparison_anchor"] == "compact64" for item in result["exact_suites"]))
        value = tune.read_json(paths[2]); value["baseline_identity"]["shader_sha256"] = "f" * 64
        paths[2].write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "baseline identity mismatch"):
            tune.qualify(args)

    def test_compact_tuning_binds_anchor_through_raw_network_and_exact_evidence(self):
        network = self.compact_pairs()
        profile = self.compact_pairs("profile")
        performance = self.write("compact-performance.json", tune.analyze(profile, network_manifest=network,
                                                                          comparison_anchor="compact64"))
        qualification = self.write("compact-qualification.json", tune.qualify(argparse.Namespace(
            exact=self.compact_exact_suites(), sequence=[], arithmetic="k16", comparison_anchor="compact64")))
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            tune.tuning(performance, qualification)
        result = tune.tuning(performance, qualification, "compact64")
        self.assertTrue(result["optimized_default_eligible"])
        self.assertEqual(result["comparison_anchor"], "compact64")
        self.assertEqual(result["records"][0]["comparison_anchor"], "compact64")
        self.assertEqual(result["default_selection"]["window_queries"], 32)
        self.assertEqual(result["identity"], self.identity())

    def qualified32_pairs(self, mode="bench", extra=None):
        def policy(value, role, pair):
            value["selected"].update(kernels="optimized", window_queries=32,
                                     **{key: False for key in tune.FUSION_KEYS})
            if role == "candidate": value["selected"]["tile_n"] = 32
            if "dispatches" in value:
                value["dispatches"][0].update(variant="amd_gemm_optimized",
                                               tile_n=value["selected"]["tile_n"])
            if extra: extra(value, role, pair)
        path = self.paired(mode, policy)
        value = tune.read_json(path); value["comparison_anchor"] = "qualified32"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_qualified32_measures_incremental_gain_against_explicit_current_policy(self):
        network = self.qualified32_pairs()
        report = tune.analyze(network, comparison_anchor="qualified32")
        self.assertEqual(report["selections"]["baseline"]["window_queries"], 32)
        self.assertTrue(report["default_performance_eligible"])
        self.assertEqual(report["network_median_ratio"], .9)
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            tune.analyze(network, comparison_anchor="compact64")
        def previous(value, role, pair):
            if role == "baseline": value["selected"]["window_queries"] = 64
        with self.assertRaisesRegex(ValueError, "qualified32 preserving N16/K16/Q32"):
            tune.analyze(self.qualified32_pairs(extra=previous), comparison_anchor="qualified32")
        def missing(value, role, pair): value["selected"].pop("window_queries")
        with self.assertRaisesRegex(ValueError, "explicitly recorded window_queries"):
            tune.analyze(self.qualified32_pairs(extra=missing), comparison_anchor="qualified32")

    def test_qualified32_anchor_binds_incremental_operator_and_exact_tuning_evidence(self):
        network = self.qualified32_pairs()
        profile = self.qualified32_pairs("profile")
        performance = self.write("qualified32-performance.json", tune.analyze(
            profile, network_manifest=network, comparison_anchor="qualified32"))
        paths = self.exact_suites()
        for path in paths:
            value = tune.read_json(path); value["comparison_anchor"] = "qualified32"
            value["baseline_selected"].update(kernels="optimized", window_queries=32)
            value["selected"].update(window_queries=32, tile_n=32)
            path.write_text(json.dumps(value), encoding="utf-8")
        qualification = self.write("qualified32-qualification.json", tune.qualify(argparse.Namespace(
            exact=paths, sequence=[], arithmetic="k16", comparison_anchor="qualified32")))
        result = tune.tuning(performance, qualification, "qualified32")
        self.assertTrue(result["optimized_default_eligible"])
        self.assertEqual(result["records"][0]["comparison_anchor"], "qualified32")
        self.assertEqual(tune.read_json(performance)["selections"]["baseline"]["window_queries"], 32)

    def test_collect_qualified32_launches_q32_baseline_with_overrides_disabled(self):
        model = self.root / "qualified32-model"; model.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                  output=self.root / "qualified32-out", mode="bench", kernels="optimized", arithmetic="k16",
                                  tile_n=32, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="qualified32", fusion=True)
        calls = []
        def child(command, **kwargs):
            role = "baseline" if len(calls) % 2 == 0 else "candidate"
            selected = {"kernels": command[command.index("--amd-kernels") + 1], "arithmetic": "k16",
                        "tile_n": int(command[command.index("--amd-tile-n") + 1]),
                        "stage_k": int(command[command.index("--amd-stage-k") + 1]),
                        "window_queries": int(command[command.index("--amd-window-queries") + 1]),
                        **{key: kwargs["env"]["DLSS5VK_AMD_" + key.upper()] == "1" for key in tune.FUSION_KEYS}}
            calls.append(selected)
            value = self.record(role); value["selected"] = selected
            Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(tune.subprocess, "run", side_effect=child): manifest = tune.collect(args)
        self.assertTrue(all(tune.preserving_baseline(value, "qualified32") for value in calls[::2]))
        self.assertTrue(all(value["window_queries"] == 32 and value["tile_n"] == 32 for value in calls[1::2]))
        self.assertEqual(tune.read_json(manifest)["comparison_anchor"], "qualified32")

    def test_legacy_historical_report_remains_accepted_without_anchor_fields(self):
        report = self.assessed_profile()
        qualification = tune.qualify(argparse.Namespace(exact=self.exact_suites(), sequence=[], arithmetic="k16"))
        def historical(value):
            if isinstance(value, dict):
                return {key: historical(item) for key, item in value.items() if key != "comparison_anchor"}
            if isinstance(value, list): return [historical(item) for item in value]
            return value
        result = tune.tuning(self.write("historical-performance.json", historical(report)),
                             self.write("historical-qualification.json", historical(qualification)))
        self.assertTrue(result["optimized_default_eligible"])
        self.assertEqual(result["comparison_anchor"], "legacy")

    def test_collect_compact_anchor_launches_explicit_optimized_q64_children(self):
        model = self.root / "model"; model.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                  output=self.root / "out", mode="bench", kernels="optimized", arithmetic="k16",
                                  tile_n=32, stage_k=64, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="compact64", fusion=True)
        calls = []
        def child(command, **kwargs):
            role = "baseline" if len(calls) % 2 == 0 else "candidate"
            selected = {"kernels": command[command.index("--amd-kernels") + 1], "arithmetic": "k16",
                        "tile_n": int(command[command.index("--amd-tile-n") + 1]),
                        "stage_k": int(command[command.index("--amd-stage-k") + 1]),
                        "window_queries": int(command[command.index("--amd-window-queries") + 1]),
                        **{key: kwargs["env"]["DLSS5VK_AMD_" + key.upper()] == "1" for key in tune.FUSION_KEYS}}
            calls.append(selected)
            value = self.record(role); value["selected"] = selected
            Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(tune.subprocess, "run", side_effect=child):
            manifest = tune.collect(args)
        self.assertTrue(all(tune.preserving_baseline(value, "compact64") for value in calls[::2]))
        self.assertTrue(all(value["tile_n"] == 32 and value["stage_k"] == 64 and value["fusion"] for value in calls[1::2]))
        output = tune.read_json(manifest)
        self.assertEqual(output["comparison_anchor"], "compact64")
        self.assertTrue(all(value["comparison_anchor"] == "compact64" for value in output["runs"]))

    def test_legacy_window_query_default_and_invalid_geometry(self):
        self.assertEqual(tune.analyze(self.paired())["selections"]["candidate"]["window_queries"],64)
        value=self.record("candidate","profile");value["dispatches"][0]["family"]="window_attention"
        value["dispatches"][0]["geometry"]={"tile_m":128}
        with self.assertRaisesRegex(ValueError,"geometry.tile_m"):
            tune.benchmark(self.write("bad-window.json",value))

    def test_network_evidence_must_be_ordinary_and_matched(self):
        profile = self.paired("profile")
        with self.assertRaisesRegex(ValueError, "uninstrumented"):
            tune.analyze(profile, network_manifest=profile)
        def changed(value, role, pair):
            value["width"] = 1920
            value["padded_width"] = 1920
        network = self.paired(transform=changed)
        with self.assertRaisesRegex(ValueError, "does not match"):
            tune.analyze(profile, network_manifest=network)

    def test_changed_arithmetic_requires_explicit_selection(self):
        def mutate(value, role, pair):
            if role == "candidate":
                value["selected"]["arithmetic"] = "final"
        path = self.paired(transform=mutate)
        with self.assertRaisesRegex(ValueError, "allow-arithmetic-change"):
            tune.analyze(path)
        self.assertEqual(tune.analyze(path, True)["selections"]["candidate"]["arithmetic"], "final")

    def test_mismatched_pair_identity_geometry_and_policy(self):
        changes = [("identity", "driver_id", "another-driver"), ("identity", "model_sha256", "e" * 64),
                   ("identity", "baseline_shader_sha256", "e" * 64), (None, "width", 1920),
                   (None, "command", "profile"), ("selected", "stage_k", 32)]
        for nested, field, changed in changes:
            def mutate(value, role, pair):
                if role == "candidate" and pair == 2:
                    (value[nested] if nested else value)[field] = changed
            with self.subTest(field=field), self.assertRaises(ValueError):
                tune.analyze(self.paired(transform=mutate))

    def test_missing_nan_negative_zero_and_minima_samples_rejected(self):
        for bad in ([], [1] * 29, [float("nan")] * 30, [-1] * 30, [0] * 30):
            value = self.record()
            value["frame_ms"] = bad
            with self.subTest(sample=bad[:1]), self.assertRaises(ValueError):
                tune.benchmark(self.write("bad.json", value))
        value = self.record(mode="profile")
        value["dispatches"][0]["frame_ms"] = [1]
        with self.assertRaisesRegex(ValueError, "exactly 30"):
            tune.benchmark(self.write("bad.json", value))

    def test_capture_and_instrumentation_are_explicit(self):
        for field, bad in (("readback", True), ("readback", None), ("instrumented", True)):
            value = self.record()
            value[field] = bad
            with self.assertRaisesRegex(ValueError, "readback"):
                tune.benchmark(self.write("bad.json", value))

    def test_order_and_repeated_selected_shader_identity(self):
        path = self.paired()
        value = tune.read_json(path)
        value["runs"][0], value["runs"][1] = value["runs"][1], value["runs"][0]
        path.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "ordered"):
            tune.analyze(path)
        def mutate(value, role, pair):
            if role == "candidate" and pair == 2:
                value["identity"]["shader_sha256"] = "e" * 64
        with self.assertRaisesRegex(ValueError, "changed between"):
            tune.analyze(self.paired(transform=mutate))

    def test_duplicate_json_keys_rejected(self):
        path = self.root / "dup.json"
        path.write_text('{"frames":30,"frames":30}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "duplicate"):
            tune.read_json(path)

    def test_exact_signed_zero_changes_fail_preservation(self):
        report = tune.qualify(argparse.Namespace(exact=self.exact_suites(changed=True), sequence=[], arithmetic="k16"))
        self.assertFalse(report["preservationQualified"])
        self.assertFalse(report["passed"])

    def test_preservation_is_distinct_from_unmet_release_gates(self):
        report = tune.qualify(argparse.Namespace(exact=self.exact_suites(), sequence=[], arithmetic="k16"))
        self.assertTrue(report["preservationQualified"])
        self.assertTrue(report["passed"])
        self.assertFalse(report["releaseGates"]["scene_quality"])
        self.assertFalse(report["releaseGates"]["complete_game_benchmarks"])

    def test_model_free_operator_suite_joins_actual_model_qualification(self):
        paths = self.exact_suites()
        value = tune.read_json(paths[0])
        value["model_free"] = True
        value.update(passed=True,validationErrors=0,mismatchedChecks=0,checks=len(value["pairs"]),operators=9)
        value["fixture_sha256"] = "e" * 64
        value["identity"].pop("model_sha256")
        value["baseline_identity"] = self.identity("baseline")
        value["baseline_identity"].pop("model_sha256")
        paths[0].write_text(json.dumps(value), encoding="utf-8")
        report = tune.qualify(argparse.Namespace(exact=paths, sequence=[], arithmetic="k16"))
        self.assertTrue(report["preservationQualified"])
        self.assertEqual(report["identity"]["model_sha256"], "a" * 64)
        value["identity"]["driver_id"] = "another-driver"
        paths[0].write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "identity differs"):
            tune.exact_manifest(paths[0])
        value["identity"]["driver_id"] = "synthetic-test-driver"
        value["validationErrors"] = 1
        paths[0].write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"Vulkan validation"):
            tune.exact_manifest(paths[0])

    def test_empty_or_incomplete_exact_buffers_cannot_qualify(self):
        paths = self.exact_suites()
        value = tune.read_json(paths[1])
        value["pairs"].pop()
        paths[1].write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "75 checkpoints"):
            tune.exact_manifest(paths[1])
        (self.root / "base.u8").write_bytes(b"")
        with self.assertRaisesRegex(ValueError, "empty"):
            tune.exact_manifest(paths[0])

    def test_tuning_requires_real_unchanged_evidence(self):
        performance = self.write("performance.json", self.assessed_profile())
        qualification = self.write("qualification.json", tune.qualify(argparse.Namespace(exact=self.exact_suites(), sequence=[], arithmetic="k16")))
        output = tune.tuning(performance, qualification)
        self.assertEqual(len(output["records"]), 1)
        self.assertEqual(output["records"][0]["evidence"]["selected"], output["default_selection"])
        self.assertEqual(set(output["records"][0]["evidence"]["selected"]), set(tune.SELECTION_KEYS))
        self.assertTrue(output["optimized_default_eligible"])
        self.assertEqual(output["identity"],self.identity())
        self.assertEqual(output["geometry"]["padded_width"],1728)
        self.assertEqual(output["default_selection"]["tile_n"],16)
        self.assertFalse(output["default_selection"]["fusion"])
        self.assertEqual(output["records"][0]["key"]["shape"]["partition"], 0)
        (self.root / "cand.u8").write_bytes(b"different bytes")
        with self.assertRaisesRegex(ValueError, "artifacts"):
            tune.tuning(performance, qualification)

    def test_tuning_binds_specializations_even_when_shader_hash_is_unchanged(self):
        def changed(value,role,pair):
            if role=="candidate":value["selected"]["stage_k"]=32
        performance=self.write("performance.json",tune.analyze(self.paired(transform=changed)))
        qualification=self.write("qualification.json",tune.qualify(argparse.Namespace(exact=self.exact_suites(),sequence=[],arithmetic="k16")))
        with self.assertRaisesRegex(ValueError,"selected policy does not match"):
            tune.tuning(performance,qualification)

    def test_no_operator_records_cannot_claim_loader_eligibility(self):
        performance=self.write("performance.json",tune.analyze(self.paired()))
        qualification=self.write("qualification.json",tune.qualify(argparse.Namespace(exact=self.exact_suites(),sequence=[],arithmetic="k16")))
        result=tune.tuning(performance,qualification)
        self.assertEqual(result["records"],[])
        self.assertFalse(result["optimized_default_eligible"])

    def test_named_or_oversized_shapes_are_analysis_only(self):
        for invalid in ("ordinary",1 << 32):
            def changed(value,role,pair):value["dispatches"][0]["shape"]["partition"]=invalid
            performance=self.write("performance-"+str(invalid)+".json",self.assessed_profile(changed))
            qualification=self.write("qualification-"+str(invalid)+".json",tune.qualify(argparse.Namespace(exact=self.exact_suites(),sequence=[],arithmetic="k16")))
            result=tune.tuning(performance,qualification)
            self.assertEqual(result["records"],[])
            self.assertFalse(result["optimized_default_eligible"])

    def test_nonaccelerated_operator_cannot_enter_native_tuning(self):
        def changed(value,role,pair):
            value["dispatches"][0]["family"]="convert"
            value["dispatches"][0]["variant"]="portable_convert_candidate" if role=="candidate" else "portable_convert_baseline"
        performance=self.write("performance.json",self.assessed_profile(changed))
        qualification=self.write("qualification.json",tune.qualify(argparse.Namespace(exact=self.exact_suites(),sequence=[],arithmetic="k16")))
        result=tune.tuning(performance,qualification)
        self.assertEqual(result["records"],[])
        self.assertFalse(result["optimized_default_eligible"])

    def test_exact_suites_bind_policies_and_reject_duplicate_suites(self):
        paths=self.exact_suites()
        with self.assertRaisesRegex(ValueError,"duplicate exact suites"):
            tune.qualify(argparse.Namespace(exact=[*paths,paths[0]],sequence=[],arithmetic="k16"))
        value=tune.read_json(paths[2]);value["selected"]["window_queries"]=16
        paths[2].write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"selected policy mismatch"):
            tune.qualify(argparse.Namespace(exact=paths,sequence=[],arithmetic="k16"))

    def test_exact_relative_fixture_root_resolves_from_manifest(self):
        paths=self.exact_suites();fixture=self.root/"actual-fixtures";fixture.mkdir()
        (fixture/"base.u8").write_bytes((self.root/"base.u8").read_bytes())
        (fixture/"cand.u8").write_bytes((self.root/"cand.u8").read_bytes())
        value=tune.read_json(paths[0]);value["fixtureRoot"]="actual-fixtures"
        paths[0].write_text(json.dumps(value),encoding="utf-8")
        self.assertTrue(tune.exact_manifest(paths[0])["passed"])

    def test_same_candidate_cannot_join_different_frozen_baseline_proofs(self):
        paths=self.exact_suites();value=tune.read_json(paths[2])
        value["baseline_identity"]["shader_sha256"]="e"*64
        paths[2].write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"baseline identity mismatch"):
            tune.qualify(argparse.Namespace(exact=paths,sequence=[],arithmetic="k16"))

    def test_model_free_operator_policy_can_be_flat_in_identity(self):
        paths=self.exact_suites();value=tune.read_json(paths[0])
        value.update(model_free=True,passed=True,validationErrors=0,mismatchedChecks=0,checks=len(value["pairs"]),operators=9,fixture_sha256="e"*64)
        for role,field in (("candidate","identity"),("baseline","baseline_identity")):
            value[field].pop("model_sha256")
            value[field].update(value.pop("selected" if role=="candidate" else "baseline_selected"))
        paths[0].write_text(json.dumps(value),encoding="utf-8")
        self.assertTrue(tune.qualify(argparse.Namespace(exact=paths,sequence=[],arithmetic="k16"))["passed"])

    def test_tuning_does_not_trust_edited_eligibility(self):
        report = self.assessed_profile()
        report["network_p95_ratio"] = .001
        performance = self.write("performance.json", report)
        qualification = self.write("qualification.json", tune.qualify(argparse.Namespace(exact=self.exact_suites(), sequence=[], arithmetic="k16")))
        with self.assertRaisesRegex(ValueError, "raw interleaved"):
            tune.tuning(performance, qualification)

    def test_changed_qualification_emits_no_automatic_records(self):
        performance = self.write("performance.json", self.assessed_profile())
        qualification = self.write("qualification.json", tune.qualify(argparse.Namespace(exact=self.exact_suites(changed=True), sequence=[], arithmetic="k16")))
        self.assertEqual(tune.tuning(performance, qualification)["records"], [])

    def test_merge_selects_per_shape_winners_without_promoting_combination(self):
        base = {"key": {**self.identity(), "arithmetic": "k16", "family": "gemm",
                        "shape": {"rows": 1024, "N": 32, "K": 64, "batches": 1, "flags": 0, "partition": 0}},
                "variant": "N16", "tile_n": 16, "stage_k": 16, "qualified": True,
                "operator_improvement_fraction": .1, "network_median_ratio": .94, "network_p95_ratio": .95}
        faster = {**base, "variant": "N32", "tile_n": 32, "operator_improvement_fraction": .2}
        with mock.patch.object(tune, "tuning", side_effect=[{"records": [base]}, {"records": [faster]}]) as assess:
            result = tune.merge_candidates([[Path("perf-a"), Path("qual-a")], [Path("perf-b"), Path("qual-b")]])
        self.assertEqual(assess.call_count, 2)
        self.assertEqual(result["records"][0]["variant"], "N32")
        self.assertFalse(result["optimized_default_eligible"])
        self.assertIsNone(result["default_selection"])

    def test_collect_launches_interleaved_isolated_children(self):
        model = self.root / "model"
        model.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                  output=self.root / "out", mode="bench", kernels="optimized", arithmetic="k16",
                                  tile_n=16, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False,window_queries=16)
        calls = []
        def child(command, **kwargs):
            role = "baseline" if command[command.index("--amd-kernels") + 1] == "baseline" else "candidate"
            calls.append((role, kwargs["env"]))
            output = Path(command[command.index("--json") + 1])
            value=self.record(role);value["selected"]["window_queries"]=int(command[command.index("--amd-window-queries")+1])
            output.write_text(json.dumps(value), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.dict(os.environ, {"DLSS5VK_UNFUSED": "1", "DLSS5VK_AMD_ARITHMETIC": "final"}), mock.patch.object(tune.subprocess, "run", side_effect=child):
            manifest = tune.collect(args)
            self.assertEqual(os.environ["DLSS5VK_UNFUSED"], "1")
            self.assertEqual(os.environ["DLSS5VK_AMD_ARITHMETIC"], "final")
        self.assertEqual([x[0] for x in calls], ["baseline", "candidate"] * 3)
        self.assertTrue(all("DLSS5VK_UNFUSED" not in env and env["DLSS5VK_AMD_ARITHMETIC"] == "k16" for _, env in calls))
        self.assertEqual([env["DLSS5VK_AMD_WINDOW_QUERIES"] for _,env in calls],["64","16"]*3)
        self.assertEqual(tune.read_json(manifest)["runs"][0]["executable_sha256"], tune.sha256(Path(sys.executable)))
        self.assertTrue((args.output / "performance.json").is_file())
        with self.assertRaises(FileExistsError):
            tune.collect(args)

    def test_collect_rejects_silent_forced_fallback(self):
        model = self.root / "model"
        model.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                  output=self.root / "out", mode="bench", kernels="optimized", arithmetic="k16",
                                  tile_n=16, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False)
        def child(command, **kwargs):
            Path(command[command.index("--json") + 1]).write_text(json.dumps(self.record("baseline")), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(tune.subprocess, "run", side_effect=child), self.assertRaisesRegex(ValueError, "silently"):
            tune.collect(args)

    @staticmethod
    def metadata(frame=0):
        return {"frame_id": frame, "sequence_id": "synthetic", "seed": 123,
                "model_sha256": "a" * 64, "input_sha256": "f" * 64, "controls": {"intensity": 1},
                "render_resolution": [11, 11], "output_resolution": [11, 11], "pipeline_point": "pre-fsr",
                "color_space": "scene-linear", "pre_exposure": 1, "exposure_scale": 1,
                "jitter": [0, 0], "motion_scale": [1, 1], "reset": True, "history_frame_ids": []}

    def sequence(self, mode="identical", changed=False, arithmetic="final"):
        values = array.array("f", [2.0, .5, -0.1] * 121)
        candidate = array.array("f", [1.0 if changed else 2.0, .5, -0.1] * 121)
        sign = b"-1\n" if sys.byteorder == "little" else b"1\n"
        (self.root / "a.pfm").write_bytes(b"PF\n11 11\n" + sign + values.tobytes())
        (self.root / "b.pfm").write_bytes(b"PF\n11 11\n" + sign + candidate.tobytes())
        metadata = self.metadata()
        selected=tune.selected_policy(self.record("candidate")["selected"]);selected["arithmetic"]=arithmetic
        return self.write(mode + ".json", {"format": "OpenNR-quality-sequence-v1", "identity": self.identity(),"selected":selected,
                          "history_mode": mode, "coverage": sorted(tune.SCENE_COVERAGE), "data_range": 1.0,
                          "frames": [{"reference": "a.pfm", "candidate": "b.pfm",
                                      "metadata": {"reference": metadata, "candidate": metadata}}]})

    def test_experimental_quality_requires_unclamped_both_history_modes(self):
        left, right = self.sequence(), self.sequence("evolved")
        report = tune.qualify(argparse.Namespace(exact=[], sequence=[left, right], arithmetic="final"))
        self.assertTrue(report["passed"])
        self.assertFalse(report["preservationQualified"])
        self.assertTrue(report["releaseGates"]["scene_quality"])
        self.assertFalse(report["releaseGates"]["motion_review"])
        with self.assertRaisesRegex(ValueError, "both history"):
            tune.qualify(argparse.Namespace(exact=[], sequence=[left], arithmetic="final"))

    def test_quality_cannot_be_relabelled_as_different_actual_arithmetic(self):
        left,right=self.sequence(),self.sequence("evolved")
        with self.assertRaisesRegex(ValueError,"actual arithmetic differs"):
            tune.qualify(argparse.Namespace(exact=[],sequence=[left,right],arithmetic="k32"))

    def test_highlight_error_fails_fixed_range_gate(self):
        left, right = self.sequence(changed=True,arithmetic="k32"), self.sequence("evolved", changed=True,arithmetic="k32")
        report = tune.qualify(argparse.Namespace(exact=[], sequence=[left, right], arithmetic="k32"))
        self.assertFalse(report["passed"])
        self.assertLess(report["quality_sequences"][0]["result"]["frames"][0]["psnr_db"], 40)
        value = tune.read_json(left)
        value["data_range"] = 16.0
        left.write_text(json.dumps(value), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "data_range"):
            tune.qualify(argparse.Namespace(exact=[], sequence=[left, right], arithmetic="k32"))

    def capture_sequence_fixture(self,count=2):
        directory=self.root/"captured-sequence";directory.mkdir()
        for index in range(count):
            frame=directory/("frame-"+str(index));frame.mkdir()
            value={"format":"OpenNR-game-capture-v1","gameCapture":True,"sequence_id":"synthetic-sequence",
                   "capture_ordinal":index,"requested_capture_count":count,"frame_id":index,"width":11,"height":11,
                   "modelManifestSha256":"a"*64,"filesSha256":{"source-packed.f32":"f"*64},
                   "controls":{"intensity":1},"pre_exposure":1,"exposure_scale":1,"jitter":[0,0],"motion_scale":[1,1],
                   "reset":index==0}
            (frame/"manifest.json").write_text(json.dumps(value),encoding="utf-8")
        return directory

    def test_capture_sequence_requires_complete_genuine_contiguous_metadata(self):
        directory=self.capture_sequence_fixture()
        self.assertEqual(len(tune.captured_sequence(directory)),2)
        first=directory/"frame-0"/"manifest.json";value=tune.read_json(first);value["gameCapture"]=False
        first.write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"genuine"):
            tune.captured_sequence(directory)
        self.assertEqual(len(tune.captured_sequence(directory,True)),2)
        value["gameCapture"]=True;first.write_text(json.dumps(value),encoding="utf-8")
        second=directory/"frame-1"/"manifest.json";other=tune.read_json(second);other["frame_id"]=3
        second.write_text(json.dumps(other),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"frame gap"):
            tune.captured_sequence(directory)
        other["reset"]=True;second.write_text(json.dumps(other),encoding="utf-8")
        self.assertEqual(len(tune.captured_sequence(directory)),2)
        other["capture_ordinal"]=2;second.write_text(json.dumps(other),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"ordinal"):
            tune.captured_sequence(directory)

    def test_staged_or_missing_capture_does_not_count_as_complete(self):
        directory=self.capture_sequence_fixture()
        (directory/"frame-1").rename(directory/".frame-1-staging")
        with self.assertRaisesRegex(ValueError,"incomplete"):
            tune.captured_sequence(directory)

    def test_sequence_driver_builds_both_history_modes_with_separate_ancestry(self):
        directory=self.capture_sequence_fixture();model=self.root/"model";model.mkdir()
        args=argparse.Namespace(capture_sequence=directory,executable=Path(sys.executable),model=model,
                                shaders=None,game_shaders=None,output=self.root/"replays",history_mode="both",
                                kernels="optimized",arithmetic="k16",tile_n=16,stage_k=16,timeout=30,
                                reference_backend="reference",coverage=["sdr","motion"],allow_nongame=False,
                                allow_arithmetic_change=False)
        calls=[]
        def child(command,**kwargs):
            destination=Path(command[command.index("--fixture")+1]);destination.mkdir()
            capture=tune.read_json(Path(command[command.index("--recorded-frame")+1])/"manifest.json")
            mode=command[command.index("--history-mode")+1];frame=capture["frame_id"]
            calls.append((mode,frame,command,kwargs["env"]))
            report={"sourceFrameId":frame,"historyMode":mode,"seed":frame,"reset":frame==0,
                    "historyFrameIds":[] if frame==0 else [frame-1],"identity":self.identity(),
                    "selected":tune.selected_policy(self.record("candidate")["selected"])}
            (destination/"manifest.json").write_text(json.dumps(report),encoding="utf-8")
            pixels=array.array("f",[2.0,.5,-.1]*121);sign=b"-1\n" if sys.byteorder=="little" else b"1\n"
            (destination/"recorded-scene-linear-rgb.pfm").write_bytes(b"PF\n11 11\n"+sign+pixels.tobytes())
            return subprocess.CompletedProcess(command,0)
        with mock.patch.object(tune.subprocess,"run",side_effect=child):
            report=tune.replay_sequence(args)
        self.assertTrue(report["thresholds_passed"])
        self.assertTrue(report["genuine_game_capture"])
        self.assertFalse(report["performance_representative"])
        self.assertEqual(len(calls),8)
        for mode,frame,command,environment in calls:
            self.assertEqual("--previous-replay" in command,mode=="evolved" and frame==1)
            self.assertEqual(environment["DLSS5VK_CHAIN"],"0")
        evolved=tune.read_json(args.output/"evolved-sequence.json")
        self.assertEqual(evolved["frames"][1]["metadata"]["candidate"]["history_frame_ids"],[0])


if __name__ == "__main__":
    unittest.main()
