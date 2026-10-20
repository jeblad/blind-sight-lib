#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Depth-map generation for a single photosphere (blind-sight-lib demo).

See notes.md, "Precomputed depth for photospheres — the plan". Invoked as
a one-shot subprocess from the demo's "Create depth map" menu entry, not
run interactively — stdout carries plain-text progress a tester (or the
app itself) can follow.

One whole-sphere pass at the model's native (small) resolution, no
patch-based refinement yet — deliberately the simplest option first, see
notes.md for why (spatial-hearing precision likely doesn't demand more).

The model's own output is relative/ordinal depth (affine-invariant, no
metric scale) — written out as-is; anchoring it to real-world distance
via ArUco reference markers is a separate, not-yet-built step (see
notes.md's calibration-marker plan).
"""

import argparse
import sys
import time
from pathlib import Path

import numpy as np
import OpenEXR
import Imath
from PIL import Image


def find_device() -> str:
    """CPU unless a working CUDA device is actually present — never
    assumed, see requirements.txt's own reasoning."""
    import torch

    return "cuda" if torch.cuda.is_available() else "cpu"


def depth_model_repo(model_size: str) -> str:
    """"small"/"base"/"large" -> the matching HF repo id — confirmed real,
    all three actually exist (depth-anything/Depth-Anything-V2-{Small,
    Base,Large}-hf), not guessed. The "-Metric-*" variants seen alongside
    these while checking are a different product (absolute-distance
    depth, domain-specific) — not what this script uses, see its own
    module docstring on relative/ordinal depth."""
    return f"depth-anything/Depth-Anything-V2-{model_size.capitalize()}-hf"


def generate_depth(image_path: Path, model_size: str = "small") -> np.ndarray:
    from transformers import pipeline

    device = find_device()
    print(f"[depth] device: {device}", file=sys.stderr)

    repo = depth_model_repo(model_size)
    print(f"[depth] loading {repo}...", file=sys.stderr)
    t0 = time.monotonic()
    depth_pipe = pipeline(
        task="depth-estimation",
        model=repo,
        device=device,
    )
    print(f"[depth] model loaded in {time.monotonic() - t0:.1f}s",
          file=sys.stderr)

    image = Image.open(image_path).convert("RGB")
    print(f"[depth] running inference on {image.size[0]}x{image.size[1]} "
          f"source...", file=sys.stderr)
    t0 = time.monotonic()
    result = depth_pipe(image)
    print(f"[depth] inference done in {time.monotonic() - t0:.1f}s",
          file=sys.stderr)

    # The pipeline already resizes its prediction back up to the source
    # image's own resolution (a PIL "F" mode image, 32-bit float per
    # pixel) — written straight to half-float EXR below, no extra resize
    # here. np.asarray(), not np.array(dtype=...) — the latter tripped a
    # numpy 2.x DeprecationWarning about the copy keyword when converting
    # directly from a PIL image; asarray() + an explicit astype() avoids
    # it.
    depth = np.asarray(result["predicted_depth"]).astype(np.float32)
    return depth


def write_depth_exr(depth: np.ndarray, output_path: Path) -> None:
    height, width = depth.shape
    header = OpenEXR.Header(width, height)
    half_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.HALF))
    header["channels"] = {"Z": half_chan}
    # "Z" — the conventional single-channel depth-pass name in EXR-based
    # VFX pipelines, not an arbitrary choice (so any standard EXR viewer
    # already knows to treat it as depth).
    exr = OpenEXR.OutputFile(str(output_path), header)
    exr.writePixels({"Z": depth.astype(np.float16).tobytes()})
    exr.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Source photosphere image")
    parser.add_argument(
        "--output", type=Path, default=None,
        help="Depth file path (default: <image>.depth.exr next to the "
             "source image)")
    parser.add_argument(
        "--model-size", default="small", choices=["small", "base", "large"],
        help="Depth-Anything-V2 model size — see depth-model-size in "
             "main.cpp's GSettings schema (default: small)")
    args = parser.parse_args()

    if not args.image.is_file():
        print(f"[depth] error: {args.image} not found", file=sys.stderr)
        return 1

    output_path = args.output
    if output_path is None:
        # <name>.<ext> -> <name>.depth.exr, matching the naming
        # convention noted in notes.md (still a default, not yet
        # finalized with John).
        output_path = args.image.parent / (args.image.stem + ".depth.exr")

    depth = generate_depth(args.image, model_size=args.model_size)
    write_depth_exr(depth, output_path)
    print(f"[depth] wrote {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
