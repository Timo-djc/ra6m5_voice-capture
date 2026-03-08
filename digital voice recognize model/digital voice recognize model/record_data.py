import time
from pathlib import Path
import sounddevice as sd
from scipy.io import wavfile

SAMPLE_RATE = 16000
RECORD_SECONDS = 1.5
BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = BASE_DIR / "data"
DIGITS = [str(i) for i in range(10)]

def record_one_digit(digit_text: str, index: int) -> None:
    save_dir = DATA_DIR / digit_text
    save_dir.mkdir(parents=True, exist_ok=True)

    print(f"\n[{digit_text}] 第 {index} 次，按回车开始录音")
    input()

    audio = sd.rec(
        int(RECORD_SECONDS * SAMPLE_RATE),
        samplerate=SAMPLE_RATE,
        channels=1,
        dtype="int16",
    )
    sd.wait()

    save_path = save_dir / f"{digit_text}_{int(time.time())}.wav"
    wavfile.write(str(save_path), SAMPLE_RATE, audio)
    print(f"已保存: {save_path}")


def main() -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    print("语音数字录制工具")
    print(f"采样率={SAMPLE_RATE}Hz, 时长={RECORD_SECONDS}s")
    print(f"数据目录: {DATA_DIR}")

    while True:
        digit_text = input("\n输入数字(0-9)，输入 q 退出: ").strip()
        if digit_text.lower() == "q":
            break
        if digit_text not in DIGITS:
            print("输入无效，请输入 0~9。")
            continue

        count_text = input("录制次数（建议20~30）: ").strip()
        if not count_text.isdigit():
            print("次数无效。")
            continue

        count = int(count_text)
        for i in range(1, count + 1):
            record_one_digit(digit_text, i)

    print("录制结束。")


if __name__ == "__main__":
    main()
