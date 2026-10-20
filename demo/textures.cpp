// SPDX-License-Identifier: GPL-3.0-only
//
// textures.cpp — see textures.hpp for scope.

#include "textures.hpp"

#include <glib/gi18n.h>

#include <chrono>
#include <numbers>
#include <vector>

// Bridge into main.cpp's anonymous namespace — stays defined in
// main.cpp but needed here too (also used for icon/schema lookups
// unrelated to textures, so it doesn't belong in this module).
std::string bridge_resource_path(const std::string& filename);

namespace {

// Same angular-size reasoning as the depth/prod-distance grid math in
// main.cpp — duplicated here rather than shared via a header, same as
// every other GSettings-key-style constant this split introduced.
constexpr double kViewportFovDegrees = 75.0;
constexpr double kFovealRadiusDeg = 1.0;  // ~2° diameter
constexpr int kMinCursorDiameterPx = 8;

constexpr auto kProdCueDurationMsKey = "prod-cue-duration-ms";
constexpr auto kProdCueDiameterScaleKey = "prod-cue-diameter-scale";
constexpr auto kProdCuePeakOpacityKey = "prod-cue-peak-opacity";

// Same asymmetric periscope-graticule shape main.cpp's fire_head_prod()
// sweep draws its elevations from (kHeadProdSequenceElevationsDeg, a
// different, overlapping-but-not-identical list) — duplicated here for
// the texture itself.
constexpr double kHeadCrossTopElevationDeg = 15.0;
constexpr double kHeadCrossBottomElevationDeg = -30.0;
constexpr double kHeadCrossTickElevationsDeg[] = {10.0, 5.0,  -5.0, -10.0,
                                                    -15.0, -20.0, -25.0};

constexpr auto kSplashFile = "splash-image.jpeg";
constexpr auto kSplashBatFile = "blind-sight-bat.svg";
constexpr double kSplashBatCenterXFraction = 0.60;
constexpr double kSplashBatCenterYFraction = 0.30;
constexpr double kSplashBatWidthFraction = 0.07;
constexpr double kSplashBatRotationDegrees = 30.0;

constexpr auto kNoImageTestFile = "test-image.png";
constexpr double kNoImageLabelBoxCenterFraction = 64.0 / 548.0;

}  // namespace

int gaze_cursor_diameter_px(Gtk::Widget& widget) {
    double px_per_degree =
        std::max(widget.get_width(), 1) / kViewportFovDegrees;
    int diameter_px =
        static_cast<int>(std::round(2.0 * kFovealRadiusDeg * px_per_degree));
    return std::max(diameter_px, kMinCursorDiameterPx);
}

// The gaze-circle drawing itself — outline only, a ring rather than a
// filled disc, so it doesn't obscure what's underneath. Shared by
// Playback so the replayed gaze/prod position shows the exact same
// circle rather than a differently-styled marker.
Glib::RefPtr<Gdk::Texture> render_gaze_circle_texture(int size) {
    auto surface = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, size, size);
    auto cr = Cairo::Context::create(surface);
    cr->set_operator(Cairo::Context::Operator::SOURCE);
    cr->set_source_rgba(0, 0, 0, 0);
    cr->paint();
    cr->set_operator(Cairo::Context::Operator::OVER);

    // A thin dark stroke plus a slightly offset light stroke keeps it
    // visible on both light and dark image content.
    double r = size / 2.0 - 1.5;
    cr->arc(size / 2.0, size / 2.0, r, 0, 2.0 * std::numbers::pi);
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
    cr->set_line_width(2.0);
    cr->stroke_preserve();
    cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
    cr->set_line_width(1.0);
    cr->stroke();

    std::vector<unsigned char> png_bytes;
    surface->write_to_png_stream(
        [&png_bytes](const unsigned char* data, unsigned int length) {
            png_bytes.insert(png_bytes.end(), data, data + length);
            return CAIRO_STATUS_SUCCESS;
        });

    auto bytes = Glib::Bytes::create(png_bytes.data(), png_bytes.size());
    return Gdk::Texture::create_from_bytes(bytes);
}

Glib::RefPtr<Gdk::Cursor> make_gaze_cursor(Gtk::Widget& widget) {
    int size = gaze_cursor_diameter_px(widget);
    auto texture = render_gaze_circle_texture(size);
    return Gdk::Cursor::create(texture, size / 2, size / 2);
}

