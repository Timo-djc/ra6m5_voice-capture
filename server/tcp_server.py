import asyncio
import logging
import time
from dataclasses import dataclass
from typing import Awaitable, Callable, Literal

try:
    from .audio_utils import pcm16_to_wav_bytes
    from .config import CONFIG
    from .speaker_service import SpeakerService, SpeakerServiceError
except ImportError:  # pragma: no cover
    from audio_utils import pcm16_to_wav_bytes
    from config import CONFIG
    from speaker_service import SpeakerService, SpeakerServiceError

LOGGER = logging.getLogger("uvicorn.error")
PAYLOAD_CHUNK_BYTES = 4096
IdentifyHandler = Callable[..., Awaitable[object]]


@dataclass
class IdentifyFrame:
    session: int
    slot: int
    expected_speaker_id: str
    samples: int
    payload_bytes: int


@dataclass
class EnrollFrame:
    session: int
    slot: int
    speaker_id: str
    utter_idx: int
    utter_total: int
    samples: int
    payload_bytes: int


def parse_frame_header(line: str) -> tuple[Literal["SVI", "SVR"], IdentifyFrame | EnrollFrame]:
    parts = line.strip().split(",")
    if not parts:
        raise ValueError("empty header")

    kind = parts[0]
    if kind == "SVI":
        if parts[1] != "1":
            raise ValueError("invalid SVI header")
        if len(parts) == 9:
            expected_speaker_id = ""
            sample_rate, bit_depth, channels = int(parts[4]), int(parts[5]), int(parts[6])
            samples, payload_bytes = int(parts[7]), int(parts[8])
        elif len(parts) == 10:
            expected_speaker_id = parts[4]
            sample_rate, bit_depth, channels = int(parts[5]), int(parts[6]), int(parts[7])
            samples, payload_bytes = int(parts[8]), int(parts[9])
        else:
            raise ValueError("invalid SVI header")

        if (sample_rate, bit_depth, channels) != (16000, 16, 1):
            raise ValueError("unsupported audio format")
        frame = IdentifyFrame(
            session=int(parts[2]),
            slot=int(parts[3]),
            expected_speaker_id=expected_speaker_id,
            samples=samples,
            payload_bytes=payload_bytes,
        )
        if frame.payload_bytes != frame.samples * 2:
            raise ValueError("payload length mismatch")
        return "SVI", frame

    if kind == "SVR":
        if len(parts) != 12 or parts[1] != "1":
            raise ValueError("invalid SVR header")
        sample_rate, bit_depth, channels = int(parts[7]), int(parts[8]), int(parts[9])
        if (sample_rate, bit_depth, channels) != (16000, 16, 1):
            raise ValueError("unsupported audio format")
        frame = EnrollFrame(
            session=int(parts[2]),
            slot=int(parts[3]),
            speaker_id=parts[4],
            utter_idx=int(parts[5]),
            utter_total=int(parts[6]),
            samples=int(parts[10]),
            payload_bytes=int(parts[11]),
        )
        if frame.payload_bytes != frame.samples * 2:
            raise ValueError("payload length mismatch")
        return "SVR", frame

    raise ValueError(f"unsupported frame type: {kind}")


