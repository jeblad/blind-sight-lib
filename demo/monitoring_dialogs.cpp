// SPDX-License-Identifier: GPL-3.0-only
//
// monitoring_dialogs.cpp — see monitoring_dialogs.hpp for scope.

#include "monitoring_dialogs.hpp"

#include <adwaita.h>
#include <glib/gi18n.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>

#include "audio_devices.hpp"
#include "audio_routing.hpp"
#include "preferences.hpp"
#include "recording.hpp"

using blind_sight::AudioDeviceInfo;
using blind_sight::AudioRoutingEngine;
using blind_sight::DeviceAssignment;
using blind_sight::enumerate_audio_devices;
using blind_sight::find_counterpart_device;
using blind_sight::is_muted;
using blind_sight::kAmbientMicSourceId;
using blind_sight::kSpeakerSinkId;
using blind_sight::kSynthesizedSourceId;
using blind_sight::MonitoringState;
using blind_sight::participant_names;
using blind_sight::reconcile_monitoring_state;
using blind_sight::RecordMeta;
using blind_sight::read_record_meta;
using blind_sight::set_muted;

// Bridges into main.cpp's anonymous namespace, same pattern every other
// *_dialogs.cpp file already uses for icon loading.
std::string bridge_resource_path(const std::string& filename);

namespace {

// Plain-Gtk::Image loader, same shape as collection_editor.cpp's own
// (local, not itself bridged) set_image_icon_from_file() — needed here
// for the Synthesized column's icon header, which isn't inside a
// button.
// Every matrix cell — toggle buttons, participant header buttons, the
// Speaker/Synthesized icons — is forced to this exact square size, so
// the grid's column/row widths come out identical regardless of how
// much natural width any one cell's own content (icon vs. two-letter
// text) would otherwise ask for.
constexpr int kMatrixCellSize = 40;

void set_image_icon(Gtk::Image* image, const std::string& filename, int size = 16) {
    auto file = Gio::File::create_for_path(bridge_resource_path("icons/" + filename));
    image->set(Gtk::IconPaintable::create(file, size));
    image->set_pixel_size(size);
}

Gtk::Image* make_icon_image(const std::string& filename, int size = 16) {
    auto* image = Gtk::make_managed<Gtk::Image>();
    set_image_icon(image, filename, size);
    return image;
}

// A header whose assigned device is missing right now: its own content
// (`base`) is dimmed, with disconnected-symbolic laid over it at full
// strength in the foreground color (see main.cpp's
// .blind-sight-matrix-disconnected-mark).
void mark_disconnected(Gtk::Overlay* overlay, Gtk::Widget* base) {
    base->add_css_class("dimmed");
    auto* mark = make_icon_image("disconnected-symbolic.svg");
    mark->add_css_class("blind-sight-matrix-disconnected-mark");
    overlay->add_overlay(*mark);
}

Glib::ustring label_for_id(const std::string& id) {
    if (id == kSpeakerSinkId) {
        return _("Ambient Speaker");
    }
    if (id == kAmbientMicSourceId) {
        return _("Ambient Microphone");
    }
    if (id == kSynthesizedSourceId) {
        return _("Synthesized");
    }
    return id;
}

// Toggle-cell icon: a checkbox outline (checkbox-symbolic.svg), always
// visible, with a checkmark (object-select-symbolic.svg) layered on top
// only when the sink actually hears this source — an ordinary checkbox
// look, not a mic icon, since the cell itself is a listening
// relationship. Both SVGs are transparent outside their own glyph, so
// the overlay reads as one combined icon rather than two stacked boxes.
// Replaced the earlier circle-symbolic/circle-check-cross-symbolic pair
// — John: "circle*symbolic fungerer dårlig" (worked poorly at this
// size/density).
struct ToggleCellIcon {
    Gtk::Widget* widget;
    Gtk::Image* checkmark;
};

ToggleCellIcon make_toggle_cell_icon(bool muted) {
    auto* overlay = Gtk::make_managed<Gtk::Overlay>();
    auto* background = make_icon_image("checkbox-symbolic.svg");
    // Dimmed so it recedes behind the checkmark instead of competing
    // with it — the checkbox outline is a container, not the signal.
    background->set_opacity(0.4);
    overlay->set_child(*background);
    auto* checkmark = make_icon_image("object-select-symbolic.svg");
    checkmark->set_visible(!muted);
    overlay->add_overlay(*checkmark);
    return {overlay, checkmark};
}

std::vector<std::string> split_name_words(const std::string& name) {
    std::vector<std::string> words;
    std::size_t start = 0;
    while (start < name.size()) {
        std::size_t end = name.find(' ', start);
        if (end == std::string::npos) {
            end = name.size();
        }
        if (end > start) {
            words.push_back(name.substr(start, end - start));
        }
        start = end + 1;
    }
    return words;
}

char to_upper_char(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

// Candidate two-letter header labels for one participant's name, most
// preferred first: the first word's own first letter, paired with the
// first letter of each subsequent word in turn ("John Erling Blad" ->
// "JE", then "JB"); falling back to pairing that same first letter with
// each later letter *within* the first word itself ("JO", "JH", "JN")
// once the whole-word pairings are exhausted.
std::vector<std::string> bigram_candidates(const std::string& name) {
    std::vector<std::string> words = split_name_words(name);
    std::vector<std::string> candidates;
    if (words.empty()) {
        return candidates;
    }
    char first = to_upper_char(words.front()[0]);
    for (std::size_t i = 1; i < words.size(); ++i) {
        candidates.push_back({first, to_upper_char(words[i][0])});
    }
    for (std::size_t i = 1; i < words.front().size(); ++i) {
        candidates.push_back({first, to_upper_char(words.front()[i])});
    }
    if (candidates.empty()) {
        candidates.push_back({first, first});
    }
    return candidates;
}

// One two-letter header label per participant (same order as
// `participant_order`), resolving collisions by walking from the LAST
// participant toward the front, moving a colliding entry to its own
// next candidate — so an earlier (more prominent, see
// MonitoringState's own ordering comment) participant keeps its
// first-choice pair while later ones give way. Not guaranteed
// collision-free (e.g. two participants sharing an exact name) — a
// best effort for a header button's text, not a stable identifier (see
// monitoring.hpp's own comment on name-based identity).
//
// `preferred_bigrams` (Preferences' own "Preferred Bigrams" list, see
// get_preferred_bigrams()) is consulted first for each name — a pinned
// choice is tried before the natural candidates, so it's respected
// whenever it doesn't collide with an earlier participant's own result,
// same best-effort guarantee as everything else here.
std::vector<std::string> assign_bigrams(
    const std::vector<std::string>& participant_order,
    const std::map<Glib::ustring, Glib::ustring>& preferred_bigrams) {
    std::size_t n = participant_order.size();
    std::vector<std::vector<std::string>> candidates(n);
    for (std::size_t i = 0; i < n; ++i) {
        auto preferred = preferred_bigrams.find(participant_order[i]);
        if (preferred != preferred_bigrams.end() && !preferred->second.empty()) {
            candidates[i].push_back(preferred->second);
        }
        std::vector<std::string> natural = bigram_candidates(participant_order[i]);
        candidates[i].insert(candidates[i].end(), natural.begin(), natural.end());
    }
    std::vector<std::string> result(n);
    for (std::size_t i = 0; i < n; ++i) {
        result[i] = candidates[i].empty() ? std::string("??") : candidates[i][0];
    }
    for (std::size_t idx = n; idx-- > 0;) {
        std::size_t attempt = 0;
        while (attempt + 1 < candidates[idx].size()) {
            bool collides = false;
            for (std::size_t other = 0; other < n; ++other) {
                if (other != idx && result[other] == result[idx]) {
                    collides = true;
                    break;
                }
            }
            if (!collides) {
                break;
            }
            ++attempt;
            result[idx] = candidates[idx][attempt];
        }
    }
    return result;
}

}  // namespace

// Real device picker — enumerates via enumerate_audio_devices(), shows a
// selectable boxed-list (one row per device, subtitle = Bluetooth/
// connection status, further annotated by `annotate_devices` if set)
// with an optional leading "None" row (omitted entirely when
// `none_row_label` is empty — John: picking a device for Ambient
// Speaker/Microphone's own setup has no sensible "none" to fall back
// to, unlike a participant falling back to the shared ambient device).
// Pre-selects the row matching `current_device_id`, if any. `on_save`
// gets the chosen device (nullptr for "None") plus a `close` callback it
// must call itself once it's actually done — not called automatically,
// so a caller could show something on top of this dialog and leave it
// open, though the current callers all close immediately now that
// picking an already-assigned device transfers ownership rather than
// being blocked.
//
// Exposed (monitoring_dialogs.hpp) rather than kept file-local — no
// MonitoringState coupling at all, so preferences.cpp reuses it directly
// for the Preferred Devices group's own device pickers.
std::function<void(const std::string& device_id)> show_device_picker_dialog(
    Gtk::Window* parent, const Glib::ustring& title, const Glib::ustring& none_row_label,
    const std::string& gst_device_class, const std::string& current_device_id,
    std::function<void(std::vector<AudioDeviceInfo>&)> annotate_devices,
    std::function<void(const AudioDeviceInfo*, std::function<void()> close)> on_save,
    Gtk::Widget* footer) {
    auto devices =
        std::make_shared<std::vector<AudioDeviceInfo>>(enumerate_audio_devices(gst_device_class));
    if (annotate_devices) {
        annotate_devices(*devices);
    }
    bool has_none_row = !none_row_label.empty();

    auto* dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, title.c_str());
    adw_dialog_set_content_width(dialog, 420);

    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    box->set_margin(24);

