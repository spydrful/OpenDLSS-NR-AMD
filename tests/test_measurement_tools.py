import array
import importlib.util
import math
import json
import subprocess
from pathlib import Path
import sys
import tempfile
import unittest


def load(name):
    path = Path(__file__).resolve().parents[1] / "tools" / (name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


performance = load("analyze_performance")
images = load("compare_images")


class MeasurementTests(unittest.TestCase):
    def test_frame_fps_and_warmup(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "timing.csv"
            path.write_text("frame_ms,neural_ms\n100,80\n10,2\n20,3\n", encoding="utf-8")
            report = performance.summarize(path, 1)
            self.assertEqual(report["samples"], 2)
            self.assertAlmostEqual(report["real_rendered_fps"], 1000 / 15)
            self.assertEqual(report["timings"]["neural_ms"]["median"], 2.5)
            self.assertFalse(report["p95_frame_budget_met_60fps"])

    def test_invalid_measurements(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "timing.csv"
            for text in ("neural_ms\n1\n", "frame_ms\n0\n", "frame_ms\nnan\n", "frame_ms\n-1\n"):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    performance.summarize(path, 0)
            path.write_text("frame_ms\n10\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                performance.summarize(path, 1)

    def test_vram_and_bypasses(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "timing.csv"
            path.write_text("frame_ms,vram_mib,bypassed,bypass_reason\n100,9999,1,warmup\n10,2500,0,\n12,2750,1,busy\n", encoding="utf-8")
            report = performance.summarize(path, 1)
            self.assertEqual(report["vram_mib"]["peak"], 2750)
            self.assertEqual(report["bypassed_frames"], 1)
            self.assertEqual(report["bypass_reasons"], {"busy": 1})
            self.assertFalse(report["all_measured_frames_executed_nr"])
            for text in ("frame_ms,vram_mib\n10,nan\n", "frame_ms,bypassed\n10,true\n", "frame_ms,bypass_reason\n10,busy\n"):
                path.write_text(text, encoding="utf-8")
                with self.assertRaises(ValueError):
                    performance.summarize(path, 0)

    def test_exact_and_changed_ppm(self):
        with tempfile.TemporaryDirectory() as scratch:
            a, b = Path(scratch) / "a.ppm", Path(scratch) / "b.ppm"
            a.write_bytes(b"P6\n# capture\n1 1\n255\n\x00\x80\xff")
            b.write_bytes(a.read_bytes())
            self.assertTrue(images.compare(a, b)["exact"])
            b.write_bytes(b"P6\n1 1\n255\n\x00\x81\xff")
            result = images.compare(a, b)
            self.assertEqual(result["different_samples"], 1)
            self.assertAlmostEqual(result["mae"], 1 / (255 * 3))
            self.assertAlmostEqual(result["psnr_db"], 10 * math.log10(3 * 255 * 255))

    def test_pfm_endianness_and_row_order(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "linear.pfm"
            values = array.array("f", [4, 5, 6, 1, 2, 3])
            sign = b"-1\n" if sys.byteorder == "little" else b"1\n"
            path.write_bytes(b"PF\n1 2\n" + sign + values.tobytes())
            result = images.read_image(path)
            self.assertEqual(result[:3], (1, 2, 3))
            self.assertEqual(list(result[3]), [1, 2, 3, 4, 5, 6])
            values.byteswap()
            sign = b"1\n" if sys.byteorder == "little" else b"-1\n"
            path.write_bytes(b"PF\n1 2\n" + sign + values.tobytes())
            self.assertEqual(list(images.read_image(path)[3]), [1, 2, 3, 4, 5, 6])

    def test_invalid_captures(self):
        with tempfile.TemporaryDirectory() as scratch:
            path = Path(scratch) / "bad.ppm"
            for content in (b"P6\n1 1\n255\n\x00", b"P6\n0 1\n255\n", b"PF\n1 1\n0\n", b"hello"):
                path.write_bytes(content)
                with self.assertRaises(ValueError):
                    images.read_image(path)

    @staticmethod
    def metadata(frame=0, reset=True):
        return {"frame_id": frame, "sequence_id": "synthetic", "seed": 123,
                "model_sha256": "e" * 64, "input_sha256": "a" * 64, "controls": {"intensity": 1},
                "render_resolution": [11, 11], "output_resolution": [11, 11], "pipeline_point": "pre-fsr",
                "color_space": "scene-linear", "pre_exposure": 1, "exposure_scale": 1,
                "jitter": [0, 0], "motion_scale": [1, 1], "reset": reset,
                "history_frame_ids": [] if reset else [frame - 1]}

    def test_ssim_identity_and_changed_image(self):
        with tempfile.TemporaryDirectory() as scratch:
            a, b = Path(scratch) / "a.ppm", Path(scratch) / "b.ppm"
            a.write_bytes(b"P6\n11 11\n255\n" + bytes([128, 128, 128]) * 121)
            b.write_bytes(a.read_bytes())
            self.assertAlmostEqual(images.compare(a, b)["ssim"], 1, places=12)
            b.write_bytes(b"P6\n11 11\n255\n" + bytes([0, 0, 0]) * 121)
            report = images.compare(a, b)
            self.assertLess(report["ssim"], 0.01)
            self.assertFalse(images.thresholds_passed(report, None, None, 0.99))
            # An independent direct 11x11 Gaussian calculation checks the
            # covariance/moment formula and channel-average convention.
            import numpy as np
            grid = np.arange(-5, 6, dtype=np.float64)
            weights = np.exp(-(grid * grid) / (2 * 1.5 * 1.5)); weights /= weights.sum()
            left = np.arange(121, dtype=np.float64).reshape(11, 11) / 120
            right = left * .9 + .02
            kernel = weights[:, None] * weights[None, :]
            mx, my = float((left * kernel).sum()), float((right * kernel).sum())
            vx, vy = float((left * left * kernel).sum()) - mx * mx, float((right * right * kernel).sum()) - my * my
            cov = float((left * right * kernel).sum()) - mx * my
            expected = ((2 * mx * my + .01**2) * (2 * cov + .03**2)) / ((mx * mx + my * my + .01**2) * (vx + vy + .03**2))
            self.assertAlmostEqual(images.ssim(array.array("d", left.flat), array.array("d", right.flat), 11, 11, 1, 1), expected, places=12)

    def test_metadata_and_sequence_gate(self):
        with tempfile.TemporaryDirectory() as scratch:
            scratch = Path(scratch)
            (scratch / "a.ppm").write_bytes(b"P6\n11 11\n255\n" + bytes([128, 128, 128]) * 121)
            (scratch / "b.ppm").write_bytes((scratch / "a.ppm").read_bytes())
            sequence = {"format": "OpenNR-quality-sequence-v1", "data_range": 1,
                        "frames": [{"reference": "a.ppm", "candidate": "b.ppm", "metadata":
                                    {"reference": self.metadata(), "candidate": self.metadata()}}]}
            path = scratch / "sequence.json"
            path.write_text(json.dumps(sequence), encoding="utf-8")
            self.assertTrue(images.compare_sequence(path, min_ssim=.99)["thresholds_passed"])
            sequence["frames"][0]["metadata"]["candidate"]["seed"] = 124
            path.write_text(json.dumps(sequence), encoding="utf-8")
            with self.assertRaises(ValueError):
                images.compare_sequence(path, min_ssim=.99)
            sequence["frames"][0]["metadata"]["candidate"]["seed"] = 123
            sequence["frames"].append({"reference": "a.ppm", "candidate": "b.ppm", "metadata":
                                       {"reference": self.metadata(2, False), "candidate": self.metadata(2, False)}})
            path.write_text(json.dumps(sequence), encoding="utf-8")
            with self.assertRaises(ValueError):
                images.compare_sequence(path, min_ssim=.99)
            tool = Path(__file__).resolve().parents[1] / "tools" / "compare_images.py"
            result = subprocess.run([sys.executable, str(tool), str(scratch / "a.ppm"), str(scratch / "b.ppm"), "--min-ssim", ".99"], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"metadata", result.stderr)


if __name__ == "__main__":
    unittest.main()
