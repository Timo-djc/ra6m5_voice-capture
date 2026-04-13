import argparse
import audioop
import hashlib
import math
import sys
import textwrap
import wave
from pathlib import Path


SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2
DEFAULT_SECONDS = 6
DEFAULT_ENROLL_COUNT = 3


def require_sounddevice():
    try:
        import sounddevice as sd  # type: ignore
        import numpy as np  # type: ignore
    except ImportError as exc:  # pragma: no cover
        raise SystemExit("Please install sounddevice and numpy first: pip install sounddevice numpy") from exc
    return sd, np


def record_one(sd, np, seconds: int, sample_rate: int, channels: int, device: str | int | None = None) -> bytes:
    frames = int(seconds * sample_rate)
    audio = sd.rec(frames, samplerate=sample_rate, channels=channels, dtype="int16", device=device)
    sd.wait()
    return np.asarray(audio, dtype=np.int16).tobytes()


def normalize_pcm16(pcm16: bytes, sample_rate: int, channels: int) -> bytes:
    if channels == 2:
        pcm16 = audioop.tomono(pcm16, SAMPLE_WIDTH, 0.5, 0.5)
        channels = 1
    elif channels != 1:
        raise SystemExit(f"unsupported input channel count: {channels}")

    if sample_rate != SAMPLE_RATE:
        pcm16, _ = audioop.ratecv(pcm16, SAMPLE_WIDTH, channels, sample_rate, SAMPLE_RATE, None)

    return pcm16


def pcm_stats(np, pcm16: bytes) -> dict[str, float | int | str]:
    samples = np.frombuffer(pcm16, dtype=np.int16)
    if samples.size == 0:
        return {"sha1": hashlib.sha1(pcm16).hexdigest(), "rms": 0.0, "peak": 0, "nonzero_ratio": 0.0}

    peak = int(np.max(np.abs(samples)))
    rms = float(math.sqrt(float(np.mean(samples.astype(np.float64) ** 2))))
    nonzero_ratio = float(np.count_nonzero(samples)) / float(samples.size)
    return {
        "sha1": hashlib.sha1(pcm16).hexdigest(),
        "rms": rms,
        "peak": peak,
        "nonzero_ratio": nonzero_ratio,
    }


def pcm_to_wav_bytes(pcm16: bytes) -> bytes:
    import io

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(SAMPLE_WIDTH)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm16)
    return buf.getvalue()


def validate_wav(path: Path) -> bytes:
    with wave.open(str(path), "rb") as wf:
        if wf.getnchannels() != CHANNELS:
            raise SystemExit(f"{path}: expected mono")
        if wf.getsampwidth() != SAMPLE_WIDTH:
            raise SystemExit(f"{path}: expected 16-bit PCM")
        if wf.getframerate() != SAMPLE_RATE:
            raise SystemExit(f"{path}: expected 16k sample rate")
        frames = wf.getnframes()
        if frames != SAMPLE_RATE * DEFAULT_SECONDS:
            raise SystemExit(f"{path}: expected {DEFAULT_SECONDS}s exactly")
    return path.read_bytes()


def format_c_array(name: str, data: bytes) -> str:
    lines = []
    for idx in range(0, len(data), 12):
        chunk = data[idx: idx + 12]
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};\n"


