#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Downloads (without diarizing anything) pyannote's gated diarization
model, using an HF access token from the environment.

See notes.md, "Diarization setup: a keyring-based HuggingFace token".
Run automatically by show_diarization_setup_offer()'s "Ok" response in
main.cpp, right after it confirms a token is actually in the keyring —
John, 2026-09-26: "Vi skal ikke vente. Da oppstår det et avbrudd under
testing og det er uheldig" — the same "run this well ahead of time, not
lazily on first real test" reasoning prefetch_models.py already applies
to the transcription model itself, now applied to the diarization model
too. HF_TOKEN is read from the environment, never passed as a command-
line argument (visible in `ps`) or written to a file — the caller sets
it only for this one subprocess's own environment (see
lookup_huggingface_token()/main.cpp).

NOT YET LIVE-TESTED against a real HuggingFace account/token.
"""

import os
import sys
from pathlib import Path

# transcribe.py lives right next to this script, not necessarily on
# sys.path otherwise (invoked with an absolute path from main.cpp).
sys.path.insert(0, str(Path(__file__).resolve().parent))

import transcribe  # noqa: E402  (must follow the sys.path fix above)


def main() -> int:
    hf_token = os.environ.get("HF_TOKEN")
    if not hf_token:
        print("[prefetch] error: HF_TOKEN is not set in the environment",
              file=sys.stderr)
        return 1

    device, _ = transcribe.find_device_and_compute_type()
    print(f"[prefetch] device: {device}", file=sys.stderr)
    transcribe.load_diarization_pipeline(device, hf_token)
    print("[prefetch] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