// Pixel offset from the viewport's own vertical center for a given
// elevation angle — inverts the same tan()-based mapping
// equirect_pixel_uv() uses for the center column at pitch 0, so a tick
// mark lands exactly where that elevation really renders.
int elevation_to_pixel_offset(double elevation_deg, int viewport_height_px,
                               double fov_degrees) {
    double fov_rad = fov_degrees * std::numbers::pi / 180.0;
    double tan_half_fov = std::tan(fov_rad / 2.0);
    double elevation_rad = elevation_deg * std::numbers::pi / 180.0;
    return static_cast<int>(std::lround(
        -(viewport_height_px / 2.0) * std::tan(elevation_rad) / tan_half_fov));
}

// Total pixel height needed to cover kHeadCrossTopElevationDeg down to
// kHeadCrossBottomElevationDeg at the given viewport size/FOV.
int head_direction_cross_height_px(int viewport_height_px, double fov_degrees) {
    int top = elevation_to_pixel_offset(kHeadCrossTopElevationDeg,
                                         viewport_height_px, fov_degrees);
    int bottom = elevation_to_pixel_offset(kHeadCrossBottomElevationDeg,
                                            viewport_height_px, fov_degrees);
    return std::max(bottom - top, 1);
}

// Head-direction cross — the rendered viewport is always re-yawed/
// re-pitched to face wherever the head currently points, so head
// direction is *always* dead center of the image, by construction. A
// stylized periscope graticule: a vertical stroke from +15° to -30°
// elevation, with short horizontal ticks at the listed elevations —
// asymmetric around 0° deliberately. Same two-tone stroke technique as
// render_gaze_circle_texture() so the two read as one consistent family.
Glib::RefPtr<Gdk::Texture> render_head_direction_cross_texture(
    int width, int viewport_height_px, double fov_degrees) {
    int top_offset = elevation_to_pixel_offset(kHeadCrossTopElevationDeg,
                                                viewport_height_px, fov_degrees);
    int height = head_direction_cross_height_px(viewport_height_px, fov_degrees);
    auto surface = Cairo::ImageSurface::create(Cairo::Surface::Format::ARGB32,
                                                width, height);
    auto cr = Cairo::Context::create(surface);
    cr->set_operator(Cairo::Context::Operator::SOURCE);
    cr->set_source_rgba(0, 0, 0, 0);
    cr->paint();
    cr->set_operator(Cairo::Context::Operator::OVER);

    double mid_x = width / 2.0;
    // Row within *this texture* for a given elevation — elevation_to_
    // pixel_offset() is relative to the viewport's own center, so
    // subtracting top_offset re-anchors it to this texture's own top
    // edge (row 0) instead.
    auto row_for = [&](double elevation_deg) {
        return static_cast<double>(
            elevation_to_pixel_offset(elevation_deg, viewport_height_px,
                                       fov_degrees) -
            top_offset);
    };

    // The vertical stroke, full requested elevation range.
    cr->move_to(mid_x, 0.0);
    cr->line_to(mid_x, static_cast<double>(height));
    // The main horizontal crossbar, at 0° elevation, full width.
    double zero_row = row_for(0.0);
    cr->move_to(0.0, zero_row);
    cr->line_to(static_cast<double>(width), zero_row);
    // Shorter ticks at the listed elevations — half the main crossbar's
    // width.
    double tick_half_width = width / 4.0;
    for (double deg : kHeadCrossTickElevationsDeg) {
        double row = row_for(deg);
        cr->move_to(mid_x - tick_half_width, row);
        cr->line_to(mid_x + tick_half_width, row);
    }
    cr->set_source_rgba(0.0, 0.0, 0.0, 0.9);
    cr->set_line_width(2.0);
    cr->stroke_preserve();
    cr->set_source_rgba(1.0, 1.0, 1.0, 0.9);
    cr->set_line_width(1.0);
    cr->stroke();

    std::vector<unsigned char> png_bytes;
    surface->write_to_png_stream(
        [&png_bytes](const unsigned char* data, unsigned int length) {
            png_bytes.insert(png_bytes.end(), data, data + length);
            return CAIRO_STATUS_SUCCESS;
        });

    auto bytes = Glib::Bytes::create(png_bytes.data(), png_bytes.size());
    return Gdk::Texture::create_from_bytes(bytes);
}

namespace {

// The prod-event visual cue — a soft, radially-faded white glow rather
// than a hard-edged stroke. `peak_opacity` only sets the *center's*
// starting alpha — the gradient always reaches fully transparent at the
// edge, which blends the glow into the underlying image rather than
// leaving a visible disc edge.
Glib::RefPtr<Gdk::Texture> render_prod_cue_texture(int size, double peak_opacity) {
    auto surface = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, size, size);
    auto cr = Cairo::Context::create(surface);
    cr->set_operator(Cairo::Context::Operator::SOURCE);
    cr->set_source_rgba(0, 0, 0, 0);
    cr->paint();
    cr->set_operator(Cairo::Context::Operator::OVER);

