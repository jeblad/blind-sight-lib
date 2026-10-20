// SPDX-License-Identifier: GPL-3.0-only
//
// preferences.hpp — the Preferences dialog (AdwPreferencesDialog): gaze
// cursor, prod cue, viewport navigation, panning input, audio processing
// (transcode/transcribe/diarize), depth generation, point data, and the
// noise-pulse carrier.

#pragma once

#include <adwaita.h>
#include <gtkmm.h>

#include <map>
#include <string>
#include <utility>

void show_preferences(Gtk::Window* parent, const Glib::RefPtr<Gio::Settings>& settings,
                       AdwToastOverlay* toast_overlay);

// Name -> pinned two-letter bigram (monitoring_dialogs.cpp's own header
// labels, assign_bigrams()) — normally just a couple of entries, see the
// "preferred-bigrams" GSettings key's own description. Exposed here
// rather than kept local to preferences.cpp since monitoring_dialogs.cpp
// needs read access when building the matrix's header buttons.
std::map<Glib::ustring, Glib::ustring> get_preferred_bigrams(
    const Glib::RefPtr<Gio::Settings>& settings);
void set_preferred_bigrams(const Glib::RefPtr<Gio::Settings>& settings,
                            const std::map<Glib::ustring, Glib::ustring>& bigrams);

// A fixed participant's own preferred device, one map per axis — the
// same (device id, display name) shape as monitoring.hpp's own
// DeviceAssignment, just not that type directly, since this file has no
// reason to depend on monitoring.hpp. Keyed the same way as
// get_preferred_bigrams()'s map plus the two ambient sentinel ids
// (blind_sight::kSpeakerSinkId/kAmbientMicSourceId as plain strings —
// this header doesn't include monitoring.hpp either, so these are
// spelled out at each call site instead of named here). An empty device
// id (or no entry at all) means "no preferred device, assign it by hand
// each session" — see seed_preferred_devices() (monitoring_dialogs.hpp)
// for how these are actually applied to a session's live state, and why
// that's a one-way default rather than a lock.
std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>> get_preferred_sink_devices(
    const Glib::RefPtr<Gio::Settings>& settings);
void set_preferred_sink_devices(
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>>& devices);
std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>> get_preferred_source_devices(
    const Glib::RefPtr<Gio::Settings>& settings);
void set_preferred_source_devices(
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>>& devices);

// Per-id volumes, keyed like the preferred devices above: a listener's
// synthesized and participants volumes, and a speaker's voice (source)
// volume. See the matching schema keys for what each one is applied to.
std::map<std::string, double> get_synthesized_volumes(
    const Glib::RefPtr<Gio::Settings>& settings);
std::map<std::string, double> get_participants_volumes(
    const Glib::RefPtr<Gio::Settings>& settings);
std::map<std::string, double> get_source_volumes(const Glib::RefPtr<Gio::Settings>& settings);
void set_synthesized_volume(const Glib::RefPtr<Gio::Settings>& settings, const std::string& id,
                            double volume);
void set_participants_volume(const Glib::RefPtr<Gio::Settings>& settings,
                             const std::string& id, double volume);
void set_source_volume(const Glib::RefPtr<Gio::Settings>& settings, const std::string& id,
                       double volume);

// Per-id voice levelling on/off (source-agc); no entry means on.
std::map<std::string, bool> get_source_agc(const Glib::RefPtr<Gio::Settings>& settings);
void set_source_agc(const Glib::RefPtr<Gio::Settings>& settings, const std::string& id,
                    bool enabled);
