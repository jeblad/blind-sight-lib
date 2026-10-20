// SPDX-License-Identifier: GPL-3.0-only
//
// preferences.cpp — see preferences.hpp for scope.

#include "preferences.hpp"

#include <glib/gi18n.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "audio.hpp"
#include "depth.hpp"
#include "monitoring.hpp"
#include "monitoring_dialogs.hpp"
#include "transcription.hpp"

using blind_sight::AudioDeviceInfo;
using blind_sight::kAmbientMicSourceId;
using blind_sight::kSpeakerSinkId;

// Bridges into main.cpp's anonymous namespace — these stay defined in
// main.cpp but are needed here too.
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body);
void bridge_show_setup_required_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                         const Glib::ustring& body,
                                         const Glib::ustring& command);
int bridge_get_viewing_distance_mm();
void bridge_set_viewing_distance_mm(int value);

namespace {

constexpr auto kAutoloadProjectKey = "autoload-project";
constexpr auto kAutoloadSessionKey = "autoload-session";
constexpr auto kAutoloadImageKey = "autoload-image";
constexpr auto kViewingDistanceKey = "viewing-distance";
constexpr auto kProdCueDurationMsKey = "prod-cue-duration-ms";
constexpr auto kProdCueDiameterScaleKey = "prod-cue-diameter-scale";
constexpr auto kProdCuePeakOpacityKey = "prod-cue-peak-opacity";
constexpr auto kHeadProdSequenceStepMsKey = "head-prod-sequence-step-ms";
constexpr auto kTraditionalScrollKey = "traditional-viewport-scroll";
constexpr auto kYawAugmentedInputKey = "yaw-augmented-input";
constexpr auto kTiltAugmentedInputKey = "tilt-augmented-input";
constexpr auto kMicrophoneInitiallyOnKey = "microphone-initially-on";
constexpr auto kSpeakerInitiallyOnKey = "speaker-initially-on";
constexpr auto kTranscodeAudioToOpusKey = "transcode-audio-to-opus";
constexpr auto kTranscribeAudioKey = "transcribe-audio";
constexpr auto kTranscriptionModelSizeKey = "transcription-model-size";
constexpr auto kDiarizeAudioKey = "diarize-audio";
constexpr auto kDepthGenerationEnabledKey = "depth-generation-enabled";
constexpr auto kDepthModelSizeKey = "depth-model-size";
constexpr auto kDepthUseFiducialMarkersKey = "depth-use-fiducial-markers";
constexpr auto kShowDepthAtPointerKey = "show-depth-at-pointer";
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
constexpr auto kPreferredBigramsKey = "preferred-bigrams";
constexpr auto kPreferredSinkDevicesKey = "preferred-sink-devices";
constexpr auto kPreferredSourceDevicesKey = "preferred-source-devices";
constexpr auto kSynthesizedVolumesKey = "synthesized-volumes";
constexpr auto kParticipantsVolumesKey = "participants-volumes";
constexpr auto kSourceVolumesKey = "source-volumes";

std::map<std::string, double> get_volumes(const Glib::RefPtr<Gio::Settings>& settings,
                                          const char* key) {
    Glib::VariantBase value;
    settings->get_value(key, value);
    auto typed =
        Glib::VariantBase::cast_dynamic<Glib::Variant<std::map<Glib::ustring, double>>>(value);
    std::map<std::string, double> volumes;
    for (const auto& [id, volume] : typed.get()) {
        volumes[id] = volume;
    }
    return volumes;
}

void set_volume(const Glib::RefPtr<Gio::Settings>& settings, const char* key,
                const std::string& id, double volume) {
    std::map<Glib::ustring, double> volumes;
    for (const auto& [existing_id, existing] : get_volumes(settings, key)) {
        volumes[existing_id] = existing;
    }
    volumes[id] = volume;
    settings->set_value(key, Glib::Variant<std::map<Glib::ustring, double>>::create(volumes));
}

}  // namespace

std::map<Glib::ustring, Glib::ustring> get_preferred_bigrams(
    const Glib::RefPtr<Gio::Settings>& settings) {
    Glib::VariantBase value;
    settings->get_value(kPreferredBigramsKey, value);
    auto typed = Glib::VariantBase::cast_dynamic<
        Glib::Variant<std::map<Glib::ustring, Glib::ustring>>>(value);
    return typed.get();
}

void set_preferred_bigrams(const Glib::RefPtr<Gio::Settings>& settings,
                            const std::map<Glib::ustring, Glib::ustring>& bigrams) {
    settings->set_value(kPreferredBigramsKey,
                         Glib::Variant<std::map<Glib::ustring, Glib::ustring>>::create(bigrams));
}

namespace {

// (device id, display name) encoded as one string, "id\x1Fname" — kept
// to the same proven a{ss}/std::map<Glib::ustring, Glib::ustring> shape
// get_preferred_bigrams() already uses, rather than a nested a{s(ss)}
// dict-of-tuples: that nesting turned out not to round-trip through
// glibmm's generic Variant<std::map<K, V>> template the way its API
// alone suggested — cast_dynamic() threw std::bad_cast reading it back,
// crashing the app (uncaught in the GTK signal handler that triggered
// the read). \x1F (ASCII unit separator) can't occur in a real device
// id (a BlueZ MAC or GStreamer node name) or display name.
constexpr char kDeviceFieldSeparator = '\x1F';

Glib::ustring encode_preferred_device(const Glib::ustring& device_id,
                                       const Glib::ustring& display_name) {
    return device_id + kDeviceFieldSeparator + display_name;
}

std::pair<Glib::ustring, Glib::ustring> decode_preferred_device(const Glib::ustring& encoded) {
    auto sep = encoded.find(kDeviceFieldSeparator);
    if (sep == Glib::ustring::npos) {
        return {encoded, Glib::ustring()};
    }
    return {encoded.substr(0, sep), encoded.substr(sep + 1)};
}

std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>> get_preferred_devices(
    const Glib::RefPtr<Gio::Settings>& settings, const char* key) {
    Glib::VariantBase value;
    settings->get_value(key, value);
    auto typed = Glib::VariantBase::cast_dynamic<
        Glib::Variant<std::map<Glib::ustring, Glib::ustring>>>(value);
    std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>> result;
    for (const auto& [id, encoded] : typed.get()) {
        result[id] = decode_preferred_device(encoded);
    }
    return result;
}

void set_preferred_devices(
    const Glib::RefPtr<Gio::Settings>& settings, const char* key,
    const std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>>& devices) {
    std::map<Glib::ustring, Glib::ustring> encoded;
    for (const auto& [id, device] : devices) {
        encoded[id] = encode_preferred_device(device.first, device.second);
    }
    settings->set_value(key,
                         Glib::Variant<std::map<Glib::ustring, Glib::ustring>>::create(encoded));
}

}  // namespace

std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>> get_preferred_sink_devices(
    const Glib::RefPtr<Gio::Settings>& settings) {
    return get_preferred_devices(settings, kPreferredSinkDevicesKey);
}

void set_preferred_sink_devices(
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>>& devices) {
    set_preferred_devices(settings, kPreferredSinkDevicesKey, devices);
}

std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>> get_preferred_source_devices(
    const Glib::RefPtr<Gio::Settings>& settings) {
    return get_preferred_devices(settings, kPreferredSourceDevicesKey);
}

void set_preferred_source_devices(
    const Glib::RefPtr<Gio::Settings>& settings,
    const std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>>& devices) {
    set_preferred_devices(settings, kPreferredSourceDevicesKey, devices);
}

std::map<std::string, double> get_synthesized_volumes(
    const Glib::RefPtr<Gio::Settings>& settings) {
    return get_volumes(settings, kSynthesizedVolumesKey);
}

std::map<std::string, double> get_participants_volumes(
    const Glib::RefPtr<Gio::Settings>& settings) {
    return get_volumes(settings, kParticipantsVolumesKey);
}

std::map<std::string, double> get_source_volumes(const Glib::RefPtr<Gio::Settings>& settings) {
    return get_volumes(settings, kSourceVolumesKey);
}

void set_synthesized_volume(const Glib::RefPtr<Gio::Settings>& settings, const std::string& id,
                            double volume) {
    set_volume(settings, kSynthesizedVolumesKey, id, volume);
}

void set_participants_volume(const Glib::RefPtr<Gio::Settings>& settings,
                             const std::string& id, double volume) {
    set_volume(settings, kParticipantsVolumesKey, id, volume);
}