    auto* list_box = Gtk::make_managed<Gtk::ListBox>();
    list_box->add_css_class("boxed-list");
    list_box->set_selection_mode(Gtk::SelectionMode::SINGLE);
    // Explicit rather than relying on GTK's own default — a single
    // click must activate a row immediately for the direct-action
    // picker below to work.
    list_box->set_activate_on_single_click(true);

    // A checkmark on the row in use, as GNOME's own choice lists do — the
    // selection highlight alone reads weakly once the dialog stays open
    // after a pick. One per row, in row order. Each is referenced here:
    // while the dialog is torn down the list emits row-selected again,
    // after some rows (and their marks) are already gone.
    auto checkmarks = std::shared_ptr<std::vector<GtkWidget*>>(
        new std::vector<GtkWidget*>, [](std::vector<GtkWidget*>* marks) {
            for (GtkWidget* mark : *marks) {
                g_object_unref(mark);
            }
            delete marks;
        });
    auto add_checkmark = [checkmarks](AdwActionRow* row) {
        GtkWidget* mark = GTK_WIDGET(make_icon_image("object-select-symbolic.svg")->gobj());
        gtk_widget_set_visible(mark, FALSE);
        adw_action_row_add_suffix(row, mark);
        checkmarks->push_back(GTK_WIDGET(g_object_ref(mark)));
    };

    if (has_none_row) {
        auto* none_row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(none_row), none_row_label.c_str());
        // Plain boolean GObject property — same fix collection_menu.cpp
        // already needed (see its own identical comment): an AdwActionRow
        // doesn't fire row-activated on a plain click without this set,
        // confirmed live as the actual cause of a participant's device
        // pick never committing at all once the separate Save button
        // was removed in favor of direct-action row clicks.
        g_object_set(none_row, "activatable", TRUE, nullptr);
        add_checkmark(ADW_ACTION_ROW(none_row));
        list_box->append(*Glib::wrap(none_row, false));
    }

    Gtk::ListBoxRow* selected_row = nullptr;
    for (const auto& device : *devices) {
        auto* row = adw_action_row_new();
        g_object_set(row, "activatable", TRUE, nullptr);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), device.display_name.c_str());
        if (!device.subtitle.empty()) {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), device.subtitle.c_str());
        }
        if (!device.owner_bigram.empty()) {
            // Right-aligned, on the title line itself (add_suffix, not the
            // subtitle) — the bigram alone is an implicit "this one's taken"
            // signal, no need to spell out "assigned to" for it to read.
            auto* owner_label =
                Gtk::make_managed<Gtk::Label>("(" + device.owner_bigram + ")");
            owner_label->add_css_class("dim-label");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(owner_label->gobj()));
        }
        add_checkmark(ADW_ACTION_ROW(row));
        auto* wrapped_row = Glib::wrap(GTK_LIST_BOX_ROW(row), false);
        list_box->append(*wrapped_row);
        if (!current_device_id.empty() && device.id == current_device_id) {
            selected_row = wrapped_row;
        }
    }
    list_box->signal_row_selected().connect([checkmarks](Gtk::ListBoxRow* row) {
        int selected = row ? row->get_index() : -1;
        for (std::size_t i = 0; i < checkmarks->size(); ++i) {
            gtk_widget_set_visible((*checkmarks)[i], static_cast<int>(i) == selected);
        }
    });
    list_box->select_row(selected_row ? *selected_row : *list_box->get_row_at_index(0));

    auto* list_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    list_scroller->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
    list_scroller->set_max_content_height(300);
    list_scroller->set_propagate_natural_height(true);
    list_scroller->set_child(*list_box);
    box->append(*list_scroller);
    if (footer) {
        box->append(*footer);
    }

    // No Cancel/Save row at all — this dialog acts directly on a row
    // pick, so backing out just means closing it (the header bar's own
    // close button, now that it has one). The old "Dump to File" debug
    // aid moved to the hamburger menu's "Dump Sound Paths" action
    // (main.cpp) — it was always a development tool for tracing
    // enumeration problems in general, not something that belonged
    // bolted onto one specific picker instance.

    // adw_dialog_set_title() alone sets metadata only — nothing paints
    // it unless the dialog's content includes an AdwHeaderBar, which
    // picks the title up automatically when placed inside an AdwDialog
    // (confirmed in libadwaita's own AdwHeaderBar docs). Without one,
    // as this dialog had until now, the title row is just blank on a
    // regular desktop window — not a Mutter/window-manager thing, since
    // AdwDialog is an in-process sheet GTK draws itself, never a real
    // WM-decorated top-level surface.
    auto* toolbar_view = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(adw_header_bar_new()));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar_view), GTK_WIDGET(box->gobj()));
    adw_dialog_set_child(dialog, toolbar_view);

    // Direct action, no separate Save step — John: "Disse dialogene
    // setter ett valg, og bruker dette." GtkListBox activates a row on
    // a single click by default, so this fires as soon as one is
    // picked.
    list_box->signal_row_activated().connect(
        [dialog, devices, has_none_row, on_save](Gtk::ListBoxRow* row) {
            auto close = [dialog]() { adw_dialog_close(dialog); };
            if (!row || (has_none_row && row->get_index() == 0)) {
                on_save(nullptr, close);
                return;
            }
            auto device_index =
                static_cast<std::size_t>(row->get_index() - (has_none_row ? 1 : 0));
            if (device_index < devices->size()) {
                on_save(&(*devices)[device_index], close);
            } else {
                close();
            }
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));

    // A weak pointer, nulled by GObject once the list is gone: a caller may
    // reselect after the dialog has closed (a confirmation answered
    // later). A strong reference would form a cycle through the caller's
    // own row-activated handler and keep the list alive forever.
    auto list = std::shared_ptr<GtkListBox*>(new GtkListBox*(list_box->gobj()),
                                             [](GtkListBox** p) {
                                                 if (*p) {
                                                     g_object_remove_weak_pointer(
                                                         G_OBJECT(*p), reinterpret_cast<gpointer*>(p));
                                                 }
                                                 delete p;
                                             });
    g_object_add_weak_pointer(G_OBJECT(*list), reinterpret_cast<gpointer*>(list.get()));
    return [list, devices, has_none_row](const std::string& device_id) {
        if (!*list) {
            return;
        }
        int index = -1;
        if (device_id.empty()) {
            index = has_none_row ? 0 : -1;
        } else {
            for (std::size_t i = 0; i < devices->size(); ++i) {
                if ((*devices)[i].id == device_id) {
                    index = static_cast<int>(i) + (has_none_row ? 1 : 0);
                    break;
                }
            }
        }
        if (index < 0) {
            gtk_list_box_unselect_all(*list);
        } else {
            gtk_list_box_select_row(*list, gtk_list_box_get_row_at_index(*list, index));
        }
    };
}

