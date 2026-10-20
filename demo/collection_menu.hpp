// SPDX-License-Identifier: GPL-3.0-only
//
// collection_menu.hpp — a single, model-agnostic header-bar dropdown menu,
// shared by the project/session/image recents popovers. Same spirit as
// collection_editor.hpp's show_collection_editor() — this component only
// ever sees opaque string ids, with CollectionRowContent/CollectionAction
// (collection_editor.hpp) reused directly rather than redefined, since
// they're already exactly the right shape.
//
// Unlike the Edit Collection dialog, this is a persistent widget refreshed
// repeatedly for the app's whole lifetime: the popover rebuilds its row
// list fresh every time it's shown (list_ids()/row_content() are called
// again then), same "cheap enough to just rebuild" convention every
// existing popover in this app already uses.
//
// Header row: left = the single optional details_action, implicitly
// acting on "whichever item is currently current" — never a specific row
// click, hence CollectionAction (no id parameter) rather than
// CollectionSelectionAction. Right = leading_actions, selection-
// independent, in registration order ("New", "Open", "Edit Collection…").
// The caller never asks for a side directly; it's implied by which field
// a CollectionAction is placed in. Same left-is-selection-dependent,
// right-is-independent convention as collection_editor.hpp's own header
// bar — John: independent actions belong on the side a user still reaches
// for with nothing current/selected.

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "collection_editor.hpp"

struct CollectionMenuHandle {
    Gtk::Popover* popover;
    // nullptr if CollectionMenuConfig::details_action was unset. Exposed
    // as a raw pointer specifically so callers can gate its sensitivity
    // from wherever their own "is there a current item" state changes —
    // every existing Details button in this app already needs that (see
    // refresh_transport_sensitivity()/update_project_button_label()/
    // update_sequence_button_label()), from code far away from this
    // popover's own construction.
    Gtk::Button* details_button = nullptr;
    // Full row-list rebuild — already wired to the popover's own
    // signal_show(), exposed too since some callers also need to trigger
    // it right after an action lands (e.g. right after creating a brand
    // new item, before the popover is reopened).
    std::function<void()> refresh_list;
    // Cheap current-item-only update, no row rebuild — a no-op if
    // CollectionMenuConfig::is_current was never set. For callers whose
    // "current" state changes far more often than the list itself (e.g.
    // Next/Previous during Playback), same reasoning as this app's
    // existing update_sequence_checkmarks().
    std::function<void()> refresh_current;
};

struct CollectionMenuConfig {
    std::function<std::vector<std::string>()> list_ids;  // in display order
    std::function<CollectionRowContent(const std::string& id)> row_content;
    std::function<void(const std::string& id)> activate;  // row click
    std::optional<std::function<bool(const std::string& id)>> is_current;
    std::vector<CollectionAction> leading_actions;
    std::optional<CollectionAction> details_action;
};

// `button` must already exist (label/tooltip is the caller's concern, same
// as today) — this only attaches a freshly-built popover to it.
CollectionMenuHandle show_collection_menu(Gtk::MenuButton* button, CollectionMenuConfig config);
