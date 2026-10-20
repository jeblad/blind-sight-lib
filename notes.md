# notes.md — blind-sight-lib working note

The evolving design note for this library: what it has to do, the reasoning
behind each choice, questions still open, and decisions as they are made. Kept in
this repo so a session can work on the library without needing the private
concept repo.

Origin: this restates, in original wording for a GPL repo, the render-side design
worked out in the concept note (`github.com/jeblad/blind-sight`, private,
all-rights-reserved). Ideas carry over; text does not. This repo is the public
**demo that Blind Sight works** — not a place to write out the concept's full
rationale.

## Purpose

Input: a stream of depth frames, plus a gaze point per frame. Output: a binaural
audio stream in which the surface the user is looking at is heard — its distance,
its size/structure, its direction, and its motion. Real time, low latency, close
enough to a natural sound field that ordinary spatial hearing does the
interpreting.

## The render pipeline (target shape)

1. **Gaze → region of interest.** Project the gaze point into the depth frame.
   Take the surface patch around it at full temporal rate; the rest of the
   forward cone can update at a lower rate. Reads from two **precomputed**
   per-scene maps (John, 2026-09-26), not values derived on the fly at
   render time:
   - A **reflective-properties map**, computed from the depth map itself —
     surface and edge orientation, feeding the surface-angle and
     edge-density cues below.
   - A **material-softness map**, computed from the photosphere (the RGB
     image, not the depth map) via material recognition — how soft/
     damping a given surface is, feeding the softness cue below.
   Neither is scoped further yet (which recognizer, output resolution/
   format, where in `demo/depth/` this lives) — see TODO.md's ArUco
   entry for the sibling depth-processing work this sits alongside.

   **Whole-sphere precompute is a demo-only shortcut, not the target
   architecture** (John, 2026-09-26): "Det er en effektivisering vi kan
   tillate oss i denne demoen." This demo can precompute both maps for
   the *entire* photosphere up front because it works from a static,
   already-captured image — there's no live camera feed to keep up with.
   A real deployment (live depth-camera + eye-tracking input) would only
   ever precompute these maps for a small region around the current
   gaze/prod point, specifically so it can answer a prod quickly without
   large delay: "vi kun gjøre forhåndsberegningene for en mindre del av
   kula, slik at vi kan svare raskt når en prod kommer, uten store
   forsinkelser." Consistent with the already-established real-time
   constraint elsewhere in this doc/AGENTS.md (full rate only near gaze,
   the rest of the forward cone slower) — this just extends the same
   principle to these two new precomputed maps specifically.

   **Real-system precompute shape, elaborated (2026-09-26): a mip-map,
   keyed by gaze-landing probability rather than geometric distance.**
   Precompute at high precision wherever the gaze is estimated likely to
   land next; resolution then halves successively outward as that
   probability drops — the same halving structure as a texture mip-map,
   but the "distance" driving each level is *predicted gaze probability*,
   not distance from the current fixation point. If a gaze prod actually
   lands somewhere only covered at low resolution, answer immediately
   from whatever precision is already available there (protecting the
   latency budget), then compute that area's higher-resolution version in
   the background so a follow-up response there can use the refined data
   — respond first, refine after, never block the response on refining.

   **A real complication the demo doesn't have to solve: tracking the
   scene sphere's orientation relative to the eyes.** In reality, an
   event pulls the head to turn toward it, which continuously changes how
   the (world-fixed) scene sphere sits relative to the eyes — the mapping
   between "where high-precision precompute is" and "where the eyes are
   currently pointed" keeps shifting and has to be tracked live. John:
   "I demoen har vi bedre kontroll" — the demo drives head/gaze
   orientation directly (recorded input / Surface Dial / mouse, not
   measured physiological head tracking), so this sphere-vs-eyes tracking
   problem doesn't arise there the same way.

   **Live-update precompute is bounded to the reachable gaze field.**
   Outside the region the eyes can actually be directed to from the
   current head orientation, no live-update map is computed at all —
   consistent with "only a forward gaze cone is synthesized" (AGENTS.md).
   The demo doesn't take this shortcut either way: "I demoen beregner vi
   uansett hele kula" — it precomputes the whole sphere regardless, since
   (per the point above) it's working from a static already-captured
   photosphere with no live camera feed to bound against.
