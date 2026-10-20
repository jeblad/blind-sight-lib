#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Checks whether the current default audio output is a Bluetooth sink
(blind-sight-lib demo).

Invoked every time Interactive mode is (re-)shown — see main.cpp's
show_interactive_bar() — since the user can connect/disconnect a
Bluetooth output at any point during a run, not just at startup. Cheap: a
single `pw-dump` call, no audio I/O.

Stdlib only, writes its result as JSON to --output, same convention as
every other subprocess this project launches (never piped stdout).
"""

import argparse
import json
import subprocess
from pathlib import Path

# Codec ids (PipeWire's own api.bluez5.codec values — confirmed against
# this machine's installed spa-bluez5 codec plugins via `strings`, not
# guessed) that are specifically engineered for low round-trip latency,
# unlike plain SBC/SBC-XQ/AAC/LDAC/aptX/aptX-HD. aptX-LL only helps if
# *this* connected device actually negotiated it — a device with no aptX
# support at all (confirmed for the HK Onyx Studio 6 this session) can
# never end up on this codec regardless of what this machine supports.
LOW_LATENCY_CODECS = {"aptx_ll", "aptx_ll_duplex", "faststream", "faststream_duplex"}


def default_sink_bluetooth_info() -> dict:
    """{"is_bluetooth", "device_name", "codec", "low_latency_codec"} for
    whichever sink is currently the system default. device_name/codec are
    "" if not Bluetooth or the lookup fails for any reason — never fatal,
    same reasoning as trace_delay.py's own sink_codec()."""
    try:
        objects = json.loads(
            subprocess.run(["pw-dump"], capture_output=True, text=True,
                             timeout=5, check=True).stdout)
    except (subprocess.SubprocessError, OSError, json.JSONDecodeError):
        return {"is_bluetooth": False, "device_name": "", "codec": "",
                "low_latency_codec": False}

    target = None
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
        return {"is_bluetooth": False, "device_name": "", "codec": "",
                "low_latency_codec": False}

    for obj in objects:
        props = (obj.get("info") or {}).get("props") or {}
        if props.get("media.class") != "Audio/Sink" or props.get("node.name") != target:
            continue
        codec = props.get("api.bluez5.codec") or ""
        is_bluetooth = bool(codec)
        name = props.get("node.description") or props.get("node.name") or ""
        return {
            "is_bluetooth": is_bluetooth,
            "device_name": name if is_bluetooth else "",
            "codec": codec,
            "low_latency_codec": codec in LOW_LATENCY_CODECS,
        }
    return {"is_bluetooth": False, "device_name": "", "codec": "", "low_latency_codec": False}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(default_sink_bluetooth_info()))


if __name__ == "__main__":
    main()
