from dataclasses import dataclass

@dataclass
class ServerConfig:
    adapter: str = "mock"
    mock_text: str = "test recognition result"
    mock_confidence: float = 0.95


CONFIG = ServerConfig()
