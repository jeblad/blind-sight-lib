// SPDX-License-Identifier: GPL-3.0-only
//
// panorama.cpp — see panorama.hpp for scope.

#include "panorama.hpp"

#include <gexiv2/gexiv2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <thread>
#include <vector>

PanoramaProjection resolve_panorama_projection(PanoramaProjection proj, int src_w,
                                                int src_h) {
    proj.src_w = src_w;
    proj.src_h = src_h;
    if (proj.cylindrical) {
        proj.cyl_focal_px = src_w / (2.0 * std::numbers::pi);
    }
    return proj;
}

// Whether vertical panning makes sense for this image at all — false
// for anything that isn't a genuinely full 180°-tall sphere. The
// zoom-to-fill in equirect_pixel_uv() is only ever an *exact* fit at
// pitch exactly 0; any nonzero pitch immediately starts exposing real
// black beyond the crop's true edge, by construction. Locking pitch at
// 0 for anything short of a full sphere sidesteps the whole class of
// problem. Applies to the cylindrical fallback too, not just GPano
// crops.
bool panorama_supports_vertical_panning(const PanoramaProjection& proj) {
    return !proj.cylindrical && proj.crop_height_frac >= 0.999;
}

// Whether horizontal panning wraps at all, or is bounded at the
// content's real edges instead. The cylindrical fallback's
// `cyl_focal_px = src_w / 2π` assumption mathematically treats the
// image as spanning a full 360°, so panning far enough wrapped around
// into the *same* photo again — which looks broken, since a plain
// photo's left and right edges aren't really adjacent in the world.
// Only a real, metadata-confirmed full-width panorama genuinely wraps.
bool panorama_supports_horizontal_wrap(const PanoramaProjection& proj) {
    return !proj.cylindrical && proj.crop_width_frac >= 0.999;
}

// Yaw range (centidegrees) allowed when panning is bounded rather than
// wrapping — the range within which the viewport's *left/right edges*
// stay inside the content's real horizontal extent, not just where the
// viewport's center does. `atan(aspect * tan_half_fov)` is the
// horizontal counterpart of the vertical center-column exactness in
// equirect_pixel_uv(). Collapses both bounds to the content's midpoint
// if the viewport is wider than the content itself.
void panorama_yaw_bounds_centidegrees(const PanoramaProjection& proj,
                                       double fov_degrees, double aspect,
                                       std::int32_t& min_yaw_centideg,
                                       std::int32_t& max_yaw_centideg) {
    double fov_rad = fov_degrees * std::numbers::pi / 180.0;
    double tan_half_fov = std::tan(fov_rad / 2.0);
    double viewport_half_hfov = std::atan(aspect * tan_half_fov);
    double lon_min_content = (proj.crop_left_frac - 0.5) * 2.0 * std::numbers::pi;
    double lon_max_content =
        (proj.crop_left_frac + proj.crop_width_frac - 0.5) * 2.0 * std::numbers::pi;
    double min_yaw = lon_min_content + viewport_half_hfov;
    double max_yaw = lon_max_content - viewport_half_hfov;
    if (min_yaw > max_yaw) {
        double mid = (lon_min_content + lon_max_content) / 2.0;
        min_yaw = max_yaw = mid;
    }
    min_yaw_centideg =
        static_cast<std::int32_t>(std::lround(min_yaw * 180.0 / std::numbers::pi * 100.0));
    max_yaw_centideg =
        static_cast<std::int32_t>(std::lround(max_yaw * 180.0 / std::numbers::pi * 100.0));
}

