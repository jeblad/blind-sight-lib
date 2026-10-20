#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Transcription for a single recorded audio segment (blind-sight-lib demo).

See notes.md, "Transcript generation via WhisperX". Invoked as a one-shot
subprocess right after a segment's audio settles — chained after the
optional Opus encode, not parallel to it, so it always reads whichever
file actually survived (see maybe_transcribe_segment() in main.cpp).

Speaker diarization (John, 2026-09-26, see notes.md "Diarization setup: a
keyring-based HuggingFace token") is optional, via --diarize — off unless
the caller passes it. It's the one part of whisperx that needs a gated
HuggingFace model plus a manual license acceptance, so the token itself
is never handled here directly: the caller (main.cpp) reads it from GNOME
Keyring and passes it in as the HF_TOKEN environment variable only for
this one process's lifetime.

Exit codes: 0 = transcript written (with or without diarization). 1 =
transcription itself failed, no transcript written. 2 = transcription
succeeded but diarization specifically failed (missing/invalid/expired
token, or pyannote access revoked) — the transcript is still written,
just without speaker labels; the caller shows a setup/retry dialog on
this code specifically, distinct from a hard failure.

NOT YET LIVE-TESTED against a real recording (unlike generate_depth.py,
which was verified end-to-end against a real photosphere) — written
against WhisperX's documented API, but flag any failure back rather than
assuming this is correct on the first real run.
"""

import argparse
import os
import sys
import time
from pathlib import Path


# "small" — a deliberate speed/accuracy tradeoff, not the more accurate
# but much slower large-v2/v3 WhisperX itself defaults to in many
# examples. Matters most on CPU (no guarantee of a real GPU like this
# project's own dev machine has, see setup-transcribe-env.sh's own
# reasoning), and this app's own audio is short spoken dialogue, not a
# demanding transcription target — "small" is a reasonable starting
# point, easy to change here if it turns out to be the wrong tradeoff.
MODEL_NAME = "small"

# Gated on HuggingFace, along with two more repos pulled in internally
# by the pipeline itself: pyannote/segmentation-3.0, and — found only by
# actually running this, 2026-09-26 — pyannote/speaker-diarization-
# community-1 (a PLDA component the pipeline fetches at load time, not
# mentioned in pyannote's own top-level docs). Accepting only some of the
# three is a well-documented gotcha in other projects (setup docs
# pointing at the wrong repo), so the app's own setup dialog links all
# three explicitly. See notes.md.
DIARIZATION_MODEL = "pyannote/speaker-diarization-3.1"


def find_device_and_compute_type() -> tuple[str, str]:
    """CPU (int8) unless a working CUDA device is actually present —
    never assumed, same reasoning as generate_depth.py's find_device()."""
    import torch

    if torch.cuda.is_available():
        return "cuda", "float16"
    return "cpu", "int8"


def format_timestamp(seconds: float) -> str:
    """WebVTT timestamp: HH:MM:SS.mmm."""
    hours = int(seconds // 3600)
    minutes = int((seconds % 3600) // 60)
    secs = seconds % 60
    return f"{hours:02d}:{minutes:02d}:{secs:06.3f}"


def write_vtt(segments: list[dict], output_path: Path) -> None:
    """Plain WebVTT, or with <v Speaker N> voice tags when a segment
    carries a "speaker" field (diarization ran successfully) — the
    format this file always anticipated, per the previous version of
    this comment, now actually used."""
    with open(output_path, "w", encoding="utf-8") as f:
        f.write("WEBVTT\n\n")
        for segment in segments:
            start = format_timestamp(segment["start"])
            end = format_timestamp(segment["end"])
            text = segment["text"].strip()
            speaker = segment.get("speaker")
            if speaker:
                # WebVTT voice tag — "SPEAKER_00" (pyannote's own raw
                # label) is kept as-is rather than remapped to "Speaker 1"
                # here; a friendlier per-session numbering, if wanted, is
                # a Playback-side display concern, not a transcript-
                # generation one.
                f.write(f"{start} --> {end}\n<v {speaker}>{text}</v>\n\n")
            else:
                f.write(f"{start} --> {end}\n{text}\n\n")


def transcribe(audio_path: Path, model_size: str = MODEL_NAME) -> tuple[list[dict], str, object]:
    """Returns (segments, language, loaded_audio) — the audio array is
    returned alongside so diarize() can reuse it without decoding the
    file a second time. `model_size` defaults to MODEL_NAME ("small") but
    is caller-overridable — John, 2026-09-30: "brukeren skal imidlertid
    kunne be om en større modell" (see kTranscriptionModelSizeKey in
    main.cpp's schema comment)."""
    import whisperx

    device, compute_type = find_device_and_compute_type()
    print(f"[transcribe] device: {device} ({compute_type})", file=sys.stderr)

    print(f"[transcribe] loading whisperx model \"{model_size}\"...",
          file=sys.stderr)
    t0 = time.monotonic()
    model = whisperx.load_model(model_size, device, compute_type=compute_type)
    print(f"[transcribe] model loaded in {time.monotonic() - t0:.1f}s",
          file=sys.stderr)

    audio = whisperx.load_audio(str(audio_path))

    print("[transcribe] running transcription...", file=sys.stderr)
    t0 = time.monotonic()
    result = model.transcribe(audio, batch_size=16)
    print(f"[transcribe] transcription done in {time.monotonic() - t0:.1f}s "
          f"(language: {result['language']})", file=sys.stderr)

    # Freed as soon as transcription is done, not held onto until the
    # function returns — real bug, found live 2026-09-26: on a small GPU
    # (5.66GB here), the whisper model + alignment model (3.85GB by
    # itself) + diarization pipeline together don't fit at once, and
    # diarize() runs later in this same process. "CUDA out of memory.
    # Tried to allocate 312.00 MiB... 64.56 MiB is free." A documented
    # WhisperX pattern for exactly this (its own README recommends the
    # same del/gc.collect()/empty_cache() sequence between stages on
    # constrained GPUs).
    del model
    if device == "cuda":
        import gc
        import torch
        gc.collect()
        torch.cuda.empty_cache()

    # Word-level alignment — a second, language-specific model
    # (wav2vec2, via torchaudio pipelines or HuggingFace, no token
    # needed), giving each word its own timestamp rather than just each
    # segment. Also a prerequisite for whisperx.assign_word_speakers()
    # below, which matches diarization turns against word-level
    # timestamps, not just segment-level ones.
    #
    # Not every language whisper itself can transcribe has an alignment
    # model — whisperx.alignment's own DEFAULT_ALIGN_MODELS_TORCH/_HF
    # dicts cover 38 of the 99 (John, 2026-09-28: languages preferences
    # lets the user pre-cache). load_align_model() raises ValueError for
    # anything outside those two dicts, so membership is checked first
    # rather than relying on an exception — transcription still works for
    # every language either way, just without word-level alignment (and
    # therefore without diarization's own speaker assignment, which needs
    # it) when there's no model for it.
    from whisperx.alignment import DEFAULT_ALIGN_MODELS_HF, DEFAULT_ALIGN_MODELS_TORCH
    language = result["language"]
    if language in DEFAULT_ALIGN_MODELS_TORCH or language in DEFAULT_ALIGN_MODELS_HF:
        print("[transcribe] aligning...", file=sys.stderr)
        t0 = time.monotonic()
        model_a, metadata = whisperx.load_align_model(
            language_code=language, device=device)
        result = whisperx.align(result["segments"], model_a, metadata, audio,
                                 device, return_char_alignments=False)
        print(f"[transcribe] alignment done in {time.monotonic() - t0:.1f}s",
              file=sys.stderr)

        # Same reasoning as freeing `model` above — the alignment model
        # isn't needed once alignment is done, and diarize() (if it runs
        # at all) needs the GPU memory back.
        del model_a
        if device == "cuda":
            import gc
            import torch
            gc.collect()
            torch.cuda.empty_cache()
    else:
        print(f"[transcribe] no alignment model for language \"{language}\" "
              f"— skipping alignment, timestamps stay segment-level",
              file=sys.stderr)

    return result["segments"], device, audio


def load_diarization_pipeline(device: str, hf_token: str):
    """Just the model load/download, no audio involved — factored out of
    diarize() so prefetch_diarization_model.py can force the download
    ahead of time (John, 2026-09-26: "Vi skal ikke vente. Da oppstår det
    et avbrudd under testing og det er uheldig" — the same "run this well
    ahead of time" reasoning as prefetch_models.py for the transcription
    model itself, now applied here too). Raises on any failure, same
    reasoning as diarize() below.

    Explicit submodule import, not attribute access off `whisperx`
    itself — two real, live-tested guesses both failed first:
    `whisperx.diarize.DiarizationPipeline` ("no attribute 'diarize'")
    and `whisperx.DiarizationPipeline` ("no attribute
    'DiarizationPipeline'") — this installed version doesn't re-export it
    at the top level at all, whatever some docs/examples elsewhere show.
    `from whisperx.diarize import DiarizationPipeline` doesn't depend on
    whisperx's own `__init__.py` re-exporting anything; it imports the
    submodule directly regardless. Constructor param is `token`, not
    `use_auth_token` (also live-tested wrong first — confirmed via
    `inspect.signature()` against the actual installed class rather than
    guessing a third time: `(self, model_name=None, token=None,
    device=..., cache_dir=None)`)."""
    from whisperx.diarize import DiarizationPipeline

    print(f"[transcribe] loading diarization model \"{DIARIZATION_MODEL}\"...",
          file=sys.stderr)
    t0 = time.monotonic()
    diarize_model = DiarizationPipeline(
        model_name=DIARIZATION_MODEL, token=hf_token, device=device)
    print(f"[transcribe] diarization model loaded in "
          f"{time.monotonic() - t0:.1f}s", file=sys.stderr)
    return diarize_model


def diarize(segments: list[dict], audio, device: str, hf_token: str,
            max_speakers: int | None = None) -> list[dict]:
    """Runs pyannote's diarization pipeline (via whisperx's own thin
    wrapper) and assigns a "speaker" label to each already-transcribed
    segment. Raises on any failure — missing/invalid/expired token,
    revoked model access, or anything else pyannote/HuggingFace itself
    raises — left uncaught deliberately so main() can tell "transcription
    is fine, diarization specifically isn't" apart from any other
    failure and report it via the dedicated exit code, rather than
    guessing at the exception's meaning here.

    `max_speakers`, when known (see speaker_count_hint() in main.cpp) —
    John, 2026-09-26, found live: without any hint, pyannote has to guess
    the number of speakers purely from clustering, which is exactly where
    it's least reliable on short/noisy audio, and can split one real
    speaker's voice into several "speaker n" labels. Not `min_speakers`
    too — someone present isn't guaranteed to have actually spoken in any
    given segment, so only the upper bound is a safe hint."""
    import whisperx  # real bug, found live 2026-09-26: "name 'whisperx'
    # is not defined" — load_diarization_pipeline()'s own local import is
    # scoped to that function only, not this one.

    diarize_model = load_diarization_pipeline(device, hf_token)

    print("[transcribe] running diarization...", file=sys.stderr)
    t0 = time.monotonic()
    if max_speakers is not None:
        diarize_segments = diarize_model(audio, max_speakers=max_speakers)
    else:
        diarize_segments = diarize_model(audio)
    result = whisperx.assign_word_speakers(diarize_segments, {"segments": segments})
    print(f"[transcribe] diarization done in {time.monotonic() - t0:.1f}s",
          file=sys.stderr)
    return result["segments"]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("audio", type=Path,
                         help="Source audio file (WAV or Opus)")
    parser.add_argument(
        "--output", type=Path, default=None,
        help="Transcript file path (default: <audio>.vtt next to the "
             "source audio, extension stripped)")
    parser.add_argument(
        "--diarize", action="store_true",
        help="Also label speakers, via pyannote — needs HF_TOKEN set in "
             "the environment (see notes.md, \"Diarization setup\")")
    parser.add_argument(
        "--max-speakers", type=int, default=None,
        help="Upper bound on the number of distinct speakers, if known "
             "(see speaker_count_hint() in main.cpp) — only used with "
             "--diarize")
    parser.add_argument(
        "--model-size", default=MODEL_NAME,
        help="Whisper model size (small/medium/large-v3/…) — see "
             "transcription-model-size in main.cpp's GSettings schema "
             f"(default: {MODEL_NAME})")
    args = parser.parse_args()

    if not args.audio.is_file():
        print(f"[transcribe] error: {args.audio} not found", file=sys.stderr)
        return 1

    output_path = args.output
    if output_path is None:
        output_path = args.audio.parent / (args.audio.stem + ".vtt")

    segments, device, audio = transcribe(args.audio, model_size=args.model_size)

    diarization_failed = False
    if args.diarize:
        hf_token = os.environ.get("HF_TOKEN")
        if not hf_token:
            print("[transcribe] diarization requested but HF_TOKEN is not "
                  "set in the environment", file=sys.stderr)
            diarization_failed = True
        else:
            try:
                segments = diarize(segments, audio, device, hf_token,
                                    max_speakers=args.max_speakers)
            except Exception as e:
                print(f"[transcribe] diarization failed: {e}", file=sys.stderr)
                diarization_failed = True

    write_vtt(segments, output_path)
    print(f"[transcribe] wrote {output_path}")
    return 2 if diarization_failed else 0


if __name__ == "__main__":
    sys.exit(main())
