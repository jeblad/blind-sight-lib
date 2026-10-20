// SPDX-License-Identifier: GPL-3.0-only

#include "images.hpp"

#include <glaze/glaze.hpp>
#include <glibmm/ustring.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <random>

namespace blind_sight {

namespace {

std::string images_json_path(const std::string& project_dir) {
    return project_dir + "/images.json";
}

}  // namespace

ImagesIndex read_images_index(const std::string& project_dir) {
    ImagesIndex index;
    std::ifstream in(images_json_path(project_dir));
    if (!in) {
        return index;
    }
    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
    if (content.empty()) {
        return index;
    }
    ImagesIndex parsed;
    if (glz::read_json(parsed, content)) {
        return index;  // malformed — treat as empty rather than crash
    }
    return parsed;
}

bool write_images_index(const std::string& project_dir, const ImagesIndex& index) {
    std::string json;
    if (auto ec = glz::write_json(index, json); ec) {
        return false;
    }
    std::ofstream out(images_json_path(project_dir), std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json;
    return static_cast<bool>(out);
}

std::string generate_image_id(const std::string& project_dir) {
    std::random_device rd;
    std::string images_dir = project_dir + "/images";
    for (;;) {
        auto now_ns =
            std::chrono::system_clock::now().time_since_epoch().count();
        std::uint64_t entropy =
            (static_cast<std::uint64_t>(rd()) << 32) ^ static_cast<std::uint64_t>(rd());
        std::uint64_t seed = static_cast<std::uint64_t>(now_ns) ^ entropy;
        std::size_t h = std::hash<std::uint64_t>{}(seed);
        // std::format(), not an ostringstream — see generate_session_id()'s
        // own comment (sessions.cpp) for why: iostream numeric formatting
        // is locale-sensitive, std::format() isn't unless asked to be.
        std::string candidate = std::format("{:016x}", h);
        if (!std::filesystem::exists(images_dir + "/" + candidate)) {
            return candidate;
        }
    }
}

std::string image_file_path(const std::string& project_dir, const std::string& id,
                              const std::string& original_filename) {
    return project_dir + "/images/" + id + "/" + original_filename;
}

std::string slugify(const std::string& text) {
    Glib::ustring lower = Glib::ustring(text).lowercase();
    std::string out;
    out.reserve(lower.bytes());
    bool pending_hyphen = false;
    for (gunichar ch : lower) {
        std::string piece;
        if (ch == 0x00E6) {
            piece = "ae";
        } else if (ch == 0x00F8) {
            piece = "o";
        } else if (ch == 0x00E5) {
            piece = "a";
        } else if (ch < 128 && std::isalnum(static_cast<int>(ch))) {
            piece = std::string(1, static_cast<char>(ch));
        }
        if (piece.empty()) {
            pending_hyphen = !out.empty();
            continue;
        }
        if (pending_hyphen) {
            out += '-';
            pending_hyphen = false;
        }
        out += piece;
    }
    return out;
}

std::string generate_image_slug(const std::string& project_dir,
                                  const std::string& original_filename) {
    std::string stem = std::filesystem::path(original_filename).stem().string();
    std::string base = slugify(stem);
    if (base.empty()) {
        base = "image";
    }
    auto index = read_images_index(project_dir);
    auto collides = [&index](const std::string& candidate) {
        return std::any_of(index.images.begin(), index.images.end(),
                            [&candidate](const ImageEntry& entry) {
                                return entry.slug == candidate;
                            });
    };
    if (!collides(base)) {
        return base;
    }
    for (int n = 2;; ++n) {
        std::string candidate = base + "-" + std::to_string(n);
        if (!collides(candidate)) {
            return candidate;
        }
    }
}

}  // namespace blind_sight
