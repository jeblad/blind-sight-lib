// SPDX-License-Identifier: GPL-3.0-only
//
// transcription.cpp — see transcription.hpp for scope.

#include "transcription.hpp"

#include <adwaita.h>
#include <glib/gi18n.h>
#include <gtkmm.h>
#include <libsecret/secret.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>

#include "audio.hpp"
#include "recording.hpp"
#include "tracks.hpp"

using blind_sight::RecordMeta;
using blind_sight::read_record_meta;
using blind_sight::read_tracks_index;

// Bridges into main.cpp's anonymous namespace — these few things stay
// defined in main.cpp but are needed here too.
void bridge_show_error_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                const Glib::ustring& body);
void bridge_show_setup_required_dialog(Gtk::Window* parent, const Glib::ustring& heading,
                                         const Glib::ustring& body,
                                         const Glib::ustring& command);

namespace {

constexpr auto kAppId = "io.github.jeblad.blind-sight-demo";
constexpr auto kTranscriptionModelSizeKey = "transcription-model-size";
constexpr auto kTranscribeAudioKey = "transcribe-audio";
constexpr auto kDiarizeAudioKey = "diarize-audio";

// BLIND_SIGHT_TRANSCRIBE_DIR's venv — kept separate from the source
// tree since it's genuinely per-user, writable, regenerable state.
// Glib::get_user_cache_dir() is the portable way to get $XDG_CACHE_HOME.
std::string transcribe_venv_dir() {
    return Glib::build_filename(Glib::get_user_cache_dir(), kAppId, "transcribe-venv");
}

// BLIND_SIGHT_TRANSCRIBE_DIR (the source tree's demo/transcribe/) only
// holds the *script*, transcribe.py; the venv it runs in is
// transcribe_venv_dir(), not underneath it. Shares its base with
// track_audio_path()/track_audio_opus_path() (via track_audio_base())
// rather than being derived from whichever actually exists, so the
// transcript file's name doesn't depend on whether Opus encoding
// succeeded for that segment.
std::string transcript_file_path_for(const std::string& audio_path) {
    std::filesystem::path p(audio_path);
    return (p.parent_path() / (p.stem().string() + ".vtt")).string();
}

bool has_transcript_file(const std::string& audio_path) {
    if (audio_path.empty()) {
        return false;
    }
    return std::filesystem::exists(transcript_file_path_for(audio_path));
}

// Same "cheap, definitive check" pattern as depth_generation_available().
bool transcription_available_impl() {
    return std::filesystem::exists(transcribe_venv_dir() + "/bin/python3");
}

}  // namespace

bool transcription_available() { return transcription_available_impl(); }

// Whether the fixed transcription model has already been downloaded —
// checked separately from transcription_available(): libraries and
// models are two distinct setup steps, each with its own dialog. Only
// ever a *partial* answer: the word-alignment model transcribe.py also
// downloads is chosen by detected language at transcription time, not
// known in advance, so there's no fixed path to check for it here.
// huggingface_hub's actual default cache root
// (~/.cache/huggingface/hub, not necessarily $XDG_CACHE_HOME) is
// checked directly; an HF_HOME override isn't accounted for.
//
// `model_size` — "Systran/faster-whisper-<size>" is faster_whisper's
// fixed naming convention (confirmed against its installed utils.py
// `_MODELS` dict).
bool whisper_model_cached(const std::string& model_size) {
    return std::filesystem::exists(Glib::get_home_dir() +
                                    "/.cache/huggingface/hub/"
                                    "models--Systran--faster-whisper-" +
                                    model_size);
}

// Same "cheap, definitive check" pattern as whisper_model_cached().
// Without this, turning "Diarize Recordings" on always re-runs the
// (successful) prefetch subprocess, which on success calls
// adw_switch_row_set_active(row, TRUE) — which re-emits that row's own
// "notify::active", re-entering the toggle handler as if the user had
// clicked it again, launching *another* prefetch, forever. Checking
// this first and skipping the whole prefetch when already cached
// breaks the cycle.
//
// Three separate gated repos, since pyannote/speaker-diarization-3.1's
// pipeline pulls all three in (the third, speaker-diarization-
// community-1, was only found by actually running this). A non-gated
// fourth model also gets cached alongside these but isn't checked here
// — it downloads without a token regardless.
bool diarization_model_cached() {
    std::string hub_dir = Glib::get_home_dir() + "/.cache/huggingface/hub/";
    return std::filesystem::exists(hub_dir + "models--pyannote--segmentation-3.0") &&
           std::filesystem::exists(
               hub_dir + "models--pyannote--speaker-diarization-3.1") &&
           std::filesystem::exists(
               hub_dir + "models--pyannote--speaker-diarization-community-1");
}

