// SPDX-License-Identifier: GPL-3.0-only
//
// collection_editor.cpp — see collection_editor.hpp for scope.

#include "collection_editor.hpp"

#include <glib/gi18n.h>

#include <algorithm>

// Bridges into main.cpp's anonymous namespace, same pattern as every other
// *_dialogs.cpp file.
void bridge_set_button_icon_from_file(Gtk::Button* button, const std::string& filename);
std::string bridge_resource_path(const std::string& filename);

namespace {

// Gtk::Image counterpart to main.cpp's set_button_icon_from_file() — same
// reasoning (some icon names don't reliably resolve via the icon theme),
// just for a plain status/handle icon rather than a button's own child.
void set_image_icon_from_file(Gtk::Image* image, const std::string& filename, int size = 16) {
    auto file = Gio::File::create_for_path(bridge_resource_path("icons/" + filename));
    image->set(Gtk::IconPaintable::create(file, size));
    image->set_pixel_size(size);
}

constexpr auto kDropAboveClass = "blind-sight-collection-drop-above";
constexpr auto kDropBelowClass = "blind-sight-collection-drop-below";
constexpr auto kIconButtonClass = "blind-sight-collection-icon-button";
constexpr auto kListClass = "blind-sight-collection-list";

// This component's own CSS — deliberately self-contained rather than
// depending on main.cpp's own load_app_css() (which the standalone test rig
// never loads at all, and this file is meant to work the same either way).
//
// kIconButtonClass: same round-background-at-rest look as main.cpp's
// .blind-sight-round-icon-button, plus a clearer focus-visible ring than
// GTK's own default — a flat/circular button's default focus indicator is
// easy to miss against its own round background.
//
// kDropAboveClass/kDropBelowClass: live insertion-point feedback while
// dragging — a thin accent-colored edge on whichever row the pointer is
// over, top or bottom half depending on where the drop would actually
// land (see signal_motion()/signal_drop() below, which use the same
// half-split).
//
// kListClass: the drop-indicator edge rendered oddly against boxed-list's
// own rounded-corner clipping at the first/last row. Padding inside the
// list pushes every row past that curve, and squaring the rows off
// (overriding boxed-list's own first/last corner-rounding) means the
// indicator is always a plain straight edge, never clipped by a curve.
//
// Loaded once per process — this component can be instantiated repeatedly
// (once per show_collection_editor() call), but the provider only needs
// adding once.
void ensure_component_css() {
    static bool loaded = false;
    if (loaded) {
        return;
    }
    loaded = true;
    auto provider = Gtk::CssProvider::create();
    provider->load_from_data(
        std::string(".") + kIconButtonClass +
        " { background-color: alpha(@theme_fg_color, 0.08); }"
        "." + kIconButtonClass + ":hover { background-color: alpha(@theme_fg_color, 0.15); }"
        "." + kIconButtonClass + ":active { background-color: alpha(@theme_fg_color, 0.22); }"
        "." + kIconButtonClass +
        ":focus-visible { outline: 2px solid @accent_color; outline-offset: 1px; "
        "background-color: alpha(@theme_fg_color, 0.15); }"
        "." + kDropAboveClass + " { border-top: 2px solid @accent_color; }"
        "." + kDropBelowClass + " { border-bottom: 2px solid @accent_color; }"
        "." + kListClass + " { padding-top: 8px; padding-bottom: 8px; }"
        "." + kListClass + " > row { border-radius: 0; }"
        // GTK's own default drop-target styling outlines the whole row
        // (all four edges) the moment it becomes a drag's current target —
        // reset here so only the single edge kDropAboveClass/
        // kDropBelowClass draws is ever visible.
        "." + kListClass + " > row:drop(active) { box-shadow: none; outline: none; "
        "background: none; }");
    Gtk::StyleContext::add_provider_for_display(Gdk::Display::get_default(), provider,
                                                 GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

}  // namespace

void show_collection_editor(Gtk::Window* parent, CollectionEditorConfig config) {
    ensure_component_css();
    // on_changed is called unconditionally below — normalized to a no-op
    // once here, rather than null-checking at every call site, since
    // *_dialogs.hpp's own show_*_collection_editor() signatures all default
    // it to {} (optional).
    if (!config.on_changed) {
        config.on_changed = [] {};
    }

    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, config.title.c_str());
    adw_dialog_set_content_width(dialog, 480);
    adw_dialog_set_content_height(dialog, 520);

    // Real, confirmed crash (gdb backtrace, 2026-10-08): closing this
    // dialog while a row is selected segfaults inside gtk_button_set_child
    // — list_box's own row teardown fires "selected-rows-changed" (clearing
    // the now-destroyed selection), reaching recompute_toolbar_state()
    // below, which still tries to set the visibility button's icon even
    // though that button (a sibling of list_box, built earlier) is itself
    // already torn down by then. AdwDialog's own "closed" signal fires
    // once the dialog is actually closing, by any means (button, Escape),
    // so recompute_toolbar_state() can bail out before touching any
    // toolbar button from then on.
    auto closing = std::make_shared<bool>(false);
    g_signal_connect_data(
        dialog, "closed",
        G_CALLBACK(+[](AdwDialog*, gpointer data) {
            **static_cast<std::shared_ptr<bool>*>(data) = true;
        }),
        new std::shared_ptr<bool>(closing),
        +[](gpointer data, GClosure*) {
            delete static_cast<std::shared_ptr<bool>*>(data);
        },
        GConnectFlags(0));

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->add_css_class(kListClass);
    // SINGLE, not MULTIPLE — deliberately: with at most one row ever
    // selected, visibility/destructive actions never face a mixed-state
    // selection to disambiguate (see collection_editor.hpp's own comment).
    // SINGLE, not BROWSE, since BROWSE can never reach zero selected, and
    // "click the sole-selected row to deselect it" (the click-gesture
    // below) depends on zero being reachable.
    list_box->set_selection_mode(Gtk::SelectionMode::SINGLE);
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_vexpand(true);
    scroller->set_child(*list_box);

    // Mirrors the row order list_box actually holds — a selected
    // Gtk::ListBoxRow's get_index() is the lookup key into this.
    auto row_ids = std::make_shared<std::vector<std::string>>();

    auto get_selected_ids = [list_box, row_ids]() {
        std::vector<std::string> ids;
        for (auto* row : list_box->get_selected_rows()) {
            auto index = static_cast<std::size_t>(row->get_index());
            if (index < row_ids->size()) {
                ids.push_back((*row_ids)[index]);
            }
        }
        return ids;
    };

    // Declared here (filled in below, by refresh()'s own construction) so
    // leading-action button handlers can already capture the shared_ptr —
    // a leading action's effect can land asynchronously (e.g. after a file
    // picker returns), well after this loop runs.
    auto refresh = std::make_shared<std::function<void()>>();

    // --- Header bar buttons: left/pack_start = selection-dependent,
    // right/pack_end = selection-independent, nearest the header bar's
    // own close button (see collection_editor.hpp's own layout comment
    // for why). Built before the list so the visibility/destructive
    // buttons exist for refresh()'s own recompute closure below to
    // capture.
    auto* header_bar = adw_header_bar_new();

    // Non-destructive selection action (e.g. image's "Info") — leftmost/
    // outermost of the left group, then visibility, then destructive
    // closest to center. pack_start()'s first call lands outermost, so
    // this forward order already reads left-to-right correctly.
    Gtk::Button* selection_action_button = nullptr;
    if (config.selection_action) {
        selection_action_button = Gtk::make_managed<Gtk::Button>();
        bridge_set_button_icon_from_file(selection_action_button,
                                          config.selection_action->icon_filename);
        selection_action_button->set_tooltip_text(config.selection_action->tooltip);
        // Square, not circular — John: only the window's own close
        // button (a window control, not an action) should be round in
        // a real header bar; this was circular from its old life in a
        // plain content-area toolbar, before this dialog had a header
        // bar at all.
        selection_action_button->add_css_class("flat");
        selection_action_button->add_css_class(kIconButtonClass);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                                   GTK_WIDGET(selection_action_button->gobj()));
    }

