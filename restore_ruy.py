import os

search_dir = 'd:/keil_v5/keil/tensorflow/tensorflow-lite-micro/1.25.2/tensorflow/lite/kernels/internal/reference'
target = '// #include "ruy/profiler/instrumentation.h"'
replacement = '#include "ruy/profiler/instrumentation.h"'

count = 0
for root, _, files in os.walk(search_dir):
    for f in files:
        if f.endswith('.h') or f.endswith('.cpp'):
            path = os.path.join(root, f)
            try:
                with open(path, 'r', encoding='utf-8', errors='ignore') as fp:
                    content = fp.read()
                
                if target in content:
                    new_content = content.replace(target, replacement)
                    with open(path, 'w', encoding='utf-8') as fp:
                        fp.write(new_content)
                    print('Restored', path)
                    count += 1
            except Exception as e:
                print('Error processing', path, ':', e)

print(f"Restored {count} files.")
