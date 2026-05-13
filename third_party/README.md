# third_party/

Vendored dependencies that are NOT checked into this repo and must be
provisioned locally before the build will configure.

The whole directory is gitignored except for this README. Don't commit
anything else here — the AE SDK in particular has license terms that
forbid redistribution.

## After Effects SDK (required)

The AE SDK is distributed by Adobe and cannot be checked into a public
repo. Download it from the Adobe Developer Console and extract it into
`third_party/AfterEffectsSDK/` so the layout is:

```
third_party/AfterEffectsSDK/
├── Win/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK/
│   └── Examples/
│       ├── Headers/
│       ├── Resources/
│       └── Util/
└── Mac/AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK/
    └── Examples/ ...
```

CMake checks for `Examples/Headers/AE_Effect.h` and errors out at
configure time if the SDK isn't where it expects it. If you've
extracted a differently-named SDK release, adjust `AE_SDK_PLATFORM_DIR`
in the top-level `CMakeLists.txt`.

Windows builds additionally need `Examples/Resources/PiPLtool.exe`
(ships with the SDK) for the PiPL resource pipeline.

macOS builds need Apple's `Rez` from the Xcode Command Line Tools:

```
xcode-select --install
```

## vcpkg (required)

The build uses vcpkg in manifest mode for OpenEXR / Imath. Clone vcpkg
into `third_party/vcpkg/`:

```
git clone https://github.com/microsoft/vcpkg third_party/vcpkg
third_party/vcpkg/bootstrap-vcpkg.bat        # Windows
third_party/vcpkg/bootstrap-vcpkg.sh         # macOS
```

`CMakePresets.json` points at `third_party/vcpkg/scripts/buildsystems/vcpkg.cmake`
as the toolchain file, so the first configure pulls and builds
dependencies automatically.

The `mac-arm64-release` preset additionally uses the overlay triplet
at `cmake/overlay-triplets/arm64-osx.cmake` to pin
`VCPKG_OSX_DEPLOYMENT_TARGET=11.0`. Without that pin, vcpkg builds
the static libs against the build host's running macOS SDK, which
makes the resulting plugin fail at runtime on older Macs.
