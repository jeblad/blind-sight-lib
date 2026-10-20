// SPDX-License-Identifier: GPL-3.0-only
//
// collection_editor.hpp — a single, model-agnostic "Edit Collection" dialog,
// shared by the project/session/image recents-list editors. The component
// only ever sees opaque string ids; everything entry-specific (what an id
// means, how it's read/written/removed) comes in through CollectionEditorConfig.
//
// Interaction model: rows are selectable, at most one at a time (plain
// click selects it; clicking the already-selected row deselects it — see
// collection_editor.cpp's own click-gesture comment for why GTK's selection
// modes don't already do this), the whole row is the hit target. Single
// selection, deliberately: visibility/destructive actions always act on
// exactly zero or one row, so there's never a "mixed state across the
// selection" case to disambiguate. A drag handle replaces Move Up/Move Down
// for dialogs that support reordering. Visibility (if the model has it) is
// a passive per-row status icon plus one header-bar button that infers its
// own direction ("Show"/"Hide") from the selected row's state.
//
// The dialog's own AdwHeaderBar (not an in-content toolbar) splits
// selection-dependent actions (left/pack_start) from selection-independent
// ones (right/pack_end): left is an optional non-destructive selection
// action (e.g. image's "Info"), then visibility, then the single
// destructive action closest to center; right is leading_actions, in
// registration order, nearest the header bar's own close button. John:
// independent actions belong next to Close, since they're the ones that
// still make sense to reach for with nothing selected — the opposite of
// collection_menu.hpp's pre-swap layout, now brought in line with it.

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

// One row's display content. `subtitle`/`tooltip` empty means "don't show."
struct CollectionRowContent {
    Glib::ustring title;
    Glib::ustring subtitle;
    Glib::ustring tooltip;
};

// A right-side (selection-independent) header-bar action — "New", "Open",
// "Import", etc. This dialog is never closed implicitly by an action —
// New/Open hand off to another (modal) dialog on top of this one, same as
// Import does, rather than closing first. `activate` receives
// `notify_changed`: call it once the action's effect has actually landed
// (which may be later, e.g. after an async file picker or sub-dialog
// returns) to refresh this dialog's own row list and fire the outer
// on_changed.
struct CollectionAction {
    std::string icon_filename;  // resource/icons/<icon_filename>
    Glib::ustring tooltip;
    std::function<void(std::function<void()> notify_changed)> activate;
};

// Visibility support for models that have it (session/image). Absent in
// CollectionEditorConfig means no status icon, no toolbar toggle at all.
struct CollectionVisibility {
    std::function<bool(const std::string& id)> is_hidden;
    std::function<void(const std::vector<std::string>& ids, bool hidden)> set_hidden;
};

// Reorder support for models that have it (session/image, not project's
// flat MRU list). `new_order_ids` is the complete id list in its new order,
// computed from the rows after a drag-drop completes.
struct CollectionReorder {
    std::function<void(const std::vector<std::string>& new_order_ids)> reorder;
};

// A left-side, selection-dependent but non-destructive action (e.g.
// image's "Info"), shown between the visibility toggle and the
// destructive action. Enabled only when exactly one row is selected — a
// future multi-select mode would otherwise need a mixed-selection answer
// this never has to give. `activate` receives the sole selected id and
// `notify_changed`, same "call it once the effect lands" contract as
// CollectionAction.
struct CollectionSelectionAction {
    std::string icon_filename;  // resource/icons/<icon_filename>
    Glib::ustring tooltip;
    std::function<void(const std::string& id, std::function<void()> notify_changed)> activate;
};

// The single right-most action — "locally destructive" within this dialog
// even when it isn't real data loss (project's Remove vs. session/image's
// real Delete). `confirm`, if set, must call `on_confirmed` itself once the
// user actually confirms; if empty, the action runs immediately on click.
// `enabled_for`, if set, disables the action for a specific id regardless of
// selection (e.g. ImageEntry::ref_count > 0) — the whole action goes
// insensitive while any selected id is disabled.
struct CollectionDestructiveAction {
    Glib::ustring label;
    std::function<bool(const std::string& id)> enabled_for;
    std::function<void(Gtk::Window* parent, const std::vector<std::string>& ids,
                        std::function<void()> on_confirmed)>
        confirm;
    std::function<void(const std::vector<std::string>& ids)> remove;
};

struct CollectionEditorConfig {
    // The dialog's own AdwHeaderBar title — "Projects"/"Sessions"/
    // "Images"/"Tracks", one per model this component is instantiated
    // for. No generic fallback; every caller sets this explicitly.
    Glib::ustring title;
    std::function<std::vector<std::string>()> list_ids;  // in display order
    std::function<CollectionRowContent(const std::string& id)> row_content;
    std::vector<CollectionAction> leading_actions;
    std::optional<CollectionReorder> reorder;
    std::optional<CollectionVisibility> visibility;
    std::optional<CollectionSelectionAction> selection_action;
    // Unset means no destructive action at all — the trash button is
    // skipped entirely (e.g. tracks, which don't have a delete yet).
    std::optional<CollectionDestructiveAction> destructive;
    std::function<void()> on_changed;  // fired after any mutation
};

void show_collection_editor(Gtk::Window* parent, CollectionEditorConfig config);
