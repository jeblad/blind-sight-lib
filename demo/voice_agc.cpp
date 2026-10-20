// SPDX-License-Identifier: GPL-3.0-only
//
// voice_agc.cpp — a LADSPA plugin, loaded by PipeWire's filter-chain module
// (one instance per participant microphone, see audio_routing.cpp), that
// levels speech with a slow automatic gain control, then applies the
// participant's voice volume, mute, and a safety limiter.
//
// Order matters: the AGC sits before the volume so it never undoes a
// volume the participant chose. Mute cuts the output only. While muted,
// the AGC gain is frozen, but the noise-floor and short-term level
// estimates keep following the microphone, so they are settled when the
// voice comes back. That knowingly bends the promise that a muted
// microphone is used for nothing; nothing muted is heard or recorded. It
// levels speech, it doesn't compress:
// the speech level is measured over seconds and the gain moves slowly,
// following the speaker rather than individual words. It adapts only
// while the speaker is talking (level well above the microphone's own
// noise floor), so pauses don't pump the room noise up, and it freezes
// while muted, so the voice doesn't come back at a jump. Everything runs
// in dB.
//
// It also reports the microphone's level, before any gain, every
// kLevelWindowS to the app (LevelReporter), which logs it with the
// recording. Before the gain because the log is for telling which
// microphone a voice dominates; levelled, every microphone would look
// equally loud. Each report says whether the microphone was muted, and
// the app logs no level for those.

#include <ladspa.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

namespace {

enum Port : unsigned long {
    kTargetDb = 0,
    kAttackS,
    kReleaseS,
    kMaxGainDb,
    kAgcOn,
    kVolumeDb,
    kMute,
    kIn,
    kOut,
    kGainDb,  // output: the AGC gain right now, for display and testing
    kPortCount
};

// Level measurement and gain updates run once per block; the gain
// actually applied is still smoothed per sample (kApplySmoothingS).
constexpr double kBlockS = 0.005;
// Short-term level for speech detection.
constexpr double kShortTermS = 0.05;
// The noise floor follows the level down quickly and up slowly, so it
// settles on the quiet between words, not on the words themselves.
constexpr double kFloorFallS = 0.3;
constexpr double kFloorRiseS = 8.0;
// Talking: short-term level this far above the noise floor, and above an
// absolute floor (a dead-silent input has no speech in it).
constexpr double kSpeechAboveFloorDb = 9.0;
constexpr double kSpeechMinDb = -60.0;
// How quickly the measured speech level follows the speaker.
constexpr double kSpeechLevelS = 1.5;
constexpr double kOutlierDb = 10.0;
// Smooths every gain change into the signal, so neither the AGC, a volume
// move nor a mute ever clicks.
constexpr double kApplySmoothingS = 0.02;
// Safety only: catches the odd peak the slow AGC lets through.
constexpr double kLimitCeiling = 0.891;  // -1 dBFS
constexpr double kLimitReleaseS = 0.1;

constexpr double kSilenceDb = -120.0;

// The reported level: mean energy over this window, before any gain.
constexpr double kLevelWindowS = 0.05;

// Sends level reports to the app over a local datagram socket, named by
// the BLIND_SIGHT_LEVEL_SOCKET environment variable, tagged with the
// microphone's id from BLIND_SIGHT_LEVEL_ID. The audio thread only pushes
// into a lock-free single-producer ring; a thread of its own does the
// I/O. Reports are dropped, never waited for, if the ring is full or the
// app isn't listening.
class LevelReporter {
public:
    struct Sample {
        std::int64_t t_ns;  // CLOCK_MONOTONIC, the app's steady_clock
        float level_db;
        bool muted;
    };

    static std::unique_ptr<LevelReporter> from_environment() {
        const char* path = std::getenv("BLIND_SIGHT_LEVEL_SOCKET");
        const char* id = std::getenv("BLIND_SIGHT_LEVEL_ID");
        if (!path || !id || std::strlen(path) >= sizeof(sockaddr_un::sun_path)) {
            return nullptr;
        }
        int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            return nullptr;
        }
        return std::unique_ptr<LevelReporter>(new LevelReporter(fd, path, id));
    }

    ~LevelReporter() {
        stop_.store(true);
        thread_.join();
        close(fd_);
    }

    // Real-time safe: no allocation, no locks, no system calls.
    void push(const Sample& sample) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t next = (head + 1) % kRing;
        if (next == tail_.load(std::memory_order_acquire)) {
            return;  // full: drop
        }
        ring_[head] = sample;
        head_.store(next, std::memory_order_release);
    }

