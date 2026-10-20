// SPDX-License-Identifier: GPL-3.0-only

#include "monitoring.hpp"

#include "recording.hpp"

#include <algorithm>
#include <set>

namespace blind_sight {

std::vector<std::string> participant_names(const RecordMeta& meta) {
    std::vector<std::string> names;
    if (!meta.creator.empty()) {
        names.push_back(meta.creator);
    }
    // Same comma/semicolon split as transcription.cpp's
    // speaker_count_hint(), but keeping each trimmed piece instead of
    // just counting it.
    const std::string& field = meta.contributor;
    std::size_t start = 0;
    while (start <= field.size()) {
        std::size_t end = field.find_first_of(",;", start);
        std::string piece = field.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        std::size_t first = piece.find_first_not_of(" \t");
        if (first != std::string::npos) {
            std::size_t last = piece.find_last_not_of(" \t");
            names.push_back(piece.substr(first, last - first + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return names;
}

void reconcile_monitoring_state(MonitoringState& state,
                                 const std::vector<std::string>& current_names) {
    std::vector<std::string> kept;
    kept.reserve(state.participant_order.size());
    for (const auto& name : state.participant_order) {
        if (std::find(current_names.begin(), current_names.end(), name) !=
            current_names.end()) {
            kept.push_back(name);
        }
    }
    for (const auto& name : current_names) {
        if (std::find(kept.begin(), kept.end(), name) == kept.end()) {
            kept.push_back(name);
        }
    }
    state.participant_order = kept;

    std::set<std::string> live(kept.begin(), kept.end());
    for (auto it = state.muted.begin(); it != state.muted.end();) {
        const auto& sink_id = it->first.first;
        const auto& source_id = it->first.second;
        bool sink_ok = sink_id == kSpeakerSinkId || live.count(sink_id) != 0;
        bool source_ok = source_id == kAmbientMicSourceId ||
                          source_id == kSynthesizedSourceId || live.count(source_id) != 0;
        if (!sink_ok || !source_ok) {
            it = state.muted.erase(it);
        } else {
            ++it;
        }
    }
    auto prune_assignment_pool = [&live](std::map<std::string, DeviceAssignment>& pool,
                                          const char* sentinel) {
        for (auto it = pool.begin(); it != pool.end();) {
            bool key_ok = it->first == sentinel || live.count(it->first) != 0;
            if (!key_ok) {
                it = pool.erase(it);
            } else {
                ++it;
            }
        }
    };
    prune_assignment_pool(state.sink_device_assignment, kSpeakerSinkId);
    prune_assignment_pool(state.source_device_assignment, kAmbientMicSourceId);

    auto prune_override_set = [&live](std::set<std::string>& overrides, const char* sentinel) {
        for (auto it = overrides.begin(); it != overrides.end();) {
            bool key_ok = *it == sentinel || live.count(*it) != 0;
            if (!key_ok) {
                it = overrides.erase(it);
            } else {
                ++it;
            }
        }
    };
    prune_override_set(state.muted_sink_override, kSpeakerSinkId);
    prune_override_set(state.muted_source_override, kAmbientMicSourceId);

    std::vector<std::string> sink_ids = kept;
    sink_ids.push_back(kSpeakerSinkId);
    std::vector<std::string> source_ids = kept;
    source_ids.push_back(kAmbientMicSourceId);
    source_ids.push_back(kSynthesizedSourceId);
    for (const auto& sink_id : sink_ids) {
        for (const auto& source_id : source_ids) {
            state.muted.try_emplace({sink_id, source_id}, is_default_muted(sink_id, source_id));
        }
    }
}

bool is_default_muted(const std::string& sink_id, const std::string& source_id) {
    // The Ambient Speaker shares the room with the microphones recording
    // the conversation, so anything live played through it is picked up
    // again; only synthesized sound reaches it by default.
    if (sink_id == kSpeakerSinkId) {
        return source_id != kSynthesizedSourceId;
    }
    return sink_id == source_id || source_id == kAmbientMicSourceId;
}

bool is_muted(const MonitoringState& state, const std::string& sink_id,
              const std::string& source_id) {
    auto it = state.muted.find({sink_id, source_id});
    if (it != state.muted.end()) {
        return it->second;
    }
    return is_default_muted(sink_id, source_id);
}

void set_muted(MonitoringState& state, const std::string& sink_id,
               const std::string& source_id, bool muted) {
    state.muted[{sink_id, source_id}] = muted;
}

bool is_effectively_muted(const MonitoringState& state, const std::string& sink_id,
                           const std::string& source_id) {
    return is_muted(state, sink_id, source_id) ||
           state.muted_sink_override.count(sink_id) != 0 ||
           state.muted_source_override.count(source_id) != 0;
}

}  // namespace blind_sight
