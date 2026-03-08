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
MODEL_SECONDS = 1.2
N_MELS = 40
N_FFT = 512
HOP_LENGTH = 160
WIN_LENGTH = 400
TARGET_FRAMES = 120
DIGITS = [str(i) for i in range(10)]
NUM_CLASSES = 10

# 训练配置：使用全量数据训练，同时控制训练时间
EPOCHS = 40
BATCH_SIZE = 16
LEARNING_RATE = 1e-3
RANDOM_SEED = 44

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


def normalize_audio(audio: np.ndarray) -> np.ndarray:
    peak = float(np.max(np.abs(audio))) if audio.size else 0.0
    if peak < 1e-8:
        return audio.astype(np.float32)
    return (audio / peak).astype(np.float32)

def trim_and_fit(audio: np.ndarray, target_seconds: float = MODEL_SECONDS, top_db: int = 40) -> np.ndarray:
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
    audio = trim_and_fit(audio, target_seconds=MODEL_SECONDS, top_db=40)
    audio = normalize_audio(audio)
    return audio_to_feature(audio)


def collect_dataset() -> Tuple[np.ndarray, np.ndarray]:
    features: List[np.ndarray] = []
    labels: List[int] = []

    for idx, digit in enumerate(DIGITS):
        folder = DATA_DIR / digit
        if not folder.exists():
            continue
        for wav_path in sorted(folder.glob("*.wav")):
            audio = load_audio_mono(wav_path, target_sr=SAMPLE_RATE)
            feat = extract_feature_from_audio(audio)
            features.append(feat)
            labels.append(idx)

    if not features:
        raise RuntimeError("未找到训练数据，请先录音。")
    return np.asarray(features, dtype=np.float32), np.asarray(labels, dtype=np.int64)


def build_light_model(input_shape: Tuple[int, int, int], lr: float) -> tf.keras.Model:
    # 轻量CNN：参数不大，精度比极简模型更稳
    inputs = tf.keras.Input(shape=input_shape)
    x = tf.keras.layers.Conv2D(12, (3, 3), padding="same", activation="relu")(inputs)
    x = tf.keras.layers.MaxPooling2D((2, 2))(x)
    x = tf.keras.layers.Conv2D(16, (3, 3), padding="same", activation="relu")(x)
    x = tf.keras.layers.MaxPooling2D((2, 2))(x)
    x = tf.keras.layers.Conv2D(24, (3, 3), padding="same", activation="relu")(x)
    x = tf.keras.layers.Flatten()(x)
    x = tf.keras.layers.Dense(40, activation="relu")(x)
    x = tf.keras.layers.Dropout(0.10)(x)
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
    x_all: np.ndarray,
    y_all: np.ndarray,
    seed: int,
) -> Tuple[tf.keras.Model, dict, float]:
    np.random.seed(seed)
    tf.keras.utils.set_random_seed(seed)

    model = build_light_model(input_shape=x_all.shape[1:], lr=LEARNING_RATE)
    callbacks = [
        tf.keras.callbacks.EarlyStopping(
            monitor="loss",
            patience=5,
            min_delta=1e-4,
            restore_best_weights=False,
            verbose=0,
        ),
        tf.keras.callbacks.ReduceLROnPlateau(
            monitor="loss",
            factor=0.5,
            patience=2,
            min_lr=1e-5,
            verbose=0,
        ),
    ]

    history = model.fit(
        x_all,
        y_all,
        epochs=EPOCHS,
        batch_size=BATCH_SIZE,
        verbose=0,
        callbacks=callbacks,
        shuffle=True,
    )

    full_acc = calc_accuracy(model, x_all, y_all)
    return model, history.history, full_acc


def main() -> None:
    tf.get_logger().setLevel("ERROR")
    x_all, y_all = collect_dataset()

    model, history_dict, full_acc = train_on_all_data(
        x_all,
        y_all,
        seed=RANDOM_SEED,
    )
    export_int8_tflite(model, x_all, MODEL_PATH)
    model_size_kb = MODEL_PATH.stat().st_size / 1024.0

    print("训练完成")
    print(f"固定随机种子: {RANDOM_SEED}")
    print(f"总样本数: {len(y_all)}")
    print(f"训练轮数: {len(history_dict.get('loss', []))}")
    print(f"全量训练集重测准确率: {full_acc:.4f}")
    print(f"tflite模型: {MODEL_PATH}")
    print(f"模型大小: {model_size_kb:.2f} KB")

    if full_acc < 0.97:
        print("提示: 全量训练集重测低于97%")


if __name__ == "__main__":
    main()