namespace {

// Who else (another PARTICIPANT only — Ambient Speaker/Microphone are
// exempt from this whole ownership contest, see
// show_participant_device_dialog()'s own comment) already holds
// `device_id` in `pool`, or empty if none/unowned.
std::string find_participant_owner(const std::map<std::string, DeviceAssignment>& pool,
                                    const std::string& id, const std::string& device_id) {
    if (device_id.empty()) {
        return "";
    }
    for (const auto& [other_id, assignment] : pool) {
        if (other_id == id || other_id == kSpeakerSinkId || other_id == kAmbientMicSourceId) {
            continue;
        }
        if (assignment.device_id == device_id) {
            return other_id;
        }
    }
    return "";
}

// Commits `device` to `id` within the pool matching `is_source`. If
// another participant already holds it, takes ownership from them —
// John: "Hvis en velger en slik enhet så tar en eierskap på den, og den
// andre deltageren mister sin device." If that same device_id is ALSO
// that participant's assignment in the OTHER pool (one physical device
// serving as both their sink and source), they lose that one too — John:
// "skal deltageren miste både source og sink om dette er en kombinert
// enhet." Pure state mutation, no confirmation and no notify_changed —
// see assign_device_with_confirmation() for the warn-before-steal flow
// callers are expected to go through instead of calling this directly.
void assign_device(MonitoringState& state, bool is_source, const std::string& id,
                    const AudioDeviceInfo& device) {
    auto& pool = is_source ? state.source_device_assignment : state.sink_device_assignment;
    auto& other_pool = is_source ? state.sink_device_assignment : state.source_device_assignment;
    std::string previous_owner = find_participant_owner(pool, id, device.id);
    if (!previous_owner.empty()) {
        pool.erase(previous_owner);
        auto other_it = other_pool.find(previous_owner);
        if (other_it != other_pool.end() && other_it->second.device_id == device.id) {
            other_pool.erase(other_it);
        }
    }
    pool[id] = DeviceAssignment{device.id, device.display_name};
}

// Heap-allocated since AdwAlertDialog's "response" signal is async and
// needs a plain C callback (no gtkmm wrapper exists for this raw
// libadwaita type) — freed by the GClosureNotify passed to
// g_signal_connect_data() in assign_device_with_confirmation().
struct StealConfirmContext {
    MonitoringState* state;
    bool is_source;
    std::string id;
    AudioDeviceInfo device;
    std::function<void()> on_committed;
    std::function<void()> on_cancelled;
};

void on_steal_confirm_response(AdwAlertDialog*, const gchar* response, gpointer user_data) {
    auto* context = static_cast<StealConfirmContext*>(user_data);
    if (g_strcmp0(response, "steal") == 0) {
        assign_device(*context->state, context->is_source, context->id, context->device);
        context->on_committed();
    } else if (context->on_cancelled) {
        context->on_cancelled();
    }
}

// Warns, by the owning participant's full name, before taking a device
// away from them — John: picking an in-use device should no longer
// transfer ownership silently (the picker's own "(XY)" bigram suffix
// isn't enough warning on its own); the user must see who loses it and
// explicitly continue. No dialog at all, straight commit, if the device
// is currently unowned. `on_committed` runs only once the assignment
// actually happens — never on Cancel — so callers can chain follow-up
// steps (e.g. the counterpart-device offer) after it; `on_cancelled`, if
// given, runs on Cancel instead.
void assign_device_with_confirmation(Gtk::Window* parent, MonitoringState* state, bool is_source,
                                      const std::string& id, const AudioDeviceInfo& device,
                                      std::function<void()> on_committed,
                                      std::function<void()> on_cancelled = {}) {
    auto& pool = is_source ? state->source_device_assignment : state->sink_device_assignment;
    std::string previous_owner = find_participant_owner(pool, id, device.id);
    if (previous_owner.empty()) {
        assign_device(*state, is_source, id, device);
        on_committed();
        return;
    }

    Glib::ustring body = Glib::ustring::compose(
        _("%1 is currently using this device. Continuing will disconnect it from %1."),
        label_for_id(previous_owner));
    AdwDialog* confirm = adw_alert_dialog_new(_("Reassign Device?"), body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(confirm), "cancel", _("Cancel"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(confirm), "steal", _("Continue"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(confirm), "steal",
                                              ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(confirm), "cancel");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(confirm), "cancel");

    auto* context = new StealConfirmContext{state, is_source, id, device, on_committed, on_cancelled};
    g_signal_connect_data(
        confirm, "response", G_CALLBACK(on_steal_confirm_response), context,
        +[](gpointer data, GClosure*) { delete static_cast<StealConfirmContext*>(data); },
        GConnectFlags(0));

    adw_dialog_present(confirm, GTK_WIDGET(parent->gobj()));
}

// Heap-allocated for the same reason as StealConfirmContext — freed by
// the GClosureNotify passed to g_signal_connect_data() in
// offer_counterpart_assignment().
struct OfferResponseContext {
    Gtk::Window* parent;
    MonitoringState* state;
    std::string id;
    bool other_is_source;
    AudioDeviceInfo counterpart;
    std::function<void()> notify_changed;
};

void on_offer_response(AdwAlertDialog*, const gchar* response, gpointer user_data) {
    auto* context = static_cast<OfferResponseContext*>(user_data);
    if (g_strcmp0(response, "yes") == 0) {
        assign_device_with_confirmation(context->parent, context->state, context->other_is_source,
                                         context->id, context->counterpart,
                                         context->notify_changed);
        return;
    }
    // Declining a handsfree-only mic means the participant is heard
    // through the ambient microphone instead — unless they already have
    // a mic of their own.
    if (context->other_is_source && context->counterpart.handsfree_only_mic) {
        auto& pool = context->state->source_device_assignment;
        auto existing = pool.find(context->id);
        if (existing == pool.end() || existing->second.device_id.empty()) {
            pool[context->id] =
                DeviceAssignment{kAmbientMicSourceId, label_for_id(kAmbientMicSourceId)};
            context->notify_changed();
        }
    }
}

// Headphones can come in oddly split variants — John: "Hodetelefonen
// som sink kan være analog, mens microfonen er digital over BT." If the
// device just picked genuinely also works the other way (see
// find_counterpart_device()), offer to save the user from repeating the
// whole picker for the matching device, rather than assuming they want
// it — it's still their call, and the same warn-before-steal rule
// applies even on a "yes" (via assign_device_with_confirmation()).
void offer_counterpart_assignment(Gtk::Window* parent, MonitoringState* state,
                                   const std::string& id, bool other_is_source,
                                   const AudioDeviceInfo& counterpart,
                                   std::function<void()> notify_changed) {
    bool reduced_quality_mic = other_is_source && counterpart.handsfree_only_mic;
    Glib::ustring body =
        reduced_quality_mic
            ? Glib::ustring::compose(
                  _("%1 can only be used as a microphone in handsfree mode, which reduces "
                    "the sound quality in both directions — including what you hear. If not, "
                    "the ambient microphone is used for you. Use it as your microphone anyway?"),
                  counterpart.display_name)
            : Glib::ustring::compose(_("%1 can also be used as your %2. Assign it there too?"),
                                     counterpart.display_name,
                                     other_is_source ? _("microphone") : _("listening device"));
    AdwDialog* offer = adw_alert_dialog_new(
        other_is_source ? _("Also Use as Microphone?") : _("Also Use as Listening Device?"),
        body.c_str());
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(offer), "no", _("No"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(offer), "yes", _("Yes"));
    const char* recommended = reduced_quality_mic ? "no" : "yes";
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(offer), recommended,
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(offer), recommended);
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(offer), "no");

