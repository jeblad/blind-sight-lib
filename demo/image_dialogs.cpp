// SPDX-License-Identifier: GPL-3.0-only
//
// image_dialogs.cpp — see image_dialogs.hpp for scope.

#include "image_dialogs.hpp"

#include <glib/gi18n.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>

#include "collection_editor.hpp"
#include "collection_menu.hpp"
#include "depth.hpp"
#include "external_change_guard.hpp"

using blind_sight::generate_image_id;
using blind_sight::image_file_path;
using blind_sight::ImageEntry;
using blind_sight::read_images_index;
using blind_sight::slugify;
using blind_sight::write_images_index;

// Bridges into main.cpp's anonymous namespace — these stay defined in
// main.cpp but are needed here too. apply_last_folder()/remember_last_
// folder()/confirm_delete_entry() are generic, shared with the
// project/session dialogs.
void bridge_apply_last_folder(const Glib::RefPtr<Gtk::FileDialog>& file_dialog,
                                const Glib::RefPtr<Gio::Settings>& settings,
                                const char* key);
void bridge_remember_last_folder(const Glib::RefPtr<Gio::Settings>& settings,
                                   const char* key, const Glib::RefPtr<Gio::File>& file);
void bridge_confirm_delete_entry(Gtk::Window* parent, const Glib::ustring& title,
                                   const Glib::ustring& body,
                                   std::function<void()> on_delete);
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                 const Glib::ustring& body);
// {title, caption}/{creator, copyright} — IPTC/XMP metadata
// (ImageMetadata, main.cpp's own read_image_metadata()), same fields the
// image dropdown's own rows show, so the collection editor's rows match
// them. std::pair rather than ImageMetadata itself, which has internal
// linkage to main.cpp. Split into two bridges rather than one 4-tuple —
// row_content (below) only ever needs the first pair.
std::pair<Glib::ustring, Glib::ustring> bridge_read_image_title_caption(
    const std::string& path);
std::pair<Glib::ustring, Glib::ustring> bridge_read_image_creator_copyright(
    const std::string& path);
void bridge_dismiss_splash();

namespace {

constexpr auto kLastImageFolderKey = "last-image-folder";
constexpr auto kProjectProtocolFileName = "protocol.md";

// Trivial and pure enough to just duplicate rather than bridge, same
// as project_dialogs.cpp's copy — main.cpp keeps its own too.
std::string project_protocol_path(const std::string& project_dir) {
    return Glib::build_filename(project_dir, kProjectProtocolFileName);
}

// Shared by sorted_image_entries() here and sorted_session_entries()
// (session_dialogs.cpp) — both just sort by `.order`. Duplicated per
// file rather than shared via a header.
template <typename Entry>
std::vector<Entry> sorted_by_order(std::vector<Entry> entries) {
    std::stable_sort(entries.begin(), entries.end(),
                      [](const Entry& a, const Entry& b) { return a.order < b.order; });
    return entries;
}

}  // namespace

std::vector<ImageEntry> sorted_image_entries(const std::string& project_dir) {
    return sorted_by_order(read_images_index(project_dir).images);
}

std::vector<std::string> visible_image_paths(const std::string& project_dir) {
    std::vector<std::string> paths;
    for (const auto& entry : sorted_image_entries(project_dir)) {
        if (!entry.hidden) {
            paths.push_back(image_file_path(project_dir, entry.id, entry.filename));
        }
    }
    return paths;
}

namespace {

std::optional<ImageEntry> find_image_entry(const std::string& project_dir,
                                            const std::string& id) {
    for (const auto& entry : read_images_index(project_dir).images) {
        if (entry.id == id) {
            return entry;
        }
    }
    return std::nullopt;
}

// The reverse lookup — needed specifically for Image Details inside the
// popover (show_image_collection_menu(), below), which only ever knows
// the *current path*, never an id. Avoids needing main.cpp's own
// image_id_from_path()/slug_for_image_path() (internal linkage, no
// bridge exists) by reconstructing each candidate's own path instead.
std::optional<ImageEntry> find_image_entry_by_path(const std::string& project_dir,
                                                    const std::string& path) {
    for (const auto& entry : read_images_index(project_dir).images) {
        if (image_file_path(project_dir, entry.id, entry.filename) == path) {
            return entry;
        }
    }
    return std::nullopt;
}

}  // namespace

