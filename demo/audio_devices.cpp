// SPDX-License-Identifier: GPL-3.0-only
//
// audio_devices.cpp — see audio_devices.hpp for scope.

#include "audio_devices.hpp"

#include <gio/gio.h>
#include <giomm/subprocess.h>
#include <glaze/glaze.hpp>
#include <glib/gi18n.h>
#include <glibmm/miscutils.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>

namespace blind_sight {

namespace {

// True if `device_props` (an org.bluez.Device1 "a{sv}" dict) advertises
// a Bluetooth service whose UUID starts with `prefix` (an 8-hex-digit
// SIG service-class id, e.g. "0000110b" for A2DP Sink) — BlueZ's own
// `UUIDs` property is an array of full 128-bit UUID strings.
bool bluez_device_has_uuid_prefix(GVariant* device_props, const char* prefix) {
    GVariant* uuids = g_variant_lookup_value(device_props, "UUIDs", G_VARIANT_TYPE("as"));
    if (!uuids) {
        return false;
    }
    bool found = false;
    GVariantIter iter;
    g_variant_iter_init(&iter, uuids);
    const gchar* uuid = nullptr;
    while (!found && g_variant_iter_next(&iter, "&s", &uuid)) {
        if (uuid && g_ascii_strncasecmp(uuid, prefix, strlen(prefix)) == 0) {
            found = true;
        }
    }
    g_variant_unref(uuids);
    return found;
}

// Pulls every paired device exposing org.bluez.Device1 whose Icon
// property starts with "audio-" (audio-card/audio-headset/
// audio-headphones — a coarse "is this an audio device at all" gate;
// a device with no Icon passes if it advertises an audio UUID)
// AND whose own advertised Bluetooth service UUIDs actually match the
// requested class — confirmed live this distinction matters: "HK Onyx
// Studio 6," an ordinary Bluetooth speaker, showed up in the Ambient
// Microphone picker too before this, since every paired audio device
// was previously included in both lists unconditionally. A2DP Sink
// (0000110b) means "can receive audio" (a listening device); Handsfree
// (0000111e) or Headset (00001108) means it offers an uplink mic over
// HFP/HSP — some speakers (Onyx Studio's own built-in call/speakerphone
// mic is a real, documented feature of that line) genuinely advertise
// both. A device with no UUIDs property at all (rare, older/nonstandard
// devices) falls back to being treated as Sink-only, the more common
// case, rather than silently disappearing from every list. Degrades to
// an empty list on any D-Bus error (bluetoothd not running, no BlueZ at
// all, etc.) rather than failing the whole enumeration — same "missing/
// broken external state isn't a hard error" idiom this project already
// uses for tracks.json/sessions.json.
std::vector<AudioDeviceInfo> list_bluez_paired_audio_devices(const std::string& gst_device_class) {
    std::vector<AudioDeviceInfo> devices;

    GError* error = nullptr;
    GDBusConnection* connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &error);
    if (!connection) {
        if (error) {
            g_error_free(error);
        }
        return devices;
    }

    GVariant* result = g_dbus_connection_call_sync(
        connection, "org.bluez", "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects", nullptr, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!result) {
        if (error) {
            g_error_free(error);
        }
        g_object_unref(connection);
        return devices;
    }

