// SPDX-License-Identifier: GPL-3.0-only
//
// main.cpp — blind-sight-lib demo: minimal app window (splash image + label).
//
// First milestone (see notes.md, "Demo architecture"): a plain window that
// opens with a splash image and a centered explanatory label, and closes
// cleanly via the window's own close button. No header bar, no file loading,
// no library link yet.
//
// GTK4 is used through gtkmm; libadwaita has no official C++ binding, so its
// application/window objects are created through the C API and wrapped into
// gtkmm objects with Glib::wrap() where that is safe. Content is set through
// adw_application_window_set_content() rather than Gtk::Window::set_child(),
// since AdwApplicationWindow does not support the generic GtkWindow child API
// for its content. Keep this interop confined to this file's app/window setup
// so a future switch to libadwaitamm (if it matures) stays local — see
// notes.md, "Demo stack".

#include <adwaita.h>
#include <gexiv2/gexiv2.h>
#include <gst/gst.h>
#include <gtkmm.h>
#include <glib/gi18n.h>
#include <gudev/gudev.h>
#include <libsecret/secret.h>
// OpenEXR — reads the depth file's "Z" channel for the "distance at
// pointer" readout. Same half-float format demo/depth/generate_depth.py
// writes; see load_depth_exr() below.
#include <ImfInputFile.h>
#include <ImfHeader.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>

#include <pipewire/pipewire.h>

#include "images.hpp"
#include "noise_pulse.hpp"
#include "protocol.hpp"
#include "recording.hpp"
#include "sessions.hpp"
#include "tracks.hpp"
#include "monitoring.hpp"
#include "audio.hpp"
#include "depth.hpp"
#include "panorama.hpp"
#include "textures.hpp"
#include "project_dialogs.hpp"
#include "session_dialogs.hpp"
#include "image_dialogs.hpp"
#include "track_dialogs.hpp"
#include "monitoring_dialogs.hpp"
#include "audio_routing.hpp"
#include "transcript_types.hpp"
#include "transcription.hpp"
#include "preferences.hpp"
#include "trace_delay.hpp"
#include "bt_output_check.hpp"
#include "external_change_guard.hpp"

// Raw evdev reading for the Surface Dial: its rotation arrives as
// REL_DIAL, a relative axis GTK/libinput never turn into an ordinary
// scroll event (confirmed with evtest), so there's no GTK-level signal
// to hook — this is the standard low-level way to read such a device.
// See find_evdev_by_name()/try_connect_dial() in on_activate().
#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <numbers>
#include <limits>
#include <optional>
#include <deque>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#include <string>

namespace {

using blind_sight::AudioDeviceInfo;
using blind_sight::enumerate_audio_devices;
using blind_sight::GazeEvent;
using blind_sight::ImageEntry;
using blind_sight::ImageEvent;
using blind_sight::ImagesIndex;
using blind_sight::LeaveEvent;
using blind_sight::LevelEvent;
using blind_sight::MicrophoneEvent;
using blind_sight::kAmbientMicSourceId;
using blind_sight::kSpeakerSinkId;
using blind_sight::MonitoringState;
using blind_sight::NoisePulseEngine;
using blind_sight::NoisePulseParams;
using blind_sight::ProdEvent;
using blind_sight::Protocol;
using blind_sight::resolved_description;
using blind_sight::resolved_title;
using blind_sight::ProtocolEntry;
using blind_sight::RecordEvent;
using blind_sight::RecordMeta;
using blind_sight::ScenarioPages;
using blind_sight::SessionEntry;
using blind_sight::SourcesEvent;
using blind_sight::SessionsIndex;
using blind_sight::TestStep;
using blind_sight::TestStepBlock;
using blind_sight::TestStepEvent;
using blind_sight::TrackEntry;
using blind_sight::TracksIndex;
using blind_sight::ViewEvent;
using blind_sight::WaitEvent;
using blind_sight::generate_image_id;
using blind_sight::generate_image_slug;
using blind_sight::generate_session_id;
using blind_sight::generate_track_id;
using blind_sight::image_file_path;
using blind_sight::participant_names;
using blind_sight::parse_protocol_file;
using blind_sight::parse_scenario_pages;
using blind_sight::read_images_index;
using blind_sight::read_record_events;
using blind_sight::read_record_meta;
using blind_sight::read_sessions_index;
using blind_sight::read_tracks_index;
using blind_sight::reconcile_monitoring_state;
using blind_sight::session_file_path;
using blind_sight::slugify;
using blind_sight::write_event;
using blind_sight::write_images_index;
using blind_sight::write_protocol_file;
using blind_sight::write_record_meta;
using blind_sight::write_sessions_index;
using blind_sight::write_tracks_index;

// Shared between the splash image's motion controller and Record's
// Forward/Stop buttons, wired up at very different times. A null
// `file` means "not recording."
//
// Resuming into the same file after Stop needs `elapsed_ms()` to be a
// running total across every active segment, not reset at each
// Forward — hence `accumulated_ms` (frozen at Stop) plus a fresh
// `segment_start` per Forward.
struct RecordingSession {
    std::shared_ptr<std::ofstream> file;
    // The .bs file Forward appends to — set once a session is chosen or
    // created in the Record bar. Empty means no session yet; Forward/Stop stay
    // disabled until one exists.
    std::string session_path;
    std::chrono::steady_clock::time_point segment_start;
    std::chrono::steady_clock::time_point stopped_at;
    std::int32_t accumulated_ms = 0;
    // False only before the very first Forward press ever, so Forward can
    // tell "starting fresh" (no WaitEvent — there's no gap yet) apart from
    // "resuming" (write one).
    bool has_run_before = false;
    // "Track" number for the clock display — starts at 1, incremented
    // every Forward resume, matching how Playback counts tracks from
    // the same WaitEvents. Recovered from the file's own WaitEvent
    // count when resuming from outside this run, not reset to 1.
    int current_track = 1;
    // The current track's own stable id (tracks.hpp) — minted once, at
    // the same moment current_track is set, by whichever of the two
    // track-start sites just ran (the Forward handler, or
    // write_implicit_track_change()). Every file path for the current
    // track goes through this, not current_track's number — see
    // audio.hpp's "_for_id" path helpers. Never recovered on reload: a
    // session resumed after an app restart starts its next track fresh,
    // same as any other session.
    std::string current_track_id;

    // Live, in-memory "who hears whom" setup for this session — reset
    // and reconciled against RecordMeta every time a session is loaded
    // (apply_shared_session()), and again each time the monitoring
    // matrix popover is about to show. Deliberately not persisted
    // itself (see monitoring.hpp's own comment); only a small snapshot
    // of it lands in each minted TrackEntry (mint_new_track()).
    MonitoringState monitoring;

    // True between a Record-button pause and its matching resume —
    // same semantics as the Playback button. The file stays open and
    // the track stays the same; only gaze/view/prod capture suspends.
    // accumulated_ms already reflects time up to the pause, so
    // segment_start stays stale until resume sets it fresh — see
    // add_record_controls()'s pause/resume handler.
    bool paused = false;

    // Microphone — one WAV file per track, not one continuous file
    // paused/resumed across Stop/Forward: a throwaway harness confirmed
    // that pausing/resuming a single pipewiresrc-based GStreamer
    // pipeline deadlocks on shutdown, while a fresh pipeline per track
    // doesn't. `audio_pipeline` is non-null exactly while the current
    // track's capture is running.
    GstElement* audio_pipeline = nullptr;
    // Live toggle state, not just the GSettings default — the
    // microphone can always be switched on even when it defaults off.
    bool microphone_on = false;
    // Counts how many separate audio pipelines the current track has
    // had — toggling the mic mid-track can't resume the same WAV file
    // (same deadlock risk above), so each re-enable gets its own file,
    // numbered by this. Reset to 0 only by Forward (a fresh track).
    int audio_segment = 0;

    // Current head orientation — a type:image is always followed by a
    // type:view, so a reader always has both facts at a segment
    // boundary. Live via arrow keys (on_activate()); re-rendered from
    // equirect_source below.
    std::int32_t view_yaw = 0;
    std::int32_t view_pitch = 0;

    // The currently-loaded equirectangular or cylindrical-panorama
    // source (see panorama_projection below for which projection
    // applies). Cached so arrow-key re-renders don't reload/redecode
    // the file on every keypress. Null/false when the current image is
    // flat.
    Glib::RefPtr<Gdk::Pixbuf> equirect_source;
    bool viewing_equirect = false;
    // How to interpret equirect_source — viewing_equirect is true for
    // either projection; this picks which one render_equirect_viewport()
    // actually uses. See PanoramaProjection's own comment.
    PanoramaProjection panorama_projection;

    // The pixel size render_current_view() last rendered at, so a tick
    // callback can tell a real resize apart from a no-op frame. The
    // gaze cursor's simulated-FOV math (gaze_cursor_diameter_px())
    // depends on the viewport's *current* displayed pixel size.
    int last_render_width = 0;
    int last_render_height = 0;

    // The path load_image() was last called with — lets the recents
    // dropdown's "Image Details" entry re-read metadata on demand,
    // without load_image() showing that dialog every time.
    std::string current_image_path;

    // Position in the Interactive-mode image sequence (images.json) the
    // Record bar's nav buttons are at. -1 means "no image" (test-image
    // placeholder). Initialized from current_image_path's own position
    // when Record is entered; walked by sync_record_display() in
    // add_record_controls().
    int nav_index = -1;

    // Precomputed depth for the "distance at pointer" readout — cached
    // in memory the same way equirect_source is, reloaded on each
    // load_image() call. Empty when the current image has no depth
    // file; the readout then shows nothing, checked by depth_width == 0.
    std::vector<float> depth_data;
    int depth_width = 0;
    int depth_height = 0;

    // Milliseconds, not fractional seconds — matches the fixed-point
    // integer fields on GazeEvent/ProdEvent/etc.
    std::int32_t elapsed_ms() const {
        auto since_segment_start = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - segment_start)
                                        .count();
        return accumulated_ms + static_cast<std::int32_t>(since_segment_start);
    }
};

// Same cross-cutting-state shape as RecordingSession: the splash
// overlay's marker widget (wired at startup) needs to be moved by
// Playback's Forward/Stop buttons, wired much later.
//
// Forward always restarts playback from the beginning (`next_index = 0`,
// `base_ms = 0`) — a real stop/reset, not a pause, since there's no
// separate Pause control. A type:wait halts playback until the next
// Forward press — a stand-in for the "jump to the end of the gap"
// fast-forward behavior in notes.md, not built yet.

struct PlaybackSession {
    std::vector<blind_sight::RecordEvent> events;
    // The .bs file `events` was read from — lets the Playback bar's
    // "Session Details" entry re-read the meta line on demand, same
    // on-demand pattern as the image side's current_image_path/Image
    // Details.
    std::string session_path;
    // Track boundaries — t_ms 0 plus every WaitEvent's own t_ms — computed
    // once whenever `events` is (re)loaded.
    // track_starts[i] is where track i+1 (1-based) begins; the count of
    // entries <= a given ms is that position's 1-based track number.
    std::vector<std::int32_t> track_starts;

    // Speaker — Record's microphone_on/audio_pipeline mirrored for
    // output. No deadlock risk here (verified: playbin/pipewiresink
    // tolerates repeated pause/seek/teardown cycles), unlike the
    // capture side. audio_segments gives exact mic-on/off timing from
    // the recording's own MicrophoneEvents; current_audio_segment lets
    // sync_playback_audio() skip touching GStreamer every frame when
    // nothing's actually changed.
    GstElement* audio_pipeline = nullptr;
    bool speaker_on = false;
    std::vector<AudioSegment> audio_segments;
    int current_audio_segment = -1;
    // The rate audio_pipeline was last started at — lets
    // sync_playback_audio() detect an FF/FB rate change (which
    // rate-seeks the audio rather than silencing it) even when the
    // segment itself hasn't changed.
    double current_audio_rate = 1.0;

    // Subtitles — computed once per (re)load, same shape as
    // audio_segments. subtitle_label is set once in build_splash(),
    // styled like the "distance at pointer" readout.
    std::vector<TranscriptCue> transcript_cues;
    Gtk::Label* subtitle_label = nullptr;

    std::size_t next_index = 0;
    std::chrono::steady_clock::time_point started;
    std::int32_t base_ms = 0;
    // A frame-clock tick, not a fixed timeout: recorded gaze samples
    // land far more often than a 100ms poll could show, so a
    // fixed-interval poll collapses many into one, reading as stutter.
    // The frame clock dispatches once per repaint instead.
    guint tick_id = 0;
    bool tick_active = false;
    Gtk::Picture* marker = nullptr;  // shows the same circle as
                                      // make_gaze_cursor() — see its comment
    // The prod-event glow (show_prod_cue()) — a separate overlay from
    // `marker`: the gaze ring tracks the current position, this one
    // appears briefly at the most recent ProdEvent. Built once in
    // build_splash().
    Gtk::Picture* prod_cue = nullptr;
    // Head-direction cross (render_head_direction_cross_texture()) —
    // static, always dead center, unlike marker/prod_cue which track a
    // recorded position. Rebuilt fresh on every render_playback_view()
    // call, hidden whenever !viewing_equirect.
    Gtk::Picture* head_direction_cross = nullptr;
    // Tick callbacks only fire on a *mapped* (visible) widget — marker
    // starts hidden, so the tick is registered against the splash
    // overlay instead, which stays visible for as long as the window
    // is.
    Gtk::Widget* tick_host = nullptr;

    // True for as long as the Playback bar is open — the gaze-circle
    // cursor is a Record-time sighted-tester aid, with no meaning
    // during replay. See its use in build_splash()'s motion
    // controller.
    bool active = false;
    // The splash Gtk::Picture itself — set once in build_splash() so
    // add_playback_controls() can update its cursor immediately when
    // `active` flips, not just on the next pointer-enter.
    Gtk::Widget* image = nullptr;
    // Same "no image" placeholder Record's nav buttons use — a track
    // with no ImageEvent at its start shows this rather than whatever
    // the previous track left on screen. See load_playback_image()'s
    // own empty-path branch.
    Gtk::Picture* no_image_picture = nullptr;

    // Signed playback rate: sign is direction, magnitude the
    // multiplier (1×, or 3×/9×/27× via FF/FB) — see notes.md "The
    // clock, and Playback's fast-forward/backward." Reset to 1.0 on
    // Forward or a fresh file load.
    double speed = 1.0;

    // Viewport state driven by ImageEvent/ViewEvent during dispatch —
    // mirrors RecordingSession's own fields (see there), kept separate
    // since Record/Playback already have entirely separate
    // control-building functions. render_playback_view()/
    // load_playback_image() are free functions, needed from both
    // add_playback_controls()'s dispatch loop and on_activate()'s
    // resize tick callback.
    Glib::RefPtr<Gdk::Pixbuf> equirect_source;
    bool viewing_equirect = false;
    // Mirrors RecordingSession's own panorama_projection (see there).
    PanoramaProjection panorama_projection;
    std::int32_t view_yaw = 0;
    std::int32_t view_pitch = 0;
    int last_render_width = 0;
    int last_render_height = 0;
    std::string current_image_path;

    std::int32_t elapsed_ms() const {
        auto since_started = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started)
                                  .count();
        return base_ms +
               static_cast<std::int32_t>(static_cast<double>(since_started) * speed);
    }
};

// Scales a widget-pixel coordinate to the [0, 65535] fixed-point range
// GazeEvent/ProdEvent store (see recording.hpp). Clamped: pointer
// coordinates can land exactly on (or, with rounding, a hair past) a
// widget's edge, which would otherwise wrap around as an unsigned integer.
std::uint16_t quantize_normalized(double position_px, int extent_px) {
    double fraction = extent_px > 0 ? position_px / extent_px : 0.0;
    fraction = std::clamp(fraction, 0.0, 1.0);
    return static_cast<std::uint16_t>(std::lround(fraction * 65535.0));
}

constexpr auto kAppId = "io.github.jeblad.blind-sight-demo";
constexpr auto kGettextDomain = "blind-sight-demo";
constexpr int kDefaultWidth = 1600;
constexpr int kDefaultHeight = 1000;
constexpr auto kSplashFile = "splash-image.jpeg";
constexpr auto kSplashBatFile = "blind-sight-bat.svg";
// Where the bat logo sits on the splash image, and how big/rotated it
// is — all fractions of the splash image's own native size, so it's
// baked into the same texture
// (render_splash_texture()) rather than a separately positioned overlay
// widget, same reasoning as test-image.png's "NO IMAGE" text: it becomes
// part of the one image that's already displayed, so it scales/crops
// exactly however the rest of the photo does under COVER fit, with no
// separate per-frame position tracking at all.
constexpr double kSplashBatCenterXFraction = 0.60;
constexpr double kSplashBatCenterYFraction = 0.30;
constexpr double kSplashBatWidthFraction = 0.07;
constexpr double kSplashBatRotationDegrees = 30.0;
// The centered test card shown over a plain #7b7b7b background when
// Record is entered without an image chosen (see notes.md).
constexpr auto kNoImageTestFile = "test-image.png";
// The margin around the test card *at that image's own native, unscaled
// resolution* — test-image.png is 716×548, and 40/716=5.59%,
// 40/548=7.30%, so it's the same 40px on all four sides at native
// scale, just two different percentages because the image isn't
// square. As the card scales to fit the window, the margin has to
// scale right along with it by the same factor — see the tick callback
// in on_activate() that keeps no_image_picture's margins in sync with
// splash_picture's current size, below.
constexpr double kNoImageMarginAtNativeScalePx = 40.0;
// The "NO IMAGE" label's target vertical center — test-image.png's own
// top black field, as a fraction of the image's native 548px height
// (measured directly: top edge y=43, bottom edge y=84 inclusive, so
// center = (43+84+1)/2 = 64). Re-measure this if test-image.png is ever
// replaced with a card whose top field sits somewhere else.
constexpr double kNoImageLabelBoxCenterFraction = 64.0 / 548.0;

std::string resource_path(const std::string& filename) {
    return std::string(BLIND_SIGHT_RESOURCE_DIR) + "/" + filename;
}

// images/ holds the sample photosphere/panorama images a tester opens
// live, kept separate from resource/'s internal-use-only assets.
// BLIND_SIGHT_IMAGES_DIR itself is untouched, waiting for the
// example-project rework that will repackage these into a proper
// example project.
//
// Resolves a /dev/input/eventN path by the device's own reported name, via
// sysfs — evdev numbers aren't stable across reconnects, especially for a
// Bluetooth device (confirmed: a Surface Dial's own two interfaces were
// event15/event16 on one connection, and hardcoding either would silently
// break the next time it reconnects with different numbers). Empty
// string if nothing matches — this is optional hardware, so no device
// found is a normal, silent case, not an error.
std::string find_evdev_by_name(const std::string& name) {
    std::error_code ec;
    // Diagnostic trail for a real, intermittent bug: udev fires a Dial
    // "add" event, but this scan finds no exact match, likely a race
    // with the sysfs `name` file not yet populated. Logged only on an
    // outright miss; safe unconditionally since this only runs while
    // not connected (see try_connect_dial()), never during
    // steady-state operation.
    std::vector<std::string> dial_prefixed_seen;
    int unreadable_count = 0;
    for (const auto& entry :
         std::filesystem::directory_iterator("/sys/class/input", ec)) {
        auto name_path = entry.path() / "device" / "name";
        std::ifstream in(name_path);
        std::string line;
        if (std::getline(in, line)) {
            if (line == name) {
                return "/dev/input/" + entry.path().filename().string();
            }
            if (line.rfind("Surface Dial", 0) == 0) {
                dial_prefixed_seen.push_back(entry.path().filename().string() +
                                              "=\"" + line + "\"");
            }
        } else {
            ++unreadable_count;
        }
    }
    if (!dial_prefixed_seen.empty()) {
        std::string joined;
        for (const auto& entry : dial_prefixed_seen) {
            if (!joined.empty()) {
                joined += ", ";
            }
            joined += entry;
        }
        g_message("blind-sight: find_evdev_by_name(\"%s\"): no exact match, "
                   "but saw: %s",
                   name.c_str(), joined.c_str());
    } else {
        g_message("blind-sight: find_evdev_by_name(\"%s\"): no match, no "
                   "Surface Dial-prefixed names seen (%d unreadable name "
                   "files)",
                   name.c_str(), unreadable_count);
    }
    return {};
}

// Equirectangular viewport synthesis — see panorama.hpp/.cpp for the
// detection/rendering math. The two constants below stay here since
// they're needed by non-panorama-specific code: kViewportFovDegrees by
// compute_prod_distance_m()'s own depth-grid math, kEquirectSourceMaxWidth
// by the image-load downscale step.
constexpr double kViewportFovDegrees = 75.0;  // 60-90° for a natural,
                                               // non-distorted look
// Downscaled on load — the source photospheres are 8704×4352 (~38MP);
// nothing our viewport render or its bilinear sampling needs more
// resolution than this.
constexpr int kEquirectSourceMaxWidth = 4096;

// Fallback render quality while a navigation key is held/repeating —
// full resolution lands once released. Needed for realistic target
// hardware (assumed decade-old machines), not this dev machine:
// measured via `taskset` limiting this machine to 1-2 cores, a single
// full-quality 4K render alone can pass 100ms. Divided on each linear
// dimension, cutting pixel count by kEquirectFastRenderDivisor²;
// Gtk::Picture's own ContentFit::COVER stretches the result to fill the
// widget.
constexpr int kEquirectFastRenderDivisor = 4;
constexpr int kEquirectFastRenderMinSize = 64;

// IPTC metadata read from an opened image, shown in the image-details
// bubble. Read via gexiv2's raw C API — GExiv2Metadata has no gtkmm
// wrap table, so passing it through Glib::wrap() would crash
// ("wrap_func_table != nullptr"). Any field not present is left empty,
// the common case, not an error.
struct ImageMetadata {
    std::string title;      // Xmp.dc.title, falls back to Iptc.Application2.ObjectName
    std::string caption;    // Xmp.dc.description, falls back to Iptc.Application2.Caption
    std::string creator;    // Xmp.dc.creator, falls back to Iptc.Application2.Byline
    // The copyright holder is whoever released the image under a
    // license. Falls back to legacy Iptc.Application2.Copyright; some
    // tools write a CC license link to Xmp.xmpRights.WebStatement
    // instead of plain text, shown as-is.
    std::string copyright;
};

// This app displays Norwegian Bokmål (po/nb.po), and image metadata is
// tagged in that language too. Xmp.dc.title can hold both a stale
// `lang="x-default"` entry and a corrected `lang="nb-NO"` one — editing
// in the tagging tool only touches nb-NO, so preferring x-default would
// make a fixed typo look permanently stuck.
constexpr auto kXmpPreferredLang = "nb-NO";

// Exiv2's plain string form of an XMP LangAlt value includes the
// language qualifier literally (e.g. `lang="nb-NO" Helleplassen`) and
// can hold several comma-separated entries — gexiv2 has no dedicated
// LangAlt accessor, so this parses all entries and picks
// kXmpPreferredLang, falling back to x-default, then to whichever came
// first.
std::string pick_xmp_lang_alt(const std::string& raw) {
    if (raw.rfind("lang=\"", 0) != 0) {
        return raw;
    }
    std::map<std::string, std::string> by_lang;
    std::string first_value;
    std::size_t pos = 0;
    while (pos < raw.size() && raw.compare(pos, 6, "lang=\"") == 0) {
        auto close_quote = raw.find('"', pos + 6);
        if (close_quote == std::string::npos) {
            break;
        }
        std::string lang = raw.substr(pos + 6, close_quote - (pos + 6));
        std::size_t value_start = close_quote + 1;
        while (value_start < raw.size() && raw[value_start] == ' ') {
            ++value_start;
        }
        auto next_lang = raw.find(", lang=\"", value_start);
        std::string value = (next_lang == std::string::npos)
                                 ? raw.substr(value_start)
                                 : raw.substr(value_start, next_lang - value_start);
        if (by_lang.empty()) {
            first_value = value;
        }
        by_lang.emplace(std::move(lang), value);
        if (next_lang == std::string::npos) {
            break;
        }
        pos = next_lang + 2;  // skip past ", " before the next `lang="`
    }
    if (auto it = by_lang.find(kXmpPreferredLang); it != by_lang.end()) {
        return it->second;
    }
    if (auto it = by_lang.find("x-default"); it != by_lang.end()) {
        return it->second;
    }
    return first_value.empty() ? raw : first_value;
}

ImageMetadata read_image_metadata(const std::string& path) {
    ImageMetadata result;
    auto* metadata = gexiv2_metadata_new();
    GError* error = nullptr;
    if (gexiv2_metadata_open_path(metadata, path.c_str(), &error)) {
        auto get_tag = [metadata](const char* tag) {
            GError* tag_error = nullptr;
            std::string value;
            if (gexiv2_metadata_try_has_tag(metadata, tag, &tag_error)) {
                gchar* raw = gexiv2_metadata_try_get_tag_string(metadata, tag,
                                                                 &tag_error);
                if (raw) {
                    value = raw;
                    g_free(raw);
                }
            }
            if (tag_error) {
                g_error_free(tag_error);
            }
            return value;
        };
        auto get_tag_preferring = [&](const char* xmp_tag,
                                       const char* iptc_tag, bool lang_alt) {
            std::string value = get_tag(xmp_tag);
            if (!value.empty()) {
                return lang_alt ? pick_xmp_lang_alt(value) : value;
            }
            return get_tag(iptc_tag);
        };
        result.title = get_tag_preferring("Xmp.dc.title",
                                           "Iptc.Application2.ObjectName", true);
        result.caption = get_tag_preferring(
            "Xmp.dc.description", "Iptc.Application2.Caption", true);
        result.creator = get_tag_preferring(
            "Xmp.dc.creator", "Iptc.Application2.Byline", false);
        result.copyright = get_tag_preferring(
            "Xmp.xmpRights.WebStatement", "Iptc.Application2.Copyright", false);
    }
    if (error) {
        g_error_free(error);
    }
    g_object_unref(metadata);
    return result;
}

// Forward declaration — its real body comes much further down, but
// show_record_meta_dialog()/the project/session/image Details Save
// handlers (all defined before that point) need to report a conflict via
// it.
void show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                        const Glib::ustring& body);

// Playback's counterpart to render_current_view() — ViewEvents
// dispatched during playback must actually be applied to the rendered
// viewport, not just dispatched-past. A free function, not a lambda,
// since it's needed from two scopes: add_playback_controls()'s dispatch
// tick and on_activate()'s resize tick callback.
//
// Head-direction cross — a stylized periscope graticule: a vertical
// stroke from +15° to -30° elevation, with short horizontal ticks at
// the listed elevations, asymmetric around 0° deliberately. Mapped
// through the real viewport projection (elevation_to_pixel_offset(),
// below), not a decorative scale, with no text labels.
constexpr double kHeadCrossTopElevationDeg = 15.0;
constexpr double kHeadCrossBottomElevationDeg = -30.0;
constexpr double kHeadCrossTickElevationsDeg[] = {10.0, 5.0,  -5.0, -10.0,
                                                    -15.0, -20.0, -25.0};

// A head-prod's own elevation sweep (fire_head_prod()) — same
// elevations as the graticule above minus its -30° bottom endpoint:
// starts at -25°, works upward to +15°, firing a prod at every 5° mark.
// File-scope: a local constexpr array needs explicit lambda capture to
// index.
constexpr double kHeadProdSequenceElevationsDeg[] = {-25.0, -20.0, -15.0, -10.0,
                                                        -5.0,   0.0,   5.0,  10.0,
                                                        15.0};


void render_playback_view(const std::shared_ptr<PlaybackSession>& playback,
                           bool fast = false) {
    if (!playback->viewing_equirect || !playback->equirect_source) {
        if (playback->head_direction_cross) {
            playback->head_direction_cross->set_visible(false);
        }
        return;
    }
    auto* picture = static_cast<Gtk::Picture*>(playback->image);
    int out_width = picture->get_width() > 0 ? picture->get_width() : 1024;
    int out_height = picture->get_height() > 0 ? picture->get_height() : 640;
    int render_width = out_width;
    int render_height = out_height;
    if (fast) {
        render_width = std::max(out_width / kEquirectFastRenderDivisor,
                                 kEquirectFastRenderMinSize);
        render_height = std::max(out_height / kEquirectFastRenderDivisor,
                                  kEquirectFastRenderMinSize);
    }
    auto rendered = render_equirect_viewport(
        playback->equirect_source, render_width, render_height,
        kViewportFovDegrees, playback->view_yaw / 100.0,
        playback->view_pitch / 100.0, playback->panorama_projection);
    picture->set_paintable(Gdk::Texture::create_for_pixbuf(rendered));
    playback->last_render_width = out_width;
    playback->last_render_height = out_height;

    if (playback->head_direction_cross) {
        int width = gaze_cursor_diameter_px(*picture);
        int viewport_height = std::max(picture->get_height(), 1);
        int height =
            head_direction_cross_height_px(viewport_height, kViewportFovDegrees);
        int top_offset = elevation_to_pixel_offset(
            kHeadCrossTopElevationDeg, viewport_height, kViewportFovDegrees);
        playback->head_direction_cross->set_size_request(width, height);
        playback->head_direction_cross->set_paintable(
            render_head_direction_cross_texture(width, viewport_height,
                                                 kViewportFovDegrees));
        playback->head_direction_cross->set_margin_start(
            (std::max(picture->get_width(), 1) - width) / 2);
        playback->head_direction_cross->set_margin_top(viewport_height / 2 +
                                                         top_offset);
        playback->head_direction_cross->set_visible(true);
    }
}

// A project is a directory holding one protocol.md plus its related
// recording sessions. The directory, not the protocol.md path, is
// remembered/opened/passed around; "protocol.md" is a fixed name the
// app creates implicitly.
constexpr auto kLastProjectKey = "last-project";
constexpr auto kProjectTitleCutoffCharsKey = "project-title-cutoff-chars";
constexpr auto kImageTitleCutoffCharsKey = "image-title-cutoff-chars";
constexpr auto kProjectProtocolFileName = "protocol.md";

std::string project_protocol_path(const std::string& project_dir) {
    return Glib::build_filename(project_dir, kProjectProtocolFileName);
}

// Truncates to at most `max_chars` *characters* (not bytes — Glib::ustring
// is UTF-8-aware throughout), appending a single ellipsis character when
// it does. Generic — used by every always-visible header-bar dropdown's
// own button-face label (project, session, …), each with its own cutoff
// gsettings key.
Glib::ustring truncate_dropdown_title(const Glib::ustring& title, int max_chars) {
    if (max_chars <= 0 || static_cast<int>(title.size()) <= max_chars) {
        return title;
    }
    return title.substr(0, static_cast<Glib::ustring::size_type>(max_chars)) +
           Glib::ustring("…");
}

// Converts a pixel budget into Gtk::Label::set_max_width_chars()'s own
// character-based unit, since GTK has no max-width-in-pixels property
// for an ellipsizing label. Measures a representative string on the
// label's own Pango context for a real average glyph width, rather
// than guessing a constant.
double label_avg_char_width_px(Gtk::Label* label) {
    auto layout = label->create_pango_layout("0123456789abcdefghij");
    int width_px = 0;
    int height_px = 0;
    layout->get_pixel_size(width_px, height_px);
    return width_px / 20.0;
}

// True once the app has made any attempt to change the displayed image.
// The bat-logo-plus-welcome-text splash is only the genuinely pristine,
// nothing-tried-yet state; once dismissed, every later "no image"
// display falls back to the plain placeholder instead. Record's
// close_self() checks this to decide which to restore on Stop.
// Declared early because load_playback_image() below needs it.
bool g_splash_dismissed = false;

// The splash caption (bat-welcome text) widget itself, set once from
// on_activate() right after build_splash() creates it. A raw global
// pointer, same reasoning as every other "needed past on_activate()'s own
// lexical scope" global in this file — needed here specifically because
// sync_record_display()/start_recording_out() (add_record_controls(), a
// separate top-level function) and load_playback_image() (above) can't
// otherwise reach a local on_activate() captures without threading it
// through several more parameter lists.
Gtk::Widget* g_splash_caption = nullptr;

// Marks the image as changed/attempted (see g_splash_dismissed's own
// comment) and hides the caption text with it — the two can otherwise
// go out of sync, since the caption is a separate overlay widget from
// splash_picture, and Next/Previous's own "image changed" handling
// never touched it.
void dismiss_splash() {
    g_splash_dismissed = true;
    if (g_splash_caption) {
        g_splash_caption->set_visible(false);
    }
}

// Playback's counterpart to load_image()'s image-loading half — same
// equirect detection/caching, no recents-list/GSettings/ImageEvent side
// effects. A no-op if `path` already matches, except for the empty/"no
// image" case: starting playback must always force the shared picture
// back to "No image" explicitly, or a session with zero ImageEvents
// would silently keep showing whatever unrelated image Interactive had
// up before Play was pressed, for the session's entire length.
void load_playback_image(const std::shared_ptr<PlaybackSession>& playback,
                          const std::string& path) {
    if (!path.empty() && playback->current_image_path == path) {
        return;
    }
    playback->current_image_path = path;
    dismiss_splash();

    // Empty means the track this position falls in never got an
    // ImageEvent — the test-image placeholder, same as Record's own
    // "no image" nav position. Callers pass "" explicitly; a real path
    // is never empty.
    if (path.empty()) {
        auto* image_picture = static_cast<Gtk::Picture*>(playback->image);
        playback->viewing_equirect = false;
        playback->panorama_projection = PanoramaProjection{};
        playback->equirect_source.reset();
        image_picture->set_paintable({});
        image_picture->add_css_class("blind-sight-no-image-background");
        playback->no_image_picture->set_visible(true);
        return;
    }
    playback->no_image_picture->set_visible(false);
    static_cast<Gtk::Picture*>(playback->image)
        ->remove_css_class("blind-sight-no-image-background");

    int width = 0, height = 0;
    gdk_pixbuf_get_file_info(path.c_str(), &width, &height);
    if (auto proj = detect_panorama_projection(path, width, height)) {
        playback->equirect_source =
            Gdk::Pixbuf::create_from_file(path, kEquirectSourceMaxWidth, -1, true);
        playback->viewing_equirect = true;
        playback->panorama_projection = *proj;
        render_playback_view(playback);
    } else {
        playback->viewing_equirect = false;
        playback->panorama_projection = PanoramaProjection{};
        playback->equirect_source.reset();
        static_cast<Gtk::Picture*>(playback->image)->set_filename(path);
    }
}

// The id is the path's own parent directory name
// ("<project_dir>/images/<id>/<filename>") — every lookup from a path
// back to its images.json entry starts here.
std::string image_id_from_path(const std::string& path) {
    return std::filesystem::path(path).parent_path().filename().string();
}

// Three hops up from the same "<project_dir>/images/<id>/<filename>"
// layout image_id_from_path() reads — lets a path-only call site (no
// active_protocol/project_dir of its own in scope) still look up this
// image's images.json entry.
std::string project_dir_from_image_path(const std::string& path) {
    return std::filesystem::path(path).parent_path().parent_path().parent_path().string();
}

// {title_override, caption_override} for the ImageEntry matching `id` —
// both empty if project_dir/id isn't tracked in images.json at all. The
// one place every image title/subtitle display (the sequence dropdown's
// own button and rows, the track dropdown's subtitle, Image Details)
// reads this project's own correction to the file's IPTC/XMP title/
// caption (see ImageEntry's own comment, images.hpp).
std::pair<std::string, std::string> image_overrides_for_id(const std::string& project_dir,
                                                              const std::string& id) {
    for (const auto& entry : read_images_index(project_dir).images) {
        if (entry.id == id) {
            return {entry.title_override, entry.caption_override};
        }
    }
    return {"", ""};
}

// Image Details' own slug lookup — shown read-only so
// whoever is hand-authoring protocol.md has somewhere to find it. "" if
// the image isn't tracked in images.json at all (no project active, or
// an image whose path predates this lookup).
std::string slug_for_image_path(const std::string& project_dir, const std::string& path) {
    if (project_dir.empty() || path.empty()) {
        return "";
    }
    std::string id = image_id_from_path(path);
    auto index = read_images_index(project_dir);
    for (const auto& entry : index.images) {
        if (entry.id == id) {
            return entry.slug;
        }
    }
    return "";
}

// The reverse of slug_for_image_path() — protocol.md's own
// ProtocolEntry::image_ref is a slug, so resolving it to an actual path
// goes through images.json first (protocol.cpp itself has no notion of
// images.json, see protocol.hpp). "" if the slug isn't tracked.
std::string path_for_image_slug(const std::string& project_dir, const std::string& slug) {
    if (project_dir.empty() || slug.empty()) {
        return "";
    }
    auto index = read_images_index(project_dir);
    for (const auto& entry : index.images) {
        if (entry.slug == slug) {
            return image_file_path(project_dir, entry.id, entry.filename);
        }
    }
    return "";
}

// The protocol entry whose H2 task this image belongs to, if any — same
// slug-based match g_protocol_load_scenario_image() uses, just starting
// from a path instead of a slug: an ordinary image switch during Record
// (Forward/Back nav, the image dropdown) always has a path on hand, not
// a slug, unlike the protocol walkthrough itself. "" if the image isn't
// tied to any scenario, there's no active protocol, or no project.
// Deliberately applied at every image switch during Record, not just
// the walkthrough — an active protocol should tag a matching image the
// same way regardless of how the user got there.
std::string scenario_title_for_image(const std::shared_ptr<Protocol>& active_protocol,
                                      const std::string& project_dir,
                                      const std::string& image_path) {
    std::string slug = slug_for_image_path(project_dir, image_path);
    if (slug.empty()) {
        return "";
    }
    for (const auto& entry : active_protocol->entries) {
        if (entry.image_ref == slug) {
            return entry.title;
        }
    }
    return "";
}

// A separate, top-level Gtk::Window for the protocol section of the
// current test step — deliberately not a dialog/popover, so it can be
// dragged to a second monitor. A heap-allocated singleton, created
// once and only ever hidden (set_hide_on_close()), never destroyed, so
// its position/size survive being reopened. Not registered with the
// Gtk::Application, so it never keeps the app alive on its own while
// sitting hidden.
//
// Built around AdwNavigationView/AdwNavigationPage, a good fit for a
// sequence of pages pushed forward as test steps complete.
// g_protocol_toast_overlay wraps the nav view, for the Back-navigation
// warning toast.
Gtk::Window* g_protocol_window = nullptr;
AdwNavigationView* g_protocol_nav_view = nullptr;
AdwToastOverlay* g_protocol_toast_overlay = nullptr;
// The Protocol window *drives* which image shows, strictly forward
// through the whole protocol from page one, starting from "No image" —
// never the reverse (mirroring whatever image happens to be on screen).
// See start_protocol_walkthrough() and g_protocol_load_scenario_image
// below.

// Called with a scenario's own slug the moment the user's navigation
// actually *reaches* that scenario's content, not merely because pages
// for it exist. Loads that scenario's image and, in Record mode,
// starts a new track like any other image change. Assigned once in
// on_activate().
std::function<void(const std::string&)> g_protocol_load_scenario_image;

// Called with the just-completed scenario's own slug when its last test
// step resolves (Validated or Failed, never Back) — finds its index in
// active_protocol->entries (protocol.md's own order) and loads the
// next scenario's pages, or shows the completion page and resets to
// "No image" if that was the last one.
std::function<void(const std::string&)> g_protocol_advance_scenario;

// A plain, button-less page — just a message. Used for "no matching
// scenario" and the edge case of an empty scenario (no intro, no test
// steps), since an AdwNavigationView with zero pages isn't valid.
AdwNavigationPage* build_protocol_message_page(const Glib::ustring& title,
                                                  const Glib::ustring& message) {
    auto* label = Gtk::make_managed<Gtk::Label>(message);
    label->set_wrap(true);
    label->set_xalign(0.0f);
    label->set_valign(Gtk::Align::START);
    label->set_margin(12);

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_vexpand(true);
    scroller->set_child(*label);

    auto* page =
        adw_navigation_page_new(GTK_WIDGET(scroller->gobj()), title.c_str());
    adw_navigation_page_set_can_pop(page, FALSE);
    return page;
}

// An "intro" page — protocol-level (text before the first H2,
// introducing the protocol) or scenario-level (text before the first
// H3, presenting the scenario) — just the
// text and a single "Next" button, never Validated/Failed/Back: it's an
// overview, not a test step of its own.
AdwNavigationPage* build_protocol_intro_page(const Glib::ustring& title,
                                               const std::string& intro_text,
                                               std::function<void()> on_next) {
    auto* label = Gtk::make_managed<Gtk::Label>(intro_text);
    label->set_wrap(true);
    label->set_xalign(0.0f);
    label->set_valign(Gtk::Align::START);
    label->set_margin(12);
    label->set_selectable(true);

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_vexpand(true);
    scroller->set_child(*label);

    auto* next_button = Gtk::make_managed<Gtk::Button>(_("Next"));
    next_button->add_css_class("suggested-action");
    next_button->set_halign(Gtk::Align::END);
    next_button->set_margin(12);
    next_button->signal_clicked().connect(on_next);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box->append(*scroller);
    box->append(*next_button);

    auto* page = adw_navigation_page_new(GTK_WIDGET(box->gobj()), title.c_str());
    adw_navigation_page_set_can_pop(page, FALSE);
    return page;
}

// One test step's own page — its task text, then its H4 sub-blocks,
// collapsed by default (an "expected result" is just one of many
// possible labels; Gtk::Expander fits this directly), then the
// Validated/Failed/Back row. Back is only sensitive when there's
// something to pop back to.
//
// Shared by build_protocol_step_page() (Record/Interactive) and
// build_protocol_step_page_readonly() (Playback) — the body text and
// H4 blocks are identical; only what goes below differs.
Gtk::Widget* build_protocol_step_content(const TestStep& step) {
    auto* body_label = Gtk::make_managed<Gtk::Label>(step.body);
    body_label->set_wrap(true);
    body_label->set_xalign(0.0f);
    body_label->set_margin(12);
    body_label->set_selectable(true);

    auto* content_box =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    content_box->append(*body_label);

    for (const auto& block : step.blocks) {
        auto* expander = Gtk::make_managed<Gtk::Expander>(block.label);
        expander->set_margin_start(12);
        expander->set_margin_end(12);
        auto* block_label = Gtk::make_managed<Gtk::Label>(block.body);
        block_label->set_wrap(true);
        block_label->set_xalign(0.0f);
        block_label->set_margin(8);
        block_label->set_selectable(true);
        expander->set_child(*block_label);
        content_box->append(*expander);
    }

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    scroller->set_vexpand(true);
    scroller->set_child(*content_box);
    return scroller;
}

AdwNavigationPage* build_protocol_step_page(const TestStep& step,
                                               bool can_go_back,
                                               std::function<void()> on_validated,
                                               std::function<void()> on_failed,
                                               std::function<void()> on_back) {
    Gtk::Widget* scroller = build_protocol_step_content(step);

    // Back is allowed, but logged loudly (TestStepEvent's own "back"
    // subtype, plus the toast on_back itself shows) rather than blocked
    // outright, since it can affect whether later results are
    // representative.
    auto* back_button = Gtk::make_managed<Gtk::Button>(_("Back"));
    back_button->set_sensitive(can_go_back);
    back_button->signal_clicked().connect(on_back);

    auto* failed_button = Gtk::make_managed<Gtk::Button>(_("Failed"));
    failed_button->add_css_class("destructive-action");
    failed_button->signal_clicked().connect(on_failed);

    auto* validated_button = Gtk::make_managed<Gtk::Button>(_("Validated"));
    validated_button->add_css_class("suggested-action");
    validated_button->signal_clicked().connect(on_validated);

    auto* button_row =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    button_row->set_margin(12);
    button_row->append(*back_button);
    auto* spacer = Gtk::make_managed<Gtk::Box>();
    spacer->set_hexpand(true);
    button_row->append(*spacer);
    button_row->append(*failed_button);
    button_row->append(*validated_button);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box->append(*scroller);
    box->append(*button_row);

    auto* page =
        adw_navigation_page_new(GTK_WIDGET(box->gobj()), step.title.c_str());
    adw_navigation_page_set_can_pop(page, FALSE);
    return page;
}

// Playback's own rendering of a test step — read-only, reconstructing
// "which step was current" from the already-logged events; never
// writes a new TestStepEvent. `outcome_type` reports what was already
// decided during Record ("validated"/"failed"/"back").
AdwNavigationPage* build_protocol_step_page_readonly(const TestStep& step,
                                                         const std::string& outcome_type) {
    Gtk::Widget* scroller = build_protocol_step_content(step);

    Glib::ustring outcome_text;
    const char* outcome_css = nullptr;
    if (outcome_type == "validated") {
        outcome_text = _("Validated");
        outcome_css = "success";
    } else if (outcome_type == "failed") {
        outcome_text = _("Failed");
        outcome_css = "error";
    } else {
        outcome_text = _("Back navigation may invalidate results");
    }

    auto* outcome_label = Gtk::make_managed<Gtk::Label>(outcome_text);
    outcome_label->set_margin(12);
    if (outcome_css) {
        outcome_label->add_css_class(outcome_css);
    }

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    box->append(*scroller);
    box->append(*outcome_label);

    auto* page =
        adw_navigation_page_new(GTK_WIDGET(box->gobj()), step.title.c_str());
    adw_navigation_page_set_can_pop(page, FALSE);
    return page;
}

// Rebuilds g_protocol_nav_view from scratch for the scenario whose
// image is now on screen: lead-in page(s) first (the protocol's own
// intro when `is_first_scenario`, and/or this scenario's own, skipped
// when empty), then the first test step. Later steps are pushed
// lazily from the previous step's own Validated/Failed handler.
//
// TestStepEvent logging happens here, only while actually recording —
// Interactive mode never creates these events; it exists only so the
// user can verify things are wired correctly.
//
// `drive_display`: true for the walkthrough itself, where reaching
// this scenario's content should drive the display; false for the
// single-click "mirror what's already on screen" path
// (refresh_protocol_window()), so the image already showing is never
// reloaded or re-logged as a new track.
void load_protocol_scenario(
    const Protocol& protocol_snapshot, const ProtocolEntry& entry,
    bool is_first_scenario,
    const std::shared_ptr<RecordingSession>& recording_session,
    const std::shared_ptr<int>& current_mode, bool drive_display = true) {
    if (!g_protocol_nav_view) {
        return;
    }
    auto pages = std::make_shared<ScenarioPages>(parse_scenario_pages(entry.body));
    auto scenario_title = std::make_shared<std::string>(entry.title);
    auto scenario_slug = std::make_shared<std::string>(entry.image_ref);

    auto log_event = [recording_session, current_mode](
                          const char* type_value, const std::string& scenario,
                          const std::string& step) {
        if (*current_mode != 1 || !recording_session->file) {
            return;
        }
        TestStepEvent event;
        event.type = type_value;
        event.t_ms = recording_session->accumulated_ms;
        event.scenario = scenario;
        event.step = step;
        write_event(*recording_session->file, event);
    };

    // build_step(i): builds step i's own page (not pushed). push_step(i):
    // builds and pushes it. Kept separate so the very first step (which
    // may need to become the *initial* replace() page instead of a
    // push(), when there's no intro page at all) can reuse the same
    // builder without going through push.
    auto build_step =
        std::make_shared<std::function<AdwNavigationPage*(std::size_t, bool)>>();
    auto push_step = std::make_shared<std::function<void(std::size_t)>>();
    *build_step = [pages, scenario_title, scenario_slug, log_event, push_step](
                       std::size_t index, bool can_go_back) -> AdwNavigationPage* {
        const TestStep& step = pages->steps[index];
        bool is_last = (index + 1 == pages->steps.size());
        std::string step_title = step.title;
        auto advance = [push_step, scenario_slug, index, is_last]() {
            if (is_last) {
                if (g_protocol_advance_scenario) {
                    g_protocol_advance_scenario(*scenario_slug);
                }
            } else {
                (*push_step)(index + 1);
            }
        };
        auto on_validated = [log_event, scenario_title, step_title, advance]() {
            log_event("validated", *scenario_title, step_title);
            advance();
        };
        auto on_failed = [log_event, scenario_title, step_title, advance]() {
            log_event("failed", *scenario_title, step_title);
            advance();
        };
        auto on_back = [log_event, scenario_title, step_title]() {
            log_event("back", *scenario_title, step_title);
            if (g_protocol_toast_overlay) {
                AdwToast* toast = adw_toast_new(
                    _("Back navigation may invalidate results"));
                adw_toast_overlay_add_toast(g_protocol_toast_overlay, toast);
            }
            if (g_protocol_nav_view) {
                adw_navigation_view_pop(g_protocol_nav_view);
            }
        };
        return build_protocol_step_page(step, can_go_back, on_validated,
                                          on_failed, on_back);
    };
    // push_step(index): builds step `index` (always with can_go_back=
    // true — by the time this runs there's always at least one page
    // already on the stack beneath it) and pushes it.
    *push_step = [pages, build_step](std::size_t index) {
        if (!g_protocol_nav_view || index >= pages->steps.size()) {
            return;
        }
        AdwNavigationPage* page = (*build_step)(index, /*can_go_back=*/true);
        adw_navigation_view_push(g_protocol_nav_view, page);
    };

    if (pages->steps.empty() && pages->intro.empty() &&
        !(is_first_scenario && !protocol_snapshot.intro.empty())) {
        AdwNavigationPage* page = build_protocol_message_page(
            entry.title.empty() ? Glib::ustring(_("Protocol")) : Glib::ustring(entry.title),
            _("This scenario has no test steps."));
        AdwNavigationPage* stack[] = {page};
        adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
        return;
    }

    bool has_protocol_intro =
        is_first_scenario && !protocol_snapshot.intro.empty();
    bool has_scenario_intro = !pages->intro.empty();

    std::function<void()> start_steps = [push_step]() { (*push_step)(0); };

    // Fires exactly once, the moment the user's own navigation actually
    // *reaches* this scenario's own content — not merely because pages
    // for it now exist. The scenario's own intro text *is* that
    // content's own image definition, since the `[tekst](slug)` link is
    // literally the first line of it.
    auto load_scenario_image = [scenario_slug, drive_display]() {
        if (drive_display && g_protocol_load_scenario_image) {
            g_protocol_load_scenario_image(*scenario_slug);
        }
    };

    AdwNavigationPage* first_page = nullptr;
    if (has_protocol_intro && has_scenario_intro) {
        // The protocol intro is shown first, on its own, with "No
        // image" still up — this scenario's own content, and its
        // image, only arrive once the protocol intro's "Next" is
        // actually clicked.
        AdwNavigationPage* scenario_intro_page = build_protocol_intro_page(
            entry.title, pages->intro, start_steps);
        first_page = build_protocol_intro_page(
            _("Protocol"), protocol_snapshot.intro,
            [load_scenario_image, scenario_intro_page]() {
                load_scenario_image();
                if (g_protocol_nav_view) {
                    adw_navigation_view_push(g_protocol_nav_view,
                                               scenario_intro_page);
                }
            });
    } else if (has_protocol_intro) {
        // No scenario intro of its own — the protocol intro's "Next"
        // leads straight into step 0, loading the image at that point.
        first_page = build_protocol_intro_page(
            _("Protocol"), protocol_snapshot.intro,
            [load_scenario_image, push_step]() {
                load_scenario_image();
                (*push_step)(0);
            });
    } else if (has_scenario_intro) {
        // No protocol intro in the way — this scenario's own content is
        // what's shown immediately, so its image loads right now.
        load_scenario_image();
        first_page =
            build_protocol_intro_page(entry.title, pages->intro, start_steps);
    } else {
        // No intro at all, anywhere — step 0 becomes the initial page
        // directly, shown (and its image loaded) immediately, with
        // nothing before it to go Back to.
        load_scenario_image();
        first_page = (*build_step)(0, /*can_go_back=*/false);
    }

    AdwNavigationPage* stack[] = {first_page};
    adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
}

// Always starts the whole protocol from page one, regardless of
// whatever image happens to be on screen already — the Protocol window
// drives the display, not the other way around. Called both for the
// window's first-ever open
// and for a "reload" (the caller re-parses protocol.md first in that
// case; this function doesn't care which).
void start_protocol_walkthrough(
    const std::shared_ptr<Protocol>& active_protocol,
    const std::shared_ptr<RecordingSession>& recording_session,
    const std::shared_ptr<int>& current_mode) {
    if (!g_protocol_window) {
        return;
    }
    if (active_protocol->entries.empty()) {
        if (g_protocol_nav_view) {
            AdwNavigationPage* page = build_protocol_message_page(
                _("Protocol"), _("This protocol has no scenarios."));
            AdwNavigationPage* stack[] = {page};
            adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
        }
        return;
    }
    load_protocol_scenario(*active_protocol, active_protocol->entries[0], true,
                             recording_session, current_mode);
}

// Shows whichever scenario's own section matches whatever image is
// already on screen — mirroring the current state is right for a
// single click, but conflicts with starting the walkthrough carousel;
// the double-click override on the doc button handles that case
// instead. `drive_display=false` means reaching this scenario's
// content never re-loads the image or logs a spurious new track —
// this function only ever *reads* the current state.
void refresh_protocol_window(
    const std::string& project_dir, const std::shared_ptr<Protocol>& active_protocol,
    const std::string& current_image_path,
    const std::shared_ptr<RecordingSession>& recording_session,
    const std::shared_ptr<int>& current_mode) {
    if (!g_protocol_window) {
        return;
    }
    // Nothing on screen to mirror (splash never dismissed, or explicit
    // "No image" — both leave current_image_path empty): fall through
    // to starting the walkthrough rather than reporting a "no match".
    // "No Protocol Section" below is reserved for a real image with no
    // matching section.
    if (current_image_path.empty()) {
        start_protocol_walkthrough(active_protocol, recording_session, current_mode);
        return;
    }
    std::string slug = slug_for_image_path(project_dir, current_image_path);
    const ProtocolEntry* match = nullptr;
    std::size_t match_index = 0;
    if (!slug.empty()) {
        for (std::size_t i = 0; i < active_protocol->entries.size(); ++i) {
            if (active_protocol->entries[i].image_ref == slug) {
                match = &active_protocol->entries[i];
                match_index = i;
                break;
            }
        }
    }
    if (match) {
        load_protocol_scenario(*active_protocol, *match, match_index == 0,
                                 recording_session, current_mode,
                                 /*drive_display=*/false);
    } else if (g_protocol_nav_view) {
        AdwNavigationPage* page = build_protocol_message_page(
            _("No Protocol Section"),
            _("The image currently on screen has no matching section in "
              "protocol.md."));
        AdwNavigationPage* stack[] = {page};
        adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
    }
}

// Lazily builds g_protocol_window the first time it's needed — see its
// own comment above for why it's a bare top-level window rather than
// anything parented/managed.
void ensure_protocol_window() {
    if (g_protocol_window) {
        return;
    }
    auto* window = new Gtk::Window();
    window->set_title(_("Protocol"));
    window->set_default_size(480, 420);
    window->set_hide_on_close(true);

    auto* header = Gtk::make_managed<Gtk::HeaderBar>();
    window->set_titlebar(*header);

    auto* nav_view = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
    adw_navigation_view_set_pop_on_escape(nav_view, FALSE);
    AdwNavigationPage* placeholder =
        build_protocol_message_page(_("Protocol"), "");
    AdwNavigationPage* stack[] = {placeholder};
    adw_navigation_view_replace(nav_view, stack, 1);

    auto* toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(toast_overlay, GTK_WIDGET(nav_view));
    window->set_child(*Glib::wrap(GTK_WIDGET(toast_overlay), false));

    g_protocol_window = window;
    g_protocol_nav_view = nav_view;
    g_protocol_toast_overlay = toast_overlay;
}

// Increments the referenced image's own ref_count — only the first
// time *this* session references it, checked against that session's
// own already-written events, so reopening a session to record more
// never double-counts (one count per session, not per track). Call
// before writing the new ImageEvent, or the check would find the one
// about to be written. No-op if the image isn't tracked.
//
// Only counts whole sessions being created/deleted — deleting a single
// track (not the whole session) would need revisiting this scheme,
// since a session could lose references without being deleted.
void count_new_image_reference(const std::string& project_dir,
                                 const std::string& session_path,
                                 const std::string& image_path) {
    if (project_dir.empty() || image_path.empty()) {
        return;
    }
    for (auto& event : read_record_events(session_path)) {
        if (auto* img = std::get_if<ImageEvent>(&event); img && img->path == image_path) {
            return;  // already referenced earlier in this same session
        }
    }
    std::string id = image_id_from_path(image_path);
    auto index = read_images_index(project_dir);
    for (auto& entry : index.images) {
        if (entry.id == id) {
            ++entry.ref_count;
            write_images_index(project_dir, index);
            return;
        }
    }
}

// The counterpart to count_new_image_reference() — decrements
// ref_count for every distinct image a session referenced, once per
// image regardless of track count. Called before the session's
// directory is removed, while the event stream is still readable.
void release_image_references(const std::string& project_dir,
                                const std::string& session_path) {
    std::set<std::string> referenced_paths;
    for (auto& event : read_record_events(session_path)) {
        if (auto* img = std::get_if<ImageEvent>(&event)) {
            referenced_paths.insert(img->path);
        }
    }
    if (referenced_paths.empty()) {
        return;
    }
    auto index = read_images_index(project_dir);
    bool changed = false;
    for (const auto& path : referenced_paths) {
        std::string id = image_id_from_path(path);
        for (auto& entry : index.images) {
            if (entry.id == id) {
                // Floored at 0, not asserted — a stale/inconsistent
                // count shouldn't go negative and jam the delete
                // button off forever.
                entry.ref_count = std::max(0, entry.ref_count - 1);
                changed = true;
                break;
            }
        }
    }
    if (changed) {
        write_images_index(project_dir, index);
    }
}

// Reconciles active_protocol's entries against the project's own
// images.json visible list, matched by *slug* (not path, since
// protocol.md references images by slug — see ProtocolEntry::image_ref's
// own comment) and writes the result back to source_path. No-op
// (returns false) when no project is active.
//
// An existing entry's own title/body is never touched — the slug only
// decides whether an entry exists already or needs to be added fresh;
// images.json's own current order is what moves an existing section to
// match. A brand-new entry's title/link text come from the same IPTC
// fields the image list's own rows show (title, caption).
//
// Called only from the hamburger menu's "Reconcile Protocol" action,
// not automatically. (`settings` is unused now but kept in the
// signature to avoid an unnecessary parameter-list change.)
//
// A section whose slug no longer matches any visible image is never
// dropped, only moved to the bottom — nothing with real facilitator
// text gets deleted just for falling out of the image list. Matching
// consumes entries by index, not a slug map, so two sections sharing a
// slug don't collide and silently lose one.
bool reconcile_and_write_protocol(const Glib::RefPtr<Gio::Settings>& settings,
                                    const std::shared_ptr<Protocol>& active_protocol) {
    if (active_protocol->source_path.empty()) {
        return false;
    }
    // Re-read from disk first, not the in-memory active_protocol — a
    // hand-edit made directly to protocol.md any time after the project
    // was opened (or since the last reconcile) must not be silently
    // discarded by writing back whatever was loaded earlier. protocol.md
    // specifically invites this: unlike session/image, it's the one file
    // this app expects to also be hand-edited while the project is open.
    auto fresh = parse_protocol_file(active_protocol->source_path);
    if (!fresh) {
        return false;
    }
    std::string project_dir = Glib::path_get_dirname(fresh->source_path);
    std::vector<ProtocolEntry>& entries = fresh->entries;
    std::vector<bool> consumed(entries.size(), false);
    auto take_matching = [&](const std::string& slug) -> ProtocolEntry* {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!consumed[i] && entries[i].image_ref == slug) {
                consumed[i] = true;
                return &entries[i];
            }
        }
        return nullptr;
    };
    std::vector<ProtocolEntry> reconciled;
    for (auto& img_entry : sorted_image_entries(project_dir)) {
        if (img_entry.hidden) {
            continue;
        }
        if (ProtocolEntry* match = take_matching(img_entry.slug)) {
            reconciled.push_back(*match);
            continue;
        }
        std::string path =
            image_file_path(project_dir, img_entry.id, img_entry.filename);
        auto meta = read_image_metadata(path);
        ProtocolEntry fresh;
        fresh.title = !img_entry.title_override.empty() ? img_entry.title_override
                       : !meta.title.empty()             ? meta.title
                                                           : img_entry.filename;
        fresh.image_ref = img_entry.slug;
        fresh.body = "<image:" + img_entry.slug + ">";
        reconciled.push_back(fresh);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (!consumed[i]) {
            reconciled.push_back(entries[i]);
        }
    }
    fresh->entries = reconciled;
    if (!write_protocol_file(fresh->source_path, *fresh)) {
        return false;
    }
    // Keep the in-memory copy in sync with what's now actually on disk,
    // rather than leaving it holding the pre-reconcile snapshot.
    *active_protocol = *fresh;
    return true;
}

// Session-file counterparts of the functions above — replaces the old
// gsettings-based recent-sessions/hidden-sessions keys (sessions are
// now always project-scoped, see sessions.hpp) with reads/
// writes against the active project's own sessions.json.

// A project is only "active" once a protocol.md has actually been opened
// (open_project_at() sets active_protocol->source_path) — empty here means
// no project, same check g_project_active mirrors.
std::string project_dir_from_protocol(const std::shared_ptr<Protocol>& active_protocol) {
    if (active_protocol->source_path.empty()) {
        return "";
    }
    return Glib::path_get_dirname(active_protocol->source_path);
}

// Autoload — three independent flags, each gating a restore: project
// (last-project), session (current-working-session), and image
// (last-image, a plain path so it stays meaningful as the sequence is
// edited). All default true.
constexpr auto kAutoloadProjectKey = "autoload-project";
constexpr auto kAutoloadSessionKey = "autoload-session";
constexpr auto kAutoloadImageKey = "autoload-image";
constexpr auto kLastImagePathKey = "last-image";

void set_last_image_path(const Glib::RefPtr<Gio::Settings>& settings,
                          const std::string& path) {
    settings->set_string(kLastImagePathKey, path);
}

constexpr auto kMicrophoneInitiallyOnKey = "microphone-initially-on";
constexpr auto kSpeakerInitiallyOnKey = "speaker-initially-on";
constexpr auto kTranscodeAudioToOpusKey = "transcode-audio-to-opus";
constexpr auto kTranscribeAudioKey = "transcribe-audio";
constexpr auto kDiarizeAudioKey = "diarize-audio";
constexpr auto kTranscriptionModelSizeKey = "transcription-model-size";
constexpr auto kDepthModelSizeKey = "depth-model-size";
constexpr auto kDepthGenerationEnabledKey = "depth-generation-enabled";
constexpr auto kDepthUseFiducialMarkersKey = "depth-use-fiducial-markers";
constexpr auto kYawAugmentedInputKey = "yaw-augmented-input";
constexpr auto kTiltAugmentedInputKey = "tilt-augmented-input";
constexpr auto kShowDepthAtPointerKey = "show-depth-at-pointer";
constexpr auto kProdCueDurationMsKey = "prod-cue-duration-ms";
constexpr auto kProdCueDiameterScaleKey = "prod-cue-diameter-scale";
constexpr auto kProdCuePeakOpacityKey = "prod-cue-peak-opacity";
constexpr auto kHeadProdSequenceStepMsKey = "head-prod-sequence-step-ms";
constexpr auto kDepthEstimateGridSizeKey = "depth-estimate-grid-size";
constexpr auto kNoisePulseAttackMsKey = "noise-pulse-attack-ms";
constexpr auto kNoisePulseDecayMsKey = "noise-pulse-decay-ms";
constexpr auto kNoisePulseSustainLevelKey = "noise-pulse-sustain-level";
constexpr auto kNoisePulseSustainMsKey = "noise-pulse-sustain-ms";
constexpr auto kNoisePulseReleaseMsKey = "noise-pulse-release-ms";
constexpr auto kNoisePulseColorKey = "noise-pulse-color";
constexpr auto kNoisePulseGainKey = "noise-pulse-gain";
constexpr auto kNoisePulseMaxDelayMsKey = "noise-pulse-max-delay-ms";
constexpr auto kNoisePulseMaxDelayIsNoDataKey = "noise-pulse-max-delay-is-no-data";

// Mono 16kHz 16-bit PCM WAV — plenty for spoken dialogue, and keeps
// files small without needing a compressed codec at all (an optional
// off-line Opus encode step handles that separately, see
// encode_wav_to_opus_async()). Whatever PipeWire exposes as the default
// input — no device selection here; a real dual-mic (tester + testee)
// setup is left entirely to PipeWire/WirePlumber's own default-device
// and mixing behavior, downmixed to mono regardless of what comes in.
// No-op if a pipeline is already running for this track.
void start_audio_track(const std::shared_ptr<RecordingSession>& session,
                        const std::string& path) {
    if (session->audio_pipeline) {
        return;
    }
    GError* error = nullptr;
    auto* pipeline = gst_parse_launch(
        "pipewiresrc ! audioconvert ! audioresample ! "
        "audio/x-raw,format=S16LE,channels=1,rate=16000 ! wavenc ! "
        "filesink name=sink",
        &error);
    if (!pipeline) {
        if (error) {
            g_error_free(error);
        }
        return;
    }
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    g_object_set(sink, "location", path.c_str(), nullptr);
    g_object_unref(sink);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    session->audio_pipeline = pipeline;

    // Timing info for lining audio up correctly during Playback, and
    // later for a transcript. Written here rather than at
    // each call site so it can never be forgotten at a future one.
    if (session->file) {
        MicrophoneEvent event;
        event.t_ms = session->elapsed_ms();
        event.on = true;
        write_event(*session->file, event);
    }
}

// Cleanly finalizes the current track's WAV file. Straight PLAYING ->
// NULL, no EOS event: verified as the reliable, non-hanging shutdown
// for a pipewiresrc-based pipeline — an explicit EOS event was tried
// first and never arrived back on the bus in this GStreamer/PipeWire
// combination.
void stop_audio_track(const std::shared_ptr<RecordingSession>& session) {
    if (!session->audio_pipeline) {
        return;
    }
    gst_element_set_state(session->audio_pipeline, GST_STATE_NULL);
    gst_object_unref(session->audio_pipeline);
    session->audio_pipeline = nullptr;

    // Paired with the "on" event in start_audio_track() above — every
    // call site (Stop, Close, the mic toggle) calls this before the
    // session file itself is closed, so `session->file` is always still
    // valid here whenever a pipeline actually was running.
    if (session->file) {
        MicrophoneEvent event;
        event.t_ms = session->elapsed_ms();
        event.on = false;
        write_event(*session->file, event);
    }
}

// .osd gives a translucent dark backing with no control over
// opacity/rounding/padding, and .dim-label sets *opacity* on the label
// itself — exactly the "transparent text" effect that should never
// happen. A small dedicated stylesheet instead: opaque text, tunable
// background alpha, explicit padding.
void load_app_css() {
    auto provider = Gtk::CssProvider::create();
    provider->load_from_data(
        // "No image chosen yet" placeholder background for Record.
        ".blind-sight-no-image-background {"
        "  background-color: #7b7b7b;"
        "}"
        ".blind-sight-splash-caption {"
        "  background-color: rgba(0, 0, 0, 0.75);"
        "  border-radius: 12px;"
        "  padding: 16px 24px;"
        "  color: rgb(255, 255, 255);"
        "}"
        ".blind-sight-splash-copyright {"
        "  color: rgb(200, 200, 200);"
        "}"
        // Active-state indicators for the Record/Playback action bar buttons —
        // just the (symbolic-icon) foreground color, not a filled
        // background like .destructive-action/.success give you. @error_
        // color / @success_color are libadwaita's named theme colors, so
        // this still follows light/dark and accent changes correctly.
        ".blind-sight-record-active {"
        "  color: @error_color;"
        "}"
        ".blind-sight-playback-active {"
        "  color: @success_color;"
        "}"
        // Round icon buttons (Details/Edit Collection) — ".flat" alone
        // only shows a background on hover/press; this gives a visible
        // background already at rest (matching GNOME QuickSettings'
        // own icon style), hover/press just shifting it
        // further. @theme_fg_color (light/dark-adaptive, same reasoning
        // as @error_color/@success_color above) kept at low alpha
        // instead of a fixed color so this still works in both themes.
        // Loaded at APPLICATION priority (this whole provider is), so it
        // wins over the theme's own ".flat" background rule regardless
        // of class order on the widget.
        ".blind-sight-round-icon-button {"
        "  background-color: alpha(@theme_fg_color, 0.08);"
        "}"
        ".blind-sight-round-icon-button:hover {"
        "  background-color: alpha(@theme_fg_color, 0.15);"
        "}"
        ".blind-sight-round-icon-button:active {"
        "  background-color: alpha(@theme_fg_color, 0.22);"
        "}"
        // "Distance at pointer" readout — a sighted-
        // tester verification aid, not part of the actual sensory-
        // substitution output, so it reads as a light, unobtrusive
        // grey impression rather than a heavy black .osd blob: a
        // translucent grey pill, small padding, barely rounded corners.
        ".blind-sight-distance-label {"
        "  background-color: rgba(60, 60, 60, 0.55);"
        "  border-radius: 6px;"
        "  padding: 2px 8px;"
        "  color: rgba(255, 255, 255, 0.9);"
        "}"
        // track_button sits one box deeper than the header bar's other
        // dropdown buttons (clock_group is its direct parent), so its
        // corners need shaping by hand to read as "joined onto
        // clock_label's right edge" — square on the left, round only
        // on the right, without .linked's own wrong border/background
        // treatment for a non-button neighbor.
        ".blind-sight-clock-track-button {"
        "  border-top-left-radius: 0;"
        "  border-bottom-left-radius: 0;"
        "}"
        // clock_label's edit-mode entry rendered with the theme's
        // ordinary (dark/near-black) entry background instead of
        // blending into the header bar like the other dropdown buttons'
        // own flat look — GtkEditableLabel's internal
        // editing widget is a plain GtkText, CSS node name "text".
        ".blind-sight-clock-label text {"
        "  background-color: rgba(128, 128, 128, 0.35);"
        "  color: inherit;"
        "}"
        // GtkEditableLabel keeps a visible entry-like box even in
        // plain-text display mode, unlike track_button's own .flat.
        // Forced transparent at rest, matching track_button's own join
        // (round on the left, square on the right where it meets
        // track_button), grey only on hover or while focused/editing.
        ".blind-sight-clock-label,"
        ".blind-sight-clock-label label {"
        "  background-color: transparent;"
        "  border-radius: 6px 0px 0px 6px;"
        "}"
        ".blind-sight-clock-label:hover,"
        ".blind-sight-clock-label:focus-within {"
        "  background-color: rgba(127, 127, 127, 0.15);"
        "}"
        // Monitoring matrix toggle cells (monitoring_dialogs.cpp) — same
        // muted-background idea as .blind-sight-round-icon-button, but
        // square, not circular, and explicitly restating the same
        // background for :checked so a muted vs. unmuted cell differs
        // only by its icon, never by GtkToggleButton's own themed
        // "checked" color; only hover/press change the shade. Corner
        // rounding is added per-cell in code, only for the toggle
        // grid's own four true outer corners — everywhere else stays
        // square. border-radius lives in its own rule, deliberately NOT
        // paired with :checked — a class+pseudo-class selector outranks
        // the corner classes' plain class selector, which silently
        // defeated the top-left corner's own rounding specifically
        // (the diagonal self-mute cell, the one corner that's checked
        // by default) until this was split out.
        // padding/min-width/min-height here, not just the widget-level
        // set_size_request() call in monitoring_dialogs.cpp — a plain
        // Gtk::Button's own natural size (theme padding plus its label/
        // icon content) still varies with content and can exceed a
        // bare set_size_request() floor, which only guarantees a
        // minimum, not an exact size; that's what was making the
        // two-letter participant buttons wider than the icon-only
        // toggle cells next to them despite both requesting the same
        // fixed size.
        // Zeroing padding and pinning min-width/min-height here removes
        // that content-driven variance, so every cell in the matrix —
        // button or icon — ends up exactly the same size (kept in sync
        // with monitoring_dialogs.cpp's own kMatrixCellSize constant).
        ".blind-sight-matrix-cell {"
        "  border-radius: 0;"
        "  padding: 0;"
        "  min-width: 40px;"
        "  min-height: 40px;"
        "}"
        ".blind-sight-matrix-cell,"
        ".blind-sight-matrix-cell:checked {"
        "  background-color: alpha(@theme_fg_color, 0.08);"
        "}"
        ".blind-sight-matrix-cell:hover,"
        ".blind-sight-matrix-cell:checked:hover {"
        "  background-color: alpha(@theme_fg_color, 0.15);"
        "}"
        ".blind-sight-matrix-cell:active,"
        ".blind-sight-matrix-cell:checked:active {"
        "  background-color: alpha(@theme_fg_color, 0.22);"
        "}"
        // Header buttons (participant/Speaker/Ambient Mic,
        // monitoring_dialogs.cpp) whose axis has a real device assigned —
        // same foreground-only convention as .blind-sight-record-active/
        // .blind-sight-playback-active just above: @accent_color is
        // libadwaita's own named role color, indirected through the
        // user's system accent choice (Settings > Appearance > Accent
        // Color) rather than this rule picking or baking in a color
        // itself, so it tracks whatever they've chosen — green today,
        // something else tomorrow — automatically.
        ".blind-sight-matrix-device-assigned {"
        "  color: @accent_color;"
        "}"
        // The disconnected mark is small enough that accent made it hard
        // to see; plain foreground stands out against the dimmed content.
        ".blind-sight-matrix-disconnected-mark {"
        "  color: @theme_fg_color;"
        "}"
        ".blind-sight-matrix-corner-top-left {"
        "  border-top-left-radius: 6px;"
        "}"
        ".blind-sight-matrix-corner-top-right {"
        "  border-top-right-radius: 6px;"
        "}"
        ".blind-sight-matrix-corner-bottom-left {"
        "  border-bottom-left-radius: 6px;"
        "}"
        ".blind-sight-matrix-corner-bottom-right {"
        "  border-bottom-right-radius: 6px;"
        "}");
    Gtk::StyleContext::add_provider_for_display(
        Gdk::Display::get_default(), provider,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

// Registers resource/icons/ as an icon search path, so e.g.
// "blind-sight-bat-symbolic" resolves as a real symbolic icon name,
// with the same automatic light/dark recoloring every stock -symbolic
// icon gets. Files placed directly in the search path (no
// hicolor/<size>/<context>/ subdirs) work as an icon-name fallback per
// Gtk::IconTheme::add_search_path()'s own docs.
void register_app_icons() {
    Gtk::IconTheme::get_for_display(Gdk::Display::get_default())
        ->add_search_path(resource_path("icons"));
}

// Loads an SVG straight from resource/icons/, bypassing icon-theme name
// lookup — some icon names (e.g. "reorder-symbolic") don't reliably
// resolve even with register_app_icons()'s search path, since it's
// ambiguous which icon theme is installed as default.
// Gtk::IconPaintable::create() still honors the "-symbolic" naming
// convention for recoloring. add_css_class("image-button") matches
// what set_icon_name() applies internally, so sizing/padding stays
// consistent.
void set_button_icon_from_file(Gtk::Button* button, const std::string& filename,
                                  int size = 16) {
    auto file = Gio::File::create_for_path(resource_path("icons/" + filename));
    auto paintable = Gtk::IconPaintable::create(file, size);
    auto* image = Gtk::make_managed<Gtk::Image>(paintable);
    image->set_pixel_size(size);
    button->set_child(*image);
    button->add_css_class("image-button");
}

// "Dump Sound Paths" hamburger-menu action — "sound path" rather than
// "PipeWire," since enumerate_audio_devices() already merges in BlueZ
// too, same broader scope Trace Sound Delay's own name already quietly
// covers (PipeWire, Bluetooth, and other bits) without either label
// growing past what still reads as a normal menu entry. Was originally
// a "Dump to File" button bolted onto the monitoring matrix's own
// device-picker dialog (monitoring_dialogs.cpp), but that was really a
// development aid for tracing enumeration problems in general, not a
// per-dialog user feature; John moved it here as one standalone dump
// covering both directions at once, rather than whichever single
// picker instance happened to be open, and asked for a save-location
// prompt rather than a fixed path. enumerate_audio_devices() already
// merges BlueZ and GStreamer/PipeWire results per class, so each
// section here is already BT+PipeWire combined, not raw per-backend
// dumps.
void dump_sound_paths(Gtk::Window* parent, AdwToastOverlay* toast_overlay) {
    auto file_dialog = Gtk::FileDialog::create();
    file_dialog->set_title(_("Dump Sound Paths"));
    file_dialog->set_initial_name("blind-sight-audio-devices.txt");
    file_dialog->save(*parent, [file_dialog, toast_overlay](
                                     Glib::RefPtr<Gio::AsyncResult>& result) {
        try {
            auto file = file_dialog->save_finish(result);
            std::string path = file->get_path();
            std::ofstream out(path, std::ios::out | std::ios::trunc);
            auto dump_class = [&out](const std::string& gst_device_class) {
                out << "== " << gst_device_class << " ==\n";
                for (const auto& device : enumerate_audio_devices(gst_device_class)) {
                    out << device.id << " | " << device.display_name << " | "
                        << device.subtitle << " | "
                        << (device.connected ? "connected" : "not connected") << '\n';
                }
            };
            dump_class("Audio/Sink");
            dump_class("Audio/Source");
            AdwToast* toast =
                adw_toast_new(Glib::ustring::compose(_("Dumped to %1"), path).c_str());
            adw_toast_overlay_add_toast(toast_overlay, toast);
        } catch (const Glib::Error&) {
            // User cancelled the picker.
        }
    });
}

// All user-facing strings go through _() (gettext) so the app is
// translatable. English is the source (msgid) language; Norwegian and
// others are .po translations (see po/, compiled via msgfmt). _() is a
// safe no-op for any locale without a catalog.

// Gaze cursor — a circle standing in for the fovea's high-acuity
// patch. Sighted-tester aid only; a blind end user never sees it.
//
// Sized from the *simulated* field of view, not the real screen:
// imagine a homunculus inside the window seeing a 2° gaze — in
// fullscreen the user becomes that homunculus, so the circle scales
// up. kViewportFovDegrees is that simulated FOV; the cursor's angular
// size is just widget-pixels-per-degree × the foveal angle, which
// gives the right size in both a small window and fullscreen for free.
constexpr double kFovealRadiusDeg = 1.0;  // ~2° diameter
constexpr int kDefaultViewingDistanceMm = 375;  // ~35-40cm; kept for a
    // future physical-glasses calibration, unused by
    // gaze_cursor_diameter_px() now.

// Depth→distance approximation — first take on the acoustic model's
// distance cue. Depth-Anything-V2's raw output is *relative inverse
// depth* (larger = closer), the opposite of what this app needs;
// distance ≈ k/raw_depth is the natural first approximation, linear in
// 1/raw_depth. k=10 calibrated against one anchor point: raw depth 1 ≈
// 10 meters. Relative accuracy matters here, not absolute — k/raw_depth
// is monotonically increasing by construction for any raw_depth > 0.
constexpr double kDepthToDistanceK = 10.0;
double raw_depth_to_distance_m(float raw_depth) {
    constexpr float kMinRawDepth = 1e-3f;
    return kDepthToDistanceK / std::max(raw_depth, kMinRawDepth);
}

// Whether the current image has a depth map loaded — gates the
// gaze-marker/prod-event apparatus, not the distance estimate alone.
// Without depth data, gaze markers and prods are meaningless, but
// panning and metadata viewing stay unaffected.
bool session_has_depth(const RecordingSession& session) {
    return !session.depth_data.empty() && session.depth_width > 0 &&
           session.depth_height > 0;
}

// Mean raw depth within a small grid centered on a prod's position,
// converted to an estimated distance — the grid covers the same area
// the gaze circle does. `grid_n` is the grid's own side length (1/4/9/
// 16/25-element options in Preferences, kDepthEstimateGridSizeKey).
//
// The grid's radius in depth-map pixels is only approximate: a precise
// version would re-run equirect_pixel_uv() per grid point, not needed
// for this rough estimate. Instead treats the depth map as spanning a
// full 360°×180° and scales kFovealRadiusDeg directly into depth-map
// pixels — good enough for a relative estimate, not exact at a GPano
// crop's own edge.
//
// Sky/undefined regions give a raw depth near zero, a real, large
// fraction of typical scenes (measured: 30-40% of pixels in several
// example depth files are under 0.01). raw_depth_to_distance_m()'s
// reciprocal blows that up into an enormous distance for undefined
// content — deciding here whether that's unreliable would need a
// threshold less meaningful than a delay/time-domain limit;
// distance_to_delay_ms() below handles that instead.
std::optional<double> compute_prod_distance_m(const RecordingSession& session,
                                               Gtk::Widget& picture,
                                               std::uint16_t x_norm,
                                               std::uint16_t y_norm,
                                               int grid_n) {
    if (session.depth_data.empty() || session.depth_width <= 0 ||
        session.depth_height <= 0) {
        return std::nullopt;
    }
    double fov_rad = kViewportFovDegrees * std::numbers::pi / 180.0;
    double yaw_rad = session.view_yaw / 100.0 * std::numbers::pi / 180.0;
    double pitch_rad = session.view_pitch / 100.0 * std::numbers::pi / 180.0;
    double tan_half_fov = std::tan(fov_rad / 2.0);
    double aspect = static_cast<double>(std::max(picture.get_width(), 1)) /
                     std::max(picture.get_height(), 1);
    PanoramaProjection proj = resolve_panorama_projection(
        session.panorama_projection,
        session.equirect_source ? session.equirect_source->get_width() : 1,
        session.equirect_source ? session.equirect_source->get_height() : 1);

    int x_px = static_cast<int>(x_norm / 65535.0 * picture.get_width());
    int y_px = static_cast<int>(y_norm / 65535.0 * picture.get_height());
    double u, v;
    equirect_pixel_uv(x_px, y_px, picture.get_width(), picture.get_height(),
                       tan_half_fov, aspect, std::cos(yaw_rad),
                       std::sin(yaw_rad), std::cos(pitch_rad),
                       std::sin(pitch_rad), proj, u, v);
    int center_dx = static_cast<int>(u * session.depth_width);
    int center_dy = std::clamp(static_cast<int>(v * session.depth_height), 0,
                                session.depth_height - 1);

    double radius_x = kFovealRadiusDeg / 360.0 * session.depth_width;
    double radius_y = kFovealRadiusDeg / 180.0 * session.depth_height;

    double sum = 0.0;
    int count = 0;
    for (int row = 0; row < grid_n; ++row) {
        double fy = grid_n > 1 ? -1.0 + 2.0 * row / (grid_n - 1) : 0.0;
        for (int col = 0; col < grid_n; ++col) {
            double fx = grid_n > 1 ? -1.0 + 2.0 * col / (grid_n - 1) : 0.0;
            int dx = center_dx + static_cast<int>(std::lround(fx * radius_x));
            int dy = center_dy + static_cast<int>(std::lround(fy * radius_y));
            dx %= session.depth_width;
            if (dx < 0) {
                dx += session.depth_width;
            }
            dy = std::clamp(dy, 0, session.depth_height - 1);
            sum += session.depth_data[static_cast<std::size_t>(dy) *
                                           static_cast<std::size_t>(
                                               session.depth_width) +
                                       static_cast<std::size_t>(dx)];
            ++count;
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return raw_depth_to_distance_m(static_cast<float>(sum / count));
}

// Reads/parses kDepthEstimateGridSizeKey into the grid's own side length
// (grid_n — 1..5, for compute_prod_distance_m() above) — factored out
// since both fire_prod() (on_activate()) and fire_eye_prod()
// (build_splash()) need the same parse, not just one of them.
int depth_estimate_grid_n(const Glib::RefPtr<Gio::Settings>& settings) {
    std::string grid_size_str = settings->get_string(kDepthEstimateGridSizeKey).raw();
    int grid_elements = 9;
    try {
        grid_elements = std::stoi(grid_size_str);
    } catch (const std::exception&) {
        grid_elements = 9;
    }
    return std::max(static_cast<int>(std::lround(std::sqrt(grid_elements))), 1);
}

// Distance → echo delay — first take on this part of the acoustic
// model: a linearized delay, roughly 0.3 seconds per 100 meters (see
// notes.md, "Distance → repetition pitch"). Only the *sound* is
// delayed, not the glow, which is instant confirmation the prod
// registered.
//
// Capped at kNoisePulseMaxDelayMsKey: sky/undefined regions give a raw
// depth near zero (30-40% of pixels in several example depth files),
// and raw_depth_to_distance_m()'s reciprocal blows that up to an
// enormous, unclamped delay — up to 30 seconds, decoupled from
// anything currently happening.
//
// Returns nullopt instead of the clamped max when
// kNoisePulseMaxDelayIsNoDataKey is on: past the cap, the content has
// no meaningful depth reading, so the honest response is no sound
// rather than a guessed distance. No depth data at all (distance_m
// itself empty) always plays immediately — nothing unreliable to
// decide about.
constexpr double kDistanceToDelaySecPerMeter = 0.3 / 100.0;
std::optional<unsigned int> distance_to_delay_ms(
    std::optional<double> distance_m, const Glib::RefPtr<Gio::Settings>& settings) {
    if (!distance_m) {
        return 0u;
    }
    double delay_s = *distance_m * kDistanceToDelaySecPerMeter;
    unsigned int delay_ms =
        static_cast<unsigned int>(std::lround(std::max(0.0, delay_s) * 1000.0));
    auto max_delay_ms =
        static_cast<unsigned int>(settings->get_int(kNoisePulseMaxDelayMsKey));
    if (delay_ms <= max_delay_ms) {
        return delay_ms;
    }
    if (settings->get_boolean(kNoisePulseMaxDelayIsNoDataKey)) {
        return std::nullopt;
    }
    return max_delay_ms;
}

// Screen size/resolution come from the monitor; viewing distance can't,
// so it's the one input that has to be a setting (first real content in
// Preferences). mm internally, cm in the Preferences row — purely a
// display concern.
//
// The GSettings key itself holds no unit in its name — it assumes SI
// (metres) implicitly, so 37.5cm is stored as 0.375. The mm/cm
// conversion happens only where GSettings is read or written.
constexpr auto kViewingDistanceKey = "viewing-distance";
int g_viewing_distance_mm = kDefaultViewingDistanceMm;

// Set once inside on_activate() (first real build only), to
// open_project_at() itself. main()'s signal_command_line() handler calls
// this with whatever project directory a command-line argument resolved
// to. A global, not a parameter, because it must survive past
// on_activate() returning: a second `blind-sight-demo <path>` launch
// while already running never re-enters on_activate()'s build logic,
// only re-fires signal_command_line() via D-Bus on this same process.
// Returns whether the project was actually found/opened.
std::function<bool(const std::string&)> g_open_project_by_dir;

// Companion to g_open_project_by_dir — clears back to "No Project": a
// command-line path that can't be found should still leave the app
// open, just with nothing selected.
std::function<void()> g_reset_to_no_project;

// Runs on_activate()'s own "New Project…" flow from anywhere, not just
// the project dropdown's own button, so the project collection editor's
// plus icon can trigger the identical flow. Set once in on_activate().
// The parameter, if set, fires once the new project is actually created
// and switched to — the collection editor's own leading action uses it to
// refresh its row list without needing to close first.
std::function<void(std::function<void()>)> g_start_new_project;

// Same reasoning, for "New Session".
std::function<void(std::function<void()>)> g_start_new_session;

// Same pattern for `--session <file>` — unlike the project path, isn't
// resolved first (already a specific file). Success locks
// session_recent_button insensitive; failure shows its own error dialog
// and leaves the dropdown sensitive.
std::function<void(const std::string&)> g_open_session_by_path;

// Re-applies the "Image" autoload switch's own remembered image, if
// still on disk. Needed because open_project_at()/apply_shared_session()
// both reset the shared picture to "No image" by default, which would
// otherwise clobber what the startup autoload-image block restored if a
// --project/--session argument runs afterward. Called a second time,
// after that argument handling; a no-op if the switch is off.
std::function<void()> g_reapply_last_image_autoload;

// Companion to g_reapply_last_image_autoload — autoload-session/
// autoload-image only fire once a project is active (whether via
// autoload-project or an explicit --project), or they'd defeat the
// point of turning autoload-project off for privacy. No-op without an
// active project, with the switch off, or if session_arg already set a
// session explicitly.
std::function<void()> g_reapply_session_autoload;

// Set once a project is active (open_project_at()'s success path),
// cleared by reset_to_no_project() — what the two autoload functions
// above gate on.
bool g_project_active = false;

// Traditional (true, default) vs. natural viewport-scroll direction —
// GNOME's own terminology; "natural" genuinely differs person to
// person. See show_preferences().
constexpr auto kTraditionalScrollKey = "traditional-viewport-scroll";

// Empty string (default, "nothing remembered yet") is left alone — the
// file dialog's own default applies.
void apply_last_folder(const Glib::RefPtr<Gtk::FileDialog>& file_dialog,
                        const Glib::RefPtr<Gio::Settings>& settings,
                        const char* key) {
    auto folder = settings->get_string(key);
    if (!folder.empty()) {
        file_dialog->set_initial_folder(Gio::File::create_for_path(folder));
    }
}

void remember_last_folder(const Glib::RefPtr<Gio::Settings>& settings,
                           const char* key,
                           const Glib::RefPtr<Gio::File>& file) {
    if (auto parent_dir = file->get_parent()) {
        settings->set_string(key, parent_dir->get_path());
    }
}

// The image-sequence dropdown's "Edit Sequence…" entry — add/remove/
// reorder the prepared, specific-order set of images a test run steps
// through. Up/down buttons, not drag-and-drop: order matters here
// (unlike sessions), but a short list doesn't need full drag-reorder
// machinery. Every change writes straight back and rebuilds the row
// list in place, no separate Save action; the main dropdown picks up
// changes the next time it's opened.
//
// `on_changed`, if given, runs after every add/remove/reorder — the
// active project's own eager-write hook. project_dir is the active
// project's directory; every row comes from its own images.json, never
// gsettings.
//
// Deletion uses reference counting rather than always/never-allowed: a
// counter on each images.json entry ticks up when a session references
// it and down when deleted, and the delete action stays disabled while
// nonzero (see count_new_image_reference()/release_image_references(),
// ImageEntry::ref_count, and CollectionDestructiveAction::enabled_for in
// image_dialogs.cpp's own config). protocol.md is separate: the app
// doesn't track references into it incrementally (externally authored),
// so image_dialogs.cpp's destructive.confirm callback checks it fresh at
// deletion time and folds the result into the confirmation dialog's
// body instead of blocking on it.
// Shared "Cancel/Delete" confirmation plumbing — image_dialogs.cpp's/
// session_dialogs.cpp's own destructive.confirm callbacks only differ in
// title/body text and what deleting does, both passed in here. Same
// heap-allocated-context + destroy-notify idiom every raw-C-API dialog
// here uses to outlive the function that opened it.
void confirm_delete_entry(Gtk::Window* parent, const Glib::ustring& title,
                            const Glib::ustring& body, std::function<void()> on_delete) {
    auto* dialog = adw_alert_dialog_new(title.c_str(), nullptr);
    adw_alert_dialog_format_body(ADW_ALERT_DIALOG(dialog), "%s", body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", _("Cancel"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "delete", _("Delete"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete",
                                               ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");

    auto* ctx = new std::function<void()>(std::move(on_delete));
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            auto* ctx = static_cast<std::function<void()>*>(data);
            if (std::string(response) == "delete") {
                (*ctx)();
            }
        }),
        ctx,
        +[](gpointer data, GClosure*) { delete static_cast<std::function<void()>*>(data); },
        GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent->gobj()));
}

// Forward declarations — defined further down, alongside the other
// Preferences-adjacent dialogs; needed here already for maybe_generate_
// depth_on_the_fly()'s own setup-required case further down.
void show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                         const Glib::ustring& body);
void show_setup_required_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                 const Glib::ustring& body, const Glib::ustring& command);

// out_caption/out_picture/out_no_image_picture/out_head_direction_cross/
// out_prod_cue, if given, receive the corresponding widget so callers
// (on_activate()) can swap/hide them later. session is shared with
// Record's Forward/Stop buttons (added later) — see RecordingSession's
// own comment; a null session->file means "not recording."
Gtk::Widget* build_splash(
    std::shared_ptr<RecordingSession> session,
    std::shared_ptr<PlaybackSession> playback,
    const Glib::RefPtr<Gio::Settings>& settings,
    std::shared_ptr<std::chrono::steady_clock::time_point> last_head_movement_at,
    std::shared_ptr<NoisePulseEngine> noise_pulse_engine,
    Gtk::Widget** out_caption = nullptr, Gtk::Picture** out_picture = nullptr,
    Gtk::Picture** out_no_image_picture = nullptr,
    Gtk::Picture** out_head_direction_cross = nullptr,
    Gtk::Picture** out_prod_cue = nullptr) {
    auto* overlay = Gtk::make_managed<Gtk::Overlay>();

    auto* picture = Gtk::make_managed<Gtk::Picture>();
    picture->set_paintable(render_splash_texture());
    picture->set_content_fit(Gtk::ContentFit::COVER);
    picture->set_can_shrink(true);
    overlay->set_child(*picture);
    if (out_picture) {
        *out_picture = picture;
    }

    // test-image.png, centered over splash_picture's own plain background
    // whenever Record is entered without an image chosen — a second
    // overlay child, so the background still shows to the window edge
    // while this one is inset. FILL+expand gives it the whole overlay
    // area; margins inset its own box; CONTAIN fit scales/centers the
    // card inside that. The "NO IMAGE" text is baked into the pixels
    // (render_no_image_placeholder_texture()) rather than a separate
    // overlay label, which visibly jumps around on resize.
    //
    // Margins start at 0 — they track the card's own scaled size via
    // on_activate()'s tick callback (see kNoImageMarginAtNativeScalePx),
    // not a fixed pixel count.
    auto* no_image_picture = Gtk::make_managed<Gtk::Picture>();
    no_image_picture->set_paintable(
        render_no_image_placeholder_texture(*no_image_picture));
    no_image_picture->set_content_fit(Gtk::ContentFit::CONTAIN);
    no_image_picture->set_can_shrink(true);
    no_image_picture->set_hexpand(true);
    no_image_picture->set_vexpand(true);
    no_image_picture->set_halign(Gtk::Align::FILL);
    no_image_picture->set_valign(Gtk::Align::FILL);
    no_image_picture->set_visible(false);
    // Same reasoning as prod_cue/head_direction_cross below: purely a
    // placeholder, never a click target. Without this, its margin-
    // shrunk box sits on top of `picture` and swallows clicks that land
    // within it, while clicks near the edges still reach `picture`
    // underneath.
    no_image_picture->set_can_target(false);
    overlay->add_overlay(*no_image_picture);
    if (out_no_image_picture) {
        *out_no_image_picture = no_image_picture;
    }

    // Playback's marker — the same outline gaze circle as the live cursor
    // (render_gaze_circle_texture()), not a differently-styled dot, so
    // the two read as the same thing. Hidden until Playback's Forward
    // starts moving it; texture/size are set lazily once Playback starts
    // (widget must be realized first, same as make_gaze_cursor()).
    auto* marker = Gtk::make_managed<Gtk::Picture>();
    marker->set_can_shrink(false);
    marker->set_halign(Gtk::Align::START);
    marker->set_valign(Gtk::Align::START);
    marker->set_visible(false);
    overlay->add_overlay(*marker);
    playback->marker = marker;
    playback->tick_host = overlay;
    playback->image = picture;
    playback->no_image_picture = no_image_picture;

    // Prod-event glow (show_prod_cue()) — a separate overlay widget from
    // `marker`, shown briefly and faded rather than tracking the pointer
    // continuously. Shared by Record's live click handler and Playback's
    // ProdEvent dispatch.
    auto* prod_cue = Gtk::make_managed<Gtk::Picture>();
    prod_cue->set_can_shrink(false);
    prod_cue->set_halign(Gtk::Align::START);
    prod_cue->set_valign(Gtk::Align::START);
    prod_cue->set_visible(false);
    // Purely decorative, centered on the click that triggered it — without
    // this it wins hit-testing over `picture`, and the gaze cursor
    // silently reverts to the plain system arrow on every click.
    prod_cue->set_can_target(false);
    overlay->add_overlay(*prod_cue);
    playback->prod_cue = prod_cue;
    if (out_prod_cue) {
        *out_prod_cue = prod_cue;
    }

    // Head-direction cross (render_head_direction_cross_texture()) — a
    // periscope graticule, vertical stroke from +15° to -30° elevation
    // with ticks every 5°, mapped through the actual viewport
    // projection. That range is asymmetric around 0°, so START/START
    // plus explicit per-render margins (not CENTER/CENTER) is what lands
    // elevation 0° at the viewport's true vertical center. Not
    // interactive, same can_target(false) reasoning as `prod_cue` — it
    // would otherwise steal the gaze cursor near the image's middle.
    // Shown/sized/positioned by render_current_view()/
    // render_playback_view(), not here.
    auto* head_direction_cross = Gtk::make_managed<Gtk::Picture>();
    head_direction_cross->set_can_shrink(false);
    head_direction_cross->set_halign(Gtk::Align::START);
    head_direction_cross->set_valign(Gtk::Align::START);
    head_direction_cross->set_visible(false);
    head_direction_cross->set_can_target(false);
    overlay->add_overlay(*head_direction_cross);
    playback->head_direction_cross = head_direction_cross;
    if (out_head_direction_cross) {
        *out_head_direction_cross = head_direction_cross;
    }

    // Subtitles — same .blind-sight-distance-label styling as the
    // "distance at pointer" readout below. Bottom-center, unlike
    // distance_grid which follows the pointer — wrapped and width-capped
    // so a long line reads as a caption, not a banner.
    auto* subtitle_label = Gtk::make_managed<Gtk::Label>();
    subtitle_label->set_halign(Gtk::Align::CENTER);
    subtitle_label->set_valign(Gtk::Align::END);
    subtitle_label->set_margin_bottom(24);
    subtitle_label->set_wrap(true);
    subtitle_label->set_justify(Gtk::Justification::CENTER);
    subtitle_label->set_max_width_chars(60);
    subtitle_label->add_css_class("blind-sight-distance-label");
    subtitle_label->set_visible(false);
    overlay->add_overlay(*subtitle_label);
    playback->subtitle_label = subtitle_label;

    // Point-data readout — a general tracker for whatever data is
    // available at the pointer's target, not specifically depth or
    // "debug." Fixed in the content's bottom-right corner; content still
    // tracks the pointer, only the label's position doesn't. Three rows
    // for now (azimuth, elevation, depth); damping and reflection join
    // once those acoustic cues exist. Hidden with no depth data for the
    // current image, or if the pointer maps outside it.
    //
    // A Gtk::Grid, not one multi-line string: a grid column's width is
    // its widest cell's natural width, so without the explicit
    // set_width_chars() on the value column, values would jitter as
    // their digit counts change.
    auto* distance_grid = Gtk::make_managed<Gtk::Grid>();
    distance_grid->set_halign(Gtk::Align::END);
    distance_grid->set_valign(Gtk::Align::END);
    distance_grid->set_margin(12);
    distance_grid->set_row_spacing(2);
    distance_grid->set_column_spacing(6);
    distance_grid->add_css_class("blind-sight-distance-label");
    distance_grid->set_visible(false);
    overlay->add_overlay(*distance_grid);

    auto add_distance_row = [distance_grid](int row, const Glib::ustring& label_text) {
        auto* label = Gtk::make_managed<Gtk::Label>(label_text);
        label->set_halign(Gtk::Align::END);
        distance_grid->attach(*label, 0, row);
        auto* value = Gtk::make_managed<Gtk::Label>();
        value->set_halign(Gtk::Align::START);
        value->set_width_chars(7);
        distance_grid->attach(*value, 1, row);
        return value;
    };
    Gtk::Label* azimuth_value = add_distance_row(0, _("azimuth:"));
    Gtk::Label* elevation_value = add_distance_row(1, _("elevation:"));
    Gtk::Label* depth_value = add_distance_row(2, _("depth:"));

    // Gaze cursor over the image only — the standard cursor set has
    // nothing circular. Computed on enter, not construction: the widget
    // has no surface to measure against until realized. type:leave is
    // logged (unlike plain outside-window motion) so Playback knows to
    // hide its marker instead of leaving it at the last sampled
    // position; no matching type:enter, since the next type:gaze already
    // implies "back in, here."
    //
    // Saccade-style implicit eye-prod: moving to a new point fires a
    // prod, mirroring fire_head_prod()/notify_pan_input()'s handling of
    // head direction. One-shot — each reset below replaces the last, so
    // it fires once per movement-then-pause. `eye_prod_last_pos` carries
    // the latest position to whichever reset actually fires; declared
    // here since signal_enter()/signal_leave() below also touch it.
    //
    // Suppressed while head movement was recent: arrow-key input should
    // register as head-prod, not eye-prod, and the vestibulo-ocular
    // reflex keeps sighted eyes compensating through head motion without
    // that being a deliberate gaze shift — provisional, since it's
    // unclear this applies the same way for most blind users.
    auto eye_prod_settle_connection = std::make_shared<sigc::connection>();
    auto eye_prod_last_pos =
        std::make_shared<std::pair<std::uint16_t, std::uint16_t>>(0, 0);
    // Where the last eye-prod (click or settle) was. A settle this close to
    // it is the same spot: a hand isn't perfectly still while clicking, and
    // the few pixels it drifts on release would otherwise land a second
    // prod right after the click's own.
    constexpr double kSameSpotPx = 8.0;
    auto last_eye_prod_pos =
        std::make_shared<std::optional<std::pair<std::uint16_t, std::uint16_t>>>();

    // Shared by the explicit mouse-click eye-prod and the implicit
    // settle-triggered one below — consolidates what would otherwise be
    // duplicated at both call sites (show_prod_cue + ProdEvent write +
    // sound trigger), including the depth gate, into one place.
    auto fire_eye_prod = [noise_pulse_engine, settings, session, picture, prod_cue, playback,
                           last_eye_prod_pos](std::uint16_t x, std::uint16_t y) {
        // A click or the pointer settling is Record/Interactive
        // feedback, meaningless and actively confusing while watching a
        // replay.
        if (playback->active) {
            return;
        }
        if (!session_has_depth(*session)) {
            return;
        }
        *last_eye_prod_pos = std::make_pair(x, y);
        show_prod_cue(prod_cue, picture, settings, x, y);
        if (session->file && !session->paused) {
            ProdEvent event;
            event.type = "eye-prod";
            event.t_ms = session->elapsed_ms();
            event.x = x;
            event.y = y;
            write_event(*session->file, event);
        }

        NoisePulseParams pulse_params;
        pulse_params.attack_ms = settings->get_double(kNoisePulseAttackMsKey);
        pulse_params.decay_ms = settings->get_double(kNoisePulseDecayMsKey);
        pulse_params.sustain_level =
            settings->get_double(kNoisePulseSustainLevelKey);
        pulse_params.sustain_ms = settings->get_double(kNoisePulseSustainMsKey);
        pulse_params.release_ms = settings->get_double(kNoisePulseReleaseMsKey);
        pulse_params.color = settings->get_double(kNoisePulseColorKey);
        pulse_params.gain = settings->get_double(kNoisePulseGainKey);

        int grid_n = depth_estimate_grid_n(settings);
        auto distance_m =
            compute_prod_distance_m(*session, *picture, x, y, grid_n);
        // Dry pulse plays immediately, always — confirmation the prod
        // registered, independent of distance reliability. The echo is
        // the same rendered pulse, handled inside the engine (see
        // NoisePulseEngine::trigger()). nullopt from distance_to_delay_ms()
        // means no reliable distance: dry pulse still plays, no echo.
        unsigned int delay_ms = distance_to_delay_ms(distance_m, settings).value_or(0);
        noise_pulse_engine->trigger(pulse_params, delay_ms);
    };

    auto motion = Gtk::EventControllerMotion::create();
    motion->signal_enter().connect(
        [picture, playback, session, eye_prod_last_pos](double x, double y) {
            // The gaze circle is a Record-time aid, meaningless while
            // watching a replay with a real pointer that isn't gaze.
            if (playback->active) {
                return;
            }
            // No depth map, no gaze marker — navigating the image is
            // still fully usable; the pointer just stays the ordinary
            // system cursor.
            if (session_has_depth(*session)) {
                picture->set_cursor(make_gaze_cursor(*picture));
            }
            // Seeds the eye-prod baseline without arming its settle
            // timer: re-entering isn't a deliberate gaze fixation, it's
            // the tester's hand coming back to the mouse. Without this,
            // GTK's just-entered motion sample looked like real movement
            // and could fire a spurious eye-prod moments later. A
            // genuine subsequent movement still arms it normally.
            *eye_prod_last_pos = {quantize_normalized(x, picture->get_width()),
                                   quantize_normalized(y, picture->get_height())};
        });
    motion->signal_leave().connect(
        [picture, session, eye_prod_settle_connection]() {
            picture->set_cursor();
            // Cancels any eye-prod still waiting to settle — without
            // this, a pause cut short by the pointer leaving the window
            // still fired its eye-prod a fixation time later, well after
            // the pointer was gone.
            eye_prod_settle_connection->disconnect();
            if (!session->file || session->paused) {
                return;
            }
            LeaveEvent event;
            event.t_ms = session->elapsed_ms();
            write_event(*session->file, event);
        });
    // type:gaze — motion only, while a recording is open. Position
    // normalized to the widget's own size: with ContentFit::COVER the
    // image fills the widget with no letterboxing, so widget-relative
    // coordinates already are "relative to the current viewport" per
    // GazeEvent's contract.
    motion->signal_motion().connect(
        [picture, session, distance_grid, azimuth_value, elevation_value,
         depth_value, settings, eye_prod_settle_connection,
         eye_prod_last_pos, last_head_movement_at, last_eye_prod_pos,
         fire_eye_prod](double x, double y) {
            std::uint16_t qx = quantize_normalized(x, picture->get_width());
            std::uint16_t qy = quantize_normalized(y, picture->get_height());
            if (session->file && !session->paused) {
                GazeEvent event;
                event.t_ms = session->elapsed_ms();
                event.x = qx;
                event.y = qy;
                write_event(*session->file, event);
            }
            // Only a *real* position change resets the settle timer.
            // Arrow-key panning re-renders splash_picture every press via
            // render_current_view(), and swapping its paintable while the
            // pointer sits still apparently makes GTK redeliver a motion
            // event at the same position — without this check that
            // registered as eye movement instead of head movement.
            // Exact-equality, not a tolerance: a synthetic redelivery
            // reports literally the same coordinates, since the pointer
            // itself never moved.
            bool moved = qx != eye_prod_last_pos->first || qy != eye_prod_last_pos->second;
            *eye_prod_last_pos = {qx, qy};
            if (moved) {
                // Read per arm, so a change in Preferences applies at once.
                auto settle_ms = static_cast<unsigned int>(settings->get_int("pointer-fixation-ms"));
                eye_prod_settle_connection->disconnect();
                *eye_prod_settle_connection = Glib::signal_timeout().connect(
                    [picture, eye_prod_last_pos, last_head_movement_at, last_eye_prod_pos,
                     fire_eye_prod, settle_ms]() {
                        // Recent head movement wins — see this handler's
                        // own comment above for why. "Recent" is the same
                        // window as the settle delay itself: any head
                        // movement since this particular pause started is
                        // enough to explain the whole pause as VOR
                        // tracking, not a deliberate eye saccade.
                        auto since_head_movement =
                            std::chrono::steady_clock::now() - *last_head_movement_at;
                        if (since_head_movement <
                            std::chrono::milliseconds(settle_ms)) {
                            return false;  // one-shot, suppressed
                        }
                        if (*last_eye_prod_pos) {
                            double dx = (static_cast<double>(eye_prod_last_pos->first) -
                                         (*last_eye_prod_pos)->first) /
                                        65535.0 * picture->get_width();
                            double dy = (static_cast<double>(eye_prod_last_pos->second) -
                                         (*last_eye_prod_pos)->second) /
                                        65535.0 * picture->get_height();
                            if (std::hypot(dx, dy) <= kSameSpotPx) {
                                return false;  // one-shot, same spot as the last prod
                            }
                        }
                        // "eye-prod" — see ProdEvent's own comment
                        // (recording.hpp) and the mouse-click handler
                        // below, which fires the same subtype explicitly.
                        // fire_eye_prod() itself handles the no-depth
                        // gate — session_has_depth()'s own comment.
                        fire_eye_prod(eye_prod_last_pos->first,
                                      eye_prod_last_pos->second);
                        return false;  // one-shot
                    },
                    settle_ms);
            }

            // "Distance at pointer" — session's depth cache (populated by
            // load_image()) is already right here. Equirect/cylindrical
            // only (viewing_equirect) — a flat image's widget-to-source-
            // pixel mapping is a plain COVER-fit crop/scale, not this
            // yaw/pitch ray projection, and isn't implemented yet. Must
            // match whatever projection render_current_view() actually
            // rendered with, or this readout points at the wrong source
            // pixel for a cylindrical panorama.
            if (!settings->get_boolean(kShowDepthAtPointerKey) ||
                !session->viewing_equirect || session->depth_width == 0) {
                distance_grid->set_visible(false);
                return;
            }
            double fov_rad = kViewportFovDegrees * std::numbers::pi / 180.0;
            double yaw_rad = session->view_yaw / 100.0 * std::numbers::pi / 180.0;
            double pitch_rad =
                session->view_pitch / 100.0 * std::numbers::pi / 180.0;
            double tan_half_fov = std::tan(fov_rad / 2.0);
            double aspect = static_cast<double>(
                                 std::max(picture->get_width(), 1)) /
                             std::max(picture->get_height(), 1);
            double u, v, lon, lat;
            PanoramaProjection proj = resolve_panorama_projection(
                session->panorama_projection,
                session->equirect_source ? session->equirect_source->get_width() : 1,
                session->equirect_source ? session->equirect_source->get_height() : 1);
            equirect_pixel_uv(static_cast<int>(x), static_cast<int>(y),
                               picture->get_width(), picture->get_height(),
                               tan_half_fov, aspect, std::cos(yaw_rad),
                               std::sin(yaw_rad), std::cos(pitch_rad),
                               std::sin(pitch_rad), proj, u, v, &lon, &lat);
            int dx = static_cast<int>(u * session->depth_width) %
                     session->depth_width;
            if (dx < 0) {
                dx += session->depth_width;
            }
            int dy = std::clamp(static_cast<int>(v * session->depth_height),
                                 0, session->depth_height - 1);
            float depth =
                session->depth_data[static_cast<std::size_t>(dy) *
                                         static_cast<std::size_t>(
                                             session->depth_width) +
                                     static_cast<std::size_t>(dx)];
            // The ray's true world-frame angle (lon/lat), NOT inverted
            // from u/v: u/v are texture coordinates within the source
            // image, which for a crop or the cylindrical tan() mapping
            // are no longer a simple linear rescale of lat/lon —
            // inverting them as if they still were gave nonsense angles.
            double azimuth_deg = lon * 180.0 / std::numbers::pi;
            double elevation_deg = lat * 180.0 / std::numbers::pi;
            // Reuses raw_depth_to_distance_m() directly — the same k=10
            // scale the echo delay itself is calculated from — so this
            // readout shows the exact number driving the delay. Still
            // relative/ordinal, not a verified real-world distance;
            // labelled "depth", not "distance".
            azimuth_value->set_text(Glib::ustring::compose(
                "%1°", Glib::ustring::format(std::fixed, std::setprecision(1),
                                             azimuth_deg)));
            elevation_value->set_text(Glib::ustring::compose(
                "%1°", Glib::ustring::format(std::fixed, std::setprecision(1),
                                             elevation_deg)));
            depth_value->set_text(Glib::ustring::format(
                std::fixed, std::setprecision(2), raw_depth_to_distance_m(depth)));
            distance_grid->set_visible(true);
        });
    picture->add_controller(motion);

    // type:prod — button-down, not full click-and-release —
    // signal_pressed() fires immediately on press, matching "button down".
    auto click = Gtk::GestureClick::create();
    click->signal_pressed().connect(
        [picture, fire_eye_prod, eye_prod_settle_connection,
         eye_prod_last_pos](int, double x, double y) {
            std::uint16_t qx = quantize_normalized(x, picture->get_width());
            std::uint16_t qy = quantize_normalized(y, picture->get_height());
            // Cancels any eye-prod still waiting to settle and seeds its
            // baseline at the click's position: a real hand isn't
            // perfectly still while clicking, so GTK's own motion sample
            // around a click often registers as genuine movement to the
            // settle timer above — left alone, one physical click could
            // produce two prods.
            eye_prod_settle_connection->disconnect();
            *eye_prod_last_pos = {qx, qy};
            // The glow and pulse (mouse click stands in for a real
            // blink-triggered prod) show/sound regardless of whether a
            // recording is open — feedback the click registered,
            // independent of whether it's also being logged.
            fire_eye_prod(qx, qy);
        });
    picture->add_controller(click);

    auto* text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    text->set_halign(Gtk::Align::CENTER);
    text->set_valign(Gtk::Align::CENTER);
    text->add_css_class("blind-sight-splash-caption");
    // Margin: gap between the card and the window/overlay edge. Padding
    // (inside the card, between its edge and the text) is separate and
    // lives in the CSS class above — the two were conflated before.
    text->set_margin(16);

    auto* title = Gtk::make_managed<Gtk::Label>(_("Blind Sight"));
    title->add_css_class("title-1");

    auto* tagline = Gtk::make_managed<Gtk::Label>(
        _("Echolocation for blind and visually impaired people"));

    // A demo/test rig for verifying the acoustic model works, not the
    // sensory-substitution product itself. Styled like `copyright`
    // below, not `tagline` above — a clarifying aside, not a headline.
    auto* subtitle = Gtk::make_managed<Gtk::Label>(
        _("Test rig for functional verification"));
    subtitle->add_css_class("caption");

    auto* copyright = Gtk::make_managed<Gtk::Label>(
        _("© John Erling Blad, 2026"));
    copyright->add_css_class("caption");
    copyright->add_css_class("blind-sight-splash-copyright");

    for (auto* line : {title, tagline, subtitle, copyright}) {
        line->set_halign(Gtk::Align::CENTER);
        line->set_justify(Gtk::Justification::CENTER);
        text->append(*line);
    }

    overlay->add_overlay(*text);
    if (out_caption) {
        *out_caption = text;
    }
    return overlay;
}

// translator_credits uses the gettext convention — the msgid is
// literally "translator-credits", and each .po file's *translation* of
// it is that language's translator name(s) (po/nb.po).
void show_about(Gtk::Window* parent) {
    auto* about = ADW_ABOUT_DIALOG(adw_about_dialog_new());
    adw_about_dialog_set_application_name(about, _("Blind Sight"));
    // Registered as a symbolic icon name (register_app_icons()), not a
    // raw file load, so it gets the same automatic light/dark
    // recoloring as every stock -symbolic icon elsewhere.
    adw_about_dialog_set_application_icon(about, "blind-sight-bat-symbolic");
    adw_about_dialog_set_version(about, BLIND_SIGHT_VERSION);
    adw_about_dialog_set_developer_name(about, "John Erling Blad");

    const char* developers[] = {"John Erling Blad", nullptr};
    adw_about_dialog_set_developers(about, developers);

    adw_about_dialog_set_translator_credits(about, _("translator-credits"));
    adw_about_dialog_set_copyright(about, "© 2026 John Erling Blad");

    // GTK_LICENSE_GPL_3_0 only labels the license family — the enum has
    // no -only vs -or-later distinction; LICENSE and the SPDX headers
    // carry that, not this dialog's badge + "Read License" link.
    adw_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_website(about, "https://github.com/jeblad/blind-sight-lib");
    adw_about_dialog_set_issue_url(
        about, "https://github.com/jeblad/blind-sight-lib/issues");

    adw_dialog_present(ADW_DIALOG(about), GTK_WIDGET(parent->gobj()));
}

// GtkShortcutsWindow is deprecated as of GTK 4.18 with no official
// replacement widget, so this is plain AdwActionRows in an
// AdwPreferencesDialog, same pattern as show_preferences() — a key combo
// in the row's subtitle, kept deliberately simple.
void show_shortcuts(Gtk::Window* parent) {
    auto* shortcuts = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    auto* page = adw_preferences_page_new();

    auto* viewport_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(viewport_group, _("Viewport"));
    auto* look_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(look_row),
                                   _("Look around"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(look_row), "← → ↑ ↓");
    adw_preferences_group_add(viewport_group, GTK_WIDGET(look_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), viewport_group);

    // Steps through image-sequence (page_navigate in on_activate(),
    // also driven by nav_prev_button/nav_next_button, in Interactive and
    // Record). A no-op in Playback.
    auto* images_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(images_group, _("Images"));
    auto* next_image_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(next_image_row),
                                   _("Next Image"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(next_image_row), "Page Down");
    adw_preferences_group_add(images_group, GTK_WIDGET(next_image_row));
    auto* previous_image_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(previous_image_row),
                                   _("Previous Image"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(previous_image_row), "Page Up");
    adw_preferences_group_add(images_group, GTK_WIDGET(previous_image_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), images_group);

    auto* window_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(window_group, _("Window"));
    auto* fullscreen_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fullscreen_row),
                                   _("Toggle Fullscreen"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(fullscreen_row), "F11");
    adw_preferences_group_add(window_group, GTK_WIDGET(fullscreen_row));
    auto* exit_fullscreen_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(exit_fullscreen_row),
                                   _("Exit Fullscreen"));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(exit_fullscreen_row), "Esc");
    adw_preferences_group_add(window_group, GTK_WIDGET(exit_fullscreen_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), window_group);

    adw_preferences_dialog_add(shortcuts, ADW_PREFERENCES_PAGE(page));
    adw_dialog_present(ADW_DIALOG(shortcuts), GTK_WIDGET(parent->gobj()));
}

// Record/Playback are one-shot actions that surface a transport-control
// action bar in the header bar; each action bar carries its own X to
// dismiss it and return to the quiet default where neither is active —
// GNOME's usual pattern for this kind of transient contextual state.
// Play/Rec are physically removed from the header bar the moment either
// action bar is showing (see the three-state rework in on_activate()),
// so the two can never be triggered at once.

// Forward declarations — add_playback_controls()/add_record_controls()
// below build their own session open/dropdown/details UI directly,
// each action bar owning its own file-open flow rather than a separate
// function showing a dialog before the action bar even exists. Called
// before they're defined further down the file.
void show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                        const Glib::ustring& body);

// Shown at most once per run — depth generation is
// triggered automatically (see maybe_generate_depth_on_the_fly() below),
// so telling the user to run setup-depth-env.sh needs to happen once, not
// every single time a depth-less image gets opened.
bool g_shown_depth_setup_dialog = false;

// Paths currently being auto-generated, so opening the same image twice
// in quick succession (or switching away and back before a ~9s generation
// finishes) doesn't launch a second subprocess on top of the first.
std::set<std::string> g_depth_generation_in_progress;

// Depth maps are generated automatically, not via a manual menu entry:
// called from load_image() for any image without a depth file. Async
// via Gio::Subprocess::wait_async() plus a toast, not a blocking call
// (measured ~9s against a real 8704x4352 photosphere on CPU alone —
// blocking the main loop that long would freeze the window). The
// subprocess keeps running to completion regardless of what the user
// does meanwhile; `session`'s depth cache is only adopted afterward if
// still looking at the same image, but the file itself is written
// either way.
void maybe_generate_depth_on_the_fly(Gtk::Window* parent,
                                      const std::shared_ptr<RecordingSession>& session,
                                      AdwToastOverlay* toast_overlay,
                                      const Glib::RefPtr<Gio::Settings>& settings,
                                      const std::string& path) {
    // Off by default. Turning this on is also what prefetches the model
    // (depth_group's switch row in show_preferences(),
    // start_depth_model_prefetch()), so by the time this setting can be
    // true, the model already is too — no separate "not downloaded yet"
    // path needed.
    if (!settings->get_boolean(kDepthGenerationEnabledKey)) {
        return;
    }
    if (path.empty() || has_depth_file(path) ||
        g_depth_generation_in_progress.count(path) > 0) {
        return;
    }
    if (!depth_generation_available()) {
        if (!g_shown_depth_setup_dialog) {
            g_shown_depth_setup_dialog = true;
            show_setup_required_dialog(
                parent, _("Depth Generation Not Set Up"),
                _("Run this from a terminal — images will then get a "
                  "depth map automatically when opened."),
                "demo/depth/setup-depth-env.sh");
        }
        return;
    }
    std::string model_size = settings->get_string(kDepthModelSizeKey).raw();
    g_depth_generation_in_progress.insert(path);
    AdwToast* generating_toast = adw_toast_new(_("Generating depth map…"));
    adw_toast_set_timeout(generating_toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, generating_toast);
    try {
        std::vector<std::string> argv = {
            depth_venv_dir() + "/bin/python3",
            std::string(BLIND_SIGHT_DEPTH_DIR) + "/generate_depth.py", path,
            "--model-size", model_size};
        // Opportunistic HF_TOKEN — Depth-Anything-V2 isn't gated, but
        // sending a token already in the keyring is a free courtesy to
        // HuggingFace's rate limits.
        auto opportunistic_token = lookup_huggingface_token();
        Glib::RefPtr<Gio::Subprocess> proc;
        if (opportunistic_token) {
            auto launcher =
                Gio::SubprocessLauncher::create(Gio::Subprocess::Flags::NONE);
            launcher->setenv("HF_TOKEN", *opportunistic_token, true);
            proc = launcher->spawn(argv);
        } else {
            proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        }
        proc->wait_async(
            [proc, generating_toast, session,
             path](Glib::RefPtr<Gio::AsyncResult>& result) {
                proc->wait_finish(result);
                if (!proc->get_successful()) {
                    g_message("blind-sight: depth generation failed for %s",
                               path.c_str());
                }
                g_depth_generation_in_progress.erase(path);
                adw_toast_dismiss(generating_toast);
                // Only adopt it into the session's live depth cache if
                // still looking at this same image — switching back to
                // it later picks it up normally regardless.
                if (session->current_image_path == path && has_depth_file(path)) {
                    load_depth_exr(depth_file_path_for(path), session->depth_data,
                                    session->depth_width, session->depth_height);
                }
            });
    } catch (const Glib::Error& e) {
        // Silent beyond the log — this runs automatically in the
        // background, so a modal dialog here would be a surprise
        // interruption rather than a response to a user action.
        g_depth_generation_in_progress.erase(path);
        adw_toast_dismiss(generating_toast);
        g_message("blind-sight: failed to launch depth generation for %s: %s",
                   path.c_str(), e.what());
    }
}

// The track dropdown's own two per-row lookups, merged into one scan
// over the same [track_start_ms, track_end_ms) range. `path` is the
// first ImageEvent::path in range regardless of scenario, while
// `scenario` is the first *non-empty* ImageEvent::scenario in range,
// since a track's image may not be recorded against a protocol at all
// — not necessarily the same event, so both are tracked independently.
struct TrackImageInfo {
    std::string path;
    std::string scenario;
};

TrackImageInfo image_info_for_track(const std::vector<blind_sight::RecordEvent>& events,
                                      std::int32_t track_start_ms,
                                      std::int32_t track_end_ms) {
    TrackImageInfo info;
    bool have_path = false;
    for (const auto& event : events) {
        auto* image = std::get_if<ImageEvent>(&event);
        if (!image || image->t_ms < track_start_ms || image->t_ms >= track_end_ms) {
            continue;
        }
        if (!have_path) {
            info.path = image->path;
            have_path = true;
        }
        if (info.scenario.empty() && !image->scenario.empty()) {
            info.scenario = image->scenario;
        }
        if (have_path && !info.scenario.empty()) {
            break;
        }
    }
    return info;
}

// The earliest t_ms at which `image_path` appears as an ImageEvent in
// `events` — std::nullopt if it never does. Used by the image dropdown
// to turn "select this image" into a seek during Playback: the image
// shows as a consequence of jumping to the track that plays it, rather
// than writing to playback_session directly.
std::optional<std::int32_t> find_first_image_event_ms(
    const std::vector<blind_sight::RecordEvent>& events,
    const std::string& image_path) {
    for (const auto& event : events) {
        if (auto* image = std::get_if<ImageEvent>(&event)) {
            if (image->path == image_path) {
                return image->t_ms;
            }
        }
    }
    return std::nullopt;
}

// Tears down the current track's capture pipeline (stop_audio_track())
// and, if kTranscodeAudioToOpusKey is on (the default), immediately
// transcodes the WAV segment it just finished writing: measured against
// a live 47s recording, encoding takes ~0.1s (~470x realtime) — fast
// enough that a separate manual "Encode Audio to Opus" step makes no
// sense.
//
// A thin wrapper around stop_audio_track() rather than folded into it:
// that function's job is pipeline teardown + the MicrophoneEvent log
// entry, with no reason to know about GSettings or the toast overlay.
// `audio_segment` has already been incremented past the finalized
// track by the matching start_audio_track() call, hence `- 1`.
void stop_audio_track_and_encode(Gtk::Window* window,
                                  const std::shared_ptr<RecordingSession>& session,
                                  const Glib::RefPtr<Gio::Settings>& settings,
                                  AdwToastOverlay* toast_overlay) {
    if (!session->audio_pipeline) {
        return;
    }
    std::string wav_path = track_audio_path_for_id(
        session->session_path, session->current_track_id, session->audio_segment - 1);
    stop_audio_track(session);

    // Off means no transcoding happens, the WAV is left as-is.
    // Transcription (WhisperX) is a separate concern — still runs
    // either way, straight off the WAV since there's no encode step to
    // chain after here.
    if (!settings->get_boolean(kTranscodeAudioToOpusKey)) {
        maybe_transcribe_segment(window, settings, toast_overlay, wav_path,
                                  session->session_path, session->current_track);
        return;
    }

    // Genuinely async, so the toast can't lean on "the whole app is
    // blocked anyway" to stay visible — it needs explicit minimum-
    // visible-time bookkeeping: 0 (never auto-dismiss) timeout,
    // dismissed by hand once the encode completes and at least 1s has
    // elapsed since shown, so a near-instant encode still reads as a
    // real step rather than a flicker.
    AdwToast* toast = adw_toast_new(_("Transcoding to Opus…"));
    adw_toast_set_timeout(toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, toast);
    auto toast_shown_at = std::chrono::steady_clock::now();

    std::string opus_path = track_audio_opus_path_for_id(
        session->session_path, session->current_track_id, session->audio_segment - 1);
    std::string session_path_for_meta = session->session_path;
    int track_number_for_meta = session->current_track;
    encode_wav_to_opus_async(
        wav_path, opus_path,
        [window, settings, toast, toast_shown_at, toast_overlay, wav_path,
         opus_path, session_path_for_meta, track_number_for_meta](bool ok) {
            if (ok) {
                // Unconditional — there's no separate "keep WAV too"
                // choice: transcoding to Opus succeeding is what
                // discards the WAV.
                std::remove(wav_path.c_str());
            } else {
                // Never lose the only copy — WAV stays.
                g_message("blind-sight: auto-encode to Opus failed for %s",
                           wav_path.c_str());
                AdwToast* failed_toast =
                    adw_toast_new(_("Could not transcode to Opus — kept as WAV"));
                adw_toast_overlay_add_toast(toast_overlay, failed_toast);
            }
            // Chained here, not launched in parallel with the encode
            // above. Whichever file actually exists now gets transcribed.
            maybe_transcribe_segment(window, settings, toast_overlay,
                                      ok ? opus_path : wav_path,
                                      session_path_for_meta, track_number_for_meta);
            auto elapsed = std::chrono::steady_clock::now() - toast_shown_at;
            auto remaining = std::chrono::seconds(1) - elapsed;
            if (remaining <= std::chrono::steady_clock::duration::zero()) {
                adw_toast_dismiss(toast);
                return;
            }
            auto remaining_ms = static_cast<unsigned int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                    .count());
            Glib::signal_timeout().connect(
                [toast]() {
                    adw_toast_dismiss(toast);
                    return false;  // one-shot
                },
                remaining_ms);
        });
}

// Mints a stable id for the track `session->current_track` just became,
// creates its directory, and records it in that session's tracks.json —
// called from both places a new track actually starts (the Forward
// handler and write_implicit_track_change(), below), never from
// start_audio_track() itself, which also runs for a plain same-track mic
// re-enable and must not mint anything then. `order` defaults equal to
// `track_number`'s own 0-based position — the Edit Tracks dialog's own
// reorder (track_dialogs.cpp) is what makes them diverge later.
void mint_new_track(const std::shared_ptr<RecordingSession>& session) {
    std::string session_dir = Glib::path_get_dirname(session->session_path);
    session->current_track_id = generate_track_id(session_dir);
    std::filesystem::create_directories(session_dir + "/tracks/" + session->current_track_id);
    TracksIndex index = read_tracks_index(session_dir);
    TrackEntry entry;
    entry.id = session->current_track_id;
    entry.track_number = session->current_track;
    entry.order = session->current_track - 1;
    // sink_device_assignment/source_device_assignment are each shared by
    // participants and their own sentinel id (Speaker, ambient mic) —
    // split back apart here, since TrackEntry's own maps are keyed by
    // participant name only. Only the display name is snapshotted
    // (diagnostic only, see TrackEntry's own comment), not the stable
    // device id.
    for (const auto& [id, assignment] : session->monitoring.sink_device_assignment) {
        if (id != kSpeakerSinkId) {
            entry.participant_devices[id] = assignment.display_name;
        }
    }
    for (const auto& [id, assignment] : session->monitoring.source_device_assignment) {
        if (id != kAmbientMicSourceId) {
            entry.participant_mic_devices[id] = assignment.display_name;
        }
    }
    auto ambient_it = session->monitoring.source_device_assignment.find(kAmbientMicSourceId);
    entry.ambient_mic_device = ambient_it != session->monitoring.source_device_assignment.end()
                                    ? ambient_it->second.display_name
                                    : "";
    index.tracks.push_back(entry);
    write_tracks_index(session_dir, index);
}

// A track boundary without a real pause — used when the Record bar's
// forward/backward image-navigation buttons change the image while a
// recording is already running, implicitly creating a new track. Same
// WaitEvent-based boundary a real Stop+Forward writes
// (see forward_button's own handler in add_record_controls()), except
// the file never actually closes, so there's no wall-clock gap to
// measure — duration_ms is unconditionally 0. A new track also means a
// new audio file, same one-WAV-per-track
// convention as everywhere else — so the ending track's pipeline is
// closed out first, current_track still pointing at it, before the new
// one starts.
void write_implicit_track_change(Gtk::Window* window,
                                  const std::shared_ptr<RecordingSession>& session,
                                  const Glib::RefPtr<Gio::Settings>& settings,
                                  AdwToastOverlay* toast_overlay) {
    if (session->microphone_on) {
        stop_audio_track_and_encode(window, session, settings, toast_overlay);
    }

    // Same double-counting guard as stop_recording_out — if paused,
    // segment_start is stale (already folded in once) and shouldn't be
    // folded in again.
    if (!session->paused) {
        session->accumulated_ms += static_cast<std::int32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - session->segment_start)
                .count());
    }

    WaitEvent wait_event;
    wait_event.t_ms = session->accumulated_ms;
    wait_event.duration_ms = 0;
    write_event(*session->file, wait_event);
    ++session->current_track;
    mint_new_track(session);

    session->segment_start = std::chrono::steady_clock::now();
    session->audio_segment = 0;
    if (session->microphone_on) {
        start_audio_track(
            session, track_audio_path_for_id(session->session_path,
                                              session->current_track_id,
                                              session->audio_segment));
        ++session->audio_segment;
    }
}

// resolve_track_audio_path()'s own read-time lookup, extended to also
// try a track's real per-id directory first — a session recorded since
// this feature shipped has a tracks.json entry (non-empty id) for
// `track_n`; a legacy track, or one never touched since, doesn't, and
// falls straight back to the flat "recording.trackN.*" naming exactly as
// before. Read once, not cached — tracks.json rarely changes, and this
// only runs when playback actually switches segments, not every frame.
std::string resolve_track_audio_path_any(const std::string& session_path, int track_n,
                                          int segment_n) {
    std::string session_dir = Glib::path_get_dirname(session_path);
    for (const auto& entry : read_tracks_index(session_dir).tracks) {
        if (entry.track_number == track_n && !entry.id.empty()) {
            return resolve_track_audio_path_for_id(session_path, entry.id, segment_n);
        }
    }
    return resolve_track_audio_path(session_path, track_n, segment_n);
}

// Playback's counterpart to start_audio_track()/stop_audio_track() —
// plays a track's recorded WAV file back instead of capturing one.
// `playbin` with an explicit pipewiresink, mirroring pipewiresrc on the
// capture side — playbin's state/seek handling is simpler to get right
// for playback than a raw filesrc!wavparse chain. Direct PLAYING ->
// NULL teardown, no EOS wait — verified (throwaway harness, real
// hardware) as hang-free, including mid-PLAYING seeks and repeated
// create/teardown cycles simulating track switches.
void stop_playback_audio(const std::shared_ptr<PlaybackSession>& playback) {
    if (!playback->audio_pipeline) {
        return;
    }
    gst_element_set_state(playback->audio_pipeline, GST_STATE_NULL);
    gst_object_unref(playback->audio_pipeline);
    playback->audio_pipeline = nullptr;
    playback->current_audio_segment = -1;
}

// Keeps audio in step with the visual replay. Called from
// update_active_buttons(), which already runs at exactly the moments
// playback state changes, so no extra per-frame wiring is needed. FF/FB
// rate-seek the same pipeline instead of going silent (GStreamer's own
// rate-seek, not sample-dropping). A moment with no recorded mic-on
// segment is a silent no-op — current_audio_segment is still updated
// (to -1) so this isn't retried every call.
void sync_playback_audio(const std::shared_ptr<PlaybackSession>& playback) {
    if (!playback->speaker_on || !playback->tick_active) {
        stop_playback_audio(playback);
        return;
    }
    auto ms = playback->elapsed_ms();
    const auto* segment = audio_segment_for_ms(playback->audio_segments, ms);
    int found = segment ? static_cast<int>(segment - playback->audio_segments.data())
                         : -1;
    if (playback->current_audio_segment == found &&
        playback->current_audio_rate == playback->speed) {
        return;  // already playing the right thing at the right rate
    }
    stop_playback_audio(playback);
    if (!segment) {
        playback->current_audio_segment = -1;
        return;
    }
    std::string path = resolve_track_audio_path_any(playback->session_path, segment->track_n,
                                                      segment->segment_n);
    if (path.empty()) {
        playback->current_audio_segment = found;
        return;
    }
    auto* pipeline = gst_element_factory_make("playbin", nullptr);
    auto* sink = gst_element_factory_make("pipewiresink", nullptr);
    g_object_set(pipeline, "audio-sink", sink, nullptr);
    gchar* uri = gst_filename_to_uri(path.c_str(), nullptr);
    g_object_set(pipeline, "uri", uri, nullptr);
    g_free(uri);
    // PAUSED (and settled, via get_state()) before the seek — seeking an
    // element that hasn't yet negotiated duration/position is
    // unreliable.
    //
    // Bounded, not GST_CLOCK_TIME_NONE — a real hang showed this is
    // necessary: the infinite wait was only ever verified against WAV;
    // the day Playback first hit a freshly auto-encoded .opus segment,
    // playbin had to auto-plug an Ogg/Opus chain it had never exercised
    // before, and an infinite wait on the GTK main thread means any
    // negotiation stall freezes the app with no way back short of
    // kill -9. 5s is generous for local-file negotiation — skip audio
    // for this segment rather than hang.
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    GstStateChangeReturn state_ret =
        gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
    if (state_ret == GST_STATE_CHANGE_FAILURE ||
        state_ret == GST_STATE_CHANGE_ASYNC) {
        g_message("blind-sight: playback audio pipeline for %s didn't reach "
                   "PAUSED (result %d) — skipping audio for this segment",
                   path.c_str(), state_ret);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        playback->current_audio_segment = found;
        return;
    }
    std::int32_t offset_ms = std::max(0, ms - segment->start_ms);
    gint64 offset_ns = static_cast<gint64>(offset_ms) * GST_MSECOND;
    if (playback->speed >= 0) {
        // Forward, 1x or FF — play from offset_ns to the end of this
        // segment's own file at the given rate. GST_CLOCK_TIME_NONE as
        // the stop position is GStreamer's own "to the natural end."
        gst_element_seek(
            pipeline, playback->speed, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
            GST_SEEK_TYPE_SET, offset_ns, GST_SEEK_TYPE_SET, GST_CLOCK_TIME_NONE);
    } else {
        // FB (reverse) — a negative rate plays backward from `stop`
        // toward `start`, so the segment to play is [0, offset_ns] with
        // offset_ns as the stop position, not the start.
        gst_element_seek(
            pipeline, playback->speed, GST_FORMAT_TIME,
            static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
            GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_SET, offset_ns);
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    playback->audio_pipeline = pipeline;
    playback->current_audio_segment = found;
    playback->current_audio_rate = playback->speed;
}

// Forward (play) + Stop — full transport (fast-forward/backward, seek)
// is separate follow-up work; this is enough to prove the
// dialog-then-controls flow.
//
// Forward always restarts from the beginning; Stop is a genuine stop,
// not a pause (see PlaybackSession's comment). While running, a 100ms
// tick walks forward through playback->events, applying each one whose
// t_ms has been reached: gaze/prod move the marker over the image;
// type:wait halts the tick until another Forward press — a stand-in for
// the fast-forward "jump past the gap" behaviour in notes.md, not built
// yet.
void add_playback_controls(
    Gtk::Window* window, GtkWidget* header_bar,
    std::shared_ptr<PlaybackSession> playback,
    const Glib::RefPtr<Gio::Settings>& settings,
    std::shared_ptr<Protocol> active_protocol,
    std::shared_ptr<std::function<void(bool)>> page_navigate,
    std::function<void(bool)> default_page_navigate,
    std::shared_ptr<std::function<void()>> close_self,
    std::shared_ptr<std::function<void(std::int32_t)>> playback_seek_to,
    std::function<void(const std::string&)> update_sequence_button_label,
    Gtk::Button* forward_button, Gtk::Button* stop_button,
    Gtk::Button* fast_forward_button, Gtk::Button* fast_backward_button,
    Gtk::EditableLabel* clock_label,
    std::shared_ptr<sigc::connection> wait_resume_connection) {
    playback->active = true;
    // In case the pointer is already sitting over the image right now —
    // otherwise this would wait for the next enter event.
    playback->image->set_cursor();

    // Starting state only — mirrors Record's own microphone_on init;
    // the speaker icon below can still toggle it for the rest of this
    // session.
    playback->speaker_on = settings->get_boolean(kSpeakerInitiallyOnKey);

    // clock_label and the transport buttons are shared, permanently-
    // visible widgets built once in on_activate() and passed in, not
    // constructed here. FF/FB stay inert placeholders — still
    // referenced below for their active-speed CSS highlight, just not
    // wired to a click handler by this function.

    // format_clock/place_marker/restore_viewport/seek_to/start_ticking
    // are all built and driven from on_activate(), since FF/FB need to
    // scrub from Interactive as well as Play.

    // Next/Previous, now that seek_to() exists (overrides the no-op set
    // above) — steps to the next/previous *track* within this
    // recording, the same WaitEvent-delimited unit the track dropdown
    // jumps between. Finds the track the read head is currently inside,
    // then seeks to the adjacent one, clamped rather than wrapping. A
    // track doesn't necessarily carry its own ImageEvent, so this
    // doesn't guarantee a different picture shows afterward —
    // restore_viewport()'s track-boundary rule handles that either way.
    *page_navigate = [playback, playback_seek_to](bool forward) {
        auto now_ms = playback->elapsed_ms();
        std::size_t current = 0;
        for (std::size_t i = 0; i < playback->track_starts.size(); ++i) {
            if (playback->track_starts[i] <= now_ms) {
                current = i;
            } else {
                break;
            }
        }
        if (forward && current + 1 < playback->track_starts.size()) {
            (*playback_seek_to)(playback->track_starts[current + 1]);
        } else if (!forward && current > 0) {
            (*playback_seek_to)(playback->track_starts[current - 1]);
        }
    };

    // update_active_buttons(), start_ticking(), the Play pause/resume
    // toggle, and the Stop/reset logic all live in on_activate() —
    // nothing left for this function to assign for any of them.
    bool has_shared_session = !playback->session_path.empty();
    forward_button->set_sensitive(has_shared_session);
    fast_forward_button->set_sensitive(has_shared_session);
    fast_backward_button->set_sensitive(has_shared_session);

    // track_button/track_list/track_popover/clock_label are all built
    // and wired once, permanently, in on_activate() — not constructed/
    // grouped here per Play-press.

    // session_recent_button is a single, permanently-packed widget in
    // on_activate(), shared with Record, never built or torn down here.
    // playback_bar_start/_end are left empty now that the transport
    // buttons, clock, mic/speaker/protocol are all shared/permanent —
    // kept as a structural placeholder for close_self's pack/remove.
    auto* playback_bar_start =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* playback_bar_end =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

    // The actual teardown, reusable — on_activate()'s mode-switch click
    // handlers call this via close_self whenever the user switches away
    // from Playback to any other mode, not just back to Interactive.
    *close_self = [header_bar, playback_bar_start, playback_bar_end, playback,
                    wait_resume_connection, page_navigate,
                    default_page_navigate]() {
        // Without this, a pending "Wait n" auto-continue timeout could
        // fire after this action bar — and every widget its lambda
        // captures by raw pointer — is gone, a real use-after-free.
        wait_resume_connection->disconnect();
        stop_playback_audio(playback);
        if (playback->tick_active) {
            playback->tick_host->remove_tick_callback(playback->tick_id);
            playback->tick_active = false;
        }
        if (playback->marker) {
            playback->marker->set_visible(false);
        }
        if (playback->subtitle_label) {
            playback->subtitle_label->set_visible(false);
        }
        playback->active = false;
        // Same immediate-update reasoning as setting it on open: don't
        // wait for a fresh enter event if the pointer is already there.
        playback->image->set_cursor(make_gaze_cursor(*playback->image));
        *page_navigate = default_page_navigate;
        adw_header_bar_remove(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(playback_bar_start->gobj()));
        adw_header_bar_remove(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(playback_bar_end->gobj()));
    };

    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(playback_bar_start->gobj()));
    // pack_end, not pack_start: the menu button (also pack_end, added
    // first) sits at the true edge, and each later pack_end lands closer
    // to the center, landing this immediately to its left.
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(playback_bar_end->gobj()));
}

// Modal "this can't proceed, here's why" message — the narrow case this
// demo has is an unreadable/unparsable/wrong-version Playback file.
// AdwAlertDialog, not a raw GtkMessageDialog (deprecated).
void show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                        const Glib::ustring& body) {
    auto* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", _("OK"));
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent->gobj()));
}

// Same modal shape as show_error_dialog() above, for "not set up"
// dialogs that name a command/path the user needs to copy.
// AdwAlertDialog's body has no public "selectable" setter, so the
// copyable part goes in `extra_child` instead, a real supported
// extension point. `.monospace` is the same style class already used
// for the Record/Playback clock labels.
void show_setup_required_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                 const Glib::ustring& body,
                                 const Glib::ustring& command) {
    auto* dialog = adw_alert_dialog_new(heading.c_str(), body.c_str());
    auto* command_label = Gtk::make_managed<Gtk::Label>(command);
    command_label->set_selectable(true);
    command_label->set_halign(Gtk::Align::START);
    command_label->set_wrap(true);
    command_label->add_css_class("monospace");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog),
                                      GTK_WIDGET(command_label->gobj()));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", _("OK"));
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent->gobj()));
}

// Record's own transport controls — the counterpart to
// add_playback_controls(). Forward *starts* recording — no pause — and
// the active one is shown red; .destructive-action is used purely for
// its red styling, not any "destructive" meaning.
void add_record_controls(Gtk::Window* window, GtkWidget* header_bar,
                          std::shared_ptr<RecordingSession> session,
                          const Glib::RefPtr<Gio::Settings>& settings,
                          const std::string& project_dir,
                          const std::shared_ptr<Protocol>& active_protocol,
                          AdwToastOverlay* toast_overlay,
                          Gtk::Picture* splash_picture,
                          Gtk::Picture* no_image_picture,
                          std::function<void(const std::string&)> load_image,
                          std::shared_ptr<std::function<void(bool)>> page_navigate,
                          std::function<void(bool)> default_page_navigate,
                          std::shared_ptr<std::function<void()>> close_self,
                          std::function<void(const std::string&)>
                              update_sequence_button_label,
                          Gtk::Button* forward_button, Gtk::Button* stop_button,
                          Gtk::EditableLabel* clock_label,
                          std::shared_ptr<std::function<void()>> start_recording_out,
                          std::shared_ptr<std::function<void()>> stop_recording_out,
                          std::shared_ptr<std::function<void()>> toggle_pause_out,
                          std::shared_ptr<std::function<void(const std::string&)>>
                              change_record_image_out) {
    // Starting state only — the mic icon below can still toggle it for
    // the rest of this session.
    session->microphone_on = settings->get_boolean(kMicrophoneInitiallyOnKey);

    // Forward/backward image navigation, Record-bar-only. Starts at
    // whatever image was already open from Interactive mode, or -1
    // ("no image") if none. Snapshotted once rather than re-read per nav
    // press — the sequence can't change while the Record bar is up
    // (Interactive mode, where "Edit Sequence…" lives, is hidden).
    //
    // No display change happens here, deliberately — entering Record
    // keeps showing whatever was already on screen until a nav button
    // is pressed or the record-start button forces the splash→test-image
    // switch itself.
    auto sequence = std::make_shared<std::vector<std::string>>(
        visible_image_paths(project_dir));
    {
        auto it = std::find(sequence->begin(), sequence->end(),
                             session->current_image_path);
        session->nav_index = (it != sequence->end())
            ? static_cast<int>(std::distance(sequence->begin(), it))
            : -1;
    }

    // Draws whatever nav_index now points at: a real image (via
    // load_image()) at a valid index, or the test-image placeholder at
    // -1. Only called from the nav button handlers below, never at
    // entry.
    auto sync_record_display = [splash_picture, no_image_picture, session,
                                 sequence, load_image, settings,
                                 update_sequence_button_label]() {
        int idx = session->nav_index;
        if (idx < 0 || idx >= static_cast<int>(sequence->size())) {
            session->current_image_path.clear();
            session->viewing_equirect = false;
            session->panorama_projection = PanoramaProjection{};
            session->equirect_source.reset();
            // "No image" has no depth map, by definition — without
            // this, session_has_depth() kept reporting true from
            // whatever image was loaded before, so clicking still fired
            // a real prod using that stale depth data.
            session->depth_data.clear();
            session->depth_width = 0;
            session->depth_height = 0;
            dismiss_splash();
            splash_picture->set_paintable({});
            splash_picture->add_css_class("blind-sight-no-image-background");
            no_image_picture->set_visible(true);
            update_sequence_button_label("");
            set_last_image_path(settings, "");
        } else {
            no_image_picture->set_visible(false);
            splash_picture->remove_css_class("blind-sight-no-image-background");
            load_image((*sequence)[idx]);
        }
    };

    // Real stopwatch: starts on Forward, stops on Stop. Whole seconds
    // only — a precise ticking display stresses test subjects —
    // connect_seconds() ticks aligned to second boundaries rather than a
    // sub-second interval truncated for display.
    //
    // clock_label, shared with Playback and Interactive, is built once
    // in on_activate() and passed in; refresh_transport_sensitivity()
    // there makes it (and track_button) insensitive for the duration of
    // Record, while this function keeps writing the ticking text to it
    // — the time keeps updating, but the user can't edit it.

    // forward_button/stop_button are the shared, permanently-visible
    // transport buttons, built once in on_activate(), not built here.

    // Forward/backward image navigation steps nav_index through
    // `sequence`, wrapping through -1 ("no image") at both ends. No
    // dedicated buttons of its own: the always-visible nav_prev_button/
    // nav_next_button and Page Up/Page Down both drive this directly,
    // through go_to_nav_index().

    // session->microphone_on is reset from settings by the caller right
    // before calling this function — no mute button reads/writes it any
    // more (removed; the monitoring matrix and the Speaker/ambient-mic
    // device dialogs took over that role), it just gates whether
    // start_audio_track() runs at each track-start site below.

    // Protocol window toggle — protocol_button is permanent too, wired
    // once in on_activate(). Nothing left for this function to build or
    // connect.

    auto elapsed_seconds = std::make_shared<int>(0);
    auto tick_connection = std::make_shared<sigc::connection>();

    auto format_clock = [](int total_seconds) {
        int minutes = total_seconds / 60;
        int seconds = total_seconds % 60;
        auto two_digits = [](int n) {
            return (n < 10 ? std::string("0") : std::string()) + std::to_string(n);
        };
        return two_digits(minutes) + ":" + two_digits(seconds);
    };
    // Icon-color-only class, not .destructive-action, which tints the
    // whole button, not just the icon.
    //
    // Forward (re)opens the file in append mode, always the same file.
    // On the first press there's no gap to report; on every press after
    // a Stop, this is a resume, and a WaitEvent goes in first, anchored
    // at the accumulated position where the previous segment ended. The
    // display clock continues from that position rather than resetting
    // to 00:00, matching `t_ms` staying a running total.
    //
    // Out-parameter, not a direct signal_clicked() connection:
    // forward_button is the shared, permanent record_button, clicked
    // from on_activate()'s handler, which calls this after wiring it in
    // (same reassignable-shared_ptr pattern as seek_to_out/page_navigate
    // elsewhere).
    *start_recording_out = [forward_button, clock_label, elapsed_seconds,
                             tick_connection, format_clock, session,
                             splash_picture, no_image_picture, project_dir,
                             active_protocol]() {
            forward_button->add_css_class("blind-sight-record-active");

            session->file = std::make_shared<std::ofstream>(
                session->session_path, std::ios::app);

            // Still on the "no image" nav position when capture starts —
            // switches the display from the plain splash to the same
            // test-image placeholder sync_record_display() uses.
            if (session->current_image_path.empty()) {
                dismiss_splash();
                splash_picture->set_paintable({});
                splash_picture->add_css_class("blind-sight-no-image-background");
                no_image_picture->set_visible(true);
            }

            if (session->has_run_before) {
                auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - session->stopped_at);
                WaitEvent wait_event;
                wait_event.t_ms = session->accumulated_ms;
                wait_event.duration_ms = static_cast<std::int32_t>(gap_ms.count());
                write_event(*session->file, wait_event);
                // A new track starts exactly where a WaitEvent gets
                // written — same boundary Playback will later read back
                // from this same file.
                ++session->current_track;
            }
            session->has_run_before = true;
            session->paused = false;
            session->segment_start = std::chrono::steady_clock::now();
            mint_new_track(session);

            // Fresh track, fresh audio segment numbering — one WAV file
            // per track; see RecordingSession's own comment for why.
            session->audio_segment = 0;
            if (session->microphone_on) {
                start_audio_track(
                    session, track_audio_path_for_id(session->session_path,
                                                       session->current_track_id,
                                                       session->audio_segment));
                ++session->audio_segment;
            }

            *elapsed_seconds = session->accumulated_ms / 1000;
            // Time only, no "Track n" prefix — the scenario already
            // carries a name via the protocol, when one is in use.
            clock_label->set_text(format_clock(*elapsed_seconds));
            tick_connection->disconnect();
            *tick_connection = Glib::signal_timeout().connect_seconds(
                [clock_label, elapsed_seconds, format_clock]() {
                    ++(*elapsed_seconds);
                    clock_label->set_text(format_clock(*elapsed_seconds));
                    return true;  // keep ticking
                },
                1);

            // No image open — position -1; recording is allowed to
            // start here, but nothing gets logged until the user
            // navigates to a real image, which writes its own
            // ImageEvent/ViewEvent pair at that point.
            if (!session->current_image_path.empty()) {
                // Before the write, not after — count_new_image_reference()'s
                // "already referenced" check reads the file as it stands now.
                count_new_image_reference(project_dir, session->session_path,
                                           session->current_image_path);
                ImageEvent image_event;
                image_event.t_ms = session->accumulated_ms;
                image_event.path = session->current_image_path;
                image_event.scenario = scenario_title_for_image(
                    active_protocol, project_dir, session->current_image_path);
                write_event(*session->file, image_event);

                // Always right after type:image — a reader should never
                // find one without the other at a segment boundary.
                ViewEvent view_event;
                view_event.t_ms = session->accumulated_ms;
                view_event.yaw = session->view_yaw;
                view_event.pitch = session->view_pitch;
                write_event(*session->file, view_event);
            }
    };

    *stop_recording_out = [window, forward_button, tick_connection, session,
                            settings, toast_overlay, clock_label, elapsed_seconds,
                            format_clock]() {
            forward_button->remove_css_class("blind-sight-record-active");
            tick_connection->disconnect();
            stop_audio_track_and_encode(window, session, settings, toast_overlay);
            if (session->file) {
                // Only fold the current segment in if still running — a
                // pause already folded it in once and left segment_start
                // stale, so doing it again on a Stop from a paused state
                // would double-count that interval.
                if (!session->paused) {
                    session->accumulated_ms += static_cast<std::int32_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - session->segment_start)
                            .count());
                }
                session->paused = false;
                session->stopped_at = std::chrono::steady_clock::now();
                session->file->close();
                session->file.reset();
            }
            // The per-second ticker's last tick is whole-seconds-rounded
            // and can land up to ~1s short of the precise accumulated_ms
            // computed just above, so this corrects the display. Not
            // reset to 00:00 like Playback's Stop — accumulated_ms is a
            // running total across Forward/Stop cycles.
            *elapsed_seconds = session->accumulated_ms / 1000;
            clock_label->set_text(format_clock(*elapsed_seconds));
    };

    // Record-button pause/resume, triggered by pressing Record again
    // while already recording — a lighter sibling of Stop: the file
    // stays open, the track stays the same (no WaitEvent), only
    // gaze/view/prod capture and the display ticker suspend for as long
    // as session->paused is true.
    *toggle_pause_out = [forward_button, session, tick_connection, clock_label,
                          elapsed_seconds, format_clock]() {
        if (session->paused) {
            session->segment_start = std::chrono::steady_clock::now();
            session->paused = false;
            forward_button->add_css_class("blind-sight-record-active");
            tick_connection->disconnect();
            *tick_connection = Glib::signal_timeout().connect_seconds(
                [clock_label, elapsed_seconds, format_clock]() {
                    ++(*elapsed_seconds);
                    clock_label->set_text(format_clock(*elapsed_seconds));
                    return true;  // keep ticking
                },
                1);
        } else {
            session->accumulated_ms += static_cast<std::int32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - session->segment_start)
                    .count());
            session->paused = true;
            tick_connection->disconnect();
            forward_button->remove_css_class("blind-sight-record-active");
            *elapsed_seconds = session->accumulated_ms / 1000;
            clock_label->set_text(format_clock(*elapsed_seconds));
        }
    };

    // Applies a new nav_index: if a recording is already running, first
    // marks the implicit track boundary this image change causes and
    // updates the clock display in place — the per-second ticker keeps
    // running and re-reads session->current_track live, so only the
    // immediate label text needs correcting here. Then redraws
    // (sync_record_display) and, only for a real (non -1) target while
    // actively recording, writes the fresh ImageEvent/ViewEvent pair —
    // skipped at -1, same rule the record-start button uses.
    auto go_to_nav_index = [session, sync_record_display, window, settings,
                             toast_overlay, clock_label, elapsed_seconds,
                             format_clock, project_dir, active_protocol](int new_index) {
        session->nav_index = new_index;
        bool recording_active = (session->file != nullptr);
        if (recording_active) {
            write_implicit_track_change(window, session, settings,
                                         toast_overlay);
            *elapsed_seconds = session->accumulated_ms / 1000;
            clock_label->set_text(format_clock(*elapsed_seconds));
        }

        sync_record_display();

        if (recording_active && !session->current_image_path.empty()) {
            // Before the write, not after — see start_recording_out's
            // own identical comment just above.
            count_new_image_reference(project_dir, session->session_path,
                                       session->current_image_path);
            ImageEvent image_event;
            image_event.t_ms = session->accumulated_ms;
            image_event.path = session->current_image_path;
            image_event.scenario = scenario_title_for_image(active_protocol, project_dir,
                                                               session->current_image_path);
            write_event(*session->file, image_event);

            ViewEvent view_event;
            view_event.t_ms = session->accumulated_ms;
            view_event.yaw = session->view_yaw;
            view_event.pitch = session->view_pitch;
            write_event(*session->file, view_event);
        }
    };

    // Lets the image dropdown's row-click handler (activate_image(),
    // on_activate()) go through the same go_to_nav_index() the
    // Forward/Backward buttons use, rather than calling load_image()
    // directly and silently skipping the implicit track change +
    // ImageEvent/ViewEvent writing.
    *change_record_image_out = [sequence, go_to_nav_index](const std::string& path) {
        auto it = std::find(sequence->begin(), sequence->end(), path);
        if (it != sequence->end()) {
            go_to_nav_index(static_cast<int>(std::distance(sequence->begin(), it)));
        }
    };

    // Page Up/Page Down and nav_prev_button/nav_next_button both call
    // go_to_nav_index() directly, reimplementing the index math rather
    // than calling .activate() on a never-packed button pair: a
    // Gtk::Widget that's never been realized/mapped doesn't reliably
    // emit "clicked" from .activate() in GTK4 — intermittent, not every
    // click.
    *page_navigate = [session, sequence, go_to_nav_index](bool forward) {
        int n = static_cast<int>(sequence->size());
        if (n == 0) {
            return;
        }
        int next = forward ? session->nav_index + 1 : session->nav_index - 1;
        if (next >= n) {
            next = -1;
        } else if (next < -1) {
            // Same backward-out-of-"No image" wrap as
            // interactive_page_navigate() — see its own comment.
            next = n - 1;
        }
        go_to_nav_index(next);
    };

    // Session loading is not this bar's concern — New/Open Session, the
    // recents list, and Session Details all live in the shared session
    // dropdown in on_activate(). All this bar needs is to reflect
    // whatever's already loaded there.
    bool has_shared_session = !session->session_path.empty();
    forward_button->set_sensitive(has_shared_session);
    stop_button->set_sensitive(has_shared_session);

    // No close (X) button any more — the header bar's Stop button
    // (on_activate) drives returning to Interactive, via close_self
    // below. A fresh pair of Boxes built every time Record is entered —
    // no reference() needed, left to be destroyed on close.
    //
    // session_recent_button is a single, permanently-packed widget in
    // on_activate(), shared with Playback, never built or torn down
    // here. record_bar_start/_end are left empty now that the transport
    // buttons, clock, and mic/speaker/protocol are all shared/permanent
    // — kept as a structural placeholder for close_self's pack/remove.
    auto* record_bar_start =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    auto* record_bar_end =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);

    // The actual teardown, reusable — on_activate()'s mode-switch click
    // handlers call this via close_self whenever the user switches away
    // from Record to any other mode, not just back to Interactive. No
    // open file handle exists yet to close if Forward was never
    // pressed — write_record_meta()/update_record_meta() each open,
    // write, and close in one call. Closing mid-run does still need to
    // close session->file, same as Stop.
    *close_self = [window, header_bar, record_bar_start, record_bar_end,
                    tick_connection, session, settings, toast_overlay,
                    splash_picture, no_image_picture, page_navigate,
                    default_page_navigate]() {
        // Stop the tick first — pointless once the action bar (and its
        // elapsed_seconds counter) is gone, even though clock_label
        // itself is permanent now and survives this.
        tick_connection->disconnect();
        stop_audio_track_and_encode(window, session, settings, toast_overlay);
        // Closing the action bar without pressing Stop first shouldn't
        // leave the file open.
        if (session->file) {
            session->file->close();
            session->file.reset();
        }
        // Leaving Record always returns to a blank display, undoing
        // whichever of the "no image" placeholder or a real image was
        // on screen. The bat-logo-plus-text splash is only ever the
        // pristine, nothing-tried-yet state — once dismissed this run,
        // restoring it here would bring the welcome text back after the
        // app has moved past it, so this falls back to the plain
        // no-image placeholder instead.
        if (g_splash_dismissed) {
            splash_picture->set_paintable({});
            splash_picture->add_css_class("blind-sight-no-image-background");
            no_image_picture->set_visible(true);
        } else {
            splash_picture->remove_css_class("blind-sight-no-image-background");
            splash_picture->set_paintable(render_splash_texture());
            no_image_picture->set_visible(false);
        }
        // Page Up/Page Down goes back to browsing image-sequence
        // directly, not Record's nav buttons — those don't exist
        // once this bar is gone.
        *page_navigate = default_page_navigate;
        adw_header_bar_remove(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(record_bar_start->gobj()));
        adw_header_bar_remove(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(record_bar_end->gobj()));
    };

    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(record_bar_start->gobj()));
    // pack_end, not pack_start: the menu button (also pack_end, added
    // first) sits at the true edge, and each later pack_end lands closer
    // to the center, landing this immediately to its left.
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(record_bar_end->gobj()));
}

void on_activate(const Glib::RefPtr<Gtk::Application>& app) {
    // A second launch re-activates this same (single-instance) process
    // via D-Bus rather than starting a new one, so this guard is
    // needed, or every re-activation would build a second complete
    // window, including its own RecordingSession/audio_pipeline. Two
    // windows both reaching for the same default PipeWire capture
    // device is resource contention that could cause an audio stall.
    if (auto* existing = app->get_active_window()) {
        existing->present();
        return;
    }

    auto* adw_win = ADW_APPLICATION_WINDOW(
        adw_application_window_new(app->gobj()));

    // Wrapped here, early: the window title reflects whichever of
    // Interactive/Playback/Record is current, so title-setting needs
    // `window` wherever an action bar switch happens, not just once at
    // the end.
    auto* window = Glib::wrap(GTK_WINDOW(adw_win), false);
    window->set_title(_("Blind Sight"));
    window->set_default_size(kDefaultWidth, kDefaultHeight);

    // AdwApplicationWindow rejects both Gtk::Window::set_child() and
    // set_titlebar() (the latter aborts with a hard Adwaita-ERROR, not
    // just a warning). The supported structure is an AdwToolbarView as
    // the window's content, holding an AdwHeaderBar as its top bar.
    // GNOME/Mutter draws no server-side decorations under Wayland, so
    // without this the window would have no close button at all.
    auto* header_bar = adw_header_bar_new();

    // Primary/hamburger menu, right of the window controls — slightly
    // against current GNOME HIG guidance but common enough in real
    // apps. Record/Playback are buttons, not menu entries, so the menu
    // is just app-wide, one-shot dialogs.
    auto menu = Gio::Menu::create();

    // Opus encoding happens automatically at Stop, see
    // stop_audio_track_and_encode() — no manual "Encode Audio to Opus"
    // menu entry.

    auto other_section = Gio::Menu::create();
    // Not the project dropdown — that's New/Open plus the recent-
    // projects list, nothing that just runs an action in place.
    // Ellipsis per GNOME HIG: only on an action that needs more
    // information from the user before it actually runs (Trace/Dump
    // both open a dialog that gathers that first) — not on Preferences
    // (instant-apply, no pending action waiting on input) or the
    // purely informational Shortcuts/About.
    other_section->append(_("Reconcile Protocol"), "app.reconcile-protocol");
    other_section->append(_("Trace Sound Delay…"), "app.trace-delay");
    other_section->append(_("Dump Sound Paths…"), "app.dump-sound-paths");
    other_section->append(_("Preferences"), "app.preferences");
    other_section->append(_("Keyboard Shortcuts"), "app.shortcuts");
    other_section->append(_("About Blind Sight"), "app.about");
    menu->append_section(other_section);

    auto* menu_button = Gtk::make_managed<Gtk::MenuButton>();
    menu_button->set_icon_name("open-menu-symbolic");
    menu_button->set_menu_model(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(menu_button->gobj()));

    // protocol_button is permanent but stands in its own group now,
    // between nav_group and transport_group (see the standalone
    // adw_header_bar_pack_end() call further down, right after
    // transport_group's own) — deliberately not part of any .linked
    // group, so it keeps rounded corners on all four sides. Constructed
    // here, with the other permanently-packed widgets, since its own
    // click handler/sensitivity wiring further down already assumes it
    // exists by then.
    auto* protocol_button = Gtk::make_managed<Gtk::Button>();
    // TODO: placeholder icon — pick something that actually reads as
    // "protocol" rather than this generic data-processor glyph.
    set_button_icon_from_file(protocol_button, "data-processor-symbolic.svg");
    protocol_button->set_tooltip_text(_("Show Protocol"));
    // No project selected yet at startup; update_project_button_label()
    // only re-gates this on an actual project-state change, so it needs
    // its own initial value, same reasoning as project_button's own
    // construction-time tooltip.
    protocol_button->set_sensitive(false);
    // Mic/Speaker/Monitoring, permanently packed between the transport
    // toolbar and the hamburger menu. Packed *before* transport_group
    // below, not after — pack_end() lands each later call closer to
    // center, so calling this one first (right after menu_button) puts
    // it immediately to menu_button's left, with transport_group
    // landing further left in turn.
    //
    // Mic/speaker here are a plain, always-sensitive global mute toggle
    // — not the old (removed) "start/stop this track's audio pipeline"
    // behavior, and not tied to Record/Playback mode at all. Per John:
    // speaker-mute still streams audio, just makes it inaudible; mic-
    // mute means there's nothing being captured to stream at all (bar
    // the app's own synthesized output). Mic-mute cuts every voice
    // leveller's output and freezes its AGC (connected further down,
    // once the routing engine exists); it doesn't pause the recording
    // pipeline itself.
    auto* mic_button = Gtk::make_managed<Gtk::Button>();
    mic_button->set_tooltip_text(_("Mute All Microphones"));
    auto mic_muted = std::make_shared<bool>(false);
    auto refresh_mic_mute_icon = [mic_button](bool muted) {
        set_button_icon_from_file(mic_button,
                                   muted ? "mic-no-symbolic.svg" : "mic-symbolic.svg");
    };
    refresh_mic_mute_icon(false);
    auto toggle_mic_mute = [mic_muted, refresh_mic_mute_icon]() {
        *mic_muted = !*mic_muted;
        refresh_mic_mute_icon(*mic_muted);
    };
    mic_button->signal_clicked().connect(toggle_mic_mute);

    auto* speaker_button = Gtk::make_managed<Gtk::Button>();
    speaker_button->set_tooltip_text(_("Mute All Speakers"));
    auto speaker_muted = std::make_shared<bool>(false);
    auto refresh_speaker_mute_icon = [speaker_button](bool muted) {
        set_button_icon_from_file(speaker_button,
                                   muted ? "speaker-no-symbolic.svg" : "speaker-symbolic.svg");
    };
    refresh_speaker_mute_icon(false);
    auto toggle_speaker_mute = [speaker_muted, refresh_speaker_mute_icon]() {
        *speaker_muted = !*speaker_muted;
        refresh_speaker_mute_icon(*speaker_muted);
    };
    speaker_button->signal_clicked().connect(toggle_speaker_mute);

    // Rightmost in the group — a plain dropdown chevron now that
    // mic/speaker have their own icons; opens the monitoring matrix
    // popover (who hears whom). Sensitivity wired in
    // refresh_transport_sensitivity(), gated on has_session only.
    auto* matrix_button = Gtk::make_managed<Gtk::MenuButton>();
    matrix_button->set_icon_name("pan-down-symbolic");
    matrix_button->set_tooltip_text(_("Monitor…"));
    matrix_button->set_sensitive(false);
    auto* doc_group =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    doc_group->add_css_class("linked");
    doc_group->append(*mic_button);
    doc_group->append(*speaker_button);
    doc_group->append(*matrix_button);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(doc_group->gobj()));

    // The transport — Record/Play/Stop/FB/FF, five buttons with no
    // separate mode selector: when none is pressed, the app is
    // implicitly in Interactive mode. This just reserves the widgets
    // and their position; wiring happens further down once
    // recording_session/playback_session/active_protocol/page_navigate
    // all exist. FB/FF are inert placeholders until the hold-to-scrub
    // gesture replaces this later.
    auto* record_button = Gtk::make_managed<Gtk::Button>();
    record_button->set_icon_name("media-record-symbolic");
    record_button->set_tooltip_text(_("Record"));
    record_button->set_sensitive(false);
    auto* play_button = Gtk::make_managed<Gtk::Button>();
    play_button->set_icon_name("media-playback-start-symbolic");
    play_button->set_tooltip_text(_("Play"));
    play_button->set_sensitive(false);
    auto* stop_button = Gtk::make_managed<Gtk::Button>();
    stop_button->set_icon_name("media-playback-stop-symbolic");
    stop_button->set_tooltip_text(_("Stop"));
    stop_button->set_sensitive(false);
    auto* fast_backward_button = Gtk::make_managed<Gtk::Button>();
    fast_backward_button->set_icon_name("media-seek-backward-symbolic");
    fast_backward_button->set_sensitive(false);
    auto* fast_forward_button = Gtk::make_managed<Gtk::Button>();
    fast_forward_button->set_icon_name("media-seek-forward-symbolic");
    fast_forward_button->set_sensitive(false);
    auto* transport_group =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    transport_group->add_css_class("linked");
    // FB/FF, in that order, to the left of Record.
    transport_group->append(*fast_backward_button);
    transport_group->append(*fast_forward_button);
    transport_group->append(*record_button);
    transport_group->append(*play_button);
    transport_group->append(*stop_button);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(transport_group->gobj()));
    // protocol_button's own call, deliberately not part of any .linked
    // group — placed right after transport_group's own pack_end() and
    // before nav_group's (further down), landing it visually between
    // transport_group's own leftmost button (FB) and nav_group's own
    // rightmost button (Next), per the same "later call lands closer to
    // center" pack_end() ordering transport_group/nav_group/clock_group
    // already rely on.
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(protocol_button->gobj()));

    auto settings = Gio::Settings::create(kAppId);
    g_viewing_distance_mm = static_cast<int>(
        std::lround(settings->get_double(kViewingDistanceKey) * 1000.0));

    // First take on the acoustic model's carrier signal — started once
    // for the app's lifetime, triggered per-prod from fire_prod()
    // further down, and played into each listener's device by the routing
    // engine. Never fatal if it fails to start (no PipeWire running), same
    // convention as this app's other optional-hardware probes.
    auto noise_pulse_engine = std::make_shared<NoisePulseEngine>();
    if (!noise_pulse_engine->start()) {
        g_message("blind-sight: noise pulse engine failed to start — "
                   "prods will show their glow but stay silent");
    }

    // Real-time audio routing for the monitoring matrix — one instance
    // for the whole app's lifetime, since it has no session-specific
    // state of its own beyond whatever MonitoringState it's handed on
    // each sync() call. See audio_routing.hpp.
    auto audio_routing_engine =
        std::make_shared<blind_sight::AudioRoutingEngine>(noise_pulse_engine);

    // No example-image auto-seed: the image list is project-scoped
    // (images.json), so every project starts empty until something is
    // actually imported into it.

    // The project dropdown — leftmost item in the header bar, packed
    // unconditionally: a project is a cross-mode concept (it bundles a
    // protocol.md with its related recording sessions), not something
    // that only makes sense in Interactive. Its label shows the active
    // project's H1 title, truncated per project-title-cutoff-chars —
    // "No Project" until one exists. Popover content is built further
    // down, once active_protocol/refresh_sequence_list exist.
    // project_button/session_recent_button/sequence_button each shrink
    // to their natural width rather than a fixed minimum, since
    // Previous/Next now live on the other side of the header bar, so a
    // changing dropdown width can't shift them into a misclick. A
    // *maximum* width still applies, so a long title can't push into
    // the window's title area.
    //
    // Right-aligns the label flush against the dropdown arrow —
    // set_label()'s convenience always centers the label+arrow pair as
    // one block, leaving a gap on the left whenever max_width_chars caps
    // a long title short of the button's allocated width. Builds the
    // label directly and makes it fill the button (set_hexpand) so its
    // xalign positions text against the button's right edge.
    // set_always_show_arrow() is required once a custom child replaces
    // set_label()'s built-in one.
    auto setup_dropdown_button = [](Gtk::MenuButton* button) {
        auto* label = Gtk::make_managed<Gtk::Label>();
        label->set_xalign(1.0f);
        label->set_hexpand(true);
        label->set_ellipsize(Pango::EllipsizeMode::END);
        button->set_child(*label);
        button->set_always_show_arrow(true);
        return label;
    };

    auto* project_button = Gtk::make_managed<Gtk::MenuButton>();
    auto* project_button_label = setup_dropdown_button(project_button);
    project_button_label->set_text(_("No Project"));
    // update_project_button_label() (below) only ever runs on an actual
    // project-state change, never once unconditionally at startup — so
    // this needs its own initial value, same as session/image/track's
    // own construction-time tooltip, rather than staying unset until
    // the first project opens or closes.
    project_button->set_tooltip_text(_("No active projects"));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                               GTK_WIDGET(project_button->gobj()));

    // The image-sequence dropdown — one MenuButton with both the title
    // and "Open Image" (as a row inside its own popover, not a separate
    // button next to it). Stands in for a prepared, specific-order set
    // of images for a test run, not an auto-tracked MRU list — see
    // visible_image_paths()'s own comment. A MenuButton+popover+ListBox
    // rather than a plain
    // Gtk::DropDown: rows need per-row markup (a checkmark on the
    // currently-loaded one) a plain DropDown's value-list model doesn't
    // support.
    auto* sequence_button = Gtk::make_managed<Gtk::MenuButton>();
    auto* sequence_button_label = setup_dropdown_button(sequence_button);
    sequence_button_label->set_text(_("No image"));
    // update_sequence_button_label() (below) only ever runs on an
    // actual image-state change, never once unconditionally at
    // startup — so this needs its own initial value.
    sequence_button->set_tooltip_text(_("No active images"));

    // Keeps sequence_button's label in sync with whichever image is
    // actually on screen — same title-or-basename fallback as the
    // dropdown's own rows (refresh_sequence_list, below) and Image
    // Details. "" means no image shown — load_image() and every call
    // site that clears current_image_path to "" both call this, so it
    // never goes stale.
    auto update_sequence_button_label = [sequence_button, sequence_button_label,
                                           settings](const std::string& path) {
        if (path.empty()) {
            sequence_button_label->set_text(_("No image"));
            sequence_button->set_tooltip_text(_("No active images"));
            return;
        }
        auto meta = read_image_metadata(path);
        auto overrides = image_overrides_for_id(project_dir_from_image_path(path),
                                                  image_id_from_path(path));
        const std::string& title_override = overrides.first;
        Glib::ustring title = !title_override.empty() ? title_override
                               : !meta.title.empty()   ? meta.title
                                                        : Glib::path_get_basename(path);
        // Same cutoff-chars-then-ellipsis convention as project_button/
        // session_recent_button (truncate_dropdown_title()) — an
        // uncapped title is what let this button's width vary.
        sequence_button_label->set_text(truncate_dropdown_title(
            title, settings->get_int(kImageTitleCutoffCharsKey)));
        // The menu button's own tooltip — same subtitle text the
        // current image's row would show (caption_override, else the
        // file's own caption), not the generic collection name, so
        // hovering the button previews the active image without
        // opening the popover. Falls back to the generic collection
        // name when there's no caption to show.
        const std::string& caption_override = overrides.second;
        std::string caption = !caption_override.empty() ? caption_override : meta.caption;
        sequence_button->set_tooltip_text(!caption.empty() ? caption : _("No active images"));
    };

    // The Interactive bar — the default/home state, shown at startup.
    // "Action bar" is the general term for this category (Interactive/
    // Record/Playback are the three concrete instances), chosen to
    // avoid colliding with the real Adw.Toast concept. Mode switching is
    // driven by the transport buttons (current_mode below), not a close
    // (X) button on each bar.
    //
    // sequence_button is packed once, unconditionally — it stays packed
    // across all three modes, same as project_button. The actual
    // pack_start() call is further down, since session_recent_button
    // needs to land between project_button and this one, in that order.

    // Next/Previous — always visible too, packed on the header bar's
    // other side from the image dropdown. Just calls whichever
    // *page_navigate currently means — the same dispatcher Page Up/Page
    // Down drives, reassigned per mode by add_record_controls()/
    // add_playback_controls()/the mode_dropdown handler below, so this
    // needs no mode-awareness of its own. Wired further down, once
    // page_navigate itself exists.
    auto* nav_prev_button = Gtk::make_managed<Gtk::Button>();
    nav_prev_button->set_icon_name("go-previous-symbolic");
    nav_prev_button->set_tooltip_text(_("Previous"));
    auto* nav_next_button = Gtk::make_managed<Gtk::Button>();
    nav_next_button->set_icon_name("go-next-symbolic");
    nav_next_button->set_tooltip_text(_("Next"));
    auto* nav_group =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
    nav_group->add_css_class("linked");
    nav_group->append(*nav_prev_button);
    nav_group->append(*nav_next_button);
    // pack_end() call is further down, between clock_group and
    // transport_group — its own group, not merged into either, since it
    // manipulates both transport and images.

    // Declared here so show_interactive_bar() below can capture
    // recording_session too — it needs to read current_image_path back
    // to restore a protocol-aware window title on return to Interactive.
    auto recording_session = std::make_shared<RecordingSession>();
    auto playback_session = std::make_shared<PlaybackSession>();

    // Loads `path` as the shared working session — populates both
    // recording_session's resume state (what Record needs to append
    // correctly) and playback_session's replay state (what Playback
    // needs to replay), from the same file: there's only one session
    // concept, not two independent "currently open" ones. False only
    // when the file can't be read as a recording at all; callers decide
    // what that means (show an error, or — for Record — "doesn't exist
    // yet, create it"). Deliberately doesn't touch session_recent_button
    // or its sensitivity-gated buttons — each call site updates its own
    // UI immediately afterward.
    auto apply_shared_session = [recording_session, playback_session, settings,
                                  audio_routing_engine, update_sequence_button_label](
                                     const std::string& path) -> bool {
        RecordMeta meta;
        if (!read_record_meta(path, meta)) {
            return false;
        }
        auto events = read_record_events(path);
        std::int32_t last_t_ms = 0;
        int wait_count = 0;
        for (auto& event : events) {
            last_t_ms = std::max(
                last_t_ms, std::visit([](auto&& e) { return e.t_ms; }, event));
            if (std::holds_alternative<WaitEvent>(event)) {
                ++wait_count;
            }
        }
        recording_session->session_path = path;
        recording_session->accumulated_ms = last_t_ms;
        recording_session->has_run_before = !events.empty();
        recording_session->paused = false;
        recording_session->stopped_at = std::chrono::steady_clock::now();
        recording_session->current_track = wait_count + 1;
        // Fresh monitoring defaults every session load — see
        // MonitoringState's own comment on why this isn't persisted.
        recording_session->monitoring = MonitoringState{};
        reconcile_monitoring_state(recording_session->monitoring, participant_names(meta));
        seed_preferred_devices(recording_session->monitoring, settings);
        audio_routing_engine->sync(recording_session->monitoring);

        stop_playback_audio(playback_session);
        playback_session->track_starts = compute_track_starts(events);
        playback_session->audio_segments = compute_audio_segments(events);
        playback_session->transcript_cues =
            compute_transcript_cues(path, playback_session->audio_segments);
        playback_session->events = std::move(events);
        playback_session->session_path = path;
        playback_session->next_index = 0;
        playback_session->base_ms = 0;
        playback_session->speed = 1.0;
        load_playback_image(playback_session, "");
        update_sequence_button_label("");

        set_current_working_session(settings, path);
        return true;
    };

    // The monitoring matrix popover's content — built once, rebuilt
    // (via its own `refresh`) every time the popover is about to show.
    // Forward reference for the Popover itself (matrix_popover_ptr):
    // the content's own header buttons need to close the popover before
    // opening a dialog on top of it, but the Popover can only be
    // constructed once this content already exists — same idiom as
    // project_details_button_ptr elsewhere in this file.
    auto matrix_popover_ptr = std::make_shared<Gtk::Popover*>(nullptr);
    MonitoringMatrixWidget matrix_widget = build_monitoring_matrix_content(
        window, &recording_session->monitoring,
        [recording_session]() { return recording_session->session_path; }, matrix_popover_ptr,
        settings, audio_routing_engine.get());
    auto* matrix_popover = Gtk::make_managed<Gtk::Popover>();
    matrix_popover->set_child(*matrix_widget.widget);
    matrix_button->set_popover(*matrix_popover);
    *matrix_popover_ptr = matrix_popover;
    // Without this, the popover anchors to matrix_button alone (the
    // chevron) — visually hanging off the right edge of the linked
    // group rather than the group as a whole, even though mic/speaker
    // are part of the same control. Point it at doc_group's own bounds
    // instead, translated into matrix_button's coordinate space (what
    // set_pointing_to() expects, since that's the widget the popover is
    // actually parented to via set_popover()). Computed on every show,
    // not once at construction, since widget bounds aren't valid until
    // the window is realized.
    matrix_popover->signal_show().connect([matrix_widget, doc_group, matrix_button,
                                            matrix_popover]() {
        if (auto bounds = doc_group->compute_bounds(*matrix_button)) {
            matrix_popover->set_pointing_to(
                Gdk::Rectangle(static_cast<int>(bounds->get_x()), static_cast<int>(bounds->get_y()),
                               static_cast<int>(bounds->get_width()),
                               static_cast<int>(bounds->get_height())));
        }
        matrix_widget.refresh();
    });

    // Re-reads just the track/event metadata from disk — the track
    // dropdown's narrower counterpart to apply_shared_session() above:
    // opening the dropdown without pressing Play first would otherwise
    // still show whatever track list was loaded last, since Record
    // writes straight to its own file handle, never through
    // playback_session. Deliberately *not* apply_shared_session() itself:
    // that also resets next_index/base_ms and forces the shared picture
    // to "No image" — right when Play is about to replace Interactive's
    // display, wrong here, where the dropdown can open while Interactive
    // shows a real image. track_starts/events only grow by appending, so
    // the existing next_index/base_ms stay valid positions into the
    // refreshed array.
    auto reread_track_metadata = [playback_session](const std::string& path) {
        auto events = read_record_events(path);
        playback_session->track_starts = compute_track_starts(events);
        playback_session->audio_segments = compute_audio_segments(events);
        playback_session->transcript_cues =
            compute_transcript_cues(path, playback_session->audio_segments);
        playback_session->events = std::move(events);
    };

    // Shared by the track dropdown's row click and clock_label's editing-
    // commit handler — "seek" while nothing is on screen to visually
    // restore just means priming next_index/base_ms so the next Play
    // press starts exactly there. current_mode is left untouched —
    // picking a position doesn't start playback, only Play does.
    auto prime_playback_position = [playback_session](std::int32_t target_ms) {
        auto it = std::lower_bound(
            playback_session->events.begin(), playback_session->events.end(),
            target_ms, [](const RecordEvent& event, std::int32_t ms) {
                return std::visit([](auto&& e) { return e.t_ms; }, event) < ms;
            });
        playback_session->next_index =
            static_cast<std::size_t>(it - playback_session->events.begin());
        playback_session->base_ms = target_ms;
    };

    // Declared here since the image dropdown built below needs to read
    // it too, to know whether clicking an entry should load_image()
    // directly (Interactive/Record) or seek Playback to wherever that
    // image occurs in the loaded recording instead — the image shows as
    // a consequence of jumping to the track that plays it.
    // 0=Interactive, 1=Record, 2=Playback.
    auto current_mode = std::make_shared<int>(0);

    // Reassignable shared_ptr<function<>> indirection: the real
    // implementation (close_record()/close_playback(), *current_mode = 0,
    // show_interactive_bar()) isn't assembled until much further down,
    // but the session collection editor's delete-confirmation flow needs
    // to call it too — deleting a session in use for Record or Playback
    // must switch the mode back to Interactive.
    auto return_to_interactive = std::make_shared<std::function<void()>>([] {});

    // Same reassignable indirection — assigned for real inside
    // add_record_controls(), but the image dropdown's row-click handler
    // (built before Record exists as a concept this run) needs to reach
    // it too, so picking an image while actively recording writes the
    // same implicit track change + ImageEvent/ViewEvent go_to_nav_index()
    // writes for Forward/Backward, rather than just updating the
    // display. A no-op by default.
    auto change_record_image_action =
        std::make_shared<std::function<void(const std::string&)>>(
            [](const std::string&) {});

    // Same indirection, assigned once right after the real seek_to() is
    // defined below, so call sites defined *before* seek_to() exists
    // (the track dropdown's row click, clock_label's editing-commit
    // handler) still compile.
    auto playback_seek_to =
        std::make_shared<std::function<void(std::int32_t)>>([](std::int32_t) {});

    // start_playing_action/stop_playing_action: declared here because
    // the permanent Playback engine block below assigns them directly,
    // once, rather than add_playback_controls() reassigning them on
    // every Play-press.
    auto start_playing_action = std::make_shared<std::function<void()>>([] {});
    auto stop_playing_action = std::make_shared<std::function<void()>>([] {});

    // The protocol currently backing image-sequence, if any — empty
    // (default Protocol{}) until "New Protocol"/"Open Protocol…" sets
    // it. Kept separate from image-sequence itself, which stays the
    // plain path list a user builds up, so every existing consumer
    // (this dropdown, Record's nav buttons, Edit Sequence) keeps working
    // unchanged; this only carries the extra title/caption/body a
    // protocol adds on top, reconciled back by "Save Protocol…".
    // Declared here, ahead of show_interactive_bar() below, since that
    // needs to capture it too for a protocol-aware window title even
    // when just returning to Interactive.
    auto active_protocol = std::make_shared<Protocol>();

    // refresh_transport_sensitivity() is declared further down, after
    // clock_label/track_button exist, which it also controls.

    // The track (time) dropdown — permanent and always visible, not
    // rebuilt/torn-down with Playback's action bar. Picking a track in
    // Interactive mode selects it and leaves the app in stop-state;
    // pressing Play starts that track. Reads/writes playback_session
    // directly; only the *widgets* inside add_playback_controls() get
    // rebuilt per Play-press.
    auto format_clock_ms = [](std::int32_t total_ms) {
        int total_seconds = total_ms / 1000;
        int minutes = total_seconds / 60;
        int seconds = total_seconds % 60;
        auto two_digits = [](int n) {
            return (n < 10 ? std::string("0") : std::string()) + std::to_string(n);
        };
        return two_digits(minutes) + ":" + two_digits(seconds);
    };
    auto* track_button = Gtk::make_managed<Gtk::MenuButton>();
    track_button->set_icon_name("pan-down-symbolic");
    track_button->set_tooltip_text(_("No active tracks"));

    auto* track_list = Gtk::make_managed<Gtk::ListBox>();
    track_list->add_css_class("boxed-list");
    track_list->set_selection_mode(Gtk::SelectionMode::NONE);
    auto* track_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    track_scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    track_scroller->set_max_content_height(300);
    track_scroller->set_propagate_natural_height(true);
    track_scroller->set_size_request(220, -1);
    track_scroller->set_child(*track_list);
    track_scroller->set_margin(8);

    // The only header action this popover has — selection-independent
    // (it acts on the whole track list, not "whichever track is
    // current"), so there's nothing to pair it with on the right. Opens
    // show_track_collection_editor() (track_dialogs.cpp): hide/reorder/
    // note, no delete yet. Wired further down, once rebuild_track_rows
    // exists.
    auto* edit_tracks_button = Gtk::make_managed<Gtk::Button>();
    set_button_icon_from_file(edit_tracks_button, "reorder-symbolic.svg");
    edit_tracks_button->set_tooltip_text(_("Edit Tracks…"));
    edit_tracks_button->add_css_class("flat");
    edit_tracks_button->add_css_class("circular");
    edit_tracks_button->add_css_class("blind-sight-round-icon-button");
    edit_tracks_button->set_margin(8);

    // Right-side, selection-dependent (acts on "whichever track is
    // current," same as session/image/project's own Details button) —
    // same icon/styling, paired with Edit Tracks on the left via a
    // hexpand spacer. Opens the single-field Note editor directly for
    // the current track, without going through Edit Tracks first. Wired
    // further down, once current_track_number exists.
    auto* track_details_button = Gtk::make_managed<Gtk::Button>();
    track_details_button->set_icon_name("info-outline-symbolic");
    track_details_button->set_tooltip_text(_("Track Note"));
    track_details_button->add_css_class("flat");
    track_details_button->add_css_class("circular");
    track_details_button->add_css_class("blind-sight-round-icon-button");
    track_details_button->set_margin(8);

    auto* track_header_spacer = Gtk::make_managed<Gtk::Box>();
    track_header_spacer->set_hexpand(true);
    auto* track_header = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    track_header->append(*edit_tracks_button);
    track_header->append(*track_header_spacer);
    track_header->append(*track_details_button);

    auto* track_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 0);
    track_box->append(*track_header);
    track_box->append(*track_scroller);

    auto* track_popover = Gtk::make_managed<Gtk::Popover>();
    track_popover->set_child(*track_box);
    track_button->set_popover(*track_popover);

    // Checkmark widgets from the last rebuild, one per row, in order —
    // lets the 300ms poll below update just checkmark visibility
    // without touching the rows themselves.
    auto track_checkmarks = std::make_shared<std::vector<Gtk::Image*>>();
    // Row index (as GTK sees it, after hidden tracks are skipped and
    // tracks.json's own order is applied) -> raw index into
    // playback_session->track_starts/events — the same "id_from_row"
    // mapping every other popover's filtered list already needs
    // (visible_sessions()/visible_image_paths()), just local here since
    // the track dropdown was never generalized onto collection_menu.hpp.
    auto visible_track_indices = std::make_shared<std::vector<std::size_t>>();

    // 1-based, matching rebuild_track_rows()'s own "Track N" — shared by
    // update_track_checkmarks() below and track_details_button's click
    // handler, so both agree on what "current" means.
    auto current_track_number = [playback_session]() -> int {
        auto now_ms = playback_session->tick_active ? playback_session->elapsed_ms()
                                                      : playback_session->base_ms;
        int track_number = 1;
        for (std::size_t i = 0; i < playback_session->track_starts.size(); ++i) {
            if (playback_session->track_starts[i] <= now_ms) {
                track_number = static_cast<int>(i) + 1;
            } else {
                break;
            }
        }
        return track_number;
    };

    auto update_track_checkmarks = [playback_session, track_checkmarks,
                                     visible_track_indices, current_track_number]() {
        std::size_t current_track_index = static_cast<std::size_t>(current_track_number()) - 1;
        for (std::size_t row = 0; row < track_checkmarks->size(); ++row) {
            bool is_current = row < visible_track_indices->size() &&
                               (*visible_track_indices)[row] == current_track_index;
            (*track_checkmarks)[row]->set_visible(is_current);
        }
    };

    // Rebuilds the rows themselves — only on open, never on the 300ms
    // poll: rebuilding on the poll would tear down and recreate every
    // row out from under the user's click, so a click's press and
    // release could land on two different row objects and never
    // resolve into row-activated at all. Hidden tracks (tracks.json,
    // written by the Edit Tracks dialog, track_dialogs.cpp) are skipped
    // entirely; the rest are shown in their own `order`, defaulting to
    // natural position when untouched — same "no entry yet means
    // default" rule track_dialogs.cpp's own list_ids() already uses.
    auto rebuild_track_rows = [track_list, playback_session,
                                track_checkmarks, update_track_checkmarks,
                                visible_track_indices]() {
        track_list->remove_all();
        track_checkmarks->clear();
        visible_track_indices->clear();

        std::string session_dir = playback_session->session_path.empty()
                                       ? std::string()
                                       : Glib::path_get_dirname(playback_session->session_path);
        TracksIndex tracks_index =
            session_dir.empty() ? TracksIndex{} : read_tracks_index(session_dir);
        auto entry_for_track_number = [&tracks_index](int track_number) -> const TrackEntry* {
            for (const auto& entry : tracks_index.tracks) {
                if (entry.track_number == track_number) {
                    return &entry;
                }
            }
            return nullptr;
        };

        std::size_t total = playback_session->track_starts.size();
        std::vector<std::size_t> order(total);
        for (std::size_t i = 0; i < total; ++i) {
            order[i] = i;
        }
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const TrackEntry* ea = entry_for_track_number(static_cast<int>(a) + 1);
            const TrackEntry* eb = entry_for_track_number(static_cast<int>(b) + 1);
            int oa = ea ? ea->order : static_cast<int>(a);
            int ob = eb ? eb->order : static_cast<int>(b);
            return oa < ob;
        });

        for (std::size_t i : order) {
            const TrackEntry* entry = entry_for_track_number(static_cast<int>(i) + 1);
            if (entry && entry->hidden) {
                continue;
            }
            std::int32_t start = playback_session->track_starts[i];
            std::int32_t end = (i + 1 < total) ? playback_session->track_starts[i + 1]
                                                : std::numeric_limits<std::int32_t>::max();
            TrackImageInfo image_info =
                image_info_for_track(playback_session->events, start, end);
            int track_n = static_cast<int>(i) + 1;
            int total_seconds = start / 1000;
            int minutes = total_seconds / 60;
            std::string seconds = (total_seconds % 60 < 10 ? "0" : "") +
                                   std::to_string(total_seconds % 60);
            // Title is purely the user's own override (TrackEntry::title,
            // set via the Note dialog) — never the live scenario name,
            // which lives in the subtitle instead, below. Collapses to
            // the no-title form when unset — same shape, same rule,
            // Edit Tracks' own row_content() (track_dialogs.cpp) uses;
            // this popover and that list must never disagree.
            Glib::ustring title = (entry && !entry->title.empty())
                                       ? Glib::ustring::compose(_("Track %1 – %2 (%3:%4)"),
                                                                 track_n, entry->title, minutes,
                                                                 seconds)
                                       : Glib::ustring::compose(_("Track %1 (%2:%3)"), track_n,
                                                                 minutes, seconds);
            auto* row = adw_action_row_new();
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
            // Subtitle is the live H2 derivation (image_info.scenario) —
            // a reference to what it's derived from, never stored. Empty
            // (no subtitle at all) when this track has no scenario tie.
            if (!image_info.scenario.empty()) {
                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), image_info.scenario.c_str());
                adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 1);
            }
            g_object_set(row, "activatable", TRUE, nullptr);
            auto* current_icon = Gtk::make_managed<Gtk::Image>();
            current_icon->set_from_icon_name("object-select-symbolic");
            current_icon->set_visible(false);  // update_track_checkmarks() sets this
            adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                       GTK_WIDGET(current_icon->gobj()));
            track_checkmarks->push_back(current_icon);
            visible_track_indices->push_back(i);
            track_list->append(*Glib::wrap(row, false));
        }
        update_track_checkmarks();
    };

    edit_tracks_button->signal_clicked().connect(
        [window, track_popover, playback_session, rebuild_track_rows]() {
            track_popover->popdown();
            if (!playback_session->session_path.empty()) {
                show_track_collection_editor(window, playback_session->session_path,
                                              rebuild_track_rows);
            }
        });

    // Silent no-op guard if there's no session, not a user-visible
    // error — same precedent as project_details_button's own click
    // handler (reaching here with nothing current shouldn't happen via
    // the button itself).
    track_details_button->signal_clicked().connect(
        [window, track_popover, playback_session, current_track_number,
         rebuild_track_rows]() {
            track_popover->popdown();
            if (playback_session->session_path.empty()) {
                return;
            }
            show_track_note_dialog_for_number(window, playback_session->session_path,
                                               current_track_number(), rebuild_track_rows);
        });

    // Polled every 300ms while open, same "live-follows-the-read-head"
    // reasoning as the images dropdown's own sequence_refresh_connection
    // (below) — see its comment.
    auto track_refresh_connection = std::make_shared<sigc::connection>();
    track_popover->signal_show().connect(
        [playback_session, current_mode, reread_track_metadata, rebuild_track_rows,
         update_track_checkmarks, track_refresh_connection]() {
            // Re-reads track/event metadata from disk first (see
            // reread_track_metadata()). Only in Interactive — a
            // concurrent read while Record still has the file open for
            // writing risks catching a partially-written last line, and
            // mid-Playback already has an up to date list.
            if (*current_mode == 0 && !playback_session->session_path.empty()) {
                reread_track_metadata(playback_session->session_path);
            }
            rebuild_track_rows();
            track_refresh_connection->disconnect();
            *track_refresh_connection = Glib::signal_timeout().connect(
                [update_track_checkmarks]() {
                    update_track_checkmarks();
                    return true;
                },
                300);
        });
    track_popover->signal_closed().connect(
        [track_refresh_connection]() { track_refresh_connection->disconnect(); });

    track_list->signal_row_activated().connect(
        [playback_session, current_mode, playback_seek_to, prime_playback_position,
         track_popover, visible_track_indices](Gtk::ListBoxRow* row) {
            // row->get_index() is this row's position in the *filtered*
            // list rebuild_track_rows() actually built — visible_track_
            // indices maps that back to track_starts' own raw index.
            auto row_index = static_cast<std::size_t>(row->get_index());
            if (row_index < visible_track_indices->size()) {
                std::size_t index = (*visible_track_indices)[row_index];
                if (index < playback_session->track_starts.size()) {
                    std::int32_t target_ms = playback_session->track_starts[index];
                    if (*current_mode == 2) {
                        // Actually playing — the real seek_to() (visual
                        // marker/viewport restore, resumes ticking if
                        // appropriate) is live via playback_seek_to.
                        (*playback_seek_to)(target_ms);
                    } else {
                        prime_playback_position(target_ms);
                    }
                }
            }
            track_popover->popdown();
        });

    // clock_label — permanent too, right next to track_button, shared
    // across all three modes. Record's ticker still writes its elapsed
    // time into this same widget, it just can't be clicked into edit
    // mode while insensitive (refresh_transport_sensitivity() gates
    // that on current_mode == 1).
    auto* clock_label = Gtk::make_managed<Gtk::EditableLabel>("00:00");
    clock_label->add_css_class("monospace");
    clock_label->add_css_class("blind-sight-clock-label");
    clock_label->set_valign(Gtk::Align::CENTER);
    clock_label->set_width_chars(5);
    clock_label->set_max_width_chars(5);
    clock_label->set_alignment(0.5f);

    // parse_clock() — format_clock_ms()'s inverse, "MM:SS" (or bare
    // "SS") to total milliseconds. std::nullopt on anything that
    // doesn't parse.
    auto parse_clock = [](const Glib::ustring& text) -> std::optional<std::int32_t> {
        std::string s = text.raw();
        auto colon = s.find(':');
        try {
            std::size_t consumed = 0;
            int minutes = 0;
            int seconds;
            if (colon == std::string::npos) {
                seconds = std::stoi(s, &consumed);
                if (consumed != s.size()) return std::nullopt;
            } else {
                minutes = std::stoi(s.substr(0, colon), &consumed);
                if (consumed != colon) return std::nullopt;
                std::string secs = s.substr(colon + 1);
                seconds = std::stoi(secs, &consumed);
                if (consumed != secs.size()) return std::nullopt;
            }
            if (minutes < 0 || seconds < 0) return std::nullopt;
            return (minutes * 60 + seconds) * 1000;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    // Permanent editing-commit handler, connected once. Mode-aware the
    // same way the track dropdown's row click is: a real seek while
    // actually playing, otherwise just priming the position for next
    // Play.
    clock_label->property_editing().signal_changed().connect(
        [clock_label, parse_clock, playback_session, current_mode,
         playback_seek_to, prime_playback_position, format_clock_ms]() {
            if (clock_label->get_editing()) {
                // Strip down to a bare, directly-editable "mm:ss" — the
                // display form ("Track n – mm:ss") isn't something a
                // user should have to type or delete around.
                clock_label->set_text(format_clock_ms(
                    playback_session->tick_active ? playback_session->elapsed_ms()
                                                    : playback_session->base_ms));
                return;
            }
            if (auto target_ms = parse_clock(clock_label->get_text())) {
                if (*current_mode == 2) {
                    (*playback_seek_to)(*target_ms);
                } else {
                    prime_playback_position(*target_ms);
                    clock_label->set_text(format_clock_ms(*target_ms));
                }
            } else {
                clock_label->set_text(format_clock_ms(
                    playback_session->tick_active ? playback_session->elapsed_ms()
                                                    : playback_session->base_ms));
            }
        });

    // Not .linked — that class merges adjacent *buttons'* borders into
    // one group, wrong for a text field paired with a button: it made
    // track_button render as an ordinary non-flat button and
    // clock_label's edit-mode entry render with the wrong background.
    // The other dropdowns get AdwHeaderBar's automatic flat-button
    // styling for free because they're packed directly into it;
    // track_button is one level deeper, so it needs flattening added
    // explicitly. No spacing — a gap would leave track_button's
    // square-cornered left edge floating instead of joined onto
    // clock_label's right edge.
    auto* clock_group =
        Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
    track_button->add_css_class("flat");
    track_button->add_css_class("blind-sight-clock-track-button");
    clock_group->append(*clock_label);
    clock_group->append(*track_button);

    // The Playback tick engine, permanent for the app's lifetime, not
    // rebuilt per Play-press: FF/FB need to scrub from Interactive/
    // "stop" as well as from Play.
    auto place_marker = [](Gtk::Widget* marker, Gtk::Widget* picture,
                            std::uint16_t x, std::uint16_t y) {
        double fx = x / 65535.0;
        double fy = y / 65535.0;
        marker->set_margin_start(
            static_cast<int>(fx * picture->get_width()) - marker->get_width() / 2);
        marker->set_margin_top(
            static_cast<int>(fy * picture->get_height()) - marker->get_height() / 2);
        marker->set_visible(true);
    };
    auto ensure_marker_ready = [playback_session]() {
        int diameter = gaze_cursor_diameter_px(*playback_session->image);
        playback_session->marker->set_size_request(diameter, diameter);
        playback_session->marker->set_paintable(render_gaze_circle_texture(diameter));
    };
    auto restore_marker = [playback_session, place_marker](std::size_t index) {
        playback_session->marker->set_visible(false);
        for (std::size_t i = index; i-- > 0;) {
            const auto& event = playback_session->events[i];
            if (auto* gaze = std::get_if<GazeEvent>(&event)) {
                place_marker(playback_session->marker,
                              playback_session->marker->get_parent(), gaze->x, gaze->y);
                return;
            } else if (auto* prod = std::get_if<ProdEvent>(&event)) {
                place_marker(playback_session->marker,
                              playback_session->marker->get_parent(), prod->x, prod->y);
                return;
            } else if (std::holds_alternative<LeaveEvent>(event)) {
                return;
            }
        }
    };
    auto restore_viewport = [playback_session, window, active_protocol,
                              update_sequence_button_label](std::size_t index) {
        std::optional<std::string> image_path;
        std::optional<std::pair<std::int32_t, std::int32_t>> view;
        for (std::size_t i = index; i-- > 0 && (!image_path || !view);) {
            const auto& event = playback_session->events[i];
            if (!image_path) {
                if (auto* img = std::get_if<ImageEvent>(&event)) {
                    image_path = img->path;
                } else if (std::holds_alternative<WaitEvent>(event)) {
                    image_path = "";
                }
            }
            if (!view) {
                if (auto* v = std::get_if<ViewEvent>(&event)) {
                    view = {v->yaw, v->pitch};
                }
            }
        }
        load_playback_image(playback_session, image_path.value_or(""));
        update_sequence_button_label(playback_session->current_image_path);
        if (view) {
            playback_session->view_yaw = view->first;
            playback_session->view_pitch = view->second;
        }
        render_playback_view(playback_session);
    };

    // Playback's read-only reconstruction of "which step was current"
    // from the logged TestStepEvents, never writing one or driving the
    // walkthrough itself. A no-op whenever the Protocol window isn't
    // open. show_test_step_event() resolves one event directly (the
    // forward-dispatch tick below already has it); restore_test_step_
    // display() is the backward-scan version, same "latest matching
    // event <= now" idiom restore_viewport() uses, needed for
    // seek_to()/scrubbing where no single event is "the one just
    // reached."
    auto show_test_step_event = [active_protocol](const TestStepEvent* event) {
        if (!g_protocol_window || !g_protocol_window->get_visible() ||
            !g_protocol_nav_view) {
            return;
        }
        if (!event) {
            AdwNavigationPage* page = build_protocol_message_page(
                _("Protocol"),
                _("No test step has been recorded yet at this position."));
            AdwNavigationPage* stack[] = {page};
            adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
            return;
        }
        const ProtocolEntry* entry = nullptr;
        for (const auto& candidate : active_protocol->entries) {
            if (candidate.title == event->scenario) {
                entry = &candidate;
                break;
            }
        }
        const TestStep* step = nullptr;
        ScenarioPages pages;
        if (entry) {
            pages = parse_scenario_pages(entry->body);
            for (const auto& candidate : pages.steps) {
                if (candidate.title == event->step) {
                    step = &candidate;
                    break;
                }
            }
        }
        if (!step) {
            AdwNavigationPage* page = build_protocol_message_page(
                _("Protocol"), _("This test step is no longer in protocol.md."));
            AdwNavigationPage* stack[] = {page};
            adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
            return;
        }
        AdwNavigationPage* page = build_protocol_step_page_readonly(*step, event->type);
        AdwNavigationPage* stack[] = {page};
        adw_navigation_view_replace(g_protocol_nav_view, stack, 1);
    };
    auto restore_test_step_display = [playback_session, show_test_step_event](
                                           std::size_t index) {
        const TestStepEvent* found = nullptr;
        for (std::size_t i = index; i-- > 0;) {
            if (auto* step_event =
                    std::get_if<TestStepEvent>(&playback_session->events[i])) {
                found = step_event;
                break;
            }
        }
        show_test_step_event(found);
    };

    auto refresh_clock_display = [clock_label, playback_session, format_clock_ms]() {
        auto ms = playback_session->tick_active ? playback_session->elapsed_ms()
                                                 : playback_session->base_ms;
        clock_label->set_text(format_clock_ms(ms));
    };
    auto format_wait_clock = [format_clock_ms](int wait_n, std::int32_t duration_ms) {
        return std::string(_("Wait")) + " " + std::to_string(wait_n) + " – " +
               format_clock_ms(duration_ms);
    };
    auto wait_resume_connection = std::make_shared<sigc::connection>();
    auto start_ticking = std::make_shared<std::function<void(double)>>();

    auto seek_to = [playback_session, restore_marker, restore_viewport,
                     restore_test_step_display, ensure_marker_ready, refresh_clock_display,
                     wait_resume_connection, start_ticking](std::int32_t target_ms) {
        bool resume_playing = playback_session->tick_active ||
                               wait_resume_connection->connected();
        wait_resume_connection->disconnect();
        auto it = std::lower_bound(
            playback_session->events.begin(), playback_session->events.end(), target_ms,
            [](const RecordEvent& event, std::int32_t ms) {
                return std::visit([](auto&& e) { return e.t_ms; }, event) < ms;
            });
        playback_session->next_index =
            static_cast<std::size_t>(it - playback_session->events.begin());
        playback_session->base_ms = target_ms;
        playback_session->started = std::chrono::steady_clock::now();

        ensure_marker_ready();
        restore_marker(playback_session->next_index);
        restore_viewport(playback_session->next_index);
        restore_test_step_display(playback_session->next_index);
        refresh_clock_display();
        if (resume_playing) {
            (*start_ticking)(playback_session->speed);
        }
    };
    *playback_seek_to = seek_to;

    auto update_active_buttons = [playback_session, play_button, fast_forward_button,
                                   fast_backward_button]() {
        sync_playback_audio(playback_session);
        play_button->remove_css_class("blind-sight-playback-active");
        fast_forward_button->remove_css_class("blind-sight-playback-active");
        fast_backward_button->remove_css_class("blind-sight-playback-active");
        if (!playback_session->tick_active) {
            return;
        }
        if (playback_session->speed < 0) {
            fast_backward_button->add_css_class("blind-sight-playback-active");
        } else if (playback_session->speed > 1.0) {
            fast_forward_button->add_css_class("blind-sight-playback-active");
        } else {
            play_button->add_css_class("blind-sight-playback-active");
        }
    };

    *start_ticking = [playback_session, clock_label, format_clock_ms, format_wait_clock,
                       place_marker, restore_marker, restore_viewport,
                       restore_test_step_display, show_test_step_event,
                       update_active_buttons, start_ticking, wait_resume_connection,
                       window, active_protocol, settings, update_sequence_button_label,
                       stop_button](double new_speed) {
        wait_resume_connection->disconnect();
        playback_session->speed = new_speed;
        playback_session->started = std::chrono::steady_clock::now();

        if (playback_session->tick_active) {
            playback_session->tick_host->remove_tick_callback(playback_session->tick_id);
            playback_session->tick_active = false;
        }
        playback_session->tick_id = playback_session->tick_host->add_tick_callback(
            [playback_session, clock_label, format_clock_ms, format_wait_clock,
             place_marker, restore_marker, restore_viewport, restore_test_step_display,
             show_test_step_event, update_active_buttons,
             start_ticking, wait_resume_connection, window, active_protocol, settings,
             update_sequence_button_label,
             stop_button](const Glib::RefPtr<Gdk::FrameClock>&) {
                auto now_ms = playback_session->elapsed_ms();

                const TranscriptCue* cue =
                    transcript_cue_for_ms(playback_session->transcript_cues, now_ms);
                if (cue) {
                    playback_session->subtitle_label->set_text(cue->text);
                    playback_session->subtitle_label->set_visible(true);
                } else {
                    playback_session->subtitle_label->set_visible(false);
                }

                bool viewport_dirty = false;
                while (playback_session->next_index < playback_session->events.size()) {
                    const auto& event = playback_session->events[playback_session->next_index];
                    auto event_t_ms =
                        std::visit([](auto&& e) { return e.t_ms; }, event);
                    if (event_t_ms > now_ms) {
                        break;
                    }
                    if (auto* gaze = std::get_if<GazeEvent>(&event)) {
                        place_marker(playback_session->marker,
                                      playback_session->marker->get_parent(), gaze->x,
                                      gaze->y);
                    } else if (auto* prod = std::get_if<ProdEvent>(&event)) {
                        place_marker(playback_session->marker,
                                      playback_session->marker->get_parent(), prod->x,
                                      prod->y);
                        show_prod_cue(playback_session->prod_cue, playback_session->image,
                                      settings, prod->x, prod->y);
                    } else if (std::holds_alternative<LeaveEvent>(event)) {
                        playback_session->marker->set_visible(false);
                    } else if (auto* image = std::get_if<ImageEvent>(&event)) {
                        load_playback_image(playback_session, image->path);
                        update_sequence_button_label(playback_session->current_image_path);
                        viewport_dirty = false;
                    } else if (auto* view = std::get_if<ViewEvent>(&event)) {
                        playback_session->view_yaw = view->yaw;
                        playback_session->view_pitch = view->pitch;
                        viewport_dirty = true;
                    } else if (auto* step_event = std::get_if<TestStepEvent>(&event)) {
                        // Reached straight from hand — no need for
                        // restore_test_step_display()'s own backward
                        // scan, this *is* the event it would have found.
                        show_test_step_event(step_event);
                    } else if (auto* wait = std::get_if<WaitEvent>(&event)) {
                        // Only a *real* gap (duration_ms > 0, an actual
                        // Stop) freezes playback and shows "Wait N" for
                        // kWaitDisplaySeconds. An implicit, zero-duration
                        // boundary (an in-recording image change,
                        // write_implicit_track_change()) has nothing to
                        // represent: freezing on "No image" for 2 real
                        // seconds before the new track's own ImageEvent
                        // (at this same t_ms) runs would otherwise show
                        // the wrong image. A real Wait still falls
                        // through to "No image" too — only the
                        // freeze-and-resume is conditional.
                        if (wait->duration_ms > 0) {
                            load_playback_image(playback_session, "");
                            update_sequence_button_label("");

                            auto it = std::find(playback_session->track_starts.begin(),
                                                 playback_session->track_starts.end(),
                                                 event_t_ms);
                            int wait_n = static_cast<int>(
                                it - playback_session->track_starts.begin());
                            clock_label->set_text(
                                format_wait_clock(wait_n, wait->duration_ms));

                            if (playback_session->speed == 1.0) {
                                ++playback_session->next_index;
                                playback_session->base_ms = event_t_ms;
                                playback_session->tick_active = false;
                                update_active_buttons();
                                constexpr int kWaitDisplaySeconds = 2;
                                *wait_resume_connection =
                                    Glib::signal_timeout().connect_seconds(
                                        [start_ticking]() {
                                            (*start_ticking)(1.0);
                                            return false;
                                        },
                                        kWaitDisplaySeconds);
                                return false;
                            }
                        }
                    }
                    ++playback_session->next_index;
                }
                if (viewport_dirty) {
                    render_playback_view(playback_session);
                }

                bool moved_back = false;
                while (playback_session->next_index > 0) {
                    auto prev_t_ms = std::visit(
                        [](auto&& e) { return e.t_ms; },
                        playback_session->events[playback_session->next_index - 1]);
                    if (prev_t_ms <= now_ms) {
                        break;
                    }
                    --playback_session->next_index;
                    moved_back = true;
                }
                if (moved_back) {
                    restore_marker(playback_session->next_index);
                    restore_viewport(playback_session->next_index);
                    restore_test_step_display(playback_session->next_index);
                }

                if (playback_session->next_index == 0 && now_ms < 0) {
                    if (!clock_label->get_editing()) {
                        clock_label->set_text(format_clock_ms(0));
                    }
                    playback_session->marker->set_visible(false);
                    playback_session->tick_active = false;
                    update_active_buttons();
                    return false;
                }

                if (!clock_label->get_editing()) {
                    clock_label->set_text(format_clock_ms(now_ms));
                }
                if (playback_session->next_index >= playback_session->events.size()) {
                    playback_session->tick_active = false;
                    stop_button->activate();
                    return false;
                }
                sync_playback_audio(playback_session);
                return true;
            });
        playback_session->tick_active = true;
        update_active_buttons();
    };

    // Play's own pause/resume toggle — permanent, assigned once rather
    // than reassigned by add_playback_controls() every Play-press.
    auto playback_pause_or_resume = [ensure_marker_ready, start_ticking, playback_session,
                                      wait_resume_connection, update_active_buttons]() {
        if (playback_session->tick_active) {
            playback_session->base_ms = playback_session->elapsed_ms();
            playback_session->tick_host->remove_tick_callback(playback_session->tick_id);
            playback_session->tick_active = false;
            wait_resume_connection->disconnect();
            update_active_buttons();
            return;
        }
        ensure_marker_ready();
        (*start_ticking)(playback_session->speed);
    };
    *start_playing_action = playback_pause_or_resume;

    auto playback_stop_and_reset = [playback_session, clock_label, update_active_buttons,
                                     format_clock_ms, wait_resume_connection]() {
        wait_resume_connection->disconnect();
        if (playback_session->tick_active) {
            playback_session->tick_host->remove_tick_callback(playback_session->tick_id);
            playback_session->tick_active = false;
        }
        playback_session->next_index = 0;
        playback_session->base_ms = 0;
        playback_session->speed = 1.0;
        if (playback_session->marker) {
            playback_session->marker->set_visible(false);
        }
        if (playback_session->subtitle_label) {
            playback_session->subtitle_label->set_visible(false);
        }
        clock_label->set_text(format_clock_ms(0));
        update_active_buttons();
    };
    *stop_playing_action = playback_stop_and_reset;

    // Sensitivity rules: Interactive (nothing pressed) — Record/Play
    // sensitive only once a session is loaded; Stop/FB/FF insensitive;
    // clock_label/track_button sensitive (picking a track or typing a
    // time while stopped primes the position for next Play). Record
    // pressed — Record itself stays sensitive as the pause/resume
    // toggle; FB/FF/clock_label/track_button all insensitive, though
    // Record's ticker keeps writing clock_label's text regardless,
    // insensitive only blocking clicks into it. Play pressed — Play
    // stays sensitive, same pause/resume reasoning; clock_label/
    // track_button/FB/FF all stay sensitive (seeking/picking a track
    // while playing is the main use case).
    //
    // Session Details implicitly acts on "whichever session is currently
    // active," a selection-dependent action — built later, via
    // show_session_collection_menu() (session_dialogs.cpp), once
    // session_recent_button/toast_overlay/etc. exist. Forward reference
    // (same pattern as project_details_button_ptr above): captured here
    // so refresh_transport_sensitivity() can gate it on has_session too,
    // but only ever dereferenced later, at call time, well after the
    // adapter call below fills it in.
    auto session_details_button_ptr = std::make_shared<Gtk::Button*>(nullptr);

    auto refresh_transport_sensitivity =
        [record_button, play_button, stop_button, fast_backward_button,
         fast_forward_button, clock_label, track_button, current_mode,
         recording_session, session_details_button_ptr, matrix_button]() {
            bool has_session = !recording_session->session_path.empty();
            record_button->set_sensitive(has_session &&
                                          (*current_mode == 0 || *current_mode == 1));
            play_button->set_sensitive(has_session &&
                                        (*current_mode == 0 || *current_mode == 2));
            stop_button->set_sensitive(*current_mode != 0);
            fast_backward_button->set_sensitive(has_session && *current_mode != 1);
            fast_forward_button->set_sensitive(has_session && *current_mode != 1);
            clock_label->set_sensitive(has_session && *current_mode != 1);
            track_button->set_sensitive(has_session && *current_mode != 1);
            if (*session_details_button_ptr) {
                (*session_details_button_ptr)->set_sensitive(has_session);
            }
            // doc (protocol_button) and matrix_button both stay ungated
            // by mode — neither reading the protocol nor configuring
            // who-hears-whom has a mode precondition.
            matrix_button->set_sensitive(has_session);
        };

    // The session dropdown — a single, always-visible control shared by
    // Record and Playback. New/Open Session, the recents list, and
    // Session Details all funnel through apply_shared_session() above,
    // writing both recording_session's and playback_session's loaded
    // state together.
    auto* session_recent_button = Gtk::make_managed<Gtk::MenuButton>();
    auto* session_recent_button_label = setup_dropdown_button(session_recent_button);
    session_recent_button_label->set_text(
        session_button_label(RecordMeta{}, "", settings));
    session_recent_button->set_tooltip_text(_("No active sessions"));

    // Caps project_button/session_recent_button/sequence_button's label
    // width at roughly a third each of 40% of the window's current
    // width, ellipsizing past that instead of growing into the title
    // area. No plain GTK4 "on resize" signal for a bare window, so this
    // is checked every frame — cheap, and a no-op whenever the computed
    // character budget hasn't changed since the last tick.
    constexpr double kHeaderDropdownWidthFraction = 0.40;
    auto last_dropdown_max_chars = std::make_shared<int>(-1);
    window->add_tick_callback(
        [window, project_button_label, session_recent_button_label,
         sequence_button_label,
         last_dropdown_max_chars](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            int window_width = window->get_width();
            if (window_width <= 0) {
                return true;
            }
            double avg_char_px = label_avg_char_width_px(project_button_label);
            if (avg_char_px <= 0) {
                return true;
            }
            int budget_px = static_cast<int>(
                window_width * kHeaderDropdownWidthFraction / 3.0);
            int max_chars = std::max(
                4, static_cast<int>(budget_px / avg_char_px));
            if (max_chars == *last_dropdown_max_chars) {
                return true;
            }
            *last_dropdown_max_chars = max_chars;
            project_button_label->set_max_width_chars(max_chars);
            session_recent_button_label->set_max_width_chars(max_chars);
            sequence_button_label->set_max_width_chars(max_chars);
            return true;
        });

    // Small helper — every call site below does the exact same "read
    // the file's meta back, show it on the button face" step right
    // after a successful apply_shared_session().
    auto refresh_session_button_label = [session_recent_button, session_recent_button_label,
                                          settings](const std::string& path) {
        RecordMeta meta;
        read_record_meta(path, meta);
        session_recent_button_label->set_text(
            session_title_or_fallback(meta, path, settings));
        // The menu button's own tooltip — same subtitle text the
        // current session's row would show in its popover
        // (session_creator_contributor_text()), not a generic label, so
        // hovering the button previews the active session without
        // opening the popover. Falls back to the generic collection
        // name when there's nothing more specific to show (including no
        // session being current at all, same as path being empty).
        std::string subtitle = session_creator_contributor_text(meta, path);
        session_recent_button->set_tooltip_text(!subtitle.empty() ? subtitle : _("No active sessions"));
    };

    // The `--session <file>` command-line handler — a file, not a
    // directory to search, so it goes straight to apply_shared_session()
    // with no resolution step first. Success locks session_recent_button
    // insensitive; failure shows its own error dialog and leaves it
    // sensitive, since the user has to go find the right file.
    g_open_session_by_path = [window, apply_shared_session,
                               refresh_session_button_label,
                               refresh_transport_sensitivity,
                               session_recent_button](const std::string& path) {
        if (apply_shared_session(path)) {
            refresh_session_button_label(path);
            refresh_transport_sensitivity();
            session_recent_button->set_sensitive(false);
        } else {
            show_error_dialog(
                window, _("Can't Open Recording"),
                _("This file isn't a recording this version of Blind "
                  "Sight understands — it may be corrupted, or written "
                  "by an incompatible version."));
        }
    };

    // New Session — creates a new session inside the active project's
    // sessions/ directory. No file dialog: the id, directory, and .bs
    // path are all generated. Requires a project; the id and directory
    // aren't created until the meta dialog is confirmed, so a cancelled
    // "New Session" leaves nothing behind. Factored out into
    // g_start_new_session so the session collection editor's plus icon
    // can trigger the identical flow without this dropdown's popover
    // needing to be open.
    g_start_new_session = [window, active_protocol, apply_shared_session,
                            refresh_session_button_label,
                            refresh_transport_sensitivity](std::function<void()> on_done) {
        std::string project_dir = project_dir_from_protocol(active_protocol);
        if (project_dir.empty()) {
            show_error_dialog(window, _("No Project"),
                                _("Open or create a project first — "
                                  "sessions always live inside a project."));
            return;
        }
        RecordMeta initial;
        initial.date = Glib::DateTime::create_now_local().format_iso8601();
        // Pre-filled from the active protocol, if any.
        if (!active_protocol->title.empty()) {
            initial.title = active_protocol->title;
            initial.protocol_file = active_protocol->source_path;
        }
        show_record_meta_dialog(
            window, initial,
            [project_dir, apply_shared_session, refresh_session_button_label,
             refresh_transport_sensitivity, on_done](const RecordMeta& meta) {
                std::string id = generate_session_id(project_dir);
                std::string path = session_file_path(project_dir, id);
                std::filesystem::create_directories(
                    std::filesystem::path(path).parent_path());
                if (!write_record_meta(path, meta) ||
                    !apply_shared_session(path)) {
                    return;
                }
                auto index = read_sessions_index(project_dir);
                SessionEntry entry;
                entry.id = id;
                entry.order = static_cast<int>(index.sessions.size());
                entry.hidden = false;
                index.sessions.push_back(entry);
                write_sessions_index(project_dir, index);
                refresh_session_button_label(path);
                refresh_transport_sensitivity();
                if (on_done) {
                    on_done();
                }
            });
    };

    // Autoload-session is attempted further down (reapply_session_
    // autoload, after project-autoload runs) — it requires a project to
    // already be active, not known yet at this point.

    // pack_start: session_recent_button didn't exist yet at
    // sequence_button's own construction point above and needs to land
    // between project_button and sequence_button — pack_start() lands
    // each subsequent call closer to center, so this order gives
    // left-to-right: project_button, session_recent_button,
    // sequence_button.
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                              GTK_WIDGET(session_recent_button->gobj()));
    adw_header_bar_pack_start(ADW_HEADER_BAR(header_bar),
                              GTK_WIDGET(sequence_button->gobj()));

    // nav_group (Previous/Next) sits between the clock and the
    // transport toolbar, as its own group rather than merged into
    // either, since it manipulates both transport and images.
    // pack_end()'s "each later call lands closer to center" rule means
    // the call order needs to be transport_group (already packed) then
    // nav_group then clock_group for left-to-right: clock_group,
    // nav_group, transport_group, menu_button.
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(nav_group->gobj()));
    adw_header_bar_pack_end(ADW_HEADER_BAR(header_bar),
                             GTK_WIDGET(clock_group->gobj()));

    // The transport buttons, project_button, and sequence_button are
    // all packed once and stay packed across all three modes — nothing
    // in the header bar is Interactive-only. Record and Playback both
    // keep sequence_button's label live-synced as they run, but
    // returning *to* Interactive still needs its own explicit resync:
    // the label is one shared piece of text, last written by whichever
    // mode was running, and recording_session's current_image_path
    // isn't necessarily what that was.
    // The image dropdown's popover (visible image sequence, Import/Edit
    // Collection/Details) is built later, via show_image_collection_menu()
    // (image_dialogs.cpp) — once load_image()/activate_image()/
    // toast_overlay exist, below — rather than inline here.
    // image_details_button_ptr is a forward reference (same pattern as
    // the other two popovers' Details buttons): the per-frame tick
    // callback a few lines down captures it now, but only ever
    // dereferences it later, well after the adapter call fills it in.
    auto image_details_button_ptr = std::make_shared<Gtk::Button*>(nullptr);

    Gtk::Widget* splash_caption = nullptr;
    Gtk::Picture* splash_picture = nullptr;
    Gtk::Picture* no_image_picture = nullptr;
    Gtk::Picture* head_direction_cross = nullptr;
    Gtk::Picture* prod_cue = nullptr;
    // Created here, not inside build_splash(), so this one instance can
    // be shared with the key/Dial/scroll handlers further down too (see
    // touch_head_movement()).
    auto last_head_movement_at =
        std::make_shared<std::chrono::steady_clock::time_point>();
    auto* toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar_view, header_bar);
    adw_toolbar_view_set_content(
        toolbar_view, build_splash(recording_session, playback_session,
                                    settings, last_head_movement_at,
                                    noise_pulse_engine, &splash_caption,
                                    &splash_picture, &no_image_picture,
                                    &head_direction_cross, &prod_cue)
                          ->gobj());
    g_splash_caption = splash_caption;

    // Real AdwToastOverlay — for transient, auto-dismissing notices,
    // distinct from the persistent Interactive/Record/Playback action
    // bars. Wraps the whole toolbar_view, not just its content, so a
    // toast floats over everything regardless of which bar is showing.
    auto* toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(toast_overlay, GTK_WIDGET(toolbar_view));
    adw_application_window_set_content(adw_win, GTK_WIDGET(toast_overlay));

    // protocol_button's click handler — the button is permanent and
    // visible in every mode, so this resolves whichever of
    // recording_session/playback_session is actually current.
    //
    // A single click mirrors whatever image is already on screen; a
    // *double*-click instead force-restarts the whole walkthrough from
    // page one via start_protocol_walkthrough(). Either way, re-reads
    // protocol.md from disk first if the window's already open —
    // externally authored, so a repeat click is the only way to pick up
    // changes made since it was last parsed.
    //
    // Playback is a separate case: it never drives the walkthrough or
    // mirrors via image/slug lookup, it just shows Playback's read-only
    // reconstruction of the current step from the event log
    // (restore_test_step_display()) — both click counts do the same
    // thing there.
    auto reload_protocol_if_open = [active_protocol]() {
        std::string project_dir = project_dir_from_protocol(active_protocol);
        if (g_protocol_window && g_protocol_window->get_visible() &&
            !project_dir.empty()) {
            if (auto parsed =
                    parse_protocol_file(project_protocol_path(project_dir))) {
                *active_protocol = *parsed;
            }
        }
    };
    protocol_button->signal_clicked().connect(
        [active_protocol, recording_session, playback_session, current_mode,
         reload_protocol_if_open, restore_test_step_display]() {
            reload_protocol_if_open();
            ensure_protocol_window();
            if (*current_mode == 2) {
                restore_test_step_display(playback_session->next_index);
                g_protocol_window->present();
                return;
            }
            std::string project_dir = project_dir_from_protocol(active_protocol);
            refresh_protocol_window(project_dir, active_protocol,
                                      recording_session->current_image_path,
                                      recording_session, current_mode);
            g_protocol_window->present();
        });
    auto protocol_double_click = Gtk::GestureClick::create();
    protocol_double_click->signal_pressed().connect(
        [active_protocol, recording_session, playback_session, current_mode,
         reload_protocol_if_open, restore_test_step_display](int n_press, double, double) {
            if (n_press < 2) {
                return;
            }
            reload_protocol_if_open();
            ensure_protocol_window();
            if (*current_mode == 2) {
                restore_test_step_display(playback_session->next_index);
                g_protocol_window->present();
                return;
            }
            start_protocol_walkthrough(active_protocol, recording_session,
                                         current_mode);
            g_protocol_window->present();
        });
    protocol_button->add_controller(protocol_double_click);

    // The session dropdown's popover — see show_session_collection_menu()
    // (session_dialogs.cpp) for what it builds. session_details_button_ptr
    // was declared earlier, alongside refresh_transport_sensitivity();
    // this is where it actually gets filled in.
    auto session_menu = show_session_collection_menu(
        session_recent_button, window, settings, active_protocol,
        [recording_session]() { return recording_session->session_path; },
        apply_shared_session, refresh_session_button_label, refresh_transport_sensitivity,
        [recording_session, playback_session, current_mode, return_to_interactive,
         refresh_session_button_label, refresh_transport_sensitivity]() {
            // Deleting the session currently loaded into Record/Playback/
            // shown on the dropdown's label — its directory is gone, so
            // there's nothing left to record into, play back, or point
            // the dropdown at. Checked regardless of mode.
            if (!recording_session->session_path.empty() &&
                !std::filesystem::exists(recording_session->session_path)) {
                if (*current_mode != 0) {
                    (*return_to_interactive)();
                }
                recording_session->session_path.clear();
                playback_session->session_path.clear();
                refresh_session_button_label("");
                refresh_transport_sensitivity();
            }
        },
        toast_overlay);
    *session_details_button_ptr = session_menu.session_details_button;

    // load_image() and refresh_sequence_list() call each other (opening
    // an image updates the list; clicking a list row opens an image) —
    // the forward-declaration breaks that cycle. Needs splash_picture,
    // so this can't be wired until after build_splash().
    //
    // shared_ptr<function<>>, not a plain std::function local captured
    // by reference — load_image() and everything below is stored in
    // GTK signal handlers and a FileDialog's async completion callback,
    // all of which outlive on_activate() itself. A `&`-captured
    // reference to this local was already dangling by the time any of
    // those ran; it "worked" during testing only because nothing had
    // overwritten that stack slot yet. The heap-allocated function
    // object here is captured *by value* everywhere below (copying the
    // shared_ptr), so it stays alive for as long as those callbacks do.
    auto refresh_sequence_list = std::make_shared<std::function<void()>>();

    // Re-renders recording_session->equirect_source at the current
    // view_yaw/pitch — shared by load_image() and the arrow-key handler
    // below. A no-op for a flat image.
    //
    // fast=true renders at kEquirectFastRenderDivisor-reduced resolution
    // (arrow-key presses, potentially still repeating/held) rather than
    // the widget's full size (everywhere else, including the matching
    // key-*release*). last_render_width/height always records the
    // *full* target size regardless — a fast render doesn't change what
    // the widget is actually allocated at, so the resize tick callback
    // below must keep comparing against that.
    auto render_current_view = [splash_picture, recording_session,
                                 head_direction_cross](bool fast = false) {
        if (!recording_session->viewing_equirect || !recording_session->equirect_source) {
            if (head_direction_cross) {
                head_direction_cross->set_visible(false);
            }
            return;
        }
        int out_width =
            splash_picture->get_width() > 0 ? splash_picture->get_width() : 1024;
        int out_height =
            splash_picture->get_height() > 0 ? splash_picture->get_height() : 640;
        int render_width = out_width;
        int render_height = out_height;
        if (fast) {
            render_width = std::max(out_width / kEquirectFastRenderDivisor,
                                     kEquirectFastRenderMinSize);
            render_height = std::max(out_height / kEquirectFastRenderDivisor,
                                      kEquirectFastRenderMinSize);
        }
        auto rendered = render_equirect_viewport(
            recording_session->equirect_source, render_width, render_height,
            kViewportFovDegrees, recording_session->view_yaw / 100.0,
            recording_session->view_pitch / 100.0,
            recording_session->panorama_projection);
        splash_picture->set_paintable(Gdk::Texture::create_for_pixbuf(rendered));
        recording_session->last_render_width = out_width;
        recording_session->last_render_height = out_height;

        // No depth map, no gaze marker — see session_has_depth().
        // Panning itself is unaffected; only the cross's visibility is.
        if (head_direction_cross && session_has_depth(*recording_session)) {
            int width = gaze_cursor_diameter_px(*splash_picture);
            int viewport_height = std::max(splash_picture->get_height(), 1);
            int height = head_direction_cross_height_px(viewport_height,
                                                          kViewportFovDegrees);
            int top_offset = elevation_to_pixel_offset(
                kHeadCrossTopElevationDeg, viewport_height, kViewportFovDegrees);
            head_direction_cross->set_size_request(width, height);
            head_direction_cross->set_paintable(render_head_direction_cross_texture(
                width, viewport_height, kViewportFovDegrees));
            head_direction_cross->set_margin_start(
                (std::max(splash_picture->get_width(), 1) - width) / 2);
            head_direction_cross->set_margin_top(viewport_height / 2 + top_offset);
            head_direction_cross->set_visible(true);
        } else if (head_direction_cross) {
            head_direction_cross->set_visible(false);
        }
    };

    // Re-render on an actual size change (window resize, or F11
    // fullscreen) — without this, fullscreen would stretch the old,
    // wrong-size render instead of re-synthesizing the viewport, which
    // is exactly what the gaze cursor's simulated-FOV sizing
    // (gaze_cursor_diameter_px()) depends on being correct. GTK4 has no
    // plain "resize" signal on a bare Gtk::Picture, so a frame-clock
    // tick callback checking allocated size against what was last
    // rendered is the way to notice it, since window-manager resizes
    // land asynchronously.
    splash_picture->add_tick_callback(
        [recording_session, render_current_view, playback_session,
         splash_picture](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            if (recording_session->viewing_equirect &&
                (splash_picture->get_width() !=
                     recording_session->last_render_width ||
                 splash_picture->get_height() !=
                     recording_session->last_render_height)) {
                render_current_view();
            }
            // Same reasoning, Playback side — Record and Playback never
            // run at once, but both share this one picture.
            if (playback_session->viewing_equirect &&
                (splash_picture->get_width() !=
                     playback_session->last_render_width ||
                 splash_picture->get_height() !=
                     playback_session->last_render_height)) {
                render_playback_view(playback_session);
            }
            return true;
        });

    // Keeps no_image_picture's margins proportional to its current
    // scaled size, not a fixed pixel count, scaling together with the
    // card as the window resizes. Reads splash_picture's size rather
    // than no_image_picture's own — margins are subtracted from a
    // widget's allocation before get_width()/get_height() see it, so
    // reading the widget being margined would be self-referential.
    // Same frame-clock-tick polling as the two callbacks above, for the
    // same reason.
    auto last_no_image_margin_px = std::make_shared<int>(-1);
    no_image_picture->add_tick_callback(
        [splash_picture, no_image_picture, last_no_image_margin_px](
            const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            if (!no_image_picture->get_visible()) {
                return true;
            }
            auto paintable = no_image_picture->get_paintable();
            int box_w = splash_picture->get_width();
            int box_h = splash_picture->get_height();
            if (!paintable || box_w <= 0 || box_h <= 0) {
                return true;
            }
            double native_w = paintable->get_intrinsic_width();
            double native_h = paintable->get_intrinsic_height();
            if (native_w <= 0 || native_h <= 0) {
                return true;
            }
            // Fit the *outer* box (card + its native margin) into the
            // available area — the margin at that scale is what's left
            // over once the card itself is drawn.
            double outer_w = native_w + 2.0 * kNoImageMarginAtNativeScalePx;
            double outer_h = native_h + 2.0 * kNoImageMarginAtNativeScalePx;
            double scale = std::min(box_w / outer_w, box_h / outer_h);
            int margin_px = static_cast<int>(
                std::lround(scale * kNoImageMarginAtNativeScalePx));
            if (margin_px == *last_no_image_margin_px) {
                return true;
            }
            *last_no_image_margin_px = margin_px;
            no_image_picture->set_margin_start(margin_px);
            no_image_picture->set_margin_end(margin_px);
            no_image_picture->set_margin_top(margin_px);
            no_image_picture->set_margin_bottom(margin_px);
            return true;
        });

    // The Protocol window drives the display itself (see
    // start_protocol_walkthrough()'s own comment), so nothing here needs
    // to poll the main image on its behalf.

    // Keeps image_details_button in sync with whatever image is
    // actually on screen — polled every
    // frame, same as the Protocol window just above, rather than wired
    // into each of the many places current_image_path changes (there
    // are far more of those than for session_path, which is why
    // session_details_button's own equivalent could just be folded into
    // refresh_transport_sensitivity() instead). Mirrors
    // update_sequence_checkmarks()'s own current-image ternary.
    splash_picture->add_tick_callback(
        [image_details_button_ptr, recording_session, playback_session,
         current_mode](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            const std::string& current_path = *current_mode == 2
                                                    ? playback_session->current_image_path
                                                    : recording_session->current_image_path;
            if (*image_details_button_ptr) {
                (*image_details_button_ptr)->set_sensitive(!current_path.empty());
            }
            return true;
        });

    // Keeps track_button's own tooltip in sync with whichever track is
    // actually current — same per-frame polling reasoning as
    // image_details_button's tick callback just above (current track
    // changes in far more places than a session/project ever does).
    // Shows the same subtitle text the current track's row would show
    // (the live H2 derivation); falls back to "No active tracks" when
    // the current track has no scenario tie, or there's no
    // session/track at all.
    splash_picture->add_tick_callback(
        [track_button, playback_session,
         current_track_number](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            const auto& track_starts = playback_session->track_starts;
            std::size_t i = static_cast<std::size_t>(current_track_number()) - 1;
            if (i >= track_starts.size()) {
                track_button->set_tooltip_text(_("No active tracks"));
                return true;
            }
            std::int32_t start = track_starts[i];
            std::int32_t end = (i + 1 < track_starts.size())
                                    ? track_starts[i + 1]
                                    : std::numeric_limits<std::int32_t>::max();
            TrackImageInfo image_info = image_info_for_track(playback_session->events, start, end);
            track_button->set_tooltip_text(
                !image_info.scenario.empty() ? image_info.scenario : _("No active tracks"));
            return true;
        });

    auto load_image = [splash_picture, recording_session, settings,
                        refresh_sequence_list, render_current_view, window,
                        toast_overlay, no_image_picture,
                        update_sequence_button_label](const std::string& path) {
        recording_session->current_image_path = path;
        dismiss_splash();
        update_sequence_button_label(path);
        set_last_image_path(settings, path);
        // A real image is always being shown from here on — clears
        // whatever test-image placeholder might still be up. load_image()
        // is only ever called with an actual path, never "" (handled
        // directly at each call site), so this is safe unconditionally.
        splash_picture->remove_css_class("blind-sight-no-image-background");
        no_image_picture->set_visible(false);
        // Loaded once here rather than re-read from disk on every
        // pointer motion event, for the "distance at pointer" readout.
        // Reset first so a missing/failed depth file for the new image
        // doesn't leave the *previous* image's depth data showing.
        recording_session->depth_data.clear();
        recording_session->depth_width = 0;
        recording_session->depth_height = 0;
        if (has_depth_file(path)) {
            load_depth_exr(depth_file_path_for(path),
                            recording_session->depth_data,
                            recording_session->depth_width,
                            recording_session->depth_height);
        } else {
            // No depth file yet — generate one on the fly (see
            // maybe_generate_depth_on_the_fly()).
            maybe_generate_depth_on_the_fly(window, recording_session,
                                             toast_overlay, settings, path);
        }
        // Photosphere (equirectangular, GPano-cropped-equirect, or
        // cylindrical-fallback panorama) → an actual pinhole-camera
        // viewport into it, at the current head orientation; anything
        // else just displays flat. gdk_pixbuf_get_file_info() reads only
        // the header — cheap even for an 8704×4352 source — so detection
        // doesn't pay for a full decode first.
        int width = 0, height = 0;
        gdk_pixbuf_get_file_info(path.c_str(), &width, &height);
        if (auto proj = detect_panorama_projection(path, width, height)) {
            // Cached on the session, not just a local — arrow keys
            // (on_activate(), below) re-render from this without reloading
            // the file on every keypress.
            recording_session->equirect_source = Gdk::Pixbuf::create_from_file(
                path, kEquirectSourceMaxWidth, -1, true);
            recording_session->viewing_equirect = true;
            recording_session->panorama_projection = *proj;
            // A stale nonzero pitch left over from a previous image
            // would otherwise immediately show the same real black edge
            // apply_pitch_delta's gate prevents going forward (see
            // panorama_supports_vertical_panning()).
            if (!panorama_supports_vertical_panning(*proj)) {
                recording_session->view_pitch = 0;
            }
            render_current_view();
        } else {
            recording_session->viewing_equirect = false;
            recording_session->panorama_projection = PanoramaProjection{};
            recording_session->equirect_source.reset();
            splash_picture->set_filename(path);
        }
        // Shared by Interactive mode and the Record bar's forward/
        // backward navigation (add_record_controls()'s
        // sync_record_display()). Any ImageEvent/ViewEvent writing this
        // needs while recording is the *caller's* job (go_to_nav_index())
        // — this function only updates display/depth/equirect state.
        (*refresh_sequence_list)();
    };

    // Returning to Interactive from Playback must restore the actual
    // display state, not just the dropdown's label text — the shared
    // picture/no_image_picture widgets are otherwise left in whatever
    // state Playback's last dispatched event set them to: Playback
    // ending on a "No image" stretch would leave the placeholder
    // showing even though recording_session->current_image_path is a
    // real image, and rotating the Surface Dial would then render the
    // real image underneath without ever hiding the placeholder, so
    // both end up visible at once. Reuses load_image() for a real path
    // rather than reimplementing it — a redundant decode, not a hot path.
    // "" means nothing warned about yet this run — check_bt_output_warning()
    // only re-shows the toast when the detected device actually changes, so
    // returning to Interactive repeatedly with the same still-connected
    // Bluetooth output doesn't re-nag every time.
    auto last_warned_bt_device = std::make_shared<std::string>();
    // show_interactive_bar() itself is never called for the app's own
    // initial Interactive state (the window just starts there by
    // construction) — only later, on returning to Interactive from
    // Record/Playback. Without this explicit call, the very first time
    // Interactive is shown would silently skip the check, exactly the one
    // moment the user most needs to see it, before ever starting a
    // recording or making any judgment at all.
    check_bt_output_warning(toast_overlay, last_warned_bt_device);

    auto show_interactive_bar = [recording_session, update_sequence_button_label,
                                   load_image, splash_picture, no_image_picture,
                                   toast_overlay, last_warned_bt_device]() {
        check_bt_output_warning(toast_overlay, last_warned_bt_device);
        if (!recording_session->current_image_path.empty()) {
            load_image(recording_session->current_image_path);
            return;
        }
        update_sequence_button_label("");
        dismiss_splash();
        splash_picture->set_paintable({});
        splash_picture->add_css_class("blind-sight-no-image-background");
        no_image_picture->set_visible(true);
    };

    // The image dropdown's "select this image" entry point — one
    // row-click handler, but what it does depends on *current_mode:
    // Playback seeks to wherever that path already occurs in the loaded
    // recording, since writing to recording_session via load_image()
    // while Playback is active would be silently wrong (the wrong
    // session, not even the one on screen). Interactive loads it
    // directly; Record goes through change_record_image_action — the
    // same implicit track change + ImageEvent/ViewEvent
    // go_to_nav_index() writes for Forward/Backward, so Playback later
    // knows it needs to switch to it. Does nothing if the image never
    // occurs in the currently loaded recording.
    auto activate_image = [current_mode, playback_session, playback_seek_to,
                            load_image, change_record_image_action,
                            toast_overlay](const std::string& path) {
        if (*current_mode == 1) {
            (*change_record_image_action)(path);
            return;
        }
        if (*current_mode != 2) {
            load_image(path);
            return;
        }
        if (auto ms = find_first_image_event_ms(playback_session->events, path)) {
            (*playback_seek_to)(*ms);
        } else {
            auto* toast = adw_toast_new(_("That image doesn't occur in this recording"));
            adw_toast_set_timeout(toast, 3);
            adw_toast_overlay_add_toast(toast_overlay, toast);
        }
    };

    // Page Up/Page Down step through image-sequence — this is
    // Interactive mode's own version; Record swaps it for its nav
    // buttons' logic (including the implicit track-change side
    // effects), Playback swaps it for a no-op, both restoring this one
    // on close. A reassignable shared_ptr<function<>> so the
    // window-level key controller always calls whichever is active
    // without needing to know which mode that is.
    auto interactive_page_navigate = [settings, load_image, splash_picture,
                                       no_image_picture, recording_session,
                                       active_protocol,
                                       update_sequence_button_label](bool forward) {
        auto sequence = visible_image_paths(project_dir_from_protocol(active_protocol));
        int n = static_cast<int>(sequence.size());
        int idx = -1;
        if (!recording_session->current_image_path.empty()) {
            auto it = std::find(sequence.begin(), sequence.end(),
                                 recording_session->current_image_path);
            if (it != sequence.end()) {
                idx = static_cast<int>(std::distance(sequence.begin(), it));
            }
        }
        int next = forward ? idx + 1 : idx - 1;
        if (next >= n) {
            next = -1;
        } else if (next < -1) {
            // Backward out of "No image" wraps to the last real image:
            // without this, going past "No image" backward would keep
            // decrementing into ever-more-negative territory, which the
            // `next < 0` check below also treats as "No image" — stuck
            // there forever.
            next = n - 1;
        }
        if (next < 0 || next >= n) {
            recording_session->current_image_path.clear();
            recording_session->viewing_equirect = false;
            recording_session->panorama_projection = PanoramaProjection{};
            recording_session->equirect_source.reset();
            // "No image" has no depth map, by definition (see
            // sync_record_display()).
            recording_session->depth_data.clear();
            recording_session->depth_width = 0;
            recording_session->depth_height = 0;
            dismiss_splash();
            splash_picture->set_paintable({});
            splash_picture->add_css_class("blind-sight-no-image-background");
            no_image_picture->set_visible(true);
            update_sequence_button_label("");
            set_last_image_path(settings, "");
        } else {
            load_image(sequence[next]);
        }
    };
    auto page_navigate = std::make_shared<std::function<void(bool)>>(
        interactive_page_navigate);

    nav_prev_button->signal_clicked().connect(
        [page_navigate]() { (*page_navigate)(false); });
    nav_next_button->signal_clicked().connect(
        [page_navigate]() { (*page_navigate)(true); });

    // The image dropdown's popover — see show_image_collection_menu()
    // (image_dialogs.cpp) for what it builds. image_details_button_ptr
    // was declared earlier, alongside the per-frame sensitivity tick
    // callback; this is where it actually gets filled in.
    auto image_menu = show_image_collection_menu(
        sequence_button, window, settings,
        [active_protocol]() { return project_dir_from_protocol(active_protocol); },
        [recording_session, playback_session, current_mode]() -> std::string {
            return *current_mode == 2 ? playback_session->current_image_path
                                       : recording_session->current_image_path;
        },
        activate_image, update_sequence_button_label,
        [active_protocol, recording_session, splash_picture, no_image_picture, settings,
         update_sequence_button_label]() {
            // "Remove from Collection" — same gap as the session editor's
            // delete: removing the image currently on screen left it
            // showing a path no longer in the collection at all.
            std::string project_dir = project_dir_from_protocol(active_protocol);
            if (!recording_session->current_image_path.empty()) {
                auto sequence = visible_image_paths(project_dir);
                if (std::find(sequence.begin(), sequence.end(),
                               recording_session->current_image_path) == sequence.end()) {
                    recording_session->current_image_path.clear();
                    recording_session->viewing_equirect = false;
                    recording_session->panorama_projection = PanoramaProjection{};
                    recording_session->equirect_source.reset();
                    recording_session->depth_data.clear();
                    recording_session->depth_width = 0;
                    recording_session->depth_height = 0;
                    dismiss_splash();
                    splash_picture->set_paintable({});
                    splash_picture->add_css_class("blind-sight-no-image-background");
                    no_image_picture->set_visible(true);
                    set_last_image_path(settings, "");
                }
            }
            // Also covers the Edit Collection dialog's own "Image
            // Details" action editing the *currently shown* image's
            // title/caption override — not just the removed-from-
            // collection case above.
            update_sequence_button_label(recording_session->current_image_path);
        });
    *refresh_sequence_list = image_menu.refresh_sequence_list;
    *image_details_button_ptr = image_menu.image_details_button;

    // The project dropdown's popover (recent projects, New/Open/Edit
    // Collection/Details) is built later, via show_project_collection_menu()
    // — once open_project_at()/g_start_new_project exist, below — rather
    // than inline here. project_details_button_ptr is a forward reference
    // (same pattern as refresh_sequence_list elsewhere in this file):
    // update_project_button_label() captures it now, but it's only ever
    // actually dereferenced later, at click time, well after
    // show_project_collection_menu() has filled it in.
    auto project_details_button_ptr = std::make_shared<Gtk::Button*>(nullptr);

    auto update_project_button_label = [settings, active_protocol, project_button,
                                          project_button_label, project_details_button_ptr,
                                          protocol_button]() {
        // Project Details implicitly acts on "the active project" — same
        // has_session-gated treatment session_details_button gets via
        // refresh_transport_sensitivity(), folded in here instead since
        // this function is already the one place every project-state
        // change already calls through. protocol_button (reading the
        // protocol itself) has the same precondition.
        if (*project_details_button_ptr) {
            (*project_details_button_ptr)->set_sensitive(!active_protocol->source_path.empty());
        }
        protocol_button->set_sensitive(!active_protocol->source_path.empty());
        if (active_protocol->source_path.empty()) {
            project_button_label->set_text(_("No Project"));
            project_button->set_tooltip_text(_("No active projects"));
            return;
        }
        std::string resolved = resolved_title(*active_protocol);
        Glib::ustring title =
            resolved.empty() ? Glib::ustring(_("Untitled Project")) : Glib::ustring(resolved);
        project_button_label->set_text(truncate_dropdown_title(
            title, settings->get_int(kProjectTitleCutoffCharsKey)));
        // The menu button's own tooltip — same subtitle text the
        // project's row would show (resolved_description()), not a
        // generic label, so hovering the button itself previews the
        // active project without opening the popover. Falls back to the
        // generic collection name when there's no description to show.
        std::string description = resolved_description(*active_protocol);
        project_button->set_tooltip_text(!description.empty() ? description : _("No active projects"));
    };

    // The whole header bar stays insensitive until a project is active,
    // except project_button and menu_button (always reachable, so the
    // user can get *to* a project) and the window's own title-bar
    // buttons. Setting a container's sensitivity cascades to every
    // child without touching each button's own flag —
    // refresh_transport_sensitivity()'s finer-grained per-button logic
    // still runs independently underneath this.
    auto update_header_bar_sensitivity = [session_recent_button, sequence_button,
                                            nav_group, clock_group, transport_group,
                                            current_mode]() {
        session_recent_button->set_sensitive(g_project_active);
        sequence_button->set_sensitive(g_project_active);
        // Next/Previous are insensitive during Record while the Protocol
        // window is visible, since the Protocol window drives the
        // display in that case; otherwise Next/Previous do. Scoped to
        // Record specifically — Interactive/Playback never risk a
        // double-driven, inconsistent display.
        bool protocol_driving_nav = *current_mode == 1 && g_protocol_window &&
                                      g_protocol_window->get_visible();
        nav_group->set_sensitive(g_project_active && !protocol_driving_nav);
        clock_group->set_sensitive(g_project_active);
        transport_group->set_sensitive(g_project_active);
    };
    update_header_bar_sensitivity();

    // No plain GTK4 signal for "a different top-level window's
    // visibility changed," so this is polled every frame — cheap, since
    // update_header_bar_sensitivity() is just a handful of
    // set_sensitive() calls, idempotent when nothing changed.
    splash_picture->add_tick_callback(
        [update_header_bar_sensitivity](const Glib::RefPtr<Gdk::FrameClock>&) -> bool {
            update_header_bar_sensitivity();
            return true;
        });

    // Opens an existing project directory — shared by the recents list,
    // "Open Project…" and the startup auto-reopen below. Jumps to the
    // current image if it's still in this protocol, else shows the
    // placeholder. Returns whether it succeeded — the command-line-
    // argument path (g_open_project_by_dir, main()) needs to know, to
    // fall back to reset_to_no_project() on failure.
    auto open_project_at = [window, settings, active_protocol, refresh_sequence_list,
                             recording_session, playback_session, current_mode,
                             return_to_interactive, refresh_session_button_label,
                             refresh_transport_sensitivity, load_image, splash_picture,
                             no_image_picture, update_project_button_label,
                             update_sequence_button_label,
                             update_header_bar_sensitivity](
                                const std::string& project_dir) -> bool {
        std::string path = project_protocol_path(project_dir);
        auto parsed = parse_protocol_file(path);
        if (!parsed) {
            show_error_dialog(window, _("Couldn't Open Project"),
                                _("No protocol.md found in this folder."));
            return false;
        }
        *active_protocol = *parsed;
        active_protocol->source_path = path;
        // Image-sequence is not reseeded from the protocol's own
        // entries here — the project's images.json already has its
        // real, persistent list; refresh_sequence_list() below reads
        // it directly via active_protocol->source_path, just set above.
        (*refresh_sequence_list)();

        // entry.image_ref is a slug, not a path, so this resolves each
        // one back through images.json before comparing (see
        // path_for_image_slug()).
        bool current_is_in_protocol = false;
        for (auto& entry : active_protocol->entries) {
            if (!entry.image_ref.empty() &&
                path_for_image_slug(project_dir, entry.image_ref) ==
                    recording_session->current_image_path) {
                current_is_in_protocol = true;
                break;
            }
        }
        if (current_is_in_protocol) {
            load_image(recording_session->current_image_path);
        } else if (!recording_session->current_image_path.empty()) {
            // Only resets anything if an image was showing that doesn't
            // belong to the newly-opened project. At startup,
            // current_image_path is already empty, and clearing
            // kLastImagePathKey here would wipe out the very path
            // autoload-image is about to read right after this call
            // returns — without this guard, a defined project would
            // relaunch into "No image" instead of the one left showing.
            recording_session->current_image_path.clear();
            recording_session->viewing_equirect = false;
            recording_session->panorama_projection = PanoramaProjection{};
            recording_session->equirect_source.reset();
            // "No image" has no depth map, by definition (see
            // sync_record_display()).
            recording_session->depth_data.clear();
            recording_session->depth_width = 0;
            recording_session->depth_height = 0;
            splash_picture->set_paintable({});
            splash_picture->add_css_class("blind-sight-no-image-background");
            no_image_picture->set_visible(true);
            update_sequence_button_label("");
            set_last_image_path(settings, "");
        }

        // Same reasoning as the image reset just above, for the session
        // dropdown — a session loaded from the *previous* project points
        // into a directory that no longer has anything to do with this
        // one. Same treatment as deleting the active session out from
        // under Record/Playback (edit_session_collection_button's own
        // handler): back to Interactive first if a mode was active, then
        // clear both session paths.
        if (!recording_session->session_path.empty() &&
            std::filesystem::path(recording_session->session_path)
                    .parent_path().parent_path().parent_path() !=
                std::filesystem::path(project_dir)) {
            if (*current_mode != 0) {
                (*return_to_interactive)();
            }
            recording_session->session_path.clear();
            playback_session->session_path.clear();
            refresh_session_button_label("");
            refresh_transport_sensitivity();
        }

        remember_recent_project(settings, project_dir);
        settings->set_string(kLastProjectKey, project_dir);
        update_project_button_label();
        g_project_active = true;
        update_header_bar_sensitivity();
        return true;
    };
    g_open_project_by_dir = open_project_at;

    // Explicit "no project" reset — only the command-line-argument path
    // uses this, so a project named on the command line that can't be
    // found still leaves the app open with nothing selected. Deliberately
    // *not* folded into open_project_at()'s own failure path: an
    // interactive "Open Project…" pick that fails shouldn't discard
    // whatever project the user already had open — only a failed
    // *startup* request should.
    auto reset_to_no_project = [active_protocol, settings, refresh_sequence_list,
                                 recording_session, splash_picture, no_image_picture,
                                 update_project_button_label,
                                 update_sequence_button_label,
                                 update_header_bar_sensitivity]() {
        *active_protocol = Protocol{};
        // active_protocol->source_path is empty now, so
        // refresh_sequence_list() below resolves to an empty
        // project_dir and reads nothing.
        (*refresh_sequence_list)();
        recording_session->current_image_path.clear();
        recording_session->viewing_equirect = false;
        recording_session->panorama_projection = PanoramaProjection{};
        recording_session->equirect_source.reset();
        // "No image" has no depth map, by definition (see
        // sync_record_display()).
        recording_session->depth_data.clear();
        recording_session->depth_width = 0;
        recording_session->depth_height = 0;
        dismiss_splash();
        splash_picture->set_paintable({});
        splash_picture->add_css_class("blind-sight-no-image-background");
        no_image_picture->set_visible(true);
        update_sequence_button_label("");
        set_last_image_path(settings, "");
        update_project_button_label();
        g_project_active = false;
        update_header_bar_sensitivity();
    };
    g_reset_to_no_project = reset_to_no_project;

    // The project dropdown's popover (recent projects, New/Open/Edit
    // Collection/Details) — see show_project_collection_menu()
    // (project_dialogs.cpp) for what it builds. Row activation opens the
    // project via the g_open_project_by_dir bridge, which is assigned to
    // open_project_at() a few lines above this point — already live by
    // the time any row is actually clicked. project_details_button_ptr
    // was declared earlier, alongside update_project_button_label(); this
    // is where it actually gets filled in.
    auto project_menu = show_project_collection_menu(
        project_button, window, settings, active_protocol, update_project_button_label);
    auto refresh_project_list = std::make_shared<std::function<void()>>(
        project_menu.refresh_project_list);
    *project_details_button_ptr = project_menu.project_details_button;

    // Factored out into g_start_new_project so the project collection
    // editor's plus icon can trigger the identical flow without this
    // dropdown's popover needing to be open.
    g_start_new_project = [window, settings, active_protocol, refresh_sequence_list,
                            update_project_button_label, recording_session,
                            playback_session, current_mode, return_to_interactive,
                            refresh_session_button_label, refresh_transport_sensitivity,
                            splash_picture, no_image_picture,
                            update_sequence_button_label,
                            update_header_bar_sensitivity](std::function<void()> on_done) {
        show_new_project_dialog(
            window, settings,
            [settings, active_protocol, refresh_sequence_list, window,
             update_project_button_label, recording_session, playback_session,
             current_mode, return_to_interactive, refresh_session_button_label,
             refresh_transport_sensitivity, splash_picture,
             no_image_picture, update_sequence_button_label,
             update_header_bar_sensitivity, on_done](const std::string& project_dir,
                                            const Protocol& protocol) {
                *active_protocol = protocol;
                // A brand-new project has no images.json yet;
                // read_images_index() already returns an empty index
                // for that. Nothing to clear.
                (*refresh_sequence_list)();
                remember_recent_project(settings, project_dir);
                settings->set_string(kLastProjectKey, project_dir);
                update_project_button_label();
                // A brand-new project is always imageless — force the
                // display into the "No image" placeholder, since
                // open_project_at()'s equivalent reset only fires when
                // switching *away* from a real image, which this
                // handler never went through.
                recording_session->current_image_path.clear();
                recording_session->viewing_equirect = false;
                recording_session->panorama_projection = PanoramaProjection{};
                recording_session->equirect_source.reset();
                recording_session->depth_data.clear();
                recording_session->depth_width = 0;
                recording_session->depth_height = 0;
                dismiss_splash();
                splash_picture->set_paintable({});
                splash_picture->add_css_class("blind-sight-no-image-background");
                no_image_picture->set_visible(true);
                update_sequence_button_label("");
                set_last_image_path(settings, "");
                // A brand-new project is always session-less too, same
                // reasoning as the image reset just above — whatever
                // session was active belongs to a different project
                // entirely now.
                if (!recording_session->session_path.empty()) {
                    if (*current_mode != 0) {
                        (*return_to_interactive)();
                    }
                    recording_session->session_path.clear();
                    playback_session->session_path.clear();
                    refresh_session_button_label("");
                    refresh_transport_sensitivity();
                }
                g_project_active = true;
                update_header_bar_sensitivity();
                if (on_done) {
                    on_done();
                }
                // Straight into adding images — an empty project isn't
                // useful on its own. No on_changed needed — protocol.md
                // is reconciled only on demand, via "Reconcile Protocol".
                show_image_sequence_editor(window, settings, project_dir);
            });
    };

    // The image loads the moment a scenario's content is actually
    // *reached*, not deferred to whenever the *previous* scenario's
    // last step resolves — that would mean scenario *one* never gets
    // this treatment, since nothing "completed" before it.
    // g_protocol_load_scenario_image owns that trigger, called from
    // load_protocol_scenario() at the exact right point.
    //
    // load_image() drives the display in both Record and Interactive;
    // Record additionally gets the same ImageEvent/ViewEvent/track-
    // boundary side effects go_to_nav_index() writes for an ordinary
    // image change — duplicated inline rather than reusing that
    // function, since its nav_index lookup is into visible_image_paths()
    // (images.json order), the wrong sequence for protocol order.
    auto reset_main_display_to_no_image = [recording_session, splash_picture,
                                              no_image_picture, settings,
                                              update_sequence_button_label]() {
        recording_session->current_image_path.clear();
        recording_session->viewing_equirect = false;
        recording_session->panorama_projection = PanoramaProjection{};
        recording_session->equirect_source.reset();
        recording_session->depth_data.clear();
        recording_session->depth_width = 0;
        recording_session->depth_height = 0;
        dismiss_splash();
        splash_picture->set_paintable({});
        splash_picture->add_css_class("blind-sight-no-image-background");
        no_image_picture->set_visible(true);
        update_sequence_button_label("");
        set_last_image_path(settings, "");
    };

    g_protocol_load_scenario_image = [active_protocol, recording_session,
                                        current_mode, load_image, window, settings,
                                        toast_overlay, clock_label,
                                        reset_main_display_to_no_image,
                                        format_clock_ms](const std::string& slug) {
        std::string project_dir = project_dir_from_protocol(active_protocol);
        if (project_dir.empty()) {
            return;
        }
        std::string path = path_for_image_slug(project_dir, slug);
        if (path.empty()) {
            // A scenario with no image link of its own at all — same
            // "No image" state as everywhere else in this file,
            // not an error.
            reset_main_display_to_no_image();
            return;
        }
        std::string scenario_title;
        for (const auto& entry : active_protocol->entries) {
            if (entry.image_ref == slug) {
                scenario_title = entry.title;
                break;
            }
        }
        load_image(path);
        if (*current_mode == 1 && recording_session->file) {
            write_implicit_track_change(window, recording_session, settings,
                                          toast_overlay);
            clock_label->set_text(
                format_clock_ms(recording_session->accumulated_ms));
            if (!recording_session->current_image_path.empty()) {
                count_new_image_reference(project_dir, recording_session->session_path,
                                            recording_session->current_image_path);
                ImageEvent image_event;
                image_event.t_ms = recording_session->accumulated_ms;
                image_event.path = recording_session->current_image_path;
                image_event.scenario = scenario_title;
                write_event(*recording_session->file, image_event);
                ViewEvent view_event;
                view_event.t_ms = recording_session->accumulated_ms;
                view_event.yaw = recording_session->view_yaw;
                view_event.pitch = recording_session->view_pitch;
                write_event(*recording_session->file, view_event);
            }
        }
    };

    // Finds the just-completed scenario's index in
    // active_protocol->entries and either hands the next one straight
    // back to load_protocol_scenario(), or, if that was the last one,
    // pushes the completion page as one more page inside the Protocol
    // window itself, not a separate modal dialog, and resets the main
    // display to "No image".
    g_protocol_advance_scenario = [active_protocol, recording_session, current_mode,
                                     reset_main_display_to_no_image](
                                        const std::string& completed_slug) {
        auto& entries = active_protocol->entries;
        std::size_t completed_index = entries.size();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].image_ref == completed_slug) {
                completed_index = i;
                break;
            }
        }
        if (completed_index >= entries.size()) {
            return;
        }
        if (completed_index + 1 >= entries.size()) {
            if (g_protocol_nav_view) {
                AdwNavigationPage* page = build_protocol_message_page(
                    _("Test Protocol Complete"),
                    _("Every scenario in this protocol has been completed."));
                adw_navigation_view_push(g_protocol_nav_view, page);
            }
            reset_main_display_to_no_image();
            return;
        }
        load_protocol_scenario(*active_protocol, entries[completed_index + 1],
                                 false, recording_session, current_mode);
    };

    // Reopen whatever project was active last time, same "remember the
    // working context" convention as current-working-session. A
    // command-line project argument always wins regardless of this
    // setting. Defaults to *off*: reopening a session while a different
    // test participant is present is a real privacy concern, so turning
    // it on is a deliberate opt-in.
    if (settings->get_boolean(kAutoloadProjectKey)) {
        if (auto last = settings->get_string(kLastProjectKey);
            !last.empty() && std::filesystem::exists(project_protocol_path(last))) {
            open_project_at(last);
        }
    }

    // Autoload-session runs here, after project-autoload, gated on
    // g_project_active: without a project active, reopening the last
    // session would defeat the point of turning autoload-project off
    // for privacy. Factored out so main()'s signal_command_line()
    // handler can re-run it once a --project argument makes
    // g_project_active true, after on_activate() already returned.
    auto reapply_session_autoload = [settings, apply_shared_session,
                                      refresh_session_button_label,
                                      refresh_transport_sensitivity,
                                      toast_overlay]() {
        if (!settings->get_boolean(kAutoloadSessionKey) || !g_project_active) {
            return;
        }
        std::string path = get_current_working_session(settings);
        if (path.empty()) {
            return;
        }
        if (apply_shared_session(path)) {
            refresh_session_button_label(path);
            refresh_transport_sensitivity();
        } else {
            // Same reasoning as session_list's row-activated handler,
            // just at startup instead of a click.
            AdwToast* toast = adw_toast_new(
                _("Can't open recording — wrong or corrupted format"));
            adw_toast_overlay_add_toast(toast_overlay, toast);
        }
    };
    // Microphone levels into the recording: one LevelEvent per 50 ms slot
    // with every microphone's level side by side, after a SourcesEvent
    // naming them at the start of each track. The levellers report from
    // separate processes, so their times don't line up exactly; each
    // report goes in the slot nearest its time, and a slot is written once
    // reports have moved two slots past it. Before any sync(), so every
    // leveller is started knowing where to report.
    struct LevelLog {
        std::vector<std::string> sources;
        std::string track_id;
        std::map<std::int32_t, std::vector<std::optional<std::int16_t>>> pending;
    };
    auto level_log = std::make_shared<LevelLog>();
    audio_routing_engine->on_microphone_level([recording_session, level_log](
                                                  const std::string& id, std::int64_t t_ns,
                                                  double level_dbfs, bool muted) {
        constexpr std::int32_t kSlotMs = 50;
        auto& session = *recording_session;
        auto& log = *level_log;
        auto write_pending = [&session, &log](std::int32_t before_ms) {
            while (!log.pending.empty() && log.pending.begin()->first < before_ms) {
                LevelEvent event;
                event.t_ms = log.pending.begin()->first;
                event.level = std::move(log.pending.begin()->second);
                write_event(*session.file, event);
                log.pending.erase(log.pending.begin());
            }
        };
        if (!session.file || session.paused) {
            log.pending.clear();
            log.track_id.clear();  // a fresh SourcesEvent once recording resumes
            return;
        }
        std::chrono::steady_clock::time_point at{std::chrono::nanoseconds(t_ns)};
        if (at < session.segment_start) {
            return;  // measured before this stretch of recording began
        }
        std::int32_t t_ms =
            session.accumulated_ms +
            static_cast<std::int32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(at - session.segment_start)
                    .count());
        std::int32_t slot = (t_ms + kSlotMs / 2) / kSlotMs * kSlotMs;

        std::vector<std::string> sources = {kAmbientMicSourceId};
        sources.insert(sources.end(), session.monitoring.participant_order.begin(),
                       session.monitoring.participant_order.end());
        if (sources != log.sources || session.current_track_id != log.track_id) {
            write_pending(std::numeric_limits<std::int32_t>::max());
            SourcesEvent event;
            event.t_ms = slot;
            event.sources = sources;
            write_event(*session.file, event);
            log.sources = std::move(sources);
            log.track_id = session.current_track_id;
        }
        auto index = std::find(log.sources.begin(), log.sources.end(), id) - log.sources.begin();
        if (index >= static_cast<std::ptrdiff_t>(log.sources.size())) {
            return;
        }
        auto& levels = log.pending[slot];
        levels.resize(log.sources.size());
        levels[static_cast<std::size_t>(index)] =
            muted ? std::nullopt
                  : std::optional<std::int16_t>(static_cast<std::int16_t>(
                        std::clamp(std::lround(level_dbfs * 10.0), -32768L, 32767L)));
        write_pending(slot - 2 * kSlotMs);
    });

    // Before any sync(), so the first one already applies them.
    for (const auto& [id, db] : get_synthesized_volumes(settings)) {
        audio_routing_engine->set_synthesized_volume_db(recording_session->monitoring, id, db);
    }
    for (const auto& [id, db] : get_participants_volumes(settings)) {
        audio_routing_engine->set_participants_volume_db(recording_session->monitoring, id, db);
    }
    for (const auto& [id, db] : get_source_volumes(settings)) {
        audio_routing_engine->set_source_volume_db(recording_session->monitoring, id, db);
    }
    for (const auto& [id, enabled] : get_source_agc(settings)) {
        audio_routing_engine->set_agc_enabled(id, enabled);
    }
    // The levelling settings are shared by every microphone and edited in
    // Preferences, which knows nothing of the routing engine — so follow
    // the keys themselves.
    auto apply_agc_settings = [settings, engine = audio_routing_engine.get()]() {
        blind_sight::AudioRoutingEngine::AgcSettings agc;
        agc.target_dbfs = settings->get_double("agc-target-dbfs");
        agc.attack_s = settings->get_int("agc-attack-ms") / 1000.0;
        agc.release_s = settings->get_int("agc-release-ms") / 1000.0;
        agc.max_gain_db = settings->get_double("agc-max-gain-db");
        engine->set_agc_settings(agc);
    };
    apply_agc_settings();
    for (const char* key :
         {"agc-target-dbfs", "agc-attack-ms", "agc-release-ms", "agc-max-gain-db"}) {
        settings->signal_changed(key).connect(
            [apply_agc_settings](const Glib::ustring&) { apply_agc_settings(); });
    }
    // Runs after toggle_mic_mute (connected earlier), so it sees the new
    // state: a muted microphone is cut and its leveller frozen.
    mic_button->signal_clicked().connect(
        [mic_muted, engine = audio_routing_engine.get()]() {
            engine->set_microphones_muted(*mic_muted);
        });
    reapply_session_autoload();
    g_reapply_session_autoload = reapply_session_autoload;
    // The synthesized stream never autoconnects, so without a session
    // load to trigger the first sync() prods would stay silent until the
    // matrix is opened.
    if (recording_session->session_path.empty()) {
        reconcile_monitoring_state(recording_session->monitoring, {});
        seed_preferred_devices(recording_session->monitoring, settings);
        audio_routing_engine->sync(recording_session->monitoring);
    }
    // An open matrix popover is rebuilt instead (its refresh syncs too), so
    // its headers show a device going or coming back straight away.
    audio_routing_engine->watch_devices([engine = audio_routing_engine.get(), recording_session,
                                         matrix_widget, matrix_popover]() {
        if (matrix_popover->get_visible()) {
            matrix_widget.refresh();
        } else {
            engine->sync(recording_session->monitoring);
        }
    });

    // Same gating, for the last-viewed image — runs after the project
    // autoload above; load_image() doesn't require the path to be a
    // member of image-sequence, any readable file works. Factored out
    // so main()'s signal_command_line() handler can re-run it after a
    // --project/--session argument (see g_reapply_last_image_autoload).
    auto reapply_last_image_autoload = [settings, load_image]() {
        if (!settings->get_boolean(kAutoloadImageKey) || !g_project_active) {
            return;
        }
        if (auto last_image = settings->get_string(kLastImagePathKey);
            !last_image.empty() &&
            std::filesystem::exists(std::string(last_image))) {
            load_image(last_image);
        }
    };
    reapply_last_image_autoload();
    g_reapply_last_image_autoload = reapply_last_image_autoload;

    // The image popover's own "rebuild on show" + "poll the checkmark
    // every 300ms while open" + "dismiss the splash caption on open" are
    // all wired inside show_image_collection_menu() itself now.

    // Splash caption hides the moment the user opens the hamburger menu
    // — the first sign of interacting with the app at all. Also needs
    // to fire on Play/Rec themselves (added below), since going
    // straight to Playback/Record never touches this trigger. Goes
    // through dismiss_splash(), not a direct set_visible(false), so
    // g_splash_dismissed tracks this too — otherwise Record's
    // close_self() could later restore the bat-image splash onto a
    // caption that's already gone.
    menu_button->get_popover()->signal_show().connect(
        []() { dismiss_splash(); });

    app->add_action("about", [app]() { show_about(app->get_active_window()); });
    app->add_action("preferences", [app, settings, toast_overlay]() {
        show_preferences(app->get_active_window(), settings, toast_overlay);
    });
    app->add_action("shortcuts",
                     [app]() { show_shortcuts(app->get_active_window()); });
    app->add_action("reconcile-protocol",
                     [app, settings, active_protocol, toast_overlay,
                      update_project_button_label, refresh_project_list]() {
                         auto* parent = app->get_active_window();
                         if (project_dir_from_protocol(active_protocol).empty()) {
                             show_error_dialog(
                                 parent, _("No Project"),
                                 _("Open or create a project first — "
                                   "the protocol always lives inside a "
                                   "project."));
                             return;
                         }
                         if (reconcile_and_write_protocol(settings, active_protocol)) {
                             // The fresh-from-disk re-read inside
                             // reconcile_and_write_protocol() may have
                             // picked up a hand-edited title/description
                             // too, not just entries.
                             update_project_button_label();
                             (*refresh_project_list)();
                         }
                         AdwToast* toast = adw_toast_new(_("Protocol reconciled"));
                         adw_toast_overlay_add_toast(toast_overlay, toast);
                     });
    app->add_action("trace-delay", [app, toast_overlay]() {
        show_trace_delay_dialog(app->get_active_window(), toast_overlay);
    });
    app->add_action("dump-sound-paths", [app, toast_overlay]() {
        dump_sound_paths(app->get_active_window(), toast_overlay);
    });

    // Record/Play/Stop drive mode switching — pressing Record/Play
    // builds that mode's action bar directly, rather than showing a
    // dialog first and only building the bar afterward.
    //
    // Rec is always enterable, even with no image chosen yet:
    // add_record_controls()'s nav_index/sync_record_display() decide
    // whether the placeholder or a real image shows. Play is never
    // gated by this either: it opens its own file and reads whatever
    // image that file names, independent of what's currently showing.
    //
    // Record/Play/Stop are the single authoritative mode dispatcher:
    // each bar's own close_self (a reassignable-shared_ptr out-parameter,
    // same pattern as page_navigate) does the teardown needed to leave
    // that mode. current_mode tracks which bar is actually open, so
    // leaving a mode always tears down the right one regardless of
    // which button was pressed.
    //
    // start_recording_action/stop_recording_action are out-parameters
    // add_record_controls() fills in — invoked explicitly, right after
    // wiring, since clicking record_button/play_button/stop_button is
    // what *causes* a mode to be entered, same shape of problem
    // activate_image() solves for the image dropdown.
    // start_playing_action/stop_playing_action are the same idea but
    // assigned once, permanently, by the Playback engine above.
    auto close_record = std::make_shared<std::function<void()>>([] {});
    auto close_playback = std::make_shared<std::function<void()>>([] {});
    auto start_recording_action = std::make_shared<std::function<void()>>([] {});
    auto stop_recording_action = std::make_shared<std::function<void()>>([] {});
    auto toggle_record_pause_action = std::make_shared<std::function<void()>>([] {});
    // start_playing_action/stop_playing_action declared much earlier now
    // (right before the permanent Playback engine that assigns them) —
    // see that declaration's own comment.

    // FF/FB hold-to-scrub: hold to scrub at 2x/4x/8x, doubling each time
    // the same button is pressed again within a short window right
    // after releasing it; any other press starts a fresh 2x scrub.
    // Releasing (once the window closes without a re-press) returns
    // immediately to whatever was active before the *first* press of
    // this scrub — Play resumes at plain 1x from wherever the scrub
    // left the position; Interactive ("stop") stops there and hands
    // the shared picture back to Interactive's current image. Sensitive
    // only in Interactive/Play, never during Record.
    auto scrub_prior_mode = std::make_shared<int>(0);
    auto scrub_multiplier = std::make_shared<double>(2.0);
    auto scrub_direction = std::make_shared<double>(1.0);
    auto scrub_rearm_connection = std::make_shared<sigc::connection>();

    // GTK's own double-click-time setting for the re-press window
    // (part of GTK's accessibility settings), with a fallback if it's
    // ever unreadable.
    auto scrub_rearm_window_ms = []() -> unsigned int {
        constexpr int kFallbackMs = 300;
        auto gtk_settings = Gtk::Settings::get_default();
        int value =
            gtk_settings ? gtk_settings->property_gtk_double_click_time().get_value() : 0;
        return static_cast<unsigned int>(value > 0 ? value : kFallbackMs);
    };

    // Hands the shared picture back to Interactive's current image —
    // FF/FB's scrub reuses playback_session's render path (the same
    // widget Interactive draws on), so without this the scrubbed-to
    // frame would just stay on screen instead of returning to "stop."
    auto restore_interactive_view = [recording_session, load_image, splash_picture,
                                      no_image_picture]() {
        if (!recording_session->current_image_path.empty()) {
            load_image(recording_session->current_image_path);
        } else {
            splash_picture->set_paintable({});
            splash_picture->add_css_class("blind-sight-no-image-background");
            no_image_picture->set_visible(true);
        }
    };

    auto begin_or_rearm_scrub = [scrub_prior_mode, scrub_multiplier, scrub_direction,
                                  scrub_rearm_connection, current_mode, playback_session,
                                  start_ticking](double direction) {
        bool rearming =
            scrub_rearm_connection->connected() && *scrub_direction == direction;
        scrub_rearm_connection->disconnect();
        if (rearming) {
            *scrub_multiplier = std::min(8.0, *scrub_multiplier * 2.0);
        } else {
            *scrub_prior_mode = *current_mode;
            *scrub_multiplier = 2.0;
            *scrub_direction = direction;
        }
        // base_ms is only ever the anchor start_ticking() measures from,
        // not a continuously-updated live position — capture the
        // current position first, or (re)starting ticking at the new
        // rate jumps back to wherever base_ms was last anchored.
        if (playback_session->tick_active) {
            playback_session->base_ms = playback_session->elapsed_ms();
        }
        (*start_ticking)(direction * (*scrub_multiplier));
    };

    auto end_scrub = [scrub_prior_mode, scrub_multiplier, start_ticking, playback_session,
                       playback_pause_or_resume, restore_interactive_view]() {
        if (*scrub_prior_mode == 2) {
            // Same anchor-capture reasoning as begin_or_rearm_scrub —
            // capture the scrubbed-to position first, or resuming at 1x
            // would jump back to wherever base_ms was last.
            playback_session->base_ms = playback_session->elapsed_ms();
            (*start_ticking)(1.0);
        } else {
            // Same pause logic Play's own toggle uses — current_mode
            // never left Interactive during a scrub, so there's no
            // "mode" to restore, just the picture.
            playback_pause_or_resume();
            restore_interactive_view();
        }
        *scrub_multiplier = 2.0;
    };

    auto wire_scrub_button = [begin_or_rearm_scrub, end_scrub, scrub_rearm_connection,
                               scrub_rearm_window_ms](Gtk::Button* button, double direction) {
        auto gesture = Gtk::GestureClick::create();
        button->add_controller(gesture);
        gesture->signal_pressed().connect(
            [begin_or_rearm_scrub, direction](int, double, double) {
                begin_or_rearm_scrub(direction);
            });
        gesture->signal_released().connect(
            [end_scrub, scrub_rearm_connection,
             scrub_rearm_window_ms](int, double, double) {
                scrub_rearm_connection->disconnect();
                *scrub_rearm_connection = Glib::signal_timeout().connect(
                    [end_scrub]() {
                        end_scrub();
                        return false;  // one-shot
                    },
                    scrub_rearm_window_ms());
            });
    };
    wire_scrub_button(fast_forward_button, 1.0);
    wire_scrub_button(fast_backward_button, -1.0);

    record_button->signal_clicked().connect(
        [current_mode, close_playback, window, header_bar, recording_session,
         settings, active_protocol, toast_overlay, splash_picture, no_image_picture,
         load_image, page_navigate, interactive_page_navigate,
         update_sequence_button_label, record_button, stop_button, clock_label,
         close_record, start_recording_action, stop_recording_action,
         toggle_record_pause_action, change_record_image_action, splash_caption,
         refresh_transport_sensitivity]() {
            if (*current_mode == 1) {
                // Already recording — Record itself is the pause/resume
                // toggle, nothing to rebuild.
                (*toggle_record_pause_action)();
                refresh_transport_sensitivity();
                return;
            }
            if (*current_mode == 2) {
                (*close_playback)();
            }
            *current_mode = 1;
            dismiss_splash();
            add_record_controls(window, header_bar, recording_session, settings,
                                 project_dir_from_protocol(active_protocol), active_protocol,
                                 toast_overlay, splash_picture, no_image_picture,
                                 load_image, page_navigate, interactive_page_navigate,
                                 close_record, update_sequence_button_label,
                                 record_button, stop_button, clock_label,
                                 start_recording_action, stop_recording_action,
                                 toggle_record_pause_action, change_record_image_action);
            (*start_recording_action)();
            refresh_transport_sensitivity();
        });

    play_button->signal_clicked().connect(
        [current_mode, close_record, window, header_bar, playback_session,
         settings, active_protocol, page_navigate, interactive_page_navigate,
         update_sequence_button_label, play_button, stop_button,
         fast_forward_button, fast_backward_button, clock_label, close_playback,
         start_playing_action, stop_playing_action, playback_seek_to,
         wait_resume_connection, splash_caption, refresh_transport_sensitivity,
         apply_shared_session, prime_playback_position]() {
            if (*current_mode == 2) {
                // Already playing — Play itself is the pause/resume
                // toggle, nothing to rebuild.
                (*start_playing_action)();
                refresh_transport_sensitivity();
                return;
            }
            if (*current_mode == 1) {
                (*close_record)();
            }
            // Re-reads events/track_starts/audio_segments/transcript_cues
            // from disk — playback_session only ever gets these from
            // apply_shared_session(), which runs once when the session is
            // opened, not again just because Record added more tracks
            // since. close_record() above already flushed/closed the
            // file if Record was active, so this sees whatever's
            // actually on disk now.
            //
            // apply_shared_session() unconditionally resets next_index/
            // base_ms to the start, correct when switching to a
            // *different* session — but here it's re-reading the *same*
            // one, and wiping the position would silently undo whatever
            // track/time the user just picked. Save and restore across
            // the call — the events before that position can only have
            // grown, never changed.
            if (!playback_session->session_path.empty()) {
                std::int32_t primed_base_ms = playback_session->base_ms;
                apply_shared_session(playback_session->session_path);
                prime_playback_position(primed_base_ms);
            }
            *current_mode = 2;
            dismiss_splash();
            add_playback_controls(window, header_bar, playback_session, settings,
                                   active_protocol, page_navigate,
                                   interactive_page_navigate, close_playback,
                                   playback_seek_to, update_sequence_button_label,
                                   play_button, stop_button, fast_forward_button,
                                   fast_backward_button, clock_label,
                                   wait_resume_connection);
            (*start_playing_action)();
            refresh_transport_sensitivity();
        });

    // The real implementation, now that close_record/close_playback/
    // stop_recording_action/stop_playing_action/show_interactive_bar all
    // exist — see return_to_interactive's forward declaration near
    // current_mode, above.
    *return_to_interactive = [current_mode, close_record, close_playback,
                               stop_recording_action, stop_playing_action,
                               show_interactive_bar, refresh_transport_sensitivity]() {
        if (*current_mode == 1) {
            (*stop_recording_action)();
            (*close_record)();
        } else if (*current_mode == 2) {
            (*stop_playing_action)();
            (*close_playback)();
        }
        *current_mode = 0;
        show_interactive_bar();
        refresh_transport_sensitivity();
    };

    stop_button->signal_clicked().connect(
        [return_to_interactive]() { (*return_to_interactive)(); });

    // Viewport control: arrow keys. On the window, not the splash
    // picture itself, so it works regardless of which widget has focus.
    // A no-op (returns false, unhandled) unless a photosphere is
    // actually loaded.
    constexpr std::int32_t kYawStepCentidegrees = 500;    // 5°
    constexpr std::int32_t kPitchStepCentidegrees = 500;  // 5°
    // Clamped shy of the poles, not all the way to ±90° — avoids the
    // degenerate case of the projection looking straight along the sphere's
    // seam.
    constexpr std::int32_t kMaxPitchCentidegrees = 8900;
    // Changes view_yaw by a signed delta, wraps or clamps it, renders,
    // and logs a ViewEvent if recording — shared by the arrow-key
    // Left/Right cases, the Surface Dial's rotation handler, and the
    // mouse-scroll-wheel handler, so every yaw input goes through the
    // same logic.
    auto apply_yaw_delta = [recording_session, render_current_view,
                             splash_picture](std::int32_t delta_centidegrees,
                                             bool fast) {
        std::int32_t new_yaw = recording_session->view_yaw + delta_centidegrees;
        if (panorama_supports_horizontal_wrap(recording_session->panorama_projection)) {
            // Yaw wraps at ±180° — matches the projection's own longitude
            // wrap (render_equirect_viewport() via std::atan2()).
            new_yaw = ((new_yaw + 18000) % 36000 + 36000) % 36000 - 18000;
        } else {
            // Bounded content (cylindrical fallback, or a partial-width
            // GPano crop) — clamp instead of wrapping, so panning stops
            // at the content's real edge rather than looping back into
            // the same photo.
            int out_width = splash_picture->get_width() > 0
                                 ? splash_picture->get_width()
                                 : 1024;
            int out_height = splash_picture->get_height() > 0
                                  ? splash_picture->get_height()
                                  : 640;
            double aspect = static_cast<double>(out_width) / out_height;
            std::int32_t min_yaw = 0, max_yaw = 0;
            panorama_yaw_bounds_centidegrees(recording_session->panorama_projection,
                                              kViewportFovDegrees, aspect, min_yaw,
                                              max_yaw);
            new_yaw = std::clamp(new_yaw, min_yaw, max_yaw);
        }
        recording_session->view_yaw = new_yaw;
        render_current_view(fast);
        // "Logged only when the viewport actually changes" (recording.hpp,
        // ViewEvent) — every yaw change while recording, not just at
        // image-load time.
        if (recording_session->file) {
            ViewEvent view_event;
            view_event.t_ms = recording_session->elapsed_ms();
            view_event.yaw = recording_session->view_yaw;
            view_event.pitch = recording_session->view_pitch;
            write_event(*recording_session->file, view_event);
        }
    };

    // Pitch's counterpart to apply_yaw_delta() — clamped, not wrapped
    // (going past the pole doesn't make sense to render). Shared by the
    // arrow-key Up/Down cases and the mouse-scroll-wheel handler.
    //
    // Suppresses repeat toasts while one's already showing — holding
    // Up/Down fires many key-pressed events in the time it takes to
    // read one toast. A shared_ptr since apply_pitch_delta may be
    // copied into more than one signal connection below.
    auto vertical_pan_toast_active = std::make_shared<bool>(false);
    auto apply_pitch_delta = [recording_session, render_current_view,
                               kMaxPitchCentidegrees, toast_overlay,
                               vertical_pan_toast_active](
                                  std::int32_t delta_centidegrees, bool fast,
                                  bool notify_if_disabled = false) {
        // No-op for anything short of a full sphere (see
        // panorama_supports_vertical_panning()). A single gate covers
        // every input path that changes pitch. Told plainly rather than
        // silently doing nothing, but only for arrow keys — a toast per
        // scroll tick would spam, so only arrow-key call sites pass
        // notify_if_disabled=true.
        if (!panorama_supports_vertical_panning(recording_session->panorama_projection)) {
            if (notify_if_disabled && !*vertical_pan_toast_active) {
                *vertical_pan_toast_active = true;
                AdwToast* toast = adw_toast_new(_("Vertical panning disabled for panorama"));
                adw_toast_set_timeout(toast, 3);
                adw_toast_overlay_add_toast(toast_overlay, toast);
                Glib::signal_timeout().connect_once(
                    [vertical_pan_toast_active]() { *vertical_pan_toast_active = false; },
                    3000);
            }
            return;
        }
        recording_session->view_pitch = std::clamp(
            recording_session->view_pitch + delta_centidegrees,
            -kMaxPitchCentidegrees, kMaxPitchCentidegrees);
        render_current_view(fast);
        if (recording_session->file) {
            ViewEvent view_event;
            view_event.t_ms = recording_session->elapsed_ms();
            view_event.yaw = recording_session->view_yaw;
            view_event.pitch = recording_session->view_pitch;
            write_event(*recording_session->file, view_event);
        }
    };

    // Center in the same [0,65535] fixed-point convention as GazeEvent/
    // ProdEvent's x/y — 65535/2 rounded. Shared by fire_head_prod() and
    // the Surface Dial block's own click -> prod.
    constexpr std::uint16_t kViewportCenterNormalized = 32767;

    // Shared by fire_head_prod()/fire_eye_prod() below — same glow
    // either way, only the logged ProdEvent's `type` string and
    // position differ (see ProdEvent, recording.hpp).
    auto fire_prod = [recording_session, prod_cue, splash_picture, settings,
                       noise_pulse_engine, current_mode](const char* type, std::uint16_t x,
                                           std::uint16_t y) {
        // Head-prod is Record/Interactive feedback tied to
        // recording_session's view state — meaningless, and actively
        // misleading, while actually in Playback. Same gap as
        // build_splash()'s fire_eye_prod().
        if (*current_mode == 2) {
            return;
        }
        // No depth map, no prod (see session_has_depth()). Panning/
        // navigation is untouched; this only gates prod firing itself.
        if (!session_has_depth(*recording_session)) {
            return;
        }
        show_prod_cue(prod_cue, splash_picture, settings, x, y);
        if (recording_session->file) {
            ProdEvent event;
            event.type = type;
            event.t_ms = recording_session->elapsed_ms();
            event.x = x;
            event.y = y;
            write_event(*recording_session->file, event);
        }
        // Distance estimate — computed once here, used both for the
        // echo delay below and logged so it's checkable against real
        // scenes while the formula/grid size get calibrated live.
        int grid_n = depth_estimate_grid_n(settings);
        auto distance_m = compute_prod_distance_m(*recording_session,
                                                    *splash_picture, x, y, grid_n);
        if (distance_m) {
            g_message("blind-sight: %s distance estimate: %.2f m (grid %dx%d)",
                       type, *distance_m, grid_n, grid_n);
        }

        // First take on the acoustic model's carrier signal — same
        // ADSR+color+gain for every prod regardless of subtype or
        // position (not yet modulated by distance — only *when* it
        // plays is, via the delay below). NoisePulseEngine::trigger()
        // is real-time safe, fine to call straight from this UI code.
        NoisePulseParams pulse_params;
        pulse_params.attack_ms = settings->get_double(kNoisePulseAttackMsKey);
        pulse_params.decay_ms = settings->get_double(kNoisePulseDecayMsKey);
        pulse_params.sustain_level =
            settings->get_double(kNoisePulseSustainLevelKey);
        pulse_params.sustain_ms = settings->get_double(kNoisePulseSustainMsKey);
        pulse_params.release_ms = settings->get_double(kNoisePulseReleaseMsKey);
        pulse_params.color = settings->get_double(kNoisePulseColorKey);
        pulse_params.gain = settings->get_double(kNoisePulseGainKey);

        // Dry pulse plays immediately; the echo (the same rendered
        // pulse, not a second one) is handled inside the engine — same
        // shape as fire_eye_prod() (build_splash()).
        unsigned int delay_ms = distance_to_delay_ms(distance_m, settings).value_or(0);
        noise_pulse_engine->trigger(pulse_params, delay_ms);
    };
    // A second trigger while one sequence is still sweeping supersedes
    // it rather than interleaving two sweeps' steps together — same
    // generation-counter pattern show_prod_cue() uses for rapid
    // re-triggers.
    auto fire_head_prod_generation = std::make_shared<unsigned int>(0);
    auto fire_head_prod = [fire_prod, splash_picture, settings,
                            kViewportCenterNormalized,
                            fire_head_prod_generation]() {
        unsigned int generation = ++(*fire_head_prod_generation);
        int viewport_height = std::max(splash_picture->get_height(), 1);
        auto step = std::make_shared<std::size_t>(0);
        // Self-referencing shared_ptr<function<>>, same pattern as
        // start_ticking() elsewhere in this file — needs to reschedule
        // itself from inside a callback it itself schedules.
        auto fire_next_step = std::make_shared<std::function<void()>>();
        *fire_next_step = [fire_prod, kViewportCenterNormalized,
                            viewport_height, step, fire_head_prod_generation,
                            generation, settings, fire_next_step]() {
            if (*fire_head_prod_generation != generation ||
                *step >= std::size(kHeadProdSequenceElevationsDeg)) {
                return;  // superseded, or the sequence already finished
            }
            double elevation_deg = kHeadProdSequenceElevationsDeg[*step];
            int offset = elevation_to_pixel_offset(
                elevation_deg, viewport_height, kViewportFovDegrees);
            double fy = std::clamp(
                (viewport_height / 2.0 + offset) / viewport_height, 0.0, 1.0);
            auto y = static_cast<std::uint16_t>(std::lround(fy * 65535.0));
            fire_prod("head-prod", kViewportCenterNormalized, y);
            ++(*step);
            if (*step >= std::size(kHeadProdSequenceElevationsDeg)) {
                return;  // just fired the last step
            }
            int step_ms = settings->get_int(kHeadProdSequenceStepMsKey);
            Glib::signal_timeout().connect_once(
                [fire_next_step]() { (*fire_next_step)(); },
                std::max(step_ms, 0));
        };
        (*fire_next_step)();
    };
    // Eye-prod: at the actual pointer/gaze position, not center — a
    // mouse click, or (eye_prod_settle_connection, further down) the
    // pointer settling in place after moving.
    auto fire_eye_prod = [fire_prod](std::uint16_t x, std::uint16_t y) {
        fire_prod("eye-prod", x, y);
    };

    // Marks "head movement just happened" — read by build_splash()'s
    // motion handler to suppress eye-prod for a short window after any
    // head movement. Touched by every head-movement input below:
    // arrow-key press/release and notify_pan_input() (Dial rotation,
    // scroll wheel).
    auto touch_head_movement = [last_head_movement_at]() {
        *last_head_movement_at = std::chrono::steady_clock::now();
    };

    // Saccade-style implicit prod for the Dial/scroll-driven pan
    // channel: moving to a new point fires a prod once movement
    // settles. Arrow keys are explicitly excluded — smoothing their
    // movement to detect a "stop" adds complication not worth the cost.
    // Only the Dial's rotation and the scroll wheel feed this, the two
    // inputs meant to approximate a real head-worn device's continuous
    // motion.
    //
    // One shared debounce, not a separate one per device/axis: yaw
    // (from either device) and pitch (from scroll) reset the *same*
    // timer, so the prod only fires once neither axis has moved for
    // kPanProdSettleMs, approximating "the composite vector has
    // settled." Deliberately a longer window than the 150ms render-
    // quality settle timers further down use — a false-positive
    // "stopped" here fires a real, logged prod, a materially worse
    // mistake than a render staying at reduced quality a little longer.
    constexpr unsigned int kPanProdSettleMs = 250;
    auto pan_prod_settle_connection = std::make_shared<sigc::connection>();
    auto notify_pan_input = [pan_prod_settle_connection, fire_head_prod,
                              touch_head_movement, kPanProdSettleMs]() {
        touch_head_movement();
        pan_prod_settle_connection->disconnect();
        *pan_prod_settle_connection = Glib::signal_timeout().connect(
            [fire_head_prod]() {
                fire_head_prod();
                return false;  // one-shot
            },
            kPanProdSettleMs);
    };

    // Fullscreen toggle: F11 is the de facto GNOME/freedesktop
    // convention. Escape exits fullscreen but — deliberately — never
    // enters it, so it still falls through for whatever else already
    // uses Escape (dialogs closing). Not gated on viewing_equirect —
    // fullscreen is useful regardless of what's on screen.
    auto key_controller = Gtk::EventControllerKey::create();
    // Arrow-key repeat-vs-first-press tracking for fire_head_prod():
    // fires on every autorepeat tick and unconditionally on release,
    // never on a key's first/non-repeat press, which is unambiguously
    // the *start* of movement.
    auto held_pan_keys = std::make_shared<std::set<guint>>();
    key_controller->signal_key_pressed().connect(
        [recording_session, render_current_view, settings, window,
         toolbar_view, apply_yaw_delta, apply_pitch_delta, kYawStepCentidegrees,
         kPitchStepCentidegrees, page_navigate, fire_head_prod,
         touch_head_movement, held_pan_keys, toggle_mic_mute,
         toggle_speaker_mute](guint keyval, guint, Gdk::ModifierType) -> bool {
            if (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down) {
                (*page_navigate)(keyval == GDK_KEY_Page_Down);
                return true;
            }
            // Hardware mic-mute/speaker-mute keys, same toggle the
            // header-bar buttons use. Untested whether these actually
            // reach a focused app window on GNOME/Wayland — this
            // project's own earlier Surface Dial work found GNOME's
            // media-key handling grabs some keys globally before an
            // app ever sees them (see notes.md/TODO.md), and mic/
            // speaker mute are exactly the kind of key gnome-settings-
            // daemon claims for itself. Confirm live; the header-bar
            // buttons are the reliable fallback either way.
            if (keyval == GDK_KEY_AudioMicMute) {
                toggle_mic_mute();
                return true;
            }
            if (keyval == GDK_KEY_AudioMute) {
                toggle_speaker_mute();
                return true;
            }
            if (keyval == GDK_KEY_F11) {
                // Header bar hidden while fullscreen — besides matching
                // GNOME's own fullscreen apps, this sidesteps a real
                // bug: clicking the header bar's "restore" button while
                // fullscreen left the window showing solid black. With
                // no header bar visible, that button isn't there to
                // click. adw_toolbar_view_set_reveal_top_bars() animates
                // the transition rather than an abrupt set_visible().
                if (window->is_fullscreen()) {
                    window->unfullscreen();
                    adw_toolbar_view_set_reveal_top_bars(toolbar_view, TRUE);
                } else {
                    adw_toolbar_view_set_reveal_top_bars(toolbar_view, FALSE);
                    window->fullscreen();
                }
                return true;
            }
            if (keyval == GDK_KEY_Escape && window->is_fullscreen()) {
                window->unfullscreen();
                adw_toolbar_view_set_reveal_top_bars(toolbar_view, TRUE);
                return true;
            }
            if (!recording_session->viewing_equirect) {
                return false;
            }
            // What reads as "natural" scroll direction genuinely differs
            // from person to person, same distinction GNOME's own
            // mouse/touchpad settings expose. Traditional (default):
            // arrow keys move the viewport, bringing content from the
            // arrow's direction into the center. Natural: the opposite
            // sign, arrow keys drag the content instead. Read fresh
            // each press — a GSettings read is cheap.
            int sign = settings->get_boolean(kTraditionalScrollKey) ? 1 : -1;
            // Touches unconditionally (first press *and* every repeat) —
            // touch_head_movement()'s own comment.
            auto pan_key_pressed = [held_pan_keys, fire_head_prod,
                                     touch_head_movement](guint key) {
                touch_head_movement();
                bool is_repeat = held_pan_keys->count(key) > 0;
                held_pan_keys->insert(key);
                if (is_repeat) {
                    fire_head_prod();
                }
            };
            switch (keyval) {
                case GDK_KEY_Left:
                    // Reduced quality here — this press might be the
                    // start of a held/repeating key. The matching
                    // release, below, always lands one full-quality
                    // render once movement has actually stopped.
                    apply_yaw_delta(sign * kYawStepCentidegrees, /*fast=*/true);
                    pan_key_pressed(keyval);
                    return true;
                case GDK_KEY_Right:
                    apply_yaw_delta(-sign * kYawStepCentidegrees, /*fast=*/true);
                    pan_key_pressed(keyval);
                    return true;
                case GDK_KEY_Up:
                    apply_pitch_delta(sign * kPitchStepCentidegrees,
                                       /*fast=*/true, /*notify_if_disabled=*/true);
                    pan_key_pressed(keyval);
                    return true;
                case GDK_KEY_Down:
                    apply_pitch_delta(-sign * kPitchStepCentidegrees,
                                       /*fast=*/true, /*notify_if_disabled=*/true);
                    pan_key_pressed(keyval);
                    return true;
                default:
                    return false;
            }
        },
        false);
    // The full-quality counterpart to the fast render above — GTK only
    // emits one "released" per physical key-up even after a run of
    // repeated "pressed" signals, so this fires exactly once movement
    // stops.
    key_controller->signal_key_released().connect(
        [recording_session, render_current_view, fire_head_prod,
         touch_head_movement, held_pan_keys](guint keyval, guint,
                                              Gdk::ModifierType) {
            if (!recording_session->viewing_equirect) {
                return;
            }
            switch (keyval) {
                case GDK_KEY_Left:
                case GDK_KEY_Right:
                case GDK_KEY_Up:
                case GDK_KEY_Down:
                    held_pan_keys->erase(keyval);
                    touch_head_movement();
                    render_current_view(/*fast=*/false);
                    // Unconditional — "key up" is its own trigger
                    // regardless of whether autorepeat ever kicked in.
                    fire_head_prod();
                    break;
                default:
                    break;
            }
        });
    window->add_controller(key_controller);

    // Surface Dial support — read via raw evdev, not any GTK/GDK
    // signal: its rotation is REL_DIAL, a relative axis GTK/libinput
    // never turn into an ordinary scroll event (confirmed against the
    // real device with evtest). Optional hardware: silently does
    // nothing if it isn't connected or its event node can't be opened —
    // never an error, this is bonus input, not a dependency.
    //
    // Mapping: rotation -> yaw (apply_yaw_delta, same path the arrow
    // keys use, continuous instead of stepped); click -> a prod at the
    // exact viewport center, since that's where the head/gaze already
    // is. Only the "System Multi Axis" interface is read — the dial's
    // other one ("System Control") only carries SLEEP/WAKEUP power
    // keys, nothing this app uses.
    constexpr std::int32_t kDialYawStepCentidegrees = 20;  // 0.2° per tick
    // Reopens the Dial's evdev node whenever it's present — not just at
    // startup, since the Dial can drop out later (sleep, a battery
    // swap, Bluetooth instability) and needs retrying on reconnect.
    // dial_connected guards against opening a second fd if a udev event
    // fires while already watching one.
    //
    // "Lost contact with Surface Dial" toast — shown for as long as
    // contact is lost, dismissed the moment it's back. No timeout
    // (persistent until explicitly dismissed) — an auto-dismissing
    // toast would defeat the point of signaling the problem is ongoing.
    auto dial_lost_contact_toast = std::make_shared<AdwToast*>(nullptr);
    auto show_dial_lost_contact_toast = [toast_overlay,
                                          dial_lost_contact_toast]() {
        if (*dial_lost_contact_toast != nullptr) {
            return;  // already showing
        }
        AdwToast* toast = adw_toast_new(_("Lost contact with Surface Dial"));
        adw_toast_set_timeout(toast, 0);
        adw_toast_overlay_add_toast(toast_overlay, toast);
        *dial_lost_contact_toast = toast;
    };
    auto dismiss_dial_lost_contact_toast = [dial_lost_contact_toast]() {
        if (*dial_lost_contact_toast == nullptr) {
            return;
        }
        adw_toast_dismiss(*dial_lost_contact_toast);
        *dial_lost_contact_toast = nullptr;
    };
    auto dial_connected = std::make_shared<bool>(false);
    auto try_connect_dial = [dial_connected, dismiss_dial_lost_contact_toast,
                              show_dial_lost_contact_toast, recording_session,
                              apply_yaw_delta, render_current_view, settings,
                              window, kDialYawStepCentidegrees, fire_head_prod,
                              notify_pan_input]() {
        if (*dial_connected) {
            return;
        }
        std::string dial_path =
            find_evdev_by_name("Surface Dial System Multi Axis");
        if (dial_path.empty()) {
            return;
        }
        int dial_fd = open(dial_path.c_str(), O_RDONLY | O_NONBLOCK);
        if (dial_fd < 0) {
            // Found the sysfs entry but couldn't open it (e.g. `input`
            // group membership not applied yet) — worth logging
            // explicitly, easy to mistake for the Dial simply not being
            // there.
            g_message("blind-sight: Surface Dial found at %s but open() "
                       "failed: %s",
                       dial_path.c_str(), std::strerror(errno));
            return;
        }
        *dial_connected = true;
        g_message("blind-sight: Surface Dial connected (%s)",
                   dial_path.c_str());
        dismiss_dial_lost_contact_toast();
        // Debounces the same fast-while-moving/full-once-settled render
        // split a held arrow key gets — rotation has no discrete
        // "released," so a short idle timer stands in: reset on every
        // tick, fires once spinning actually stops.
        auto settle_connection = std::make_shared<sigc::connection>();
        Glib::signal_io().connect(
            [dial_fd, dial_connected, show_dial_lost_contact_toast,
             recording_session, apply_yaw_delta, settle_connection,
             render_current_view, settings, window, kDialYawStepCentidegrees,
             fire_head_prod, notify_pan_input](Glib::IOCondition condition) -> bool {
                // The device disappearing (confirmed: this dial drops
                // off after ~5 min 20s idle, and the Bluetooth link
                // itself is genuinely unstable) leaves this fd
                // permanently hung up, and poll() reports HUP/ERR in
                // revents regardless of what's requested (POSIX), so
                // without this check the source fires on every
                // main-loop iteration forever — a confirmed bug: other
                // windows on the desktop stalled on drag, with `top`
                // showing this process pegged at 100% CPU. Close and
                // stop watching; dial_connected flips back to false so
                // the next udev "add" event reopens it via
                // try_connect_dial().
                if ((condition & (Glib::IOCondition::IO_HUP |
                                   Glib::IOCondition::IO_ERR)) !=
                    Glib::IOCondition(0)) {
                    // Known gap: this only fires when the kernel
                    // actually tears down the uhid device. BlueZ can
                    // lose the Bluetooth link entirely while the
                    // kernel-side evdev node lingers untouched, so no
                    // HUP ever arrives and this branch never runs. Not
                    // yet handled; would need BlueZ's own Connected
                    // property, not just HUP.
                    close(dial_fd);
                    *dial_connected = false;
                    g_message("blind-sight: Surface Dial disconnected "
                               "(HUP/ERR on fd)");
                    show_dial_lost_contact_toast();
                    return false;  // stop watching this fd
                }
                struct input_event ev;
                while (read(dial_fd, &ev, sizeof(ev)) ==
                       static_cast<ssize_t>(sizeof(ev))) {
                    if (!recording_session->viewing_equirect) {
                        continue;  // nothing to pan
                    }
                    // /dev/input/eventN has no notion of window focus —
                    // the kernel broadcasts every event to all open file
                    // descriptors regardless of which window is active
                    // (confirmed: two instances side by side both
                    // panned from the same physical Dial at once).
                    // Ordinary keyboard/mouse input is routed by the
                    // window manager; raw evdev reads bypass that, so
                    // it has to be done by hand here.
                    if (!window->is_active()) {
                        continue;  // still drain the fd, just don't act
                    }
                    if (ev.type == EV_REL && ev.code == REL_DIAL &&
                        settings->get_string(kYawAugmentedInputKey) ==
                            "surface-dial") {
                        // Same traditional-vs-natural distinction the
                        // arrow keys respect: the dial's raw,
                        // uncorrected direction always turns the
                        // content, matching Natural but backwards for
                        // Traditional (the default). `-sign`, not
                        // `sign`, since the dial's uncorrected direction
                        // already *was* what Natural wants.
                        int sign =
                            settings->get_boolean(kTraditionalScrollKey) ? 1
                                                                          : -1;
                        apply_yaw_delta(-sign *
                                             static_cast<std::int32_t>(ev.value) *
                                             kDialYawStepCentidegrees,
                                         /*fast=*/true);
                        settle_connection->disconnect();
                        *settle_connection = Glib::signal_timeout().connect(
                            [render_current_view]() {
                                render_current_view(/*fast=*/false);
                                return false;  // one-shot
                            },
                            150);
                        notify_pan_input();
                    } else if (ev.type == EV_KEY && ev.code == BTN_0 &&
                               ev.value == 1 &&
                               settings->get_string(kYawAugmentedInputKey) ==
                                   "surface-dial") {
                        // Gated the same as rotation above — the dial is
                        // either fully active or not at all.
                        // fire_head_prod() itself still gates the
                        // *logged* ProdEvent on recording_session->file;
                        // the glow shows regardless, same as the
                        // mouse-click prod.
                        fire_head_prod();
                    }
                }
                return true;  // keep watching
            },
            dial_fd,
            Glib::IOCondition::IO_IN | Glib::IOCondition::IO_HUP |
                Glib::IOCondition::IO_ERR);
    };
    try_connect_dial();

    // GUdev client watching the "input" subsystem for hotplug events —
    // the fix for the "isn't reopened if it reconnects" gap above.
    // Deliberately coarse: rather than parsing GUdevDevice properties
    // to check whether *this specific* uevent is the Surface Dial,
    // every "add" event on the subsystem just re-runs
    // try_connect_dial(), reusing the already-verified
    // find_evdev_by_name() sysfs scan. Input hotplug events are rare,
    // so the small extra scan cost per event is a non-issue.
    //
    // udev_client is deliberately never unreffed — an app-lifetime
    // singleton, same as the main window.
    const char* udev_subsystems[] = {"input", nullptr};
    GUdevClient* udev_client = g_udev_client_new(udev_subsystems);
    g_signal_connect_data(
        udev_client, "uevent",
        G_CALLBACK(+[](GUdevClient*, const gchar* action, GUdevDevice* device,
                        gpointer data) {
            if (g_strcmp0(action, "add") == 0) {
                // Logs every input-subsystem add, not just Dial-related
                // ones — the device name lets the log distinguish
                // signal from noise, and seeing the *rate* of these via
                // journalctl is what makes connection-instability
                // debugging possible.
                const gchar* name = g_udev_device_get_name(device);
                g_message("blind-sight: udev input add: %s",
                           name ? name : "(unnamed)");
                (*static_cast<std::function<void()>*>(data))();
            }
        }),
        new std::function<void()>(try_connect_dial),
        +[](gpointer data, GClosure*) {
            delete static_cast<std::function<void()>*>(data);
        },
        GConnectFlags(0));

    // Mouse scroll wheel as an optional augmented input — ordinary GTK
    // scroll events, unlike the dial's raw evdev REL_DIAL, since
    // Gtk::EventControllerScroll already handles REL_WHEEL/smooth-scroll
    // natively. Vertical scroll (dy) drives tilt; horizontal scroll
    // (dx, a tilting wheel or shift+scroll) drives yaw. Each axis is
    // gated independently by its own Preferences setting, same as the
    // dial — arrow keys keep working regardless of either.
    auto scroll_controller = Gtk::EventControllerScroll::create();
    scroll_controller->set_flags(Gtk::EventControllerScroll::Flags::BOTH_AXES);
    // Same fast-while-scrolling/full-once-settled debounce as the dial's
    // own rotation handling — a burst of scroll events has no discrete
    // "released" the way a keypress does either.
    auto scroll_settle_connection = std::make_shared<sigc::connection>();
    scroll_controller->signal_scroll().connect(
        [recording_session, apply_yaw_delta, apply_pitch_delta, settings,
         render_current_view, scroll_settle_connection, notify_pan_input,
         kYawStepCentidegrees, kPitchStepCentidegrees](double dx,
                                                        double dy) -> bool {
            if (!recording_session->viewing_equirect) {
                return false;
            }
            int sign = settings->get_boolean(kTraditionalScrollKey) ? 1 : -1;
            bool handled = false;
            if (dx != 0.0 &&
                settings->get_string(kYawAugmentedInputKey) == "scroll-wheel") {
                apply_yaw_delta(
                    sign * static_cast<std::int32_t>(std::lround(dx)) *
                        kYawStepCentidegrees,
                    /*fast=*/true);
                handled = true;
            }
            if (dy != 0.0 && settings->get_string(kTiltAugmentedInputKey) ==
                                  "scroll-wheel") {
                apply_pitch_delta(
                    -sign * static_cast<std::int32_t>(std::lround(dy)) *
                        kPitchStepCentidegrees,
                    /*fast=*/true);
                handled = true;
            }
            if (handled) {
                scroll_settle_connection->disconnect();
                *scroll_settle_connection = Glib::signal_timeout().connect(
                    [render_current_view]() {
                        render_current_view(/*fast=*/false);
                        return false;  // one-shot
                    },
                    150);
                notify_pan_input();
            }
            return handled;
        },
        false);
    window->add_controller(scroll_controller);

    window->present();

    // No background threads yet, so the default Gtk::Application
    // shutdown (last window closes -> run() returns -> process exits)
    // is sufficient. Revisit when the audio render thread lands: it
    // must be stopped and joined on shutdown, or it becomes a dangling
    // process.
}

}  // namespace

// Bridges to this file's anonymous namespace above, needed now that
// preferences.cpp/transcription.cpp are separate translation units —
// everything above has internal linkage, so no forward-declaring it
// would let another .cpp call it. Thin forwards only; the real
// implementations are unchanged. Distinct names, not overloads, to
// avoid ambiguity for unqualified call sites inside the namespace.
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body) {
    show_error_dialog(parent, heading, body);
}
void bridge_show_setup_required_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                         const Glib::ustring& body,
                                         const Glib::ustring& command) {
    show_setup_required_dialog(parent, heading, body, command);
}
int bridge_get_viewing_distance_mm() { return g_viewing_distance_mm; }
void bridge_set_viewing_distance_mm(int value) { g_viewing_distance_mm = value; }
std::string bridge_resource_path(const std::string& filename) {
    return resource_path(filename);
}
void bridge_apply_last_folder(const Glib::RefPtr<Gtk::FileDialog>& file_dialog,
                                const Glib::RefPtr<Gio::Settings>& settings,
                                const char* key) {
    apply_last_folder(file_dialog, settings, key);
}
void bridge_remember_last_folder(const Glib::RefPtr<Gio::Settings>& settings,
                                   const char* key, const Glib::RefPtr<Gio::File>& file) {
    remember_last_folder(settings, key, file);
}
void bridge_set_button_icon_from_file(Gtk::Button* button, const std::string& filename) {
    set_button_icon_from_file(button, filename);
}
void bridge_open_project_by_dir(const std::string& dir) {
    if (g_open_project_by_dir) {
        g_open_project_by_dir(dir);
    }
}
void bridge_start_new_project(std::function<void()> on_done) {
    if (g_start_new_project) {
        g_start_new_project(on_done);
    }
}
void bridge_start_new_session(std::function<void()> on_done) {
    if (g_start_new_session) {
        g_start_new_session(on_done);
    }
}
void bridge_confirm_delete_entry(Gtk::Window* parent, const Glib::ustring& title,
                                   const Glib::ustring& body,
                                   std::function<void()> on_delete) {
    confirm_delete_entry(parent, title, body, std::move(on_delete));
}
void bridge_release_image_references(const std::string& project_dir,
                                        const std::string& session_path) {
    release_image_references(project_dir, session_path);
}
std::pair<Glib::ustring, Glib::ustring> bridge_read_image_title_caption(
    const std::string& path) {
    auto meta = read_image_metadata(path);
    return {meta.title, meta.caption};
}
std::pair<Glib::ustring, Glib::ustring> bridge_read_image_creator_copyright(
    const std::string& path) {
    auto meta = read_image_metadata(path);
    return {meta.creator, meta.copyright};
}
Glib::ustring bridge_truncate_dropdown_title(const Glib::ustring& title, int max_chars) {
    return truncate_dropdown_title(title, max_chars);
}
void bridge_dismiss_splash() { dismiss_splash(); }
// image_info_for_track()/TrackImageInfo have internal linkage; only the
// `scenario` field is needed outside this file (track_dialogs.cpp's own
// row display, matching the live H2 derivation the track dropdown's rows
// already show — see tracks.hpp's own comment on why this stays a
// derivation, not a stored copy).
std::string bridge_scenario_for_track(const std::vector<blind_sight::RecordEvent>& events,
                                       std::int32_t track_start_ms,
                                       std::int32_t track_end_ms) {
    return image_info_for_track(events, track_start_ms, track_end_ms).scenario;
}

int main(int argc, char** argv) {
    // Dev-time GSettings schema lookup — points GLib at demo/data/
    // (compiled by CMake) instead of requiring the schema installed
    // system-wide. Has to run before anything touches GSettings at all
    // — first line of main(), not just "early."
    g_setenv("GSETTINGS_SCHEMA_DIR", BLIND_SIGHT_SCHEMA_DIR, TRUE);

    // Glib::wrap() needs gtkmm's per-module wrap tables registered
    // before it can resolve a GObject to a C++ wrapper, and that
    // registration only happens as a side effect of constructing a
    // real gtkmm object. Wrapping a raw AdwApplication* here, before
    // any such object existed, crashed with "wrap_func_table !=
    // nullptr". Fix: create the application through
    // Gtk::Application::create() and initialize libadwaita explicitly
    // instead of routing through AdwApplication — AdwApplicationWindow
    // only requires a GtkApplication, so this loses nothing.
    gtk_init();
    adw_init();
    // gexiv2 requires this before any of its other functions are used —
    // read_image_metadata() is the only caller.
    gexiv2_initialize();
    // Same requirement — GStreamer for the Record action bar's
    // microphone capture.
    gst_init(&argc, &argv);
    // Same requirement again — native PipeWire for the noise-pulse
    // engine. Without this, NoisePulseEngine::start() runs but never
    // registers as a PipeWire client at all — confirmed live, `wpctl
    // status` showing no "blind-sight" client while a prod produced no
    // sound.
    pw_init(&argc, &argv);

    // Gettext bootstrap — catalogs are in locale/<lang>/LC_MESSAGES/ under
    // the build tree, compiled from po/*.po by the build.
    setlocale(LC_ALL, "");
    bindtextdomain(kGettextDomain, BLIND_SIGHT_LOCALE_DIR);
    bind_textdomain_codeset(kGettextDomain, "UTF-8");
    textdomain(kGettextDomain);

    // HANDLES_COMMAND_LINE, not the simpler HANDLES_OPEN — needed once a
    // second, *named* argument ("--session <file>") joined the plain
    // positional one. HANDLES_OPEN only hands GApplication's file-list
    // straight to signal_open(); it has no way to carry a separate
    // flag's value through signal_command_line()'s single-instance-
    // correct round-trip. This app still does its own hand-rolled
    // parsing below rather than registering option entries — this flag
    // buys single-instance forwarding, not parsing help.
    auto app =
        Gtk::Application::create(kAppId, Gio::Application::Flags::HANDLES_COMMAND_LINE);
    app->signal_activate().connect([app]() {
        load_app_css();
        register_app_icons();
        on_activate(app);
    });
    app->signal_command_line().connect(
        [app](const Glib::RefPtr<Gio::ApplicationCommandLine>& cmdline) -> int {
            load_app_css();
            register_app_icons();
            // Builds the window on a genuinely first launch; on
            // reactivation while already running, this just presents
            // the existing one and returns (its own existing guard) —
            // either way, g_open_project_by_dir/g_open_session_by_path
            // are valid by the time this runs.
            on_activate(app);

            // The only two forms this app understands: one bare,
            // non-flag argument (the project path) and "--session
            // <file>" (order-independent). Everything else is silently
            // ignored rather than treated as an error.
            int cmd_argc = 0;
            char** cmd_argv = cmdline->get_arguments(cmd_argc);
            std::string project_arg;
            std::string session_arg;
            for (int i = 1; i < cmd_argc; ++i) {
                std::string arg = cmd_argv[i];
                if (arg == "--session" && i + 1 < cmd_argc) {
                    session_arg = cmd_argv[++i];
                } else if (project_arg.empty() && arg.rfind("--", 0) != 0) {
                    project_arg = arg;
                }
            }
            g_strfreev(cmd_argv);

            if (!project_arg.empty() && g_open_project_by_dir) {
                // A *file* path's parent directory is the project; a
                // *directory* path is the project directly, found via
                // its own protocol.md.
                std::string project_dir =
                    std::filesystem::is_directory(project_arg)
                        ? project_arg
                        : Glib::path_get_dirname(project_arg);
                // A failed command-line path still shows its own error
                // dialog (open_project_at()'s job) — this makes sure the
                // app doesn't end up silently stuck on whatever
                // on_activate()'s last-project auto-reopen just loaded.
                if (!g_open_project_by_dir(project_dir) && g_reset_to_no_project) {
                    g_reset_to_no_project();
                }
            }
            if (!session_arg.empty() && g_open_session_by_path) {
                g_open_session_by_path(session_arg);
            } else if (g_reapply_session_autoload) {
                // No explicit --session this run — a --project
                // argument may just have made g_project_active true,
                // which autoload-session couldn't know yet when
                // on_activate() first tried it.
                g_reapply_session_autoload();
            }
            // Both open_project_at() and apply_shared_session() reset
            // the shared picture to "No image" by default — put the
            // autoloaded image back now that a project may be active,
            // or launching into the right project and session would
            // still land on "No image". Self-gated on g_project_active,
            // so a no-op without a project.
            if (g_reapply_last_image_autoload) {
                g_reapply_last_image_autoload();
            }
            return 0;
        },
        false);

    return app->run(argc, argv);
}
