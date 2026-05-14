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

## UI framework — Dear ImGui (integrated)

Dear ImGui is wired in via vcpkg with platform backends:

- **Windows**: subclasses AE's container HWND directly (no child
  window). DX11 swap chain bound to that HWND; `ImGui_ImplDX11` +
  `ImGui_ImplWin32`. `~60 Hz` `WM_TIMER` drives redraws.
- **macOS**: adds an MTKView (`ChaseMakerMTKView` subclass) as a
  subview of AE's container NSView. Metal + `ImGui_ImplMetal` +
  `ImGui_ImplOSX`. Display link drives redraws.
- Cross-platform UI code lives in `src/panel_ui.cpp` and works
  against the shared `PanelState`; both platform renderers just call
  `panel_ui::RenderFrame()` from their per-frame hook.

Keyboard input was hard-won. See the
[ImGui keyboard input inside an AE panel](C:\Users\User\.claude\projects\C--Users-User-Documents-GitHub-ChaseMaker\memory\imgui_keyboard_focus.md)
memory for the working recipe (Windows deferred SetFocus + consume-
on-WantCapture; Mac lazy-init + KeyEventResponder firstResponder
routing; *no* NSEvent consume-monitor on Mac — it breaks the
`interpretKeyEvents:` → `insertText:` character-input path).

Qt was the original alternative and remains a known-good fallback if
ImGui ever gets in the way, but ImGui has carried us through panel
chrome, table, modal file picker, sidecar JSON UI, thumbnails (GPU-
uploaded textures), and the entire sort-mode UX without complaint.

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
  },
  "active_order": ["<display_name>", ...]
}
```

`active_order` is the user-curated included subset in the order it
should play in the chase. Additive to the original contract —
readers that don't know about it can fall back to sorting `layers`
by cx themselves.

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

## Per-layer metrics & sort modes

Computed once per scan in `src/exr_scan.cpp::ComputeMetrics`. All
sort modes are pure comparator changes over the cached values —
switching modes is instant.

Per layer:
- **Whole-layer centroid** (`cx`, `cy`) — luminance-weighted, Rec.709
  (`0.2126*R + 0.7152*G + 0.0722*B`), negative pixels clamped to 0,
  `cx = Σ(L*x) / ΣL`, normalized to `[0,1]`. Pulls toward broad
  spread (spill, bounce).
- **Hotspot centroid** (`cx_hot`, `cy_hot`) — same formula but only
  over pixels at ≥ 50% of the layer's peak luminance. Snaps to the
  bright concentrated source; ignores soft glow.
- **Peak position** (`peak_x`, `peak_y`) — single brightest pixel.
- **Total luminance** (`total`) — sum of L over the whole layer.

Sort modes exposed in the UI:
- Centroid (Left → Right) / (Top → Bottom)
- Hotspot (Left → Right) / (Top → Bottom)
- Brightness (Brightest first)
- Radial Sweep (atan2 angle around image center)
- Distance from Center (in → out)
- Random (deterministic per-session seed; re-shuffle button reseeds)

Plus a universal **Reverse** toggle that flips any sort.

Skip list (no centroid computed, layer doesn't appear in the active
table): `Image`, `Alpha`, `World`, `HDRI`, `Ambient`, anything
containing `Crypto*`. Matches EXRDemux JSX's "pinned at bottom +
disabled by default" set.

Validated against the Wall_Curtains 52-light reference scene; sort
order matches the actual scene layout by eye. Reference Python
implementation: `EXRDemux/scripts/dev/luminance_centroid.py`
(centroid only — the hotspot/peak variants exist only in
ChaseMaker).

## Build

```sh
# Windows (from Developer Command Prompt or a vcvars64-initialized shell)
cmake --preset win-x64-release
cmake --build --preset win-x64-release