2. **Patch → acoustic parameters.** John, 2026-09-26: aim for an acoustic
   model as close as possible to what the ear can actually be assumed to work
   with — deliberately a small number of effects, not an exhaustive physical
   simulation. See "Acoustic model — core effects" below for the
   discussion this section reflects.
   - *Distance* of the patch → repetition pitch, via **two delay stages, not
     one**: a *base delay* (the distance mapping itself, compressed/warped so
     the ear's timing-based pitch perception can actually track it — this is
     the compression curve, see open questions) plus an *intermediate delay*
     layered on top of it that produces the actual comb-filter ("sagtann" /
     sawtooth-shaped spectral) structure — regularly spaced spectral
     peaks/notches read by the ear as a pitch (Huygens–Bilsen repetition
     pitch). The base delay alone sets *where* the pitch sits; the
     intermediate delay is what actually *creates* it. This is the
     foundational effect ("Dette er det grunnleggende").
   - *Distance* also drives increasing damping/lowpass filtering as
     distance grows (John, 2026-09-26) — real air-absorption behavior
     (high frequencies attenuate faster over distance) available "for
     free" as a second, independent distance cue riding on the same
     value already driving pitch. Flagged as "en vesentlig faktor vi kan
     utnytte" — a significant, exploitable factor, not yet wired into any
     concrete curve.
   - *Structure / size* → spectral shape. A larger, flatter surface returns more
     low-frequency energy → darker timbre; smaller or more broken structure →
     brighter. Derive this from the patch geometry (extent, normal spread), not
     from a single "size" scalar.
   - *Material softness* (a separate cue from size), read from the
     material-softness map above → a lowpass filter whose cutoff/damping
     tracks how soft the recognized material is — clothing, for instance.
     Softer → more high-frequency damping → duller timbre. Hard surfaces
     give a much more distinct echo (sharper transient) than soft ones,
     which links this cue directly to overall echo distinctness, not just
     tone color.
   - *Surface angle*, read from the reflective-properties map above →
     whether a reflection returns at all, and how much. A surface facing
     away from the source is not guaranteed to return anything; a
     corrugated surface returns something even facing away. The falloff
     from facing-toward to facing-away is expected to need a **nonlinear**
     attenuation curve (not a plain cosine law) to sound right — exact
     shape is an open, empirical question, likely another pluggable curve
     like the distance→pitch compression curve.
   - *Edge density*, also from the reflective-properties map → diffraction.
     Modeled directly from how many edges are present in the patch, not by
     tracing whether energy diffracted around an edge actually reaches
     something else. Scope is deliberately bounded to **direct return
     only** — no multi-bounce path tracing ("Vi skal ikke 'forfølge
     bølger' for å se om de reflekteres av andre ting").
   - *Direction* of the patch relative to the head → ITD and ILD for azimuth,
     HRTF spectral cues for elevation. A structural (non-individualized) HRTF is
     acceptable because only a forward cone is rendered.
3. **Parameters → additive synthesis.** A parallel filterbank fed from the depth
   patch: each band a partial of the rendered sound, amplitudes and delays set
   by the parameters above. Sum to a binaural pair. The simulated acoustic
   beam aimed at the gaze circle is itself split into a small mosaic of
   sub-rays (each contributing its own direct-return partial); how many rays
   are actually needed before the composite result is interpretable by ear is
   an open, empirical question for the demonstrator (John, 2026-09-26) — not
   a number to guess in advance.
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
- Layout:
  ```
  blind-sight-lib/
    lib/            # the library — planned, not created yet
    demo/           # GTK app: images, viewport, recording, audio
    resource/       # the app's own assets (icons, splash)
    scratch/example # a sample project, committed (see .gitignore)
    CMakeLists.txt          # top-level
    CMakeUserPresets.json   # 6 build / 4 test (John's hardware cap)
  ```
  `lib/` must have no upward dependency on `demo/`. Separate repo for the demo
  only if it grows to need it — not now.

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
  meaningful.
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
trajectory. Summary:

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
  type)` → normalized internal form. In practice: a JPEG image with a
  separate half-float EXR depth file next to it (see Decisions,
  2026-09-23). Stereo later = another loader
  (stereo pair → disparity → depth) behind the same normalized form; for us
  stereo is just a depth source, not a separate track.
  - **Multiple source projections, confirmed as a real need (2026-09-11)**,
    not just equirectangular — John has cylindrical panoramas (partial FOV,
    not full 360°) and plain perspective photos in play too. Each needs its
    own pixel ↔ ray-direction mapping: equirectangular (parameter-free —
    azimuth/elevation linear in width/height, the only one needing no
    metadata), cylindrical (azimuth linear, but elevation via
    `v ∝ f·tan(elevation)` — needs a known focal length/vertical FOV),
    perspective/rectilinear (standard pinhole — needs focal length/FOV,
    maybe principal point). Equirectangular and cylindrical are implemented
    (`demo/panorama.cpp`); perspective is not.
  - **Sample images:** John's own captures — equirectangular photospheres
    (Pixel 5, 8704×4352, GPano metadata, so self-describing) of Fagernes
    Helleplassen, two Leira shop interiors and Valdres folkemuseum, and two
    cylindrical ~180° panoramas of Leirskogen, winter and summer, with no
    projection metadata. Kept locally (`images/`); three photospheres make
    up `scratch/example/`.
  - **Why these particular scenes:** dense shop interiors (the two Leira
    shop photospheres) and large open spaces are both classic hard cases
    in O&M (orientation & mobility) literature for blind travellers —
    dense/cluttered for unpredictable obstacle density, open spaces for
    having no nearby surfaces to echolocate or cane against. Not arbitrary
    tourist shots; both failure modes the concept is meant to address are
    represented in this set.
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

## Demo stack

- **GTK4** — the established GNOME path (not GTK3, not a light toolkit; John has
  bad experience with "light" solutions). Target GNOME 50, expected to keep
  working GNOME 51 / 52.
- **libadwaita**, through its C API (see Decisions, 2026-09-11).
- **Language: C++ with gtkmm-4.0** — official bindings, tracks GTK4, no
  second toolchain next to the C/C++ library. (Python + PyGObject and Rust +
  gtk-rs were considered.)
- **Build: all CMake**, John's presets + 6/4 caps. Demo target pulls
  `gtkmm-4.0`, `libadwaita-1`, `libpipewire-0.3` via `pkg-config`. GNOME
  convention is Meson but nothing here needs it.
- **Image display:** a `Gtk::Picture` fed `GdkTexture`s rendered on the CPU,
  with the gaze cursor as an overlay. `GtkGLArea` remains the upgrade if the
  viewport warp ever needs the GPU.
- **Audio out: PipeWire.** The synthesized sound is a native `libpipewire`
  stream; routing between devices is done by PipeWire itself, driven from
  the app (see "Audio in the demo" below).

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
- **A future companion/support app (2026-09-23, floated)** — distinct from
  the glasses' own embedded software above — John suspects this would
  likely also end up Android, e.g. a paired-phone app for setup/
  configuration/support rather than firmware running on the glasses
  themselves. Not designed at all yet; noted so the distinction (core
  library vs. glasses firmware vs. a possible separate companion app,
  all potentially Android but different pieces) doesn't get lost.

## Process conventions

- Chat with John in Norwegian; all code, docs, comments, commit messages in
  English.
- John does **all** git himself. Draft commit messages, hand them back. No
  AI-authorship line in commits.

---

---

# Demo design — current state (2026-10-20)

The dated development log that used to follow here is archived, verbatim,
in `logs/development-2026-10-20.md` (local, not in the repo). This section
keeps what of it still holds; the code is authoritative for details.

- **Projects.** A project is a directory: `protocol.md`, `images/<id>/` with
  `images.json`, and its recording sessions. A project is always required —
  there is no default project. Projects the app creates are named
  `<slug>-<serial>`.
- **Protocol.** `protocol.md`: each H2 is a scenario tied to an image by its
  slug; each H3 is a test step with a validation point. The Protocol window
  visualizes where the session is; it never drives it (see "Protocol view
  is a visualization" below). Step outcomes are logged as `TestStepEvent`s.
- **Recording.** One `.bs` file per session, JSON Lines; `demo/recording.hpp`
  is the schema. A Dublin Core meta line, then events: gaze, prods, view,
  image, leave, wait, microphone, test steps, microphone sources/levels.
  **No pause control, by design:** stopping and resuming into the same file
  writes a `WaitEvent` with the real downtime, because time away from the
  task still counts (offline consolidation) and shouldn't be hidden as a
  neutral gap. Tracks are the stretches between waits.
- **Recorded audio and transcripts.** Audio per track as 16 kHz mono WAV,
  optionally transcoded to Opus. Transcription with WhisperX, optional
  diarization; the HuggingFace token lives in the GNOME Keyring (read through
  libsecret), never in a file. Models are fetched ahead of a test, never
  mid-test (see "Language models" below).
- **Images and viewport.** Equirectangular photospheres and cylindrical
  panoramas; a pinhole viewport is synthesized from yaw/pitch each frame.
  The head turns but never moves.
- **Depth.** Precomputed per image with Depth Anything V2 and stored next to
  it as `<image>.depth.exr` (single-channel half-float); generated on first
  open if missing, and copied along when an image with one is imported.
- **Input.** The mouse pointer stands in for gaze. A click is an eye-prod;
  the pointer resting for "Pointer Fixation Time" (default 250 ms, tuned for
  a mouse) is an implicit eye-prod, unless it lands within 8 px of the last
  prod. Head direction comes from arrow keys, the scroll wheel or a Surface
  Dial (reopened on reconnect through GUdev); a head-prod is at the
  viewport's centre.

## Open questions

- **Arrow-key viewport occasionally "jumps"/resets when panning fast
  (2026-09-12, Heisenbug)** — John, testing the real example images:
  "virker som viewport av og til hopper ukontrollert?", narrowed down to:
  the Intersport photosphere, panning Left rapidly and then either
  stopping or pressing Right once; "ser ut som viewport resettes" (looks
  like the viewport resets). Investigated and ruled out: the yaw wrap
  arithmetic at ±180° (traced by hand through the boundary cases, C++'s
  sign-of-dividend `%` accounted for), a stitching seam in the source
  image at the wrap boundary (checked by cropping/comparing the actual
  file's left/right edges), every code path that touches
  `recording_session->view_yaw`/`view_pitch` (none reset it), and the
  renderer itself (a pure function of source/yaw/pitch, no state carried
  between frames). Couldn't reproduce it on demand to dig further ("Jeg
  klarer ikke å regenerere") — no input-injection tooling available in
  this environment to drive the real window either, so left as a noted,
  unconfirmed issue rather than chased further for now.
- **Header bar content sits slightly high** (2026-09-12) — John attached a
  screenshot (`resource/Skjermbilde fra 2026-09-12 11-06-15.png`,
  Playback's toast open): the icon/clock/title row has visibly more space
  below it than above within the header bar, i.e. pushed up slightly.
  "Ikke så mye at det egentlig gjør noe... om du ikke kan påvise grunnen så
  bare går vi videre." Checked for anything we set explicitly (padding,
  margin, valign) on the header bar or its content — found nothing; best
  guess is it's libadwaita/Adwaita's own `AdwHeaderBar` CSS (header bars
  are known to have asymmetric top/bottom padding for CSD drag-area
  reasons) rather than an app bug, but unconfirmed without live GTK
  Inspector access. Flagged rather than chased further.
- **A non-deprecated Shortcuts dialog.** `GtkShortcutsWindow` is deprecated
  (GTK 4.18+); libadwaita now has `AdwShortcutsDialog` (present in the
  installed 1.10).
- **AI credit in the About dialog.** John (2026-09-12): he's credited AI
  tools as contributors/translators before and expects to do it here too,
  but explicitly leave the About dialog as-is (John only) for now.
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
- **Pitch:** return-delay versus repetition-pitch τ — are they the same
  delay, or should they be kept apart?

## Design idea: a slower, explicit "identify this" layer on top of the real-time scene (2026-09-22)

John, wanting to get this down before stopping for the night: not a change
to "auditory spatial sense" itself (the core, real-time acoustic scene this
library renders) — it's about the objects the user is sensing through it,
and sits as a layer *on top of* the real-time render, not inside its latency
budget.

**The starting observation.** As the user builds up spatial awareness
through the acoustic scene, some objects' echoes already carry a cue "close
to" something recognizable purely from their acoustic signature (structure
→ timbre already does real work here). John suspects users may end up able
to recognize far more this way than we currently assume — an open,
untested hypothesis, not a claim.

**The specific gap this fills.** In some cases the user can plausibly tell
*that* something is a person, but not *which* person — e.g. knowing Per,
Pål, and Espen are all present in a room without knowing who is standing
where. Proposed mechanic: a **"double-prod"** (a deliberate extension of
the existing single-prod/click mechanic already implemented — see
"Interaction" in Decisions below) aimed at a person triggers an *additional,
separate* step: the glasses run that direction through face recognition and
speak the result ("Espen").

**Why this is a much easier problem than it sounds.** Not matching against
a big external face registry. Instead: an AI listening to the ambient
conversation can associate spoken names with recognized faces/voices
*in context* — e.g. someone says "Espen, can you pass me that" and the
system quietly learns "that voice/face = Espen" for this session, no
external database lookup needed. A session-local, in-context association
problem, not open-set face identification against a large corpus.

**The more common case, though, is simpler still: concrete near-field
object recognition** — chair, table, sink, toilet. Ordinary object
detection, not face recognition, and probably the primary real use of this
mechanic day to day.

**Why this is a separate layer, not a core-library concern.** The
real-time render loop (depth → acoustic scene) has to stay inside the
inter-saccade budget (~200-300ms, see AGENTS.md's design constraints) —
that's non-negotiable and this library's whole job. An explicit,
user-initiated "identify this" request is different: the user has
deliberately paused to ask for more detail, so it can afford to be slow
(a proper vision/face pass, an LLM listening to context, a round-trip to
somewhere with real compute) without threatening the sensorimotor loop.
Implementation-wise this likely belongs in the demo/hardware-integration
layer (or a future "glasses" firmware/companion-app layer) that sits above
this library, consuming the same gaze/depth data but on its own, much
looser timing budget — not something `render()` itself needs to know about.

Not designed further than this yet — captured here so the idea survives
until there's time to work out the actual mechanic (timing/debounce against
two accidental single prods, what "context" the name-association AI
actually has access to, etc.).

**One concrete piece is settled, though**: this is two distinct discrete
events, not one event with a modifier — a `prod` and a separate
`dobbeltprod` (double-prod), each its own event type in the recording, as
`ProdEvent` already is.

## Design idea: a haptic "dunk" on prod, to reinforce the sense of sending (2026-09-23)

John: strengthen the felt sense that a prod actually *sends* something —
in addition to the existing click (the physical mechanical click already
present when using a Surface Dial for prodding) and whatever the resulting
echo sounds like once it returns — by also firing a distinct, felt "dunk"
(a single haptic pulse, not sustained buzzing) in the Dial at the exact
moment of the prod. A tactile "this just went out" cue, complementary to
the auditory return, rather than relying on the mechanical click alone.

**Directly actionable, not just speculative**: a sibling project
(`~/spaces/gnome-surface-dial`, its own `notes.md`) dug into the Dial's
haptic HID feature report the same day and found a real Linux kernel
source (`samples/bpf`, a HID-BPF patch) demonstrating exactly this kind of
control via `HID_REQ_SET_REPORT` — an 8-byte report with a `Repeat Count`
byte and a `Waveform Cutoff Time` byte that look like they support firing
a single discrete pulse rather than only continuous vibration, not just
something gatekept by Windows' own driver. Field-level semantics aren't
confirmed empirically yet (noted as open there too) — but this is a
plausible, concretely-scoped piece of work rather than an open research
question. See that project's notes for the report layout in full before
implementing.

**Scope note**: this is optional-hardware polish (the Surface Dial is
already optional input here, see "Surface Dial input" above), not
something the core render loop or the prod mechanic's actual semantics
depend on — same category as the already-floated, still-unbuilt "Zzzz"
sleep/wake audio cue (TODO.md). Natural to build once there's working
haptic-control code from the `gnome-surface-dial` side to draw on, rather
than duplicating that research here.

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
  PipeWire for audio, CPU-rendered `GdkTexture`s for display.
- **2026-09-10** — Compute baseline is CPU-only (scalar + NEON), sustained
  budget. Accelerators are drop-in compute backends, not required. Skip NNAPI.
  Core must cross-compile to Android `arm64-v8a` via the NDK.
- **2026-09-10** — Saccade *synthesis* is demo-side; saccade/gaze-event
  *detection* is a separate concern that belongs in the library for the
  prototype.
- **2026-09-11** — Demo GUI language confirmed: **gtkmm4, C++**.
- **2026-09-11** — Naming: `blind-sight` always hyphenated in single-token
  identifiers (`blindsight` collides with the neuroscience term). See
  "Naming" above.
- **2026-09-11** — libadwaita used via its C API + `Glib::wrap()`, not
  `libadwaitamm` (immature/unofficial) — confined to app/window setup in
  `demo/main.cpp` so a future switch stays local.
- **2026-09-11** — i18n: English source strings, gettext (`_()`), same
  approach as `~/spaces/gnome-agent-proxy`. Updating the `.po` files from the
  sources stays manual; since 2026-10-20 the build compiles them with
  `msgfmt` into the build tree, and no `.mo` is committed.
- **2026-09-11** — Compiler hardening flags adopted from
  `~/spaces/gnome-agent-proxy`, minus `-Werror` (third-party GTK/glib headers
  we don't control make that too brittle here).
- **2026-09-11** — `package.json` + `commit-and-tag-version` for
  Conventional-Commits-driven `CHANGELOG.md` + version tags, matching another
  of John's projects. Commit messages follow Conventional Commits from here
  on.
- **2026-09-23** — Precomputed depth for a photosphere is stored as a
  **separate file from the RGB image, not packed into it (no custom
  RGB-D format)**. Reasoning: RGB wants lossy JPEG compression at the
  panorama resolutions already in hand (8704×4352); depth needs lossless,
  higher-than-8-bit storage (16-bit PNG, per the loader's own earlier
  description above) — cramming both into one file would force the whole
  thing lossless, losing JPEG's size benefit for the color side. Two
  ordinary files also load with completely standard libraries each,
  unlike a bespoke combined format, and — a further plus John noted —
  makes it straightforward to generate a depth file for an image
  *after the fact* without touching the original image at all. Explicitly
  not spending time designing a new file format for this. Named
  `<image>.depth.exr`, next to the image.

  **Depth file format, same day**: leaning **single-channel half-float
  (FP16) OpenEXR** for the depth file specifically, rather than the
  16-bit-integer PNG floated earlier — floating point gives roughly
  constant *relative* precision across the range (finer near, coarser
  far) rather than a fixed-point format's constant *absolute* precision,
  which matches what depth actually needs (temporal jitter is directly
  audible as pitch flutter, so precision distribution matters, not just
  raw bit count). EXR supports multi-channel files (RGB + a "Z" depth
  channel together is a standard VFX pattern), but deliberately **not**
  using that here — explicitly considered and rejected reusing it to
  combine color and depth into one file, since that would drag EXR's own
  (non-JPEG) compression onto the color side too and undo the size/
  tooling benefits of the separate-files decision above. JPEG stays for
  color, EXR half-float only for depth — "vi holder de adskilt til vi
  eventuelt støter på problemer" (John). An 8-bit mini-float (FP8,
  E4M3/E5M2-class) was also floated as an even more compact option, but
  flagged as needing empirical precision testing before trusting it —
  not decided either way.

## Settled

- Target platform: Ubuntu / Linux / GNOME. English throughout the repo.
- Programming: C ABI + C++ implementation; CMake build with `CMakeUserPresets`
  (6 build / 4 test).
- Demo GUI: GTK4 + libadwaita via gtkmm4, C++.

## Not yet chosen

- Audio I/O layer on the library side is a swappable backend; the demo uses
  PipeWire native. Buffer sizes, sample rate: open.
- Depth-frame format and units for the *library's own* input contract
  (radial distance is the only fixed part) — separate from the demo-side
  storage-file decision above (single-channel half-float EXR, leaning
  choice as of 2026-09-23): the library only ever sees an in-memory
  depth frame handed to it, never a file, so this is about the runtime
  data layout/units the loader normalizes *into*, not the file format.

## Language models, and never downloading mid-test (2026-09-25)

John, after watching the Dismiss/Next/Ready/Still-Not-Downloaded/retry
chain above: "Hm, dette blir et virrvar av dialoger. Tror vi må tenke
nytt. Trodde igrunnen ikke dette var så komplisert." What followed was a
real redesign discussion, not a patch — **nothing below is built yet**;
this section exists so the shape isn't lost before the next discussion
pass John asked for ("Vi skal ta en diskusjon til før vi gjør noe mer").

### How this started: a real safety question, not just messy UI

John: "Den automatiserte installasjonen av biblioteker, den skal jo
forutsette at brukeren er sudo? Hvordan er dette gjort?" Checked
`setup-transcribe-env.sh` directly: no sudo needed anywhere — the venv
and every `pip install` are pure per-user operations under
`~/.cache/...`. The one dependency that could need root (`ffmpeg`) is
never auto-installed, only checked, with a clear "install this
yourself" message if missing. So the auto-install itself was never the
actual risk.

The real risk, once John named it: "Vi kan brute-force hele greia, men
dette er snakk om installering av flere Gigabyte... Kjøre tester hvor
maskinen så plutselig henger fordi noe skal lastes ned er veldig ukult.
Legg til at deler av dette kan ende med at en får nedlastinger pga
persontilpasset kjøring (eg nynorsk/bokmål, eller også svensk, dansk,
engelsk, etc)." WhisperX auto-detects language per recording — a test
subject speaking a language nobody prefetched could trigger a multi-GB
download in the middle of a live session with that person sitting
there. That is the actual thing worth designing around, not the dialog
polish.

### Two facts that reshaped the whole design

- **The fixed transcription model is small and multilingual.** Measured
  directly earlier this session: `Systran/faster-whisper-small`, 463MB.
  Whisper's tiny/base/small/medium sizes are multilingual by design (only
  `.en`-suffixed variants are English-only, and this app doesn't use
  those) — one download covers every language WhisperX can transcribe.
  It is **not** part of the per-testee-language risk at all.
- **Only the word-alignment model is language-specific**, and it's the
  one that varies wildly in size (measured ~3.6GB for Norwegian Bokmål).
  It's also, right now, **not even used by the VTT output** —
  `write_vtt()` reads `segment["start"]`/`segment["end"]`/`segment["text"]`
  straight from `model.transcribe()`'s own result, before
  `whisperx.align()` ever runs (see transcribe.py). Alignment currently
  exists purely as future-proofing for precise word-level sync, not
  something today's feature depends on. That means the whole risk is
  concentrated in one optional step — not spread across the pipeline —
  and that step can be made to fail soft instead of blocking.

### The redesigned shape

**Setup (Preferences), simplified back to one dialog.** John: "Lurer på
om det gir mening i å slå basis-modellen sammen med bibliotekene.
Brukeren har ingen forhold til hva som er hva." The base model becomes
part of the same install step as the venv/libraries (one combined "is
the tool installed" check) — `setup-transcribe-env.sh` (or an
equivalent) becomes responsible for prefetching it too. This removes
dialog 2 (`show_transcription_model_setup_offer()`) and its whole
"Ready"/"Still Not Downloaded"/retry sub-tree entirely — none of that
machinery survives this redesign. Dialog 1
(`show_transcription_setup_offer()`) stays, but simplifies: John, "uten
en dialog 2 så skal det ikke være 'Next' bare en 'ok'" — the "Next →"
label specifically implied progressing to a further step that no longer
exists; same two responses (Dismiss / an install-triggering one), just
relabeled since there's nothing left to "next" into.

**New, independent "Language Models" Preferences section**, replacing
the whole reactive Dismiss/Next model-dialog concept with a proactive
management surface instead. John: "lista over språk er himla lang. Da
blir det en liste over installerte språk og en knapp 'Legg til
språk'." Not gated by the "Transcribe Recordings" toggle at all —
available any time, matching how John already wants this done well
ahead of an actual test ("folk vil sette opp og laste ned på jobben,
før de tester et annet sted"):

- A list of languages already downloaded (human-readable names —
  "Norwegian Bokmål," not "no" or a HuggingFace repo id — mapped
  internally to whatever WhisperX/HF identifiers they actually need).
- An "Add Language" button opening a picker over WhisperX's full
  supported-language list, downloading the chosen one (reuses
  `prefetch_models.py`, already built and live-verified against "no"
  this session).

**Recording stays completely silent, no matter what's missing.**
Transcription triggered automatically by Stop runs the alignment step
in a strict offline mode (`HF_HUB_OFFLINE=1` or equivalent — **not yet
verified this actually makes whisperx/faster-whisper fail closed rather
than attempt a download; needs a real check before relying on it**).
Segment-level transcription (what the VTT already uses) always proceeds
normally regardless, since the base model is guaranteed present by the
setup step above. If the detected language's alignment model isn't
already cached, alignment is just skipped — no error surfaced, no
download attempted, the VTT gets exactly the segment-level output it
already produces today. John: "kan alignment utsettes hvis vi får feil,
slik at den ikke kommer under en recording?" — yes, deferred rather than
attempted or failed loudly. Same silence applies to anything else
language-model-related: "språk-spesifikk tilpassing må også utsettes
under opptak."

**A new sidecar file flags what got deferred** — John: "Vi må vel
flagge at fila trenger realignment," then, once asked sidecar-file vs.
a WebVTT `NOTE` block: "Hva med `<transkript>.todo` for å åpne for at
det ligger flere meldinger der?" — a generic, line-based pending-actions
file (not realignment-specific by name), one line per item, e.g.:

```
realign no
```

Same "plain text, simple line scan" parsing style as `.vtt` itself
(`parse_vtt()`). Per-segment, matching `.vtt`'s own granularity (one
`.todo` living next to each segment's own transcript) — not
independently confirmed with John, but the natural fit given `.vtt`
already works this way.

**Playback is where deferred work actually gets a chance to run.**
John: "Om vi gjør en realinment under playback spiller liten rolle...
Hvis avspilling oppdager at vi trenger realinment så kan det legges opp
en toast som initierer nedlasting via en 'update' knapp." On finding a
`.todo` flagging pending realignment for the currently loaded session,
show an "Update" toast (Playback only — "Vi må stoppe eventuelle
oppdateringer av modeller under opptak, ingen toast da" — same silence
rule as Record, just the inverse: here it's *allowed*, during Record
it never is). Clicking it downloads whatever language model is needed
and re-runs alignment, presumably clearing that segment's `.todo` entry
once done.

**The two "installed languages" surfaces must stay in sync.** A
language downloaded via Playback's own "Update" toast (not through
Preferences' "Add Language" button) still needs to show up in
Preferences' language list afterward: "Innstillinger sin liste av
installerte språk må oppdateres hvis det under playback blir installert
nye språk."

### Explicitly not decided yet

- How C++ checks "is language X's alignment model cached" in general
  (today's `whisper_model_cached()` only ever checked the one fixed
  transcription model by a hardcoded path — this needs a per-language
  version, and WhisperX's own language→HF-repo mapping would need to be
  either duplicated or queried somehow).
- Where the "Add Language" picker's language list itself comes from —
  WhisperX's/Whisper's own supported-language list needs to live
  somewhere (a small static table, most likely).
- Whether `HF_HUB_OFFLINE` (or whichever specific mechanism) actually
  behaves as assumed against whisperx/faster-whisper specifically —
  flagged above, not yet checked.
- What "realignment" actually changes in the *visible* output today,
  given word-level timestamps aren't surfaced in the VTT/subtitle
  display yet either — likely still correct to build as infrastructure
  ahead of that landing, per John's own "det kan brukes" — but worth
  keeping in mind that the near-term visible payoff is small.
- All of today's now-superseded code
  (`show_transcription_model_setup_offer()`, `whisper_model_cached()`,
  the "Ready"/"Still Not Downloaded" dialogs, the four
  `Glib::signal_idle()` deferrals wrapping them) will need to be removed
  or reworked once this is actually implemented — not touched yet, per
  John: "Vi skal ta en diskusjon til før vi gjør noe mer."

## Acoustic model — core effects, design only, not built (2026-09-26)

John: "Jeg vil forsøke å lage en akustisk modell som er så tett på hva
hørselen kan antas å jobbe med som mulig. Det gjør at jeg kommer til å
fokusere på noen få effekter." The full breakdown he gave, restated
above in "The render pipeline (target shape)" (now updated to reflect
all of this) — kept here as the original discussion for provenance:

- **Pitch, two-stage delay.** A synthetic impulse ("påtrykk") on an area
  gets a base delay, itself manipulated so the ear's timing-based
  hearing can actually process the signal — then an *intermediate*
  delay is layered on to create the "sagtann"-filtering (sawtooth/comb
  spectral structure) that is what actually produces the pitch. "Dette
  er det grunnleggende" — the foundational effect everything else sits
  on top of.
- **Softness → lowpass.** Modeled from recognized material softness
  (his example: clothing is soft) — the softer the object is recognized
  to be, the greater the damping applied.
- **Surface angle → reflection presence/amount.** A surface facing away
  isn't guaranteed to return anything; a corrugated surface still
  returns something even facing away. Expects the angle-to-damping
  relationship will need a nonlinear function, not a plain law, to
  sound right — to be tuned empirically.
- **Edge density → diffraction.** Modeled from how many edges are in the
  patch. Explicit non-goal: "Vi skal ikke 'forfølge bølger' for å se om
  de reflekteres av andre ting" — only direct return matters, no
  multi-bounce tracing.
- **Hard vs. soft → echo distinctness.** Hard surfaces give a much more
  distinct echo than soft ones — noted as a general observation tying
  the softness/angle/edge cues together, not a separate mechanism of
  its own.
- **Beam mosaic / sampling density.** The simulated acoustic beam aimed
  at the gaze circle is split into a small mosaic, and how many rays are
  actually needed for the user to get an acoustically interpretable
  result is explicitly an open question to test empirically, not a
  number to pick in advance.

Not yet promoted to AGENTS.md's "Design constraints carried over from
the concept work" — that list is meant to be settled-with-John and this
is fresh; revisit once it's had time to prove out, rather than locking
it in immediately.

## Diarization setup: keyring-based HuggingFace token (2026-09-26)

Design-only so far, not yet implemented — see TODO.md for the concrete
checklist. Worked out before touching code, per the usual back-and-forth.

**Why a HuggingFace token is needed at all.** Diarization (speaker
labeling on top of plain transcription) means invoking `pyannote.audio`'s
diarization pipeline, already an installed transitive dependency of
`whisperx` but never invoked so far (`transcribe.py`'s own comment:
"Diarization is the one part of whisperx that needs a gated HuggingFace
model plus a manual license acceptance"). Confirmed via live web search
(2026-09-26, current as of that date): the pipeline needs a HF access
token **and** the user must separately visit and accept the license on
**two** gated model pages — `pyannote/segmentation-3.0` **and**
`pyannote/speaker-diarization-3.1` — accepting only one is a
commonly-hit, commonly-mis-documented gotcha in other projects (found a
GitHub issue about exactly this: setup docs naming the wrong gated repo).
If the user creates a *fine-grained* token (HF's newer, narrower token
type) rather than a classic "read" token, they must also explicitly
check "Read access to contents of all public gated repos you can
access" under its Repositories permissions, or gated downloads fail with
a confusing 401/403 despite the license already being accepted.

**Where the token lives — two options considered, one rejected mid-
design.** First proposal: let the user run `huggingface-cli login`
(HuggingFace's own tool), which caches the token at
`~/.cache/huggingface/token`. John rejected this directly: "`.cache` er
ikke et sikkert sted å lagre data." Two real problems with it, not just
a style preference:

1. **XDG semantics**: `$XDG_CACHE_HOME` is documented as non-essential,
   safe-to-delete-anytime data. A credential doesn't belong there
   conceptually — a cache-cleanup tool or a "free up disk space" pass
   could delete it without the user realizing they just lost a
   credential, exactly the "token forsvinner" failure John was
   originally worried about.
2. **Plaintext at rest**: file permissions (`0600`) aren't encryption —
   anyone with account or backup access can read it directly, a real
   concern given this machine is explicitly internet-facing (see
   CLAUDE.md's own Security section).

**Settled on: GNOME Keyring via libsecret** — genuinely encrypted at
rest, unlocked with the user's login session, semantically a real
credential store rather than a cache. John confirmed libsecret ships on
every GNOME machine, so no exotic new dependency. Concretely:

- A one-time manual step, matching this project's existing "copyable
  terminal command" setup philosophy (no in-app credential UI at all):
  ```
  secret-tool store --label="blind-sight-lib HuggingFace token" service blind-sight-lib key hf-token
  ```
  prompts for the token value itself (hidden input) and stores it under
  the attributes `service=blind-sight-lib`/`key=hf-token`.
- The app only ever *reads* — `secret_password_lookup_sync()` against
  those same attributes — never writes. New `libsecret-1-dev` build
  dependency (same category of addition as `gudev-1.0` for the Surface
  Dial).
- Retrieved value is passed as an `HF_TOKEN` environment variable only to
  the diarization subprocess's own environment when it's actually
  launched — never written to any file, never logged.

**Two dialogs, both using the "Ok, implicit recheck" pattern below.**

1. A setup "poster", shown the moment "Diarize Recordings" is switched on
   in Preferences and a keyring lookup finds nothing: numbered steps
   (HF account → accept both gated model licenses, linked → create a
   token → run the `secret-tool store` command above). Single "Ok"
   button.
2. A runtime error dialog, shown if the diarization subprocess actually
   fails with an auth error — token expired/invalid, or pyannote having
   revoked/changed access requirements for the model.

**Default: on, not off** — unlike Opus/transcode, which default off.
John: "Eneste grunnen til at dette ikke slås på er at brukeren av en
eller annen grunn må spare plass" — the only reason to disable it is
disk space, so unlike a privacy-sensitive or resource-heavy-by-default
feature, there's no principled reason to make the user opt in.

## The "Ok, implicit recheck" dialog pattern, generalized (2026-09-26)

Floated first for the diarization poster, then explicitly generalized by
John to every dialog of this shape: "Det samme gjelder ved andre slike
dialoger... Jeg tror teksten på knappen er 'Ok', og at sjekken er
implisitt." The underlying quirk: the user can leave one of these
dialogs open while going and doing the requested step somewhere else
(another terminal window, a web browser) — so whatever the dialog is
waiting on must be re-checked at the moment "Ok" is actually clicked,
never trusted as still true from when the dialog first opened. Shape:
**one button, "Ok"**; no separate "Dismiss"/"Check Again"/retry-loop
dialog. Success → proceed silently (the setting itself flipping is
enough feedback). Failure → John, explicit: "avsluttes dialogen og
toggelen settes til off" — just close, and make sure the underlying
setting is false, no extra notice.

Retrofitted the same day onto two existing dialogs that had a similar
but not-quite-this shape:

- `show_transcription_model_setup_offer()` — was "Dismiss"/"Next →" plus
  a nested "Still Not Downloaded" dialog that re-presented the whole
  first dialog again on failure (a real retry loop). Collapsed to a
  single "Ok" that re-checks `whisper_model_cached()` right then.
- The "Opus Encoding Not Available" dialog (`transcode_row`'s
  `notify::active` handler, main.cpp) — was a pure one-shot notice with
  the switch already forced off *before* the dialog even opened, no
  recheck at all. Now checks `opus_encoding_available()` again on "Ok".

**Not** applied to `show_transcription_setup_offer()` ("Transcription
Libraries Not Set Up", dialog 1 of the transcribe chain) — a different
shape: its "Next" actively runs the install itself rather than asking
the user to go do something manually elsewhere, so there's nothing
external to re-check.

**A real hazard found and avoided while retrofitting the Opus dialog**:
an early draft captured the Preferences row (`AdwSwitchRow*`) in the
dialog's response context, to flip the switch back on directly after a
successful recheck. But this alert is presented against the main window,
not nested inside the Preferences dialog itself — so the user can close
Preferences (destroying the row) while the alert is still sitting open
waiting on a manual `apt install`, leaving a dangling pointer.
`install_transcription_libraries_and_continue()` already had a comment
explaining exactly this class of risk for its own (longer, async)
install path, deliberately avoided by never capturing the row across it.
Fixed by applying the same discipline here: only `settings` (a `RefPtr`,
not tied to any widget's lifetime) crosses into the dialog's response —
the switch just shows the correct state again next time Preferences is
reopened, same as the install path already did.

**Superseded the same day — "avoid touching it" was too blunt an
instrument.** John caught this live, using the real diarization-model
dialog: he ran the copyable command, clicked "Ok", and the switch stayed
off — even though `gsettings get ... transcribe-audio` showed `true` and
a full app restart showed the switch correctly on. "Det er sjekken ved
retur til Innstillinger som ikke oppdaterer tilstanden korrekt." The
setting was always right; only the *already-open* Preferences instance's
own switch never got told about it, because nothing ever touched the row
after the setup dialog resolved. The dangling-pointer risk above was
real, but the fix didn't have to be "never touch it" — `GWeakRef` gives
both properties at once: safely becomes `NULL` if the row's destroyed
(no dangling reference, same protection as before), but still lets the
switch update live in the ordinary case where Preferences is still open.
Applied to all three dialogs of this shape
(`show_transcription_model_setup_offer()`, "Opus Encoding Not
Available", `show_diarization_setup_offer()`) — each now takes the row
as a parameter (nullable, since one of `show_transcription_model_setup_
offer()`'s two call sites, the async post-install chain, genuinely has
no row to give) and holds it via `GWeakRef` rather than either a raw
pointer or avoiding it entirely.

Hit the file's own previously-documented `G_CALLBACK` pitfall again
while doing this: a brace-init's top-level comma (`new
OpusRecheckContext{ctx->settings, {}}`), sitting inside another
`G_CALLBACK(...)`'s lambda body, reads as a second argument to that
*outer* macro — the preprocessor only tracks `()` nesting when splitting
macro arguments, not `{}`. Caught immediately by the compiler
("G_CALLBACK passed 2 arguments"), fixed the same way the existing
precedent already did: field-by-field construction instead of a
brace-init list.

## Panorama/photosphere rendering — open question: cylindrical stretch near the edges (2026-09-30)

Photospheres with real GPano XMP metadata (`test-360.pano.jpg`, a real
Pixel 5 capture) render correctly via the equirectangular-crop path and
feel natural. The cylindrical fallback (used for wide images with no
panorama metadata at all — the two `Leirskogen*.panorama.jpeg` files,
which have no usable EXIF/XMP, Photini apparently having stripped it)
feels different: John, testing live against those files: "Vi har nå et
litt unaturlig strekk i bildet når noe nærmer seg kanten... Fotoboblene
virker mer naturlige." His own tentative read on the cause, worth
revisiting before trying anything else: the viewing distance implied by
the render may not match the opening angle (FOV) the source image
actually represents, and — more interestingly — a real wide-angle lens
applies a geometric compression toward the edge of the frame that this
projection doesn't model at all. The cylindrical mapping here is a
simple tan()-based single-focal-length projection (see
`resolve_panorama_projection()`'s own comment in main.cpp); it has no
lens-distortion term, so content that a real wide-angle capture would
compress toward the edges instead spreads out at the same rate as the
center does. That asymmetry between "no real lens data, so no distortion
model either" and "a real wide shot always has one" may be exactly what
reads as unnatural stretch.

Tried and reverted same day: scaling the fallback's assumed horizontal
FOV by the image's own aspect ratio (instead of always assuming the
image spans a full 360°), holding a fixed assumed vertical FOV
(`kAssumedCylindricalVfovDegrees`) constant — physically motivated (a
stitched cylindrical panorama only extends horizontal FOV by panning
further; vertical FOV stays at the camera's native single-shot value).
Made the stretch-near-edges problem more visible, not less ("Kan du
bygge på nytt? Jeg har kjørt rewind på koden, for det ble helt feil") —
reverted back to the plain `cyl_focal_px = src_w / (2π)` assumption
rather than chase it further.

Parked here, not acted on further for now ("Tror vi lar det bli med det
foreløpig, og går videre med noe annet"): revisit only with John, and
likely only after real per-image FOV/lens data becomes available some
other way — guessing harder at the assumed FOV doesn't seem to be the
fix. Photospheres (GPano-tagged) remain the primary/preferred capture
format either way ("jeg tror vi som oftest vil bruke fotobobler, men
panorama er en fallback") — cylindrical is explicitly the degraded
fallback path, not the one to invest more guessing into right now.

## Projects (2026-10-03)

A project is a directory holding one `protocol.md` plus its related
recording sessions — John: "jeg tror vi egentlig har et prosjekt, og at
dette prosjektet har en protokoll-fil. Jeg tror at prosjektet er en
katalog." Replaces the earlier "New/Open/Save Protocol" trio (bare,
unanchored `.md` files, no directory concept) with a directory-first
model, surfaced as a new leftmost header-bar dropdown (`project_button`,
packed once, persists across Interactive/Record/Playback — unlike
Open/the image-sequence dropdown, a project isn't an Interactive-only
concept).

- **Naming.** New Project names are turned into a directory via a
  Jekyll-style slug (`slugify()`: lowercase, æ→ae, ø→o, å→a, everything
  else collapsed to hyphens) plus a global, monotonically-incrementing
  counter from gsettings (`project-slug-counter`, zero-padded to
  `project-slug-counter-digits`), retried on an on-disk collision
  (`create_project_directory()`). John: "tror vi legger til et tall fra
  gsettings for å unngå kollisjoner... vi gir ingen garanti om at to
  apper sine tall skal være gjensidig konsistente." Confirmed live
  (2026-10-03): a project named "Echo echo" created `echo-echo-000001`
  correctly.
- **No explicit Save.** Every image-sequence change (Edit Sequence's
  add/remove/reorder) eager-writes the active project's `protocol.md`
  (`reconcile_and_write_protocol()`, same match-by-path/preserve-body
  reconciliation the old Save Protocol handler used). John: "jeg tror vi
  oppretter protokoll-fila i riktig katalog med en gang, og så legger
  til bilder i denne fila så fort de er tilgjengelig."
- **Recents + auto-reopen.** `recent-projects` is the same MRU shape as
  `recent-sessions` (most-recent-first, capped at `max-recent-projects`,
  pruned when a project's `protocol.md` no longer exists). `last-project`
  reopens automatically on launch, same convention as
  `current-working-session`.
- **Dropdown button face.** Shows the active protocol's own H1,
  truncated with an ellipsis at `project-title-cutoff-chars` characters
  (`truncate_project_title()`, UTF-8-character-aware via `Glib::ustring`).
  That cutoff length, the counter digit count, and `max-recent-projects`
  are all gsettings-only — no Preferences row for any of them, same "edit
  the key directly if you ever need to" convention `max-recent-sessions`
  already set.
- **Deliberately out of scope this round**: "Merge Protocol" (local-wins
  conflict resolution against an externally-edited `protocol.md`) was
  discussed at length but never concretely specified as a trigger/UI, so
  it wasn't built. A project also doesn't yet own *where* a recording
  session gets saved — that's still whatever Record's own save dialog
  was pointed at, unrelated to the project directory. Both are real
  follow-ups (see TODO.md), not forgotten.

**Known limitation, not ours to fix: the folder picker shows files, not
just folders.** New/Open Project's parent-folder picker
(`Gtk::FileDialog::select_folder()`) displays ordinary files alongside
folders, with no visible dimming to mark them non-selectable — confirmed
live (2026-10-03) via a real screenshot of our own "Open Project" dialog
standing in a folder containing both a project directory and a plain
`ping.txt`, both rendered at the same visual weight. Traced to the
underlying library, not our code: a throwaway probe
(`Gtk::FileDialog::select_folder()` + `dbus-monitor`) confirmed the call
goes out as `org.freedesktop.portal.FileChooser.OpenFile` with
`directory: true` in the options dict — `GtkFileDialog` (GTK 4.10+,
replacing the deprecated `GtkFileChooserDialog`/
`GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER`) exposes no "hide files" option
at all; which method you call (`select_folder()` vs `open()`/`save()`)
is the only mode selector it has, and everything past that — including
whether non-folder entries are shown or dimmed — is up to the
portal/`GtkFileChooserWidget` backend (`xdg-desktop-portal-gnome` 50.0,
GTK 4.22.4 on this machine). This is also a long-standing, frequently
reported GNOME complaint with no fix forthcoming, not a regression this
project introduced. **Noting this here so a future "folder picker shows
files" report isn't re-investigated as if it were new** — nothing in
blind-sight-lib's own code can change it.

## Project-scoped sessions (2026-10-05)

A long discussion, triggered by noticing the session dropdown's own
recents list — not just the session collection editor — shows
contributor names by default. **None of this is implemented yet.**
Captured here because the discussion spanned many exchanges and the
reasoning behind each call is easy to lose. See TODO.md for the one
genuinely open loose end this surfaced (image vs. session deletion
semantics).

### The motivating analogy

John recalled a GNOME Secrets redesign as the cautionary tale: Secrets
used to keep its secret entries on a *secondary* pane, only shown when
actually editing one — ordinary quick-access use never touched it. A
later, "fancier" redesign put the primary (quick-access) and secondary
(secret-holding) panes side by side in the same window permanently,
exposing data that used to require a deliberate extra step to reach.

Applied here: the session collection editor (with its eye/visibility
toggle, built 2026-10-04) is the right place for contributor names to
live — reaching them takes a deliberate step. But the session
dropdown's own recents list — the "primary pane," always one click
away — showed that same data by default (`session_button_label()`,
"[creator] – [contributor]"). Structurally the same mistake, regardless
of the file-picker leak (`--session`/"Legg til sesjon" browsing a real
filesystem, which also exposes sibling filenames) being a separate,
shallower concern.

### What's actually wrong, underneath

Two things compound:

1. **Careless filenames.** A session file could end up with a
   participant's real name in its *filename*, not just inside the
   file's own meta fields — browsing to pick one in a file-chooser
   exposes every sibling filename in that folder too, independent of
   any in-app hiding mechanism.
2. **Consent withdrawal.** The real driver, stated explicitly partway
   through: a test participant can withdraw consent, and the app must
   then be able to *reliably and completely* delete their data — not
   just unlist it. This is what elevates "delete a session" from a
   convenience feature to something that has to actually work.

### The settled design

- **Sessions always live inside the project's own `sessions/`
  directory.** Not optional, not "elsewhere on the machine" — that
  flexibility (seriously considered mid-discussion: "en skal kunne
  navigere til sesjoner andre steder på maskina") was explicitly
  dropped in favor of a much simpler mental model: the session
  collection dialog *is* a listing of what's physically in that
  directory, nothing more. No "Legg til sesjon" either — there's
  nothing external to add.
- **Each session gets its own subdirectory, named by a hash+counter
  scheme** — explicitly *not* a slug of the title the way project
  directories are named, because a title-derived slug could leak a
  name the same way a careless filename could. "Session-hash-counter,"
  hash replacing slug.
- **A project-local `sessions.json` at the project root** — not
  gsettings. The reasoning carries over directly from protocol.md's own
  role: gsettings is this-machine-only and doesn't travel if the
  project directory is copied, moved, or shared; a file inside the
  project directory does. This is also where the per-session
  visibility flag now lives (superseding the gsettings `hidden-sessions`
  key built 2026-10-04 for the same purpose — that one was always meant
  to be provisional).
- **A new append-only event type logs every file a session creates**
  (audio segments, Opus encodes, transcripts) — just the path, nothing
  more (John: "tror det holder med filstien"). Deletion reads this list
  back as the authoritative set of what to remove, instead of
  reconstructing expected filenames from the naming convention the way
  `track_audio_path()` does today. No backward compatibility needed for
  existing `.bs` files without these events — "vi har ingen eksisterende
  sesjoner som vi trenger å ta vare på (fortsatt i utvikling)." Existing
  sessions get deleted outright when this lands, not migrated.
- **Deletion is a single, atomic, confirmed action.** One confirmation
  dialog, shown before anything happens; confirming removes the row
  from `sessions.json` *and* deletes the directory from disk together,
  as one step — not the two-step "remove from list, then separately ask
  about the files" flow floated mid-discussion and then dropped once
  the stricter "sessions always live in their own directory" model made
  the list-vs-file distinction meaningless. Cancelling leaves both
  untouched. This only works safely *because* a session is always a
  standalone, self-contained unit nothing else references — see the
  TODO.md entry on why the same hard-deletion model doesn't
  straightforwardly apply to the image collection, which is a shared
  resource across sessions.

### Final rule: project required, no exceptions

The app always starts with no project selected. **Everything in the
header bar is insensitive except the project dropdown, the hamburger
menu, and window buttons, until the user explicitly opens or creates
one** — a command-line `--project` argument satisfies this the same way
it already does today.

Image-sequence curation was briefly considered as an exception (it's
gsettings-backed today, not project-file-backed, so technically
*could* work without a project) — "fikling med bilder" to prepare a
candidate set before committing to a project felt like a real,
separate use case. John reconsidered and rejected the exception: curating
a set of images for a protocol *is* the beginning of building a
project, not a separate activity that happens to precede one — "jeg
tror vi sier at fikling med bilder må skje i et prosjekt." No
exceptions beyond the three named above.

**Autoload defaults flipped to off, same day, same reasoning.**
`autoload-project`, `autoload-session`, and `autoload-image` (all built
2026-10-03/04, all previously defaulting to on to preserve old
always-autoload behavior) now default to *off* — the toggles themselves
stay, for a deliberate opt-in use case John named explicitly (reopening
the last project/session when *analyzing* recordings afterward, not
during live testing), but a fresh install or Preferences reset no
longer exposes a previous participant's project by accident. Since
autoload-session/autoload-image already require a project to be active
first (2026-10-04's own `g_project_active` gate), turning off just
autoload-project would have been enough on its own — the other two were
flipped too anyway, for defense in depth rather than relying on that
indirection alone.

**Implementation, split into four sequential parts (2026-10-05).** Built
one at a time, each confirmed with John before starting the next:

- **Part A (done):** `demo/sessions.hpp`/`sessions.cpp` — the
  `SessionsIndex`/`SessionEntry` data model, `read_sessions_index()`/
  `write_sessions_index()`, `generate_session_id()`, and
  `session_file_path()`. Verified with a throwaway round-trip harness,
  not wired into the UI yet at this point.
- **Part B (done):** wired into the UI. "New Session" no longer shows a
  file dialog — it requires an active project, generates an id, creates
  `<project>/sessions/<id>/` only once the meta dialog is confirmed (a
  cancelled dialog leaves nothing behind), and appends the entry to
  `sessions.json`. "Open Session" and "Legg til sesjon" are both gone —
  a session is only ever created the one way now.
  `show_session_collection_editor()` reads/writes `sessions.json`
  directly (Move Up/Down swap `order`, the eye toggle flips `hidden`);
  its delete button is deliberately not wired yet (Part C's job). The
  old gsettings keys this replaced (`recent-sessions`,
  `max-recent-sessions`, `hidden-sessions`, `last-session-folder`) are
  removed from the schema entirely.
- **Part C (done):** the collection editor's trash icon is back, now
  wired to a single `AdwAlertDialog` confirmation (Cancel/Delete).
  Confirming removes the `sessions.json` entry and
  `remove_all()`s the session's own directory together, in the same
  response handler; cancelling or closing touches neither.
  Deleting the session currently loaded into Record/Playback (John,
  same day: "Hvis brukeren forsøker å slette en sesjon som er i bruk
  for record eller playback så skal mode skifte til interactive") now
  drops back to Interactive automatically — the editor's `on_changed`
  checks whether `recording_session->session_path` still exists on
  disk after any change, and if not (and the app isn't already in
  Interactive), calls a new `return_to_interactive`, holding exactly
  stop_button's own Stop logic.
- **Part D (done):** `update_header_bar_sensitivity()` ties
  `session_recent_button`, `sequence_button`, `nav_group`, `clock_group`,
  and `transport_group` to `g_project_active` — five containers (so
  `set_sensitive()` cascades to every child), called at construction
  and at the end of both `open_project_at()`/`reset_to_no_project()`.
  `project_button`/`menu_button` stay the only two exceptions; no
  exception for image curation either, per "fikling med bilder må skje
  i et prosjekt" above. This closes out all four parts of the
  project-scoped-sessions redesign — end-to-end hand-testing is still
  outstanding.

## Project-scoped images (2026-10-06)

A long discussion, triggered by the same self-containment goal the
sessions redesign served — images referenced from wherever the user
happened to pick them from don't travel with the project the way
project-scoped sessions already do. **Only Part A (the data model) is
implemented so far.**

**The deeper problem this actually started from.** Partway through,
John surfaced something the images-folder question alone doesn't
explain: there's an implicit conflict between "an image used in a test
step" and "an image shown in the browsable image list," and it bites
specifically when you want to change which image a given test step
uses. Root cause: the image list *is* the protocol today —
`reconcile_and_write_protocol()` rewrites protocol.md's own entries
from image-sequence on every single add/remove/reorder, so the two
concepts are locked together 1:1 by path. There's no way to say "this
step's image changed" without it being indistinguishable from "the
curated list changed."

**The settled design, decoupling the two:**

- **`images.json`** (new, per-project, same shape as `sessions.json`)
  is the authoritative, browsable image list from now on — replacing
  the gsettings `image-sequence` key entirely (John, 2026-10-06: "Vi
  skal gjøre prosjektet self-contained... Rekkefølgen til images lista
  skal i images.json, slik at en annen bruker kan overta hele
  prosjektet. Eneste som jeg tror kommer fra gsettings er
  project-lista. Denne er brukerspesifikk" — same "gsettings is
  this-machine-only" reasoning sessions.json was built on). Each entry:
  `id` (opaque hash, directory name under `<project>/images/`), `slug`
  (human-facing, see below), `order`, `hidden` (same eye-toggle concept
  the session editor already has).
- **Images live inside the project**, one directory per image —
  `<project>/images/<id>/<original filename>` (John: "nede i
  underfolderen så kan vi beholde filnavnene slik de er" — only the
  directory is the opaque id, the file itself keeps its name). Same
  reasoning as sessions: per-image auxiliary files (today: the depth
  map, `depth_file_path_for()`, already relative to the image's own
  path so this needed zero code changes to keep working; more such
  files are expected later) get a natural, shared home for free.
- **"Legg til bilde(r)" becomes "Importer bilde(r)"** (Part B) — it now
  *copies* the picked file into its own new `<project>/images/<id>/`
  directory rather than just referencing wherever it was picked from.
- **Slugs.** Every image gets a short, human-facing identifier,
  auto-generated from the original filename the same way project
  directories get theirs from the title (Jekyll-style transliteration,
  `slugify()` — moved from main.cpp into images.hpp so both share it),
  de-duplicated against images.json's own current entries with a plain
  "-2"/"-3" suffix. Shown read-only in Image Details, so whoever is
  hand-authoring protocol.md has somewhere to find it. The reason for a
  slug at all, rather than referencing by path or id directly: "Grunnen
  til at vi bruker slugs er at vi skal tilfredstille renderen som
  brukes for Markdown. En ref med spaces kan skape problemer" — same
  "spaces in machine-facing identifiers cause portability problems"
  lesson as the session-ID locale bug just above, applied proactively
  here instead of discovered the hard way.
- **protocol.md becomes an externally-authored, attached document** —
  the app stops auto-rewriting it on every image-list edit. "Redigering
  av protocol.md gjøres i separat verktøy." A brand-new project still
  gets a minimal protocol.md at creation (just a place for the
  project's own title) — extra project-level data, if ever needed,
  goes in YAML frontmatter at the top, not more auto-generated body
  content.
- **Images are referenced from protocol.md as `[tekst](slug)`** — a
  completely ordinary inline Markdown link, just with the slug in place
  of a path or URL (John confirmed this renders fine, at least in
  VSCode's own preview — a renderer that tries to resolve it as a
  relative file path would show a broken-looking link cosmetically, but
  nothing would actually break, since this app's own parser is the only
  thing that needs to resolve it for real). This is the *same* inline-
  link format `reconcile_and_write_protocol()` already writes today —
  only what goes inside the parens changes, from a path to a slug, so
  no reference-style footnote-link parsing is needed.
- **"Reconcile protocol" button**, new, in the project dropdown — runs
  on demand (not automatically) what's essentially today's
  `reconcile_and_write_protocol()` logic, just matched by slug instead
  of path: walks images.json's *visible* entries in order; an existing
  section whose link already resolves to that slug keeps its content
  but moves to the right position; an image with no matching section
  gets a fresh one created in place; any section whose slug no longer
  matches a visible image gets pushed to the bottom of the file rather
  than deleted. "reconcile_and_write_protocol() er vel omtrent det
  'Regenerate protocol'-knappen vil gjøre. Kanskje vi bør kalle den
  'Reconcile protocol'" — renamed accordingly. The automatic call from
  the image collection editor's own `on_changed` goes away entirely.
- **A new, separate "Protocol" window** — a button next to Mic/Speaker
  in the Record bar opens a standalone top-level window (not a panel or
  popover) rendering whichever protocol section matches the image
  currently on screen, updating live as the image changes. Deliberately
  a real separate window, not embedded: "muligens vil brukeren dra det
  til en separat skjerm" — the facilitator is expected to want this on
  a second monitor, away from whatever the test participant sees on the
  main window.
- **Image deletion** (Part C) gets the same real-delete-with-
  confirmation treatment sessions already have, now that images live in
  their own self-contained directory too — but explicitly flagged as a
  decision still to make at that point, not inherited silently: the
  *old* non-destructive "Remove from Collection" behavior existed
  specifically because one image can be referenced by multiple
  sessions' own `ImageEvent.path`, and hard-deleting could break
  another session's playback. That tension doesn't automatically
  resolve itself just because the storage model now matches sessions'.
- **Example images get repackaged as a proper example *project***
  (John: "Vi pakker om eksempelbildene og lager et eksempel-prosjekt")
  — deliberately deferred, not part of this sequence. The current
  gsettings-based auto-seed-on-first-launch (`get_image_sequence(settings).empty()`
  → six `examples/` paths) doesn't make sense once the image list is
  project-scoped, and just gets dropped in Part B; a fresh project
  starts with an empty images.json until the example-project rework
  lands separately.

**Implementation, split into six sequential parts, same discipline as
the sessions redesign — built and confirmed one at a time:**

- **Part A (done, 2026-10-06):** `demo/images.hpp`/`images.cpp` — the
  data model and file I/O (`ImageEntry`/`ImagesIndex`,
  `read_images_index()`/`write_images_index()`,
  `generate_image_id()`), plus `slugify()` (moved here from main.cpp)
  and `generate_image_slug()`. Verified via a throwaway round-trip
  harness. Not wired into any UI yet.
- **Part B (done, 2026-10-06):** every `get_image_sequence`/
  `set_image_sequence` call site now goes through
  `sorted_image_entries()`/`visible_image_paths()` instead (same shape
  as the session equivalents) — the gsettings `image-sequence` key is
  gone from the schema entirely. `open_project_at()`'s own reseed-from-
  protocol step and `reset_to_no_project()`'s own clear are gone, not
  migrated — images.json is already the project's own persistent state,
  nothing left to reconstruct or wipe on project switch. The collection
  editor takes `project_dir` now, has the hidden/eye toggle, and its
  "Add Image(s)…" button is "Import Image(s)…" — copies each picked
  file into its own new `<project>/images/<id>/` directory (original
  filename kept, per-batch slug de-duplication done in-memory against
  the batch being built, not via repeated disk reads) rather than just
  referencing wherever it was picked from. Image Details shows the new
  read-only "Slug" field. The gsettings-based example-image auto-seed
  at startup is gone, along with `example_path()` (now genuinely
  unused, caught by the build's own `-Wunused-function`) — a brand-new
  project just starts with an empty images.json, pending the
  separately-deferred example-project rework.
- **Part C (done, 2026-10-06):** settled on reference counting, two
  separate mechanisms for two separate kinds of reference (John: "Bruk
  referansetelling... Fordi vi ikke selv har kontrol på protocol-fila,
  så må denne sjekkes før en sletting blir gjort"). Sessions (fully
  app-controlled): `ImageEntry::ref_count`, incremented the first time a
  session's own event stream gets a new `ImageEvent` for an image
  (checked against that session's own file, not in-memory state, so
  reopening it later never double-counts — "Én telling per sesjon"),
  decremented when that session is deleted. While nonzero, the
  collection editor's own trash button is always built (same row
  position) but invisible/inert — John hand-tested Part C immediately
  and caught the row-shifting regression the first cut had ("når
  søppelbøtta blir borte så må du sette inn en placeholder. Mennesker
  har en tendens til å generalisere, og forvente at ting står på samme
  plass"), same fixed-position lesson as Previous/Next's own width fix
  earlier the same day. protocol.md (externally authored, not
  app-tracked): checked fresh at the moment of deletion — a plain
  `](slug)` text search — and folded into the confirmation dialog's own
  dynamic body as a warning, not a hard block. Flagged while building
  this: if per-track (not whole-session) deletion is ever added, the
  increment/decrement pairing here stops being correct.
- **Part D (done, 2026-10-06):** `ProtocolEntry::image_path` renamed to
  `image_ref` — protocol.cpp stays a plain markdown reader/writer with
  no notion of images.json, just extracting whatever's in a link's
  parens verbatim. All slug↔path resolution lives in main.cpp: new
  `path_for_image_slug()` mirrors the existing `slug_for_image_path()`;
  `reconcile_and_write_protocol()` now matches existing entries by slug
  (keyed on `image_ref`) instead of path, synthesizing a fresh entry
  (`[filename](slug)`) for any image not yet referenced;
  `open_project_at()`'s "is the current image already in the protocol"
  check resolves each entry's slug back to a path before comparing.
  Builds clean; not yet hand-tested.
- **Part E (done, 2026-10-06):** new "Reconcile Protocol" button in the
  project dropdown, calling `reconcile_and_write_protocol()` directly
  (with a "No Project" guard and an `AdwToast` confirmation). The
  automatic calls this replaces — `edit_sequence_button`'s and
  `new_project_button`'s own `show_image_sequence_editor()` on_changed
  callbacks — are gone; protocol.md is now reconciled strictly on
  demand, never as a side effect of editing the collection.
- **Part F (done, 2026-10-06):** the separate "Protocol" window — a
  button next to Mic in Record's own action bar opens a real, standalone
  `Gtk::Window` singleton (never destroyed, just hidden/re-presented;
  not registered with the `Gtk::Application`) showing the protocol
  section matching whatever image is currently on screen. Content is
  kept live by a per-frame tick callback on `splash_picture` (same
  pattern as the equirect-render and "No image" margin-sync ticks right
  next to it) that resolves the current image to a slug and matches it
  against `active_protocol->entries`, mirroring
  `update_sequence_checkmarks()`'s own Record/Playback ternary so it
  tracks whichever mode is actually showing an image.
- **UX fix right after Part E (John, 2026-10-06):** "Avstem protokoll-
  knappen hører ikke egentlig hjemme i project-dropdown... Den passer
  bedre i hamburgermenyen for odde ting." Moved from the project
  dropdown into `menu_button`'s own `other_section` (Preferences/
  Keyboard Shortcuts/About) as `app.reconcile-protocol`, same
  registration style as those three — the project dropdown is New/Open
  plus the recent-projects list, not a place for an action that just
  runs in place.
- **Bug fixed (2026-10-06):** `reconcile_and_write_protocol()` was
  silently dropping any section whose slug no longer matched a visible
  image, where the settled design says it should be pushed to the
  bottom of the file instead. Rewritten to consume existing entries by
  *index* (a `std::vector<bool> consumed`, one `take_matching()` scan
  per image) rather than building a `slug -> entry` map — the old map
  approach had a second, latent problem noticed while fixing this: two
  entries sharing a slug (or both having no link at all, a legitimate
  case per John, 2026-09-25: "en seksjon uten lenke kunne bruke
  test-image") would collide in the map and silently lose one. Whatever
  entries are left unconsumed after walking the visible image list get
  appended to the bottom, in their original order, instead of being
  discarded.

**Bug found by John right after Part E (2026-10-06):** a freshly
synthesized protocol entry's section title and link text both used the
bare filename — John: "seksjonstittelen er det samme som title på
entries i image lista, og teksten på lenka er det samme som
stikktittelen." Fixed to read IPTC Title/Caption via
`read_image_metadata()` (same fields and fallback the image list's own
rows already use) instead. Confirmed the existing-entry side of
reconciliation needed no change — matching by slug only ever decides
whether to keep an entry (verbatim, including any hand-edited text) or
add a fresh one, never rewrites an already-present entry's content.

**Bug found by John right after Part D (2026-10-06):** creating a new
project left the main window black until images were imported — "Nå
blir vinduet 'svart' når det ikke er definert noen bilder for et nytt
prosjekt." `new_project_button`'s handler had never gone through
`open_project_at()` or any equivalent reset, so `splash_picture`/
`no_image_picture` were left showing whatever the previously active
project had on screen, with no CSS class and no placeholder card put
back, and `g_project_active` never got set either. Fixed by forcing the
same "No image" reset `edit_sequence_button` and `open_project_at()`
already use, unconditionally — a brand-new project can never have a
current image to preserve.

## Test steps and validation (2026-10-06)

A long design conversation, same collaboration rhythm as the project-
scoped-images redesign: John floated the idea, we went back and forth
on specifics, settled a lettered plan (A–F), then started building.

**The concept.** A scenario (H2, one per image) is a "test task,"
divided into "test steps" (H3). Each step has a validation point — the
facilitator judges whether the participant reached the expected
result, and presses Validated or Failed. John: "Seksjonen som vises i
vinduet er en 'test task' eller et 'scenario'... Hver slikt step har en
validation point med et expected result. Hvis expected result ikke
nås så er test step ikke validert."

**UI shape: AdwNavigationView/AdwNavigationPage**, one page per test
step, pushed forward as steps complete — John's own suggestion, and a
good structural fit (built for exactly this "sequence of pages, push
forward" shape). Each page shows the step's own task text plus however
many H4 sub-blocks it has, collapsed by default and revealed on
demand — "alle H4 listes, men med skjult tekst. En 'expected result'
vil bare være en av mange mulige slike." The parser never special-
cases "Expected result" as a fixed label; H4 is a fully generic named
block. Ahead of the first step: an initial page showing the scenario's
own intro text (whatever sits before the first H3 — the image link,
any overview prose), with just a plain "Next" button rather than
Validated/Failed — it's an overview, not a test step of its own (John,
caught right after Part A's first draft silently dropped this text:
"Tekst før første H3 skal brukes i en presentasjon av scenariet. Den
havner på en initiell page som har en next-page-knapp"). Same idea one
level up: text before the first H2 (right after the protocol's own H1
title) gets its own opening page introducing the whole protocol,
before the first scenario's pages — "Tilsvarende vil initiell tekst før
første H2 introdusere protokollen." Either intro page is skipped
entirely when there's no such text, rather than showing a blank page
("Finnes det ingen tekst før første H2 eller H3 så hoppes disse sidene
over").

**Back navigation is allowed, but flagged hard.** First instinct was to
forbid it entirely ("tilbakenavigering vil gi feil resultat"), but John
settled on allowing it with heavy warnings instead: logged as its own
outcome value in the same event (Part B), *and* a toast in the Protocol
window itself the moment Back is pressed ("Back navigation may
invalidate results" or similar) — belt and suspenders, the log for
later analysis, the toast for in-the-moment awareness.

**What gets logged, and when.** Validated/Failed/Back become a new
event type (Part B), written only in Record — "I interactive så lages
det ikke events, det er kun for at brukeren skal kunne verifisere at
ting henger sammen." Interactive gets the exact same step-by-step
navigation and auto-advance behavior, just silently (a dry-run/preview
mode for whoever is setting up the protocol). Playback never drives
navigation at all — it's read-only, reconstructing "which step was
current" from the already-logged events at the current playback
position (Part F), same "latest matching event ≤ now" idiom
image/view resolution already uses elsewhere in this file.

**Auto-advance drives the main window's own image, following
protocol.md's own order, not images.json's.** John caught this
distinction explicitly: "protocol-fila har sin egen rekkefølge av
bilder, og den følger ikke billedlista hvis ikke det er kjørt en
reconcile." Completing a scenario's last step (Validated/Failed, not
Back) loads the *next* H2's own image — `active_protocol->entries`
order, resolved through `path_for_image_slug()`, not
`sorted_image_entries()`/`visible_image_paths()`. Finishing the last
scenario shows a standard completion message and resets the main
window to "No image" (Part D).

**Next/Previous sensitivity, Record only.** "Jeg mistenker at
next/previous skal være insensitive under opptak om protocol-vinduet
vises... Grunnen er at hvis ikke protocol-vinduet vises så er det
next/previous som driver visningen. Hvis protocol-vinduet vises så er
det dette som driver visningen fremover" (Part E) — scoped to Record
specifically; Interactive/Playback are untouched since nothing there
risks corrupting a logged result.

**The six-part plan, as agreed:**

- **Part A (done, 2026-10-06):** protocol.hpp/protocol.cpp — new
  `TestStepBlock`/`TestStep`, and `parse_test_steps(body)` splitting a
  scenario's own already-extracted `body` text into its H3 steps and
  each step's own H4 blocks. Reuses `kHeadingPattern`/`trim()` from the
  file's existing anonymous namespace, one level down from how H1/H2
  are already handled (H3 = step boundary, H4 = block boundary within a
  step). Deliberately a *derived view* only — `ProtocolEntry::body`
  stays the single canonical, verbatim text `write_protocol_file()`
  reads/writes; nothing from `TestStep`/`TestStepBlock` is ever written
  back independently of it. A stray H4 before any H3 opens is treated
  as plain text, not a crash or a silent drop of the whole step. Not
  wired into anything yet — `parse_test_steps()` has no caller until
  Part C.
- **Corrected same day:** the first draft silently dropped any text
  before the first H3. John caught it: "Tekst før første H3 skal
  brukes i en presentasjon av scenariet. Den havner på en initiell
  page som har en next-page-knapp" — that leading text (the image
  link, any overview prose) gets its own initial page in the Protocol
  window's paged presentation, with just a "Next" button, not a
  Validated/Failed test step. New `ScenarioPages{intro, steps}` +
  `parse_scenario_pages(body)` capture both in one parse pass;
  `parse_test_steps()` is now a thin wrapper around it for callers that
  only need `.steps`.
- **Extended again, same day — one level up:** "Tilsvarende vil
  initiell tekst før første H2 introdusere protokollen. Finnes det
  ingen tekst før første H2 eller H3 så hoppes disse sidene over." Same
  gap, same fix, at the protocol level — new `Protocol::intro`
  (text between the H1 and the first H2), captured by
  `parse_protocol_file()`, round-tripped by `write_protocol_file()`.
  New shared `join_lines()` (anonymous namespace) replaces what would
  otherwise have been three copies of the same "join lines, trim" loop
  across `ProtocolEntry`/`Protocol`/`ScenarioPages`. Skipping the page
  entirely when the text is empty is the UI's job (Part C), not the
  parser's — it just reports `""` either way.
- **Part B (done, 2026-10-06):** new `TestStepEvent{type, t_ms,
  scenario, step}` — three subtypes via `type` ("validated"/"failed"/
  "back"), same convention as `ProdEvent`'s own head-prod/eye-prod
  split, not a separate outcome field. `scenario` matches
  `ImageEvent::scenario` (H2 heading text); `step` is the H3 heading
  text. Added to `RecordEvent`, dispatched in `read_record_events()`.
  `kRecordFormatVersion` bumped 4 → 5 — matches this file's own
  precedent of bumping on any event-schema addition, but means any .bs
  file recorded with an earlier build today is now unreadable, same as
  every previous bump.
- **Part C (done, 2026-10-06):** Protocol window rebuilt around
  AdwNavigationView/AdwNavigationPage. `load_protocol_scenario()`
  builds one scenario's lead-in page(s) (protocol intro only on the
  first scenario, then the scenario's own, each skipped when empty)
  then the first test step; later steps are built and pushed lazily
  from the previous step's own Validated/Failed handler
  (`push_step`/`build_step`, self-referential `shared_ptr<function<>>`s
  like `g_start_new_project`). H4 sub-blocks render as collapsed-by-
  default `Gtk::Expander` rows. Every page gets
  `can_pop(FALSE)` — Back is the *only* way back, so its own logging/
  toast can't be bypassed by the view's built-in gesture/Escape pop.
  Validated/Failed/Back write a new `TestStepEvent` only while actually
  recording (`*current_mode == 1 && recording_session->file`), same
  guard every other record-only event already uses; Back additionally
  shows a toast via a new `g_protocol_toast_overlay` wrapping the whole
  window. The last step's Validated/Failed calls a new
  `g_protocol_scenario_complete` hook — a no-op until Part D assigns it.

  **Real bug found and fixed while building, not reported by John:**
  `g_protocol_window_shown_for_slug` was a plain `std::string`
  defaulting to `""`, indistinguishable from a legitimately resolved
  "no matching scenario" slug — meant opening the window for the very
  first time with no image on screen silently left the placeholder
  page blank, since the very first refresh's own "unless changed" check
  saw `"" == ""`. Changed to `std::optional<std::string>`.

  Known rough edge, not addressed: a lead-in page built but never
  clicked through before the image changes again leaks that one page
  (never added to any container, so its floating ref is never dropped)
  — narrow, low-stakes, left alone. All visual layout is unverified —
  this part especially needs real hand-testing.
- **Part D (done, 2026-10-06):** `g_protocol_scenario_complete` now
  takes the completed scenario's slug, assigned in `on_activate()`.
  Walks `active_protocol->entries` in protocol order (not
  images.json's), resolves the next entry's image via
  `path_for_image_slug()`, and `load_image()`s it. Record additionally
  gets `go_to_nav_index()`'s own event side effects (track-change/clock/
  ImageEvent/ViewEvent/ref-count) duplicated inline, since that
  function's own `nav_index` lookup is into `visible_image_paths()`,
  the wrong sequence for protocol order. No image on the next entry →
  "No image," not an error. Last scenario finishing → completion dialog
  + "No image."
- **Part E (done, 2026-10-06):** `update_header_bar_sensitivity()`
  (already `nav_group`'s one sensitivity owner) ANDs the existing
  `g_project_active` gate with `!(current_mode == 1 &&` Protocol
  window visible`)` — Record only. A new per-frame tick just calls it
  unconditionally every frame, since there's no plain signal for
  another window's visibility changing.
- **Part F (done, 2026-10-06):** Playback's own read-only reconstruction of
  "current step" from the logged events at the current position. See
  its own section further down for the implementation.

**Follow-up (2026-10-06):** "Hvis protocol-vinduet er åpent, og brukeren
klikker på document-knappen, så skal protocol leses på nytt. I dette
tilfellet så fungerer knappen som en 'reload'." `protocol_button`'s own
handler now checks visibility first — if already open, re-parses
`protocol.md` and replaces `*active_protocol` wholesale, then clears
`g_protocol_window_shown_for_slug` so the refresh actually rebuilds
even when the resolved slug hasn't changed. First-ever open is
unaffected.

**Bug found by John right after Part B's version bump (2026-10-06):**
"Hvis vi forsøker å lese en fil med feil format, dvs feil versjon, så
må det postes en toast... og ikke bare feile stille." Auditing every
`apply_shared_session()` call site found the real gap: the
`--session` command-line path already shows its own error dialog, but
clicking an existing session in the dropdown and the last-session-at-
startup autoload both just silently did nothing on failure. Both now
post a toast. `session_list`'s own row-activated handler had to move
to after `toast_overlay` exists — same "construct early, wire late"
split already used for several other permanent widgets in this file.

### Part F: Playback's own read-only reconstruction (2026-10-06)

The last of the six-part plan. Playback never drives the walkthrough
and never writes a `TestStepEvent` - it only ever reconstructs "which
step was current" by reading back what Record already logged, at
whatever position playback is currently at. Same idiom
`restore_viewport()` already uses for Image/ViewEvent: scan backward
from the current event index for the latest matching event.

`build_protocol_step_page()`'s body/H4-block rendering was factored
out into `build_protocol_step_content()` so a new sibling,
`build_protocol_step_page_readonly(step, outcome_type)`, can reuse it
without the Back/Failed/Validated row - just a plain label reporting
the outcome that was already decided during Record ("Validated"/
"Failed"/"Back navigation may invalidate results", all reusing
strings that already existed for the interactive buttons/toast).

Two new local lambdas, defined in `on_activate()` right next to
`restore_viewport()` so they can share its idiom and its neighbors'
capture lists: `show_test_step_event(event)` takes one `TestStepEvent`
directly and resolves it to a page - matching `event->scenario`/
`event->step` text back against `active_protocol->entries` and
`parse_scenario_pages()`, falling back to a message page if nothing's
been recorded yet at all, or if the matched entry/step no longer
exists in protocol.md (it's hand-edited, so this has to be handled,
not assumed impossible). `restore_test_step_display(index)` is the
backward-scan wrapper around it, for `seek_to()` and scrubbing, where
no single event is "the one just reached" the way the forward tick
loop always has one in hand.

Wired into three places: `seek_to()` (right after `restore_viewport()`
itself), a new `TestStepEvent` branch in the forward-dispatch tick
loop (calls `show_test_step_event()` directly - it already has the
event, no need to re-scan for it), and the tick loop's `moved_back`
block (rewinding live during negative-speed playback needs the same
re-resolution `restore_marker()`/`restore_viewport()` already get
there).

`protocol_button`'s single- and double-click handlers both gained an
early `current_mode == 2` branch that just calls
`restore_test_step_display()` and presents the window - no mirror-
vs-force-restart distinction the way Record/Interactive has, since in
Playback the position alone already fully determines what there is to
show; clicking either way just means "show me where we are."

## Open question: the Protocol window's own focus problem (2026-10-06)

Right after finishing Part F, John raised a real problem with the
whole two-window design (main image window + the separate Protocol
window): "Vi har nå to vinduer. Testee gjor et forsok i bilderuta, og
sa far det et utfall. Det klikkes sa med musepekeren i protokollruta
av tester. Da settes focus til protokollruta... Da fungerer pekeren,
fordi vi jukser litt, men ikke Surface Dial, for vinduet har ikke
fokus." Only one of the app's two top-level windows can hold keyboard
focus at a time, and the Surface Dial apparently needs the image
window to actually hold it (plain mouse motion still gets through some
other way - "vi jukser litt" - but the Dial doesn't).

Two candidate fixes were discussed:

1. **Programmatic focus-steal-back.** Confirmed via
   `/usr/include/gtk-4.0/gdk/gdktoplevel.h` that `gdk_toplevel_focus()`
   exists and that `Gtk::Window::present()` already wraps it. On
   Wayland this only works when called synchronously from within a
   real input-event handler (e.g. right inside a button's own
   `clicked` handler) - GDK ties the activation request to that
   event's own serial via `xdg_activation_v1`, which is how Wayland
   compositors (Mutter included) distinguish a legitimate same-app
   focus handoff from arbitrary focus-stealing. Calling it from a
   timer or other async context would not work. Not yet implemented -
   superseded by option 2 below.
2. **Remove the second window entirely (chosen).** John's own
   counter-proposal: "Jeg tror det er viktigere a fa dette til a
   fungere pa enkle maskiner, slik at alminnelige brukere kan teste.
   Far de ikke testet selv sa klarer vi ikke a bygge nodvendig tillit."
   Fixes the problem at the root rather than patching it, and doesn't
   depend on a second monitor either. His first sketch was reusing
   `AdwBanner` with two buttons hidden inside its one button slot -
   checked `/usr/include/libadwaita-1/adw-banner.h` and that's not
   possible (`adw_banner_set_button_label()` is a single slot,
   `G_DECLARE_FINAL_TYPE` rules out subclassing) - but the underlying
   idea survives with a plain custom widget instead of real AdwBanner:
   a small revealer-based strip docked in the main window itself,
   showing the step text plus Failed/Validated (Record) or a read-only
   outcome (Playback), so keyboard/pointer focus never leaves the one
   window in the first place.

**Confirmed so far:**
- The separate Protocol window is going away entirely (not kept
  alongside the strip) - John: "Vi fjerner det separate vinduet. Det
  er enklere med en skjerm."
- H4 blocks ("expected result" etc.) must still be shown somehow -
  John: "jeg tror vi ma kunne vise H4, for det er en hjelp til de som
  skal bedomme hva som kan valideres" - likely a small "show more"
  affordance within the strip, not a full page.
- The Back button is being dropped entirely, once Next/Previous
  (`nav_group`) work normally again - John: "Back-knappen er ikke
  nodvendig og kan fjernes nar vi har din losning hvor Next/Previous
  fungerer." This also means Part E's sensitivity coupling
  (`nav_group` insensitive during Record while the Protocol window is
  visible) becomes moot and should be removed along with the window
  itself - there's no second window "driving" the display to protect
  Next/Previous from any more.
- What's reusable as-is: the data model (protocol.hpp/cpp's
  `TestStepBlock`/`TestStep`/`ScenarioPages`/`parse_scenario_pages()`)
  and `TestStepEvent` (recording.hpp/cpp) are both UI-independent and
  need no changes - only the main.cpp rendering/driving code (Parts
  C-F) gets replaced.

**Still open, John wants to think more before deciding:**
- Strip placement in the main window - top (under the header bar,
  above the image) or bottom (above the action bar)?
- Whether `protocol_button` keeps toggling the strip's visibility
  (revealer) manually, or the strip just shows/hides itself
  automatically whenever there's a relevant step to show.

Nothing has been implemented yet for this redesign - Parts C-F (the
separate-window code) are still in place and working exactly as
documented above, pending this decision.

## Protocol view is a visualization, not a driver (2026-10-11)

Surfaced while building the tracks feature's "stikktittel" (its short
identifying name, derived from the protocol's active H2 task):
`ImageEvent::scenario` - the field that identifying name is ultimately
read from - was only ever written by `g_protocol_load_scenario_image()`,
reached exclusively through a double-click on the protocol button
(`start_protocol_walkthrough()`). An ordinary Record session, created
and advanced the normal way John actually works (pick a project, create
a session, record, advance tracks via Next/Previous), never touches
that code path at all, so no track in such a session ever got a
scenario tie recorded - regardless of whether the project's protocol.md
had a matching H2 for the image being shown.

John's diagnosis, once the empty `"scenario":""` fields in a real
session's event log confirmed this wasn't a staleness bug: "I wonder if
the problem emerged because the 'protocol view' becomes the driver for
the recording. It is more like a visualization of where we are." The
double-click walkthrough treats the protocol as a second, competing
navigation mechanism you must drive through to get scenario data
recorded at all - but the protocol window's real job is just to reflect
whichever image is already showing, however the user got there.

The underlying distinction, stated precisely: Next/Previous navigates
the *image list* (`images.json`'s own sequence); the protocol window's
walkthrough navigates the *protocol file* (`protocol.md`'s own
entries) - two potentially different orderings, not the same list
twice. "Reconcile Protocol" is what makes them coincide; before that,
only the images the two lists actually agree on (by slug) overlap at
all.

Fixed by matching on *identity*, not *position*: a new
`scenario_title_for_image()` (main.cpp) takes whatever image is
currently showing, resolves it to a slug (`slug_for_image_path()`), and
looks for a protocol entry whose `image_ref` matches that slug -
entirely order-independent, so it's correct whether or not the two
lists have been reconciled. Wired into every place Record actually
writes an `ImageEvent` (the Forward-press handler's first-image write,
`go_to_nav_index()`'s nav-triggered write), not just the walkthrough's
own - so a matching image gets its scenario tag regardless of which
control was used to reach it. The double-click walkthrough itself is
untouched, and - per John - is leftover code from an early design pass
that was never revisited; not relied on by the actual workflow.

Likely to resurface: John expects this image-list-vs-protocol-file
distinction to come up again later, hence this note.

## Audio in the demo — current design (2026-10-20)

- **Synthesized sound.** `NoisePulseEngine`: one native PipeWire stream,
  stereo FL/FR (the same signal in both until spatial rendering exists) —
  noise through a one-pole lowpass ("color") and an ADSR envelope, with an
  echo delayed by the distance at the prod. It never autoconnects; the
  routing decides where it goes.
- **Monitoring matrix: who hears whom.** Rows are listeners (the Ambient
  Speaker and each participant), columns are sources (the Ambient
  Microphone, the synthesized sound, each participant). Defaults: nobody
  hears themselves; the Ambient Microphone is opt-in; the Ambient Speaker
  hears only the synthesized sound, since it shares the room with the
  microphones. Double-clicking a header mutes its whole row or column.
  A participant can use the Ambient devices instead of their own, and one
  set to the Ambient device itself counts as doing so.
- **Routing** (`demo/audio_routing.cpp`). Each path is keyed by the actual
  (sink node, source node), so participants sharing a device share one
  path. Voices go through one `pw-loopback` process per path (10 ms, never
  falling back to another device); the synthesized sound is linked directly
  with `pw-link`, adding no buffering, for the testee's latency. Everything
  runs out of process: GStreamer's pipewire elements share one thread-loop
  per process, and stopping one while another ran deadlocked the GTK thread
  (confirmed by gdb). A `pw-link -m` watcher resyncs when devices come and
  go and relinks a lost synthesized link.
- **Voice levelling.** Every microphone of its own, and the Ambient
  Microphone, runs through a PipeWire filter chain (`pipewire -c`, one
  process each) hosting `demo/voice_agc.cpp` as a LADSPA plugin: a slow AGC
  in dB that adapts only while the speaker talks (tracked against the
  microphone's own noise floor, with loud outliers such as laughs clamped),
  then the voice volume, mute, and a limiter at −1 dBFS. Target −23 dBFS,
  in line with EBU R 128's −23 LUFS. The settings are shared (Preferences);
  levelling is switched on or off per participant. Mute cuts the output and
  freezes the gain while the level estimates keep running, so the voice
  returns at a settled level (John's call).
- **Volumes**, all in dB. *Synthesized*: the listener's own device volume,
  since the direct links carry no gain. *Participants*: the voice loopbacks
  into that device, set relative to Synthesized (at most +20 dB) so the two
  act independently. *Voice*: in the leveller, after the AGC. Anyone on an
  Ambient device gets that device's settings.
- **Level log.** Each leveller reports its microphone's level before any
  gain every 50 ms; recordings get `sources` and `level` events, `null`
  while a microphone is muted or absent. For telling which microphone a
  voice dominates, next to diarization — level alone can't tell, e.g. when
  someone leans over another participant's microphone.
- **Bluetooth.** A classic Bluetooth headset's microphone only works over
  HFP: mono, 16 kHz at best, for the wearer's playback too — which also
  flattens a stereo scene. The device picker says so and defaults to not
  using such a microphone. LE Audio (LC3, both directions) works with the
  built-in controller once bluetoothd's `Experimental` is on.
- **Open:** recording the levelled voices instead of the system's default
  microphone; latency of a headset's virtual sink versus its device node;
  prods to mono sinks (see TODO.md).

## Eye gaze input: candidate hardware, nothing built yet (2026-10-14)

Today the gaze point comes from the mouse pointer standing in for an eye
tracker. Two candidates for real gaze input came up. Neither is being
built: John, 2026-10-14 — "Vi skriver ikke noe for en slik enhet. Først
må vi få det vi har til å fungere." This note only records the options.

**Webcam + face landmarks.** A Logitech MX Brio (60 fps, which John
judges sufficient) with OpenCV for capture and Google MediaPipe for face
and iris landmarks, gaze estimated from those. MediaPipe ships
ready-made C++ and Python bindings in its newer packages (John also
listed `mediapipe-silicon`, which as far as I know is the Apple Silicon
build and not relevant on Linux). Expected accuracy is a few degrees,
probably good enough here. Ordinary hardware, fully on Linux, no vendor
software in the loop.

**Gazepoint GP3 HD.** A screen-mounted tracker, around NOK 4–5 000
used/refurbished. 150 Hz, stated accuracy 0.5–1°. Set up with
Gazepoint's software on Windows, but can be run under Linux. Exposes the
open OpenGaze API: XML messages over TCP on port 4242, including
fixations directly (FPOG, with duration and validity) — which may make
our own fixation detection unnecessary for it. OpenGaze has no
authentication, so the server must only be reachable from this machine
(`localhost`), never from outside: the machine is internet-facing.

**Either way, fixation timing changes.** "Pointer Fixation Time (ms)"
(`pointer-fixation-ms`, default 250) is tuned for the mouse; a real
tracker's fixations are much shorter. See TODO.md's entry on a second
fixation setting: it may be better as a detector on the gaze signal
itself (velocity threshold, I-VT, or dispersion window, I-DT), or
taken from the tracker's own fixations where it reports them, as
OpenGaze does.

The library itself is unaffected: it consumes a gaze point and never
does eye tracking (see AGENTS.md, "Input"). Only the demo's input layer
would change.

An earlier candidate, the Tobii Eye Tracker 5 (2026-09-23), has no official
Linux support — only community reverse-engineering (`tobiifree`).