namespace {

// The attributes a matching secret-tool invocation must use — arbitrary
// names, just needing to match on both ends.
//
// SECRET_SCHEMA_DONT_MATCH_NAME, not SECRET_SCHEMA_NONE: without this
// flag, secret_password_lookup_sync() also implicitly requires the
// stored item to be tagged with this schema's name (an `xdg:schema`
// attribute libsecret adds automatically for schema-aware lookups).
// `secret-tool store` never uses a schema — it just sets the raw
// attributes given on the command line — so an item it creates never
// gets that tag, and a name-matching lookup would silently find
// nothing even though the attributes match exactly. This flag matches
// on attributes only.
const SecretSchema* huggingface_token_schema() {
    static const SecretSchema schema = {
        "io.github.jeblad.blind-sight-demo.HuggingFaceToken",
        SECRET_SCHEMA_DONT_MATCH_NAME,
        {
            {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
            {"key", SECRET_SCHEMA_ATTRIBUTE_STRING},
        }};
    return &schema;
}

}  // namespace

// Reads the HuggingFace access token diarization needs, from GNOME
// Keyring — never written by this app, only read. std::nullopt on any
// lookup error, same as not found — callers treat both the same way.
// Synchronous — only called right before launching the (already-async)
// diarization subprocess, or from a Preferences toggle's own
// synchronous check, neither worth an async version for one fast local
// D-Bus round-trip.
std::optional<std::string> lookup_huggingface_token() {
    GError* error = nullptr;
    char* password = secret_password_lookup_sync(
        huggingface_token_schema(), nullptr, &error, "service",
        "blind-sight-lib", "key", "hf-token", nullptr);
    if (error) {
        g_message("blind-sight: HuggingFace token lookup failed: %s",
                   error->message);
        g_error_free(error);
        return std::nullopt;
    }
    if (!password) {
        return std::nullopt;
    }
    std::string token(password);
    secret_password_free(password);
    return token;
}

bool huggingface_token_available() { return lookup_huggingface_token().has_value(); }

namespace {

// A guess, not a certainty — used only to pre-fill the language argument
// for prefetch_models.py's download command, never to gate anything.
// g_get_language_names() returns the OS/desktop's prioritized locale
// list (e.g. "nb_NO.UTF-8", "nb_NO", "nb", "no", …, down to "C"); this
// takes the first 2-letter-looking prefix. Norwegian is special-cased
// to "no" — Whisper's language codes don't distinguish Bokmål/Nynorsk
// the way glibc locales do ("nb"/"nn"); a Norwegian Bokmål recording is
// auto-detected by WhisperX as language "no", not "nb". Falls back to
// "en" if nothing usable is found.
std::string whisper_language_code_for_ui_locale() {
    for (const gchar* const* names = g_get_language_names(); *names != nullptr; ++names) {
        std::string name = *names;
        if (name.size() < 2 || name == "C" || name == "POSIX") {
            continue;
        }
        std::string code = name.substr(0, 2);
        if (!std::isalpha(static_cast<unsigned char>(code[0])) ||
            !std::isalpha(static_cast<unsigned char>(code[1]))) {
            continue;
        }
        for (auto& c : code) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (code == "nb" || code == "nn") {
            return "no";
        }
        return code;
    }
    return "en";
}

// Every language Whisper itself can transcribe — 100, not hand-typed:
// generated straight from the installed venv's whisperx/utils.py
// LANGUAGES dict. Static and fixed — this is whisper's own table,
// unrelated to what's actually installed on this machine (see
// kAlignableLanguages in transcription.hpp for that). No Sámi language
// of any kind is in this list.
struct WhisperLanguage {
    const char* code;
    const char* name;  // English name, as whisperx itself labels it
};
constexpr WhisperLanguage kWhisperLanguages[] = {
    {"en", "english"},        {"zh", "chinese"},      {"de", "german"},
    {"es", "spanish"},        {"ru", "russian"},      {"ko", "korean"},
    {"fr", "french"},         {"ja", "japanese"},     {"pt", "portuguese"},
    {"tr", "turkish"},        {"pl", "polish"},       {"ca", "catalan"},
    {"nl", "dutch"},          {"ar", "arabic"},       {"sv", "swedish"},
    {"it", "italian"},        {"id", "indonesian"},   {"hi", "hindi"},
    {"fi", "finnish"},        {"vi", "vietnamese"},   {"he", "hebrew"},
    {"uk", "ukrainian"},      {"el", "greek"},        {"ms", "malay"},
    {"cs", "czech"},          {"ro", "romanian"},     {"da", "danish"},
    {"hu", "hungarian"},      {"ta", "tamil"},        {"no", "norwegian"},
    {"th", "thai"},           {"ur", "urdu"},         {"hr", "croatian"},
    {"bg", "bulgarian"},      {"lt", "lithuanian"},   {"la", "latin"},
    {"mi", "maori"},          {"ml", "malayalam"},    {"cy", "welsh"},
    {"sk", "slovak"},         {"te", "telugu"},       {"fa", "persian"},
    {"lv", "latvian"},        {"bn", "bengali"},      {"sr", "serbian"},
    {"az", "azerbaijani"},    {"sl", "slovenian"},    {"kn", "kannada"},
    {"et", "estonian"},       {"mk", "macedonian"},   {"br", "breton"},
    {"eu", "basque"},         {"is", "icelandic"},    {"hy", "armenian"},
    {"ne", "nepali"},         {"mn", "mongolian"},    {"bs", "bosnian"},
    {"kk", "kazakh"},         {"sq", "albanian"},     {"sw", "swahili"},
    {"gl", "galician"},       {"mr", "marathi"},      {"pa", "punjabi"},
    {"si", "sinhala"},        {"km", "khmer"},        {"sn", "shona"},
    {"yo", "yoruba"},         {"so", "somali"},       {"af", "afrikaans"},
    {"oc", "occitan"},        {"ka", "georgian"},     {"be", "belarusian"},
    {"tg", "tajik"},          {"sd", "sindhi"},       {"gu", "gujarati"},
    {"am", "amharic"},        {"yi", "yiddish"},      {"lo", "lao"},
    {"uz", "uzbek"},          {"fo", "faroese"},      {"ht", "haitian creole"},
    {"ps", "pashto"},         {"tk", "turkmen"},      {"nn", "nynorsk"},
    {"mt", "maltese"},        {"sa", "sanskrit"},     {"lb", "luxembourgish"},
    {"my", "myanmar"},        {"bo", "tibetan"},      {"tl", "tagalog"},
    {"mg", "malagasy"},       {"as", "assamese"},     {"tt", "tatar"},
    {"haw", "hawaiian"},      {"ln", "lingala"},      {"ha", "hausa"},
    {"ba", "bashkir"},        {"jw", "javanese"},     {"su", "sundanese"},
    {"yue", "cantonese"},
};

}  // namespace

// English display name for a whisper language code — falls back to the
// code itself if somehow not found (shouldn't happen, kWhisperLanguages
// is exhaustive).
const char* whisper_language_name(const std::string& code) {
    for (const auto& lang : kWhisperLanguages) {
        if (code == lang.code) {
            return lang.name;
        }
    }
    return code.c_str();
}

// nullptr if this language has no alignment model at all (most of the
// 100 don't — see kAlignableLanguages in transcription.hpp).
const AlignmentModelInfo* find_alignment_model(const std::string& code) {
    for (const auto& info : kAlignableLanguages) {
        if (code == info.code) {
            return &info;
        }
    }
    return nullptr;
}

namespace {

// "org/repo" -> "models--org--repo", the HuggingFace Hub cache directory
// naming convention (same one diarization_model_cached() already relies
// on for pyannote's models).
std::string huggingface_cache_dirname(const std::string& repo_id) {
    std::string result = "models--";
    for (char c : repo_id) {
        result += (c == '/') ? std::string("--") : std::string(1, c);
    }
    return result;
}

}  // namespace

// Whether a given language's alignment model is actually downloaded on
// this machine, checked directly against each backend's own real cache
// location — not anything this app tracks itself. Cheap, safe to call
// live every time Preferences opens.
bool language_alignment_model_cached(const AlignmentModelInfo& info) {
    if (info.backend == AlignmentBackend::kTorch) {
        return std::filesystem::exists(Glib::get_home_dir() +
                                        "/.cache/torch/hub/checkpoints/" + info.id);
    }
    return std::filesystem::exists(Glib::get_home_dir() +
                                    "/.cache/huggingface/hub/" +
                                    huggingface_cache_dirname(info.id));
}

// Second Dismiss/Next dialog, offered once libraries are confirmed
// ready but the fixed transcription model isn't cached yet. One
// button, "Ok" — the check is implicit in it: the user may leave this
// dialog open while running the copyable command in a terminal, so
// "Ok" re-checks whisper_model_cached() right then rather than
// trusting whatever was true when the dialog first opened. Found ->
// kTranscribeAudioKey true. Still not found -> the dialog just closes
// and kTranscribeAudioKey goes back to false.
void show_transcription_model_setup_offer(Gtk::Window* parent,
                                           const Glib::RefPtr<Gio::Settings>& settings,
                                           AdwSwitchRow* row) {
    std::string language = whisper_language_code_for_ui_locale();
    std::string model_size = settings->get_string(kTranscriptionModelSizeKey).raw();
    std::string command = transcribe_venv_dir() + "/bin/python3 " +
                           std::string(BLIND_SIGHT_TRANSCRIBE_DIR) +
                           "/prefetch_models.py " + language +
                           " --model-size " + model_size;
    auto* dialog = adw_alert_dialog_new(
        _("Transcription Model Not Downloaded Yet"),
        _("Run this to download the transcription model, plus the "
          "alignment model for the language this interface appears to "
          "be running in (guessed from your system's language setting — "
          "edit the language code in the command if that's wrong). Safe "
          "to run well ahead of time, even on a different machine or "
          "network than the one you'll actually test on — these can be "
          "several GB."));
    auto* command_label = Gtk::make_managed<Gtk::Label>(command);
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

    // `row` (nullable — the async post-install chain in
    // install_transcription_libraries_and_continue() has none to give)
    // is held via a GWeakRef: this dialog can sit open for as long as
    // the user takes to run the command in a terminal, long enough
    // that closing Preferences meanwhile is real.
    struct ModelDialogContext {
        Glib::RefPtr<Gio::Settings> settings;
        GWeakRef row_ref;
    };
    auto* ctx = new ModelDialogContext{settings, {}};
    g_weak_ref_init(&ctx->row_ref, row);
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
            auto* ctx = static_cast<ModelDialogContext*>(data);
            bool ready = whisper_model_cached(
                ctx->settings->get_string(kTranscriptionModelSizeKey).raw());
            ctx->settings->set_boolean(kTranscribeAudioKey, ready);
            if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&ctx->row_ref))) {
                adw_switch_row_set_active(ADW_SWITCH_ROW(obj), ready);
                g_object_unref(obj);
            }
        }),
        ctx,
        +[](gpointer data, GClosure*) {
            auto* ctx = static_cast<ModelDialogContext*>(data);
            g_weak_ref_clear(&ctx->row_ref);
            delete ctx;
        },
        GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent->gobj()));
}