// Picks one or more image files, copies each into its own new
// <project_dir>/images/<id>/ directory (original filename kept as-is),
// and appends a fresh ImageEntry per file to images.json — factored out
// so the image dropdown's header icon can trigger the identical import
// without the Edit Collection dialog needing to be open. on_imported
// runs once, only if at least one file was actually added. No duplicate
// check against already-imported files — a user importing several
// near-identical images may have a real reason to.
void import_images_to_collection(Gtk::Window* parent,
                                   const Glib::RefPtr<Gio::Settings>& settings,
                                   const std::string& project_dir,
                                   std::function<void()> on_imported) {
    auto image_filter = Gtk::FileFilter::create();
    image_filter->set_name(_("Images"));
    image_filter->add_mime_type("image/jpeg");
    image_filter->add_mime_type("image/png");
    auto image_filters = Gio::ListStore<Gtk::FileFilter>::create();
    image_filters->append(image_filter);

    auto file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title(_("Import Images to Collection"));
    file_dialog->set_filters(image_filters);
    file_dialog->set_default_filter(image_filter);
    bridge_apply_last_folder(file_dialog, settings, kLastImageFolderKey);
    file_dialog->open_multiple(
        *parent, [file_dialog, settings, project_dir,
                   on_imported](Glib::RefPtr<Gio::AsyncResult>& result) {
            try {
                auto files = file_dialog->open_multiple_finish(result);
                if (!files.empty()) {
                    bridge_remember_last_folder(settings, kLastImageFolderKey,
                                          files.front());
                }
                auto index = read_images_index(project_dir);
                int next_order = static_cast<int>(index.images.size());
                bool changed = false;
                // Slug collisions are checked against `index` in-memory,
                // not via generate_image_slug() (which re-reads
                // images.json from disk) — several files picked in the
                // same batch can share a stem, and nothing's written
                // back until the loop below finishes, so a disk-reading
                // check would hand out the same slug twice.
                auto slug_collides = [&index](const std::string& candidate) {
                    return std::any_of(
                        index.images.begin(), index.images.end(),
                        [&candidate](const ImageEntry& e) {
                            return e.slug == candidate;
                        });
                };
                for (const auto& file : files) {
                    std::string source_path = file->get_path();
                    std::string filename = Glib::path_get_basename(source_path);
                    std::string id = generate_image_id(project_dir);
                    std::string dest_path =
                        image_file_path(project_dir, id, filename);
                    std::error_code ec;
                    std::filesystem::create_directories(
                        std::filesystem::path(dest_path).parent_path(), ec);
                    if (ec) {
                        continue;
                    }
                    std::filesystem::copy_file(source_path, dest_path, ec);
                    if (ec) {
                        continue;
                    }
                    // A depth map already made for this image comes along,
                    // rather than the project generating a new one on first
                    // open. A failed copy only costs that regeneration.
                    std::string source_depth = depth_file_path_for(source_path);
                    if (std::filesystem::exists(source_depth)) {
                        std::filesystem::copy_file(source_depth, depth_file_path_for(dest_path),
                                                   ec);
                    }
                    std::string base = slugify(
                        std::filesystem::path(filename).stem().string());
                    if (base.empty()) {
                        base = "image";
                    }
                    std::string slug = base;
                    for (int n = 2; slug_collides(slug); ++n) {
                        slug = base + "-" + std::to_string(n);
                    }
                    ImageEntry entry;
                    entry.id = id;
                    entry.slug = slug;
                    entry.filename = filename;
                    entry.order = next_order++;
                    entry.hidden = false;
                    index.images.push_back(entry);
                    changed = true;
                }
                if (changed) {
                    write_images_index(project_dir, index);
                    if (on_imported) {
                        on_imported();
                    }
                }
            } catch (const Glib::Error&) {
                // User cancelled the picker.
            }
        });
}

