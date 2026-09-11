# notes.md — blind-sight-lib working note

The evolving design note for this library: what it has to do, the reasoning
behind each choice, questions still open, and decisions as they are made. Kept in
this repo so a session can work on the library without needing the private
concept repo.

Origin: this restates, in original wording for a GPL repo, the render-side design
worked out in the concept note (`github.com/jeblad/blind-sight`, private,
all-rights-reserved). Ideas carry over; text does not. This repo is the public
**demo that Blind Sight works** — not a place to write out the concept's full
rationale. Follow-up items for the concept note go to
`~/paper/blind-sight/additions.md`.

## Purpose

Input: a stream of depth frames, plus a gaze point per frame. Output: a binaural
audio stream in which the surface the user is looking at is heard — its distance,
its size/structure, its direction, and its motion. Real time, low latency, close
enough to a natural sound field that ordinary spatial hearing does the
interpreting.

## The render pipeline (target shape)

1. **Gaze → region of interest.** Project the gaze point into the depth frame.
   Take the surface patch around it at full temporal rate; the rest of the
   forward cone can update at a lower rate.
2. **Patch → acoustic parameters.**
   - *Distance* of the patch → repetition pitch. Timing-based pitch: a delayed
     copy of a broadband carrier produces a pitch at 1/delay. Mapping distance
     to delay is the compression curve (see open questions).
   - *Structure / size* → spectral shape. A larger, flatter surface returns more
     low-frequency energy → darker timbre; smaller or more broken structure →
     brighter. Derive this from the patch geometry (extent, normal spread), not
     from a single "size" scalar.
   - *Direction* of the patch relative to the head → ITD and ILD for azimuth,
     HRTF spectral cues for elevation. A structural (non-individualized) HRTF is
     acceptable because only a forward cone is rendered.
3. **Parameters → additive synthesis.** A parallel filterbank fed from the depth
   patch: each band a partial of the rendered sound, amplitudes and delays set
   by the parameters above. Sum to a binaural pair.
4. **Motion.** Between frames, decompose patch motion into:
   - *tangential* (across the field of view) → angular displacement of the
     source in the binaural image;
   - *radial* (approach / recede) → Doppler shift applied to the patch's sound.

## How much to render — additive, and the "caricature" question

Start minimal: one patch, distance + direction audible. Add structure/timbre,
then motion. The concept work frames deliberate exaggeration — compressed
distance, sharpened edges — as something the demonstrator should test, the way
vision tolerates a caricature. For the library that means: every mapping
(distance→pitch, size→timbre, motion→Doppler) is a parameterised curve with a
"natural" setting and room to push past it. Do not bake in a fixed amount of
stretch.

---

# Session decisions — 2026-09-10

