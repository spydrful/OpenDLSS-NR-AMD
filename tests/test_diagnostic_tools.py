import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


def load(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parents[1] / "tools" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


presentmon = load("analyze_presentmon")
runtime = load("analyze_runtime")


class DiagnosticTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.path = Path(self.scratch.name) / "capture.csv"

    def capture(self, text):
        self.path.write_text(text, encoding="utf-8")
        return self.path

    def test_v1_keeps_dropped_and_outliers(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,TimeInSeconds,MsBetweenPresents,Dropped\n"
            "Cyberpunk2077.exe,4,0x1,0,0,0\nCyberpunk2077.exe,4,0x1,.01,10,1\n"
            "Cyberpunk2077.exe,4,0x1,.03,20,0\nCyberpunk2077.exe,4,0x1,1.03,1000,1\n")
        report, rows = presentmon.analyze(path, frame_generation_off=True)
        self.assertEqual([x["frame_ms"] for x in rows], [10, 20, 1000])
        self.assertEqual(report["dropped_presents_retained"], 2)
        self.assertEqual(report["initial_intervals_unavailable"], 1)
        self.assertAlmostEqual(report["real_rendered_fps"], 3000 / 1030)
        self.assertFalse(report["p95_frame_budget_met_60fps"])
        report, rows = presentmon.analyze(path, start_seconds=".01", end_seconds=".03")
        self.assertEqual([x["frame_ms"] for x in rows], [20])
        self.assertIsNone(report["real_rendered_fps"])
        self.assertEqual(report["range_excluded_intervals"], 2)

    def test_v1_without_clock_retains_all_valid_intervals(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,MsBetweenPresents\nG,1,A,10\nG,1,A,0\nG,1,A,20\n")
        report, rows = presentmon.analyze(path)
        self.assertEqual([x["frame_ms"] for x in rows], [10, 0, 20])
        self.assertEqual(report["zero_intervals_retained"], 1)
        with self.assertRaises(ValueError):
            presentmon.analyze(path, start_seconds=0)

    def test_v2_seconds_and_explicit_milliseconds(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,CPUStartTime,FrameTime\nG,1,A,1,999\nG,1,A,1.016,999\nG,1,A,1.032,999\n")
        report, rows = presentmon.analyze(path, frame_generation_off=True)
        self.assertEqual([x["frame_ms"] for x in rows], [16, 16])
        self.assertEqual(report["real_rendered_fps"], 62.5)
        self.assertEqual(report["clock_unit"], "seconds")
        self.assertEqual(report["initial_intervals_unavailable"], 1)
        self.capture(self.path.read_text().replace("1.016", "1016").replace("1.032", "1032").replace("A,1,", "A,1000,"))
        report, rows = presentmon.analyze(path, cpu_start_unit="milliseconds")
        self.assertEqual([x["frame_ms"] for x in rows], [16, 16])

    def test_exact_qpc_large_ticks_and_window(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,CPUStartQPC\n"
            "G,1,A,1000000000000000000\nG,1,A,1000000000000160000\nG,1,A,1000000000000320000\n")
        with self.assertRaises(ValueError):
            presentmon.analyze(path)
        report, rows = presentmon.analyze(path, qpc_frequency=10000000,
            qpc_start="1000000000000160000", qpc_end="1000000000000320000")
        self.assertEqual([x["frame_ms"] for x in rows], [16])
        self.assertEqual(report["range_excluded_intervals"], 1)
        self.assertEqual(report["selection"]["qpc_unit"], "ticks")
        with self.assertRaises(ValueError):
            presentmon.analyze(path, qpc_frequency=10000000, start_seconds=0)

    def test_qpc_milliseconds_and_ambiguous_v1(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,CPUStartQPCTime\nG,1,A,123000.0\nG,1,A,123016.0\n")
        report, rows = presentmon.analyze(path)
        self.assertEqual(rows[0]["frame_ms"], 16)
        self.assertEqual(report["clock_unit"], "milliseconds")
        path = self.capture("Application,ProcessID,SwapChainAddress,TimeInSeconds,MsBetweenPresents,QPCTime\nG,1,A,1,0,123000\nG,1,A,1.016,16,123016\n")
        with self.assertRaises(ValueError):
            presentmon.analyze(path, qpc_start=123000, qpc_end=123016)
        report, rows = presentmon.analyze(path, qpc_start=123000, qpc_end=123016, qpc_unit="milliseconds")
        self.assertEqual(rows[0]["frame_ms"], 16)

    def test_explicit_fg_bridged_unknown_retained(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,TimeInSeconds,MsBetweenPresents,FrameType,Dropped\n"
            "G,1,A,0,0,Application,0\nG,1,A,.005,5,AMD AFMF,0\nG,1,A,.010,5,Application,0\n"
            "G,1,A,.015,5,Intel XeSS-FG,0\nG,1,A,.020,5,Unknown,1\n")
        report, rows = presentmon.analyze(path)
        self.assertEqual([x["frame_ms"] for x in rows], [10, 10])
        self.assertEqual(report["explicit_generated_rows_removed"], 2)
        self.assertEqual(report["dropped_presents_retained"], 1)
        self.assertIsNone(report["real_rendered_fps"])
        with self.assertRaises(ValueError):
            presentmon.analyze(path, frame_generation_off=True)

    def test_multiple_process_or_swapchain_requires_selection(self):
        path = self.capture("Application,ProcessID,SwapChainAddress,MsBetweenPresents\nG,1,A,10\nG,1,B,100\nG,2,A,200\nH,3,A,300\n")
        for options in ({}, {"process_name": "G"}, {"process_id": 1}):
            with self.assertRaises(ValueError):
                presentmon.analyze(path, **options)
        report, rows = presentmon.analyze(path, process_id=1, swap_chain="a")
        self.assertEqual(rows[0]["frame_ms"], 10)
        self.assertEqual(report["selection"]["process_id"], 1)

    def test_invalid_and_cli_no_overwrite(self):
        header = "Application,ProcessID,SwapChainAddress,CPUStartTime\n"
        for text in (header + "G,1,A,1\nG,1,A,.9\n", header + "G,1,A,nan\n",
                     "Application,application,ProcessID,SwapChainAddress,MsBetweenPresents\nG,G,1,A,10\n"):
            self.capture(text)
            with self.assertRaises(ValueError):
                presentmon.analyze(self.path)
        self.capture("Application,ProcessID,SwapChainAddress,MsBetweenPresents\nG,1,A,10\nG,1,A,20\n")
        with self.assertRaises(ValueError):
            presentmon.analyze(self.path, warmup=2)
        result = Path(self.scratch.name) / "report.json"
        frames = Path(self.scratch.name) / "frames.csv"
        script = Path(presentmon.__file__)
        command = [sys.executable, str(script), str(self.path), "--frame-generation-off", "--output", str(result), "--frames-csv", str(frames)]
        self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)
        original = result.read_bytes()
        self.assertNotEqual(subprocess.run(command, capture_output=True).returncode, 0)
        self.assertEqual(result.read_bytes(), original)

    def test_runtime_segments_counters_no_fps(self):
        path = self.capture("frame_id,pack_ms,preprocess_ms,inference_ms,composite_ms,unpack_ms,neural_ms,vram_mib,allocated_neural_bytes,submitted_frames,bypassed_frames\n"
            "0,1,2,3,4,5,20,-1,500,5,2\n1,1,2,3,4,5,21,2000,600,7,3\n"
            "0,1,2,3,4,5,22,-1,700,1,0\n1,1,2,3,4,5,23,2500,800,2,0\n")
        report = runtime.summarize(path)
        self.assertEqual(report["detected_segments"], 2)
        self.assertNotIn("fps", str(report))
        self.assertEqual(report["segments"][0]["observed_counter_increases"], {"submitted_frames": 2, "bypassed_frames": 1})
        self.assertEqual(report["segments"][0]["ending_session_counters"]["bypassed_frames"], 3)
        self.assertEqual(report["segments"][1]["vram_mib_sampled"]["peak"], 2500)
        self.assertEqual(report["segments"][1]["allocated_neural_bytes_partial_peak"], 800)
        self.assertEqual(report["segments"][1]["vram_unknown_jobs"], 1)
        report = runtime.summarize(path, warmup=1, segment=1)
        self.assertEqual(len(report["segments"]), 1)
        self.assertEqual(report["segments"][0]["completed_jobs"], 1)
        self.assertFalse(report["segments"][0]["first_job_counter_interval_unobserved"])
        with self.assertRaises(ValueError):
            runtime.summarize(path, segment=3)
        with self.assertRaises(ValueError):
            runtime.summarize(path, warmup=2)
        self.capture(path.read_text().replace("2000", "nan"))
        with self.assertRaises(ValueError):
            runtime.summarize(path)


if __name__ == "__main__":
    unittest.main()
