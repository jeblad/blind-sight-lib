// SPDX-License-Identifier: GPL-3.0-only
//
// textures.hpp — the overlay textures/cursor drawn over the displayed
// image: the gaze cursor circle, the head-direction cross, the prod-
// event glow, the splash image (bat logo composited on splash-
// image.jpeg), and the "No image" placeholder card.
//
// Scope note: build_splash() stays in main.cpp — it takes
// RecordingSession/PlaybackSession directly, and it's the splash-screen
// *widget tree* builder, not texture rendering itself.
// render_prod_cue_texture() stays file-local to textures.cpp — nothing
// outside show_prod_cue() ever calls it directly.

#pragma once

#include <cstdint>

#include <gdkmm.h>
#include <giomm.h>
#include <gtkmm.h>

// Diameter (px) the gaze cursor/circle should render at on `widget`, for
// the configured viewing distance/FOV — a fixed angular size
// (kFovealRadiusDeg) converted to pixels at the widget's own current
// width.
int gaze_cursor_diameter_px(Gtk::Widget& widget);

// The gaze-circle drawing itself — outline only, a ring rather than a
// filled disc. Shared by the live gaze cursor and Playback's replayed
// marker.
Glib::RefPtr<Gdk::Texture> render_gaze_circle_texture(int size);

Glib::RefPtr<Gdk::Cursor> make_gaze_cursor(Gtk::Widget& widget);

// Pixel offset from the viewport's own vertical center for a given
// elevation angle.
int elevation_to_pixel_offset(double elevation_deg, int viewport_height_px,
                               double fov_degrees);
// Total pixel height needed to cover the head-direction cross's full
// elevation range at the given viewport size/FOV.
int head_direction_cross_height_px(int viewport_height_px, double fov_degrees);
Glib::RefPtr<Gdk::Texture> render_head_direction_cross_texture(
    int width, int viewport_height_px, double fov_degrees);

// Shows the prod-event glow centered at a normalized ([0,65535]) position
// over `reference`, then fades it out over prod-cue-duration-ms. Shared
// by the live Record click handler and Playback's ProdEvent dispatch.
void show_prod_cue(Gtk::Picture* cue, Gtk::Widget* reference,
                   const Glib::RefPtr<Gio::Settings>& settings, std::uint16_t x,
                   std::uint16_t y);

// splash-image.jpeg with the bat logo composited on top. Cached
// internally (the splash never changes at runtime).
Glib::RefPtr<Gdk::Texture> render_splash_texture();

// test-image.png with a localized "NO IMAGE" baked directly into the
// pixels, at test-image.png's own native resolution.
Glib::RefPtr<Gdk::Texture> render_no_image_placeholder_texture(
    Gtk::Widget& font_context_widget);
