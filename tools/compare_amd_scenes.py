"""Verify and compare six matched synthetic scene-linear NR regression cases.

Every declared input/output hash is checked against the actual files. PFM RGB
must reproduce the unclamped float RGBA output. These generated scenes do not
establish captured-game quality, motion review, performance or NVIDIA parity.
"""
from __future__ import annotations
import argparse
import array
import hashlib
import importlib.util
import json
import math
from pathlib import Path
import sys


CASES = tuple(domain + "-" + state for domain in ("sdr", "hdr") for state in ("reset", "temporal", "camera-reset"))
HASH_FILES = {"sourceSha256": "source.f32", "historySha256": "previous-history.f32",
              "featuresSha256": "features.f32", "headSha256": "head.f32", "outputSha256": "scene-linear-rgba.f32"}
SHARED_IDENTITIES = ("modelManifestSha256", "preprocessSpvSha256", "compositeSpvSha256")


def sha256(path: Path) -> str:
    digest=hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda:source.read(1024*1024),b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    if path.stat().st_size>4*1024*1024:raise ValueError("scene manifest exceeds4MiB")
    def unique(pairs):
        result={}
        for key,value in pairs:
            if key in result:raise ValueError("duplicate JSON key: "+key)
            result[key]=value
        return result
    value=json.loads(path.read_text(encoding="utf-8-sig"),object_pairs_hook=unique,
                     parse_constant=lambda item:(_ for _ in ()).throw(ValueError("nonfinite JSON: "+item)))
    if not isinstance(value,dict):raise ValueError("scene manifest must be an object")
    return value


def valid_hash(value) -> str:
    if not isinstance(value,str) or len(value)!=64 or any(c not in "0123456789abcdefABCDEF" for c in value):
        raise ValueError("manifest hash must be a SHA-256 string")
    return value.lower()


def scene_manifest(directory: Path) -> tuple[dict,dict]:
    value=read_json(directory/"manifest.json")
    if value.get("format")!="OpenNR-local-scene-composite-v1" or value.get("syntheticScene") is not True or value.get("capturedGameFrames") is not False:
        raise ValueError("six-case comparison requires explicitly synthetic scene manifests")
    for field in ("width","height"):
        if type(value.get(field)) is not int or not 11<=value[field]<=3840:
            raise ValueError("invalid scene geometry")
    for key in SHARED_IDENTITIES:valid_hash(value.get(key))
    ident,selected=value.get("identity"),value.get("selected")
    if ident is not None or selected is not None:
        if not isinstance(ident,dict) or not isinstance(selected,dict):raise ValueError("executed identity and policy must both be recorded")
        for key in ("model_sha256","shader_sha256","baseline_shader_sha256"):valid_hash(ident.get(key))
        if valid_hash(ident["model_sha256"])!=valid_hash(value["modelManifestSha256"]):raise ValueError("executed identity used a different model manifest")
        for key in ("device_id","driver_id"):
            if not isinstance(ident.get(key),str) or not ident[key]:raise ValueError("executed identity lacks "+key)
        backend=value.get("backend","amd")
        if backend=="reference":
            if selected.get("kernels")!="baseline" or selected.get("arithmetic")!="reference":
                raise ValueError("executed reference policy is invalid")
        elif backend!="amd" or selected.get("kernels") not in ("baseline","optimized") or selected.get("arithmetic") not in ("k16","k32","final"):
            raise ValueError("executed AMD policy is invalid")
        for key in ("tile_n","stage_k"):
            if type(selected.get(key)) is not int or selected[key] not in (16,32,64):raise ValueError("executed AMD tile policy is invalid")
        if type(selected.get("window_queries",64)) is not int or selected.get("window_queries",64) not in (16,32,64):
            raise ValueError("executed window query policy is invalid")
    cases=value.get("cases")
    if not isinstance(cases,list) or len(cases)!=6 or any(not isinstance(item,dict) for item in cases):
        raise ValueError("scene manifest must include all six cases")
    indexed={item.get("name"):item for item in cases}
    if set(indexed)!=set(CASES):raise ValueError("scene cases are missing, duplicated or unsupported")
    if value.get("cameraResetReproducesFirstOutput") is not True:
        raise ValueError("scene generator did not establish reset/history isolation")
    return value,indexed