    auto* context =
        new OfferResponseContext{parent, state, id, other_is_source, counterpart, notify_changed};
    g_signal_connect_data(
        offer, "response", G_CALLBACK(on_offer_response), context,
        +[](gpointer data, GClosure*) { delete static_cast<OfferResponseContext*>(data); },
        GConnectFlags(0));

    adw_dialog_present(offer, GTK_WIDGET(parent->gobj()));
}

// `gst_device_class` — "Audio/Source" (opened from the top row/column-
// header axis, a participant's own mic) or "Audio/Sink" (opened from
// the left column/row-header axis, a participant's own listening
// device) for a participant; always "Audio/Sink" for kSpeakerSinkId,
// always "Audio/Source" for kAmbientMicSourceId (each has only one
// direction, see MonitoringState's own comment). John: "Jeg tror
// dialogen må utvides med en overskrift om det er 'Input (microphone)'
// eller 'Output (speaker)'" — the title reflects this directly rather
// than a single generic "Listening Device" label for every case.
//
// Ambient Speaker/Microphone (`is_ambient_itself`) are exempt from the
// whole ownership-transfer story: no "(assigned to X)" annotation, no
// taking a device from a participant, no "None" row at all (there's no
// sensible fallback for the ambient device's own setup). Everything in
// that paragraph is participant-only.
// A volume slider in dB. Logarithmic by construction: equal slider steps
// are equal loudness steps. `on_change` fires once the slider has rested
// for kVolumeSettleMs, not on every motion step, since each change to a
// voice volume spawns pw-cli.
constexpr double kVolumeMinDb = -40.0;
constexpr double kVolumeMaxDb = 10.0;
constexpr unsigned int kVolumeSettleMs = 150;

// `suppress` set: the value is being shown, not chosen — no on_change.
Gtk::Widget* make_volume_footer(const Glib::ustring& title, double initial_db,
                                std::function<void(double db)> on_change,
                                std::shared_ptr<bool> suppress, Gtk::Scale** scale_out) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
    auto* label = Gtk::make_managed<Gtk::Label>(title);
    label->set_xalign(0.0);
    label->add_css_class("heading");
    box->append(*label);

    auto* scale = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::HORIZONTAL);
    scale->set_range(kVolumeMinDb, kVolumeMaxDb);
    scale->set_increments(1.0, 6.0);
    scale->set_round_digits(0);
    scale->set_value(initial_db);
    scale->set_draw_value(true);
    scale->set_value_pos(Gtk::PositionType::RIGHT);
    scale->set_format_value_func([](double db) {
        // A real minus sign, not a hyphen, before the number.
        return Glib::ustring::compose(_("%1 dB"),
                                      db < -0.5 ? Glib::ustring("\u2212") +
                                                      Glib::ustring::format(std::lround(-db))
                                                : Glib::ustring::format(std::lround(db)));
    });
    scale->add_mark(0.0, Gtk::PositionType::BOTTOM, "");
    scale->set_hexpand(true);
    box->append(*scale);

    *scale_out = scale;
    auto pending = std::make_shared<sigc::connection>();
    scale->signal_value_changed().connect([scale, pending, on_change, suppress]() {
        if (*suppress) {
            return;
        }
        double db = scale->get_value();
        pending->disconnect();
        *pending = Glib::signal_timeout().connect(
            [on_change, db]() {
                on_change(db);
                return false;
            },
            kVolumeSettleMs);
    });
    return box;
}

