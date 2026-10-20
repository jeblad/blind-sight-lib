// SPDX-License-Identifier: GPL-3.0-only
//
// noise_pulse.hpp — real-time colored-noise pulse synthesis, played over
// native PipeWire. See noise_pulse.cpp's top comment for the full
// design reasoning (why PipeWire, not the GStreamer this demo already
// uses elsewhere, and the real-time-safety discipline trigger() relies
// on). First take on the acoustic model's carrier signal: a classic
// ADSR envelope plus a color (brightness) parameter.

#pragma once

#include <memory>
#include <string>

namespace blind_sight {

// One ADSR + color pulse's worth of parameters. Sustain is a *level* in
// classic ADSR terms, but a prod is a discrete, instantaneous event
// with no held/release gesture — so this adds `sustain_ms`, how long
// that level is held before Release begins. `color` is the one-pole
// lowpass filter coefficient applied to the underlying white noise:
// near 1.0 barely filters (bright), near 0.0 filters heavily (dark).
struct NoisePulseParams {
    double attack_ms = 5.0;
    double decay_ms = 50.0;
    double sustain_level = 0.3;  // 0..1
    double sustain_ms = 30.0;
    double release_ms = 100.0;
    double color = 0.6;  // 0 (dark) .. 1 (bright)
    double gain = 0.5;   // linear, 0..1
};

// Defined in noise_pulse.cpp — not nested inside NoisePulseEngine, so its
// own free functions (the PipeWire process() callback in particular) can
// see it without needing friend declarations.
struct NoisePulseImpl;

// Ceiling on the `delay_ms` trigger() accepts — must stay in sync with
// the Preferences-facing noise-pulse-max-delay-ms GSettings range
// (gschema.xml). This is only the fixed-size delay line's own
// pre-allocated capacity (real-time-safe — never resized after start()).
constexpr unsigned int kNoisePulseMaxDelayMs = 30000;

// Prefix of the node.name of each output stream (see add_output()).
constexpr const char* kNoisePulseNodeName = "blind-sight-noise-pulse";

// Owns a dedicated PipeWire thread loop and one playback stream per
// output device, each with its own gain and its own pool of voices (see
// noise_pulse.cpp). Every trigger() plays on all of them.
class NoisePulseEngine {
public:
    NoisePulseEngine();
    ~NoisePulseEngine();
    NoisePulseEngine(const NoisePulseEngine&) = delete;
    NoisePulseEngine& operator=(const NoisePulseEngine&) = delete;

    // Starts the PipeWire thread loop. Returns false (after logging why)
    // if it failed — never fatal, matching this app's existing GStreamer
    // availability-probing convention.
    bool start();

    // Starts a stream playing into the sink node `sink_node`, at
    // `gain_db` relative to the pulse's own gain. Links directly to that
    // sink and nowhere else, never falling back to another device. Returns
    // the stream's node.name, or empty (after logging why) if it couldn't
    // be created. A second call for the same sink only updates the gain.
    std::string add_output(const std::string& sink_node, double gain_db);
    void remove_output(const std::string& sink_node);
    void set_output_gain_db(const std::string& sink_node, double gain_db);

    // Plays the pulse immediately (dry), and — if delay_ms > 0 — mixes
    // in an exact, sample-accurate echo of this *same* pulse's rendered
    // audio that many milliseconds later, via a real internal delay
    // line, so the echo is literally the dry pulse's own waveform, not
    // a second independently-synthesized one. Real-time safe to call
    // from the main thread: never blocks, never allocates.
    void trigger(const NoisePulseParams& params, unsigned int delay_ms = 0);

private:
    std::unique_ptr<NoisePulseImpl> impl_;
};

}  // namespace blind_sight