class SpeakerTcpServer:
    def __init__(
        self,
        service: SpeakerService,
        identify_handler: IdentifyHandler | None = None,
    ) -> None:
        self.service = service
        self.identify_handler = identify_handler
        self._server: asyncio.base_events.Server | None = None

    async def start(self) -> None:
        self._server = await asyncio.start_server(
            self._handle_client,
            host=CONFIG.tcp_host,
            port=CONFIG.tcp_port,
        )

    async def stop(self) -> None:
        if self._server is None:
            return
        self._server.close()
        await self._server.wait_closed()
        self._server = None

    @staticmethod
    async def _read_payload(reader: asyncio.StreamReader, size: int) -> bytes:
        payload = bytearray()
        deadline = time.monotonic() + CONFIG.tcp_payload_timeout_s

        while len(payload) < size:
            remaining = size - len(payload)
            chunk_size = PAYLOAD_CHUNK_BYTES if remaining > PAYLOAD_CHUNK_BYTES else remaining
            timeout_s = deadline - time.monotonic()
            if timeout_s <= 0.0:
                raise TimeoutError(f"payload timeout after {len(payload)}/{size} bytes")

            try:
                chunk = await asyncio.wait_for(reader.read(chunk_size), timeout=timeout_s)
            except asyncio.TimeoutError as exc:
                raise TimeoutError(f"payload timeout after {len(payload)}/{size} bytes") from exc
            if not chunk:
                raise ConnectionError(f"peer closed after {len(payload)}/{size} bytes")

            payload.extend(chunk)

        return bytes(payload)

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        peer = writer.get_extra_info("peername")
        LOGGER.info("speaker tcp accepted peer=%s", peer)
        try:
            while not reader.at_eof():
                frame = None
                started = time.monotonic()
                line_raw = await asyncio.wait_for(reader.readline(), timeout=CONFIG.tcp_header_timeout_s)
                if not line_raw:
                    LOGGER.info("speaker tcp peer=%s closed before header", peer)
                    break
                try:
                    frame_type, frame = parse_frame_header(line_raw.decode("ascii", errors="ignore"))
                    LOGGER.info(
                        "speaker tcp frame peer=%s type=%s session=%s slot=%s payload=%s",
                        peer,
                        frame_type,
                        frame.session,
                        frame.slot,
                        frame.payload_bytes,
                    )
                    payload = await self._read_payload(reader, frame.payload_bytes)
                    wav_bytes = pcm16_to_wav_bytes(payload, sample_rate=16000)
                    if frame_type == "SVI":
                        assert isinstance(frame, IdentifyFrame)
                        source_ip = peer[0] if isinstance(peer, tuple) and len(peer) >= 1 else ""
                        source_port = peer[1] if isinstance(peer, tuple) and len(peer) >= 2 else None
                        if self.identify_handler is not None:
                            result = await self.identify_handler(
                                wav_bytes=wav_bytes,
                                expected_speaker_id=frame.expected_speaker_id,
                                notify_on_failure=bool(frame.expected_speaker_id),
                                flow="mcu-relay",
                                source_ip=source_ip,
                                source_port=source_port,
                            )
                        else:
                            result = self.service.identify_wav(wav_bytes)
                        reply = f"IDENTIFY,{frame.session},{frame.slot},{result.status},{result.speaker_id},{result.score:.3f}\n"
                    else:
                        assert isinstance(frame, EnrollFrame)
                        result = self.service.enroll_wav(
                            speaker_id=frame.speaker_id,
                            wav_bytes=wav_bytes,
                            utter_idx=frame.utter_idx,
                            source="mcu-relay",
                        )
                        reply = f"ENROLL,{frame.session},{frame.slot},{result.speaker_id},{result.accepted},{result.required},{result.score:.3f}\n"
                    writer.write(reply.encode("ascii"))
                    await writer.drain()
                    LOGGER.info(
                        "speaker tcp replied peer=%s type=%s elapsed_ms=%d reply=%s",
                        peer,
                        frame_type,
                        int((time.monotonic() - started) * 1000.0),
                        reply.strip(),
                    )
                except SpeakerServiceError as exc:
                    session = frame.session if frame is not None else 0
                    slot = frame.slot if frame is not None else 0
                    reply = f"ERROR,{session},{slot},{str(exc).replace(',', '_')}\n"
                    writer.write(reply.encode("ascii", errors="ignore"))
                    await writer.drain()
                    LOGGER.warning("speaker tcp service error peer=%s reply=%s", peer, reply.strip())
                except (TimeoutError, asyncio.TimeoutError) as exc:
                    session = frame.session if frame is not None else 0
                    slot = frame.slot if frame is not None else 0
                    reply = f"ERROR,{session},{slot},TIMEOUT\n"
                    writer.write(reply.encode("ascii"))
                    await writer.drain()
                    LOGGER.warning("speaker tcp timeout peer=%s detail=%s", peer, exc)
                    break
                except ConnectionError as exc:
                    session = frame.session if frame is not None else 0
                    slot = frame.slot if frame is not None else 0
                    reply = f"ERROR,{session},{slot},SHORT_PAYLOAD\n"
                    writer.write(reply.encode("ascii"))
                    await writer.drain()
                    LOGGER.warning("speaker tcp short payload peer=%s detail=%s", peer, exc)
                    break
                except Exception:
                    session = frame.session if frame is not None else 0
                    slot = frame.slot if frame is not None else 0
                    writer.write(f"ERROR,{session},{slot},PARSE_FAIL\n".encode("ascii"))
                    await writer.drain()
                    LOGGER.exception("speaker tcp parse fail peer=%s", peer)
                    break
        finally:
            LOGGER.info("speaker tcp closing peer=%s", peer)
            writer.close()
            await writer.wait_closed()
