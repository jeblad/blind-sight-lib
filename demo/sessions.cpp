// SPDX-License-Identifier: GPL-3.0-only

#include "sessions.hpp"

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

std::string sessions_json_path(const std::string& project_dir) {
    return project_dir + "/sessions.json";
}

}  // namespace

SessionsIndex read_sessions_index(const std::string& project_dir) {
    SessionsIndex index;
    std::ifstream in(sessions_json_path(project_dir));
    if (!in) {
        return index;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (content.empty()) {
        return index;
    }
    SessionsIndex parsed;
    if (glz::read_json(parsed, content)) {
        return index;  // malformed — treat as empty rather than crash
    }
    return parsed;
}

bool write_sessions_index(const std::string& project_dir, const SessionsIndex& index) {
    std::string json;
    if (auto ec = glz::write_json(index, json); ec) {
        return false;
    }
    std::ofstream out(sessions_json_path(project_dir), std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json;
    return static_cast<bool>(out);
}

std::string generate_session_id(const std::string& project_dir) {
    std::random_device rd;
    std::string sessions_dir = project_dir + "/sessions";
    for (;;) {
        auto now_ns =
            std::chrono::system_clock::now().time_since_epoch().count();
        std::uint64_t entropy =
            (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        std::uint64_t seed = static_cast<std::uint64_t>(now_ns) ^ entropy;
        std::size_t h = std::hash<std::uint64_t>{}(seed);
        // std::format(), not an ostringstream — iostream numeric
        // formatting goes through whatever locale is globally imbued at
        // the time, and GTK's startup locale setup triggers exactly
        // this: it group-separates the hex digits with spaces
        // ("2deb0ca20433e8aa" becomes "2 deb 0ca 204 33e 8aa" on disk).
        // std::format() is locale-independent unless a locale is passed
        // explicitly.
        std::string candidate = std::format("{:016x}", h);
        if (!std::filesystem::exists(sessions_dir + "/" + candidate)) {
            return candidate;
        }
    }
}

std::string session_file_path(const std::string& project_dir, const std::string& id) {
    return project_dir + "/sessions/" + id + "/session.bs";
}

}  // namespace blind_sight
