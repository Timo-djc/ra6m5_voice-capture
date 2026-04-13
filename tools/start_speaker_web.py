import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request
import webbrowser
from pathlib import Path
from typing import Any

HOST = "127.0.0.1"
PORT = 8000
ROOT = Path(__file__).resolve().parents[1]
MODELSCOPE_CACHE_DIR = ROOT / "server_data" / "modelscope_cache"
MODELSCOPE_CREDENTIALS_DIR = ROOT / "server_data" / "modelscope_credentials"
MODELSCOPE_MODEL_ID = "iic/speech_eres2net_sv_zh-cn_16k-common"
LOCAL_MODELSCOPE_MODEL_DIR = (
    Path.home()
    / ".cache"
    / "modelscope"
    / "hub"
    / "models"
    / "iic"
    / "speech_eres2net_sv_zh-cn_16k-common"
)


def is_port_open(host: str, port: int, timeout: float = 0.5) -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(timeout)
        return sock.connect_ex((host, port)) == 0


def run_command(command: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(command, capture_output=True, text=True, check=False)


def describe_port_task(port: int) -> dict[str, Any] | None:
    command = [
        "powershell",
        "-NoProfile",
        "-Command",
        (
            "$conn = Get-NetTCPConnection -LocalPort %d -State Listen -ErrorAction SilentlyContinue | Select-Object -First 1; "
            "if ($null -eq $conn) { return }; "
            "$proc = Get-CimInstance Win32_Process -Filter \"ProcessId = $($conn.OwningProcess)\" -ErrorAction SilentlyContinue; "
            "[pscustomobject]@{ "
            "pid = $conn.OwningProcess; "
            "name = if ($proc) { $proc.Name } else { '' }; "
            "command_line = if ($proc) { $proc.CommandLine } else { '' } "
            "} | ConvertTo-Json -Compress"
        )
        % port,
    ]
    result = run_command(command)
    payload = (result.stdout or "").strip()
    if result.returncode != 0 or not payload:
        return None
    try:
        data = json.loads(payload)
    except json.JSONDecodeError:
        return None
    if not isinstance(data, dict):
        return None
    return data


def summarize_port_task(task: dict[str, Any] | None, port: int) -> str:
    if not task:
        return f"Port {port} is free. No listener is running."
    pid = task.get("pid", "?")
    name = task.get("name") or "unknown"
    command_line = (task.get("command_line") or "").strip()
    role = "other process"
    lowered = command_line.lower()
    if "server.main:app" in lowered:
        role = "current speaker web service"
    elif "uvicorn" in lowered:
        role = "uvicorn service"
    elif "python" in name.lower():
        role = "python task"
    if command_line:
        return f"Port {port} is currently owned by PID {pid} ({name}, {role}): {command_line}"
    return f"Port {port} is currently owned by PID {pid} ({name}, {role})."


def stop_port_task(task: dict[str, Any] | None) -> bool:
    if not task:
        return True
    pid = task.get("pid")
    if not pid:
        return False
    result = run_command(["taskkill", "/PID", str(pid), "/T", "/F"])
    return result.returncode == 0


def wait_port_closed(host: str, port: int, timeout_s: float = 10.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not is_port_open(host, port):
            return True
        time.sleep(0.3)
    return False


def start_server() -> subprocess.Popen:
    env = os.environ.copy()
    env.setdefault("SPEAKER_BACKEND", "modelscope")
    default_model = str(LOCAL_MODELSCOPE_MODEL_DIR) if LOCAL_MODELSCOPE_MODEL_DIR.exists() else MODELSCOPE_MODEL_ID
    env.setdefault("SPEAKER_MODEL_ID", default_model)
    env.setdefault("SPEAKER_MODEL_DEVICE", "cpu")
    env.setdefault("SPEAKER_IDENTIFY_THRESHOLD", "0.70")
    env.setdefault("SPEAKER_MIN_RMS", "0.003")
    MODELSCOPE_CACHE_DIR.mkdir(parents=True, exist_ok=True)
    MODELSCOPE_CREDENTIALS_DIR.mkdir(parents=True, exist_ok=True)
    cache_path = str(MODELSCOPE_CACHE_DIR)
    credentials_path = str(MODELSCOPE_CREDENTIALS_DIR)
    env.setdefault("MODELSCOPE_CACHE", cache_path)
    env.setdefault("MODELSCOPE_HOME", cache_path)
    env.setdefault("MODELSCOPE_CREDENTIALS_PATH", credentials_path)
    command = [sys.executable, "-m", "uvicorn", "server.main:app", "--host", HOST, "--port", str(PORT)]
    creationflags = getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
    return subprocess.Popen(command, cwd=str(ROOT), creationflags=creationflags, env=env)


def wait_server(host: str, port: int, timeout_s: float = 20.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if is_port_open(host, port):
            return True
        time.sleep(0.3)
    return False


def fetch_server_status(base_url: str) -> dict[str, Any] | None:
    try:
        with urllib.request.urlopen(f"{base_url.rstrip('/')}/api/system/status", timeout=3.0) as response:
            return json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
        return None


def print_server_status(status: dict[str, Any] | None) -> None:
    if not status:
        print("[speaker] backend status endpoint is unavailable.")
        return
    strategy = status.get("identify_strategy", "--")
    threshold = status.get("identify_threshold")
    threshold_text = f"{float(threshold):.3f}" if isinstance(threshold, (int, float)) else "--"
    print(
        "[speaker] configured={configured} effective={effective} adapter={adapter} strategy={strategy} threshold={threshold}".format(
            configured=status.get("configured_backend", "--"),
            effective=status.get("effective_backend", "--"),
            adapter=status.get("adapter_class", "--"),
            strategy=strategy,
            threshold=threshold_text,
        )
    )
    model_id = status.get("model_id") or ""
    model_revision = status.get("model_revision") or ""
    if model_id:
        revision_text = f" @ {model_revision}" if model_revision else ""
        print(f"[speaker] model={model_id}{revision_text}")
    message = status.get("message") or ""
    if message:
        level = "warning" if status.get("warning") else "note"
        print(f"[speaker] {level}: {message}")


def main() -> int:
    url = f"http://{HOST}:{PORT}/"
    task = describe_port_task(PORT)
    print(summarize_port_task(task, PORT))
    if task:
        print(f"Stopping PID {task.get('pid')} on port {PORT} before restart.")
        if not stop_port_task(task):
            print(f"Failed to stop PID {task.get('pid')} on port {PORT}.")
            return 1
        if not wait_port_closed(HOST, PORT):
            print(f"Port {PORT} is still busy after stop attempt.")
            return 1

    print(f"Starting server.main:app on {url}")
    process = start_server()
    if not wait_server(HOST, PORT):
        print("Server did not start in time.")
        if process.poll() is not None:
            print(f"Server process exited with code {process.returncode}.")
        return 1

    print_server_status(fetch_server_status(url.rstrip("/")))
    print(f"Opening browser: {url}")
    webbrowser.open(url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
