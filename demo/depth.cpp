// SPDX-License-Identifier: GPL-3.0-only
//
// depth.cpp — see depth.hpp for scope.

#include "depth.hpp"

#include <glib/gi18n.h>

#include <cctype>
#include <filesystem>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputFile.h>

#include "transcription.hpp"

// Bridges into main.cpp's own anonymous namespace (see its own comment
// there, right after that namespace closes) — stays defined in main.cpp
// but needed here too.
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body);

namespace {

constexpr auto kAppId = "io.github.jeblad.blind-sight-demo";
constexpr auto kDepthGenerationEnabledKey = "depth-generation-enabled";

}  // namespace

// Where the depth-map tool's Python venv lives — per-user, writable,
// regenerable state, so it never belongs in the source tree.
// Glib::get_user_cache_dir() is the portable way to get $XDG_CACHE_HOME.
std::string depth_venv_dir() {
    return Glib::build_filename(Glib::get_user_cache_dir(), kAppId, "depth-venv");
}

// BLIND_SIGHT_DEPTH_DIR (the source tree's demo/depth/) only ever holds
// the *script*, generate_depth.py; the venv it runs in is
// depth_venv_dir(), not underneath it.
//
// Naming convention: <image>.depth.exr next to the source image.
std::string depth_file_path_for(const std::string& image_path) {
    std::filesystem::path p(image_path);
    return (p.parent_path() / (p.stem().string() + ".depth.exr")).string();
}

bool has_depth_file(const std::string& image_path) {
    if (image_path.empty()) {
        return false;
    }
    return std::filesystem::exists(depth_file_path_for(image_path));
}

// A cheap, definitive check — the venv setup-depth-env.sh creates is
// either there or it isn't.
bool depth_generation_available() {
    return std::filesystem::exists(depth_venv_dir() + "/bin/python3");
}

// Checked when the user turns "Use Fiducial Markers" on in Preferences —
// not the cheap "does the venv exist" check the other *_available()
// functions use, since OpenCV isn't in demo/depth/requirements.txt yet.
// A real, brief, synchronous import check — the one place in this
// module that blocks on a subprocess rather than going through
// wait_async(), since this only runs once, when the switch is clicked.
bool aruco_detection_available() {
    try {
        auto proc = Gio::Subprocess::create(
            {depth_venv_dir() + "/bin/python3", "-c", "import cv2.aruco"},
            Gio::Subprocess::Flags::NONE);
        proc->wait();
        return proc->get_successful();
    } catch (const Glib::Error&) {
        return false;
    }
}

namespace {

// HF Hub cache dir naming convention, same as diarization/whisper model
// caching — "org/repo" -> "models--org--repo". Its own function so a
// future delete-the-model feature can point at the exact same path
// without re-deriving it.
std::string depth_model_cache_path(const std::string& model_size) {
    std::string size_capitalized = model_size;
    if (!size_capitalized.empty()) {
        size_capitalized[0] =
            static_cast<char>(std::toupper(static_cast<unsigned char>(size_capitalized[0])));
    }
    return Glib::get_home_dir() +
           "/.cache/huggingface/hub/"
           "models--depth-anything--Depth-Anything-V2-" +
           size_capitalized + "-hf";
}

}  // namespace

// Whether the Depth-Anything-V2 model for a given size is downloaded —
// same cheap disk-check pattern as whisper_model_cached().
bool depth_model_cached(const std::string& model_size) {
    return std::filesystem::exists(depth_model_cache_path(model_size));
}

