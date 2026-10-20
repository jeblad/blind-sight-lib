// SPDX-License-Identifier: GPL-3.0-only
//
// trace_delay.cpp — see trace_delay.hpp for scope. The measurement itself
// (click + simultaneous recording + onset detection, run several times)
// lives in trace_delay.py, launched as a one-shot subprocess the same way
// depth generation/transcription are — this file is just the distance
// dialog, the "Measuring…" toast around the subprocess, and the results
// dialog that reads back its JSON output.
//
// Why ask for a distance at all: the round-trip measurement itself needs
// no wired reference device (pw-record's own startup lag cancels out of
// the trigger/onset subtraction — see trace_delay.py's own top comment),
// but it does still include the real time sound takes to travel from the
// speaker to the microphone, which grows with distance. Subtracting that
// out needs to know the distance; there's no way to measure it
// acoustically from inside this same round trip.

#include "trace_delay.hpp"

#include <glaze/glaze.hpp>
#include <glib/gi18n.h>

#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

// Bridges into main.cpp's anonymous namespace, same pattern as every other
// *_dialogs.cpp file.
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body);

namespace {

// Field names/types must match trace_delay.py's own JSON output exactly —
// glaze reflects this struct's members directly, no separate schema to
// keep in sync by hand. -1 for pipewire_quantum_ms means "couldn't be
// determined" (see trace_delay.py) rather than a second optional type for
// no real benefit.
struct TraceDelayResult {
    bool success = false;
    std::string error;
    double latency_ms = 0;
    double latency_min_ms = 0;
    double latency_max_ms = 0;
    int valid_trials = 0;
    int total_trials = 0;
    double sound_travel_ms = 0;
    double corrected_latency_ms = 0;
    double pipewire_quantum_ms = -1;
    std::string codec;
    bool low_latency_codec = false;
};

std::string new_temp_result_path() {
    return Glib::build_filename(
        Glib::get_tmp_dir(),
        "blind-sight-trace-delay-" + std::to_string(g_get_monotonic_time()) + ".json");
}

void show_trace_delay_result(Gtk::Window* parent, const TraceDelayResult& result) {
    if (!result.success) {
        bridge_show_error_dialog(
            parent, _("Trace Sound Delay Failed"),
            result.error.empty() ? _("The measurement didn't produce a usable result.")
                                   : Glib::ustring(result.error));
        return;
    }

    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, _("Trace Sound Delay"));
    adw_dialog_set_content_width(dialog, 440);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::NONE);

    auto add_row = [list_box](const Glib::ustring& title, const Glib::ustring& subtitle) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
        list_box->append(*Glib::wrap(row, false));
    };

    std::ostringstream measured;
    measured << static_cast<int>(result.latency_ms + 0.5) << " ms (range "
              << static_cast<int>(result.latency_min_ms + 0.5) << "–"
              << static_cast<int>(result.latency_max_ms + 0.5) << " ms, "
              << result.valid_trials << "/" << result.total_trials << " valid trials)";
    add_row(_("Measured Round Trip"), measured.str());

    std::ostringstream travel;
    travel << "−" << static_cast<int>(result.sound_travel_ms + 0.5) << " ms";
    add_row(_("Sound Travel Time (from distance)"), travel.str());

    std::ostringstream corrected;
    corrected << static_cast<int>(result.corrected_latency_ms + 0.5) << " ms";
    add_row(_("Corrected Latency"), corrected.str());

    Glib::ustring quantum_text =
        result.pipewire_quantum_ms >= 0
            ? Glib::ustring::compose("%1 ms",
                                       static_cast<int>(result.pipewire_quantum_ms + 0.5))
            : Glib::ustring(_("Unknown"));
    add_row(_("PipeWire's Own Quantum (included above)"), quantum_text);

    Glib::ustring codec_text =
        result.codec.empty()
            ? Glib::ustring(_("Not Bluetooth, or unknown"))
            : Glib::ustring::compose(
                  "%1 (%2)", result.codec,
                  result.low_latency_codec ? _("low-latency") : _("not low-latency"));
    add_row(_("Output Codec"), codec_text);

    box->append(*list_box);

    // Always shown, not just on request — a missing contributor silently
    // folded into "the Bluetooth part" (or any other part) without this
    // note would be actively misleading, not just incomplete.
    auto* caveat_label = Gtk::make_managed<Gtk::Label>(
        _("Microphone capture latency is not isolated by this measurement "
          "— it's included in the corrected latency above, along with the "
          "output device's own latency. If either device is itself a USB "
          "audio interface (common even for \"wired\" speakers/"
          "headphones), that includes USB Audio Class buffering, not just "
          "acoustic/DAC delay."));
    caveat_label->set_wrap(true);
    caveat_label->set_halign(Gtk::Align::START);
    caveat_label->add_css_class("dim-label");
    box->append(*caveat_label);

    auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    button_box->set_halign(Gtk::Align::END);
    auto* close_button = Gtk::make_managed<Gtk::Button>(_("Close"));
    close_button->add_css_class("suggested-action");
    button_box->append(*close_button);
    box->append(*button_box);

    adw_dialog_set_child(dialog, GTK_WIDGET(box->gobj()));
    close_button->signal_clicked().connect([dialog]() { adw_dialog_close(dialog); });
    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

