// SPDX-License-Identifier: GPL-3.0-only
//
// audio_routing.cpp — see audio_routing.hpp for scope.

#include "audio_routing.hpp"

#include "monitoring.hpp"
#include "noise_pulse.hpp"

#include <giomm/datainputstream.h>
#include <giomm/subprocess.h>
#include <giomm/socket.h>
#include <giomm/socketsource.h>
#include <giomm/subprocesslauncher.h>
#include <giomm/unixsocketaddress.h>
#include <glaze/glaze.hpp>
#include <glib.h>
#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <cmath>
#include <csignal>
#include <cstdint>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace blind_sight {

namespace {

// (sink node, source node). Edges are keyed by what they actually connect,
// not by matrix cell: participants sharing a device (several on the
// Ambient Speaker, say) map many cells onto one pair, and that pair must
// be one link or loopback — PipeWire refuses a second identical link, and
// two identical loopbacks would double the sound.
using EdgeKey = std::pair<std::string, std::string>;

// Each edge is its own `pw-loopback` process, never an in-process
// GStreamer pipeline. GStreamer's pipewire elements share one PipeWire
// thread-loop per process, and stopping one while another runs can
// deadlock the calling thread (confirmed by gdb backtrace: the loop
// thread waits in a libgstpipewire callback while holding the loop lock
// the stopping thread needs) — on the GTK main thread that froze the app.
// A separate process can only ever be signalled, never waited on. It
// also leaves clock drift between independently clocked devices to
// PipeWire's adaptive resampling, instead of buffers that grow or drop.
//
// 10 ms is the loopback's own buffering. A Bluetooth sink adds its codec
// latency on top (typically 100 ms or more), which nothing here reduces.
constexpr int kLoopbackLatencyMs = 10;

// Without these, WirePlumber relinks a stream whose target is missing or
// disappears to the default device — a mic would start playing into
// whatever speaker happens to be the default.
constexpr const char* kLoopbackStreamProps =
    "{ node.dont-reconnect = true node.dont-fallback = true }";

// `label` names one matrix cell that wants this edge, for the log.
// `generation` identifies this particular start of the edge to
// asynchronous callbacks.
struct RunningEdge {
    Glib::RefPtr<Gio::Subprocess> proc;  // loopback edges only
    bool is_synth = false;               // synthesized edges: a NoisePulseEngine output, no process
    std::uint64_t generation = 0;
    std::string label;
    // The node.name of the stream playing into the sink: the loopback's
    // playback side, or the synthesized output.
    std::string playback_node;
    // Loopback edges: whether the watcher has seen the playback stream
    // appear, and the volume set on it (a fresh stream starts at 0 dB).
    // pw-cli reports success even for a node that doesn't exist yet, so a
    // volume is only set once the stream is known to be there.
    bool playback_seen = false;
    double applied_db = 0.0;
};

// One microphone's voice leveller: a PipeWire filter-chain process running
// voice_agc.cpp between the microphone and a virtual source, which every
// listener's loopback captures from instead of the microphone itself.
// Being a filter-chain, it leaves clock drift to PipeWire, as pw-loopback
// does; being its own process, it can only be signalled, never waited on.
// It runs whenever the microphone is present, listened to or not, so the
// slow AGC has already settled by the time anyone hears it.
struct VoiceProcessor {
    Glib::RefPtr<Gio::Subprocess> proc;
    std::string mic_node;
    std::string node_name;  // the virtual source; the capture side is "<node_name>.capture"
    std::string conf_path;
    std::string applied_params;  // last controls sent with pw-cli
};

// stdout silenced: pw-cli prints every parameter object it sets, which
// would flood the terminal; stderr is kept for real errors.
Glib::RefPtr<Gio::Subprocess> spawn(const std::vector<std::string>& argv) {
    try {
        return Gio::Subprocess::create(argv, Gio::Subprocess::Flags::STDOUT_SILENCE);
    } catch (const Glib::Error& e) {
        g_message("blind-sight: failed to run %s: %s", argv.front().c_str(), e.what());
        return {};
    }
}

// Runs pw-link invocations strictly one after another. Launched
// concurrently, a quick mute/unmute could have its unlink land after the
// relink and leave an unmuted edge silent.
class LinkQueue : public std::enable_shared_from_this<LinkQueue> {
public:
    using Done = std::function<void(bool ok)>;

    void push(std::vector<std::string> argv, Done done = {}) {
        jobs_.push_back({std::move(argv), std::move(done)});
        run_next();
    }

private:
    struct Job {
        std::vector<std::string> argv;
        Done done;
    };

    void run_next() {
        if (busy_ || jobs_.empty()) {
            return;
        }
        Job job = std::move(jobs_.front());
        jobs_.pop_front();
        auto proc = spawn(job.argv);
        if (!proc) {
            if (job.done) {
                job.done(false);
            }
            run_next();
            return;
        }
        busy_ = true;
        std::weak_ptr<LinkQueue> weak_self = shared_from_this();
        proc->wait_async([proc, weak_self, done = std::move(job.done)](
                             Glib::RefPtr<Gio::AsyncResult>& result) {
            bool ok = false;
            try {
                proc->wait_finish(result);
                ok = proc->get_successful();
            } catch (const Glib::Error&) {
            }
            auto self = weak_self.lock();
            if (!self) {
                return;
            }
            self->busy_ = false;
            if (done) {
                done(ok);
            }
            self->run_next();
        });
    }

