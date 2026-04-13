import shutil
import sqlite3
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, List, Optional
from uuid import uuid4

try:
    from .config import CONFIG
except ImportError:  # pragma: no cover
    from config import CONFIG


def pack_embedding(values: Iterable[float]) -> bytes:
    vals = [float(v) for v in values]
    if not vals:
        raise ValueError("embedding is empty")
    return struct.pack("<I", len(vals)) + struct.pack(f"<{len(vals)}f", *vals)


def unpack_embedding(blob: bytes) -> List[float]:
    if len(blob) < 4:
        raise ValueError("invalid embedding blob")
    dim = struct.unpack("<I", blob[:4])[0]
    raw = blob[4:]
    expected = dim * 4
    if len(raw) != expected:
        raise ValueError("embedding blob length mismatch")
    return list(struct.unpack(f"<{dim}f", raw))


@dataclass
class SpeakerRecord:
    speaker_id: str
    display_name: Optional[str]
    created_at: str
    sample_count: int
    required_samples: int
    active: bool
    last_source: str
    last_sample_at: str


@dataclass
class IdentifyEventRecord:
    event_id: int
    flow: str
    expected_speaker_id: str
    status: str
    speaker_id: str
    score: float
    margin: float
    passed: bool
    notification_attempted: bool
    notification_sent: bool
    notification_error: str
    created_at: str


@dataclass
class RuntimeResetSummary:
    deleted_speakers: int
    deleted_identify_events: int


