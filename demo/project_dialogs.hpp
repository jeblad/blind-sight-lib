// SPDX-License-Identifier: GPL-3.0-only
//
// project_dialogs.hpp — the project-management UI: recent-projects
// bookkeeping (gsettings-backed, this-machine-only), creating a new
// project (name + parent folder + a fresh protocol.md), opening an
// existing one, and the "Edit Collection" list of recent projects.

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <functional>
#include <string>
#include <vector>

#include "protocol.hpp"

std::vector<std::string> get_recent_projects(const Glib::RefPtr<Gio::Settings>& settings);
void remember_recent_project(const Glib::RefPtr<Gio::Settings>& settings,
                              const std::string& project_dir);
void forget_recent_project(const Glib::RefPtr<Gio::Settings>& settings,
                            const std::string& project_dir);

// Picks a free, collision-safe directory name under `parent_dir` for a
// new project titled `title`, creates it, and returns its path; "" only
// if parent_dir itself turned out to be unusable.
std::string create_project_directory(const Glib::RefPtr<Gio::Settings>& settings,
                                      const std::string& parent_dir,
                                      const std::string& title);

// "New Project" — one combined dialog for name + parent folder. Creates
// the directory, writes an empty protocol.md into it (title = the name
// just entered), and hands the result back via `on_created`; the caller
// is what actually makes it the active project.
void show_new_project_dialog(
    Gtk::Window* parent, const Glib::RefPtr<Gio::Settings>& settings,
    std::function<void(const std::string& project_dir, const blind_sight::Protocol& protocol)>
        on_created);

// The "Open Project…" flow — a folder picker, then whatever's wired to
// open a project by directory. `on_done` fires once a project was
// actually opened.
void start_open_project_dialog(Gtk::Window* parent,
                                const Glib::RefPtr<Gio::Settings>& settings,
                                std::function<void()> on_done = {});

// The recent-projects list editor ("Edit Collection").
void show_project_collection_editor(Gtk::Window* parent,
                                     const Glib::RefPtr<Gio::Settings>& settings,
                                     std::function<void()> on_changed = {});

// "Project Details" — edits the project's own title/description overrides
// (Protocol::frontmatter_title/frontmatter_description, protocol.hpp),
// pre-filled with the *effective* current value (resolved_title()/
// resolved_description(), not the raw override — same "start from what's
// actually showing" rule the image/session Details dialogs already use).
// Only hands back the two edited strings; the caller owns updating
// Protocol and writing it back via write_protocol_file().
void show_project_details_dialog(
    Gtk::Window* parent, const blind_sight::Protocol& current,
    std::function<void(const std::string& frontmatter_title,
                        const std::string& frontmatter_description)>
        on_save);

// Returns what main.cpp needs by name — refresh_project_list for the
// existing call sites that already trigger it, and the Details button
// itself (via collection_menu.hpp's own CollectionMenuHandle) so main.cpp
// can keep gating its sensitivity on "is a project active" from
// update_project_button_label(), the same way it does today.
struct ProjectMenuHandle {
    std::function<void()> refresh_project_list;
    Gtk::Button* project_details_button = nullptr;
};

// The project dropdown's popover — recent projects (title/description via
// resolved_title()/resolved_description(), most-recent-first), New/Open/
// Edit Collection on the left, Project Details on the right.
// `on_project_updated` is called after a successful Project Details save,
// alongside this app's own update_project_button_label() — the button
// *face* label is a separate widget this popover knows nothing about.
ProjectMenuHandle show_project_collection_menu(
    Gtk::MenuButton* button, Gtk::Window* parent,
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::shared_ptr<blind_sight::Protocol>& active_protocol,
    std::function<void()> on_project_updated);