// Reads the "Z" channel written by demo/depth/generate_depth.py — a
// single-channel half-float EXR, requested here as FLOAT (OpenEXR
// converts on read, no manual half-to-float unpacking needed). Returns
// false on any read failure — a missing/corrupt file just means the
// pointer readout shows nothing, not that the app fails to load the
// image.
bool load_depth_exr(const std::string& path, std::vector<float>& out,
                     int& out_width, int& out_height) {
    try {
        Imf::InputFile file(path.c_str());
        Imath::Box2i dw = file.header().dataWindow();
        int width = dw.max.x - dw.min.x + 1;
        int height = dw.max.y - dw.min.y + 1;
        std::vector<float> data(static_cast<std::size_t>(width) * height);
        Imf::FrameBuffer frame_buffer;
        // xStride/yStride left at their default (0): Make() computes
        // them from the FLOAT pixel size and dw's width. Passing
        // width/height there directly is wrong (those are byte stride
        // counts, not pixel dimensions) and was the actual bug behind an
        // observed "depth: 0.00 everywhere" readout — readPixels() only
        // wrote a tiny fraction of `data`, leaving the rest zeroed.
        frame_buffer.insert("Z", Imf::Slice::Make(Imf::FLOAT, data.data(), dw));
        file.setFrameBuffer(frame_buffer);
        file.readPixels(dw.min.y, dw.max.y);
        out = std::move(data);
        out_width = width;
        out_height = height;
        return true;
    } catch (const std::exception& e) {
        g_message("blind-sight: failed to read depth file %s: %s",
                   path.c_str(), e.what());
        return false;
    }
}

// Same auto-subprocess-on-toggle shape as transcription.cpp's
// start_diarization_model_prefetch() — no HuggingFace account/token
// setup needed first (Depth-Anything-V2 isn't gated), so this can just
// launch the download directly the moment the switch is flipped on.
void start_depth_model_prefetch(Gtk::Window* parent,
                                 const Glib::RefPtr<Gio::Settings>& settings,
                                 AdwToastOverlay* toast_overlay, AdwSwitchRow* row,
                                 const std::string& model_size) {
    struct DepthPrefetchContext {
        Gtk::Window* parent;
        Glib::RefPtr<Gio::Settings> settings;
        GWeakRef row_ref;
    };
    auto* pctx = new DepthPrefetchContext();
    pctx->parent = parent;
    pctx->settings = settings;
    g_weak_ref_init(&pctx->row_ref, row);

    AdwToast* toast = adw_toast_new(_("Downloading depth model…"));
    adw_toast_set_timeout(toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, toast);
    try {
        std::vector<std::string> argv = {
            depth_venv_dir() + "/bin/python3",
            std::string(BLIND_SIGHT_DEPTH_DIR) + "/prefetch_depth_model.py",
            "--model-size", model_size};
        // Opportunistic HF_TOKEN — not required (this repo isn't
        // gated), just a rate-limit courtesy when one's already in the
        // keyring.
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
            [proc, toast, pctx](Glib::RefPtr<Gio::AsyncResult>& result) {
                proc->wait_finish(result);
                bool ok = proc->get_successful();
                adw_toast_dismiss(toast);
                pctx->settings->set_boolean(kDepthGenerationEnabledKey, ok);
                if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&pctx->row_ref))) {
                    adw_switch_row_set_active(ADW_SWITCH_ROW(obj), ok);
                    g_object_unref(obj);
                }
                if (!ok) {
                    g_message("blind-sight: depth model prefetch failed");
                    bridge_show_error_dialog(
                        pctx->parent, _("Depth Model Download Failed"),
                        _("Downloading the depth model failed — check "
                          "your network connection, then try again."));
                }
                g_weak_ref_clear(&pctx->row_ref);
                delete pctx;
            });
    } catch (const Glib::Error& e) {
        adw_toast_dismiss(toast);
        g_message("blind-sight: failed to launch depth model prefetch: %s", e.what());
        settings->set_boolean(kDepthGenerationEnabledKey, false);
        if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&pctx->row_ref))) {
            adw_switch_row_set_active(ADW_SWITCH_ROW(obj), FALSE);
            g_object_unref(obj);
        }
        g_weak_ref_clear(&pctx->row_ref);
        delete pctx;
    }
}
