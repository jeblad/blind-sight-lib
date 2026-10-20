// SPDX-License-Identifier: GPL-3.0-only
//
// monitoring_dialogs.hpp — the monitoring matrix popover's content (who
// hears whom) and the device-label dialog reached from it, by clicking
// a row/column's own header button — participants, Speaker, and the
// ambient mic column all open the same dialog, just keyed by a
// different id. See monitoring.hpp for the underlying in-memory data
// model. Device labeling is a free-text placeholder this round — there
// is no real PipeWire device enumeration anywhere in this codebase yet.

#pragma once

#include <gtkmm.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio_devices.hpp"
#include "monitoring.hpp"

namespace blind_sight {
class AudioRoutingEngine;
}

// The popover's content widget, plus a refresh callback the caller must
// invoke every time the popover is about to show (e.g. from
// signal_show()) — the grid is rebuilt from scratch against the
// session's current RecordMeta at that point, not kept live otherwise.
struct MonitoringMatrixWidget {
    Gtk::Widget* widget;
    std::function<void()> refresh;
};

// `state` is the live, in-memory monitoring state for the current
// session (owned by RecordingSession in main.cpp) — this call does not
// take ownership, `state` must outlive the returned widget.
// `get_session_path` is called fresh every refresh, not read once at
// construction time, so a session change is picked up correctly.
// `popover_ptr` is a forward reference, same idiom as this file's own
// callers use elsewhere for a not-yet-constructed widget (e.g.
// main.cpp's project_details_button_ptr) — the Popover itself can only
// be built once this content already exists, but the header buttons
// built here need to close it before opening a dialog on top. The
// caller fills in `*popover_ptr` right after constructing the Popover.
// `routing_engine` is non-owning (lives for the app's whole process
// lifetime, owned by main.cpp) — synced after every mutation reachable
// from this widget, so the matrix's mute state and device assignments
// actually produce live audio (audio_routing.hpp).
MonitoringMatrixWidget build_monitoring_matrix_content(
    Gtk::Window* parent, blind_sight::MonitoringState* state,
    std::function<std::string()> get_session_path,
    std::shared_ptr<Gtk::Popover*> popover_ptr, const Glib::RefPtr<Gio::Settings>& settings,
    blind_sight::AudioRoutingEngine* routing_engine);

// Real device picker — see monitoring_dialogs.cpp's own comment. No
// MonitoringState coupling at all, so preferences.cpp reuses this
// directly for the Preferred Devices group's own device pickers rather
// than building a second copy of this dialog. `footer`, if given, goes
// below the device list. Returns a function that selects the row for a
// device id (empty: the "None" row), for a caller keeping the dialog open
// across picks.
std::function<void(const std::string& device_id)> show_device_picker_dialog(
    Gtk::Window* parent, const Glib::ustring& title, const Glib::ustring& none_row_label,
    const std::string& gst_device_class, const std::string& current_device_id,
    std::function<void(std::vector<blind_sight::AudioDeviceInfo>&)> annotate_devices,
    std::function<void(const blind_sight::AudioDeviceInfo*, std::function<void()> close)>
        on_save,
    Gtk::Widget* footer = nullptr);

// Fills in a default device assignment for every fixed participant
// (Ambient Speaker/Microphone, and any participant with a preferred
// bigram) that doesn't already have one in `state` — from Preferences'
// own "Preferred Devices" list (get_preferred_sink_devices()/
// get_preferred_source_devices(), preferences.hpp). A default only:
// never overwrites an existing assignment, so it's freely overridable
// within a session and, once overridden, never reseeded. Called once at
// session load (main.cpp) and again every matrix-popover refresh (a
// participant who only appears partway through a session still gets
// seeded the first time they show up).
void seed_preferred_devices(blind_sight::MonitoringState& state,
                             const Glib::RefPtr<Gio::Settings>& settings);
