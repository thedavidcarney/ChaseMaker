// ae_build — the dumb-comps comp builder. Reads chase data from
// PanelState and emits a new AE comp per chase via AEGP suites.
//
// "Dumb" because it doesn't try to reuse EXRDemux's existing precomp
// flow (SplitAndSortPassesToPrecomps.jsx). For each chase stage member
// we create a fresh footage layer pointing at the source EXR, apply
// EXRDemux to it, and set Layer Hash Hi/Lo to FNV-1a-32 of the layer's
// display name. Opacity + Exposure-Gamma keyframes per the chase's
// envelope are the next iteration.
//
// All AEGP work runs synchronously inside the renderer's deferred-
// action drain (so we know we're outside the ImGui frame). The whole
// build is wrapped in a single AEGP undo group; one Ctrl+Z in AE
// rolls back everything.

#pragma once

#include <string>

struct PanelState;

namespace ae_build {

struct BuildResult {
    bool        success = false;
    std::string message;        // status text — gets stashed on state.last_build_status
    int         comps_created = 0;
    int         layers_added = 0;
    // True if the comps built but "tdcarney EXRDemux" wasn't installed
    // — the layers have no hash-driven selection and render wrong
    // until EXRDemux is installed (drives a prominent UI warning).
    bool        exrdemux_missing = false;
};

// Build a single chase. `chase_index` is into state->chases.
// Reads chase.stages, chase.timing, chase.name. Active source's
// path supplies the EXR footage (auto-imported if not in project).
BuildResult BuildChase(PanelState* state, int chase_index);

// Build every chase in state->chases. Bind precomps are shared
// across chases (TODO — for now binds expand as flat layer lists).
BuildResult BuildAllChases(PanelState* state);

// Poll the active AE comp's frame rate and publish it to
// state->chase_preview_fps so the chase preview's timing matches the
// project. Cheap; safe to call every idle tick. No-op without an
// AEGP context. Must run from a registered-hook context (idle hook).
void RefreshProjectFps(PanelState* state);

} // namespace ae_build
