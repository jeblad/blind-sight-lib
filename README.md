# blind-sight-lib
Concept lib for Blind Sight — a gaze-steered synthetic-echo sensory-substitution system for blind and severely visually impaired users

## Status: early work in progress

This project is under active development, and large parts of the intended
solution are still missing. Expect things to change, break, or be
incomplete.

The demo simulates what an advanced pair of AI glasses would do — sense
depth, follow the wearer's gaze, and turn both into sound — so the concept
can be explored and tested without the cost of developing hardware first.
What exists today is a **demo application** (`demo/`) for running and
recording such test sessions: viewing images and panoramas with depth maps,
simulated gaze and head "prods" (probing the scene at one point: a short
sound pulse whose echo returns after a delay set by the depth there), a
per-participant audio monitoring matrix with voice levelling, and session
recording and playback with transcription. It runs on Linux with PipeWire
only.

The demo is a testbed for parts of the concept, not an implementation of it.
In particular, it does **not** implement an adequate psychoacoustic model,
and it has **not** yet been used to establish the minimum the sound must
convey for spatial orientation and awareness.

What is **not** there yet:

- **The acoustic scene itself.** The synthesized sound is a single noise
  pulse with an echo delayed by the distance at the gaze point, identical
  in both ears. Distance as repetition pitch, structure as timbre,
  direction through interaural cues and HRTFs, and motion are not
  implemented.
- **A library API.** Everything lives in the demo app for now.
- **Eye tracking.** The mouse pointer stands in for an eye tracker.
- **Stability.** File formats, settings and interfaces change without
  notice, and older recordings or settings may not carry over.

## Building and running

Linux only, with PipeWire as the sound server. Developed on Ubuntu; the
package names below are Ubuntu/Debian ones.

### Prerequisites you install yourself

CMake cannot install system packages, so these have to be in place first.

**To build:**

```bash
sudo apt install cmake ninja-build g++ pkgconf git gettext libglib2.0-bin \
    libgtkmm-4.0-dev libadwaita-1-dev libgexiv2-dev libgstreamer1.0-dev \
    libgudev-1.0-dev libopenexr-dev libsecret-1-dev libpipewire-0.3-dev \
    ladspa-sdk
```

