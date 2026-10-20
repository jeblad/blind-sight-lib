// SPDX-License-Identifier: GPL-3.0-only
//
// recording.hpp — data model and file I/O for the Record/Playback log.
//
// Deliberately separate from main.cpp: this is the non-UI half of
// Record/Playback (data + serialization) — kept apart from the
// dialog/menu/window wiring so main.cpp doesn't become one enormous file.
//
// File format: JSON Lines, plain JSON (not Glaze's BEVE binary format) —
// one JSON value per line, so recordings are processable with ordinary
// tools, not just via Glaze. Extension is `.bs`, not `.jsonl` — a
// distinct suffix that signals "our schema" rather than a generic one.
// This is a demo-only concern; the library never sees any of it.

#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace blind_sight {

// Bumped whenever RecordMeta's shape changes. Needed because Glaze
// errors on unknown JSON keys by default (the strictness is what makes
// it fast, not a flaw to work around), so an old recording made under a
// since-changed shape would otherwise fail to open with no explanation.
// read_record_meta() rejects anything that doesn't match
// kRecordFormatVersion exactly. Legend:
//   1 — retroactive: every recording made before this field existed.
//   2 — the current Dublin Core shape (see RecordMeta's own comment).
//   3 — adds RecordMeta::protocol_file and ImageEvent::scenario.
//   4 — splits ProdEvent's own `type` into two subtypes, "head-prod"/
//       "eye-prod" (see ProdEvent's own comment).
//   5 — adds TestStepEvent (see its own comment).
constexpr int kRecordFormatVersion = 5;

// Appends one JSON Lines record to an already-open stream — the
// per-event counterpart to write_record_meta() below, for the live
// gaze/view/image stream while a recording is running. flush()es so
// data survives the process being killed mid-run, not just a clean
// Stop. Header-only template: T varies per call site, so this can't be
// a single compiled function the way write_record_meta() is.
template <typename T>
bool write_event(std::ofstream& out, const T& event) {
    std::string json;
    if (auto ec = glz::write_json(event, json); ec) {
        return false;
    }
    out << json << '\n';
    out.flush();
    return static_cast<bool>(out);
}

// The type:meta line written at the start of a recording — Dublin Core
// element names as the JSON keys themselves, not this project's own
// ad-hoc names, so reuse elsewhere doesn't need a translation layer.
// Bare DCMI element names, no "dc:" namespace prefix (flat JSON, not
// RDF/XML); `creator`/`contributor` are deliberately left open to
// whatever roles a given recording actually has, not glossed with a
// "tester"/"testee" framing. Plain aggregate — Glaze v7.5.0 reflects
// the field names directly, no glz::meta needed.
struct RecordMeta {
    std::string type = "meta";
    // The file's format version, not a recording/session number — see
    // kRecordFormatVersion above. Defaults to 0, an otherwise-invalid
    // sentinel, NOT kRecordFormatVersion — Glaze only touches fields
    // present in the JSON being parsed, so a file with no "version" key
    // at all would otherwise silently read back as the current version,
    // defeating read_record_meta()'s check. write_record_meta() is what
    // actually stamps kRecordFormatVersion on the way out.
    int version = 0;
    std::string title;
    std::string creator;
    std::string contributor;
    std::string date;
    std::string description;
    // Usage restrictions — free text, not a fixed set. The two
    // suggested values are shown as a hint in the dialog.
    std::string rights;
    // Which protocol (.md file, see protocol.hpp) this session's images
    // came from — empty for a session not run against a protocol. The
    // protocol's *title* deliberately isn't a separate field — it's
    // the same value as `title` above, made non-editable in the dialog
    // whenever a protocol set it, since matching by the protocol's own
    // H1 is more robust than matching by this field (the same protocol
    // can exist at a different path on a different machine). This field
    // is only the filename/path, for re-opening the same file again.
    std::string protocol_file;
};

// Writes just the meta line, truncating the file — correct for a
// brand-new recording.
bool write_record_meta(const std::string& path, const RecordMeta& meta);