// The image dropdown's "Edit Collection…" — reorder (drag handle),
// visibility (passive status icon + toolbar smart-toggle: open means the
// image shows in the dropdown's gallery; closed/crossed hides it there
// without deleting it), and real delete (with confirmation, disabled while
// a session still references the image — see ImageEntry::ref_count).
void show_image_sequence_editor(Gtk::Window* parent,
                                 const Glib::RefPtr<Gio::Settings>& settings,
                                 const std::string& project_dir,
                                 std::function<void()> on_changed) {
    CollectionEditorConfig config;
    config.title = _("Images");
    config.list_ids = [project_dir]() {
        std::vector<std::string> ids;
        for (const auto& entry : sorted_image_entries(project_dir)) {
            ids.push_back(entry.id);
        }
        return ids;
    };
    config.row_content = [project_dir](const std::string& id) {
        CollectionRowContent content;
        auto entry = find_image_entry(project_dir, id);
        if (entry) {
            std::string path =
                image_file_path(project_dir, entry->id, entry->filename);
            // Same title/subtitle as the image dropdown's own rows
            // (refresh_sequence_list(), main.cpp):
            // title_override/caption_override (ImageEntry, a test run's own
            // correction) first, then the file's own IPTC/XMP title/caption,
            // then — title only — the filename. Seeing the bare filename is
            // itself the signal to go set an override in Image Details; no
            // weaker fallback (e.g. parsing protocol.md) is worth the
            // ambiguity of the same image appearing under different text in
            // different scenarios.
            auto [meta_title, meta_caption] = bridge_read_image_title_caption(path);
            if (!entry->title_override.empty()) {
                content.title = entry->title_override;
            } else if (!meta_title.empty()) {
                content.title = meta_title;
            } else {
                content.title = Glib::path_get_basename(path);
            }
            if (!entry->caption_override.empty()) {
                content.subtitle = entry->caption_override;
            } else if (!meta_caption.empty()) {
                content.subtitle = meta_caption;
            }
        }
        return content;
    };
    config.leading_actions.push_back(CollectionAction{
        "upload-symbolic.svg", _("Import Image(s)…"),
        [parent, settings, project_dir](std::function<void()> notify_changed) {
            import_images_to_collection(parent, settings, project_dir,
                                          notify_changed);
        }});
    config.selection_action = CollectionSelectionAction{
        "info-outline-symbolic.svg", _("Image Details"),
        [parent, project_dir](const std::string& id, std::function<void()> notify_changed) {
            auto entry = find_image_entry(project_dir, id);
            if (!entry) {
                return;
            }
            std::string path = image_file_path(project_dir, entry->id, entry->filename);
            auto [file_title, file_caption] = bridge_read_image_title_caption(path);
            auto [file_creator, file_copyright] = bridge_read_image_creator_copyright(path);
            show_image_metadata(parent, project_dir, entry->id, file_title, file_caption,
                                  file_creator, file_copyright, entry->slug, notify_changed);
        }};
    config.reorder = CollectionReorder{
        [project_dir](const std::vector<std::string>& new_order_ids) {
            auto index = read_images_index(project_dir);
            for (std::size_t i = 0; i < new_order_ids.size(); ++i) {
                for (auto& image : index.images) {
                    if (image.id == new_order_ids[i]) {
                        image.order = static_cast<int>(i);
                        break;
                    }
                }
            }
            write_images_index(project_dir, index);
        }};
    config.visibility = CollectionVisibility{
        [project_dir](const std::string& id) {
            auto entry = find_image_entry(project_dir, id);
            return entry ? entry->hidden : false;
        },
        [project_dir](const std::vector<std::string>& ids, bool hidden) {
            auto index = read_images_index(project_dir);
            for (auto& image : index.images) {
                if (std::find(ids.begin(), ids.end(), image.id) != ids.end()) {
                    image.hidden = hidden;
                }
            }
            write_images_index(project_dir, index);
        }};
    config.destructive.emplace();
    config.destructive->label = _("Delete Image");
    config.destructive->enabled_for = [project_dir](const std::string& id) {
        auto entry = find_image_entry(project_dir, id);
        return entry ? entry->ref_count == 0 : false;
    };
    config.destructive->confirm = [project_dir](
                                      Gtk::Window* confirm_parent,
                                      const std::vector<std::string>& ids,
                                      std::function<void()> on_confirmed) {
        if (ids.empty()) {
            return;
        }
        auto entry = find_image_entry(project_dir, ids.front());
        if (!entry) {
            return;
        }
        std::string path =
            image_file_path(project_dir, entry->id, entry->filename);
        Glib::ustring title = Glib::path_get_basename(path);
        bool protocol_uses_it = false;
        if (!entry->slug.empty()) {
            std::ifstream in(project_protocol_path(project_dir));
            std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            protocol_uses_it = content.find("](" + entry->slug + ")") != std::string::npos;
        }
        Glib::ustring body =
            protocol_uses_it
                ? Glib::ustring::compose(
                      _("“%1” is referenced from protocol.md — deleting it will leave "
                        "that reference pointing at nothing. This can't be undone."),
                      title)
                : Glib::ustring::compose(
                      _("“%1” will be permanently deleted. This can't be undone."), title);
        bridge_confirm_delete_entry(confirm_parent, _("Delete Image?"), body, on_confirmed);
    };
    config.destructive->remove = [project_dir](const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            auto index = read_images_index(project_dir);
            auto& images = index.images;
            images.erase(std::remove_if(images.begin(), images.end(),
                                          [&id](const ImageEntry& entry) {
                                              return entry.id == id;
                                          }),
                          images.end());
            write_images_index(project_dir, index);
            std::error_code ec;
            std::filesystem::remove_all(project_dir + "/images/" + id, ec);
        }
    };
    config.on_changed = on_changed;
    show_collection_editor(parent, std::move(config));
}