namespace {

// Detected purely by aspect ratio, not GPano XMP metadata: a real
// equirectangular photosphere is *exactly* 2:1 by construction, so a
// tolerance-checked ratio cleanly separates it without needing to parse
// embedded XML. Same reasoning for the cylindrical threshold below —
// comfortably clear of equirect's tolerance ceiling on one side,
// comfortably below real stitched panoramas' ratios on the other.
constexpr double kEquirectAspectRatio = 2.0;
constexpr double kEquirectAspectTolerance = 0.03;
constexpr double kCylindricalPanoramaMinAspectRatio = 2.2;

bool looks_equirectangular(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    double ratio = static_cast<double>(width) / height;
    return std::abs(ratio - kEquirectAspectRatio) <= kEquirectAspectTolerance;
}

// A plain wide-but-not-equirect image — this assumes the full image
// width spans 360° horizontally, the same assumption
// render_equirect_viewport()'s cylindrical branch relies on for its
// single shared focal length.
bool looks_cylindrical_panorama(int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    double ratio = static_cast<double>(width) / height;
    return ratio >= kCylindricalPanoramaMinAspectRatio;
}

// Google's Photo Sphere XMP (the "GPano" namespace):
// `Xmp.GPano.ProjectionType`, `FullPanoWidthPixels`/`HeightPixels` (what
// the full sphere would be if this capture covered all of it),
// `CroppedAreaImageWidthPixels`/`HeightPixels` and
// `CroppedAreaLeftPixels`/`TopPixels` (where this file's pixels sit
// within that hypothetical full sphere). `has_data` is false whenever
// any field is missing/unparseable or ProjectionType isn't
// "equirectangular" — real files with no GPano data are expected, so
// the cylindrical fallback still matters.
struct PanoramaMetadata {
    bool has_data = false;
    int full_width = 0;
    int full_height = 0;
    int cropped_width = 0;
    int cropped_height = 0;
    int cropped_left = 0;
    int cropped_top = 0;
};

PanoramaMetadata read_gpano_metadata(const std::string& path) {
    PanoramaMetadata result;
    auto* metadata = gexiv2_metadata_new();
    GError* error = nullptr;
    if (gexiv2_metadata_open_path(metadata, path.c_str(), &error)) {
        auto get_int_tag = [metadata](const char* tag, int& out) {
            GError* tag_error = nullptr;
            bool ok = false;
            if (gexiv2_metadata_try_has_tag(metadata, tag, &tag_error)) {
                gchar* raw = gexiv2_metadata_try_get_tag_string(metadata, tag,
                                                                 &tag_error);
                if (raw) {
                    out = std::atoi(raw);
                    ok = out > 0;
                    g_free(raw);
                }
            }
            if (tag_error) {
                g_error_free(tag_error);
            }
            return ok;
        };
        GError* proj_error = nullptr;
        gchar* projection_type = gexiv2_metadata_try_has_tag(
                                      metadata, "Xmp.GPano.ProjectionType", &proj_error)
                                      ? gexiv2_metadata_try_get_tag_string(
                                            metadata, "Xmp.GPano.ProjectionType", &proj_error)
                                      : nullptr;
        bool is_equirectangular =
            projection_type && std::string(projection_type) == "equirectangular";
        if (projection_type) {
            g_free(projection_type);
        }
        if (proj_error) {
            g_error_free(proj_error);
        }
        result.has_data =
            is_equirectangular &&
            get_int_tag("Xmp.GPano.FullPanoWidthPixels", result.full_width) &&
            get_int_tag("Xmp.GPano.FullPanoHeightPixels", result.full_height) &&
            get_int_tag("Xmp.GPano.CroppedAreaImageWidthPixels", result.cropped_width) &&
            get_int_tag("Xmp.GPano.CroppedAreaImageHeightPixels", result.cropped_height);
        // Left/Top default to 0 (a valid, common value — no crop on that
        // axis) rather than making the whole result invalid if absent.
        get_int_tag("Xmp.GPano.CroppedAreaLeftPixels", result.cropped_left);
        get_int_tag("Xmp.GPano.CroppedAreaTopPixels", result.cropped_top);
    }
    if (error) {
        g_error_free(error);
    }
    g_object_unref(metadata);
    return result;
}

}  // namespace

