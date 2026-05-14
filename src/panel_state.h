// Shared panel state: the EXR pick + scan results that the renderer
// reads each frame and the file-dialog + scan paths mutate.
//
// Owned by the PanelRenderer (one instance per AE panel). The scan
// runs on a worker thread; access to the result fields is guarded by
// `mu`. `scanning` is atomic so the UI can show a spinner without
// taking the lock.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>
#include <vector>

struct LayerInfo {
    std::string display_name;     // e.g. "Curtain Column_001"
    uint32_t    fnv1a_hash = 0;   // matches EXRDemux's hash byte-for-byte
    // ---- Centroid family (all normalized [0,1]) ----
    // cx/cy: luminance-weighted centroid over the WHOLE layer. Pulls
    //   toward broad-spread contributions; good for "where does this
    //   light's energy concentrate overall."
    // cx_hot/cy_hot: centroid restricted to pixels above 50% of the
    //   layer's peak luminance. Snaps to the bright hotspot and
    //   ignores soft bounce / spill.
    // peak_x/peak_y: position of the single brightest pixel.
    //   Sharpest but most sensitive to noise.
    float       cx = 0.f;
    float       cy = 0.f;
    float       cx_hot = 0.f;
    float       cy_hot = 0.f;
    float       peak_x = 0.f;
    float       peak_y = 0.f;
    float       peak_lum = 0.f;   // value at the brightest pixel
    double      total = 0.0;      // sum of luminance over the layer
    bool        included = true;  // false = excluded from main chase order

    // Per-layer thumbnail used for the preview animation. RGBA8,
    // generated during scan from the full-res EXR pixels then
    // dropped on a worker thread. The platform renderer picks it up
    // each frame and lazily uploads it to GPU; `texture_id` is set
    // to a non-zero ImTextureID (uint64) once that happens.
    std::vector<uint8_t> thumb_rgba;
    int         thumb_w = 0;
    int         thumb_h = 0;
    uint64_t    texture_id = 0;
};

struct SkippedLayer {
    std::string display_name;
    std::string reason;
};

struct PanelState {
    std::mutex                 mu;          // guards everything below
    std::string                exr_path;
    int                        image_width = 0;
    int                        image_height = 0;
    std::vector<LayerInfo>     layers;
    std::vector<SkippedLayer>  skipped;
    std::string                last_error;
    std::string                last_status;  // human-readable progress msg
    std::atomic<bool>          scanning{false};
    std::atomic<bool>          sidecar_written{false};

    // Deferred-action flags: set by the UI during a frame, consumed
    // by the platform renderer AFTER ImGui::Render() returns and the
    // swap chain has been presented. This avoids spinning the OS file
    // dialog (which pumps its own message loop) while we're still in
    // the middle of an ImGui frame — that triggered reentrant
    // RenderFrame calls and a crash on Windows.
    std::atomic<bool>          want_pick_exr{false};
    std::atomic<bool>          want_write_sidecar{false};

    // ---- Preview animation (cycles through included layers) ----
    // Touched only from the UI thread; no synchronisation needed.
    bool   preview_playing  = false;
    int    preview_index    = 0;     // index into the included-only sequence
    float  preview_ms_step  = 250.f; // dwell per layer in ms
    float  preview_accum_ms = 0.f;   // accumulator across frames

    // Bulk-exclude filter buffer (ImGui input). Empty = no filter.
    char   exclude_filter[128] = {};

    // User-selected row, tracked by FNV hash so it survives re-sort
    // (the array index would otherwise point at a different layer
    // after the sort mode changes). 0 = nothing selected.
    uint32_t selected_hash = 0;

    // Ordering algorithm. Applied to the layers vector each time it
    // changes via panel_ui (cheap — all per-layer metrics are cached
    // on LayerInfo during scan, so no re-scan needed). New scans
    // apply the current mode automatically.
    enum class SortMode : int {
        CentroidX = 0,    // left to right by whole-layer luminance centroid
        CentroidY,        // top to bottom by whole-layer luminance centroid
        HotspotX,         // left to right by hotspot-only centroid
        HotspotY,         // top to bottom by hotspot-only centroid
        Brightness,       // brightest first by total luminance
        RadialSweep,      // atan2 angle from image center (clockwise sweep)
        DistanceFromCenter, // distance from (0.5, 0.5), nearest first
        Random,           // deterministic shuffle (seed in random_seed)
    };
    SortMode sort_mode = SortMode::CentroidX;

