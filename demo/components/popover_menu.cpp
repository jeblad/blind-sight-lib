// SPDX-License-Identifier: GPL-3.0-only
//
// popover_menu.cpp — see popover_menu.hpp for scope.

#include "popover_menu.hpp"

PopoverMenu::PopoverMenu() {
    spacer_.set_hexpand(true);
    header_.append(independent_group_);
    header_.append(spacer_);
    header_.append(selection_group_);
    root_.append(header_);
    popover_.set_child(root_);
    set_popover(popover_);
}

Gtk::Button* PopoverMenu::add_button(Gtk::Widget* icon, const Glib::ustring& tooltip,
                                      bool depends_on_selection,
                                      std::function<void()> on_clicked) {
    auto* button = Gtk::make_managed<Gtk::Button>();
    button->set_child(*icon);
    button->set_tooltip_text(tooltip);
    button->add_css_class("flat");
    button->add_css_class("circular");
    button->set_margin(8);
    (depends_on_selection ? selection_group_ : independent_group_).append(*button);

    // popdown() always runs before on_clicked — see this function's own
    // header comment for why (a dialog on_clicked opens must never find a
    // popover still floating underneath it).
    button->signal_clicked().connect([this, on_clicked]() {
        popover_.popdown();
        if (on_clicked) {
            on_clicked();
        }
    });
    return button;
}

void PopoverMenu::set_selection_active(bool active) {
    selection_group_.set_sensitive(active);
}

void PopoverMenu::set_content(Gtk::Widget* content) {
    root_.append(*content);
}

sigc::connection PopoverMenu::connect_popover_show(std::function<void()> callback) {
    return popover_.signal_show().connect(callback);
}
