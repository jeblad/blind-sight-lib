// SPDX-License-Identifier: GPL-3.0-only
//
// tracks.hpp — data model and file I/O for a session's own tracks.json.
//
// Unlike sessions.json/images.json (one index per project), this index is
// per-session, at <project_dir>/sessions/<session_id>/tracks.json — a
// track only ever means something relative to the one session it's part
// of, the same way session.bs's own event log is per-session.
//
// A track minted after this feature shipped gets a stable id and its own
// directory, <project_dir>/sessions/<session_id>/tracks/<id>/ (mirroring
// images/<id>/) — see track_directory(), audio.hpp. A track recorded
// before this feature shipped has no entry here at all; it's identified
// only by its position (1 + however many WaitEvents precede it, same as
// always) and keeps living at the old flat "recording.trackN.*" naming
// under the session directory. TrackEntry::id is empty for exactly this
// case: a legacy track that was touched (hidden, reordered, noted) in the
// Edit Tracks dialog gets a real entry here, keyed by its position, but
// an empty id, since it never had files moved into a tracks/ directory —
// only its metadata lives here.

#pragma once

#include <map>
#include <string>
#include <vector>

namespace blind_sight {

constexpr int kTracksIndexFormatVersion = 1;

// One track's entry in a session's tracks.json. `id` is this track's
// directory name under <session_dir>/tracks/ when it has one (minted at
// record time, see audio.hpp's track_directory()) — empty for a legacy
// track that predates this feature and still lives at the old flat
// naming.
//
// `track_number` is the track's real, *never-changed* 1-based recording
// position — "1 + however many WaitEvents precede it," same count
// compute_track_starts()/compute_audio_segments() (audio.cpp) already
// use. This is the stable key every file/event-log lookup keys off, and
// it never moves, even once the track has an `id`. `order` is the
// *display* position in the Edit Tracks dialog's list — independent and
// user-adjustable (reorder there is view-only, see track_dialogs.cpp),
// initialized equal to `track_number`'s own position but free to diverge
// afterward. Conflating the two was an early mistake in this design:
// reordering the list must never change which file a given track_number
// resolves to. `hidden` mirrors SessionEntry/ImageEntry's eye-toggle.
//
// `title` is an optional, user-entered override of the track's short
// identifying name ("stikktittel") — empty by default, *never*
// auto-populated (not even once, at record time): the default
// stikktittel is a pure live *derivation* from the protocol's active
// scenario (H2), the same lookup the track dropdown's own rows already
// do (image_info_for_track(), main.cpp, reading ImageEvent::scenario
// from the event log). `title`, when the user does set it, overrides
// that derivation for display — entirely the user's call whether a
// track needs something "mer spesifikt" than what's already shown;
// nothing here ever copies the derived value into it. `note` is a
// separate, unrelated piece of free text — both are entered via the
// Edit Tracks dialog's "Note" action.
struct TrackEntry {
    std::string id;
    int track_number = 0;
    int order = 0;
    bool hidden = false;
    std::string title;
    std::string note;
    // A snapshot of whatever device (monitoring.hpp's MonitoringState)
    // was assigned to each participant at the moment this track was
    // minted — diagnostic only (explains an audio anomaly later), not a
    // stable identifier system. Keyed by participant name, same
    // identity MonitoringState itself uses. `participant_devices` is
    // each participant's own listening (sink) device;
    // `participant_mic_devices` is their own mic (source) device — the
    // two are independent assignments, possibly different physical
    // hardware (see MonitoringState's own comment on why), so neither
    // one implies the other.
    std::map<std::string, std::string> participant_devices;
    std::map<std::string, std::string> participant_mic_devices;
    // Separate from the per-participant maps above — the ambient mic
    // isn't a participant, see monitoring.hpp's own comment.
    std::string ambient_mic_device;
};

struct TracksIndex {
    int version = kTracksIndexFormatVersion;
    std::vector<TrackEntry> tracks;
};

// Reads <session_dir>/tracks.json — an empty TracksIndex (not a hard
// failure) if the file doesn't exist yet or fails to parse, same
// reasoning as read_sessions_index()/read_images_index(). Every track
// recorded before this feature shipped, and every track in a session
// never opened in the Edit Tracks dialog, is simply absent — not an
// error, since "no entry" already means "legacy/untouched" everywhere
// this index is read.
TracksIndex read_tracks_index(const std::string& session_dir);

// Writes tracks.json back out, overwriting whatever was there.
bool write_tracks_index(const std::string& session_dir, const TracksIndex& index);

// Generates a new, not-yet-used track id for this session — same hash-
// of-time-plus-entropy scheme as generate_session_id()/generate_image_id(),
// duplicated rather than shared since the only difference is which
// subfolder collision-checks run against (<session_dir>/tracks/<id>).
std::string generate_track_id(const std::string& session_dir);

}  // namespace blind_sight
