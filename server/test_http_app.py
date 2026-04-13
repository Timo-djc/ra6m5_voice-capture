import asyncio
import hashlib
import json
import unittest
from unittest.mock import patch
from pathlib import Path
from uuid import uuid4

from server.alert_notifier import AlertNotificationResult, BaseIdentifyFailureNotifier, IdentifyFailureContext
from server.audio_utils import load_wav_bytes, pcm16_to_wav_bytes
from server.main import _NoopSpeakerTcpServer, create_app
from server.mvp_demo_export import MvpDemoExporter
from server.speaker_model_adapter import BaseSpeakerModelAdapter, SpeakerEmbedding
from server.speaker_service import SpeakerService
from server.speaker_storage import SpeakerStorage


def synth_tone(freq_hz: float, samples: int = 24000, sample_rate: int = 16000) -> bytes:
    import math
    import struct

    pcm = bytearray()
    for idx in range(samples):
        value = int(14000.0 * math.sin(2.0 * math.pi * freq_hz * idx / sample_rate))
        pcm += struct.pack("<h", value)
    return bytes(pcm)


class MockAsrAdapter:
    def recognize_wav(self, wav_bytes: bytes, sample_rate: int):
        _ = wav_bytes
        _ = sample_rate
        return type("AsrResult", (), {"code": 0, "text": "ok", "confidence": 0.99})()


class DeterministicSpeakerAdapter(BaseSpeakerModelAdapter):
    def __init__(self) -> None:
        self._vectors: dict[str, list[float]] = {}

    def register_wav(self, wav_bytes: bytes, vector: list[float]) -> None:
        normalized = load_wav_bytes(wav_bytes, target_sample_rate=16000)
        digest = hashlib.sha256(normalized.pcm16).hexdigest()
        self._vectors[digest] = vector

    def extract_embedding(self, pcm16: bytes, sample_rate: int) -> SpeakerEmbedding:
        _ = sample_rate
        digest = hashlib.sha256(pcm16).hexdigest()
        return SpeakerEmbedding(values=self._vectors[digest], backend="test")


class FakeNotifier(BaseIdentifyFailureNotifier):
    def __init__(self, *, result: AlertNotificationResult | None = None) -> None:
        self.result = result or AlertNotificationResult(attempted=True, sent=True, error="")
        self.calls: list[dict[str, object]] = []

    async def notify_failure(
        self,
        *,
        expected_speaker_id: str,
        actual_status: str,
        actual_speaker_id: str,
        score: float,
        margin: float,
        context: IdentifyFailureContext | None = None,
    ) -> AlertNotificationResult:
        self.calls.append(
            {
                "expected_speaker_id": expected_speaker_id,
                "actual_status": actual_status,
                "actual_speaker_id": actual_speaker_id,
                "score": score,
                "margin": margin,
                "context": context,
            }
        )
        return self.result


class AsgiResponse:
    def __init__(self, status_code: int, headers: list[tuple[bytes, bytes]], body: bytes) -> None:
        self.status_code = status_code
        self.headers = headers
        self.body = body

    @property
    def text(self) -> str:
        return self.body.decode("utf-8", errors="replace")

    def json(self):
        return json.loads(self.body.decode("utf-8"))


