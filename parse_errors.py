import re

with open('build_output39.txt', 'r', encoding='utf-8', errors='ignore') as f:
    text = f.read()

text = re.sub(r'(?m)^.*schema_generated.h.*static assertion failed.*$', '', text)

lines = text.split('\n')
errors = []
for line in lines:
    if 'error:' in line.lower() or 'Error: L' in line:
        if 'armclang: error: no input files' not in line:
            errors.append(line.strip())

unique_errors = sorted(list(set(errors)))
with open('parsed_errors.txt', 'w', encoding='utf-8') as f:
    for e in unique_errors:
        f.write(e + '\n')
