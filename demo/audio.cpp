// SPDX-License-Identifier: GPL-3.0-only
//
// audio.cpp — see audio.hpp for scope.

#include "audio.hpp"

#include <gst/gst.h>
#include <glib/gi18n.h>

#include <limits>
#include <string_view>

using blind_sight::MicrophoneEvent;
using blind_sight::RecordEvent;
using blind_sight::WaitEvent;

// The session path with its ".bs" extension stripped, plus the
// ".trackN[.M]" suffix shared by both the recorded-WAV and encoded-Opus
// paths below — "recording.bs" -> track 3 -> "recording.track3". `M`
// (the audio segment) is omitted when 0, present when the mic was
// toggled off and back on within the track — "recording.track3.1".
std::string track_audio_base(const std::string& session_path, int track_n,
                              int segment_n) {
    std::string base = session_path;
    constexpr std::string_view kExtension = ".bs";
    if (base.size() >= kExtension.size() &&
        base.compare(base.size() - kExtension.size(), kExtension.size(),
                      kExtension) == 0) {
        base.resize(base.size() - kExtension.size());
    }
    base += ".track" + std::to_string(track_n);
    if (segment_n > 0) {
        base += "." + std::to_string(segment_n);
    }
    return base;
}

// This track's own recorded audio file — always WAV, since that's the
// only format start_audio_track() ever captures to. This is Record's
// own file, the "before" of the later encode step.
std::string track_audio_path(const std::string& session_path, int track_n,
                              int segment_n) {
    return track_audio_base(session_path, track_n, segment_n) + ".wav";
}

// The same segment's *encoded* file — written automatically once a
// track's capture pipeline closes (stop_audio_track_and_encode(),
// main.cpp). Never both at once in normal use: encoding deletes the
// WAV it just replaced.
std::string track_audio_opus_path(const std::string& session_path, int track_n,
                                   int segment_n) {
    return track_audio_base(session_path, track_n, segment_n) + ".opus";
}

// The same segment's transcript, if any — shares its base with both
// audio paths above rather than being derived from whichever currently
// exists on disk, so its name never depends on whether that segment
// ended up staying WAV or becoming Opus.
std::string track_transcript_path(const std::string& session_path, int track_n,
                                   int segment_n) {
    return track_audio_base(session_path, track_n, segment_n) + ".vtt";
}

// What Playback actually plays for a segment — whichever of the two above
// exists, Opus preferred (the WAV it replaced is usually gone by then
// anyway). Empty if neither does (nothing was ever recorded for this
// segment, or the file was moved/deleted by hand).
std::string resolve_track_audio_path(const std::string& session_path, int track_n,
                                      int segment_n) {
    std::string opus_path = track_audio_opus_path(session_path, track_n, segment_n);
    if (Glib::file_test(opus_path, Glib::FileTest::EXISTS)) {
        return opus_path;
    }
    std::string wav_path = track_audio_path(session_path, track_n, segment_n);
    if (Glib::file_test(wav_path, Glib::FileTest::EXISTS)) {
        return wav_path;
    }
    return {};
}

std::string track_directory(const std::string& session_path, const std::string& track_id) {
    return Glib::path_get_dirname(session_path) + "/tracks/" + track_id;
}

// Same ".M" segment-suffix convention as track_audio_base() — only the
// directory differs, not the per-segment naming within it.
std::string track_audio_base_for_id(const std::string& session_path,
                                     const std::string& track_id, int segment_n) {
    std::string base = track_directory(session_path, track_id) + "/recording";
    if (segment_n > 0) {
        base += "." + std::to_string(segment_n);
    }
    return base;
}

std::string track_audio_path_for_id(const std::string& session_path,
                                     const std::string& track_id, int segment_n) {
    return track_audio_base_for_id(session_path, track_id, segment_n) + ".wav";
}

std::string track_audio_opus_path_for_id(const std::string& session_path,
                                          const std::string& track_id, int segment_n) {
    return track_audio_base_for_id(session_path, track_id, segment_n) + ".opus";
}

std::string track_transcript_path_for_id(const std::string& session_path,
                                          const std::string& track_id, int segment_n) {
    return track_audio_base_for_id(session_path, track_id, segment_n) + ".vtt";
}

std::string resolve_track_audio_path_for_id(const std::string& session_path,
                                             const std::string& track_id, int segment_n) {
    std::string opus_path = track_audio_opus_path_for_id(session_path, track_id, segment_n);
    if (Glib::file_test(opus_path, Glib::FileTest::EXISTS)) {
        return opus_path;
    }
    std::string wav_path = track_audio_path_for_id(session_path, track_id, segment_n);
    if (Glib::file_test(wav_path, Glib::FileTest::EXISTS)) {
        return wav_path;
    }
    return {};
}

// A "track" is just whatever lies between two WaitEvents — Record's
// resume logic already guarantees one exists at every Stop/Forward
// boundary. track_starts[0] is always 0 (track 1's start); every
// subsequent entry is a WaitEvent's t_ms (where that gap's resume
// happened).
std::vector<std::int32_t> compute_track_starts(const std::vector<RecordEvent>& events) {
    std::vector<std::int32_t> starts{0};
    for (const auto& event : events) {
        if (auto* wait = std::get_if<WaitEvent>(&event)) {
            starts.push_back(wait->t_ms);
        }
    }
    return starts;
}