    std::deque<Job> jobs_;
    bool busy_ = false;
};

void stop_edge(const EdgeKey& key, RunningEdge& edge, NoisePulseEngine& noise_pulse) {
    if (edge.is_synth) {
        noise_pulse.remove_output(key.first);
        edge.is_synth = false;
    }
    if (edge.proc) {
        edge.proc->send_signal(SIGTERM);
        edge.proc.reset();
    }
}

// The exit callback can fire after the engine is destroyed; `alive`
// expiring with the engine tells it not to touch `running` then.
RunningEdge start_loopback_edge(std::map<EdgeKey, RunningEdge>* running,
                                 const std::shared_ptr<bool>& alive, const EdgeKey& key,
                                 const std::string& label, std::uint64_t generation) {
    const std::string& sink_node = key.first;
    const std::string& source_node = key.second;
    std::string loop_name = "blind-sight-route-" + std::to_string(generation);
    RunningEdge edge;
    std::vector<std::string> argv = {"pw-loopback",
                                      "--name",
                                      loop_name,
                                      "--latency",
                                      std::to_string(kLoopbackLatencyMs),
                                      "--capture",
                                      source_node,
                                      "--playback",
                                      sink_node,
                                      "--capture-props",
                                      kLoopbackStreamProps,
                                      "--playback-props",
                                      kLoopbackStreamProps};
    g_message("blind-sight: audio routing edge (%s) starting: %s -> %s", label.c_str(),
              source_node.c_str(), sink_node.c_str());
    try {
        auto launcher = Gio::SubprocessLauncher::create(Gio::Subprocess::Flags::NONE);
        // A loopback orphaned by a crashed or killed app would keep a mic
        // playing into a speaker with nothing left to stop it.
        launcher->set_child_setup([]() { prctl(PR_SET_PDEATHSIG, SIGTERM); });
        edge.proc = launcher->spawn(argv);
    } catch (const Glib::Error& e) {
        g_message("blind-sight: audio routing edge (%s) failed to start: %s", label.c_str(),
                  e.what());
        return edge;
    }

    // pw-loopback exits on its own when a target node is missing or goes
    // away. Dropping the edge then lets the next sync() start it again
    // once the device is back.
    std::weak_ptr<bool> weak_alive = alive;
    Gio::Subprocess* raw = edge.proc.get();
    edge.proc->wait_async([weak_alive, running, key, label, raw](Glib::RefPtr<Gio::AsyncResult>&) {
        if (weak_alive.expired()) {
            return;
        }
        auto it = running->find(key);
        if (it == running->end() || it->second.proc.get() != raw) {
            return;  // stopped deliberately, or already replaced
        }
        g_message("blind-sight: audio routing edge (%s) exited on its own", label.c_str());
        running->erase(it);
    });

    edge.label = label;
    edge.generation = generation;
    edge.playback_node = "output." + loop_name;
    return edge;
}

// The audio nodes PipeWire has right now, plus BlueZ MAC -> node.name,
// built fresh each sync() call via dump_pipewire_audio_nodes.py in a
// separate process. Never via
// GStreamer's in-process pipewiredeviceprovider: it takes the same
// process-wide PipeWire thread-loop lock as the recording/playback
// pipelines elsewhere in this app, and was confirmed by gdb backtrace to
// deadlock against one of them. Synchronous, since sync()'s callers are
// synchronous click handlers; the subprocess round-trip is bounded.
struct LiveNodes {
    bool ok = false;  // false: the dump failed, and nothing below is known
    std::set<std::string> names;
    std::map<std::string, int> ids;  // node.name -> PipeWire object id
    std::vector<std::pair<std::string, std::string>> sink_nodes;    // (address, node.name)
    std::vector<std::pair<std::string, std::string>> source_nodes;  // (address, node.name)
};

// Field names must match dump_pipewire_audio_nodes.py's own JSON output
// exactly — glaze reflects this struct's members directly. Only the
// fields this engine needs, so it must be read with unknown keys
// tolerated (see dump_live_nodes()).
struct PipeWireAudioNode {
    int id = -1;
    std::string node_name;
    std::string media_class;
    std::string bluez_address;
};

std::string lookup_bluez_node(const std::vector<std::pair<std::string, std::string>>& nodes,
                               const std::string& mac_address) {
    for (const auto& [address, node_name] : nodes) {
        if (g_ascii_strcasecmp(address.c_str(), mac_address.c_str()) == 0) {
            return node_name;
        }
    }
    return "";
}

LiveNodes dump_live_nodes() {
    LiveNodes cache;
    std::string output_path = Glib::build_filename(
        Glib::get_tmp_dir(),
        "blind-sight-pw-nodes-" + std::to_string(g_get_monotonic_time()) + ".json");
    std::vector<std::string> argv = {"python3", BLIND_SIGHT_DUMP_AUDIO_NODES_SCRIPT, "--output",
                                      output_path};
    try {
        auto proc = Gio::Subprocess::create(argv, Gio::Subprocess::Flags::NONE);
        proc->wait_check();
        std::ifstream in(output_path);
        std::string content((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        std::error_code ec;
        std::filesystem::remove(output_path, ec);
        std::vector<PipeWireAudioNode> nodes;
        // glaze rejects unknown keys by default, and the script emits more
        // fields than PipeWireAudioNode declares — without this every parse
        // fails and every Bluetooth device silently resolves to nothing.
        if (content.empty() ||
            glz::read<glz::opts{.error_on_unknown_keys = false}>(nodes, content)) {
            g_message("blind-sight: failed to parse PipeWire node dump");
            return cache;
        }
        cache.ok = true;
        for (const auto& node : nodes) {
            cache.names.insert(node.node_name);
            cache.ids[node.node_name] = node.id;
        }
        // A headset can expose two sinks under one MAC: WirePlumber's
        // app-facing virtual sink, and behind it the device node itself,
        // classed "Audio/Sink/Internal". Prefer the app-facing one, so the
        // choice never depends on pw-dump's ordering; an Internal node is
        // used only when it is the device's only sink.
        for (bool internal_pass : {false, true}) {
            for (const auto& node : nodes) {
                if (node.bluez_address.empty() ||
                    node.media_class.ends_with("/Internal") != internal_pass) {
                    continue;
                }
                auto* list = node.media_class.starts_with("Audio/Sink")     ? &cache.sink_nodes
                             : node.media_class.starts_with("Audio/Source") ? &cache.source_nodes
                                                                            : nullptr;
                if (list && lookup_bluez_node(*list, node.bluez_address).empty()) {
                    list->emplace_back(node.bluez_address, node.node_name);
                }
            }
        }
    } catch (const Glib::Error& e) {
        g_message("blind-sight: failed to resolve BlueZ PipeWire nodes: %s", e.what());
    }
    return cache;
}

// device_id -> literal PipeWire node.name, resolving the one BlueZ-MAC
// indirection (see audio_devices.hpp's id-scheme comment) against the
// already-built-this-call `cache` rather than re-querying. Empty if the
// device isn't present right now — a loopback started against a missing
// node would only exit again.
std::string resolve_node_name(const std::string& device_id, bool is_sink,
                               const LiveNodes& cache) {
    if (device_id.empty()) {
        return "";
    }
    // Same MAC-format heuristic as find_counterpart_device()
    // (audio_devices.cpp) — a BlueZ id is always a 17-char
    // "AA:BB:CC:DD:EE:FF" address; anything else is already a literal
    // PipeWire node.name, usable directly.
    if (device_id.size() == 17 && device_id.find(':') != std::string::npos) {
        return lookup_bluez_node(is_sink ? cache.sink_nodes : cache.source_nodes, device_id);
    }
    return cache.names.count(device_id) != 0 ? device_id : "";
}

// Resolves `id`'s effective device, following the one-hop "use ambient"
// indirection: if the assignment's device_id equals the matching
// ambient sentinel, substitute the ambient entry's OWN assignment
// instead (never recursive — Ambient Mic/Speaker never carry that
// marker on themselves). Returns empty if there's no device at all, or
// the ambient indirection target has none either.
std::string resolve_target_node(const MonitoringState& state, const std::string& id, bool is_sink,
                                 const LiveNodes& cache) {
    const auto& pool = is_sink ? state.sink_device_assignment : state.source_device_assignment;
    const char* ambient_sentinel = is_sink ? kSpeakerSinkId : kAmbientMicSourceId;

    auto it = pool.find(id);
    if (it == pool.end() || it->second.device_id.empty()) {
        return "";
    }
    std::string device_id = it->second.device_id;
    if (device_id == ambient_sentinel) {
        auto ambient_it = pool.find(ambient_sentinel);
        if (ambient_it == pool.end() || ambient_it->second.device_id.empty()) {
            return "";
        }
        device_id = ambient_it->second.device_id;
    }
    return resolve_node_name(device_id, is_sink, cache);
}

}  // namespace

struct AudioRoutingImpl {
    std::shared_ptr<NoisePulseEngine> noise_pulse;
    std::map<EdgeKey, RunningEdge> running;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::shared_ptr<LinkQueue> links = std::make_shared<LinkQueue>();
    std::uint64_t next_generation = 1;

    std::function<void()> on_device_change;
    Glib::RefPtr<Gio::Subprocess> watcher;
    Glib::RefPtr<Gio::DataInputStream> watcher_lines;
    sigc::connection watcher_debounce;
    sigc::connection watcher_restart;
    // Device nodes that disappeared since the last sync(). PipeWire drops
    // every link to a vanished node, so a link edge to one must be
    // relinked even if the node is back under the same name by then.
    std::set<std::string> vanished_nodes;
    // The port whose links pw-link -m is currently listing.
    std::string watcher_port;
    // Per sink node: unexpected link losses within kRelinkWindowUs, and
    // when the last one happened.
    std::map<std::string, std::pair<int, std::int64_t>> link_losses;

    // What PipeWire had at the last successful sync().
    LiveNodes last_live;

    // Per id, in dB (see AudioRoutingEngine::set_synthesized_volume_db()).
    std::map<std::string, double> sink_db;  // Synthesized
    std::map<std::string, double> participants_db;
    std::map<std::string, double> source_db;

    // Per id with a microphone of its own (participant or Ambient Mic).
    std::map<std::string, VoiceProcessor> voices;
    // Per id: leveller exits within kRelinkWindowUs, and the last one.
    std::map<std::string, std::pair<int, std::int64_t>> voice_failures;
    AudioRoutingEngine::AgcSettings agc;
    std::map<std::string, bool> agc_on;
    bool microphones_muted = false;
    // Level reports from the voice levellers (voice_agc.cpp's
    // LevelReporter), one datagram socket for all of them.
    std::function<void(const std::string&, std::int64_t, double, bool)> on_level;
    Glib::RefPtr<Gio::Socket> level_socket;
    std::string level_socket_path;
    sigc::connection level_watch;
    // The state the last sync() ran with, for recomputing a leveller's
    // controls when a setting changes between syncs. MonitoringState
    // outlives the engine's use of it (owned by the app's RecordingSession).
    const MonitoringState* last_state = nullptr;
};

namespace {

double db_to_linear(double db) {
    return std::pow(10.0, db / 20.0);
}

std::string format_number(double value) {
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    return g_ascii_dtostr(text, sizeof text, value);
}

// The device node `id` itself owns on the given axis, or empty if it has
// none, uses the Ambient device, or the device isn't present right now.
std::string own_device_node(const MonitoringState& state, const std::string& id, bool is_sink,
                            const LiveNodes& live) {
    const auto& pool = is_sink ? state.sink_device_assignment : state.source_device_assignment;
    auto it = pool.find(id);
    const char* ambient_sentinel = is_sink ? kSpeakerSinkId : kAmbientMicSourceId;
    if (it == pool.end() || it->second.device_id.empty() ||
        it->second.device_id == ambient_sentinel) {
        return "";
    }
    return resolve_node_name(it->second.device_id, is_sink, live);
}

// The listener whose own device `sink_node` is: the first such in matrix
// order, or empty. Listeners on the Ambient Speaker have none of their
// own, so for its device this is the Ambient Speaker itself.
std::string owner_of(const AudioRoutingImpl* impl, const std::string& sink_node) {
    if (!impl->last_state) {
        return "";
    }
    const MonitoringState& state = *impl->last_state;
    std::vector<std::string> listeners = {kSpeakerSinkId};
    listeners.insert(listeners.end(), state.participant_order.begin(),
                     state.participant_order.end());
    for (const auto& id : listeners) {
        if (own_device_node(state, id, /*is_sink=*/true, impl->last_live) == sink_node) {
            return id;
        }
    }
    return "";
}

double volume_db(const std::map<std::string, double>& volumes, const std::string& id) {
    auto it = volumes.find(id);
    return it != volumes.end() ? it->second : 0.0;
}

void apply_loop_gain(AudioRoutingImpl* impl, const EdgeKey& key, RunningEdge& edge) {
    if (edge.is_synth || !edge.playback_seen) {
        return;
    }
    double db = volume_db(impl->participants_db, owner_of(impl, key.first));
    if (edge.applied_db == db) {
        return;
    }
    edge.applied_db = db;
    impl->links->push({"pw-cli", "set-param", edge.playback_node, "Props",
                       "{ volume = " + format_number(db_to_linear(db)) + " }"});
}

// The output is the edge; once it's gone nothing of it outlives the app.
RunningEdge start_synth_edge(AudioRoutingImpl* impl, const EdgeKey& key, const std::string& label,
                             std::uint64_t generation) {
    RunningEdge edge;
    g_message("blind-sight: audio routing edge (%s) starting: %s -> %s", label.c_str(),
              kNoisePulseNodeName, key.first.c_str());
    edge.playback_node = impl->noise_pulse->add_output(
        key.first, volume_db(impl->sink_db, owner_of(impl, key.first)));
    if (edge.playback_node.empty()) {
        g_message("blind-sight: audio routing edge (%s) failed to start", label.c_str());
        return edge;
    }
    edge.is_synth = true;
    edge.generation = generation;
    edge.label = label;
    return edge;
}

// Sets every running edge's volume: the listener's Synthesized volume on
// their synthesized output, their Participants volume on the voice
// loopbacks into their device.
void apply_volumes(AudioRoutingImpl* impl) {
    for (auto& [key, edge] : impl->running) {
        if (edge.is_synth) {
            impl->noise_pulse->set_output_gain_db(
                key.first, volume_db(impl->sink_db, owner_of(impl, key.first)));
        } else {
            apply_loop_gain(impl, key, edge);
        }
    }
}

// Only device nodes count. The engine's own loopbacks and the synthesized
// stream add and remove ports too, and reacting to those would make every
// sync() trigger another.
bool is_device_port_event(const std::string& line) {
    if (line.size() < 2 || (line[0] != '+' && line[0] != '-')) {
        return false;
    }
    std::string_view port(line.data() + 1, line.size() - 1);
    return port.starts_with("alsa_") || port.starts_with("bluez_");
}

// A device appearing brings several ports, and a Bluetooth device often
// switches profile while connecting; waiting for quiet resyncs once.
constexpr unsigned int kDeviceSettleMs = 500;

constexpr unsigned int kWatcherRestartMs = 2000;

// A sink that keeps dropping a fresh link would otherwise be relinked
// forever, twice a second.
constexpr int kMaxRelinks = 5;
constexpr std::int64_t kRelinkWindowUs = 10'000'000;

void schedule_resync(AudioRoutingImpl* impl) {
    impl->watcher_debounce.disconnect();
    impl->watcher_debounce = Glib::signal_timeout().connect(
        [impl]() {
            if (impl->on_device_change) {
                impl->on_device_change();
            }
            return false;
        },
        kDeviceSettleMs);
}

std::string node_of(const std::string& port) {
    return port.substr(0, port.rfind(':'));
}

constexpr const char* kVoiceNodePrefix = "blind-sight-voice-";

bool voice_muted(const AudioRoutingImpl* impl, const std::string& id) {
    return impl->microphones_muted ||
           (impl->last_state && impl->last_state->muted_source_override.count(id) != 0);
}

// The leveller's controls for `id`, named as voice_agc.cpp names them.
std::vector<std::pair<const char*, double>> voice_controls(const AudioRoutingImpl* impl,
                                                           const std::string& id) {
    auto agc_it = impl->agc_on.find(id);
    auto volume_it = impl->source_db.find(id);
    return {
        {"Target (dBFS)", impl->agc.target_dbfs},
        {"Attack (s)", impl->agc.attack_s},
        {"Release (s)", impl->agc.release_s},
        {"Max gain (dB)", impl->agc.max_gain_db},
        {"AGC", agc_it == impl->agc_on.end() || agc_it->second ? 1.0 : 0.0},
        {"Volume (dB)", volume_it != impl->source_db.end() ? volume_it->second : 0.0},
        {"Mute", voice_muted(impl, id) ? 1.0 : 0.0},
    };
}

// Sends `id`'s controls to its running leveller, unless they're what it
// already has. `force` resends regardless: pw-cli reports success even
// when the node doesn't exist yet, so whatever was sent before the
// leveller appeared may have gone nowhere.
void push_voice_controls(AudioRoutingImpl* impl, const std::string& id, bool force = false) {
    auto it = impl->voices.find(id);
    if (it == impl->voices.end()) {
        return;
    }
    std::string params = "{ params = [";
    for (const auto& [name, value] : voice_controls(impl, id)) {
        params += std::string(" \"agc:") + name + "\" " + format_number(value);
    }
    params += " ] }";
    if (!force && params == it->second.applied_params) {
        return;
    }
    it->second.applied_params = params;
    impl->links->push({"pw-cli", "set-param", it->second.node_name, "Props", params});
}

std::string voice_conf(const AudioRoutingImpl* impl, const std::string& id,
                       const VoiceProcessor& voice) {
    std::string controls;
    for (const auto& [name, value] : voice_controls(impl, id)) {
        controls += std::string(" \"") + name + "\" = " + format_number(value);
    }
    // Base modules as in PipeWire's own filter-chain.conf; the capture side
    // never falls back to another microphone (see kLoopbackStreamProps).
    return std::string(R"(context.properties = { log.level = 0 }
context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }
context.modules = [
  { name = libpipewire-module-rt args = { } flags = [ ifexists nofail ] }
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-client-node }
  { name = libpipewire-module-adapter }
  { name = libpipewire-module-filter-chain
    args = {
      node.description = "Blind Sight voice"
      filter.graph = {
        nodes = [ { type = ladspa name = agc plugin = ")") +
           BLIND_SIGHT_VOICE_PLUGIN + R"(" label = blind_sight_voice
                    control = {)" + controls + R"( } } ]
      }
      audio.position = [ MONO ]
      capture.props = { node.name = ")" + voice.node_name + R"(.capture" target.object = ")" +
           voice.mic_node + R"(" node.dont-reconnect = true node.dont-fallback = true }
      playback.props = { node.name = ")" + voice.node_name + R"(" media.class = Audio/Source }
    }
  }
]
)";
}