// Prefetches the (possibly sizeable) pyannote diarization model right
// now, synchronized with an explicit user action — turning the
// Preferences toggle on, or clicking "Ok" in show_diarization_setup_
// offer() below — rather than lazily on first real test, which would
// otherwise interrupt testing with a large download. `row` is taken as
// a plain pointer — safe to weak-ref from directly here, since both
// call sites invoke this synchronously from a still-live signal
// handler — but the async completion below can genuinely outlive the
// *caller's* context, so this builds its own independently-owned
// context up front.
void start_diarization_model_prefetch(Gtk::Window* parent,
                                       const Glib::RefPtr<Gio::Settings>& settings,
                                       AdwToastOverlay* toast_overlay, AdwSwitchRow* row,
                                       const std::string& hf_token) {
    struct PrefetchContext {
        Gtk::Window* parent;
        Glib::RefPtr<Gio::Settings> settings;
        GWeakRef row_ref;
    };
    auto* pctx = new PrefetchContext();
    pctx->parent = parent;
    pctx->settings = settings;
    g_weak_ref_init(&pctx->row_ref, row);

    AdwToast* toast = adw_toast_new(_("Downloading diarization model…"));
    adw_toast_set_timeout(toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, toast);
    try {
        auto launcher = Gio::SubprocessLauncher::create(Gio::Subprocess::Flags::NONE);
        launcher->setenv("HF_TOKEN", hf_token, true);
        auto proc = launcher->spawn(
            {transcribe_venv_dir() + "/bin/python3",
             std::string(BLIND_SIGHT_TRANSCRIBE_DIR) +
                 "/prefetch_diarization_model.py"});
        proc->wait_async(
            [proc, toast, pctx](Glib::RefPtr<Gio::AsyncResult>& result) {
                proc->wait_finish(result);
                bool ok = proc->get_successful();
                adw_toast_dismiss(toast);
                pctx->settings->set_boolean(kDiarizeAudioKey, ok);
                if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&pctx->row_ref))) {
                    adw_switch_row_set_active(ADW_SWITCH_ROW(obj), ok);
                    g_object_unref(obj);
                }
                if (!ok) {
                    g_message("blind-sight: diarization model prefetch failed");
                    bridge_show_error_dialog(
                        pctx->parent, _("Diarization Setup Failed"),
                        _("Downloading the diarization model failed — "
                          "check that all three gated model licenses were "
                          "accepted and the token is valid, then try "
                          "again."));
                }
                g_weak_ref_clear(&pctx->row_ref);
                delete pctx;
            });
    } catch (const Glib::Error& e) {
        adw_toast_dismiss(toast);
        g_message("blind-sight: failed to launch diarization model prefetch: %s",
                   e.what());
        settings->set_boolean(kDiarizeAudioKey, false);
        if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&pctx->row_ref))) {
            adw_switch_row_set_active(ADW_SWITCH_ROW(obj), FALSE);
            g_object_unref(obj);
        }
        g_weak_ref_clear(&pctx->row_ref);
        delete pctx;
    }
}

