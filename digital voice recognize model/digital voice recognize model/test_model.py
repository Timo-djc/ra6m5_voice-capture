import shutil
import tempfile
from pathlib import Path
from typing import Dict, List, Tuple
import numpy as np
import sounddevice as sd
from scipy import ndimage, signal

from train_model import (
    DIGITS,
    HOP_LENGTH,
    MODEL_PATH,
    MODEL_SECONDS,
    SAMPLE_RATE,
    WIN_LENGTH,
    extract_feature_from_audio,
    load_audio_mono,
    normalize_audio,
    trim_and_fit,
)

def load_tflite(model_path: Path):
    import tensorflow as tf

    try:
        interpreter = tf.lite.Interpreter(model_path=str(model_path))
    except ValueError:
        # 兼容 Windows 非 ASCII 路径下 TFLite 读取失败
        temp_model = Path(tempfile.gettempdir()) / "digit_model_temp_int8.tflite"
        shutil.copyfile(str(model_path), str(temp_model))
        interpreter = tf.lite.Interpreter(model_path=str(temp_model))

    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]
    return interpreter, input_detail, output_detail


def infer_tflite(interpreter, input_detail: dict, output_detail: dict, features: np.ndarray) -> np.ndarray:
    if features.ndim == 3:
        features = features[np.newaxis, ...]

    outputs = []
    input_dtype = input_detail["dtype"]
    output_dtype = output_detail["dtype"]
    input_scale, input_zero = input_detail["quantization"]
    output_scale, output_zero = output_detail["quantization"]

    for feat in features:
        tensor = feat[np.newaxis, ...].astype(np.float32)
        if input_dtype == np.int8:
            tensor = np.round(tensor / input_scale + input_zero).astype(np.int8)
        elif input_dtype == np.uint8:
            tensor = np.round(tensor / input_scale + input_zero).astype(np.uint8)
        else:
            tensor = tensor.astype(input_dtype)

        interpreter.set_tensor(input_detail["index"], tensor)
        interpreter.invoke()
        out = interpreter.get_tensor(output_detail["index"])

        if output_dtype in (np.int8, np.uint8):
            out = (out.astype(np.float32) - output_zero) * output_scale
        outputs.append(out[0])

    return np.asarray(outputs, dtype=np.float32)


def _frame_rms(audio: np.ndarray, frame_length: int, hop_length: int) -> np.ndarray:
    if len(audio) < frame_length:
        pad = np.pad(audio, (0, frame_length - len(audio)))
        return np.array([np.sqrt(np.mean(pad**2) + 1e-12)], dtype=np.float32)

    count = 1 + (len(audio) - frame_length) // hop_length
    out = np.empty(count, dtype=np.float32)
    for i in range(count):
        s = i * hop_length
        frame = audio[s : s + frame_length]
        out[i] = np.sqrt(np.mean(frame**2) + 1e-12)
    return out


def _mask_to_segments(mask: np.ndarray) -> List[Tuple[int, int]]:
    change = np.diff(mask.astype(np.int8), prepend=0, append=0)
    starts = np.where(change == 1)[0]
    ends = np.where(change == -1)[0]
    return [(int(s), int(e)) for s, e in zip(starts, ends) if e > s]


def _normalize_segments(segments: List[Tuple[int, int]], expected_count: int, total_len: int) -> List[Tuple[int, int]]:
    if not segments:
        segments = [(0, total_len)]

    while len(segments) > expected_count:
        gaps = [segments[i + 1][0] - segments[i][1] for i in range(len(segments) - 1)]
        i = int(np.argmin(gaps))
        segments = segments[:i] + [(segments[i][0], segments[i + 1][1])] + segments[i + 2 :]

    while len(segments) < expected_count:
        lengths = [b - a for a, b in segments]
        i = int(np.argmax(lengths))
        a, b = segments[i]
        m = (a + b) // 2
        segments = segments[:i] + [(a, m), (m, b)] + segments[i + 1 :]

    if len(segments) != expected_count:
        step = total_len // expected_count
        segments = [(i * step, (i + 1) * step if i < expected_count - 1 else total_len) for i in range(expected_count)]
    return segments


