from fastapi import FastAPI, Header, Response

from asr_adapter import build_adapter
from models import RecognizeError, RecognizeSuccess

app = FastAPI(title="W800 ASR Mock Service", version="1.0.0")
adapter = build_adapter()


def _is_wav(data: bytes) -> bool:
    return len(data) >= 12 and data[0:4] == b"RIFF" and data[8:12] == b"WAVE"


@app.post("/asr/recognize")
async def recognize(
    body: bytes,
    x_device_id: str = Header(default="", alias="X-Device-Id"),
    x_sample_rate: int = Header(default=16000, alias="X-Sample-Rate"),
    x_format: str = Header(default="wav", alias="X-Format"),
):
    if not body:
        err = RecognizeError(code=1001, message="empty body")
        return Response(content=err.model_dump_json(), media_type="application/json", status_code=400)

    if x_format.lower() != "wav":
        err = RecognizeError(code=1001, message="X-Format must be wav")
        return Response(content=err.model_dump_json(), media_type="application/json", status_code=400)

    if not _is_wav(body):
        err = RecognizeError(code=1001, message="invalid wav payload")
        return Response(content=err.model_dump_json(), media_type="application/json", status_code=400)

    _ = x_device_id

    result = adapter.recognize_wav(body, x_sample_rate)
    if result.code != 0:
        err = RecognizeError(code=result.code, message="asr failed")
        return Response(content=err.model_dump_json(), media_type="application/json", status_code=500)

    ok = RecognizeSuccess(code=0, text=result.text, confidence=result.confidence)
    return Response(content=ok.model_dump_json(), media_type="application/json", status_code=200)
