// SPDX-License-Identifier: GPL-3.0-only

#include "recording.hpp"

#include <glaze/glaze.hpp>

#include <cstdio>
#include <fstream>

namespace blind_sight {

bool write_record_meta(const std::string& path, const RecordMeta& meta) {
    // The one place kRecordFormatVersion gets stamped onto a file —
    // callers don't need to remember to set it themselves. A local
    // copy, not mutating the caller's own meta.
    RecordMeta versioned = meta;
    versioned.version = kRecordFormatVersion;
    std::string json;
    if (auto ec = glz::write_json(versioned, json); ec) {
        return false;
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << json << '\n';
    return static_cast<bool>(out);
}

bool read_record_meta(const std::string& path, RecordMeta& out_meta) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string first_line;
    if (!std::getline(in, first_line)) {
        return false;
    }
    if (glz::read_json(out_meta, first_line)) {
        return false;
    }
    // Not just "did it parse" — "is it a version this build understands."
    // A pre-versioning file (every recording before this field existed)
    // reads back with version's default-constructed 0, so it's rejected
    // here too, not just files that fail to parse outright.
    return out_meta.version == kRecordFormatVersion;
}

bool update_record_meta(const std::string& path, const RecordMeta& meta) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::string first_line;
    if (!std::getline(in, first_line)) {
        return false;
    }
    RecordMeta versioned = meta;
    versioned.version = kRecordFormatVersion;
    std::string json;
    if (auto ec = glz::write_json(versioned, json); ec) {
        return false;
    }

    std::string temp_path = path + ".tmp";
    {
        std::ofstream out(temp_path, std::ios::out | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << json << '\n';
        std::string line;
        while (std::getline(in, line)) {
            out << line << '\n';
        }
        if (!out) {
            return false;
        }
    }
    in.close();
    if (std::rename(temp_path.c_str(), path.c_str()) != 0) {
        return false;
    }
    return true;
}

namespace {

// Just enough to route a line to the right struct below. Glaze defaults
// to erroring on unknown keys, which every line has relative to this
// bare tag — error_on_unknown_keys=false turns that off for this peek
// only; the real per-type parse afterward uses the normal strict
// default.
struct TypeTag {
    std::string type;
};

template <typename T>
bool try_parse(const std::string& line, std::vector<RecordEvent>& out) {
    T event;
    if (glz::read_json(event, line)) {
        return false;
    }
    out.push_back(event);
    return true;
}

}  // namespace

std::vector<RecordEvent> read_record_events(const std::string& path) {
    std::vector<RecordEvent> events;
    std::ifstream in(path);
    if (!in) {
        return events;
    }

    std::string line;
    std::getline(in, line);  // the type:meta line — already read separately

    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        TypeTag tag;
        if (glz::read<glz::opts{.error_on_unknown_keys = false}>(tag, line)) {
            continue;  // malformed line — skip rather than abort the read
        }
        if (tag.type == "gaze") {
            try_parse<GazeEvent>(line, events);
        } else if (tag.type == "head-prod" || tag.type == "eye-prod") {
            // Two type-field values, one struct — see ProdEvent's own
            // comment (recording.hpp).
            try_parse<ProdEvent>(line, events);
        } else if (tag.type == "image") {
            try_parse<ImageEvent>(line, events);
        } else if (tag.type == "view") {
            try_parse<ViewEvent>(line, events);
        } else if (tag.type == "wait") {
            try_parse<WaitEvent>(line, events);
        } else if (tag.type == "leave") {
            try_parse<LeaveEvent>(line, events);
        } else if (tag.type == "microphone") {
            try_parse<MicrophoneEvent>(line, events);
        } else if (tag.type == "validated" || tag.type == "failed" ||
                   tag.type == "back") {
            // Three type-field values, one struct — see TestStepEvent's
            // own comment (recording.hpp).
            try_parse<TestStepEvent>(line, events);
        }
        // Any other type: skip — forward-compatible with a schema addition
        // this build doesn't know about yet.
    }
    return events;
}

}  // namespace blind_sight
