import hashlib
import math
import os
import os.path as osp
import tempfile
from abc import ABC, abstractmethod
from array import array
from dataclasses import dataclass
from typing import Any, List, Sequence

try:
    from .audio_utils import pcm16_to_wav_bytes
    from .config import CONFIG
except ImportError:  # pragma: no cover
    from audio_utils import pcm16_to_wav_bytes
    from config import CONFIG


def l2_normalize(vector: Sequence[float]) -> List[float]:
    norm = math.sqrt(sum(v * v for v in vector))
    if norm <= 1e-8:
        return [0.0 for _ in vector]
    return [float(v / norm) for v in vector]


def cosine_similarity(lhs: Sequence[float], rhs: Sequence[float]) -> float:
    if len(lhs) != len(rhs):
        raise ValueError("embedding dimension mismatch")
    return float(sum(a * b for a, b in zip(lhs, rhs)))


@dataclass
class SpeakerEmbedding:
    values: List[float]
    backend: str


@dataclass(frozen=True)
class SpeakerBackendStatus:
    configured_backend: str
    effective_backend: str
    model_id: str
    model_revision: str
    adapter_class: str
    message: str = ""
    warning: bool = False


class BaseSpeakerModelAdapter(ABC):
    @abstractmethod
    def extract_embedding(self, pcm16: bytes, sample_rate: int) -> SpeakerEmbedding:
        raise NotImplementedError