// Shared by load_image() and load_playback_image()'s detection blocks
// so this 3-way "GPano, then aspect-ratio heuristics, then flat"
// decision exists exactly once. GPano checked first and
// unconditionally — real metadata always wins over a guess; the
// aspect-ratio checks are what's left for images with no usable GPano
// data.
std::optional<PanoramaProjection> detect_panorama_projection(const std::string& path,
                                                              int width, int height) {
    PanoramaMetadata gpano = read_gpano_metadata(path);
    if (gpano.has_data) {
        PanoramaProjection proj;
        // Fractions of the full sphere, not raw pixel counts: GPano's
        // pixel fields are in the *original* captured resolution, but
        // the loaded pixbuf is downscaled whenever the original is
        // larger, so mixing raw metadata pixels with the loaded image
        // in one formula would silently use two different scales.
        proj.crop_left_frac = static_cast<double>(gpano.cropped_left) / gpano.full_width;
        proj.crop_top_frac = static_cast<double>(gpano.cropped_top) / gpano.full_height;
        proj.crop_width_frac = static_cast<double>(gpano.cropped_width) / gpano.full_width;
        proj.crop_height_frac = static_cast<double>(gpano.cropped_height) / gpano.full_height;
        return proj;
    }
    if (looks_equirectangular(width, height)) {
        return PanoramaProjection{};  // plain equirect — defaults fill in at render time
    }
    if (looks_cylindrical_panorama(width, height)) {
        PanoramaProjection proj;
        proj.cylindrical = true;
        return proj;
    }
    return std::nullopt;
}

void equirect_pixel_uv(int x, int y, int out_width, int out_height,
                        double tan_half_fov, double aspect, double cos_yaw,
                        double sin_yaw, double cos_pitch, double sin_pitch,
                        const PanoramaProjection& proj, double& u, double& v,
                        double* out_lon, double* out_lat) {
    double ndc_y = (1.0 - 2.0 * (y + 0.5) / out_height) * tan_half_fov;
    double ndc_x = (2.0 * (x + 0.5) / out_width - 1.0) * aspect * tan_half_fov;

    // Camera space: looking down -Z, +X right, +Y up.
    double dx = ndc_x, dy = ndc_y, dz = -1.0;
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    dx /= len;
    dy /= len;
    dz /= len;

    // Pitch: rotate around X (look up/down).
    double y1 = dy * cos_pitch - dz * sin_pitch;
    double z1 = dy * sin_pitch + dz * cos_pitch;
    // Yaw: rotate around Y (look left/right).
    double x2 = dx * cos_yaw + z1 * sin_yaw;
    double z2 = -dx * sin_yaw + z1 * cos_yaw;

    double lon = std::atan2(x2, -z2);
    double lat = std::asin(std::clamp(y1, -1.0, 1.0));
    // The ray's true world-frame angle — independent of however it
    // ends up mapped into source-image pixels below. Only requested by
    // the "distance at pointer" readout, which needs real
    // azimuth/elevation, not a projection-specific texture coordinate.
    if (out_lon) {
        *out_lon = lon;
    }
    if (out_lat) {
        *out_lat = lat;
    }
    if (proj.cylindrical) {
        // Horizontal is linear in angle either way — same "image width
        // spans 360°" assumption looks_cylindrical_panorama() itself
        // already makes, for lack of any real metadata to do better.
        u = lon / (2.0 * std::numbers::pi) + 0.5;
        v = 0.5 - (proj.cyl_focal_px * std::tan(lat)) / proj.src_h;
    } else {
        double u_full = lon / (2.0 * std::numbers::pi) + 0.5;
        // "Zoom to fill" the viewport vertically rather than leaving
        // real black gaps when the crop covers less vertical angle than
        // the viewport's FOV. Scales `lat` so the crop's true
        // half-extent lines up with the viewport's half-extent —
        // clamped to never exceed 1.0, so a full, uncropped photosphere
        // renders exactly as before.
        //
        // Centered on the *crop's* true vertical center
        // (crop_center_v_full), not the world horizon: a GPano crop's
        // vertical center can sit a couple of degrees off the true
        // horizon, so zooming around the horizon instead would push the
        // effective view too far toward one edge, exposing black at
        // the other.
        double crop_center_v_full = proj.crop_top_frac + proj.crop_height_frac / 2.0;
        double viewport_half_vfov = std::atan(tan_half_fov);
        double crop_half_vfov = proj.crop_height_frac * (std::numbers::pi / 2.0);
        double lat_scale = viewport_half_vfov > 0.0
                                ? std::min(1.0, crop_half_vfov / viewport_half_vfov)
                                : 1.0;
        double v_full = crop_center_v_full - (lat * lat_scale) / std::numbers::pi;
        // Linear in angle on both axes against the *full* hypothetical
        // sphere, then translated into this image's [0,1) fraction by
        // the crop's fraction of that sphere. Plain equirect falls out
        // of this for free at the default crop_left/top_frac=0,
        // crop_width/height_frac=1: u = (u_full - 0)/1 = u_full.
        u = (u_full - proj.crop_left_frac) / proj.crop_width_frac;
        v = (v_full - proj.crop_top_frac) / proj.crop_height_frac;
    }
}

