import os
import random
import shutil
import json
from collections import defaultdict
import time

source_dir = '/Volumes/SD4TB'
dest_dir = os.path.expanduser('~/Pictures/photos')
target_size_bytes = 100 * 1024**3

# Extension stats: ext -> {'count': 0, 'size': 0, 'files': [(path, size), ...]}
stats = defaultdict(lambda: {'count': 0, 'size': 0, 'files': []})

print("Scanning directory...")
start_time = time.time()
def scan_dir(path):
    try:
        with os.scandir(path) as it:
            for entry in it:
                if entry.name.startswith('.'):
                    continue
                if entry.is_dir(follow_symlinks=False):
                    scan_dir(entry.path)
                elif entry.is_file(follow_symlinks=False):
                    try:
                        size = entry.stat().st_size
                        ext = os.path.splitext(entry.name)[1].lower()
                        if not ext:
                            ext = 'no_extension'
                        stats[ext]['count'] += 1
                        stats[ext]['size'] += size
                        stats[ext]['files'].append((entry.path, size))
                    except Exception:
                        pass
    except Exception:
        pass

scan_dir(source_dir)
print(f"Scanning completed in {time.time() - start_time:.2f} seconds.")

# Print analysis
analysis = {}
for ext, data in stats.items():
    analysis[ext] = {'count': data['count'], 'size_mb': data['size'] / (1024**2)}

with open('analysis.json', 'w') as f:
    json.dump(analysis, f, indent=2)

print(f"Total file types found: {len(stats)}")

# Select files to copy
for ext in stats:
    random.shuffle(stats[ext]['files'])

selected_files = []
current_size = 0

iters = {ext: iter(stats[ext]['files']) for ext in stats}
active_exts = list(iters.keys())

print("Selecting files...")
while active_exts and current_size < target_size_bytes:
    # Make a copy of active_exts to iterate over
    for ext in list(active_exts):
        try:
            filepath, size = next(iters[ext])
            # Only add file if it doesn't push us way over (allowing a small overshoot is fine but let's be strict)
            if current_size + size <= target_size_bytes:
                selected_files.append((filepath, size))
                current_size += size
            
            if current_size >= target_size_bytes:
                break
        except StopIteration:
            active_exts.remove(ext)
            
    if current_size >= target_size_bytes:
        break

print(f"Selected {len(selected_files)} files, total size: {current_size / (1024**3):.2f} GB")

# Copy files
print("Copying files...")
os.makedirs(dest_dir, exist_ok=True)
copied_count = 0
for filepath, size in selected_files:
    try:
        filename = os.path.basename(filepath)
        dest_path = os.path.join(dest_dir, filename)
        counter = 1
        while os.path.exists(dest_path):
            name, ext = os.path.splitext(filename)
            dest_path = os.path.join(dest_dir, f"{name}_{counter}{ext}")
            counter += 1
            
        shutil.copy2(filepath, dest_path)
        copied_count += 1
        if copied_count % 100 == 0:
            print(f"Copied {copied_count}/{len(selected_files)} files, {copied_count/len(selected_files)*100:.1f}%...")
    except Exception as e:
        print(f"Failed to copy {filepath}: {e}")

print("Done!")