class FallbackSpeakerModelAdapter(BaseSpeakerModelAdapter):
    def _frame_feature(self, samples: Sequence[int]) -> List[float]:
        if not samples:
            return [0.0, 0.0]
        energy = 0.0
        zcr = 0.0
        prev = samples[0]
        for sample in samples:
            energy += abs(sample)
            if (sample >= 0 > prev) or (sample < 0 <= prev):
                zcr += 1.0
            prev = sample
        return [energy / (len(samples) * 32768.0), zcr / max(len(samples) - 1, 1)]

    def extract_embedding(self, pcm16: bytes, sample_rate: int) -> SpeakerEmbedding:
        _ = sample_rate
        pcm = array("h")
        pcm.frombytes(pcm16)
        frame_count = 32
        frame_size = max(1, len(pcm) // frame_count)
        feats: List[float] = []
        for idx in range(frame_count):
            start = idx * frame_size
            end = len(pcm) if idx == frame_count - 1 else min(len(pcm), start + frame_size)
            feats.extend(self._frame_feature(pcm[start:end]))

        digest = hashlib.sha256(pcm16[: min(len(pcm16), 8192)]).digest()
        for byte_val in digest[:8]:
            feats.append((byte_val / 255.0) - 0.5)

        return SpeakerEmbedding(values=l2_normalize(feats), backend="fallback")


def _patch_modelscope_config_reader() -> None:
    if os.name != "nt":
        return
    from modelscope.utils.config import Config  # type: ignore

    if getattr(Config, "_speaker_web_safe_file_patch", False):
        return

    original = Config._file2dict

    def safe_file2dict(filename: str) -> tuple[dict[str, Any], str]:
        filename = osp.abspath(osp.expanduser(filename))
        ext = osp.splitext(filename)[1].lower()
        if ext in {".json", ".yaml", ".yml"}:
            from modelscope.fileio import load  # type: ignore

            return load(filename), filename
        return original(filename)

    Config._file2dict = staticmethod(safe_file2dict)
    Config._speaker_web_safe_file_patch = True


class ModelScopeSpeakerModelAdapter(BaseSpeakerModelAdapter):
    def __init__(self) -> None:
        _patch_modelscope_config_reader()
        from modelscope.pipelines import pipeline  # type: ignore
        from modelscope.utils.constant import Tasks  # type: ignore

        revision = CONFIG.model_revision or None
        kwargs = {
            "task": Tasks.speaker_verification,
            "model": CONFIG.model_id,
            "device": os.getenv("SPEAKER_MODEL_DEVICE", "cpu"),
        }
        if revision:
            kwargs["model_revision"] = revision
        self._pipeline = pipeline(**kwargs)

    @staticmethod
    def _flatten_embedding(value: Any) -> List[float]:
        if value is None:
            raise ValueError("empty embedding")
        if hasattr(value, "tolist"):
            value = value.tolist()
        if isinstance(value, (list, tuple)):
            if value and isinstance(value[0], (list, tuple)):
                value = value[0]
            return [float(v) for v in value]
        raise ValueError(f"unsupported embedding type: {type(value)!r}")

    def _extract_from_result(self, result: Any) -> List[float]:
        if isinstance(result, dict):
            for key in ("embs", "output_emb", "embedding", "embeddings", "spk_embedding", "xvector"):
                if key in result:
                    return self._flatten_embedding(result[key])
        raise ValueError(f"pipeline result did not contain an embedding: {result!r}")

    def extract_embedding(self, pcm16: bytes, sample_rate: int) -> SpeakerEmbedding:
        wav_bytes = pcm16_to_wav_bytes(pcm16, sample_rate=sample_rate)
        fd, tmp_path = tempfile.mkstemp(prefix="speaker_", suffix=".wav")
        os.close(fd)
        try:
            with open(tmp_path, "wb") as handle:
                handle.write(wav_bytes)

            try:
                result = self._pipeline([tmp_path], output_emb=True)
            except TypeError:
                result = self._pipeline(tmp_path, output_emb=True)
            embedding = self._extract_from_result(result)
            return SpeakerEmbedding(values=l2_normalize(embedding), backend="modelscope")
        finally:
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def _configured_backend() -> str:
    backend = (CONFIG.speaker_backend or "").strip().lower()
    return backend or "modelscope"


def detect_speaker_backend(adapter: BaseSpeakerModelAdapter) -> str:
    if isinstance(adapter, ModelScopeSpeakerModelAdapter):
        return "modelscope"
    if isinstance(adapter, FallbackSpeakerModelAdapter):
        return "fallback"
    backend_name = getattr(adapter, "backend_name", "")
    if isinstance(backend_name, str) and backend_name.strip():
        return backend_name.strip().lower()
    return type(adapter).__name__


def describe_speaker_backend(
    adapter: BaseSpeakerModelAdapter,
    *,
    message: str = "",
    warning: bool = False,
) -> SpeakerBackendStatus:
    return SpeakerBackendStatus(
        configured_backend=_configured_backend(),
        effective_backend=detect_speaker_backend(adapter),
        model_id=CONFIG.model_id,
        model_revision=CONFIG.model_revision,
        adapter_class=type(adapter).__name__,
        message=message,
        warning=warning,
    )


def format_speaker_backend_status_lines(status: SpeakerBackendStatus) -> List[str]:
    summary = [
        f"[speaker] configured={status.configured_backend}",
        f"effective={status.effective_backend}",
        f"adapter={status.adapter_class}",
    ]
    if status.model_id:
        summary.append(f"model={status.model_id}")
    if status.model_revision:
        summary.append(f"revision={status.model_revision}")

    lines = [" ".join(summary)]
    if status.message:
        level = "warning" if status.warning else "note"
        lines.append(f"[speaker] {level}: {status.message}")
    return lines


def build_speaker_adapter_with_status() -> tuple[BaseSpeakerModelAdapter, SpeakerBackendStatus]:
    configured_backend = _configured_backend()
    if configured_backend == "fallback":
        adapter = FallbackSpeakerModelAdapter()
        return adapter, describe_speaker_backend(
            adapter,
            message="fallback backend forced by SPEAKER_BACKEND=fallback.",
        )

    try:
        adapter = ModelScopeSpeakerModelAdapter()
        return adapter, describe_speaker_backend(adapter)
    except Exception as exc:
        adapter = FallbackSpeakerModelAdapter()
        return adapter, describe_speaker_backend(
            adapter,
            message=(
                "failed to initialize the ModelScope speaker backend "
                f"({exc.__class__.__name__}: {exc}); using fallback backend instead."
            ),
            warning=True,
        )


def build_speaker_adapter() -> BaseSpeakerModelAdapter:
    adapter, _status = build_speaker_adapter_with_status()
    return adapter