void set_source_volume(const Glib::RefPtr<Gio::Settings>& settings, const std::string& id,
                       double volume) {
    set_volume(settings, kSourceVolumesKey, id, volume);
}

std::map<std::string, bool> get_source_agc(const Glib::RefPtr<Gio::Settings>& settings) {
    Glib::VariantBase value;
    settings->get_value("source-agc", value);
    auto typed =
        Glib::VariantBase::cast_dynamic<Glib::Variant<std::map<Glib::ustring, bool>>>(value);
    std::map<std::string, bool> enabled;
    for (const auto& [id, on] : typed.get()) {
        enabled[id] = on;
    }
    return enabled;
}

void set_source_agc(const Glib::RefPtr<Gio::Settings>& settings, const std::string& id,
                    bool enabled) {
    std::map<Glib::ustring, bool> all;
    for (const auto& [existing_id, on] : get_source_agc(settings)) {
        all[existing_id] = on;
    }
    all[id] = enabled;
    settings->set_value("source-agc", Glib::Variant<std::map<Glib::ustring, bool>>::create(all));
}

// First real content, the rest still lands later (render scale,
// depth-estimation override, max-measured-depth). AdwPreferencesDialog is
// the current idiom (AdwPreferencesWindow's replacement). GNOME
// preferences apply live, no Save button.
void show_preferences(Gtk::Window* parent, const Glib::RefPtr<Gio::Settings>& settings,
                       AdwToastOverlay* toast_overlay) {
    auto* prefs = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    auto* page = adw_preferences_page_new();

    // Autoload — three independent flags, each gating an otherwise-
    // automatic restore at launch: the last-opened project, the current
    // working session, and the last-viewed image. A facilitator
    // resetting the app between test participants needs this easy to
    // find, so the previous one's content never flashes up
    // automatically. All three default true.
    auto* autoload_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(autoload_group, _("Autoload"));
    adw_preferences_group_set_description(
        autoload_group,
        _("What reopens automatically at launch — turn these off to start "
          "with nothing from a previous session exposed"));

    auto* autoload_project_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(autoload_project_row),
                                   _("Project"));
    settings->bind(kAutoloadProjectKey,
                    Glib::wrap(GTK_WIDGET(autoload_project_row), false), "active");
    adw_preferences_group_add(autoload_group, GTK_WIDGET(autoload_project_row));

    auto* autoload_session_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(autoload_session_row),
                                   _("Session"));
    settings->bind(kAutoloadSessionKey,
                    Glib::wrap(GTK_WIDGET(autoload_session_row), false), "active");
    adw_preferences_group_add(autoload_group, GTK_WIDGET(autoload_session_row));

    auto* autoload_image_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(autoload_image_row),
                                   _("Image"));
    settings->bind(kAutoloadImageKey,
                    Glib::wrap(GTK_WIDGET(autoload_image_row), false), "active");
    adw_preferences_group_add(autoload_group, GTK_WIDGET(autoload_image_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), autoload_group);

    auto* gaze_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(gaze_group, _("Gaze Cursor"));

    auto* distance_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(20.0, 300.0, 1.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(distance_row),
                                   _("Viewing Distance (cm)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(distance_row),
        _("How far you sit from the screen — sizes the gaze cursor to "
          "match your actual field of view"));
    // ÷10/×10 here is a display-time cm shift, not a unit conversion on
    // the stored quantity — the viewing-distance value never leaves
    // whole millimetres. std::lround(), not a narrowing cast, so the
    // shift back on write is exact rounding, not truncation.
    adw_spin_row_set_value(distance_row,
                            static_cast<double>(bridge_get_viewing_distance_mm()) / 10.0);
    // Glib::wrap(ptr, true) — true, not the false/default used
    // elsewhere in this file, because adw_spin_row_get_adjustment() is
    // a plain getter returning a *borrowed* GtkAdjustment* the row
    // still owns. Wrapping it with false would make the temporary
    // RefPtr's destructor unref a reference it never actually took,
    // silently over-releasing it — the symptom is GTK_IS_ADJUSTMENT
    // assertion failures a few seconds after opening Preferences, and
    // this row going dead/reading back 0.
    Glib::wrap(adw_spin_row_get_adjustment(distance_row), true)
        ->signal_value_changed()
        .connect([distance_row, settings]() {
            int mm = static_cast<int>(
                std::lround(adw_spin_row_get_value(distance_row) * 10.0));
            bridge_set_viewing_distance_mm(mm);
            // The only place mm crosses back into GSettings' plain-SI
            // metres.
            settings->set_double(kViewingDistanceKey, mm / 1000.0);
        });
    adw_preferences_group_add(gaze_group, GTK_WIDGET(distance_row));
    auto* settle_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(50.0, 2000.0, 50.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(settle_row), _("Pointer Fixation Time (ms)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(settle_row),
        _("How long the mouse pointer must rest before it prods where it stopped"));
    adw_spin_row_set_value(settle_row, settings->get_int("pointer-fixation-ms"));
    Glib::wrap(adw_spin_row_get_adjustment(settle_row), true)
        ->signal_value_changed()
        .connect([settle_row, settings]() {
            settings->set_int("pointer-fixation-ms",
                              static_cast<int>(std::lround(adw_spin_row_get_value(settle_row))));
        });
    adw_preferences_group_add(gaze_group, GTK_WIDGET(settle_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), gaze_group);

    // Prod-event glow — tunable rather than fixed in code: "same
    // perceived intensity" is something to calibrate against real
    // testers, not a number to guess once and hardcode. Peak opacity is
    // stored 0.0–1.0, shown as a 0–100 percentage.
    auto* prod_cue_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(prod_cue_group, _("Prod Cue"));

    auto* duration_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(0.0, 5000.0, 50.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(duration_row),
                                   _("Duration (ms)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(duration_row),
        _("How long the glow shown at a prod (click) takes to fade out"));
    adw_spin_row_set_value(duration_row, settings->get_int(kProdCueDurationMsKey));
    Glib::wrap(adw_spin_row_get_adjustment(duration_row), true)
        ->signal_value_changed()
        .connect([duration_row, settings]() {
            settings->set_int(kProdCueDurationMsKey,
                               static_cast<int>(adw_spin_row_get_value(duration_row)));
        });
    adw_preferences_group_add(prod_cue_group, GTK_WIDGET(duration_row));

    auto* cue_size_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(1.0, 8.0, 0.1));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cue_size_row),
                                   _("Size (× gaze cursor)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(cue_size_row),
        _("Glow diameter, as a multiple of the gaze cursor's own diameter"));
    adw_spin_row_set_value(cue_size_row,
                            settings->get_double(kProdCueDiameterScaleKey));
    Glib::wrap(adw_spin_row_get_adjustment(cue_size_row), true)
        ->signal_value_changed()
        .connect([cue_size_row, settings]() {
            settings->set_double(kProdCueDiameterScaleKey,
                                  adw_spin_row_get_value(cue_size_row));
        });
    adw_preferences_group_add(prod_cue_group, GTK_WIDGET(cue_size_row));

    auto* cue_opacity_row = ADW_SPIN_ROW(adw_spin_row_new_with_range(0.0, 100.0, 5.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(cue_opacity_row),
                                   _("Peak Opacity (%)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(cue_opacity_row),
        _("How strong the glow's center is the moment it appears — it "
          "always fades to fully transparent at its own edge regardless"));
    adw_spin_row_set_value(
        cue_opacity_row, settings->get_double(kProdCuePeakOpacityKey) * 100.0);
    Glib::wrap(adw_spin_row_get_adjustment(cue_opacity_row), true)
        ->signal_value_changed()
        .connect([cue_opacity_row, settings]() {
            settings->set_double(kProdCuePeakOpacityKey,
                                  adw_spin_row_get_value(cue_opacity_row) / 100.0);
        });
    adw_preferences_group_add(prod_cue_group, GTK_WIDGET(cue_opacity_row));

    auto* head_prod_step_row =
        ADW_SPIN_ROW(adw_spin_row_new_with_range(0.0, 1000.0, 10.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(head_prod_step_row),
                                   _("Head-Prod Sweep Step (ms)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(head_prod_step_row),
        _("Delay between each 5° step of a head-prod's elevation sweep "
          "(-25° to +15°) — 0 fires the whole sequence at once"));
    adw_spin_row_set_value(head_prod_step_row,
                            settings->get_int(kHeadProdSequenceStepMsKey));
    Glib::wrap(adw_spin_row_get_adjustment(head_prod_step_row), true)
        ->signal_value_changed()
        .connect([head_prod_step_row, settings]() {
            settings->set_int(
                kHeadProdSequenceStepMsKey,
                static_cast<int>(adw_spin_row_get_value(head_prod_step_row)));
        });
    adw_preferences_group_add(prod_cue_group, GTK_WIDGET(head_prod_step_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), prod_cue_group);

    // Two named radio rows, not a single on/off switch — a bare switch
    // can't show *which* state means what without also reading a
    // subtitle describing whichever state you're NOT currently in; two
    // clearly-labelled rows avoid that ambiguity.
    auto* viewport_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(viewport_group, _("Viewport Navigation"));

    auto* traditional_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(traditional_row),
                                   _("Traditional"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(traditional_row),
        _("Arrow keys move the viewport — new content enters from the "
          "direction pressed"));

    auto* natural_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(natural_row), _("Natural"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(natural_row),
        _("Arrow keys move the content — as if dragging the image in the "
          "direction pressed"));

    // Gtk::CheckButton in radio mode (GTK4 dropped RadioButton —
    // set_group() is how two checkbuttons become mutually exclusive
    // now), set as each row's activatable widget so clicking anywhere
    // on the row toggles it, not just the radio dot.
    auto* traditional_radio = Gtk::make_managed<Gtk::CheckButton>();
    auto* natural_radio = Gtk::make_managed<Gtk::CheckButton>();
    natural_radio->set_group(*traditional_radio);
    traditional_radio->set_valign(Gtk::Align::CENTER);
    natural_radio->set_valign(Gtk::Align::CENTER);
    traditional_radio->set_active(settings->get_boolean(kTraditionalScrollKey));
    natural_radio->set_active(!settings->get_boolean(kTraditionalScrollKey));

    adw_action_row_add_prefix(ADW_ACTION_ROW(traditional_row),
                               GTK_WIDGET(traditional_radio->gobj()));
    adw_action_row_add_prefix(ADW_ACTION_ROW(natural_row),
                               GTK_WIDGET(natural_radio->gobj()));
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(traditional_row),
                                           GTK_WIDGET(traditional_radio->gobj()));
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(natural_row),
                                           GTK_WIDGET(natural_radio->gobj()));

    // Only the newly-activated radio in a mutually exclusive pair fires
    // a meaningful "toggled" — the one being deactivated also fires, so
    // each handler only acts when its own button is the one now active.
    traditional_radio->signal_toggled().connect([traditional_radio, settings]() {
        if (traditional_radio->get_active()) {
            settings->set_boolean(kTraditionalScrollKey, true);
        }
    });
    natural_radio->signal_toggled().connect([natural_radio, settings]() {
        if (natural_radio->get_active()) {
            settings->set_boolean(kTraditionalScrollKey, false);
        }
    });

    adw_preferences_group_add(viewport_group, GTK_WIDGET(traditional_row));
    adw_preferences_group_add(viewport_group, GTK_WIDGET(natural_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), viewport_group);

    // Optional augmented input per axis — arrow keys always work
    // regardless; these two rows only pick an *additional* device on
    // top of them. Deliberately short option lists — only input
    // methods actually validated against real hardware are offered.
    auto* panning_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(panning_group, _("Panning Input"));

    // AdwComboRow has no gtkmm binding, so its "selected" change is
    // observed via the raw GObject "notify::selected" signal rather
    // than a typed sigc::signal — Glib::wrap()'s generic fallback only
    // reaches Gtk::Widget, which has no "selected" property. A small
    // heap-allocated context carries what the plain C callback needs,
    // freed via the signal connection's destroy-notify.
    struct AugmentedInputRowContext {
        Glib::RefPtr<Gio::Settings> settings;
        const char* key;
        std::vector<std::string> option_keys;
    };
    auto build_augmented_input_row =
        [settings](const Glib::ustring& title, const Glib::ustring& subtitle,
                   const char* key,
                   const std::vector<std::pair<Glib::ustring, std::string>>&
                       options) {
            auto* row = ADW_COMBO_ROW(adw_combo_row_new());
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                           title.c_str());
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());

            std::vector<const char*> labels;
            std::vector<std::string> option_keys;
            for (const auto& option : options) {
                labels.push_back(option.first.c_str());
                option_keys.push_back(option.second);
            }
            labels.push_back(nullptr);
            auto* model = gtk_string_list_new(labels.data());
            adw_combo_row_set_model(row, G_LIST_MODEL(model));
            // Without this, each row in the dropdown shows blank —
            // AdwComboRow needs to be told how to turn a GtkStringObject
            // item into display text; it doesn't infer that from a
            // plain GtkStringList automatically.
            auto* expression = gtk_property_expression_new(
                GTK_TYPE_STRING_OBJECT, nullptr, "string");
            adw_combo_row_set_expression(row, expression);
            gtk_expression_unref(expression);

            std::string current = settings->get_string(key).raw();
            auto it = std::find(option_keys.begin(), option_keys.end(), current);
            guint index = it != option_keys.end()
                              ? static_cast<guint>(it - option_keys.begin())
                              : 0;
            adw_combo_row_set_selected(row, index);

            auto* ctx = new AugmentedInputRowContext{settings, key, option_keys};
            g_signal_connect_data(
                row, "notify::selected",
                G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
                    auto* ctx = static_cast<AugmentedInputRowContext*>(data);
                    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(obj));
                    if (selected < ctx->option_keys.size()) {
                        ctx->settings->set_string(ctx->key,
                                                   ctx->option_keys[selected]);
                    }
                }),
                ctx,
                +[](gpointer data, GClosure*) {
                    delete static_cast<AugmentedInputRowContext*>(data);
                },
                GConnectFlags(0));
            return row;
        };

    auto* yaw_row = build_augmented_input_row(
        _("Horizontal Panning (Yaw)"),
        _("Extra input for turning left/right, alongside arrow keys — "
          "only options tested on real hardware are listed"),
        kYawAugmentedInputKey,
        {{_("None"), "none"},
         {_("Mouse Scroll Wheel"), "scroll-wheel"},
         {_("Surface Dial"), "surface-dial"}});
    adw_preferences_group_add(panning_group, GTK_WIDGET(yaw_row));

    auto* tilt_row = build_augmented_input_row(
        _("Vertical Panning (Tilt)"),
        _("Extra input for turning up/down, alongside arrow keys — only "
          "options tested on real hardware are listed"),
        kTiltAugmentedInputKey,
        {{_("None"), "none"}, {_("Mouse Scroll Wheel"), "scroll-wheel"}});
    adw_preferences_group_add(panning_group, GTK_WIDGET(tilt_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), panning_group);

    // A plain on/off state, not named alternatives like
    // Traditional/Natural above — no ambiguity about what "on"/"off"
    // mean for a microphone.
    auto* mic_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(mic_group, _("Microphone"));

    auto* mic_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(mic_row),
                                   _("Microphone Initially On"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(mic_row),
        _("Whether Record's microphone starts on when opened — it can "
          "always be toggled during a session"));
    settings->bind(kMicrophoneInitiallyOnKey,
                    Glib::wrap(GTK_WIDGET(mic_row), false), "active");
    adw_preferences_group_add(mic_group, GTK_WIDGET(mic_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), mic_group);

    // Shared by every participant's microphone; each one is only switched
    // on or off on its own (the microphone's own device dialog).
    auto* voice_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(voice_group, _("Voice Levelling"));
    adw_preferences_group_set_description(
        voice_group, _("A slow automatic gain control on every microphone, before its volume"));
    // `integer`: the key holds an int (the millisecond times), not a double.
    auto add_voice_row = [voice_group, settings](const char* key, const char* title,
                                                 const char* subtitle, double min, double max,
                                                 double step, bool integer) {
        auto* row = ADW_SPIN_ROW(adw_spin_row_new_with_range(min, max, step));
        adw_spin_row_set_digits(row, 0);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        adw_spin_row_set_value(row, integer ? settings->get_int(key) : settings->get_double(key));
        Glib::wrap(adw_spin_row_get_adjustment(row), true)
            ->signal_value_changed()
            .connect([row, settings, key, integer]() {
                double value = adw_spin_row_get_value(row);
                if (integer) {
                    settings->set_int(key, static_cast<int>(std::lround(value)));
                } else {
                    settings->set_double(key, value);
                }
            });
        adw_preferences_group_add(voice_group, GTK_WIDGET(row));
    };
    add_voice_row("agc-target-dbfs", _("Target Level (dBFS)"),
                  _("Speech level aimed for; −23 matches the EBU R 128 loudness target"),
                  -40.0, -6.0, 1.0, false);
    add_voice_row("agc-attack-ms", _("Attack (ms)"),
                  _("How quickly a loud speaker is turned down"), 50.0, 30000.0, 50.0, true);
    add_voice_row("agc-release-ms", _("Release (ms)"),
                  _("How quickly a quiet speaker is turned up"), 50.0, 60000.0, 50.0, true);
    add_voice_row("agc-max-gain-db", _("Maximum Gain (dB)"),
                  _("The most a speaker is raised or lowered"), 0.0, 40.0, 1.0, false);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), voice_group);

    // Playback's counterpart to the Microphone group above.
    auto* speaker_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(speaker_group, _("Speaker"));

    auto* speaker_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(speaker_row),
                                   _("Speaker Initially On"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(speaker_row),
        _("Whether Playback's speaker starts on when opened — it can "
          "always be toggled during a session"));
    settings->bind(kSpeakerInitiallyOnKey,
                    Glib::wrap(GTK_WIDGET(speaker_row), false), "active");
    adw_preferences_group_add(speaker_group, GTK_WIDGET(speaker_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), speaker_group);

    // "Audio Processing," covering both encoding and transcription
    // below. Subtitle deliberately never says "delete" — the original
    // going away is an implied consequence of transcoding, not
    // something separately announced.
    auto* encoding_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(encoding_group, _("Audio Processing"));

    auto* transcode_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(transcode_row),
                                   _("Transcode Audio to Opus"));
    // Computed from the same opus_encoding_available() check the
    // active-state revalidation below needs, so it's read once and
    // shared rather than called twice.
    bool opus_libraries_ok = opus_encoding_available();
    Glib::ustring transcode_subtitle =
        _("Recordings are transcoded to Opus automatically once Stop is "
          "pressed — turn off to leave recordings as WAV instead");
    if (!opus_libraries_ok) {
        transcode_subtitle += _(" (Libraries are missing)");
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(transcode_row),
                                 transcode_subtitle.c_str());
    // Not a plain settings->bind() — turning it on should check
    // opusenc/oggmux are actually installed rather than silently
    // leaving a setting on that can't do anything. The row's initial
    // state is still read from GSettings, but every change goes
    // through this handler instead. Reverting the row to inactive on a
    // failed check re-fires this same handler with active=false, which
    // just writes false — no special-casing needed.
    //
    // Re-validated against current reality here too (same staleness
    // bug as the transcribe row below): a stored `true` doesn't mean
    // opusenc/oggmux are still there if they were removed since.
    bool transcode_active = settings->get_boolean(kTranscodeAudioToOpusKey) && opus_libraries_ok;
    if (transcode_active != settings->get_boolean(kTranscodeAudioToOpusKey)) {
        settings->set_boolean(kTranscodeAudioToOpusKey, transcode_active);
    }
    adw_switch_row_set_active(transcode_row, transcode_active);
    struct TranscodeRowContext {
        Glib::RefPtr<Gio::Settings> settings;
        Gtk::Window* parent;
    };
    auto* transcode_ctx = new TranscodeRowContext{settings, parent};
    g_signal_connect_data(
        transcode_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<TranscodeRowContext*>(data);
            auto* row = ADW_SWITCH_ROW(obj);
            bool active = adw_switch_row_get_active(row);
            if (active && !opus_encoding_available()) {
                // Same "Ok" + implicit-recheck shape used throughout
                // this file's setup dialogs — the user may install the
                // package while this dialog is open, so the check
                // happens when "Ok" is clicked, not before.
                //
                // `row` is held via a GWeakRef, not a raw pointer — this
                // alert is presented against the main window, not
                // nested inside Preferences, so the user can close
                // Preferences (destroying `row`) while this stays open
                // and they go run `apt install` in a terminal.
                // g_weak_ref_get() (safely NULL if Preferences has since
                // closed) is what lets the switch reflect a successful
                // recheck without a full app restart.
                auto* dialog = adw_alert_dialog_new(
                    _("Opus Encoding Not Available"),
                    _("This GStreamer installation doesn't include Opus "
                      "support (opusenc/oggmux). On Debian/Ubuntu-based "
                      "distributions, install it with the command below, "
                      "then press Ok. Restarting the app afterwards "
                      "shouldn't be necessary, but try it if the setting "
                      "still won't turn on."));
                auto* command_label = Gtk::make_managed<Gtk::Label>(
                    "sudo apt install gstreamer1.0-plugins-base");
                command_label->set_selectable(true);
                command_label->set_halign(Gtk::Align::START);
                command_label->set_wrap(true);
                command_label->add_css_class("monospace");
                adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog),
                                                  GTK_WIDGET(command_label->gobj()));
                adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", _("Ok"));
                adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "ok",
                                                          ADW_RESPONSE_SUGGESTED);
                adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
                adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "ok");

                struct OpusRecheckContext {
                    Glib::RefPtr<Gio::Settings> settings;
                    GWeakRef row_ref;
                };
                // Field-by-field, not a brace-init with a comma — this
                // block sits inside the outer G_CALLBACK(...), and the
                // preprocessor's argument-splitting for that macro only
                // tracks () nesting, not {}.
                auto* recheck_ctx = new OpusRecheckContext();
                recheck_ctx->settings = ctx->settings;
                g_weak_ref_init(&recheck_ctx->row_ref, row);
                g_signal_connect_data(
                    dialog, "response",
                    G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
                        auto* recheck_ctx = static_cast<OpusRecheckContext*>(data);
                        bool ready = opus_encoding_available();
                        recheck_ctx->settings->set_boolean(kTranscodeAudioToOpusKey, ready);
                        if (auto* obj = static_cast<GObject*>(
                                g_weak_ref_get(&recheck_ctx->row_ref))) {
                            adw_switch_row_set_active(ADW_SWITCH_ROW(obj), ready);
                            g_object_unref(obj);
                        }
                    }),
                    recheck_ctx,
                    +[](gpointer data, GClosure*) {
                        auto* recheck_ctx = static_cast<OpusRecheckContext*>(data);
                        g_weak_ref_clear(&recheck_ctx->row_ref);
                        delete recheck_ctx;
                    },
                    GConnectFlags(0));
                adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ctx->parent->gobj()));
                // Revert visually right away — immediate feedback
                // while the user installs the package; the response
                // handler above flips it back on if the recheck
                // succeeds.
                adw_switch_row_set_active(row, FALSE);
                return;
            }
            ctx->settings->set_boolean(kTranscodeAudioToOpusKey, active);
        }),
        transcode_ctx,
        +[](gpointer data, GClosure*) {
            delete static_cast<TranscodeRowContext*>(data);
        },
        GConnectFlags(0));
    adw_preferences_group_add(encoding_group, GTK_WIDGET(transcode_row));

    // Whether maybe_transcribe_segment() runs at all, not what audio
    // format survives (the Opus toggle above).
    //
    // Two-level check, both hard requirements: off until both libraries
    // and the fixed transcription model (whisper_model_cached()) are in
    // place. "Model" here only means that fixed model — the language-
    // specific alignment model can't be checked in advance, since
    // WhisperX only picks it once it detects the language during an
    // actual transcription, and always downloads lazily on first use.
    //
    // Subtitle also names whichever prerequisite is currently missing —
    // computed once when the row is built; doesn't live-update while
    // Preferences stays open.
    auto* transcribe_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(transcribe_row),
                                   _("Transcribe Recordings"));
    bool transcribe_libraries_ok = transcription_available();
    bool transcribe_ready =
        transcribe_libraries_ok &&
        whisper_model_cached(settings->get_string(kTranscriptionModelSizeKey).raw());
    Glib::ustring transcribe_subtitle =
        _("Recorded speech is transcribed automatically once Stop is "
          "pressed — turn off to skip transcription entirely");
    if (!transcribe_libraries_ok) {
        transcribe_subtitle += _(" (Libraries are missing)");
    } else if (!transcribe_ready) {
        transcribe_subtitle += _(" (Model is missing)");
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(transcribe_row),
                                 transcribe_subtitle.c_str());
    // The stored setting can be stale relative to what's actually on
    // disk — the model may have been deleted since this was last
    // turned on. Corrected here, not just visually: the setting itself
    // is written back to match, so maybe_transcribe_segment() (which
    // reads kTranscribeAudioKey directly, not through this row) doesn't
    // keep acting on a stale "on" value.
    bool transcribe_active = settings->get_boolean(kTranscribeAudioKey) && transcribe_ready;
    if (transcribe_active != settings->get_boolean(kTranscribeAudioKey)) {
        settings->set_boolean(kTranscribeAudioKey, transcribe_active);
    }
    adw_switch_row_set_active(transcribe_row, transcribe_active);
    struct TranscribeRowContext {
        Glib::RefPtr<Gio::Settings> settings;
        Gtk::Window* parent;
        AdwToastOverlay* toast_overlay;
    };
    auto* transcribe_ctx = new TranscribeRowContext{settings, parent, toast_overlay};
    g_signal_connect_data(
        transcribe_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<TranscribeRowContext*>(data);
            auto* row = ADW_SWITCH_ROW(obj);
            bool active = adw_switch_row_get_active(row);
            if (active && !transcription_available()) {
                // Reverted immediately — show_transcription_setup_
                // offer()'s "Next" flow is what sets kTranscribeAudioKey
                // back to true later, once an install it kicks off
                // finishes.
                adw_switch_row_set_active(row, FALSE);
                show_transcription_setup_offer(ctx->parent, ctx->settings,
                                                ctx->toast_overlay);
                return;
            }
            if (active && !whisper_model_cached(
                              ctx->settings->get_string(kTranscriptionModelSizeKey).raw())) {
                // Same deferred-set shape as the libraries branch above.
                adw_switch_row_set_active(row, FALSE);
                show_transcription_model_setup_offer(ctx->parent, ctx->settings, row);
                return;
            }
            ctx->settings->set_boolean(kTranscribeAudioKey, active);
        }),
        transcribe_ctx,
        +[](gpointer data, GClosure*) {
            delete static_cast<TranscribeRowContext*>(data);
        },
        GConnectFlags(0));
    adw_preferences_group_add(encoding_group, GTK_WIDGET(transcribe_row));

    // "Transcription Model Size", not just "Model Size" — depth
    // generation has its own model and could plausibly get its own size
    // setting later, so a bare "Model Size" row here would be ambiguous.
    // No automatic "pick the largest that fits" (faster_whisper's
    // WhisperModel constructor has no such negotiation — it just loads
    // and throws CUDA OOM if it doesn't fit) — a plain manual choice.
    // Reuses build_augmented_input_row() — same "plain string enum in
    // GSettings" shape as yaw-/tilt-augmented-input.
    auto* model_size_row = build_augmented_input_row(
        _("Transcription Model Size"),
        _("Bigger models transcribe more accurately, especially outside "
          "English, but need more GPU memory and are slower"),
        kTranscriptionModelSizeKey,
        {{_("Small"), "small"}, {_("Medium"), "medium"}, {_("Large-v3"), "large-v3"}});
    // Hidden, not just insensitive, while Transcribe Recordings is off
    // — live-tracked further below, once diarize_row/language_group/
    // all_languages_group also exist, by one shared listener.
    gtk_widget_set_visible(GTK_WIDGET(model_size_row), transcribe_active);
    adw_preferences_group_add(encoding_group, GTK_WIDGET(model_size_row));

    // "Diarize Recordings" — labels speakers within an already-produced
    // transcript, so it only means anything when Transcribe Recordings
    // above is itself on. Turning it on triggers the same
    // setup-poster-if-missing shape as Opus/transcribe.
    auto* diarize_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(diarize_row),
                                   _("Diarize Recordings"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(diarize_row),
        _("Separates the speakers in the transcript (speaker 1, speaker "
          "2, …) without identifying who they are — needs a one-time "
          "HuggingFace setup the first time this is turned on"));
    gtk_widget_set_visible(GTK_WIDGET(diarize_row), transcribe_active);
    // Same staleness correction as transcribe_row above — a token
    // present when this was last turned on may have since been deleted
    // from the keyring, or the model prefetch may never have completed.
    bool diarize_active = settings->get_boolean(kDiarizeAudioKey) &&
                           transcribe_active && huggingface_token_available() &&
                           diarization_model_cached();
    if (diarize_active != settings->get_boolean(kDiarizeAudioKey)) {
        settings->set_boolean(kDiarizeAudioKey, diarize_active);
    }
    adw_switch_row_set_active(diarize_row, diarize_active);
    struct DiarizeRowContext {
        Glib::RefPtr<Gio::Settings> settings;
        Gtk::Window* parent;
        AdwToastOverlay* toast_overlay;
    };
    auto* diarize_ctx = new DiarizeRowContext{settings, parent, toast_overlay};
    g_signal_connect_data(
        diarize_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<DiarizeRowContext*>(data);
            auto* row = ADW_SWITCH_ROW(obj);
            bool active = adw_switch_row_get_active(row);
            // `!diarization_model_cached()` guard is load-bearing, not
            // an optimization — same shape as the Opus/transcribe rows'
            // `!available()` checks: without it, a *successful*
            // prefetch's own adw_switch_row_set_active(row, TRUE)
            // re-emits this row's "notify::active", re-entering this
            // handler as if the user had clicked it again — a real
            // infinite loop. Once cached, re-entering just falls
            // through to the plain settings write below.
            if (active && !diarization_model_cached()) {
                // "Ok" + implicit-recheck shape; `row` passed through
                // (held safely via GWeakRef inside, same as the Opus
                // dialog) so the switch reflects the outcome
                // immediately if Preferences is still open when it
                // resolves.
                adw_switch_row_set_active(row, FALSE);
                auto token = lookup_huggingface_token();
                if (!token) {
                    show_diarization_setup_offer(ctx->parent, ctx->settings,
                                                  ctx->toast_overlay, row);
                    return;
                }
                start_diarization_model_prefetch(ctx->parent, ctx->settings,
                                                  ctx->toast_overlay, row, *token);
                return;
            }
            ctx->settings->set_boolean(kDiarizeAudioKey, active);
        }),
        diarize_ctx,
        +[](gpointer data, GClosure*) {
            delete static_cast<DiarizeRowContext*>(data);
        },
        GConnectFlags(0));
    adw_preferences_group_add(encoding_group, GTK_WIDGET(diarize_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), encoding_group);

    // Transcription language pre-caching — lets the user pre-download
    // alignment models for languages they expect to use, ahead of
    // actually recording, same "avoid an interruption during testing"
    // reasoning as diarization's prefetch. Whisper keeps auto-detecting
    // the spoken language per recording either way and downloads
    // whichever alignment model it needs on first real use regardless;
    // pre-installing just avoids that download landing mid-test.
    // Rebuilt from disk every time Preferences opens, not from a stored
    // setting — the only way this list can be correct, since there's no
    // other record of what's actually been downloaded.
    auto* language_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(language_group, _("Transcription Languages"));
    adw_preferences_group_set_description(
        language_group,
        _("Avoids a download interrupting a test — Whisper works without "
          "these too."));

    // A separate, title-less group for just this one button —
    // unconditional and always visible, so a user browsing the
    // filtered picker always has an obvious way to reach the full,
    // unfiltered list too. Kept in its own group, not appended into
    // language_group, so it always renders below every language row
    // regardless of how many get added later (AdwPreferencesGroup only
    // ever appends, it can't insert before an existing child).
    auto* all_languages_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    auto* all_languages_button = Gtk::make_managed<Gtk::Button>(_("All Languages"));
    all_languages_button->add_css_class("flat");
    adw_preferences_group_add(all_languages_group, GTK_WIDGET(all_languages_button->gobj()));

    auto lang_state = std::make_shared<LanguagePreferencesState>();
    g_weak_ref_init(&lang_state->language_group_ref, language_group);

    all_languages_button->signal_clicked().connect(
        [parent, toast_overlay, lang_state]() {
            show_add_language_dialog(parent, toast_overlay, lang_state, "");
        });

    for (const auto& info : kAlignableLanguages) {
        if (language_alignment_model_cached(info)) {
            auto* row = adw_action_row_new();
            std::string name = whisper_language_name(info.code);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name.c_str());
            make_language_row_activatable(row, parent, toast_overlay, lang_state, name);
            adw_preferences_group_add(language_group, row);
            lang_state->rows[info.code] = row;
        }
    }

    // Hidden, not just insensitive, while Transcribe Recordings is off
    // — same reasoning as model_size_row/diarize_row above. Both
    // groups, not just language_group — the "All Languages" fallback
    // makes no more sense than the list itself when nothing reads it.
    gtk_widget_set_visible(GTK_WIDGET(language_group), transcribe_active);
    gtk_widget_set_visible(GTK_WIDGET(all_languages_group), transcribe_active);
    // One shared listener for all four transcribe-dependent rows/groups
    // rather than one per widget, since they're all driven by the same
    // condition.
    struct TranscribeDependentVisibility {
        GtkWidget* model_size_row;
        GtkWidget* diarize_row;
        GtkWidget* language_group;
        GtkWidget* all_languages_group;
    };
    auto* visibility_ctx =
        new TranscribeDependentVisibility{GTK_WIDGET(model_size_row), GTK_WIDGET(diarize_row),
                                           GTK_WIDGET(language_group),
                                           GTK_WIDGET(all_languages_group)};
    g_signal_connect_data(
        transcribe_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<TranscribeDependentVisibility*>(data);
            gboolean active = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
            gtk_widget_set_visible(ctx->model_size_row, active);
            gtk_widget_set_visible(ctx->diarize_row, active);
            gtk_widget_set_visible(ctx->language_group, active);
            gtk_widget_set_visible(ctx->all_languages_group, active);
        }),
        visibility_ctx,
        +[](gpointer data, GClosure*) {
            delete static_cast<TranscribeDependentVisibility*>(data);
        },
        GConnectFlags(0));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), language_group);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), all_languages_group);

    // "Depth Generation" — same shape as Transcribe/Diarize above: a
    // toggle that prefetches the model immediately when turned on,
    // rather than waiting for the first image that needs one.
    auto* depth_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(depth_group, _("Depth Generation"));

    auto* depth_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(depth_row),
                                   _("Generate Depth Maps"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(depth_row),
        _("Images without a depth map get one generated automatically "
          "when opened — turning this on downloads the model right "
          "away, not on first use"));
    // Same staleness correction as transcribe_row/diarize_row above.
    bool depth_active = settings->get_boolean(kDepthGenerationEnabledKey) &&
                         depth_generation_available() &&
                         depth_model_cached(settings->get_string(kDepthModelSizeKey).raw());
    if (depth_active != settings->get_boolean(kDepthGenerationEnabledKey)) {
        settings->set_boolean(kDepthGenerationEnabledKey, depth_active);
    }
    adw_switch_row_set_active(depth_row, depth_active);
    struct DepthRowContext {
        Glib::RefPtr<Gio::Settings> settings;
        Gtk::Window* parent;
        AdwToastOverlay* toast_overlay;
    };
    auto* depth_ctx = new DepthRowContext{settings, parent, toast_overlay};
    g_signal_connect_data(
        depth_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<DepthRowContext*>(data);
            auto* row = ADW_SWITCH_ROW(obj);
            bool active = adw_switch_row_get_active(row);
            if (active && !depth_generation_available()) {
                adw_switch_row_set_active(row, FALSE);
                bridge_show_setup_required_dialog(
                    ctx->parent, _("Depth Generation Not Set Up"),
                    _("Run this from a terminal, then turn this back on."),
                    "demo/depth/setup-depth-env.sh");
                return;
            }
            // `!depth_model_cached()` guard is load-bearing, not an
            // optimization — same infinite-loop reasoning as
            // diarize_row's guard above.
            std::string model_size =
                ctx->settings->get_string(kDepthModelSizeKey).raw();
            if (active && !depth_model_cached(model_size)) {
                adw_switch_row_set_active(row, FALSE);
                start_depth_model_prefetch(ctx->parent, ctx->settings,
                                                    ctx->toast_overlay, row, model_size);
                return;
            }
            ctx->settings->set_boolean(kDepthGenerationEnabledKey, active);
        }),
        depth_ctx,
        +[](gpointer data, GClosure*) { delete static_cast<DepthRowContext*>(data); },
        GConnectFlags(0));
    adw_preferences_group_add(depth_group, GTK_WIDGET(depth_row));

    auto* depth_model_size_row = build_augmented_input_row(
        _("Depth Model Size"),
        _("Bigger models are more accurate but slower and need more "
          "memory — depth generation often runs on CPU, so this matters "
          "even without a GPU"),
        kDepthModelSizeKey,
        {{_("Small"), "small"}, {_("Base"), "base"}, {_("Large"), "large"}});
    adw_preferences_group_add(depth_group, GTK_WIDGET(depth_model_size_row));

    // "Use Fiducial Markers" — ArUco markers for distance/3D-position
    // reference points. No prefetch/model-download flow like the other
    // switches above — ArUco detection needs no downloaded weights,
    // just an installed OpenCV. Same shape as "Transcode Audio to
    // Opus" instead: the library check is silent/passive here (only
    // correcting a stale stored `true` back to `false` if OpenCV
    // vanished since), and only turns into a visible dialog when the
    // user actually tries to turn this on. The actual marker-detection/
    // pose-estimation implementation doesn't exist yet — this only
    // carries the on/off intent so far.
    auto* fiducial_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(fiducial_row),
                                   _("Use Fiducial Markers"));
    bool aruco_libraries_ok = aruco_detection_available();
    Glib::ustring fiducial_subtitle =
        _("Anchors generated depth to real-world distance using ArUco "
          "reference markers placed in the scene");
    if (!aruco_libraries_ok) {
        fiducial_subtitle += _(" (Libraries are missing)");
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(fiducial_row), fiducial_subtitle.c_str());
    bool fiducial_active =
        settings->get_boolean(kDepthUseFiducialMarkersKey) && aruco_libraries_ok;
    if (fiducial_active != settings->get_boolean(kDepthUseFiducialMarkersKey)) {
        settings->set_boolean(kDepthUseFiducialMarkersKey, fiducial_active);
    }
    adw_switch_row_set_active(fiducial_row, fiducial_active);
    struct FiducialRowContext {
        Glib::RefPtr<Gio::Settings> settings;
        Gtk::Window* parent;
    };
    auto* fiducial_ctx = new FiducialRowContext{settings, parent};
    g_signal_connect_data(
        fiducial_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<FiducialRowContext*>(data);
            auto* row = ADW_SWITCH_ROW(obj);
            bool active = adw_switch_row_get_active(row);
            if (active && !aruco_detection_available()) {
                // Same "Ok" + implicit-recheck shape as transcode_row's
                // own Opus dialog above.
                auto* dialog = adw_alert_dialog_new(
                    _("ArUco Detection Not Available"),
                    _("The depth environment doesn't have OpenCV "
                      "installed yet. Install it with the command below, "
                      "then press Ok."));
                auto* command_label = Gtk::make_managed<Gtk::Label>(
                    depth_venv_dir() + "/bin/pip install opencv-python-headless");
                command_label->set_selectable(true);
                command_label->set_halign(Gtk::Align::START);
                command_label->set_wrap(true);
                command_label->add_css_class("monospace");
                adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog),
                                                  GTK_WIDGET(command_label->gobj()));
                adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", _("Ok"));
                adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "ok",
                                                          ADW_RESPONSE_SUGGESTED);
                adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
                adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "ok");

                struct FiducialRecheckContext {
                    Glib::RefPtr<Gio::Settings> settings;
                    GWeakRef row_ref;
                };
                auto* recheck_ctx = new FiducialRecheckContext();
                recheck_ctx->settings = ctx->settings;
                g_weak_ref_init(&recheck_ctx->row_ref, row);
                g_signal_connect_data(
                    dialog, "response",
                    G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
                        auto* recheck_ctx = static_cast<FiducialRecheckContext*>(data);
                        bool ready = aruco_detection_available();
                        recheck_ctx->settings->set_boolean(kDepthUseFiducialMarkersKey, ready);
                        if (auto* obj = static_cast<GObject*>(
                                g_weak_ref_get(&recheck_ctx->row_ref))) {
                            adw_switch_row_set_active(ADW_SWITCH_ROW(obj), ready);
                            g_object_unref(obj);
                        }
                    }),
                    recheck_ctx,
                    +[](gpointer data, GClosure*) {
                        auto* recheck_ctx = static_cast<FiducialRecheckContext*>(data);
                        g_weak_ref_clear(&recheck_ctx->row_ref);
                        delete recheck_ctx;
                    },
                    GConnectFlags(0));
                adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ctx->parent->gobj()));
                adw_switch_row_set_active(row, FALSE);
                return;
            }
            ctx->settings->set_boolean(kDepthUseFiducialMarkersKey, active);
        }),
        fiducial_ctx,
        +[](gpointer data, GClosure*) { delete static_cast<FiducialRowContext*>(data); },
        GConnectFlags(0));
    adw_preferences_group_add(depth_group, GTK_WIDGET(fiducial_row));

    // Both depth_model_size_row and fiducial_row hidden, not just
    // insensitive, while depth generation is off — same shared-listener
    // shape as the Transcribe Recordings dependents above.
    struct DepthDependentVisibility {
        GtkWidget* model_size_row;
        GtkWidget* fiducial_row;
    };
    auto* depth_visibility_ctx =
        new DepthDependentVisibility{GTK_WIDGET(depth_model_size_row), GTK_WIDGET(fiducial_row)};
    gtk_widget_set_visible(depth_visibility_ctx->model_size_row, depth_active);
    gtk_widget_set_visible(depth_visibility_ctx->fiducial_row, depth_active);
    g_signal_connect_data(
        depth_row, "notify::active",
        G_CALLBACK(+[](GObject* obj, GParamSpec*, gpointer data) {
            auto* ctx = static_cast<DepthDependentVisibility*>(data);
            gboolean active = adw_switch_row_get_active(ADW_SWITCH_ROW(obj));
            gtk_widget_set_visible(ctx->model_size_row, active);
            gtk_widget_set_visible(ctx->fiducial_row, active);
        }),
        depth_visibility_ctx,
        +[](gpointer data, GClosure*) { delete static_cast<DepthDependentVisibility*>(data); },
        GConnectFlags(0));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), depth_group);

    // The point-data readout — not itself "debug," but a general
    // readout of whatever data is available for the point the pointer
    // is over (azimuth/elevation/depth today, damping/reflection meant
    // to join once those acoustic cues exist). A sighted-tester aid,
    // same category as the gaze cursor — not part of what a blind end
    // user would ever see.
    auto* point_data_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(point_data_group, _("Point Data"));

    auto* show_depth_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(show_depth_row),
                                   _("Show Point Data"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(show_depth_row),
        _("A small readout in the content's bottom-right corner, "
          "showing data for whatever point the pointer is over — "
          "azimuth, elevation, and depth for now"));
    settings->bind(kShowDepthAtPointerKey,
                    Glib::wrap(GTK_WIDGET(show_depth_row), false), "active");
    adw_preferences_group_add(point_data_group, GTK_WIDGET(show_depth_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), point_data_group);

    // First take on the acoustic model's distance cue — separate from
    // Point Data above since this is computed at each *prod*, not
    // continuously under the pointer.
    auto* distance_estimate_group =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(distance_estimate_group,
                                     _("Distance Estimate"));
    auto* grid_size_row = build_augmented_input_row(
        _("Averaging Grid"),
        _("How many depth samples (in a square grid around the prod's "
          "own position) are averaged into its distance estimate"),
        kDepthEstimateGridSizeKey,
        {{_("1 (single point)"), "1"},
         {_("4 (2×2)"), "4"},
         {_("9 (3×3)"), "9"},
         {_("16 (4×4)"), "16"},
         {_("25 (5×5)"), "25"}});
    adw_preferences_group_add(distance_estimate_group, GTK_WIDGET(grid_size_row));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), distance_estimate_group);

    // First take on the acoustic model's carrier signal (see
    // noise_pulse.hpp/.cpp) — all seven ADSR parameters need to be
    // reachable and re-testable live, hence a shared row-building helper
    // rather than writing each out by hand.
    auto* noise_pulse_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(noise_pulse_group, _("Noise Pulse"));
    auto add_noise_pulse_row = [settings, noise_pulse_group](
                                    const Glib::ustring& title,
                                    const Glib::ustring& subtitle,
                                    const char* key, double min, double max,
                                    double step, double display_scale = 1.0) {
        auto* row = ADW_SPIN_ROW(adw_spin_row_new_with_range(min, max, step));
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title.c_str());
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle.c_str());
        adw_spin_row_set_value(row, settings->get_double(key) * display_scale);
        Glib::wrap(adw_spin_row_get_adjustment(row), true)
            ->signal_value_changed()
            .connect([row, settings, key, display_scale]() {
                settings->set_double(key,
                                      adw_spin_row_get_value(row) / display_scale);
            });
        adw_preferences_group_add(noise_pulse_group, GTK_WIDGET(row));
    };
    add_noise_pulse_row(
        _("Attack (ms)"),
        _("Time to reach full volume from the moment a prod fires"),
        kNoisePulseAttackMsKey, 0.0, 500.0, 1.0);
    add_noise_pulse_row(_("Decay (ms)"),
                         _("Time to fall from full volume to the sustain level"),
                         kNoisePulseDecayMsKey, 0.0, 500.0, 1.0);
    add_noise_pulse_row(_("Sustain Level (%)"),
                         _("The level held during the sustain phase"),
                         kNoisePulseSustainLevelKey, 0.0, 100.0, 1.0, 100.0);
    add_noise_pulse_row(
        _("Sustain Duration (ms)"),
        _("How long the sustain level is held before Release begins — a "
          "prod has no held/released gesture for classic ADSR's own "
          "Sustain to last \"as long as\""),
        kNoisePulseSustainMsKey, 0.0, 1000.0, 5.0);
    add_noise_pulse_row(
        _("Release (ms)"),
        _("Time to fade to silence after the sustain phase ends"),
        kNoisePulseReleaseMsKey, 0.0, 2000.0, 5.0);
    add_noise_pulse_row(
        _("Color (%)"),
        _("Brightness of the underlying noise — 0 is dark/filtered, 100 "
          "is bright/unfiltered white noise"),
        kNoisePulseColorKey, 0.0, 100.0, 5.0, 100.0);
    add_noise_pulse_row(_("Gain (%)"), _("Overall loudness of the pulse"),
                         kNoisePulseGainKey, 0.0, 100.0, 5.0, 100.0);

    // Distance → echo delay — integer ms, not one of
    // add_noise_pulse_row()'s double-valued rows above, hence written
    // out directly.
    auto* max_delay_row =
        ADW_SPIN_ROW(adw_spin_row_new_with_range(0.0, 30000.0, 100.0));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(max_delay_row),
                                   _("Max Echo Delay (ms)"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(max_delay_row),
        _("Ceiling on the distance-based delay before a prod's sound "
          "plays — see the toggle below for what happens past it"));
    adw_spin_row_set_value(max_delay_row,
                            settings->get_int(kNoisePulseMaxDelayMsKey));
    Glib::wrap(adw_spin_row_get_adjustment(max_delay_row), true)
        ->signal_value_changed()
        .connect([max_delay_row, settings]() {
            settings->set_int(
                kNoisePulseMaxDelayMsKey,
                static_cast<int>(adw_spin_row_get_value(max_delay_row)));
        });
    adw_preferences_group_add(noise_pulse_group, GTK_WIDGET(max_delay_row));

    auto* max_delay_is_no_data_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(max_delay_is_no_data_row),
                                   _("Max Delay Means No Data"));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(max_delay_is_no_data_row),
        _("On: past the max delay, play nothing — the depth reading "
          "wasn't reliable, not genuinely that far away. Off: clamp to "
          "the max delay and play it anyway"));
    settings->bind(kNoisePulseMaxDelayIsNoDataKey,
                    Glib::wrap(GTK_WIDGET(max_delay_is_no_data_row), false),
                    "active");
    adw_preferences_group_add(noise_pulse_group,
                               GTK_WIDGET(max_delay_is_no_data_row));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), noise_pulse_group);

    // Preferred Bigrams — a short, rarely-touched list (John: normally
    // just the user's own full name) pinning monitoring_dialogs.cpp's
    // own two-letter header label for a name, so a participant's label
    // stays recognizable across sessions instead of whatever
    // assign_bigrams() would otherwise land on fresh each time.
    // AdwEntryRow only has one text field, so each entry is a plain
    // two-Gtk::Entry row built by hand rather than reusing that widget.
    auto* bigrams_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(bigrams_group, _("Preferred Bigrams"));
    adw_preferences_group_set_description(
        bigrams_group,
        _("Pin the two-letter label a name gets in the monitoring matrix "
          "— normally just your own full name, so your own header stays "
          "the same across sessions"));

    auto* bigrams_list = Gtk::make_managed<Gtk::ListBox>();
    bigrams_list->add_css_class("boxed-list");
    bigrams_list->set_selection_mode(Gtk::SelectionMode::NONE);

    // Forward-referenced the same way monitoring_dialogs.cpp's own
    // refresh_ptr is — add_bigram_row's Remove button needs to call
    // save_bigrams, and the "Add" button below needs to call
    // add_bigram_row, both before either is fully defined.
    // rebuild_devices_list is filled in later still (it needs the
    // Preferred Devices group's own widgets, built further down), but
    // save_bigrams already needs to call it — the fixed-participant
    // list it builds is derived straight from this one.
    auto save_bigrams = std::make_shared<std::function<void()>>();
    auto add_bigram_row =
        std::make_shared<std::function<void(const Glib::ustring&, const Glib::ustring&)>>();
    auto rebuild_devices_list = std::make_shared<std::function<void()>>();

    *save_bigrams = [bigrams_list, settings, rebuild_devices_list]() {
        std::map<Glib::ustring, Glib::ustring> bigrams;
        for (auto* child = bigrams_list->get_first_child(); child != nullptr;
             child = child->get_next_sibling()) {
            auto* row = dynamic_cast<Gtk::ListBoxRow*>(child);
            auto* row_box = row ? dynamic_cast<Gtk::Box*>(row->get_child()) : nullptr;
            auto* name_entry =
                row_box ? dynamic_cast<Gtk::Entry*>(row_box->get_first_child()) : nullptr;
            auto* bigram_entry =
                name_entry ? dynamic_cast<Gtk::Entry*>(name_entry->get_next_sibling()) : nullptr;
            if (!bigram_entry) {
                continue;
            }
            Glib::ustring name = name_entry->get_text();
            if (name.empty()) {
                continue;
            }
            bigrams[name] = bigram_entry->get_text();
        }
        set_preferred_bigrams(settings, bigrams);
        if (*rebuild_devices_list) {
            (*rebuild_devices_list)();
        }
    };

    *add_bigram_row = [bigrams_list, save_bigrams](const Glib::ustring& name,
                                                     const Glib::ustring& bigram) {
        auto* row_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 8);
        row_box->set_margin(8);

        auto* name_entry = Gtk::make_managed<Gtk::Entry>();
        name_entry->set_placeholder_text(_("Full name"));
        name_entry->set_text(name);
        name_entry->set_hexpand(true);

        auto* bigram_entry = Gtk::make_managed<Gtk::Entry>();
        bigram_entry->set_placeholder_text(_("Bigram"));
        bigram_entry->set_text(bigram);
        bigram_entry->set_max_length(2);
        bigram_entry->set_width_chars(3);

        auto* remove_button = Gtk::make_managed<Gtk::Button>();
        remove_button->set_icon_name("list-remove-symbolic");
        remove_button->add_css_class("flat");

        row_box->append(*name_entry);
        row_box->append(*bigram_entry);
        row_box->append(*remove_button);

        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->set_child(*row_box);
        bigrams_list->append(*row);

        name_entry->signal_changed().connect([save_bigrams]() { (*save_bigrams)(); });
        bigram_entry->signal_changed().connect([save_bigrams]() { (*save_bigrams)(); });
        remove_button->signal_clicked().connect([bigrams_list, row, save_bigrams]() {
            bigrams_list->remove(*row);
            (*save_bigrams)();
        });
    };

    for (const auto& [name, bigram] : get_preferred_bigrams(settings)) {
        (*add_bigram_row)(name, bigram);
    }

    adw_preferences_group_add(bigrams_group, GTK_WIDGET(bigrams_list->gobj()));

    auto* add_bigram_button = Gtk::make_managed<Gtk::Button>(_("Add"));
    add_bigram_button->set_halign(Gtk::Align::START);
    add_bigram_button->set_margin_top(8);
    add_bigram_button->signal_clicked().connect(
        [add_bigram_row]() { (*add_bigram_row)(Glib::ustring(), Glib::ustring()); });
    adw_preferences_group_add(bigrams_group, GTK_WIDGET(add_bigram_button->gobj()));

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), bigrams_group);

    // Preferred Devices — one row per fixed participant: Ambient
    // Microphone/Speaker (always present, one axis each) plus every
    // name with a preferred bigram above (both axes, same as a real
    // participant in the matrix — John: "de tidligere navnene som er
    // gitt bigrammer," derived from that list rather than edited
    // separately). Each device button reuses monitoring_dialogs.cpp's
    // own show_device_picker_dialog() — no MonitoringState involved
    // here, this only ever seeds a session's live state
    // (seed_preferred_devices()) when that state doesn't already have
    // an assignment, so it's a default, not a lock, and a track's
    // logged device is always whatever the live session state actually
    // ended up with, never this setting directly. Left empty, a fixed
    // participant falls back to manual per-session assignment exactly
    // as before.
    auto* devices_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(devices_group, _("Preferred Devices"));
    adw_preferences_group_set_description(
        devices_group,
        _("A starting device for Ambient Microphone, Ambient Speaker, "
          "and every name above — pre-fills the monitoring matrix's own "
          "assignment at the start of a session, but stays freely "
          "overridable there. Leave a field as “None” to assign "
          "that device by hand each session instead."));

    auto* devices_list = Gtk::make_managed<Gtk::ListBox>();
    devices_list->add_css_class("boxed-list");
    devices_list->set_selection_mode(Gtk::SelectionMode::NONE);

    auto make_device_button = [parent, settings](bool is_source, const Glib::ustring& id) {
        auto* button = Gtk::make_managed<Gtk::Button>();
        auto refresh_label = std::make_shared<std::function<void()>>();
        *refresh_label = [button, settings, is_source, id]() {
            auto devices = is_source ? get_preferred_source_devices(settings)
                                       : get_preferred_sink_devices(settings);
            auto it = devices.find(id);
            button->set_label(it != devices.end() && !it->second.first.empty()
                                   ? it->second.second
                                   : Glib::ustring(_("None")));
        };
        (*refresh_label)();
        button->signal_clicked().connect([parent, settings, is_source, id, refresh_label]() {
            auto devices = is_source ? get_preferred_source_devices(settings)
                                       : get_preferred_sink_devices(settings);
            auto existing = devices.find(id);
            std::string current_device_id =
                existing != devices.end() ? std::string(existing->second.first) : "";
            show_device_picker_dialog(
                parent, is_source ? _("Input (Microphone)") : _("Output (Speaker)"), _("None"),
                is_source ? "Audio/Source" : "Audio/Sink", current_device_id, {},
                [settings, is_source, id, refresh_label](const AudioDeviceInfo* device,
                                                           std::function<void()> close) {
                    auto devices = is_source ? get_preferred_source_devices(settings)
                                               : get_preferred_sink_devices(settings);
                    if (!device) {
                        devices.erase(id);
                    } else {
                        devices[id] = {Glib::ustring(device->id),
                                        Glib::ustring(device->display_name)};
                    }
                    if (is_source) {
                        set_preferred_source_devices(settings, devices);
                    } else {
                        set_preferred_sink_devices(settings, devices);
                    }
                    (*refresh_label)();
                    close();
                });
        });
        return button;
    };

    *rebuild_devices_list = [devices_list, settings, make_device_button]() {
        while (auto* child = devices_list->get_first_child()) {
            devices_list->remove(*child);
        }
        auto add_device_row = [devices_list, make_device_button](
                                    const Glib::ustring& label, const Glib::ustring& id,
                                    bool has_sink, bool has_source) {
            auto* row = ADW_ACTION_ROW(adw_action_row_new());
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
            if (has_sink) {
                adw_action_row_add_suffix(row,
                                            GTK_WIDGET(make_device_button(false, id)->gobj()));
            }
            if (has_source) {
                adw_action_row_add_suffix(row, GTK_WIDGET(make_device_button(true, id)->gobj()));
            }
            devices_list->append(*Glib::wrap(GTK_LIST_BOX_ROW(row), false));
        };
        add_device_row(_("Ambient Microphone"), kAmbientMicSourceId, false, true);
        add_device_row(_("Ambient Speaker"), kSpeakerSinkId, true, false);
        for (const auto& [name, bigram] : get_preferred_bigrams(settings)) {
            add_device_row(name, name, true, true);
        }
    };
    (*rebuild_devices_list)();

    adw_preferences_group_add(devices_group, GTK_WIDGET(devices_list->gobj()));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), devices_group);

    adw_preferences_dialog_add(prefs, ADW_PREFERENCES_PAGE(page));

    adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(parent->gobj()));
}
