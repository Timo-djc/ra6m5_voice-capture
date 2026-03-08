import re

proj_file = 'd:/keil_v5/keil/PROJECT/A_i2s_capture/A_i2s_capture.uvprojx'
with open(proj_file, 'r', encoding='utf-8') as f:
    text = f.read()

# Find all File blocks
files = re.findall(r'<File>[\s\S]*?</File>', text)

# Count instances of each filename
from collections import Counter
file_names = []
for f in files:
    m = re.search(r'<FileName>(.*?)</FileName>', f)
    if m:
        file_names.append(m.group(1))

counts = Counter(file_names)
for name, count in counts.most_common():
    if count > 1:
        print(f'DUPLICATE ({count}x): {name}')
