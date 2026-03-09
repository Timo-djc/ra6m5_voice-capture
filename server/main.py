from fastapi import FastAPI, Header, Request, Response

from asr_adapter import build_adapter
from models import RecognizeError, RecognizeSuccess

app = FastAPI(title="W800 ASR Mock Service", version="1.0.0")
adapter = build_adapter()

def _json_response(payload: str, status_code: int) -> Response:
    return Response(
        content=payload,
        media_type="application/json",
        status_code=status_code,
    )


def _is_wav(data: bytes) -> bool:
    return len(data) >= 12 and data[0:4] == b"RIFF" and data[8:12] == b"WAVE"


@app.post("/asr/recognize")
async def recognize(
    request: Request,
    x_device_id: str = Header(default="", alias="X-Device-Id"),
    x_sample_rate: int = Header(default=16000, alias="X-Sample-Rate"),
    x_format: str = Header(default="wav", alias="X-Format"),
):
    body = await request.body()
    if not body:
        err = RecognizeError(code=1001, message="empty body")
        return _json_response(err.model_dump_json(), 400)

    if x_format.lower() != "wav":
        err = RecognizeError(code=1001, message="X-Format must be wav")
        return _json_response(err.model_dump_json(), 400)

    if not _is_wav(body):
        err = RecognizeError(code=1001, message="invalid wav payload")
        return _json_response(err.model_dump_json(), 400)

    _ = x_device_id

    result = adapter.recognize_wav(body, x_sample_rate)
    if result.code != 0:
        err = RecognizeError(code=result.code, message="asr failed")
        return _json_response(err.model_dump_json(), 500)

    ok = RecognizeSuccess(code=0, text=result.text, confidence=result.confidence)
    return _json_response(ok.model_dump_json(), 200)
