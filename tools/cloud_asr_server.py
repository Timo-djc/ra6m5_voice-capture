#!/usr/bin/env python3
import argparse
import socket
import threading
from typing import Tuple


def recv_exact(conn: socket.socket, length: int) -> bytes:
    chunks = []
    remaining = length
    while remaining > 0:
        data = conn.recv(remaining)
        if not data:
            raise ConnectionError("client disconnected")
        chunks.append(data)
        remaining -= len(data)
    return b"".join(chunks)


def parse_header(line: str) -> Tuple[int, int, int, int]:
    parts = line.strip().split(",")
    if len(parts) != 9:
        raise ValueError("invalid header field count")
    if parts[0] != "ASR" or parts[1] != "1":
        raise ValueError("invalid header prefix")

    session = int(parts[2])
    slot = int(parts[3])
    samples = int(parts[7])
    payload_bytes = int(parts[8])
    return session, slot, samples, payload_bytes


def fake_digit_from_pcm(pcm: bytes) -> Tuple[int, float]:
    if len(pcm) < 2:
        return -1, 0.0

    sample_count = len(pcm) // 2
    energy = 0
    for i in range(0, len(pcm), 2):
        sample = int.from_bytes(pcm[i : i + 2], byteorder="little", signed=True)
        energy += abs(sample)

    mean = energy / max(sample_count, 1)
    digit = int((mean / 600.0) % 10)
    conf = min(0.99, 0.35 + mean / 10000.0)
    return digit, conf


def handle_client(conn: socket.socket, addr: Tuple[str, int]) -> None:
    print(f"[+] client connected: {addr}")
    try:
        with conn:
            buf = b""
            while True:
                while b"\n" not in buf:
                    chunk = conn.recv(1024)
                    if not chunk:
                        return
                    buf += chunk

                line_raw, buf = buf.split(b"\n", 1)
                line = line_raw.decode("ascii", errors="ignore")

                try:
                    session, slot, samples, payload_bytes = parse_header(line)
                    if payload_bytes != samples * 2:
                        raise ValueError("payload length mismatch")

                    if len(buf) < payload_bytes:
                        need = payload_bytes - len(buf)
                        buf += recv_exact(conn, need)

                    pcm = buf[:payload_bytes]
                    buf = buf[payload_bytes:]

                    digit, conf = fake_digit_from_pcm(pcm)
                    if digit >= 0:
                        reply = f"RESULT,{session},{slot},{digit},{conf:.3f}\n"
                    else:
                        reply = f"ERROR,{session},{slot},MODEL_FAIL\n"
                    conn.sendall(reply.encode("ascii"))
                    print(f"[ASR] session={session} slot={slot} digit={digit} conf={conf:.3f}")
                except Exception as exc:
                    err = f"ERROR,0,0,PARSE_FAIL\n"
                    conn.sendall(err.encode("ascii"))
                    print(f"[!] frame error: {exc}")
    finally:
        print(f"[-] client disconnected: {addr}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Cloud ASR demo server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(4)
        print(f"Cloud ASR demo server listening on {args.host}:{args.port}")
        while True:
            conn, addr = server.accept()
            thread = threading.Thread(target=handle_client, args=(conn, addr), daemon=True)
            thread.start()


if __name__ == "__main__":
    main()
