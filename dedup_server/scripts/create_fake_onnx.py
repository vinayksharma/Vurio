#!/usr/bin/env python3
"""
Create a fake ONNX model file that ONNX Runtime can at least attempt to load.
This creates a binary file with ONNX-like structure.
"""

import struct
import os

def create_fake_onnx_model():
    """Create a fake ONNX model file."""
    
    model_path = "models/clip-image-vitb32.onnx"
    
    # Create a minimal ONNX-like binary file
    # ONNX files start with a magic number and have a specific structure
    with open(model_path, 'wb') as f:
        # Write a simple header that looks like an ONNX file
        # This is not a real ONNX model but should be parseable enough to not crash
        
        # Write some binary data that ONNX Runtime might accept
        f.write(b'ONNX\x00\x00\x00\x00')  # Simple header
        f.write(b'\x08\x01')  # Some version info
        f.write(b'\x12\x10')  # Length prefix
        f.write(b'minimal_model' + b'\x00' * 4)  # Model name
        f.write(b'\x1a\x08')  # Another length prefix
        f.write(b'graph' + b'\x00' * 4)  # Graph info
        
        # Add some more binary data to make it look like a real model
        for i in range(1000):
            f.write(struct.pack('<f', i * 0.001))  # Write some float values
    
    print(f"Created fake ONNX model at {model_path}")
    return True

if __name__ == "__main__":
    try:
        create_fake_onnx_model()
        print("Success!")
    except Exception as e:
        print(f"Error: {e}")
        exit(1)
