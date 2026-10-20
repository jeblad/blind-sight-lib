// SPDX-License-Identifier: GPL-3.0-only
//
// noise_pulse.cpp — real-time colored-noise pulse synthesis over native
// PipeWire, the acoustic model's first-take carrier signal (see
// noise_pulse.hpp for the ADSR+color parameter shape).
//
// Native libpipewire, not GStreamer (used elsewhere for file-based mic
// capture/Opus transcode/playback): this needs a persistent, continuous
// low-latency stream, not a pipeline built and torn down per file
// operation.
//
// Real-time-safety discipline: on_process() below never allocates,
// locks, or calls anything that might block. trigger() (called from the
// GTK main thread) hands new parameters to the audio thread through a
// small, fixed, pre-allocated ring buffer (kParamSlots) plus two atomic
// indices — never a heap allocation or a mutex on either side.
//
// A proper queue, not a single "latest wins" slot: on_process()'s poll
// interval isn't guaranteed to beat every trigger spacing (a head-prod
// sweep fires nine triggers 50ms apart), so a single overwritten slot
// could silently drop a trigger that arrives before the previous one
// was polled.
//
// The echo is a real recording of the dry pulse, replayed later, not a
// second, independent trigger() call with the same parameters — two
// separate noise draws would only be statistically similar, not
// identical, and a GLib main-loop timer isn't sample-accurate.
//
// Not a full delay line either: each voice (kVoiceCount below) only
// ever needs to hold one pulse's worth of audio at a time, so
// `recording` is sized to the worst-case *pulse duration*, not the
// worst-case *delay* (up to 30 seconds).
//
// Polyphonic — a small, fixed pool of kVoiceCount independent voices,
// not one shared monophonic voice: a second prod firing before the
// first one's echo had played (exactly what a head-prod sweep does)
// would otherwise silently abandon the first one's pending echo. Each
// trigger() claims the next voice in a fixed round-robin pool, so
// independent prods never cancel each other's echo, and a voice's
// buffer is reused once its pulse-plus-echo cycle has finished.
// kVoiceCount=16 comfortably covers a full 9-step sweep with headroom;
// every voice's buffer is pre-allocated once, at construction, never
// resized.
//
// One stream per output device, each with its own voice pool and trigger
// queue, never one stream linked to several devices: a stream has a
// single volume, and the only other place to set a listener's volume was
// their device itself, which turned everything else playing on it up or
// down too. Separate devices also run on separate clocks, so each stream
// renders the pulse itself rather than sharing one rendering. Every
// trigger carries its own noise seed, so all outputs still play the
// identical pulse.

#include "noise_pulse.hpp"

#include <glib.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace blind_sight {

namespace {

constexpr uint32_t kSampleRate = 48000;

// Interleaved FL/FR. The output is a dummy-head stereo scene in the
// making; until spatial rendering exists both channels carry the same
// signal.
constexpr uint32_t kChannels = 2;

// Ring buffer capacity for the trigger queue — never freed/reallocated.
// 16 comfortably outlasts a full head-prod sweep (9 steps) queuing up
// before a single on_process() poll, the worst realistic case.
constexpr int kParamSlots = 16;

// Independent voice pool size — see this file's top comment. Kept
// equal to kParamSlots, though nothing requires them to match.
constexpr int kVoiceCount = 16;

// Recording-buffer capacity, in samples, *per voice* — sized for the
// worst-case *pulse* duration (attack+decay+sustain+release, each at
// its own GSettings-declared max: 500+500+1000+2000 ms), with a little
// headroom, not the worst-case delay. Allocated once per voice, at
// construction, never resized afterward.
constexpr uint32_t kMaxPulseDurationMs = 5000;
constexpr uint32_t kRecordingBufferSamples =
    (kMaxPulseDurationMs * kSampleRate) / 1000;

enum class Stage { Idle, Attack, Decay, Sustain, Release };

// xorshift32 — fast, allocation-free, lock-free: safe to call from the
// real-time audio thread, unlike std::mt19937/std::uniform_real_
// distribution (heavier, and not guaranteed allocation-free).
uint32_t next_xorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void on_process(void* userdata);

constexpr struct pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_process,
};

}  // namespace

// One independent pulse-plus-echo voice — see this file's top comment
// for why there's a fixed pool of these. Everything here is
// audio-thread-only (the main thread never touches a Voice directly,
// only its Output's trigger queue below), so none of it needs
// synchronization.
struct Voice {
    NoisePulseParams current_params;
    Stage stage = Stage::Idle;
    uint64_t stage_sample = 0;
    double envelope = 0.0;
    double release_start_level = 0.0;
    double filter_state = 0.0;
    uint32_t noise_state = 0x9e3779b9u;  // never 0 — xorshift's one fixed point; reseeded per trigger

