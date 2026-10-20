// SPDX-License-Identifier: GPL-3.0-only
//
// project_dialogs.cpp — see project_dialogs.hpp for scope.

#include "project_dialogs.hpp"

#include <glib/gi18n.h>

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "collection_editor.hpp"
#include "collection_menu.hpp"
#include "external_change_guard.hpp"
#include "images.hpp"

using blind_sight::Protocol;
using blind_sight::parse_protocol_file;
using blind_sight::resolved_description;
using blind_sight::resolved_title;
using blind_sight::slugify;
using blind_sight::write_protocol_file;

// Bridges into main.cpp's anonymous namespace — these stay defined in
// main.cpp but are needed here too. apply_last_folder()/remember_last_
// folder() are generic file-dialog helpers also used by the image-import
// dialogs, so they stay put rather than moving with just one of their
// two callers.
void bridge_apply_last_folder(const Glib::RefPtr<Gtk::FileDialog>& file_dialog,
                                const Glib::RefPtr<Gio::Settings>& settings,
                                const char* key);
void bridge_remember_last_folder(const Glib::RefPtr<Gio::Settings>& settings,
                                   const char* key, const Glib::RefPtr<Gio::File>& file);
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body);
void bridge_set_button_icon_from_file(Gtk::Button* button, const std::string& filename);
// Thin forwards to the global function-pointer-ish callbacks on_activate()
// wires up — those globals have internal linkage, so a plain forward
// declaration of the std::function itself wouldn't link.
void bridge_open_project_by_dir(const std::string& dir);
void bridge_start_new_project(std::function<void()> on_done = {});

namespace {

constexpr auto kRecentProjectsKey = "recent-projects";
constexpr auto kMaxRecentProjectsKey = "max-recent-projects";
constexpr auto kLastProjectParentFolderKey = "last-project-parent-folder";
constexpr auto kProjectSlugCounterKey = "project-slug-counter";
constexpr auto kProjectSlugCounterDigitsKey = "project-slug-counter-digits";
constexpr auto kProjectProtocolFileName = "protocol.md";

// Trivial and pure enough to just duplicate rather than bridge —
// main.cpp keeps its own copy too, used directly by its protocol-
// window/reconcile code.
std::string project_protocol_path(const std::string& project_dir) {
    return Glib::build_filename(project_dir, kProjectProtocolFileName);
}

// Only ever called from the three functions right below it, within this
// same file.
void write_recent_projects(const Glib::RefPtr<Gio::Settings>& settings,
                             const std::vector<std::string>& paths) {
    std::vector<Glib::ustring> values(paths.begin(), paths.end());
    settings->set_string_array(kRecentProjectsKey, values);
}

}  // namespace

// A project counts as stale once its protocol.md is gone, not just the
// directory itself (a directory with no protocol.md left in it isn't a
// project any more).
std::vector<std::string> get_recent_projects(const Glib::RefPtr<Gio::Settings>& settings) {
    auto values = settings->get_string_array(kRecentProjectsKey);
    std::vector<std::string> paths(values.begin(), values.end());
    auto stale = std::remove_if(paths.begin(), paths.end(), [](const std::string& path) {
        return !std::filesystem::exists(project_protocol_path(path));
    });
    if (stale != paths.end()) {
        paths.erase(stale, paths.end());
        write_recent_projects(settings, paths);
    }
    return paths;
}

void remember_recent_project(const Glib::RefPtr<Gio::Settings>& settings,
                               const std::string& project_dir) {
    auto recents = get_recent_projects(settings);
    recents.erase(std::remove(recents.begin(), recents.end(), project_dir), recents.end());
    recents.insert(recents.begin(), project_dir);
    auto max_recent = static_cast<std::size_t>(
        std::max(1, settings->get_int(kMaxRecentProjectsKey)));
    if (recents.size() > max_recent) {
        recents.resize(max_recent);
    }
    write_recent_projects(settings, recents);
}

void forget_recent_project(const Glib::RefPtr<Gio::Settings>& settings,
                             const std::string& project_dir) {
    auto recents = get_recent_projects(settings);
    recents.erase(std::remove(recents.begin(), recents.end(), project_dir), recents.end());
    write_recent_projects(settings, recents);
}