void show_participant_device_dialog(Gtk::Window* parent, MonitoringState* state,
                                     const std::string& id, const std::string& gst_device_class,
                                     const Glib::RefPtr<Gio::Settings>& settings,
                                     AudioRoutingEngine* routing_engine,
                                     std::function<void()> notify_changed) {
    bool is_source = gst_device_class == "Audio/Source";
    bool is_ambient_itself = id == kSpeakerSinkId || id == kAmbientMicSourceId;
    std::map<std::string, DeviceAssignment>& pool =
        is_source ? state->source_device_assignment : state->sink_device_assignment;
    auto existing = pool.find(id);
    std::string current_device_id = existing != pool.end() ? existing->second.device_id : "";

    Glib::ustring title = is_source ? _("Input (Microphone)") : _("Output (Speaker)");
    Glib::ustring none_row_label = is_ambient_itself ? Glib::ustring() : _("None (use Ambient)");

    // Only a device of its own has its own levelling and volume. On the
    // Ambient device (or with none) the controls stay in place but can't
    // be changed, and show the Ambient device's own settings — the ones
    // that apply to this participant then.
    const char* ambient_sentinel = is_source ? kAmbientMicSourceId : kSpeakerSinkId;
    auto suppress = std::make_shared<bool>(false);
    auto* controls = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 12);
    AdwSwitchRow* agc_row = nullptr;
    if (is_source) {
        // In signal order: levelling first, then the volume on top of it.
        auto* agc_list = Gtk::make_managed<Gtk::ListBox>();
        agc_list->add_css_class("boxed-list");
        agc_list->set_selection_mode(Gtk::SelectionMode::NONE);
        agc_row = ADW_SWITCH_ROW(adw_switch_row_new());
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(agc_row), _("Voice Levelling"));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(agc_row),
                                    _("Slowly evens out how loud this microphone sounds"));
        auto on_agc_toggled = new std::function<void(bool)>(
            [id, settings, routing_engine, suppress](bool enabled) {
                if (*suppress) {
                    return;
                }
                routing_engine->set_agc_enabled(id, enabled);
                set_source_agc(settings, id, enabled);
            });
        g_signal_connect_data(
            agc_row, "notify::active",
            G_CALLBACK(+[](GObject* row, GParamSpec*, gpointer data) {
                (*static_cast<std::function<void(bool)>*>(data))(
                    adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
            }),
            on_agc_toggled,
            +[](gpointer data, GClosure*) { delete static_cast<std::function<void(bool)>*>(data); },
            GConnectFlags(0));
        agc_list->append(*Glib::wrap(GTK_WIDGET(agc_row), false));
        controls->append(*agc_list);
    }
    // Each slider with how to read its current value for an id, so the
    // view can show either this participant's or the Ambient device's.
    std::vector<std::pair<Gtk::Scale*, std::function<double(const std::string&)>>> sliders;
    if (is_source) {
        Gtk::Scale* scale = nullptr;
        controls->append(*make_volume_footer(
            _("Voice Volume"), 0.0,
            [state, id, settings, routing_engine](double db) {
                routing_engine->set_source_volume_db(*state, id, db);
                set_source_volume(settings, id, db);
            },
            suppress, &scale));
        sliders.emplace_back(scale, [routing_engine](const std::string& shown) {
            return routing_engine->source_volume_db(shown);
        });
    } else {
        // The synthesized sound first: it's what the listener is there for.
        Gtk::Scale* synthesized = nullptr;
        controls->append(*make_volume_footer(
            _("Synthesized Volume"), 0.0,
            [state, id, settings, routing_engine](double db) {
                routing_engine->set_synthesized_volume_db(*state, id, db);
                set_synthesized_volume(settings, id, db);
            },
            suppress, &synthesized));
        sliders.emplace_back(synthesized, [routing_engine](const std::string& shown) {
            return routing_engine->synthesized_volume_db(shown);
        });
        Gtk::Scale* participants = nullptr;
        controls->append(*make_volume_footer(
            _("Participants Volume"), 0.0,
            [state, id, settings, routing_engine](double db) {
                routing_engine->set_participants_volume_db(*state, id, db);
                set_participants_volume(settings, id, db);
            },
            suppress, &participants));
        sliders.emplace_back(participants, [routing_engine](const std::string& shown) {
            return routing_engine->participants_volume_db(shown);
        });
    }

    // The dialog stays open across picks, so after every pick — or a
    // cancelled confirmation — it shows what is actually assigned now.
    auto select_device = std::make_shared<std::function<void(const std::string&)>>();
    // Own references: this can run after the dialog has closed.
    auto ref = [](gpointer object) {
        return std::shared_ptr<GObject>(G_OBJECT(g_object_ref(object)),
                                        [](GObject* o) { g_object_unref(o); });
    };
    auto controls_ref = ref(controls->gobj());
    std::vector<std::pair<std::shared_ptr<GObject>, std::function<double(const std::string&)>>>
        slider_refs;
    for (const auto& [scale, value_for] : sliders) {
        slider_refs.emplace_back(ref(scale->gobj()), value_for);
    }
    auto agc_ref = agc_row ? ref(agc_row) : nullptr;
    auto refresh_view = [state, id, is_source, ambient_sentinel, routing_engine, suppress,
                         controls_ref, slider_refs, agc_ref, select_device]() {
        const auto& pool =
            is_source ? state->source_device_assignment : state->sink_device_assignment;
        auto it = pool.find(id);
        std::string device_id = it != pool.end() ? it->second.device_id : "";
        bool own_device = !device_id.empty() && device_id != ambient_sentinel;
        const std::string& shown = own_device ? id : std::string(ambient_sentinel);
        *suppress = true;
        for (const auto& [scale, value_for] : slider_refs) {
            gtk_range_set_value(GTK_RANGE(scale.get()), value_for(shown));
        }
        if (agc_ref) {
            adw_switch_row_set_active(ADW_SWITCH_ROW(agc_ref.get()),
                                      routing_engine->agc_enabled(shown));
        }
        *suppress = false;
        gtk_widget_set_sensitive(GTK_WIDGET(controls_ref.get()), own_device);
        if (*select_device) {
            (*select_device)(own_device ? device_id : "");
        }
    };
    refresh_view();
    notify_changed = [notify_changed, refresh_view]() {
        notify_changed();
        refresh_view();
    };

    std::function<void(std::vector<AudioDeviceInfo>&)> annotate_devices;
    if (!is_ambient_itself) {
        // The ambient entry's own device (if it has one) is already
        // reachable via "None (use Ambient)" — listing it again as a
        // normal row too would be the same physical device shown twice
        // under two different pickable entries.
        const char* ambient_sentinel = is_source ? kAmbientMicSourceId : kSpeakerSinkId;
        const std::map<std::string, DeviceAssignment>& ambient_pool =
            is_source ? state->source_device_assignment : state->sink_device_assignment;
        auto ambient_it = ambient_pool.find(ambient_sentinel);
        std::string ambient_device_id =
            ambient_it != ambient_pool.end() ? ambient_it->second.device_id : "";

        // Same bigram scheme as the matrix's own header buttons
        // (build_monitoring_matrix_content()) — reassigned here from
        // participant_order rather than shared, since this dialog has no
        // access to that refresh's local map.
        std::vector<std::string> bigrams =
            assign_bigrams(state->participant_order, get_preferred_bigrams(settings));
        std::map<std::string, std::string> bigram_for_name;
        for (std::size_t i = 0; i < state->participant_order.size(); ++i) {
            bigram_for_name[state->participant_order[i]] = bigrams[i];
        }

        annotate_devices = [&pool, id, ambient_device_id,
                             bigram_for_name](std::vector<AudioDeviceInfo>& devices) {
            if (!ambient_device_id.empty()) {
                devices.erase(std::remove_if(devices.begin(), devices.end(),
                                              [&ambient_device_id](const AudioDeviceInfo& device) {
                                                  return device.id == ambient_device_id;
                                              }),
                              devices.end());
            }
            for (auto& device : devices) {
                std::string owner = find_participant_owner(pool, id, device.id);
                if (owner.empty()) {
                    continue;
                }
                auto bigram_it = bigram_for_name.find(owner);
                device.owner_bigram =
                    bigram_it != bigram_for_name.end() ? bigram_it->second : owner;
            }
        };
    }

    *select_device = show_device_picker_dialog(
        parent, title, none_row_label, gst_device_class, current_device_id, annotate_devices,
        [parent, state, id, gst_device_class, is_source, is_ambient_itself, notify_changed,
         refresh_view](const AudioDeviceInfo* device, std::function<void()>) {
            std::map<std::string, DeviceAssignment>& pool =
                is_source ? state->source_device_assignment : state->sink_device_assignment;
            if (!device) {
                if (is_ambient_itself) {
                    // No "None" row exists for Ambient Speaker/
                    // Microphone's own picker — only reachable here if
                    // the device list was empty and nothing got
                    // selected at all.
                    pool.erase(id);
                } else {
                    // "None (use Ambient)" is a deliberate choice, not
                    // "never configured" — John: the header button
                    // should go active either way. Stored as the
                    // matching ambient sentinel's own id/label rather
                    // than erased, so it reads distinctly from an
                    // untouched entry while never colliding with a real
                    // enumerated device's id.
                    const char* ambient_sentinel = is_source ? kAmbientMicSourceId : kSpeakerSinkId;
                    pool[id] = DeviceAssignment{ambient_sentinel, label_for_id(ambient_sentinel)};
                }
                notify_changed();
                return;
            }
            if (is_ambient_itself) {
                pool[id] = DeviceAssignment{device->id, device->display_name};
                notify_changed();
                return;
            }

            // Copied by value — the confirmation dialog this leads into
            // is async, and `device` points into the picker's own device
            // list, which is gone once the picker is closed.
            AudioDeviceInfo picked = *device;
            auto after_committed = [parent, state, id, gst_device_class, is_source, picked,
                                     notify_changed]() {
                notify_changed();

                AudioDeviceInfo counterpart = find_counterpart_device(picked, gst_device_class);
                if (counterpart.id.empty()) {
                    return;
                }
                bool other_is_source = !is_source;
                std::map<std::string, DeviceAssignment>& other_pool =
                    other_is_source ? state->source_device_assignment
                                     : state->sink_device_assignment;
                auto other_existing = other_pool.find(id);
                if (other_existing != other_pool.end() &&
                    other_existing->second.device_id == counterpart.id) {
                    return;  // already assigned to the same device, nothing to offer
                }
                offer_counterpart_assignment(parent, state, id, other_is_source, counterpart,
                                              notify_changed);
            };
            assign_device_with_confirmation(parent, state, is_source, id, picked,
                                             after_committed, refresh_view);
        },
        controls);
}