void stop_voice(VoiceProcessor& voice) {
    if (voice.proc) {
        voice.proc->send_signal(SIGTERM);
        voice.proc.reset();
    }
    std::error_code ec;
    std::filesystem::remove(voice.conf_path, ec);
}

// A leveller that keeps dying (a broken plugin, say) would otherwise be
// restarted on every sync, forever.
bool voice_failing(AudioRoutingImpl* impl, const std::string& id) {
    auto it = impl->voice_failures.find(id);
    return it != impl->voice_failures.end() && it->second.first > kMaxRelinks &&
           g_get_monotonic_time() - it->second.second <= kRelinkWindowUs;
}

void start_voice(AudioRoutingImpl* impl, const std::string& id, const std::string& mic_node) {
    VoiceProcessor voice;
    voice.mic_node = mic_node;
    voice.node_name = kVoiceNodePrefix + std::to_string(impl->next_generation++);
    std::string dir = Glib::build_filename(Glib::get_user_runtime_dir(), "blind-sight");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    voice.conf_path = Glib::build_filename(dir, voice.node_name + ".conf");
    {
        std::ofstream conf(voice.conf_path);
        conf << voice_conf(impl, id, voice);
    }
    g_message("blind-sight: voice leveller for %s starting on %s", id.c_str(), mic_node.c_str());
    try {
        auto launcher = Gio::SubprocessLauncher::create(Gio::Subprocess::Flags::NONE);
        launcher->set_child_setup([]() { prctl(PR_SET_PDEATHSIG, SIGTERM); });
        if (!impl->level_socket_path.empty()) {
            launcher->setenv("BLIND_SIGHT_LEVEL_SOCKET", impl->level_socket_path, true);
            launcher->setenv("BLIND_SIGHT_LEVEL_ID", id, true);
        }
        voice.proc = launcher->spawn({"pipewire", "-c", voice.conf_path});
    } catch (const Glib::Error& e) {
        g_message("blind-sight: voice leveller for %s failed to start: %s", id.c_str(), e.what());
        std::filesystem::remove(voice.conf_path, ec);
        return;
    }
    std::weak_ptr<bool> weak_alive = impl->alive;
    Gio::Subprocess* raw = voice.proc.get();
    voice.proc->wait_async([impl, weak_alive, id, raw](Glib::RefPtr<Gio::AsyncResult>&) {
        if (weak_alive.expired()) {
            return;
        }
        auto it = impl->voices.find(id);
        if (it == impl->voices.end() || it->second.proc.get() != raw) {
            return;  // stopped deliberately, or already replaced
        }
        g_message("blind-sight: voice leveller for %s exited on its own", id.c_str());
        auto& [count, last_us] = impl->voice_failures[id];
        std::int64_t now_us = g_get_monotonic_time();
        count = now_us - last_us > kRelinkWindowUs ? 1 : count + 1;
        last_us = now_us;
        stop_voice(it->second);
        impl->voices.erase(it);
        schedule_resync(impl);
    });
    impl->voices[id] = std::move(voice);
}

