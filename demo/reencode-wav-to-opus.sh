#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
#
# Maintenance script (John, 2026-09-26): "Tror vi kan trenge et script som
# pakker om wav-filer til Opus. Hvis en får flere hundre testsessions og
# noen har glemt å kode til Opus kan det bli digre kataloger!" — a catch-up
# pass for any session whose WAV segments never got transcoded, e.g. because
# "Transcode Audio to Opus" was off, or an individual segment's automatic
# encode (stop_audio_track_and_encode(), main.cpp) failed and was left as
# WAV. Not run automatically by anything else in this repo; opt-in, by hand.
#
# Uses the exact same GStreamer pipeline as the app's own
# encode_wav_to_opus_async() (main.cpp), so the result is indistinguishable
# from what the app itself would have produced, and Playback's
# resolve_track_audio_path() (which prefers .opus, same base name, over
# .wav) picks it up with no further changes needed.
#
# Default is read-only w.r.t. the original WAVs — a session recording may
# be the only copy of that data, so unlike the app's own paired
# "transcode-audio-to-opus" setting (which deletes the WAV as part of the
# same on/off choice), this script never deletes anything unless --delete
# is passed explicitly.
set -euo pipefail

usage() {
    echo "Usage: $0 [--delete] DIR" >&2
    echo "  DIR      Directory to scan recursively for .wav files," >&2
    echo "           typically a project directory" >&2
    echo "  --delete Remove each .wav after it's successfully encoded" >&2
    echo "           (default: keep both files)" >&2
    exit 1
}

DELETE_WAV=0
SCAN_DIR=""
for arg in "$@"; do
    case "$arg" in
        --delete) DELETE_WAV=1 ;;
        -h|--help) usage ;;
        -*) echo "error: unknown option: $arg" >&2; usage ;;
        *)
            if [ -n "$SCAN_DIR" ]; then
                echo "error: only one directory argument is allowed" >&2
                usage
            fi
            SCAN_DIR="$arg"
            ;;
    esac
done

# Projects live wherever the user created them, so there is no default.
if [ -z "$SCAN_DIR" ]; then
    usage
fi

if [ ! -d "$SCAN_DIR" ]; then
    echo "error: directory does not exist: $SCAN_DIR" >&2
    exit 1
fi

if ! command -v gst-launch-1.0 >/dev/null 2>&1; then
    echo "error: gst-launch-1.0 not found (gstreamer1.0-tools)." >&2
    exit 1
fi
if ! gst-inspect-1.0 opusenc >/dev/null 2>&1 || ! gst-inspect-1.0 oggmux >/dev/null 2>&1; then
    echo "error: opusenc/oggmux not available (gstreamer1.0-plugins-base)." >&2
    exit 1
fi

encoded=0
skipped=0
failed=0

while IFS= read -r -d '' wav_path; do
    opus_path="${wav_path%.wav}.opus"

    if [ -e "$opus_path" ]; then
        skipped=$((skipped + 1))
        continue
    fi

    echo "Encoding: $wav_path"
    if gst-launch-1.0 -q \
        filesrc location="$wav_path" ! wavparse ! audioconvert ! audioresample ! \
        opusenc ! oggmux ! filesink location="$opus_path" \
        >/dev/null 2>&1; then
        encoded=$((encoded + 1))
        if [ "$DELETE_WAV" -eq 1 ]; then
            rm -f "$wav_path"
        fi
    else
        echo "  failed" >&2
        rm -f "$opus_path"
        failed=$((failed + 1))
    fi
done < <(find "$SCAN_DIR" -type f -name '*.wav' -print0)

echo
echo "Encoded: $encoded, already had .opus: $skipped, failed: $failed"
if [ "$failed" -gt 0 ]; then
    exit 1
fi
