#!/usr/bin/env python3
"""
Download a real CLIP ONNX model for testing.
This script downloads a pre-trained CLIP model and converts it to ONNX format.
"""

import os
import sys
import requests
import tempfile
from pathlib import Path

def is_real_onnx(path):
    """Return True if path exists and looks like a real ONNX model (not a placeholder)."""
    if not os.path.isfile(path):
        return False
    size = os.path.getsize(path)
    if size < 100_000:
        return False
    with open(path, "rb") as f:
        head = f.read(20)
    return b"ONNX placeholder" not in head


def download_clip_onnx_model():
    """Download a real CLIP ONNX model."""
    model_path = "models/clip-image-vitb32.onnx"
    if is_real_onnx(model_path):
        print(f"Model already exists and looks valid: {model_path} (skipping download)")
        return True

    # Server supports both image-only and full CLIP ONNX (adapter feeds image + optional dummy text
    # inputs and uses the first output = image embeddings). Use the same source as setup_clip_image_encoder.sh
    # so downloaded models match the known-working export (sayantan47 full CLIP ~578MB).
    model_urls = [
        "https://huggingface.co/sayantan47/clip-vit-b32-onnx/resolve/main/onnx/model.onnx",
        "https://github.com/onnx/models/raw/main/vision/classification/clip/model/clip-vit-base-patch32-224.onnx",
        "https://huggingface.co/onnx/clip-vit-base-patch32/resolve/main/model.onnx",
        "https://github.com/microsoft/onnxruntime-extensions/raw/main/test/data/clip-vit-base-patch32.onnx",
    ]
    
    for url in model_urls:
        try:
            print(f"Trying to download from: {url}")
            # Stream download (avoids loading full ~350MB into memory; connect 15s, read 10 min)
            response = requests.get(url, stream=True, timeout=(15, 600))
            response.raise_for_status()
            content_type = response.headers.get('content-type', '').lower()
            if 'text/html' in content_type:
                print(f"Skipping {url} - got HTML content")
                continue
            content_length = response.headers.get('content-length')
            size_hint = int(content_length) if content_length else 0
            if 0 < size_hint < 100_000:
                print(f"Skipping {url} - content too small ({size_hint} bytes)")
                continue
            total = 0
            with open(model_path, 'wb') as f:
                for chunk in response.iter_content(chunk_size=1024 * 1024):
                    if chunk:
                        f.write(chunk)
                        total += len(chunk)
                        if total % (10 * 1024 * 1024) < 1024 * 1024 and total >= 10 * 1024 * 1024:
                            print(f"  ... {total // (1024*1024)} MB")
            if total < 100_000:
                print(f"Skipping {url} - downloaded too small ({total} bytes)")
                try:
                    os.remove(model_path)
                except Exception:
                    pass
                continue
            print(f"Successfully downloaded model to {model_path} ({total} bytes)")
            return True
        except Exception as e:
            print(f"Failed to download from {url}: {e}")
            try:
                if os.path.exists(model_path):
                    os.remove(model_path)
            except Exception:
                pass
            continue
    
    print("Failed to download from all sources")
    return False

if __name__ == "__main__":
    success = download_clip_onnx_model()
    sys.exit(0 if success else 1)
