import shutil
from pathlib import Path

from fastapi import FastAPI, File, Form, Header, HTTPException, Query, Request, Response, UploadFile
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

try:
    from .alert_notifier import (
        AlertNotificationResult,
        BaseIdentifyFailureNotifier,
        EmailIdentifyFailureNotifier,
        IdentifyFailureContext,
    )
    from .asr_adapter import build_adapter
    from .config import CONFIG
    from .models import (
        AdminResetResponse,
        IdentifyEventSummary,
        RecognizeError,
        RecognizeSuccess,
        SpeakerCreateRequest,
        SpeakerEnrollResponse,
        SpeakerIdentifyResponse,
        SpeakerBackendStatusResponse,
        SpeakerSummary,
    )
    from .mvp_demo_export import MvpDemoExporter, MvpDemoExportError
    from .speaker_model_adapter import format_speaker_backend_status_lines
    from .speaker_service import SpeakerService, SpeakerServiceError
    from .tcp_server import SpeakerTcpServer
except ImportError:  # pragma: no cover
    from alert_notifier import (
        AlertNotificationResult,
        BaseIdentifyFailureNotifier,
        EmailIdentifyFailureNotifier,
        IdentifyFailureContext,
    )
    from asr_adapter import build_adapter
    from config import CONFIG
    from models import (
        AdminResetResponse,
        IdentifyEventSummary,
        RecognizeError,
        RecognizeSuccess,
        SpeakerCreateRequest,
        SpeakerEnrollResponse,
        SpeakerIdentifyResponse,
        SpeakerBackendStatusResponse,
        SpeakerSummary,
    )
    from mvp_demo_export import MvpDemoExporter, MvpDemoExportError
    from speaker_model_adapter import format_speaker_backend_status_lines
    from speaker_service import SpeakerService, SpeakerServiceError
    from tcp_server import SpeakerTcpServer


try:
    import multipart  # type: ignore  # noqa: F401

    MULTIPART_ENABLED = True
except Exception:  # pragma: no cover
    MULTIPART_ENABLED = False

STATIC_DIR = Path(__file__).resolve().parent / "static"
INDEX_FILE = STATIC_DIR / "index.html"
LEGACY_SERVER_DATA_DIR = Path(__file__).resolve().parent / "server_data"


def _json_response(payload: str, status_code: int) -> Response:
    return Response(content=payload, media_type="application/json", status_code=status_code)


def _is_wav(data: bytes) -> bool:
    return len(data) >= 12 and data[0:4] == b"RIFF" and data[8:12] == b"WAVE"


def _request_source(request: Request) -> tuple[str, int | None]:
    forwarded_for = request.headers.get("x-forwarded-for", "").split(",")[0].strip()
    client = request.client
    source_ip = forwarded_for or (client.host if client else "")
    source_port = client.port if client else None
    return source_ip, source_port


def _speaker_summary(record) -> SpeakerSummary:
    return SpeakerSummary(
        speaker_id=record.speaker_id,
        display_name=record.display_name,
        created_at=record.created_at,
        sample_count=record.sample_count,
        required_samples=record.required_samples,
        active=record.active,
        last_source=record.last_source,
        last_sample_at=record.last_sample_at,
    )


def _identify_event_summary(record) -> IdentifyEventSummary:
    return IdentifyEventSummary(
        event_id=record.event_id,
        flow=record.flow,
        expected_speaker_id=record.expected_speaker_id,
        status=record.status,
        speaker_id=record.speaker_id,
        score=record.score,
        margin=record.margin,
        passed=record.passed,
        notification_attempted=record.notification_attempted,
        notification_sent=record.notification_sent,
        notification_error=record.notification_error,
        created_at=record.created_at,
    )


class _NoopSpeakerTcpServer:
    async def start(self) -> None:
        return None

    async def stop(self) -> None:
        return None


