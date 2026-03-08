import os
from pathlib import Path
from typing import Dict, List, Tuple
import numpy as np
from scipy import signal
from scipy.io import wavfile
os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
import tensorflow as tf


# ========= 基础配置 =========
SAMPLE_RATE = 16000
MODEL_SECONDS = 1.5  # 增加到1.5秒，充分利用完整录音
N_MELS = 40
N_FFT = 1024  # 增大FFT窗口：512→1024，提高频率分辨率
HOP_LENGTH = 160
WIN_LENGTH = 400
PRE_EMPHASIS = 0.97  # 预加重系数
TARGET_FRAMES = 150  # 对应1.5秒的帧数
DIGITS = [str(i) for i in range(10)]
NUM_CLASSES = 10

# 训练配置：使用全量数据训练，增加训练充分度
EPOCHS = 80
BATCH_SIZE = 16
LEARNING_RATE = 1e-3
RANDOM_SEED = 44

# 数据增强配置（增强强度）
AUGMENT_ENABLED = True  # 是否启用数据增强
TIME_STRETCH_RANGE = (0.75, 1.25)  # 时间拉伸范围（扩大范围：0.85-1.15 → 0.75-1.25）
PITCH_SHIFT_RANGE = (-5, 5)  # 音高变换范围（扩大范围：-3~3 → -5~5）
NOISE_FACTOR = 0.012  # 噪声强度（增加：0.008 → 0.012）
VOLUME_SCALE_RANGE = (0.3, 1.2)  # 音量缩放范围（扩大范围：0.5-1.0 → 0.3-1.2）
SPEC_AUG_FREQ_MASK = 12  # 频谱遮蔽宽度（增加：10 → 12）
SPEC_AUG_TIME_MASK = 25  # 时间遮蔽宽度（增加：20 → 25）
AUGMENT_PER_SAMPLE = 5  # 每个样本生成的增强版本数（增加：3 → 5）

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
MODEL_PATH = BASE_DIR / "digit_model_int8.tflite"

_MEL_CACHE: Dict[Tuple[int, int, int, int], np.ndarray] = {}


