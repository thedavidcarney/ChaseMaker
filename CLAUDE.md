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
   derived data, builds the chase comps directly via the AEGP API
   (the "dumb comps" builder), applying the `tdcarney EXRDemux` effect
   and setting layer hashes itself. Hand-off in: the EXR file.

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
  window). `ImGui_ImplDX11` + `ImGui_ImplWin32`, `~30 Hz` `WM_TIMER`.
  **The D3D11 device is the WARP software rasterizer, NOT the
  hardware GPU driver — this is load-bearing, do not change it.**
  A second *hardware* D3D device in AE's process (alongside AE's own
  GPU use) destabilized the NVIDIA driver and froze/crashed all of
  AE; every crash dump faulted inside `nvwgf2umx.dll`. The panel is
  a 2D ImGui UI with zero need for GPU accel, so WARP is correct and
  removes us from the GPU driver entirely. Swap chain is flip-model
  with a frame-latency *waitable* (polled non-blocking — never block
  AE's UI thread in Present). Review the panel renderer
  lifecycle + GPU invariants notes before touching the renderer.
- **macOS**: adds an MTKView (`ChaseMakerMTKView` subclass) as a
  subview of AE's container NSView. Metal + `ImGui_ImplMetal` +
  `ImGui_ImplOSX`. Display link drives redraws. (No WARP analog —
  Metal is fine; the GPU-driver issue was Windows/NVIDIA-specific.)

**Renderer lifecycle invariant (both backends):** AEGP has no
panel-destroy callback; AE destroys the container and makes a new
one on reopen. The Windows renderer self-destructs on `WM_NCDESTROY`.
`PanelState` (a plugin global) outlives the renderer, so any
per-renderer GPU handle cached on it (`LayerInfo::texture_id`,
`chase_composite_texture_id`) MUST be zeroed when the renderer
releases its textures — on Win *and* Mac — or a reopened panel draws
a freed cross-renderer texture and crashes AE. `src/diag_log.h`
writes load-path/teardown milestones to
`<temp>/chasemaker_load.log` for diagnosing any regression.
- Cross-platform UI code lives in `src/panel_ui.cpp` and works
  against the shared `PanelState`; both platform renderers just call
  `panel_ui::RenderFrame()` from their per-frame hook.

Keyboard input was hard-won. See the ImGui keyboard-input notes
for the working recipe (Windows deferred SetFocus + consume-
on-WantCapture; Mac lazy-init + KeyEventResponder firstResponder
routing; *no* NSEvent consume-monitor on Mac — it breaks the
`interpretKeyEvents:` → `insertText:` character-input path).

Qt was the original alternative and remains a known-good fallback if
ImGui ever gets in the way, but ImGui has carried us through panel
chrome, table, modal file picker, thumbnails (GPU-uploaded
textures), and the entire sort-mode UX without complaint.

## Communication contract with EXRDemux

**AEGP API at runtime.** Chase Maker reads project state and calls
AEGP suites to apply `tdcarney EXRDemux` effects, build comps, set
layer hashes, etc. In-process, immediate. This is the only path.

> The old JSON luminosity sidecar (`<exr>.luminosity.json` consumed
> by EXRDemux's JSX) has been **removed** — superseded by the
> in-process "dumb comps" AEGP builder. Don't reintroduce a sidecar
> or JSX hand-off; if external/batch participation is ever needed,
> design it fresh against the then-current builder, not this dropped
> contract.

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

## Release checklist

Releasing is handled **in tandem with EXRDemux** — same conventions
(see EXRDemux's CLAUDE.md "Release checklist"). Chase Maker is an
AEGP panel, not a PF effect, so there are no `PF_VERSION` macros or
`.r` `AE_Effect_Version` to bump — the version lives in the **git
tag** and the GitHub Release. (Known gap: the panel title currently
shows a build *timestamp*, not the release version — surfacing the
tag in the title is a future nicety, not done yet. CMake
`project(... VERSION)` is not maintained per-release, mirroring
EXRDemux.)

Convention: the **1.0 release candidate is tagged `v0.9.0`** (plain
semver, no `-rc` suffix — exactly how EXRDemux does its 1.0 RC),
described in the README as "the 1.0 release candidate aimed at AE
2026". Bump to `v1.0.0` only when the team signs off.

When cutting a release:

1. **README.md** — keep the RC callout + "Things I didn't test very
   much" current; the version line names the tag (e.g. `v0.9.0`).
2. **Build artifacts** — rebuild Win Release `.aex` + Mac Release
   `.plugin` (Mac `codesign --force --deep --sign -`), zip each with
   `THIRD_PARTY_LICENSES.txt` into
   `release/ChaseMaker-vX.Y.Z-{win-x64,mac-arm64}.zip`. The Mac zip
   must be made **on the Mac** (`zip -r -y` / `ditto`) to preserve
   the bundle + `_CodeSignature`.
3. **`release/` is gitignored** (build outputs) — the zips must be
   force-added: `git add -f release/ChaseMaker-vX.Y.Z-*.zip`. (Same
   as EXRDemux.)
4. **Git tag** `vX.Y.Z` on the release commit.
5. **GitHub Release** — mark **pre-release** for an RC; attach both
   zips; release notes = summary of commits since the last tag.
   EXRDemux uses the web UI (no `gh`); ChaseMaker's remote works with
   `gh` if preferred. David runs all git/tag/push/release steps.

David drives every git/tag/push/release action himself (see "Working
with David"); the assistant prepares files, zips, the commit message
and release notes, but does not run git or gh.

## Source map

- `src/chase_maker.cpp` — AEGP plugin entry point. EntryPointFunc,
  ChaseMakerPlugin class, panel hook registration. Owns the panel
  state global (`i_panel_state`) so state survives a panel close/
  reopen even when AE destroys the platform view.
- `src/chase_maker.r` — PiPL resource. `Kind { AEGP }`.
- `src/panel_renderer.h` — abstract factory. `CreatePanelRenderer(void*, PanelState*)`.
- `src/panel_renderer_win.cpp` — Windows backend (ImGui on a **WARP**
  D3D11 device, flip-model waitable swapchain). Subclasses AE's HWND;
  self-destructs on WM_NCDESTROY; zeroes PanelState texture ids on
  release. (Do not switch to a hardware D3D device — see UI section.)
- `src/panel_renderer_mac.mm` — macOS backend (Metal + ImGui).
  MTKView subview; NSMutableArray<MTLTexture> thumbnail cache; same
  zero-PanelState-texture-ids-on-release rule. `-fobjc-arc`.
- `src/diag_log.h` — header-only, crash-resilient load-path/teardown
  logger → `<temp>/chasemaker_load.log` (Win+Mac). Diagnostic aid.
- `src/panel_state.h` — `LayerInfo`, `PanelState`, `SortMode`,
  `SortLayers()`, `Chase`/`ChaseStage`/`ScatterHit`, `Regenerate
  Scatter()` (shared inline — builder regenerates it too), Binds/
  Tags/overrides, undo snapshot. Mutex-guarded shared data. NOTE:
  must stay free of `std::max`/`std::min` (windows.h macro clash).
- `src/panel_ui.h/.cpp` — cross-platform ImGui drawing. Header
  (open EXR / status), sort-mode combo, preview controls,
  layer table, centroid scatter canvas, thumbnail preview pane.
- `src/exr_scan.h/.cpp` — EXR multipart enumeration (Blender 5.x
  multipart compatible), per-layer metrics (`ComputeMetrics`),
  thumbnail downsample (`GenerateThumbnail`).
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

Same scene as EXRDemux. The local reference EXR lives on the dev
machine only — its path is intentionally not checked into this
public repo.

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

**In** as of 2026-05-15 (the chase-building flow is now substantially
built — this section was very stale before):
- AEGP panel scaffolding (Win WARP+ImGui / Mac Metal+ImGui), keyboard
  input, native file picker, layer table with shift/ctrl multi-select
  + drag-reorder, exclusion + substring filter, centroid scatter map.
- EXR multipart scan (worker thread), per-layer metrics (whole-layer
  + hotspot centroid, peak, total luminance), thumbnail textures with
  `scan_generation` lifecycle, preview animation.
- Tags: auto-tag by name prefix AND a single-member tag per
  unique-named layer (every light is reachable from the wizard's tag
  filter — no untagged bucket).
- Session-level Binds + a separate **chase-local "Bind"** weld (same
  Staging multi-select UX, but welds only that chase — NOT a session
  bind; see feedback_chase_editor_ux.md).
- Chases: color-coded tabs, New-Chase wizard with multi-select tag
  filter (All/None) and templates: Left→Right, Top→Bottom, **Center
  Out** (HotspotX, symmetric mirrored pairs), 3 Step, **Random**
  (the stratified scatter generator — its own editor: loop length,
  density, seed/reseed, seamless wrap), Custom. Manual stage editing
  (drag-reorder + chase-local Bind/Unbind, `manual_stages`
  persistence), `symmetric_pairs`, `desired_stage_count`.
- Chase preview: live composite + click-to-pin a light; centroid map
  filtered to that chase's lights. Opacity + Exposure-gamma envelope.
- **"Dumb comps" AEGP builder is wired and working** (per-chase
  "Build in AE" + session "Build all chases"): comps/footage/layers,
  EXRDemux applied + FNV hashes set, opacity/gamma keyframes, black
  solid + Lighten, a SINGLE reused "ChaseMaker" folder at project
  root (comps inside are name-versioned `_vNN` on collision so
  nothing is overwritten — the folder is no longer re-versioned per
  build), scatter build path with seamless-loop wrap layers.
- Session save/load (JSON), undo/redo. Tab-X delete confirmation
  (the only delete affordance). Build-stamp + auto-install
  POST_BUILD. Windows build emits a matching PDB; load-path
  diagnostic log (diag_log.h).
- **Playtest UX (2026-05-18/19), all shipped — see
  `project_playtest_followups.md` for the locked invariants:**
  ONE project FPS (default 30, edited/pinned in the Sources tab,
  drives preview + every build); animation/sequence **loop-mode**
  (auto for sequence sources, footage time-locked, seamless loop =
  source duration × integer loop_multiple, wrapped envelope
  keyframes); Lighten preview compositing with shared normalization;
  **Chunks** stage grouping (number of groups, even split — NOT the
  rejected per-light "clumps"); `FrameSlider` widget (left label,
  drag, Ctrl/double-click to type, -/+); preview-size buttons
  S/M/L/XL/XXL = thumbnail resolution, pane grows / stages list
  squished; per-source Rescan; EXRDemux-missing in-panel warning;
  built comp layers renamed to the light name; strict
  digits-only sequence resolver. Keyboard nav code exists but is
  inert (AE host doesn't forward arrows — deferred).

**Not yet**:
- Bind → precomp emission. Binds currently expand as flat stacked
  layers; one reusable precomp per bind is still planned (see
  project_dumb_comps_builder.md).
- Sub-groups beyond chase-local Bind (multiple independent ordered
  groups with their own sort played alongside the main chase).
- Random "pleasantness": brightness/spatial weighting was explicitly
  deferred — v1 scatter is temporal-only stratification.
- Notarization (Mac ad-hoc signed). Version scheme + release + CI.

## Working notes (current — 2026-05-19)

- **Stage grouping is "Chunks" and that's FINAL.** It churned
  stages→clumps→Chunks across playtests. Chunks = number of groups,
  even split. Do NOT revert to "clumps" (lights-per-group) or the
  old ceil-of-ceil stage count. See `project_playtest_followups.md`.
- **Preview size = thumbnail resolution** (S/M/L/XL/XXL buttons);
  the split pane grows the preview and squishes the stages list.
- **Keyboard nav is still dead** (inert code, AE host doesn't
  forward arrow/Ctrl keydowns). Don't rabbit-hole without the
  platform-layer key-forwarding work; it's a deferred focused task.
- Default project FPS is 30 (David's 99% case).

- **The multi-day AE crash/freeze saga is RESOLVED** and
  user-confirmed stable on Win + Mac. Root cause + the (3) fixes +
  the do-not-revert directives are in
  `panel_close_reopen_lifecycle.md`. Read it before touching the
  renderer. WARP on Windows is load-bearing.
- Keyboard-input recipe: `imgui_keyboard_focus.md` if text input
  ever breaks again.
- David likes the diverse sort modes (Hotspot/Centroid/Radial/etc.) —
  low-effort high-visibility area to keep enriching.
- Chase-local "Bind" is deliberately chase-scoped, not a session
  bind, despite reusing Staging's pattern + name. Don't "unify" them.
- "Random" = the scatter generator, not a sort mode. Don't revert it
  to random-sort-one-per-stage.
