// SPDX-License-Identifier: GPL-3.0-only
//
// collection_menu.cpp — see collection_menu.hpp for scope.

#include "collection_menu.hpp"

#include <glib/gi18n.h>

// Bridges into main.cpp's anonymous namespace, same pattern as every other
// *_dialogs.cpp file (see collection_editor.cpp's identical declaration).
void bridge_set_button_icon_from_file(Gtk::Button* button, const std::string& filename);

namespace {

// kIconButtonClass intentionally reuses main.cpp's own
// .blind-sight-round-icon-button (load_app_css(), always loaded before any
// popover exists) rather than defining a self-contained one the way
// collection_editor.cpp does — that component was built to also work in a
// standalone test rig; this one is only ever used from inside the running
// app, so there's nothing to duplicate CSS loading for.
constexpr auto kIconButtonClass = "blind-sight-round-icon-button";

Gtk::Button* build_header_button(const CollectionAction& action,
                                  const std::shared_ptr<std::function<void()>>& refresh_list,
                                  Gtk::Popover* popover) {
    auto* button = Gtk::make_managed<Gtk::Button>();
    bridge_set_button_icon_from_file(button, action.icon_filename);
    button->set_tooltip_text(action.tooltip);
    button->add_css_class("flat");
    button->add_css_class("circular");
    button->add_css_class(kIconButtonClass);
    button->set_margin(8);
    // popdown() always runs first — every existing header-bar button in
    // this app already closes its popover before running its own action,
    // so an action that opens a dialog never finds a popover still
    // floating underneath it.
    button->signal_clicked().connect([action, refresh_list, popover]() {
        popover->popdown();
        action.activate([refresh_list]() { (*refresh_list)(); });
    });
    return button;
}

}  // namespace

CollectionMenuHandle show_collection_menu(Gtk::MenuButton* button, CollectionMenuConfig config) {
    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::NONE);
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_max_content_height(300);
    scroller->set_propagate_natural_height(true);
    // Without an explicit width, the popover collapses to the row
    // content's minimum size (same reasoning as every existing popover's
    // own scroller in main.cpp).
    scroller->set_size_request(360, -1);
    scroller->set_child(*list_box);
    scroller->set_margin(8);

    auto* header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    auto* header_spacer = Gtk::make_managed<Gtk::Box>();
    header_spacer->set_hexpand(true);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box->append(*header);
    box->append(*scroller);

    auto* popover = Gtk::make_managed<Gtk::Popover>();
    popover->set_child(*box);
    button->set_popover(*popover);

    // Left: the single optional details_action (selection/current-
    // dependent). Right: leading_actions, in registration order
    // (selection-independent). header_spacer between them is what makes
    // this split, not anything either side asks for directly — same
    // left-is-dependent, right-is-independent convention as
    // collection_editor.hpp's own header bar now uses.
    auto refresh_list = std::make_shared<std::function<void()>>();
    Gtk::Button* details_button = nullptr;
    if (config.details_action) {
        details_button = build_header_button(*config.details_action, refresh_list, popover);
        header->append(*details_button);
    }
    header->append(*header_spacer);

    for (const auto& action : config.leading_actions) {
        header->append(*build_header_button(action, refresh_list, popover));
    }

    // Mirrors the row order list_box actually holds — a row's own
    // get_index() is the lookup key into both of these.
    auto row_ids = std::make_shared<std::vector<std::string>>();
    auto checkmarks = std::make_shared<std::vector<Gtk::Image*>>();

    *refresh_list = [=]() {
        list_box->remove_all();
        row_ids->clear();
        checkmarks->clear();
        for (const auto& id : config.list_ids()) {
            auto content = config.row_content(id);
            auto* row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), content.title.c_str());
            if (!content.subtitle.empty()) {
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), content.subtitle.c_str());
            }
            // Unlike the Edit Collection dialog's own single-line
            // subtitle, this row can wrap across as many lines as it
            // needs (0 = unlimited) — a fixed-width button face can't,
            // but a popover row can.
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
            adw_action_row_set_title_lines(ADW_ACTION_ROW(row), 1);
            if (!content.tooltip.empty()) {
                gtk_widget_set_tooltip_text(row, content.tooltip.c_str());
            }
            // Plain boolean GObject property — see image_dialogs.cpp's
            // own identical comment for why this, not
            // adw_action_row_set_activatable_widget().
            g_object_set(row, "activatable", TRUE, nullptr);

            if (config.is_current) {
                auto* current_icon = Gtk::make_managed<Gtk::Image>();
                current_icon->set_from_icon_name("object-select-symbolic");
                current_icon->set_visible((*config.is_current)(id));
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(current_icon->gobj()));
                checkmarks->push_back(current_icon);
            }

            row_ids->push_back(id);
            list_box->append(*Glib::wrap(row, false));
        }
    };

    list_box->signal_row_activated().connect([config, row_ids, popover](Gtk::ListBoxRow* row) {
        auto index = static_cast<std::size_t>(row->get_index());
        if (index < row_ids->size()) {
            config.activate((*row_ids)[index]);
        }
        popover->popdown();
    });

    popover->signal_show().connect([refresh_list]() { (*refresh_list)(); });

    CollectionMenuHandle handle;
    handle.popover = popover;
    handle.details_button = details_button;
    handle.refresh_list = [refresh_list]() { (*refresh_list)(); };
    if (config.is_current) {
        handle.refresh_current = [config, row_ids, checkmarks]() {
            for (std::size_t i = 0; i < checkmarks->size() && i < row_ids->size(); ++i) {
                (*checkmarks)[i]->set_visible((*config.is_current)((*row_ids)[i]));
            }
        };
    } else {
        handle.refresh_current = []() {};
    }
    return handle;
}
