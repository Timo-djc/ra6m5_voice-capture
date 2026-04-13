import os
from dataclasses import dataclass
from pathlib import Path


def _env_bool(name: str, default: bool) -> bool:
    value = os.getenv(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


def _env_float(name: str, default: float) -> float:
    try:
        return float(os.getenv(name, str(default)))
    except ValueError:
        return default


def _env_int(name: str, default: int) -> int:
    try:
        return int(os.getenv(name, str(default)))
    except ValueError:
        return default


@dataclass
class ServerConfig:
    adapter: str = os.getenv("ASR_ADAPTER", "mock")
    mock_text: str = os.getenv("ASR_MOCK_TEXT", "test recognition result")
    mock_confidence: float = _env_float("ASR_MOCK_CONFIDENCE", 0.95)
    speaker_backend: str = os.getenv("SPEAKER_BACKEND", "modelscope")
    model_id: str = os.getenv("SPEAKER_MODEL_ID", "iic/speech_eres2net_sv_zh-cn_16k-common")
    model_revision: str = os.getenv("SPEAKER_MODEL_REVISION", "")
    data_root: Path = Path(os.getenv("SPEAKER_DATA_ROOT", "server_data"))
    sqlite_path: Path = Path(os.getenv("SPEAKER_SQLITE_PATH", "server_data/speakers.db"))
    tcp_host: str = os.getenv("SPEAKER_TCP_HOST", "0.0.0.0")
    tcp_port: int = _env_int("SPEAKER_TCP_PORT", 18080)
    tcp_header_timeout_s: float = _env_float("SPEAKER_HEADER_TIMEOUT_S", 5.0)
    tcp_payload_timeout_s: float = _env_float("SPEAKER_PAYLOAD_TIMEOUT_S", 45.0)
    target_sample_rate: int = 16000
    min_samples: int = _env_int("SPEAKER_MIN_SAMPLES", 16000)
    max_samples: int = _env_int("SPEAKER_MAX_SAMPLES", 96000)
    required_samples: int = _env_int("SPEAKER_REQUIRED_SAMPLES", 3)
    min_rms: float = _env_float("SPEAKER_MIN_RMS", 0.003)
    identify_threshold: float = _env_float("SPEAKER_IDENTIFY_THRESHOLD", 0.70)
    identify_margin: float = _env_float("SPEAKER_IDENTIFY_MARGIN", 0.05)
    fallback_identify_threshold: float = _env_float("SPEAKER_FALLBACK_IDENTIFY_THRESHOLD", 0.90)
    alert_email_enabled: bool = _env_bool("SPEAKER_ALERT_EMAIL_ENABLED", False)
    alert_smtp_host: str = os.getenv("SPEAKER_ALERT_SMTP_HOST", "")
    alert_smtp_port: int = _env_int("SPEAKER_ALERT_SMTP_PORT", 587)
    alert_smtp_username: str = os.getenv("SPEAKER_ALERT_SMTP_USERNAME", "")
    alert_smtp_password: str = os.getenv("SPEAKER_ALERT_SMTP_PASSWORD", "")
    alert_smtp_use_tls: bool = _env_bool("SPEAKER_ALERT_SMTP_USE_TLS", True)
    alert_smtp_use_ssl: bool = _env_bool("SPEAKER_ALERT_SMTP_USE_SSL", False)
    alert_from: str = os.getenv("SPEAKER_ALERT_FROM", "")
    alert_to: str = os.getenv("SPEAKER_ALERT_TO", "")
    alert_subject_prefix: str = os.getenv("SPEAKER_ALERT_SUBJECT_PREFIX", "[Speaker Alert]")


CONFIG = ServerConfig()
CONFIG.data_root.mkdir(parents=True, exist_ok=True)
CONFIG.sqlite_path.parent.mkdir(parents=True, exist_ok=True)

