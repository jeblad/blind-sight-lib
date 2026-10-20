// SPDX-License-Identifier: GPL-3.0-only
//
// monitoring.hpp — in-memory "who hears whom" state for a recording
// session's participants (the monitoring matrix popover, main.cpp).
//
// Deliberately in-memory only, reset every new session: no JSON file, no
// GSettings key. This mirrors demo/data/io.github.jeblad.blind-sight-demo.
// gschema.xml's own history of moving growing/session-scoped state OUT of
// GSettings (toward sessions.json/images.json/tracks.json) — this state is
// one step further still, since it's not even worth a dedicated file.
// Only a small per-track snapshot of it ever gets persisted, see
// TrackEntry::participant_devices/ambient_mic_device (tracks.hpp).
//
// Device assignment is a real device id now (see audio_devices.hpp's
// enumerate_audio_devices()), not free text — but actual audio-routing
// enforcement from the mute matrix (and the "unassigned participants
// fall back to Ambient Mic/Speaker" rule) is still a separate, later
// round: this library has no per-participant audio pipeline to route
// into yet.
//
// A participant has *two* independent device assignments — their own
// listening device (sink) and their own mic (source) — never one
// combined pick. John: headphones can come in oddly split variants, a
// sink that's plain analog paired with a mic that's digital over
// Bluetooth on the very same physical headset, so the two directions
// can't be assumed to share one identity. sink_device_assignment and
// source_device_assignment are therefore entirely separate maps.
// Picking an already-assigned device transfers it from its previous
// owner (another participant) rather than being blocked — see
// monitoring_dialogs.cpp's assign_device()/find_participant_owner().
// Ambient Speaker/Microphone never take part in that ownership contest
// at all, in either direction.

#pragma once

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace blind_sight {

struct RecordMeta;

// Fixed, non-reorderable sentinel ids — chosen so they can never collide
// with a free-text participant name (RecordMeta::creator/contributor are
// user-typed). "Speaker" is a sink only (a loudspeaker for bystanders, no
// matching source/column). "AmbientMic"/"Synthesized" are sources only
// (no matching sink/row), in that left-to-right column order — the
// ambient mic (a fallback, opt-in-only input participants can choose to
// listen to) sits just left of Synthesized (the app's own output,
// listened to by everyone by default).
constexpr const char* kSpeakerSinkId = "__speaker__";
constexpr const char* kAmbientMicSourceId = "__ambient_mic__";
constexpr const char* kSynthesizedSourceId = "__synthesized__";

// A real device assignment — `device_id` is an AudioDeviceInfo::id
// (audio_devices.hpp), stable across reconnects for Bluetooth devices,
// not a display string. `display_name` is a snapshot taken at
// assignment time, kept alongside the id purely for display/diagnostics
// (e.g. TrackEntry's own snapshot, tracks.hpp) even if the device is
// later renamed or briefly unavailable.
struct DeviceAssignment {
    std::string device_id;
    std::string display_name;
};

// One session's live monitoring state. See RecordingSession::monitoring
// (main.cpp) for where this actually lives and resets.
struct MonitoringState {
    // Display order for both the row list and the column list — the same
    // underlying participant list, reordered once via the Edit
    // Participants dialog (collection_editor.hpp's CollectionReorder) and
    // mirrored onto both axes since there's only one list. Canonical
    // identity is the trimmed name itself — two identically-named
    // participants aren't distinguished; not handled this round.
    std::vector<std::string> participant_order;

    // Every (sink_id, source_id) pair this session currently has an
    // opinion about. Fully populated by reconcile_monitoring_state() for
    // every pair that's currently valid, so is_muted() never needs a
    // default-rule fallback at lookup time.
    std::map<std::pair<std::string, std::string>, bool> muted;

    // Keyed by participant name, or by kSpeakerSinkId (Ambient Speaker
    // has no mic concept, so it's sink-only). Absence means unassigned.
    std::map<std::string, DeviceAssignment> sink_device_assignment;
    // Keyed by participant name, or by kAmbientMicSourceId (sole
    // source-only entry). Absence means unassigned.
    std::map<std::string, DeviceAssignment> source_device_assignment;

    // A hard mute override on an entire row/column — John: some
    // participants can suddenly panic hearing their own voice, so muting
    // them needs to not depend on first finding the right cell(s) in the
    // matrix. Toggled by double-clicking a header button
    // (monitoring_dialogs.cpp) rather than through the matrix at all.
    // Deliberately NOT consulted by is_muted()/the matrix's own toggle
    // cells — John: switching an override back off must leave the
    // matrix exactly as it already was, so the override needs to stay
    // fully independent of the per-cell `muted` map rather than
    // overlaying it (that would make is_muted() misreport the matrix's
    // own cell state while an override is active, and the grid doesn't
    // even get rebuilt when one is toggled). Only the header's own
    // visual (icon swap for Speaker/Ambient Mic; a backslash over the
    // bigram for participants, a later round) reacts to these; real
    // audio-routing logic, once it exists, is expected to check these
    // ahead of the per-cell map rather than through is_muted() itself.
    // Keyed the same way as sink_device_assignment/source_device_
    // assignment — participant name, or the matching sentinel
    // (kSpeakerSinkId / kAmbientMicSourceId).
    std::set<std::string> muted_sink_override;
    std::set<std::string> muted_source_override;
};

// Splits RecordMeta::contributor the same way transcription.cpp's
// speaker_count_hint() already does (comma/semicolon, each piece
// trimmed), but keeps the trimmed substrings themselves rather than just
// counting them, and prepends `creator` first when non-empty.
std::vector<std::string> participant_names(const RecordMeta& meta);

// Reconciles `state` against the session's current participant names:
// appends any name in `current_names` not yet in participant_order (at
// the end, so an existing reorder survives); drops any participant_order
// entry — and its muted/device_assignment/mute-override rows — for a
// name no longer present;
// then fills in any missing (sink, source) pair in `muted` with
// is_default_muted(). Safe to call repeatedly and non-destructively —
// called once per session load, and again each time the matrix popover
// is about to show.
void reconcile_monitoring_state(MonitoringState& state,
                                 const std::vector<std::string>& current_names);

// Default mute state for a (sink, source) pair not yet set explicitly:
//   - kSpeakerSinkId x kSynthesizedSourceId: unmuted.
//   - kSpeakerSinkId x anything else: muted — the Ambient Microphone and
//     participants' mics share the room with the speaker.
//   - participant x same participant (diagonal): muted.
//   - participant x kAmbientMicSourceId: muted (opt-in only).
//   - everything else, including participant x kSynthesizedSourceId:
//     unmuted.
bool is_default_muted(const std::string& sink_id, const std::string& source_id);

bool is_muted(const MonitoringState& state, const std::string& sink_id,
              const std::string& source_id);
void set_muted(MonitoringState& state, const std::string& sink_id,
               const std::string& source_id, bool muted);

// Combines the per-cell mute map with both header-button override sets
// (muted_sink_override/muted_source_override stay UI-only everywhere
// else, per their own comment) — the one place that needs them
// overlaid, to decide whether an edge should actually carry audio
// (audio_routing.hpp's own sync()). The matrix's own toggle cells and
// header icons deliberately keep using is_muted() alone.
bool is_effectively_muted(const MonitoringState& state, const std::string& sink_id,
                           const std::string& source_id);

}  // namespace blind_sight