// A leveller's ports coming or going: once it's there its controls are
// resent (see push_voice_controls()), and either way the edges listening
// to it need a sync.
void on_voice_port(AudioRoutingImpl* impl, bool added, const std::string& node) {
    if (added) {
        for (auto& [id, voice] : impl->voices) {
            if (node == voice.node_name) {
                push_voice_controls(impl, id, /*force=*/true);
            }
        }
    } else {
        impl->vanished_nodes.insert(node);
    }
    schedule_resync(impl);
}


// A link from a synthesized output vanished. Outputs this engine removed
// itself show up here too; they are no longer the edge's playback node.
void on_synth_link_removed(AudioRoutingImpl* impl, const std::string& output_node,
                           const std::string& sink_port) {
    std::string sink_node = node_of(sink_port);
    auto edge = impl->running.find({sink_node, kNoisePulseNodeName});
    if (edge == impl->running.end() || edge->second.playback_node != output_node) {
        return;
    }
    auto& [count, last_us] = impl->link_losses[sink_node];
    std::int64_t now_us = g_get_monotonic_time();
    if (now_us - last_us > kRelinkWindowUs) {
        count = 0;
    }
    last_us = now_us;
    if (++count > kMaxRelinks) {
        if (count == kMaxRelinks + 1) {
            g_message("blind-sight: %s keeps dropping its link, not relinking until it reconnects",
                      sink_node.c_str());
        }
        return;
    }
    impl->vanished_nodes.insert(sink_node);
    schedule_resync(impl);
}

