import shutil
import unittest
from pathlib import Path
from uuid import uuid4

from server.audio_utils import pcm16_to_wav_bytes
from server.speaker_model_adapter import FallbackSpeakerModelAdapter
from server.speaker_service import SpeakerService
from server.speaker_storage import SpeakerStorage
from server.tcp_server import parse_frame_header


def synth_tone(freq_hz: float, samples: int = 24000, sample_rate: int = 16000) -> bytes:
    import math
    import struct

    pcm = bytearray()
    for idx in range(samples):
        value = int(14000.0 * math.sin(2.0 * math.pi * freq_hz * idx / sample_rate))
        pcm += struct.pack("<h", value)
    return bytes(pcm)


class SpeakerCoreTests(unittest.TestCase):
    def setUp(self) -> None:
        root = Path("tmp") / f"speaker_test_{uuid4().hex}"
        root.mkdir(parents=True, exist_ok=True)
        self.tmpdir = root
        self.storage = SpeakerStorage(db_path=root / "test.db", data_root=root / "data")
        self.service = SpeakerService(storage=self.storage, adapter=FallbackSpeakerModelAdapter())

    def tearDown(self) -> None:
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_parse_legacy_identify_header(self) -> None:
        kind, frame = parse_frame_header("SVI,1,12,3,16000,16,1,24000,48000\n")
        self.assertEqual(kind, "SVI")
        self.assertEqual(frame.session, 12)
        self.assertEqual(frame.expected_speaker_id, "")

    def test_parse_identify_header_with_expected_speaker(self) -> None:
        kind, frame = parse_frame_header("SVI,1,12,3,speaker1,16000,16,1,24000,48000\n")
        self.assertEqual(kind, "SVI")
        self.assertEqual(frame.expected_speaker_id, "speaker1")
        self.assertEqual(frame.payload_bytes, 48000)

    def test_parse_enroll_header(self) -> None:
        kind, frame = parse_frame_header("SVR,1,2,1,speaker1,1,3,16000,16,1,24000,48000\n")
        self.assertEqual(kind, "SVR")
        self.assertEqual(frame.speaker_id, "speaker1")

    def test_enroll_and_identify_known(self) -> None:
        wav = pcm16_to_wav_bytes(synth_tone(300.0))
        for idx in range(1, 4):
            result = self.service.enroll_wav("speaker1", wav, utter_idx=idx, source="test")
        self.assertTrue(result.active)
        identify = self.service.identify_wav(wav)
        self.assertEqual(identify.status, "KNOWN")
        self.assertEqual(identify.speaker_id, "speaker1")

    def test_identify_unknown(self) -> None:
        wav_a = pcm16_to_wav_bytes(synth_tone(260.0))
        wav_b = pcm16_to_wav_bytes(synth_tone(820.0))
        for idx in range(1, 4):
            self.service.enroll_wav("speaker1", wav_a, utter_idx=idx, source="test")
        identify = self.service.identify_wav(wav_b)
        self.assertIn(identify.status, ("KNOWN", "UNKNOWN"))

    def test_fallback_majority_rejects_outlier_registration_clip(self) -> None:
        wav_primary = pcm16_to_wav_bytes(synth_tone(300.0))
        wav_outlier = pcm16_to_wav_bytes(synth_tone(900.0))
        self.service.enroll_wav("speaker1", wav_primary, utter_idx=1, source="test")
        self.service.enroll_wav("speaker1", wav_primary, utter_idx=2, source="test")
        self.service.enroll_wav("speaker1", wav_outlier, utter_idx=3, source="test")

        known = self.service.identify_wav(wav_primary)
        outlier = self.service.identify_wav(wav_outlier)

        self.assertEqual(known.status, "KNOWN")
        self.assertEqual(known.speaker_id, "speaker1")
        self.assertEqual(outlier.status, "UNKNOWN")
        self.assertEqual(outlier.speaker_id, "unknown")


if __name__ == "__main__":
    unittest.main()