    // Smart visibility toggle — infers "Show"/"Hide" from the current
    // selection's state rather than two separately-named actions (see
    // collection_editor.hpp's own comment).
    Gtk::Button* visibility_button = nullptr;
    if (config.visibility) {
        visibility_button = Gtk::make_managed<Gtk::Button>();
        visibility_button->add_css_class("flat");
        visibility_button->add_css_class(kIconButtonClass);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                                   GTK_WIDGET(visibility_button->gobj()));
    }

    Gtk::Button* destructive_button = nullptr;
    if (config.destructive) {
        destructive_button = Gtk::make_managed<Gtk::Button>();
        destructive_button->set_icon_name("user-trash-symbolic");
        destructive_button->set_tooltip_text(config.destructive->label);
        destructive_button->add_css_class("flat");
        destructive_button->add_css_class(kIconButtonClass);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                                   GTK_WIDGET(destructive_button->gobj()));
    }

    // Independent actions, right/pack_end — reverse iteration, since
    // pack_end()'s first call lands outermost (rightmost, next to
    // Close) and each later call shifts toward center: packing the
    // vector back-to-front puts its last element outermost and its
    // first element innermost, i.e. still reading left-to-right in
    // registration order within this cluster.
    for (auto it = config.leading_actions.rbegin(); it != config.leading_actions.rend(); ++it) {
        const CollectionAction& action = *it;
        auto* button = Gtk::make_managed<Gtk::Button>();
        bridge_set_button_icon_from_file(button, action.icon_filename);
        button->set_tooltip_text(action.tooltip);
        button->add_css_class("flat");
        button->add_css_class(kIconButtonClass);
        button->signal_clicked().connect([action, refresh, config]() {
            action.activate([refresh, config]() {
                (*refresh)();
                config.on_changed();
            });
        });
        adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar), GTK_WIDGET(button->gobj()));
    }

    auto recompute_toolbar_state = [=]() {
        // See the "closed"-signal comment above dialog creation — once
        // the dialog is closing, the toolbar buttons this touches may
        // already be destroyed.
        if (*closing) {
            return;
        }
        auto selected = get_selected_ids();
        if (selection_action_button) {
            selection_action_button->set_sensitive(selected.size() == 1);
        }
        if (visibility_button && config.visibility) {
            if (selected.empty()) {
                // Insensitive either way, but still needs a real icon —
                // defaults to the same "Show" direction the mixed-
                // selection case below uses, so the button never opens
                // (or lands back at) childless.
                bridge_set_button_icon_from_file(visibility_button, "eye-open-symbolic.svg");
                visibility_button->set_tooltip_text(_("Show Selected"));
                visibility_button->set_sensitive(false);
            } else {
                visibility_button->set_sensitive(true);
                bool all_visible = std::all_of(
                    selected.begin(), selected.end(),
                    [&](const std::string& id) { return !config.visibility->is_hidden(id); });
                // All-visible -> next click hides; anything else
                // (all-hidden or mixed) -> next click shows.
                if (all_visible) {
                    bridge_set_button_icon_from_file(visibility_button, "eye-closed-symbolic.svg");
                    visibility_button->set_tooltip_text(_("Hide Selected"));
                } else {
                    bridge_set_button_icon_from_file(visibility_button, "eye-open-symbolic.svg");
                    visibility_button->set_tooltip_text(_("Show Selected"));
                }
            }
        }
        if (destructive_button && config.destructive) {
            bool destructive_ok =
                !selected.empty() &&
                std::all_of(selected.begin(), selected.end(), [&](const std::string& id) {
                    return !config.destructive->enabled_for || config.destructive->enabled_for(id);
                });
            destructive_button->set_sensitive(destructive_ok);
        }
    };

    // --- Row construction ---
    *refresh = [=]() {
        // Every row is rebuilt from scratch below, which drops GTK's own
        // selection state along with the old rows — restored afterward by
        // id, so toggling visibility (or any other refresh()-triggering
        // action) doesn't silently clear what the user had selected, which
        // would otherwise stop them from iterating (toggle, look, toggle
        // back) without reselecting each time.
        auto previously_selected = get_selected_ids();
        list_box->remove_all();
        row_ids->clear();
        for (const auto& id : config.list_ids()) {
            auto content = config.row_content(id);
            auto* row = adw_action_row_new();
            // Single wrap for this row — used for add_controller() below as
            // well as the final list_box->append(); a second, independent
            // Glib::wrap() call on the same raw GObject is exactly the
            // "wrap_func_table" foot-gun documented elsewhere in this
            // codebase (main.cpp), so there must be only one.
            auto* row_widget = Glib::wrap(row, false);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), content.title.c_str());
            if (!content.subtitle.empty()) {
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), content.subtitle.c_str());
                adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
            }
            adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
            if (!content.tooltip.empty()) {
                gtk_widget_set_tooltip_text(row, content.tooltip.c_str());
            }

            // GtkListBox's own SINGLE-mode click handling treats a plain
            // click on an already-selected row as a no-op, not a deselect
            // — intercepted here, in CAPTURE phase (so this runs before
            // GtkListBox's own bubble-phase handling), for exactly that one
            // case. The modifier check is a defensive no-op now that
            // selection is single rather than multiple (there's no
            // toggle/range-select left to defer to), kept rather than
            // removed in case a held modifier ever means something else
            // here later. No per-row selected-state indicator beyond GTK's
            // own row:selected background — redundant on top of a color
            // change.
            auto click_gesture = Gtk::GestureClick::create();
            click_gesture->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
            click_gesture->set_button(GDK_BUTTON_PRIMARY);
            click_gesture->signal_pressed().connect(
                [list_box, row_widget, click_gesture](int, double, double) {
                    auto event = click_gesture->get_current_event();
                    auto state = event ? event->get_modifier_state() : Gdk::ModifierType{};
                    if ((state & (Gdk::ModifierType::CONTROL_MASK |
                                   Gdk::ModifierType::SHIFT_MASK)) != Gdk::ModifierType{}) {
                        return;  // let GTK's own ctrl/shift handling run
                    }
                    auto selected = list_box->get_selected_rows();
                    bool is_sole_selected =
                        selected.size() == 1 &&
                        GTK_WIDGET(selected.front()->gobj()) == row_widget->gobj();
                    if (is_sole_selected) {
                        list_box->unselect_all();
                        click_gesture->set_state(Gtk::EventSequenceState::CLAIMED);
                    }
                });
            row_widget->add_controller(click_gesture);

            if (config.reorder) {
                auto* handle = Gtk::make_managed<Gtk::Image>();
                set_image_icon_from_file(handle, "draggable-symbolic.svg");
                // No margin of our own here — if the handle still reads as
                // inset from the edge after this, it's the SVG's own
                // lateral padding, not this widget's layout.
                auto drag_source = Gtk::DragSource::create();
                drag_source->set_actions(Gdk::DragAction::MOVE);
                std::string dragged_id = id;
                drag_source->signal_prepare().connect(
                    [dragged_id](double, double) -> Glib::RefPtr<Gdk::ContentProvider> {
                        Glib::Value<Glib::ustring> value;
                        value.init(Glib::Value<Glib::ustring>::value_type());
                        value.set(dragged_id);
                        return Gdk::ContentProvider::create(value);
                    },
                    false);
                handle->add_controller(drag_source);
                adw_action_row_add_prefix(ADW_ACTION_ROW(row), GTK_WIDGET(handle->gobj()));

                auto drop_target = Gtk::DropTarget::create(
                    Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);
                std::string target_id = id;
                // Top half of the row -> insert above it; bottom half ->
                // insert below. The live indicator (motion/leave below) and
                // the actual drop use this same split, so what's shown is
                // exactly what happens.
                auto is_above = [row_widget](double y) {
                    int height = row_widget->get_height();
                    return height <= 0 || y < height / 2.0;
                };
                drop_target->signal_motion().connect(
                    [row_widget, is_above](double, double y) -> Gdk::DragAction {
                        bool above = is_above(y);
                        row_widget->remove_css_class(above ? kDropBelowClass : kDropAboveClass);
                        row_widget->add_css_class(above ? kDropAboveClass : kDropBelowClass);
                        return Gdk::DragAction::MOVE;
                    },
                    false);
                drop_target->signal_leave().connect([row_widget]() {
                    row_widget->remove_css_class(kDropAboveClass);
                    row_widget->remove_css_class(kDropBelowClass);
                });
                drop_target->signal_drop().connect(
                    [config, target_id, refresh, row_widget, is_above](
                        const Glib::ValueBase& value, double, double y) -> bool {
                        row_widget->remove_css_class(kDropAboveClass);
                        row_widget->remove_css_class(kDropBelowClass);
                        if (value.gobj()->g_type != Glib::Value<Glib::ustring>::value_type()) {
                            return false;
                        }
                        Glib::Value<Glib::ustring> source_value;
                        source_value.init(value.gobj());
                        std::string source_id = source_value.get();
                        if (source_id == target_id) {
                            return false;
                        }
                        auto ids = config.list_ids();
                        auto source_it = std::find(ids.begin(), ids.end(), source_id);
                        auto target_it = std::find(ids.begin(), ids.end(), target_id);
                        if (source_it == ids.end() || target_it == ids.end()) {
                            return false;
                        }
                        ids.erase(source_it);
                        target_it = std::find(ids.begin(), ids.end(), target_id);
                        if (!is_above(y)) {
                            ++target_it;
                        }
                        ids.insert(target_it, source_id);
                        config.reorder->reorder(ids);
                        // Deferred to the next idle iteration, not called
                        // synchronously: refresh() destroys every row via
                        // list_box->remove_all(), including this row and
                        // the very drop_target controller whose signal
                        // handler is still executing right here — freeing
                        // a GObject out from under its own in-flight
                        // signal emission is a use-after-free that
                        // doesn't necessarily crash immediately, but on
                        // some later, unrelated interaction that finally
                        // touches the corrupted memory.
                        Glib::signal_idle().connect_once([config, refresh]() {
                            (*refresh)();
                            config.on_changed();
                        });
                        return true;
                    },
                    false);
                row_widget->add_controller(drop_target);

                // Keyboard equivalent — a DragSource gesture has no
                // keyboard path at all, so without this, reordering would
                // be mouse/touch-only. Alt+Up/Alt+Down moves the focused
                // row; refresh() rebuilds every row (destroying this one),
                // so focus is re-grabbed on the row's new position
                // afterward rather than silently dropping to nothing.
                auto key_controller = Gtk::EventControllerKey::create();
                std::string row_id = id;
                key_controller->signal_key_pressed().connect(
                    [config, row_id, refresh, list_box](guint keyval, guint,
                                                          Gdk::ModifierType state) -> bool {
                        if ((state & Gdk::ModifierType::ALT_MASK) == Gdk::ModifierType{} ||
                            (keyval != GDK_KEY_Up && keyval != GDK_KEY_Down)) {
                            return false;
                        }
                        auto ids = config.list_ids();
                        auto it = std::find(ids.begin(), ids.end(), row_id);
                        if (it == ids.end()) {
                            return false;
                        }
                        bool move_up = (keyval == GDK_KEY_Up);
                        bool moved = false;
                        if (move_up && it != ids.begin()) {
                            std::iter_swap(it - 1, it);
                            moved = true;
                        } else if (!move_up && it + 1 != ids.end()) {
                            std::iter_swap(it, it + 1);
                            moved = true;
                        }
                        if (!moved) {
                            return true;  // already at that end — consumed, no-op
                        }
                        config.reorder->reorder(ids);
                        // Deferred to the next idle iteration, same
                        // use-after-free reasoning as the drop handler
                        // above — refresh() destroys this row (and the
                        // key_controller whose handler is still running)
                        // via list_box->remove_all(). The focus-regrab
                        // has to happen after refresh() too, so it's
                        // folded into the same deferred callback.
                        Glib::signal_idle().connect_once([config, refresh, list_box, ids,
                                                           row_id]() {
                            (*refresh)();
                            config.on_changed();
                            auto new_index =
                                std::find(ids.begin(), ids.end(), row_id) - ids.begin();
                            if (auto* new_row = list_box->get_row_at_index(
                                    static_cast<int>(new_index))) {
                                new_row->grab_focus();
                            }
                        });
                        return true;
                    },
                    false);
                row_widget->add_controller(key_controller);
            }

            // eye-open-negative-filled-symbolic, deliberately plainer than
            // the toolbar button's own eye-open/eye-closed pair, so a
            // passive status indicator never reads as another clickable
            // action. Hidden entries get no icon at all, not a closed-eye
            // glyph — absence is a stronger signal that visibility isn't
            // set than a closed eye would be.
            if (config.visibility && !config.visibility->is_hidden(id)) {
                auto* status = Gtk::make_managed<Gtk::Image>();
                set_image_icon_from_file(status, "eye-open-negative-filled-symbolic.svg");
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(status->gobj()));
            }

            row_ids->push_back(id);
            list_box->append(*row_widget);
        }
        for (std::size_t i = 0; i < row_ids->size(); ++i) {
            if (std::find(previously_selected.begin(), previously_selected.end(),
                           (*row_ids)[i]) == previously_selected.end()) {
                continue;
            }
            if (auto* row = list_box->get_row_at_index(static_cast<int>(i))) {
                list_box->select_row(*row);
            }
        }
        recompute_toolbar_state();
    };

    list_box->signal_selected_rows_changed().connect(recompute_toolbar_state);

    if (selection_action_button && config.selection_action) {
        selection_action_button->signal_clicked().connect([=]() {
            auto selected = get_selected_ids();
            if (selected.size() != 1) {
                return;
            }
            config.selection_action->activate(selected.front(), [refresh, config]() {
                (*refresh)();
                config.on_changed();
            });
        });
    }

    if (visibility_button && config.visibility) {
        visibility_button->signal_clicked().connect([=]() {
            auto selected = get_selected_ids();
            if (selected.empty()) {
                return;
            }
            bool all_visible = std::all_of(
                selected.begin(), selected.end(),
                [&](const std::string& id) { return !config.visibility->is_hidden(id); });
            config.visibility->set_hidden(selected, all_visible);
            (*refresh)();
            config.on_changed();
        });
    }

    if (destructive_button) {
        destructive_button->signal_clicked().connect([=]() {
            auto selected = get_selected_ids();
            if (selected.empty()) {
                return;
            }
            auto do_remove = [=]() {
                config.destructive->remove(selected);
                (*refresh)();
                config.on_changed();
            };
            if (config.destructive->confirm) {
                config.destructive->confirm(parent, selected, do_remove);
            } else {
                do_remove();
            }
        });
    }

    (*refresh)();
    box->append(*scroller);

    // No explicit Close button — the header bar supplies its own, now
    // that it actually has one (adw_header_bar_new()'s own close
    // button, same as every other AdwDialog in this codebase that was
    // missing a header bar entirely until now).
    auto* toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), header_bar);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(box->gobj()));
    adw_dialog_set_child(dialog, toolbar_view);

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}
