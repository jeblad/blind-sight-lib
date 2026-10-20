// SPDX-License-Identifier: GPL-3.0-only
//
// protocol.hpp — data model for protocol (.md) files.
//
// Deliberately separate from recording.hpp: that file is the .bs
// recording format itself; this is the *plan* a recording can be run
// against, a completely different concern (markdown, not JSON Lines;
// hand-authored/printed, not machine-appended) that just happens to be
// related. One image plus its task description is one scenario; a
// protocol is several scenarios.

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace blind_sight {

// One scenario: an H2 (only) section of a protocol file. `title` is the
// heading text, doubling as this scenario's identifier in the image
// list and in a recording's ImageEvent::scenario. `image_ref` is the
// target of the first `<image:slug>` marker found under the heading
// (empty if the section has none — falls back to the test-image
// placeholder).
//
// `<image:slug>` is a Markdown autolink (`<scheme:address>`, valid
// CommonMark even though "image" isn't a real URI scheme) rather than an
// ordinary `[text](url)` link — deliberately, since there's no visible
// link text or title attribute this app ever shows anywhere; those would
// just be two more pieces of text to keep in sync with nothing. `slug` is
// a project-local *slug* (images.json), not a filesystem path — a slug
// has no characters that break this marker's own syntax. This parser has
// no opinion on what the slug resolves to; it just extracts whatever's
// inside the marker verbatim. Resolving a slug back to an actual path is
// main.cpp's job — protocol.cpp stays a plain, generic markdown
// reader/writer with no notion of images.json.
//
// `body` is everything else in the section — the facilitator's task
// script, meant to be printed and read from paper, never parsed or
// shown by this app. Kept verbatim so a protocol can be re-saved after
// reordering its scenarios in the UI without losing that text.
struct ProtocolEntry {
    std::string title;
    std::string image_ref;
    std::string body;
};

// A generic named sub-block inside a test step (H4). The parser doesn't
// special-case any specific H4 title ("Expected result" is just one
// common label among several, e.g. hints, time limits, common
// mistakes) — `label` is whatever text follows the `####`, `body` is
// everything under it up to the next H4/H3/EOF. The Protocol window
// shows these collapsed by default, revealed on demand, so a
// facilitator doesn't read the expected result off by accident.
struct TestStepBlock {
    std::string label;
    std::string body;
};

// One test step: an H3 section inside a scenario's body. `title` is the
// heading text. `body` is everything before the first H4 under this
// step — the task description, read by the facilitator before asking
// the participant to attempt it. `blocks` are the H4 sub-blocks that
// follow, in document order.
//
// Parsed from a ProtocolEntry's `body` on demand (parse_test_steps(),
// protocol.cpp) — this is a derived, structured *view* of that text,
// not a second source of truth. `body` itself stays the canonical,
// verbatim text write_protocol_file() reads back out.
struct TestStep {
    std::string title;
    std::string body;
    std::vector<TestStepBlock> blocks;
};

// A scenario's `body`, broken into whatever the Protocol window's
// paged presentation needs: `intro` is the text before the first H3 —
// the image link, any overview prose — shown on its own initial page.
// Empty when `body` has no text before its first H3 — the UI then
// skips this page rather than showing a blank one. `steps` is
// everything parse_test_steps() produced. Both are still a derived
// *view* of `body` — see TestStep's own comment.
struct ScenarioPages {
    std::string intro;
    std::vector<TestStep> steps;
};

// A whole protocol file. `title` is the file's H1 — the only heading
// level reserved for the protocol title, so it can never be confused
// with a scenario's H2, keeping the section boundary unambiguous when
// reordering entries. `source_path` is where it was last opened/saved
// from — empty for a protocol built fresh via "New Protocol".
//
// `intro` is whatever text sits between the H1 title and the first H2
// — the protocol-level counterpart to ScenarioPages::intro, shown as
// the Protocol window's opening page. Empty when the file has no such
// text — the UI then skips straight to the first scenario.
//
// `frontmatter_title`/`frontmatter_description` are plain `key: value`
// lines from the very top of the file, ending at a lone `---` line — not
// a second H1/intro, but a short, dropdown-friendly override for each.
// The relationship runs opposite directions for the two: `title` (the H1)
// is free to grow more descriptive once `frontmatter_title` exists to
// show in a popover instead; `intro` is free-form prose that can run
// long, so `frontmatter_description` is the normal case there, falling
// back to just intro's own first paragraph when unset. See
// resolved_title()/resolved_description() below — never read these two
// fields directly for display.
struct Protocol {
    std::string title;
    std::string intro;
    std::string frontmatter_title;
    std::string frontmatter_description;
    std::string source_path;
    std::vector<ProtocolEntry> entries;
};

// Reads a protocol file (see ProtocolEntry/Protocol above). std::nullopt
// only if `path` itself can't be opened — a file with no H1 or no H2
// sections still parses to a Protocol with an empty title and/or no
// entries rather than failing, since this is a hand-authored plain-text
// format.
std::optional<Protocol> parse_protocol_file(const std::string& path);

// frontmatter_title if set, else the H1 (`title`). "" if neither.
std::string resolved_title(const Protocol& protocol);
// frontmatter_description if set, else `intro`'s own first paragraph. ""
// if neither.
std::string resolved_description(const Protocol& protocol);

// Splits a scenario's own `body` text into its H3-delimited test steps
// (see TestStep's own comment for why this is a derived view, not a
// second source of truth). Empty if `body` has no H3 at all.
std::vector<TestStep> parse_test_steps(const std::string& body);

// Same split as parse_test_steps(), plus the scenario's intro text
// (everything before the first H3). One parse pass, not two: a
// separate parse_test_steps() pass alone would have no way to recover
// that leading text afterward.
ScenarioPages parse_scenario_pages(const std::string& body);

// Writes a protocol back out — each entry's `body` is emitted verbatim,
// so this never needs to reconstruct markdown link syntax itself. Safe
// to call after reordering `entries`; no entry's content depends on its
// position in the vector.
bool write_protocol_file(const std::string& path, const Protocol& protocol);

}  // namespace blind_sight
