// Shared panel state: the EXR pick + scan results that the renderer
// reads each frame and the file-dialog + scan paths mutate.
//
// Owned by the PanelRenderer (one instance per AE panel). The scan
// runs on a worker thread; access to the result fields is guarded by
// `mu`. `scanning` is atomic so the UI can show a spinner without
// taking the lock.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct LayerInfo {
    std::string display_name;     // e.g. "Curtain Column_001"
    uint32_t    fnv1a_hash = 0;   // matches EXRDemux's hash byte-for-byte
    float       cx = 0.f;         // luminance centroid, normalized [0,1]
    float       cy = 0.f;
    double      total = 0.0;      // sum of luminance over the layer
    bool        included = true;  // false = excluded from main chase order
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
};
