import re
from collections import Counter

proj_file = 'd:/keil_v5/keil/PROJECT/A_i2s_capture/A_i2s_capture.uvprojx'
with open(proj_file, 'r', encoding='utf-8') as f:
    text = f.read()

# Count filenames
files = re.findall(r'<File>[\s\S]*?</File>', text)
file_counts = Counter()
for f in files:
    m = re.search(r'<FileName>(.*?)</FileName>', f)
    if m:
        file_counts[m.group(1)] += 1

duplicates = {name for name, count in file_counts.items() if count > 1}
print('Duplicates:', duplicates)

# For each duplicate, keep only the first occurrence by removing subsequent ones
seen = set()
def keep_first(match):
    block = match.group(0)
    m = re.search(r'<FileName>(.*?)</FileName>', block)
    if m:
        fname = m.group(1)
        if fname in duplicates:
            if fname in seen:
                print(f'Removing duplicate: {fname}')
                return ''
            else:
                seen.add(fname)
    return block

new_text = re.sub(r'<File>[\s\S]*?</File>', keep_first, text)

with open(proj_file, 'w', encoding='utf-8') as f:
    f.write(new_text)

print('Done removing duplicates')
