// SPDX-License-Identifier: GPL-3.0-only
//
// trace_delay.hpp — "Trace Sound Delay", the hamburger menu's audio round-trip
// latency diagnostic. See trace_delay.cpp's own top comment and
// demo/trace_delay.py for the measurement itself; this is just the dialog
// flow around it (ask for the speaker-to-microphone distance, run the
// measurement, show the result).

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

void show_trace_delay_dialog(Gtk::Window* parent, AdwToastOverlay* toast_overlay);
