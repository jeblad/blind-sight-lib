// SPDX-License-Identifier: GPL-3.0-only

#include "tracks.hpp"

#include <glaze/glaze.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <random>

namespace blind_sight {

namespace {

std::string tracks_json_path(const std::string& session_dir) {
    return session_dir + "/tracks.json";
}

}  // namespace

TracksIndex read_tracks_index(const std::string& session_dir) {
    TracksIndex index;
    std::ifstream in(tracks_json_path(session_dir));
    if (!in) {
        return index;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (content.empty()) {
        return index;
    }
    TracksIndex parsed;
    if (glz::read_json(parsed, content)) {
        return index;  // malformed — treat as empty rather than crash
    }
    return parsed;
}

bool write_tracks_index(const std::string& session_dir, const TracksIndex& index) {
    std::string json;
    if (auto ec = glz::write_json(index, json); ec) {
        return false;
    }
    std::ofstream out(tracks_json_path(session_dir), std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json;
    return static_cast<bool>(out);
}

std::string generate_track_id(const std::string& session_dir) {
    std::random_device rd;
    std::string tracks_dir = session_dir + "/tracks";
    for (;;) {
        auto now_ns =
            std::chrono::system_clock::now().time_since_epoch().count();
        std::uint64_t entropy =
            (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        std::uint64_t seed = static_cast<std::uint64_t>(now_ns) ^ entropy;
        std::size_t h = std::hash<std::uint64_t>{}(seed);
        // std::format(), not an ostringstream — see generate_session_id()'s
        // own comment (sessions.cpp) for why.
        std::string candidate = std::format("{:016x}", h);
        if (!std::filesystem::exists(tracks_dir + "/" + candidate)) {
            return candidate;
        }
    }
}

}  // namespace blind_sight