// Wires up a header button's click behavior. A single click opens the
// device dialog via `open_dialog` — but only after waiting out the
// system's own double-click window, so the first click of an actual
// double-click never flashes it open first (which would also swallow
// the second click, since the dialog is modal). A double click instead
// toggles a hard mute override on the whole row/column this header
// represents — John: some participants can suddenly panic hearing their
// own voice, so muting them needs to not depend on finding the right
// cell(s) in the matrix first — see MonitoringState's own comment on
// muted_sink_override/muted_source_override. `is_source` says which of
// the two override sets this header's axis maps to; `id` is the sink/
// source id itself (participant name or sentinel).
//
// Detected by hand off plain ::clicked (two clicks within the system's
// double-click time, via Gtk::Settings — John: "Sentralt skal det være
// en doubleclick time. Bruk denne," the same single value both decides
// "was that a double-click" and sets the deferred-open delay), not via
// a separate Gtk::GestureClick's own n_press count — that came back
// unreliable here, layered onto a Gtk::Button's own internal click
// gesture on the same widget.
//
// `on_override_toggled` updates only this button's own visual (e.g. the
// icon swap make_icon_header_button wires up below) — deliberately NOT
// the matrix's own notify_changed()/grid rebuild. John: this is an
// override, the matrix itself doesn't need to change at all, and
// toggling it back off has to leave the matrix exactly as it already
// was — which a full rebuild driven by is_muted() could never guarantee
// if the override ever fed into that function. `routing_engine` is
// synced separately right here, regardless — the override has to
// affect real audio immediately even though it skips the UI rebuild.
void wire_header_button_click(Gtk::Button* button, MonitoringState* state, bool is_source,
                               const std::string& id, std::function<void()> open_dialog,
                               std::function<void()> on_override_toggled,
                               AudioRoutingEngine* routing_engine) {
    auto pending_open = std::make_shared<sigc::connection>();
    auto last_click_ms = std::make_shared<gint64>(0);
    button->signal_clicked().connect([state, is_source, id, open_dialog, on_override_toggled,
                                       routing_engine, pending_open, last_click_ms]() {
        pending_open->disconnect();
        constexpr int kFallbackMs = 400;
        auto gtk_settings = Gtk::Settings::get_default();
        int delay_ms =
            gtk_settings ? gtk_settings->property_gtk_double_click_time().get_value() : 0;
        if (delay_ms <= 0) {
            delay_ms = kFallbackMs;
        }
        gint64 now_ms = g_get_monotonic_time() / 1000;
        bool is_double_click = *last_click_ms != 0 && (now_ms - *last_click_ms) <= delay_ms;
        *last_click_ms = is_double_click ? 0 : now_ms;
        if (is_double_click) {
            std::set<std::string>& overrides =
                is_source ? state->muted_source_override : state->muted_sink_override;
            if (overrides.count(id) != 0) {
                overrides.erase(id);
            } else {
                overrides.insert(id);
            }
            on_override_toggled();
            routing_engine->sync(*state);
            return;
        }
        *pending_open = Glib::signal_timeout().connect(
            [open_dialog]() {
                open_dialog();
                return false;  // one-shot
            },
            delay_ms);
    });
}

// "<name> — <state>" for a header button: whether this axis has a device,
// whether it's present right now, and whether the axis is muted by its
// override. A disconnected device is reported ahead of the mute state,
// since reconnecting it is what the user has to act on.
Glib::ustring header_tooltip(const Glib::ustring& name, const MonitoringState& state,
                             const std::string& id, bool is_source,
                             const AudioRoutingEngine& routing_engine) {
    const auto& pool = is_source ? state.source_device_assignment : state.sink_device_assignment;
    const char* ambient_sentinel = is_source ? kAmbientMicSourceId : kSpeakerSinkId;
    auto it = pool.find(id);
    bool assigned = it != pool.end() && !it->second.device_id.empty();
    if (assigned && it->second.device_id == ambient_sentinel) {
        auto ambient_it = pool.find(ambient_sentinel);
        assigned = ambient_it != pool.end() && !ambient_it->second.device_id.empty();
    }
    const auto& overrides = is_source ? state.muted_source_override : state.muted_sink_override;
    Glib::ustring status = !assigned ? _("No device assigned")
                           : routing_engine.is_disconnected(state, id, !is_source)
                               ? _("Assigned device is disconnected")
                           : overrides.count(id) != 0 ? _("Device is muted")
                                                      : _("Device is unmuted");
    return Glib::ustring::compose(_("%1 — %2"), name, status);
}

// A row/column header for one participant — a two-letter button that
// doubles as direct navigation to that participant's info dialog.
// `gst_device_class` says which axis this instance represents (see
// show_participant_device_dialog()'s own comment) — the same
// participant gets a different button instance (and a different device
// pool) on each axis.
Gtk::Button* make_participant_header_button(Gtk::Window* parent, MonitoringState* state,
                                             const std::string& participant_id,
                                             const std::string& gst_device_class,
                                             const Glib::ustring& bigram,
                                             std::shared_ptr<Gtk::Popover*> popover_ptr,
                                             const Glib::RefPtr<Gio::Settings>& settings,
                                             AudioRoutingEngine* routing_engine,
                                             std::function<void()> notify_changed) {
    auto* button = Gtk::make_managed<Gtk::Button>();
    bool is_source = gst_device_class == "Audio/Source";

    // Backslash (no-symbolic.svg) laid over the bigram text, shown only
    // while this axis's mute override is active — same Overlay/
    // visibility idiom as make_toggle_cell_icon()'s checkbox+checkmark,
    // just over a Label instead of a background icon. The filename must
    // end in "-symbolic" (not just "no-symbol.svg", which is what this
    // was first named) — that's the suffix GTK's icon loading actually
    // keys its CSS-driven recoloring on, independent of the SVG's own
    // internal url(#gpa:foreground) paint-server reference, which is
    // only for the GNOME Symbolic Preview authoring tool. Without the
    // right suffix the icon stays rendered in its embedded fixed black
    // stroke, never picking up the button's own `color` (accent when
    // blind-sight-matrix-device-assigned is set, plain foreground
    // otherwise) the way every other -symbolic icon in this set does.
    auto* overlay = Gtk::make_managed<Gtk::Overlay>();
    auto* label = Gtk::make_managed<Gtk::Label>(bigram);
    overlay->set_child(*label);
    auto* mute_mark = make_icon_image("no-symbolic.svg");
    overlay->add_overlay(*mute_mark);
    button->set_child(*overlay);
    if (routing_engine->is_disconnected(*state, participant_id, !is_source)) {
        mark_disconnected(overlay, label);
    }

    auto update_mute_mark = [button, mute_mark, state, is_source, participant_id,
                             routing_engine]() {
        std::set<std::string>& overrides =
            is_source ? state->muted_source_override : state->muted_sink_override;
        mute_mark->set_visible(overrides.count(participant_id) != 0);
        button->set_tooltip_text(
            header_tooltip(participant_id, *state, participant_id, is_source, *routing_engine));
    };
    update_mute_mark();

    button->set_size_request(kMatrixCellSize, kMatrixCellSize);
    button->add_css_class("flat");
    // Same muted tone/hover/press as the toggle cells, square by
    // default — callers add one of the blind-sight-matrix-corner-*
    // classes for the few header buttons that sit at the matrix's own
    // outer edge.
    button->add_css_class("blind-sight-matrix-cell");
    wire_header_button_click(
        button, state, is_source, participant_id,
        [parent, state, participant_id, gst_device_class, popover_ptr, settings, routing_engine,
         notify_changed]() {
            if (*popover_ptr) {
                (*popover_ptr)->popdown();
            }
            show_participant_device_dialog(parent, state, participant_id, gst_device_class,
                                            settings, routing_engine, notify_changed);
        },
        update_mute_mark, routing_engine);
    return button;
}