namespace {

// render_equirect_viewport() renders at this granularity rather than
// per-pixel — 16 verified (throwaway bench/diff harness, worst cases
// checked: near the ±180° yaw seam and near the pitch clamp close to
// the pole) to be visually indistinguishable from a full per-pixel
// render while cutting the expensive part (sqrt/atan2/asin) by
// ~kEquirectGridStep².
constexpr int kEquirectGridStep = 16;

// Corner values of the last-fetched source texel, keyed by that
// texel's (x0, y0) — render_equirect_viewport() shows the source
// magnified (only ~75° of a 360° source is visible, stretched across
// most of the output), so neighboring output pixels often land in the
// same 2x2 source block; skipping the refetch measured ~10% faster.
// One instance per render thread — not safe to share across threads.
struct EquirectTexelCache {
    int x0 = -1;
    int y0 = -1;
    double p00[3], p10[3], p01[3], p11[3];
};

// Bilinear sample at fractional source coordinates (fx, fy). Horizontal
// wraps around (longitude is cyclic — panning past ±180° re-enters the
// opposite edge of the source image); vertical clamps (latitude does not
// wrap — the poles are real endpoints, not a seam).
void sample_equirect_bilinear(const guint8* pixels, int stride, int channels,
                               int src_w, int src_h, double fx, double fy,
                               guint8* out_rgb, EquirectTexelCache& cache) {
    fx -= 0.5;
    fy = std::clamp(fy - 0.5, 0.0, static_cast<double>(src_h - 1));
    int x0 = static_cast<int>(std::floor(fx));
    int y0 = static_cast<int>(std::floor(fy));
    double tx = fx - x0;
    double ty = fy - y0;
    int x1 = x0 + 1;
    int y1 = std::min(y0 + 1, src_h - 1);
    y0 = std::clamp(y0, 0, src_h - 1);
    // Longitude wraps, but rarely — only right at the seam — so a branch
    // beats an unconditional modulo here (~20% of this function's cost,
    // measured).
    if (x0 < 0) {
        x0 += src_w;
    } else if (x0 >= src_w) {
        x0 -= src_w;
    }
    if (x1 < 0) {
        x1 += src_w;
    } else if (x1 >= src_w) {
        x1 -= src_w;
    }

    if (x0 != cache.x0 || y0 != cache.y0) {
        const guint8* row0 = pixels + y0 * stride;
        const guint8* row1 = pixels + y1 * stride;
        const guint8* p00p = row0 + x0 * channels;
        const guint8* p10p = row0 + x1 * channels;
        const guint8* p01p = row1 + x0 * channels;
        const guint8* p11p = row1 + x1 * channels;
        for (int c = 0; c < 3; ++c) {
            cache.p00[c] = p00p[c];
            cache.p10[c] = p10p[c];
            cache.p01[c] = p01p[c];
            cache.p11[c] = p11p[c];
        }
        cache.x0 = x0;
        cache.y0 = y0;
    }
    for (int c = 0; c < 3; ++c) {
        double top = cache.p00[c] + (cache.p10[c] - cache.p00[c]) * tx;
        double bottom = cache.p01[c] + (cache.p11[c] - cache.p01[c]) * tx;
        out_rgb[c] = static_cast<guint8>(std::round(top + (bottom - top) * ty));
    }
}

}  // namespace

