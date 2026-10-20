// SPDX-License-Identifier: GPL-3.0-only
//
// audio.hpp — per-track audio file naming, async WAV->Opus encoding, and
// reconstructing a recording's own track/segment structure from its
// event log.
//
// Scope note: start_audio_track()/stop_audio_track()/
// stop_audio_track_and_encode()/write_implicit_track_change()/
// stop_playback_audio()/sync_playback_audio() stay in main.cpp — they
// take RecordingSession/PlaybackSession directly, and neither core type
// has a header of its own yet (same reasoning as depth.hpp). This
// module is the RecordingSession/PlaybackSession-independent plumbing
// underneath them: paths, encoding, and track/segment reconstruction
// from a plain event vector.

#pragma once

#include <glibmm.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "recording.hpp"
#include "transcript_types.hpp"

std::string track_audio_base(const std::string& session_path, int track_n, int segment_n);
std::string track_audio_path(const std::string& session_path, int track_n, int segment_n);
std::string track_audio_opus_path(const std::string& session_path, int track_n,
                                   int segment_n);
std::string track_transcript_path(const std::string& session_path, int track_n,
                                   int segment_n);
// Whichever of the WAV/Opus path actually exists on disk, Opus
// preferred; empty if neither does.
std::string resolve_track_audio_path(const std::string& session_path, int track_n,
                                      int segment_n);

// A track minted a stable id (tracks.hpp) keeps its files under their
// own directory, "<session_dir>/tracks/<track_id>/", rather than the
// flat "recording.trackN[.M]" naming the four functions above still
// serve — those stay exactly as they are, read-only fallback for a track
// recorded before this scheme existed (see audio.cpp's own comment).
// "<session_dir>/tracks/<track_id>".
std::string track_directory(const std::string& session_path, const std::string& track_id);
std::string track_audio_base_for_id(const std::string& session_path,
                                     const std::string& track_id, int segment_n);
std::string track_audio_path_for_id(const std::string& session_path,
                                     const std::string& track_id, int segment_n);
std::string track_audio_opus_path_for_id(const std::string& session_path,
                                          const std::string& track_id, int segment_n);
std::string track_transcript_path_for_id(const std::string& session_path,
                                          const std::string& track_id, int segment_n);
std::string resolve_track_audio_path_for_id(const std::string& session_path,
                                             const std::string& track_id, int segment_n);

// track_starts[0] is always 0; every subsequent entry is a WaitEvent's
// own t_ms.
std::vector<std::int32_t> compute_track_starts(
    const std::vector<blind_sight::RecordEvent>& events);

// Rebuilds the mic-on/mic-off segment list from a recording's own
// MicrophoneEvents.
std::vector<AudioSegment> compute_audio_segments(
    const std::vector<blind_sight::RecordEvent>& events);
// The segment containing `ms`, or nullptr if the mic wasn't recording at
// that instant.
const AudioSegment* audio_segment_for_ms(const std::vector<AudioSegment>& segments,
                                          std::int32_t ms);

bool opus_encoding_available();
// Async WAV->Opus transcode; `on_done` is called exactly once, with
// `true` on success.
void encode_wav_to_opus_async(const std::string& wav_path, const std::string& opus_path,
                               std::function<void(bool)> on_done);
