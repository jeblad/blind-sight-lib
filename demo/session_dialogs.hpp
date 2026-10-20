// SPDX-License-Identifier: GPL-3.0-only
//
// session_dialogs.hpp — the session-management UI: the project-scoped
// sessions.json list (sorted/visible views), the "current working
// session" gsettings pointer, session dropdown labels, and the "Edit
// Collection" editor (reorder/hide/delete).

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <functional>
#include <string>
#include <vector>

#include "images.hpp"
#include "protocol.hpp"
#include "recording.hpp"
#include "sessions.hpp"

// The collection editor's own full, unfiltered list — every session this
// project has, ordered by its own `order` field.
std::vector<blind_sight::SessionEntry> sorted_session_entries(
    const std::string& project_dir);
// The main dropdown's own filtered view — same ordering, minus whatever
// the editor's eye toggle has hidden.
std::vector<std::string> visible_session_paths(const std::string& project_dir);

void set_current_working_session(const Glib::RefPtr<Gio::Settings>& settings,
                                  const std::string& path);
std::string get_current_working_session(const Glib::RefPtr<Gio::Settings>& settings);

// The identifying text for a session row/button — `creator`, falling
// back to the protocol-derived `title`, then the bare filename.
std::string session_creator_or_fallback(const blind_sight::RecordMeta& meta,
                                         const std::string& path);
// The session dropdown's own button-face label.
Glib::ustring session_button_label(const blind_sight::RecordMeta& meta,
                                    const std::string& path,
                                    const Glib::RefPtr<Gio::Settings>& settings);
// Same creator/contributor composition as session_button_label(), but
// untruncated — for the recents popover's own wrapping row subtitle.
Glib::ustring session_creator_contributor_text(const blind_sight::RecordMeta& meta,
                                                const std::string& path);
// The session's own title, falling back to session_button_label()'s
// creator/contributor composition when no title is set.
Glib::ustring session_title_or_fallback(const blind_sight::RecordMeta& meta,
                                         const std::string& path,
                                         const Glib::RefPtr<Gio::Settings>& settings);

void show_session_collection_editor(Gtk::Window* parent,
                                     const Glib::RefPtr<Gio::Settings>& settings,
                                     const std::string& project_dir,
                                     std::function<void()> on_changed = {});

// Record's metadata dialog — Dublin Core field names throughout (see
// RecordMeta's own comment, recording.hpp). Shared by both Record action
// bar paths — creating a brand-new session and editing an existing one's
// metadata — pre-filled from `initial` either way, with `on_continue`
// deciding what happens to the result (write_record_meta() for a new
// file, update_record_meta() for an edit); the dialog itself doesn't know
// or care which. `path`, when known, fills the read-only Location row —
// "" (the default) for a brand-new session, whose path doesn't exist
// yet at dialog-open time.
void show_record_meta_dialog(Gtk::Window* parent, const blind_sight::RecordMeta& initial,
                              std::function<void(const blind_sight::RecordMeta&)> on_continue,
                              const std::string& path = "");

// Returns what main.cpp needs by name — refresh_session_list for the
// existing call sites that already trigger it, and the Details button
// itself (via collection_menu.hpp's own CollectionMenuHandle) so main.cpp
// can keep gating its sensitivity from refresh_transport_sensitivity(),
// the same way it does today.
struct SessionMenuHandle {
    std::function<void()> refresh_session_list;
    Gtk::Button* session_details_button = nullptr;
};

// The session dropdown's popover — recent (visible, unhidden) sessions,
// most-recent-order from sorted_session_entries(), New Session/Edit
// Collection on the left, Session Details on the right, a checkmark on
// whichever session is current.
//
// RecordingSession/PlaybackSession are main.cpp-local types (defined
// there, not in any header) — rather than naming them here, this takes
// plain callbacks/values instead, same "bridge via plain types, never an
// internal-linkage struct" rule this codebase already follows at every
// other file boundary:
//   - `current_session_path` reads recording_session->session_path live.
//   - `apply_shared_session`/`refresh_session_button_label`/
//     `refresh_transport_sensitivity` are main.cpp's own existing
//     closures (apply_shared_session's bool return distinguishes a
//     malformed/wrong-version file, reported via a toast on
//     `toast_overlay` — row activation is the only path that can fail
//     this way, unlike New/Open elsewhere).
//   - `on_collection_edited` is called after Edit Collection's own
//     changes land (hiding/reordering/deleting) — main.cpp's own
//     "did the currently-loaded session just get deleted out from under
//     Record/Playback" cleanup, which needs types this function never
//     sees.
SessionMenuHandle show_session_collection_menu(
    Gtk::MenuButton* button, Gtk::Window* window,
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::shared_ptr<blind_sight::Protocol>& active_protocol,
    std::function<std::string()> current_session_path,
    std::function<bool(const std::string&)> apply_shared_session,
    std::function<void(const std::string&)> refresh_session_button_label,
    std::function<void()> refresh_transport_sensitivity,
    std::function<void()> on_collection_edited, AdwToastOverlay* toast_overlay);