private:
    static constexpr std::size_t kRing = 1024;
    static constexpr auto kSendInterval = std::chrono::milliseconds(100);

    LevelReporter(int fd, const char* path, const char* id) : fd_(fd), id_(id) {
        address_.sun_family = AF_UNIX;
        std::strcpy(address_.sun_path, path);
        thread_ = std::thread([this]() {
            while (!stop_.load()) {
                std::this_thread::sleep_for(kSendInterval);
                send_pending();
            }
            send_pending();
        });
    }

    // One datagram: the id on the first line, then "t_ns level_db muted"
    // per sample.
    void send_pending() {
        std::string message = id_ + "\n";
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        std::size_t head = head_.load(std::memory_order_acquire);
        if (tail == head) {
            return;
        }
        char line[64];
        while (tail != head) {
            const Sample& s = ring_[tail];
            std::snprintf(line, sizeof line, "%lld %.1f %d\n", static_cast<long long>(s.t_ns),
                          static_cast<double>(s.level_db), s.muted ? 1 : 0);
            message += line;
            tail = (tail + 1) % kRing;
        }
        tail_.store(tail, std::memory_order_release);
        sendto(fd_, message.data(), message.size(), MSG_DONTWAIT | MSG_NOSIGNAL,
               reinterpret_cast<const sockaddr*>(&address_), sizeof address_);
    }

    int fd_;
    sockaddr_un address_{};
    std::string id_;
    std::array<Sample, kRing> ring_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

std::int64_t monotonic_ns() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);  // vDSO: no system call, real-time safe
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

double to_db(double mean_square) {
    return mean_square > 1e-12 ? 10.0 * std::log10(mean_square) : kSilenceDb;
}

double from_db(double db) {
    return std::pow(10.0, db / 20.0);
}

// One-pole smoothing factor for a step of `dt` towards a target with time
// constant `tau`.
double approach(double dt, double tau) {
    return tau > 0.0 ? 1.0 - std::exp(-dt / tau) : 1.0;
}

struct Voice {
    LADSPA_Data* ports[kPortCount] = {};
    double rate = 48000.0;
    std::uint32_t block_len = 240;

    std::uint32_t block_pos = 0;
    double block_energy = 0.0;

    double short_term_db = kSilenceDb;
    double floor_db = -60.0;
    // The speaker's level, averaged in dB over talking blocks only. Each
    // block counts for at most kOutlierDb above the current estimate: in
    // energy, one laugh or cough outweighs seconds of speech and would
    // drag the gain down for as long again; the limiter handles peaks.
    // Starts at the target, so the gain starts at 0 dB.
    double speech_db = 0.0;
    bool speech_db_set = false;
    double agc_gain_db = 0.0;

    double applied_gain = 1.0;
    double limit_env = 0.0;

    std::unique_ptr<LevelReporter> reporter;
    std::uint32_t level_window_len = 2400;
    std::uint32_t level_pos = 0;
    double level_energy = 0.0;

    void update_block(double target_db, double attack_s, double release_s, double max_gain_db,
                      bool adapt) {
        double ms = block_energy / block_len;
        block_energy = 0.0;
        double dt = static_cast<double>(block_len) / rate;

        double level_db = to_db(ms);
        short_term_db += (level_db - short_term_db) * approach(dt, kShortTermS);
        double floor_tau = short_term_db < floor_db ? kFloorFallS : kFloorRiseS;
        floor_db += (short_term_db - floor_db) * approach(dt, floor_tau);

        if (!speech_db_set) {
            speech_db = target_db;
            speech_db_set = true;
        }
        // The block itself must be above the threshold too, not only the
        // smoothed level: that keeps the tail after each word, where the
        // smoothed level still lingers, out of the speech estimate.
        double threshold_db = floor_db + kSpeechAboveFloorDb;
        bool talking = short_term_db > threshold_db && level_db > threshold_db &&
                       short_term_db > kSpeechMinDb;
        if (!adapt || !talking) {
            return;
        }
        double counted_db = std::min(level_db, speech_db + kOutlierDb);
        speech_db += (counted_db - speech_db) * approach(dt, kSpeechLevelS);
        double wanted_db = std::clamp(target_db - speech_db, -max_gain_db, max_gain_db);
        double tau = wanted_db < agc_gain_db ? attack_s : release_s;
        agc_gain_db += (wanted_db - agc_gain_db) * approach(dt, tau);
    }

