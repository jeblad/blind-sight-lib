// SPDX-License-Identifier: GPL-3.0-only
//
// popover_menu_test.cpp — a standalone, interactive sanity check for
// PopoverMenu, built and run on its own, deliberately not wired into the
// blind-sight-demo target yet (see popover_menu.hpp's own scope comment —
// this widget is meant to stand on its own, so it's tested on its own).
//
// What to look for when running it:
//   - Two plain icon buttons on the LEFT of the popover's header row
//     ("New", "Open"), always clickable.
//   - One icon button on the RIGHT ("Info"), initially insensitive.
//   - The "Selection active" switch in the main window toggles that Info
//     button's sensitivity — flip it on, reopen the popover, Info is now
//     clickable; flip it off, Info goes insensitive again — all without
//     touching the Info button itself.
//   - A plain three-row list below the header (set_content()), unrelated
//     to the header buttons. Each row's label carries an "(open N)" count
//     that increments every time the popover opens — connect_popover_show()
//     rebuilding the list fresh, same as every real popover in this app
//     already does on signal_show().
//   - Clicking any header button closes the popover immediately (popdown
//     before the click's own action runs), then prints which button fired.

#include <gtkmm.h>

#include <iostream>

#include "popover_menu.hpp"

namespace {

Gtk::Image* icon(const Glib::ustring& icon_name) {
    auto* image = Gtk::make_managed<Gtk::Image>();
    image->set_from_icon_name(icon_name);
    return image;
}

}  // namespace

int main(int argc, char** argv) {
    auto app = Gtk::Application::create("org.example.popover-menu-test");
    app->signal_activate().connect([app]() {
        auto* window = new Gtk::ApplicationWindow();
        app->add_window(*window);
        window->set_title("PopoverMenu test");
        window->set_default_size(360, 160);

        auto* menu_button = Gtk::make_managed<PopoverMenu>();
        menu_button->set_label("Menu");

        menu_button->add_button(icon("list-add-symbolic"), "New", false,
                                 []() { std::cout << "New clicked\n"; });
        menu_button->add_button(icon("document-open-symbolic"), "Open", false,
                                 []() { std::cout << "Open clicked\n"; });
        menu_button->add_button(icon("dialog-information-symbolic"), "Info", true,
                                 []() { std::cout << "Info clicked\n"; });
        menu_button->set_selection_active(false);

        auto* list = Gtk::make_managed<Gtk::ListBox>();
        list->add_css_class("boxed-list");
        menu_button->set_content(list);

        auto open_count = std::make_shared<int>(0);
        menu_button->connect_popover_show([list, open_count]() {
            ++*open_count;
            std::cout << "popover shown (open " << *open_count << ")\n";
            list->remove_all();
            for (const auto& label : {"Row one", "Row two", "Row three"}) {
                list->append(*Gtk::make_managed<Gtk::Label>(
                    Glib::ustring(label) + " (open " + std::to_string(*open_count) + ")"));
            }
        });

        auto* selection_switch = Gtk::make_managed<Gtk::Switch>();
        selection_switch->property_active().signal_changed().connect(
            [menu_button, selection_switch]() {
                menu_button->set_selection_active(selection_switch->get_active());
            });

        auto* row = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
        row->set_margin(16);
        row->append(*menu_button);
        row->append(*Gtk::make_managed<Gtk::Label>("Selection active:"));
        row->append(*selection_switch);
        window->set_child(*row);

        window->present();
    });
    return app->run(argc, argv);
}
