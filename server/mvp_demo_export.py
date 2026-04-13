from pathlib import Path
import shutil
import textwrap

try:
    from .audio_utils import load_wav_bytes, pcm16_to_wav_bytes
except ImportError:  # pragma: no cover
    from audio_utils import load_wav_bytes, pcm16_to_wav_bytes


class MvpDemoExportError(RuntimeError):
    pass


class MvpDemoExporter:
    def __init__(
        self,
        data_root: Path = Path("tmp/web_mvp_dataset"),
        output_c: Path = Path("src/mvp/test_audio_data.c"),
        output_h: Path = Path("src/mvp/test_audio_data.h"),
    ) -> None:
        self.data_root = data_root
        self.output_c = output_c
        self.output_h = output_h
        self.data_root.mkdir(parents=True, exist_ok=True)
        self.output_c.parent.mkdir(parents=True, exist_ok=True)
        self.output_h.parent.mkdir(parents=True, exist_ok=True)

    def _speaker_dir(self, speaker_id: str) -> Path:
        return self.data_root / speaker_id

    def _normalize_wav(self, wav_bytes: bytes) -> bytes:
        normalized = load_wav_bytes(wav_bytes, target_sample_rate=16000)
        return pcm16_to_wav_bytes(normalized.pcm16, normalized.sample_rate)

    def _write_wav(self, path: Path, wav_bytes: bytes) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(self._normalize_wav(wav_bytes))

    def save_register_clips(self, speaker_id: str, wav_files: list[bytes]) -> dict:
        if len(wav_files) < 1:
            raise MvpDemoExportError("register clips are required")

        speaker_dir = self._speaker_dir(speaker_id)
        if speaker_dir.exists():
            shutil.rmtree(speaker_dir, ignore_errors=True)
        speaker_dir.mkdir(parents=True, exist_ok=True)

        for idx, wav_bytes in enumerate(wav_files[:3], start=1):
            self._write_wav(speaker_dir / f"{idx:02d}_enroll.wav", wav_bytes)

        return self._export_dataset(speaker_id)

    def save_identify_clip(self, speaker_id: str, wav_bytes: bytes) -> dict:
        speaker_dir = self._speaker_dir(speaker_id)
        enroll_paths = sorted(speaker_dir.glob("*_enroll.wav"))
        if not enroll_paths:
            raise MvpDemoExportError("register clips must be exported before identify clip")

        self._write_wav(speaker_dir / "04_identify.wav", wav_bytes)
        return self._export_dataset(speaker_id)

    def reset_dataset(self) -> dict:
        if self.data_root.exists():
            shutil.rmtree(self.data_root, ignore_errors=True)
        self.data_root.mkdir(parents=True, exist_ok=True)
        self._generate_c([])
        return {
            "embedded_clip_count": 0,
            "output_c": str(self.output_c),
            "output_h": str(self.output_h),
            "data_root": str(self.data_root),
        }

    @staticmethod
    def _format_c_array(name: str, data: bytes) -> str:
        lines = []
        for idx in range(0, len(data), 12):
            chunk = data[idx: idx + 12]
            lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
        return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"

    def _generate_c(self, clips: list[dict]) -> None:
        header = textwrap.dedent(
            """\
            #ifndef MVP_TEST_AUDIO_DATA_H_
            #define MVP_TEST_AUDIO_DATA_H_

            #include <stddef.h>
            #include <stdint.h>

            typedef enum e_test_audio_role
            {
                TEST_AUDIO_ROLE_ENROLL = 0,
                TEST_AUDIO_ROLE_IDENTIFY = 1
            } test_audio_role_t;

            typedef struct st_test_audio_clip
            {
                const char * name;
                const char * speaker_id;
                const char * expected_speaker_id;
                const uint8_t * wav_data;
                size_t wav_size;
                uint8_t role;
                uint8_t utter_idx;
                uint8_t utter_total;
            } test_audio_clip_t;

            size_t test_audio_clip_count(void);
            const test_audio_clip_t * test_audio_get_clip(size_t index);

            #endif
            """
        )
        self.output_h.write_text(header, encoding="ascii")

        if not clips:
            source = textwrap.dedent(
                """\
                #include "test_audio_data.h"

                static const uint8_t s_empty_clip[] = { 0x00 };
                static const test_audio_clip_t s_test_audio_clips[] =
                {
                    {"", "", "", s_empty_clip, 0, TEST_AUDIO_ROLE_ENROLL, 0, 0},
                };

                size_t test_audio_clip_count(void)
                {
                    return 0;
                }

                const test_audio_clip_t * test_audio_get_clip(size_t index)
                {
                    (void) index;
                    return 0;
                }
                """
            )
            self.output_c.write_text(source, encoding="ascii")
            return

        arrays = []
        table = []
        for idx, clip in enumerate(clips):
            array_name = f"s_clip_{idx}"
            arrays.append(self._format_c_array(array_name, clip["wav_bytes"]))
            table.append(
                f'    {{"{clip["name"]}", "{clip["speaker_id"]}", "{clip["expected_speaker_id"]}", {array_name}, sizeof({array_name}), '
                f'{"TEST_AUDIO_ROLE_ENROLL" if clip["role"] == "enroll" else "TEST_AUDIO_ROLE_IDENTIFY"}, '
                f'{clip["utter_idx"]}, {clip["utter_total"]}}},'
            )

        source = '#include "test_audio_data.h"\n\n'
        source += "\n".join(arrays) + "\n"
        source += "static const test_audio_clip_t s_test_audio_clips[] =\n{\n"
        source += "\n".join(table)
        source += "\n};\n\n"
        source += "size_t test_audio_clip_count(void)\n{\n    return sizeof(s_test_audio_clips) / sizeof(s_test_audio_clips[0]);\n}\n\n"
        source += "const test_audio_clip_t * test_audio_get_clip(size_t index)\n{\n"
        source += "    if (index >= test_audio_clip_count())\n    {\n        return 0;\n    }\n\n"
        source += "    return &s_test_audio_clips[index];\n}\n"
        self.output_c.write_text(source, encoding="ascii")

    def _export_dataset(self, speaker_id: str) -> dict:
        speaker_dir = self._speaker_dir(speaker_id)
        enroll_paths = sorted(speaker_dir.glob("*_enroll.wav"))
        if not enroll_paths:
            raise MvpDemoExportError("no enroll clips found")

        identify_path = speaker_dir / "04_identify.wav"
        identify_present = identify_path.exists()
        if not identify_present:
            identify_path = enroll_paths[-1]

        clips = []
        for idx, wav_path in enumerate(enroll_paths, start=1):
            clips.append(
                {
                    "name": wav_path.name,
                    "speaker_id": speaker_id,
                    "expected_speaker_id": "",
                    "wav_bytes": wav_path.read_bytes(),
                    "role": "enroll",
                    "utter_idx": idx,
                    "utter_total": len(enroll_paths),
                }
            )

        clips.append(
            {
                "name": identify_path.name if identify_present else "04_identify.wav",
                "speaker_id": speaker_id,
                "expected_speaker_id": speaker_id,
                "wav_bytes": identify_path.read_bytes(),
                "role": "identify",
                "utter_idx": 1,
                "utter_total": 0,
            }
        )
        self._generate_c(clips)
        return {
            "speaker_id": speaker_id,
            "enroll_count": len(enroll_paths),
            "identify_present": identify_present,
            "embedded_clip_count": len(clips),
            "output_c": str(self.output_c),
            "output_h": str(self.output_h),
            "data_root": str(self.data_root),
        }
