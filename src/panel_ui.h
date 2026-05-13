// Cross-platform Dear ImGui rendering for the Chase Maker panel.
//
// Both panel_renderer_win.cpp and panel_renderer_mac.mm forward their
// per-frame callback into this single entry point, so the panel
// content stays uniform across Win/Mac. ImGui context setup is the
// platform side's job; this just emits draw commands.

#pragma once

struct PanelState;

namespace panel_ui {

// Emit one frame of ImGui draw commands. Width/height are the panel's
// logical (point-space) size. `host_view` is the platform's container
// pointer (HWND or NSView*), forwarded to the file dialog as parent.
void RenderFrame(PanelState* state, float w, float h, void* host_view,
                 const char* backend_label);

} // namespace panel_ui
