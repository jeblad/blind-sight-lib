// SPDX-License-Identifier: GPL-3.0-only
//
// session_dialogs.cpp — see session_dialogs.hpp for scope.

#include "session_dialogs.hpp"

#include <glib/gi18n.h>

#include <algorithm>
#include <filesystem>

#include "collection_editor.hpp"
#include "collection_menu.hpp"
#include "external_change_guard.hpp"

using blind_sight::read_record_meta;
using blind_sight::read_sessions_index;
using blind_sight::RecordMeta;
using blind_sight::session_file_path;
using blind_sight::SessionEntry;
using blind_sight::update_record_meta;
using blind_sight::write_sessions_index;

// Bridges into main.cpp's anonymous namespace — these stay defined in
// main.cpp but are needed here too. confirm_delete_entry()/
// truncate_dropdown_title() are generic, shared with the image/project
// dialogs, so they stay put rather than moving with only one of their
// several callers. release_image_references() stays paired with its
// counterpart count_new_image_reference(), used far more broadly
// (RecordingSession's image-load path).
void bridge_confirm_delete_entry(Gtk::Window* parent, const Glib::ustring& title,
                                   const Glib::ustring& body,
                                   std::function<void()> on_delete);
void bridge_release_image_references(const std::string& project_dir,
                                        const std::string& session_path);
Glib::ustring bridge_truncate_dropdown_title(const Glib::ustring& title, int max_chars);
void bridge_start_new_session(std::function<void()> on_done = {});
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body);

namespace {

constexpr auto kSessionTitleCutoffCharsKey = "session-title-cutoff-chars";
constexpr auto kCurrentWorkingSessionKey = "current-working-session";

// Shared by sorted_session_entries() here and sorted_image_entries()
// (main.cpp) — both just sort by `.order`. Duplicated rather than
// shared via a header.
template <typename Entry>
std::vector<Entry> sorted_by_order(std::vector<Entry> entries) {
    std::stable_sort(entries.begin(), entries.end(),
                      [](const Entry& a, const Entry& b) { return a.order < b.order; });
    return entries;
}

}  // namespace

// The collection editor's own full, unfiltered list — every session this
// project has, ordered by its own `order` field (Move Up/Down writes this
// back), not insertion order.
std::vector<SessionEntry> sorted_session_entries(const std::string& project_dir) {
    return sorted_by_order(read_sessions_index(project_dir).sessions);
}

// The main dropdown's own filtered view — same ordering, minus whatever
// the editor's eye toggle has hidden, so normal use doesn't surface
// privacy-sensitive sessions by default.
std::vector<std::string> visible_session_paths(const std::string& project_dir) {
    std::vector<std::string> paths;
    for (const auto& entry : sorted_session_entries(project_dir)) {
        if (!entry.hidden) {
            paths.push_back(session_file_path(project_dir, entry.id));
        }
    }
    return paths;
}

// A shared "current working session" — opening a session file in either
// Playback or Record sets this, and both pick it up automatically the
// next time either is entered.
void set_current_working_session(const Glib::RefPtr<Gio::Settings>& settings,
                                  const std::string& path) {
    settings->set_string(kCurrentWorkingSessionKey, path);
}

std::string get_current_working_session(const Glib::RefPtr<Gio::Settings>& settings) {
    return settings->get_string(kCurrentWorkingSessionKey);
}

// The identifying text for a session row/button — `creator`, falling
// back to the protocol-derived `title`, then the bare filename: a
// session with an empty creator but a real title should still show
// something more useful than the raw path.
std::string session_creator_or_fallback(const RecordMeta& meta, const std::string& path) {
    if (!meta.creator.empty()) {
        return meta.creator;
    }
    if (!meta.title.empty()) {
        return meta.title;
    }
    return Glib::path_get_basename(path);
}