    // The pulse recording itself. `record_pos` is both the live write
    // position while this voice is actively recording (stage != Idle)
    // *and*, once it stops advancing (stage == Idle), the frozen length
    // of what got recorded — one counter serves both purposes.
    std::vector<float> recording =
        std::vector<float>(kRecordingBufferSamples, 0.0f);
    uint64_t record_pos = 0;

    // Echo playback scheduling for this voice's own current pulse.
    uint64_t samples_since_trigger = 0;
    uint64_t echo_delay_samples = 0;
    bool echo_pending = false;
    uint64_t echo_playback_pos = 0;

    void advance_envelope();
    // Dry + echo for this voice alone, *not* clamped — on_process()
    // sums every voice together first and clamps once, on the mix.
    float render_one_sample();
};

void Voice::advance_envelope() {
    switch (stage) {
        case Stage::Idle:
            envelope = 0.0;
            return;
        case Stage::Attack: {
            auto n = static_cast<uint64_t>(std::max<int64_t>(
                1, std::llround(current_params.attack_ms / 1000.0 * kSampleRate)));
            envelope = static_cast<double>(stage_sample) / static_cast<double>(n);
            if (++stage_sample >= n) {
                stage = Stage::Decay;
                stage_sample = 0;
            }
            return;
        }
        case Stage::Decay: {
            auto n = static_cast<uint64_t>(std::max<int64_t>(
                1, std::llround(current_params.decay_ms / 1000.0 * kSampleRate)));
            double t = static_cast<double>(stage_sample) / static_cast<double>(n);
            envelope = 1.0 + t * (current_params.sustain_level - 1.0);
            if (++stage_sample >= n) {
                stage = Stage::Sustain;
                stage_sample = 0;
            }
            return;
        }
        case Stage::Sustain: {
            auto n = static_cast<uint64_t>(std::max<int64_t>(
                0, std::llround(current_params.sustain_ms / 1000.0 * kSampleRate)));
            envelope = current_params.sustain_level;
            if (++stage_sample >= n) {
                stage = Stage::Release;
                stage_sample = 0;
                release_start_level = envelope;
            }
            return;
        }
        case Stage::Release: {
            auto n = static_cast<uint64_t>(std::max<int64_t>(
                1, std::llround(current_params.release_ms / 1000.0 * kSampleRate)));
            double t = static_cast<double>(stage_sample) / static_cast<double>(n);
            envelope = release_start_level * (1.0 - t);
            if (++stage_sample >= n) {
                stage = Stage::Idle;
                stage_sample = 0;
                envelope = 0.0;
            }
            return;
        }
    }
}

float Voice::render_one_sample() {
    // Dry synthesis — only while this voice's pulse is actually active.
    // Idle stays completely inert (no noise draw, no filter/envelope
    // update) until this voice is next claimed by a trigger.
    float dry = 0.0f;
    if (stage != Stage::Idle) {
        uint32_t r = next_xorshift32(noise_state);
        float white = static_cast<float>(r) / static_cast<float>(UINT32_MAX) * 2.0f - 1.0f;
        // One-pole lowpass — "color" *is* the filter coefficient
        // directly: near 1.0 barely filters (bright/white), near 0.0
        // filters heavily (dark/brown). The user sets the target
        // brightness; this is the filter that actually produces it.
        double a = std::clamp(current_params.color, 0.0, 1.0);
        filter_state = filter_state + a * (static_cast<double>(white) - filter_state);
        advance_envelope();
        dry = static_cast<float>(filter_state * envelope * current_params.gain);
        // Record it — bounds-checked defensively (kRecordingBufferSamples
        // comfortably covers every real ADSR combination, so this
        // should never actually trip) rather than writing out of bounds.
        if (record_pos < recording.size()) {
            recording[record_pos] = dry;
        }
        ++record_pos;
    }

    // Echo: replay the recording once its delay has elapsed. Reads are
    // gated on echo_playback_pos < record_pos — record_pos is both "how
    // much has been recorded so far" while active and the frozen final
    // length once Idle, so this correctly handles a delay shorter than
    // the pulse's own duration (playback "chases" the still-recording
    // source instead of reading stale samples ahead of it).
    float echo = 0.0f;
    if (echo_pending) {
        if (samples_since_trigger >= echo_delay_samples) {
            if (echo_playback_pos < record_pos) {
                echo = recording[echo_playback_pos];
                ++echo_playback_pos;
            } else if (stage == Stage::Idle) {
                echo_pending = false;  // caught up, and nothing more coming
            }
        }
        ++samples_since_trigger;
    }

    return dry + echo;
}