void handle_watcher_line(AudioRoutingImpl* impl, const std::string& line) {
    constexpr std::string_view kLinkRemoved = "-  |-> ";
    if (line.starts_with(kLinkRemoved)) {
        if (impl->watcher_port.starts_with(std::string(kNoisePulseNodeName) + "-")) {
            on_synth_link_removed(impl, node_of(impl->watcher_port),
                                  line.substr(kLinkRemoved.size()));
        }
        return;
    }
    if (line.size() < 2 || line[1] == ' ') {
        return;  // another link line
    }
    impl->watcher_port = line.substr(1);
    if (impl->watcher_port.starts_with("output.blind-sight-route-")) {
        if (line[0] == '+') {
            std::string node = node_of(impl->watcher_port);
            for (auto& [key, edge] : impl->running) {
                if (edge.playback_node == node && !edge.playback_seen) {
                    edge.playback_seen = true;
                    apply_loop_gain(impl, key, edge);
                }
            }
        }
        return;
    }
    if (impl->watcher_port.starts_with(kVoiceNodePrefix)) {
        // Only a port appearing or disappearing; "*" marks a change to an
        // existing one, such as a listener's loopback linking to it.
        if (line[0] == '+' || line[0] == '-') {
            on_voice_port(impl, line[0] == '+', node_of(impl->watcher_port));
        }
        return;
    }
    if (!is_device_port_event(line)) {
        return;
    }
    std::string node = node_of(impl->watcher_port);
    if (line[0] == '-') {
        impl->vanished_nodes.insert(node);
    } else {
        impl->link_losses.erase(node);  // reconnected: a fresh start
    }
    schedule_resync(impl);
}