def split_candidates(audio: np.ndarray, expected_count: int = 4) -> List[Dict[str, object]]:
    working = normalize_audio(audio.astype(np.float32) - np.mean(audio))
    frame_len, hop = WIN_LENGTH, HOP_LENGTH
    rms = _frame_rms(working, frame_len, hop)
    total_len = len(working)
    candidates: List[Dict[str, object]] = []

    vad_settings = [
        (0.10, 58, 2, 1),
        (0.13, 65, 2, 1),
        (0.16, 72, 1, 0),
    ]
    for ratio, pctl, dil, ero in vad_settings:
        thr = max(float(np.max(rms)) * ratio, float(np.percentile(rms, pctl)))
        mask = rms > thr
        if dil > 0:
            mask = ndimage.binary_dilation(mask, iterations=dil)
        if ero > 0:
            mask = ndimage.binary_erosion(mask, iterations=ero)

        segs = []
        for fs, fe in _mask_to_segments(mask):
            s = max(0, fs * hop)
            e = min(total_len, fe * hop + frame_len)
            if e - s >= int(0.15 * SAMPLE_RATE):
                segs.append((s, e))

        segs = _normalize_segments(segs, expected_count, total_len)
        candidates.append({"method": f"vad_{ratio}_{pctl}", "segments": segs})

    abs_audio = np.abs(working)
    smooth_n = max(1, int(0.08 * SAMPLE_RATE))
    smooth = np.convolve(abs_audio, np.ones(smooth_n) / smooth_n, mode="same")
    frame_energy = smooth[::hop]

    avg_digit_len = len(working) / max(1, expected_count)
    min_peak_distance = max(1, int(max(0.22 * SAMPLE_RATE, avg_digit_len * 0.45) / hop))
    peaks, _ = signal.find_peaks(
        frame_energy,
        distance=min_peak_distance,
        prominence=max(float(np.percentile(frame_energy, 70) * 0.15), 1e-5),
    )
    if len(peaks) >= expected_count:
        order = np.argsort(frame_energy[peaks])[-expected_count:]
        peaks = sorted(int(peaks[i]) for i in order)
        centers = [int(p * hop + frame_len // 2) for p in peaks]
        bounds = [0]
        for i in range(expected_count - 1):
            bounds.append((centers[i] + centers[i + 1]) // 2)
        bounds.append(len(working))
        segs = [(bounds[i], bounds[i + 1]) for i in range(expected_count)]
        segs = _normalize_segments(segs, expected_count, total_len)
        candidates.append({"method": "energy_peaks", "segments": segs})

    uniq = {}
    for c in candidates:
        uniq[tuple(c["segments"])] = c
    return list(uniq.values())


def clips_from_segments(audio: np.ndarray, segments: List[Tuple[int, int]]) -> Tuple[List[np.ndarray], List[Tuple[float, float]]]:
    clips = []
    ranges = []
    pad = int(0.04 * SAMPLE_RATE)
    for s, e in segments:
        ss = max(0, s - pad)
        ee = min(len(audio), e + pad)
        clip = audio[ss:ee]
        clip = trim_and_fit(clip, target_seconds=MODEL_SECONDS, top_db=40)
        clip = normalize_audio(clip)
        clips.append(clip.astype(np.float32))
        ranges.append((ss / SAMPLE_RATE, ee / SAMPLE_RATE))
    return clips, ranges


def predict_single(interpreter, input_detail, output_detail, duration: float) -> None:
    print(f"开始录音: {duration:.2f}s（单数字）")
    audio = sd.rec(int(duration * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=1, dtype="float32")
    sd.wait()
    audio = audio.squeeze(axis=1).astype(np.float32)

    feat = extract_feature_from_audio(audio)
    prob = infer_tflite(interpreter, input_detail, output_detail, feat)[0]

    top_idx = np.argsort(prob)[-3:][::-1]
    best = int(top_idx[0])
    print(f"预测: {DIGITS[best]} | 置信度 {prob[best]:.4f}")
    print("Top3:")
    for i, idx in enumerate(top_idx, start=1):
        print(f"{i}. {DIGITS[int(idx)]} | {prob[int(idx)]:.4f}")


def predict_four(interpreter, input_detail, output_detail, duration: float, show_candidates: bool) -> None:
    print(f"开始录音: {duration:.2f}s（连续四位）")
    audio = sd.rec(int(duration * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=1, dtype="float32")
    sd.wait()
    audio = audio.squeeze(axis=1).astype(np.float32)
    audio = normalize_audio(audio - np.mean(audio))

    candidates = split_candidates(audio, expected_count=4)
    if not candidates:
        raise RuntimeError("分段失败，请重新录音并减小停顿。")

    scored = []
    for cand in candidates:
        clips, ranges = clips_from_segments(audio, cand["segments"])
        feats = np.asarray([extract_feature_from_audio(c) for c in clips], dtype=np.float32)
        probs = infer_tflite(interpreter, input_detail, output_detail, feats)
        labels = np.argmax(probs, axis=1)
        confs = np.max(probs, axis=1)
        text = "".join(DIGITS[int(v)] for v in labels)

        mean_c = float(np.mean(confs))
        min_c = float(np.min(confs))
        std_c = float(np.std(confs))
        score = 0.65 * mean_c + 0.35 * min_c - 0.10 * std_c

        scored.append(
            {
                "text": text,
                "labels": labels,
                "confs": confs,
                "ranges": ranges,
                "method": cand["method"],
                "score": score,
            }
        )

    scored.sort(key=lambda x: x["score"], reverse=True)
    best = scored[0]
    print(f"预测4位: {best['text']} | 分段策略={best['method']} | 分数={best['score']:.4f}")
    for i in range(4):
        a, b = best["ranges"][i]
        print(f"{i+1}: {DIGITS[int(best['labels'][i])]} | conf={best['confs'][i]:.4f} | {a:.2f}s~{b:.2f}s")

    if show_candidates:
        print("候选Top3:")
        for i, item in enumerate(scored[:3], start=1):
            print(f"{i}. {item['text']} | {item['method']} | score={item['score']:.4f}")


def ask_float(prompt: str, default_value: float) -> float:
    text = input(f"{prompt}（默认 {default_value}）: ").strip()
    if not text:
        return default_value
    try:
        return float(text)
    except ValueError:
        print("输入无效，使用默认值。")
        return default_value


def main() -> None:
    print("\nPC端语音测试")

    default_model = MODEL_PATH
    model_input = input(f"输入模型路径（回车使用默认 {default_model}）: ").strip()
    model_path = Path(model_input) if model_input else default_model
    if not model_path.exists():
        raise RuntimeError(f"模型不存在: {model_path}，请先运行 python train_model.py")

    interpreter, input_detail, output_detail = load_tflite(model_path)

    while True:
        print("\n选择模式:")
        print("1 -> 单数字识别")
        print("4 -> 连续四位识别")
        print("q -> 退出")
        choice = input("请输入: ").strip().lower()

        if choice == "q":
            print("已退出。")
            break
        if choice == "1":
            duration = ask_float("单数字录音时长(秒)", 1.5)
            predict_single(interpreter, input_detail, output_detail, duration=duration)
        elif choice == "4":
            duration = ask_float("连续四位录音时长(秒)", 4.8)
            show_text = input("是否显示候选分段结果? (y/n, 默认n): ").strip().lower()
            show_candidates = show_text == "y"
            predict_four(interpreter, input_detail, output_detail, duration=duration, show_candidates=show_candidates)
        else:
            print("输入无效，请输入 1 / 4 / q。")


if __name__ == "__main__":
    main()