def generate_c(output_h: Path, output_c: Path, clips: list[dict]) -> None:
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
    output_h.write_text(header, encoding="ascii")

    arrays = []
    table = []
    for idx, clip in enumerate(clips):
        array_name = f"s_clip_{idx}"
        arrays.append(format_c_array(array_name, clip["wav_bytes"]))
        table.append(
            f'    {{"{clip["name"]}", "{clip["speaker_id"]}", {array_name}, sizeof({array_name}), '
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
    output_c.write_text(source, encoding="ascii")


def main() -> None:
    parser = argparse.ArgumentParser(description="Record 6s speaker clips and generate src/mvp/test_audio_data.[ch]")
    parser.add_argument("--speaker-id", required=True)
    parser.add_argument("--count", type=int, default=5, help="total clip count, first 3 used for enroll")
    parser.add_argument("--seconds", type=int, default=DEFAULT_SECONDS)
    parser.add_argument("--enroll-count", type=int, default=DEFAULT_ENROLL_COUNT)
    parser.add_argument("--device", default=None, help="sounddevice input device name or index")
    parser.add_argument("--list-devices", action="store_true", help="list input devices and exit")
    parser.add_argument("--dataset-dir", default="tmp/speaker_dataset")
    parser.add_argument("--output-c", default="src/mvp/test_audio_data.c")
    parser.add_argument("--output-h", default="src/mvp/test_audio_data.h")
    args = parser.parse_args()

    if args.count < args.enroll_count:
        raise SystemExit("count must be >= enroll-count")
    if args.seconds != DEFAULT_SECONDS:
        raise SystemExit("current MCU demo expects 6-second clips")

    sd, np = require_sounddevice()
    if args.device is not None:
        try:
            args.device = int(args.device)
        except ValueError:
            pass

    if args.list_devices:
        for idx, dev in enumerate(sd.query_devices()):
            if int(dev["max_input_channels"]) > 0:
                print(
                    f"{idx:>3}  {dev['name']} | in={int(dev['max_input_channels'])} "
                    f"default_sr={int(float(dev['default_samplerate']))}"
                )
        return

    dataset_dir = Path(args.dataset_dir) / args.speaker_id
    dataset_dir.mkdir(parents=True, exist_ok=True)

    try:
        default_input = sd.query_devices(args.device, "input")
        print(f"input device: {default_input['name']}")
        device_sample_rate = int(float(default_input["default_samplerate"]))
        device_channels = int(default_input["max_input_channels"])
        if device_channels > 2:
            device_channels = 2
        elif device_channels < 1:
            raise SystemExit("selected device has no input channels")
        print(f"device config: sample_rate={device_sample_rate} channels={device_channels}")
    except Exception:
        print(f"input device: {args.device!r}")
        device_sample_rate = SAMPLE_RATE
        device_channels = CHANNELS

    clips = []
    seen_hashes: dict[str, str] = {}
    for idx in range(args.count):
        role = "enroll" if idx < args.enroll_count else "identify"
        name = f"{idx + 1:02d}_{role}.wav"
        out_path = dataset_dir / name

        print(f"[{idx + 1}/{args.count}] Press Enter to record {args.seconds}s for {role}: {out_path.name}")
        input()
        pcm16 = record_one(sd, np, args.seconds, device_sample_rate, device_channels, device=args.device)
        pcm16 = normalize_pcm16(pcm16, device_sample_rate, device_channels)
        stats = pcm_stats(np, pcm16)
        duplicate_of = seen_hashes.get(stats["sha1"])
        print(
            f"stats: sha1={stats['sha1']} rms={stats['rms']:.1f} "
            f"peak={stats['peak']} nonzero={stats['nonzero_ratio']:.3f}"
        )
        if duplicate_of is not None:
            raise SystemExit(
                f"{out_path.name}: recorded audio is byte-identical to {duplicate_of}. "
                "Check the selected input device or microphone path."
            )
        wav_bytes = pcm_to_wav_bytes(pcm16)
        out_path.write_bytes(wav_bytes)
        validate_wav(out_path)
        seen_hashes[stats["sha1"]] = out_path.name

        clips.append(
            {
                "name": out_path.name,
                "speaker_id": args.speaker_id,
                "wav_bytes": wav_bytes,
                "role": role,
                "utter_idx": (idx + 1) if role == "enroll" else (idx + 1 - args.enroll_count),
                "utter_total": args.enroll_count if role == "enroll" else 0,
            }
        )
        print(f"saved: {out_path}")

    generate_c(Path(args.output_h), Path(args.output_c), clips)
    print(f"generated: {args.output_h}")
    print(f"generated: {args.output_c}")


if __name__ == "__main__":
    main()