void start_watcher(AudioRoutingImpl* impl);

void read_watcher_line(AudioRoutingImpl* impl, std::weak_ptr<bool> weak_alive) {
    impl->watcher_lines->read_line_async(
        [impl, weak_alive](Glib::RefPtr<Gio::AsyncResult>& result) {
            if (weak_alive.expired()) {
                return;
            }
            std::string line;
            bool ok = false;
            try {
                ok = impl->watcher_lines->read_line_finish(result, line);
            } catch (const Glib::Error&) {
            }
            if (!ok) {
                g_message("blind-sight: audio device watcher stopped, restarting");
                impl->watcher_restart = Glib::signal_timeout().connect(
                    [impl]() {
                        start_watcher(impl);
                        return false;
                    },
                    kWatcherRestartMs);
                return;
            }
            handle_watcher_line(impl, line);
            read_watcher_line(impl, weak_alive);
        },
        {});
}

// `pw-link -m -l` prints "+node:port" / "-node:port" as ports come and go
// (starting with every port that already exists), "*node:port" when one
// changes, and under each such line "+  |-> peer" / "-  |-> peer" for its
// links coming and going.
void start_watcher(AudioRoutingImpl* impl) {
    try {
        auto launcher = Gio::SubprocessLauncher::create(Gio::Subprocess::Flags::STDOUT_PIPE |
                                                        Gio::Subprocess::Flags::STDERR_SILENCE);
        launcher->set_child_setup([]() { prctl(PR_SET_PDEATHSIG, SIGTERM); });
        impl->watcher = launcher->spawn({"pw-link", "-m", "-o", "-i", "-l"});
    } catch (const Glib::Error& e) {
        g_message("blind-sight: failed to start audio device watcher: %s", e.what());
        return;
    }
    impl->watcher_lines = Gio::DataInputStream::create(impl->watcher->get_stdout_pipe());
    read_watcher_line(impl, impl->alive);
}

}  // namespace