// Picks a free, collision-safe directory name under `parent_dir` for a
// new project titled `title` — {slug}-{counter:0Nd}, where the counter
// is the global, monotonically-incrementing project-slug-counter
// gsettings value. Creates the directory and returns its path; "" only
// if parent_dir itself turned out to be unusable.
std::string create_project_directory(const Glib::RefPtr<Gio::Settings>& settings,
                                       const std::string& parent_dir,
                                       const std::string& title) {
    // slugify() lives in images.hpp so generate_image_slug() can share
    // it. "project" fallback stays here, as the caller's own concern
    // (images.hpp's version falls back to "image" instead).
    std::string slug = slugify(title);
    if (slug.empty()) {
        slug = "project";
    }
    int digits = std::max(1, settings->get_int(kProjectSlugCounterDigitsKey));
    for (int attempt = 0; attempt < 1000; ++attempt) {
        int counter = settings->get_int(kProjectSlugCounterKey) + 1;
        settings->set_int(kProjectSlugCounterKey, counter);
        std::ostringstream name;
        name << slug << '-' << std::setw(digits) << std::setfill('0') << counter;
        std::string full_path = Glib::build_filename(parent_dir, name.str());
        std::error_code ec;
        if (std::filesystem::create_directory(full_path, ec) && !ec) {
            return full_path;
        }
        // Either a genuine name collision (try the next counter value)
        // or parent_dir itself doesn't work — the loop's own attempt cap
        // keeps the latter from spinning forever.
    }
    return "";
}