// "[creator] – [contributor]", en dash rather than a colon since creator
// and contributor are peers, not a label/value pair. contributor appended
// only when creator itself (not a title/filename fallback) is present, so
// a fallback name never gets an unrelated contributor list tacked on.
// Shared by session_button_label() (truncated, for a fixed-width button
// face) and session_creator_contributor_text() (untruncated, for a
// popover row's own wrapping subtitle).
std::string session_creator_contributor_label(const RecordMeta& meta,
                                                const std::string& path) {
    std::string label = session_creator_or_fallback(meta, path);
    if (!meta.creator.empty() && !meta.contributor.empty()) {
        label += " – " + meta.contributor;
    }
    return label;
}

// The session dropdown's button-face label — truncated per session-
// title-cutoff-chars, same treatment as the project dropdown, since a
// fixed-width button has nowhere to wrap to.
Glib::ustring session_button_label(const RecordMeta& meta, const std::string& path,
                                     const Glib::RefPtr<Gio::Settings>& settings) {
    if (path.empty()) {
        return _("No Session");
    }
    return bridge_truncate_dropdown_title(
        session_creator_contributor_label(meta, path),
        settings->get_int(kSessionTitleCutoffCharsKey));
}

// Same composition, untruncated — for the recents popover's own row
// list, where the subtitle wraps across multiple lines instead of being
// cut to a fixed character budget.
Glib::ustring session_creator_contributor_text(const RecordMeta& meta,
                                                 const std::string& path) {
    if (path.empty()) {
        return "";
    }
    return session_creator_contributor_label(meta, path);
}

// The session's own title (RecordMeta::title, freely editable — see
// show_record_meta_dialog()'s own comment, main.cpp) is now the primary
// identifying text; session_button_label()'s creator/contributor
// composition is the fallback for a session with no title set yet, same
// as it already was one layer further down inside session_creator_or_
// fallback().
Glib::ustring session_title_or_fallback(const RecordMeta& meta, const std::string& path,
                                          const Glib::RefPtr<Gio::Settings>& settings) {
    if (!meta.title.empty()) {
        return meta.title;
    }
    return session_button_label(meta, path, settings);
}