    GVariant* objects = g_variant_get_child_value(result, 0);
    GVariantIter object_iter;
    g_variant_iter_init(&object_iter, objects);
    GVariant* entry;
    while ((entry = g_variant_iter_next_value(&object_iter)) != nullptr) {
        GVariant* interfaces = g_variant_get_child_value(entry, 1);  // a{sa{sv}}
        GVariant* device_props =
            g_variant_lookup_value(interfaces, "org.bluez.Device1", G_VARIANT_TYPE("a{sv}"));
        if (device_props) {
            gboolean paired = FALSE;
            g_variant_lookup(device_props, "Paired", "b", &paired);
            const gchar* icon = nullptr;
            g_variant_lookup(device_props, "Icon", "&s", &icon);
            GVariant* uuids_value =
                g_variant_lookup_value(device_props, "UUIDs", G_VARIANT_TYPE("as"));
            bool has_uuids = uuids_value != nullptr;
            if (uuids_value) {
                g_variant_unref(uuids_value);
            }
            bool has_mic_uuid = bluez_device_has_uuid_prefix(device_props, "0000111e") ||  // Handsfree
                                 bluez_device_has_uuid_prefix(device_props, "00001108");    // Headset
            bool has_sink_uuid =
                bluez_device_has_uuid_prefix(device_props, "0000110b");  // A2DP Sink
            // LE Audio's Audio Stream Control Service — the only Bluetooth
            // path to a mic without dropping to call quality.
            bool has_le_audio_uuid = bluez_device_has_uuid_prefix(device_props, "0000184e");
            bool class_matches;
            if (gst_device_class == "Audio/Source") {
                class_matches = has_mic_uuid;
            } else if (gst_device_class == "Audio/Sink+Mic") {
                // A participant's own device needs both directions (play
                // audio to them, capture their voice) — John: "Alle
                // deltagere skal kun få opp enheter med mic," unlike
                // Ambient Speaker, which only needs to play audio.
                class_matches = has_sink_uuid && has_mic_uuid;
            } else {
                class_matches = !has_uuids || has_sink_uuid;
            }
            // Some devices expose no Icon at all (confirmed: Bose
            // Hearphones) — for those, advertised audio UUIDs alone decide.
            bool is_audio_device = icon ? g_str_has_prefix(icon, "audio-")
                                        : (has_sink_uuid || has_mic_uuid);
            if (paired && is_audio_device && class_matches) {
                const gchar* address = nullptr;
                g_variant_lookup(device_props, "Address", "&s", &address);
                const gchar* alias = nullptr;
                if (!g_variant_lookup(device_props, "Alias", "&s", &alias)) {
                    g_variant_lookup(device_props, "Name", "&s", &alias);
                }
                gboolean connected = FALSE;
                g_variant_lookup(device_props, "Connected", "b", &connected);

                if (address) {
                    AudioDeviceInfo info;
                    info.id = address;
                    info.display_name = alias ? alias : address;
                    info.connected = connected;
                    info.handsfree_only_mic = has_mic_uuid && !has_le_audio_uuid;
                    if (gst_device_class == "Audio/Source" && info.handsfree_only_mic) {
                        info.subtitle =
                            connected ? _("Bluetooth handsfree, reduced quality — Connected")
                                      : _("Bluetooth handsfree, reduced quality — Not Connected");
                    } else {
                        info.subtitle = connected ? _("Bluetooth — Connected")
                                                  : _("Bluetooth — Not Connected");
                    }
                    devices.push_back(std::move(info));
                }
            }
            g_variant_unref(device_props);
        }
        g_variant_unref(interfaces);
        g_variant_unref(entry);
    }
    g_variant_unref(objects);
    g_variant_unref(result);
    g_object_unref(connection);
    return devices;
}

// One GStreamer-class device, plus its ALSA card number (-1 if it
// doesn't have one) — only used internally, to correlate a Sink against
// a Source sharing the same physical card for "Audio/Sink+Mic" (see
// list_gstreamer_devices() below); never exposed in AudioDeviceInfo
// itself, which stays a plain display-oriented struct.
struct GstCandidate {
    AudioDeviceInfo info;
    int alsa_card = -1;
};

// Field names must match dump_pipewire_audio_nodes.py's own JSON output
// exactly — glaze reflects this struct's members directly, same pattern
// as bt_output_check.cpp's own BtOutputInfo.
struct PipeWireAudioNode {
    std::string node_name;
    std::string media_class;
    std::string display_name;
    std::string bluez_address;
    bool is_bluez = false;
    bool is_virtual = false;
    std::string profile_description;
    std::string api;
    int alsa_card = -1;
};

// Dumps every currently-live PipeWire Audio/Sink*/Audio/Source* node via
// a separate `pw-dump` subprocess (dump_pipewire_audio_nodes.py) — not
// GStreamer's in-process pipewiredeviceprovider/GstDeviceMonitor, which
// this file used until a real, confirmed deadlock (John, live testing,
// gdb backtrace, 2026-10): the main thread blocked in
// gst_device_provider_get_devices() -> pw_thread_loop_lock(), while a
// separate thread — the shared, process-global PipeWire thread-loop
// GStreamer's own pipewire plugin uses for every pipewiresrc/
// pipewiresink element in this process, including audio_routing.cpp's
// own already-running routing pipelines — sat mid node-state-change
// holding that exact lock. Any GstDeviceProvider/GstDeviceMonitor query
// from this same process can collide with an active routing pipeline
// this way; it isn't a frequency problem, a single query can deadlock
// just as well. A separate process has its own, fully independent
// PipeWire connection, so it can never collide with this app's own
// pipelines. Synchronous (wait_check() blocks the caller for one
// subprocess round-trip, typically well under a second) — every caller
// here is a synchronous click handler (open a device picker, offer a
// counterpart device), not worth restructuring into async for.
std::vector<PipeWireAudioNode> dump_pipewire_audio_nodes() {
    std::vector<PipeWireAudioNode> nodes;
    std::string output_path = Glib::build_filename(
        Glib::get_tmp_dir(),
        "blind-sight-pw-nodes-" + std::to_string(g_get_monotonic_time()) + ".json");
    std::vector<std::string> argv = {"python3", BLIND_SIGHT_DUMP_AUDIO_NODES_SCRIPT, "--output",
                                      output_path};
    try {
        auto proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        proc->wait_check();
        std::ifstream in(output_path);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        // Tolerate fields this struct doesn't declare (e.g. "id"): glaze
        // rejects unknown keys by default.
        if (content.empty() ||
            glz::read<glz::opts{.error_on_unknown_keys = false}>(nodes, content)) {
            return {};  // malformed/failed — treat as "nothing to list"
        }
    } catch (const Glib::Error& e) {
        g_message("blind-sight: failed to dump PipeWire audio nodes: %s", e.what());
    }
    return nodes;
}

// Builds a candidate from `node` if it matches `gst_device_class` and
// passes the usual filters; returns false (leaving *out untouched) if
// this node should be skipped entirely.
bool make_gst_candidate(const PipeWireAudioNode& node, const std::string& gst_device_class,
                         GstCandidate* out) {
    // Prefix, not exact match — some devices report a more specific
    // sub-class than the plain "Audio/Sink"/"Audio/Source" GStreamer
    // itself used to report (confirmed live: Bose Hearphones reports
    // "Audio/Sink/Internal"). Moot for a Bluetooth node specifically
    // (excluded below regardless, already covered by the BlueZ D-Bus
    // pass), but kept general for any future non-Bluetooth sub-class.
    if (!node.media_class.starts_with(gst_device_class)) {
        return false;
    }
    if (node.is_bluez || node.is_virtual) {
        return false;
    }
    // This app's own nodes (the voice levellers' virtual sources) are not
    // devices anyone should pick.
    if (node.node_name.starts_with("blind-sight-")) {
        return false;
    }
    out->info.id = node.node_name;
    out->info.display_name = node.display_name.empty() ? node.node_name : node.display_name;
    out->info.connected = true;  // only appears here while actually live
    // Prefer the port/profile description (e.g. "Speakers" vs. "Front
    // Headphones" vs. "S/PDIF Output") over the generic API name
    // ("alsa" for all three) — confirmed live that a single physical
    // card exposing multiple output profiles otherwise looks like the
    // same entry repeated.
    out->info.subtitle = node.profile_description.empty() ? node.api : node.profile_description;
    out->alsa_card = node.alsa_card;
    return true;
}

std::vector<GstCandidate> list_gst_candidates(const std::string& gst_device_class) {
    std::vector<GstCandidate> result;
    for (const auto& node : dump_pipewire_audio_nodes()) {
        GstCandidate candidate;
        if (make_gst_candidate(node, gst_device_class, &candidate)) {
            result.push_back(std::move(candidate));
        }
    }
    return result;
}

// Non-Bluetooth devices (analog/USB) — only ever listed while actually
// present, since they have no "paired but absent" concept. Bluetooth
// devices found here are skipped outright: a connected Bluetooth node
// is necessarily already paired, so it's already covered by
// list_bluez_paired_audio_devices() above — listing it again here would
// show the same physical device twice. Also skips `node.virtual` nodes
// (PipeWire modules like EasyEffects' own sink, or the sandboxing
// "Dummy Source for Restricted Clients") — confirmed live via
// `gst-device-monitor-1.0`, these aren't real listening devices and have
// no business in a device picker.
//
// "Audio/Sink+Mic" (a participant's own device, which needs to both play
// audio to them and capture their voice — unlike Ambient Speaker, which
// only plays) has no direct GStreamer class: a USB headset's playback
// and capture sides are two entirely separate PipeWire nodes, not one.
// Correlated here by `alsa.card` — the same physical card's sink is kept
// only if that same card also has a source.
std::vector<AudioDeviceInfo> list_gstreamer_devices(const std::string& gst_device_class) {
    if (gst_device_class == "Audio/Sink+Mic") {
        std::vector<GstCandidate> sinks = list_gst_candidates("Audio/Sink");
        std::vector<GstCandidate> sources = list_gst_candidates("Audio/Source");
        std::set<int> cards_with_mic;
        for (const auto& source : sources) {
            if (source.alsa_card >= 0) {
                cards_with_mic.insert(source.alsa_card);
            }
        }
        std::vector<AudioDeviceInfo> devices;
        for (auto& sink : sinks) {
            if (sink.alsa_card >= 0 && cards_with_mic.count(sink.alsa_card) != 0) {
                devices.push_back(std::move(sink.info));
            }
        }
        return devices;
    }

    std::vector<GstCandidate> candidates = list_gst_candidates(gst_device_class);
    std::vector<AudioDeviceInfo> devices;
    devices.reserve(candidates.size());
    for (auto& candidate : candidates) {
        devices.push_back(std::move(candidate.info));
    }
    return devices;
}

}  // namespace

