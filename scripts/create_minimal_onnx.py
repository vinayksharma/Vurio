#!/usr/bin/env python3
"""
Create a minimal valid ONNX model for testing.
This creates a simple model that ONNX Runtime can load without errors.
"""

import numpy as np
import onnx
from onnx import helper, TensorProto
import os

def create_minimal_onnx_model():
    """Create a minimal valid ONNX model."""
    
    # Create a simple model: input -> reshape -> output
    input_tensor = helper.make_tensor_value_info(
        'input', 
        TensorProto.FLOAT, 
        [1, 3, 224, 224]  # Batch, Channels, Height, Width
    )
    
    output_tensor = helper.make_tensor_value_info(
        'output', 
        TensorProto.FLOAT, 
        [1, 512]  # Embedding dimension
    )
    
    # Create a simple reshape operation
    reshape_node = helper.make_node(
        'Reshape',
        inputs=['input'],
        outputs=['output'],
        name='reshape'
    )
    
    # Create the model
    graph = helper.make_graph(
        [reshape_node],
        'minimal_clip_model',
        [input_tensor],
        [output_tensor]
    )
    
    model = helper.make_model(graph)
    model.opset_import[0].version = 11
    
    # Save the model
    model_path = "models/clip-image-vitb32.onnx"
    onnx.save(model, model_path)
    
    print(f"Created minimal ONNX model at {model_path}")
    return True

if __name__ == "__main__":
    try:
        create_minimal_onnx_model()
        print("Success!")
    except Exception as e:
        print(f"Error: {e}")
        exit(1)