    double r = size / 2.0;
    auto gradient = Cairo::RadialGradient::create(r, r, 0.0, r, r, r);
    gradient->add_color_stop_rgba(0.0, 1.0, 1.0, 1.0, peak_opacity);
    gradient->add_color_stop_rgba(1.0, 1.0, 1.0, 1.0, 0.0);
    cr->set_source(gradient);
    cr->arc(r, r, r, 0, 2.0 * std::numbers::pi);
    cr->fill();

    std::vector<unsigned char> png_bytes;
    surface->write_to_png_stream(
        [&png_bytes](const unsigned char* data, unsigned int length) {
            png_bytes.insert(png_bytes.end(), data, data + length);
            return CAIRO_STATUS_SUCCESS;
        });

    auto bytes = Glib::Bytes::create(png_bytes.data(), png_bytes.size());
    return Gdk::Texture::create_from_bytes(bytes);
}

}  // namespace

// Shows the glow centered at a normalized ([0,65535], same fixed-point
// convention as GazeEvent/ProdEvent) position over `reference` (the
// widget whose pixel size the position/sizing is relative to — the
// displayed image, not the cue widget itself), then fades it out over
// prod-cue-duration-ms via a repeating ~60Hz timeout — settings read
// fresh on every call, so a Preferences change applies to the next
// prod, live.
void show_prod_cue(Gtk::Picture* cue, Gtk::Widget* reference,
                   const Glib::RefPtr<Gio::Settings>& settings, std::uint16_t x,
                   std::uint16_t y) {
    int duration_ms = settings->get_int(kProdCueDurationMsKey);
    double diameter_scale = settings->get_double(kProdCueDiameterScaleKey);
    double peak_opacity = settings->get_double(kProdCuePeakOpacityKey);
    int size = std::max(
        static_cast<int>(std::lround(gaze_cursor_diameter_px(*reference) *
                                      diameter_scale)),
        1);

    cue->set_size_request(size, size);
    cue->set_paintable(render_prod_cue_texture(size, peak_opacity));
    double fx = x / 65535.0;
    double fy = y / 65535.0;
    cue->set_margin_start(static_cast<int>(fx * reference->get_width()) - size / 2);
    cue->set_margin_top(static_cast<int>(fy * reference->get_height()) - size / 2);
    cue->set_opacity(1.0);
    cue->set_visible(true);

    // A rapid second prod restarts the glow from full opacity rather than
    // stacking with it — without this, the still-running timeout from
    // the *previous* prod would keep fighting this one over the same
    // widget's opacity every ~16ms until the older one's duration ran
    // out. Plain GObject data lets that older timeout recognize it's
    // been superseded and quietly stop touching the widget.
    auto* obj = G_OBJECT(cue->gobj());
    auto generation =
        GPOINTER_TO_UINT(g_object_get_data(obj, "prod-cue-generation")) + 1;
    g_object_set_data(obj, "prod-cue-generation", GUINT_TO_POINTER(generation));

    if (duration_ms <= 0) {
        cue->set_visible(false);
        return;
    }
    auto started_at = std::chrono::steady_clock::now();
    Glib::signal_timeout().connect(
        [cue, obj, generation, started_at, duration_ms]() {
            if (GPOINTER_TO_UINT(g_object_get_data(obj, "prod-cue-generation")) !=
                generation) {
                return false;  // superseded by a newer prod
            }
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_at)
                                   .count();
            if (elapsed_ms >= duration_ms) {
                cue->set_visible(false);
                return false;  // one-shot chain ends
            }
            cue->set_opacity(1.0 - static_cast<double>(elapsed_ms) / duration_ms);
            return true;
        },
        16);
}