// Reads only the first line and stops — matches the current Playback
// scope: identify the file, present its metadata, nothing more yet.
// False if the file can't be opened or the first line doesn't parse as
// RecordMeta (out_meta left however it was).
bool read_record_meta(const std::string& path, RecordMeta& out_meta);

// Replaces just the first line (the meta block) of an *existing* file,
// leaving every other line untouched — write_record_meta() truncates
// the whole file, which would destroy every recorded event on a file
// that already has some. Writes to a temp file next to `path` and
// renames over it, so a crash mid-write can't leave the file
// half-rewritten. False if `path` doesn't exist/can't be read, or the
// temp file can't be written/renamed.
bool update_record_meta(const std::string& path, const RecordMeta& meta);

// A single pointer-position sample — motion only. Position is
// normalized and relative to the *current viewport*, not absolute in
// the full equirectangular image — that's what the pointer position
// over the displayed crop gives directly, with no projection math
// needed at capture time. Reconstructing an absolute position means
// combining a sample with the most recent ViewEvent before it.
//
// `t_ms`/`x`/`y` are fixed-point integers, not `double` — a normalized
// fraction serialized as JSON text is a long, "messy" decimal
// (floating-point noise included), unnecessarily large for what the
// numbers carry. `t_ms` is whole milliseconds since the recording
// started; `x`/`y` are the normalized [0, 1] position scaled to
// [0, 65535] and rounded. Every other event type below uses the same
// convention without restating it.
struct GazeEvent {
    std::string type = "gaze";
    std::int32_t t_ms = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};

// A click/button-down, or a saccade settling — its own type rather than
// a field on GazeEvent, since a click is a genuinely different input
// event from motion. Same shape and fixed-point convention as
// GazeEvent — a prod is anchored at a position too, latched at the
// click point.
//
// Two *subtypes*, not two separate structs — only `type` differs,
// since head-direction and eye/pointer prods are separate trigger
// paths (see main.cpp's fire_head_prod()/fire_eye_prod()):
//   - "head-prod": a Surface Dial click, or Dial/scroll-wheel rotation
//     settling. Always centered (x/y at the viewport's own center) —
//     head direction is always dead center of the rendered viewport.
//   - "eye-prod": a mouse click, or the pointer settling after moving.
//     x/y is the actual pointer/gaze position, not center.
struct ProdEvent {
    std::string type = "head-prod";
    std::int32_t t_ms = 0;
    std::uint16_t x = 0;
    std::uint16_t y = 0;
};

// The pointer left the window — its own type rather than a modifier
// field on GazeEvent: a leave is a discrete state transition, not a
// position sample, so it doesn't fit GazeEvent's shape (GTK's leave
// signal carries no position either). No matching `type:enter`: the
// next `type:gaze` after a `type:leave` already means "back in, at
// this position" on its own.
struct LeaveEvent {
    std::string type = "leave";
    std::int32_t t_ms = 0;
};

// The current viewport orientation into the panorama — the "head"
// control (keyboard/drag, i.e. the inverse-IMU). Logged only when the
// viewport actually changes, not per gaze sample — changes are far
// less frequent than raw pointer movement, the whole point of keeping
// this a separate type from GazeEvent.
//
// `yaw`/`pitch` are centidegrees (degrees × 100, rounded), same
// fixed-point convention as GazeEvent's position, extended to angles.
// ±18000 (±180.00°) comfortably fits `std::int32_t` with room to spare.
struct ViewEvent {
    std::string type = "view";
    std::int32_t t_ms = 0;
    std::int32_t yaw = 0;
    std::int32_t pitch = 0;
};

// Which image/source is active. Appended once right after (re)opening
// the file for a recording run (see WaitEvent below for the run-
// boundary model) — not per-frame.
struct ImageEvent {
    std::string type = "image";
    std::int32_t t_ms = 0;
    std::string path;
    // The protocol section (H2 heading text) this image came from, if
    // the running session is following a protocol at all — empty
    // otherwise (see protocol.hpp). Written even when `path` is empty:
    // a protocol section with no image still shows the test-image
    // placeholder and still needs to be correlated back to its scenario.
    std::string scenario;
};