// One output device's stream. The main thread creates and destroys it,
// and only ever touches the trigger queue and `gain` in between; the
// voices belong to the audio thread.
struct Output {
    struct pw_stream* stream = nullptr;
    std::string node_name;

    // Parameter hand-off — a lock-free SPSC ring buffer, not a single
    // "latest wins" mailbox slot (see this file's top comment).
    // delay_samples_slots and seed_slots are parallel arrays, filled by
    // trigger() on the main thread.
    NoisePulseParams param_slots[kParamSlots];
    uint64_t delay_samples_slots[kParamSlots] = {};
    uint32_t seed_slots[kParamSlots] = {};
    std::atomic<unsigned int> write_index{0};
    std::atomic<unsigned int> read_index{0};

    // Linear, applied to the mixed voices before clamping.
    std::atomic<float> gain{1.0f};

    // Round-robin through the fixed pool; audio-thread-only.
    std::array<Voice, kVoiceCount> voices;
    unsigned int next_voice = 0;
};

struct NoisePulseImpl {
    struct pw_thread_loop* thread_loop = nullptr;
    // Keyed by sink node.name. Main-thread-only.
    std::map<std::string, std::unique_ptr<Output>> outputs;
    unsigned int next_output = 1;
    uint32_t seed_state = 0x9e3779b9u;
};

namespace {

float gain_from_db(double db) {
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

void on_process(void* userdata) {
    auto* out = static_cast<Output*>(userdata);
    struct pw_buffer* b = pw_stream_dequeue_buffer(out->stream);
    if (!b) {
        return;
    }
    struct spa_buffer* buf = b->buffer;
    auto* dst = static_cast<float*>(buf->datas[0].data);
    if (!dst) {
        pw_stream_queue_buffer(out->stream, b);
        return;
    }

    constexpr uint32_t kStride = sizeof(float) * kChannels;
    uint32_t n_frames = buf->datas[0].maxsize / kStride;
    if (b->requested != 0 && b->requested < n_frames) {
        n_frames = static_cast<uint32_t>(b->requested);
    }

    // Drain every trigger queued since the last poll — real-time safe,
    // see this file's top comment. Each claims the next voice in the
    // pool round-robin, independent of whatever that voice was doing
    // before — a genuinely new pulse-plus-echo cycle starts on it, not
    // layered onto the old one.
    unsigned int write = out->write_index.load(std::memory_order_acquire);
    unsigned int read = out->read_index.load(std::memory_order_relaxed);
    while (read != write) {
        unsigned int slot = read % kParamSlots;
        Voice& voice = out->voices[out->next_voice % kVoiceCount];
        ++out->next_voice;
        voice.current_params = out->param_slots[slot];
        voice.stage = Stage::Attack;
        voice.stage_sample = 0;
        voice.filter_state = 0.0;
        voice.noise_state = out->seed_slots[slot];
        voice.record_pos = 0;
        voice.samples_since_trigger = 0;
        voice.echo_delay_samples = out->delay_samples_slots[slot];
        // 0 means "dry only, no echo" — without this check, a zero
        // delay would still set echo_pending and the echo would
        // immediately start "chasing" the dry signal one sample behind.
        voice.echo_pending = voice.echo_delay_samples > 0;
        voice.echo_playback_pos = 0;
        ++read;
    }
    out->read_index.store(read, std::memory_order_release);

    float gain = out->gain.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n_frames; ++i) {
        float mixed = 0.0f;
        for (auto& voice : out->voices) {
            mixed += voice.render_one_sample();
        }
        float sample = std::clamp(mixed * gain, -1.0f, 1.0f);
        for (uint32_t c = 0; c < kChannels; ++c) {
            dst[i * kChannels + c] = sample;
        }
    }

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<int32_t>(kStride);
    buf->datas[0].chunk->size = n_frames * kStride;
    pw_stream_queue_buffer(out->stream, b);
}

}  // namespace

NoisePulseEngine::NoisePulseEngine() : impl_(std::make_unique<NoisePulseImpl>()) {}

NoisePulseEngine::~NoisePulseEngine() {
    if (!impl_->thread_loop) {
        return;
    }
    pw_thread_loop_lock(impl_->thread_loop);
    for (auto& [sink, out] : impl_->outputs) {
        pw_stream_destroy(out->stream);
    }
    pw_thread_loop_unlock(impl_->thread_loop);
    pw_thread_loop_stop(impl_->thread_loop);
    pw_thread_loop_destroy(impl_->thread_loop);
}

bool NoisePulseEngine::start() {
    impl_->thread_loop = pw_thread_loop_new("blind-sight-noise-pulse", nullptr);
    if (!impl_->thread_loop) {
        g_message("blind-sight: noise pulse: pw_thread_loop_new() failed");
        return false;
    }
    if (pw_thread_loop_start(impl_->thread_loop) < 0) {
        g_message("blind-sight: noise pulse: pw_thread_loop_start() failed");
        pw_thread_loop_destroy(impl_->thread_loop);
        impl_->thread_loop = nullptr;
        return false;
    }
    return true;
}

