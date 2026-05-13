# Chase Maker

An After Effects AEGP panel plugin for building virtual light chases
from per-light EXR passes. Companion tool to
[EXRDemux](https://github.com/thedavidcarney/EXRDemux).

> ## ⚠️ Pre-alpha — nothing usable yet
>
> This repo is in the scaffolding phase. The plugin currently registers
> an empty panel under `Window → Chase Maker`; there's no UI, no EXR
> reading, no chase building. Come back later.
>
> ## How it's being built
>
> Vibe-coded with [Claude](https://www.anthropic.com/claude) (Anthropic).

## What Chase Maker is (eventually)

A panel inside After Effects (`Window → Chase Maker`) that:

- Reads a multilayer EXR's per-light passes
- Lets you preview and organize lights spatially
- Sequences light "chases" (which lights are on at which time) with
  fine-grained control
- Drives EXRDemux to assemble the chase as an AE comp — one render-time
  layer per light, the chase animation baked in

The render side is handled by EXRDemux (separately maintained), which
exposes per-layer EXR selection as a scriptable, name-based property.
Chase Maker is the orchestration / authoring side.

## What Chase Maker isn't

A compositor. Color, finishing, and final-frame work happen elsewhere.
Chase Maker only builds the chase structure.

## Building

The build uses CMake + vcpkg, cross-platform from day one (Windows x64
+ macOS arm64). See [`third_party/README.md`](third_party/README.md)
for the one-time AE SDK + vcpkg setup, then:

### Windows

From a Developer Command Prompt (so MSVC + Ninja are on PATH):

```
cmake --preset win-x64-release
cmake --build --preset win-x64-release
```

The build drops `ChaseMaker.aex` into
`build/win-x64-release/` and (on this dev machine) auto-installs it to
`C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\ChaseMaker\`.
Override the install location with `-DCHASEMAKER_AE_PLUGIN_DIR=<path>`
or disable with `-DCHASEMAKER_AE_PLUGIN_DIR=`.

### macOS

See [`docs/mac_setup.md`](docs/mac_setup.md) for one-time prereqs.
After that:

```
cmake --preset mac-arm64-release
cmake --build --preset mac-arm64-release
```

Produces a `ChaseMaker.plugin` bundle under
`build/mac-arm64-release/`.

## Install

**Windows:** drop `ChaseMaker.aex` into
`C:\Program Files\Adobe\Adobe After Effects 2025\Support Files\Plug-ins\ChaseMaker\`
and restart AE. (Windows builds on this dev machine install
automatically — see Building.)

**macOS:** drop the `ChaseMaker.plugin` bundle into
`/Applications/Adobe After Effects 2025/Plug-ins/ChaseMaker/` and
restart AE. The bundle is ad-hoc signed; if Gatekeeper blocks it, run
`xattr -dr com.apple.quarantine /Applications/Adobe\ After\ Effects\ 2025/Plug-ins/ChaseMaker/ChaseMaker.plugin`.

The panel appears under **Window → Chase Maker**.

## Acknowledgements

Chase Maker statically links **OpenEXR** and **Imath**
(both BSD-3-Clause).

## License

MIT — see [`LICENSE`](LICENSE).
