#!/usr/bin/env bash
# ensure_models.sh — make sure every model the server loads at runtime is
# really here before the backend is built, downloading any that are missing.
#
#   usage: ensure_models.sh [--check]
#     --check   report only; download nothing; exit 1 if anything is missing.
#
# Why this exists: models/ and extensions/ are gitignored, so a fresh worktree
# has none of them. The CMake build's fallback for a missing CLIP model was to
# write a 99-byte placeholder TEXT file, which produced a server that started
# cleanly and then failed to embed every single image ("Protobuf parsing
# failed") — a broken install that looked healthy. This script runs ahead of
# that build from ./restart -b, and it never writes a placeholder: a model is
# either verified real, or the script fails loudly and the build does not run.
#
# What counts as "real": the file exists, is at least its expected size, and
# starts with the right magic bytes (ONNX protobuf, "GGUF", "{", "#version").
# A placeholder fails all three, so one left behind by an older build is
# replaced.
#
# Sources (all public, pinned by repo/release, verified against the byte
# sizes of the files the existing worktrees were exported from):
#   CLIP ViT-B/32 image + text towers ... Xenova/clip-vit-base-patch32 (ONNX
#       conversion of openai/clip-vit-base-patch32; inputs `pixel_values` /
#       `input_ids`+`attention_mask`, outputs the projected 512-d
#       `image_embeds` / `text_embeds` the adapter looks for). Both towers
#       come from the one checkpoint, so text->image search stays aligned.
#   CLIP tokenizer vocab.json + merges.txt ... openai/clip-vit-base-patch32
#   Face embedder ... InsightFace antelopev2 glintr100.onnx (ArcFace
#       ResNet100, 112x112 in, 512-d out — the model architecture_design.md
#       names). Pulled out of the release zip; the detector models in the
#       same zip are not needed (detection is Apple Vision).
#   Query-structuring LLM ... Qwen/Qwen2.5-1.5B-Instruct-GGUF q4_k_m
#   sqlite-vec KNN extension ... scripts/install_sqlite_vec.sh (pinned there)
#
# VURIO_MODELS_FROM=<dir>: copy from another checkout's models/ directory
# instead of downloading (2 GB is a lot to pull for a third worktree). Only
# files that validate are taken; anything else still downloads.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKEND_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
MODELS_DIR="$BACKEND_DIR/models"
EXT_DIR="$BACKEND_DIR/extensions"

CHECK_ONLY=false
[ "${1:-}" = "--check" ] && CHECK_ONLY=true

HF="https://huggingface.co"
CLIP_ONNX_REPO="$HF/Xenova/clip-vit-base-patch32/resolve/main"
CLIP_TOK_REPO="$HF/openai/clip-vit-base-patch32/resolve/main"
QWEN_REPO="$HF/Qwen/Qwen2.5-1.5B-Instruct-GGUF/resolve/main"
ANTELOPE_ZIP="https://github.com/deepinsight/insightface/releases/download/v0.7/antelopev2.zip"
ANTELOPE_MEMBER="antelopev2/glintr100.onnx"

# name|dest (relative to models/)|min bytes|magic (hex prefix)|sha256 of the download|url
# ONNX protobuf starts with field 1 (ir_version) varint: 0x08. GGUF has its
# own 4-byte magic. Sizes are ~2% under the known-good files so a re-export
# of the same tower still passes while a truncated download does not.
#
# The sha256 is what the pinned source serves (Hugging Face publishes it as
# the LFS x-linked-etag; the face model's is of the member as unzip's CRC
# check extracted it). It is enforced on DOWNLOADS only: a file already in
# models/ that passes size+magic is trusted as-is, because a checkout may
# legitimately carry its own export of a tower (vurio's do), and replacing a
# working model behind someone's back is worse than the gap this closes.
# What it closes: a resumed download whose partial was damaged when the
# first attempt was killed — right size, right magic, garbage in the middle,
# and llama.cpp aborts the whole server on it (GGML_ASSERT in the grammar).
ASSETS=(
  "CLIP image encoder|clip-image-vitb32.onnx|340000000|08|fd6e1402a588279d1723c7534d4bcba5bc0b14b47dfab0e46f8c47b8270d7d40|$CLIP_ONNX_REPO/onnx/vision_model.onnx"
  "CLIP text encoder|clip-text-vitb32.onnx|245000000|08|3f6571f5bad13a97c469c1622e1cfc4d9aef78b79fdbfcff804ca357bfada8cc|$CLIP_ONNX_REPO/onnx/text_model.onnx"
  "CLIP tokenizer vocab|vocab.json|800000|7b|5047b556ce86ccaf6aa22b3ffccfc52d391ea4accdab9c2f2407da5b742d4363|$CLIP_TOK_REPO/vocab.json"
  "CLIP tokenizer merges|merges.txt|500000|2376657273696f6e|f526393189112391ce6f9795d4695f704121ce452c3aad1f5335cc41337eba85|$CLIP_TOK_REPO/merges.txt"
  "Face embedder (ArcFace r100)|face-embed.onnx|250000000|08|4ab1d6435d639628a6f3e5008dd4f929edf4c4124b1a7169e1048f9fef534cdf|zip:$ANTELOPE_ZIP#$ANTELOPE_MEMBER"
  "Query LLM (Qwen2.5 1.5B q4_k_m)|qwen2.5-1.5b-instruct-q4_k_m.gguf|1100000000|47475546|6a1a2eb6d15622bf3c96857206351ba97e1af16c30d7a74ee38970e434e9407e|$QWEN_REPO/qwen2.5-1.5b-instruct-q4_k_m.gguf"
)