// The diarization setup "poster" — numbered steps rather than a single
// command, since this genuinely needs several manual actions on
// huggingface.co that can't be collapsed into one copyable line:
// create an account, accept the license on all three gated model
// pages, create a token, then store it locally. Same "Ok, implicit
// recheck" shape as show_transcription_model_setup_offer() — the user
// may do all of this in a browser/terminal while this dialog stays
// open, so the keyring lookup happens when "Ok" is clicked.
void show_diarization_setup_offer(Gtk::Window* parent,
                                   const Glib::RefPtr<Gio::Settings>& settings,
                                   AdwToastOverlay* toast_overlay, AdwSwitchRow* row) {
    auto* dialog = adw_alert_dialog_new(
        _("Diarization Not Set Up"),
        _("Speaker labeling needs a one-time HuggingFace setup:\n\n"
          "1. Create a free HuggingFace account, if you don't have one "
          "(link below).\n"
          "2. Visit these three pages and accept the license on each — "
          "all three are required (links below).\n"
          "3. Create an access token (link below) — a \"Read\" token, or "
          "a fine-grained token with \"Read access to contents of all "
          "public gated repos you can access\" checked.\n"
          "4. Run this command below. When it asks for a password, that "
          "is where you paste the token — not your account or login "
          "password."));
    // AdwAlertDialog's "body" is plain text, no markup — every link
    // lives in extra_child instead, alongside the copyable command, so
    // they can be real clickable links. Display text omits the scheme
    // for readability, but the href needs it.
    //
    // Three gated model pages, not two: pyannote/speaker-diarization-3.1's
    // pipeline internally pulls a PLDA component from a *third* gated
    // repo, pyannote/speaker-diarization-community-1 — undocumented,
    // only surfaced as a 403/GatedRepoError once the pipeline tries to
    // load.
    auto* extra_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 8);
    auto* links_label = Gtk::make_managed<Gtk::Label>();
    links_label->set_markup(
        "<a href=\"https://huggingface.co/join\">huggingface.co/join</a>\n"
        "<a href=\"https://huggingface.co/pyannote/segmentation-3.0\">"
        "huggingface.co/pyannote/segmentation-3.0</a>\n"
        "<a href=\"https://huggingface.co/pyannote/speaker-diarization-3.1\">"
        "huggingface.co/pyannote/speaker-diarization-3.1</a>\n"
        "<a href=\"https://huggingface.co/pyannote/speaker-diarization-community-1\">"
        "huggingface.co/pyannote/speaker-diarization-community-1</a>\n"
        "<a href=\"https://huggingface.co/settings/tokens\">"
        "huggingface.co/settings/tokens</a>");
    links_label->set_selectable(true);
    links_label->set_halign(Gtk::Align::START);
    extra_box->append(*links_label);
    auto* command_label = Gtk::make_managed<Gtk::Label>(
        "secret-tool store --label=\"blind-sight-lib HuggingFace token\" "
        "service blind-sight-lib key hf-token");
    command_label->set_selectable(true);
    command_label->set_halign(Gtk::Align::START);
    command_label->set_wrap(true);
    command_label->add_css_class("monospace");
    extra_box->append(*command_label);
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog),
                                      GTK_WIDGET(extra_box->gobj()));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", _("Ok"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "ok",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "ok");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "ok");

    // `row` via GWeakRef, same reasoning as the model dialog above — this
    // one can sit open even longer (creating a HuggingFace account from
    // scratch).
    struct DiarizeDialogContext {
        Gtk::Window* parent;
        Glib::RefPtr<Gio::Settings> settings;
        AdwToastOverlay* toast_overlay;
        GWeakRef row_ref;
    };
    auto* dctx = new DiarizeDialogContext();
    dctx->parent = parent;
    dctx->settings = settings;
    dctx->toast_overlay = toast_overlay;
    g_weak_ref_init(&dctx->row_ref, row);
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char*, gpointer data) {
            auto* dctx = static_cast<DiarizeDialogContext*>(data);
            auto token = lookup_huggingface_token();
            auto* obj = static_cast<GObject*>(g_weak_ref_get(&dctx->row_ref));
            auto* row = obj ? ADW_SWITCH_ROW(obj) : nullptr;
            if (!token) {
                dctx->settings->set_boolean(kDiarizeAudioKey, false);
                if (row) {
                    adw_switch_row_set_active(row, FALSE);
                }
            } else {
                start_diarization_model_prefetch(dctx->parent, dctx->settings,
                                                  dctx->toast_overlay, row, *token);
            }
            if (obj) {
                g_object_unref(obj);
            }
        }),
        dctx,
        +[](gpointer data, GClosure*) {
            auto* dctx = static_cast<DiarizeDialogContext*>(data);
            g_weak_ref_clear(&dctx->row_ref);
            delete dctx;
        },
        GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent->gobj()));
}

// Rows in "Transcription Languages" don't carry their own delete
// button — clicking the row reopens the picker filtered to exactly
// this language, its checkbox already checked, so unchecking it and
// clicking Update is how it actually gets removed.
//
// NOT via AdwActionRow's own "activate" signal: that's GTK4's generic
// keybinding-activation mechanism, fired by Enter/Space or an explicit
// adw_action_row_activate() call, never by an ordinary mouse click. A
// listbox click is handled by the containing GtkListBox, but
// AdwPreferencesGroup owns that internal listbox with no public
// accessor to hook the same way. adw_action_row_set_activatable_widget()
// sidesteps this: a row click delegates into activating this specific
// suffix button.
void make_language_row_activatable(GtkWidget* row, Gtk::Window* parent,
                                    AdwToastOverlay* toast_overlay,
                                    const std::shared_ptr<LanguagePreferencesState>& lang_state,
                                    const std::string& language_name) {
    auto* open_button = Gtk::make_managed<Gtk::Button>();
    open_button->set_icon_name("go-next-symbolic");
    open_button->add_css_class("flat");
    open_button->set_valign(Gtk::Align::CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(open_button->gobj()));
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row),
                                           GTK_WIDGET(open_button->gobj()));
    open_button->signal_clicked().connect(
        [parent, toast_overlay, lang_state, language_name]() {
            show_add_language_dialog(parent, toast_overlay, lang_state, language_name);
        });
}

// Actually deletes a language's cached alignment model from disk — one
// of two real cache locations depending on backend, same ones
// language_alignment_model_cached() checks. A failed delete is left in
// place and just logged; the row stays too, since the file didn't go
// anywhere.
void delete_language_alignment_model(const std::shared_ptr<LanguagePreferencesState>& lang_state,
                                      const AlignmentModelInfo& info) {
    std::string path = (info.backend == AlignmentBackend::kTorch)
                            ? Glib::get_home_dir() + "/.cache/torch/hub/checkpoints/" + info.id
                            : Glib::get_home_dir() + "/.cache/huggingface/hub/" +
                                  huggingface_cache_dirname(info.id);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    if (ec) {
        g_message("blind-sight: failed to delete alignment model for %s: %s", info.code,
                   ec.message().c_str());
        return;
    }
    if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&lang_state->language_group_ref))) {
        auto it = lang_state->rows.find(info.code);
        if (it != lang_state->rows.end()) {
            adw_preferences_group_remove(ADW_PREFERENCES_GROUP(obj), it->second);
            lang_state->rows.erase(it);
        }
        g_object_unref(obj);
    }
}

// Downloads one language's alignment model and, on success, leaves a
// row for it in language_group. The row appears immediately, with an
// animated Gtk::Spinner suffix removed on success or the whole row
// removed on failure. Skipped entirely if Preferences has already
// closed by the time this runs — the download still proceeds, it just
// won't have anything to show until Preferences reopens and rescans.
void start_language_alignment_prefetch(Gtk::Window* parent, AdwToastOverlay* toast_overlay,
                                        const std::shared_ptr<LanguagePreferencesState>& lang_state,
                                        const std::string& language_code) {
    std::string label = whisper_language_name(language_code);
    AdwToast* toast = adw_toast_new(
        (std::string(_("Downloading")) + " " + label + "…").c_str());
    adw_toast_set_timeout(toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, toast);

    Gtk::Spinner* spinner = nullptr;
    if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&lang_state->language_group_ref))) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), label.c_str());
        spinner = Gtk::make_managed<Gtk::Spinner>();
        spinner->set_valign(Gtk::Align::CENTER);
        spinner->start();
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), GTK_WIDGET(spinner->gobj()));
        make_language_row_activatable(row, parent, toast_overlay, lang_state, label);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(obj), row);
        lang_state->rows[language_code] = row;
        g_object_unref(obj);
    }

    try {
        std::vector<std::string> argv = {
            transcribe_venv_dir() + "/bin/python3",
            std::string(BLIND_SIGHT_TRANSCRIBE_DIR) + "/prefetch_align_model.py",
            language_code};
        // Opportunistic, not required — alignment models aren't gated,
        // but sending HF_TOKEN when one's already in the keyring is a
        // free courtesy to HuggingFace's rate limits.
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
        proc->wait_async([proc, toast, lang_state, language_code,
                           spinner](Glib::RefPtr<Gio::AsyncResult>& result) {
            proc->wait_finish(result);
            bool ok = proc->get_successful();
            adw_toast_dismiss(toast);
            if (auto* obj =
                    static_cast<GObject*>(g_weak_ref_get(&lang_state->language_group_ref))) {
                if (ok) {
                    if (spinner) {
                        spinner->stop();
                        spinner->set_visible(false);
                    }
                } else {
                    auto it = lang_state->rows.find(language_code);
                    if (it != lang_state->rows.end()) {
                        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(obj), it->second);
                        lang_state->rows.erase(it);
                    }
                }
                g_object_unref(obj);
            }
            if (!ok) {
                g_message("blind-sight: alignment model prefetch failed for %s",
                           language_code.c_str());
            }
        });
    } catch (const Glib::Error& e) {
        adw_toast_dismiss(toast);
        g_message("blind-sight: failed to launch alignment model prefetch for %s: %s",
                   language_code.c_str(), e.what());
        if (auto* obj = static_cast<GObject*>(g_weak_ref_get(&lang_state->language_group_ref))) {
            auto it = lang_state->rows.find(language_code);
            if (it != lang_state->rows.end()) {
                adw_preferences_group_remove(ADW_PREFERENCES_GROUP(obj), it->second);
                lang_state->rows.erase(it);
            }
            g_object_unref(obj);
        }
    }
}

