import unittest

from server.alert_notifier import EmailIdentifyFailureNotifier, IdentifyFailureContext
from server.config import ServerConfig


class FakeSmtpClient:
    def __init__(self, should_fail: bool = False) -> None:
        self.should_fail = should_fail
        self.started_tls = False
        self.logged_in = None
        self.sent_messages = []
        self.quit_called = False

    def starttls(self) -> None:
        self.started_tls = True

    def login(self, username: str, password: str) -> None:
        self.logged_in = (username, password)

    def send_message(self, message) -> None:
        if self.should_fail:
            raise RuntimeError("smtp boom")
        self.sent_messages.append(message)

    def quit(self) -> None:
        self.quit_called = True


class EmailIdentifyFailureNotifierTests(unittest.IsolatedAsyncioTestCase):
    def build_config(self) -> ServerConfig:
        return ServerConfig(
            alert_email_enabled=True,
            alert_smtp_host="smtp.example.com",
            alert_smtp_port=587,
            alert_smtp_username="robot@example.com",
            alert_smtp_password="secret",
            alert_smtp_use_tls=True,
            alert_smtp_use_ssl=False,
            alert_from="robot@example.com",
            alert_to="ops@example.com",
            alert_subject_prefix="[Alert]",
        )

    async def test_notify_failure_sends_summary_email(self) -> None:
        client = FakeSmtpClient()
        notifier = EmailIdentifyFailureNotifier(
            config=self.build_config(),
            smtp_factory=lambda host, port, timeout: client,
        )

        result = await notifier.notify_failure(
            expected_speaker_id="speaker1",
            actual_status="UNKNOWN",
            actual_speaker_id="unknown",
            score=0.12,
            margin=0.03,
            context=IdentifyFailureContext(
                flow="pc-direct",
                source_ip="192.168.1.9",
                source_port=45678,
                device_id="device-01",
                location_label="browser-geolocation",
                latitude=31.2304,
                longitude=121.4737,
                accuracy_m=18.5,
            ),
        )

        self.assertTrue(result.attempted)
        self.assertTrue(result.sent)
        self.assertEqual(result.error, "")
        self.assertTrue(client.started_tls)
        self.assertEqual(client.logged_in, ("robot@example.com", "secret"))
        self.assertTrue(client.quit_called)
        self.assertEqual(len(client.sent_messages), 1)
        message = client.sent_messages[0]
        self.assertIn("speaker1", message["Subject"])
        body = message.get_content()
        self.assertIn("expected_speaker_id: speaker1", body)
        self.assertIn("actual_status: UNKNOWN", body)
        self.assertIn("score: 0.120000", body)
        self.assertIn("margin: 0.030000", body)
        self.assertIn("flow: pc-direct", body)
        self.assertIn("source_ip: 192.168.1.9", body)
        self.assertIn("source_port: 45678", body)
        self.assertIn("device_id: device-01", body)
        self.assertIn("location_label: browser-geolocation", body)
        self.assertIn("latitude: 31.230400", body)
        self.assertIn("longitude: 121.473700", body)
        self.assertIn("accuracy_m: 18.500", body)
        self.assertIn("map_url: https://maps.google.com/?q=31.230400,121.473700", body)

    async def test_notify_failure_returns_error_when_disabled(self) -> None:
        config = self.build_config()
        config.alert_email_enabled = False
        notifier = EmailIdentifyFailureNotifier(config=config)

        result = await notifier.notify_failure(
            expected_speaker_id="speaker1",
            actual_status="UNKNOWN",
            actual_speaker_id="unknown",
            score=0.1,
            margin=0.01,
        )

        self.assertFalse(result.attempted)
        self.assertFalse(result.sent)
        self.assertEqual(result.error, "email alerts are disabled")

    async def test_notify_failure_returns_error_when_config_missing(self) -> None:
        config = self.build_config()
        config.alert_to = ""
        notifier = EmailIdentifyFailureNotifier(config=config)

        result = await notifier.notify_failure(
            expected_speaker_id="speaker1",
            actual_status="UNKNOWN",
            actual_speaker_id="unknown",
            score=0.1,
            margin=0.01,
        )

        self.assertFalse(result.attempted)
        self.assertFalse(result.sent)
        self.assertIn("SPEAKER_ALERT_TO", result.error)

    async def test_notify_failure_returns_error_when_smtp_raises(self) -> None:
        client = FakeSmtpClient(should_fail=True)
        notifier = EmailIdentifyFailureNotifier(
            config=self.build_config(),
            smtp_factory=lambda host, port, timeout: client,
        )

        result = await notifier.notify_failure(
            expected_speaker_id="speaker1",
            actual_status="KNOWN",
            actual_speaker_id="speaker2",
            score=0.88,
            margin=0.40,
        )

        self.assertTrue(result.attempted)
        self.assertFalse(result.sent)
        self.assertEqual(result.error, "smtp boom")
        self.assertTrue(client.quit_called)


if __name__ == "__main__":
    unittest.main()