namespace {

// Reads every datagram waiting: the reporting microphone's id on the first
// line, then "t_ns level_db muted" per sample (see LevelReporter).
bool read_levels(AudioRoutingImpl* impl) {
    char buffer[8192];
    while (true) {
        gssize n = 0;
        try {
            n = impl->level_socket->receive(buffer, sizeof buffer - 1);
        } catch (const Glib::Error&) {
            break;  // would block: nothing more waiting
        }
        if (n <= 0) {
            break;
        }
        buffer[n] = '\0';
        std::istringstream lines(buffer);
        std::string id;
        if (!std::getline(lines, id) || !impl->on_level) {
            continue;
        }
        long long t_ns = 0;
        double level_db = 0.0;
        int muted = 0;
        while (lines >> t_ns >> level_db >> muted) {
            impl->on_level(id, t_ns, level_db, muted != 0);
        }
    }
    return true;
}

}  // namespace

AudioRoutingEngine::AudioRoutingEngine(std::shared_ptr<NoisePulseEngine> noise_pulse)
    : impl_(std::make_unique<AudioRoutingImpl>()) {
    impl_->noise_pulse = std::move(noise_pulse);
}

void AudioRoutingEngine::on_microphone_level(
    std::function<void(const std::string&, std::int64_t, double, bool)> callback) {
    impl_->on_level = std::move(callback);
    // Per app instance, in the user's private runtime directory; levellers
    // started from now on are told where it is.
    std::string dir = Glib::build_filename(Glib::get_user_runtime_dir(), "blind-sight");
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::permissions(dir, std::filesystem::perms::owner_all, ec);
    std::string path = Glib::build_filename(dir, "levels-" + std::to_string(getpid()) + ".sock");
    std::filesystem::remove(path, ec);
    try {
        impl_->level_socket = Gio::Socket::create(Gio::SocketFamily::UNIX, Gio::Socket::Type::DATAGRAM,
                                                  Gio::Socket::Protocol::DEFAULT);
        impl_->level_socket->set_blocking(false);
        impl_->level_socket->bind(Gio::UnixSocketAddress::create(path), true);
    } catch (const Glib::Error& e) {
        g_message("blind-sight: no microphone level reports: %s", e.what());
        impl_->level_socket.reset();
        return;
    }
    impl_->level_socket_path = path;
    AudioRoutingImpl* impl = impl_.get();
    impl_->level_watch = impl_->level_socket->create_source(Glib::IOCondition::IO_IN)
                             ->connect([impl](Glib::IOCondition) { return read_levels(impl); });
    // A socket source built this way is attached to the default context
    // by connect(); nothing else to do.
}

AudioRoutingEngine::~AudioRoutingEngine() {
    impl_->level_watch.disconnect();
    if (!impl_->level_socket_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(impl_->level_socket_path, ec);
    }
    impl_->watcher_debounce.disconnect();
    impl_->watcher_restart.disconnect();
    if (impl_->watcher) {
        impl_->watcher->send_signal(SIGTERM);
    }
    for (auto& [key, edge] : impl_->running) {
        stop_edge(key, edge, *impl_->noise_pulse);
    }
    for (auto& [id, voice] : impl_->voices) {
        stop_voice(voice);
    }
}

void AudioRoutingEngine::watch_devices(std::function<void()> on_change) {
    impl_->on_device_change = std::move(on_change);
    start_watcher(impl_.get());
}

bool AudioRoutingEngine::is_disconnected(const MonitoringState& state, const std::string& id,
                                          bool is_sink) const {
    const LiveNodes& live = impl_->last_live;
    if (!live.ok) {
        return false;
    }
    const auto& pool = is_sink ? state.sink_device_assignment : state.source_device_assignment;
    auto it = pool.find(id);
    if (it == pool.end() || it->second.device_id.empty()) {
        return false;
    }
    // "Use ambient" with no ambient device is unassigned, not disconnected.
    const char* ambient_sentinel = is_sink ? kSpeakerSinkId : kAmbientMicSourceId;
    if (it->second.device_id == ambient_sentinel) {
        auto ambient_it = pool.find(ambient_sentinel);
        if (ambient_it == pool.end() || ambient_it->second.device_id.empty()) {
            return false;
        }
    }
    return resolve_target_node(state, id, is_sink, live).empty();
}

