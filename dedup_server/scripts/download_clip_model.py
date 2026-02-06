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

def download_clip_onnx_model():
    """Download a real CLIP ONNX model."""
    
    # Try to download from a reliable source
    model_urls = [
        "https://github.com/onnx/models/raw/main/vision/classification/clip/model/clip-vit-base-patch32-224.onnx",
        "https://huggingface.co/onnx/clip-vit-base-patch32/resolve/main/model.onnx",
        "https://github.com/microsoft/onnxruntime-extensions/raw/main/test/data/clip-vit-base-patch32.onnx"
    ]
    
    model_path = "models/clip-image-vitb32.onnx"
    
    for url in model_urls:
        try:
            print(f"Trying to download from: {url}")
            response = requests.get(url, timeout=30)
            response.raise_for_status()
            
            # Check if we got HTML instead of binary data
            content_type = response.headers.get('content-type', '').lower()
            if 'text/html' in content_type:
                print(f"Skipping {url} - got HTML content")
                continue
                
            # Check if content looks like binary ONNX data
            content = response.content
            if len(content) < 1000:  # Too small for a real model
                print(f"Skipping {url} - content too small ({len(content)} bytes)")
                continue
                
            # Save the model
            with open(model_path, 'wb') as f:
                f.write(content)
                
            print(f"Successfully downloaded model to {model_path} ({len(content)} bytes)")
            return True
            
        except Exception as e:
            print(f"Failed to download from {url}: {e}")
            continue
    
    print("Failed to download from all sources")
    return False

if __name__ == "__main__":
    success = download_clip_onnx_model()
    sys.exit(0 if success else 1)