// A non-participant header that's still clickable — same
// .flat/.blind-sight-matrix-cell treatment and click-opens-info-dialog
// behavior as a participant header button, but icon-only, reusing
// `device_id` directly as its device-pool key (Speaker, the ambient mic
// column — neither is a participant, but show_participant_device_dialog
// only ever needs an opaque id string). `gst_device_class` is always
// fixed per caller (Speaker: "Audio/Sink", Ambient Mic: "Audio/Source").
// `icon_filename_muted` is shown instead of `icon_filename` whenever
// this header's own mute override is active — swapped in place on the
// same button (not a grid rebuild) both at construction and on every
// double-click toggle, since Speaker/Ambient Mic are the only headers
// with a visual this round (see wire_header_button_click's own comment).
Gtk::Button* make_icon_header_button(Gtk::Window* parent, MonitoringState* state,
                                      const std::string& device_id,
                                      const std::string& gst_device_class,
                                      const std::string& icon_filename,
                                      const std::string& icon_filename_muted,
                                      const Glib::ustring& tooltip,
                                      std::shared_ptr<Gtk::Popover*> popover_ptr,
                                      const Glib::RefPtr<Gio::Settings>& settings,
                                      AudioRoutingEngine* routing_engine,
                                      std::function<void()> notify_changed) {
    auto* button = Gtk::make_managed<Gtk::Button>();
    bool is_source = gst_device_class == "Audio/Source";
    auto* overlay = Gtk::make_managed<Gtk::Overlay>();
    auto* icon = Gtk::make_managed<Gtk::Image>();
    overlay->set_child(*icon);
    button->set_child(*overlay);
    auto update_icon = [button, icon, state, is_source, device_id, icon_filename,
                        icon_filename_muted, tooltip, routing_engine]() {
        std::set<std::string>& overrides =
            is_source ? state->muted_source_override : state->muted_sink_override;
        set_image_icon(icon,
                       overrides.count(device_id) != 0 ? icon_filename_muted : icon_filename);
        button->set_tooltip_text(
            header_tooltip(tooltip, *state, device_id, is_source, *routing_engine));
    };
    update_icon();
    if (routing_engine->is_disconnected(*state, device_id, !is_source)) {
        mark_disconnected(overlay, icon);
    }
    button->set_size_request(kMatrixCellSize, kMatrixCellSize);
    button->add_css_class("flat");
    button->add_css_class("blind-sight-matrix-cell");
    wire_header_button_click(
        button, state, is_source, device_id,
        [parent, state, device_id, gst_device_class, popover_ptr, settings, routing_engine,
         notify_changed]() {
            if (*popover_ptr) {
                (*popover_ptr)->popdown();
            }
            show_participant_device_dialog(parent, state, device_id, gst_device_class, settings,
                                            routing_engine, notify_changed);
        },
        update_icon, routing_engine);
    return button;
}

}  // namespace

void seed_preferred_devices(MonitoringState& state, const Glib::RefPtr<Gio::Settings>& settings) {
    auto preferred_sinks = get_preferred_sink_devices(settings);
    auto preferred_sources = get_preferred_source_devices(settings);
    auto seed_pool = [](std::map<std::string, DeviceAssignment>& pool,
                         const std::map<Glib::ustring, std::pair<Glib::ustring, Glib::ustring>>&
                             preferred,
                         const std::string& id) {
        if (pool.count(id) != 0) {
            return;
        }
        auto it = preferred.find(Glib::ustring(id));
        if (it == preferred.end() || it->second.first.empty()) {
            return;
        }
        pool[id] = DeviceAssignment{it->second.first, it->second.second};
    };
    seed_pool(state.sink_device_assignment, preferred_sinks, kSpeakerSinkId);
    seed_pool(state.source_device_assignment, preferred_sources, kAmbientMicSourceId);
    for (const auto& name : state.participant_order) {
        seed_pool(state.sink_device_assignment, preferred_sinks, name);
        seed_pool(state.source_device_assignment, preferred_sources, name);
    }

    // A participant set to the Ambient device itself is on the Ambient
    // device — Preferences has no "use Ambient" choice, so a preset says it
    // this way. Stored as the sentinel, it gets no volume or leveller of
    // its own and follows the Ambient device if that changes. Rechecked on
    // every call, since this runs before every routing sync.
    auto to_ambient = [&state](std::map<std::string, DeviceAssignment>& pool,
                               const char* sentinel) {
        auto ambient = pool.find(sentinel);
        if (ambient == pool.end() || ambient->second.device_id.empty()) {
            return;
        }
        std::string ambient_device = ambient->second.device_id;
        for (const auto& name : state.participant_order) {
            auto it = pool.find(name);
            if (it != pool.end() && it->second.device_id == ambient_device) {
                it->second = DeviceAssignment{sentinel, label_for_id(sentinel)};
            }
        }
    };
    to_ambient(state.sink_device_assignment, kSpeakerSinkId);
    to_ambient(state.source_device_assignment, kAmbientMicSourceId);
}