void AudioRoutingEngine::sync(const MonitoringState& state) {
    // sink_ids/source_ids exactly as the matrix itself iterates them
    // (monitoring_dialogs.cpp's refresh_ptr).
    std::vector<std::string> sink_ids = {kSpeakerSinkId};
    sink_ids.insert(sink_ids.end(), state.participant_order.begin(), state.participant_order.end());
    std::vector<std::string> source_ids = {kAmbientMicSourceId, kSynthesizedSourceId};
    source_ids.insert(source_ids.end(), state.participant_order.begin(),
                       state.participant_order.end());

    LiveNodes live = dump_live_nodes();
    if (!live.ok) {
        return;  // keep whatever runs now rather than tear it all down on a failed dump
    }
    impl_->last_live = live;
    impl_->last_state = &state;

    // Levellers: one per microphone of its own. Restarted when the
    // microphone changes or went away in between, since the capture side
    // never reconnects by itself.
    std::vector<std::string> talker_ids = {kAmbientMicSourceId};
    talker_ids.insert(talker_ids.end(), state.participant_order.begin(),
                      state.participant_order.end());
    std::set<std::string> talkers(talker_ids.begin(), talker_ids.end());
    for (auto it = impl_->voices.begin(); it != impl_->voices.end();) {
        if (talkers.count(it->first) == 0) {
            stop_voice(it->second);
            it = impl_->voices.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& id : talker_ids) {
        std::string mic = own_device_node(state, id, /*is_sink=*/false, live);
        auto it = impl_->voices.find(id);
        if (it != impl_->voices.end() &&
            (mic != it->second.mic_node || impl_->vanished_nodes.count(it->second.mic_node) != 0)) {
            stop_voice(it->second);
            impl_->voices.erase(it);
            it = impl_->voices.end();
        }
        if (it != impl_->voices.end()) {
            push_voice_controls(impl_.get(), id);  // a mute may have changed
        } else if (!mic.empty() && !voice_failing(impl_.get(), id)) {
            start_voice(impl_.get(), id, mic);
        }
    }
    // What a listener hears from `source_id`: its leveller, or the Ambient
    // Microphone's for anyone on the Ambient Microphone. Empty until that
    // leveller is up; the watcher resyncs once it is.
    auto voice_node = [this, &state, &live](const std::string& source_id) -> std::string {
        const auto& pool = state.source_device_assignment;
        auto it = pool.find(source_id);
        if (it == pool.end() || it->second.device_id.empty()) {
            return "";
        }
        std::string talker =
            it->second.device_id == kAmbientMicSourceId ? kAmbientMicSourceId : source_id;
        auto voice = impl_->voices.find(talker);
        if (voice == impl_->voices.end() || live.names.count(voice->second.node_name) == 0) {
            return "";
        }
        return voice->second.node_name;
    };

    std::map<EdgeKey, std::string> desired;  // -> label of the first cell wanting it
    for (const auto& sink_id : sink_ids) {
        for (const auto& source_id : source_ids) {
            if (is_effectively_muted(state, sink_id, source_id)) {
                continue;
            }
            std::string sink_node = resolve_target_node(state, sink_id, /*is_sink=*/true, live);
            std::string source_node = source_id == kSynthesizedSourceId
                                          ? std::string(kNoisePulseNodeName)
                                          : voice_node(source_id);
            if (sink_node.empty() || source_node.empty()) {
                g_message("blind-sight: audio routing edge (%s -> %s) skipped: nothing live for %s",
                          source_id.c_str(), sink_id.c_str(),
                          sink_node.empty() ? sink_id.c_str() : source_id.c_str());
                continue;
            }
            desired.try_emplace({sink_node, source_node}, source_id + " -> " + sink_id);
        }
    }

    // Stop anything no longer desired, or touching a node that vanished
    // since the last sync() — its links are gone even if the node is back.
    for (auto it = impl_->running.begin(); it != impl_->running.end();) {
        bool keep = desired.count(it->first) != 0 &&
                    impl_->vanished_nodes.count(it->first.first) == 0 &&
                    impl_->vanished_nodes.count(it->first.second) == 0;
        if (!keep) {
            g_message("blind-sight: audio routing edge (%s) stopping", it->second.label.c_str());
            stop_edge(it->first, it->second, *impl_->noise_pulse);
            it = impl_->running.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [key, label] : desired) {
        if (impl_->running.count(key) != 0) {
            continue;
        }
        RunningEdge edge =
            key.second == kNoisePulseNodeName
                ? start_synth_edge(impl_.get(), key, label, impl_->next_generation++)
                : start_loopback_edge(&impl_->running, impl_->alive, key, label,
                                      impl_->next_generation++);
        if (edge.proc || edge.is_synth) {
            impl_->running[key] = std::move(edge);
        }
    }
    apply_volumes(impl_.get());
    impl_->vanished_nodes.clear();
}

void AudioRoutingEngine::set_synthesized_volume_db(const MonitoringState&,
                                                   const std::string& id, double db) {
    impl_->sink_db[id] = db;
    apply_volumes(impl_.get());
}

void AudioRoutingEngine::set_participants_volume_db(const MonitoringState&,
                                                    const std::string& id, double db) {
    impl_->participants_db[id] = db;
    apply_volumes(impl_.get());
}

void AudioRoutingEngine::set_source_volume_db(const MonitoringState&, const std::string& id,
                                              double db) {
    impl_->source_db[id] = db;
    push_voice_controls(impl_.get(), id);
}

void AudioRoutingEngine::set_agc_settings(const AgcSettings& settings) {
    impl_->agc = settings;
    for (const auto& [id, voice] : impl_->voices) {
        push_voice_controls(impl_.get(), id);
    }
}

void AudioRoutingEngine::set_agc_enabled(const std::string& id, bool enabled) {
    impl_->agc_on[id] = enabled;
    push_voice_controls(impl_.get(), id);
}

bool AudioRoutingEngine::agc_enabled(const std::string& id) const {
    auto it = impl_->agc_on.find(id);
    return it == impl_->agc_on.end() || it->second;
}

void AudioRoutingEngine::set_microphones_muted(bool muted) {
    impl_->microphones_muted = muted;
    for (const auto& [id, voice] : impl_->voices) {
        push_voice_controls(impl_.get(), id);
    }
}

double AudioRoutingEngine::synthesized_volume_db(const std::string& id) const {
    auto it = impl_->sink_db.find(id);
    return it != impl_->sink_db.end() ? it->second : 0.0;
}

double AudioRoutingEngine::participants_volume_db(const std::string& id) const {
    auto it = impl_->participants_db.find(id);
    return it != impl_->participants_db.end() ? it->second : 0.0;
}

double AudioRoutingEngine::source_volume_db(const std::string& id) const {
    auto it = impl_->source_db.find(id);
    return it != impl_->source_db.end() ? it->second : 0.0;
}

}  // namespace blind_sight
