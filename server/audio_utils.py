import audioop
import io
import wave
from dataclasses import dataclass


@dataclass
class NormalizedAudio:
    pcm16: bytes
    sample_rate: int
    channels: int
    samples: int


class AudioFormatError(ValueError):
    pass


def _ensure_pcm16(sample_width: int, frames: bytes) -> bytes:
    if sample_width == 2:
        return frames
    if sample_width == 1:
        return audioop.bias(audioop.lin2lin(frames, 1, 2), 2, -128)
    if sample_width == 4:
        return audioop.lin2lin(frames, 4, 2)
    raise AudioFormatError(f"unsupported sample width: {sample_width}")


def load_wav_bytes(data: bytes, target_sample_rate: int = 16000) -> NormalizedAudio:
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            channels = wav.getnchannels()
            sample_width = wav.getsampwidth()
            sample_rate = wav.getframerate()
            frames = wav.readframes(wav.getnframes())
    except wave.Error as exc:
        raise AudioFormatError(f"invalid wav: {exc}") from exc

    pcm16 = _ensure_pcm16(sample_width, frames)
    if channels == 2:
        pcm16 = audioop.tomono(pcm16, 2, 0.5, 0.5)
        channels = 1
    elif channels != 1:
        raise AudioFormatError(f"unsupported channels: {channels}")

    if sample_rate != target_sample_rate:
        pcm16, _ = audioop.ratecv(pcm16, 2, 1, sample_rate, target_sample_rate, None)
        sample_rate = target_sample_rate

    samples = len(pcm16) // 2
    return NormalizedAudio(pcm16=pcm16, sample_rate=sample_rate, channels=1, samples=samples)


def pcm16_to_wav_bytes(pcm16: bytes, sample_rate: int = 16000) -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(sample_rate)
        wav.writeframes(pcm16)
    return buffer.getvalue()
