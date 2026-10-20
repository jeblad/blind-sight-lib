#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Dev-time convenience: sets up a Python venv for depth-map generation
# (John, 2026-09-23 — see notes.md, "Precomputed depth for photospheres
# — the plan"). Run once by a tester before first using the "Create depth
# map" menu entry; not run automatically by anything else in this repo,
# same opt-in philosophy as install-desktop-entry.sh.
#
# CPU-only PyTorch, deliberately: most testers have no discrete NVIDIA
# GPU, or one card doing double duty as both display and compute, unlike
# this project's own dev machine. Whoever has real CUDA can reinstall
# torch afterward from https://pytorch.org/get-started/locally/ — this
# script never assumes it.
#
# Venv lives under $XDG_CACHE_HOME (falling back to ~/.cache per the XDG
# Base Directory spec), not inside this source tree (John, 2026-09-25:
# "Disse er i utviklingskataloger, og vil ikke fungere når noen prøver
# en ordinær install") — it's genuinely per-user, writable, regenerable
# state, same category as pip's own ~/.cache/pip and HuggingFace's own
# ~/.cache/huggingface, which this venv already shares a machine with.
# Must match main.cpp's depth_venv_dir() exactly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/io.github.jeblad.blind-sight-demo/depth-venv"

# The Python OpenEXR bindings (for writing the half-float depth file)
# build a C extension against the system OpenEXR library — the runtime
# lib alone (commonly already present) isn't enough, the -dev headers
# are needed too. Checked explicitly rather than letting pip fail with
# a much less obvious compiler error partway through.
if ! pkg-config --exists OpenEXR 2>/dev/null; then
    echo "error: OpenEXR development headers not found." >&2
    echo "       Install them first, e.g. on Debian/Ubuntu:" >&2
    echo "         sudo apt install libopenexr-dev" >&2
    exit 1
fi

if [ ! -d "$VENV_DIR" ]; then
    python3 -m venv "$VENV_DIR"
fi

# shellcheck source=/dev/null
source "$VENV_DIR/bin/activate"

# CPU wheel index first and separately from requirements.txt — torch
# isn't listed there because a plain `pip install torch` from PyPI can
# still resolve to a much larger CUDA-bundled build depending on
# platform; pinning the index here is what actually guarantees CPU-only.
pip install --upgrade pip
pip install torch --index-url https://download.pytorch.org/whl/cpu
pip install -r "$SCRIPT_DIR/requirements.txt"

echo "Depth generation environment ready: $VENV_DIR"