# macOS
cmake --preset mac-arm64-release
cmake --build --preset mac-arm64-release
```

POST_BUILD on Windows auto-installs `ChaseMaker.aex` to
`C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\ChaseMaker\`.
Disable with `-DCHASEMAKER_AE_PLUGIN_DIR=`. **AE has to be closed**
when the install step runs (Windows memory-maps the .aex).

vcpkg deps: `openexr`, `imath`, `imgui` (with `docking-experimental`
+ platform backends). Mac additionally links Cocoa, CoreFoundation,
Metal, MetalKit, QuartzCore, GameController (`imgui_impl_osx.mm`
imports `<GameController/GameController.h>` unconditionally — lazy-
loaded at runtime, no cost),  UniformTypeIdentifiers.

Mac build pins `VCPKG_OSX_DEPLOYMENT_TARGET=11.0` via an overlay
triplet — without it, vcpkg builds against the build host's SDK and
the binaries fail at runtime on older macOS.

Build stamp: `cmake/gen_build_stamp.cmake` regenerates
`generated/chase_maker_build_stamp.h` on every build, baked into the
panel title via `AEGP_SetTitle` so each test cycle is visually
distinct.

## Source map

- `src/chase_maker.cpp` — AEGP plugin entry point. EntryPointFunc,
  ChaseMakerPlugin class, panel hook registration. Owns the panel
  state global (`i_panel_state`) so state survives a panel close/
  reopen even when AE destroys the platform view.
- `src/chase_maker.r` — PiPL resource. `Kind { AEGP }`.
- `src/panel_renderer.h` — abstract factory. `CreatePanelRenderer(void*, PanelState*)`.
- `src/panel_renderer_win.cpp` — Windows backend (DX11 + ImGui).
  Subclasses AE's HWND; manages DX11 thumbnail texture cache.
- `src/panel_renderer_mac.mm` — macOS backend (Metal + ImGui).
  MTKView subview; manages NSMutableArray<MTLTexture> thumbnail
  cache. Compiled with `-fobjc-arc`.
- `src/panel_state.h` — `LayerInfo`, `PanelState`, `SortMode`,
  `SortLayers()`, `LayerDisplayX/Y()`. The mutex-guarded data the
  scanner and UI share.
- `src/panel_ui.h/.cpp` — cross-platform ImGui drawing. Header
  (open EXR / status / sidecar), sort-mode combo, preview controls,
  layer table, centroid scatter canvas, thumbnail preview pane.
- `src/exr_scan.h/.cpp` — EXR multipart enumeration (Blender 5.x
  multipart compatible), per-layer metrics (`ComputeMetrics`),
  thumbnail downsample (`GenerateThumbnail`), sidecar JSON writer.
  Worker-thread scan kicked off by `StartScan`.
- `src/hash.h/.cpp` — FNV-1a 32-bit, byte-for-byte compatible with
  EXRDemux's `HashLayerName`.
- `src/file_dialog.h` + `src/file_dialog_win.cpp` + `src/file_dialog_mac.mm`
  — native open-file picker.
- `cmake/gen_build_stamp.cmake` — regenerated header injected as
  `CM_BUILD_STAMP`.
- `cmake/MacInfoPlist.in` — bundle plist. `CFBundlePackageType = AEgx`
  (NOT `eFKT` — that's the PF effect package type, AE silently
  filters AEGP bundles that get it wrong).

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

## What's in vs. what's not

**In** as of May 2026:
- AEGP panel scaffolding (Win + Mac), Dear ImGui rendering, keyboard
  input cross-platform, file picker, scrollable layer table, click-
  to-select, exclusion checkbox + filter-by-substring buttons.
- EXR multipart scan on a worker thread, per-layer metrics (whole-
  layer centroid, hotspot centroid, peak, total luminance), GPU
  thumbnail textures cached per platform with `scan_generation`
  lifecycle, preview-cycle animation with adjustable speed and
  thumbnail resolution.
- Eight sort modes plus universal Reverse and stable Random.
- JSON sidecar writer with `active_order`.
- Build-stamp generator + auto-install POST_BUILD.

**Not yet**:
- Driving AE itself — no `AEGP_*` calls to create comps, apply
  EXRDemux effects, set layer hashes, etc. The hand-off into AE is
  the next big design pass (David hasn't sat down with this).
- Sub-groups (multiple ordered groups, e.g. "fire group" with its
  own sort, played alongside the main chase). Today's "exclude" is
  a binary toggle; there's nowhere for the excluded set to *be*.
- Notarization (Mac builds still ad-hoc signed).
- A version-numbering scheme + release process + CI.

## Working notes from this session (2026-05-13)

- The kbd input saga revealed a lot about how AE handles events.
  Captured in the `imgui_keyboard_focus.md` memory; if any text
  input ever breaks again, start there.
- David explicitly liked the diverse sort modes (Hotspot vs.
  Centroid, Radial Sweep, etc.). Sort modes are a low-effort high-
  visibility area to keep enriching.
- David flagged but did NOT design sub-grouping — wants the actual
  chase-building flow figured out first so groups fall out of it
  naturally. **Don't pre-empt that with speculative group UX.**
