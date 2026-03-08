"""诊断模型是否崩塌：用各种输入测试输出分布"""
import numpy as np
from pathlib import Path
import tensorflow as tf

model_path = Path(__file__).resolve().parent / "voice_ai" / "voice_ai" / "digit_model_int8.tflite"
interp = tf.lite.Interpreter(model_path=str(model_path))
interp.allocate_tensors()
inp = interp.get_input_details()[0]
out = interp.get_output_details()[0]
in_scale, in_zp = inp['quantization']
out_scale, out_zp = out['quantization']

print(f"Input:  {inp['shape']} {inp['dtype']}  scale={in_scale} zp={in_zp}")
print(f"Output: {out['shape']} {out['dtype']}  scale={out_scale} zp={out_zp}")

def run(label, data):
    interp.set_tensor(inp['index'], data.astype(np.int8))
    interp.invoke()
    r = interp.get_tensor(out['index'])[0]
    probs = (r.astype(np.float32) - out_zp) * out_scale
    digit = int(np.argmax(probs))
    print(f"  [{label:12s}] digit={digit}  conf={probs[digit]:.3f}  probs={[f'{p:.3f}' for p in probs]}")
    return digit

print("\n=== 极端输入测试 ===")
run("全零(zp=-33)", np.zeros(inp['shape'], dtype=np.int8))
run("全zp=-33",     np.full(inp['shape'], -33, dtype=np.int8))
run("全-128",       np.full(inp['shape'], -128, dtype=np.int8))
run("全127",        np.full(inp['shape'], 127, dtype=np.int8))
run("全-55",        np.full(inp['shape'], -55, dtype=np.int8))

print("\n=== 随机输入测试 (模型应输出不同数字) ===")
results = []
for i in range(20):
    d = run(f"随机{i:02d}", np.random.randint(-128, 128, inp['shape']))
    results.append(d)
from collections import Counter
c = Counter(results)
print(f"\n随机输入20次: 输出分布={dict(c)}")
if len(c) == 1:
    print("*** 严重: 模型崩塌! 所有输入都输出同一类别 ***")
elif len(c) <= 3:
    print("*** 警告: 模型输出多样性很低 ***")
else:
    print("OK: 模型对不同输入有不同响应")

# 测试: 模拟不同"特征形状"
print("\n=== 模拟不同频率模式 ===")
shape = inp['shape']  # [1,40,150,1]
for mel_band in [0, 10, 20, 30, 39]:
    data = np.full(shape, in_zp, dtype=np.int8)
    data[0, mel_band, 20:80, 0] = 50  # 在特定mel band有能量
    run(f"mel={mel_band}", data)

print("\n=== 模拟不同时间位置 ===")
for t_start in [0, 30, 60, 90, 120]:
    data = np.full(shape, in_zp, dtype=np.int8)
    data[0, 5:25, t_start:t_start+20, 0] = 50  # 时间窗口有能量
    run(f"t={t_start}", data)
