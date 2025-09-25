#!/usr/bin/env python3
import os
from huggingface_hub import list_repo_files, hf_hub_download

REPO = os.environ.get('CLIP_REPO', 'onnx-community/clip-vit-base-patch32')

def pick_file(files):
    # Prefer image/vision encoder ONNX
    candidates = [f for f in files if f.lower().endswith('.onnx')]
    for key in ['image', 'vision', 'vision_model', 'encoder']:
        for f in candidates:
            if key in f.lower():
                return f
    # fallback to top-level model.onnx
    for f in candidates:
        if f.split('/')[-1] == 'model.onnx':
            return f
    return candidates[0] if candidates else None

def main():
    files = list_repo_files(REPO)
    target = pick_file(files)
    if not target:
        raise SystemExit('No ONNX file found in repo: ' + REPO)
    dest_dir = 'models'
    os.makedirs(dest_dir, exist_ok=True)
    out_path = os.path.join(dest_dir, 'clip-image-vitb32.onnx')
    print(f'Downloading {target} from {REPO} -> {out_path}')
    p = hf_hub_download(repo_id=REPO, filename=target, local_dir=dest_dir, local_dir_use_symlinks=False)
    # ensure final name
    if p != out_path:
        try:
            if os.path.exists(out_path):
                os.remove(out_path)
        except Exception:
            pass
        os.rename(p, out_path)
    print(out_path)

if __name__ == '__main__':
    main()




