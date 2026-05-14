// Cross-platform abstract handle for the per-panel ImGui renderer.
//
// `container` is AE's AEGP_PlatformViewRef as a void* — HWND on
// Windows, NSView* on macOS. We keep it untyped here so this header
// stays free of <windows.h> / <Cocoa/Cocoa.h>; the platform-specific
// .cpp/.mm files do the cast.
//
// Lifetime: one PanelRenderer per AEGP panel-create event. The
// AEGP_PanelFunctions1 table has no destroy callback, so a renderer
// effectively lives for the AE session — but AE may create a fresh
// platform view (and call CreatePanelHook again) if the user closes
// the panel via its X button. To survive that, the data the user
// has built up (scan results, exclusion choices, preview state) is
// owned by the global ChaseMakerPlugin via a PanelState* the
// renderer borrows, NOT by the renderer itself.

#pragma once

struct PanelState;

class PanelRenderer
{
public:
    virtual ~PanelRenderer() = default;

protected:
    PanelRenderer() = default;
    PanelRenderer(const PanelRenderer&) = delete;
    PanelRenderer& operator=(const PanelRenderer&) = delete;
};

PanelRenderer* CreatePanelRenderer(void* container, PanelState* state);
