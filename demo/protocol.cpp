// SPDX-License-Identifier: GPL-3.0-only
#include "protocol.hpp"

#include <fstream>
#include <optional>
#include <regex>
#include <sstream>

namespace blind_sight {

namespace {

// ATX heading only (`# Title`, `## Title`, …) — the whole controlled
// subset of markdown this format actually needs; no setext (`===`/`---`
// underline) headings, since a hand-written protocol is expected to use
// `#`-style throughout.
const std::regex kHeadingPattern(R"re(^(#{1,6})[ \t]+(.*?)[ \t]*#*[ \t]*$)re");

// `<image:slug>` — a Markdown autolink, not an ordinary `[text](url)`
// link (see ProtocolEntry's own comment, protocol.hpp, for why). One
// capture group: the slug itself.
const std::regex kLinkPattern(R"(<image:([^>\s]+)>)");

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Shared by every "collect a run of raw lines, then flush as one
// trimmed block" site in this file — joins with '\n' and trims the
// result.
std::string join_lines(const std::vector<std::string>& lines) {
    std::ostringstream out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out << lines[i];
        if (i + 1 < lines.size()) {
            out << '\n';
        }
    }
    return trim(out.str());
}

// Everything between one H2 heading and the next (or EOF) is kept
// verbatim as ProtocolEntry::body — this is what makes reordering
// lossless: writing a protocol back out just re-emits each entry's body
// unchanged, no reconstruction of the link syntax needed. image_ref below
// is just parsed *out* of that body for the app's own use, not a
// separate source of truth.
ProtocolEntry make_entry(const std::string& title,
                          const std::vector<std::string>& body_lines) {
    ProtocolEntry entry;
    entry.title = title;

    std::ostringstream body;
    for (std::size_t i = 0; i < body_lines.size(); ++i) {
        body << body_lines[i];
        if (i + 1 < body_lines.size()) {
            body << '\n';
        }
    }
    entry.body = trim(body.str());

    // First `<image:slug>` marker anywhere in the body, not just the
    // first line — tolerant of a blank line (or more) between the
    // heading and the marker.
    std::smatch match;
    if (std::regex_search(entry.body, match, kLinkPattern)) {
        entry.image_ref = match[1].str();
    }
    return entry;
}

}  // namespace

std::optional<Protocol> parse_protocol_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }

    std::vector<std::string> lines;
    {
        std::string raw_line;
        while (std::getline(in, raw_line)) {
            lines.push_back(raw_line);
        }
    }

    Protocol protocol;
    protocol.source_path = path;

    // Frontmatter: plain `key: value` lines from the very top, ending at
    // a lone "---" line — no heading of any level permitted before that
    // boundary. A file starting directly with "# Title" (every existing
    // protocol.md before this feature) hits the heading-match branch on
    // the very first line, undoing anything collected and resuming
    // normal parsing from line 0 — zero behavior change for that case.
    std::size_t start = 0;
    for (; start < lines.size(); ++start) {
        std::smatch frontmatter_heading;
        if (std::regex_match(lines[start], frontmatter_heading, kHeadingPattern)) {
            protocol.frontmatter_title.clear();
            protocol.frontmatter_description.clear();
            start = 0;
            break;
        }
        if (trim(lines[start]) == "---") {
            ++start;  // skip the delimiter itself
            break;
        }
        auto colon = lines[start].find(':');
        if (colon != std::string::npos) {
            std::string key = trim(lines[start].substr(0, colon));
            std::string value = trim(lines[start].substr(colon + 1));
            if (key == "title") {
                protocol.frontmatter_title = value;
            } else if (key == "description") {
                protocol.frontmatter_description = value;
            }
        }
    }

    std::string current_title;
    std::vector<std::string> current_body;
    std::vector<std::string> intro_lines;
    bool in_entry = false;

    auto flush_entry = [&]() {
        if (in_entry) {
            protocol.entries.push_back(make_entry(current_title, current_body));
        }
        current_body.clear();
    };

    for (std::size_t i = start; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::smatch heading;
        if (std::regex_match(line, heading, kHeadingPattern)) {
            int level = static_cast<int>(heading[1].str().size());
            std::string text = trim(heading[2].str());
            if (level == 1) {
                // The protocol's title — H1 is reserved for this, never
                // a scenario. Only the first one found counts; a second
                // H1 later in the file is treated as ordinary body text
                // of whatever section is currently open.
                if (protocol.title.empty()) {
                    protocol.title = text;
                    continue;
                }
            } else if (level == 2) {
                flush_entry();
                current_title = text;
                in_entry = true;
                continue;
            }
            // H3+ isn't a section boundary — falls through to the
            // ordinary body-line handling below, same as any other line.
        }
        if (in_entry) {
            current_body.push_back(line);
        } else {
            // Between the H1 and the first H2 — Protocol::intro.
            intro_lines.push_back(line);
        }
    }
    flush_entry();
    protocol.intro = join_lines(intro_lines);

    return protocol;
}