// splash-image.jpeg with the bat logo composited on top, at the
// position/size/rotation in kSplashBatCenterXFraction and friends.
// Cached in a function-local static — the splash never changes at
// runtime, so there's no reason to redo this compose more than once.
Glib::RefPtr<Gdk::Texture> render_splash_texture() {
    static Glib::RefPtr<Gdk::Texture> cached;
    if (cached) {
        return cached;
    }

    auto base = Gdk::Pixbuf::create_from_file(bridge_resource_path(kSplashFile));
    int w = base->get_width();
    int h = base->get_height();

    auto surface = Cairo::ImageSurface::create(
        Cairo::Surface::Format::ARGB32, w, h);
    auto cr = Cairo::Context::create(surface);
    Gdk::Cairo::set_source_pixbuf(cr, base, 0, 0);
    cr->paint();

    // -1 height preserves the SVG's own aspect ratio at this target
    // width, same as the equirect loader elsewhere in this file.
    int bat_width = static_cast<int>(std::lround(w * kSplashBatWidthFraction));
    auto bat = Gdk::Pixbuf::create_from_file(
        bridge_resource_path(kSplashBatFile), bat_width, -1, true);

    double cx = w * kSplashBatCenterXFraction;
    double cy = h * kSplashBatCenterYFraction;
    cr->save();
    cr->translate(cx, cy);
    // Cairo's rotate() turns the axes from +x toward +y; with this
    // surface's y-axis pointing down (image coordinates), a positive
    // angle is already a clockwise turn on screen — no sign flip needed.
    cr->rotate(kSplashBatRotationDegrees * std::numbers::pi / 180.0);
    cr->translate(-bat->get_width() / 2.0, -bat->get_height() / 2.0);
    Gdk::Cairo::set_source_pixbuf(cr, bat, 0, 0);
    cr->paint();
    cr->restore();

    std::vector<unsigned char> png_bytes;
    surface->write_to_png_stream(
        [&png_bytes](const unsigned char* data, unsigned int length) {
            png_bytes.insert(png_bytes.end(), data, data + length);
            return CAIRO_STATUS_SUCCESS;
        });
    auto bytes = Glib::Bytes::create(png_bytes.data(), png_bytes.size());
    cached = Gdk::Texture::create_from_bytes(bytes);
    return cached;
}

// test-image.png with a localized "NO IMAGE" baked directly into the
// pixels, at its native resolution — the text becomes part of the one
// image no_image_picture displays, so it scales/centers exactly as
// precisely as the rest of the card, with no per-frame position math.
// Built once, since the locale can't change mid-run.
//
// font_context_widget is used only to resolve the desktop's default
// font via its Pango context — a bare Cairo::Context has no such
// context to inherit from and falls back to a generic default.
Glib::RefPtr<Gdk::Texture> render_no_image_placeholder_texture(
    Gtk::Widget& font_context_widget) {
    auto surface =
        Cairo::ImageSurface::create_from_png(bridge_resource_path(kNoImageTestFile));
    auto cr = Cairo::Context::create(surface);

    // Source string stays natural case — all-caps is a presentation
    // choice, not something every language wants (it reads as shouting
    // in Norwegian). Translators write whatever reads naturally;
    // .uppercase() applies the visual effect at render time instead.
    Glib::ustring text = Glib::ustring(_("No image")).uppercase();
    auto layout = font_context_widget.create_pango_layout(text);
    Pango::FontDescription font =
        font_context_widget.get_pango_context()->get_font_description();
    // Family/weight/style all stay at the desktop default — only the
    // size changes, to read clearly inside the card's own 42px-tall top
    // black field at native resolution.
    font.set_absolute_size(28.0 * Pango::SCALE);
    layout->set_font_description(font);

    // Ink extents (the visible glyph pixels), not the logical/line-
    // height box — centering on the logical box measured dead center
    // but still read as sitting low, since a line box reserves descent
    // space this all-caps text never uses.
    Pango::Rectangle ink, logical;
    layout->get_extents(ink, logical);
    double text_w = logical.get_width() / static_cast<double>(Pango::SCALE);
    double ink_h = ink.get_height() / static_cast<double>(Pango::SCALE);
    double ink_top_offset = ink.get_y() / static_cast<double>(Pango::SCALE);
    double x = (surface->get_width() - text_w) / 2.0;
    double y = kNoImageLabelBoxCenterFraction * surface->get_height() -
               ink_top_offset - ink_h / 2.0;

    cr->set_source_rgb(1.0, 1.0, 1.0);
    cr->move_to(x, y);
    layout->show_in_cairo_context(cr);

    std::vector<unsigned char> png_bytes;
    surface->write_to_png_stream(
        [&png_bytes](const unsigned char* data, unsigned int length) {
            png_bytes.insert(png_bytes.end(), data, data + length);
            return CAIRO_STATUS_SUCCESS;
        });

    auto bytes = Glib::Bytes::create(png_bytes.data(), png_bytes.size());
    return Gdk::Texture::create_from_bytes(bytes);
}