// "New Project" — one combined dialog for both halves of creating a
// project: a name field and a parent-folder picker together, rather
// than two separate steps. Creates the directory, writes an empty
// protocol.md into it (title = the name just entered, the protocol's
// H1), and hands the result back via `on_created`; the caller
// (on_activate()) is what actually makes it the active project, since
// that needs active_protocol/refresh_sequence_list, which this
// standalone function doesn't know about.
void show_new_project_dialog(
    Gtk::Window* parent, const Glib::RefPtr<Gio::Settings>& settings,
    std::function<void(const std::string& project_dir, const Protocol& protocol)>
        on_created) {
    auto* dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, _("New Project"));
    adw_dialog_set_content_width(dialog, 420);

    auto* toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    auto* header = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_end_title_buttons(header, FALSE);
    adw_header_bar_set_show_start_title_buttons(header, FALSE);
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    cancel_button->signal_clicked().connect(
        [dialog]() { adw_dialog_close(dialog); });
    adw_header_bar_pack_start(header, GTK_WIDGET(cancel_button->gobj()));

    auto* create_button = Gtk::make_managed<Gtk::Button>(_("Create"));
    create_button->add_css_class("suggested-action");
    create_button->set_sensitive(false);
    adw_header_bar_pack_end(header, GTK_WIDGET(create_button->gobj()));

    auto* name_entry = Gtk::make_managed<Gtk::Entry>();
    name_entry->set_placeholder_text(_("Project Name"));

    auto parent_dir = std::make_shared<std::string>();
    auto* folder_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row),
                                   _("No folder chosen"));
    auto* choose_folder_button = Gtk::make_managed<Gtk::Button>(_("Choose…"));
    choose_folder_button->set_valign(Gtk::Align::CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(folder_row),
                               GTK_WIDGET(choose_folder_button->gobj()));

    auto recompute_sensitive = std::make_shared<std::function<void()>>();
    *recompute_sensitive = [name_entry, parent_dir, create_button]() {
        create_button->set_sensitive(!name_entry->get_text().empty() &&
                                       !parent_dir->empty());
    };
    name_entry->signal_changed().connect(
        [recompute_sensitive]() { (*recompute_sensitive)(); });

    choose_folder_button->signal_clicked().connect(
        [parent, settings, parent_dir, folder_row, recompute_sensitive]() {
            auto file_dialog = Gtk::FileDialog::create();
            file_dialog->set_title(_("Choose Parent Folder"));
            bridge_apply_last_folder(file_dialog, settings, kLastProjectParentFolderKey);
            file_dialog->select_folder(
                *parent, [file_dialog, settings, parent_dir, folder_row,
                           recompute_sensitive](Glib::RefPtr<Gio::AsyncResult>& result) {
                    try {
                        auto folder = file_dialog->select_folder_finish(result);
                        *parent_dir = folder->get_path();
                        settings->set_string(kLastProjectParentFolderKey, *parent_dir);
                        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(folder_row),
                                                        parent_dir->c_str());
                        (*recompute_sensitive)();
                    } catch (const Glib::Error&) {
                        // User cancelled the picker.
                    }
                });
        });

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);
    box->append(*name_entry);
    auto* folder_list = Gtk::make_managed<Gtk::ListBox>();
    folder_list->add_css_class("boxed-list");
    folder_list->set_selection_mode(Gtk::SelectionMode::NONE);
    folder_list->append(*Glib::wrap(folder_row, false));
    box->append(*folder_list);
    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(box->gobj()));
    adw_dialog_set_child(dialog, GTK_WIDGET(toolbar_view));

    create_button->signal_clicked().connect(
        [dialog, parent, settings, name_entry, parent_dir, on_created]() {
            std::string title = name_entry->get_text();
            std::string dir = create_project_directory(settings, *parent_dir, title);
            if (dir.empty()) {
                bridge_show_error_dialog(parent, _("Couldn't Create Project"),
                                           _("The project folder couldn't be created."));
                return;
            }
            Protocol protocol;
            protocol.title = title;
            protocol.source_path = project_protocol_path(dir);
            if (!write_protocol_file(protocol.source_path, protocol)) {
                bridge_show_error_dialog(parent, _("Couldn't Create Project"),
                                           _("protocol.md couldn't be written."));
                return;
            }
            adw_dialog_close(dialog);
            on_created(dir, protocol);
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

// The "Open Project…" flow — a folder picker, then whatever's wired to
// open a project by directory (on_activate() sets this to
// open_project_at()) — factored out so both the project dropdown's icon
// button and the Edit Collection dialog's matching icon can trigger it.
// `on_done`, if set, fires once a project was actually opened — the Edit
// Collection dialog's leading action uses it to refresh its own row list.
void start_open_project_dialog(Gtk::Window* parent,
                                  const Glib::RefPtr<Gio::Settings>& settings,
                                  std::function<void()> on_done) {
    auto file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title(_("Open Project"));
    bridge_apply_last_folder(file_dialog, settings, kLastProjectParentFolderKey);
    file_dialog->select_folder(
        *parent, [file_dialog, settings, on_done](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto folder = file_dialog->select_folder_finish(result);
                bridge_remember_last_folder(settings, kLastProjectParentFolderKey, folder);
                bridge_open_project_by_dir(folder->get_path());
                if (on_done) {
                    on_done();
                }
            } catch (const Glib::Error&) {
                // User cancelled the picker.
            }
        });
}

// The recent-projects list editor ("Edit Collection…") — recent-projects
// is this-machine-and-this-user-only, so it lives in gsettings, unlike
// sessions.json/images.json, which travel with the project directory.
// Just the list, and a "Remove from list" action — no reorder (MRU order
// isn't user-rearrangeable) and no visibility (project has no hidden
// concept), so this is the simplest CollectionEditorConfig of the three.
// "Remove from list" has no confirmation dialog, deliberately: it's only
// locally destructive within this dropdown (drops a recents entry), never
// touches the project directory itself.
void show_project_collection_editor(Gtk::Window* parent,
                                      const Glib::RefPtr<Gio::Settings>& settings,
                                      std::function<void()> on_changed) {
    CollectionEditorConfig config;
    config.title = _("Projects");
    config.list_ids = [settings]() { return get_recent_projects(settings); };
    config.row_content = [](const std::string& dir) {
        auto parsed = parse_protocol_file(project_protocol_path(dir));
        CollectionRowContent content;
        std::string title = parsed ? resolved_title(*parsed) : "";
        content.title = !title.empty() ? title : Glib::path_get_basename(dir);
        content.subtitle = parsed ? resolved_description(*parsed) : "";
        return content;
    };
    config.leading_actions.push_back(CollectionAction{
        "plus-symbolic.svg", _("New Project…"),
        [](std::function<void()> notify_changed) {
            bridge_start_new_project(notify_changed);
        }});
    config.leading_actions.push_back(CollectionAction{
        "document-open-symbolic.svg", _("Open Project…"),
        [parent, settings](std::function<void()> notify_changed) {
            start_open_project_dialog(parent, settings, notify_changed);
        }});
    config.destructive.emplace();
    config.destructive->label = _("Remove from list");
    config.destructive->remove = [settings](const std::vector<std::string>& ids) {
        for (const auto& dir : ids) {
            forget_recent_project(settings, dir);
        }
    };
    config.on_changed = on_changed;
    show_collection_editor(parent, std::move(config));
}

// "Project Details" — pre-filled with the *effective* current title/
// description (resolved_title()/resolved_description(), not the raw
// override), same "editing starts from what's actually showing" rule
// show_image_metadata() (main.cpp) already established. Only Title/
// Description are editable; there's no file-metadata equivalent here to
// show read-only alongside them the way image's Creator/Copyright/Slug
// are.
void show_project_details_dialog(
    Gtk::Window* parent, const Protocol& current,
    std::function<void(const std::string& frontmatter_title,
                        const std::string& frontmatter_description)>
        on_save) {
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, _("Project Details"));
    adw_dialog_set_content_width(dialog, 420);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::NONE);

    auto* title_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(title_row), _("Title"));
    gtk_editable_set_text(GTK_EDITABLE(title_row), resolved_title(current).c_str());
    list_box->append(*Glib::wrap(title_row, false));

    auto* description_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(description_row), _("Description"));
    gtk_editable_set_text(GTK_EDITABLE(description_row), resolved_description(current).c_str());
    list_box->append(*Glib::wrap(description_row, false));

    // Read-only — where protocol.md itself actually lives on disk. Frees
    // the popover/editor row's own tooltip to show the subtitle instead
    // of this path.
    auto* location_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(location_row), _("Location"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(location_row), current.source_path.c_str());
    adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(location_row), 0);
    list_box->append(*Glib::wrap(location_row, false));

    box->append(*list_box);

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
        [dialog, title_row, description_row, on_save]() {
            std::string title = gtk_editable_get_text(GTK_EDITABLE(title_row));
            std::string description = gtk_editable_get_text(GTK_EDITABLE(description_row));
            adw_dialog_close(dialog);
            on_save(title, description);
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

// The project dropdown's popover — row content/leading actions mirror
// show_project_collection_editor() above almost exactly (same bridges,
// same New/Open wiring); the two differ in what a row click does (open
// the project, not select it for removal) and in having a right-side
// Details action the editor dialog doesn't.
ProjectMenuHandle show_project_collection_menu(
    Gtk::MenuButton* button, Gtk::Window* parent,
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::shared_ptr<Protocol>& active_protocol,
    std::function<void()> on_project_updated) {
    CollectionMenuConfig config;
    config.list_ids = [settings]() { return get_recent_projects(settings); };
    config.row_content = [](const std::string& dir) {
        auto parsed = parse_protocol_file(project_protocol_path(dir));
        CollectionRowContent content;
        std::string title = parsed ? resolved_title(*parsed) : "";
        content.title = !title.empty() ? title : Glib::path_get_basename(dir);
        content.subtitle = parsed ? resolved_description(*parsed) : "";
        return content;
    };
    config.activate = [](const std::string& dir) { bridge_open_project_by_dir(dir); };
    // Same dir-from-source_path logic as main.cpp's own
    // project_dir_from_protocol() — duplicated rather than bridged, same
    // "trivial and pure enough" call as project_protocol_path() above.
    config.is_current = [active_protocol](const std::string& dir) {
        return !active_protocol->source_path.empty() &&
               Glib::path_get_dirname(active_protocol->source_path) == dir;
    };
    config.leading_actions.push_back(CollectionAction{
        "plus-symbolic.svg", _("New Project…"),
        [](std::function<void()> notify_changed) {
            bridge_start_new_project(notify_changed);
        }});
    config.leading_actions.push_back(CollectionAction{
        "document-open-symbolic.svg", _("Open Project…"),
        [parent, settings](std::function<void()> notify_changed) {
            start_open_project_dialog(parent, settings, notify_changed);
        }});
    config.leading_actions.push_back(CollectionAction{
        "reorder-symbolic.svg", _("Edit Collection…"),
        [parent, settings](std::function<void()> notify_changed) {
            show_project_collection_editor(parent, settings, notify_changed);
        }});
    config.details_action = CollectionAction{
        "info-outline-symbolic.svg", _("Project Details"),
        [parent, active_protocol, on_project_updated](std::function<void()> notify_changed) {
            // Sensitivity (main.cpp's update_project_button_label(), via
            // the details_button this function hands back) already
            // gates this under normal use — same silent no-op guard
            // session/image's own Details buttons use, not a user-
            // visible error, since reaching here with no active project
            // shouldn't happen via the button itself.
            if (active_protocol->source_path.empty()) {
                return;
            }
            auto change_guard =
                std::make_shared<ExternalChangeGuard>(active_protocol->source_path);
            show_project_details_dialog(
                parent, *active_protocol,
                [parent, active_protocol, on_project_updated, notify_changed,
                 change_guard](const std::string& frontmatter_title,
                                const std::string& frontmatter_description) {
                    // Skip the write entirely if Save was clicked without
                    // actually changing either field — an extra safety
                    // net against overwriting the file for what would
                    // otherwise be a no-op.
                    if (frontmatter_title == active_protocol->frontmatter_title &&
                        frontmatter_description == active_protocol->frontmatter_description) {
                        return;
                    }
                    if (change_guard->has_changed()) {
                        bridge_show_error_dialog(
                            parent, _("Couldn't Save"),
                            _("protocol.md was changed by something else while "
                              "this dialog was open — your edit was not saved. "
                              "Reopen and try again."));
                        return;
                    }
                    active_protocol->frontmatter_title = frontmatter_title;
                    active_protocol->frontmatter_description = frontmatter_description;
                    write_protocol_file(active_protocol->source_path, *active_protocol);
                    on_project_updated();
                    notify_changed();
                });
        }};

    auto handle = show_collection_menu(button, config);
    ProjectMenuHandle result;
    result.refresh_project_list = handle.refresh_list;
    result.project_details_button = handle.details_button;
    return result;
}
