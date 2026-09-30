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
#include <vector>

struct PanelState;
struct LayerInfo;

namespace exr_scan {

// Why this pass is not a chase light, or empty if it is one.
// Cryptomattes, the beauty/alpha passes, and the environment passes
// (World / HDRI / Ambient) — matched EXACTLY, because pass names are
// arbitrary and a substring rule would silently drop real fixtures
// like "World_Light".
std::string SkipReason(const std::string& display);

// A softer signal for the setup screen: names that merely LOOK like
// scenery ("Ambient_Fill", "HDRI_Sky_001"). Returns a human-readable
// reason, or empty. Never applied automatically — it exists so the
// user can be shown a suggestion and confirm it in one click. Names
// already caught by SkipReason return empty (they are not suggestions,
// they are already excluded).
std::string EnvironmentHint(const std::string& display);

// Add a source by path, routing on file type. Video clips
// (.mov/.mp4/.mxf) become a single-light "movie" source: a placeholder
// Source is created immediately (so it shows in the Sources tab) and
// queued for AE-render analysis (the OpenEXR scanner can't read video).
// Everything else falls through to StartScan (the EXR/sequence path).
// This is the ONE chokepoint for drag-in, the file picker, and session
// restore so extension routing lives in a single place. `append`,
// `source_id`, and `frame_index` carry the same meaning as StartScan.
void AddSourcePath(const std::string& path, PanelState* state,
                   bool append = true, uint32_t source_id = 0,
                   int frame_index = 0);

// Compute centroid metrics + thumbnail from ONE already-decoded frame
// (linear R/G/B planes, w*h floats each) and write them into `out`
// (cx/cy/cx_hot/cy_hot/peak_*/total and thumb_rgba/thumb_w/thumb_h/
// thumb_peak; texture_id is left as-is so the renderer re-uploads).
// Shares the exact metric + thumbnail math the EXR scan uses; the movie
// path feeds it pixels from an AE-rendered frame instead of OpenEXR.
// `thumb_max_w` mirrors PanelState::thumb_max_width.
void AnalyzeFramePixels(const std::vector<float>& r,
                        const std::vector<float>& g,
                        const std::vector<float>& b,
                        int w, int h, int thumb_max_w,
                        LayerInfo& out);

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
// tags / chases keep resolving. If a source with that id already
// exists it is replaced in place (used by the frame re-scan), so the
// row keeps its position and active selection.
//
// `path` may be a concrete .exr, a directory of frames, or a
// sequence-token path (e.g. name_[0001-0060].exr / name_####.exr /
// name_%04d.exr). It is resolved to a single frame to scan;
// `frame_index` (0-based, clamped) selects which frame. The scanner
// only needs one representative frame — light positions don't move
// across the chase, the chase is the animation layered on top.
void StartScan(const std::string& path, PanelState* state,
               bool append = true, uint32_t source_id = 0,
               int frame_index = 0);

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
