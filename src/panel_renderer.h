// Cross-platform abstract handle for the per-panel ImGui renderer.
//
// `container` is AE's AEGP_PlatformViewRef as a void* — HWND on
// Windows, NSView* on macOS. We keep it untyped here so this header
// stays free of <windows.h> / <Cocoa/Cocoa.h>; the platform-specific
// .cpp/.mm files do the cast.
//
// Lifetime: one PanelRenderer per AEGP_PanelH AE creates. The
// AEGP_PanelFunctions1 table has no destroy callback, so a renderer
// effectively lives for the AE session once instantiated.

#pragma once

class PanelRenderer
{
public:
    virtual ~PanelRenderer() = default;

protected:
    PanelRenderer() = default;
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;
};

PanelRenderer* CreatePanelRenderer(void* container);