std::string NoisePulseEngine::add_output(const std::string& sink_node, double gain_db) {
    if (!impl_->thread_loop) {
        return "";
    }
    auto existing = impl_->outputs.find(sink_node);
    if (existing != impl_->outputs.end()) {
        set_output_gain_db(sink_node, gain_db);
        return existing->second->node_name;
    }
    auto out = std::make_unique<Output>();
    out->gain.store(gain_from_db(gain_db), std::memory_order_relaxed);
    out->node_name = std::string(kNoisePulseNodeName) + "-" + std::to_string(impl_->next_output++);
    const std::string& node_name = out->node_name;

    pw_thread_loop_lock(impl_->thread_loop);

    // Linked by WirePlumber straight to `sink_node`, which also converts
    // to the sink's own channel layout (a mono speaker gets a downmix).
    // Without dont-reconnect/dont-fallback a stream whose sink goes away
    // would move to the default device. WirePlumber's remembered stream
    // volume and target would override this engine's own.
    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Accessibility", PW_KEY_NODE_NAME, node_name.c_str(),
        PW_KEY_TARGET_OBJECT, sink_node.c_str(), "node.dont-reconnect", "true",
        "node.dont-fallback", "true", "state.restore-props", "false",
        "state.restore-target", "false", nullptr);
    out->stream = pw_stream_new_simple(pw_thread_loop_get_loop(impl_->thread_loop),
                                       node_name.c_str(), props, &kStreamEvents, out.get());
    if (!out->stream) {
        g_message("blind-sight: noise pulse: pw_stream_new_simple() failed for %s",
                  sink_node.c_str());
        pw_thread_loop_unlock(impl_->thread_loop);
        return "";
    }

    uint8_t pod_buffer[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    struct spa_audio_info_raw info {};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.rate = kSampleRate;
    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(
        out->stream, SPA_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                           PW_STREAM_FLAG_MAP_BUFFERS |
                                           PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    if (res < 0) {
        pw_stream_destroy(out->stream);
        pw_thread_loop_unlock(impl_->thread_loop);
        g_message("blind-sight: noise pulse: pw_stream_connect() failed for %s: %s",
                  sink_node.c_str(), spa_strerror(res));
        return "";
    }
    pw_thread_loop_unlock(impl_->thread_loop);
    std::string result = node_name;
    impl_->outputs[sink_node] = std::move(out);
    return result;
}

void NoisePulseEngine::remove_output(const std::string& sink_node) {
    auto it = impl_->outputs.find(sink_node);
    if (it == impl_->outputs.end()) {
        return;
    }
    // Once destroyed under the lock, its process callback can't run again,
    // so the Output can go.
    pw_thread_loop_lock(impl_->thread_loop);
    pw_stream_destroy(it->second->stream);
    pw_thread_loop_unlock(impl_->thread_loop);
    impl_->outputs.erase(it);
}

void NoisePulseEngine::set_output_gain_db(const std::string& sink_node, double gain_db) {
    auto it = impl_->outputs.find(sink_node);
    if (it != impl_->outputs.end()) {
        it->second->gain.store(gain_from_db(gain_db), std::memory_order_relaxed);
    }
}

void NoisePulseEngine::trigger(const NoisePulseParams& params, unsigned int delay_ms) {
    uint64_t delay_samples =
        (static_cast<uint64_t>(std::min(delay_ms, kNoisePulseMaxDelayMs)) * kSampleRate) /
        1000;
    uint32_t seed = next_xorshift32(impl_->seed_state);
    for (auto& [sink, out] : impl_->outputs) {
        // Single producer (only ever called from the GTK main thread), so
        // read-modify-write on write_index here needs no CAS loop —
        // nothing else ever advances it.
        unsigned int write = out->write_index.load(std::memory_order_relaxed);
        unsigned int read = out->read_index.load(std::memory_order_acquire);
        if (write - read >= static_cast<unsigned int>(kParamSlots)) {
            // Queue full (on_process() hasn't polled in a while) — drop
            // the *oldest* still-pending trigger rather than the new one;
            // it's already stale/superseded by everything queued after it.
            out->read_index.store(read + 1, std::memory_order_release);
        }
        unsigned int slot = write % kParamSlots;
        out->param_slots[slot] = params;
        out->delay_samples_slots[slot] = delay_samples;
        out->seed_slots[slot] = seed;
        out->write_index.store(write + 1, std::memory_order_release);
    }
}

}  // namespace blind_sight
