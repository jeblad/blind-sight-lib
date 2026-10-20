// SPDX-License-Identifier: GPL-3.0-only
//
// track_dialogs.hpp — the "Edit Tracks…" collection editor: hide, reorder
// (view order only — never touches a file), and a free-text note per
// track. Deliberately no delete yet — see tracks.hpp's own comment and
// notes.md for why (no event in the log carries a track number today,
// and images.json's ref_count is only tracked per session, not per
// track).

#pragma once

#include <gtkmm.h>

#include <functional>
#include <string>

// Opens the Edit Tracks dialog for one session. Every track in the
// session's event log gets a row — real, minted-id tracks and legacy
// (pre-feature) ones alike, see tracks.hpp's own comment on how a
// legacy track's opaque id is synthesized. `on_changed` fires after any
// hide/reorder/note edit lands.
void show_track_collection_editor(Gtk::Window* parent, const std::string& session_path,
                                   std::function<void()> on_changed = {});

// The track popover's own right-side "Details" button, mirroring
// session/image/project's — opens the same single-field Note editor
// Edit Tracks' "Note" action uses, directly for one track by its
// recording-position number, without going through Edit Tracks first.
void show_track_note_dialog_for_number(Gtk::Window* parent, const std::string& session_path,
                                        int track_number,
                                        std::function<void()> on_changed = {});