def load_audio_mono(wav_path: Path, target_sr: int = SAMPLE_RATE) -> np.ndarray:
    sr, audio = wavfile.read(str(wav_path))
    if audio.ndim > 1:
        audio = np.mean(audio, axis=1)

    if audio.dtype == np.int16:
        audio = audio.astype(np.float32) / 32768.0
    elif audio.dtype == np.int32:
        audio = audio.astype(np.float32) / 2147483648.0
    elif audio.dtype == np.uint8:
        audio = (audio.astype(np.float32) - 128.0) / 128.0
    else:
        audio = audio.astype(np.float32)

    if sr != target_sr:
        gcd = np.gcd(sr, target_sr)
        audio = signal.resample_poly(audio, up=target_sr // gcd, down=sr // gcd).astype(np.float32)
    return audio.astype(np.float32)


def pre_emphasis(audio: np.ndarray, coef: float = PRE_EMPHASIS) -> np.ndarray:
    """
    预加重：通过高通滤波器增强高频成分
    y[n] = x[n] - coef * x[n-1]
    """
    if len(audio) < 2:
        return audio
    return np.append(audio[0], audio[1:] - coef * audio[:-1]).astype(np.float32)


def normalize_audio(audio: np.ndarray) -> np.ndarray:
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    if peak < 1e-8:
        return audio.astype(np.float32)
    return (audio / peak).astype(np.float32)

def trim_and_fit(audio: np.ndarray, target_seconds: float = MODEL_SECONDS, top_db: int = 30) -> np.ndarray:
    target_len = int(SAMPLE_RATE * target_seconds)
    if audio.size == 0:
        return np.zeros(target_len, dtype=np.float32)

    abs_audio = np.abs(audio)
    peak = float(np.max(abs_audio))
    if peak < 1e-8:
        trimmed = audio
    else:
        threshold = peak * (10 ** (-top_db / 20.0))
        active_idx = np.where(abs_audio >= threshold)[0]
        if active_idx.size == 0:
            trimmed = audio
        else:
            pad = int(0.03 * SAMPLE_RATE)
            start = max(0, int(active_idx[0]) - pad)
            end = min(len(audio), int(active_idx[-1]) + pad + 1)
            trimmed = audio[start:end]

    if len(trimmed) > target_len:
        start = (len(trimmed) - target_len) // 2
        trimmed = trimmed[start : start + target_len]
    elif len(trimmed) < target_len:
        pad = target_len - len(trimmed)
        left = pad // 2
        right = pad - left
        trimmed = np.pad(trimmed, (left, right))
    return trimmed.astype(np.float32)

def _hz_to_mel(freq: np.ndarray) -> np.ndarray:
    return 2595.0 * np.log10(1.0 + freq / 700.0)

def _mel_to_hz(mel: np.ndarray) -> np.ndarray:
    return 700.0 * (10 ** (mel / 2595.0) - 1.0)


def _mel_filter_bank(sr: int, n_fft: int, n_mels: int, fmax: int) -> np.ndarray:
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
    从音频提取特征（训练和测试必须使用相同的预处理流程）
    """
    audio = trim_and_fit(audio, target_seconds=MODEL_SECONDS, top_db=30)
    audio = normalize_audio(audio)
    return audio_to_feature(audio)


# ========= 数据增强函数 =========
def time_stretch_audio(audio: np.ndarray, rate: float) -> np.ndarray:
    """
    时间拉伸：改变语速但不改变音高
    rate > 1.0: 加速, rate < 1.0: 减速
    """
    if abs(rate - 1.0) < 1e-6:
        return audio
    
    indices = np.arange(0, len(audio), rate)
    indices = indices[indices < len(audio)].astype(np.int32)
    stretched = audio[indices]
    return stretched.astype(np.float32)


def pitch_shift_audio(audio: np.ndarray, n_steps: int, sr: int = SAMPLE_RATE) -> np.ndarray:
    """
    音高变换：改变音高但保持时长
    n_steps: 半音数，正数升高，负数降低
    """
    if n_steps == 0:
        return audio
    
    # 使用重采样实现音高变换
    rate = 2 ** (n_steps / 12.0)
    
    # 先改变采样率（改变音高和时长）
    stretched_len = int(len(audio) / rate)
    if stretched_len < 1:
        return audio
    
    indices = np.linspace(0, len(audio) - 1, stretched_len)
    shifted = np.interp(indices, np.arange(len(audio)), audio)
    
    # 再拉伸回原时长（保持音高变化，恢复时长）
    indices = np.linspace(0, len(shifted) - 1, len(audio))
    result = np.interp(indices, np.arange(len(shifted)), shifted)
    
    return result.astype(np.float32)


def add_noise(audio: np.ndarray, noise_factor: float = NOISE_FACTOR) -> np.ndarray:
    """
    添加白噪声
    """
    noise = np.random.randn(len(audio)).astype(np.float32) * noise_factor
    noisy = audio + noise
    return noisy.astype(np.float32)


def volume_scale_audio(audio: np.ndarray, scale: float) -> np.ndarray:
    """
    音量缩放：模拟不同音量的朗读（特别是小声音场景）
    scale: 缩放因子，0.5表示音量减半，1.0表示保持不变
    """
    return (audio * scale).astype(np.float32)


def formant_shift_audio(audio: np.ndarray, shift_factor: float = 1.0) -> np.ndarray:
    """
    共振峰偏移：模拟不同体型/年龄的说话者
    shift_factor > 1.0: 模拟较小体型（儿童、女性）
    shift_factor < 1.0: 模拟较大体型（成年男性）
    """
    if abs(shift_factor - 1.0) < 1e-6 or len(audio) < 2:
        return audio
    
    # 使用简单的频域变换模拟共振峰偏移
    # 通过改变频谱的频率轴来实现
    fft_audio = np.fft.rfft(audio)
    freqs = np.fft.rfftfreq(len(audio), 1.0 / SAMPLE_RATE)
    
    # 创建新的频率索引
    new_freqs = freqs * shift_factor
    
    # 插值到新的频率
    shifted_fft = np.interp(freqs, new_freqs, np.abs(fft_audio)) * np.exp(1j * np.angle(fft_audio))
    shifted_audio = np.fft.irfft(shifted_fft, n=len(audio))
    
    return shifted_audio.astype(np.float32)


def add_reverb(audio: np.ndarray, room_scale: float = 0.3) -> np.ndarray:
    """
    添加混响：模拟不同房间大小的声学环境
    room_scale: 房间大小系数 (0.1-0.5)，越大混响越强
    """
    if room_scale < 0.01 or len(audio) < 2:
        return audio
    
    # 创建简单的指数衰减混响
    reverb_len = int(SAMPLE_RATE * room_scale)  # 混响长度
    reverb_ir = np.exp(-np.arange(reverb_len) / (reverb_len * 0.3))  # 指数衰减
    reverb_ir = reverb_ir / np.sum(reverb_ir)  # 归一化
    
    # 卷积实现混响
    reverb_audio = signal.convolve(audio, reverb_ir, mode='same')
    
    # 混合原始信号和混响信号
    mixed = 0.7 * audio + 0.3 * reverb_audio
    
    return mixed.astype(np.float32)


def add_colored_noise(audio: np.ndarray, noise_type: str = 'white', noise_factor: float = NOISE_FACTOR) -> np.ndarray:
    """
    添加有色噪声：模拟不同类型的环境噪声
    noise_type: 'white' (白噪声), 'pink' (粉红噪声), 'brown' (布朗噪声)
    """
    noise = np.random.randn(len(audio)).astype(np.float32)
    
    if noise_type == 'pink':
        # 粉红噪声：1/f 频谱（低频更强）
        fft_noise = np.fft.rfft(noise)
        freqs = np.fft.rfftfreq(len(noise))
        freqs[0] = 1e-10  # 避免除零
        pink_filter = 1.0 / np.sqrt(freqs)
        pink_filter[0] = 0
        fft_noise *= pink_filter
        noise = np.fft.irfft(fft_noise, n=len(audio))
    elif noise_type == 'brown':
        # 布朗噪声：1/f^2 频谱（低频更强）
        fft_noise = np.fft.rfft(noise)
        freqs = np.fft.rfftfreq(len(noise))
        freqs[0] = 1e-10  # 避免除零
        brown_filter = 1.0 / freqs
        brown_filter[0] = 0
        fft_noise *= brown_filter
        noise = np.fft.irfft(fft_noise, n=len(audio))
    # else: 'white' - 保持原始白噪声
    
    # 归一化噪声
    noise = noise / (np.std(noise) + 1e-8)
    noisy = audio + noise * noise_factor
    
    return noisy.astype(np.float32)


def spec_augment(log_mel: np.ndarray, freq_mask_width: int = SPEC_AUG_FREQ_MASK, 
                 time_mask_width: int = SPEC_AUG_TIME_MASK) -> np.ndarray:
    """
    SpecAugment: 频谱增强
    在频率和时间维度上随机遮蔽
    """
    augmented = log_mel.copy()
    n_mels, n_frames = augmented.shape[:2]
    
    # 频率遮蔽
    if freq_mask_width > 0 and n_mels > freq_mask_width:
        f0 = np.random.randint(0, n_mels - freq_mask_width)
        augmented[f0:f0 + freq_mask_width, :] = 0
    
    # 时间遮蔽
    if time_mask_width > 0 and n_frames > time_mask_width:
        t0 = np.random.randint(0, n_frames - time_mask_width)
        augmented[:, t0:t0 + time_mask_width] = 0
    
    return augmented


def augment_audio(audio: np.ndarray, augment_level: int = 0) -> np.ndarray:
    """
    分层数据增强策略：为5个增强版本应用不同强度的增强
    augment_level: 0-4，表示增强版本编号
    
    策略：
    - Level 0: 基础增强（音量+噪声）
    - Level 1: 时间变换（时间拉伸+音量）
    - Level 2: 音高变换（音高偏移+噪声）
    - Level 3: 说话者变换（共振峰偏移+混响）
    - Level 4: 综合增强（多种增强组合）
    """
    if augment_level == 0:
        # Level 0: 基础增强 - 音量+白噪声
        scale = np.random.uniform(*VOLUME_SCALE_RANGE)
        audio = volume_scale_audio(audio, scale)
        if np.random.rand() < 0.8:
            audio = add_colored_noise(audio, noise_type='white')
    
    elif augment_level == 1:
        # Level 1: 时间变换 - 时间拉伸+音量
        rate = np.random.uniform(*TIME_STRETCH_RANGE)
        audio = time_stretch_audio(audio, rate)
        scale = np.random.uniform(*VOLUME_SCALE_RANGE)
        audio = volume_scale_audio(audio, scale)
    
    elif augment_level == 2:
        # Level 2: 音高变换 - 音高偏移+粉红噪声
        n_steps = np.random.randint(PITCH_SHIFT_RANGE[0], PITCH_SHIFT_RANGE[1] + 1)
        audio = pitch_shift_audio(audio, n_steps)
        if np.random.rand() < 0.7:
            audio = add_colored_noise(audio, noise_type='pink')
    
    elif augment_level == 3:
        # Level 3: 说话者变换 - 共振峰偏移+混响
        shift_factor = np.random.uniform(0.85, 1.15)  # 模拟不同体型
        audio = formant_shift_audio(audio, shift_factor)
        if np.random.rand() < 0.6:
            room_scale = np.random.uniform(0.1, 0.4)
            audio = add_reverb(audio, room_scale)
    
    else:  # augment_level == 4
        # Level 4: 综合增强 - 多种增强组合
        # 音量
        if np.random.rand() < 0.7:
            scale = np.random.uniform(*VOLUME_SCALE_RANGE)
            audio = volume_scale_audio(audio, scale)
        
        # 时间或音高（二选一）
        if np.random.rand() < 0.5:
            rate = np.random.uniform(*TIME_STRETCH_RANGE)
            audio = time_stretch_audio(audio, rate)
        else:
            n_steps = np.random.randint(PITCH_SHIFT_RANGE[0], PITCH_SHIFT_RANGE[1] + 1)
            audio = pitch_shift_audio(audio, n_steps)
        
        # 噪声（随机类型）
        if np.random.rand() < 0.7:
            noise_type = np.random.choice(['white', 'pink', 'brown'])
            audio = add_colored_noise(audio, noise_type=noise_type)
        
        # 共振峰或混响（小概率）
        if np.random.rand() < 0.3:
            shift_factor = np.random.uniform(0.9, 1.1)
            audio = formant_shift_audio(audio, shift_factor)
        if np.random.rand() < 0.3:
            room_scale = np.random.uniform(0.1, 0.3)
            audio = add_reverb(audio, room_scale)
    
    return audio


def extract_feature_with_augment(audio: np.ndarray, apply_augment: bool = True, augment_level: int = 0) -> np.ndarray:
    """
    提取特征并可选地应用数据增强
    
    Args:
        audio: 输入音频
        apply_augment: 是否应用增强
        augment_level: 增强级别 (0-4)，用于分层增强策略
    """
    # 音频级增强（在特征提取前）
    if apply_augment and AUGMENT_ENABLED:
        audio = augment_audio(audio, augment_level)
    
    # 标准预处理和特征提取
    audio = trim_and_fit(audio, target_seconds=MODEL_SECONDS, top_db=30)
    audio = normalize_audio(audio)
    log_mel = audio_to_feature(audio)
    
    # 频谱级增强（在特征提取后）- 提高应用概率
    if apply_augment and AUGMENT_ENABLED and np.random.rand() < 0.7:
        log_mel_2d = log_mel.squeeze(-1)  # 移除通道维度
        log_mel_2d = spec_augment(log_mel_2d)
        log_mel = log_mel_2d[..., np.newaxis]  # 恢复通道维度
    
    return log_mel


def collect_dataset(test_split: float = 0.20) -> Tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """
    收集数据集并划分训练集/测试集
    
    Args:
        test_split: 测试集比例（默认20%）
    
    Returns:
        x_train, y_train, x_test, y_test
    """
    # 按数字分别收集原始样本（保存音频和路径）
    digit_samples: Dict[int, List[Tuple[np.ndarray, Path]]] = {i: [] for i in range(NUM_CLASSES)}
    digit_counts: Dict[int, int] = {i: 0 for i in range(NUM_CLASSES)}
    
    total_files = 0
    print("\n" + "="*50)
    print("原始数据集统计:")
    print("="*50)
    
    for idx, digit in enumerate(DIGITS):
        folder = DATA_DIR / digit
        if not folder.exists():
            print(f"数字 {digit}: 0 个样本 (文件夹不存在)")
            continue
        
        wav_files = sorted(folder.glob("*.wav"))
        count = len(wav_files)
        digit_counts[idx] = count
        total_files += count
        
        print(f"数字 {digit}: {count} 个样本")
        
        for wav_path in wav_files:
            audio = load_audio_mono(wav_path, target_sr=SAMPLE_RATE)
            # 只提取特征，不增强
            feat = extract_feature_with_augment(audio, apply_augment=False)
            digit_samples[idx].append((feat, wav_path))
    
    print("-"*50)
    print(f"总计: {total_files} 个原始样本")
    print("="*50)
    
    if total_files == 0:
        raise RuntimeError("未找到训练数据，请先录音。")
    
    # 划分训练集和测试集（按数字分层划分）
    train_features: List[np.ndarray] = []
    train_labels: List[int] = []
    test_features: List[np.ndarray] = []
    test_labels: List[int] = []
    
    train_counts: Dict[int, int] = {i: 0 for i in range(NUM_CLASSES)}
    test_counts: Dict[int, int] = {i: 0 for i in range(NUM_CLASSES)}
    
    print("\n训练集/测试集划分:")
    print("="*50)
    
    for digit_idx, samples in digit_samples.items():
        if not samples:
            continue
        
        n_samples = len(samples)
        n_test = max(1, int(n_samples * test_split))  # 至少1个测试样本
        
        # 随机打乱
        np.random.shuffle(samples)
        
        # 划分
        test_samples = samples[:n_test]
        train_samples = samples[n_test:]
        
        # 测试集：不增强
        for feat, _ in test_samples:
            test_features.append(feat)
            test_labels.append(digit_idx)
            test_counts[digit_idx] += 1
        
        # 训练集：原始样本 + 增强样本
        for feat, wav_path in train_samples:
            # 原始样本
            train_features.append(feat)
            train_labels.append(digit_idx)
            train_counts[digit_idx] += 1
            
            # 数据增强：每个训练样本生成5个不同级别的增强版本
            if AUGMENT_ENABLED:
                for aug_level in range(AUGMENT_PER_SAMPLE):
                    # 重新加载音频并应用对应级别的增强
                    audio = load_audio_mono(wav_path, target_sr=SAMPLE_RATE)
                    feat_aug = extract_feature_with_augment(audio, apply_augment=True, augment_level=aug_level)
                    train_features.append(feat_aug)
                    train_labels.append(digit_idx)
                    train_counts[digit_idx] += 1
        
        n_train_original = len(train_samples)
        n_train_total = train_counts[digit_idx]
        print(f"数字 {DIGITS[digit_idx]}: 训练={n_train_total} (原始{n_train_original}+增强{n_train_total-n_train_original}), 测试={test_counts[digit_idx]}")
    
    x_train = np.asarray(train_features, dtype=np.float32)
    y_train = np.asarray(train_labels, dtype=np.int64)
    x_test = np.asarray(test_features, dtype=np.float32)
    y_test = np.asarray(test_labels, dtype=np.int64)
    
    print("-"*50)
    print(f"训练集总计: {len(y_train)} 样本")
    print(f"测试集总计: {len(y_test)} 样本")
    print(f"数据扩增倍数: {len(y_train) / (total_files - len(y_test)):.1f}x")
    print("="*50)
    
    return x_train, y_train, x_test, y_test


def build_light_model(input_shape: Tuple[int, int, int], lr: float) -> tf.keras.Model:
    """
    轻量CNN模型：适合RA6M5部署，增加正则化防止过拟合
    """
    inputs = tf.keras.Input(shape=input_shape)
    
    # 第一层卷积块
    x = tf.keras.layers.Conv2D(12, (3, 3), padding="same", activation="relu")(inputs)
    x = tf.keras.layers.BatchNormalization()(x)  # 添加BN层
    x = tf.keras.layers.MaxPooling2D((2, 2))(x)
    x = tf.keras.layers.Dropout(0.15)(x)  # 添加Dropout
    
    # 第二层卷积块
    x = tf.keras.layers.Conv2D(16, (3, 3), padding="same", activation="relu")(x)
    x = tf.keras.layers.BatchNormalization()(x)  # 添加BN层
    x = tf.keras.layers.MaxPooling2D((2, 2))(x)
    x = tf.keras.layers.Dropout(0.15)(x)  # 添加Dropout
    
    # 第三层卷积块
    x = tf.keras.layers.Conv2D(24, (3, 3), padding="same", activation="relu")(x)
    x = tf.keras.layers.BatchNormalization()(x)  # 添加BN层
    
    # 全连接层
    x = tf.keras.layers.Flatten()(x)
    x = tf.keras.layers.Dense(40, activation="relu")(x)
    x = tf.keras.layers.Dropout(0.25)(x)  # 增加Dropout比例
    outputs = tf.keras.layers.Dense(NUM_CLASSES, activation="softmax")(x)

    model = tf.keras.Model(inputs=inputs, outputs=outputs, name="digit_light_cnn")
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=lr),
        loss="sparse_categorical_crossentropy",
        metrics=["accuracy"],
    )
    return model


def export_int8_tflite(model: tf.keras.Model, x_ref: np.ndarray, out_path: Path) -> None:
    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]

    sample_count = min(len(x_ref), 120)
    idx = np.random.choice(len(x_ref), sample_count, replace=False)
    reps = x_ref[idx]

    def rep_gen():
        for item in reps:
            yield [item[np.newaxis, ...].astype(np.float32)]

    converter.representative_dataset = rep_gen
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    out_path.write_bytes(converter.convert())


def calc_accuracy(model: tf.keras.Model, x: np.ndarray, y: np.ndarray) -> float:
    prob = model.predict(x, verbose=0)
    pred = np.argmax(prob, axis=1)
    return float(np.mean(pred == y))


def train_on_all_data(
    x_train: np.ndarray,
    y_train: np.ndarray,
    x_test: np.ndarray,
    y_test: np.ndarray,
    seed: int,
) -> Tuple[tf.keras.Model, dict, float, float]:
    """
    训练模型并在测试集上评估
    
    Returns:
        model, history, train_acc, test_acc
    """
    np.random.seed(seed)
    tf.keras.utils.set_random_seed(seed)

    model = build_light_model(input_shape=x_train.shape[1:], lr=LEARNING_RATE)
    
    # 优化的回调函数
    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="loss",
            patience=8,  # 增加耐心值
            min_delta=1e-4,
            restore_best_weights=False,
            verbose=0,
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="loss",
            factor=0.3,  # 更激进的学习率衰减
            patience=3,
            min_lr=1e-6,
            verbose=0,
        ),
    ]

    print(f"开始训练: epochs={EPOCHS}, batch_size={BATCH_SIZE}, lr={LEARNING_RATE}")
    history = model.fit(
        x_train,
        y_train,
        epochs=EPOCHS,
        batch_size=BATCH_SIZE,
        verbose=1,  # 显示训练进度
        callbacks=callbacks,
        shuffle=True,
    )

    # 在训练集和测试集上评估
    train_acc = calc_accuracy(model, x_train, y_train)
    test_acc = calc_accuracy(model, x_test, y_test)
    
    return model, history.history, train_acc, test_acc


def main() -> None:
    tf.get_logger().setLevel("ERROR")
    
    # 收集数据并划分训练集/测试集（80:20）
    x_train, y_train, x_test, y_test = collect_dataset(test_split=0.20)
    
    # 训练模型
    model, _, train_acc, test_acc = train_on_all_data(
        x_train, y_train, x_test, y_test, seed=RANDOM_SEED
    )
    
    # 导出模型（使用训练集数据作为量化参考）
    export_int8_tflite(model, x_train, MODEL_PATH)
    model_size_kb = MODEL_PATH.stat().st_size / 1024.0

    print(f"\n{'='*50}")
    print(f"训练完成!")
    print(f"{'='*50}")
    print(f"训练集准确率: {train_acc:.4f}")
    print(f"测试集准确率: {test_acc:.4f}")
    print(f"模型大小: {model_size_kb:.1f} KB")
    print(f"模型路径: {MODEL_PATH}")
    
    # 过拟合检测
    if train_acc - test_acc > 0.10:
        print(f"\n⚠️  警告: 训练集和测试集准确率差距较大 ({train_acc - test_acc:.2%})")
        print(f"   可能存在过拟合，建议:")
        print(f"   1. 增加训练数据量")
        print(f"   2. 增加数据增强强度")
        print(f"   3. 增加Dropout比例")
    elif test_acc >= 0.95:
        print(f"\n✓ 测试集准确率优秀 (≥95%)，模型泛化能力良好！")
    elif test_acc >= 0.90:
        print(f"\n✓ 测试集准确率良好 (≥90%)，可以使用。")
    else:
        print(f"\n⚠️  测试集准确率偏低 (<90%)，建议:")
        print(f"   1. 检查录音质量")
        print(f"   2. 增加训练数据量")
        print(f"   3. 增加训练轮数")


if __name__ == "__main__":
    main()
