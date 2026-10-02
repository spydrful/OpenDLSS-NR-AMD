import array
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


spec=importlib.util.spec_from_file_location("compare_amd_scenes",Path(__file__).resolve().parents[1]/"tools"/"compare_amd_scenes.py")
scenes=importlib.util.module_from_spec(spec);spec.loader.exec_module(scenes)


class SceneComparisonTests(unittest.TestCase):
    def setUp(self):
        self.scratch=tempfile.TemporaryDirectory();self.addCleanup(self.scratch.cleanup)
        self.root=Path(self.scratch.name)

    def fixture(self,name,candidate_error=0,identity=True):
        directory=self.root/name;directory.mkdir()
        manifest={"format":"OpenNR-local-scene-composite-v1","width":11,"height":11,"syntheticScene":True,
                  "capturedGameFrames":False,"modelManifestSha256":"a"*64,"preprocessSpvSha256":"b"*64,
                  "compositeSpvSha256":"c"*64,"cameraResetReproducesFirstOutput":True,"cases":[]}
        if identity:
            manifest["identity"]={"device_id":"1002:7550","driver_id":"test-driver","model_sha256":"a"*64,
                                  "shader_sha256":"d"*64,"baseline_shader_sha256":"e"*64}
            manifest["selected"]={"kernels":"baseline","arithmetic":"k16","tile_n":16,"stage_k":16,"window_queries":64}
        for case in scenes.CASES:
            values=[2.0 if case.startswith("hdr") else .5,.25,-.125,.75]*121
            if case=="hdr-temporal":values[0]+=candidate_error
            rgba=array.array("f",values)
            rgb=array.array("f",(rgba[(y*11+x)*4+c] for y in range(10,-1,-1) for x in range(11) for c in range(3)))
            output=rgba.tobytes()
            source=array.array("f",[.5,.25,.125,1.0,0,0,1,0]*121+[1,0,0,0]).tobytes()
            history=array.array("f",[.1,.2,.3,1]*121).tobytes()
            features=array.array("f",[.125]*121*16).tobytes()
            head=array.array("f",[.25]*121*4).tobytes()
            item={"name":case}
            payload={"source.f32":source,"previous-history.f32":history,"features.f32":features,"head.f32":head,
                     "scene-linear-rgba.f32":output}
            for field,suffix in scenes.HASH_FILES.items():
                (directory/(case+"-"+suffix)).write_bytes(payload[suffix]);item[field]=hashlib.sha256(payload[suffix]).hexdigest().upper()
            (directory/(case+"-controls.bin")).write_bytes(bytes(72))
            sign=b"-1\n" if sys.byteorder=="little" else b"1\n"
            (directory/(case+"-scene-linear-rgb.pfm")).write_bytes(b"PF\n11 11\n"+sign+rgb.tobytes())
            manifest["cases"].append(item)
        (directory/"manifest.json").write_text(json.dumps(manifest),encoding="utf-8")
        return directory

    def test_all_six_exact_cases_validate_hdr_and_scope(self):
        report=scenes.compare(self.fixture("reference"),self.fixture("candidate"))
        self.assertTrue(report["thresholds_passed"])
        self.assertEqual(len(report["cases"]),6)
        self.assertTrue(report["actual_kernel_policy_recorded"])
        self.assertFalse(report["captured_game_quality_established"])
        self.assertFalse(report["motion_review_completed"])
        self.assertEqual(report["cases"][3]["provenance"]["reference"]["maximum_rgb"],2)

    def test_reference_policy_is_distinct_from_amd_arithmetic(self):
        left,right=self.fixture("reference"),self.fixture("candidate")
        path=left/"manifest.json";value=scenes.read_json(path)
        value["backend"]="reference";value["selected"]["arithmetic"]="reference"
        path.write_text(json.dumps(value),encoding="utf-8")
        self.assertTrue(scenes.compare(left,right)["actual_kernel_policy_recorded"])
        value["backend"]="amd";path.write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"AMD policy"):
            scenes.compare(left,right)
        value["backend"]="reference";value["selected"]["arithmetic"]="k16"
        path.write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"reference policy"):
            scenes.compare(left,right)

    def test_hdr_failure_is_not_hidden_by_other_cases_or_dynamic_peak(self):
        report=scenes.compare(self.fixture("reference"),self.fixture("candidate",candidate_error=3))
        self.assertFalse(report["thresholds_passed"])
        self.assertEqual(report["data_range"],1)
        failed=[item["name"] for item in report["cases"] if not item["thresholds_passed"]]
        self.assertEqual(failed,["hdr-temporal"])

    def test_raw_hash_tampering_rejected(self):
        left,right=self.fixture("reference"),self.fixture("candidate")
        (right/"sdr-reset-head.f32").write_bytes(b"wrong")
        with self.assertRaisesRegex(ValueError,"declared hash"):
            scenes.compare(left,right)

    def test_control_or_actual_input_mismatch_rejected(self):
        left,right=self.fixture("reference"),self.fixture("candidate")
        (right/"sdr-temporal-controls.bin").write_bytes(b"x"*72)
        with self.assertRaisesRegex(ValueError,"not matched"):
            scenes.compare(left,right)

    def test_clamped_preview_does_not_qualify_as_linear_output(self):
        left,right=self.fixture("reference"),self.fixture("candidate")
        pixels=array.array("f",[1,.25,0]*121);sign=b"-1\n" if sys.byteorder=="little" else b"1\n"
        (right/"hdr-reset-scene-linear-rgb.pfm").write_bytes(b"PF\n11 11\n"+sign+pixels.tobytes())
        with self.assertRaisesRegex(ValueError,"raw scene-linear"):
            scenes.compare(left,right)

    def test_missing_duplicate_or_nonfinite_case_data_rejected(self):
        left,right=self.fixture("reference"),self.fixture("candidate")
        path=right/"manifest.json";value=scenes.read_json(path);value["cases"][5]=value["cases"][0]
        path.write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"duplicated"):
            scenes.compare(left,right)

    def test_legacy_fixture_provenance_remains_explicit(self):
        report=scenes.compare(self.fixture("reference",identity=False),self.fixture("candidate",identity=False))
        self.assertTrue(report["thresholds_passed"])
        self.assertFalse(report["actual_kernel_policy_recorded"])

    def test_wrong_model_identity_or_game_claim_rejected(self):
        left,right=self.fixture("reference"),self.fixture("candidate")
        path=right/"manifest.json";value=scenes.read_json(path);value["identity"]["model_sha256"]="f"*64
        path.write_text(json.dumps(value),encoding="utf-8")
        with self.assertRaisesRegex(ValueError,"different model"):
            scenes.compare(left,right)

    def test_cli_failed_quality_returns_one_and_preserves_report(self):
        left,right=self.fixture("reference"),self.fixture("candidate",candidate_error=3)
        output=self.root/"report.json"
        result=subprocess.run([sys.executable,str(Path(scenes.__file__)),"--reference",str(left),"--candidate",str(right),"--output",str(output)],capture_output=True)
        self.assertEqual(result.returncode,1,result.stderr.decode())
        self.assertFalse(scenes.read_json(output)["thresholds_passed"])


if __name__=="__main__":unittest.main()
