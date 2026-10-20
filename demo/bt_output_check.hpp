// SPDX-License-Identifier: GPL-3.0-only
//
// bt_output_check.hpp — warns once per distinct Bluetooth output device
// that gaze/saccade-driven sonification (the live "prod" cues,
// NoisePulseEngine) can't keep up with typical Bluetooth audio latency.
// See bt_output_check.cpp's own top comment and demo/check_bt_output.py.

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <memory>
#include <string>

// Call every time Interactive mode is (re-)shown — see main.cpp's
// show_interactive_bar(). Cheap and async: never blocks the mode switch,
// and does nothing visible unless the default output is genuinely
// Bluetooth. `last_warned_device` is this run's own state (create one
// shared_ptr<std::string> in on_activate(), pass the same one every
// call) — "" means nothing warned about yet. Only shows the toast when
// the detected device differs from last time, so returning to
// Interactive repeatedly with the same still-connected device doesn't
// re-nag.
void check_bt_output_warning(AdwToastOverlay* toast_overlay,
                              std::shared_ptr<std::string> last_warned_device);
