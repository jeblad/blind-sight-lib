#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Downloads (without generating any depth map) the Depth-Anything-V2
model for one chosen size.

Launched directly by main.cpp's own "Generate Depth Maps" Preferences
toggle the moment it's turned on (start_depth_model_prefetch()) — not run
by hand, same shape as demo/transcribe/prefetch_diarization_model.py, not
prefetch_align_model.py/prefetch_models.py (those two stay manual/
copy-paste, since transcription's own setup needs a terminal step
either way). Same "avoid an interruption during testing" reasoning
either way (John, 2026-09-30, "Vi skal ha samme metodikk for å laste ned
modellen for dybdedata... Nedlastingen skal ikke vente til et bilde skal
analyseres, vi skal gjøre prefetch").
"""

import sys
from pathlib import Path

# generate_depth.py lives right next to this script, not necessarily on
# sys.path otherwise (invoked with an absolute path from main.cpp).
sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_depth  # noqa: E402  (must follow the sys.path fix above)


def main() -> int:
    import argparse

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--model-size", default="small", choices=["small", "base", "large"],
        help="Depth-Anything-V2 model size — see depth-model-size in "
             "main.cpp's GSettings schema (default: small)")
    args = parser.parse_args()

    device = generate_depth.find_device()
    print(f"[prefetch-depth] device: {device}", file=sys.stderr)

    from transformers import pipeline

    repo = generate_depth.depth_model_repo(args.model_size)
    print(f"[prefetch-depth] downloading {repo}...", file=sys.stderr)
    pipeline(task="depth-estimation", model=repo, device=device)

    print("[prefetch-depth] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
