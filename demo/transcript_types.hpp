// SPDX-License-Identifier: GPL-3.0-only
//
// transcript_types.hpp — AudioSegment/TranscriptCue, shared between
// main.cpp (PlaybackSession, audio-track bookkeeping) and
// transcription.cpp (reading transcripts back for subtitles), so both
// translation units see the same type without duplicating it.

#pragma once

#include <cstdint>
#include <string>

// One mic-on/mic-off stretch, i.e. one audio WAV file — a track has
// exactly one of these unless the mic was toggled off and back on
// mid-recording, in which case it has more, each with its own file.
// `segment_n` matches track_audio_path()'s numbering (0 = the track's
// first pipeline). `end_ms` is exclusive;
// std::numeric_limits<std::int32_t>::max() means no matching off event
// was ever recorded — an app killed mid-recording can leave one open.
struct AudioSegment {
    std::int32_t start_ms;
    std::int32_t end_ms;
    int track_n;
    int segment_n;
};

// One WebVTT cue, subtitle-style rather than one word each —
// start_ms/end_ms are session-absolute, not relative to whichever
// segment's audio file WhisperX processed (see
// compute_transcript_cues(), transcription.cpp).
struct TranscriptCue {
    std::int32_t start_ms;
    std::int32_t end_ms;
    std::string text;
};
