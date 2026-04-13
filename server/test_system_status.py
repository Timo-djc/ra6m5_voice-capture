import unittest
from pathlib import Path
from uuid import uuid4

from server.main import _NoopSpeakerTcpServer, create_app
from server.speaker_model_adapter import BaseSpeakerModelAdapter, SpeakerEmbedding
from server.speaker_service import SpeakerService
from server.speaker_storage import SpeakerStorage
from server.test_http_app import MiniAsgiClient, MockAsrAdapter, synth_tone
from server.audio_utils import load_wav_bytes, pcm16_to_wav_bytes


class DeterministicSpeakerAdapter(BaseSpeakerModelAdapter):
    def __init__(self) -> None:
        self._vectors: dict[str, list[float]] = {}

    def register_wav(self, wav_bytes: bytes, vector: list[float]) -> None:
        import hashlib

        normalized = load_wav_bytes(wav_bytes, target_sample_rate=16000)
        digest = hashlib.sha256(normalized.pcm16).hexdigest()
        self._vectors[digest] = vector

    def extract_embedding(self, pcm16: bytes, sample_rate: int) -> SpeakerEmbedding:
        import hashlib

        _ = sample_rate
        digest = hashlib.sha256(pcm16).hexdigest()
        return SpeakerEmbedding(values=self._vectors[digest], backend="test")


class SystemStatusTests(unittest.TestCase):
    def setUp(self) -> None:
        root = Path("tmp") / f"system_status_test_{uuid4().hex}"
        root.mkdir(parents=True, exist_ok=True)
        self.tmpdir = root
        storage = SpeakerStorage(db_path=root / "test.db", data_root=root / "data")
        adapter = DeterministicSpeakerAdapter()
        sample = pcm16_to_wav_bytes(synth_tone(300.0))
        adapter.register_wav(sample, [1.0, 0.0])
        service = SpeakerService(storage=storage, adapter=adapter)
        self.app = create_app(
            asr_adapter=MockAsrAdapter(),
            speaker_service_instance=service,
            speaker_tcp_server_instance=_NoopSpeakerTcpServer(),
        )
        self.client = MiniAsgiClient(self.app)

    def tearDown(self) -> None:
        import shutil

        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_system_status_endpoint_reports_backend_details(self) -> None:
        response = self.client.get("/api/system/status")
        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertEqual(payload["effective_backend"], "DeterministicSpeakerAdapter")
        self.assertEqual(payload["adapter_class"], "DeterministicSpeakerAdapter")
        self.assertEqual(payload["identify_strategy"], "centroid")
        self.assertIn("identify_threshold", payload)

    def test_root_page_contains_backend_status_slots(self) -> None:
        response = self.client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn('id="backendConfiguredText"', response.text)
        self.assertIn('id="backendEffectiveText"', response.text)
        self.assertIn('id="backendStrategyText"', response.text)
        self.assertIn('id="backendMessageText"', response.text)


if __name__ == "__main__":
    unittest.main()
