"""Compare matched scene-linear shader exports without assigning a quality verdict.

PSNR/SSIM use the caller's explicit linear RGB data range (default 1.0). SSIM
uses an 11x11 Gaussian window, sigma 1.5, population moments, K1=.01/K2=.03,
averaged over valid interior pixels and RGB channels. HDR values remain
unclamped for these metrics. Generated previews use a separate labeled
Reinhard+sRGB curve solely to inspect the local synthetic scene diagnostic.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def d3d_half_store(values):
    """Finite float32 to half, Direct3D resource-format RTZ with saturation.

    Direct3D 11.3 Functional Specification section 3.2.2 requires RTZ for
    high-to-low float format conversion and finite overflow saturation.
    This oracle matched all 409,600 channels of the RX9070XT bridge capture.
    """
    values = np.asarray(values, dtype=np.float32)
    if not np.isfinite(values).all():
        raise ValueError("nonfinite D3D half-store input")
    bits = values.view(np.uint32)
    sign = ((bits >> 16) & 0x8000).astype(np.uint16)
    exponent = ((bits >> 23) & 255).astype(np.int32) - 112
    mantissa = bits & 0x7fffff
    result = sign.copy()
    normal = (exponent > 0) & (exponent < 31)
    result[normal] |= ((exponent[normal].astype(np.uint32) << 10) | (mantissa[normal] >> 13)).astype(np.uint16)
    subnormal = (exponent <= 0) & (exponent >= -10)
    result[subnormal] |= ((mantissa[subnormal] | 0x800000) >> (14 - exponent[subnormal]).astype(np.uint32)).astype(np.uint16)
    result[exponent >= 31] |= np.uint16(0x7bff)
    return result.view(np.float16).astype(np.float32)


def gaussian(values):
    axis = np.arange(-5, 6, dtype=np.float64)
    kernel = np.exp(-axis * axis / (2 * 1.5 * 1.5))
    kernel /= kernel.sum()
    result = values
    for dimension in (0, 1):
        padding = [(0, 0)] * result.ndim
        padding[dimension] = (5, 5)
        padded = np.pad(result, padding, mode="reflect")
        blurred = np.zeros_like(result)
        for offset, weight in enumerate(kernel):
            slices = [slice(None)] * result.ndim
            slices[dimension] = slice(offset, offset + result.shape[dimension])
            blurred += weight * padded[tuple(slices)]
        result = blurred
    return result


def metrics(actual, reference, data_range):
    if not np.isfinite(actual).all() or not np.isfinite(reference).all():
        raise ValueError("nonfinite composed RGB")
    actual = actual.astype(np.float64)
    reference = reference.astype(np.float64)
    difference = actual - reference
    mse = float(np.mean(difference * difference))
    mu_a, mu_b = gaussian(actual), gaussian(reference)
    var_a = np.maximum(gaussian(actual * actual) - mu_a * mu_a, 0)
    var_b = np.maximum(gaussian(reference * reference) - mu_b * mu_b, 0)
    covariance = gaussian(actual * reference) - mu_a * mu_b
    c1, c2 = (.01 * data_range) ** 2, (.03 * data_range) ** 2
    ssim = ((2 * mu_a * mu_b + c1) * (2 * covariance + c2) /
            ((mu_a * mu_a + mu_b * mu_b + c1) * (var_a + var_b + c2)))
    return {
        "samples": int(actual.size), "rmse": math.sqrt(mse),
        "maxAbs": float(np.max(np.abs(difference))),
        "psnrDb": None if mse == 0 else 10 * math.log10(data_range * data_range / mse),
        "bitExact": bool(np.array_equal(actual.view(np.uint64), reference.view(np.uint64))),
        "ssimGaussian11Rgb": float(np.mean(ssim[5:-5, 5:-5])),
        "referenceMinimum": float(np.min(reference)), "referenceMaximum": float(np.max(reference)),
        "actualMinimum": float(np.min(actual)), "actualMaximum": float(np.max(actual)),
    }


def preview(values):
    positive = np.maximum(values, 0)
    linear = positive / (1 + positive)
    encoded = np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - .055)
    return np.uint8(np.clip(encoded * 255 + .5, 0, 255))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--actual", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--data-range", type=float, default=1.0)
    parser.add_argument("--previews", action="store_true")
    args = parser.parse_args()
    if not math.isfinite(args.data_range) or args.data_range <= 0:
        raise ValueError("data range must be finite and positive")
    reference = json.loads((args.reference / "manifest.json").read_text())
    actual = json.loads((args.actual / "manifest.json").read_text())
    for field in ("format", "width", "height", "modelManifestSha256", "preprocessSpvSha256", "compositeSpvSha256", "domain", "syntheticScene", "capturedGameFrames"):
        if actual[field] != reference[field]:
            raise ValueError(f"reference differs in {field}")
    if actual["format"] != "OpenNR-local-scene-composite-v1":
        raise ValueError("unsupported scene fixture")
    if reference["capturedGameFrames"] and actual.get("sourceCaptureManifestSha256") != reference.get("sourceCaptureManifestSha256"):
        raise ValueError("recorded source capture differs")
    names = [case["name"] for case in reference["cases"]]
    if names != [case["name"] for case in actual["cases"]]:
        raise ValueError("case lists differ")
    width, height = reference["width"], reference["height"]
    output = args.output or args.actual / "composite-comparison.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    results = {}
    for name in names:
        for kind in ("source.f32", "previous-history.f32", "controls.bin", "features.f32"):
            file = name + "-" + kind
            if sha(args.actual / file) != sha(args.reference / file):
                raise ValueError("matched input differs: " + file)
        file = name + "-scene-linear-rgba.f32"
        a = np.fromfile(args.actual / file, dtype="<f4")
        b = np.fromfile(args.reference / file, dtype="<f4")
        if a.size != width * height * 4 or b.size != a.size:
            raise ValueError("composed output extent differs")
        a, b = a.reshape(height, width, 4), b.reshape(height, width, 4)
        if not np.array_equal(a[:, :, 3].view(np.uint32), b[:, :, 3].view(np.uint32)):
            raise ValueError("alpha bits differ")
        result = metrics(a[:, :, :3], b[:, :, :3], args.data_range)
        result["alphaBitExact"] = True
        half_actual, half_reference = d3d_half_store(a[:, :, :3]), d3d_half_store(b[:, :, :3])
        result["d3dRtzHalfSaturatedValues"] = int(np.count_nonzero(np.abs(a[:, :, :3]) > 65504) + np.count_nonzero(np.abs(b[:, :, :3]) > 65504))
        result["d3dRtzHalfSceneRgb"] = metrics(half_actual, half_reference, args.data_range)
        results[name] = result
        print(f"{name}: scene-linear RMSE {result['rmse']:.8g}; max {result['maxAbs']:.8g}; "
              f"PSNR(range={args.data_range}) {result['psnrDb']}; Gaussian SSIM {result['ssimGaussian11Rgb']:.8g}")
        if args.previews:
            from PIL import Image
            Image.fromarray(preview(b[:, :, :3])).save(output.parent / (name + "-reference-reinhard-srgb.png"))
            Image.fromarray(preview(a[:, :, :3])).save(output.parent / (name + "-actual-reinhard-srgb.png"))
            error = np.abs(a[:, :, :3] - b[:, :, :3])
            Image.fromarray(np.uint8(np.clip(error * (255 / args.data_range) * 8, 0, 255))).save(output.parent / (name + "-absolute-error-times8.png"))
    report = {
        "format": "OpenNR-local-scene-comparison-v1", "syntheticScene": reference["syntheticScene"],
        "capturedGameFrames": reference["capturedGameFrames"], "nvidiaParityEstablished": False, "visualQualityAccepted": False,
        "referenceBackend": reference["backend"], "actualBackend": actual["backend"],
        "domain": reference["domain"], "dataRange": args.data_range,
        "halfPublication": "CPU Direct3D resource-format RTZ binary16 prediction with finite saturation; oracle matched 409600 actual RX9070XT bridge channels; replay metrics are not captured D3D12 textures",
        "halfPublicationSource": "https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#3.2.2%20Floating%20Point%20Conversion",
        "ssim": "Gaussian11 sigma1.5 population moments K1=.01 K2=.03 valid interior RGB mean",
        "matchedInputs": ["source", "previous history", "controls", "preprocessed features", "model", "game shader SPVs"],
        "modelManifestSha256": reference["modelManifestSha256"],
        "sourceCaptureManifestSha256": reference.get("sourceCaptureManifestSha256"),
        "previews": "Reinhard + sRGB diagnostic only; not the game's HDR tonemapper" if args.previews else None,
        "cases": results,
    }
    output.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    print("Diagnostics saved to", output)


if __name__ == "__main__":
    main()