// Head: fixed position, never translation; orientation variable is
// viewport yaw/pitch/roll — roll is left at zero, not exposed yet (no
// control produces it).
//
// Performance: a naive per-pixel version of this (sqrt+atan2+asin,
// then a bilinear texture sample, per output pixel) measured ~420ms at
// 4K on this machine — nowhere near interactive. Two changes fix that:
//
//   1. equirect_pixel_uv() (the sqrt/atan2/asin part) is only evaluated
//      on a coarse kEquirectGridStep grid; every other output pixel's
//      (u, v) is bilinearly interpolated from that grid instead. u
//      wraps at the seam, so grid values are unwrapped relative to
//      their neighbor before interpolating.
//   2. The interpolation-and-sample loop (still the bulk of the cost)
//      is split across std::thread::hardware_concurrency() worker
//      threads by row range — embarrassingly parallel, since each
//      thread only reads the read-only grid and writes disjoint rows.
//
// Together: ~420ms -> ~28ms at 4K on a 16-core machine (~15x); ~66ms ->
// ~5ms at a typical windowed size. Threads spawned fresh per call
// rather than pooled — renders happen on discrete events, not every
// frame.
//
// Cylindrical's own reasoning: a cylindrical panorama's horizontal axis
// is linear in angle, same as equirectangular, but its vertical axis
// isn't — an ordinary single-lens capture uses one focal length for
// both axes at once, so `cyl_focal_px = src_w / 2π` is reused for the
// vertical mapping too. Physically sound for a real single-lens
// stitched panorama, used only as a fallback when there's no GPano
// crop data.
Glib::RefPtr<Gdk::Pixbuf> render_equirect_viewport(
    const Glib::RefPtr<Gdk::Pixbuf>& source, int out_width, int out_height,
    double fov_degrees, double yaw_degrees, double pitch_degrees,
    PanoramaProjection proj) {
    out_width = std::max(out_width, 1);
    out_height = std::max(out_height, 1);

    const guint8* src_pixels = source->get_pixels();
    int src_stride = source->get_rowstride();
    int src_channels = source->get_n_channels();
    int src_w = source->get_width();
    int src_h = source->get_height();
    proj = resolve_panorama_projection(proj, src_w, src_h);

    auto out = Gdk::Pixbuf::create(Gdk::Colorspace::RGB, false, 8, out_width,
                                    out_height);
    guint8* out_pixels = out->get_pixels();
    int out_stride = out->get_rowstride();

    double fov_rad = fov_degrees * std::numbers::pi / 180.0;
    double yaw_rad = yaw_degrees * std::numbers::pi / 180.0;
    double pitch_rad = pitch_degrees * std::numbers::pi / 180.0;
    double tan_half_fov = std::tan(fov_rad / 2.0);
    double aspect = static_cast<double>(out_width) / out_height;
    double cos_yaw = std::cos(yaw_rad), sin_yaw = std::sin(yaw_rad);
    double cos_pitch = std::cos(pitch_rad), sin_pitch = std::sin(pitch_rad);

    int grid_w = out_width / kEquirectGridStep + 2;
    int grid_h = out_height / kEquirectGridStep + 2;
    std::vector<double> grid_u(static_cast<std::size_t>(grid_w) * grid_h);
    std::vector<double> grid_v(static_cast<std::size_t>(grid_w) * grid_h);
    auto grid_x_pixel = [&](int gx) {
        return std::min(gx * kEquirectGridStep, out_width - 1);
    };
    auto grid_y_pixel = [&](int gy) {
        return std::min(gy * kEquirectGridStep, out_height - 1);
    };
    for (int gy = 0; gy < grid_h; ++gy) {
        for (int gx = 0; gx < grid_w; ++gx) {
            equirect_pixel_uv(grid_x_pixel(gx), grid_y_pixel(gy), out_width,
                               out_height, tan_half_fov, aspect, cos_yaw,
                               sin_yaw, cos_pitch, sin_pitch, proj,
                               grid_u[gy * grid_w + gx], grid_v[gy * grid_w + gx]);
        }
    }

    auto unwrap_toward = [](double u, double reference) {
        while (u - reference > 0.5) u -= 1.0;
        while (u - reference < -0.5) u += 1.0;
        return u;
    };

    auto render_rows = [&](int y_start, int y_end) {
        // Local to this call, i.e. one per worker thread below — never
        // shared, so no locking needed for the cache-reuse it enables.
        EquirectTexelCache texel_cache;
        for (int y = y_start; y < y_end; ++y) {
            int gy0 = y / kEquirectGridStep;
            int gy1 = std::min(gy0 + 1, grid_h - 1);
            double ty =
                static_cast<double>(y - gy0 * kEquirectGridStep) / kEquirectGridStep;
            for (int x = 0; x < out_width; ++x) {
                int gx0 = x / kEquirectGridStep;
                int gx1 = std::min(gx0 + 1, grid_w - 1);
                double tx = static_cast<double>(x - gx0 * kEquirectGridStep) /
                            kEquirectGridStep;

                double u00 = grid_u[gy0 * grid_w + gx0];
                double u10 = unwrap_toward(grid_u[gy0 * grid_w + gx1], u00);
                double u01 = unwrap_toward(grid_u[gy1 * grid_w + gx0], u00);
                double u11 = unwrap_toward(grid_u[gy1 * grid_w + gx1], u00);
                double u_top = u00 + (u10 - u00) * tx;
                double u_bottom = u01 + (u11 - u01) * tx;
                double u = u_top + (u_bottom - u_top) * ty;
                u -= std::floor(u);  // back into [0, 1) after unwrapping

                double v00 = grid_v[gy0 * grid_w + gx0];
                double v10 = grid_v[gy0 * grid_w + gx1];
                double v01 = grid_v[gy1 * grid_w + gx0];
                double v11 = grid_v[gy1 * grid_w + gx1];
                double v_top = v00 + (v10 - v00) * tx;
                double v_bottom = v01 + (v11 - v01) * tx;
                double v = v_top + (v_bottom - v_top) * ty;

                guint8* dst = out_pixels + y * out_stride + x * 3;
                // Genuinely outside the captured content — e.g. a GPano
                // crop covering less vertical FOV than the viewport
                // itself. Black, not a clamped sample. Horizontal is
                // left wrapping unconditionally for now (correct for
                // every full-360°-wide file in hand today); a genuinely
                // partial-horizontal crop would need the same treatment.
                if (v < 0.0 || v >= 1.0) {
                    dst[0] = dst[1] = dst[2] = 0;
                } else {
                    sample_equirect_bilinear(src_pixels, src_stride, src_channels,
                                              src_w, src_h, u * src_w, v * src_h,
                                              dst, texel_cache);
                }
            }
        }
    };

    unsigned n_threads = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    int rows_per_thread = (out_height + static_cast<int>(n_threads) - 1) /
                          static_cast<int>(n_threads);
    for (unsigned t = 0; t < n_threads; ++t) {
        int y_start = static_cast<int>(t) * rows_per_thread;
        int y_end = std::min(y_start + rows_per_thread, out_height);
        if (y_start >= y_end) {
            break;
        }
        workers.emplace_back(render_rows, y_start, y_end);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return out;
}