// A recording is allowed to be stopped and later resumed into the
// *same* file — the test subject hits a problem that isn't sufficiently
// explained, and restarting mid-session is better than losing the whole
// run. At rest the file is closed; resuming reopens it, appends a fresh
// ImageEvent, and recording continues. `t_ms` on every other event
// keeps counting only while actively recording, so a WaitEvent records
// the gap when recording resumes: `t_ms` is the active-recording
// position where the resume happened, `duration_ms` the real elapsed
// downtime. This lets analysis reconstruct true total elapsed time, and
// lets playback reproduce the same pause rather than jumping over it.
// Called "wait," not "pause," to avoid confusion with the separate,
// deliberately-rejected pause *control*.
struct WaitEvent {
    std::string type = "wait";
    std::int32_t t_ms = 0;
    std::int32_t duration_ms = 0;
};

// Marks exactly when the microphone actually starts/stops capturing
// audio — needed for transcript timing. A track's own start already
// comes from WaitEvent, but the mic can be toggled independently
// mid-track, producing more than one audio file per track; without
// this, Playback has no way to know precisely when each one begins or
// ends. `on` covers both directions with one type, matching
// LeaveEvent's "no position, just a state transition" shape.
struct MicrophoneEvent {
    std::string type = "microphone";
    std::int32_t t_ms = 0;
    bool on = false;
};

// A test step's own validation point, pressed in the Protocol window:
// each step has a validation point with an expected result, and the
// step counts as validated only once that result is reached. `scenario`
// matches ImageEvent::scenario exactly (the H2 heading text); `step` is
// the H3 heading text within that scenario — a real heading, not a
// position-dependent index.
//
// Three *subtypes*, not a separate outcome field, same convention as
// ProdEvent's head-prod/eye-prod split:
//   - "validated": the participant reached the expected result.
//   - "failed": they didn't.
//   - "back": the facilitator navigated backward to an earlier step —
//     allowed, but only reluctantly, since whatever comes after this
//     point in the log may not be representative.
// Written only during an actual recording — Interactive drives this
// same navigation for dry-run verification, but never logs it.
struct TestStepEvent {
    std::string type = "validated";
    std::int32_t t_ms = 0;
    std::string scenario;
    std::string step;
};

// Which microphones the LevelEvents after it refer to, by position: the
// Ambient Microphone ("__ambient_mic__") first, then every participant by
// name, in the same order for the whole track. Written at the start of
// each track, and again only if the participants change mid-track. `unit`
// and `scale` say how to read LevelEvent::level: integers in tenths of
// dBFS, so -382 is -38.2 dBFS.
struct SourcesEvent {
    std::string type = "sources";
    std::int32_t t_ms = 0;
    std::string unit = "dBFS";
    double scale = 0.1;
    std::vector<std::string> sources;
};

// Each microphone's level before any gain — mean energy over 50 ms, one
// event per 50 ms — in the order of the last SourcesEvent. For telling
// which microphone a voice dominates, which a levelled signal hides.
// null where there is no level: the microphone is muted or absent, or
// that participant has no microphone of their own. Not read back by
// Playback; read_record_events() skips it like any type it doesn't know.
struct LevelEvent {
    std::string type = "level";
    std::int32_t t_ms = 0;
    std::vector<std::optional<std::int16_t>> level;
};

// One line's worth of possibilities, for Playback — every event type
// has its own struct rather than one shared shape, so reading them back
// needs some way to hold "one of these, whichever this line turned out
// to be" per line, in order.
using RecordEvent = std::variant<GazeEvent, ProdEvent, ImageEvent, ViewEvent,
                                  WaitEvent, LeaveEvent, MicrophoneEvent,
                                  TestStepEvent>;

// Reads every line after the first (the type:meta line, already read
// via read_record_meta()) into an ordered sequence, dispatching each
// line by its "type" field. A line that's empty, malformed, or an
// unrecognized type is skipped rather than aborting the whole read.
std::vector<RecordEvent> read_record_events(const std::string& path);

}  // namespace blind_sight
