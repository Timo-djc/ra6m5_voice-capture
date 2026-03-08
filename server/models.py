from pydantic import BaseModel


class RecognizeSuccess(BaseModel):
    code: int = 0
    text: str
    confidence: float


class RecognizeError(BaseModel):
    code: int = 1001
    message: str
