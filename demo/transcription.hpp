// SPDX-License-Identifier: GPL-3.0-only
//
// transcription.hpp — WhisperX transcription, pyannote diarization, and
// per-language alignment-model setup: availability checks, the
// Preferences-facing setup dialogs, the transcribe-on-stop queue, and
// reading a session's .vtt files back into subtitle cues for Playback.
//
// Everything declared here is this module's public API: what main.cpp
// and preferences.cpp call into. Internal helpers (the venv path, the
// download queue, the VTT parser, etc.) stay file-local to
// transcription.cpp.

#pragma once

#include <gtk/gtk.h>
#include <adwaita.h>
#include <gtkmm.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "transcript_types.hpp"

// Two disjoint backends for a language's word-alignment model, each
// downloaded/cached completely differently — see kAlignableLanguages'
// own comment in transcription.cpp for the full breakdown.
enum class AlignmentBackend { kTorch, kHuggingFace };
struct AlignmentModelInfo {
    const char* code;
    AlignmentBackend backend;
    const char* id;  // torch: checkpoint filename; huggingface: "org/repo"
};

// The 41 (of Whisper's 100) languages that whisperx can actually align.
// Two disjoint backends, each downloaded/cached completely differently:
//   - kTorch (5 languages): torchaudio's own bundled pipelines, cached
//     as a single checkpoint file under ~/.cache/torch/hub/checkpoints/
//     — the exact filename per bundle (confirmed by reading
//     torchaudio's pipelines/_wav2vec2/impl.py), NOT the same string as
//     the bundle name whisperx's dict uses, which is a Python symbol
//     name, not a filename.
//   - kHuggingFace (36 languages): an ordinary HuggingFace model repo,
//     cached the same way pyannote's diarization models already are —
//     "org/repo" becomes "models--org--repo" under
//     ~/.cache/huggingface/hub/.
// Generated straight from whisperx.alignment's
// DEFAULT_ALIGN_MODELS_TORCH/_HF dicts, not hand-typed — whisperx's API
// isn't safe to guess at from memory.
inline constexpr AlignmentModelInfo kAlignableLanguages[] = {
    {"en", AlignmentBackend::kTorch, "wav2vec2_fairseq_base_ls960_asr_ls960.pth"},
    {"fr", AlignmentBackend::kTorch, "wav2vec2_voxpopuli_base_10k_asr_fr.pt"},
    {"de", AlignmentBackend::kTorch, "wav2vec2_voxpopuli_base_10k_asr_de.pt"},
    {"es", AlignmentBackend::kTorch, "wav2vec2_voxpopuli_base_10k_asr_es.pt"},
    {"it", AlignmentBackend::kTorch, "wav2vec2_voxpopuli_base_10k_asr_it.pt"},
    {"ja", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-japanese"},
    {"zh", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-chinese-zh-cn"},
    {"nl", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-dutch"},
    {"uk", AlignmentBackend::kHuggingFace, "Yehor/wav2vec2-xls-r-300m-uk-with-small-lm"},
    {"pt", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-portuguese"},
    {"ar", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-arabic"},
    {"cs", AlignmentBackend::kHuggingFace, "comodoro/wav2vec2-xls-r-300m-cs-250"},
    {"ru", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-russian"},
    {"pl", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-polish"},
    {"hu", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-hungarian"},
    {"fi", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-finnish"},
    {"fa", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-persian"},
    {"el", AlignmentBackend::kHuggingFace, "jonatasgrosman/wav2vec2-large-xlsr-53-greek"},
    {"tr", AlignmentBackend::kHuggingFace, "mpoyraz/wav2vec2-xls-r-300m-cv7-turkish"},
    {"da", AlignmentBackend::kHuggingFace, "saattrupdan/wav2vec2-xls-r-300m-ftspeech"},
    {"he", AlignmentBackend::kHuggingFace, "imvladikon/wav2vec2-xls-r-300m-hebrew"},
    {"vi", AlignmentBackend::kHuggingFace, "nguyenvulebinh/wav2vec2-base-vi-vlsp2020"},
    {"ko", AlignmentBackend::kHuggingFace, "kresnik/wav2vec2-large-xlsr-korean"},
    {"ur", AlignmentBackend::kHuggingFace, "kingabzpro/wav2vec2-large-xls-r-300m-Urdu"},
    {"te", AlignmentBackend::kHuggingFace, "anuragshas/wav2vec2-large-xlsr-53-telugu"},
    {"hi", AlignmentBackend::kHuggingFace, "theainerd/Wav2Vec2-large-xlsr-hindi"},
    {"ca", AlignmentBackend::kHuggingFace, "softcatala/wav2vec2-large-xlsr-catala"},
    {"ml", AlignmentBackend::kHuggingFace, "gvs/wav2vec2-large-xlsr-malayalam"},
    {"no", AlignmentBackend::kHuggingFace, "NbAiLab/nb-wav2vec2-1b-bokmaal-v2"},
    {"nn", AlignmentBackend::kHuggingFace, "NbAiLab/nb-wav2vec2-1b-nynorsk"},
    {"sk", AlignmentBackend::kHuggingFace, "comodoro/wav2vec2-xls-r-300m-sk-cv8"},
    {"sl", AlignmentBackend::kHuggingFace, "anton-l/wav2vec2-large-xlsr-53-slovenian"},
    {"hr", AlignmentBackend::kHuggingFace, "classla/wav2vec2-xls-r-parlaspeech-hr"},
    {"ro", AlignmentBackend::kHuggingFace, "gigant/romanian-wav2vec2"},
    {"eu", AlignmentBackend::kHuggingFace, "stefan-it/wav2vec2-large-xlsr-53-basque"},
    {"gl", AlignmentBackend::kHuggingFace, "ifrz/wav2vec2-large-xlsr-galician"},
    {"ka", AlignmentBackend::kHuggingFace, "xsway/wav2vec2-large-xlsr-georgian"},
    {"lv", AlignmentBackend::kHuggingFace, "jimregan/wav2vec2-large-xlsr-latvian-cv"},
    {"tl", AlignmentBackend::kHuggingFace, "Khalsuu/filipino-wav2vec2-l-xls-r-300m-official"},
    {"sv", AlignmentBackend::kHuggingFace, "KBLab/wav2vec2-large-voxrex-swedish"},
    {"id", AlignmentBackend::kHuggingFace, "cahya/wav2vec2-large-xlsr-indonesian"},
};

// Shared state for the Preferences "Transcription Languages" group —
// threaded through the row-click handler, the install/delete flows, and
// the language picker dialog, all of which mutate the same live set of
// rows. `language_group_ref` is a GWeakRef, not a raw pointer: several
// of these paths are async (a model download) or presented against the
// main window rather than Preferences itself, so Preferences closing
// meanwhile is a real possibility.
struct LanguagePreferencesState {
    GWeakRef language_group_ref;
    std::map<std::string, GtkWidget*> rows;
    ~LanguagePreferencesState() { g_weak_ref_clear(&language_group_ref); }
};

bool transcription_available();
bool whisper_model_cached(const std::string& model_size);
bool diarization_model_cached();
bool huggingface_token_available();
std::optional<std::string> lookup_huggingface_token();
const char* whisper_language_name(const std::string& code);
const AlignmentModelInfo* find_alignment_model(const std::string& code);
bool language_alignment_model_cached(const AlignmentModelInfo& info);

void show_transcription_setup_offer(Gtk::Window* parent,
                                     const Glib::RefPtr<Gio::Settings>& settings,
                                     AdwToastOverlay* toast_overlay);
void show_transcription_model_setup_offer(Gtk::Window* parent,
                                           const Glib::RefPtr<Gio::Settings>& settings,
                                           AdwSwitchRow* row = nullptr);
void show_diarization_setup_offer(Gtk::Window* parent,
                                   const Glib::RefPtr<Gio::Settings>& settings,
                                   AdwToastOverlay* toast_overlay, AdwSwitchRow* row);
// Called directly by preferences.cpp's own diarize_row handler too, not
// just internally by show_diarization_setup_offer()'s "Ok" response.
void start_diarization_model_prefetch(Gtk::Window* parent,
                                       const Glib::RefPtr<Gio::Settings>& settings,
                                       AdwToastOverlay* toast_overlay, AdwSwitchRow* row,
                                       const std::string& hf_token);
void show_add_language_dialog(Gtk::Window* parent, AdwToastOverlay* toast_overlay,
                               const std::shared_ptr<LanguagePreferencesState>& lang_state,
                               const std::string& initial_filter);
void make_language_row_activatable(
    GtkWidget* row, Gtk::Window* parent, AdwToastOverlay* toast_overlay,
    const std::shared_ptr<LanguagePreferencesState>& lang_state,
    const std::string& language_name);

// Enqueues a segment for transcription once Stop settles its audio file
// — a no-op if Transcribe Recordings is off, the file's already
// transcribed, or it's already queued/running. At most one transcribe.py
// runs app-wide at a time (GPU memory); see transcription.cpp.
void maybe_transcribe_segment(Gtk::Window* parent,
                               const Glib::RefPtr<Gio::Settings>& settings,
                               AdwToastOverlay* toast_overlay,
                               const std::string& audio_path,
                               const std::string& session_path, int track_number);

// Every transcript cue for a whole session, session-absolute, and the one
// active at a given playback position — Playback's own subtitle lookup.
std::vector<TranscriptCue> compute_transcript_cues(
    const std::string& session_path, const std::vector<AudioSegment>& segments);
const TranscriptCue* transcript_cue_for_ms(const std::vector<TranscriptCue>& cues,
                                            std::int32_t ms);
