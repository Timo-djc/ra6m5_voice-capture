#!/usr/bin/env python3
import argparse
import re
import time

import serial


STAT_RE = re.compile(r"^STAT,")


def parse_stat(line: str):
    parts = line.strip().split(",")
    if len(parts) < 23 or parts[0] != "STAT":
        return None
    try:
        return {
            "t": int(parts[1]),
            "samples": int(parts[2]),
            "cb_count": int(parts[4]),
            "ws_edge": int(parts[19]),
            "bck_edge": int(parts[20]),
            "pin_err": int(parts[21]),
        }
    except ValueError:
        return None


def main():
    parser = argparse.ArgumentParser(description="Reset后抓取串口并给出INMP441链路诊断")
    parser.add_argument("--port", default="COM16")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--seconds", type=float, default=15.0)
    args = parser.parse_args()

    print("=" * 64)
    print("准备抓取重启后的串口日志")
    print("=" * 64)
    print(f"串口: {args.port} @ {args.baud}")
    print("请先按回车，然后立刻按开发板 RESET。")
    input("按回车开始...")

    stats = []
    diag_lines = 0

    with serial.Serial(args.port, args.baud, timeout=1) as ser:
        ser.reset_input_buffer()
        start = time.time()
        print(f"\n开始抓取 {args.seconds:.1f}s 日志...\n")
        while time.time() - start < args.seconds:
            raw = ser.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace").rstrip()
            print(line)
            if line.startswith("DIAG:") or line.startswith("DIAG_START:") or line.startswith("INFO:"):
                diag_lines += 1
            if STAT_RE.match(line):
                parsed = parse_stat(line)
                if parsed is not None:
                    stats.append(parsed)

    print("\n" + "-" * 64)
    print(f"DIAG行数: {diag_lines}")
    print(f"STAT行数: {len(stats)}")

    if not stats:
        print("未抓到STAT，先检查串口下载与复位时序。")
        return 1

    avg_samples = sum(s["samples"] for s in stats) / len(stats)
    avg_cb = sum(s["cb_count"] for s in stats) / len(stats)
    avg_ws = sum(s["ws_edge"] for s in stats) / len(stats)
    avg_bck = sum(s["bck_edge"] for s in stats) / len(stats)
    print(f"avg_samples={avg_samples:.1f}, avg_cb={avg_cb:.1f}, avg_ws_edge={avg_ws:.1f}, avg_bck_edge={avg_bck:.1f}")

    if avg_samples <= 0 and avg_cb <= 0:
        print("\n[结论] I2S链路未起流（无样本、无回调）。")
        if avg_ws <= 0 and avg_bck <= 0:
            print("[定位] BCK/WS 无边沿，优先检查硬件连线和使能电平：")
            print("  - INMP441 SCK -> RA6M5 P112")
            print("  - INMP441 WS  -> RA6M5 P113")
            print("  - INMP441 SD  -> RA6M5 P114")
            print("  - INMP441 L/R -> GND（左声道）")
            print("  - INMP441 CHIPEN -> VDD（高电平使能）")
            print("  - VDD=3.3V, GND共地")
        else:
            print("[定位] 有时钟边沿但无回调，优先检查SSI中断链路与RX数据线质量。")
    else:
        print("\n[结论] 链路已起流，可继续做RMS/稳定性验收。")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