MonitoringMatrixWidget build_monitoring_matrix_content(
    Gtk::Window* parent, MonitoringState* state, std::function<std::string()> get_session_path,
    std::shared_ptr<Gtk::Popover*> popover_ptr, const Glib::RefPtr<Gio::Settings>& settings,
    AudioRoutingEngine* routing_engine) {
    auto* grid = Gtk::make_managed<Gtk::Grid>();
    // Same spacing as doc_group's own linked header-bar button group
    // (main.cpp) — cells read as a related group without touching.
    grid->set_row_spacing(4);
    grid->set_column_spacing(4);
    grid->set_margin(8);

    auto* grid_scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    grid_scroller->set_policy(Gtk::PolicyType::AUTOMATIC, Gtk::PolicyType::AUTOMATIC);
    grid_scroller->set_max_content_width(480);
    grid_scroller->set_max_content_height(360);
    grid_scroller->set_propagate_natural_width(true);
    grid_scroller->set_propagate_natural_height(true);
    grid_scroller->set_child(*grid);

    // A plain std::function can't capture itself by value while it's
    // still being constructed — the participant header buttons built
    // inside this closure need to call it again later (Save in their
    // own info dialog), so it's wrapped in a shared_ptr first and filled
    // in afterward, same forward-reference idiom this file's own
    // popover_ptr already uses. The resulting self-reference cycle leaks
    // (refresh_ptr's own function holds a shared_ptr to refresh_ptr),
    // which is fine — this popover's content is a singleton built once
    // and never destroyed for the app's whole run, same as every other
    // permanent popover in main.cpp.
    auto refresh_ptr = std::make_shared<std::function<void()>>();
    *refresh_ptr = [grid, state, get_session_path, parent, popover_ptr, settings, routing_engine,
                     refresh_ptr]() {
        RecordMeta meta;
        read_record_meta(get_session_path(), meta);
        reconcile_monitoring_state(*state, participant_names(meta));
        seed_preferred_devices(*state, settings);
        routing_engine->sync(*state);

        while (auto* child = grid->get_first_child()) {
            grid->remove(*child);
        }

        // Speaker/Ambient Mic go first on their own axis, right next to
        // the blank corner cell — John: the leftmost column reads as
        // the row headers, the top row reads as the column headers, so
        // putting each axis's own non-participant entry immediately
        // next to that corner (rather than trailing at the far end)
        // makes which strip is which unambiguous, symmetric on both
        // axes. Synthesized stays last on the column axis — it's purely
        // informational, not something to lead with.
        std::vector<std::string> sink_ids = {kSpeakerSinkId};
        sink_ids.insert(sink_ids.end(), state->participant_order.begin(),
                         state->participant_order.end());
        std::vector<std::string> source_ids = {kAmbientMicSourceId};
        source_ids.insert(source_ids.end(), state->participant_order.begin(),
                           state->participant_order.end());
        source_ids.push_back(kSynthesizedSourceId);
        // Bug fixed live (crashed — out-of-bounds vector access): bigrams
        // is indexed by position in participant_order, not by position in
        // sink_ids/source_ids — those two no longer agree now that
        // Speaker/Ambient Mic sit at the front instead of the back, so
        // looking a bigram up by raw row/col index was wrong (and, for
        // the last participant, out of bounds outright). Looked up by
        // name instead, which is correct regardless of either list's
        // ordering.
        std::vector<std::string> bigrams =
            assign_bigrams(state->participant_order, get_preferred_bigrams(settings));
        std::map<std::string, std::string> bigram_for_name;
        for (std::size_t i = 0; i < state->participant_order.size(); ++i) {
            bigram_for_name[state->participant_order[i]] = bigrams[i];
        }
        std::function<void()> notify_changed = [refresh_ptr]() { (*refresh_ptr)(); };
        // The top row's last clickable column — now the last
        // participant, since Ambient Mic moved to the front — gets the
        // frame's top-right corner; Synthesized stays a plain icon past
        // that frame. Purely index-based (first/last position), so this
        // same formula already works regardless of which id sits where.
        std::size_t last_clickable_col = state->participant_order.size();

        // Corner cell, blank.
        grid->attach(*Gtk::make_managed<Gtk::Label>(), 0, 0);

        for (std::size_t col = 0; col < source_ids.size(); ++col) {
            // Synthesized gets a plain, upright icon instead of a
            // header button — it's purely informational.
            if (source_ids[col] == kSynthesizedSourceId) {
                auto* icon = make_icon_image("sound-wave-symbolic.svg");
                icon->set_size_request(kMatrixCellSize, kMatrixCellSize);
                icon->set_halign(Gtk::Align::CENTER);
                icon->set_valign(Gtk::Align::CENTER);
                icon->set_tooltip_text(label_for_id(kSynthesizedSourceId));
                grid->attach(*icon, static_cast<int>(col) + 1, 0);
                continue;
            }
            Gtk::Button* header_button =
                source_ids[col] == kAmbientMicSourceId
                    ? make_icon_header_button(parent, state, kAmbientMicSourceId, "Audio/Source",
                                               "mic-symbolic.svg", "mic-no-symbolic.svg",
                                               label_for_id(kAmbientMicSourceId), popover_ptr,
                                               settings, routing_engine, notify_changed)
                    : make_participant_header_button(parent, state, source_ids[col], "Audio/Source",
                                                      bigram_for_name[source_ids[col]], popover_ptr,
                                                      settings, routing_engine, notify_changed);
            if (state->source_device_assignment.count(source_ids[col]) != 0) {
                header_button->add_css_class("blind-sight-matrix-device-assigned");
            }
            // Top row: only the leftmost (top-left) and the last
            // clickable column (top-right) round, framing the header
            // row — everything else square.
            if (col == 0) {
                header_button->add_css_class("blind-sight-matrix-corner-top-left");
            } else if (col == last_clickable_col) {
                header_button->add_css_class("blind-sight-matrix-corner-top-right");
            }
            header_button->set_valign(Gtk::Align::END);
            grid->attach(*header_button, static_cast<int>(col) + 1, 0);
        }
        for (std::size_t row = 0; row < sink_ids.size(); ++row) {
            // Speaker is a button too (it opens its own device dialog,
            // same as a participant), just icon-only since it isn't a
            // participant — it's always this column's true top entry
            // (see sink_ids' own construction above), so the corner
            // logic below needs no special case for it.
            Gtk::Button* header_button =
                sink_ids[row] == kSpeakerSinkId
                    ? make_icon_header_button(parent, state, kSpeakerSinkId, "Audio/Sink",
                                               "speaker-symbolic.svg", "speaker-no-symbolic.svg",
                                               label_for_id(kSpeakerSinkId), popover_ptr, settings,
                                               routing_engine, notify_changed)
                    : make_participant_header_button(parent, state, sink_ids[row], "Audio/Sink",
                                                      bigram_for_name[sink_ids[row]], popover_ptr,
                                                      settings, routing_engine, notify_changed);
            if (state->sink_device_assignment.count(sink_ids[row]) != 0) {
                header_button->add_css_class("blind-sight-matrix-device-assigned");
            }
            // Left column: only the topmost (top-left) and bottommost
            // (bottom-left) corners round, framing the header column —
            // everything else square.
            if (row == 0) {
                header_button->add_css_class("blind-sight-matrix-corner-top-left");
            } else if (row + 1 == sink_ids.size()) {
                header_button->add_css_class("blind-sight-matrix-corner-bottom-left");
            }
            header_button->set_halign(Gtk::Align::END);
            grid->attach(*header_button, 0, static_cast<int>(row) + 1);
        }

        for (std::size_t row = 0; row < sink_ids.size(); ++row) {
            for (std::size_t col = 0; col < source_ids.size(); ++col) {
                const std::string sink_id = sink_ids[row];
                const std::string source_id = source_ids[col];
                auto* toggle = Gtk::make_managed<Gtk::ToggleButton>();
                toggle->set_size_request(kMatrixCellSize, kMatrixCellSize);
                toggle->add_css_class("flat");
                toggle->add_css_class("blind-sight-matrix-cell");
                // Square everywhere except the toggle grid's top-right/
                // bottom-left/bottom-right corners — top-left stays
                // square too, deliberately, per John.
                if (row == 0 && col + 1 == source_ids.size()) {
                    toggle->add_css_class("blind-sight-matrix-corner-top-right");
                } else if (row + 1 == sink_ids.size() && col == 0) {
                    toggle->add_css_class("blind-sight-matrix-corner-bottom-left");
                } else if (row + 1 == sink_ids.size() && col + 1 == source_ids.size()) {
                    toggle->add_css_class("blind-sight-matrix-corner-bottom-right");
                }
                bool muted = is_muted(*state, sink_id, source_id);
                toggle->set_active(muted);
                ToggleCellIcon cell_icon = make_toggle_cell_icon(muted);
                toggle->set_child(*cell_icon.widget);
                toggle->add_css_class("image-button");
                toggle->set_tooltip_text(Glib::ustring::compose(
                    _("Whether %1 hears %2"), label_for_id(sink_id), label_for_id(source_id)));
                toggle->signal_toggled().connect(
                    [toggle, state, sink_id, source_id, routing_engine,
                     checkmark = cell_icon.checkmark]() {
                        bool now_muted = toggle->get_active();
                        set_muted(*state, sink_id, source_id, now_muted);
                        checkmark->set_visible(!now_muted);
                        routing_engine->sync(*state);
                    });
                grid->attach(*toggle, static_cast<int>(col) + 1, static_cast<int>(row) + 1);
            }
        }
    };

    return {grid_scroller, *refresh_ptr};
}
