# RA6M5 + W800 MVP (Real Socket/TCP/HTTP)

## 1. What this MVP does
- Uses fixed WAV data embedded from `data (1)/data/0/0_1770889392.wav`.
- Configures W800 Wi-Fi by confirmed AT commands.
- Opens TCP socket with `AT+SKCT`, sends HTTP POST with `AT+SKSND`, receives response via `AT+SKSTT` + `AT+SKRCV`.
- Parses HTTP + JSON (`code`, `text`, `confidence`) and prints over UART7 logs.

## 2. Files
- MCU modules: `src/mvp/*`
- Server: `server/*`

## 3. Configure MCU
Edit `src/mvp/config.h`:
- `WIFI_SSID`, `WIFI_PASSWORD`
- `CLOUD_HOST_IP`, `CLOUD_PORT`, `CLOUD_PATH`
- Optional: `CLOUD_HOST_NAME`

## 4. Build and flash
1. Open `A_i2s_capture.uvprojx` in Keil.
2. Build and flash as usual.
3. Open UART7 terminal for logs.

## 5. Run server
```bash
cd server
pip install -r requirements.txt
uvicorn main:app --host 0.0.0.0 --port 8000
```

## 6. Expected flow
- `WIFI_INIT -> WIFI_CONFIG -> WIFI_REBOOT -> WIFI_WAIT_READY -> WIFI_JOIN -> WIFI_CHECK_IP`
- `UPLOAD_START -> UPLOAD_SEND -> UPLOAD_WAIT_RESP -> RESULT_PARSE -> DONE`

## 7. Socket behavior implemented
- Open: `AT+SKCT=0,0,<host>,<remote_port>,<local_port>`
- Send:
  - `AT+SKSND=<socket>,<size>`
  - wait `+OK=<actualsize>`
  - send raw payload (no `>` prompt required)
- Receive:
  - `AT+SKSTT=<socket>` poll `rx_data`
  - `AT+SKRCV=<socket>,<maxsize>`
  - parse `+OK=<size>` then read exact binary bytes
- Close: `AT+SKCLS=<socket>`

## 8. Known conservative defaults
- `SKRPTM` is set to `0` (polling mode).
- HTTP parser currently targets normal `Content-Length` responses.
- `AT+E` explicit set is attempted, fallback to toggle if firmware only supports toggle.
