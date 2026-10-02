"""Compare matched PPM (normalized SDR) or PFM (float) captures.

Supply captures from the same frame and pipeline point. Temporal comparisons
require aligned reference sequences; this utility does not estimate motion.
SSIM uses NumPy, an 11x11 Gaussian (sigma 1.5), population moments, K1=.01,
K2=.03, the explicit peak/data range, valid windows, and a mean over channels.
"""
from __future__ import annotations
import argparse
import array
import json
import math
import sys
from pathlib import Path

CAPTURE_KEYS = (
    "frame_id", "sequence_id", "seed", "model_sha256", "input_sha256", "controls",
    "render_resolution", "output_resolution", "pipeline_point", "color_space",
    "pre_exposure", "exposure_scale", "jitter", "motion_scale", "reset", "history_frame_ids",
)


def ssim(a: array.array, b: array.array, width: int, height: int, channels: int, peak: float) -> float | None:
    if width < 11 or height < 11:
        return None
    try:
        import numpy as np
    except ImportError:
        return None
    x = np.asarray(a, dtype=np.float64).reshape(height, width, channels)
    y = np.asarray(b, dtype=np.float64).reshape(height, width, channels)
    coords = np.arange(-5, 6, dtype=np.float64)
    weights = np.exp(-(coords * coords) / (2 * 1.5 * 1.5))
    weights /= weights.sum()

    def blur(values):
        # Separable convolution avoids a huge 11x11 sliding-window allocation.
        across = np.zeros((height, width - 10), dtype=np.float64)
        for at, weight in enumerate(weights):
            across += values[:, at:at + width - 10] * weight
        result = np.zeros((height - 10, width - 10), dtype=np.float64)
        for at, weight in enumerate(weights):
            result += across[at:at + height - 10, :] * weight
        return result

    scores = []
    c1, c2 = (0.01 * peak) ** 2, (0.03 * peak) ** 2
    for channel in range(channels):
        left, right = x[:, :, channel], y[:, :, channel]
        mx, my = blur(left), blur(right)
        vx = np.maximum(blur(left * left) - mx * mx, 0)
        vy = np.maximum(blur(right * right) - my * my, 0)
        covariance = blur(left * right) - mx * my
        # Roundoff can put covariance slightly outside its valid bounds.
        bound = np.sqrt(vx * vy)
        covariance = np.clip(covariance, -bound, bound)
        score = ((2 * mx * my + c1) * (2 * covariance + c2)) / ((mx * mx + my * my + c1) * (vx + vy + c2))
        value = float(score.mean())
        if not math.isfinite(value):
            raise ValueError("SSIM overflow; check capture units and data range")
        scores.append(value)
    return math.fsum(scores) / channels


def verify_metadata(pair: dict) -> dict:
    if not isinstance(pair, dict) or not isinstance(pair.get("reference"), dict) or not isinstance(pair.get("candidate"), dict):
        raise ValueError("metadata must have reference and candidate capture objects")
    a, b = pair["reference"], pair["candidate"]
    json.dumps(pair, allow_nan=False)  # Reject nonfinite scalars anywhere in controls.
    for key in CAPTURE_KEYS:
        if key not in a or key not in b or a[key] != b[key]:
            raise ValueError(f"missing or mismatched capture metadata: {key}")
    for key in ("frame_id", "seed"):
        if type(a[key]) is not int or a[key] < 0:
            raise ValueError(f"metadata {key} must be a nonnegative integer")
    for key in ("model_sha256", "input_sha256"):
        if not isinstance(a[key], str) or len(a[key]) != 64 or any(c not in "0123456789abcdef" for c in a[key]):
            raise ValueError(f"metadata {key} must be a lowercase SHA-256")
    for key in ("sequence_id", "pipeline_point", "color_space"):
        if not isinstance(a[key], str) or not a[key].strip():
            raise ValueError(f"metadata {key} must be a nonempty string")
    if a["pipeline_point"] not in ("pre-fsr", "post-fsr"):
        raise ValueError("metadata pipeline_point must be pre-fsr or post-fsr composed RGB")
    if not isinstance(a["controls"], dict) or not a["controls"]:
        raise ValueError("metadata controls must be a nonempty object")
    for key in ("render_resolution", "output_resolution"):
        if not isinstance(a[key], list) or len(a[key]) != 2 or any(type(v) is not int or v <= 0 for v in a[key]):
            raise ValueError(f"invalid metadata {key}")
    for key in ("pre_exposure", "exposure_scale"):
        if type(a[key]) not in (int, float) or not math.isfinite(a[key]) or a[key] <= 0:
            raise ValueError(f"invalid metadata {key}")
    for key in ("jitter", "motion_scale"):
        if not isinstance(a[key], list) or len(a[key]) != 2 or any(type(v) not in (int, float) or not math.isfinite(v) for v in a[key]):
            raise ValueError(f"invalid metadata {key}")
    history = a["history_frame_ids"]
    if type(a["reset"]) is not bool or not isinstance(history, list) or any(type(v) is not int or v < 0 or v >= a["frame_id"] for v in history):
        raise ValueError("invalid history/reset metadata")
    if history != sorted(set(history)) or (a["reset"] and history):
        raise ValueError("reset captures must have empty history; history IDs must be ordered and unique")
    return {key: a[key] for key in CAPTURE_KEYS}