// The language picker — a single reflect-and-sync view, not an
// add-only one — all 100 whisper languages, searchable, each showing
// whether it has an alignment model. Checkboxes start pre-checked for
// whatever's already in `lang_state->rows` — reached either from the
// standalone "Add Language" fallback button (`initial_filter` empty)
// or by clicking an existing language row. "Update" starts insensitive
// and only becomes clickable once at least one checkbox differs from
// its opening state — newly-checked languages install, newly-unchecked
// ones delete. Presented against `parent` (the main window), not the
// Preferences dialog itself — same "outlives its opener" reason as
// every other setup dialog in this file.
void show_add_language_dialog(Gtk::Window* parent, AdwToastOverlay* toast_overlay,
                               const std::shared_ptr<LanguagePreferencesState>& lang_state,
                               const std::string& initial_filter) {
    auto* dialog = ADW_DIALOG(adw_dialog_new());
    // "Languages", not "Add Language" — this same dialog both installs
    // and removes depending on how a checkbox moved, and it's reached
    // from an "All Languages" button too.
    adw_dialog_set_title(dialog, _("Languages"));
    adw_dialog_set_content_width(dialog, 420);
    adw_dialog_set_content_height(dialog, 560);

    auto* toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    auto* header = ADW_HEADER_BAR(adw_header_bar_new());
    adw_header_bar_set_show_end_title_buttons(header, FALSE);
    adw_header_bar_set_show_start_title_buttons(header, FALSE);
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

    auto* cancel_button = Gtk::make_managed<Gtk::Button>(_("Cancel"));
    cancel_button->signal_clicked().connect(
        [dialog]() { adw_dialog_close(dialog); });
    adw_header_bar_pack_start(header, GTK_WIDGET(cancel_button->gobj()));

    // "Update", not "Install" — this view can both install and delete,
    // depending on which way a checkbox moved. Starts insensitive; see
    // the dirty-tracking below.
    auto* update_button = Gtk::make_managed<Gtk::Button>(_("Update"));
    update_button->add_css_class("suggested-action");
    update_button->set_sensitive(false);
    adw_header_bar_pack_end(header, GTK_WIDGET(update_button->gobj()));

    auto* search_entry = Gtk::make_managed<Gtk::SearchEntry>();
    search_entry->set_placeholder_text(_("Filter…"));
    search_entry->set_text(initial_filter);
    search_entry->set_margin_start(12);
    search_entry->set_margin_end(12);
    search_entry->set_margin_top(12);
    search_entry->set_margin_bottom(6);

    auto* list = Gtk::make_managed<Gtk::ListBox>();
    list->add_css_class("boxed-list");
    list->set_selection_mode(Gtk::SelectionMode::NONE);

    // One checkbox per language, keyed by code — read back
    // synchronously in the "Update" handler below, before the dialog
    // (and these rows) are destroyed. Pre-checked from
    // `lang_state->rows` (not a fresh disk check) — that's the single
    // source of truth for what this Preferences instance currently
    // shows as present, including a language still mid-install.
    auto checkboxes = std::make_shared<std::map<std::string, Gtk::CheckButton*>>();
    auto initial_checked = std::make_shared<std::map<std::string, bool>>();
    auto recompute_dirty = std::make_shared<std::function<void()>>();
    for (const auto& lang : kWhisperLanguages) {
        auto* row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), lang.name);
        // The code itself is searchable too — `lang.code` is a string
        // literal with program lifetime, so no dup/free needed. See the
        // filter function below for the "nb" alias — Whisper has no
        // "nb" code, only "no".
        g_object_set_data(G_OBJECT(row), "language-code",
                           const_cast<char*>(lang.code));
        const AlignmentModelInfo* align = find_alignment_model(lang.code);
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            align ? _("Has an alignment model")
                  : _("No alignment model — transcribes without word timing"));
        auto* check = Gtk::make_managed<Gtk::CheckButton>();
        check->set_valign(Gtk::Align::CENTER);
        bool already = lang_state->rows.count(lang.code) > 0;
        check->set_active(already);
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), GTK_WIDGET(check->gobj()));
        adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row),
                                               GTK_WIDGET(check->gobj()));
        (*checkboxes)[lang.code] = check;
        (*initial_checked)[lang.code] = already;
        check->signal_toggled().connect(
            [recompute_dirty]() { (*recompute_dirty)(); });
        list->append(*Glib::wrap(row, false));
    }
    *recompute_dirty = [checkboxes, initial_checked, update_button]() {
        for (const auto& [code, check] : *checkboxes) {
            if (check->get_active() != initial_checked->at(code)) {
                update_button->set_sensitive(true);
                return;
            }
        }
        update_button->set_sensitive(false);
    };

    // Plain substring match against the row's title or Whisper language
    // code. "nb" is special-cased to also match "no" — Whisper has no
    // "nb" code (see whisper_language_code_for_ui_locale()).
    auto search_text = std::make_shared<std::string>(initial_filter);
    std::transform(search_text->begin(), search_text->end(), search_text->begin(), ::tolower);
    list->set_filter_func([search_text](Gtk::ListBoxRow* row) {
        if (search_text->empty()) {
            return true;
        }
        std::string title =
            adw_preferences_row_get_title(ADW_PREFERENCES_ROW(row->gobj()));
        std::transform(title.begin(), title.end(), title.begin(), ::tolower);
        if (title.find(*search_text) != std::string::npos) {
            return true;
        }
        const char* code = static_cast<const char*>(
            g_object_get_data(G_OBJECT(row->gobj()), "language-code"));
        if (!code) {
            return false;
        }
        std::string code_str = code;
        if (code_str.find(*search_text) != std::string::npos) {
            return true;
        }
        return code_str == "no" && search_text->find("nb") != std::string::npos;
    });
    search_entry->signal_search_changed().connect(
        [search_entry, search_text, list]() {
            *search_text = search_entry->get_text();
            std::transform(search_text->begin(), search_text->end(), search_text->begin(),
                            ::tolower);
            list->invalidate_filter();
        });

    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroller->set_child(*list);
    scroller->set_vexpand(true);
    scroller->set_margin_start(12);
    scroller->set_margin_end(12);
    scroller->set_margin_bottom(12);

    auto* content_box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    content_box->append(*search_entry);
    content_box->append(*scroller);
    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(content_box->gobj()));
    adw_dialog_set_child(dialog, GTK_WIDGET(toolbar_view));

    // Downloads run in parallel, not queued — these are small wav2vec2
    // alignment models, not the tight-GPU transcribe/diarize combination
    // maybe_transcribe_segment()'s queue exists for. Deletes are
    // synchronous filesystem work, no queueing question at all.
    update_button->signal_clicked().connect(
        [dialog, parent, checkboxes, initial_checked, toast_overlay, lang_state]() {
            for (const auto& [code, check] : *checkboxes) {
                bool now = check->get_active();
                bool was = initial_checked->at(code);
                if (now == was) {
                    continue;
                }
                const AlignmentModelInfo* align = find_alignment_model(code);
                if (now) {
                    if (!align) {
                        std::string message =
                            whisper_language_name(code) + std::string(": ") +
                            _("no alignment model — will transcribe without one");
                        AdwToast* toast = adw_toast_new(message.c_str());
                        adw_toast_set_timeout(toast, 4);
                        adw_toast_overlay_add_toast(toast_overlay, toast);
                    } else if (!language_alignment_model_cached(*align)) {
                        start_language_alignment_prefetch(parent, toast_overlay, lang_state, code);
                    }
                } else if (align) {
                    delete_language_alignment_model(lang_state, *align);
                }
            }
            adw_dialog_close(dialog);
        });

    adw_dialog_present(dialog, GTK_WIDGET(parent->gobj()));
}

