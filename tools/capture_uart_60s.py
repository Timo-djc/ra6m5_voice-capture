#!/usr/bin/env python3
import argparse
import csv
import datetime as dt
import pathlib
import sys
import time

import serial


def default_output_path() -> pathlib.Path:
    timestamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    return pathlib.Path("logs") / f"audio_capture_{timestamp}.log"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Capture A_i2s_capture UART logs for a fixed duration.")
    parser.add_argument("--port", default="COM16", help="Serial port name (default: COM16)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--seconds", type=float, default=60.0, help="Capture duration in seconds (default: 60)")
    parser.add_argument("--out", type=pathlib.Path, default=default_output_path(), help="Output log file path")
    parser.add_argument("--summary-csv", type=pathlib.Path, default=None, help="Optional parsed STAT summary CSV path")
    return parser.parse_args()

def parse_stat_line(line: str):
    if not line.startswith("STAT,"):
        return None

    parts = [segment.strip() for segment in line.split(",")]
    if len(parts) not in (16, 19, 23):
        return None

    try:
        parsed = {
            "t_sec": int(parts[1]),
            "samples": int(parts[2]),
            "fs_est": int(parts[3]),
            "cb_count": int(parts[4]),
            "cb_gap_max_us": int(parts[5]),
            "rms": int(parts[6]),
            "peak": int(parts[7]),
            "clip": int(parts[8]),
            "ovf": int(parts[9]),
            "rst": int(parts[10]),
            "err": int(parts[11]),
        }

        if len(parts) in (19, 23):
            parsed["read_ok"] = int(parts[12])
            parsed["read_err"] = int(parts[13])
            parsed["no_cb_to"] = int(parts[14])
            parsed["sample_ok"] = int(parts[15])
            parsed["cb_ok"] = int(parts[16])
            parsed["error_ok"] = int(parts[17])
            parsed["audio_ok"] = int(parts[18])

            if len(parts) == 23:
                parsed["ws_edge"] = int(parts[19])
                parsed["bck_edge"] = int(parts[20])
                parsed["pin_err"] = int(parts[21])
                parsed["ssi_state"] = int(parts[22])
            else:
                parsed["ws_edge"] = 0
                parsed["bck_edge"] = 0
                parsed["pin_err"] = 0
                parsed["ssi_state"] = 0
        else:
            parsed["read_ok"] = 1
            parsed["read_err"] = 0
            parsed["no_cb_to"] = 0
            parsed["sample_ok"] = int(parts[12])
            parsed["cb_ok"] = int(parts[13])
            parsed["error_ok"] = int(parts[14])
            parsed["audio_ok"] = int(parts[15])
            parsed["ws_edge"] = 0
            parsed["bck_edge"] = 0
            parsed["pin_err"] = 0
            parsed["ssi_state"] = 0

        return parsed
    except ValueError:
        return None


def cobs_decode(data: bytes) -> bytes:
    if not data: return b""
    decoded = bytearray()
    read_index = 0
    while read_index < len(data):
        code = data[read_index]
        if code == 0: break
        read_index += 1
        for _ in range(1, code):
            if read_index < len(data):
                decoded.append(data[read_index])
                read_index += 1
        if code < 0xFF and read_index < len(data):
            decoded.append(0)
    return bytes(decoded)

IMA_INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
IMA_STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
]

def adpcm_decode_sample(nibble: int, state: dict) -> int:
    step = IMA_STEP_TABLE[state["index"]]
    diff = step >> 3
    if nibble & 4: diff += step
    if nibble & 2: diff += (step >> 1)
    if nibble & 1: diff += (step >> 2)
    if nibble & 8:
        state["val"] -= diff
    else:
        state["val"] += diff

    if state["val"] > 32767: state["val"] = 32767
    elif state["val"] < -32768: state["val"] = -32768

    state["index"] += IMA_INDEX_TABLE[nibble]
    if state["index"] < 0: state["index"] = 0
    elif state["index"] > 88: state["index"] = 88

    return state["val"]