// Breaks a scenario's body into H3-delimited test steps, each with its
// own H4 sub-blocks. Reuses kHeadingPattern/trim(), one level down (H3
// steps, H4 blocks instead of H1 title, H2 scenario). A stray H4 found
// before any H3 has opened is treated as plain body text rather than
// crashing. Also collects the intro text before the first H3, in the
// same pass rather than scanning the text twice.
ScenarioPages parse_scenario_pages(const std::string& body) {
    ScenarioPages pages;
    std::istringstream in(body);

    std::vector<std::string> intro_lines;
    TestStep current_step;
    std::vector<std::string> step_lines;
    std::string block_label;
    std::vector<std::string> block_lines;
    bool in_step = false;
    bool in_block = false;

    auto flush_block = [&]() {
        if (in_block) {
            current_step.blocks.push_back({block_label, join_lines(block_lines)});
        }
        block_lines.clear();
        in_block = false;
    };

    auto flush_step = [&]() {
        flush_block();
        if (in_step) {
            current_step.body = join_lines(step_lines);
            pages.steps.push_back(current_step);
        }
        current_step = TestStep{};
        step_lines.clear();
    };

    std::string line;
    while (std::getline(in, line)) {
        std::smatch heading;
        if (std::regex_match(line, heading, kHeadingPattern)) {
            int level = static_cast<int>(heading[1].str().size());
            std::string text = trim(heading[2].str());
            if (level == 3) {
                flush_step();
                current_step.title = text;
                in_step = true;
                continue;
            } else if (level == 4 && in_step) {
                flush_block();
                block_label = text;
                in_block = true;
                continue;
            }
        }
        if (in_block) {
            block_lines.push_back(line);
        } else if (in_step) {
            step_lines.push_back(line);
        } else {
            intro_lines.push_back(line);
        }
    }
    flush_step();
    pages.intro = join_lines(intro_lines);
    return pages;
}

std::vector<TestStep> parse_test_steps(const std::string& body) {
    return parse_scenario_pages(body).steps;
}

std::string resolved_title(const Protocol& protocol) {
    return !protocol.frontmatter_title.empty() ? protocol.frontmatter_title : protocol.title;
}

std::string resolved_description(const Protocol& protocol) {
    if (!protocol.frontmatter_description.empty()) {
        return protocol.frontmatter_description;
    }
    // intro's own first paragraph — intro as a whole can run to several
    // paragraphs, this is the natural "short version" when no override
    // is set.
    auto blank = protocol.intro.find("\n\n");
    return trim(blank == std::string::npos ? protocol.intro : protocol.intro.substr(0, blank));
}

bool write_protocol_file(const std::string& path, const Protocol& protocol) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    if (!protocol.frontmatter_title.empty() || !protocol.frontmatter_description.empty()) {
        if (!protocol.frontmatter_title.empty()) {
            out << "title: " << protocol.frontmatter_title << "\n";
        }
        if (!protocol.frontmatter_description.empty()) {
            out << "description: " << protocol.frontmatter_description << "\n";
        }
        out << "---\n";
    }
    out << "# " << protocol.title << "\n";
    if (!protocol.intro.empty()) {
        out << "\n" << protocol.intro << "\n";
    }
    for (const auto& entry : protocol.entries) {
        out << "\n## " << entry.title << "\n\n";
        out << entry.body << "\n";
    }
    return static_cast<bool>(out);
}

}  // namespace blind_sight
