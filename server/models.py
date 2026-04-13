from pydantic import BaseModel, ConfigDict, Field


class RecognizeSuccess(BaseModel):
    code: int = 0
    text: str
    confidence: float


class RecognizeError(BaseModel):
    code: int = 1001
    message: str


class SpeakerCreateRequest(BaseModel):
    speaker_id: str
    display_name: str | None = None


class SpeakerSummary(BaseModel):
    speaker_id: str
    display_name: str | None = None
    created_at: str
    sample_count: int
    required_samples: int
    active: bool
    last_source: str = ""
    last_sample_at: str = ""


class SpeakerIdentifyResponse(BaseModel):
    status: str
    speaker_id: str
    score: float
    margin: float
    expected_speaker_id: str = ""
    passed: bool = False
    notification_attempted: bool = False
    notification_sent: bool = False
    notification_error: str = ""


class SpeakerEnrollResponse(BaseModel):
    speaker_id: str
    accepted: int
    required: int
    score: float
    active: bool


class SpeakerBackendStatusResponse(BaseModel):
    model_config = ConfigDict(protected_namespaces=())

    configured_backend: str
    effective_backend: str
    adapter_class: str
    model_id: str = ""
    model_revision: str = ""
    identify_strategy: str
    identify_threshold: float
    message: str = ""
    warning: bool = False


class IdentifyEventSummary(BaseModel):
    event_id: int
    flow: str
    expected_speaker_id: str = ""
    status: str
    speaker_id: str
    score: float
    margin: float
    passed: bool
    notification_attempted: bool
    notification_sent: bool
    notification_error: str = ""
    created_at: str


class AdminResetResponse(BaseModel):
    deleted_speakers: int
    deleted_identify_events: int
    sqlite_path: str
    data_root: str
    mvp_data_root: str
    output_c: str
    output_h: str
    removed_legacy_paths: list[str] = Field(default_factory=list)
