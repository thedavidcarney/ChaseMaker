# CLAUDE.md

Briefing for Claude Code (or any AI assistant) when working on this repo.
Human-facing context lives in `README.md`.

## What this is

**Chase Maker** — AE AEGP panel plugin. The third tool in David Carney's
three-tool light-pass workflow. This tool builds *virtual light chases*
from per-light EXR passes: pick which lights light at which time,
sequence/animate the chase, drive the AE project to assemble it.

**Not a compositor.** Chase Maker doesn't do color/finishing work; it
builds chases from already-rendered light passes.

**Chase Maker was originally floated as "Lightpass Studio"** — renamed
because "Studio" implied composition/finishing tooling, which it isn't.

## How this fits with the other two tools

The suite is **three separate repos**, NOT a monorepo. The handoffs
between tools are file-format / API contracts, not shared code:

1. **Blender lightpass tool** (Python addon, separate repo) — groups
   lights into named lightpass collections in Blender, configures the
   EXR export so each light/group becomes a named layer in a
   multilayer/multipart EXR. Hand-off out: the EXR file.
2. **EXRDemux** (AE PF effect plugin, matchName `tdcarney EXRDemux`,
   separate repo) — name-based EXR layer selection per AE layer. Has a
   JSX (`SplitAndSortPassesToPrecomps.jsx`) that splits an EXR's layers
   into precomps + a Master Comp. Hand-off in: EXR file.
3. **Chase Maker** (this repo) — AEGP panel plugin. Reads EXRs, computes
   derived data, drives EXRDemux precomp setup via the AEGP API and/or
   the JSON sidecar contract below, builds the chase animations.
   Hand-off in: EXR file + the precomp structure EXRDemux's JSX builds.

Different platforms, different SDKs, different license obligations
(Blender's API drags GPL into the Blender tool; the AE plugins can be
permissively licensed). **Don't propose collapsing the three into a
monorepo.** If shared code ever emerges that's worth factoring out, the
right move is a fourth tiny utility repo, not a monorepo.

## Architecture: AEGP panel plugin

Chase Maker is an **AEGP panel plugin**, same shape as Superluminal's
`Stardust_panel.aex` and Video Copilot's Element 3D editor:

- Single `.aex` file, registered as an AEGP plugin (NOT a PF effect)
- Adds a custom panel under AE's `Window > Chase Maker`
- Panel is dockable in AE's UI alongside Project/Timeline, OR
  undockable into a floating window — feels like a separate editor
- Lives inside AE's process; no IPC, no second binary
- Full AEGP API access: read project, apply effects, create comps,
  listen to events

**What we considered and rejected** during the May 2026 EXRDemux design
conversation that led to this repo:

- **External standalone app.** Cross-process IPC complexity, install
  dance, doesn't integrate cleanly with AE.
- **CLI tool bundled with EXRDemux's .aex.** Wrong shape — EXRDemux
  stays a single-binary AE plugin (the conventional pattern); this
  tooling lives in its own repo as an AEGP panel.
- **Adding orchestration buttons to EXRDemux's effect.** Wrong product;
  EXRDemux is a render-only PF effect, this is the orchestration layer.

Don't re-litigate those without strong new information.

## UI framework — leaning Dear ImGui

For a solo dev shipping a focused tool, **Dear ImGui** is the default:

- MIT license (no LGPL dynamic-link relink dance)
- Lightweight, immediate-mode, no widget framework to fight
- Excellent for tool UIs with live data (thumbnail grids, centroid
  overlays, real-time previews)
- What most game-engine editors and DCC in-app tools use

**Qt** is what Element 3D uses but they had a team. Heavier, LGPL
licensing matters (LGPL dynamic-link is fine for distribution but
commercial Qt avoids the relink-mechanism requirement). Decide once
we know v0.1 feature scope; don't lock in early.

## Communication contracts with EXRDemux

Two paths, both valid, can coexist:

1. **AEGP API at runtime.** Chase Maker reads project state, calls AEGP
   suites to apply `tdcarney EXRDemux` effects, build precomps, set
   layer hashes, etc. In-process, immediate.
2. **JSON sidecar.** Chase Maker writes a sidecar next to the EXR;
   EXRDemux's JSX picks it up. Lets external/batch tools participate in
   the same contract.

### JSON sidecar contract (stable across the boundary)

Sidecar path: `<exr_path>.luminosity.json` next to the EXR.

```json
{
  "version": 1,
  "source": "<filename.exr>",
  "source_mtime_utc": <unix_seconds>,
  "image_size": [w, h],
  "layers": {
    "<display_name>": {"cx": 0..1, "cy": 0..1, "total": <luminance>}
  }
}
```

Display names use the `X.X -> X` dedup convention (matches EXRDemux's
layer picker — e.g. raw EXR layer key `World.World` becomes display
name `World`).

### Layer-name hashing (must match EXRDemux byte-for-byte)

EXRDemux selects layers by FNV-1a 32-bit hash of the display name,
split into two 16-bit halves stored in `Layer Hash Hi` / `Layer Hash Lo`
float-slider params. Reference implementations:

- C++: `HashLayerName()` in EXRDemux's `src/exrdemux.cpp`
- ExtendScript: `fnv1a32()` in EXRDemux's
  `scripts/SplitAndSortPassesToPrecomps.jsx`
- Python: `scripts/dev/luminance_centroid.py` (algorithm, not the hash —
  but the display-name extraction logic mirrors the picker)

If Chase Maker computes hashes itself (likely, when applying Demux to
layers via AEGP), match the C++ reference exactly. A mismatch means
saved selections silently point at wrong layers.

## Centroid algorithm (validated, ready to port)

