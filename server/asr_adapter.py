from abc import ABC, abstractmethod
from dataclasses import dataclass

from config import CONFIG


@dataclass
class ASRResult:
    code: int
    text: str
    confidence: float


class BaseASRAdapter(ABC):
    @abstractmethod
    def recognize_wav(self, wav_bytes: bytes, sample_rate: int) -> ASRResult:
        raise NotImplementedError


class MockASRAdapter(BaseASRAdapter):
    def recognize_wav(self, wav_bytes: bytes, sample_rate: int) -> ASRResult:
        _ = wav_bytes
        _ = sample_rate
        return ASRResult(code=0, text=CONFIG.mock_text, confidence=CONFIG.mock_confidence)


class RealASRAdapter(BaseASRAdapter):
    def recognize_wav(self, wav_bytes: bytes, sample_rate: int) -> ASRResult:
        # TODO: Replace this with a real backend, e.g.:
        # 1) Whisper: transcribe from bytes -> text
        # 2) FunASR: call funasr pipeline and map fields
        # 3) Internal service: forward wav bytes via HTTP/gRPC
        # Keep return schema compatible with ASRResult.
        _ = wav_bytes
        _ = sample_rate
        return ASRResult(code=1001, text="", confidence=0.0)


def build_adapter() -> BaseASRAdapter:
    if CONFIG.adapter.lower() == "real":
        return RealASRAdapter()
    return MockASRAdapter()
