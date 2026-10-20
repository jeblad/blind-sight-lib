// SPDX-License-Identifier: GPL-3.0-only
//
// images.hpp — data model and file I/O for a project's images.json.
//
// Same reasoning as sessions.hpp: an image a project uses lives inside
// the project, at <project_dir>/images/<id>/<original filename>, so the
// project stays self-contained and portable. The per-image directory
// also gives every per-image auxiliary file (today: the depth map,
// depth_file_path_for() in main.cpp) a natural, shared home next to the
// image itself.
//
// images.json, not gsettings, is authoritative for the image list's
// order: gsettings is this-machine-only and doesn't travel with the
// project. The one thing still read from gsettings is the recent-
// projects list, which is genuinely user-specific.

#pragma once

#include <string>
#include <vector>

namespace blind_sight {

constexpr int kImagesIndexFormatVersion = 1;

// One image's entry in images.json. `id` is the image's directory name
// under <project_dir>/images/ — an opaque hash, same convention as
// SessionEntry::id and for the same reason: renaming the source file or
// retitling the image later never has to rename a directory a session
// recording may already reference.
//
// `slug` is the human-facing identifier — protocol.md references an
// image by slug, as a plain inline link whose target is the slug itself
// rather than a path or the opaque id, since a slug has no characters
// that break Markdown link syntax. Auto-generated at import time
// (generate_image_slug(), below), never edited in the UI — shown
// read-only in Image Details so protocol.md can be hand-written against it.
//
// `filename` is the original file's basename, kept as-is — unlike a
// session's fixed "session.bs", an image's filename varies per image,
// so it has to be stored explicitly: the full path is
// "<project_dir>/images/<id>/<filename>" (image_file_path(), below, is
// the one place this gets assembled).
//
// `order`/`hidden` mirror SessionEntry's fields exactly: `order` is the
// image collection editor's Move Up/Move Down position; `hidden` is the
// same eye-toggle visibility concept.
//
// `ref_count` — how many sessions currently reference this image at
// least once, deduplicated per session (switching to the same image
// across several tracks still only counts once): the question this
// answers is "how many sessions would break if I deleted this image."
// Maintained incrementally by main.cpp rather than recomputed by
// scanning every session on demand. While nonzero, the collection
// editor's delete button doesn't exist for this image at all — a
// session's `ImageEvent.path` would otherwise go silently missing.
//
// `title_override`/`caption_override` — a test run sometimes gives an
// image a different interpretation than whatever is embedded in the
// file's own IPTC/XMP tags; these hold that per-project correction
// without touching the image file itself (which may be reused elsewhere
// with its original meaning intact). Empty means "no override, use the
// file's own title/caption" — the primary/fallback relationship, not a
// separate concept from the file metadata. Edited via the Image Details
// dialog, never auto-populated.
struct ImageEntry {
    std::string id;
    std::string slug;
    std::string filename;
    int order = 0;
    bool hidden = false;
    int ref_count = 0;
    std::string title_override;
    std::string caption_override;
};

struct ImagesIndex {
    int version = kImagesIndexFormatVersion;
    std::vector<ImageEntry> images;
};

// Reads <project_dir>/images.json — an empty ImagesIndex (not a hard
// failure) if the file doesn't exist yet or fails to parse, same
// reasoning as read_sessions_index().
ImagesIndex read_images_index(const std::string& project_dir);

// Writes images.json back out, overwriting whatever was there.
bool write_images_index(const std::string& project_dir, const ImagesIndex& index);

// Generates a new, not-yet-used image ID for this project — same hash-
// of-time-plus-entropy scheme as generate_session_id() (sessions.hpp),
// duplicated rather than shared since the only difference is which
// subfolder collision-checks run against.
std::string generate_image_id(const std::string& project_dir);

// The one place "<project_dir>/images/<id>/<original_filename>" gets
// spelled out, so main.cpp never hand-assembles it. original_filename is
// kept as-is, not slugified or renamed — only the directory name is the
// opaque id.
std::string image_file_path(const std::string& project_dir, const std::string& id,
                              const std::string& original_filename);

// Jekyll-style transliteration, shared by both project directory names
// (create_project_directory(), main.cpp) and image slugs below:
// lowercase, æ/ø/å spelled out, every other non-[a-z0-9] run collapsed
// to a single hyphen, leading/trailing hyphens trimmed. Returns "" if
// nothing sluggable survives — the fallback for that case is each
// caller's own concern (generate_image_slug(), below, uses "image").
std::string slugify(const std::string& text);

// Slugifies original_filename's stem and makes it unique within
// project_dir's current images.json, appending "-2", "-3", … on
// collision — checked directly against images.json's current entries
// rather than a separate monotonic gsettings counter.
std::string generate_image_slug(const std::string& project_dir,
                                  const std::string& original_filename);

}  // namespace blind_sight