class MiniAsgiClient:
    def __init__(self, app) -> None:
        self.app = app

    def request(
        self,
        method: str,
        path: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes = b"",
    ) -> AsgiResponse:
        return asyncio.run(self._request(method, path, headers=headers, body=body))

    async def _request(
        self,
        method: str,
        path: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes = b"",
    ) -> AsgiResponse:
        sent_request = False
        response_status = 500
        response_headers: list[tuple[bytes, bytes]] = []
        response_body = bytearray()

        path_only, _, query = path.partition("?")
        scope = {
            "type": "http",
            "asgi": {"version": "3.0"},
            "http_version": "1.1",
            "method": method.upper(),
            "scheme": "http",
            "path": path_only,
            "raw_path": path_only.encode("ascii"),
            "query_string": query.encode("ascii"),
            "headers": [
                (name.lower().encode("ascii"), value.encode("utf-8"))
                for name, value in (headers or {}).items()
            ],
            "client": ("127.0.0.1", 12345),
            "server": ("testserver", 80),
        }

        async def receive():
            nonlocal sent_request
            if sent_request:
                return {"type": "http.disconnect"}
            sent_request = True
            return {"type": "http.request", "body": body, "more_body": False}

        async def send(message):
            nonlocal response_status, response_headers
            if message["type"] == "http.response.start":
                response_status = int(message["status"])
                response_headers = list(message.get("headers", []))
            elif message["type"] == "http.response.body":
                response_body.extend(message.get("body", b""))

        await self.app(scope, receive, send)
        return AsgiResponse(response_status, response_headers, bytes(response_body))

    def get(self, path: str, *, headers: dict[str, str] | None = None) -> AsgiResponse:
        return self.request("GET", path, headers=headers)

    def post(
        self,
        path: str,
        *,
        headers: dict[str, str] | None = None,
        body: bytes = b"",
    ) -> AsgiResponse:
        return self.request("POST", path, headers=headers, body=body)

    def delete(self, path: str, *, headers: dict[str, str] | None = None) -> AsgiResponse:
        return self.request("DELETE", path, headers=headers)


def make_json_request(payload: dict) -> tuple[dict[str, str], bytes]:
    body = json.dumps(payload).encode("utf-8")
    headers = {
        "content-type": "application/json",
        "content-length": str(len(body)),
    }
    return headers, body


def make_multipart_request(
    files: list[tuple[str, tuple[str, bytes, str]]],
    fields: dict[str, str] | None = None,
) -> tuple[dict[str, str], bytes]:
    boundary = "----CodexBoundary7MA4YWxkTrZu0gW"
    body = bytearray()
    for field_name, value in (fields or {}).items():
        body.extend(f"--{boundary}\r\n".encode("ascii"))
        body.extend(f'Content-Disposition: form-data; name="{field_name}"\r\n\r\n'.encode("ascii"))
        body.extend(value.encode("utf-8"))
        body.extend(b"\r\n")
    for field_name, (filename, file_bytes, content_type) in files:
        body.extend(f"--{boundary}\r\n".encode("ascii"))
        body.extend(
            f'Content-Disposition: form-data; name="{field_name}"; filename="{filename}"\r\n'.encode("ascii")
        )
        body.extend(f"Content-Type: {content_type}\r\n\r\n".encode("ascii"))
        body.extend(file_bytes)
        body.extend(b"\r\n")
    body.extend(f"--{boundary}--\r\n".encode("ascii"))
    headers = {
        "content-type": f"multipart/form-data; boundary={boundary}",
        "content-length": str(len(body)),
    }
    return headers, bytes(body)