# CHAIN-5 (docs/todo_tool_chain.md §5.6): the detected-mask models. Optional:
# a missing one hides its mask source in the catalogue (the server logs it
# once) and never blocks a build — the picture tools work without them.
#   CLIPSeg rd64-refined ... Xenova/clipseg-rd64-refined, the int8 dynamic
#       quantisation of CIDAS/clipseg-rd64-refined (Apache-2.0); inputs
#       input_ids / attention_mask / pixel_values (352², ImageNet mean/std),
#       output logits (352²). Measured within 0.92–1.00 IoU of the fp16
#       export on library photos at ~80 ms vs ~105 ms (CPU).
#   MediaPipe selfie segmentation ... onnx-community/mediapipe_selfie_segmentation
#       (Apache-2.0); input pixel_values (256², [0,1]), output alphas.
#   SlimSAM-77 (CHAIN-5b) ... Xenova/slimsam-77-uniform (Apache-2.0), the
#       ONNX export of the pruned SAM ViT-B; fp32 (the int8 export disagreed
#       with it on 3 of 10 library photos). Encoder: pixel_values (1024²
#       padded, ImageNet mean/std) -> image_embeddings + positional
#       embeddings; decoder: input_points/input_labels (box corners as
#       labels 2/3, clicks as 1) -> pred_masks (3 × 256²) + iou_scores.
OPTIONAL_ASSETS=(
  "Prompt mask model (CLIPSeg int8)|clipseg-rd64-refined-q8.onnx|135000000|08|049010b152839f339d6115e668d560860d0031f7b750b24828c07167ba68812a|$HF/Xenova/clipseg-rd64-refined/resolve/main/onnx/model_quantized.onnx"
  "People mask model (MediaPipe selfie segmentation)|selfie-segmentation.onnx|450000|08|3241ac4ad8aa35bdaf33946776db29f7c283a413aa0b0dacb9483594b4531aad|$HF/onnx-community/mediapipe_selfie_segmentation/resolve/main/onnx/model.onnx"
  "Mask sharpening encoder (SlimSAM-77)|slimsam-encoder.onnx|22500000|08|9f8433273a6750b587779baa0cf5508111001bf7e7acfcf585d370139fd366d0|$HF/Xenova/slimsam-77-uniform/resolve/main/onnx/vision_encoder.onnx"
  "Mask sharpening decoder (SlimSAM-77)|slimsam-decoder.onnx|16000000|08|f4514391764fbd56e08e119060d874ecd7d52994bfb1968af159e12d4943b5bb|$HF/Xenova/slimsam-77-uniform/resolve/main/onnx/prompt_encoder_mask_decoder.onnx"
  # CHAIN-7h: AI finishing at export. NAFNet-SIDD-width32 (megvii, MIT) as
  # exported to ONNX (dynamic H/W, input "image", output "out") in the
  # emulsion release below; Real-ESRGAN x2plus (xinntao, BSD-3) as the
  # SceneWorks ONNX export (input "input", output "output", 2× out).
  "Export denoise model (NAFNet-SIDD-width32)|nafnet-sidd-w32.onnx|110000000|08|5ff072283fe8c86b7c990aa7331cdb26183e20b08961769c70057b82260b133d|https://github.com/AlfredWei/emulsion/releases/download/denoise-model-nafnet-sidd-w32-v1/nafnet_sidd_w32.onnx"
  "Export upscale model (Real-ESRGAN x2plus)|realesrgan-x2.onnx|60000000|08|7115ba92e8a1bfa63d68558ef006ef3d91273a068d321b1439f8bb1c9179002c|$HF/SceneWorks/real-esrgan-onnx/resolve/main/real_esrgan_x2.onnx"
)

