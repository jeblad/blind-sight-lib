// SPDX-License-Identifier: GPL-3.0-only
//
// bt_output_check.cpp — see bt_output_check.hpp for scope. Bluetooth
// audio (A2DP) typically runs 150-400ms of real round-trip latency (see
// demo/trace_delay.py and this session's own measurements: ~348ms over
// SBC-XQ, vs. ~113-123ms for a wired USB path) — bigger than a whole
// inter-saccade interval, which is exactly the budget the gaze/saccade-
// driven sonification this app exists to test needs to fit inside. A
// Bluetooth output isn't just "a bit laggier," it's fundamentally
// incompatible with that sensorimotor loop, so this is worth flagging
// every time it's newly true, not just once ever. Exception: a device
// that actually negotiated a specifically low-latency codec (aptX-LL,
// FastStream) doesn't carry the same risk — this machine's PipeWire
// supports aptX-LL (confirmed in libspa-codec-bluez5-aptx.so's own
// symbols), but using it also needs the *connected device* to support
// it, which plenty of ordinary Bluetooth speakers/earbuds simply don't.

#include "bt_output_check.hpp"

#include <glaze/glaze.hpp>
#include <glib/gi18n.h>

#include <filesystem>
#include <fstream>

namespace {

// Field names must match check_bt_output.py's own JSON output exactly —
// glaze reflects this struct's members directly, same pattern as
// trace_delay.cpp's own TraceDelayResult.
struct BtOutputInfo {
    bool is_bluetooth = false;
    std::string device_name;
    std::string codec;
    bool low_latency_codec = false;
};

std::string new_temp_result_path() {
    return Glib::build_filename(
        Glib::get_tmp_dir(),
        "blind-sight-bt-check-" + std::to_string(g_get_monotonic_time()) + ".json");
}

}  // namespace

void check_bt_output_warning(AdwToastOverlay* toast_overlay,
                              std::shared_ptr<std::string> last_warned_device) {
    std::string output_path = new_temp_result_path();
    std::vector<std::string> argv = {"python3", BLIND_SIGHT_CHECK_BT_OUTPUT_SCRIPT,
                                       "--output", output_path};
    try {
        auto proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        proc->wait_async([proc, toast_overlay, last_warned_device,
                            output_path](Glib::RefPtr<Gio::AsyncResult>& result) {
            proc->wait_finish(result);
            std::error_code ec;
            if (!proc->get_successful()) {
                std::filesystem::remove(output_path, ec);
                return;  // silent — this runs on every mode switch, never
                         // worth interrupting the user over.
            }
            std::ifstream in(output_path);
            std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            std::filesystem::remove(output_path, ec);

            BtOutputInfo info;
            if (content.empty() || glz::read_json(info, content)) {
                return;  // malformed — treat as "nothing to warn about"
            }

            // A device that actually negotiated a low-latency codec
            // (aptX-LL, FastStream) doesn't carry the same risk this
            // warning exists for — see check_bt_output.py's own
            // LOW_LATENCY_CODECS comment for exactly which codec ids
            // qualify, confirmed against this machine's installed
            // spa-bluez5 plugins, not guessed.
            if (!info.is_bluetooth || info.low_latency_codec) {
                *last_warned_device = "";
                return;
            }
            if (info.device_name == *last_warned_device) {
                return;  // same device already warned about this session
            }
            *last_warned_device = info.device_name;

            Glib::ustring text =
                info.device_name.empty()
                    ? Glib::ustring(
                          _("Bluetooth speaker/headphones detected, expect high latency!"))
                    : Glib::ustring::compose(
                          _("Bluetooth speaker/headphones detected (%1), expect high "
                            "latency!"),
                          info.device_name);
            adw_toast_overlay_add_toast(toast_overlay, adw_toast_new(text.c_str()));
        });
    } catch (const Glib::Error& e) {
        g_message("blind-sight: failed to launch BT output check: %s", e.what());
    }
}