def read_json(path: Path) -> dict:
    if path.stat().st_size > 4 * 1024 * 1024:
        raise ValueError("capture manifest exceeds 4 MiB")
    result = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(result, dict):
        raise ValueError("capture manifest must be an object")
    return result


def read_image(path: Path) -> tuple[int, int, int, array.array]:
    if path.stat().st_size > 128 * 1024 * 1024:
        raise ValueError("capture is larger than the 128 MiB input limit")
    with path.open("rb") as source:
        magic = source.readline().strip()
        if magic not in (b"P6", b"PF", b"Pf"):
            raise ValueError("capture must be binary RGB PPM (P6) or float PFM (PF/Pf)")

        def header() -> bytes:
            for line in source:
                content = line.split(b"#", 1)[0].strip()
                if content:
                    return content
            raise ValueError("truncated image header")

        dimensions = header().split()
        if len(dimensions) != 2:
            raise ValueError("invalid image dimensions")
        width, height = map(int, dimensions)
        channels = 1 if magic == b"Pf" else 3
        count = width * height * channels
        if width <= 0 or height <= 0 or count > 64 * 1024 * 1024:
            raise ValueError("invalid or excessive image dimensions")
        scale = header()
        data = source.read()
    if magic == b"P6":
        maximum = int(scale)
        if not 1 <= maximum <= 65535:
            raise ValueError("invalid PPM maximum")
        if len(data) != count * (1 if maximum < 256 else 2):
            raise ValueError("PPM pixel length mismatch")
        if maximum < 256:
            values = list(data)
        else:
            values = [(data[i] << 8) | data[i + 1] for i in range(0, len(data), 2)]
        if any(value > maximum for value in values):
            raise ValueError("PPM sample exceeds its maximum")
        pixels = array.array("d", (value / maximum for value in values))
    else:
        factor = float(scale)
        if not math.isfinite(factor) or factor == 0:
            raise ValueError("invalid PFM scale")
        if len(data) != count * 4:
            raise ValueError("PFM pixel length mismatch")
        values = array.array("f")
        values.frombytes(data)
        if (factor < 0) != (sys.byteorder == "little"):
            values.byteswap()
        stride = width * channels
        pixels = array.array("d", (value * abs(factor) for y in range(height - 1, -1, -1) for value in values[y * stride:(y + 1) * stride]))
    if any(not math.isfinite(value) for value in pixels):
        raise ValueError("capture contains NaN or infinity")
    return width, height, channels, pixels


def compare(reference: Path, candidate: Path, peak: float = 1.0) -> dict:
    if not math.isfinite(peak) or peak <= 0:
        raise ValueError("peak must be finite and positive")
    rw, rh, rc, a = read_image(reference)
    cw, ch, cc, b = read_image(candidate)
    if (rw, rh, rc) != (cw, ch, cc):
        raise ValueError("capture dimensions/channels differ")
    errors = array.array("d", (abs(left - right) for left, right in zip(a, b)))
    mse = math.fsum(error * error for error in errors) / len(errors)
    return {
        "reference": str(reference), "candidate": str(candidate),
        "width": rw, "height": rh, "channels": rc, "peak": peak,
        "mae": math.fsum(errors) / len(errors), "rmse": math.sqrt(mse),
        "max_absolute_error": max(errors), "different_samples": sum(error != 0 for error in errors),
        "psnr_db": None if mse == 0 else 10 * math.log10(peak * peak / mse),
        "exact": mse == 0,
        "ssim": ssim(a, b, rw, rh, rc, peak),
        "ssim_method": "11x11 Gaussian sigma=1.5, K1=.01 K2=.03, population moments, channel mean, valid windows (5-pixel border excluded), data_range=peak",
        "psnr_note": "null means exact/infinite PSNR; float/HDR comparisons use the explicitly recorded peak",
    }


def thresholds_passed(report: dict, min_psnr: float | None, max_mae: float | None, min_ssim: float | None) -> bool:
    if any(value is not None for value in (min_psnr, max_mae, min_ssim)) and report["channels"] != 3:
        raise ValueError("composed RGB quality gates require three-channel captures")
    if min_ssim is not None and report["ssim"] is None:
        raise ValueError("SSIM requires NumPy and captures at least 11x11")
    return ((min_psnr is None or report["exact"] or report["psnr_db"] >= min_psnr)
            and (max_mae is None or report["mae"] <= max_mae)
            and (min_ssim is None or report["ssim"] >= min_ssim))