// The actual work behind "Next" in show_transcription_setup_offer()
// below — unlike the other setup dialogs in this file, which just tell
// the user what to run themselves, this one runs
// setup-transcribe-env.sh as a subprocess.
//
// Deliberately doesn't try to flip transcribe_row's switch state back
// on after a successful install — the row belongs to whichever
// Preferences dialog instance triggered this, and a background install
// can easily outlive that dialog if the user closes Preferences
// meanwhile. Only `settings` and `parent` cross that async boundary
// safely; the switch just shows the correct state next time
// Preferences reopens.
void install_transcription_libraries_and_continue(
    Gtk::Window* parent, const Glib::RefPtr<Gio::Settings>& settings,
    AdwToastOverlay* toast_overlay) {
    if (transcription_available()) {
        if (whisper_model_cached(settings->get_string(kTranscriptionModelSizeKey).raw())) {
            settings->set_boolean(kTranscribeAudioKey, true);
        } else {
            // Deferred one idle iteration — this runs from show_
            // transcription_setup_offer()'s "response" handler, while
            // that dialog is still closing; presenting a second
            // AdwDialog synchronously from in there doesn't show
            // reliably. Letting dialog 1 finish closing first fixes it.
            Glib::signal_idle().connect_once(
                [parent, settings]() {
                    show_transcription_model_setup_offer(parent, settings);
                });
        }
        return;
    }
    std::string script_path =
        std::string(BLIND_SIGHT_TRANSCRIBE_DIR) + "/setup-transcribe-env.sh";
    if (!std::filesystem::exists(script_path)) {
        bridge_show_error_dialog(
            parent, _("Can't Install Automatically"),
            _("You'll need to install this manually — I can't do it "
              "automatically."));
        return;
    }
    AdwToast* toast = adw_toast_new(_("Installing transcription libraries…"));
    adw_toast_set_timeout(toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, toast);
    try {
        auto proc = Gio::Subprocess::create({script_path}, Gio::Subprocess::Flags::NONE);
        proc->wait_async(
            [proc, parent, settings, toast](Glib::RefPtr<Gio::AsyncResult>& result) {
                proc->wait_finish(result);
                adw_toast_dismiss(toast);
                if (!proc->get_successful() || !transcription_available()) {
                    g_message("blind-sight: transcription library install failed");
                    bridge_show_error_dialog(
                        parent, _("Installation Failed"),
                        _("Something went wrong installing the "
                          "transcription libraries — run demo/"
                          "transcribe/setup-transcribe-env.sh from a "
                          "terminal to see what happened."));
                    return;
                }
                if (whisper_model_cached(settings->get_string(kTranscriptionModelSizeKey).raw())) {
                    settings->set_boolean(kTranscribeAudioKey, true);
                } else {
                    Glib::signal_idle().connect_once(
                        [parent, settings]() {
                            show_transcription_model_setup_offer(parent, settings);
                        });
                }
            });
    } catch (const Glib::Error& e) {
        adw_toast_dismiss(toast);
        g_message("blind-sight: failed to launch transcription library "
                   "install: %s",
                   e.what());
        bridge_show_error_dialog(
            parent, _("Can't Install Automatically"),
            _("You'll need to install this manually — I can't do it "
              "automatically."));
    }
}

// Two-button dialog offered when "Transcribe Recordings" is turned on
// without the venv present. Dismiss does nothing further — the row was
// already reverted off by the caller before this dialog appeared.
// "Next" is wired to install_transcription_libraries_and_continue()
// above. Also shows the copyable setup command — "Next" auto-runs it,
// but a user who'd rather run it by hand still needs to see exactly
// what to run. The path is always absolute (a compile-time
// CMAKE_SOURCE_DIR substitution), not a relative string — nothing
// guarantees the source tree is the current directory wherever the
// app was launched from.
void show_transcription_setup_offer(Gtk::Window* parent,
                                     const Glib::RefPtr<Gio::Settings>& settings,
                                     AdwToastOverlay* toast_overlay) {
    std::string command =
        std::string(BLIND_SIGHT_TRANSCRIBE_DIR) + "/setup-transcribe-env.sh";
    auto* dialog = adw_alert_dialog_new(
        _("Transcription Libraries Not Set Up"),
        _("This installs what's needed to transcribe recorded speech to "
          "text — a separate step from the transcription model itself, "
          "which downloads after this, the first time a recording is "
          "actually transcribed. It needs real disk space — roughly "
          "1–2GB with CPU-only PyTorch, but up to ~5.5GB if a "
          "CUDA-capable GPU is detected. \"Next\" runs the command below "
          "automatically, or run it yourself first if you'd rather:"));
    auto* command_label = Gtk::make_managed<Gtk::Label>(command);
    command_label->set_selectable(true);
    command_label->set_halign(Gtk::Align::START);
    command_label->set_wrap(true);
    command_label->add_css_class("monospace");
    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog),
                                      GTK_WIDGET(command_label->gobj()));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "dismiss", _("Dismiss"));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "next", _("Next →"));
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "next",
                                              ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "next");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "dismiss");

    struct ResponseContext {
        Glib::RefPtr<Gio::Settings> settings;
        Gtk::Window* parent;
        AdwToastOverlay* toast_overlay;
    };
    auto* ctx = new ResponseContext{settings, parent, toast_overlay};
    g_signal_connect_data(
        dialog, "response",
        G_CALLBACK(+[](AdwAlertDialog*, const char* response, gpointer data) {
            auto* ctx = static_cast<ResponseContext*>(data);
            if (std::string(response) == "next") {
                install_transcription_libraries_and_continue(
                    ctx->parent, ctx->settings, ctx->toast_overlay);
            }
        }),
        ctx,
        +[](gpointer data, GClosure*) { delete static_cast<ResponseContext*>(data); },
        GConnectFlags(0));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent->gobj()));
}

