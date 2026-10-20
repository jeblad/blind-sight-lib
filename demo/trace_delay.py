#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Audio round-trip latency measurement (blind-sight-lib demo).

Invoked as a one-shot subprocess from the demo's "Trace Delay…" hamburger
menu entry. Writes its result as JSON to --output (never stdout — this
project's other subprocess call sites all use Gio::Subprocess::Flags::NONE
with no piped stdout, communicating through a result file instead, same as
transcribe.py's .vtt output).

Method: play a short click to the default (or --sink) output while
recording from the default (or --source) microphone, then find the
click's actual acoustic onset in the recording. Both the trigger and the
onset are measured as offsets into the *same* recording's timeline, so
pw-record's own startup lag is a constant additive bias on both and
cancels out of the subtraction — no wired reference device is needed to
calibrate "sample 0 of the recording = this wall-clock instant."

Runs several trials, not one — a single click can produce a false-positive
onset (something in the room crossed the energy threshold before the
click was even sent, which is physically impossible as a real detection
and is discarded outright).

Stdlib only, deliberately — unlike demo/transcribe/, this never needs a
venv or a "not set up" dialog.
"""

import argparse
import array
import json
import math
import re
import statistics
import subprocess
import sys
import tempfile
import time
import wave
from pathlib import Path

SAMPLE_RATE = 48000
CLICK_FREQ_HZ = 2000
CLICK_DURATION_S = 0.08
SPEED_OF_SOUND_M_S = 343.0

# Same set as check_bt_output.py's own — see that file's comment for why
# these specific codec ids, confirmed against this machine's installed
# spa-bluez5 codec plugins.
LOW_LATENCY_CODECS = {"aptx_ll", "aptx_ll_duplex", "faststream", "faststream_duplex"}

PRE_ROLL_S = 1.5
POST_ROLL_S = 2.5
WINDOW_S = 0.005  # 5ms energy windows — the detection resolution.


def generate_click(path: Path) -> None:
    n_samples = int(SAMPLE_RATE * CLICK_DURATION_S)
    amplitude = 32000
    samples = bytearray()
    for i in range(n_samples):
        t = i / SAMPLE_RATE
        value = int(amplitude * math.sin(2 * math.pi * CLICK_FREQ_HZ * t))
        samples += value.to_bytes(2, byteorder="little", signed=True)
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(SAMPLE_RATE)
        wav.writeframes(bytes(samples))


# Signed array typecode per sample width in bytes.
_ARRAY_TYPECODES = {1: "b", 2: "h", 4: "i"}


def to_mono(frames: bytes, width: int, n_channels: int) -> list[int]:
    samples = array.array(_ARRAY_TYPECODES[width])
    samples.frombytes(frames)
    if sys.byteorder != "little":  # WAV is little-endian
        samples.byteswap()
    if n_channels == 1:
        return samples.tolist()
    return [
        sum(samples[i:i + n_channels]) // n_channels
        for i in range(0, len(samples) - n_channels + 1, n_channels)
    ]


def window_rms(samples: list[int]) -> float:
    return math.sqrt(sum(x * x for x in samples) / len(samples)) if samples else 0.0


def find_onset_offset_s(path: Path) -> float | None:
    with wave.open(str(path), "rb") as wav:
        rate = wav.getframerate()
        width = wav.getsampwidth()
        n_channels = wav.getnchannels()
        frames = wav.readframes(wav.getnframes())
    samples = to_mono(frames, width, n_channels)

    window = max(1, int(rate * WINDOW_S))
    windows_rms = [
        window_rms(samples[offset:offset + window])
        for offset in range(0, len(samples) - window, window)
    ]

    # Baseline from the first ~1s, comfortably inside PRE_ROLL_S so the
    # click itself never pollutes the "silence" estimate.
    baseline_windows = int(1.0 / WINDOW_S)
    baseline = windows_rms[:baseline_windows]
    noise_floor = sum(baseline) / len(baseline) if baseline else 0
    threshold = max(noise_floor * 6, 200)  # absolute floor for a near-silent room

    for i, rms in enumerate(windows_rms):
        if i < baseline_windows:
            continue
        if rms > threshold:
            return i * WINDOW_S
    return None


def run_trial(click_path: Path, capture_path: Path, sink: str | None,
              source: str | None) -> float | None:
    """One play+record+detect trial. Returns the measured latency in
    seconds, or None if no plausible onset was found (including a
    detection that occurred before the trigger — physically impossible,
    discarded rather than reported as a negative latency)."""
    record_cmd = ["pw-record", "--rate", str(SAMPLE_RATE), "--channels", "1"]
    if source:
        record_cmd += ["--target", source]
    record_cmd.append(str(capture_path))

    recording = subprocess.Popen(record_cmd, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
    recording_start = time.monotonic()
    time.sleep(PRE_ROLL_S)

    play_cmd = ["pw-play"]
    if sink:
        play_cmd += ["--target", sink]
    play_cmd.append(str(click_path))

    trigger_wall = time.monotonic()
    subprocess.run(play_cmd, check=True, stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL)
    trigger_offset_s = trigger_wall - recording_start

    time.sleep(POST_ROLL_S)
    recording.send_signal(2)  # SIGINT — pw-record's own clean-stop signal
    recording.wait(timeout=5)

    onset_offset_s = find_onset_offset_s(capture_path)
    if onset_offset_s is None or onset_offset_s < trigger_offset_s:
        return None
    return onset_offset_s - trigger_offset_s


def pipewire_quantum_ms() -> float | None:
    """The graph-wide forced clock.rate/clock.quantum (pw-metadata's
    "settings" store) — the same two numbers pw-top shows as RATE/QUANT
    for whichever node is currently the graph driver. Best-effort: None,
    not a guess, if either key can't be found/parsed."""
    try:
        output = subprocess.run(["pw-metadata", "-n", "settings", "0"],
                                  capture_output=True, text=True, timeout=5,
                                  check=True).stdout
    except (subprocess.SubprocessError, OSError):
        return None
    rate_match = re.search(r"key:'clock\.rate' value:'(\d+)'", output)
    quantum_match = re.search(r"key:'clock\.quantum' value:'(\d+)'", output)
    if not rate_match or not quantum_match:
        return None
    rate = int(rate_match.group(1))
    quantum = int(quantum_match.group(1))
    if rate <= 0:
        return None
    return quantum / rate * 1000


def sink_codec(sink: str | None) -> str:
    """api.bluez5.codec for `sink` (its node.name) if given, else whichever
    sink is currently the system default — "" if that sink isn't
    Bluetooth, or any command/parse step fails. Never fatal: this is
    display context, not something the measurement depends on.

    Resolved via pw-dump, not `wpctl status` + `wpctl inspect` — the
    latter only exposes a numeric id matched against wpctl's own display
    name, with no way to look up a specific node.name directly (which is
    exactly what --sink/--target take). pw-dump's Metadata objects (not
    Node objects — note props sits directly under the object, not under
    "info" the way a Node's does) carry the real default under
    `default.audio.sink`, confirmed by inspecting a live dump rather than
    assumed.
    """
    try:
        objects = json.loads(
            subprocess.run(["pw-dump"], capture_output=True, text=True,
                             timeout=5, check=True).stdout)
    except (subprocess.SubprocessError, OSError, json.JSONDecodeError):
        return ""

    target = sink
    if target is None:
        for obj in objects:
            if obj.get("type") != "PipeWire:Interface:Metadata":
                continue
            if (obj.get("props") or {}).get("metadata.name") != "default":
                continue
            for entry in obj.get("metadata", []):
                if entry.get("key") == "default.audio.sink":
                    target = (entry.get("value") or {}).get("name")
            break
    if target is None:
        return ""

    for obj in objects:
        props = (obj.get("info") or {}).get("props") or {}
        if props.get("media.class") == "Audio/Sink" and props.get("node.name") == target:
            return props.get("api.bluez5.codec") or ""
    return ""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--distance-cm", type=float, required=True)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sink", default=None)
    parser.add_argument("--source", default=None)
    args = parser.parse_args()

    codec = sink_codec(args.sink)
    result = {
        "success": False,
        "error": "",
        "latency_ms": 0.0,
        "latency_min_ms": 0.0,
        "latency_max_ms": 0.0,
        "valid_trials": 0,
        "total_trials": args.trials,
        "sound_travel_ms": args.distance_cm / 100 / SPEED_OF_SOUND_M_S * 1000,
        "corrected_latency_ms": 0.0,
        # -1 means "couldn't be determined" — plain JSON null would need
        # an optional numeric type on the C++ side for no real benefit.
        "pipewire_quantum_ms": pipewire_quantum_ms() or -1.0,
        "codec": codec,
        "low_latency_codec": codec in LOW_LATENCY_CODECS,
    }

    with tempfile.TemporaryDirectory() as tmp_dir:
        click_path = Path(tmp_dir) / "click.wav"
        capture_path = Path(tmp_dir) / "capture.wav"
        generate_click(click_path)

        latencies_s = []
        for _ in range(args.trials):
            latency_s = run_trial(click_path, capture_path, args.sink, args.source)
            if latency_s is not None:
                latencies_s.append(latency_s)

    if not latencies_s:
        result["error"] = (
            f"No valid transient detected across {args.trials} trial(s) — "
            "check microphone placement/gain.")
        args.output.write_text(json.dumps(result))
        return

    latency_ms = statistics.median(latencies_s) * 1000
    result.update({
        "success": True,
        "latency_ms": latency_ms,
        "latency_min_ms": min(latencies_s) * 1000,
        "latency_max_ms": max(latencies_s) * 1000,
        "valid_trials": len(latencies_s),
        "corrected_latency_ms": latency_ms - result["sound_travel_ms"],
    })
    args.output.write_text(json.dumps(result))


if __name__ == "__main__":
    main()
