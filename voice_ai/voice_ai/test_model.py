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
    N_FFT,
    N_MELS,
    PRE_EMPHASIS,
    SAMPLE_RATE,
    TARGET_FRAMES,
    WIN_LENGTH,
    load_audio_mono,
    normalize_audio,
    pre_emphasis,
    trim_and_fit,
)

# ========= 特征提取函数（与train_model.py完全一致） =========
_MEL_CACHE: Dict[Tuple[int, int, int, int], np.ndarray] = {}


def _hz_to_mel(freq: np.ndarray) -> np.ndarray:
    """赫兹转梅尔"""
    return 2595.0 * np.log10(1.0 + freq / 700.0)


def _mel_to_hz(mel: np.ndarray) -> np.ndarray:
    """梅尔转赫兹"""
    return 700.0 * (10 ** (mel / 2595.0) - 1.0)


def _mel_filter_bank(sr: int, n_fft: int, n_mels: int, fmax: int) -> np.ndarray:
    """构建梅尔滤波器组"""
    key = (sr, n_fft, n_mels, fmax)
    if key in _MEL_CACHE:
        return _MEL_CACHE[key]

    freqs = np.linspace(0, sr / 2, n_fft // 2 + 1)
    mel_min = _hz_to_mel(np.array([20.0]))[0]
    mel_max = _hz_to_mel(np.array([float(fmax)]))[0]
    mel_points = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_points = _mel_to_hz(mel_points)

    fb = np.zeros((n_mels, len(freqs)), dtype=np.float32)
    for m in range(1, n_mels + 1):
        left, center, right = hz_points[m - 1], hz_points[m], hz_points[m + 1]
        li = np.where((freqs >= left) & (freqs <= center))[0]
        ri = np.where((freqs >= center) & (freqs <= right))[0]
        if li.size > 0:
            fb[m - 1, li] = (freqs[li] - left) / max(center - left, 1e-8)
        if ri.size > 0:
            fb[m - 1, ri] = (right - freqs[ri]) / max(right - center, 1e-8)

    _MEL_CACHE[key] = fb
    return fb


def audio_to_feature(audio: np.ndarray) -> np.ndarray:
    """
    音频转Log-Mel特征（与train_model.py完全一致）
    """
    # 预加重处理
    audio = pre_emphasis(audio, PRE_EMPHASIS)
    
    _, _, stft = signal.stft(
        audio,
        fs=SAMPLE_RATE,
        nperseg=WIN_LENGTH,
        noverlap=max(0, WIN_LENGTH - HOP_LENGTH),
        nfft=N_FFT,
        padded=False,
        boundary=None,
        window="hann",
    )
    power = (np.abs(stft) ** 2).astype(np.float32)
    mel_fb = _mel_filter_bank(SAMPLE_RATE, N_FFT, N_MELS, fmax=min(7600, SAMPLE_RATE // 2))
    mel = mel_fb @ power
    log_mel = np.log10(np.maximum(mel, 1e-10)).astype(np.float32)

    if log_mel.shape[1] > TARGET_FRAMES:
        log_mel = log_mel[:, :TARGET_FRAMES]
    elif log_mel.shape[1] < TARGET_FRAMES:
        pad = TARGET_FRAMES - log_mel.shape[1]
        log_mel = np.pad(log_mel, ((0, 0), (0, pad)), mode="constant")

    mean = float(np.mean(log_mel))
    std = float(np.std(log_mel)) + 1e-6
    log_mel = (log_mel - mean) / std
    return log_mel[..., np.newaxis].astype(np.float32)


def extract_feature_from_audio(audio: np.ndarray) -> np.ndarray:
    """
    从音频提取特征（与train_model.py完全一致的预处理流程）
    """
    audio = trim_and_fit(audio, target_seconds=MODEL_SECONDS, top_db=30)
    audio = normalize_audio(audio)
    return audio_to_feature(audio)

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


def check_segment_validity(segments: List[Tuple[int, int]]) -> float:
    """
    简化的分段合理性检查
    """
    if not segments or len(segments) != 4:
        return 0.0
    
    scores = []
    
    # 检查时长合理性（0.6-1.8秒）
    for s, e in segments:
        duration = (e - s) / SAMPLE_RATE
        if 0.6 <= duration <= 1.8:
            scores.append(1.0)
        elif 0.4 <= duration <= 2.2:
            scores.append(0.5)
        else:
            scores.append(0.0)
    
    # 检查间隔合理性
    for i in range(len(segments) - 1):
        gap = (segments[i + 1][0] - segments[i][1]) / SAMPLE_RATE
        if -0.1 <= gap <= 0.6:
            scores.append(1.0)
        else:
            scores.append(0.5)
    
    return float(np.mean(scores)) if scores else 0.0


def _normalize_segments(segments: List[Tuple[int, int]], expected_count: int, total_len: int) -> List[Tuple[int, int]]:
    """
    标准化分段数量（优化版：基于能量的智能合并/分裂）
    """
    if not segments:
        segments = [(0, total_len)]
    
    # 最小段长度（0.5秒）
    min_seg_len = int(0.5 * SAMPLE_RATE)

    # 合并：优先合并短段和间隔小的段
    while len(segments) > expected_count:
        # 计算每个段的长度和间隔
        lengths = [e - s for s, e in segments]
        gaps = [segments[i + 1][0] - segments[i][1] for i in range(len(segments) - 1)]
        
        # 找到最短的段或最小的间隔
        if min(lengths) < min_seg_len and lengths.index(min(lengths)) < len(segments) - 1:
            # 合并最短的段到下一个段
            i = lengths.index(min(lengths))
        else:
            # 合并间隔最小的两个段
            i = int(np.argmin(gaps))
        
        segments = segments[:i] + [(segments[i][0], segments[i + 1][1])] + segments[i + 2:]

    # 分裂：在段的中点分裂
    while len(segments) < expected_count:
        lengths = [b - a for a, b in segments]
        i = int(np.argmax(lengths))
        a, b = segments[i]
        m = (a + b) // 2
        segments = segments[:i] + [(a, m), (m, b)] + segments[i + 1:]

    # 如果仍然不对，使用均分
    if len(segments) != expected_count:
        step = total_len // expected_count
        segments = [(i * step, (i + 1) * step if i < expected_count - 1 else total_len) for i in range(expected_count)]
    
    return segments


def ensure_no_overlap(segments: List[Tuple[int, int]]) -> List[Tuple[int, int]]:
    """
    确保分段之间没有重叠
    如果有重叠，在重叠区域的中点切分
    """
    if len(segments) <= 1:
        return segments
    
    # 按起始位置排序
    segments = sorted(segments, key=lambda x: x[0])
    
    fixed = []
    for i, (s, e) in enumerate(segments):
        if i == 0:
            # 第一个段，直接添加
            fixed.append((s, e))
        else:
            prev_s, prev_e = fixed[-1]
            
            if s < prev_e:
                # 有重叠，在中点切分
                mid = (prev_e + s) // 2
                # 修正前一个段的结束位置
                fixed[-1] = (prev_s, mid)
                # 修正当前段的起始位置
                fixed.append((mid, e))
            else:
                # 无重叠，直接添加
                fixed.append((s, e))
    
    return fixed


def split_by_vad(audio: np.ndarray, expected_count: int = 4) -> List[Tuple[int, int]]:
    """
    使用VAD（语音活动检测）进行分段
    确保每个数字在0.6-1.2秒之间
    """
    working = normalize_audio(audio.astype(np.float32) - np.mean(audio))
    frame_len, hop = WIN_LENGTH, HOP_LENGTH
    rms = _frame_rms(working, frame_len, hop)
    total_len = len(working)
    
    # VAD阈值：使用自适应阈值
    max_rms = float(np.max(rms))
    median_rms = float(np.median(rms))
    
    # 尝试多个阈值，选择最合适的
    best_segments = None
    best_score = -1
    
    for ratio in [0.15, 0.12, 0.10, 0.08]:
        threshold = max(max_rms * ratio, median_rms * 0.5)
        mask = rms > threshold
        
        # 形态学处理：膨胀和腐蚀，连接断开的语音段
        mask = ndimage.binary_dilation(mask, iterations=3)
        mask = ndimage.binary_erosion(mask, iterations=2)
        
        # 提取语音段
        segments = []
        for fs, fe in _mask_to_segments(mask):
            s = max(0, fs * hop)
            e = min(total_len, fe * hop + frame_len)
            duration = (e - s) / SAMPLE_RATE
            
            # 过滤太短的段（<0.3秒）
            if duration >= 0.3:
                segments.append((s, e))
        
        # 合并距离很近的段（间隔<0.15秒）
        if len(segments) > 1:
            merged = [segments[0]]
            for s, e in segments[1:]:
                prev_s, prev_e = merged[-1]
                gap = (s - prev_e) / SAMPLE_RATE
                if gap < 0.15:
                    # 合并
                    merged[-1] = (prev_s, e)
                else:
                    merged.append((s, e))
            segments = merged
        
        # 评分：段数接近4，且时长合理
        if len(segments) == 0:
            continue
        
        durations = [(e - s) / SAMPLE_RATE for s, e in segments]
        avg_duration = np.mean(durations)
        
        # 评分标准：
        # 1. 段数接近4
        # 2. 平均时长在0.6-1.2秒之间
        count_score = 1.0 - abs(len(segments) - expected_count) / expected_count
        duration_score = 1.0 if 0.6 <= avg_duration <= 1.2 else max(0.0, 1.0 - abs(avg_duration - 0.9) / 0.9)
        score = 0.6 * count_score + 0.4 * duration_score
        
        if score > best_score:
            best_score = score
            best_segments = segments
    
    if not best_segments:
        # 兜底：均分
        step = total_len // expected_count
        best_segments = [(i * step, (i + 1) * step if i < expected_count - 1 else total_len) 
                        for i in range(expected_count)]
    
    # 标准化到expected_count个段
    best_segments = _normalize_segments(best_segments, expected_count, total_len)
    
    # 确保每个段在0.6-1.2秒之间
    fixed_segments = []
    for s, e in best_segments:
        duration = (e - s) / SAMPLE_RATE
        
        if duration < 0.6:
            # 太短，扩展到0.6秒
            target_len = int(0.6 * SAMPLE_RATE)
            expand = (target_len - (e - s)) // 2
            s = max(0, s - expand)
            e = min(total_len, s + target_len)
        elif duration > 1.2:
            # 太长，从中心截取1.0秒
            target_len = int(1.0 * SAMPLE_RATE)
            center = (s + e) // 2
            s = center - target_len // 2
            e = s + target_len
        
        fixed_segments.append((s, e))
    
    # 确保无重叠
    fixed_segments = ensure_no_overlap(fixed_segments)
    
    return fixed_segments


def split_candidates(audio: np.ndarray, expected_count: int = 4) -> List[Dict[str, object]]:
    """
    简化的分段算法：只使用VAD
    """
    segments = split_by_vad(audio, expected_count)
    
    # 只返回一个候选结果
    return [{"method": "vad", "segments": segments}]


def clips_from_segments(audio: np.ndarray, segments: List[Tuple[int, int]]) -> Tuple[List[np.ndarray], List[Tuple[float, float]]]:
    """
    从分段提取音频片段（简化版：直接使用VAD分段结果）
    """
    clips = []
    ranges = []
    target_len = int(SAMPLE_RATE * MODEL_SECONDS)
    
    for s, e in segments:
        clip = audio[s:e]
        
        # 归一化
        clip = normalize_audio(clip)
        
        # 调整到目标长度（1.5秒）
        if len(clip) < target_len:
            # Padding到1.5秒
            pad_total = target_len - len(clip)
            left = pad_total // 2
            right = pad_total - left
            clip = np.pad(clip, (left, right), mode='constant')
        elif len(clip) > target_len:
            # 从中心截取1.5秒
            center = len(clip) // 2
            half = target_len // 2
            clip = clip[center - half:center - half + target_len]
        
        clips.append(clip.astype(np.float32))
        ranges.append((s / SAMPLE_RATE, e / SAMPLE_RATE))
    
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


def check_confidence_consistency(confs: np.ndarray) -> float:
    """
    B1优化：检查置信度一致性
    如果4个数字的置信度差异很大，说明分段可能有问题
    
    Args:
        confs: 4个数字的置信度数组
    
    Returns:
        一致性分数 (0-1)，越高越好
    """
    if len(confs) == 0:
        return 0.0
    
    mean_conf = float(np.mean(confs))
    std_conf = float(np.std(confs))
    min_conf = float(np.min(confs))
    max_conf = float(np.max(confs))
    
    # 一致性评分标准：
    # 1. 标准差小（置信度接近）
    # 2. 最小值不能太低
    # 3. 最大最小差距不能太大
    
    # 标准差惩罚：std越大，分数越低
    std_score = max(0.0, 1.0 - std_conf * 3.0)  # std=0.33时为0
    
    # 最小值要求：min_conf至少0.5
    min_score = min(1.0, max(0.0, (min_conf - 0.3) / 0.4))  # 0.3->0, 0.7->1
    
    # 极差惩罚：max-min不应超过0.4
    range_score = max(0.0, 1.0 - (max_conf - min_conf) / 0.4)
    
    # 综合一致性分数
    consistency = 0.4 * std_score + 0.4 * min_score + 0.2 * range_score
    
    return float(consistency)


def predict_four(interpreter, input_detail, output_detail, duration: float, show_candidates: bool) -> None:
    print(f"开始录音: {duration:.2f}s（连续四位）")
    audio = sd.rec(int(duration * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=1, dtype="float32")
    sd.wait()
    audio = audio.squeeze(axis=1).astype(np.float32)
    audio = normalize_audio(audio - np.mean(audio))

    candidates = split_candidates(audio, expected_count=4)
    if not candidates:
        raise RuntimeError("分段失败，请重新录音。")

    # 只有一个VAD候选结果
    cand = candidates[0]
    clips, ranges = clips_from_segments(audio, cand["segments"])
    feats = np.asarray([audio_to_feature(c) for c in clips], dtype=np.float32)
    probs = infer_tflite(interpreter, input_detail, output_detail, feats)
    labels = np.argmax(probs, axis=1)
    confs = np.max(probs, axis=1)
    text = "".join(DIGITS[int(v)] for v in labels)

    # B1优化：计算置信度一致性
    mean_c = float(np.mean(confs))
    min_c = float(np.min(confs))
    consistency = check_confidence_consistency(confs)
    
    print(f"预测4位: {text} | 平均置信度={mean_c:.4f} | 最低置信度={min_c:.4f} | 一致性={consistency:.4f}")
    
    for i in range(4):
        a, b = ranges[i]
        duration_i = b - a
        print(f"  {i+1}: {DIGITS[int(labels[i])]} | 置信度={confs[i]:.4f} | {a:.2f}s~{b:.2f}s (时长{duration_i:.2f}s)")
    
    # 警告：如果一致性太低
    if consistency < 0.5:
        print(f"\n⚠️  警告: 置信度一致性较低 ({consistency:.2f})，可能识别不准确，建议重新录音。")
    elif min_c < 0.6:
        print(f"\n⚠️  警告: 最低置信度较低 ({min_c:.2f})，某个数字可能识别错误。")


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
            duration = ask_float("连续四位录音时长(秒)", 3.6)  # 默认3.6秒
            predict_four(interpreter, input_detail, output_detail, duration=duration, show_candidates=False)
        else:
            print("输入无效，请输入 1 / 4 / q。")


if __name__ == "__main__":
    main()
