# Mac mini setup (one-time)

Once-per-machine prerequisites for building Chase Maker on a Mac. After
this, day-to-day use is just `git pull && cmake --build` like Windows.

This file parallels EXRDemux's `docs/mac_setup.md`. If you've already
set up the Mac for EXRDemux, the tools section is satisfied and you
can jump straight to **Repo + dependencies**.

## Tools

```sh
# Xcode Command Line Tools (clang, Rez, Make)
xcode-select --install

# CMake — install whichever way you prefer:
brew install cmake          # via Homebrew (recommended)
```

Verify:
```sh
clang --version       # Apple clang 14.x or newer
cmake --version       # 3.25 or newer
xcrun --find Rez      # Apple's resource compiler
```

## Repo + dependencies

```sh
git clone https://github.com/thedavidcarney/ChaseMaker.git
cd ChaseMaker

# vcpkg — same git clone trick we used on Windows
git clone --depth 1 https://github.com/microsoft/vcpkg.git third_party/vcpkg
./third_party/vcpkg/bootstrap-vcpkg.sh -disableMetrics
```

## After Effects SDK

The AE SDK is gitignored (Adobe license forbids redistribution), so you
download it once on the Mac mini just like you did on Windows.

1. Sign in at <https://developer.adobe.com/after-effects/> with your
   Adobe Developer account.
2. Download the **Mac SDK** (the same file family as the Windows one,
   matching your AE version — currently 25.6).
3. Place the downloaded zip into `third_party/AfterEffectsSDK/Mac/` so
   you have:
   ```
   third_party/AfterEffectsSDK/Mac/AfterEffectsSDK_25.6_61_mac/
       README-HowToExtractZstdBuild-Mac.txt
       ae25.6_61.64bit.AfterEffectsSDK.tar.zstd.zip
       extractzstd.sh
       zstd
   ```
4. Run the extraction script that Adobe ships:
   ```sh
   cd third_party/AfterEffectsSDK/Mac/AfterEffectsSDK_25.6_61_mac
   chmod +x extractzstd.sh
   ./extractzstd.sh
   ```
   You should now see a folder `ae25.6_61.64bit.AfterEffectsSDK/`
   alongside the script, containing `Examples/Headers/AE_Effect.h` etc.

## First build

```sh
cmake --preset mac-arm64-release   # configures + installs vcpkg deps
cmake --build --preset mac-arm64-release
```

The output is a bundle:
```
build/mac-arm64-release/ChaseMaker.plugin/
    Contents/
        Info.plist
        MacOS/ChaseMaker
        Resources/ChaseMaker.rsrc
```

## Install in AE

```sh
mkdir -p "/Applications/Adobe After Effects 2025/Plug-ins/ChaseMaker"
cp -R build/mac-arm64-release/ChaseMaker.plugin \
      "/Applications/Adobe After Effects 2025/Plug-ins/ChaseMaker/"
```

Restart AE → **Window → Chase Maker**.

## Caveats

- **First vcpkg build is slow.** The arm64-osx triplet may not have
  prebuilt binaries cached, so the first `cmake --preset` can spend
  several minutes building OpenEXR + Imath from source. Subsequent
  configures are fast.
- **Don't strip the deployment-target overlay triplet** at
  `cmake/overlay-triplets/arm64-osx.cmake`. It pins
  `VCPKG_OSX_DEPLOYMENT_TARGET=11.0` so vcpkg builds OpenEXR/Imath
  against an SDK that older macOS hosts can actually load. Without
  this, the binaries fail at runtime on anything but the build host.
- **Ad-hoc code signing.** macOS Gatekeeper may refuse to load an
  unsigned plugin. For dev, sign the bundle ad-hoc after each build:
  ```sh
  codesign --force --deep --sign - build/mac-arm64-release/ChaseMaker.plugin
  ```
  End users hit the same issue and clear it with
  `xattr -dr com.apple.quarantine /Applications/...ChaseMaker.plugin`.
  Real notarization is on the roadmap but not wired up yet.