// Rebuilds the segment list from a recording's MicrophoneEvents — exact
// start/stop timing, so a mid-track mic toggle's segments are each
// played from and stopped at the right instant. `segment_n` counts
// on-events since the track's own start, the same count
// start_audio_track() used when naming that segment's file.
std::vector<AudioSegment> compute_audio_segments(const std::vector<RecordEvent>& events) {
    std::vector<AudioSegment> segments;
    int track_n = 1;
    int segment_n = -1;
    std::optional<std::size_t> open_index;
    for (const auto& event : events) {
        if (auto* wait = std::get_if<WaitEvent>(&event)) {
            if (open_index) {
                segments[*open_index].end_ms = wait->t_ms;
                open_index.reset();
            }
            ++track_n;
            segment_n = -1;
            continue;
        }
        auto* mic = std::get_if<MicrophoneEvent>(&event);
        if (!mic) {
            continue;
        }
        if (mic->on) {
            ++segment_n;
            segments.push_back({mic->t_ms,
                                 std::numeric_limits<std::int32_t>::max(),
                                 track_n, segment_n});
            open_index = segments.size() - 1;
        } else if (open_index) {
            segments[*open_index].end_ms = mic->t_ms;
            open_index.reset();
        }
    }
    return segments;
}

// The segment containing `ms`, or nullptr if the mic wasn't recording at
// that instant.
const AudioSegment* audio_segment_for_ms(const std::vector<AudioSegment>& segments,
                                          std::int32_t ms) {
    for (const auto& segment : segments) {
        if (ms >= segment.start_ms && ms < segment.end_ms) {
            return &segment;
        }
    }
    return nullptr;
}

// Checked when the user turns "Transcode Audio to Opus" on in
// Preferences — turning it on checks the libraries are actually there
// rather than silently leaving it on and failing at the next Stop.
// gst_element_factory_find() returns a real (reffed) factory if the
// plugin is registered; opusenc/oggmux ship in gst-plugins-base, which
// some minimal installs omit, unlike gstreamer-1.0 itself.
bool opus_encoding_available() {
    auto* enc = gst_element_factory_find("opusenc");
    auto* mux = gst_element_factory_find("oggmux");
    bool available = enc && mux;
    if (enc) {
        gst_object_unref(enc);
    }
    if (mux) {
        gst_object_unref(mux);
    }
    return available;
}

namespace {

// Ogg Opus: royalty-free (IETF RFC 6716, unlike MP3/AAC), and
// purpose-built for speech at low bitrates. Async, non-blocking encode
// — watched via gst_bus_add_watch() rather than a blocking
// `gst_bus_timed_pop_filtered()` call: a synchronous encode would block
// the entire GTK main thread, harmless at the measured ~470x-realtime
// speed for a normal segment, but a long one would freeze the app.
// `on_done` is called exactly once, with `true` on EOS or `false` on an
// error or a 30s bail-out (a real hang here means something is
// fundamentally wrong, not just slow).
struct AsyncOpusEncode {
    GstElement* pipeline = nullptr;
    guint bus_watch_id = 0;
    guint timeout_id = 0;
    std::function<void(bool)> on_done;
    bool done = false;
};

void finish_async_opus_encode(AsyncOpusEncode* state, bool ok) {
    if (state->done) {
        return;
    }
    state->done = true;
    if (state->timeout_id != 0) {
        g_source_remove(state->timeout_id);
    }
    g_source_remove(state->bus_watch_id);
    gst_element_set_state(state->pipeline, GST_STATE_NULL);
    gst_object_unref(state->pipeline);
    state->on_done(ok);
    delete state;
}

gboolean on_async_opus_encode_bus_message(GstBus*, GstMessage* msg, gpointer user_data) {
    auto* state = static_cast<AsyncOpusEncode*>(user_data);
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            finish_async_opus_encode(state, true);
            return FALSE;  // source no longer needed; state is freed
        case GST_MESSAGE_ERROR:
            finish_async_opus_encode(state, false);
            return FALSE;
        default:
            return TRUE;  // keep watching for EOS/ERROR
    }
}

gboolean on_async_opus_encode_timeout(gpointer user_data) {
    auto* state = static_cast<AsyncOpusEncode*>(user_data);
    state->timeout_id = 0;
    g_message("blind-sight: async Opus encode timed out after 30s");
    finish_async_opus_encode(state, false);
    return G_SOURCE_REMOVE;
}

}  // namespace

void encode_wav_to_opus_async(const std::string& wav_path, const std::string& opus_path,
                               std::function<void(bool)> on_done) {
    GError* error = nullptr;
    auto* pipeline = gst_parse_launch(
        "filesrc name=src ! wavparse ! audioconvert ! audioresample ! "
        "opusenc ! oggmux ! filesink name=sink",
        &error);
    if (!pipeline) {
        if (error) {
            g_error_free(error);
        }
        on_done(false);
        return;
    }
    auto* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    g_object_set(src, "location", wav_path.c_str(), nullptr);
    g_object_unref(src);
    auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    g_object_set(sink, "location", opus_path.c_str(), nullptr);
    g_object_unref(sink);

    auto* state = new AsyncOpusEncode();
    state->pipeline = pipeline;
    state->on_done = std::move(on_done);

    auto* bus = gst_element_get_bus(pipeline);
    state->bus_watch_id =
        gst_bus_add_watch(bus, on_async_opus_encode_bus_message, state);
    gst_object_unref(bus);
    state->timeout_id = g_timeout_add_seconds(30, on_async_opus_encode_timeout, state);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
}
