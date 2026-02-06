#!/usr/bin/env python3
import argparse
import os
import sys

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--model', default='openai/clip-vit-base-patch32', help='HF model id')
    p.add_argument('--out', default='models/clip-image-vitb32.onnx', help='Output ONNX path')
    p.add_argument('--opset', type=int, default=17)
    args = p.parse_args()

    try:
        import torch
        from transformers import CLIPModel, CLIPImageProcessor
    except Exception as e:
        print('ERROR: Missing deps. Install torch and transformers.', file=sys.stderr)
        return 2

    device = 'cpu'
    print(f'Loading model {args.model}...')
    model = CLIPModel.from_pretrained(args.model)
    model = model.to(device)
    model.eval()
    proc = CLIPImageProcessor.from_pretrained(args.model)

    import PIL.Image as Image
    import numpy as np

    dummy = Image.new('RGB', (224, 224), (128, 128, 128))
    inputs = proc(images=dummy, return_tensors='pt')
    pixel_values = inputs['pixel_values']  # [1,3,224,224], float32

    class ImageEncoder(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.m = m
        def forward(self, pixel_values):
            # Returns normalized image embeddings [batch, hidden]
            return self.m.get_image_features(pixel_values=pixel_values)

    wrapper = ImageEncoder(model)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    print(f'Exporting to {args.out} (opset={args.opset}) using dynamo_export...')
    try:
        from torch.onnx import dynamo_export
        exported = dynamo_export(wrapper, pixel_values)
        exported.save(args.out)
    except Exception as e:
        print(f'dynamo_export failed: {e}; falling back to torch.onnx.export', file=sys.stderr)
        torch.onnx.export(
            wrapper,
            (pixel_values,),
            args.out,
            input_names=['pixel_values'],
            output_names=['pooled_output'],
            dynamic_axes={'pixel_values': {0: 'batch'}, 'pooled_output': {0: 'batch'}},
            opset_version=args.opset,
            do_constant_folding=True,
        )
    print('Done.')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())


