// SPDX-License-Identifier: GPL-3.0-only
//
// track_dialogs.cpp — see track_dialogs.hpp for scope.

#include "track_dialogs.hpp"

#include <adwaita.h>
#include <glib/gi18n.h>

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "audio.hpp"
#include "collection_editor.hpp"
#include "recording.hpp"
#include "tracks.hpp"

using blind_sight::read_record_events;
using blind_sight::read_tracks_index;
using blind_sight::RecordEvent;
using blind_sight::TrackEntry;
using blind_sight::TracksIndex;
using blind_sight::write_tracks_index;

// Bridges into main.cpp's anonymous namespace — image_info_for_track()
// has internal linkage; only its `scenario` field is exposed.
std::string bridge_scenario_for_track(const std::vector<RecordEvent>& events,
                                       std::int32_t track_start_ms, std::int32_t track_end_ms);

namespace {

// Minutes (unpadded) / seconds (zero-padded) for the row title's "(%3:%4)"
// — same split main.cpp's own format_clock_ms() uses, duplicated rather
// than bridged (that one's a lambda local to on_activate(), not a
// linkable function).
std::pair<int, std::string> minutes_seconds(std::int32_t ms) {
    int total_seconds = ms / 1000;
    int minutes = total_seconds / 60;
    int seconds = total_seconds % 60;
    std::string padded_seconds = (seconds < 10 ? "0" : "") + std::to_string(seconds);
    return {minutes, padded_seconds};
}

std::string legacy_id(int track_number) {
    return "legacy:" + std::to_string(track_number);
}

// Parses "legacy:N" -> N, or -1 if `id` isn't that shape.
int legacy_track_number(const std::string& id) {
    constexpr std::string_view kPrefix = "legacy:";
    if (id.size() <= kPrefix.size() || id.compare(0, kPrefix.size(), kPrefix) != 0) {
        return -1;
    }
    try {
        return std::stoi(id.substr(kPrefix.size()));
    } catch (...) {
        return -1;
    }
}

// The opaque id for a given recording position — the real TrackEntry::id
// when one exists and is non-empty (a track minted after this feature
// shipped), else the synthesized legacy id — covers both an untouched
// legacy track and one already upgraded via a previous hide/reorder/note
// edit, since an upgraded legacy entry's own `id` field deliberately
// stays empty (see tracks.hpp's own comment).
std::string id_for_position(const TracksIndex& index, int track_number) {
    for (const auto& entry : index.tracks) {
        if (entry.track_number == track_number && !entry.id.empty()) {
            return entry.id;
        }
    }
    return legacy_id(track_number);
}

const TrackEntry* find_entry(const TracksIndex& index, const std::string& id) {
    int legacy_n = legacy_track_number(id);
    for (const auto& entry : index.tracks) {
        if (legacy_n >= 0 ? entry.track_number == legacy_n : entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

// Finds `id`'s entry in `index`, creating it (as an "upgraded legacy"
// entry — empty id, just metadata) if it doesn't exist yet. Only ever
// needed for a legacy id; a real minted id's entry always already
// exists (created at record time, main.cpp's mint_new_track()).
TrackEntry& find_or_create_entry(TracksIndex& index, const std::string& id) {
    int legacy_n = legacy_track_number(id);
    for (auto& entry : index.tracks) {
        if (legacy_n >= 0 ? entry.track_number == legacy_n : entry.id == id) {
            return entry;
        }
    }
    TrackEntry entry;
    entry.track_number = legacy_n >= 0 ? legacy_n : 0;
    entry.order = entry.track_number - 1;
    index.tracks.push_back(entry);
    return index.tracks.back();
}

// Title (an optional override of the track's "stikktittel") + a
// separate, unrelated free-text Note. Title starts empty, always — never
// auto-populated from the protocol's scenario, not even once at record
// time (see TrackEntry::title's own comment, tracks.hpp): filling it in
// is entirely the user's call, for a track that needs something "mer
// spesifikt" than the live derivation already shown. "(Optional)" baked
// directly into the row's own title text — AdwEntryRow has no subtitle
// to hold a separate hint (see session_dialogs.cpp's identical rights_row
// precedent). Note is its own thing — general free-text commentary,
// never meant to stand in for Title — and keeps the same TextBuffer/
// TextView/Frame shape show_record_meta_dialog() already uses for
// RecordMeta::description.
void show_track_note_dialog(Gtk::Window* parent, const std::string& session_dir,
                             const std::string& id, std::function<void()> notify_changed) {
    auto index = read_tracks_index(session_dir);
    const TrackEntry* existing = find_entry(index, id);
    std::string current_title = existing ? existing->title : "";
    std::string current_note = existing ? existing->note : "";

    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, _("Track Note"));
    adw_dialog_set_content_width(dialog, 420);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* title_list_box = Gtk::make_managed<Gtk::ListBox>();
    title_list_box->add_css_class("boxed-list");
    title_list_box->set_selection_mode(Gtk::SelectionMode::NONE);
    auto* title_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(title_row), _("Title (Optional)"));
    gtk_editable_set_text(GTK_EDITABLE(title_row), current_title.c_str());
    title_list_box->append(*Glib::wrap(title_row, false));
    box->append(*title_list_box);

    // General commentary, distinct from Title above — shown as the row's
    // own GTK subtitle in the Edit Tracks list (row_content(), below),
    // but that's just where it happens to render, not a claim that this
    // field *is* the track's stikktittel (Title already is that).
    auto* note_label = Gtk::make_managed<Gtk::Label>(_("Note"));
    note_label->set_halign(Gtk::Align::START);
    box->append(*note_label);

    auto note_buffer = Gtk::TextBuffer::create();
    note_buffer->set_text(current_note);
    auto* note_view = Gtk::make_managed<Gtk::TextView>(note_buffer);
    note_view->set_wrap_mode(Gtk::WrapMode::WORD);

    auto* note_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    note_scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    note_scroller->set_min_content_height(96);
    note_scroller->set_child(*note_view);

    auto* note_frame = Gtk::make_managed<Gtk::Frame>();
    note_frame->set_child(*note_scroller);
    box->append(*note_frame);

    // Read-only — where this track's own files actually live: its own
    // directory for a minted-id track, else the session directory
    // itself, where a legacy track's flat "recording.trackN.*" files
    // sit. Frees the popover/editor row's own tooltip to show the
    // subtitle instead of this path.
    std::string location = (existing && !existing->id.empty())
                                ? session_dir + "/tracks/" + existing->id
                                : session_dir;
    auto* location_list_box = Gtk::make_managed<Gtk::ListBox>();
    location_list_box->add_css_class("boxed-list");
    location_list_box->set_selection_mode(Gtk::SelectionMode::NONE);
    auto* location_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(location_row), _("Location"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(location_row), location.c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(location_row), 0);
    location_list_box->append(*Glib::wrap(location_row, false));
    box->append(*location_list_box);

    auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    button_box->set_halign(Gtk::Align::END);
    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    auto* save_button = Gtk::make_managed<Gtk::Button>(_("Save"));
    save_button->add_css_class("suggested-action");
    button_box->append(*cancel_button);
    button_box->append(*save_button);
    box->append(*button_box);

    adw_dialog_set_child(dialog, GTK_WIDGET(box->gobj()));

    cancel_button->signal_clicked().connect([dialog]() { adw_dialog_close(dialog); });

    save_button->signal_clicked().connect(
        [dialog, session_dir, id, title_row, note_buffer, current_title, current_note,
         notify_changed]() {
            std::string new_title = gtk_editable_get_text(GTK_EDITABLE(title_row));
            std::string new_note = note_buffer->get_text();
            adw_dialog_close(dialog);
            // Skip the write entirely if Save was clicked without
            // actually changing anything — same safety net every other
            // Details/Note dialog in this app already has.
            if (new_title == current_title && new_note == current_note) {
                return;
            }
            auto index = read_tracks_index(session_dir);
            auto& entry = find_or_create_entry(index, id);
            entry.title = new_title;
            entry.note = new_note;
            write_tracks_index(session_dir, index);
            notify_changed();
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

}  // namespace

void show_track_note_dialog_for_number(Gtk::Window* parent, const std::string& session_path,
                                        int track_number, std::function<void()> on_changed) {
    std::string session_dir = Glib::path_get_dirname(session_path);
    auto index = read_tracks_index(session_dir);
    show_track_note_dialog(parent, session_dir, id_for_position(index, track_number),
                            on_changed);
}

void show_track_collection_editor(Gtk::Window* parent, const std::string& session_path,
                                   std::function<void()> on_changed) {
    std::string session_dir = Glib::path_get_dirname(session_path);
    // Read once, for the dialog's whole lifetime — list_ids()/row_content()
    // both need it, and the event log doesn't change while this is open
    // (Edit Tracks is only ever reached from Playback).
    std::vector<RecordEvent> events = read_record_events(session_path);
    std::vector<std::int32_t> track_starts = compute_track_starts(events);
    int track_count = static_cast<int>(track_starts.size());

    CollectionEditorConfig config;
    config.title = _("Tracks");
    config.list_ids = [session_dir, track_count]() {
        auto index = read_tracks_index(session_dir);
        // (effective order, id) — "effective" because a position with no
        // entry yet (legacy, untouched) defaults to its own natural
        // position, same as a freshly-minted entry's own default order.
        std::vector<std::pair<int, std::string>> ordered;
        for (int n = 1; n <= track_count; ++n) {
            std::string id = id_for_position(index, n);
            const TrackEntry* entry = find_entry(index, id);
            ordered.push_back({entry ? entry->order : n - 1, id});
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::string> ids;
        ids.reserve(ordered.size());
        for (auto& [order, id] : ordered) {
            ids.push_back(id);
        }
        return ids;
    };
    config.row_content = [session_dir, events, track_starts](const std::string& id) {
        CollectionRowContent content;
        auto index = read_tracks_index(session_dir);
        const TrackEntry* entry = find_entry(index, id);
        int track_number = entry ? entry->track_number : legacy_track_number(id);
        bool valid_position =
            track_number >= 1 && static_cast<std::size_t>(track_number) <= track_starts.size();
        std::int32_t start_ms = valid_position
                                     ? track_starts[static_cast<std::size_t>(track_number) - 1]
                                     : 0;
        auto [minutes, seconds] = minutes_seconds(start_ms);
        // Title is purely the user's own override (TrackEntry::title) —
        // never the live scenario derivation, which lives in the
        // subtitle instead, below. Collapses to the no-title form when
        // unset, rather than leaving a dangling "– " — a title is
        // explicitly optional (see tracks.hpp's own comment), so an
        // empty one isn't a degraded case to patch around, it's a
        // normal state.
        content.title = (entry && !entry->title.empty())
                             ? Glib::ustring::compose(_("Track %1 – %2 (%3:%4)"), track_number,
                                                       entry->title, minutes, seconds)
                             : Glib::ustring::compose(_("Track %1 (%2:%3)"), track_number,
                                                       minutes, seconds);
        // Subtitle is the live H2 derivation — a reference to what it's
        // derived from, never stored. Empty (no subtitle at all) when
        // this track has no scenario tie. Note (TrackEntry::note) has
        // no row representation at all — it only ever shows inside the
        // info dialog itself.
        if (valid_position) {
            std::size_t i = static_cast<std::size_t>(track_number) - 1;
            std::int32_t end_ms = (i + 1 < track_starts.size())
                                       ? track_starts[i + 1]
                                       : std::numeric_limits<std::int32_t>::max();
            content.subtitle = bridge_scenario_for_track(events, start_ms, end_ms);
        }
        return content;
    };
    config.visibility = CollectionVisibility{
        [session_dir](const std::string& id) {
            auto index = read_tracks_index(session_dir);
            const TrackEntry* entry = find_entry(index, id);
            return entry ? entry->hidden : false;
        },
        [session_dir](const std::vector<std::string>& ids, bool hidden) {
            auto index = read_tracks_index(session_dir);
            for (const auto& id : ids) {
                find_or_create_entry(index, id).hidden = hidden;
            }
            write_tracks_index(session_dir, index);
        }};
    // View order only — never touches a file, same contract session's
    // own CollectionReorder already proves safe (session_dialogs.cpp).
    config.reorder = CollectionReorder{
        [session_dir](const std::vector<std::string>& new_order_ids) {
            auto index = read_tracks_index(session_dir);
            for (std::size_t i = 0; i < new_order_ids.size(); ++i) {
                find_or_create_entry(index, new_order_ids[i]).order = static_cast<int>(i);
            }
            write_tracks_index(session_dir, index);
        }};
    config.selection_action = CollectionSelectionAction{
        "info-outline-symbolic.svg", _("Note"),
        [parent, session_dir](const std::string& id, std::function<void()> notify_changed) {
            show_track_note_dialog(parent, session_dir, id, notify_changed);
        }};
    // No `destructive` — Delete Track is explicitly deferred (see
    // track_dialogs.hpp's own comment); collection_editor.hpp skips the
    // trash button entirely when this is unset.
    config.on_changed = on_changed;
    show_collection_editor(parent, std::move(config));
}
