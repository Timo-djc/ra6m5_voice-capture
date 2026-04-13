import math
import re
from array import array
from dataclasses import dataclass
from typing import List

try:
    from .audio_utils import AudioFormatError, load_wav_bytes, pcm16_to_wav_bytes
    from .config import CONFIG
    from .speaker_model_adapter import (
        BaseSpeakerModelAdapter,
        SpeakerBackendStatus,
        SpeakerEmbedding,
        build_speaker_adapter_with_status,
        cosine_similarity,
        describe_speaker_backend,
        l2_normalize,
    )
    from .speaker_storage import (
        IdentifyEventRecord,
        RuntimeResetSummary,
        SpeakerRecord,
        SpeakerStorage,
    )
except ImportError:  # pragma: no cover
    from audio_utils import AudioFormatError, load_wav_bytes, pcm16_to_wav_bytes
    from config import CONFIG
    from speaker_model_adapter import (
        BaseSpeakerModelAdapter,
        SpeakerBackendStatus,
        SpeakerEmbedding,
        build_speaker_adapter_with_status,
        cosine_similarity,
        describe_speaker_backend,
        l2_normalize,
    )
    from speaker_storage import (
        IdentifyEventRecord,
        RuntimeResetSummary,
        SpeakerRecord,
        SpeakerStorage,
    )

SPEAKER_ID_RE = re.compile(r"^[a-zA-Z0-9_-]{1,32}$")


class SpeakerServiceError(RuntimeError):
    pass


@dataclass
class IdentifyResult:
    status: str
    speaker_id: str
    score: float
    margin: float


@dataclass
class EnrollResult:
    speaker_id: str
    accepted: int
    required: int
    score: float
    active: bool


