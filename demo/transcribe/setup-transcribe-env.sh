#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Dev-time convenience: sets up a Python venv for automatic transcription
# (John, 2026-09-24 — see notes.md, "Transcript generation via WhisperX").
# Run once by a tester before Record ever needs to transcribe a segment;
# not run automatically by anything else in this repo, same opt-in
# philosophy as setup-depth-env.sh and install-desktop-entry.sh.
#
# Whatever torch build pip resolves for whisperx — deliberately NOT
# pinned to the CPU-only wheel the way setup-depth-env.sh pins its own
# torch (John, 2026-09-24, after that pin turned out not to hold anyway:
# whisperx's own dependency resolution pulled in a CUDA build regardless
# on this machine — confirmed live, real alignment time dropped from
# 67s, dominated by a one-time 3.6GB HuggingFace model download, to 8.9s
# once cached, running on CUDA). Decided to embrace it instead of
# fighting it: use GPU when it's actually there (transcribe.py's own
# find_device_and_compute_type() already falls back to CPU cleanly when
# it isn't), rather than forcing everyone onto the slower path this
# project's own dev machine doesn't need.
#
# No HuggingFace account/token needed for anything this script installs —
# transcribe.py never touches whisperx's speaker-diarization path (that's
# the one part of whisperx that needs a gated HuggingFace model + manual
# license acceptance; explicitly parked as a later, separate option, see
# notes.md).
#
# Venv lives under $XDG_CACHE_HOME (falling back to ~/.cache per the XDG
# Base Directory spec), not inside this source tree — same reasoning as
# setup-depth-env.sh's own venv relocation (John, 2026-09-25). Must match
# main.cpp's transcribe_venv_dir() exactly. Worth knowing before running
# this: with CUDA torch (see above), this venv alone measured ~5.5GB on
# this machine — see README's disk-space note.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/io.github.jeblad.blind-sight-demo/transcribe-venv"

# whisperx loads audio via ffmpeg as an external system binary (same as
# openai-whisper upstream) — not something pip can install, and not
# guaranteed present just because the rest of this app's dependencies
# are. Checked explicitly rather than letting a transcription job fail
# deep inside a subprocess with a much less obvious error.
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "error: ffmpeg not found on PATH." >&2
    echo "       Install it first, e.g. on Debian/Ubuntu:" >&2
    echo "         sudo apt install ffmpeg" >&2
    exit 1
fi

if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

pip install --upgrade pip
pip install -r "$SCRIPT_DIR/requirements.txt"

echo "Transcription environment ready: $VENV_DIR"
