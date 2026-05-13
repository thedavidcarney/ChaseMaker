cmake_minimum_required(VERSION 3.25)

# Enable Objective-C++ when building on macOS so .mm files compile.
if(APPLE)
    project(ChaseMaker LANGUAGES C CXX OBJC OBJCXX VERSION 0.1.0)
else()
    project(ChaseMaker LANGUAGES C CXX VERSION 0.1.0)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Match vcpkg's runtime: dynamic CRT (/MD or /MDd).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# macOS deployment target. arm64 minimum is 11.0 (Big Sur, first OS
# with Apple Silicon support). Must match VCPKG_OSX_DEPLOYMENT_TARGET
# in cmake/overlay-triplets/arm64-osx.cmake or vcpkg's static libs
# will be built against a different (typically newer) SDK and
# reference symbols missing on older macOS.
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0"
        CACHE STRING "Minimum macOS version" FORCE)
endif()

# ----- Dependencies -----
find_package(OpenEXR CONFIG REQUIRED)
find_package(Imath CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# ----- AE SDK location (gitignored, dropped in by user per third_party/README.md) -----
set(AE_SDK_ROOT "${CMAKE_SOURCE_DIR}/third_party/AfterEffectsSDK")
if(WIN32)
    set(AE_SDK_PLATFORM_DIR
        "${AE_SDK_ROOT}/Win/AfterEffectsSDK_25.6_61_win/ae25.6_61.64bit.AfterEffectsSDK")
elseif(APPLE)
    # Mac SDK extraction layout TBD; CI will need to set this.
    set(AE_SDK_PLATFORM_DIR
        "${AE_SDK_ROOT}/Mac/AfterEffectsSDK_25.6_61_mac/ae25.6_61.64bit.AfterEffectsSDK")
else()
    message(FATAL_ERROR "Unsupported platform; AE plugins are Windows + macOS only.")
endif()

set(AE_SDK_HEADERS   "${AE_SDK_PLATFORM_DIR}/Examples/Headers")
set(AE_SDK_UTIL      "${AE_SDK_PLATFORM_DIR}/Examples/Util")
set(AE_SDK_RESOURCES "${AE_SDK_PLATFORM_DIR}/Examples/Resources")

if(NOT EXISTS "${AE_SDK_HEADERS}/AE_Effect.h")
    message(FATAL_ERROR
        "After Effects SDK headers not found at:\n"
        "  ${AE_SDK_HEADERS}\n"
        "See third_party/README.md for SDK setup instructions.")
endif()

# ----- Plugin target -----
# AEGP panel plugin (not a PF effect). Matches the shape of
# Stardust_panel.aex / Element 3D's editor panel: registered via
# AEGP_PluginInitFuncPrototype, surfaces under AE's Window menu.
# Source files land in Bite 3.
add_library(ChaseMaker MODULE
    src/chase_maker.cpp
    "${AE_SDK_UTIL}/AEGP_SuiteHandler.cpp"
    "${AE_SDK_UTIL}/AEFX_SuiteHelper.c"
    "${AE_SDK_UTIL}/MissingSuiteError.cpp"
)

if(WIN32)
    target_sources(ChaseMaker PRIVATE src/panel_renderer_win.cpp)
elseif(APPLE)
    target_sources(ChaseMaker PRIVATE src/panel_renderer_mac.mm)
endif()

target_include_directories(ChaseMaker PRIVATE
    "${AE_SDK_HEADERS}"
    "${AE_SDK_HEADERS}/SP"
    "${AE_SDK_RESOURCES}"
    "${AE_SDK_UTIL}"
    src
)

target_link_libraries(ChaseMaker PRIVATE
    OpenEXR::OpenEXR
    Imath::Imath
    imgui::imgui
)

if(WIN32)
    target_include_directories(ChaseMaker PRIVATE "${AE_SDK_HEADERS}/Win")
    target_compile_definitions(ChaseMaker PRIVATE MSWindows WIN32 _WINDOWS)
    set_target_properties(ChaseMaker PROPERTIES
        SUFFIX ".aex"
        PREFIX ""
    )

    # DX11 + DXGI for the ImGui DirectX backend.
    target_link_libraries(ChaseMaker PRIVATE d3d11 dxgi d3dcompiler)

    # ----- PiPL build pipeline (Windows) -----
    # Apple Resource (.r) -> .rr -> PiPLtool -> .rrc -> Windows .rc
    set(PIPL_INPUT  "${CMAKE_SOURCE_DIR}/src/chase_maker.r")
    set(PIPL_RR     "${CMAKE_BINARY_DIR}/chase_maker.rr")
    set(PIPL_RRC    "${CMAKE_BINARY_DIR}/chase_maker.rrc")
    set(PIPL_RC     "${CMAKE_BINARY_DIR}/chase_maker.rc")
    set(PIPLTOOL    "${AE_SDK_RESOURCES}/PiPLtool.exe")

    if(NOT EXISTS "${PIPLTOOL}")
        message(FATAL_ERROR "PiPLtool.exe not found at ${PIPLTOOL}")
    endif()

    # Need cmd.exe for shell redirect; CMake's command runner doesn't redirect.
    add_custom_command(
        OUTPUT "${PIPL_RC}"
        DEPENDS "${PIPL_INPUT}"
        COMMAND cmd.exe /c "cl.exe /nologo /I \"${AE_SDK_HEADERS}\" /EP \"${PIPL_INPUT}\" > \"${PIPL_RR}\""
        COMMAND "${PIPLTOOL}" "${PIPL_RR}" "${PIPL_RRC}"
        COMMAND cmd.exe /c "cl.exe /nologo /D MSWindows /EP \"${PIPL_RRC}\" > \"${PIPL_RC}\""
        COMMENT "Compiling PiPL"
    )

    target_sources(ChaseMaker PRIVATE "${PIPL_RC}")

elseif(APPLE)
    target_include_directories(ChaseMaker PRIVATE "${AE_SDK_HEADERS}/Mac")
    target_compile_definitions(ChaseMaker PRIVATE macintosh)

    # AE_GeneralPlugPanels.h pulls in <Cocoa/Cocoa.h> on Mac, so
    # anything that includes it has to be compiled as Objective-C++.
    set_source_files_properties(src/chase_maker.cpp PROPERTIES
        LANGUAGE OBJCXX)

    # The Mac panel renderer uses ObjC ARC for tidy ownership of the
    # MTKView / device / delegate references stored in the C++ class.
    set_source_files_properties(src/panel_renderer_mac.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc")

    # Build a proper .plugin bundle (not a bare .dylib). AE looks for
    # CFBundlePackageType = "eFKT" inside the bundle's Info.plist.
    set_target_properties(ChaseMaker PROPERTIES
        BUNDLE                    TRUE
        BUNDLE_EXTENSION          "plugin"
        OUTPUT_NAME               "ChaseMaker"
        MACOSX_BUNDLE_INFO_PLIST  "${CMAKE_SOURCE_DIR}/cmake/MacInfoPlist.in"
        MACOSX_BUNDLE_BUNDLE_NAME            "ChaseMaker"
        MACOSX_BUNDLE_GUI_IDENTIFIER         "io.github.thedavidcarney.ChaseMaker"
        MACOSX_BUNDLE_BUNDLE_VERSION         "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING   "${PROJECT_VERSION}"
    )

    # Frameworks: AppKit (Cocoa) for any native UI bridging, CoreFoundation
    # for A_UTF16Char path conversion in AEGP file-picker callbacks.
    # Metal/MetalKit/QuartzCore for the ImGui Metal renderer.
    # GameController is a link-only dependency: imgui_impl_osx.mm imports
    # <GameController/GameController.h> at the top of the file for
    # optional gamepad input, and the reference exists in the .o even
    # though our code never calls into GCController. The framework is
    # lazy-loaded by the OS — no runtime cost until something actually
    # uses it, which we don't. No way to drop the link without patching
    # ImGui upstream.
    target_link_libraries(ChaseMaker PRIVATE
        "-framework Cocoa"
        "-framework CoreFoundation"
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework GameController"
    )

    # ----- PiPL build pipeline (macOS) -----
    # Same source .r file as Windows; compiled by Apple's Rez to a
    # .rsrc that ends up at Contents/Resources/ChaseMaker.rsrc inside
    # the bundle.
    set(PIPL_INPUT "${CMAKE_SOURCE_DIR}/src/chase_maker.r")
    set(PIPL_RSRC  "${CMAKE_BINARY_DIR}/ChaseMaker.rsrc")

    find_program(REZ_EXECUTABLE
        NAMES Rez
        PATHS /usr/bin /Applications/Xcode.app/Contents/Developer/usr/bin
        DOC "Apple Rez resource compiler (ships with Xcode CLT)")
    if(NOT REZ_EXECUTABLE)
        message(FATAL_ERROR
            "Rez not found. Install Xcode Command Line Tools (xcode-select --install).")
    endif()

    # Rez doesn't auto-define AE_OS_MAC, so pass it explicitly. -useDF
    # writes the resource map into the data fork; without it Rez uses
    # the legacy HFS resource fork, which file copies and the cmake
    # bundle install both strip.
    add_custom_command(
        OUTPUT "${PIPL_RSRC}"
        DEPENDS "${PIPL_INPUT}"
        COMMAND "${REZ_EXECUTABLE}"
                -useDF
                -d "AE_OS_MAC"
                -d "ARCH_64"
                -i "${AE_SDK_HEADERS}"
                -i "${AE_SDK_RESOURCES}"
                -o "${PIPL_RSRC}"
                "${PIPL_INPUT}"
        COMMENT "Compiling PiPL via Rez"
        VERBATIM)

    set_source_files_properties("${PIPL_RSRC}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
        GENERATED TRUE)
    target_sources(ChaseMaker PRIVATE "${PIPL_RSRC}")
endif()

# Quiet a few MSVC warnings the SDK headers trip but we can't fix.
if(MSVC)
    target_compile_options(ChaseMaker PRIVATE /W3)
endif()

# ----- Post-build install (dev iteration loop) -----
# Mirrors EXRDemux's deployed layout: Plug-ins/<PluginName>/<PluginName>.aex
# Pass an empty string (-DCHASEMAKER_AE_PLUGIN_DIR=) to disable; CI
# should always disable.
if(WIN32)
    set(CHASEMAKER_AE_PLUGIN_DIR
        "C:/Program Files/Adobe/Adobe After Effects 2025/Support Files/Plug-ins/ChaseMaker"
        CACHE PATH
        "AE plug-ins subdirectory to drop ChaseMaker.aex into after each build. Empty to disable.")

    if(CHASEMAKER_AE_PLUGIN_DIR)
        add_custom_command(TARGET ChaseMaker POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CHASEMAKER_AE_PLUGIN_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:ChaseMaker>"
                    "${CHASEMAKER_AE_PLUGIN_DIR}/"
            COMMENT "Installing ChaseMaker.aex to ${CHASEMAKER_AE_PLUGIN_DIR}"
            VERBATIM)
    endif()
endif()