class SpeakerService:
    def __init__(
        self,
        storage: SpeakerStorage | None = None,
        adapter: BaseSpeakerModelAdapter | None = None,
    ) -> None:
        self.storage = storage or SpeakerStorage()
        if adapter is None:
            self.adapter, self.backend_status = build_speaker_adapter_with_status()
        else:
            self.adapter = adapter
            self.backend_status = describe_speaker_backend(adapter)
        self.backend_status: SpeakerBackendStatus

    def validate_speaker_id(self, speaker_id: str) -> str:
        if not SPEAKER_ID_RE.fullmatch(speaker_id or ""):
            raise SpeakerServiceError("speaker_id must match [a-zA-Z0-9_-]{1,32}")
        return speaker_id

    def _ensure_audio_length(self, samples: int) -> None:
        if samples < CONFIG.min_samples:
            raise SpeakerServiceError("audio too short")
        if samples > CONFIG.max_samples:
            raise SpeakerServiceError("audio too long")

    def _ensure_audio_activity(self, pcm16: bytes) -> None:
        pcm = array("h")
        pcm.frombytes(pcm16)
        if not pcm:
            raise SpeakerServiceError("audio empty")
        rms = math.sqrt(sum((sample / 32768.0) ** 2 for sample in pcm) / len(pcm))
        if rms < CONFIG.min_rms:
            raise SpeakerServiceError("audio too quiet")

    def _embedding_from_wav(self, wav_bytes: bytes) -> tuple[bytes, SpeakerEmbedding]:
        try:
            normalized = load_wav_bytes(wav_bytes, target_sample_rate=CONFIG.target_sample_rate)
        except AudioFormatError as exc:
            raise SpeakerServiceError(str(exc)) from exc
        self._ensure_audio_length(normalized.samples)
        self._ensure_audio_activity(normalized.pcm16)
        embedding = self.adapter.extract_embedding(normalized.pcm16, normalized.sample_rate)
        normalized_wav = pcm16_to_wav_bytes(normalized.pcm16, normalized.sample_rate)
        return normalized_wav, embedding

    @staticmethod
    def _centroid(vectors: List[List[float]]) -> List[float]:
        if not vectors:
            raise SpeakerServiceError("empty speaker profile")
        dim = len(vectors[0])
        accum = [0.0 for _ in range(dim)]
        for vec in vectors:
            if len(vec) != dim:
                raise SpeakerServiceError("inconsistent embedding dimension")
            for idx, value in enumerate(vec):
                accum[idx] += value
        return l2_normalize([value / len(vectors) for value in accum])

    @staticmethod
    def _median(values: List[float]) -> float:
        if not values:
            return 0.0
        ordered = sorted(float(value) for value in values)
        midpoint = len(ordered) // 2
        if len(ordered) % 2 == 1:
            return ordered[midpoint]
        return float((ordered[midpoint - 1] + ordered[midpoint]) / 2.0)

    @staticmethod
    def _majority_vote_count(sample_count: int) -> int:
        return max(1, (sample_count // 2) + 1)

    @staticmethod
    def _score_margin(top_score: float, second_score: float | None) -> float:
        if second_score is None:
            return top_score
        return top_score - second_score

    def _is_fallback_backend(self) -> bool:
        return self.backend_status.effective_backend == "fallback"

    def create_speaker(self, speaker_id: str, display_name: str | None = None) -> SpeakerRecord:
        speaker_id = self.validate_speaker_id(speaker_id)
        if self.storage.speaker_exists(speaker_id):
            raise SpeakerServiceError("speaker already exists")
        self.storage.create_speaker(speaker_id, display_name)
        return self.get_speaker(speaker_id)

    def get_speaker(self, speaker_id: str) -> SpeakerRecord:
        speaker_id = self.validate_speaker_id(speaker_id)
        speaker = self.storage.get_speaker(speaker_id)
        if speaker is None:
            raise SpeakerServiceError("speaker not found")
        return speaker

    def list_speakers(self) -> List[SpeakerRecord]:
        return self.storage.list_speakers()

    def enroll_wav(
        self,
        speaker_id: str,
        wav_bytes: bytes,
        utter_idx: int | None = None,
        source: str = "pc-direct",
        display_name: str | None = None,
    ) -> EnrollResult:
        speaker_id = self.validate_speaker_id(speaker_id)
        self.storage.ensure_speaker(speaker_id, display_name)
        if display_name is not None:
            self.storage.update_display_name(speaker_id, display_name)
        normalized_wav, embedding = self._embedding_from_wav(wav_bytes)
        self.storage.add_embedding_sample(
            speaker_id=speaker_id,
            utter_idx=utter_idx,
            source=source,
            wav_bytes=normalized_wav,
            embedding=embedding.values,
            backend=embedding.backend,
        )
        vectors = self.storage.list_embeddings(speaker_id)
        centroid = self._centroid(vectors)
        sample_count = len(vectors)
        self.storage.upsert_profile(speaker_id, centroid, sample_count)
        return EnrollResult(
            speaker_id=speaker_id,
            accepted=sample_count,
            required=CONFIG.required_samples,
            score=1.0,
            active=sample_count >= CONFIG.required_samples,
        )

    def identify_wav(self, wav_bytes: bytes) -> IdentifyResult:
        _normalized_wav, embedding = self._embedding_from_wav(wav_bytes)
        profiles = self.storage.list_active_profiles()
        if not profiles:
            return IdentifyResult(status="UNKNOWN", speaker_id="unknown", score=0.0, margin=0.0)

        if self._is_fallback_backend():
            scored = []
            threshold = CONFIG.fallback_identify_threshold
            for speaker_id, centroid, _sample_count in profiles:
                centroid_score = cosine_similarity(embedding.values, centroid)
                sample_vectors = self.storage.list_embeddings(speaker_id)
                sample_scores = [cosine_similarity(embedding.values, sample) for sample in sample_vectors]
                profile_score = self._median(sample_scores) if sample_scores else centroid_score
                vote_count = sum(score >= threshold for score in sample_scores)
                required_votes = self._majority_vote_count(len(sample_scores))
                scored.append((speaker_id, profile_score, centroid_score, vote_count, required_votes))

            scored.sort(key=lambda item: (item[1], item[2], item[3]), reverse=True)
            top_id, top_score, _top_centroid, top_votes, required_votes = scored[0]
            second_score = scored[1][1] if len(scored) > 1 else None
            margin = self._score_margin(top_score, second_score)
            if (
                top_score >= CONFIG.fallback_identify_threshold
                and top_votes >= required_votes
                and margin >= CONFIG.identify_margin
            ):
                return IdentifyResult(status="KNOWN", speaker_id=top_id, score=top_score, margin=margin)
            return IdentifyResult(status="UNKNOWN", speaker_id="unknown", score=top_score, margin=margin)

        scored = [
            (speaker_id, cosine_similarity(embedding.values, centroid))
            for speaker_id, centroid, _sample_count in profiles
        ]
        scored.sort(key=lambda item: item[1], reverse=True)
        top_id, top_score = scored[0]
        second_score = scored[1][1] if len(scored) > 1 else None
        margin = self._score_margin(top_score, second_score)
        if top_score >= CONFIG.identify_threshold and margin >= CONFIG.identify_margin:
            return IdentifyResult(status="KNOWN", speaker_id=top_id, score=top_score, margin=margin)
        return IdentifyResult(status="UNKNOWN", speaker_id="unknown", score=top_score, margin=margin)

    def record_identify_event(
        self,
        *,
        flow: str,
        expected_speaker_id: str,
        status: str,
        speaker_id: str,
        score: float,
        margin: float,
        passed: bool,
        notification_attempted: bool,
        notification_sent: bool,
        notification_error: str,
    ) -> IdentifyEventRecord:
        return self.storage.add_identify_event(
            flow=flow,
            expected_speaker_id=expected_speaker_id,
            status=status,
            speaker_id=speaker_id,
            score=score,
            margin=margin,
            passed=passed,
            notification_attempted=notification_attempted,
            notification_sent=notification_sent,
            notification_error=notification_error,
        )

    def list_identify_events(self, limit: int = 20) -> List[IdentifyEventRecord]:
        return self.storage.list_identify_events(limit)

    def delete_speaker(self, speaker_id: str) -> bool:
        speaker_id = self.validate_speaker_id(speaker_id)
        return self.storage.delete_speaker(speaker_id)

    def reset_runtime_data(self) -> RuntimeResetSummary:
        return self.storage.reset_runtime_data()
