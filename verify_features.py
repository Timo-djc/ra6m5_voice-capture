import os
import sys
import numpy as np
import ctypes

# 1. PC python feature extraction
sys.path.append(r'D:\keil_v5\keil\PROJECT\A_i2s_capture\voice_ai\voice_ai')
from train_model import audio_to_feature

# Create 440Hz sine wave (1.5s)
samples = 24000
t = np.linspace(0, 1.5, samples, endpoint=False)
audio = np.sin(2 * np.pi * 440 * t).astype(np.float32)

pc_feat = audio_to_feature(audio).numpy()
print("PC Feat Shape:", pc_feat.shape)
print("PC Feat Frame 0 [0:5]:", pc_feat[0, :5])

# 2. To test C implementation, you'd compile a small C program 
# linking audio_features.c and CMSIS DSP, then call extract_log_mel_features.
# Since we are on the host and it's heavily coupled with ARM math, 
# full C vs Python test is best run on the board itself by printing the array via UART.

print(r"Please run the Keil project on the board to verify. Ensure mel features match.")