    // Reverses whatever sort is active. Universal modifier so the
    // user can flip direction without our needing two enum variants
    // per axis (Left-to-Right + Right-to-Left etc.).
    bool     sort_reverse = false;

    // Seed for the Random sort mode. Rerolled each time the user
    // (a) switches into Random mode, or (b) clicks the Re-shuffle
    // button — so within a session Random is stable, but the user
    // can ask for variety.
    uint32_t random_seed = 0;

    // Thumbnail generation max width (px). The actual thumbnail
    // height scales to preserve aspect ratio. UI-thread only.
    int    thumb_max_width = 384;

    // Bumped at the start of every scan. The platform renderer
    // tracks the last generation it saw and tears down its texture
    // cache when this advances — guarantees we don't keep GPU
    // resources around for a previous EXR's layers.
    std::atomic<int> scan_generation{0};
};

// In-place sort. Layer fields are all pre-computed on scan, so any
// sort mode is a pure comparator change — no re-scanning needed.
// `reverse` flips the result. `seed` only matters for Random.
inline void SortLayers(std::vector<LayerInfo>& layers,
                       PanelState::SortMode mode,
                       bool reverse = false,
                       uint32_t seed = 0)
{
    auto angle = [](const LayerInfo& L) {
        // atan2 of position relative to image center. Range -pi..pi.
        return std::atan2(L.cy - 0.5f, L.cx - 0.5f);
    };
    auto dist2 = [](const LayerInfo& L) {
        const float dx = L.cx - 0.5f, dy = L.cy - 0.5f;
        return dx * dx + dy * dy;
    };
    auto less_than = [&](const LayerInfo& a, const LayerInfo& b) {
        switch (mode) {
        case PanelState::SortMode::CentroidX:        return a.cx     < b.cx;
        case PanelState::SortMode::CentroidY:        return a.cy     < b.cy;
        case PanelState::SortMode::HotspotX:         return a.cx_hot < b.cx_hot;
        case PanelState::SortMode::HotspotY:         return a.cy_hot < b.cy_hot;
        case PanelState::SortMode::Brightness:       return a.total  > b.total; // desc
        case PanelState::SortMode::RadialSweep:      return angle(a) < angle(b);
        case PanelState::SortMode::DistanceFromCenter:
                                                     return dist2(a) < dist2(b);
        case PanelState::SortMode::Random:
            // Stable per-seed: hash each layer's name+seed into a
            // pseudo-random key, sort by that key. Deterministic for
            // a given (layers, seed), so switching modes back and
            // forth doesn't re-shuffle until the user re-seeds.
            {
                uint32_t ka = a.fnv1a_hash * 2654435761u + seed;
                uint32_t kb = b.fnv1a_hash * 2654435761u + seed;
                return ka < kb;
            }
        }
        return false;
    };
    std::sort(layers.begin(), layers.end(), less_than);
    if (reverse) std::reverse(layers.begin(), layers.end());
}

// Display position helpers — UI shows the spatial coordinate the
// active sort key is keyed off of (so the dot on the canvas + the
// position bar in the table track which sort is active). For non-
// spatial modes (Brightness, Random) we fall back to the whole-layer
// centroid so the canvas still meaningfully shows where lights are
// in image space.
inline float LayerDisplayX(const LayerInfo& L, PanelState::SortMode m)
{
    switch (m) {
    case PanelState::SortMode::HotspotX:
    case PanelState::SortMode::HotspotY: return L.cx_hot;
    default:                             return L.cx;
    }
}
inline float LayerDisplayY(const LayerInfo& L, PanelState::SortMode m)
{
    switch (m) {
    case PanelState::SortMode::HotspotX:
    case PanelState::SortMode::HotspotY: return L.cy_hot;
    default:                             return L.cy;
    }
}
