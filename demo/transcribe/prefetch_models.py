#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Downloads (without transcribing anything) the fixed transcription
model plus one language's alignment model.

See notes.md, "Transcription Model dialog gets Dismiss/Next too". Meant
to be run by hand, well ahead of actually testing — John, 2026-09-25:
"folk vil sette opp og laste ned på jobben, før de tester på et annet
sted" — not auto-run by the app the way setup-transcribe-env.sh is
(demo_offer_to_install_transcription_libraries in main.cpp), since the
language to prefetch is a guess (see whisper_language_code_for_ui_locale()
in main.cpp) the user may want to override, and the whole point is to
let this run unattended on a different machine/network than the actual
test.

Language codes match Whisper's/WhisperX's own convention, not strict
ISO 639-1 — notably Norwegian is "no", not "nb"/"nn" (confirmed directly
this session: a Norwegian Bokmål recording was auto-detected as "no" and
resolved to the NbAiLab/nb-wav2vec2-1b-bokmaal-v2 alignment model).
"""

import argparse
import sys
from pathlib import Path

# transcribe.py lives right next to this script, not necessarily on
# sys.path otherwise (invoked with an absolute path from main.cpp).
sys.path.insert(0, str(Path(__file__).resolve().parent))

import transcribe  # noqa: E402  (must follow the sys.path fix above)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("language", help="Whisper language code (e.g. \"no\")")
    parser.add_argument(
        "--model-size", default=transcribe.MODEL_NAME,
        help="Whisper model size (small/medium/large-v3/…) — see "
             "transcription-model-size in main.cpp's GSettings schema "
             f"(default: {transcribe.MODEL_NAME})")
    args = parser.parse_args()

    device, compute_type = transcribe.find_device_and_compute_type()
    print(f"[prefetch] device: {device} ({compute_type})", file=sys.stderr)

    import whisperx

    print(f"[prefetch] downloading transcription model "
          f"\"{args.model_size}\"...", file=sys.stderr)
    whisperx.load_model(args.model_size, device, compute_type=compute_type)

    print(f"[prefetch] downloading alignment model for language "
          f"\"{args.language}\"...", file=sys.stderr)
    whisperx.load_align_model(language_code=args.language, device=device)

    print("[prefetch] done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