def create_app(
    *,
    asr_adapter=None,
    speaker_service_instance: SpeakerService | None = None,
    speaker_tcp_server_instance=None,
    demo_exporter_instance: MvpDemoExporter | None = None,
    notifier_instance: BaseIdentifyFailureNotifier | None = None,
) -> FastAPI:
    adapter = asr_adapter or build_adapter()
    speaker_service = speaker_service_instance or SpeakerService()
    demo_exporter = demo_exporter_instance or MvpDemoExporter()
    notifier = notifier_instance or EmailIdentifyFailureNotifier()

    async def process_identify_wav(
        *,
        wav_bytes: bytes,
        expected_speaker_id: str,
        notify_on_failure: bool,
        flow: str,
        device_id: str = "",
        source_ip: str = "",
        source_port: int | None = None,
        location_label: str = "",
        latitude: float | None = None,
        longitude: float | None = None,
        accuracy_m: float | None = None,
    ) -> SpeakerIdentifyResponse:
        normalized_expected = ""
        if expected_speaker_id:
            normalized_expected = speaker_service.validate_speaker_id(expected_speaker_id)

        failure_context = IdentifyFailureContext(
            flow=flow,
            source_ip=source_ip,
            source_port=source_port,
            device_id=device_id,
            location_label=location_label,
            latitude=latitude,
            longitude=longitude,
            accuracy_m=accuracy_m,
        )

        result = speaker_service.identify_wav(wav_bytes)
        passed = bool(
            normalized_expected
            and result.status == "KNOWN"
            and result.speaker_id == normalized_expected
        )

        notification = None
        if notify_on_failure and not passed:
            if normalized_expected:
                notification = await notifier.notify_failure(
                    expected_speaker_id=normalized_expected,
                    actual_status=result.status,
                    actual_speaker_id=result.speaker_id,
                    score=result.score,
                    margin=result.margin,
                    context=failure_context,
                )
            else:
                notification = AlertNotificationResult(
                    attempted=False,
                    sent=False,
                    error="expected speaker_id is required",
                )

        response = SpeakerIdentifyResponse(
            status=result.status,
            speaker_id=result.speaker_id,
            score=result.score,
            margin=result.margin,
            expected_speaker_id=normalized_expected,
            passed=passed,
            notification_attempted=notification.attempted if notification else False,
            notification_sent=notification.sent if notification else False,
            notification_error=notification.error if notification else "",
        )
        speaker_service.record_identify_event(
            flow=flow,
            expected_speaker_id=response.expected_speaker_id,
            status=response.status,
            speaker_id=response.speaker_id,
            score=response.score,
            margin=response.margin,
            passed=response.passed,
            notification_attempted=response.notification_attempted,
            notification_sent=response.notification_sent,
            notification_error=response.notification_error,
        )
        return response

    speaker_tcp_server = speaker_tcp_server_instance or SpeakerTcpServer(
        speaker_service,
        identify_handler=process_identify_wav,
    )

    from contextlib import asynccontextmanager

    @asynccontextmanager
    async def lifespan(_app: FastAPI):
        for line in format_speaker_backend_status_lines(speaker_service.backend_status):
            print(line)
        await speaker_tcp_server.start()
        try:
            yield
        finally:
            await speaker_tcp_server.stop()

    app = FastAPI(title="W800 Speech Service", version="3.0.0", lifespan=lifespan)
    app.add_middleware(
        CORSMiddleware,
        allow_origins=["*"],
        allow_credentials=True,
        allow_methods=["*"],
        allow_headers=["*"],
    )
    app.state.speaker_service = speaker_service
    app.state.speaker_backend_status = speaker_service.backend_status
    app.state.demo_exporter = demo_exporter
    app.state.process_identify_wav = process_identify_wav
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")

    @app.get("/")
    async def index():
        return FileResponse(INDEX_FILE)

    @app.post("/asr/recognize")
    async def recognize(
        request: Request,
        x_device_id: str = Header(default="", alias="X-Device-Id"),
        x_sample_rate: int = Header(default=16000, alias="X-Sample-Rate"),
        x_format: str = Header(default="wav", alias="X-Format"),
    ):
        body = await request.body()
        if not body:
            err = RecognizeError(code=1001, message="empty body")
            return _json_response(err.model_dump_json(), 400)

        if x_format.lower() != "wav":
            err = RecognizeError(code=1001, message="X-Format must be wav")
            return _json_response(err.model_dump_json(), 400)

        if not _is_wav(body):
            err = RecognizeError(code=1001, message="invalid wav payload")
            return _json_response(err.model_dump_json(), 400)

        _ = x_device_id
        result = adapter.recognize_wav(body, x_sample_rate)
        if result.code != 0:
            err = RecognizeError(code=result.code, message="asr failed")
            return _json_response(err.model_dump_json(), 500)

        ok = RecognizeSuccess(code=0, text=result.text, confidence=result.confidence)
        return _json_response(ok.model_dump_json(), 200)

    @app.post("/api/speakers", response_model=SpeakerSummary)
    async def create_speaker(payload: SpeakerCreateRequest):
        try:
            record = speaker_service.create_speaker(payload.speaker_id, payload.display_name)
            return _speaker_summary(record)
        except SpeakerServiceError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

    @app.get("/api/speakers", response_model=list[SpeakerSummary])
    async def list_speakers():
        return [_speaker_summary(record) for record in speaker_service.list_speakers()]

    @app.get("/api/speakers/{speaker_id}", response_model=SpeakerSummary)
    async def get_speaker(speaker_id: str):
        try:
            return _speaker_summary(speaker_service.get_speaker(speaker_id))
        except SpeakerServiceError as exc:
            raise HTTPException(status_code=404, detail=str(exc)) from exc

    @app.get("/api/identify-events", response_model=list[IdentifyEventSummary])
    async def list_identify_events(limit: int = Query(default=20, ge=1, le=200)):
        return [_identify_event_summary(record) for record in speaker_service.list_identify_events(limit)]

    @app.get("/api/system/status", response_model=SpeakerBackendStatusResponse)
    async def get_system_status():
        status = speaker_service.backend_status
        effective_backend = status.effective_backend.lower()
        identify_strategy = "sample-majority" if effective_backend == "fallback" else "centroid"
        identify_threshold = (
            CONFIG.fallback_identify_threshold if effective_backend == "fallback" else CONFIG.identify_threshold
        )
        return SpeakerBackendStatusResponse(
            configured_backend=status.configured_backend,
            effective_backend=status.effective_backend,
            adapter_class=status.adapter_class,
            model_id=status.model_id,
            model_revision=status.model_revision,
            identify_strategy=identify_strategy,
            identify_threshold=identify_threshold,
            message=status.message,
            warning=status.warning,
        )

    if MULTIPART_ENABLED:

        @app.post("/api/speakers/{speaker_id}/samples", response_model=list[SpeakerEnrollResponse])
        async def upload_samples(speaker_id: str, files: list[UploadFile] = File(...)):
            results = []
            try:
                for idx, upload in enumerate(files, start=1):
                    wav_bytes = await upload.read()
                    results.append(
                        SpeakerEnrollResponse(
                            **speaker_service.enroll_wav(
                                speaker_id=speaker_id,
                                wav_bytes=wav_bytes,
                                utter_idx=idx,
                                source="pc-direct",
                            ).__dict__
                        )
                    )
                return results
            except SpeakerServiceError as exc:
                raise HTTPException(status_code=400, detail=str(exc)) from exc

        @app.post("/api/identify", response_model=SpeakerIdentifyResponse)
        async def identify(
            request: Request,
            file: UploadFile = File(...),
            speaker_id: str = Form(""),
            notify_on_failure: bool = Form(False),
            location_label: str = Form(""),
            latitude: float | None = Form(None),
            longitude: float | None = Form(None),
            accuracy_m: float | None = Form(None),
            x_device_id: str = Header(default="", alias="X-Device-Id"),
        ):
            source_ip, source_port = _request_source(request)
            try:
                return await process_identify_wav(
                    wav_bytes=await file.read(),
                    expected_speaker_id=speaker_id,
                    notify_on_failure=notify_on_failure,
                    flow="pc-direct",
                    device_id=x_device_id,
                    source_ip=source_ip,
                    source_port=source_port,
                    location_label=location_label,
                    latitude=latitude,
                    longitude=longitude,
                    accuracy_m=accuracy_m,
                )
            except SpeakerServiceError as exc:
                raise HTTPException(status_code=400, detail=str(exc)) from exc

        @app.post("/api/mvp-demo/register-export")
        async def export_register_demo(speaker_id: str = Form(...), files: list[UploadFile] = File(...)):
            try:
                speaker_id = speaker_service.validate_speaker_id(speaker_id)
                wav_files = [await upload.read() for upload in files]
                return demo_exporter.save_register_clips(speaker_id, wav_files)
            except (SpeakerServiceError, MvpDemoExportError) as exc:
                raise HTTPException(status_code=400, detail=str(exc)) from exc

        @app.post("/api/mvp-demo/identify-export")
        async def export_identify_demo(speaker_id: str = Form(...), file: UploadFile = File(...)):
            try:
                speaker_id = speaker_service.validate_speaker_id(speaker_id)
                return demo_exporter.save_identify_clip(speaker_id, await file.read())
            except (SpeakerServiceError, MvpDemoExportError) as exc:
                raise HTTPException(status_code=400, detail=str(exc)) from exc

    else:

        @app.post("/api/speakers/{speaker_id}/samples")
        async def upload_samples_unavailable(speaker_id: str, request: Request):
            _ = speaker_id
            _ = request
            raise HTTPException(status_code=503, detail="python-multipart is not installed")

        @app.post("/api/identify")
        async def identify_unavailable(request: Request):
            _ = request
            raise HTTPException(status_code=503, detail="python-multipart is not installed")

        @app.post("/api/mvp-demo/register-export")
        async def export_register_demo_unavailable(request: Request):
            _ = request
            raise HTTPException(status_code=503, detail="python-multipart is not installed")

        @app.post("/api/mvp-demo/identify-export")
        async def export_identify_demo_unavailable(request: Request):
            _ = request
            raise HTTPException(status_code=503, detail="python-multipart is not installed")

    @app.delete("/api/speakers/{speaker_id}")
    async def delete_speaker(speaker_id: str):
        try:
            deleted = speaker_service.delete_speaker(speaker_id)
            if not deleted:
                raise HTTPException(status_code=404, detail="speaker not found")
            return {"deleted": True, "speaker_id": speaker_id}
        except SpeakerServiceError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

    @app.post("/api/admin/reset", response_model=AdminResetResponse)
    async def reset_runtime_data():
        summary = speaker_service.reset_runtime_data()
        export_summary = demo_exporter.reset_dataset()
        removed_legacy_paths: list[str] = []
        if LEGACY_SERVER_DATA_DIR.exists() and LEGACY_SERVER_DATA_DIR.resolve() != speaker_service.storage.data_root.resolve():
            shutil.rmtree(LEGACY_SERVER_DATA_DIR, ignore_errors=True)
            removed_legacy_paths.append(str(LEGACY_SERVER_DATA_DIR))
        return AdminResetResponse(
            deleted_speakers=summary.deleted_speakers,
            deleted_identify_events=summary.deleted_identify_events,
            sqlite_path=str(speaker_service.storage.db_path),
            data_root=str(speaker_service.storage.data_root),
            mvp_data_root=export_summary["data_root"],
            output_c=export_summary["output_c"],
            output_h=export_summary["output_h"],
            removed_legacy_paths=removed_legacy_paths,
        )

    return app


adapter = build_adapter()
speaker_service = SpeakerService()
demo_exporter = MvpDemoExporter()
identify_failure_notifier = EmailIdentifyFailureNotifier()
app = create_app(
    asr_adapter=adapter,
    speaker_service_instance=speaker_service,
    demo_exporter_instance=demo_exporter,
    notifier_instance=identify_failure_notifier,
)


