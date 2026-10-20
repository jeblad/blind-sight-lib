#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Dumps every currently-live PipeWire Audio/Sink* or Audio/Source* node's
relevant properties via `pw-dump` (blind-sight-lib demo).

Run as a separate process specifically so this never shares GStreamer's
own in-process pipewire plugin thread-loop with an already-running
pipewiresrc/pipewiresink element in the calling process — confirmed live
via gdb (2026-10) that querying a GstDeviceProvider from the same process
as an active pipewire pipeline can deadlock: both contend for the same
process-global pw_thread_loop lock (one thread stuck mid node-state-
change while holding it, the main thread blocked trying to acquire the
same lock just to enumerate devices). A separate process has its own,
independent PipeWire connection, so this can never collide with the
app's own pipelines.

Used by both audio_devices.cpp (the device-picker enumeration, replacing
a GstDeviceProvider-based pass entirely) and audio_routing.cpp (BlueZ
MAC -> node.name resolution) — one dump, two different filters applied
on the C++ side, rather than two near-duplicate scripts.

Stdlib only, writes its result as JSON to --output, same convention as
every other subprocess this project launches (never piped stdout). No
field is ever null — always normalized to "" / -1 / false — since glaze
(the C++ side's JSON reflection library) expects concrete types, same
convention check_bt_output.py's own BtOutputInfo-shaped output already
follows.
"""

import argparse
import json
import re
import subprocess
from pathlib import Path

_MAC_RE = re.compile(r"^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$")

# For a headset, WirePlumber puts a loopback in front of the device: apps
# link to a virtual bluez_output.<MAC>/bluez_input.<MAC> node, which feeds
# the device node through bluez_*_internal.<MAC> streams. These prefixes
# are the loopback's own internals (and, for some devices, the device node
# itself) — never what an app should link to while the virtual node exists.
# audio_routing.cpp additionally prefers non-"/Internal" sinks for the same
# reason, covering device nodes named without these prefixes.
_BLUEZ_INTERNAL_PREFIXES = ("bluez_output_internal.", "bluez_input_internal.",
                            "bluez_capture_internal.", "bluez_playback_internal.")


def audio_nodes() -> list:
    """[{"id", "node_name", "media_class", "display_name", "bluez_address",
    "is_bluez", "is_virtual", "profile_description", "api",
    "alsa_card"}] for every currently-live PipeWire node whose
    media.class starts with "Audio/Sink" or "Audio/Source" — some
    devices report a more specific sub-class (e.g. "Audio/Sink/Internal",
    confirmed live for Bose Hearphones), hence startswith() rather than
    an exact match. Empty list on any failure, never fatal (same
    reasoning as check_bt_output.py's own default_sink_bluetooth_info())."""
    try:
        objects = json.loads(
            subprocess.run(["pw-dump"], capture_output=True, text=True,
                             timeout=5, check=True).stdout)
    except (subprocess.SubprocessError, OSError, json.JSONDecodeError):
        return []

    nodes = []
    for obj in objects:
        props = (obj.get("info") or {}).get("props") or {}
        media_class = props.get("media.class") or ""
        if not (media_class.startswith("Audio/Sink") or
                media_class.startswith("Audio/Source")):
            continue
        node_name = props.get("node.name")
        if not node_name:
            continue
        if node_name.startswith(_BLUEZ_INTERNAL_PREFIXES):
            continue
        display_name = (props.get("node.description") or props.get("node.nick") or
                          node_name)
        bluez_address = props.get("api.bluez5.address") or props.get("bluez5.address") or ""
        if not bluez_address:
            # Some devices never populate the address property on their
            # plain bluez_output./bluez_input. node at all (confirmed
            # live, TAT8505) even though it's the correct, connectable
            # one — but the MAC is still right there in the node name
            # itself, colon-separated. Internal nodes are already
            # skipped above, so this can't misfire on one of those.
            for prefix in ("bluez_output.", "bluez_input."):
                if node_name.startswith(prefix):
                    candidate = node_name[len(prefix):]
                    if _MAC_RE.match(candidate):
                        bluez_address = candidate
                    break
        alsa_card = props.get("alsa.card")
        nodes.append({
            "id": obj.get("id") if isinstance(obj.get("id"), int) else -1,
            "node_name": node_name,
            "media_class": media_class,
            "display_name": display_name,
            "bluez_address": bluez_address,
            # A node clearly Bluetooth-backed by its own name even
            # without an address property exposed — confirmed live that
            # the same physical device can expose both a colon-MAC-named
            # node with no address property and a separate, properly
            # profiled one that does, depending on connection/profile
            # state. A second, defensive signal so a node like this can
            # never leak into the non-Bluetooth device list just because
            # it happens to be missing the address property right now.
            "is_bluez": bool(bluez_address) or node_name.startswith(("bluez_output.",
                                                                       "bluez_input.")),
            "is_virtual": bool(props.get("node.virtual")),
            "profile_description": props.get("device.profile.description") or "",
            "api": props.get("device.api") or "",
            "alsa_card": alsa_card if isinstance(alsa_card, int) else -1,
        })
    return nodes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(audio_nodes()))


if __name__ == "__main__":
    main()
