// SPDX-License-Identifier: GPL-3.0-only
//
// popover_menu.hpp — a reusable, app-agnostic GTK4/libadwaita composite
// widget: the View half of an MVC split for "a MenuButton whose popover
// holds a row of registered action buttons plus an arbitrary content
// area below." A genuine Gtk::MenuButton subclass (per GNOME's own
// composite-widget convention — see discourse.gnome.org/t/gtk-4-and-
// subclassing/3425 and wiki.gnome.org/HowDoI/CustomWidgets — rather than
// a plain helper class standing beside a separately-built button), so it
// drops into a header bar exactly like an ordinary Gtk::MenuButton would.
// No GtkBuilder .ui template, matching the plain-C++-construction style
// already used throughout this codebase.
//
// This widget has no notion of sessions/projects/images/collections, no
// dependency on this app's resource files or CSS classes, and no opinion
// about what a click does or what the content area contains — both are
// the caller's (Model/glue's) concern, passed in as plain widgets and
// callbacks.
//
// The one piece of real behavior this widget owns: each registered
// button states only whether it depends on the current selection — not
// where it goes. Selection-independent buttons collect on the left,
// selection-dependent ones on the right, placed in registration order
// within their own side; the caller never names a side directly. Every
// selection-dependent button can be toggled insensitive together in one
// call (set_selection_active()), since they all share one parent
// container underneath — the same "set the container, not each child"
// mechanism used throughout this codebase.

#pragma once

#include <gtkmm.h>

#include <functional>

class PopoverMenu : public Gtk::MenuButton {
public:
    PopoverMenu();

    // Registers one header-row button. `icon` is a fully-built widget —
    // this widget never touches an icon name, a theme, or a resource
    // path; the caller decides where its icon comes from. `on_clicked`
    // is invoked after the popover has already closed (popdown() always
    // runs first, same order every existing button in this app already
    // uses, so an action that opens its own dialog never has a popover
    // floating underneath it).
    //
    // `depends_on_selection` decides which side the button lands on
    // (false = left, independent; true = right, selection-dependent) and
    // whether set_selection_active() reaches it — it is the only
    // placement signal this widget accepts; there is no way to ask for a
    // specific side directly.
    Gtk::Button* add_button(Gtk::Widget* icon, const Glib::ustring& tooltip,
                             bool depends_on_selection, std::function<void()> on_clicked);

    // Toggles every selection-dependent button's sensitivity together,
    // in one call — never iterate and set each one individually.
    void set_selection_active(bool active);

    // The popover's content area, below the header row of buttons.
    // Entirely the caller's concern — a list, a message, anything.
    // Called once; this widget doesn't support replacing it later.
    void set_content(Gtk::Widget* content);

    // Fires every time the popover is about to be shown — the moment a
    // caller whose content is a list typically rebuilds it fresh. A
    // forwarding method rather than exposing the Gtk::Popover itself, so
    // nothing outside this widget can reach in and do anything else to
    // the popover directly.
    sigc::connection connect_popover_show(std::function<void()> callback);

private:
    Gtk::Popover popover_;
    Gtk::Box root_{Gtk::Orientation::VERTICAL, 0};
    Gtk::Box header_{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Box independent_group_{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Box selection_group_{Gtk::Orientation::HORIZONTAL, 0};
    Gtk::Box spacer_;
};
