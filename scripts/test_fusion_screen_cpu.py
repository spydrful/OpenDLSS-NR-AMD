"""CPU-only safety checks for the fusion screen runner; no model or GPU required."""
from __future__ import annotations

import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location("fusion_screen", Path(__file__).with_name("screen_amd_fusion.py"))
screen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(screen)


class FusionScreenSafety(unittest.TestCase):
    def test_default_action_does_not_launch_child(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "must-not-exist"
            argv = ["screen", "collect", "--package", "absent-package", "--model", "absent-model", "--output", str(output)]
            with mock.patch.object(screen.sys, "argv", argv), mock.patch.object(screen.subprocess, "run") as child:
                self.assertEqual(screen.main(), 1)
                child.assert_not_called()
                self.assertFalse(output.exists())

    def test_plan_cannot_execute_gpu_even_with_switch(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "must-not-exist"
            argv = ["screen", "plan", "--execute-gpu", "--package", "absent", "--model", "absent", "--output", str(output)]
            with mock.patch.object(screen.sys, "argv", argv), mock.patch.object(screen.subprocess, "run") as child:
                self.assertEqual(screen.main(), 1)
                child.assert_not_called()
                self.assertFalse(output.exists())

    def test_output_does_not_replace_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory) / "prior"
            evidence.mkdir()
            original = evidence / "summary.json"
            original.write_bytes(b"prior bytes\n")
            with self.assertRaises(FileExistsError):
                screen.create_output(evidence)
            self.assertEqual(original.read_bytes(), b"prior bytes\n")

    def test_all_routes_have_explicit_independent_cli_and_environment(self):
        inputs = {"executable": "frozen.exe", "model_directory": "local-model", "shader_directory": "frozen-shaders"}
        for route, (ffn, qkv) in screen.ROUTES.items():
            with self.subTest(route=route):
                command = screen.cli(inputs, route, "modelcheck")
                options = dict(zip(command[2::2], command[3::2]))
                self.assertEqual(options["--amd-gemm"], "direct")
                self.assertEqual(options["--amd-arithmetic"], "k16")
                self.assertEqual(options["--amd-stage-k"], "16")
                self.assertEqual(options["--amd-window-queries"], "32")
                self.assertEqual(options["--amd-ffn32-fusion"], str(int(ffn)))
                self.assertEqual(options["--amd-qkv32-fusion"], str(int(qkv)))
                self.assertLess(command.index("--amd-fusion"), command.index("--amd-ffn32-fusion"))
                self.assertLess(command.index("--amd-fusion"), command.index("--amd-qkv32-fusion"))
                environment = screen.environment(Path("new-private-output"), route)
                self.assertEqual(environment["DLSS5VK_AMD_FFN32_FUSION"], str(int(ffn)))
                self.assertEqual(environment["DLSS5VK_AMD_QKV32_FUSION"], str(int(qkv)))
                self.assertEqual(environment["DLSS5VK_CHAIN"], "0")
                self.assertEqual(environment["DLSS5VK_AMD_EXPERT_FUSION"], "0")
                self.assertEqual(environment["DLSS5VK_AMD_BLOCK_FUSION"], "0")
                self.assertEqual(environment["DLSS5VK_AMD_HARDWARE_PUBLICATION"], "0")

    def test_silent_fallback_and_changed_identity_are_rejected(self):
        identity = {"device_id": screen.DEVICE, "driver_id": screen.DRIVER, "model_sha256": screen.MODEL_HASH,
                    "shader_sha256": "a" * 64, "baseline_shader_sha256": "b" * 64}
        inputs = {"expected_identities": {"ffn-only": identity}}
        run = {"identity": identity, "selected": screen.policy("ffn-only"),
               "width": 1707, "height": 960, "padded_width": 1728, "padded_height": 960}
        screen.check_run(run, inputs, "ffn-only")
        for group, key, value in (("identity", "driver_id", "other-driver"),
                                  ("identity", "shader_sha256", "c" * 64),
                                  ("selected", "ffn32_fusion", False),
                                  ("selected", "gemm", "shared"),
                                  ("selected", "arithmetic", "k32")):
            with self.subTest(key=key):
                altered = copy.deepcopy(run)
                altered[group][key] = value
                with self.assertRaises(ValueError):
                    screen.check_run(altered, inputs, "ffn-only")
        altered = copy.deepcopy(run)
        altered["padded_width"] = 1707
        with self.assertRaises(ValueError):
            screen.check_run(altered, inputs, "ffn-only")

    def test_byte_gate_detects_one_bit_and_rejects_equal_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / "a.bin", Path(directory) / "b.bin"
            first.write_bytes(b"\x00\x80\x7e\xff")
            second.write_bytes(first.read_bytes())
            self.assertTrue(screen.exact_pair(first, second, "signed-zero-and-saturation", 4)["exact"])
            second.write_bytes(b"\x00\x00\x7e\xff")
            result = screen.exact_pair(first, second, "one-sign-bit", 4)
            self.assertFalse(result["exact"])
            self.assertEqual(result["differing_bytes"], 1)
            first.write_bytes(b"\x00\x80")
            second.write_bytes(first.read_bytes())
            with self.assertRaises(ValueError):
                screen.exact_pair(first, second, "equal-truncation", 4)

    def test_target_missing_duplicate_or_wrong_shapes_are_rejected(self):
        full, levels = screen.model_qualification.geometry(1707, 960)
        expected = screen.model_qualification.expected_shapes(full, levels)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            manifest = {"blocks": [], "transitions": []}
            for name, (width, height, channels) in expected.items():
                group, key, number = ("blocks", "block", int(name[6:])) if name.startswith("block-") else ("transitions", "id", name[11:])
                manifest[group].append({key: number, "width": width, "height": height, "channels": channels, "file": name + ".u8"})
            with mock.patch.object(screen.model_qualification, "artifact", side_effect=lambda folder, filename, name, size: {"path": folder / filename, "bytes": size}):
                def write(value):
                    (root / "manifest.json").write_text(json.dumps(value), encoding="utf-8")
                write(manifest)
                self.assertEqual(len(screen.target_boundaries(root)), 75)
                invalid = copy.deepcopy(manifest)
                invalid["blocks"].pop()
                write(invalid)
                with self.assertRaises(ValueError):
                    screen.target_boundaries(root)
                invalid = copy.deepcopy(manifest)
                invalid["blocks"].append(invalid["blocks"][0])
                write(invalid)
                with self.assertRaises(ValueError):
                    screen.target_boundaries(root)
                invalid = copy.deepcopy(manifest)
                invalid["blocks"][0]["width"] -= 1
                write(invalid)
                with self.assertRaises(ValueError):
                    screen.target_boundaries(root)

    def test_capture_environment_does_not_inherit_amd_override(self):
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.dict(screen.os.environ, {"DLSS5VK_AMD_FUSION": "1", "OTHER_SETTING": "retained"}):
                with mock.patch.object(screen.subprocess, "run", return_value=mock.Mock(returncode=0)) as child:
                    screen.launch(["frozen.exe", "modelcheck"], {"DLSS5VK_AMD_FFN32_FUSION": "0"},
                                  Path(directory) / "capture.log", 9)
                    environment = child.call_args.kwargs["env"]
                    self.assertNotIn("DLSS5VK_AMD_FUSION", environment)
                    self.assertEqual(environment["DLSS5VK_AMD_FFN32_FUSION"], "0")
                    self.assertEqual(environment["OTHER_SETTING"], "retained")
                    self.assertEqual(screen.os.environ["DLSS5VK_AMD_FUSION"], "1")


if __name__ == "__main__":
    unittest.main(verbosity=2)
