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

    def add_vit_evidence(self, value):
        """Synthetic byte fixtures exercise metadata binding without Vulkan."""
        value["coverage"].append(tune.VIT_COVERAGE)
        cases = []
        for K, N, rows, flags, partition, batches in tune.VIT_CASES:
            name = f"gemm-vit-K{K}-N{N}-R{rows}-F{flags}-P{partition}-B{batches}"
            cases.append(dict(name=name, K=K, N=N, rows=rows, flags=flags,
                              partition=partition, batches=batches))
            allocation = ((rows + 63) // 64) * 64 * (32 + batches * N)
            sizes = {name: allocation * (1 if flags & 16 else 2)}
            if flags & 32:
                sizes[name + "-dual"] = allocation
            for buffer_name, size in sizes.items():
                filename = buffer_name + ".u8"
                (self.root / filename).write_bytes(bytes([0xA5]) * size)
                value["pairs"].append(dict(name=buffer_name, baseline=filename, candidate=filename))
        value["extended_vit"] = dict(coverage_marker=tune.VIT_COVERAGE,
                                     case_count=len(cases), cases=cases)
        if value.get("model_free"):
            value["checks"] = len(value["pairs"])
            value["operators"] += len(cases)
        if value["selected"]["gemm"] in tune.EXTENDED_GEMMS:
            self.add_prototype_evidence(value,window_padding=False)
        return value

    def add_raw_overdispatch_evidence(self, value, buffers=True):
        policy = value["selected"]
        value["raw_gemm_overdispatch"] = dict(variant=tune.GEMM_VARIANTS[policy["gemm"]],
            tile_n=policy["tile_n"], stage_k=16, publication_interval=16,
            required_subgroup_size=32, dispatch_count=3)
        if buffers:
            for name, size in (("gemm-overdispatch-v0", 10240), ("gemm-overdispatch-v3", 22528),
                               ("gemm-overdispatch-v5", 45056), ("gemm-overdispatch-v5-dual", 22528)):
                filename = name + ".u8"
                (self.root / filename).write_bytes(bytes([0xA5]) * size)
                value["pairs"].append(dict(name=name, baseline=filename, candidate=filename))
            if value.get("model_free"):
                value["checks"] = len(value["pairs"])
                value["operators"] += 3
        return value

    def add_prototype_evidence(self, value, window_padding=True):
        for field, marker, tuples in (("extended_paired",tune.PAIRED_COVERAGE,tune.PAIRED_CASES),
                                      ("extended_window_padding",tune.WINDOW_PADDING_COVERAGE,tune.WINDOW_PADDING_CASES)):
            if field in value or (field=="extended_window_padding" and not window_padding):
                continue
            cases = []
            for values in tuples:
                if field == "extended_paired":
                    K,N,rows,flags,partition,batches = values
                    name = f"gemm-paired-K{K}-N{N}-R{rows}-F{flags}-P{partition}-B{batches}"
                    case = dict(name=name,K=K,N=N,rows=rows,flags=flags,partition=partition,batches=batches)
                    allocation = ((rows+63)//64)*64*(32+batches*N)
                    sizes = {name:allocation*(1 if flags & 16 else 2)}
                    if flags & 32: sizes[name+"-dual"] = allocation
                else:
                    width,height,heads,sx,sy = values
                    name = f"window-{width}x{height}-H{heads}-S{sx}x{sy}"
                    case = dict(name=name,width=width,height=height,heads=heads,shiftX=sx,shiftY=sy)
                    sizes = {name:((width*height+63)//64)*64*heads*32}
                cases.append(case)
                for buffer_name,size in sizes.items():
                    filename=buffer_name+".u8"
                    (self.root/filename).write_bytes(bytes([0xA5])*size)
                    value["pairs"].append(dict(name=buffer_name,baseline=filename,candidate=filename))
            value["coverage"].append(marker)
            value[field] = dict(coverage_marker=marker,case_count=len(cases),cases=cases)
            if value.get("model_free"):
                value["checks"] = len(value["pairs"])
                value["operators"] += len(cases)
        return value

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
        for gemm in ("packed", "direct", "direct-rte"):
            selected = tune.selected_policy({**legacy, "gemm": gemm})
            self.assertEqual(selected["gemm"], gemm)
            self.assertFalse(tune.evidence_equal(legacy, selected))
        for gemm in ("DIRECT", "", None, 1, []):
            with self.subTest(gemm=gemm), self.assertRaisesRegex(ValueError, "selected.gemm"):
                tune.selected_policy({**legacy, "gemm": gemm})
        for stage in (32, 64):
            with self.subTest(stage=stage), self.assertRaisesRegex(ValueError, "direct GEMM requires stage_k=16"):
                tune.selected_policy({**legacy, "gemm": "direct", "stage_k": stage})
            with self.subTest(stage=stage), self.assertRaisesRegex(ValueError, "direct GEMM requires stage_k=16"):
                tune.selected_policy({**legacy, "gemm": "direct-rte", "stage_k": stage})
            self.assertEqual(tune.selected_policy({**legacy, "gemm": "packed", "stage_k": stage})["stage_k"], stage)
        with self.assertRaisesRegex(ValueError, "scalar RTE"):
            tune.selected_policy({**legacy, "gemm": "direct-rte", "hardware_publication": True})

    def test_window_layout_requires_explicit_register_geometry_and_binds_legacy(self):
        legacy = self.record("candidate")["selected"]
        self.assertEqual(tune.selected_policy(legacy)["window_layout"], "staged")
        self.assertTrue(tune.evidence_equal(legacy, {**legacy, "window_layout": "staged"}))
        registered = tune.selected_policy({**legacy, "window_queries": 32, "window_layout": "register"})
        self.assertEqual(registered["window_layout"], "register")
        self.assertFalse(tune.evidence_equal(legacy, registered))
        for layout in ("register", "register-rte", "arena-rte"):
            for queries in (16, 32):
                selected = tune.selected_policy({**legacy, "window_queries": queries, "window_layout": layout})
                self.assertEqual(selected["window_queries"], queries)
                self.assertEqual(selected["window_layout"], layout)
            with self.assertRaisesRegex(ValueError, "optimized Q16/Q32"):
                tune.selected_policy({**legacy, "window_queries": 64, "window_layout": layout})
            with self.assertRaisesRegex(ValueError, "optimized Q16/Q32"):
                tune.selected_policy({**legacy, "kernels": "baseline", "window_queries": 32, "window_layout": layout})
        rte = tune.selected_policy({**legacy, "window_queries": 32, "window_layout": "register-rte"})
        self.assertFalse(tune.evidence_equal(registered, rte))
        for layout in ("REGISTER", "", None, 1, []):
            with self.subTest(layout=layout), self.assertRaisesRegex(ValueError, "window_layout"):
                tune.selected_policy({**legacy, "window_layout": layout})
        with self.assertRaisesRegex(ValueError, "optimized Q16/Q32"):
            tune.selected_policy({**legacy, "window_queries": 64, "window_layout": "register"})
        with self.assertRaisesRegex(ValueError, "optimized Q16/Q32"):
            tune.selected_policy({**legacy, "kernels": "baseline", "window_queries": 32, "window_layout": "register"})

    def test_direct32_anchor_cannot_inherit_register_attention_baseline(self):
        def override(value, role, pair):
            if role == "baseline": value["selected"]["window_layout"] = "register"
        with self.assertRaisesRegex(ValueError, "direct32 preserving"):
            tune.analyze(self.direct32_pairs(extra=override), comparison_anchor="direct32")

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

    def direct32_pairs(self, mode="bench", extra=None):
        def direct(value, role, pair):
            value["selected"]["gemm"] = "direct"
            if "dispatches" in value:
                value["dispatches"][0]["variant"] = "amd_gemm_direct"
            if extra: extra(value, role, pair)
        path = self.qualified32_pairs(mode, direct)
        value = tune.read_json(path); value["comparison_anchor"] = "direct32"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def register_artifacts(self, layout):
        gemm = "direct-rte" if layout == "register-rte" else "direct"
        window_variant = "amd_window_register_rte" if layout == "register-rte" else "amd_window_register"
        def policy(value, role, pair):
            candidate = role == "candidate"
            value["selected"].update(tile_n=16, gemm=gemm if candidate else "direct",
                                     window_layout=layout if candidate else "staged")
            if "dispatches" in value:
                dispatch = value["dispatches"][0]
                dispatch.update(tile_n=16, variant=tune.GEMM_VARIANTS[value["selected"]["gemm"]])
                window = copy.deepcopy(dispatch)
                window.update(family="window_attention", tile_n=64, stage_k=16,
                              variant=window_variant if candidate else "amd_window_small",
                              geometry={"tile_m":32})
                window["shape"].update(N=64, K=32)
                value["dispatches"].append(window)
        network = self.direct32_pairs(extra=policy)
        profile = self.direct32_pairs("profile", extra=policy)
        performance = self.write("register-performance.json", tune.analyze(
            profile, network_manifest=network, comparison_anchor="direct32"))
        paths = self.exact_suites()
        for path in paths:
            value = tune.read_json(path); value["comparison_anchor"] = "direct32"
            value["baseline_selected"].update(kernels="optimized", gemm="direct", window_queries=32,
                                              window_layout="staged")
            value["selected"].update(kernels="optimized", gemm=gemm, window_queries=32,
                                     window_layout=layout)
            path.write_text(json.dumps(value), encoding="utf-8")
        qualification = self.write("register-qualification.json", tune.qualify(argparse.Namespace(
            exact=paths, sequence=[], arithmetic="k16", comparison_anchor="direct32")))
        return performance, qualification

    def test_register_profiles_require_explicit_query_geometry(self):
        for layout, variant in (("register", "amd_window_register"),
                                ("register-rte", "amd_window_register_rte")):
            value = self.record("candidate", "profile")
            value["selected"].update(gemm="direct", window_queries=32, window_layout=layout)
            value["dispatches"][0].update(family="window_attention", variant=variant, tile_n=64)
            with self.subTest(layout=layout), self.assertRaisesRegex(ValueError, "record actual geometry.tile_m"):
                tune.benchmark(self.write("register-missing.json", value))

    def test_register_profiles_reject_tampered_query_geometry(self):
        for layout, variant in (("register", "amd_window_register"),
                                ("register-rte", "amd_window_register_rte")):
            for queries in (16, 64, True, "32"):
                value = self.record("candidate", "profile")
                value["selected"].update(gemm="direct", window_queries=32, window_layout=layout)
                value["dispatches"][0].update(family="window_attention", variant=variant, tile_n=64,
                                              geometry={"tile_m":queries})
                with self.subTest(layout=layout, queries=queries), self.assertRaisesRegex(ValueError, "geometry.tile_m"):
                    tune.benchmark(self.write("register-tampered.json", value))

    def test_register_profiles_accept_actual_q16_and_q32(self):
        for layout, variant in (("register", "amd_window_register"),
                                ("register-rte", "amd_window_register_rte")):
            for queries in (16, 32):
                value = self.record("candidate", "profile")
                value["selected"].update(gemm="direct", window_queries=queries, window_layout=layout)
                value["dispatches"][0].update(family="window_attention", variant=variant, tile_n=64,
                                              geometry={"tile_m":queries})
                parsed = tune.benchmark(self.write("register-valid.json", value))
                with self.subTest(layout=layout, queries=queries):
                    self.assertEqual(parsed["dispatches"][0]["geometry"]["tile_m"], queries)

    def test_tuning_generates_actual_register_and_rte_module_names(self):
        for layout, variant in (("register", "amd_window_register"),
                                ("register-rte", "amd_window_register_rte")):
            performance, qualification = self.register_artifacts(layout)
            result = tune.tuning(performance, qualification, "direct32")
            records = {record["variant"]:record for record in result["records"]}
            with self.subTest(layout=layout):
                self.assertIn(variant, records)
                self.assertEqual(records[variant]["window_queries"], 32)
                self.assertEqual(records[variant]["evidence"]["selected"]["window_layout"], layout)
                self.assertTrue(result["optimized_default_eligible"])
                if layout == "register-rte": self.assertIn("amd_gemm_direct_rte", records)

    def test_tuning_rejects_missing_or_tampered_register_query_geometry(self):
        for layout in ("register", "register-rte"):
            performance, qualification = self.register_artifacts(layout)
            original = tune.read_json(performance)
            for queries in (None, 16, 64, True):
                report = copy.deepcopy(original)
                window = next(operator for operator in report["operators"] if operator["family"] == "window_attention")
                variant = window["candidate_variants"][0]
                if queries is None: variant.pop("window_queries")
                else: variant["window_queries"] = queries
                performance.write_text(json.dumps(report), encoding="utf-8")
                # Reach the generation guard independently of raw-file reanalysis,
                # whose own malformed-profile rejection is covered above.
                with self.subTest(layout=layout, queries=queries), mock.patch.object(tune, "analyze", return_value=report):
                    with self.assertRaisesRegex(ValueError, "query geometry does not match"):
                        tune.tuning(performance, qualification, "direct32")

    def test_collect_register_and_rte_preserves_named_policies_and_direct32_baseline(self):
        model = self.root / "register-model"; model.mkdir()
        baseline_shaders = self.root / "register-baseline-shaders"; baseline_shaders.mkdir()
        for layout in ("register", "register-rte"):
            gemm = "direct-rte" if layout == "register-rte" else "direct"
            args = argparse.Namespace(executable=Path(sys.executable), baseline_executable=Path(sys.executable),
                                      model=model, shaders=None, baseline_shaders=baseline_shaders,
                                      output=self.root / ("register-out-" + layout), mode="profile", kernels="optimized",
                                      arithmetic="k16", gemm=gemm, tile_n=16, stage_k=16, width=1707, height=960,
                                      warmup=5, frames=30, pairs=3, timeout=60, allow_arithmetic_change=False,
                                      window_queries=32, window_layout=layout, comparison_anchor="direct32", fusion=False)
            calls = []
            def child(command, **kwargs):
                role = "baseline" if len(calls) % 2 == 0 else "candidate"
                actual_gemm = command[command.index("--amd-gemm") + 1]
                actual_layout = command[command.index("--amd-window-layout") + 1]
                self.assertEqual(actual_gemm, "direct" if role == "baseline" else gemm)
                self.assertEqual(actual_layout, "staged" if role == "baseline" else layout)
                self.assertEqual(kwargs["env"]["DLSS5VK_AMD_GEMM"], actual_gemm)
                self.assertEqual(kwargs["env"]["DLSS5VK_AMD_WINDOW_LAYOUT"], actual_layout)
                value = self.record(role, "profile")
                value["selected"].update(kernels="optimized", gemm=actual_gemm, window_queries=32,
                                          window_layout=actual_layout, **{key:False for key in tune.FUSION_KEYS})
                value["dispatches"][0]["variant"] = tune.GEMM_VARIANTS[actual_gemm]
                window = copy.deepcopy(value["dispatches"][0])
                window.update(family="window_attention", tile_n=64, geometry={"tile_m":32},
                              variant="amd_window_small" if role == "baseline" else
                              "amd_window_register_rte" if layout == "register-rte" else "amd_window_register")
                window["shape"].update(N=64, K=32)
                value["dispatches"].append(window)
                Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
                calls.append(command)
                return subprocess.CompletedProcess(command, 0)
            with mock.patch.object(tune.subprocess, "run", side_effect=child): manifest = tune.collect(args)
            with self.subTest(layout=layout):
                self.assertEqual(len(calls), 6)
                self.assertEqual(tune.read_json(manifest)["comparison_anchor"], "direct32")

    def test_direct32_anchor_measures_incremental_gain_from_alpha3_policy(self):
        report = tune.analyze(self.direct32_pairs(), comparison_anchor="direct32")
        self.assertEqual(report["selections"]["baseline"]["gemm"], "direct")
        self.assertEqual(report["network_median_ratio"], .9)
        self.assertTrue(report["default_performance_eligible"])

    def test_direct32_anchor_rejects_shared_or_unrecorded_baseline_route(self):
        for missing in (False, True):
            def wrong(value, role, pair):
                if role == "baseline":
                    if missing: value["selected"].pop("gemm")
                    else: value["selected"]["gemm"] = "shared"
            with self.assertRaisesRegex(ValueError, "direct32 preserving"):
                tune.analyze(self.direct32_pairs(extra=wrong), comparison_anchor="direct32")

    def test_collect_direct32_forces_direct_baseline_and_disables_fusion_overrides(self):
        model = self.root / "direct32-model"; model.mkdir()
        baseline_shaders = self.root / "direct32-baseline-shaders"; baseline_shaders.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), baseline_executable=Path(sys.executable),
                                  baseline_shaders=baseline_shaders, model=model, shaders=None,
                                  output=self.root / "direct32-out", mode="bench", kernels="optimized", arithmetic="k16",
                                  gemm="direct", tile_n=32, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="direct32", fusion=True)
        calls = []
        def child(command, **kwargs):
            role = "baseline" if len(calls) % 2 == 0 else "candidate"
            selected = {"kernels": command[command.index("--amd-kernels") + 1], "arithmetic": "k16",
                        "gemm": command[command.index("--amd-gemm") + 1],
                        "tile_n": int(command[command.index("--amd-tile-n") + 1]),
                        "stage_k": int(command[command.index("--amd-stage-k") + 1]),
                        "window_queries": int(command[command.index("--amd-window-queries") + 1]),
                        **{key: kwargs["env"]["DLSS5VK_AMD_" + key.upper()] == "1" for key in tune.FUSION_KEYS}}
            self.assertEqual(kwargs["env"]["DLSS5VK_AMD_GEMM"], "direct")
            calls.append(selected)
            value = self.record(role); value["selected"] = selected
            Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(tune.subprocess, "run", side_effect=child): manifest = tune.collect(args)
        self.assertTrue(all(tune.preserving_baseline(value, "direct32") for value in calls[::2]))
        self.assertTrue(all(value["fusion"] for value in calls[1::2]))
        self.assertEqual(tune.read_json(manifest)["comparison_anchor"], "direct32")
        self.assertIn("policy", tune.read_json(manifest)["comparison_anchor_scope"])
        self.assertIn("independent release-identity proof", tune.read_json(manifest)["comparison_anchor_scope"])

    def test_collect_direct32_rejects_implicit_baseline_before_output_or_child(self):
        model = self.root / "explicit-baseline-model"; model.mkdir()
        baseline_shaders = self.root / "explicit-baseline-shaders"; baseline_shaders.mkdir()
        for missing in ("baseline_executable", "baseline_shaders", "both"):
            args = argparse.Namespace(executable=Path(sys.executable), model=model, shaders=None,
                                      output=self.root / ("missing-" + missing), comparison_anchor="direct32",
                                      baseline_executable=Path(sys.executable), baseline_shaders=baseline_shaders)
            if missing == "both":
                del args.baseline_executable; del args.baseline_shaders
            else:
                setattr(args, missing, None)
            with self.subTest(missing=missing), mock.patch.object(tune.subprocess, "run") as child:
                with self.assertRaisesRegex(ValueError, "requires explicit --baseline-executable and --baseline-shaders"):
                    tune.collect(args)
                child.assert_not_called()
                self.assertFalse(args.output.exists())

    def test_collect_binds_frozen_baseline_and_candidate_tools_and_shader_paths(self):
        model = self.root / "two-model"; model.mkdir()
        baseline_exe = self.root / "frozen.exe"; baseline_exe.write_bytes(b"frozen binary fixture")
        baseline_shaders = self.root / "old-shaders"; baseline_shaders.mkdir()
        candidate_shaders = self.root / "new-shaders"; candidate_shaders.mkdir()
        args = argparse.Namespace(executable=Path(sys.executable), baseline_executable=baseline_exe,
                                  model=model, shaders=candidate_shaders, baseline_shaders=baseline_shaders,
                                  output=self.root / "two-out", mode="bench", kernels="optimized", arithmetic="k16",
                                  gemm="direct", tile_n=16, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="direct32", fusion=False)
        calls = []
        def child(command, **kwargs):
            role = "baseline" if len(calls) % 2 == 0 else "candidate"
            expected_exe = baseline_exe if role == "baseline" else Path(sys.executable)
            expected_shaders = baseline_shaders if role == "baseline" else candidate_shaders
            self.assertEqual(command[0], str(expected_exe.resolve()))
            self.assertEqual(kwargs["cwd"], str(expected_exe.resolve().parent))
            self.assertEqual(command[command.index("--shaders") + 1], str(expected_shaders.resolve()))
            value = self.record(role)
            value["selected"].update(kernels="optimized", gemm="direct", window_queries=32,
                                      **{key: False for key in tune.FUSION_KEYS})
            Path(command[command.index("--json") + 1]).write_text(json.dumps(value), encoding="utf-8")
            calls.append(command)
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(tune.subprocess, "run", side_effect=child): manifest = tune.collect(args)
        records = tune.read_json(manifest)["runs"]
        self.assertTrue(all(item["executable_sha256"] == tune.sha256(baseline_exe) for item in records[::2]))
        self.assertTrue(all(item["executable_sha256"] == tune.sha256(Path(sys.executable)) for item in records[1::2]))

    def test_collect_rejects_replaced_frozen_executable_after_child(self):
        executable = self.root / "mutable.exe"; executable.write_bytes(b"initial binary fixture")
        model = self.root / "mutable-model"; model.mkdir()
        baseline_shaders = self.root / "mutable-baseline-shaders"; baseline_shaders.mkdir()
        args = argparse.Namespace(executable=executable, baseline_executable=executable,
                                  baseline_shaders=baseline_shaders, model=model, shaders=None,
                                  output=self.root / "mutable-out", mode="bench", kernels="optimized", arithmetic="k16",
                                  gemm="direct", tile_n=16, stage_k=16, width=1707, height=960, warmup=5, frames=30,
                                  pairs=3, timeout=60, allow_arithmetic_change=False, window_queries=32,
                                  comparison_anchor="direct32", fusion=False)
        def child(command, **kwargs):
            executable.write_bytes(b"changed after child")
            return subprocess.CompletedProcess(command, 0)
        with mock.patch.object(tune.subprocess, "run", side_effect=child):
            with self.assertRaisesRegex(ValueError, "baseline executable changed during collection"):
                tune.collect(args)
        self.assertFalse((args.output / "interleaved.json").exists())

    def test_direct32_anchor_binds_profile_and_all_exact_suites(self):
        network, profile = self.direct32_pairs(), self.direct32_pairs("profile")
        performance = self.write("direct32-performance.json", tune.analyze(
            profile, network_manifest=network, comparison_anchor="direct32"))
        paths = self.exact_suites()
        for path in paths:
            value = tune.read_json(path); value["comparison_anchor"] = "direct32"
            value["selected"].update(kernels="optimized", gemm="direct", window_queries=32, tile_n=32)
            value["baseline_selected"].update(kernels="optimized", gemm="direct", window_queries=32)
            path.write_text(json.dumps(value), encoding="utf-8")
        qualification = self.write("direct32-qualification.json", tune.qualify(argparse.Namespace(
            exact=paths, sequence=[], arithmetic="k16", comparison_anchor="direct32")))
        result = tune.tuning(performance, qualification, "direct32")
        self.assertEqual(result["comparison_anchor"], "direct32")
        self.assertEqual(result["records"][0]["comparison_anchor"], "direct32")
        self.assertEqual(result["records"][0]["evidence"]["selected"]["gemm"], "direct")

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


    def rte32_pairs(self, mode="bench", candidate_gemm="direct-rte-init", extra=None):
        def rte(value, role, pair):
            gemm = "direct-rte" if role == "baseline" else candidate_gemm
            value["selected"].update(gemm=gemm, window_layout="register-rte")
            if "dispatches" in value:
                value["dispatches"][0].update(variant=tune.GEMM_VARIANTS[gemm],
                    geometry={"threads":128,"required_subgroup_size":32,"tile_m":64})
            if extra: extra(value, role, pair)
        path = self.direct32_pairs(mode, rte)
        value = tune.read_json(path); value["comparison_anchor"] = "rte32"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_rte32_preserves_frozen_policy_without_changing_previous_anchors(self):
        for gemm in ("direct-rte-init", "direct-rte-epilogue"):
            report = tune.analyze(self.rte32_pairs(candidate_gemm=gemm), comparison_anchor="rte32")
            with self.subTest(gemm=gemm):
                self.assertEqual(report["selections"]["baseline"]["gemm"], "direct-rte")
                self.assertEqual(report["selections"]["baseline"]["window_layout"], "register-rte")
                self.assertEqual(report["selections"]["candidate"]["gemm"], gemm)
                self.assertTrue(report["default_performance_eligible"])
                with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
                    tune.analyze(self.rte32_pairs(candidate_gemm=gemm), comparison_anchor="direct32")
        self.assertEqual(tune.anchor_gemm("direct32"), "direct")
        self.assertEqual(tune.anchor_window_layout("direct32"), "staged")
        self.assertEqual(tune.anchor_gemm("qualified32"), "shared")
        self.assertEqual(tune.anchor_queries("compact64"), 64)

    def test_rte32_rejects_inherited_or_changed_baseline_policy(self):
        for key, replacement in (("gemm","direct"),("gemm","direct-rte-init"),
                                 ("window_layout","staged"),("window_layout","register"),
                                 ("window_queries",16),("tile_n",32),("fusion",True)):
            def wrong(value, role, pair):
                if role == "baseline":
                    value["selected"][key] = replacement
                    if key == "fusion":
                        value["selected"].update(ffn32_fusion=True,qkv32_fusion=True)
            with self.subTest(key=key,value=replacement), self.assertRaisesRegex(ValueError,"rte32 preserving"):
                tune.analyze(self.rte32_pairs(extra=wrong),comparison_anchor="rte32")
        for key in ("gemm", "window_layout"):
            def missing(value,role,pair):
                if role == "baseline":value["selected"].pop(key)
            with self.subTest(missing=key),self.assertRaisesRegex(ValueError,"rte32 preserving"):
                tune.analyze(self.rte32_pairs(extra=missing),comparison_anchor="rte32")

    def test_new_rte_variants_require_staging_publication_and_actual_profile_resources(self):
        for gemm in ("direct-rte-init", "direct-rte-epilogue"):
            policy = {**self.record("candidate")["selected"],"gemm":gemm}
            self.assertEqual(tune.selected_policy(policy)["gemm"],gemm)
            for stage in (32,64):
                with self.subTest(gemm=gemm,stage=stage),self.assertRaisesRegex(ValueError,"stage_k=16"):
                    tune.selected_policy({**policy,"stage_k":stage})
            with self.subTest(gemm=gemm),self.assertRaisesRegex(ValueError,"scalar RTE"):
                tune.selected_policy({**policy,"hardware_publication":True})
            for tamper in ("variant","tile_n","stage_k","threads","required_subgroup_size","tile_m","missing"):
                value=self.record("candidate","profile")
                value["selected"].update(policy)
                dispatch=value["dispatches"][0]
                dispatch.update(variant=tune.GEMM_VARIANTS[gemm],geometry={"threads":128,"required_subgroup_size":32,"tile_m":64})
                if tamper=="missing":dispatch.pop("geometry")
                elif tamper in ("threads","required_subgroup_size","tile_m"):dispatch["geometry"][tamper]=16
                else:dispatch[tamper]="amd_gemm_direct_rte" if tamper=="variant" else 32
                with self.subTest(gemm=gemm,tamper=tamper),self.assertRaisesRegex(ValueError,"module/resources"):
                    tune.benchmark(self.write("new-rte-malformed-profile.json",value))

    def test_new_rte_variant_profile_qualification_emits_exact_module(self):
        for gemm in ("direct-rte-init", "direct-rte-epilogue"):
            network=self.rte32_pairs(candidate_gemm=gemm)
            profile=self.rte32_pairs("profile",candidate_gemm=gemm)
            performance=self.write("new-rte-performance.json",tune.analyze(profile,network_manifest=network,comparison_anchor="rte32"))
            exact=self.exact_suites()
            for path in exact:
                value=tune.read_json(path);value["comparison_anchor"]="rte32"
                value["baseline_selected"].update(kernels="optimized",gemm="direct-rte",window_queries=32,window_layout="register-rte")
                value["selected"].update(kernels="optimized",gemm=gemm,window_queries=32,window_layout="register-rte",tile_n=32)
                if value["suite"] == "operators":
                    self.add_vit_evidence(value)
                    self.add_raw_overdispatch_evidence(value)
                path.write_text(json.dumps(value),encoding="utf-8")
            qualification=self.write("new-rte-qualification.json",tune.qualify(argparse.Namespace(exact=exact,sequence=[],arithmetic="k16",comparison_anchor="rte32")))
            result=tune.tuning(performance,qualification,"rte32")
            self.assertEqual(result["records"][0]["variant"],tune.GEMM_VARIANTS[gemm])
            self.assertEqual(result["default_selection"]["gemm"],gemm)
            self.assertEqual(result["default_selection"]["window_layout"],"register-rte")
            # A predecessor's module name cannot qualify a new selected route.
            original=tune.read_json(performance)
            original["operators"][0]["candidate_variants"][0]["variant"]="amd_gemm_direct_rte"
            performance.write_text(json.dumps(original),encoding="utf-8")
            with mock.patch.object(tune,"analyze",return_value=original),self.assertRaisesRegex(ValueError,"GEMM dispatch variant"):
                tune.tuning(performance,qualification,"rte32")

    def test_new_rte_model_free_proof_binds_actual_raw_overdispatch(self):
        for gemm in ("direct-rte-init", "direct-rte-epilogue"):
            path=self.exact_suites()[0];value=tune.read_json(path)
            value.update(comparison_anchor="rte32",model_free=True,passed=True,validationErrors=0,
                         mismatchedChecks=0,checks=len(value["pairs"]),operators=9,fixture_sha256="e"*64)
            value["identity"].pop("model_sha256");value["baseline_identity"].pop("model_sha256")
            value["baseline_selected"].update(kernels="optimized",gemm="direct-rte",window_queries=32,window_layout="register-rte")
            value["selected"].update(kernels="optimized",gemm=gemm,window_queries=32,window_layout="register-rte")
            path.write_text(json.dumps(value),encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"raw_gemm_overdispatch provenance"):tune.exact_manifest(path,"rte32")
            raw={"variant":tune.GEMM_VARIANTS[gemm],"tile_n":16,"stage_k":16,"publication_interval":16,
                 "required_subgroup_size":32,"dispatch_count":3}
            value["raw_gemm_overdispatch"]=raw
            path.write_text(json.dumps(value),encoding="utf-8")
            with self.assertRaisesRegex(ValueError,"requires extended ViT"):
                tune.exact_manifest(path,"rte32")
            self.add_vit_evidence(value)
            self.add_raw_overdispatch_evidence(value)
            path.write_text(json.dumps(value),encoding="utf-8")
            report=tune.exact_manifest(path,"rte32")
            self.assertTrue(report["passed"])
            self.assertEqual(report["raw_gemm_overdispatch"],raw)
            for key,wrong in (("variant","amd_gemm_direct"),("tile_n",32),("stage_k",32),
                              ("publication_interval",32),("required_subgroup_size",64),("dispatch_count",True)):
                altered=copy.deepcopy(value);altered["raw_gemm_overdispatch"][key]=wrong
                path.write_text(json.dumps(altered),encoding="utf-8")
                with self.subTest(gemm=gemm,key=key),self.assertRaisesRegex(ValueError,"module/resources"):
                    tune.exact_manifest(path,"rte32")

    def test_extended_vit_requires_bound_depth_partition_flags_and_buffers(self):
        for gemm in ("direct-rte-init", "direct-rte-epilogue"):
            path = self.exact_suites()[0]
            value = tune.read_json(path)
            value["comparison_anchor"] = "rte32"
            value["baseline_selected"].update(kernels="optimized", gemm="direct-rte",
                                               window_queries=32, window_layout="register-rte")
            value["selected"].update(kernels="optimized", gemm=gemm,
                                      window_queries=32, window_layout="register-rte")
            self.add_raw_overdispatch_evidence(value, buffers=False)
            path.write_text(json.dumps(value), encoding="utf-8")
            with self.subTest(gemm=gemm), self.assertRaisesRegex(ValueError, "requires extended ViT"):
                tune.exact_manifest(path, "rte32")
            self.add_vit_evidence(value)
            self.add_raw_overdispatch_evidence(value)
            path.write_text(json.dumps(value), encoding="utf-8")
            report = tune.exact_manifest(path, "rte32")
            self.assertTrue(report["passed"])
            self.assertEqual(report["extended_vit"], value["extended_vit"])
            self.assertEqual(sum(pair["name"].startswith("gemm-vit-") for pair in value["pairs"]), 18)
            # Neither a marker alone nor altered geometry may stand in for execution.
            mutations = [
                ("marker-only", lambda item: item.pop("extended_vit")),
                ("metadata-only", lambda item: item["coverage"].remove(tune.VIT_COVERAGE)),
                ("count-bool", lambda item: item["extended_vit"].update(case_count=True)),
                ("count-missing", lambda item: item["extended_vit"].pop("case_count")),
                ("duplicate", lambda item: item["extended_vit"]["cases"].__setitem__(1, copy.deepcopy(item["extended_vit"]["cases"][0]))),
                ("name-type", lambda item: item["extended_vit"]["cases"][0].update(name=[])),
                ("depth", lambda item: item["extended_vit"]["cases"][0].update(K=512)),
                ("partition", lambda item: item["extended_vit"]["cases"][0].update(partition=128)),
                ("flags", lambda item: item["extended_vit"]["cases"][0].update(flags=16)),
                ("batches-type", lambda item: item["extended_vit"]["cases"][0].update(batches=True)),
                ("primary-missing", lambda item: item["pairs"].pop(len(tune.EXACT_COVERAGE))),
                ("dual-missing", lambda item: item["pairs"].__setitem__(slice(None), [pair for pair in item["pairs"] if pair["name"] != item["extended_vit"]["cases"][4]["name"] + "-dual"])),
                ("wrong-allocation", lambda item: item["pairs"][len(tune.EXACT_COVERAGE)].update(candidate="cand.u8")),
            ]
            for label, mutate in mutations:
                altered = copy.deepcopy(value)
                mutate(altered)
                path.write_text(json.dumps(altered), encoding="utf-8")
                with self.subTest(gemm=gemm, mutation=label), self.assertRaisesRegex(ValueError, "extended ViT"):
                    tune.exact_manifest(path, "rte32")

    def test_historical_operator_evidence_remains_valid_without_vit_marker(self):
        path = self.exact_suites()[0]
        value = tune.read_json(path)
        value["comparison_anchor"] = "rte32"
        value["baseline_selected"].update(kernels="optimized", gemm="direct-rte",
                                           window_queries=32, window_layout="register-rte")
        value["selected"].update(kernels="optimized", gemm="direct-rte",
                                  window_queries=32, window_layout="register-rte")
        path.write_text(json.dumps(value), encoding="utf-8")
        report = tune.exact_manifest(path, "rte32")
        self.assertTrue(report["passed"])
        self.assertNotIn("extended_vit", report)

    def test_new_raw_overdispatch_proof_cannot_bypass_requirement_with_model_free_label(self):
        for gemm in ("direct-rte-init", "direct-rte-epilogue"):
            value = tune.read_json(self.exact_suites()[0])
            value["comparison_anchor"] = "rte32"
            value["baseline_selected"].update(kernels="optimized", gemm="direct-rte",
                                               window_queries=32, window_layout="register-rte")
            value["selected"].update(kernels="optimized", gemm=gemm,
                                      window_queries=32, window_layout="register-rte")
            self.add_vit_evidence(value)
            self.add_raw_overdispatch_evidence(value)
            value.update(model_free=True, fixture_sha256="e" * 64, passed=True,
                         validationErrors=0, mismatchedChecks=0, checks=len(value["pairs"]), operators=26)
            for model_free in (True, False):
                complete = copy.deepcopy(value)
                complete["model_free"] = model_free
                path = self.write("raw-label.json", complete)
                self.assertTrue(tune.exact_manifest(path, "rte32")["passed"])
                for raw in ("missing", None, {}, []):
                    altered = copy.deepcopy(complete)
                    if raw == "missing":
                        altered.pop("raw_gemm_overdispatch")
                    else:
                        altered["raw_gemm_overdispatch"] = raw
                    with self.subTest(gemm=gemm, model_free=model_free, raw=raw), self.assertRaisesRegex(ValueError, "raw_gemm_overdispatch provenance|module/resources"):
                        tune.exact_manifest(self.write("raw-label.json", altered), "rte32")
                altered = copy.deepcopy(complete)
                altered["raw_gemm_overdispatch"]["variant"] = "amd_gemm_direct_rte"
                with self.subTest(gemm=gemm, model_free=model_free), self.assertRaisesRegex(ValueError, "module/resources"):
                    tune.exact_manifest(self.write("raw-label.json", altered), "rte32")

    def test_raw_overdispatch_requires_four_executed_full_buffers_for_every_report(self):
        # A legacy route remains compatible without metadata. Once metadata is
        # supplied, its claimed execution has the same buffer contract.
        names = ("gemm-overdispatch-v0", "gemm-overdispatch-v3",
                 "gemm-overdispatch-v5", "gemm-overdispatch-v5-dual")
        for gemm in ("direct-rte", "direct-rte-init", "direct-rte-epilogue"):
            value = tune.read_json(self.exact_suites()[0])
            value["comparison_anchor"] = "rte32"
            value["baseline_selected"].update(kernels="optimized", gemm="direct-rte",
                                               window_queries=32, window_layout="register-rte")
            value["selected"].update(kernels="optimized", gemm=gemm,
                                      window_queries=32, window_layout="register-rte")
            if gemm != "direct-rte":
                self.add_vit_evidence(value)
            self.add_raw_overdispatch_evidence(value)
            value.update(fixture_sha256="e" * 64, passed=True, validationErrors=0,
                         mismatchedChecks=0, checks=len(value["pairs"]), operators=26)
            for model_free in (True, False):
                complete = copy.deepcopy(value)
                complete["model_free"] = model_free
                self.assertTrue(tune.exact_manifest(self.write("raw-buffers.json", complete), "rte32")["passed"])
                for removed in (*[(name,) for name in names], names):
                    altered = copy.deepcopy(complete)
                    altered["pairs"] = [pair for pair in altered["pairs"] if pair["name"] not in removed]
                    altered["checks"] = len(altered["pairs"])
                    with self.subTest(gemm=gemm, model_free=model_free, removed=removed), self.assertRaisesRegex(ValueError, "raw GEMM overdispatch executed output/dual"):
                        tune.exact_manifest(self.write("raw-buffers.json", altered), "rte32")
                for name in names:
                    for role in ("baseline", "candidate"):
                        altered = copy.deepcopy(complete)
                        pair = next(pair for pair in altered["pairs"] if pair["name"] == name)
                        pair[role] = "base.u8"
                        with self.subTest(gemm=gemm, model_free=model_free, name=name, role=role), self.assertRaisesRegex(ValueError, "raw GEMM overdispatch executed output/dual"):
                            tune.exact_manifest(self.write("raw-buffers.json", altered), "rte32")

    def test_collect_rte32_requires_explicit_frozen_baseline_before_child_or_output(self):
        model=self.root/"rte-model";model.mkdir()
        baseline_shaders=self.root/"rte-baseline";baseline_shaders.mkdir()
        for missing in ("baseline_executable","baseline_shaders","both"):
            args=argparse.Namespace(executable=Path(sys.executable),model=model,shaders=None,
                 output=self.root/("rte-missing-"+missing),comparison_anchor="rte32",
                 baseline_executable=Path(sys.executable),baseline_shaders=baseline_shaders)
            if missing=="both":del args.baseline_executable;del args.baseline_shaders
            else:setattr(args,missing,None)
            with self.subTest(missing=missing),mock.patch.object(tune.subprocess,"run") as child:
                with self.assertRaisesRegex(ValueError,"requires explicit --baseline-executable and --baseline-shaders"):
                    tune.collect(args)
                child.assert_not_called();self.assertFalse(args.output.exists())

    def test_collect_rte32_forces_frozen_route_and_preserves_candidate_module(self):
        model=self.root/"rte-collect-model";model.mkdir()
        baseline_exe=self.root/"alpha4.exe";baseline_exe.write_bytes(b"frozen alpha4 test binary")
        baseline_shaders=self.root/"alpha4-shaders";baseline_shaders.mkdir()
        candidate_shaders=self.root/"candidate-shaders";candidate_shaders.mkdir()
        for gemm in ("direct-rte-init","direct-rte-epilogue"):
            args=argparse.Namespace(executable=Path(sys.executable),baseline_executable=baseline_exe,
                model=model,shaders=candidate_shaders,baseline_shaders=baseline_shaders,
                output=self.root/("rte-collect-"+gemm),mode="profile",kernels="optimized",arithmetic="k16",
                gemm=gemm,tile_n=32,stage_k=16,width=1707,height=960,warmup=5,frames=30,pairs=3,
                timeout=60,allow_arithmetic_change=False,window_queries=32,window_layout="register-rte",
                comparison_anchor="rte32",fusion=True)
            calls=[]
            def child(command,**kwargs):
                role="baseline" if len(calls)%2==0 else "candidate"
                expected_gemm="direct-rte" if role=="baseline" else gemm
                expected_exe=baseline_exe if role=="baseline" else Path(sys.executable)
                expected_shaders=baseline_shaders if role=="baseline" else candidate_shaders
                self.assertEqual(command[0],str(expected_exe.resolve()))
                self.assertEqual(command[command.index("--shaders")+1],str(expected_shaders.resolve()))
                self.assertEqual(command[command.index("--amd-gemm")+1],expected_gemm)
                self.assertEqual(command[command.index("--amd-window-layout")+1],"register-rte")
                self.assertEqual(kwargs["env"]["DLSS5VK_AMD_GEMM"],expected_gemm)
                value=self.record(role,"profile")
                value["selected"].update(kernels="optimized",gemm=expected_gemm,window_queries=32,
                    window_layout="register-rte",tile_n=16 if role=="baseline" else 32,
                    **{key:kwargs["env"]["DLSS5VK_AMD_"+key.upper()]=="1" for key in tune.FUSION_KEYS})
                value["dispatches"][0].update(variant=tune.GEMM_VARIANTS[expected_gemm],
                    tile_n=value["selected"]["tile_n"],geometry={"threads":128,"required_subgroup_size":32,"tile_m":64})
                Path(command[command.index("--json")+1]).write_text(json.dumps(value),encoding="utf-8")
                calls.append(value["selected"]);return subprocess.CompletedProcess(command,0)
            with mock.patch.dict(os.environ,{"DLSS5VK_AMD_WINDOW_LAYOUT":"staged"}),mock.patch.object(tune.subprocess,"run",side_effect=child):
                path=tune.collect(args)
                self.assertEqual(os.environ["DLSS5VK_AMD_WINDOW_LAYOUT"],"staged")
            manifest=tune.read_json(path)
            self.assertTrue(all(tune.preserving_baseline(policy,"rte32") for policy in calls[::2]))
            self.assertTrue(all(policy["fusion"] for policy in calls[1::2]))
            self.assertEqual(manifest["comparison_anchor"],"rte32")
            self.assertIn("alpha 4",manifest["comparison_anchor_scope"])
            self.assertIn("independent release-identity proof",manifest["comparison_anchor_scope"])
            self.assertTrue(all(run["executable_sha256"]==tune.sha256(baseline_exe) for run in manifest["runs"][::2]))

    def prototype_artifacts(self, gemm="direct-rte-pair", layout="arena-rte"):
        def policy(value,role,pair):
            candidate=role=="candidate"
            value["selected"].update(tile_n=16,window_layout=layout if candidate else "register-rte")
            if "dispatches" in value:
                dispatch=value["dispatches"][0];dispatch["tile_n"]=16
                window=copy.deepcopy(dispatch)
                window.update(family="window_attention",variant=tune.window_variant(value["selected"]),
                    tile_n=64,geometry={"threads":128,"required_subgroup_size":32,"tile_m":32})
                window["shape"].update(N=64,K=32)
                value["dispatches"].append(window)
        network=self.rte32_pairs(candidate_gemm=gemm,extra=policy)
        profile=self.rte32_pairs("profile",candidate_gemm=gemm,extra=policy)
        performance=self.write("prototype-performance.json",tune.analyze(profile,network_manifest=network,comparison_anchor="rte32"))
        exact=self.exact_suites()
        for path in exact:
            value=tune.read_json(path);value["comparison_anchor"]="rte32"
            value["baseline_selected"].update(kernels="optimized",gemm="direct-rte",window_queries=32,window_layout="register-rte")
            value["selected"].update(kernels="optimized",gemm=gemm,window_queries=32,window_layout=layout)
            if value["suite"]=="operators":
                if gemm in tune.EXTENDED_GEMMS:
                    self.add_vit_evidence(value);self.add_raw_overdispatch_evidence(value)
                self.add_prototype_evidence(value)
            path.write_text(json.dumps(value),encoding="utf-8")
        qualification=self.write("prototype-qualification.json",tune.qualify(argparse.Namespace(
            exact=exact,sequence=[],arithmetic="k16",comparison_anchor="rte32")))
        return performance,qualification,exact

    def test_pair_selector_requires_n16_and_staging_without_changing_arithmetic_modes(self):
        policy={**self.record("candidate")["selected"],"gemm":"direct-rte-pair"}
        for arithmetic in ("k16","k32","final"):
            self.assertEqual(tune.selected_policy({**policy,"arithmetic":arithmetic})["gemm"],"direct-rte-pair")
        for tile in (32,64):
            with self.subTest(tile=tile),self.assertRaisesRegex(ValueError,"tile_n=16"):
                tune.selected_policy({**policy,"tile_n":tile})
        for stage in (32,64):
            with self.subTest(stage=stage),self.assertRaisesRegex(ValueError,"stage_k=16"):
                tune.selected_policy({**policy,"stage_k":stage})
        with self.assertRaisesRegex(ValueError,"scalar RTE"):
            tune.selected_policy({**policy,"hardware_publication":True})

    def test_arena_profile_requires_actual_module_wave_and_query_geometry(self):
        for queries in (16,32):
            value=self.record("candidate","profile")
            value["selected"].update(window_layout="arena-rte",window_queries=queries)
            dispatch=value["dispatches"][0]
            dispatch.update(family="window_attention",variant="amd_window_arena_rte",tile_n=64,
                geometry={"threads":128,"required_subgroup_size":32,"tile_m":queries})
            self.assertEqual(tune.benchmark(self.write("arena-profile.json",value))["selected"]["window_layout"],"arena-rte")
            for field in ("tile_m","threads","required_subgroup_size","variant"):
                for missing in (True,False):
                    altered=copy.deepcopy(value);entry=altered["dispatches"][0]
                    target=entry if field=="variant" else entry["geometry"]
                    if missing:target.pop(field)
                    else:target[field]="amd_window_register_rte" if field=="variant" else 64
                    with self.subTest(queries=queries,field=field,missing=missing),self.assertRaises(ValueError):
                        tune.benchmark(self.write("arena-malformed.json",altered))

    def test_pair_profile_requires_actual_module_resources(self):
        value=self.record("candidate","profile")
        value["selected"]["gemm"]="direct-rte-pair"
        value["dispatches"][0].update(variant="amd_gemm_direct_rte_pair",
            geometry={"threads":128,"required_subgroup_size":32,"tile_m":64})
        self.assertEqual(tune.benchmark(self.write("pair-profile.json",value))["selected"]["gemm"],"direct-rte-pair")
        for field in ("threads","required_subgroup_size","tile_m","variant","tile_n","stage_k"):
            altered=copy.deepcopy(value);entry=altered["dispatches"][0]
            target=entry["geometry"] if field in ("threads","required_subgroup_size","tile_m") else entry
            target[field]="amd_gemm_direct_rte_epilogue" if field=="variant" else 64 if field=="required_subgroup_size" else 32
            with self.subTest(field=field),self.assertRaisesRegex(ValueError,"module/resources"):
                tune.benchmark(self.write("pair-malformed.json",altered))

    def test_prototype_tuning_binds_separate_modules_and_unchanged_alpha4_anchor(self):
        for gemm,layout in (("direct-rte-pair","register-rte"),("direct-rte","arena-rte"),("direct-rte-pair","arena-rte")):
            performance,qualification,_=self.prototype_artifacts(gemm,layout)
            result=tune.tuning(performance,qualification,"rte32")
            self.assertEqual(result["default_selection"]["gemm"],gemm)
            self.assertEqual(result["default_selection"]["window_layout"],layout)
            variants={entry["variant"] for entry in result["records"]}
            if gemm=="direct-rte-pair": self.assertIn(tune.GEMM_VARIANTS[gemm],variants)
            if layout=="arena-rte": self.assertIn(tune.window_variant(result["default_selection"]),variants)
            report=tune.read_json(performance)
            self.assertTrue(tune.preserving_baseline(report["selections"]["baseline"],"rte32"))
            for family,wrong in (("fp8_gemm","amd_gemm_direct_rte_epilogue"),
                                 ("window_attention","amd_window_register")):
                altered=copy.deepcopy(report)
                operator=next(entry for entry in altered["operators"] if entry["family"]==family)
                operator["candidate_variants"][0]["variant"]=wrong
                operator["performance_eligible"]=True
                performance.write_text(json.dumps(altered),encoding="utf-8")
                with self.subTest(gemm=gemm,layout=layout,family=family),mock.patch.object(tune,"analyze",return_value=altered):
                    with self.assertRaisesRegex(ValueError,"dispatch variant"):
                        tune.tuning(performance,qualification,"rte32")
                performance.write_text(json.dumps(report),encoding="utf-8")

    def test_prototype_operator_extensions_require_executed_shapes_and_full_buffers(self):
        _,_,exact=self.prototype_artifacts()
        path=exact[0];original=tune.read_json(path)
        self.assertTrue(tune.exact_manifest(path,"rte32")["passed"])
        for field,marker in (("extended_paired",tune.PAIRED_COVERAGE),("extended_window_padding",tune.WINDOW_PADDING_COVERAGE)):
            mutations=[("missing",lambda value:value.pop(field)),
                ("marker",lambda value:value["coverage"].remove(marker)),
                ("count",lambda value:value[field].update(case_count=True)),
                ("duplicate",lambda value:value[field]["cases"].__setitem__(1,copy.deepcopy(value[field]["cases"][0]))),
                ("shape",lambda value:value[field]["cases"][0].update(**({"K":128} if field=="extended_paired" else {"width":2}))),
                ("missing-buffer",lambda value:value["pairs"].__setitem__(slice(None),[pair for pair in value["pairs"] if pair["name"]!=value[field]["cases"][0]["name"]])),
                ("allocation",lambda value:next(pair for pair in value["pairs"] if pair["name"]==value[field]["cases"][0]["name"]).update(candidate="cand.u8"))]
            for label,mutate in mutations:
                value=copy.deepcopy(original);mutate(value)
                path.write_text(json.dumps(value),encoding="utf-8")
                with self.subTest(field=field,mutation=label),self.assertRaisesRegex(ValueError,"paired-preload|thin-window"):
                    tune.exact_manifest(path,"rte32")
        value=copy.deepcopy(original)
        dual=next(case["name"]+"-dual" for case in value["extended_paired"]["cases"] if case["flags"]&32)
        value["pairs"]=[pair for pair in value["pairs"] if pair["name"]!=dual]
        with self.assertRaisesRegex(ValueError,"paired-preload.*allocation"):
            tune.exact_manifest(self.write("pair-missing-dual.json",value),"rte32")

    def test_repaired_init_epilogue_pair_reject_historical_880_operator_scope(self):
        for gemm in sorted(tune.EXTENDED_GEMMS):
            _,_,exact=self.prototype_artifacts(gemm,"register-rte")
            value=tune.read_json(exact[0])
            self.assertTrue(tune.exact_manifest(exact[0],"rte32")["passed"])
            value.pop("extended_paired")
            value["coverage"].remove(tune.PAIRED_COVERAGE)
            value["pairs"]=[pair for pair in value["pairs"] if not pair["name"].startswith("gemm-paired-")]
            with self.subTest(gemm=gemm),self.assertRaisesRegex(ValueError,"requires paired-preload"):
                tune.exact_manifest(self.write("historical-880.json",value),"rte32")

    def test_prototype_selectors_reject_unsafe_geometry_before_children_or_output(self):
        for action in (tune.collect,tune.replay_sequence):
            for gemm,tile,layout,queries in (("direct-rte-pair",32,"arena-rte",32),
                                            ("direct-rte",16,"arena-rte",64)):
                args=argparse.Namespace(gemm=gemm,tile_n=tile,stage_k=16,window_layout=layout,window_queries=queries,
                    kernels="optimized",arithmetic="k16",allow_arithmetic_change=False,
                    output=self.root/(action.__name__+gemm),comparison_anchor="legacy")
                with self.subTest(action=action.__name__,gemm=gemm),mock.patch.object(tune.subprocess,"run") as child:
                    with self.assertRaisesRegex(ValueError,"tile_n=16|optimized Q16/Q32"):
                        action(args)
                    child.assert_not_called();self.assertFalse(args.output.exists())

    def test_pair_arena_collection_keeps_frozen_baseline_and_actual_environment(self):
        model=self.root/"prototype-model";model.mkdir()
        shaders=self.root/"prototype-baseline";shaders.mkdir()
        args=argparse.Namespace(executable=Path(sys.executable),baseline_executable=Path(sys.executable),
            model=model,shaders=shaders,baseline_shaders=shaders,output=self.root/"prototype-collect",
            mode="profile",kernels="optimized",arithmetic="k16",gemm="direct-rte-pair",tile_n=16,stage_k=16,
            width=1707,height=960,warmup=5,frames=30,pairs=3,timeout=60,allow_arithmetic_change=False,
            window_queries=32,window_layout="arena-rte",comparison_anchor="rte32")
        calls=[]
        def child(command,**kwargs):
            candidate=bool(len(calls)%2);role="candidate" if candidate else "baseline"
            value=self.record(role,"profile")
            value["selected"].update(kernels="optimized",gemm="direct-rte-pair" if candidate else "direct-rte",
                window_layout="arena-rte" if candidate else "register-rte",window_queries=32,
                **{key:False for key in tune.FUSION_KEYS})
            policy=tune.selected_policy(value["selected"])
            value["dispatches"][0].update(variant=tune.GEMM_VARIANTS[policy["gemm"]],
                geometry={"threads":128,"required_subgroup_size":32,"tile_m":64})
            self.assertEqual(command[command.index("--amd-gemm")+1],policy["gemm"])
            self.assertEqual(kwargs["env"]["DLSS5VK_AMD_WINDOW_LAYOUT"],policy["window_layout"])
            Path(command[command.index("--json")+1]).write_text(json.dumps(value),encoding="utf-8")
            calls.append(policy);return subprocess.CompletedProcess(command,0)
        with mock.patch.object(tune.subprocess,"run",side_effect=child):manifest=tune.collect(args)
        self.assertTrue(all(tune.preserving_baseline(policy,"rte32") for policy in calls[::2]))
        self.assertTrue(all(policy["gemm"]=="direct-rte-pair" and policy["window_layout"]=="arena-rte" for policy in calls[1::2]))
        self.assertEqual(tune.read_json(manifest)["runs"][1]["environment"]["DLSS5VK_AMD_GEMM"],"direct-rte-pair")

    def test_pair_arena_replay_binds_actual_policy_in_both_history_modes(self):
        directory=self.capture_sequence_fixture();model=self.root/"prototype-replay-model";model.mkdir()
        args=argparse.Namespace(capture_sequence=directory,executable=Path(sys.executable),model=model,
            shaders=None,game_shaders=None,output=self.root/"prototype-replay",history_mode="both",
            kernels="optimized",arithmetic="k16",gemm="direct-rte-pair",window_layout="arena-rte",window_queries=32,
            tile_n=16,stage_k=16,timeout=30,reference_backend="reference",coverage=["sdr"],
            allow_nongame=False,allow_arithmetic_change=False)
        calls=[]
        def child(command,**kwargs):
            destination=Path(command[command.index("--fixture")+1]);destination.mkdir()
            capture=tune.read_json(Path(command[command.index("--recorded-frame")+1])/"manifest.json")
            environment=kwargs["env"];candidate=environment["DLSS5VK_AMD_GEMM"]=="direct-rte-pair"
            self.assertEqual(environment["DLSS5VK_AMD_WINDOW_LAYOUT"],"arena-rte" if candidate else "staged")
            selected=tune.selected_policy({**self.record("candidate")["selected"],
                "kernels":"optimized" if candidate else "baseline",
                "gemm":"direct-rte-pair" if candidate else "shared",
                "window_layout":"arena-rte" if candidate else "staged","window_queries":32 if candidate else 64})
            frame=capture["frame_id"];mode=command[command.index("--history-mode")+1]
            report=dict(sourceFrameId=frame,historyMode=mode,seed=frame,reset=frame==0,
                historyFrameIds=[] if frame==0 else [frame-1],identity=self.identity(),selected=selected)
            (destination/"manifest.json").write_text(json.dumps(report),encoding="utf-8")
            pixels=array.array("f",[2.0,.5,-.1]*121);sign=b"-1\n" if sys.byteorder=="little" else b"1\n"
            (destination/"recorded-scene-linear-rgb.pfm").write_bytes(b"PF\n11 11\n"+sign+pixels.tobytes())
            calls.append((mode,candidate));return subprocess.CompletedProcess(command,0)
        with mock.patch.object(tune.subprocess,"run",side_effect=child):result=tune.replay_sequence(args)
        self.assertTrue(result["thresholds_passed"]);self.assertEqual(len(calls),8)
        for mode in ("identical","evolved"):
            value=tune.read_json(args.output/(mode+"-sequence.json"))
            self.assertEqual(value["selected"]["gemm"],"direct-rte-pair")
            self.assertEqual(value["selected"]["window_layout"],"arena-rte")


if __name__ == "__main__":
    unittest.main()
