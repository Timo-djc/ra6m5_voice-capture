from __future__ import annotations

import asyncio
import smtplib
from abc import ABC, abstractmethod
from dataclasses import dataclass
from datetime import datetime, timezone
from email.message import EmailMessage
from typing import Callable, Protocol

try:
    from .config import CONFIG, ServerConfig
except ImportError:  # pragma: no cover
    from config import CONFIG, ServerConfig


class SmtpClient(Protocol):
    def starttls(self) -> None: ...

    def login(self, username: str, password: str) -> None: ...

    def send_message(self, message: EmailMessage) -> None: ...

    def quit(self) -> None: ...


@dataclass
class AlertNotificationResult:
    attempted: bool
    sent: bool
    error: str = ""


@dataclass(frozen=True)
class IdentifyFailureContext:
    flow: str = ""
    source_ip: str = ""
    source_port: int | None = None
    device_id: str = ""
    location_label: str = ""
    latitude: float | None = None
    longitude: float | None = None
    accuracy_m: float | None = None

    def map_url(self) -> str:
        if self.latitude is None or self.longitude is None:
            return ""
        return f"https://maps.google.com/?q={self.latitude:.6f},{self.longitude:.6f}"


class BaseIdentifyFailureNotifier(ABC):
    @abstractmethod
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
        raise NotImplementedError


class EmailIdentifyFailureNotifier(BaseIdentifyFailureNotifier):
    def __init__(
        self,
        config: ServerConfig | None = None,
        smtp_factory: Callable[[str, int, float], SmtpClient] | None = None,
    ) -> None:
        self.config = config or CONFIG
        self.smtp_factory = smtp_factory

    def _missing_config_fields(self) -> list[str]:
        missing = []
        for field_name, value in (
            ("SPEAKER_ALERT_SMTP_HOST", self.config.alert_smtp_host),
            ("SPEAKER_ALERT_FROM", self.config.alert_from),
            ("SPEAKER_ALERT_TO", self.config.alert_to),
        ):
            if not value:
                missing.append(field_name)
        if self.config.alert_smtp_use_tls and self.config.alert_smtp_use_ssl:
            missing.append("SPEAKER_ALERT_SMTP_USE_TLS/SPEAKER_ALERT_SMTP_USE_SSL conflict")
        if self.config.alert_smtp_username and not self.config.alert_smtp_password:
            missing.append("SPEAKER_ALERT_SMTP_PASSWORD")
        return missing

    def _build_message(
        self,
        *,
        expected_speaker_id: str,
        actual_status: str,
        actual_speaker_id: str,
        score: float,
        margin: float,
        context: IdentifyFailureContext | None = None,
    ) -> EmailMessage:
        context = context or IdentifyFailureContext()
        now_text = datetime.now(timezone.utc).isoformat()
        subject = f"{self.config.alert_subject_prefix} Speaker identify failed: {expected_speaker_id or 'unknown'}"
        lines = [
            "Speaker identify failed.",
            f"time_utc: {now_text}",
            f"expected_speaker_id: {expected_speaker_id or '(empty)'}",
            f"actual_status: {actual_status}",
            f"actual_speaker_id: {actual_speaker_id}",
            f"score: {score:.6f}",
            f"margin: {margin:.6f}",
        ]
        if context.flow:
            lines.append(f"flow: {context.flow}")
        if context.device_id:
            lines.append(f"device_id: {context.device_id}")
        if context.source_ip:
            lines.append(f"source_ip: {context.source_ip}")
        if context.source_port is not None:
            lines.append(f"source_port: {context.source_port}")
        if context.location_label:
            lines.append(f"location_label: {context.location_label}")
        if context.latitude is not None:
            lines.append(f"latitude: {context.latitude:.6f}")
        if context.longitude is not None:
            lines.append(f"longitude: {context.longitude:.6f}")
        if context.accuracy_m is not None:
            lines.append(f"accuracy_m: {context.accuracy_m:.3f}")
        map_url = context.map_url()
        if map_url:
            lines.append(f"map_url: {map_url}")
        body = "\n".join(lines)

        message = EmailMessage()
        message["Subject"] = subject
        message["From"] = self.config.alert_from
        message["To"] = self.config.alert_to
        message.set_content(body)
        return message

    def _create_client(self) -> SmtpClient:
        factory = self.smtp_factory
        if factory is not None:
            return factory(self.config.alert_smtp_host, self.config.alert_smtp_port, 15.0)
        if self.config.alert_smtp_use_ssl:
            return smtplib.SMTP_SSL(self.config.alert_smtp_host, self.config.alert_smtp_port, timeout=15.0)
        return smtplib.SMTP(self.config.alert_smtp_host, self.config.alert_smtp_port, timeout=15.0)

    def _send_message(self, message: EmailMessage) -> None:
        client = self._create_client()
        try:
            if self.config.alert_smtp_use_tls and not self.config.alert_smtp_use_ssl:
                client.starttls()
            if self.config.alert_smtp_username:
                client.login(self.config.alert_smtp_username, self.config.alert_smtp_password)
            client.send_message(message)
        finally:
            client.quit()

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
        if not expected_speaker_id:
            return AlertNotificationResult(attempted=False, sent=False, error="expected speaker_id is required")
        if not self.config.alert_email_enabled:
            return AlertNotificationResult(attempted=False, sent=False, error="email alerts are disabled")

        missing = self._missing_config_fields()
        if missing:
            return AlertNotificationResult(
                attempted=False,
                sent=False,
                error="missing email config: " + ", ".join(missing),
            )

        message = self._build_message(
            expected_speaker_id=expected_speaker_id,
            actual_status=actual_status,
            actual_speaker_id=actual_speaker_id,
            score=score,
            margin=margin,
            context=context,
        )
        try:
            await asyncio.to_thread(self._send_message, message)
        except Exception as exc:
            return AlertNotificationResult(attempted=True, sent=False, error=str(exc))
        return AlertNotificationResult(attempted=True, sent=True, error="")