def main() -> int:
    args = parse_args()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    if args.summary_csv is not None:
        args.summary_csv.parent.mkdir(parents=True, exist_ok=True)

    print(f"[INFO] Open serial: port={args.port} baud={args.baud} duration={args.seconds}s")
    print(f"[INFO] Log file: {args.out}")

    stats = []
    audio_data_bytes = bytearray()
    line_count = 0
    started = time.time()
    
    interrupted = False
    try:
        with serial.Serial(args.port, args.baud, timeout=0.2) as ser, args.out.open("w", encoding="utf-8", newline="") as fout:
            deadline = started + args.seconds
            
            leftover = b""
            
            while time.time() < deadline:
                chunk = ser.read(ser.in_waiting or 1)
                if not chunk:
                    continue
                    
                data = leftover + chunk
                leftover = b""
                
                # Split commands and binary buffers perfectly separated by x00 COBS terminator
                while len(data) > 0:
                    zero_idx = data.find(0x00)
                    
                    if zero_idx != -1:
                        # Found a zero terminator, meaning a full COBS frame or text separator is there
                        frame = data[:zero_idx]
                        data = data[zero_idx+1:]
                        if not frame: continue # Empty frame
                        
                        decoded = cobs_decode(frame)
                        if not decoded: continue

                        # Is it ADPCM packet? [0xAA, state_val_low, state_val_high, state_idx, payload...]
                        if decoded[0] == 0xAA and len(decoded) == 84:
                            val = int.from_bytes(decoded[1:3], byteorder='little', signed=True)
                            idx = decoded[3]
                            if idx > 88: idx = 88
                            if idx < 0: idx = 0
                            state = {"val": val, "index": idx}
                            
                            for i in range(4, len(decoded)):
                                byte_val = decoded[i]
                                # Low nibble first
                                s1 = adpcm_decode_sample(byte_val & 0x0F, state)
                                s2 = adpcm_decode_sample((byte_val >> 4) & 0x0F, state)
                                
                                audio_data_bytes.extend(s1.to_bytes(2, byteorder='little', signed=True))
                                audio_data_bytes.extend(s2.to_bytes(2, byteorder='little', signed=True))
                            continue
                            
                    # If no COBS packet, try looking for newline for log/STAT texts (assuming C code also sends \r\n texts separated from \x00)
                    # wait, standard logs aren't COBS encoded by our firmware, they are just ASCII texts.
                    nl_idx = data.find(b"\n")
                    if nl_idx != -1 and (zero_idx == -1 or nl_idx < zero_idx):
                        line_bytes = data[:nl_idx+1]
                        data = data[nl_idx+1:]
                        
                        line = line_bytes.decode("utf-8", errors="replace").rstrip("\r\n")
                        if line:
                            fout.write(line + "\n")
                            line_count += 1
                            parsed = parse_stat_line(line)
                            if parsed is not None:
                                stats.append(parsed)
                            
                            if not line.startswith("STAT,"):
                                print(line)
                    elif zero_idx == -1 and nl_idx == -1:
                        leftover = data
                        break

    except KeyboardInterrupt:
        interrupted = True
        print("\n[WARN] Capture interrupted by user (Ctrl+C). Writing partial summary...")
    except serial.SerialException as exc:
        print(f"[ERROR] Serial open/read failed: {exc}")
        return 2
    except OSError as exc:
        print(f"[ERROR] File write failed: {exc}")
        return 3

    elapsed = time.time() - started
    prefix = "[INFO] Capture interrupted" if interrupted else "[INFO] Capture complete"
    print(f"{prefix}: elapsed={elapsed:.2f}s lines={line_count} stat_lines={len(stats)}")

    if stats:
        avg_samples = sum(row["samples"] for row in stats) / len(stats)
        avg_fs = sum(row["fs_est"] for row in stats) / len(stats)
        max_gap = max(row["cb_gap_max_us"] for row in stats)
        error_windows = sum(1 for row in stats if row["error_ok"] == 0 or row["err"] > 0 or row["ovf"] > 0 or row["rst"] > 0)
        audio_ok_windows = sum(1 for row in stats if row["audio_ok"] == 1)

        print("[SUMMARY] avg_samples={:.1f} avg_fs={:.1f} max_cb_gap_us={} error_windows={} audio_ok_windows={}".format(
            avg_samples, avg_fs, max_gap, error_windows, audio_ok_windows
        ))

        if args.summary_csv is not None:
            with args.summary_csv.open("w", encoding="utf-8", newline="") as csvfile:
                writer = csv.DictWriter(csvfile, fieldnames=list(stats[0].keys()))
                writer.writeheader()
                writer.writerows(stats)
            print(f"[INFO] Summary CSV written: {args.summary_csv}")
    else:
        print("[WARN] No parsable STAT lines found.")

    if audio_data_bytes:
        wav_path = args.out.with_suffix(".wav")
        import wave
        with wave.open(str(wav_path), "wb") as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2) # 16-bit = 2 bytes
            wf.setframerate(16000) # Full ADPCM uncompressed 16kHz High Def audio
            wf.writeframes(audio_data_bytes)
        print(f"[INFO] Successfully saved captured audio ({len(audio_data_bytes) // 2} samples) to: {wav_path}")
    else:
        print("[WARN] No audio data captured.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
