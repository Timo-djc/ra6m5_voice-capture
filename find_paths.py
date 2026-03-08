import re

proj_file = 'd:/keil_v5/keil/PROJECT/A_i2s_capture/A_i2s_capture.uvprojx'
with open(proj_file, 'r', encoding='utf-8') as f:
    text = f.read()

# Check exactly which error_reporter.cpp path is being used
for match in re.finditer(r'<File>[\s\S]*?error_reporter[\s\S]*?</File>', text):
    print(match.group(0))
    print()