file_size() { stat -f%z "$1" 2>/dev/null || stat -c%s "$1" 2>/dev/null || echo 0; }

# A file is real when it is big enough and starts with the right bytes. The
# placeholder ("ONNX placeholder model...") fails both.
is_valid() {
  local path="$1" min="$2" magic="$3"
  [ -f "$path" ] || return 1
  [ "$(file_size "$path")" -ge "$min" ] || return 1
  local n=$(( ${#magic} / 2 ))
  [ "$(head -c "$n" "$path" | xxd -p | tr -d '\n')" = "$magic" ] || return 1
  return 0
}

sha256_of() { shasum -a 256 "$1" | cut -c1-64; }

human() { awk -v b="$1" 'BEGIN{ if (b>=1e9) printf "%.2f GB", b/1e9; else if (b>=1e6) printf "%.0f MB", b/1e6; else printf "%d B", b }'; }

# Download to a .part next to the destination and move into place only once
# it validates, so an interrupted pull never leaves a half file where the
# server would try to load it. -C - resumes a previous .part.
download_to() {
  local url="$1" dest="$2"
  local part="$dest.part"
  mkdir -p "$(dirname "$dest")"
  curl -fL --retry 3 --retry-delay 5 -C - --progress-bar -o "$part" "$url"
  mv -f "$part" "$dest"
}

# The face model ships inside a release zip with four detectors we do not
# use; fetch the zip once and extract just the one member.
download_zip_member() {
  local url="${1%%#*}" member="${1##*#}" dest="$2"
  local tmp; tmp="$(mktemp -d)"
  trap 'rm -rf "$tmp"' RETURN
  curl -fL --retry 3 --retry-delay 5 --progress-bar -o "$tmp/archive.zip" "$url"
  unzip -q -j -o "$tmp/archive.zip" "$member" -d "$tmp"
  mkdir -p "$(dirname "$dest")"
  mv -f "$tmp/$(basename "$member")" "$dest"
}

missing=()
fetched=()
copied=()

echo "[models] checking $MODELS_DIR"
for spec in "${ASSETS[@]}"; do
  IFS='|' read -r name rel min magic sha url <<< "$spec"
  dest="$MODELS_DIR/$rel"

  if is_valid "$dest" "$min" "$magic"; then
    printf '[models]   ok       %-36s %s\n' "$rel" "$(human "$(file_size "$dest")")"
    continue
  fi

  if [ -f "$dest" ]; then
    printf '[models]   INVALID  %-36s %s — placeholder or truncated, will replace\n' "$rel" "$(human "$(file_size "$dest")")"
  else
    printf '[models]   missing  %-36s (%s)\n' "$rel" "$name"
  fi

  if [ "$CHECK_ONLY" = true ]; then
    missing+=("$rel")
    continue
  fi

  # Cheaper than a download when another checkout already has it.
  if [ -n "${VURIO_MODELS_FROM:-}" ] && is_valid "$VURIO_MODELS_FROM/$rel" "$min" "$magic"; then
    echo "[models]   copying  $rel from $VURIO_MODELS_FROM"
    mkdir -p "$MODELS_DIR"
    cp "$VURIO_MODELS_FROM/$rel" "$dest.part" && mv -f "$dest.part" "$dest"
    copied+=("$rel")
  else
    echo "[models]   fetching $rel"
    echo "[models]            $url"
    ok=false
    # Two attempts: the first may resume a .part left by an interrupted
    # run; if that fails the checksum, the partial was damaged — drop it
    # and pull the whole file once more, fresh.
    for attempt in 1 2; do
      if [ "$attempt" = 2 ]; then
        echo "[models]   retrying $rel from scratch (checksum mismatch — resumed partial was damaged)"
        rm -f "$dest" "$dest.part"
      fi
      case "$url" in
        zip:*) download_zip_member "${url#zip:}" "$dest" || continue ;;
        *)     download_to "$url" "$dest" || continue ;;
      esac
      got="$(sha256_of "$dest")"
      if [ "$got" = "$sha" ]; then ok=true; break; fi
      echo "[models]   BAD SUM  $rel sha256 $got, expected $sha" >&2
    done
    if [ "$ok" = false ]; then
      # A network/HTTP error has already been printed by curl -f, or the
      # checksum line above says why. Keep going so the summary lists
      # everything that is missing, not just the first.
      echo "[models]   ERROR    $rel could not be downloaded intact" >&2
      rm -f "$dest"
      missing+=("$rel")
      continue
    fi
    echo "[models]   verified $rel sha256 ok"
    fetched+=("$rel")
  fi

  if ! is_valid "$dest" "$min" "$magic"; then
    echo "[models]   ERROR    $rel downloaded but failed validation ($(human "$(file_size "$dest")"), expected >= $(human "$min"))" >&2
    rm -f "$dest"
    missing+=("$rel")
  fi
done

# sqlite-vec: not a model, but the same shape of problem (gitignored, absent
# in a fresh worktree, server degrades silently without it). The installer
# pins its own release.
optional_missing=()
for spec in "${OPTIONAL_ASSETS[@]}"; do
  IFS='|' read -r name rel min magic sha url <<< "$spec"
  dest="$MODELS_DIR/$rel"
  if is_valid "$dest" "$min" "$magic"; then
    printf '[models]   ok       %-36s %s\n' "$rel" "$(human "$(file_size "$dest")")"
    continue
  fi
  printf '[models]   optional %-36s (%s) not present\n' "$rel" "$name"
  if [ "$CHECK_ONLY" = true ]; then
    optional_missing+=("$rel")
    continue
  fi
  if [ -n "${VURIO_MODELS_FROM:-}" ] && is_valid "$VURIO_MODELS_FROM/$rel" "$min" "$magic"; then
    echo "[models]   copying  $rel from $VURIO_MODELS_FROM"
    cp "$VURIO_MODELS_FROM/$rel" "$dest.part" && mv -f "$dest.part" "$dest"
    copied+=("$rel")
    continue
  fi
  echo "[models]   fetching $rel (optional)"
  echo "[models]            $url"
  if download_to "$url" "$dest" && [ "$(sha256_of "$dest")" = "$sha" ] && is_valid "$dest" "$min" "$magic"; then
    echo "[models]   verified $rel sha256 ok"
    fetched+=("$rel")
  else
    echo "[models]   WARNING  $rel could not be fetched intact; its mask source stays hidden" >&2
    rm -f "$dest" "$dest.part"
    optional_missing+=("$rel")
  fi
done
[ ${#optional_missing[@]} -gt 0 ] && echo "[models] optional, not present: ${optional_missing[*]} (detected masks limited)"

if [ -f "$EXT_DIR/vec0.dylib" ] || [ -f "$EXT_DIR/vec0.so" ]; then
  echo "[models]   ok       extensions/vec0 (sqlite-vec)"
elif [ "$CHECK_ONLY" = true ]; then
  echo "[models]   missing  extensions/vec0 (sqlite-vec KNN index)"
  missing+=("extensions/vec0")
else
  echo "[models]   fetching extensions/vec0 via install_sqlite_vec.sh"
  if ! "$SCRIPT_DIR/install_sqlite_vec.sh" >/dev/null; then
    echo "[models]   ERROR    install_sqlite_vec.sh failed" >&2
    missing+=("extensions/vec0")
  else
    fetched+=("extensions/vec0")
  fi
fi

[ ${#copied[@]}  -gt 0 ] && echo "[models] copied:  ${copied[*]}"
[ ${#fetched[@]} -gt 0 ] && echo "[models] fetched: ${fetched[*]}"

if [ ${#missing[@]} -gt 0 ]; then
  echo "[models] MISSING: ${missing[*]}" >&2
  if [ "$CHECK_ONLY" = true ]; then
    echo "[models] run ./restart -b (or dedup_server/scripts/ensure_models.sh) to download them" >&2
  else
    echo "[models] the backend was NOT built: it would embed nothing without these" >&2
  fi
  exit 1
fi
echo "[models] all runtime models present"