def rgba_rgb(directory: Path,name: str,width: int,height: int,images) -> dict:
    rgba_path=directory/(name+"-scene-linear-rgba.f32")
    if rgba_path.stat().st_size!=width*height*16:
        raise ValueError("scene RGBA output extent differs from manifest")
    raw=array.array("f");raw.frombytes(rgba_path.read_bytes())
    if sys.byteorder!="little":raw.byteswap()
    if any(not math.isfinite(value) for value in raw):raise ValueError("scene output contains nonfinite RGBA")
    pfm_path=directory/(name+"-scene-linear-rgb.pfm")
    with pfm_path.open("rb") as file:
        if file.readline().strip()!=b"PF":raise ValueError("quality comparison requires unclamped RGB float PFM")
    iw,ih,channels,pixels=images.read_image(pfm_path)
    if (iw,ih,channels)!=(width,height,3):raise ValueError("PFM dimensions/channels differ from scene manifest")
    expected=array.array("f",(raw[pixel*4+channel] for pixel in range(width*height) for channel in range(3)))
    actual=array.array("f",pixels)
    if actual.tobytes()!=expected.tobytes():raise ValueError("PFM RGB does not reproduce raw scene-linear output bits; clamped/transformed previews cannot qualify")
    return {"pfm_sha256":sha256(pfm_path),"minimum_rgb":min(pixels),"maximum_rgb":max(pixels),
            "hdr_samples_above_one":sum(x>1 for x in pixels)}


def compare(reference: Path,candidate: Path) -> dict:
    module_spec=importlib.util.spec_from_file_location("compare_images",Path(__file__).with_name("compare_images.py"))
    images=importlib.util.module_from_spec(module_spec);module_spec.loader.exec_module(images)
    left,left_cases=scene_manifest(reference);right,right_cases=scene_manifest(candidate)
    if (left["width"],left["height"])!=(right["width"],right["height"]):raise ValueError("scene geometry differs")
    for key in SHARED_IDENTITIES:
        if valid_hash(left[key])!=valid_hash(right[key]):raise ValueError("matched scene identity differs: "+key)
    reports=[]
    for name in CASES:
        verified={}
        for role,directory,cases in (("reference",reference,left_cases),("candidate",candidate,right_cases)):
            hashes={}
            for key,suffix in HASH_FILES.items():
                path=directory/(name+"-"+suffix)
                actual=sha256(path)
                if actual!=valid_hash(cases[name].get(key)):raise ValueError(role+" file differs from declared hash: "+path.name)
                hashes[suffix]=actual
            controls=directory/(name+"-controls.bin")
            if controls.stat().st_size!=72:raise ValueError("scene controls must match the72-byte runtime parameters")
            hashes["controls.bin"]=sha256(controls)
            verified[role]={"hashes":hashes,**rgba_rgb(directory,name,left["width"],left["height"],images)}
        for kind in ("source.f32","previous-history.f32","features.f32","controls.bin"):
            if verified["reference"]["hashes"][kind]!=verified["candidate"]["hashes"][kind]:
                raise ValueError("scene inputs/history/features/controls are not matched: "+name+"-"+kind)
        metrics=images.compare(reference/(name+"-scene-linear-rgb.pfm"),candidate/(name+"-scene-linear-rgb.pfm"),peak=1.0)
        passed=images.thresholds_passed(metrics,40,None,.99)
        reports.append({"name":name,"provenance":verified,"metrics":metrics,"thresholds_passed":passed})
    return {"format":"OpenNR-amd-synthetic-scene-quality-v1","reference":str(reference),"candidate":str(candidate),
            "source_manifest_sha256":{"reference":sha256(reference/"manifest.json"),"candidate":sha256(candidate/"manifest.json")},
            "synthetic_scene":True,"captured_game_quality_established":False,"nvidia_parity_established":False,
            "motion_review_completed":False,"data_range":1.0,"thresholds":{"min_psnr_db":40,"min_ssim":.99},
            "identities":{"reference":left.get("identity"),"candidate":right.get("identity")},
            "selections":{"reference":left.get("selected"),"candidate":right.get("selected")},
            "actual_kernel_policy_recorded":isinstance(left.get("identity"),dict) and isinstance(right.get("identity"),dict)
                                            and isinstance(left.get("selected"),dict) and isinstance(right.get("selected"),dict),
            "cases":reports,"thresholds_passed":all(item["thresholds_passed"] for item in reports),
            "limitations":["Generated scene regressions preserve HDR values and test reset/temporal composition; they do not establish game motion quality.",
                           "Older fixture manifests omit actual neural kernel identity/policy; raw model/game-shader/input/output hashes remain verified."]}


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference",type=Path,required=True)
    parser.add_argument("--candidate",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    try:
        report=compare(args.reference,args.candidate)
        with args.output.open("x",encoding="utf-8") as file:json.dump(report,file,indent=2,allow_nan=False);file.write("\n")
        print(args.output)
        return 0 if report["thresholds_passed"] else 1
    except (OSError,ValueError) as error:parser.error(str(error));return 2


if __name__=="__main__":raise SystemExit(main())