// The session dropdown's "Edit Collection…" — reorder (drag handle),
// visibility (passive status icon + toolbar smart-toggle: open means the
// session shows in the dropdown's recents list; closed/crossed hides it
// there without deleting it, for privacy between test participants), and
// real delete (with confirmation — a session never lives anywhere else to
// ask about, unlike project's list-only "Remove").
//
// No "Add Session…" leading action — there's nothing to add: a session is
// created once, by "New Session," and always lives in sessions/.
void show_session_collection_editor(Gtk::Window* parent,
                                      const Glib::RefPtr<Gio::Settings>& settings,
                                      const std::string& project_dir,
                                      std::function<void()> on_changed) {
    CollectionEditorConfig config;
    config.title = _("Sessions");
    config.list_ids = [project_dir]() {
        std::vector<std::string> ids;
        for (const auto& entry : sorted_session_entries(project_dir)) {
            ids.push_back(entry.id);
        }
        return ids;
    };
    config.row_content = [project_dir, settings](const std::string& id) {
        std::string path = session_file_path(project_dir, id);
        RecordMeta meta;
        read_record_meta(path, meta);
        CollectionRowContent content;
        content.title = session_title_or_fallback(meta, path, settings);
        content.subtitle = session_button_label(meta, path, settings);
        return content;
    };
    config.leading_actions.push_back(CollectionAction{
        "plus-symbolic.svg", _("New Session"),
        [](std::function<void()> notify_changed) {
            bridge_start_new_session(notify_changed);
        }});
    config.reorder = CollectionReorder{
        [project_dir](const std::vector<std::string>& new_order_ids) {
            auto index = read_sessions_index(project_dir);
            for (std::size_t i = 0; i < new_order_ids.size(); ++i) {
                for (auto& session : index.sessions) {
                    if (session.id == new_order_ids[i]) {
                        session.order = static_cast<int>(i);
                        break;
                    }
                }
            }
            write_sessions_index(project_dir, index);
        }};
    config.visibility = CollectionVisibility{
        [project_dir](const std::string& id) {
            for (const auto& session : read_sessions_index(project_dir).sessions) {
                if (session.id == id) {
                    return session.hidden;
                }
            }
            return false;
        },
        [project_dir](const std::vector<std::string>& ids, bool hidden) {
            auto index = read_sessions_index(project_dir);
            for (auto& session : index.sessions) {
                if (std::find(ids.begin(), ids.end(), session.id) != ids.end()) {
                    session.hidden = hidden;
                }
            }
            write_sessions_index(project_dir, index);
        }};
    config.destructive.emplace();
    config.destructive->label = _("Delete Session");
    config.destructive->confirm = [project_dir, settings](
                                      Gtk::Window* confirm_parent,
                                      const std::vector<std::string>& ids,
                                      std::function<void()> on_confirmed) {
        if (ids.empty()) {
            return;
        }
        std::string path = session_file_path(project_dir, ids.front());
        RecordMeta meta;
        read_record_meta(path, meta);
        Glib::ustring title = session_title_or_fallback(meta, path, settings);
        Glib::ustring body = Glib::ustring::compose(
            _("“%1” and every recording in it will be permanently "
              "deleted. This can't be undone."),
            title);
        bridge_confirm_delete_entry(confirm_parent, _("Delete Session?"), body, on_confirmed);
    };
    config.destructive->remove = [project_dir](const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            auto index = read_sessions_index(project_dir);
            auto& sessions = index.sessions;
            sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                           [&id](const SessionEntry& entry) {
                                               return entry.id == id;
                                           }),
                            sessions.end());
            write_sessions_index(project_dir, index);
            // Reads the event stream before the directory under it goes
            // away — the counterpart to count_new_image_reference(),
            // releasing every image this session held a reference to.
            bridge_release_image_references(project_dir, session_file_path(project_dir, id));
            std::error_code ec;
            std::filesystem::remove_all(project_dir + "/sessions/" + id, ec);
        }
    };
    config.on_changed = on_changed;
    show_collection_editor(parent, std::move(config));
}