- A C++23 compiler; the project is developed with GCC 15.
- `git` and network access on the first configure: CMake downloads the
  [glaze](https://github.com/stephenberry/glaze) JSON library.
- `ladspa-sdk` only provides `ladspa.h`, for the voice-levelling plugin
  (see below).
- `gettext` provides `msgfmt`, which compiles the translations in `po/`.

**To run:**

```bash
sudo apt install pipewire pipewire-bin wireplumber libpipewire-0.3-modules \
    libspa-0.2-modules gstreamer1.0-pipewire gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good python3
```

- A running PipeWire session with WirePlumber, the default on current
  Ubuntu desktops.
- `bluez` if you use Bluetooth audio devices.

### What CMake does for you

- Downloads glaze, compiles the GSettings schema and the translations, and
  builds the app and the voice-levelling plugin.
- Checks for the runtime prerequisites above. If one is missing it prints
  a warning naming the package, and the build still succeeds; the
  affected feature (audio routing, voice levelling, recording or
  playback) then fails at runtime.

### Voice levelling: PipeWire filter chains

Every microphone runs through its own voice leveller (a slow automatic
gain control, then volume and mute) before anyone hears it. Each leveller
is a PipeWire filter chain that the app starts as a separate
`pipewire -c <config>` process, with the leveller itself as a LADSPA
plugin inside it. For that to work:

- the `pipewire` program (`pipewire-bin`) runs each chain as a client of
  your PipeWire session;
- PipeWire's `filter-chain`, `protocol-native`, `client-node` and `adapter`
  modules (`libpipewire-0.3-modules`) are installed;
- the SPA plugins for format conversion, support and hosting LADSPA
  plugins (`libspa-0.2-modules`) are installed;
- the LADSPA plugin itself, `blind_sight_voice.so`, is built by CMake and
  loaded straight from the build directory.

Nothing needs configuring in PipeWire, and nothing in `~/.config` is
changed: the app writes each chain's configuration, and a socket the
levellers report microphone levels on, under
`$XDG_RUNTIME_DIR/blind-sight/`. Because the plugin is loaded by its
absolute path in the build directory, don't move or delete that directory
while the app is in use, and rebuild after moving the source tree.

### Build and start

```bash
cmake --preset default
cmake --build --preset default
build/default/demo/blind-sight-demo
```

The `default` preset limits parallel build jobs so the build fits on a
machine with 16 GB of memory.

To try it, open `scratch/example` with "Open Project…": a small example
project with three photospheres, two of them with ready-made depth maps.

## Recording format

The demo's Record and Playback modes read and write session logs as
[JSON Lines](https://jsonlines.org/) — one JSON value per line — using the
`.bs` extension rather than the generic `.jsonl`, to signal that these files
follow this project's own schema. The schema is still evolving; see
[`demo/recording.hpp`](demo/recording.hpp) for its current shape.

## Optional AI tooling: depth maps and transcription

Two features run local AI models as a one-shot subprocess, triggered
automatically when needed (opening a photosphere without a depth map,
or a Record session finishing a segment) rather than through any menu:
a precomputed depth map for photospheres (`demo/depth/`), and a
transcript of recorded dialogue via [WhisperX](https://github.com/m-bain/whisperX)
(`demo/transcribe/`). Neither is a hard dependency of the app itself —
without setup, the app just shows a one-time notice explaining what to
run, and works normally otherwise.

Each needs its own one-time setup, run by hand from a terminal:

```bash
demo/depth/setup-depth-env.sh
demo/transcribe/setup-transcribe-env.sh
```

**These download real disk space, worth knowing before you run them.**
Each script creates its own Python virtual environment under
`$XDG_CACHE_HOME` (falling back to `~/.cache` if that's unset) —
never inside this source tree — plus model weights cached separately
under `~/.cache/huggingface`. Rough sizes, measured on the project's own
dev machine:

- **Depth generation**: well under 1GB total (a CPU-only PyTorch install,
  ~600MB, plus the depth model itself, ~100MB).
- **Transcription**: **1–2GB at the low end, but potentially 8–10GB.**
  PyTorch alone is ~600MB CPU-only, but if a CUDA-capable GPU is
  detected it installs a much larger CUDA build instead — measured
  ~5.5GB on this machine. On top of that, WhisperX downloads a
  transcription model (~500MB) and a language-specific alignment model
  whose size varies a lot by language — a few hundred MB for some
  languages, but as much as **3.6GB** for at least one observed case
  (Norwegian Bokmål).

Both venvs are per-user and fully regenerable — safe to delete and
re-run the relevant setup script if you need the space back.

## Utility scripts

A few standalone scripts under `demo/`, run by hand from a terminal — none
are wired into the build or run automatically by the app itself:

- `demo/install-desktop-entry.sh` — installs a `.desktop` launcher entry and
  app icon for the current user, so the demo shows up in the GNOME
  dash/taskbar with its own icon instead of a generic one.
- `demo/reencode-wav-to-opus.sh [--delete] DIR` — catches up any recorded
  session whose WAV audio segments never got transcoded to Opus (e.g.
  because "Transcode Audio to Opus" was off during that recording).
  Recursively scans `DIR`, typically a project directory, and encodes every
  `.wav` missing a sibling `.opus`, using the same GStreamer pipeline the
  app itself uses. Needs `gst-launch-1.0` (`gstreamer1.0-tools`). Keeps the original WAV unless `--delete` is passed — a
  session recording may be the only copy of that data.

## Audio latency

Gaze/saccade-driven sonification needs the whole render loop (gaze → depth
lookup → binaural render) to finish inside the inter-saccade interval
(~200–300ms) — audio output latency eats directly into that budget, so it's
worth knowing where it comes from before relying on this app for anything
timing-sensitive.

Two built-in tools help with this, both from the hamburger menu:

- **"Trace Delay…"** measures the real round-trip latency to whatever's
  currently playing — a short click, picked up by the microphone, timed
  against when it was actually sent. Needs the speaker-to-microphone
  distance (to subtract out sound's own travel time) and reports PipeWire's
  own scheduling overhead and the output codec alongside the measured
  number.
- **A one-time toast** warns automatically whenever the default audio output
  is Bluetooth without a specifically low-latency codec (aptX-LL,
  FastStream) — ordinary Bluetooth (SBC, SBC-XQ, AAC, aptX/aptX-HD, LDAC)
  typically adds 150–400ms on its own, already bigger than the entire
  inter-saccade budget above.

Separately, PipeWire's own scheduling quantum (default 1024 samples ≈ 21ms
at 48kHz) can be lowered for a modest additional win, at the cost of a
higher risk of audio glitches/xruns under load — this is a system-wide,
per-user PipeWire setting, not specific to this app:

```bash
mkdir -p ~/.config/pipewire/pipewire.conf.d/
cat > ~/.config/pipewire/pipewire.conf.d/10-lowlatency.conf <<'EOF'
context.properties = {
    default.clock.quantum = 256
}
EOF
systemctl --user restart pipewire pipewire-pulse wireplumber
```

## Data protection and research ethics

Using this app to record a session with a real person can mean collecting
information about their disability together with their subjective
experience of an assistive technology. Within the EU/EEA, that combination
falls under **GDPR Article 9** as a *special category* of personal data
(health data / data concerning disability), which carries a higher bar
than ordinary personal data — explicit, informed consent, and typically
stricter limits on storage, purpose, and retention.

If you intend to use this app for any kind of methodical testing with real
participants, check your institution's research ethics process before
collecting anything — you may need formal approval first. In Norway,
[Sikt](https://sikt.no/) (formerly NSD, Personvernombudet for forskning) is
the standard point of contact for this, including whether a given project
needs to be registered at all.

## Acknowledgements

Created with assistance from AI tools (Gemini, Claude Sonnet and Claude
Opus) across all parts of this work.

This project was developed independently, with no external financial or
institutional support other than the AI tools mentioned.

## License

blind-sight-lib is free software under the **GNU General Public License,
version 3 only** (`GPL-3.0-only`) — see [LICENSE](LICENSE).

Version 3 *only* is deliberate: the "or (at your option) any later version"
clause is **not** granted. The `LICENSE` file holds the verbatim GPLv3 text
and is left unmodified; this statement, together with
`SPDX-License-Identifier: GPL-3.0-only` headers in the source files, is what
fixes the interpretation to version 3 only.

### Exceptions: artwork and example material

The files under `resource/` and `scratch/` (artwork, icons, and the
example projects with their images, depth maps and protocols) are
**exempt** from the GPL. Each carries its own license in its metadata or
SPDX header; a file that states none is © John Erling Blad, all rights
reserved.

For example, the Blind Sight bat logo and app icon
(`resource/blind-sight-bat.svg` and its two variants in
`resource/icons/`) are © John Erling Blad under
[CC BY-NC-ND 4.0](https://creativecommons.org/licenses/by-nc-nd/4.0/),
while the icons taken from Ubuntu's
[Yaru](https://github.com/ubuntu/yaru) theme are © 2018 Sam Hewitt under
[CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