Used for spatial sorting modes (left-to-right, top-to-bottom). Validated
against the Wall_Curtains 52-light reference scene; sort order matched
the actual scene layout by eye.

- For each RGB layer (skip cryptos, Image, Alpha): luminance =
  `0.2126*R + 0.7152*G + 0.0722*B` (Rec.709)
- Clamp negative pixels to 0 (filter ringing in float EXRs)
- `cx = Σ(L*x) / ΣL`, `cy = Σ(L*y) / ΣL`, normalized by `(w-1)`, `(h-1)`
- Skip layers where `ΣL <= 0` (all-black this frame)

Reference Python implementation:
`EXRDemux/scripts/dev/luminance_centroid.py`. No need for log-luminance
or percentile thresholding; raw luminance centroid is enough for the
first cut.

## Build (TBD — fill in as scaffolding lands)

Conventions inherited from EXRDemux:
- CMake (cross-platform from day one — Windows x64 + macOS arm64)
- vcpkg for OpenEXR / Imath / any other libs
- AE SDK lives in `third_party/` (can copy or git-submodule from the
  publicly-distributed Adobe AE SDK; do NOT keep duplicate paths in
  sync with EXRDemux — each repo is independent)
- Mac build pins `VCPKG_OSX_DEPLOYMENT_TARGET=11.0` via an overlay
  triplet — without it, vcpkg builds against the build host's SDK and
  the binaries fail at runtime on older macOS

## Reference test scene

Same scene as EXRDemux. See EXRDemux's `project_example_scene` memory
for full details. Path on the dev PC:

```
D:\Dropbox\David Carney\Blender Troubleshoot\Bad Romance (Curtains)\04_Renders\01_Components\Passes\Wall_Curtains_v1_0001.exr
```

Multipart EXR (58 parts, Blender 5.x), ~232 raw channels, 51 lightgroups
+ World/Image/Alpha + 4 cryptomattes. 5760×1440. Cryptomattes are out
of scope (handled by a different AE plugin).

## Things to be careful about

- **Mac support is non-negotiable.** Build on macOS arm64 from day
  one; no Win-only APIs in code; CI builds for both.
- **The repo is public** (matches EXRDemux). No SSH endpoints,
  usernames, or internal file paths in commits, comments, or release
  notes. Dev creds live in the assistant's local memory, not the repo.
- **Pass/lightgroup names are arbitrary.** Don't rely on regex
  patterns, zero-padding, casing rules, or any structure beyond "it's a
  string." Production scenes have inconsistent naming.
- **Spaces in pass names are normal.** No escaping prompts or warnings;
  handle routinely.
- **Cryptomattes are out of scope.** Don't filter on them as a feature,
  don't treat them as a problem to solve. They'll be in the file; ignore
  them or skip them where they'd be confusing.
- **AEGP plugin matchName is capped at 31 chars** (PF_MAX_EFFECT_NAME).
  Suggest `tdcarney Chase Maker` (20 chars) to mirror EXRDemux's
  `tdcarney EXRDemux`. Don't change it after the first release — saved
  AE projects key off it.
- **PiPL `out_flags` / `out_flags2` MUST match GlobalSetup's runtime
  values byte-for-byte.** AE rejects loading on any mismatch — surfaces
  as a load-time error dialog. (Carries over from EXRDemux's experience.)
- **Don't add features beyond what the task requires.** Prefer terse
  fixes; avoid premature abstractions; no half-finished implementations.
- **No notarization yet** (matches EXRDemux's current state). Mac
  builds will be ad-hoc signed; users will need `xattr -dr
  com.apple.quarantine ...` after download. When notarization is set
  up, replace this section. David has the Apple Developer account,
  notarization is on the table, just not done yet.

## Working with David

These rules carry over from EXRDemux work and are worth restating since
this repo's memory directory starts empty:

- **Don't run git operations.** David drives all commits/pushes/tags/
  branches/merges himself. I help by drafting commit messages and
  surfacing diffs; I never run `git commit`, `git push`, `git tag`,
  `git branch`, etc. Per-action overrides ("commit this") are fine.
  Established 2026-05-08.
- **Performance is first-class.** Speed drives architecture decisions.
  Targeting 4K+ resolution.
- **Verify in AE, not in standalone PNGs.** Standalone
  matplotlib/Pillow overlays haven't been useful for evaluation; use
  AE-native verification (solo layers, label colors, the comp panel).
- **Automate the dev iteration loop.** After each successful build,
  copy the fresh `.aex` to AE's plug-ins folder and clear any
  diagnostic log file. Don't make David do this manually.
- **Layer selection: name-based, never indices.** Index-based selection
  was the EXtractoR pain point that caused this whole project. Persist
  layer hashes (FNV-1a) of names; re-resolve on every render.
- **Naming is arbitrary.** Treat pass/lightgroup names as opaque
  strings. Don't write code that assumes regex patterns. The reference
  scene happens to use `<Role>_<NNN>` for many lightgroups, but other
  scenes will not.
- **Cryptos are out of scope.** Already said above; saying it again
  because it comes up a lot.

## What does NOT exist yet

This is a brand-new repo. Don't pretend any of the following exist when
proposing changes:

- Any source code
- A CMake setup
- A PiPL resource
- AEGP plugin scaffolding
- A UI (no framework integrated yet)
- An install path on this PC
- A version-numbering scheme
- A release process
- CI

Bootstrap accordingly. Phase A (build infra), Phase B (hello-world
panel), Phase C (proof of comm path with AE) are the first three
milestones — see the linked design discussion in
`EXRDemux/<somewhere>` if you need to go deeper. (TODO: pull design
notes into a docs/ file in this repo once it's set up.)

## Where to look

(Empty for now — fill in as code lands.)
