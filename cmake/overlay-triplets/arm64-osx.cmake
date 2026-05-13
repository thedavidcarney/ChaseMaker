# Overlay triplet: pin macOS deployment target to arm64's minimum (11.0).
#
# vcpkg's default arm64-osx triplet leaves VCPKG_OSX_DEPLOYMENT_TARGET
# unset, which means it falls through to the build machine's running OS
# version. On a Tahoe (26.x) build host, that produces static libs
# carrying minOS=26.0 in their LC_BUILD_VERSION load command. The final
# plugin links and runs on the build machine, but it can fail at runtime
# on older macOS (e.g. Sequoia 15.x) because OpenEXR/Imath code paths
# may reference symbols only present in newer system libraries.
#
# Pinning to 11.0 here — the absolute minimum for arm64 (Big Sur was
# the first macOS to support Apple Silicon) — keeps the libs portable
# down to the oldest arm64-capable Macs.

set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "11.0")