Worked through architecture with John. Stack was left open early on ("for
tidlig") and only the demo GUI was pinned late in the session. Everything below
is a working decision unless flagged open.

## Repo & licensing

- One repo, `github.com/jeblad/blind-sight-lib`, **GPL-3.0-only**.
- `LICENSE` = verbatim GPLv3 text, **left unmodified** (it is license-detection
  metadata). "-only" is expressed by: a License section in `README.md` stating
  version 3 only and that the "or later" clause is not granted; and
  `SPDX-License-Identifier: GPL-3.0-only` headers in source files, using the
  notice variant *without* "or (at your option) any later version".
- GitHub history: one commit ("Initial commit", GitHub's README+LICENSE), no
  later edits to `LICENSE`.
- Layout:
  ```
  blind-sight-lib/
    lib/            # the library — C header + C++ impl, arena, kernels
    demo/           # GTK app: load RGB-D, synthesize viewport, render audio
    data/           # sample panorama(s) to develop against
    CMakeLists.txt          # top-level; demo links blindsight
    CMakeUserPresets.json   # 6 build / 4 test (John's hardware cap)
  ```
  `lib/` has no upward dependency on `demo/`. Separate repo for the demo only
  if it grows to need it — not now.

## Library architecture

- **Public interface: C ABI** (`extern "C"`, C header). Simpler for JNI /
  prototype integration and gives a stable ABI.
- **Implementation: C++** internally (RAII, templates for the SIMD wrappers,
  readable kernels). Conservative subset: no exceptions across the C boundary
  (`-fno-exceptions`, error codes out), no RTTI, no allocating `std::`
  containers on the hot path.
- **One backend, selected at compile time.** No runtime dispatch, no vtables.
  A build option (e.g. `BLINDSIGHT_SIMD = neon | sse | scalar`) derived
  automatically from the target arch. Android build → neon, desktop → sse.
  The **scalar build is the reference / correctness oracle** and the fallback
  for any CPU with no chosen SIMD path.
- **SIMD seam: a thin `simd.h`** — a handful of 4-wide float primitives
  (load/store, add, mul, fma, broadcast) mapping 1:1 to NEON `float32x4_t`,
  with SSE and scalar variants under `#ifdef`. Every kernel is written in those
  primitives. "Eye the NEON structure" = design everything around 4-wide float
  ops; desktop SSE then falls out for free.
- **Data layout for that:** SoA, 16-byte aligned, tap/band counts padded to
  multiples of 4, hot buffers contiguous.
- **Memory: JPL "Power of Ten" rule 3** — no dynamic allocation after init.
  All allocation in an init phase. Bounded loops, no recursion on the hot path.
- **Memory ownership: hybrid — opaque handle, caller provides the backing
  store.**
  - `size_t blindsight_arena_size(const blindsight_config*)` — pure function.
  - `blindsight_t* blindsight_init(const blindsight_config*, void* arena,
    size_t size)` — the library places all its structures in `arena` and
    returns an opaque pointer (an offset into the block). The library never
    calls `malloc`.
  - `blindsight_deinit` for symmetry / teardown.
  - Works with heap, static, stack, or a custom pool; no allocator dependency;
    "zero allocation after init" is literally true and auditable.
  - Debug builds: canaries / guard zones around the arena, bounds-assert every
    kernel. Harness pads the arena with poisoned margins and checks them after
    golden tests. Gives an attributable blast radius for a stray write.
- `std::` containers are fine **internally** if they do not allocate after init
  (`std::array`, `std::span`, `std::vector` reserved once). Never across the C
  boundary — POD only there.

## Library modules (concerns; one built lib, separate headers)

- **A — acoustic scene.** Depth patch + gaze (+ optional scene annotations) →
  binaural. Voice pool, structural HRTF, comb filter, mix.
- **B — image / depth prep.** Raw depth → cleaned depth field + derived features
  (normals, confidence), optional RGB-derived enhancement (category / texture
  hints). **A degrades gracefully without B** — geometry-driven, not
  recognition-dependent. *Open:* one prep lib, or split geometric cleanup from
  recognition-enhancement (the latter pulls heavy deps like a segmentation net).
  Leaning one now, recognition as an optional module later.
- **Gaze-event detection** (candidate small module). Gaze stream →
  fixation / saccade / blink phases + fixation points. Needed in the prototype
  to classify the real eye signal. **Distinct from saccade *synthesis*** (which
  is demo-side, below).
- Three modules with their own headers inside one built lib. Split into separate
  artifacts only when the prototype team needs to take just one. Keep the seam
  minimal — function-pointer structs at most, not a plugin framework. Expect the
  kernel math to be rewritten several times before distance and direction sit.
- Keep the input contract with a **slot for head-pose** later (world-stabilized
  audio) even though the static demo does not need it. Do not assume a full
  sphere (rear field is later).

## I/O contract shape

- Library consumes `(depth frame, gaze x,y)`.
- **Depth semantics: radial distance along the ray** (true path length), not
  perpendicular z — distance→τ wants real path length. Remaining contract
  details (format, units, coordinate frame, gaze representation, timestamps)
  still open.
- **API split:**
  - `set_inputs(depth_frame, gaze_xy)` — cheap, called from the UI thread,
    swaps the working data. Head-turn → new depth frame. Mouse move → new
    `(x,y)`. The library does not care which changed.
  - `render(out, n_samples)` — continuous, called from the audio callback at a
    fixed block size. Safe to call with no new input (produces from current
    state).
  - Between them: latest-input-wins, lock-free (double-buffer + atomic swap).
    No locks, no allocation in the callback.
- **Parameter smoothing is mandatory.** τ, ITD/ILD, band gains etc. ramp to
  target over a few ms (per-block linear ramp or one-pole) or you get
  zipper / clicks.

## Interaction & render model

- Mouse = continuous **gaze direction**. It only matters at the moment of a prod.
- **Click = "prod"** — an explicit trigger.
- Each prod spawns a **voice** from a **fixed-size pool** (config-sized, in the
  arena). Pool full → steal oldest / quietest.
- A **voice** = a snapshot of the render parameters at the prod point (depth
  patch, direction, distance→τ, spectral shape) + its own envelope
  (near-instant attack, decay/release over tens–hundreds of ms — a tuning
  parameter).
- **Static demo:** voice frozen at snapshot + envelope. No scene motion, fixed
  head → no tracking, no Doppler, no motion hints.
- **Active-ping model.** Prod at `t_click`, anchored to a gaze patch. The echo
  gets a **scheduled onset** = `t_click + return_delay(distance)`, non-linear.
  Nothing audible from that ping before onset. Then the voice lives briefly and
  dies.
- **Two distinct distance-driven delays — keep them separate:**
  1. **Return delay** — `t_click` → onset. The "ping went out and came back"
     feel. Tens–hundreds of ms.
  2. **Repetition-pitch τ** — inside the voice's timbre (comb filter), ~1–30 ms,
     encodes distance as pitch. This is the one the concept note already
     describes.
- **Voice pool has three states:** *pending* (scheduled, not sounding) →
  *sounding* → *done*. Arena sized for pending + active simultaneously — you can
  prod several times before the first echo starts.
- **Emergent:** prod a near object right after a far one → the near echo returns
  first ("near things answer faster"). Free from the model, likely perceptually
  meaningful. Noted in `additions.md` as a candidate pitch point.
- `render` runs continuously but sums **only active voices**; none active →
  silence. That *is* the concept's gaze-gated non-continuous sound: the mixer
  always runs, silent between prods except for the tails.
- **Rapid clicks:** each click is an independent prod; the pool carries the
  concurrency.
- **A/B fork — when prod parameters are latched relative to the saccade:**
  - A: at click (anchor at the click point). **Default for now.**
  - B: at saccade landing (anchor at the landing point) — more faithful to the
    concept, more bookkeeping. Deferred with the rest of the tuning.
- Tuning numbers (return-delay curve, envelope length, ratio between the two
  delays, patch size) left open — swept when it plays.

## Saccade model (demo-side)

Lives in the demo's mouse→gaze adapter: the mouse gives discrete targets, the
adapter synthesizes a realistic gaze trajectory (a real eye tracker would give
the whole trajectory). The prototype does not use this — it has the real
trajectory. **Full model and sources are in `~/paper/blind-sight/additions.md`.**
Summary:

- Duration (main sequence): `D = D0 + k*A`, `D0 ≈ 20–25 ms`, `k ≈ 2–2.7 ms/deg`,
  clamp `D ≥ ~20 ms`. `A` = angular distance to target in degrees.
- Trajectory (minimum-jerk): `s(τ) = 10τ³ − 15τ⁴ + 6τ⁵`,
  `gaze(t) = start + (target − start)·s(τ)`, `τ = clamp(t/D, 0, 1)`. Zero
  velocity at both ends.
- Upgrade: gamma-shaped skewed velocity profile (Van Opstal & Van Gisbergen
  1987), skew grows with `A`.
- Mid-flight retarget: new saccade from the current position; optionally carry
  velocity.
- Deferred: dynamic overshoot, saccade latency, fixation interval.

## Demo architecture

- Panorama + viewport synthesis is **entirely demo-side**. The library still
  just gets `(depth frame, gaze)`.
- **Sensor-emulation layer** (demo-side) — exists only because the demo replaces
  real sensors with synthetic input. Each part produces exactly the stream the
  prototype gets from hardware, so the **library cannot tell it is being driven
  by a demo**:
  - **Saccade synthesis** — discrete mouse targets → realistic gaze trajectory.
  - **Inverse IMU** — keyboard / drag → head-orientation stream with plausible
    slew dynamics. Feeds the demo's viewport renderer, **not** the library (the
    static scene does not need head orientation).
  - **Panorama loader + viewport renderer** — the "depth camera emulator":
    produces a head-oriented depth frame.
- **Loader:** vendor-agnostic. `(RGB, depth, depth encoding/units, projection
  type)` → normalized internal form. Start with one concrete format — likely
  16-bit PNG depth + color, equirectangular. Stereo later = another loader
  (stereo pair → disparity → depth) behind the same normalized form; for us
  stereo is just a depth source, not a separate track.
- **Viewport synthesis:** pinhole camera, choose yaw/pitch (+ roll) and FoV
  (~60–90° for a natural / rectilinear look, not a distorted equirect crop).
  Per output pixel: ray direction → lon/lat → bilinear sample of the RGB and
  depth maps. Out = a perspective RGB-D. Demo then converts panorama depth to
  the library's convention (radial along the ray).
- **Head:** fixed position (panorama capture point), **never translation**.
  Orientation variable = viewport yaw/pitch/roll. "Move the viewport" = turn
  the head. Moving through the scene needs video — deferred.
- **Two separate demo controls:** orientation (keyboard / drag) and mouse
  pointer.
- Demo uses **one image at a time**. Audio **runs continuously** while the view
  changes — view changes modulate the sound, they do not re-trigger it.
- **First milestone (tight):** window up, load one RGB-D file, show the color
  image (+ depth as a grayscale toggle). No viewport synthesis, no audio, no
  lib link yet. Just "we see an RGB-D."

## Demo stack

- **GTK4** — the established GNOME path (not GTK3, not a light toolkit; John has
  bad experience with "light" solutions). Target GNOME 50, expected to keep
  working GNOME 51 / 52.
- **libadwaita** optional for a dev harness — start without, add for adaptive
  layout / styling if wanted.
- **Language: C++ with gtkmm-4.0** (recommended — official bindings, tracks
  GTK4, no second toolchain vs. the C/C++ lib). Alternatives considered:
  Python + PyGObject (fastest iteration), Rust + gtk-rs (extra toolchain).
  *Open — John to confirm.*
- **Build: all CMake**, John's presets + 6/4 caps. Demo target pulls
  `gtkmm-4.0`, `libadwaita-1`, `libpipewire-0.3` via `pkg-config`. GNOME
  convention is Meson but nothing here needs it.
- **Image display:** `GtkDrawingArea` + `GdkTexture` in `snapshot` to start
  (CPU frame + gaze-cursor overlay). `GtkGLArea` as the upgrade if the viewport
  warp wants the GPU.
- **Audio out: PipeWire native** (`libpipewire`) — the established path for
  continuous low-latency output on modern Linux / GNOME.

## Compute budget

- **Baseline assumes CPU only** (portable scalar + NEON). Must meet latency
  **and sustained thermal budget** on ARM64 alone — that is the glasses case.
  The budget is *sustained*, not peak (thermal throttling is the real ceiling).
- **Special HW = optional pluggable compute backend** behind the compile-time
  selection: Hexagon DSP, Vulkan compute, OpenCL, CUDA (puck / dev box only,
  never on the glasses' critical path). Not writing these now — making them
  drop-in.
- **Algorithm chosen to fit the CPU budget first:** sparse decomposition (tens
  of image-sources, not per-pixel), bounded tap count (~5–12), block
  processing. Acceleration is headroom for density / quality, not for
  correctness.
- **Skip NNAPI** (deprecated in Android 15). Anything tensor-shaped → Vulkan
  compute or a hand kernel.
- **Necessary-for-prototype checklist:**
  - the compute-backend seam exists even if only scalar / NEON is implemented;
  - the core cross-compiles to Android `arm64-v8a` via the NDK — kept as a
    build target so we do not drift;
  - bounded, known memory footprint; no alloc / locks in the render callback;
  - a benchmark in the repo measuring render time against an explicit budget,
    ideally on real ARM64;
  - end-to-end latency measured against the inter-saccade budget
    (~200–300 ms, ideally far faster).

## Android / native facts (established)

- Android runs compiled native code via the NDK (`.so` libs, JNI /
  `System.loadLibrary`, or `NativeActivity`). Native code does **not** require
  sideloading — sideloading is about install source, not code type. But the
  prototype glasses will likely be sideloaded anyway (AOSP without Play, custom
  hardware, fast iteration) or the lib baked into a system image.
- Audio backends: desktop PipeWire / PortAudio / RtAudio; Android AAudio
  (API 26+) / Oboe. Audio I/O must be a swappable backend; the core DSP must
  not bind a desktop audio API. Same real-time discipline both places (no
  allocation, no locks in the callback).

## Process conventions

- Chat with John in Norwegian; all code, docs, comments, commit messages in
  English.
- John does **all** git himself. Draft commit messages, hand them back. No
  AI-authorship line in commits.

---

# Session decisions — 2026-09-11

First working code: the demo's first app window, up through a hamburger menu
and its three dialogs. Demo GUI language question from 2026-09-10 is now
resolved: **gtkmm4, C++**.

## Naming: `blind-sight`, not `blindsight`

**One word always hyphenated: `blind-sight`.** `blindsight` (no hyphen) is an
established neuroscience term — residual visual response via subcortical
pathways in cortically blind patients (Weiskrantz) — a real, distinct
phenomenon. Squashing our name into one word collides with it. Applies
everywhere a single token is needed: executable name (`blind-sight-demo`),
app ID (`io.github.jeblad.blind-sight-demo`), gettext domain
(`blind-sight-demo`), `.pot`/`.po`/`.mo` filenames, CSS classes, CMake
compile-definition prefixes (`BLIND_SIGHT_*` — hyphens aren't valid in C
preprocessor identifiers, underscore is the equivalent break there). Free
text can still write "Blind Sight" or "blind sight" naturally.

## First app window (GTK4 + libadwaita, C++ via gtkmm4)

- Structure: `Gtk::Application` (created via gtkmm's own factory, not wrapped
  from a raw `AdwApplication*`) + `AdwApplicationWindow` (C API, wrapped with
  `Glib::wrap()`) holding an `AdwToolbarView` with an `AdwHeaderBar` top bar.
- **Two libadwaita/gtkmm interop crashes found and fixed, both worth
  remembering:**
  1. Wrapping a raw `AdwApplication*` with `Glib::wrap()` before any gtkmm
     object had been constructed through gtkmm's own factory crashed with
     `wrap_func_table != nullptr` — gtkmm's per-module wrap tables aren't
     registered until a real gtkmm object exists. Fix: create the app via
     `Gtk::Application::create()` (a genuine gtkmm object — this alone
     registers the tables) and call `adw_init()` explicitly instead of
     routing the *application* through `AdwApplication`. `AdwApplicationWindow`
     only requires a `GtkApplication`, not specifically an `AdwApplication`,
     so nothing is lost. The window itself is still built via the C API +
     `Glib::wrap()` — safe once the app has gone through gtkmm's factory.
  2. `AdwApplicationWindow` rejects both `Gtk::Window::set_child()` (silently
     wrong) and `set_titlebar()` (hard `Adwaita-ERROR`, aborts the process).
     Its content must go through `adw_application_window_set_content()` /
     `AdwToolbarView` instead.
- Without a titlebar widget, the window has **no close button at all** —
  GNOME/Mutter draws no server-side decorations under Wayland. Confirmed
  fixed: clean shutdown via the X button verified (exit code 0, and directly
  by John).
- No background threads yet, so the default `Gtk::Application` shutdown path
  is sufficient. **Forward obligation:** once the audio render thread lands,
  it must be explicitly stopped and joined on window close-request / app
  shutdown, or it becomes the dangling-process failure this milestone was
  meant to rule out.

## Splash content and styling

- Three centered lines over the splash image: "Blind Sight" (`title-1`),
  tagline, copyright — in a `Gtk::Overlay`.
- `.osd` (tried first) gives translucent backing with no control over how
  translucent, no rounding, no real padding — and `.dim-label` (tried for the
  copyright line) sets **opacity** on the label itself, i.e. actually
  transparent text, which is never wanted. Fixed with a small dedicated
  `Gtk::CssProvider` stylesheet instead: explicit `border-radius`, explicit
  `background-color` alpha (settled at `rgba(0, 0, 0, 0.75)` after a
  round-trip — "40%" and "darker" pointed opposite directions; confirmed
  darker), explicit `padding` inside the card, and `color` always fully
  opaque (the copyright line uses a dimmer *opaque* gray, not opacity).
- `margin` (gap from the window/overlay edge) and `padding` (gap from the
  card edge to the text) are different things — the first draft only had the
  former.

## i18n

- All user-facing strings through `_()` (gettext); **English is the source
  (msgid) language**, matching the rest of the repo. Norwegian is the first
  `.po` translation.
- Pipeline (manual, no CMake automation, mirrors `~/spaces/gnome-agent-proxy`):
  `po/POTFILES.in` → `xgettext` → `po/blind-sight-demo.pot` → `po/nb.po` →
  `msgfmt` → `locale/nb/LC_MESSAGES/blind-sight-demo.mo`, `.mo` committed
  directly.
- `translator-credits` follows the gettext/GNOME convention: the msgid is
  literally `"translator-credits"`; each `.po`'s *translation* of it is that
  language's translator name(s) — not application-side data. `po/nb.po`
  translates it to "John Erling Blad".
- Caveat: a string inside a raw C++ string literal (e.g. embedded UI XML)
  is **not** picked up by `xgettext` even if marked `translatable="yes"` —
  that attribute only matters when a `.ui` *resource* file is processed by
  dedicated i18n tooling. Relevant if `GtkBuilder` XML is ever embedded
  inline again.

## Hamburger menu + the three dialogs

- `AdwHeaderBar`, menu button (`open-menu-symbolic`) at the end, backed by a
  `Gio::Menu` → `app.preferences`, `app.shortcuts`, `app.about`.
- **About** (the "essential" one) — `AdwAboutDialog`: developer name/list
  ("John Erling Blad"), `translator_credits` via the gettext convention
  above, copyright, website + issue URL (GitHub), placeholder app icon
  (`audio-speakers-symbolic` — a real icon is a separate task).
  `GTK_LICENSE_GPL_3_0` is used for the dialog's license badge/link, but that
  enum has **no -only vs -or-later distinction** — the authoritative
  statement stays `LICENSE` (verbatim) + the SPDX headers, not this dialog.
- **Preferences and Shortcuts are empty stubs**, as agreed — both currently
  the same bare `AdwPreferencesDialog` with no pages.
- **Finding: `GtkShortcutsWindow` (and Section/Group/Shortcut) is deprecated
  as of GTK 4.18** (this system has 4.22) — moved to a `deprecated/` header,
  `gtk_application_window_set_help_overlay()` deprecated too, no official
  replacement widget yet. Decided not to build stub content against a widget
  family on its way out; Shortcuts uses the same empty-dialog placeholder as
  Preferences for now. Revisit — real shortcut content, and whatever pattern
  GNOME settles on — once there are app shortcuts worth documenting.

## Compiler hardening

- Adopted the OpenSSF Compiler Hardening Guide flag set from
  `~/spaces/gnome-agent-proxy`'s `CMakeLists.txt` (FORTIFY_SOURCE=3,
  stack-protector-strong, stack-clash-protection, RELRO/BIND_NOW, cf-protection,
  `-Wconversion`, etc.), plus ccache detection and an in-source-build guard.
- Two fixes needed versus the reference project's version: (1) it joins the
  flags into one string and sets `CMAKE_CXX_FLAGS` directly, which works for
  that raw-string mechanism but breaks `add_compile_options()` (which takes
  each list item as its own argument — a joined string becomes one malformed
  argument); flags are listed separately instead. (2) split linker-only
  `-Wl,...` flags into `add_link_options()` rather than passing them to every
  compile invocation.
- **`-Werror` deliberately not carried over.** The reference project is a
  small, fully first-party daemon; this one links gtkmm/libadwaita/glib
  headers we don't control, and `-Wconversion` + `-Werror` against
  third-party headers risks an unbuildable tree from a toolchain bump we
  didn't cause. Revisit once `lib/` (our own code, no GTK headers) has its
  own target.

## Housekeeping

- `SPDX-License-Identifier: GPL-3.0-only` headers added to the source files
  that existed (`CMakeLists.txt`, `demo/CMakeLists.txt`, `demo/main.cpp`) —
  closing the loop opened in "Repo & licensing" above; was decided but not
  actually done until now.
- `package.json` + `commit-and-tag-version` set up (same tool John uses on
  another project) for Conventional-Commits-driven `CHANGELOG.md` + version
  tagging. Seeded at `0.0.0`. Commit messages I draft follow Conventional
  Commits (`feat:`, `fix:`, `chore:`, `docs:`, …) from here on so the tool
  has something to parse. Verified with `--dry-run` only — the real
  `npm run release` is John's to run, same as all git.

---

## Open questions

- **One sample equirectangular RGB-D** to develop against — pick a source.
- **Real app icon.** Placeholder is the stock `audio-speakers-symbolic`
  symbolic icon, used in both the header-bar menu button and the About
  dialog.
- **Real Preferences / Shortcuts content**, and a non-deprecated pattern for
  the latter — `GtkShortcutsWindow` is deprecated (GTK 4.18+), no official
  replacement yet. See "Hamburger menu + the three dialogs", 2026-09-11.
- **Input contract details.** Depth-map format, units, coordinate frame, gaze
  representation, timestamps. (Radial distance decided; the rest open.)
- **Prep module (B).** One lib, or split geometric cleanup from
  recognition-enhancement.
- **A/B latch timing.** Defaulting to A (latch at click); B deferred.
- **Distance → pitch compression curve.** Linear in delay? Log? Piecewise?
  Pluggable, with a sensible default and a way to swap it per experiment.
- **Return-delay curve** and its relationship to the repetition-pitch τ.
- **HRTF.** Structural model vs. a fitted set vs. recalibration through use.
  Library should accept an HRTF provider interface rather than commit to one.
- **Patch definition.** How big is the "surface around the gaze point"? Fixed
  angular size, or adaptive to the depth structure?
- **Rear field.** Out of scope for now — a coarse rear "radar" layer is a
  separate concern. Keep the API from assuming a full sphere so it can be added
  later without a rewrite.
- **Update-rate split.** Full rate for the gaze patch, lower for the wider cone
  — what rates, and how to cross-fade between them without artefacts.
- **Envelope length, voice-pool size, all tuning numbers.**
- **Pitch:** should the concept note distinguish return-delay from
  repetition-pitch τ? (In `additions.md`.)
- **Record / replay of user actions**, for user-testing logs. John (2026-09-11):
  probably just logging for a replay, not audio/video capture. Design deferred
  — discuss later. Natural hook: the sensor-emulation layer (mouse, keyboard,
  source selection, prefs state) is already the one place all input passes
  through, so a recorder could tap that stream and a replayer feed it back in,
  bypassing the live GTK widgets.

## Decisions

- **2026-09-10** — Public interface is a C ABI; implementation is C++ (subset).
- **2026-09-10** — One backend, selected at compile time; no runtime dispatch.
  Scalar build is the reference oracle + fallback. SIMD via a thin `simd.h` of
  4-wide float primitives designed around NEON `float32x4_t`.
- **2026-09-10** — No dynamic allocation after init (JPL Power-of-Ten rule 3).
  Memory ownership is the hybrid: opaque handle, caller-provided arena,
  `arena_size()` + `init(cfg, arena, size)`; the library never calls `malloc`.
- **2026-09-10** — Library modules: A acoustic scene, B depth prep (optional; A
  degrades gracefully), gaze-event detection (candidate). One built lib,
  separate headers; split later only if needed.
- **2026-09-10** — API split: cheap `set_inputs` (UI thread) + continuous
  `render` (audio callback), latest-input-wins lock-free between them.
  Mandatory per-parameter smoothing.
- **2026-09-10** — Interaction: mouse = gaze direction; click = prod; each prod
  = a voice from a fixed pool; active-ping model with a scheduled non-linear
  return delay; pool states pending → sounding → done; silence between prods.
  A/B latch defaults to A.
- **2026-09-10** — Depth semantics into the library: radial distance along the
  ray.
- **2026-09-10** — Demo is a static single-panorama harness. Head position
  fixed, orientation variable; no translation. Panorama + viewport synthesis
  and all sensor emulation (saccade synthesis, inverse IMU, viewport renderer)
  live in the demo; the library cannot tell it is demo-driven.
- **2026-09-10** — Demo stack: GTK4 (established GNOME path), all-CMake build,
  PipeWire native audio out, `GtkDrawingArea` + `GdkTexture` for display.
- **2026-09-10** — Compute baseline is CPU-only (scalar + NEON), sustained
  budget. Accelerators are drop-in compute backends, not required. Skip NNAPI.
  Core must cross-compile to Android `arm64-v8a` via the NDK.
- **2026-09-10** — Saccade *synthesis* is demo-side; saccade/gaze-event
  *detection* is a separate concern that belongs in the library for the
  prototype. Full saccade model recorded in `~/paper/blind-sight/additions.md`.
- **2026-09-11** — Demo GUI language confirmed: **gtkmm4, C++**.
- **2026-09-11** — Naming: `blind-sight` always hyphenated in single-token
  identifiers (`blindsight` collides with the neuroscience term). See
  "Naming" above.
- **2026-09-11** — libadwaita used via its C API + `Glib::wrap()`, not
  `libadwaitamm` (immature/unofficial) — confined to app/window setup in
  `demo/main.cpp` so a future switch stays local.
- **2026-09-11** — i18n: English source strings, gettext (`_()`), manual
  `po/`→`locale/` pipeline (no CMake automation), same approach as
  `~/spaces/gnome-agent-proxy`.
- **2026-09-11** — Compiler hardening flags adopted from
  `~/spaces/gnome-agent-proxy`, minus `-Werror` (third-party GTK/glib headers
  we don't control make that too brittle here).
- **2026-09-11** — `package.json` + `commit-and-tag-version` for
  Conventional-Commits-driven `CHANGELOG.md` + version tags, matching another
  of John's projects. Commit messages follow Conventional Commits from here
  on.

## Settled

- Target platform: Ubuntu / Linux / GNOME. English throughout the repo.
- Programming: C ABI + C++ implementation; CMake build with `CMakeUserPresets`
  (6 build / 4 test).
- Demo GUI: GTK4 + libadwaita via gtkmm4, C++.

## Not yet chosen

- Audio I/O layer on the library side is a swappable backend; the demo uses
  PipeWire native. Buffer sizes, sample rate: open.
- Depth-frame format and units for the input contract (radial distance is the
  only fixed part).