// Creator/Copyright/Slug stay read-only: this project's own text never
// overrides those.
void show_image_metadata(Gtk::Window* parent, const std::string& project_dir,
                          const std::string& image_id, const std::string& file_title,
                          const std::string& file_caption, const std::string& file_creator,
                          const std::string& file_copyright, const std::string& slug,
                          std::function<void()> on_changed) {
    std::string title_override;
    std::string caption_override;
    std::string path;
    for (const auto& entry : read_images_index(project_dir).images) {
        if (entry.id == image_id) {
            title_override = entry.title_override;
            caption_override = entry.caption_override;
            path = image_file_path(project_dir, entry.id, entry.filename);
            break;
        }
    }
    // Watches the whole images.json, not just this entry — confirmed
    // choice: simpler, at the cost of also flagging an unrelated edit to
    // a *different* image as a conflict for this one.
    auto change_guard =
        std::make_shared<ExternalChangeGuard>(project_dir + "/images.json");

    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, _("Image Details"));
    adw_dialog_set_content_width(dialog, 420);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::NONE);

    auto* title_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(title_row), _("Title"));
    gtk_editable_set_text(
        GTK_EDITABLE(title_row),
        (title_override.empty() ? file_title : title_override).c_str());
    list_box->append(*Glib::wrap(title_row, false));

    auto* caption_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(caption_row), _("Caption"));
    gtk_editable_set_text(
        GTK_EDITABLE(caption_row),
        (caption_override.empty() ? file_caption : caption_override).c_str());
    list_box->append(*Glib::wrap(caption_row, false));

    struct Field {
        const char* title;
        const std::string& value;
    };
    // subtitle_lines(0) (unlimited wrap) on all four — only Location (the
    // path) actually needs it, but it's harmless on the short ones too.
    // Frees the popover/editor row's own tooltip to show the subtitle
    // instead of this path.
    for (const auto& field : {Field{_("Creator"), file_creator},
                               Field{_("Copyright"), file_copyright},
                               Field{_("Slug"), slug},
                               Field{_("Location"), path}}) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), field.title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), field.value.c_str());
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        list_box->append(*Glib::wrap(row, false));
    }
    box->append(*list_box);

    auto* button_box =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    button_box->set_halign(Gtk::Align::END);
    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    auto* save_button = Gtk::make_managed<Gtk::Button>(_("Save"));
    save_button->add_css_class("suggested-action");
    button_box->append(*cancel_button);
    button_box->append(*save_button);
    box->append(*button_box);

    adw_dialog_set_child(dialog, GTK_WIDGET(box->gobj()));

    cancel_button->signal_clicked().connect(
        [dialog]() { adw_dialog_close(dialog); });

    save_button->signal_clicked().connect(
        [dialog, parent, project_dir, image_id, title_row, caption_row, on_changed,
         title_override, caption_override, change_guard]() {
            std::string new_title_override = gtk_editable_get_text(GTK_EDITABLE(title_row));
            std::string new_caption_override =
                gtk_editable_get_text(GTK_EDITABLE(caption_row));
            // Skip the write entirely if Save was clicked without
            // actually changing either override — an extra safety net
            // against overwriting the file for what would otherwise be
            // a no-op.
            if (new_title_override == title_override &&
                new_caption_override == caption_override) {
                adw_dialog_close(dialog);
                return;
            }
            if (change_guard->has_changed()) {
                adw_dialog_close(dialog);
                bridge_show_error_dialog(
                    parent, _("Couldn't Save"),
                    _("images.json was changed by something else while this "
                      "dialog was open — your edit was not saved. Reopen and "
                      "try again."));
                return;
            }
            auto index = read_images_index(project_dir);
            for (auto& entry : index.images) {
                if (entry.id == image_id) {
                    entry.title_override = new_title_override;
                    entry.caption_override = new_caption_override;
                    write_images_index(project_dir, index);
                    break;
                }
            }
            adw_dialog_close(dialog);
            if (on_changed) {
                on_changed();
            }
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

// The image dropdown's popover. Import/Edit Collection mirror
// show_image_sequence_editor()'s own leading actions above (same error-
// dialog-on-no-project guard, same bridge-free project-dir handling);
// Image Details and row activation are new here — the Edit Collection
// dialog has a "Info" selection_action of its own already, but that acts
// on whichever row is *selected* there, not "whichever image is
// currently shown" the way this popover's Details does.
ImageMenuHandle show_image_collection_menu(
    Gtk::MenuButton* button, Gtk::Window* window,
    const Glib::RefPtr<Gio::Settings>& settings, std::function<std::string()> project_dir,
    std::function<std::string()> current_image_path,
    std::function<void(const std::string&)> activate_image,
    std::function<void(const std::string&)> update_sequence_button_label,
    std::function<void()> on_collection_edited) {
    CollectionMenuConfig config;
    // Same visible/hidden filtering as visible_image_paths() (main.cpp),
    // just emitting ids instead of paths — lets row_content/activate/
    // is_current below all work through find_image_entry() rather than
    // needing main.cpp's own image_id_from_path().
    config.list_ids = [project_dir]() {
        std::vector<std::string> ids;
        std::string dir = project_dir();
        if (dir.empty()) {
            return ids;
        }
        for (const auto& entry : sorted_image_entries(dir)) {
            if (!entry.hidden) {
                ids.push_back(entry.id);
            }
        }
        return ids;
    };
    config.row_content = [project_dir](const std::string& id) {
        CollectionRowContent content;
        auto entry = find_image_entry(project_dir(), id);
        if (!entry) {
            return content;
        }
        // Same override-then-fallback as show_image_sequence_editor()'s
        // own row_content above: title_override/caption_override (a test
        // run's own correction) first, then the file's own IPTC/XMP
        // title/caption, then — title only — the filename.
        std::string path = image_file_path(project_dir(), entry->id, entry->filename);
        auto [meta_title, meta_caption] = bridge_read_image_title_caption(path);
        if (!entry->title_override.empty()) {
            content.title = entry->title_override;
        } else if (!meta_title.empty()) {
            content.title = meta_title;
        } else {
            content.title = Glib::path_get_basename(path);
        }
        content.subtitle =
            !entry->caption_override.empty() ? entry->caption_override : std::string(meta_caption);
        return content;
    };
    // Opens that image — activate_image(), not load_image() directly,
    // since row activation is mode-dependent (see activate_image's own
    // comment, main.cpp).
    config.activate = [project_dir, activate_image](const std::string& id) {
        auto entry = find_image_entry(project_dir(), id);
        if (entry) {
            activate_image(image_file_path(project_dir(), entry->id, entry->filename));
        }
    };
    config.is_current = [project_dir, current_image_path](const std::string& id) {
        auto entry = find_image_entry(project_dir(), id);
        return entry && image_file_path(project_dir(), entry->id, entry->filename) ==
                             current_image_path();
    };
    config.leading_actions.push_back(CollectionAction{
        "upload-symbolic.svg", _("Import Image(s)…"),
        [window, settings, project_dir](std::function<void()> notify_changed) {
            std::string dir = project_dir();
            if (dir.empty()) {
                bridge_show_error_dialog(window, _("No Project"),
                                           _("Open or create a project first — "
                                             "images always live inside a project."));
                return;
            }
            import_images_to_collection(window, settings, dir, notify_changed);
        }});
    config.leading_actions.push_back(CollectionAction{
        "reorder-symbolic.svg", _("Edit Collection…"),
        [window, settings, project_dir,
         on_collection_edited](std::function<void()> notify_changed) {
            std::string dir = project_dir();
            if (dir.empty()) {
                bridge_show_error_dialog(window, _("No Project"),
                                           _("Open or create a project first — "
                                             "images always live inside a project."));
                return;
            }
            // on_changed does not eager-write protocol.md — that's an
            // externally-authored document, reconciled only on demand
            // via "Reconcile Protocol". on_collection_edited just fixes
            // up the display when the currently-shown image drops out of
            // the collection.
            show_image_sequence_editor(window, settings, dir,
                                         [notify_changed, on_collection_edited]() {
                                             notify_changed();
                                             on_collection_edited();
                                         });
        }});
    config.details_action = CollectionAction{
        "info-outline-symbolic.svg", _("Image Details"),
        [window, project_dir, current_image_path, update_sequence_button_label](
            std::function<void()> notify_changed) {
            std::string path = current_image_path();
            if (path.empty()) {
                return;
            }
            std::string dir = project_dir();
            auto entry = find_image_entry_by_path(dir, path);
            if (!entry) {
                return;
            }
            // Copied, not captured by reference — the dialog's on_changed
            // fires later, when Save is clicked, by which point the live
            // current image could already be a different one if the user
            // switched away while the dialog was still open.
            std::string path_copy = path;
            auto [file_title, file_caption] = bridge_read_image_title_caption(path);
            auto [file_creator, file_copyright] = bridge_read_image_creator_copyright(path);
            show_image_metadata(
                window, dir, entry->id, file_title, file_caption, file_creator, file_copyright,
                entry->slug, [notify_changed, update_sequence_button_label, path_copy]() {
                    notify_changed();
                    update_sequence_button_label(path_copy);
                });
        }};

    auto handle = show_collection_menu(button, config);

    // Checkmark follows along live while Playback/Record keeps changing
    // the current image, without tearing rows out from under a click —
    // same reasoning as this app's track dropdown poll. Only runs while
    // the popover is actually open.
    auto poll_connection = std::make_shared<sigc::connection>();
    handle.popover->signal_show().connect([handle, poll_connection]() {
        poll_connection->disconnect();
        *poll_connection = Glib::signal_timeout().connect(
            [handle]() {
                handle.refresh_current();
                return true;  // keep polling while the popover is open
            },
            300);
    });
    handle.popover->signal_closed().connect(
        [poll_connection]() { poll_connection->disconnect(); });
    // Splash caption hides the moment the user opens this dropdown — same
    // "first sign of interacting with the app" trigger the hamburger
    // menu gets in main.cpp.
    handle.popover->signal_show().connect([]() { bridge_dismiss_splash(); });

    // Eager initial build — matches this popover's own previous behavior
    // (main.cpp) of populating the row list immediately, not only once
    // first opened.
    handle.refresh_list();

    ImageMenuHandle result;
    result.refresh_sequence_list = handle.refresh_list;
    result.image_details_button = handle.details_button;
    return result;
}
