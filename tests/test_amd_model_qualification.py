"""CPU-only tests for artifact/provenance binding; no model, driver or GPU used."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


_spec = importlib.util.spec_from_file_location("qualify_amd_model", Path(__file__).resolve().parents[1] / "tools" / "qualify_amd_model.py")
qualification = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(qualification)


class ModelQualificationTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.root = Path(self.scratch.name)
        self.baseline = self.root / "baseline"
        self.candidate = self.root / "candidate"
        self.baseline_benchmark = self.root / "baseline-benchmark.json"
        self.candidate_benchmark = self.root / "candidate-benchmark.json"
        self.output = self.root / "qualification.json"

    @staticmethod
    def identity(role):
        return {"device_id": "1002:7550", "driver_id": "CPU-only synthetic test|no GPU|1",
                "model_sha256": "a" * 64, "baseline_shader_sha256": "b" * 64,
                "shader_sha256": ("b" if role == "baseline" else "c") * 64}

    @staticmethod
    def selection(role):
        return {"kernels": "baseline" if role == "baseline" else "optimized", "arithmetic": "k16",
                "tile_n": 16, "stage_k": 16, "fusion": role != "baseline", "expert_fusion": False,
                "block_fusion": False, "hardware_publication": False}

    @staticmethod
    def write(path, value):
        path.write_text(json.dumps(value), encoding="utf-8")

    @staticmethod
    def mutate(path, action):
        value = qualification._protocol.read_json(path)
        action(value)
        ModelQualificationTests.write(path, value)

    @staticmethod
    def change_byte(path, offset=0, value=b"\x80"):
        with path.open("r+b") as data:
            data.seek(offset)
            data.write(value)

    def fixtures(self, *, target=False):
        valid = (1707, 960) if target else (320, 320)
        full, levels = qualification.geometry(*valid)
        for root, role, benchmark in ((self.baseline, "baseline", self.baseline_benchmark),
                                      (self.candidate, "candidate", self.candidate_benchmark)):
            root.mkdir()
            ident, selected = self.identity(role), self.selection(role)
            def zeros(name, length):
                with (root / name).open("wb") as destination:
                    destination.truncate(length)
            zeros("features.f32", full[0] * full[1] * 64)
            feature_hash = qualification._protocol.sha256(root / "features.f32")
            zeros("head.f32", full[0] * full[1] * 16)
            zeros("captured-head.f32", full[0] * full[1] * 16)
            blocks, transitions = [], []
            if not target:
                for name, shape in qualification.expected_shapes(full, levels).items():
                    zeros(name + ".u8", shape[0] * shape[1] * shape[2])
                    entry = {"file": name + ".u8", "width": shape[0], "height": shape[1], "channels": shape[2]}
                    if name.startswith("block-"):
                        entry["block"] = int(name[6:])
                        blocks.append(entry)
                    else:
                        entry["id"] = name[11:]
                        transitions.append(entry)
            manifest = {"producer": "OpenNR Vulkan amd; local validation, not NVIDIA capture",
                        "inputGenerator": "fixedpoint-proxy-clt-noise-v1", "captureImplementation": "decomposed",
                        "identity": ident, "selected": selected,
                        "inputFeaturesSha256": feature_hash.upper(), "modelManifestSha256": "A" * 64,
                        "sourceDimensions": list(valid), "fullDimensions": list(full),
                        "inputFeatures": {"file": "features.f32"}, "checks": ["boundaries", "head"],
                        "blocks": blocks, "transitions": transitions, "referenceHead": {"file": "head.f32"},
                        "capturedHead": {"file": "captured-head.f32", "productionIdentical": True}}
            report = {"format": "OpenNR-local-modelcheck-v1", "backend": "amd", "identity": ident,
                      "selected": selected, "captureImplementation": "decomposed",
                      "nvidiaParityEstablished": False, "qualityThresholdApplied": False,
                      "identicalFeaturesSha256": feature_hash.upper(), "modelManifestSha256": "A" * 64,
                      "productionRepeatable": True, "captureHeadIdentical": True, "nonfiniteHead": 0}
            # Timing record can have another geometry: only its execution
            # identity/policy, not its timings, is bound to this exact suite.
            timing = {"format": "OpenNR-amd-benchmark-v1", "command": "bench", "readback": False,
                      "instrumented": False, "width": 1707, "height": 960, "padded_width": 1728,
                      "padded_height": 960, "frames": 3, "warmup": 1, "frame_ms": [3, 2, 4],
                      "identity": ident, "selected": selected}
            self.write(root / "manifest.json", manifest)
            self.write(root / "modelcheck-report.json", report)
            self.write(benchmark, timing)

    def run_qualification(self, *, target=False, comparison_anchor="legacy"):
        return qualification.qualify(self.baseline, self.candidate, self.baseline_benchmark,
                                     self.candidate_benchmark, self.output, target_only=target,
                                     comparison_anchor=comparison_anchor)

    def compact_anchor(self):
        for root, benchmark, role in ((self.baseline, self.baseline_benchmark, "baseline"),
                                      (self.candidate, self.candidate_benchmark, "candidate")):
            for path in (root / "manifest.json", root / "modelcheck-report.json", benchmark):
                self.mutate(path, lambda value: value["selected"].update(
                    kernels="optimized", window_queries=64 if role == "baseline" else 32))

    def test_explicit_compact_anchor_preserves_all_75_boundary_and_head_checks(self):
        self.fixtures(); self.compact_anchor()
        with self.assertRaisesRegex(ValueError, "explicit legacy baseline"):
            self.run_qualification()
        result = self.run_qualification(comparison_anchor="compact64")
        self.assertTrue(result["passed"])
        self.assertEqual(result["comparison_anchor"], "compact64")
        self.assertEqual(result["checks"], 77)
        self.assertEqual(result["baseline_selected"]["kernels"], "optimized")
        self.assertEqual(result["baseline_selected"]["window_queries"], 64)
        self.assertEqual(result["baseline_identity"], self.identity("baseline"))
        self.assertTrue(qualification._protocol.exact_manifest(self.output, "compact64")["passed"])
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            qualification._protocol.exact_manifest(self.output)

    def test_compact_target_anchor_still_rejects_signed_zero_difference(self):
        self.fixtures(target=True); self.compact_anchor()
        self.change_byte(self.candidate / "head.f32", offset=3)
        result = self.run_qualification(target=True, comparison_anchor="compact64")
        self.assertFalse(result["passed"])
        self.assertEqual(result["checks"], 2)
        self.assertEqual(result["mismatchedChecks"], 2)
        self.assertFalse(qualification._protocol.exact_manifest(self.output, "compact64")["passed"])

    def qualified32_anchor(self):
        self.compact_anchor()
        for path in (self.baseline / "manifest.json", self.baseline / "modelcheck-report.json", self.baseline_benchmark):
            self.mutate(path, lambda value: value["selected"].update(window_queries=32))

    def test_qualified32_model_binds_all_checkpoints_and_current_q32_baseline(self):
        self.fixtures(); self.qualified32_anchor()
        with self.assertRaisesRegex(ValueError, "compact64 baseline N16/K16/Q64"):
            self.run_qualification(comparison_anchor="compact64")
        result = self.run_qualification(comparison_anchor="qualified32")
        self.assertTrue(result["passed"])
        self.assertEqual(result["checks"], 77)
        self.assertEqual(result["baseline_selected"]["window_queries"], 32)
        self.assertTrue(qualification._protocol.exact_manifest(self.output, "qualified32")["passed"])

    def test_qualified32_target_retains_signed_zero_and_capture_schedule_gate(self):
        self.fixtures(target=True); self.qualified32_anchor()
        self.change_byte(self.candidate / "head.f32", offset=3)
        result = self.run_qualification(target=True, comparison_anchor="qualified32")
        self.assertFalse(result["passed"])
        self.assertEqual(result["checks"], 2)
        self.assertEqual(result["mismatchedChecks"], 2)

    def test_qualified32_cannot_assume_missing_window_geometry(self):
        self.fixtures(); self.qualified32_anchor()
        self.mutate(self.baseline_benchmark, lambda value: value["selected"].pop("window_queries"))
        with self.assertRaisesRegex(ValueError, "explicitly recorded window_queries"):
            self.run_qualification(comparison_anchor="qualified32")
        self.assertFalse(self.output.exists())

    def direct32_anchor(self):
        self.qualified32_anchor()
        for root, benchmark in ((self.baseline, self.baseline_benchmark), (self.candidate, self.candidate_benchmark)):
            for path in (root / "manifest.json", root / "modelcheck-report.json", benchmark):
                self.mutate(path, lambda value: value["selected"].update(gemm="direct"))

    def test_direct32_model_requires_explicit_alpha3_baseline_and_all_checkpoints(self):
        self.fixtures(); self.direct32_anchor()
        result = self.run_qualification(comparison_anchor="direct32")
        self.assertTrue(result["passed"])
        self.assertEqual(result["checks"], 77)
        self.assertEqual(result["baseline_selected"]["gemm"], "direct")
        self.assertTrue(qualification._protocol.exact_manifest(self.output, "direct32")["passed"])
        with self.assertRaisesRegex(ValueError, "explicit --comparison-anchor"):
            qualification._protocol.exact_manifest(self.output, "qualified32")

    def test_direct32_target_preserves_signed_zero_and_production_schedule_gates(self):
        self.fixtures(target=True); self.direct32_anchor()
        self.change_byte(self.candidate / "head.f32", offset=3)
        result = self.run_qualification(target=True, comparison_anchor="direct32")
        self.assertFalse(result["passed"])
        self.assertEqual(result["mismatchedChecks"], 2)

    def test_direct32_cannot_silently_compare_with_shared_baseline(self):
        self.fixtures(); self.direct32_anchor()
        for path in (self.baseline / "manifest.json", self.baseline / "modelcheck-report.json", self.baseline_benchmark):
            self.mutate(path, lambda value: value["selected"].update(gemm="shared"))
        with self.assertRaisesRegex(ValueError, "direct32 baseline"):
            self.run_qualification(comparison_anchor="direct32")
        self.assertFalse(self.output.exists())

    def test_register_attention_binds_candidate_layout_and_all_checkpoints(self):
        self.fixtures(); self.direct32_anchor()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json", self.candidate_benchmark):
            self.mutate(path, lambda value: value["selected"].update(window_layout="register"))
        result = self.run_qualification(comparison_anchor="direct32")
        self.assertTrue(result["passed"])
        self.assertEqual(result["checks"], 77)
        self.assertEqual(result["baseline_selected"]["window_layout"], "staged")
        self.assertEqual(result["selected"]["window_layout"], "register")

    def test_register_attention_cannot_inherit_staged_fixture_publication(self):
        self.fixtures(); self.direct32_anchor()
        self.mutate(self.candidate_benchmark, lambda value: value["selected"].update(window_layout="register"))
        with self.assertRaisesRegex(ValueError, "selected policy differs from benchmark"):
            self.run_qualification(comparison_anchor="direct32")
        self.assertFalse(self.output.exists())

    def test_register_rte_attention_binds_its_distinct_preserving_policy(self):
        self.fixtures(); self.direct32_anchor()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json", self.candidate_benchmark):
            self.mutate(path, lambda value: value["selected"].update(window_layout="register-rte", gemm="direct-rte"))
        result = self.run_qualification(comparison_anchor="direct32")
        self.assertTrue(result["passed"])
        self.assertEqual(result["checks"], 77)
        self.assertEqual(result["selected"]["window_layout"], "register-rte")
        self.assertEqual(result["selected"]["gemm"], "direct-rte")
        self.mutate(self.candidate / "manifest.json", lambda value: value["selected"].update(window_layout="register"))
        self.output = self.root / "different-policy-qualification.json"
        with self.assertRaisesRegex(ValueError, "selected policy differs from benchmark"):
            self.run_qualification(comparison_anchor="direct32")

    def test_direct_gemm_candidate_binds_explicit_route_with_legacy_shared_baseline(self):
        self.fixtures(); self.qualified32_anchor()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json", self.candidate_benchmark):
            self.mutate(path, lambda value: value["selected"].update(gemm="direct"))
        result = self.run_qualification(comparison_anchor="qualified32")
        self.assertTrue(result["passed"])
        self.assertEqual(result["checks"], 77)
        self.assertEqual(result["baseline_selected"]["gemm"], "shared")
        self.assertEqual(result["selected"]["gemm"], "direct")
        self.assertTrue(qualification._protocol.exact_manifest(self.output, "qualified32")["passed"])

    def test_gemm_route_in_fixture_must_match_actual_benchmark_selection(self):
        self.fixtures(); self.qualified32_anchor()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json"):
            self.mutate(path, lambda value: value["selected"].update(gemm="direct"))
        with self.assertRaisesRegex(ValueError, "selected policy differs from benchmark"):
            self.run_qualification(comparison_anchor="qualified32")
        self.assertFalse(self.output.exists())

    def test_qualified_anchor_cannot_use_direct_gemm_as_shared_baseline(self):
        self.fixtures(); self.qualified32_anchor()
        for path in (self.baseline / "manifest.json", self.baseline / "modelcheck-report.json", self.baseline_benchmark):
            self.mutate(path, lambda value: value["selected"].update(gemm="direct"))
        with self.assertRaisesRegex(ValueError, "qualified32 baseline"):
            self.run_qualification(comparison_anchor="qualified32")
        self.assertFalse(self.output.exists())

    def test_direct_gemm_does_not_accept_unused_staging_identity(self):
        self.fixtures(); self.qualified32_anchor()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json", self.candidate_benchmark):
            self.mutate(path, lambda value: value["selected"].update(gemm="direct", stage_k=32))
        with self.assertRaisesRegex(ValueError, "direct.*stage_k"):
            self.run_qualification(comparison_anchor="qualified32")
        self.assertFalse(self.output.exists())

    def test_compact_anchor_does_not_relabel_legacy_policy(self):
        self.fixtures()
        for root, benchmark in ((self.baseline, self.baseline_benchmark), (self.candidate, self.candidate_benchmark)):
            for path in (root / "manifest.json", root / "modelcheck-report.json", benchmark):
                self.mutate(path, lambda value: value["selected"].update(window_queries=64))
        with self.assertRaisesRegex(ValueError, "explicit compact64 baseline"):
            self.run_qualification(comparison_anchor="compact64")
        self.assertFalse(self.output.exists())

    def test_compact_anchor_requires_recorded_q64_and_agreed_identity(self):
        self.fixtures(); self.compact_anchor()
        self.mutate(self.baseline_benchmark, lambda value: value["selected"].pop("window_queries"))
        with self.assertRaisesRegex(ValueError, "explicitly recorded window_queries"):
            self.run_qualification(comparison_anchor="compact64")
        self.mutate(self.baseline_benchmark, lambda value: value["selected"].update(window_queries=64))
        self.mutate(self.baseline_benchmark, lambda value: value["identity"].update(shader_sha256="e" * 64))
        with self.assertRaisesRegex(ValueError, "execution identity differs from benchmark"):
            self.run_qualification(comparison_anchor="compact64")

    def reject(self, message):
        with self.assertRaisesRegex(ValueError, message):
            self.run_qualification()
        self.assertFalse(self.output.exists())

    def test_all_75_checkpoints_plus_head_and_both_capture_schedules(self):
        self.fixtures()
        result = self.run_qualification()
        self.assertTrue(result["passed"])
        self.assertEqual(result["checks"], 77)
        self.assertEqual({item["name"] for item in result["pairs"]}, qualification.CHECKPOINTS | {"head", "capture-production"})
        self.assertEqual(result["identity"], self.identity("candidate"))
        self.assertEqual(result["baseline_identity"], self.identity("baseline"))
        self.assertFalse(result["capturedGameFrames"])
        self.assertFalse(result["performanceQualified"])
        self.assertEqual(result["captureProductionComponents"]["candidate"]["benchmark_resolution"], [1707, 960])
        # Use the downstream gate rather than simply asserting the producer's PASS.
        self.assertTrue(qualification._protocol.exact_manifest(self.output)["passed"])

    def test_target_only_gates_two_head_schedules_without_full_boundary_exports(self):
        self.fixtures(target=True)
        result = self.run_qualification(target=True)
        self.assertTrue(result["passed"])
        self.assertEqual(result["suite"], "target")
        self.assertEqual(result["resolution"], [1707, 960])
        self.assertEqual(result["padded_resolution"], [1728, 960])
        self.assertEqual(result["checks"], 2)
        self.assertTrue(qualification._protocol.exact_manifest(self.output)["passed"])

    def test_actual_boundary_mismatch_defeats_reported_success(self):
        self.fixtures()
        self.change_byte(self.candidate / "block-47.u8", offset=73)
        result = self.run_qualification()
        self.assertFalse(result["passed"])
        self.assertEqual(result["mismatchedChecks"], 1)
        self.assertEqual(next(item for item in result["pairs"] if item["name"] == "block-47")["differentBytes"], 1)
        self.assertFalse(qualification._protocol.exact_manifest(self.output)["passed"])

    def test_signed_zero_head_bit_mismatch_is_not_tolerated(self):
        self.fixtures()
        self.change_byte(self.candidate / "head.f32", offset=3)
        result = self.run_qualification()
        self.assertFalse(result["passed"])
        self.assertEqual(result["mismatchedChecks"], 2)  # head and capture/production

    def test_baseline_capture_mismatch_is_preserved_in_gate_artifact(self):
        self.fixtures()
        self.change_byte(self.baseline / "captured-head.f32", offset=7)
        result = self.run_qualification()
        self.assertFalse(result["passed"])
        self.assertEqual(result["mismatchedChecks"], 1)
        self.assertFalse(qualification._protocol.exact_manifest(self.output)["passed"])

    def test_candidate_capture_mismatch_is_preserved_in_gate_artifact(self):
        self.fixtures()
        self.change_byte(self.candidate / "captured-head.f32", offset=7)
        self.assertFalse(self.run_qualification()["passed"])

    def test_explicit_execution_failure_is_not_overridden_by_equal_exports(self):
        self.fixtures()
        self.mutate(self.candidate / "modelcheck-report.json", lambda value: value.update(
            productionRepeatable=False, captureHeadIdentical=False, nonfiniteHead=900))
        self.mutate(self.candidate / "manifest.json", lambda value: value["capturedHead"].update(productionIdentical=False))
        self.reject("failed/incomplete execution")

    def test_actual_nonfinite_head_is_rejected_even_when_report_says_finite(self):
        self.fixtures()
        self.change_byte(self.candidate / "head.f32", value=struct.pack("<I", 0x7f800000))
        self.reject("nonfinite F32")

    def test_actual_feature_digest_must_match_recorded_hash(self):
        self.fixtures()
        self.change_byte(self.candidate / "features.f32")
        self.reject("features SHA-256")

    def test_identical_forged_summary_hash_cannot_hide_different_feature_buffers(self):
        self.fixtures()
        self.change_byte(self.candidate / "features.f32", value=struct.pack("<f", 1))
        changed = qualification._protocol.sha256(self.candidate / "features.f32")
        self.mutate(self.candidate / "manifest.json", lambda value: value.update(inputFeaturesSha256=changed))
        self.mutate(self.candidate / "modelcheck-report.json", lambda value: value.update(identicalFeaturesSha256=changed))
        self.reject("actual feature bytes differ")

    def test_actual_feature_nonfinite_values_are_rejected_even_with_correct_hash(self):
        self.fixtures()
        self.change_byte(self.candidate / "features.f32", value=struct.pack("<I", 0x7fc00000))
        changed = qualification._protocol.sha256(self.candidate / "features.f32")
        self.mutate(self.candidate / "manifest.json", lambda value: value.update(inputFeaturesSha256=changed))
        self.mutate(self.candidate / "modelcheck-report.json", lambda value: value.update(identicalFeaturesSha256=changed))
        self.reject("nonfinite F32")

    def test_model_manifest_digest_must_match_actual_benchmark_identity(self):
        self.fixtures()
        self.mutate(self.candidate / "manifest.json", lambda value: value.update(modelManifestSha256="d" * 64))
        self.reject("model manifest identity")

    def test_benchmark_is_required_and_cannot_supply_unrelated_shader_identity(self):
        self.fixtures()
        self.mutate(self.candidate_benchmark, lambda value: value["identity"].update(shader_sha256="d" * 64))
        self.reject("execution identity differs from benchmark")

    def test_actual_selected_policy_is_bound_to_benchmark_not_requested_settings(self):
        self.fixtures()
        self.mutate(self.candidate_benchmark, lambda value: value["selected"].update(tile_n=32))
        self.reject("selected policy differs from benchmark")

    def test_report_and_fixture_execution_provenance_must_agree(self):
        self.fixtures()
        self.mutate(self.candidate / "modelcheck-report.json", lambda value: value["identity"].update(shader_sha256="d" * 64))
        self.reject("execution provenance disagrees")

    def test_frozen_baseline_cannot_enable_fusion(self):
        self.fixtures()
        for path in (self.baseline / "manifest.json", self.baseline / "modelcheck-report.json", self.baseline_benchmark):
            self.mutate(path, lambda value: value["selected"].update(fusion=True))
        self.reject("baseline N16/K16")

    def test_non_k16_arithmetic_cannot_pass_preservation_gate(self):
        self.fixtures()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json", self.candidate_benchmark):
            self.mutate(path, lambda value: value["selected"].update(arithmetic="k32"))
        self.reject("preserving k16")

    def test_cross_driver_or_device_pair_is_not_qualified(self):
        self.fixtures()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json", self.candidate_benchmark):
            self.mutate(path, lambda value: value["identity"].update(driver_id="another-driver"))
        self.reject("identity differs: driver_id")

    def test_reference_backend_cannot_be_mislabeled_as_amd_baseline(self):
        self.fixtures()
        self.mutate(self.baseline / "manifest.json", lambda value: value.update(
            producer="OpenNR Vulkan reference; local validation, not NVIDIA capture"))
        self.reject("native AMD modelcheck")

    def test_selected_capture_is_not_decomposed_capture_proof(self):
        self.fixtures()
        for path in (self.candidate / "manifest.json", self.candidate / "modelcheck-report.json"):
            self.mutate(path, lambda value: value.update(captureImplementation="selected-production"))
        self.reject("decomposed execution")

    def test_missing_boundary_and_wrong_graph_shape_are_rejected(self):
        self.fixtures()
        manifest = self.candidate / "manifest.json"
        original = qualification._protocol.read_json(manifest)
        missing = copy.deepcopy(original)
        missing["blocks"].pop()
        self.write(manifest, missing)
        self.reject("all 75")
        original["blocks"][0]["channels"] = 64
        self.write(manifest, original)
        self.reject("incorrect graph shape")

    def test_duplicate_boundary_and_duplicate_json_key_are_rejected(self):
        self.fixtures()
        manifest = self.candidate / "manifest.json"
        original = qualification._protocol.read_json(manifest)
        original["blocks"].append(copy.deepcopy(original["blocks"][0]))
        self.write(manifest, original)
        self.reject("duplicate boundary")
        manifest.write_text('{"producer":"a","producer":"b"}', encoding="utf-8")
        self.reject("duplicate JSON key")

    def test_path_escape_and_incorrect_binary_length_are_rejected(self):
        self.fixtures()
        manifest = self.candidate / "manifest.json"
        original = qualification._protocol.read_json(manifest)
        escaped = copy.deepcopy(original)
        escaped["referenceHead"]["file"] = "../baseline/head.f32"
        self.write(manifest, escaped)
        self.reject("filename must stay inside")
        self.write(manifest, original)
        with (self.candidate / "head.f32").open("ab") as data:
            data.write(b"x")
        self.reject("expected .* bytes")

    def test_gate_recomputes_artifacts_after_manifest_creation(self):
        self.fixtures()
        self.assertTrue(self.run_qualification()["passed"])
        capture = self.root / "qualification-captured-heads.bin"
        self.change_byte(capture, offset=10)
        self.assertFalse(qualification._protocol.exact_manifest(self.output)["passed"])

    def test_existing_outputs_are_preserved(self):
        self.fixtures()
        self.output.write_text("do not replace", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "output already exists"):
            self.run_qualification()
        self.assertEqual(self.output.read_text(encoding="utf-8"), "do not replace")

    def test_cli_failure_code_and_required_arguments(self):
        self.fixtures()
        self.change_byte(self.candidate / "block-1.u8")
        args = [sys.executable, str(Path(qualification.__file__)), "--baseline", str(self.baseline),
                "--candidate", str(self.candidate), "--baseline-benchmark", str(self.baseline_benchmark),
                "--candidate-benchmark", str(self.candidate_benchmark), "--output", str(self.output)]
        process = subprocess.run(args, capture_output=True, text=True)
        self.assertEqual(process.returncode, 1, process.stderr)
        self.assertIn("FAIL", process.stdout)
        process = subprocess.run(args[:2], capture_output=True, text=True)
        self.assertEqual(process.returncode, 2)


    def rte32_anchor(self, candidate_gemm="direct-rte-init"):
        self.direct32_anchor()
        for root,benchmark,role in ((self.baseline,self.baseline_benchmark,"baseline"),
                                    (self.candidate,self.candidate_benchmark,"candidate")):
            for path in (root/"manifest.json",root/"modelcheck-report.json",benchmark):
                self.mutate(path,lambda value:value["selected"].update(
                    gemm="direct-rte" if role=="baseline" else candidate_gemm,window_layout="register-rte",
                    **{key:False for key in qualification._protocol.FUSION_KEYS}))

    def test_rte32_model_binds_new_modules_and_all_75_checkpoints(self):
        self.fixtures()
        for gemm in ("direct-rte-init","direct-rte-epilogue"):
            self.rte32_anchor(gemm);self.output=self.root/(gemm+"-quality.json")
            result=self.run_qualification(comparison_anchor="rte32")
            self.assertTrue(result["passed"]);self.assertEqual(result["checks"],77)
            self.assertEqual(result["baseline_selected"]["gemm"],"direct-rte")
            self.assertEqual(result["baseline_selected"]["window_layout"],"register-rte")
            self.assertEqual(result["selected"]["gemm"],gemm)
            self.assertTrue(qualification._protocol.exact_manifest(self.output,"rte32")["passed"])
            with self.assertRaisesRegex(ValueError,"explicit --comparison-anchor"):
                qualification._protocol.exact_manifest(self.output,"direct32")

    def test_rte32_cannot_inherit_old_direct_or_staged_baseline(self):
        self.fixtures();self.rte32_anchor()
        for key,wrong in (("gemm","direct"),("window_layout","staged"),("window_layout","register")):
            self.rte32_anchor()
            for path in (self.baseline/"manifest.json",self.baseline/"modelcheck-report.json",self.baseline_benchmark):
                self.mutate(path,lambda value:value["selected"].update({key:wrong}))
            with self.subTest(key=key,value=wrong),self.assertRaisesRegex(ValueError,"rte32 baseline"):
                self.run_qualification(comparison_anchor="rte32")
            self.assertFalse(self.output.exists())

    def test_rte32_new_variant_cannot_inherit_predecessor_capture_policy(self):
        self.fixtures();self.rte32_anchor()
        self.mutate(self.candidate/"manifest.json",lambda value:value["selected"].update(gemm="direct-rte"))
        with self.assertRaisesRegex(ValueError,"selected policy differs from benchmark"):
            self.run_qualification(comparison_anchor="rte32")
        self.assertFalse(self.output.exists())

    def test_rte32_target_still_checks_signed_zero_and_production_capture_bytes(self):
        self.fixtures(target=True);self.rte32_anchor("direct-rte-epilogue")
        self.change_byte(self.candidate/"head.f32",offset=3)
        result=self.run_qualification(target=True,comparison_anchor="rte32")
        self.assertFalse(result["passed"]);self.assertEqual(result["mismatchedChecks"],2)
        self.assertEqual(result["selected"]["arithmetic"],"k16")

    def test_pair_arena_model_proof_preserves_separate_selected_identities(self):
        self.fixtures();self.rte32_anchor("direct-rte-pair")
        for path in (self.candidate/"manifest.json",self.candidate/"modelcheck-report.json",self.candidate_benchmark):
            self.mutate(path,lambda value:value["selected"].update(window_layout="arena-rte"))
        result=self.run_qualification(comparison_anchor="rte32")
        self.assertTrue(result["passed"]);self.assertEqual(result["checks"],77)
        self.assertEqual(result["selected"]["gemm"],"direct-rte-pair")
        self.assertEqual(result["selected"]["window_layout"],"arena-rte")
        self.assertEqual(result["baseline_selected"]["gemm"],"direct-rte")
        self.assertEqual(result["baseline_selected"]["window_layout"],"register-rte")
        self.assertTrue(qualification._protocol.exact_manifest(self.output,"rte32")["passed"])

    def test_pair_arena_predecessor_capture_cannot_qualify_new_policy(self):
        self.fixtures();self.rte32_anchor("direct-rte-pair")
        for path in (self.candidate/"manifest.json",self.candidate/"modelcheck-report.json",self.candidate_benchmark):
            self.mutate(path,lambda value:value["selected"].update(window_layout="arena-rte"))
        for key,old in (("gemm","direct-rte-epilogue"),("window_layout","register-rte")):
            self.mutate(self.candidate/"manifest.json",lambda value:value["selected"].update({key:old}))
            with self.subTest(key=key),self.assertRaisesRegex(ValueError,"selected policy differs from benchmark"):
                self.run_qualification(comparison_anchor="rte32")
            self.assertFalse(self.output.exists())
            self.mutate(self.candidate/"manifest.json",lambda value:value["selected"].update(gemm="direct-rte-pair",window_layout="arena-rte"))


if __name__ == "__main__":
    unittest.main()
