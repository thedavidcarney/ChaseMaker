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

// Spawn a detached worker that scans `path`.
//
// `append=false` (legacy): clears any existing sources and replaces
// with the scan result. Used by the original single-EXR workflow.
//
// `append=true` (default since multi-source landed): appends a new
// source to `state.sources`, leaving existing sources intact. Used
// by the Sources-tab "Add Source..." button and by session restore.
//
// `source_id=0` (default): the new Source gets `state.next_source_id`
// assigned and that counter advances. When loading a saved session,
// pass the saved source_id explicitly so LayerRefs in saved binds /
// tags / chases keep resolving.
void StartScan(const std::string& path, PanelState* state,
               bool append = true, uint32_t source_id = 0);

// Write `<path>.luminosity.json` next to the EXR, matching the
// sidecar contract documented in CLAUDE.md. Returns true on success.
// Sets state.last_error / state.sidecar_written.
bool WriteLuminositySidecar(PanelState* state);

// Force-include a layer that the auto-skip heuristic dropped (e.g.
// a light named "Crypto_Bounce" that got false-matched as a
// cryptomatte). Re-reads only this one layer from disk, computes
// metrics + thumbnail, appends to the source's layers, removes from
// skipped. Sync; tens of milliseconds for a single layer.
//
// Returns false on failure (missing source, layer not RGB-complete,
// read error). On success: state.last_status updated. The skip
// reason no longer applies to this layer for the rest of the session;
// a re-scan via Add Source will re-skip it (this override is in-
// memory only, not yet persisted to the session JSON).
bool IncludeSkippedLayer(PanelState* state, uint32_t source_id,
                         const std::string& display_name);

} // namespace exr_scan
