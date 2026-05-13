// EXR scanner: enumerate every lightgroup-style layer in a multilayer
// (or multipart) EXR and compute its Rec.709 luminance centroid.
//
// Mirrors the Python reference in scripts/dev/luminance_centroid.py:
// - Skip cryptomatte layers (hash-encoded, no luminance signal)
// - Skip Image / Alpha (beauty pass, biased everywhere)
// - Skip layers missing a full R+G+B set
// - Skip all-black layers (no light contribution this frame)
// - Apply the "X.X" -> "X" display-name dedup that EXRDemux uses
//
// Mutates a PanelState in-place. Caller is responsible for ensuring
// the state outlives the scan; the implementation runs on a worker
// thread and grabs `state.mu` only when publishing results.

#pragma once

#include <string>

struct PanelState;

namespace exr_scan {

// Spawn a detached worker that scans `path`. Sets state.scanning
// while running; clears it on completion. On failure, state.last_error
// holds the diagnostic and state.layers is empty.
void StartScan(const std::string& path, PanelState* state);

// Write `<path>.luminosity.json` next to the EXR, matching the
// sidecar contract documented in CLAUDE.md. Returns true on success.
// Sets state.last_error / state.sidecar_written.
bool WriteLuminositySidecar(PanelState* state);

} // namespace exr_scan
