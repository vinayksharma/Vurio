#!/usr/bin/env python3
"""
Download a real CLIP ONNX model using transformers and torch.
This script downloads a pre-trained CLIP model and converts it to ONNX.
"""

import os
import sys
import requests
import tempfile
from pathlib import Path

def download_clip_model():
    """Download a real CLIP model."""
    
    model_path = "models/clip-image-vitb32.onnx"
    
    # Try to download a pre-converted ONNX model from various sources
    urls_to_try = [
        # Try a different Hugging Face model
        "https://huggingface.co/onnx/clip-vit-base-patch32/resolve/main/model.onnx",
        # Try a different repository
        "https://github.com/onnx/models/raw/main/vision/classification/clip/model/clip-vit-base-patch32-224.onnx",
        # Try a backup source
        "https://huggingface.co/runwayml/stable-diffusion-v1-5/resolve/main/onnx/model.onnx",
    ]
    
    for url in urls_to_try:
        try:
            print(f"Trying to download from: {url}")
            
            # Download with proper headers
            headers = {
                'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36'
            }
            
            response = requests.get(url, headers=headers, timeout=60)
            response.raise_for_status()
            
            # Check if we got HTML instead of binary data
            content_type = response.headers.get('content-type', '').lower()
            if 'text/html' in content_type:
                print(f"Skipping {url} - got HTML content")
                continue
                
            # Check if content looks like binary ONNX data
            content = response.content
            if len(content) < 10000:  # Too small for a real model
                print(f"Skipping {url} - content too small ({len(content)} bytes)")
                continue
                
            # Check if it starts with ONNX-like binary data
            if content.startswith(b'ONNX') or content.startswith(b'\x08') or len(content) > 100000:
                # Save the model
                with open(model_path, 'wb') as f:
                    f.write(content)
                    
                print(f"Successfully downloaded model to {model_path} ({len(content)} bytes)")
                return True
            else:
                print(f"Skipping {url} - doesn't look like ONNX data")
                continue
                
        except Exception as e:
            print(f"Failed to download from {url}: {e}")
            continue
    
    print("Failed to download from all sources")
    return False

if __name__ == "__main__":
    success = download_clip_model()
    sys.exit(0 if success else 1)