void run_trace_delay(Gtk::Window* parent, AdwToastOverlay* toast_overlay,
                       double distance_cm) {
    std::string output_path = new_temp_result_path();
    // std::to_string(double) follows the active C locale — under this
    // machine's nb_NO locale it writes a decimal comma ("5,000000"),
    // which Python's argparse (type=float) then rejects outright, since
    // float() only ever accepts a dot. std::format's default floating-
    // point formatting is locale-independent (always a dot) unless a
    // locale is explicitly passed in, which is exactly what's needed
    // here — the real bug this fixes, confirmed via a live run that
    // failed with exactly that comma in the error message.
    std::vector<std::string> argv = {"python3", BLIND_SIGHT_TRACE_DELAY_SCRIPT,
                                       "--distance-cm", std::format("{}", distance_cm),
                                       "--output", output_path};

    AdwToast* measuring_toast = adw_toast_new(_("Measuring delay…"));
    adw_toast_set_timeout(measuring_toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, measuring_toast);

    try {
        auto proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        proc->wait_async([proc, parent, measuring_toast,
                            output_path](Glib::RefPtr<Gio::AsyncResult>& result) {
            proc->wait_finish(result);
            adw_toast_dismiss(measuring_toast);
            if (!proc->get_successful()) {
                g_message("blind-sight: trace-delay script exited unsuccessfully");
                bridge_show_error_dialog(
                    parent, _("Trace Sound Delay Failed"),
                    _("The measurement script didn't run successfully."));
                return;
            }
            std::ifstream in(output_path);
            std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            std::error_code ec;
            std::filesystem::remove(output_path, ec);
            TraceDelayResult parsed;
            if (content.empty() || glz::read_json(parsed, content)) {
                bridge_show_error_dialog(
                    parent, _("Trace Sound Delay Failed"),
                    _("The measurement script's result couldn't be read."));
                return;
            }
            show_trace_delay_result(parent, parsed);
        });
    } catch (const Glib::Error& e) {
        adw_toast_dismiss(measuring_toast);
        g_message("blind-sight: failed to launch trace-delay script: %s", e.what());
        bridge_show_error_dialog(parent, _("Trace Sound Delay Failed"),
                                   _("The measurement script couldn't be started."));
    }
}

}  // namespace

void show_trace_delay_dialog(Gtk::Window* parent, AdwToastOverlay* toast_overlay) {
    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, _("Trace Sound Delay"));
    adw_dialog_set_content_width(dialog, 420);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* intro_label = Gtk::make_managed<Gtk::Label>(
        _("Measures the real round-trip delay from this app to audible "
          "sound, by playing a click and listening for it with the "
          "microphone. Needs the distance between the speaker and the "
          "microphone to subtract out sound's own travel time. Microphone "
          "capture latency can't be isolated this way — it stays folded "
          "into the result, and the result dialog says so explicitly."));
    intro_label->set_wrap(true);
    intro_label->set_halign(Gtk::Align::START);
    box->append(*intro_label);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::NONE);

    auto* distance_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(distance_row),
                                    _("Speaker-to-Microphone Distance (cm)"));
    list_box->append(*Glib::wrap(distance_row, false));
    box->append(*list_box);

    auto* button_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
    button_box->set_halign(Gtk::Align::END);
    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    auto* measure_button = Gtk::make_managed<Gtk::Button>(_("Measure"));
    measure_button->add_css_class("suggested-action");
    // Starts insensitive — a positive distance is required, same
    // recompute-on-change pattern show_new_project_dialog() (project_
    // dialogs.cpp) uses for its own Create button.
    measure_button->set_sensitive(false);
    button_box->append(*cancel_button);
    button_box->append(*measure_button);
    box->append(*button_box);

    auto recompute_sensitive = std::make_shared<std::function<void()>>();
    *recompute_sensitive = [distance_row, measure_button]() {
        try {
            double value = std::stod(gtk_editable_get_text(GTK_EDITABLE(distance_row)));
            measure_button->set_sensitive(value > 0.0);
        } catch (const std::exception&) {
            measure_button->set_sensitive(false);
        }
    };
    g_signal_connect_data(
        distance_row, "changed",
        G_CALLBACK(+[](GtkEditable*, gpointer data) {
            (*static_cast<std::shared_ptr<std::function<void()>>*>(data))->operator()();
        }),
        new std::shared_ptr<std::function<void()>>(recompute_sensitive),
        +[](gpointer data, GClosure*) {
            delete static_cast<std::shared_ptr<std::function<void()>>*>(data);
        },
        GConnectFlags(0));

    adw_dialog_set_child(dialog, GTK_WIDGET(box->gobj()));

    cancel_button->signal_clicked().connect([dialog]() { adw_dialog_close(dialog); });
    measure_button->signal_clicked().connect(
        [dialog, parent, toast_overlay, distance_row]() {
            double distance_cm =
                std::stod(gtk_editable_get_text(GTK_EDITABLE(distance_row)));
            adw_dialog_close(dialog);
            run_trace_delay(parent, toast_overlay, distance_cm);
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}
