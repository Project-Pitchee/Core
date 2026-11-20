#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path

import joblib
import numpy as np
import onnxruntime as ort
import soundfile as sf
from scipy.signal import resample_poly


SAMPLE_RATE = 16_000
FRAME_SAMPLES = 320
HOP_SAMPLES = 160
MAX_SECONDS = 12.0
CLASS_NAMES = ("male_natural", "male_unnatural", "female_natural")
COMPRESSED_SUFFIXES = {".m4a", ".mp4", ".aac", ".mp3", ".webm", ".ogg"}
SCORE_ANCHORS = (
    (0.0001, 0.0),
    (0.001, 2.0),
    (0.01, 6.0),
    (0.05, 13.0),
    (0.10, 19.0),
    (0.30, 31.0),
    (0.50, 39.0),
    (0.70, 50.0),
    (0.82, 60.0),
    (0.90, 72.0),
    (0.92, 80.0),
    (0.97, 85.0),
    (0.985, 88.0),
    (0.995, 94.0),
    (0.999, 97.0),
)


def _trim_and_crop(
    signal: np.ndarray,
    sample_rate: int,
) -> tuple[np.ndarray, dict[str, float | int]]:
    frames = np.lib.stride_tricks.sliding_window_view(signal, FRAME_SAMPLES)[
        ::HOP_SAMPLES
    ]
    rms = np.sqrt(np.mean(np.square(frames, dtype=np.float64), axis=1) + 1e-12)
    rms_db = 20.0 * np.log10(np.maximum(rms, 1e-8))
    if rms_db.size == 0:
        return signal, {
            "speech_seconds": len(signal) / sample_rate,
            "active_frames": 0,
        }

    low = float(np.quantile(rms_db, 0.10))
    high = float(np.quantile(rms_db, 0.95))
    threshold = max(low + 8.0, high - 35.0)
    active = rms_db >= threshold
    if active.sum() < max(3, int(0.01 * len(active))):
        active = rms_db >= high - 45.0
    active_indices = np.flatnonzero(active)
    if active_indices.size == 0:
        return signal, {
            "speech_seconds": len(signal) / sample_rate,
            "active_frames": 0,
        }

    margin = int(0.15 * sample_rate)
    start = max(0, int(active_indices[0]) * HOP_SAMPLES - margin)
    end = min(
        len(signal),
        int(active_indices[-1]) * HOP_SAMPLES + FRAME_SAMPLES + margin,
    )
    cropped = signal[start:end]
    maximum = int(MAX_SECONDS * sample_rate)
    if len(cropped) > maximum:
        frame_start = max(0, start // HOP_SAMPLES)
        frame_end = min(len(active), frame_start + len(cropped) // HOP_SAMPLES)
        local_active = active[frame_start:frame_end]
        window_frames = maximum // HOP_SAMPLES
        if local_active.size > window_frames:
            cumulative = np.concatenate(
                [[0], np.cumsum(local_active, dtype=np.int64)]
            )
            scores = cumulative[window_frames:] - cumulative[:-window_frames]
            best = int(np.argmax(scores))
            # This is intentionally identical to vf3.audio._trim_and_crop.
            cropped_start = max(0, best * HOP_SAMPLES - margin)
            cropped = cropped[cropped_start : cropped_start + maximum]
        else:
            cropped = cropped[:maximum]
    speech_seconds = float(active.sum() * HOP_SAMPLES / sample_rate)
    return np.asarray(cropped, dtype=np.float32), {
        "speech_seconds": speech_seconds,
        "active_frames": int(active.sum()),
    }


def read_audio(path: Path) -> tuple[np.ndarray, dict[str, float | int]]:
    signal, source_rate = sf.read(path, dtype="float32", always_2d=True)
    signal = np.asarray(signal.mean(axis=1), dtype=np.float32)
    if source_rate != SAMPLE_RATE:
        divisor = int(np.gcd(source_rate, SAMPLE_RATE))
        signal = resample_poly(
            signal,
            SAMPLE_RATE // divisor,
            source_rate // divisor,
        ).astype(np.float32)
    signal -= float(signal.mean()) if signal.size else 0.0
    source_seconds = len(signal) / SAMPLE_RATE
    if signal.size >= FRAME_SAMPLES:
        signal, trim_metadata = _trim_and_crop(signal, SAMPLE_RATE)
    else:
        signal = signal[: int(SAMPLE_RATE * MAX_SECONDS)]
        trim_metadata = {
            "speech_seconds": len(signal) / SAMPLE_RATE,
            "active_frames": int(len(signal) / HOP_SAMPLES),
        }
    minimum = int(0.4 * SAMPLE_RATE)
    if signal.size < minimum:
        signal = np.pad(signal, (0, minimum - signal.size))
    metadata = {
        "source_seconds": source_seconds,
        "analyzed_seconds": len(signal) / SAMPLE_RATE,
        **trim_metadata,
    }
    return np.asarray(signal, dtype=np.float32), metadata


def normalize_waveform(signal: np.ndarray) -> np.ndarray:
    values = np.asarray(signal, dtype=np.float64)
    if values.size == 0:
        return values.astype(np.float32)
    return ((values - values.mean()) / np.sqrt(values.var() + 1e-7)).astype(
        np.float32
    )


def calibrated_probabilities(bundle: dict, features: np.ndarray) -> np.ndarray:
    raw = bundle["model"].predict_proba(features)[0]
    logits = np.log(np.clip(raw, 1e-8, 1.0)) / float(bundle["temperature"])
    logits -= logits.max()
    probabilities = np.exp(logits)
    return probabilities / probabilities.sum()


def reference_score(probability: float) -> float:
    clipped = min(max(float(probability), 1e-6), 1.0 - 1e-6)
    logit = math.log(clipped / (1.0 - clipped))
    anchors = [
        (math.log(value / (1.0 - value)), score)
        for value, score in SCORE_ANCHORS
    ]
    score = anchors[-1][1]
    if logit <= anchors[0][0]:
        score = anchors[0][1]
    else:
        for (left_logit, left_score), (right_logit, right_score) in zip(
            anchors, anchors[1:]
        ):
            if logit <= right_logit:
                score = left_score + (logit - left_logit) / (
                    right_logit - left_logit
                ) * (right_score - left_score)
                break
    return max(1.0, min(99.0, score))


def calibrated_score(probability: float) -> float:
    clipped = min(max(float(probability), 1e-6), 1.0 - 1e-6)
    logit = math.log(clipped / (1.0 - clipped))
    z = max(-40.0, min(40.0, logit / 3.0))
    return max(1.0, min(99.0, 100.0 / (1.0 + math.exp(-z))))


def confidence_ceiling(confidence: float) -> float:
    if confidence < 0.85:
        return 49.9
    return min(100.0, 50.0 + 50.0 * (confidence - 0.85) / 0.15)


def display_scores(probabilities: np.ndarray) -> dict[str, float | bool]:
    confidence = float(probabilities.max())
    timbre_base = reference_score(float(probabilities[2]))
    timbre_cap = confidence_ceiling(confidence)
    timbre = min(timbre_base, timbre_cap)
    return {
        "confidence": confidence,
        "timbre": timbre,
        "timbre_base": timbre_base,
        "timbre_cap": timbre_cap,
        "naturalness": calibrated_score(
            float(probabilities[0] + probabilities[2])
        ),
        "passed": timbre >= 50.0,
    }


def convert_for_core(path: Path, directory: Path) -> Path:
    if path.suffix.lower() not in COMPRESSED_SUFFIXES:
        return path
    converted = directory / "decoded_16k.wav"
    subprocess.run(
        [
            "ffmpeg",
            "-v",
            "error",
            "-y",
            "-i",
            str(path),
            "-ac",
            "1",
            "-ar",
            str(SAMPLE_RATE),
            str(converted),
        ],
        check=True,
    )
    return converted


def run_core(cli: Path, model: Path, audio: Path) -> dict:
    process = subprocess.run(
        [
            str(cli),
            "--json",
            "--model",
            str(model),
            str(audio),
        ],
        text=True,
        capture_output=True,
        check=False,
    )
    if process.returncode != 0:
        raise RuntimeError(
            f"pitchee_v0_cli failed ({process.returncode}): {process.stderr.strip()}"
        )
    return json.loads(process.stdout)


def compare_record(
    path: Path,
    bundle: dict,
    session: ort.InferenceSession,
    core_result: dict,
) -> tuple[dict, list[str]]:
    signal, metadata = read_audio(path)
    normalized = normalize_waveform(signal)
    hidden = session.run(
        ["hidden"],
        {
            "input_values": normalized[None, :].astype(np.float32),
            "attention_mask": np.ones((1, len(normalized)), dtype=np.int64),
        },
    )[0]
    features = hidden.mean(axis=1).astype(np.float32)
    probabilities = calibrated_probabilities(bundle, features)
    scores = display_scores(probabilities)
    expected = {
        "prediction": CLASS_NAMES[int(probabilities.argmax())],
        "probabilities": {
            name: float(probabilities[index])
            for index, name in enumerate(CLASS_NAMES)
        },
        **scores,
        **metadata,
    }

    failures: list[str] = []
    if expected["prediction"] != core_result["prediction"]:
        failures.append(
            f"prediction {core_result['prediction']} != {expected['prediction']}"
        )
    for name in CLASS_NAMES:
        actual = float(core_result["probabilities"][name])
        wanted = float(expected["probabilities"][name])
        if abs(actual - wanted) > 1e-4:
            failures.append(f"p({name}) {actual:.9f} != {wanted:.9f}")
    for field in ("confidence", "timbre", "timbre_base", "timbre_cap", "naturalness"):
        actual_key = {
            "timbre": "timbre_score",
            "naturalness": "naturalness_score",
        }.get(field, field)
        actual = float(core_result[actual_key])
        wanted = float(expected[field])
        if abs(actual - wanted) > 0.05:
            failures.append(f"{field} {actual:.6f} != {wanted:.6f}")
    if bool(core_result["passed"]) != bool(expected["passed"]):
        failures.append(f"passed {core_result['passed']} != {expected['passed']}")
    for field in ("source_seconds", "analyzed_seconds", "speech_seconds"):
        actual = float(core_result[field])
        wanted = float(expected[field])
        if abs(actual - wanted) > 1e-6:
            failures.append(f"{field} {actual:.9f} != {wanted:.9f}")
    return expected, failures


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", type=Path, nargs="+")
    parser.add_argument("--cli", type=Path, default=Path("build-v0/pitchee_v0_cli"))
    parser.add_argument(
        "--model",
        type=Path,
        default=Path("models/wavlm-large-l12-hqq4-b128-convw8.onnx"),
    )
    parser.add_argument(
        "--classifier",
        type=Path,
        default=Path(
            "/Users/michaelzhan/Develop/Pithcee-Training/artifacts/model/model.joblib"
        ),
    )
    parser.add_argument("--threads", type=int, default=2)
    args = parser.parse_args()

    bundle = joblib.load(args.classifier)
    options = ort.SessionOptions()
    options.intra_op_num_threads = args.threads
    options.inter_op_num_threads = 1
    session = ort.InferenceSession(
        str(args.model),
        sess_options=options,
        providers=["CPUExecutionProvider"],
    )

    failed = 0
    with tempfile.TemporaryDirectory(prefix="pitchee_v0_parity_") as directory:
        temporary = Path(directory)
        for source in args.audio:
            core_input = convert_for_core(source, temporary)
            core_result = run_core(args.cli, args.model, core_input)
            reference_input = (
                core_input
                if source.suffix.lower() in COMPRESSED_SUFFIXES
                else source
            )
            expected, failures = compare_record(
                reference_input, bundle, session, core_result
            )
            prediction = expected["prediction"]
            female = expected["probabilities"]["female_natural"]
            timbre = expected["timbre"]
            naturalness = expected["naturalness"]
            status = "PASS" if not failures else "FAIL"
            print(
                f"{status} {source.name}: {prediction} "
                f"P(female)={female:.6f} timbre={timbre:.3f} "
                f"naturalness={naturalness:.3f}"
            )
            for failure in failures:
                print(f"  {failure}")
            failed += bool(failures)
    if failed:
        raise SystemExit(f"{failed}/{len(args.audio)} clips mismatched")


if __name__ == "__main__":
    main()