std::vector<AudioDeviceInfo> enumerate_audio_devices(const std::string& gst_device_class) {
    std::vector<AudioDeviceInfo> devices = list_bluez_paired_audio_devices(gst_device_class);
    std::vector<AudioDeviceInfo> gst_only = list_gstreamer_devices(gst_device_class);
    devices.insert(devices.end(), std::make_move_iterator(gst_only.begin()),
                    std::make_move_iterator(gst_only.end()));
    // Connected devices first, alphabetical within each group — purely
    // for a more usable picker list, no semantic meaning beyond that.
    std::stable_sort(devices.begin(), devices.end(),
                      [](const AudioDeviceInfo& a, const AudioDeviceInfo& b) {
                          if (a.connected != b.connected) {
                              return a.connected > b.connected;
                          }
                          return a.display_name < b.display_name;
                      });
    return devices;
}

AudioDeviceInfo find_counterpart_device(const AudioDeviceInfo& picked,
                                         const std::string& picked_class) {
    std::string other_class = picked_class == "Audio/Source" ? "Audio/Sink" : "Audio/Source";

    // Bluetooth ids are always MAC-formatted ("AA:BB:CC:DD:EE:FF", 17
    // chars) — re-query BlueZ for the opposite direction and check
    // whether the same device (same address) is in it.
    if (picked.id.size() == 17 && picked.id.find(':') != std::string::npos) {
        for (const auto& candidate : list_bluez_paired_audio_devices(other_class)) {
            if (candidate.id == picked.id) {
                return candidate;
            }
        }
        return AudioDeviceInfo{};
    }

    // GStreamer/ALSA: correlate by ALSA card number — re-probe the raw
    // candidates (not enumerate_audio_devices()'s own already-trimmed
    // AudioDeviceInfo list) since the card number only lives on
    // GstCandidate.
    int picked_card = -1;
    for (const auto& candidate : list_gst_candidates(picked_class)) {
        if (candidate.info.id == picked.id) {
            picked_card = candidate.alsa_card;
            break;
        }
    }
    if (picked_card < 0) {
        return AudioDeviceInfo{};
    }
    for (const auto& candidate : list_gst_candidates(other_class)) {
        if (candidate.alsa_card == picked_card) {
            return candidate.info;
        }
    }
    return AudioDeviceInfo{};
}

}  // namespace blind_sight
