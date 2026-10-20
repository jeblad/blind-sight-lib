// SPDX-License-Identifier: GPL-3.0-only
//
// sessions.hpp — data model and file I/O for a project's sessions.json.
//
// A session always lives inside its own project, at
// <project_dir>/sessions/<id>/session.bs. This index, not gsettings, is
// authoritative for a session's order/visibility: gsettings is
// this-machine-only and doesn't travel with the project directory the
// way a file inside it does, the same role protocol.md already has.
//
// Deliberately a single small JSON document, not JSON Lines like
// recording.hpp's .bs format — this is a whole-file index rewritten
// each time it changes, not an append-only event log.

#pragma once

#include <string>
#include <vector>

namespace blind_sight {

constexpr int kSessionsIndexFormatVersion = 1;

// One session's entry in sessions.json. `id` is the session's directory
// name under <project_dir>/sessions/ — an opaque hash, unlike the
// project directory's slug-based naming. The full path is always
// "<project_dir>/sessions/<id>/session.bs", so it's never stored here
// separately.
//
// `order` is this entry's explicit position in the session collection
// editor's list (Move Up/Move Down) — a real field rather than relying
// on array order surviving every read/write round-trip. `hidden` is the
// editor's privacy toggle.
struct SessionEntry {
    std::string id;
    int order = 0;
    bool hidden = false;
};

struct SessionsIndex {
    int version = kSessionsIndexFormatVersion;
    std::vector<SessionEntry> sessions;
};

// Reads <project_dir>/sessions.json — an empty SessionsIndex (not
// std::nullopt, and not a hard failure) if the file doesn't exist yet or
// fails to parse, since a project with no sessions recorded yet is a
// perfectly normal state, not an error.
SessionsIndex read_sessions_index(const std::string& project_dir);

// Writes sessions.json back out, overwriting whatever was there.
bool write_sessions_index(const std::string& project_dir, const SessionsIndex& index);

// Generates a new, not-yet-used session ID for this project — a hash of
// the current time plus per-call entropy, retried if it happens to
// collide with an existing <project_dir>/sessions/<id> directory
// (astronomically unlikely, but checked rather than assumed).
std::string generate_session_id(const std::string& project_dir);

// The one place "<project_dir>/sessions/<id>/session.bs" gets spelled out,
// so main.cpp never hand-assembles it.
std::string session_file_path(const std::string& project_dir, const std::string& id);

}  // namespace blind_sight
