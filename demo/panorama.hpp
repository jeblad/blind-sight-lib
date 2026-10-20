// SPDX-License-Identifier: GPL-3.0-only
//
// panorama.hpp — equirectangular/cylindrical panorama detection and
// viewport rendering: pinhole camera, choose yaw/pitch + FoV, per output
// pixel ray direction -> lon/lat -> bilinear-sample the source.
//
// Scope note: render_playback_view() stays in main.cpp — it takes
// PlaybackSession directly, which has no header of its own yet.
// ImageMetadata/read_image_metadata() (general IPTC/XMP title/caption/
// creator reading, not panorama-specific) also stay, even though they
// sit right next to PanoramaMetadata/read_gpano_metadata() in the
// original file.

#pragma once

#include <gdkmm.h>
#include <glibmm.h>

#include <cstdint>
#include <optional>
#include <string>

// What equirect_pixel_uv() needs beyond the camera ray itself — bundled
// rather than a growing parameter list. `cylindrical`: true cylindrical
// (tan-based) projection, used only as a fallback for a wide image with
// no usable GPano metadata. Otherwise (the equirect family):
// `crop_left`/`crop_top` is where this image's pixels sit within the
// hypothetical full 360°x180° sphere it's a crop of. A plain, uncropped
// equirect photosphere is just the degenerate case with
// crop_left/top_frac == 0 and crop_width/height_frac == 1.
struct PanoramaProjection {
    bool cylindrical = false;
    double cyl_focal_px = 0.0;  // cylindrical only
    double src_w = 1.0;         // equirect family only — the loaded image's
    double src_h = 1.0;         // own actual size (cylindrical only uses src_h)
    double crop_left_frac = 0.0;
    double crop_top_frac = 0.0;
    double crop_width_frac = 1.0;
    double crop_height_frac = 1.0;
};

// Fills in the per-actual-image parts of a PanoramaProjection that were
// left at their defaults when the session's copy was built at
// image-load time — `src_w`/`src_h` always come from the actually-
// loaded pixbuf, never the session.
PanoramaProjection resolve_panorama_projection(PanoramaProjection proj, int src_w,
                                                int src_h);

// Whether vertical panning makes sense for this image at all — false for
// anything that isn't a genuinely full 180°-tall sphere.
bool panorama_supports_vertical_panning(const PanoramaProjection& proj);
// Whether horizontal panning wraps at all, or is bounded at the content's
// own real edges instead.
bool panorama_supports_horizontal_wrap(const PanoramaProjection& proj);
// Yaw range (centidegrees) allowed when panning is bounded rather than
// wrapping.
void panorama_yaw_bounds_centidegrees(const PanoramaProjection& proj,
                                       double fov_degrees, double aspect,
                                       std::int32_t& min_yaw_centideg,
                                       std::int32_t& max_yaw_centideg);

// Shared by load_image()/load_playback_image()'s detection blocks:
// GPano XMP metadata checked first, then aspect-ratio heuristics, then
// plain flat. std::nullopt means no viewport synthesis.
std::optional<PanoramaProjection> detect_panorama_projection(const std::string& path,
                                                              int width, int height);

// For one output pixel: camera-space ray direction rotated by
// yaw/pitch, converted to source (u, v). The expensive part (sqrt,
// atan2, asin) of render_equirect_viewport(), exported separately
// since compute_prod_distance_m() (main.cpp) also needs the ray's true
// world-frame (lon, lat) via out_lon/out_lat.
void equirect_pixel_uv(int x, int y, int out_width, int out_height,
                        double tan_half_fov, double aspect, double cos_yaw,
                        double sin_yaw, double cos_pitch, double sin_pitch,
                        const PanoramaProjection& proj, double& u, double& v,
                        double* out_lon = nullptr, double* out_lat = nullptr);

// Renders `source` (an equirect or cylindrical panorama) into an
// out_width x out_height viewport at the given FoV/yaw/pitch. See
// panorama.cpp for the grid-interpolation + multi-threading this needs
// to stay interactive.
Glib::RefPtr<Gdk::Pixbuf> render_equirect_viewport(
    const Glib::RefPtr<Gdk::Pixbuf>& source, int out_width, int out_height,
    double fov_degrees, double yaw_degrees, double pitch_degrees,
    PanoramaProjection proj = {});