class HttpAppTests(unittest.TestCase):
    def setUp(self) -> None:
        root = Path("tmp") / f"http_app_test_{uuid4().hex}"
        root.mkdir(parents=True, exist_ok=True)
        self.tmpdir = root
        storage = SpeakerStorage(db_path=root / "test.db", data_root=root / "data")
        self.adapter = DeterministicSpeakerAdapter()
        self.notifier = FakeNotifier()
        self.demo_exporter = MvpDemoExporter(
            data_root=root / "mvp_data",
            output_c=root / "test_audio_data.c",
            output_h=root / "test_audio_data.h",
        )

        self.speaker1_wav = pcm16_to_wav_bytes(synth_tone(300.0))
        self.speaker2_wav = pcm16_to_wav_bytes(synth_tone(500.0))
        self.unknown_wav = pcm16_to_wav_bytes(synth_tone(900.0))
        self.adapter.register_wav(self.speaker1_wav, [1.0, 0.0])
        self.adapter.register_wav(self.speaker2_wav, [0.0, 1.0])
        self.adapter.register_wav(self.unknown_wav, [-1.0, 0.0])

        service = SpeakerService(storage=storage, adapter=self.adapter)
        self.app = create_app(
            asr_adapter=MockAsrAdapter(),
            speaker_service_instance=service,
            speaker_tcp_server_instance=_NoopSpeakerTcpServer(),
            demo_exporter_instance=self.demo_exporter,
            notifier_instance=self.notifier,
        )
        self.client = MiniAsgiClient(self.app)

    def tearDown(self) -> None:
        import shutil

        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def create_speaker(self, speaker_id: str) -> None:
        headers, body = make_json_request({"speaker_id": speaker_id})
        created = self.client.post("/api/speakers", headers=headers, body=body)
        self.assertEqual(created.status_code, 200)

    def enroll_speaker(self, speaker_id: str, wav_bytes: bytes) -> None:
        upload_headers, upload_body = make_multipart_request(
            [
                ("files", ("01_enroll.wav", wav_bytes, "audio/wav")),
                ("files", ("02_enroll.wav", wav_bytes, "audio/wav")),
                ("files", ("03_enroll.wav", wav_bytes, "audio/wav")),
            ]
        )
        response = self.client.post(f"/api/speakers/{speaker_id}/samples", headers=upload_headers, body=upload_body)
        self.assertEqual(response.status_code, 200)

    def test_root_serves_new_web_ui(self) -> None:
        response = self.client.get("/")
        self.assertEqual(response.status_code, 200)
        self.assertIn('id="speakerIdInput"', response.text)
        self.assertIn('id="registerDirectBtn"', response.text)
        self.assertIn('id="testRelayBtn"', response.text)
        self.assertIn('id="resetDataBtn"', response.text)
        self.assertIn('id="speakerList"', response.text)

    def test_lifespan_logs_speaker_backend_status(self) -> None:
        async def exercise_lifespan() -> list[str]:
            with patch("builtins.print") as mock_print:
                async with self.app.router.lifespan_context(self.app):
                    pass
            return [" ".join(str(arg) for arg in call.args) for call in mock_print.call_args_list]

        lines = asyncio.run(exercise_lifespan())
        self.assertTrue(any(line.startswith("[speaker] configured=") for line in lines))
        self.assertTrue(any("adapter=DeterministicSpeakerAdapter" in line for line in lines))

    def test_create_and_delete_speaker(self) -> None:
        headers, body = make_json_request({"speaker_id": "speaker1"})
        created = self.client.post("/api/speakers", headers=headers, body=body)
        self.assertEqual(created.status_code, 200)
        self.assertEqual(created.json()["speaker_id"], "speaker1")
        self.assertEqual(created.json()["last_source"], "")

        deleted = self.client.delete("/api/speakers/speaker1")
        self.assertEqual(deleted.status_code, 200)
        self.assertTrue(deleted.json()["deleted"])

    def test_upload_samples_updates_last_source_metadata(self) -> None:
        self.create_speaker("speaker1")
        self.enroll_speaker("speaker1", self.speaker1_wav)

        speakers = self.client.get("/api/speakers")
        self.assertEqual(speakers.status_code, 200)
        payload = speakers.json()
        self.assertEqual(payload[0]["speaker_id"], "speaker1")
        self.assertEqual(payload[0]["last_source"], "pc-direct")
        self.assertTrue(payload[0]["last_sample_at"])

    def test_identify_persists_history(self) -> None:
        self.create_speaker("speaker1")
        self.enroll_speaker("speaker1", self.speaker1_wav)

        headers, body = make_multipart_request(
            [("file", ("identify.wav", self.speaker1_wav, "audio/wav"))],
            fields={"speaker_id": "speaker1", "notify_on_failure": "true"},
        )
        response = self.client.post("/api/identify", headers=headers, body=body)
        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertTrue(payload["passed"])
        self.assertFalse(payload["notification_attempted"])

        history = self.client.get("/api/identify-events?limit=20")
        self.assertEqual(history.status_code, 200)
        items = history.json()
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["flow"], "pc-direct")
        self.assertEqual(items[0]["expected_speaker_id"], "speaker1")
        self.assertTrue(items[0]["passed"])

    def test_identify_failure_returns_notification_failure_without_failing_request(self) -> None:
        self.notifier.result = AlertNotificationResult(attempted=True, sent=False, error="smtp failed")
        self.create_speaker("speaker1")
        self.enroll_speaker("speaker1", self.speaker1_wav)

        headers, body = make_multipart_request(
            [("file", ("identify.wav", self.unknown_wav, "audio/wav"))],
            fields={
                "speaker_id": "speaker1",
                "notify_on_failure": "true",
                "location_label": "browser-geolocation",
                "latitude": "31.2304",
                "longitude": "121.4737",
                "accuracy_m": "18.5",
            },
        )
        headers["x-device-id"] = "device-01"
        response = self.client.post("/api/identify", headers=headers, body=body)
        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertTrue(payload["notification_attempted"])
        self.assertFalse(payload["notification_sent"])
        self.assertEqual(payload["notification_error"], "smtp failed")
        self.assertEqual(len(self.notifier.calls), 1)
        context = self.notifier.calls[0]["context"]
        self.assertIsNotNone(context)
        assert context is not None
        self.assertEqual(context.flow, "pc-direct")
        self.assertEqual(context.device_id, "device-01")
        self.assertEqual(context.source_ip, "127.0.0.1")
        self.assertEqual(context.source_port, 12345)
        self.assertEqual(context.location_label, "browser-geolocation")
        self.assertAlmostEqual(context.latitude or 0.0, 31.2304)
        self.assertAlmostEqual(context.longitude or 0.0, 121.4737)
        self.assertAlmostEqual(context.accuracy_m or 0.0, 18.5)

    def test_mcu_relay_identify_with_expected_speaker_records_history(self) -> None:
        self.create_speaker("speaker1")
        self.enroll_speaker("speaker1", self.speaker1_wav)

        payload = asyncio.run(
            self.app.state.process_identify_wav(
                wav_bytes=self.speaker1_wav,
                expected_speaker_id="speaker1",
                notify_on_failure=True,
                flow="mcu-relay",
            )
        )
        self.assertTrue(payload.passed)
        self.assertFalse(payload.notification_attempted)

        history = self.client.get("/api/identify-events?limit=5").json()
        self.assertEqual(history[0]["flow"], "mcu-relay")
        self.assertEqual(history[0]["expected_speaker_id"], "speaker1")
        self.assertTrue(history[0]["passed"])

    def test_mcu_relay_identify_without_expected_speaker_skips_notification(self) -> None:
        self.create_speaker("speaker1")
        self.enroll_speaker("speaker1", self.speaker1_wav)

        payload = asyncio.run(
            self.app.state.process_identify_wav(
                wav_bytes=self.unknown_wav,
                expected_speaker_id="",
                notify_on_failure=False,
                flow="mcu-relay",
            )
        )
        self.assertFalse(payload.notification_attempted)
        self.assertEqual(payload.notification_error, "")

        history = self.client.get("/api/identify-events?limit=5").json()
        self.assertEqual(history[0]["flow"], "mcu-relay")
        self.assertEqual(history[0]["expected_speaker_id"], "")
        self.assertFalse(history[0]["notification_attempted"])

    def test_reset_endpoint_clears_runtime_data_and_resets_export_dataset(self) -> None:
        self.create_speaker("speaker1")
        self.enroll_speaker("speaker1", self.speaker1_wav)
        self.demo_exporter.save_register_clips("speaker1", [self.speaker1_wav, self.speaker1_wav, self.speaker1_wav])
        asyncio.run(
            self.app.state.process_identify_wav(
                wav_bytes=self.speaker1_wav,
                expected_speaker_id="speaker1",
                notify_on_failure=True,
                flow="pc-direct",
            )
        )

        response = self.client.post("/api/admin/reset")
        self.assertEqual(response.status_code, 200)
        payload = response.json()
        self.assertEqual(payload["deleted_speakers"], 1)
        self.assertEqual(payload["deleted_identify_events"], 1)
        self.assertEqual(self.client.get("/api/speakers").json(), [])
        self.assertEqual(self.client.get("/api/identify-events").json(), [])
        self.assertIn("return 0;", (self.tmpdir / "test_audio_data.c").read_text(encoding="ascii"))
        self.assertIn("expected_speaker_id", (self.tmpdir / "test_audio_data.h").read_text(encoding="ascii"))


if __name__ == "__main__":
    unittest.main()

