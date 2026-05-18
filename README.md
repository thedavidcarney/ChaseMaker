# Chase Maker

An After Effects AEGP panel plug-in that builds virtual light chases
from per-light EXR passes. The authoring half of a three-tool
light-pass workflow; its render-side companion is
[EXRDemux](https://github.com/thedavidcarney/EXRDemux).

> ## ⚠️ 1.0 release candidate — gather team feedback before production
>
> **v0.9.0 is the 1.0 release candidate, aimed at AE 2026.** The full
> pipeline (scan → organize → sequence → build comps in AE) works on
> Windows x64 and macOS arm64, but it wants broad real-show testing
> before it's called v1. Use it on real work and send feedback.
>
> ## How it was built
>
> Entirely vibe-coded with [Claude](https://www.anthropic.com/claude) (Anthropic).

## What Chase Maker is

A dockable panel inside After Effects (**Window → Chase Maker**) that:

- Scans a multilayer / multipart EXR (still **or** animated image
  sequence) and pulls out every per-light pass.
- Computes per-light spatial metrics (luminance centroid, hotspot,
  peak, total) and lets you sort/organize lights by them — Hotspot,
  Centroid, Radial sweep, Distance-from-center, Brightness, Random.
- Lets you build **chases** — which lights are lit at which time —
  from templates (Left→Right, Top→Bottom, Center Out, 3 Step, Random
  scatter) or by hand, with a live in-panel preview.
- Builds the chase directly into the AE project: one render-time
  layer per light with the **EXRDemux** effect applied and the
  layer-name hash set, opacity + Exposure-gamma envelopes keyframed,
  on a Lighten stack over a black solid.
- For animated sequence sources, builds a **seamless loop** the exact
  length of the source — footage time-locked, the chase expressed as
  a wrapped envelope so it loops cleanly with the scene animation.

The render side (name-based, re-render-stable per-layer EXR selection)
is handled by EXRDemux, maintained separately. **EXRDemux must be
installed** for built comps to render correctly — Chase Maker warns
you in the panel if it isn't.

## What Chase Maker isn't

A compositor. Color, finishing, and final-frame work happen
elsewhere. Chase Maker only builds the chase structure.

## Things I didn't test very much

- AE versions other than 2025 and 2026. v0.9.0 builds and installs
  on Win x64 + macOS arm64 for AE 2025 and 2026; AE 2025 has the most
  mileage, broad AE 2026 real-comp testing is exactly what this RC is
  for.
- macOS breadth — it builds, loads, scans, previews and drives the
  builder on Apple Silicon, but it has had far less real-show time
  than Windows.
- Animated-sequence / seamless-loop builds on production scenes
  (the loop-mode path is new — verify comp length and that the loop
  stays in sync with the scene animation).
- Sessions with many sources, very large layer counts, or unusual
  pass naming.

Any testing helps — AE 2026 and animation/loop feedback especially.
Feedback or wishlists welcome.

## Install

Requires **EXRDemux** installed in the same AE
([github.com/thedavidcarney/EXRDemux](https://github.com/thedavidcarney/EXRDemux)).

**Windows:** drop `ChaseMaker.aex` into
`C:\Program Files\Adobe\Adobe After Effects <version>\Support Files\Plug-ins\ChaseMaker\`
(e.g. `Adobe After Effects 2026`) and restart AE.

**macOS:** drop the `ChaseMaker.plugin` bundle into
`/Applications/Adobe After Effects <version>/Plug-ins/ChaseMaker/`
(e.g. `Adobe After Effects 2026`) and restart AE. The bundle is
ad-hoc signed; if Gatekeeper blocks it, run
`xattr -dr com.apple.quarantine "/Applications/Adobe After Effects <version>/Plug-ins/ChaseMaker/ChaseMaker.plugin"`.

The panel appears under **Window → Chase Maker** (dockable, or
float it as a separate editor).

## Building

CMake + vcpkg, cross-platform (Windows x64 + macOS arm64). See
[`third_party/README.md`](third_party/README.md) for the one-time AE
SDK + vcpkg setup and [`docs/mac_setup.md`](docs/mac_setup.md) for
macOS prereqs.

### Windows

From a Developer Command Prompt (MSVC + Ninja on PATH):

```
cmake --preset win-x64-release
cmake --build --preset win-x64-release
```

Drops `ChaseMaker.aex` into `build/win-x64-release/` and, on this dev
machine, auto-installs it to the AE 2025 Plug-ins folder. Override
with `-DCHASEMAKER_AE_PLUGIN_DIR=<path>` or disable with
`-DCHASEMAKER_AE_PLUGIN_DIR=`.

### macOS

```
cmake --preset mac-arm64-release
cmake --build --preset mac-arm64-release
```

Produces a `ChaseMaker.plugin` bundle under `build/mac-arm64-release/`
(ad-hoc signed).

## Acknowledgements

Chase Maker is the authoring companion to
[EXRDemux](https://github.com/thedavidcarney/EXRDemux), which handles
the render-time, name-based EXR layer selection the built comps rely
on.

It statically links **OpenEXR** and **Imath** (both BSD-3-Clause) and
**Dear ImGui** (MIT). Full attribution is in
[`THIRD_PARTY_LICENSES.txt`](THIRD_PARTY_LICENSES.txt).

## License

MIT — see [`LICENSE`](LICENSE).