// Title/Creator/Contributor/Rights are typed; Date is always read-only
// (the caller decides what `initial.date` means: "now" for a brand-new
// recording, the original value when editing one). AdwEntryRow/
// AdwActionRow have no gtkmm wrapper, so they're built via the C API and
// folded into the Gtk::ListBox with Glib::wrap().
void show_record_meta_dialog(Gtk::Window* parent, const RecordMeta& initial,
                               std::function<void(const RecordMeta&)> on_continue,
                               const std::string& path) {
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, _("Recording Details"));
    adw_dialog_set_content_width(dialog, 420);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::NONE);

    // The session's own title — pre-filled from the active protocol's H1
    // when this session was started under one, but always editable: this
    // is the session file's own title, not a locked mirror of the
    // protocol's. Session-to-protocol linkage is tracked separately via
    // protocol_file (the path), not by keeping this text identical.
    auto* title_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(title_row), _("Title"));
    gtk_editable_set_text(GTK_EDITABLE(title_row), initial.title.c_str());
    list_box->append(*Glib::wrap(title_row, false));

    auto* creator_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(creator_row), _("Creator"));
    gtk_editable_set_text(GTK_EDITABLE(creator_row), initial.creator.c_str());
    list_box->append(*Glib::wrap(creator_row, false));

    auto* contributor_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(contributor_row),
                                    _("Contributor"));
    gtk_editable_set_text(GTK_EDITABLE(contributor_row),
                            initial.contributor.c_str());
    list_box->append(*Glib::wrap(contributor_row, false));

    auto* date_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(date_row), _("Date"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(date_row), initial.date.c_str());
    list_box->append(*Glib::wrap(date_row, false));

    // Free text, not a fixed set — the two suggested values are hinted
    // right in the title (AdwEntryRow has no subtitle, unlike
    // AdwActionRow), but anything can be typed here.
    auto* rights_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(rights_row),
                                    _("Rights (e.g. Confidential, Anonymize)"));
    gtk_editable_set_text(GTK_EDITABLE(rights_row), initial.rights.c_str());
    list_box->append(*Glib::wrap(rights_row, false));

    // Read-only — where session.bs itself actually lives on disk. Empty
    // for a brand-new session, whose path doesn't exist yet. Frees the
    // popover/editor row's own tooltip to show the subtitle instead of
    // this path.
    auto* location_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(location_row), _("Location"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(location_row), path.c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(location_row), 0);
    list_box->append(*Glib::wrap(location_row, false));

    box->append(*list_box);

    // Description needs multiple lines, which AdwEntryRow doesn't give — a
    // plain Gtk::TextView (full gtkmm binding, no C-API interop needed
    // here).
    auto* description_label = Gtk::make_managed<Gtk::Label>(_("Description"));
    description_label->set_halign(Gtk::Align::START);
    box->append(*description_label);

    auto description_buffer = Gtk::TextBuffer::create();
    description_buffer->set_text(initial.description);
    auto* description_view = Gtk::make_managed<Gtk::TextView>(description_buffer);
    description_view->set_wrap_mode(Gtk::WrapMode::WORD);

    auto* description_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    description_scroller->set_policy(Gtk::PolicyType::NEVER,
                                       Gtk::PolicyType::AUTOMATIC);
    description_scroller->set_min_content_height(96);
    description_scroller->set_child(*description_view);

    auto* description_frame = Gtk::make_managed<Gtk::Frame>();
    description_frame->set_child(*description_scroller);
    box->append(*description_frame);

    auto* button_box =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    button_box->set_halign(Gtk::Align::END);

    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    auto* continue_button = Gtk::make_managed<Gtk::Button>(_("Continue"));
    continue_button->add_css_class("suggested-action");
    button_box->append(*cancel_button);
    button_box->append(*continue_button);
    box->append(*button_box);

    adw_dialog_set_child(dialog, GTK_WIDGET(box->gobj()));

    cancel_button->signal_clicked().connect(
        [dialog]() { adw_dialog_close(dialog); });

    continue_button->signal_clicked().connect(
        [dialog, on_continue, title_row, creator_row, contributor_row,
         rights_row, description_buffer, date = initial.date,
         protocol_file = initial.protocol_file]() {
            RecordMeta meta;
            meta.title = gtk_editable_get_text(GTK_EDITABLE(title_row));
            meta.creator = gtk_editable_get_text(GTK_EDITABLE(creator_row));
            meta.contributor =
                gtk_editable_get_text(GTK_EDITABLE(contributor_row));
            meta.rights = gtk_editable_get_text(GTK_EDITABLE(rights_row));
            meta.description = description_buffer->get_text();
            meta.date = date;
            meta.protocol_file = protocol_file;
            adw_dialog_close(dialog);
            on_continue(meta);
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

// The session dropdown's popover. New Session/Edit Collection mirror
// show_session_collection_editor()'s own leading actions above (same
// bridge for New, same error-dialog-on-no-project guard for Edit
// Collection); Session Details and row activation are new here — the
// Edit Collection dialog has neither a per-row "open this session"
// action nor a Details button of its own.
SessionMenuHandle show_session_collection_menu(
    Gtk::MenuButton* button, Gtk::Window* window,
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::shared_ptr<blind_sight::Protocol>& active_protocol,
    std::function<std::string()> current_session_path,
    std::function<bool(const std::string&)> apply_shared_session,
    std::function<void(const std::string&)> refresh_session_button_label,
    std::function<void()> refresh_transport_sensitivity,
    std::function<void()> on_collection_edited, AdwToastOverlay* toast_overlay) {
    // Same dir-from-source_path logic as main.cpp's own
    // project_dir_from_protocol() — duplicated rather than bridged, same
    // "trivial and pure enough" choice project_dialogs.cpp already made.
    auto project_dir = [active_protocol]() {
        return active_protocol->source_path.empty()
                   ? std::string()
                   : Glib::path_get_dirname(active_protocol->source_path);
    };

    CollectionMenuConfig config;
    // Hidden sessions (the collection editor's eye toggle) are filtered
    // out here specifically — a facilitator can hide a previous
    // participant's session from this dropdown without losing it from
    // sessions.json or the collection editor's own (unfiltered) list.
    config.list_ids = [project_dir]() {
        std::string dir = project_dir();
        return dir.empty() ? std::vector<std::string>{} : visible_session_paths(dir);
    };
    config.row_content = [settings](const std::string& path) {
        RecordMeta meta;
        read_record_meta(path, meta);
        CollectionRowContent content;
        content.title = session_title_or_fallback(meta, path, settings);
        content.subtitle = session_creator_contributor_text(meta, path);
        return content;
    };
    config.is_current = [current_session_path](const std::string& path) {
        return path == current_session_path();
    };
    config.activate = [apply_shared_session, refresh_session_button_label,
                        refresh_transport_sensitivity, toast_overlay](const std::string& path) {
        if (apply_shared_session(path)) {
            refresh_session_button_label(path);
            refresh_transport_sensitivity();
        } else {
            // Malformed/wrong-version file — posts a toast rather than
            // silently doing nothing.
            AdwToast* toast =
                adw_toast_new(_("Can't open recording — wrong or corrupted format"));
            adw_toast_overlay_add_toast(toast_overlay, toast);
        }
    };
    config.leading_actions.push_back(CollectionAction{
        "plus-symbolic.svg", _("New Session"),
        [](std::function<void()> notify_changed) {
            bridge_start_new_session(notify_changed);
        }});
    config.leading_actions.push_back(CollectionAction{
        "reorder-symbolic.svg", _("Edit Collection…"),
        [window, settings, project_dir,
         on_collection_edited](std::function<void()> notify_changed) {
            std::string dir = project_dir();
            if (dir.empty()) {
                bridge_show_error_dialog(window, _("No Project"),
                                           _("Open or create a project first — "
                                             "sessions always live inside a project."));
                return;
            }
            show_session_collection_editor(
                window, settings, dir,
                [notify_changed, on_collection_edited]() {
                    notify_changed();
                    on_collection_edited();
                });
        }});
    // Always editable, writing back via update_record_meta() — one
    // shared session makes editing the only sensible default.
    config.details_action = CollectionAction{
        "info-outline-symbolic.svg", _("Session Details"),
        [window, current_session_path,
         refresh_session_button_label](std::function<void()> /*notify_changed*/) {
            std::string path = current_session_path();
            RecordMeta meta;
            if (path.empty() || !read_record_meta(path, meta)) {
                return;
            }
            auto change_guard = std::make_shared<ExternalChangeGuard>(path);
            show_record_meta_dialog(
                window, meta,
                [window, path, refresh_session_button_label, meta,
                 change_guard](const RecordMeta& updated) {
                    // Skip the write entirely if nothing editable
                    // actually changed — an extra safety net against
                    // overwriting the file for what would otherwise be a
                    // no-op (date/protocol_file are never user-edited, so
                    // they're deliberately excluded from this
                    // comparison).
                    if (updated.title == meta.title && updated.creator == meta.creator &&
                        updated.contributor == meta.contributor &&
                        updated.rights == meta.rights &&
                        updated.description == meta.description) {
                        return;
                    }
                    if (change_guard->has_changed()) {
                        bridge_show_error_dialog(
                            window, _("Couldn't Save"),
                            _("This session file was changed by something "
                              "else while this dialog was open — your edit "
                              "was not saved. Reopen and try again."));
                        return;
                    }
                    update_record_meta(path, updated);
                    refresh_session_button_label(path);
                },
                path);
        }};

    auto handle = show_collection_menu(button, config);
    SessionMenuHandle result;
    result.refresh_session_list = handle.refresh_list;
    result.session_details_button = handle.details_button;
    return result;
}
