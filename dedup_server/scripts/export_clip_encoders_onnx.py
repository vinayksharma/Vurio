#!/usr/bin/env python3
"""Export BOTH CLIP towers (image + text) from one HF checkpoint to ONNX.

The two exports must come from the same checkpoint so their embedding
spaces align (text->image search). Outputs are the PROJECTED 512-dim CLIP
embeddings (visual_projection / text_projection), NOT hidden states.

Usage:
  python export_clip_encoders_onnx.py \
      --model openai/clip-vit-base-patch32 \
      --image-out models/clip-image-vitb32.onnx \
      --text-out models/clip-text-vitb32.onnx \
      --tokenizer-out models
"""
import argparse
import os
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--model', default='openai/clip-vit-base-patch32')
    p.add_argument('--image-out', default='models/clip-image-vitb32.onnx')
    p.add_argument('--text-out', default='models/clip-text-vitb32.onnx')
    p.add_argument('--tokenizer-out', default='models',
                   help='Directory for vocab.json + merges.txt')
    p.add_argument('--opset', type=int, default=17)
    args = p.parse_args()

    try:
        import torch
        from transformers import CLIPModel, CLIPTokenizer
    except Exception:
        print('ERROR: Missing deps. Install torch and transformers.', file=sys.stderr)
        return 2

    print(f'Loading {args.model}...')
    model = CLIPModel.from_pretrained(args.model)
    model.eval()

    # Explicit tower->projection paths: stable across transformers versions
    # (get_image_features/get_text_features have changed return types).
    class ImageEncoder(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.vision_model = m.vision_model
            self.visual_projection = m.visual_projection

        def forward(self, pixel_values):
            out = self.vision_model(pixel_values=pixel_values)
            return self.visual_projection(out.pooler_output)

    class TextEncoder(torch.nn.Module):
        def __init__(self, m):
            super().__init__()
            self.text_model = m.text_model
            self.text_projection = m.text_projection

        def forward(self, input_ids, attention_mask):
            out = self.text_model(input_ids=input_ids, attention_mask=attention_mask)
            return self.text_projection(out.pooler_output)

    os.makedirs(os.path.dirname(args.image_out) or '.', exist_ok=True)
    os.makedirs(os.path.dirname(args.text_out) or '.', exist_ok=True)

    pixel_values = torch.zeros(1, 3, 224, 224, dtype=torch.float32)
    print(f'Exporting image encoder to {args.image_out}...')
    torch.onnx.export(
        ImageEncoder(model), (pixel_values,), args.image_out,
        input_names=['pixel_values'], output_names=['image_embeds'],
        dynamic_axes={'pixel_values': {0: 'batch'}, 'image_embeds': {0: 'batch'}},
        opset_version=args.opset, do_constant_folding=True,
    )

    input_ids = torch.zeros(1, 77, dtype=torch.int64)
    attention_mask = torch.ones(1, 77, dtype=torch.int64)
    print(f'Exporting text encoder to {args.text_out}...')
    torch.onnx.export(
        TextEncoder(model), (input_ids, attention_mask), args.text_out,
        input_names=['input_ids', 'attention_mask'], output_names=['text_embeds'],
        dynamic_axes={'input_ids': {0: 'batch'}, 'attention_mask': {0: 'batch'},
                      'text_embeds': {0: 'batch'}},
        opset_version=args.opset, do_constant_folding=True,
    )

    print(f'Saving tokenizer assets to {args.tokenizer_out}...')
    tok = CLIPTokenizer.from_pretrained(args.model)
    tok.save_vocabulary(args.tokenizer_out)  # writes vocab.json + merges.txt

    # Sanity: ONNX outputs must match the torch modules and each other's space.
    import numpy as np
    import onnxruntime as ort

    rng = np.random.default_rng(0)
    px = rng.standard_normal((1, 3, 224, 224)).astype(np.float32)
    ids = tok(['a photo of a dog'], padding='max_length', max_length=77,
              return_tensors='np')

    with torch.no_grad():
        ref_img = ImageEncoder(model)(torch.from_numpy(px)).numpy()[0]
        ref_txt = TextEncoder(model)(
            torch.from_numpy(ids['input_ids']),
            torch.from_numpy(ids['attention_mask'])).numpy()[0]

    img_sess = ort.InferenceSession(args.image_out, providers=['CPUExecutionProvider'])
    txt_sess = ort.InferenceSession(args.text_out, providers=['CPUExecutionProvider'])
    onnx_img = img_sess.run(None, {'pixel_values': px})[0][0]
    onnx_txt = txt_sess.run(None, {
        'input_ids': ids['input_ids'].astype(np.int64),
        'attention_mask': ids['attention_mask'].astype(np.int64)})[0][0]

    def cos(a, b):
        return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))

    print(f'image parity cos(onnx, torch) = {cos(onnx_img, ref_img):.6f}')
    print(f'text  parity cos(onnx, torch) = {cos(onnx_txt, ref_txt):.6f}')
    assert cos(onnx_img, ref_img) > 0.999, 'image ONNX export mismatch'
    assert cos(onnx_txt, ref_txt) > 0.999, 'text ONNX export mismatch'
    assert onnx_img.shape == (512,) and onnx_txt.shape == (512,)
    print('Done.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
