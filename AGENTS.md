# AGENTS.md

Cold-start brief for `blind-sight-lib`. Global preferences live in
`~/.claude/CLAUDE.md`. This file is the project context a fresh session needs.

## What this is

A library that turns a **depth map + a gaze point** into a **binaural acoustic
scene**, in real time. It is the "acoustic scene" component of the Blind Sight
concept: a gaze-steered synthetic echo as sensory substitution for blind and
severely visually impaired users.

- **Public repo**, `github.com/jeblad/blind-sight-lib`, **GPL-3.0-only**.
- **`notes.md` in this repo is the evolving design note** — pipeline shape,
  reasoning, open questions, decisions as they are made. Read it alongside this
  file; keep it current as the library takes shape.
- The concept note this library serves lives in a **separate, private,
  all-rights-reserved repo**: `github.com/jeblad/blind-sight` (Norwegian Bokmål).
  Its `notat.md` is the origin of the render-side design (sections
  "Renderarkitektur — parallell filterbank fra dybdekartet", "Rendermotor —
  detaljert gjennomgang", "Kanalbudsjett for sonifiseringen",
  "Render-omfang — additiv, og hvor mye"). `notes.md` here restates what the
  library needs in original English wording — do not copy that prose across, the
  two repos have different licences.
- The library is deliberately **hardware-independent**. It runs on recorded
  video with eye tracking, or on synthetic depth data, and plays through
  ordinary headphones. Building it first, before any prototype hardware, is the
  concept note's explicit recommendation (and has precedent: Bizoń-Angov et al.
  2021 tried a comparable sound idea in simulation before building a device).

## Design constraints carried over from the concept work

These are settled at the concept level. Revisit them only with John.

- **Input.** A per-frame depth map, plus a gaze point in the depth camera's
  frame (from eye tracking). The library does not do eye tracking or depth
  estimation — it consumes their output.
- **Output.** Binaural audio. The whole render loop (gaze → depth lookup →
  binaural render) must finish well inside the inter-saccade interval
  (~200–300 ms, ideally far faster), or the sensorimotor loop breaks.
- **Only a forward gaze cone is synthesized.** Nothing behind the user. A coarse
  rear "radar" layer is a separate concern and out of scope for now. This is why
  a structural / non-individualized HRTF is acceptable: within a forward cone,
  front-back ambiguity barely applies.
- **Encoding.**
  - Distance → repetition pitch (timing-based pitch, Huygens–Bilsen; see Moore,
    *Introduction to the Psychology of Hearing*, ch. 5.5.D). The delay sets the
    tone.
  - Structure size → spectral brightness. Larger surfaces reflect more low
    frequency (darker timbre); smaller structures give a brighter timbre. It is
    the structure that produces the timbre, not "size" as an abstract parameter.
  - Direction → the ordinary spatial-hearing cues: ITD and ILD for the
    horizontal plane, HRTF spectral cues for elevation.
  - Carrier: a short broadband signal.
- **Architecture.** Additive synthesis, a parallel filterbank driven from the
  depth map. Render only the surface around the gaze point at full rate; the
  wider frame can update slower.
- **Stay close to a natural sound field.** Stretch the mapping only as far as
  spatial hearing keeps following it. *How far it can be stretched before the
  spatial reading breaks* is a research question for the demonstrator, not
  something to hard-code a guess for — expose it as a parameter.
- **Motion.** Decompose scene motion into a tangential component (rendered as
  angular displacement of the surface) and a radial component (rendered as
  audible Doppler on the surface).

## What is NOT decided

- The distance → pitch compression curve. Research question; make it pluggable.
- HRTF individualization strategy (structural model vs. fitted vs. recalibrated
  through use).
- Output transducer, at the hardware end: bone conduction is ruled out,
  open-ear air conduction is preferred — but that is downstream of this library.

## First steps (suggested)

1. Define the input contract: depth-map format, units, coordinate frame, gaze
   representation, timestamps.
2. A minimal offline pipeline: synthetic depth frame + gaze → one binaural
   buffer. Get the distance→pitch and direction→ITD/ILD path audible first.
3. Then structure/timbre, then motion.
4. Keep a synthetic-data generator in the repo so the library is testable with
   no recordings.

## Working notes

- Code, comments, README, and API docs in **English** (public OSS library). The
  concept note is Norwegian; the library is not.
- **Target platform: Ubuntu / Linux / GNOME.** Develop and test there; no need to
  carry other platforms unless John asks.
- Targeted tests over clear execution paths. No heavyweight test framework
  unless the suite genuinely outgrows a hand-written one.
- Programming language and runtime not yet chosen — confirm with John before
  scaffolding.
- Never run `git add` / `git commit` for John. Draft the message, hand it back.
  No AI-authorship line in commits.