namespace {

// A hint for pyannote's diarization clustering, not a hard fact — how
// many people were present for a test is already known, readable from
// creator/contributor(s). Without this hint, pyannote has to guess the
// speaker count purely from clustering short/noisy audio, which is
// exactly where it's least reliable — it can mis-split a single
// speaker's voice into several "speaker n" labels. `creator` is a
// single Dublin-Core field (one facilitator); `contributor` is free
// text that may list several names, split on comma/semicolon. Returns
// 0 (let pyannote decide entirely on its own) if both fields are empty.
int speaker_count_hint(const RecordMeta& meta) {
    int count = meta.creator.empty() ? 0 : 1;
    std::string field = meta.contributor;
    std::size_t start = 0;
    while (start <= field.size()) {
        std::size_t end = field.find_first_of(",;", start);
        std::string piece = field.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::size_t first = piece.find_first_not_of(" \t");
        if (first != std::string::npos) {
            count++;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return count;
}

// Shown at most once per run, same reasoning as the depth-setup dialog's
// own equivalent in main.cpp.
bool g_shown_transcription_setup_dialog = false;

// Same "shown at most once per run" reasoning, for diarization's
// runtime auth-failure dialog — fires on a *runtime* failure
// (transcribe.py's exit code 2) after a token was found and used, not
// a missing local prerequisite (that path never auto-prompts, see
// maybe_transcribe_segment() below).
bool g_shown_diarization_auth_failed_dialog = false;

// Paths currently being transcribed, same duplicate-launch guard as
// main.cpp's own g_depth_generation_in_progress. Also covers a path while
// it's merely queued, not just while its subprocess is actually running.
std::set<std::string> g_transcription_in_progress;

// Everything maybe_transcribe_segment() needs to actually launch, held
// here while a request waits its turn in g_transcription_queue.
struct PendingTranscription {
    Gtk::Window* parent;
    Glib::RefPtr<Gio::Settings> settings;
    AdwToastOverlay* toast_overlay;
    std::string audio_path;
    std::string session_path;
    int track_number;
};

// Only one transcribe.py runs at a time, app-wide — the GPU (5.66GB)
// barely fits one pipeline's peak usage even with transcribe.py's own
// memory fixes applied, so two concurrent launches would very likely
// OOM. Requests queue here instead of racing each other; launched one
// at a time by launch_next_queued_transcription() below, which
// re-invokes itself once each subprocess completes.
std::deque<PendingTranscription> g_transcription_queue;
bool g_transcription_running = false;

// The actual launch, run for at most one queued request at a time (see
// g_transcription_running above). Re-invokes itself once a subprocess
// completes, so the queue drains on its own without any external driver.
void launch_next_queued_transcription() {
    if (g_transcription_running || g_transcription_queue.empty()) {
        return;
    }
    PendingTranscription job = std::move(g_transcription_queue.front());
    g_transcription_queue.pop_front();
    g_transcription_running = true;

    Gtk::Window* parent = job.parent;
    Glib::RefPtr<Gio::Settings> settings = job.settings;
    AdwToastOverlay* toast_overlay = job.toast_overlay;
    std::string audio_path = job.audio_path;
    std::string session_path = job.session_path;
    int track_number = job.track_number;

    // Diarization rides along with this same transcribe.py invocation
    // rather than a separate subprocess — it labels segments
    // transcribe.py already produced. Only actually attempted if a
    // token is currently in the keyring, re-checked here in case it's
    // since been deleted. If diarize-audio is on but the token's
    // vanished, this silently falls back to plain transcription.
    bool diarize = settings->get_boolean(kDiarizeAudioKey);
    // Looked up regardless of `diarize` — whisper/alignment downloads
    // aren't gated the way diarization's models are, but sending
    // HF_TOKEN when one's already in the keyring is a free courtesy to
    // HuggingFace's rate limits. Whether `--diarize` itself gets added
    // below still depends on `diarize`.
    std::optional<std::string> hf_token = lookup_huggingface_token();
    // A brief start toast and a brief "Done." toast at the end, not a
    // persistent toast held for the whole run — diarization can take
    // much longer than transcription alone, and a toast that stays up
    // the entire time risks looking stuck. "Done." names the track too
    // — with the queue above, it could arrive well after the user
    // moved on to a different track.
    std::string track_label =
        std::string(_("Track")) + " " + std::to_string(track_number);
    // Timeout 0 (never auto-dismiss) — if it self-dismissed via a
    // timeout before wait_async's callback runs, start_toast would
    // already be freed by the time adw_toast_dismiss(start_toast) below
    // runs on it — a use-after-free straight into libadwaita. Model
    // load alone is ~4.5s, well past a 3s timeout.
    AdwToast* start_toast = adw_toast_new(
        (std::string(_("Transcribing")) + " " + track_label + "…").c_str());
    adw_toast_set_timeout(start_toast, 0);
    adw_toast_overlay_add_toast(toast_overlay, start_toast);
    auto finish_job = [audio_path]() {
        g_transcription_in_progress.erase(audio_path);
        g_transcription_running = false;
        launch_next_queued_transcription();
    };
    try {
        std::vector<std::string> argv = {
            transcribe_venv_dir() + "/bin/python3",
            std::string(BLIND_SIGHT_TRANSCRIBE_DIR) + "/transcribe.py",
            audio_path,
            "--model-size",
            settings->get_string(kTranscriptionModelSizeKey).raw()};
        if (diarize && hf_token) {
            argv.push_back("--diarize");
            RecordMeta meta;
            if (read_record_meta(session_path, meta)) {
                int hint = speaker_count_hint(meta);
                if (hint > 0) {
                    argv.push_back("--max-speakers");
                    argv.push_back(std::to_string(hint));
                }
            }
        }
        Glib::RefPtr<Gio::Subprocess> proc;
        if (hf_token) {
            // A launcher only so HF_TOKEN can be set on this process's
            // own environment — never written to a file, never logged,
            // gone once this subprocess exits. Plain
            // Gio::Subprocess::create() has no way to set an
            // environment variable at all.
            auto launcher =
                Gio::SubprocessLauncher::create(Gio::Subprocess::Flags::NONE);
            launcher->setenv("HF_TOKEN", *hf_token, true);
            proc = launcher->spawn(argv);
        } else {
            proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        }
        proc->wait_async(
            [proc, start_toast, toast_overlay, audio_path, track_label,
             parent, finish_job](Glib::RefPtr<Gio::AsyncResult>& result) {
                proc->wait_finish(result);
                // Exit code 2 = transcript written, diarization
                // specifically failed — not a hard failure, but worth
                // telling the user once, since it means their
                // HuggingFace setup needs another look.
                bool hard_failure = false;
                if (proc->get_exit_status() == 2) {
                    g_message("blind-sight: diarization failed for %s "
                               "(transcript still written)",
                               audio_path.c_str());
                    if (!g_shown_diarization_auth_failed_dialog) {
                        g_shown_diarization_auth_failed_dialog = true;
                        bridge_show_error_dialog(
                            parent, _("Diarization Failed"),
                            _("The transcript was written, but speaker "
                              "labeling didn't work — the HuggingFace "
                              "token may be invalid or expired, or "
                              "pyannote may have revoked access to the "
                              "model. Check Diarize Recordings in "
                              "Preferences to redo the setup."));
                    }
                } else if (!proc->get_successful()) {
                    hard_failure = true;
                    g_message("blind-sight: transcription failed for %s",
                               audio_path.c_str());
                }
                adw_toast_dismiss(start_toast);
                // "Done." only when a transcript actually came out of
                // this — a hard failure already has its own log line
                // and produces nothing worth calling done.
                if (!hard_failure) {
                    AdwToast* done_toast = adw_toast_new(
                        (track_label + " " + _("done.")).c_str());
                    adw_toast_set_timeout(done_toast, 3);
                    adw_toast_overlay_add_toast(toast_overlay, done_toast);
                }
                finish_job();
            });
    } catch (const Glib::Error& e) {
        adw_toast_dismiss(start_toast);
        g_message("blind-sight: failed to launch transcription for %s: %s",
                   audio_path.c_str(), e.what());
        finish_job();
    }
}

}  // namespace

// Same automatic-trigger shape as main.cpp's
// maybe_generate_depth_on_the_fly(), called from
// stop_audio_track_and_encode() right after a segment's audio settles.
// Deliberately chained after the optional Opus encode there, not
// called in parallel with it — encoding is ~470x realtime
// (near-instant), transcription is not, so a parallel launch could
// have the WAV deleted out from under a transcription job that hadn't
// opened it yet if "Transcode Audio to Opus" is on.
//
// Only enqueues (see g_transcription_queue above) — actually launched
// by launch_next_queued_transcription() once it's this request's turn,
// so that at most one transcribe.py runs at a time app-wide.
void maybe_transcribe_segment(Gtk::Window* parent,
                               const Glib::RefPtr<Gio::Settings>& settings,
                               AdwToastOverlay* toast_overlay,
                               const std::string& audio_path,
                               const std::string& session_path, int track_number) {
    if (!settings->get_boolean(kTranscribeAudioKey)) {
        return;
    }
    if (audio_path.empty() || has_transcript_file(audio_path) ||
        g_transcription_in_progress.count(audio_path) > 0) {
        return;
    }
    if (!transcription_available()) {
        if (!g_shown_transcription_setup_dialog) {
            g_shown_transcription_setup_dialog = true;
            bridge_show_setup_required_dialog(
                parent, _("Transcription Not Set Up"),
                _("Run this from a terminal — recordings will then get "
                  "a transcript automatically."),
                "demo/transcribe/setup-transcribe-env.sh");
        }
        return;
    }
    g_transcription_in_progress.insert(audio_path);
    g_transcription_queue.push_back(
        {parent, settings, toast_overlay, audio_path, session_path, track_number});
    launch_next_queued_transcription();
}

namespace {

// "HH:MM:SS.mmm" — the exact shape transcribe.py's write_vtt() always
// writes, so a plain sscanf is enough; std::nullopt on anything that
// doesn't match, so one malformed cue can't take the rest of the file
// down.
std::optional<std::int32_t> parse_vtt_timestamp(const std::string& text) {
    int hours = 0;
    int minutes = 0;
    double seconds = 0.0;
    if (std::sscanf(text.c_str(), "%d:%d:%lf", &hours, &minutes, &seconds) != 3) {
        return std::nullopt;
    }
    double total_ms = (hours * 3600.0 + minutes * 60.0 + seconds) * 1000.0;
    return static_cast<std::int32_t>(std::llround(total_ms));
}

// Reads whichever cue lines a WebVTT file has — the "WEBVTT" header,
// blank lines, and numeric cue identifiers are all silently skipped by
// simply looking for a "-->" line; cue text is everything up to the
// next blank line, joined with spaces if it spans several. Returns an
// empty vector, not an error, for a missing or unparseable file.
std::vector<TranscriptCue> parse_vtt(const std::string& path) {
    std::vector<TranscriptCue> cues;
    std::ifstream in(path);
    if (!in) {
        return cues;
    }
    std::string line;
    while (std::getline(in, line)) {
        auto arrow = line.find("-->");
        if (arrow == std::string::npos) {
            continue;
        }
        auto start_ms = parse_vtt_timestamp(line.substr(0, arrow));
        auto end_ms = parse_vtt_timestamp(line.substr(arrow + 3));
        if (!start_ms || !end_ms) {
            continue;
        }
        std::string text;
        while (std::getline(in, line) && !line.empty()) {
            if (!text.empty()) {
                text += " ";
            }
            text += line;
        }
        cues.push_back({*start_ms, *end_ms, text});
    }
    return cues;
}

}  // namespace

// Every transcript cue for a whole session, session-absolute — walks
// the same AudioSegment list Playback's audio sync uses, reading each
// segment's own transcript (silently skipped if it has none yet). A
// track recorded since tracks.json shipped has a real entry (non-empty
// id) for its own track_n and lives under tracks/<id>/; a legacy track,
// or one never touched since, doesn't, and falls back to the flat
// "recording.trackN.*" naming exactly as before — same lookup
// resolve_track_audio_path_any() (main.cpp) uses for the audio itself.
// Each segment's cues are offset by `segment.start_ms`, since WhisperX
// only saw that segment's audio file starting near t=0, not the
// session's full timeline.
std::vector<TranscriptCue> compute_transcript_cues(
    const std::string& session_path, const std::vector<AudioSegment>& segments) {
    std::string session_dir = Glib::path_get_dirname(session_path);
    auto tracks_index = read_tracks_index(session_dir);
    auto track_id_for = [&tracks_index](int track_n) -> std::string {
        for (const auto& entry : tracks_index.tracks) {
            if (entry.track_number == track_n && !entry.id.empty()) {
                return entry.id;
            }
        }
        return "";
    };
    std::vector<TranscriptCue> cues;
    for (const auto& segment : segments) {
        std::string track_id = track_id_for(segment.track_n);
        std::string path = track_id.empty()
                                ? track_transcript_path(session_path, segment.track_n,
                                                          segment.segment_n)
                                : track_transcript_path_for_id(session_path, track_id,
                                                                 segment.segment_n);
        for (auto& cue : parse_vtt(path)) {
            cue.start_ms += segment.start_ms;
            cue.end_ms += segment.start_ms;
            cues.push_back(std::move(cue));
        }
    }
    return cues;
}

// The cue active at `ms`, or nullptr — same shape as main.cpp's own
// audio_segment_for_ms().
const TranscriptCue* transcript_cue_for_ms(const std::vector<TranscriptCue>& cues,
                                            std::int32_t ms) {
    for (const auto& cue : cues) {
        if (ms >= cue.start_ms && ms < cue.end_ms) {
            return &cue;
        }
    }
    return nullptr;
}
