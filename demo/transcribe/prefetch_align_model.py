#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Downloads one language's alignment model, and nothing else.

Leaner than prefetch_models.py (John, 2026-09-28, "Legg til språk" in
Preferences) — that script re-downloads/re-verifies the whisper
transcription model too on every call, which is wasted work here: the
transcription model is a single fixed download already covered by
prefetch_models.py/setup-transcribe-env.sh, independent of which
languages are added afterward. This only ever touches one language's
alignment model per invocation.

Only meant to be called for a language transcribe.py can actually align
(see its own DEFAULT_ALIGN_MODELS_TORCH/_HF membership check) — main.cpp
only offers this for the ~38 languages that have one; passing anything
else fails loudly rather than silently downloading nothing.
"""

import sys
from pathlib import Path

# transcribe.py lives right next to this script, not necessarily on
# sys.path otherwise (invoked with an absolute path from main.cpp).
sys.path.insert(0, str(Path(__file__).resolve().parent))

import transcribe  # noqa: E402  (must follow the sys.path fix above)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <language-code>", file=sys.stderr)
        return 1
    language = sys.argv[1]

    from whisperx.alignment import DEFAULT_ALIGN_MODELS_HF, DEFAULT_ALIGN_MODELS_TORCH
    if language not in DEFAULT_ALIGN_MODELS_TORCH and language not in DEFAULT_ALIGN_MODELS_HF:
        print(f"[prefetch-align] no alignment model exists for language "
              f"\"{language}\"", file=sys.stderr)
        return 1

    device, _compute_type = transcribe.find_device_and_compute_type()
    print(f"[prefetch-align] device: {device}", file=sys.stderr)

    import whisperx

    print(f"[prefetch-align] downloading alignment model for language "
          f"\"{language}\"...", file=sys.stderr)
    whisperx.load_align_model(language_code=language, device=device)

    print("[prefetch-align] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
