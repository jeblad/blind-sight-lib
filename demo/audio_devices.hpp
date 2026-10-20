// SPDX-License-Identifier: GPL-3.0-only
//
// audio_devices.hpp — real audio-device enumeration for the monitoring
// matrix's device-assignment dialogs (monitoring_dialogs.cpp). Merges two
// sources, since neither alone is enough:
//
//  - BlueZ's own paired-device list (queried directly over D-Bus, raw
//    GDBus/GVariant C API — the first D-Bus client in this codebase).
//    Every device paired at least once stays listed here regardless of
//    whether it's currently connected. This matters because some
//    true-wireless earbuds (John named the TAT8505 ones specifically)
//    drop their Bluetooth link — or at least appear to, the exact
//    mechanism wasn't confirmed — when taken out of the ear or the case,
//    which would make them vanish from a picker built only from
//    PipeWire's *live* device graph at exactly the moment someone isn't
//    wearing them.
//  - A `pw-dump` dump of PipeWire's own live node graph (via a separate
//    subprocess, dump_pipewire_audio_nodes.py — not GStreamer's
//    GstDeviceProvider/GstDeviceMonitor, which this originally used
//    until a real, confirmed deadlock: querying one from this same
//    process while a pipewiresrc/pipewiresink pipeline is active can
//    contend for the same process-global PipeWire thread-loop lock
//    GStreamer's pipewire plugin shares across every element in the
//    process), for everything that isn't Bluetooth
//    — analog/USB devices have no "paired but not present" concept,
//    they're just plugged in or not.
//
// A device the PipeWire-dump pass finds that's actually Bluetooth-backed
// (carries a bluez5 MAC property, or is named like one even without
// that property exposed) is skipped, since it's already represented by
// the BlueZ pass above — avoids listing one physical device twice when
// it happens to be connected right now.
//
// Audio routing itself lives in audio_routing.hpp/.cpp (one pw-loopback
// process per edge); this file only lists and identifies devices.

#pragma once

#include <string>
#include <vector>

namespace blind_sight {

struct AudioDeviceInfo {
    // Stable identifier: a BlueZ MAC address ("AA:BB:CC:DD:EE:FF") for
    // Bluetooth devices — works whether currently connected or not —
    // otherwise the GStreamer/PipeWire node name. Never empty for a real
    // entry.
    std::string id;
    std::string display_name;
    // e.g. "Bluetooth — Connected" / "Bluetooth — Not Connected" /
    // "Analog" — best-effort, may be empty.
    std::string subtitle;
    bool connected = false;
    // Bluetooth only: the device's mic works only over HSP/HFP, which
    // switches the whole link to narrowband call quality in both
    // directions — including what the wearer hears.
    bool handsfree_only_mic = false;
    // Picker-UI-only annotation (not filled in by enumerate_audio_devices()
    // itself) — the two-letter header bigram (monitoring_dialogs.cpp's
    // assign_bigrams()) of whichever participant already owns this device,
    // or empty if unowned. Shown on the row's title line rather than
    // folded into `subtitle`, since a full name there made rows too wide
    // for the dialog — see monitoring_dialogs.cpp's show_device_picker_dialog().
    std::string owner_bigram;
};

// `gst_device_class` is a GStreamer device-class string: "Audio/Sink"
// for listening devices (participants, Ambient Speaker), "Audio/Source"
// for capture devices (Ambient Microphone), or "Audio/Sink+Mic" — a
// participant's own device, which needs both directions (it plays audio
// to them *and* captures their voice), correlated from the separate
// Sink/Source lists rather than being a real GStreamer class of its
// own. Synchronous — both the BlueZ D-Bus call and the GStreamer
// device-monitor probe are quick, bounded, one-shot operations, not a
// persistent background watch; call this fresh each time the picker
// dialog opens.
std::vector<AudioDeviceInfo> enumerate_audio_devices(const std::string& gst_device_class);

// If `picked` (chosen under `picked_class`, "Audio/Sink" or
// "Audio/Source") can also serve the opposite direction, returns that
// counterpart device — same id for a Bluetooth device (one physical
// device, BlueZ exposes both directions under the same MAC via
// separate service UUIDs), a *different* id for a GStreamer/ALSA device
// (a card's playback and capture sides are separate PipeWire nodes,
// correlated here by ALSA card number). Returns an AudioDeviceInfo with
// an empty `id` if there's no counterpart. Used to offer "this device
// can also be used as your X — assign it there too?" after a pick,
// rather than requiring the user to repeat the whole picker for the
// matching device (see monitoring_dialogs.cpp).
AudioDeviceInfo find_counterpart_device(const AudioDeviceInfo& picked,
                                         const std::string& picked_class);

}  // namespace blind_sight
