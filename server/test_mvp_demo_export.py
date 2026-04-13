import shutil
import unittest
from pathlib import Path
from uuid import uuid4

from server.audio_utils import pcm16_to_wav_bytes
from server.mvp_demo_export import MvpDemoExporter


def synth_tone(freq_hz: float, samples: int = 24000, sample_rate: int = 16000) -> bytes:
    import math
    import struct

    pcm = bytearray()
    for idx in range(samples):
        value = int(14000.0 * math.sin(2.0 * math.pi * freq_hz * idx / sample_rate))
        pcm += struct.pack("<h", value)
    return bytes(pcm)


class MvpDemoExporterTests(unittest.TestCase):
    def setUp(self) -> None:
        root = Path("tmp") / f"mvp_export_test_{uuid4().hex}"
        root.mkdir(parents=True, exist_ok=True)
        self.tmpdir = root
        self.exporter = MvpDemoExporter(
            data_root=root / "data",
            output_c=root / "test_audio_data.c",
            output_h=root / "test_audio_data.h",
        )
        self.wav = pcm16_to_wav_bytes(synth_tone(300.0))

    def tearDown(self) -> None:
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_register_export_generates_c_files(self) -> None:
        summary = self.exporter.save_register_clips("speaker1", [self.wav, self.wav, self.wav])
        self.assertEqual(summary["speaker_id"], "speaker1")
        self.assertEqual(summary["embedded_clip_count"], 4)
        self.assertTrue((self.tmpdir / "test_audio_data.c").exists())
        self.assertTrue((self.tmpdir / "test_audio_data.h").exists())
        self.assertIn("expected_speaker_id", (self.tmpdir / "test_audio_data.h").read_text(encoding="ascii"))

    def test_identify_export_overwrites_identify_clip(self) -> None:
        self.exporter.save_register_clips("speaker1", [self.wav, self.wav, self.wav])
        summary = self.exporter.save_identify_clip("speaker1", self.wav)
        self.assertTrue(summary["identify_present"])
        self.assertTrue((self.tmpdir / "data" / "speaker1" / "04_identify.wav").exists())
        self.assertIn('"speaker1", "speaker1"', (self.tmpdir / "test_audio_data.c").read_text(encoding="ascii"))

    def test_reset_dataset_generates_empty_stub(self) -> None:
        self.exporter.save_register_clips("speaker1", [self.wav, self.wav, self.wav])
        summary = self.exporter.reset_dataset()
        self.assertEqual(summary["embedded_clip_count"], 0)
        source = (self.tmpdir / "test_audio_data.c").read_text(encoding="ascii")
        self.assertIn("return 0;", source)
        self.assertTrue((self.tmpdir / "data").exists())
        self.assertEqual(list((self.tmpdir / "data").iterdir()), [])


if __name__ == "__main__":
    unittest.main()
