// SPDX-License-Identifier: GPL-3.0-only
//
// image_dialogs.hpp — the image-management UI: the project-scoped
// images.json views (sorted/visible), importing new images, deleting
// one, and the "Edit Collection" editor (reorder/hide/delete).
//
// Scope note: image_id_from_path()/slug_for_image_path()/path_for_
// image_slug() stay in main.cpp — they're used far more broadly than
// this module (the Protocol window's slug matching, image-reference
// counting), not image-dialog-specific despite the name.
// count_new_image_reference()/release_image_references() stay too, for
// the same reason (RecordingSession's image-load path).

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <functional>
#include <string>
#include <vector>

#include "images.hpp"

std::vector<blind_sight::ImageEntry> sorted_image_entries(const std::string& project_dir);
std::vector<std::string> visible_image_paths(const std::string& project_dir);

// Picks one or more image files, copies each into its own new
// <project_dir>/images/<id>/ directory, and appends a fresh ImageEntry
// per file to images.json. `on_imported` runs once, only if at least
// one file was actually added.
void import_images_to_collection(Gtk::Window* parent,
                                  const Glib::RefPtr<Gio::Settings>& settings,
                                  const std::string& project_dir,
                                  std::function<void()> on_imported);

void show_image_sequence_editor(Gtk::Window* parent,
                                 const Glib::RefPtr<Gio::Settings>& settings,
                                 const std::string& project_dir,
                                 std::function<void()> on_changed = {});

// Title/Caption are editable (ImageEntry::title_override/caption_override,
// images.hpp) — a test run sometimes gives an image a different
// interpretation than whatever is embedded in the file's own IPTC/XMP
// tags, and this is where that correction is entered. `file_title`/
// `file_caption`/`file_creator`/`file_copyright` are that file's own
// IPTC/XMP values (read by the caller — this module has no IPTC reader of
// its own, see the bridges in image_dialogs.cpp), shown read-only
// (Creator/Copyright) or as the pre-fill fallback when no override is set
// yet (Title/Caption). `slug` is application metadata (images.json's own
// id), shown read-only too, so protocol.md's own "<image:slug>" markers
// have somewhere to find it.
void show_image_metadata(Gtk::Window* parent, const std::string& project_dir,
                          const std::string& image_id, const std::string& file_title,
                          const std::string& file_caption, const std::string& file_creator,
                          const std::string& file_copyright, const std::string& slug = "",
                          std::function<void()> on_changed = {});

// Returns what main.cpp needs by name — refresh_sequence_list for the
// existing call sites that already trigger it, and the Details button
// itself (via collection_menu.hpp's own CollectionMenuHandle) so main.cpp
// can keep gating its sensitivity from its own per-frame tick callback,
// the same way it does today.
struct ImageMenuHandle {
    std::function<void()> refresh_sequence_list;
    Gtk::Button* image_details_button = nullptr;
};

// The image dropdown's popover — the project's visible image sequence
// (not an MRU list, a prepared order), Import/Edit Collection on the
// left, Image Details on the right, a checkmark on whichever image is
// current. Unlike project/session, the current image changes far more
// often than the list itself (Next/Previous during Playback/Record), so
// this keeps a 300ms poll of just the checkmark running while the
// popover stays open — same reasoning as this app's track dropdown.
//
// `project_dir`/`current_image_path` are getters rather than plain
// values — both can change while this popover stays alive for the app's
// whole lifetime. `update_sequence_button_label`/`activate_image` are
// main.cpp's own existing closures (activate_image, not a plain
// load_image, since row activation is mode-dependent — see its own
// comment, main.cpp). `on_collection_edited` is called after Edit
// Collection's own changes land, same "needs types this function never
// sees" reasoning as session's on_collection_edited.
ImageMenuHandle show_image_collection_menu(
    Gtk::MenuButton* button, Gtk::Window* window,
    const Glib::RefPtr<Gio::Settings>& settings, std::function<std::string()> project_dir,
    std::function<std::string()> current_image_path,
    std::function<void(const std::string&)> activate_image,
    std::function<void(const std::string&)> update_sequence_button_label,
    std::function<void()> on_collection_edited);