class SpeakerStorage:
    def __init__(self, db_path: Path = CONFIG.sqlite_path, data_root: Path = CONFIG.data_root) -> None:
        self.db_path = db_path
        self.data_root = data_root
        self.data_root.mkdir(parents=True, exist_ok=True)
        self.db_path.parent.mkdir(parents=True, exist_ok=True)
        self._init_db()

    def _connect(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self.db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def _init_db(self) -> None:
        with self._connect() as conn:
            conn.executescript(
                """
                PRAGMA foreign_keys = ON;

                CREATE TABLE IF NOT EXISTS speakers (
                    speaker_id TEXT PRIMARY KEY,
                    display_name TEXT,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                CREATE TABLE IF NOT EXISTS speaker_embeddings (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    speaker_id TEXT NOT NULL,
                    utter_idx INTEGER,
                    source TEXT NOT NULL,
                    wav_path TEXT NOT NULL,
                    embedding BLOB NOT NULL,
                    backend TEXT NOT NULL,
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                    FOREIGN KEY (speaker_id) REFERENCES speakers (speaker_id) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS speaker_profiles (
                    speaker_id TEXT PRIMARY KEY,
                    centroid BLOB NOT NULL,
                    sample_count INTEGER NOT NULL,
                    updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,
                    FOREIGN KEY (speaker_id) REFERENCES speakers (speaker_id) ON DELETE CASCADE
                );

                CREATE TABLE IF NOT EXISTS identify_events (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    flow TEXT NOT NULL,
                    expected_speaker_id TEXT NOT NULL DEFAULT '',
                    status TEXT NOT NULL,
                    speaker_id TEXT NOT NULL,
                    score REAL NOT NULL,
                    margin REAL NOT NULL,
                    passed INTEGER NOT NULL DEFAULT 0,
                    notification_attempted INTEGER NOT NULL DEFAULT 0,
                    notification_sent INTEGER NOT NULL DEFAULT 0,
                    notification_error TEXT NOT NULL DEFAULT '',
                    created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
                );

                CREATE INDEX IF NOT EXISTS idx_speaker_embeddings_speaker_id
                ON speaker_embeddings (speaker_id, id);

                CREATE INDEX IF NOT EXISTS idx_identify_events_created_at
                ON identify_events (created_at DESC, id DESC);
                """
            )

    @staticmethod
    def _speaker_from_row(row: sqlite3.Row) -> SpeakerRecord:
        return SpeakerRecord(
            speaker_id=row["speaker_id"],
            display_name=row["display_name"],
            created_at=row["created_at"],
            sample_count=int(row["sample_count"]),
            required_samples=int(row["required_samples"]),
            active=bool(row["active"]),
            last_source=row["last_source"] or "",
            last_sample_at=row["last_sample_at"] or "",
        )

    @staticmethod
    def _identify_event_from_row(row: sqlite3.Row) -> IdentifyEventRecord:
        return IdentifyEventRecord(
            event_id=int(row["id"]),
            flow=row["flow"],
            expected_speaker_id=row["expected_speaker_id"] or "",
            status=row["status"],
            speaker_id=row["speaker_id"],
            score=float(row["score"]),
            margin=float(row["margin"]),
            passed=bool(row["passed"]),
            notification_attempted=bool(row["notification_attempted"]),
            notification_sent=bool(row["notification_sent"]),
            notification_error=row["notification_error"] or "",
            created_at=row["created_at"],
        )

    def create_speaker(self, speaker_id: str, display_name: str | None = None) -> None:
        with self._connect() as conn:
            conn.execute(
                "INSERT INTO speakers (speaker_id, display_name) VALUES (?, ?)",
                (speaker_id, display_name),
            )

    def ensure_speaker(self, speaker_id: str, display_name: str | None = None) -> None:
        with self._connect() as conn:
            conn.execute(
                "INSERT OR IGNORE INTO speakers (speaker_id, display_name) VALUES (?, ?)",
                (speaker_id, display_name),
            )

    def update_display_name(self, speaker_id: str, display_name: str | None) -> None:
        with self._connect() as conn:
            conn.execute(
                "UPDATE speakers SET display_name = ? WHERE speaker_id = ?",
                (display_name, speaker_id),
            )

    def speaker_exists(self, speaker_id: str) -> bool:
        with self._connect() as conn:
            row = conn.execute("SELECT 1 FROM speakers WHERE speaker_id = ?", (speaker_id,)).fetchone()
            return row is not None

    def get_speaker(self, speaker_id: str) -> Optional[SpeakerRecord]:
        with self._connect() as conn:
            row = conn.execute(
                """
                SELECT s.speaker_id,
                       s.display_name,
                       s.created_at,
                       COALESCE(p.sample_count, 0) AS sample_count,
                       ? AS required_samples,
                       CASE WHEN COALESCE(p.sample_count, 0) >= ? THEN 1 ELSE 0 END AS active,
                       COALESCE((
                           SELECT e.source
                           FROM speaker_embeddings AS e
                           WHERE e.speaker_id = s.speaker_id
                           ORDER BY e.created_at DESC, e.id DESC
                           LIMIT 1
                       ), '') AS last_source,
                       COALESCE((
                           SELECT e.created_at
                           FROM speaker_embeddings AS e
                           WHERE e.speaker_id = s.speaker_id
                           ORDER BY e.created_at DESC, e.id DESC
                           LIMIT 1
                       ), '') AS last_sample_at
                FROM speakers AS s
                LEFT JOIN speaker_profiles AS p ON p.speaker_id = s.speaker_id
                WHERE s.speaker_id = ?
                """,
                (CONFIG.required_samples, CONFIG.required_samples, speaker_id),
            ).fetchone()
            if row is None:
                return None
            return self._speaker_from_row(row)

    def list_speakers(self) -> List[SpeakerRecord]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT s.speaker_id,
                       s.display_name,
                       s.created_at,
                       COALESCE(p.sample_count, 0) AS sample_count,
                       ? AS required_samples,
                       CASE WHEN COALESCE(p.sample_count, 0) >= ? THEN 1 ELSE 0 END AS active,
                       COALESCE((
                           SELECT e.source
                           FROM speaker_embeddings AS e
                           WHERE e.speaker_id = s.speaker_id
                           ORDER BY e.created_at DESC, e.id DESC
                           LIMIT 1
                       ), '') AS last_source,
                       COALESCE((
                           SELECT e.created_at
                           FROM speaker_embeddings AS e
                           WHERE e.speaker_id = s.speaker_id
                           ORDER BY e.created_at DESC, e.id DESC
                           LIMIT 1
                       ), '') AS last_sample_at
                FROM speakers AS s
                LEFT JOIN speaker_profiles AS p ON p.speaker_id = s.speaker_id
                ORDER BY
                    CASE WHEN COALESCE(p.sample_count, 0) >= ? THEN 0 ELSE 1 END ASC,
                    COALESCE((
                        SELECT e.created_at
                        FROM speaker_embeddings AS e
                        WHERE e.speaker_id = s.speaker_id
                        ORDER BY e.created_at DESC, e.id DESC
                        LIMIT 1
                    ), s.created_at) DESC,
                    s.speaker_id ASC
                """,
                (CONFIG.required_samples, CONFIG.required_samples, CONFIG.required_samples),
            ).fetchall()
        return [self._speaker_from_row(row) for row in rows]

    def _speaker_dir(self, speaker_id: str) -> Path:
        return self.data_root / speaker_id

    def add_embedding_sample(
        self,
        speaker_id: str,
        utter_idx: int | None,
        source: str,
        wav_bytes: bytes,
        embedding: List[float],
        backend: str,
    ) -> None:
        speaker_dir = self._speaker_dir(speaker_id)
        speaker_dir.mkdir(parents=True, exist_ok=True)
        sample_label = utter_idx if utter_idx is not None else "x"
        wav_path = speaker_dir / f"{source}_{sample_label}_{uuid4().hex[:8]}.wav"
        wav_path.write_bytes(wav_bytes)

        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO speaker_embeddings (speaker_id, utter_idx, source, wav_path, embedding, backend)
                VALUES (?, ?, ?, ?, ?, ?)
                """,
                (speaker_id, utter_idx, source, str(wav_path), pack_embedding(embedding), backend),
            )

    def list_embeddings(self, speaker_id: str) -> List[List[float]]:
        with self._connect() as conn:
            rows = conn.execute(
                "SELECT embedding FROM speaker_embeddings WHERE speaker_id = ? ORDER BY id ASC",
                (speaker_id,),
            ).fetchall()
        return [unpack_embedding(row["embedding"]) for row in rows]

    def upsert_profile(self, speaker_id: str, centroid: List[float], sample_count: int) -> None:
        with self._connect() as conn:
            conn.execute(
                """
                INSERT INTO speaker_profiles (speaker_id, centroid, sample_count, updated_at)
                VALUES (?, ?, ?, CURRENT_TIMESTAMP)
                ON CONFLICT(speaker_id) DO UPDATE SET
                    centroid = excluded.centroid,
                    sample_count = excluded.sample_count,
                    updated_at = CURRENT_TIMESTAMP
                """,
                (speaker_id, pack_embedding(centroid), sample_count),
            )

    def list_active_profiles(self) -> List[tuple[str, List[float], int]]:
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT speaker_id, centroid, sample_count
                FROM speaker_profiles
                WHERE sample_count >= ?
                ORDER BY speaker_id ASC
                """,
                (CONFIG.required_samples,),
            ).fetchall()
        return [
            (row["speaker_id"], unpack_embedding(row["centroid"]), int(row["sample_count"]))
            for row in rows
        ]

    def add_identify_event(
        self,
        *,
        flow: str,
        expected_speaker_id: str,
        status: str,
        speaker_id: str,
        score: float,
        margin: float,
        passed: bool,
        notification_attempted: bool,
        notification_sent: bool,
        notification_error: str,
    ) -> IdentifyEventRecord:
        with self._connect() as conn:
            cursor = conn.execute(
                """
                INSERT INTO identify_events (
                    flow,
                    expected_speaker_id,
                    status,
                    speaker_id,
                    score,
                    margin,
                    passed,
                    notification_attempted,
                    notification_sent,
                    notification_error
                )
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    flow,
                    expected_speaker_id,
                    status,
                    speaker_id,
                    score,
                    margin,
                    1 if passed else 0,
                    1 if notification_attempted else 0,
                    1 if notification_sent else 0,
                    notification_error,
                ),
            )
            row = conn.execute(
                """
                SELECT id,
                       flow,
                       expected_speaker_id,
                       status,
                       speaker_id,
                       score,
                       margin,
                       passed,
                       notification_attempted,
                       notification_sent,
                       notification_error,
                       created_at
                FROM identify_events
                WHERE id = ?
                """,
                (cursor.lastrowid,),
            ).fetchone()
        assert row is not None
        return self._identify_event_from_row(row)

    def list_identify_events(self, limit: int = 20) -> List[IdentifyEventRecord]:
        safe_limit = max(1, min(int(limit), 200))
        with self._connect() as conn:
            rows = conn.execute(
                """
                SELECT id,
                       flow,
                       expected_speaker_id,
                       status,
                       speaker_id,
                       score,
                       margin,
                       passed,
                       notification_attempted,
                       notification_sent,
                       notification_error,
                       created_at
                FROM identify_events
                ORDER BY created_at DESC, id DESC
                LIMIT ?
                """,
                (safe_limit,),
            ).fetchall()
        return [self._identify_event_from_row(row) for row in rows]

    def delete_speaker(self, speaker_id: str) -> bool:
        with self._connect() as conn:
            deleted = conn.execute("DELETE FROM speakers WHERE speaker_id = ?", (speaker_id,)).rowcount
        speaker_dir = self._speaker_dir(speaker_id)
        if speaker_dir.exists():
            shutil.rmtree(speaker_dir, ignore_errors=True)
        return deleted > 0

    def reset_runtime_data(self) -> RuntimeResetSummary:
        with self._connect() as conn:
            deleted_speakers = int(conn.execute("SELECT COUNT(*) FROM speakers").fetchone()[0])
            deleted_identify_events = int(conn.execute("SELECT COUNT(*) FROM identify_events").fetchone()[0])
            conn.execute("DELETE FROM identify_events")
            conn.execute("DELETE FROM speakers")
            conn.execute("DELETE FROM speaker_profiles")
            conn.execute("DELETE FROM speaker_embeddings")

        if self.data_root.exists():
            shutil.rmtree(self.data_root, ignore_errors=True)
        self.data_root.mkdir(parents=True, exist_ok=True)
        return RuntimeResetSummary(
            deleted_speakers=deleted_speakers,
            deleted_identify_events=deleted_identify_events,
        )

