// SPDX-License-Identifier: GPL-3.0-only
//
// audio_routing.hpp — makes the monitoring matrix's mute state and
// device assignments produce live audio, for every currently-unmuted,
// device-resolvable (sink_id, source_id) edge:
//  - a device source (mic) gets its own pw-loopback process;
//  - kSynthesizedSourceId gets one of NoisePulseEngine's output streams
//    (noise_pulse.hpp), linked directly to the sink and adding no
//    buffering of its own — the listener actively testing hears prods
//    with the least latency possible.
// No custom mixing — PipeWire's own graph mixes multiple streams into
// one sink.
//
// One instance for the whole app process lifetime (main.cpp) — it has
// no session-specific state of its own, only whatever MonitoringState
// it's handed on each sync() call.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace blind_sight {

struct MonitoringState;
struct AudioRoutingImpl;
class NoisePulseEngine;

class AudioRoutingEngine {
public:
    // Plays the synthesized sound through `noise_pulse`, which it keeps.
    explicit AudioRoutingEngine(std::shared_ptr<NoisePulseEngine> noise_pulse);
    ~AudioRoutingEngine();
    AudioRoutingEngine(const AudioRoutingEngine&) = delete;
    AudioRoutingEngine& operator=(const AudioRoutingEngine&) = delete;

    // Computes the full desired edge set from `state` fresh, diffs it
    // against whatever's currently running, and starts/stops only what
    // changed — never tears down and rebuilds an edge that's unchanged,
    // to avoid audible clicks on unrelated matrix activity. Call after
    // every mutation that could change the desired edge set: mute-cell
    // toggles, header-button mute overrides, device (re)assignment,
    // participant list changes, and session load. Safe to call
    // repeatedly and with an unchanged state (a no-op diff).
    void sync(const MonitoringState& state);

    // Watches PipeWire for audio devices (ALSA or Bluetooth nodes)
    // appearing or disappearing, and calls `on_change` once things settle,
    // so the caller can re-run sync() with its current state. Call once.
    void watch_devices(std::function<void()> on_change);

    // Volumes in dB (0 is unchanged), applied at once if the device is
    // present and again whenever it reappears. Ids using an Ambient device
    // have none of their own; the Ambient Speaker's/Microphone's apply.
    // The devices' own volumes are never touched, so nothing else playing
    // on them changes.
    //  - Synthesized: what listener `id` hears of the synthesized sound,
    //    set on the synthesized output into their device.
    //  - Participants: what listener `id` hears of everyone's voices, set
    //    on the voice loopbacks into their device.
    //  - Voice (source): what every listener hears from `id`'s own
    //    microphone, applied in its voice leveller.
    void set_synthesized_volume_db(const MonitoringState& state, const std::string& id, double db);
    void set_participants_volume_db(const MonitoringState& state, const std::string& id,
                                    double db);
    void set_source_volume_db(const MonitoringState& state, const std::string& id, double db);

    // The volume last set for `id`, or 0 dB if none.
    double synthesized_volume_db(const std::string& id) const;
    double participants_volume_db(const std::string& id) const;
    double source_volume_db(const std::string& id) const;

    // Every microphone of its own (participants', and the Ambient
    // Microphone) runs through a voice leveller before anyone hears it:
    // slow AGC, then the voice volume, then mute (voice_agc.cpp). These
    // settings are shared by all of them.
    struct AgcSettings {
        // RMS speech level aimed for; -23 dBFS, in line with EBU R 128's
        // -23 LUFS loudness target.
        double target_dbfs = -23.0;
        double attack_s = 0.2;   // how quickly it turns a loud speaker down
        double release_s = 0.5;  // how quickly it turns a quiet speaker up
        double max_gain_db = 20.0;
    };
    void set_agc_settings(const AgcSettings& settings);
    // Whether `id`'s microphone is levelled at all; on unless turned off.
    void set_agc_enabled(const std::string& id, bool enabled);
    bool agc_enabled(const std::string& id) const;
    // "Mute All Microphones": every leveller cuts its output and freezes.
    void set_microphones_muted(bool muted);

    // Each microphone's level before any gain, as its voice leveller
    // measures it: mean energy over 50 ms in dBFS, every 50 ms, while the
    // leveller runs, with whether the microphone was muted. `t_ns` is on
    // the monotonic clock, the one std::chrono::steady_clock uses. Called
    // on the main loop; call once.
    void on_microphone_level(std::function<void(const std::string& id, std::int64_t t_ns,
                                                double level_dbfs, bool muted)>
                                 callback);

    // True if `id` (a participant, kSpeakerSinkId or kAmbientMicSourceId)
    // has a device assigned on the given axis that wasn't present at the
    // last sync(). An id with no device assigned is not disconnected.
    bool is_disconnected(const MonitoringState& state, const std::string& id,
                         bool is_sink) const;

private:
    std::unique_ptr<AudioRoutingImpl> impl_;
};

}  // namespace blind_sight