    void run(unsigned long n) {
        const LADSPA_Data* in = ports[kIn];
        LADSPA_Data* out = ports[kOut];
        double target_db = *ports[kTargetDb];
        double attack_s = *ports[kAttackS];
        double release_s = *ports[kReleaseS];
        double max_gain_db = std::max(0.0, static_cast<double>(*ports[kMaxGainDb]));
        bool agc_on = *ports[kAgcOn] > 0.5f;
        bool muted = *ports[kMute] > 0.5f;
        double volume_db = *ports[kVolumeDb];

        double apply_k = approach(1.0 / rate, kApplySmoothingS);
        double limit_release_k = approach(1.0 / rate, kLimitReleaseS);

        for (unsigned long i = 0; i < n; ++i) {
            double x = in[i];
            block_energy += x * x;
            if (reporter) {
                level_energy += x * x;
                if (++level_pos >= level_window_len) {
                    reporter->push({monotonic_ns(),
                                    static_cast<float>(to_db(level_energy / level_window_len)),
                                    muted});
                    level_pos = 0;
                    level_energy = 0.0;
                }
            }
            if (++block_pos >= block_len) {
                block_pos = 0;
                update_block(target_db, attack_s, release_s, max_gain_db, agc_on && !muted);
            }

            double wanted =
                muted ? 0.0 : from_db((agc_on ? agc_gain_db : 0.0) + volume_db);
            applied_gain += (wanted - applied_gain) * apply_k;
            double y = x * applied_gain;

            double peak = std::fabs(y);
            limit_env = peak > limit_env ? peak : limit_env + (peak - limit_env) * limit_release_k;
            if (limit_env > kLimitCeiling) {
                y *= kLimitCeiling / limit_env;
            }
            out[i] = static_cast<LADSPA_Data>(y);
        }
        if (ports[kGainDb]) {
            *ports[kGainDb] = static_cast<LADSPA_Data>(agc_gain_db);
        }
    }
};

LADSPA_Handle instantiate(const LADSPA_Descriptor*, unsigned long sample_rate) {
    auto* voice = new Voice;
    voice->rate = static_cast<double>(sample_rate);
    voice->block_len = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(kBlockS * voice->rate)));
    voice->level_window_len = std::max<std::uint32_t>(
        1, static_cast<std::uint32_t>(std::lround(kLevelWindowS * voice->rate)));
    voice->reporter = LevelReporter::from_environment();
    return voice;
}

void connect_port(LADSPA_Handle handle, unsigned long port, LADSPA_Data* data) {
    if (port < kPortCount) {
        static_cast<Voice*>(handle)->ports[port] = data;
    }
}

void run(LADSPA_Handle handle, unsigned long sample_count) {
    static_cast<Voice*>(handle)->run(sample_count);
}

void cleanup(LADSPA_Handle handle) {
    delete static_cast<Voice*>(handle);
}

const LADSPA_PortDescriptor kPortDescriptors[kPortCount] = {
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL,
    LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL, LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO,
    LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO, LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL,
};

// Control names are what filter-chain's "control" blocks and pw-cli
// "params" refer to — audio_routing.cpp spells them the same way.
const char* const kPortNames[kPortCount] = {
    "Target (dBFS)", "Attack (s)", "Release (s)", "Max gain (dB)", "AGC",
    "Volume (dB)",   "Mute",       "In",          "Out",           "Gain (dB)",
};

const LADSPA_PortRangeHint kPortHints[kPortCount] = {
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, -40.0f, -6.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0.1f, 30.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0.1f, 60.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE, 0.0f, 40.0f},
    {LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_1, 0.0f, 1.0f},
    {LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_DEFAULT_0, -40.0f,
     10.0f},
    {LADSPA_HINT_TOGGLED | LADSPA_HINT_DEFAULT_0, 0.0f, 1.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
    {0, 0.0f, 0.0f},
};

const LADSPA_Descriptor kDescriptor = {
    /*UniqueID=*/0x42535641,  // "BSVA"; unregistered, only ever loaded by path
    /*Label=*/"blind_sight_voice",
    /*Properties=*/LADSPA_PROPERTY_HARD_RT_CAPABLE,
    /*Name=*/"Blind Sight voice leveller",
    /*Maker=*/"blind-sight-lib",
    /*Copyright=*/"GPL-3.0-only",
    /*PortCount=*/kPortCount,
    /*PortDescriptors=*/kPortDescriptors,
    /*PortNames=*/kPortNames,
    /*PortRangeHints=*/kPortHints,
    /*ImplementationData=*/nullptr,
    /*instantiate=*/instantiate,
    /*connect_port=*/connect_port,
    /*activate=*/nullptr,
    /*run=*/run,
    /*run_adding=*/nullptr,
    /*set_run_adding_gain=*/nullptr,
    /*deactivate=*/nullptr,
    /*cleanup=*/cleanup,
};

}  // namespace

extern "C" const LADSPA_Descriptor* ladspa_descriptor(unsigned long index) {
    return index == 0 ? &kDescriptor : nullptr;
}