def compare_sequence(path: Path, min_psnr: float | None = None, max_mae: float | None = None, min_ssim: float | None = None) -> dict:
    manifest = read_json(path)
    if manifest.get("format") != "OpenNR-quality-sequence-v1" or not isinstance(manifest.get("frames"), list) or not manifest["frames"]:
        raise ValueError("sequence manifest must declare OpenNR-quality-sequence-v1 and nonempty frames")
    peak = manifest.get("data_range")
    if type(peak) not in (int, float) or not math.isfinite(peak) or peak <= 0:
        raise ValueError("sequence data_range must be finite and positive")
    reports, last = [], None
    for frame in manifest["frames"]:
        if not isinstance(frame, dict) or not isinstance(frame.get("reference"), str) or not isinstance(frame.get("candidate"), str):
            raise ValueError("sequence frame must declare reference and candidate file paths")
        metadata = verify_metadata(frame.get("metadata"))
        if last and (metadata["sequence_id"] != last["sequence_id"] or metadata["frame_id"] <= last["frame_id"]):
            raise ValueError("sequence IDs must match and frame IDs must increase")
        if last and metadata["frame_id"] != last["frame_id"] + 1 and not metadata["reset"]:
            raise ValueError("a sequence frame gap requires a history reset")
        report = compare(path.parent / frame["reference"], path.parent / frame["candidate"], peak)
        expected = metadata["render_resolution"] if metadata["pipeline_point"] == "pre-fsr" else metadata["output_resolution"]
        if [report["width"], report["height"]] != expected:
            raise ValueError("capture dimensions disagree with pipeline metadata")
        report["capture_metadata"] = metadata
        report["thresholds_passed"] = thresholds_passed(report, min_psnr, max_mae, min_ssim)
        reports.append(report)
        last = metadata
    ssims = [report["ssim"] for report in reports]
    return {
        "source": str(path), "frames": reports, "frame_count": len(reports), "capture_metadata_verified": True,
        "minimum_ssim": min(ssims) if all(value is not None for value in ssims) else None,
        "thresholds": {"min_psnr_db": min_psnr, "max_mae": max_mae, "min_ssim": min_ssim},
        "thresholds_passed": all(report["thresholds_passed"] for report in reports) if any(value is not None for value in (min_psnr, max_mae, min_ssim)) else None,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, nargs="?")
    parser.add_argument("candidate", type=Path, nargs="?")
    parser.add_argument("--peak", type=float, default=1.0)
    parser.add_argument("--min-psnr", type=float)
    parser.add_argument("--max-mae", type=float)
    parser.add_argument("--min-ssim", type=float)
    parser.add_argument("--metadata", type=Path, help="paired capture metadata JSON; required for thresholds")
    parser.add_argument("--sequence", type=Path, help="manifest of matched frames with per-capture metadata")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        for threshold in (args.min_psnr, args.max_mae, args.min_ssim):
            if threshold is not None and (not math.isfinite(threshold) or threshold < 0):
                raise ValueError("quality thresholds must be finite and nonnegative")
        if args.min_ssim is not None and args.min_ssim > 1:
            raise ValueError("minimum SSIM cannot exceed 1")
        has_thresholds = any(value is not None for value in (args.min_psnr, args.max_mae, args.min_ssim))
        if args.sequence:
            if args.reference or args.candidate or args.metadata:
                raise ValueError("--sequence supplies paths and metadata; omit positional captures and --metadata")
            report = compare_sequence(args.sequence, args.min_psnr, args.max_mae, args.min_ssim)
            passed = report["thresholds_passed"] is not False
        else:
            if not args.reference or not args.candidate:
                raise ValueError("two capture paths or --sequence are required")
            if has_thresholds and not args.metadata:
                raise ValueError("threshold gates require matched --metadata or --sequence")
            metadata = verify_metadata(read_json(args.metadata)) if args.metadata else None
            report = compare(args.reference, args.candidate, args.peak)
            if metadata:
                expected = metadata["render_resolution"] if metadata["pipeline_point"] == "pre-fsr" else metadata["output_resolution"]
                if [report["width"], report["height"]] != expected:
                    raise ValueError("capture dimensions disagree with pipeline metadata")
                report["capture_metadata"] = metadata
            report["capture_metadata_verified"] = metadata is not None
            passed = thresholds_passed(report, args.min_psnr, args.max_mae, args.min_ssim)
            report["thresholds"] = {"min_psnr_db": args.min_psnr, "max_mae": args.max_mae, "min_ssim": args.min_ssim}
            report["thresholds_passed"] = passed if has_thresholds else None
        text = json.dumps(report, indent=2, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as destination:
                destination.write(text)
        print(text, end="")
        return 0 if passed else 1
    except (OSError, ValueError) as error:
        parser.error(str(error))
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
