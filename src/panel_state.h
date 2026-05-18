// Shared panel state: the source files loaded into the session and
// the scan results layered on them. The renderer reads this each
// frame; the file-dialog + scan paths mutate it.
//
// Owned by the PanelRenderer (one instance per AE panel). The scan
// runs on a worker thread; access to anything inside `sources` (and
// the session-level fields below) is guarded by `mu`. Atomics like
// `scanning` can be read without the lock.
//
// As of the multi-source refactor: per-file data (path, image size,
// scanned layers, skipped layers) lives on a `Source`. PanelState
// holds a vector of them and an `active_source_index` for the
// source the UI is currently focused on. Single-source workflows
// keep `sources.size() <= 1`; multi-source loading lands in a later
// step but the data model is already in place.

#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
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

    // Linear peak the thumbnail was normalized against (the layer's
    // own brightest channel value). thumb_rgba encodes
    // sqrt(linear / thumb_peak), so the preview compositor can recover
    // a relative-truthful linear value and renormalize against a
    // shared scene peak (per-layer normalization keeps weak lights
    // visible in the layer table, but lies about relative brightness
    // — the composite preview undoes it). 0 = unknown (treat as 1).
    float       thumb_peak = 0.f;
};

struct SkippedLayer {
    std::string display_name;
    std::string reason;
};

// One source file (EXR or PNG) loaded into the session. Each source
// contributes its layers to the staged-set pool that chases pick from.
// Stable across re-scans: `source_id` survives a save/reload so chase
// references by (source_id, layer_hash) remain valid.
struct Source {
    uint32_t                    source_id = 0;     // assigned per session
    std::string                 path;              // original drag/pick path
                                                   // (file, dir, or seq token)
    std::string                 scan_path;         // concrete .exr frame opened
    int                         scan_frame = 0;    // 0-based frame scanned
    int                         frame_count = 1;   // frames in the sequence
    // Treat this source as an animation: chases built from it loop
    // seamlessly over the source's own duration (frame_count frames),
    // footage time-locked (every layer plays the same frames at the
    // same comp time), envelope sweeps. Auto-set true at scan when
    // frame_count > 1; user-overridable in the Sources tab for the
    // odd case where a multi-frame source should still be treated as
    // shift-in-time stills.
    bool                        animation = false;
    int                         image_width = 0;
    int                         image_height = 0;
    std::vector<LayerInfo>      layers;
    std::vector<SkippedLayer>   skipped;
};

// Lifted out of PanelState so chase + wizard structs can reference it
// before PanelState is complete. PanelState still re-exposes it as
// `PanelState::SortMode` via a `using` alias so legacy call sites
// (panel_ui, exr_scan) keep working unchanged.
enum class SortMode : int {
    CentroidX = 0,        // left to right by whole-layer luminance centroid
    CentroidY,            // top to bottom by whole-layer luminance centroid
    HotspotX,             // left to right by hotspot-only centroid
    HotspotY,             // top to bottom by hotspot-only centroid
    Brightness,           // brightest first by total luminance
    RadialSweep,          // atan2 angle from image center (clockwise sweep)
    DistanceFromCenter,   // distance from (0.5, 0.5), nearest first
    Random,               // deterministic shuffle (seed in random_seed)
    Alphabetical,         // display_name A → Z (lexicographic on raw bytes)
    IncludedFirst,        // ticked rows first, then unticked
    TagName,              // alphabetical by first tag's name, untagged last
    EffectiveX,           // by override-aware X
    EffectiveY,           // by override-aware Y
};

// Stable reference to a layer across a session: (source_id, layer
// hash). Survives source re-scans (the hash is keyed off display
// name, not array position). Used everywhere binds / tags / chases
// need to talk about specific layers.
struct LayerRef {
    uint32_t source_id = 0;
    uint32_t fnv1a_hash = 0;

    bool operator==(const LayerRef& o) const {
        return source_id == o.source_id && fnv1a_hash == o.fnv1a_hash;
    }
};

// Two or more layers welded into one logical light. Render-time, a
// bind becomes a pre-comp; the chase comp treats the bind as a
// single layer. Authored in the Staging tab via right-click on
// multiple selected rows.
struct Bind {
    uint32_t                bind_id = 0;
    std::string             name;              // user-editable
    uint32_t                color = 0xFF80A0FFu;  // ImU32 ABGR for row stripe
    std::vector<LayerRef>   members;
};

// Organizational label. A layer can belong to many tags. Used as a
// chase's "filter by tag" input and as a UI grouping affordance.
struct Tag {
    uint32_t                tag_id = 0;
    std::string             name;
    uint32_t                color = 0xFF80FFA0u;  // ImU32 ABGR for pill
    std::vector<LayerRef>   members;
};

// User-asserted centroid coordinate for a layer. Affects sort and
// canvas display only — the actual rendered pixels are untouched.
// Optional; absence means use the scanned centroid (and Z = 0).
//
// Z is future-proofing for a 3D-aware chase builder; for now it's
// always 0 and only the position override popup exposes it. The
// session JSON persists the value so a future build that uses Z
// reads back what the user authored.
struct PositionOverride {
    LayerRef                layer;
    float                   cx = 0.f;          // normalized [0,1]
    float                   cy = 0.f;
    float                   cz = 0.f;          // future-proof; 0 by default
};

// One step of a chase: a set of lights that fire together. Most
// chases have exactly one layer per stage; "3 Step Chase" and
// similar group multiple layers per stage.
struct ChaseStage {
    std::vector<LayerRef>   members;
};

// One scattered hit in a Random-scatter chase: a single light fires
// at `start_frame` within the loop, with the chase's hit envelope.
// Generated deterministically from (seed, density, loop length) by
// RegenerateScatter; never hand-authored. Bind members share a hit
// time (one ScatterHit per member, same start_frame).
struct ScatterHit {
    LayerRef                ref;
    float                   start_frame = 0.f;   // [0, loop_frames)
};

// Per-chase envelope shape. Maps onto the NWE Light Hit presets but
// is fully parameterised. Hit duration + step duration together
// imply overlap (if step < duration, hits overlap).
struct ChaseTiming {
    float duration       = 30.0f;  // frames; total envelope length per stage
    float attack         = 5.0f;   // frames from start to peak; release = duration - attack
    float step_duration  = 4.0f;   // frames between successive stage starts
    float opacity_peak   = 100.f;  // % at peak
    float gamma_peak     = 1.0f;   // Exposure 'Gamma Correction' at peak
    float gamma_baseline = 0.25f;  // Gamma Correction off-peak baseline
};

// One chase = one AE comp to be built. Tabs in the panel correspond
// 1:1 with chases.
struct Chase {
    uint32_t                chase_id = 0;
    std::string             name;
    // Hotspot X is the chase default: it orders by the bright
    // concentrated source (ignoring spill/bounce), which reads as the
    // truer left→right light order than the spill-influenced
    // whole-layer centroid.
    SortMode                sort_mode = SortMode::HotspotX;
    bool                    sort_reverse = false;
    uint32_t                random_seed = 0;
    // Filter by tag IDs. Empty = all staged layers are eligible.
    std::vector<uint32_t>   tag_filter;
    // 0 = "one light per stage" (the common case). >0 = collapse all
    // eligible lights into this many stages, grouped contiguously so
    // each stage fires N/desired_stage_count lights together.
    // Used by the "3 Step Chase" template (=3) and the user-facing
    // "Stages" slider in the chase editor.
    int                     desired_stage_count = 0;
    // Symmetric center-out staging. When true, RegenerateChaseStages
    // ignores desired_stage_count and instead orders the eligible
    // lights by sort_mode, then builds stages from the middle
    // outward: the center light(s) fire first, then each mirrored
    // pair welds into one stage (lights 1..5 -> [3],[2,4],[1,5]).
    // Set by the "Center Out" template.
    bool                    symmetric_pairs = false;
    // When true the user has hand-authored `stages` (drag-reorder,
    // weld, split in the chase editor). RegenerateChaseStages then
    // leaves `stages` untouched so the arrangement survives the
    // per-frame refresh and session save/load. Cleared by the
    // editor's "Reset to auto" button.
    bool                    manual_stages = false;
    // Cached stage list, regenerated on the fly from (sort_mode,
    // sort_reverse, random_seed, tag_filter, desired_stage_count,
    // staged inclusion) unless manual_stages is set, in which case
    // it is authored by the user. The chase editor auto-refreshes
    // this each frame so it never drifts from current state.
    std::vector<ChaseStage> stages;
    ChaseTiming             timing;

    // ---- Random-scatter mode (the "Random" template) ----
    // A fundamentally different generator from the stage sequence
    // above: every eligible light gets `scatter_density` copies
    // spread across a seamless loop of `loop_seconds`, one per
    // stratified time bucket so a light never stacks on itself and
    // coverage stays even (not clumpy/sparse). Deterministic from
    // `random_seed`. When true, sort_mode / desired_stage_count /
    // symmetric_pairs / manual_stages / stages are all ignored;
    // `scatter` is the cached output (regenerated each frame).
    bool                    random_scatter = false;
    float                   loop_seconds   = 10.0f;
    int                     scatter_density = 5;
    std::vector<ScatterHit> scatter;
};

// The big-three top-level tabs. Chase tabs live in their own vector
// alongside the chases themselves; this enum picks among the
// pre-defined tabs.
enum class PanelTab : int {
    Sources = 0,
    Staging,
    Chase,        // active_chase_index selects which chase
};

// ===== Equality operators (used by the undo system) ===================

inline bool operator==(const Bind& a, const Bind& b) {
    return a.bind_id == b.bind_id && a.name == b.name &&
           a.color == b.color && a.members == b.members;
}
inline bool operator==(const Tag& a, const Tag& b) {
    return a.tag_id == b.tag_id && a.name == b.name &&
           a.color == b.color && a.members == b.members;
}
inline bool operator==(const PositionOverride& a, const PositionOverride& b) {
    return a.layer == b.layer && a.cx == b.cx && a.cy == b.cy && a.cz == b.cz;
}
inline bool operator==(const ChaseTiming& a, const ChaseTiming& b) {
    return a.duration == b.duration && a.attack == b.attack &&
           a.step_duration == b.step_duration &&
           a.opacity_peak == b.opacity_peak &&
           a.gamma_peak == b.gamma_peak &&
           a.gamma_baseline == b.gamma_baseline;
}
inline bool operator==(const ChaseStage& a, const ChaseStage& b) {
    return a.members == b.members;
}
inline bool operator==(const ScatterHit& a, const ScatterHit& b) {
    return a.ref == b.ref && a.start_frame == b.start_frame;
}
inline bool operator==(const Chase& a, const Chase& b) {
    return a.chase_id == b.chase_id && a.name == b.name &&
           a.sort_mode == b.sort_mode && a.sort_reverse == b.sort_reverse &&
           a.random_seed == b.random_seed &&
           a.desired_stage_count == b.desired_stage_count &&
           a.symmetric_pairs == b.symmetric_pairs &&
           a.manual_stages == b.manual_stages &&
           a.tag_filter == b.tag_filter &&
           a.stages == b.stages && a.timing == b.timing &&
           a.random_scatter == b.random_scatter &&
           a.loop_seconds == b.loop_seconds &&
           a.scatter_density == b.scatter_density &&
           a.scatter == b.scatter;
}

// ===== Undo snapshot ==================================================
//
// Captures all authoring state — anything the user can change with a
// click, drag, type, or context-menu action. Excludes ephemeral UI
// state (preview animation playhead, drag-armed state, wizard form
// scratch, thumbnail texture IDs) and expensive derived data
// (thumb_rgba — regenerated by scan, never edited).
//
// On undo we re-apply this snapshot to the current state. Sources are
// matched by source_id (so we can survive having lost a source's
// scanned LayerInfo and just preserve included/order); thumbs and
// centroid metrics stay attached to the still-existing LayerInfo
// objects in PanelState.sources.

struct LayerInclusionSnap {
    std::string display_name;   // identity within the source
    bool        included;
};
inline bool operator==(const LayerInclusionSnap& a, const LayerInclusionSnap& b) {
    return a.display_name == b.display_name && a.included == b.included;
}

struct SourceSnap {
    uint32_t                          source_id;
    std::vector<LayerInclusionSnap>   layers;  // in display order
};
inline bool operator==(const SourceSnap& a, const SourceSnap& b) {
    return a.source_id == b.source_id && a.layers == b.layers;
}

struct UndoSnapshot {
    std::vector<SourceSnap>           sources;
    std::vector<Bind>                 binds;
    std::vector<Tag>                  tags;
    std::vector<PositionOverride>     position_overrides;
    std::vector<Chase>                chases;
    SortMode                          sort_mode = SortMode::CentroidX;
    bool                              sort_reverse = false;
    uint32_t                          random_seed = 0;
    int                               active_source_index = -1;
    PanelTab                          active_tab = PanelTab::Sources;
    int                               active_chase_index = -1;
    bool                              chase_in_wizard = false;
    ChaseTiming                       default_timing;
    bool                              hide_unchecked = false;
    uint32_t                          next_source_id = 1;
    uint32_t                          next_bind_id = 1;
    uint32_t                          next_tag_id = 1;
    uint32_t                          next_chase_id = 1;
};
inline bool operator==(const UndoSnapshot& a, const UndoSnapshot& b) {
    return a.sources == b.sources && a.binds == b.binds &&
           a.tags == b.tags && a.position_overrides == b.position_overrides &&
           a.chases == b.chases && a.sort_mode == b.sort_mode &&
           a.sort_reverse == b.sort_reverse && a.random_seed == b.random_seed &&
           a.active_source_index == b.active_source_index &&
           a.active_tab == b.active_tab &&
           a.active_chase_index == b.active_chase_index &&
           a.chase_in_wizard == b.chase_in_wizard &&
           a.default_timing == b.default_timing &&
           a.hide_unchecked == b.hide_unchecked &&
           a.next_source_id == b.next_source_id &&
           a.next_bind_id == b.next_bind_id &&
           a.next_tag_id == b.next_tag_id &&
           a.next_chase_id == b.next_chase_id;
}

// Cap on undo + redo stack sizes — combined memory worst case is
// ~kMaxUndoStack * snapshot_size, typically a couple of MB.
inline constexpr size_t kMaxUndoStack = 100;

struct PanelState {
    std::mutex                 mu;          // guards everything below

    // ---- Loaded source files ----
    // Currently always 0 or 1 entries; the multi-source loader lands
    // in a later step. `active_source_index` is -1 when no source is
    // loaded; otherwise it indexes into `sources`.
    std::vector<Source>        sources;
    int                        active_source_index = -1;
    // Monotonic counter; assigned to each new Source on creation so
    // chase references survive saves/reloads. Starts at 1 so 0 can
    // mean "unset".
    uint32_t                   next_source_id = 1;

    std::string                last_error;
    std::string                last_status;  // human-readable progress msg
    std::atomic<bool>          scanning{false};

    // Deferred-action flags: set by the UI during a frame, consumed
    // by the platform renderer AFTER ImGui::Render() returns and the
    // swap chain has been presented. This avoids spinning the OS file
    // dialog (which pumps its own message loop) while we're still in
    // the middle of an ImGui frame — that triggered reentrant
    // RenderFrame calls and a crash on Windows.
    std::atomic<bool>          want_pick_exr{false};

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

    // SortMode lives at file scope (chases/wizard reference it
    // before PanelState is complete); aliased here so call sites
    // can still write `PanelState::SortMode::CentroidX`.
    using SortMode = ::SortMode;
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

    // ---- Session-level data (binds, tags, overrides, chases) ----
    std::vector<Bind>             binds;
    std::vector<Tag>              tags;
    std::vector<PositionOverride> position_overrides;
    // User-baked "preferred starting order" — drag-to-reorder in
    // the Staging tab writes here. Empty = use the active sort.
    // Each LayerRef may live in any of the loaded sources.
    std::vector<LayerRef>         manual_order;

    std::vector<Chase>            chases;

    uint32_t                      next_bind_id  = 1;
    uint32_t                      next_tag_id   = 1;
    uint32_t                      next_chase_id = 1;

    // ---- Top-level tab state ----
    PanelTab                      active_tab = PanelTab::Sources;
    int                           active_chase_index = -1;  // when active_tab == Chase
    // Wizard mode: when true, the active chase tab shows the
    // create-chase form instead of the chase editor. Set by the
    // "+" button; cleared when the user picks a template.
    bool                          chase_in_wizard = false;
    // Set to a chase index when the user clicks a chase tab's close
    // (X) button; drives the top-level delete-confirmation modal.
    // -1 = no pending close. UI-thread only.
    int                           chase_pending_close_index = -1;

    // ---- Session file ----
    // Where the last load/save lives. Empty = unsaved.
    std::string                   session_save_path;
    std::atomic<bool>             want_save_session{false};
    std::atomic<bool>             want_load_session{false};

    // ---- Comp builder ("dumb comps") ----
    // The plugin's entry point stashes pica_basicP and aegp_plugin_id
    // here so the ae_build module can construct an AEGP_SuiteHandler
    // when draining a build request. void* / int to keep the AE SDK
    // headers out of panel_state.h's includes.
    void*                         pica_basicP = nullptr;     // SPBasicSuite*
    int                           aegp_plugin_id = 0;        // AEGP_PluginID
    // Deferred build triggers. UI sets these; renderer drains in
    // HandleDeferredActions. want_build_chase_index = -1 means idle.
    std::atomic<int>              want_build_chase_index{-1};
    std::atomic<bool>             want_build_all_chases{false};
    // Status feedback from the last build for the UI to display.
    std::string                   last_build_status;
    // Last build ran but EXRDemux wasn't installed — drives a
    // prominent, actionable UI warning (save session / install /
    // restart AE / reload + rebuild). Guarded by `mu` like the
    // status string above.
    bool                          last_build_exrdemux_missing = false;

    // ---- Wizard form state (reset each time wizard opens) ----
    int                           wizard_template_index = 0;   // index in template list
    // Selected tag IDs for the new chase's filter. Empty = all
    // staged layers (no filter). Multi-select with All/None in the
    // wizard; copied verbatim into Chase::tag_filter on create.
    std::vector<uint32_t>         wizard_tag_filter;
    float                         wizard_step_duration = 4.0f;
    float                         wizard_hit_duration = 30.0f;

    // Remembered defaults for new chases (last-used wins).
    ChaseTiming                   default_timing;

    // ---- Staging tab toggles ----
    bool                          hide_unchecked = false;

    // ---- Centroid-map drag-to-override state ----
    // While drag_layer_hash != 0, the user is holding a centroid dot
    // and moving it. drag_cursor_{x,y} are normalized 0..1 image
    // coords; mirror them in the preview-pane thumbnail. On mouse
    // release the values commit as a PositionOverride.
    uint32_t                      drag_layer_source_id = 0;
    uint32_t                      drag_layer_hash      = 0;
    float                         drag_cursor_x        = 0.f;
    float                         drag_cursor_y        = 0.f;
    // Hold-delay: pressing on a dot ARMS the drag. The drag only
    // becomes active after the user holds the button for >=
    // kCentroidDragHoldSeconds. Quick clicks stay select-only so
    // the user can pick a dot without dragging it accidentally.
    uint32_t                      drag_armed_source_id = 0;
    uint32_t                      drag_armed_layer_hash = 0;
    double                        drag_armed_press_time = 0.0;

    // ---- Multi-row selection ----
    // Shift+click extends range from last_selected_hash; Ctrl+click
    // toggles a hash in the set. selected_hash is the "focus" entry
    // (last clicked), used by single-row workflows (preview pane,
    // single-row context menu). selected_hashes is the full set
    // and drives bulk operations (bind selected, tag selected).
    std::vector<uint32_t>         selected_hashes;
    uint32_t                      last_selected_hash = 0;

    // ---- Undo / redo ----
    // The system snapshots authoring state each frame when no widget
    // is being actively interacted with (IsAnyItemActive == false),
    // compares to the last snapshot, and pushes the prior state onto
    // undo_stack when something changed. Ctrl+Z pops; Ctrl+Shift+Z
    // (or Ctrl+Y) redoes. last_stable + has_last_stable cache the
    // "current authoring state" between frames so we only do one
    // comparison per stable transition.
    std::vector<UndoSnapshot>     undo_stack;
    std::vector<UndoSnapshot>     redo_stack;
    UndoSnapshot                  last_stable;
    bool                          has_last_stable = false;

    // ---- Chase preview state ----
    // Live composite of active-stage layer thumbnails, modulated by
    // each stage's opacity + gamma envelope at the current playhead.
    // UI-thread only: the UI fills `composite_rgba`, sets `dirty`,
    // and the platform renderer next frame (re)uploads it as a
    // single texture and publishes the resulting id back here.
    bool                          chase_preview_playing = false;
    float                         chase_preview_frame   = 0.f;
    // Tracks the active AE comp's frame rate. Written from the AEGP
    // idle hook (ae_build::RefreshProjectFps) so the preview's
    // seconds readout + playback speed match the project instead of
    // a hardcoded value; read on the UI thread — hence atomic.
    std::atomic<float>            chase_preview_fps{24.f};
    // Click-to-pin uses the shared `selected_hash` (set by a chase
    // table row click or a centroid-dot click): when not playing and
    // that layer belongs to the chase, the preview freezes on its
    // whole stage. See the build-composite block in DrawChaseEditor.
    std::vector<uint8_t>          chase_composite_rgba;
    int                           chase_composite_w     = 0;
    int                           chase_composite_h     = 0;
    std::atomic<bool>             chase_composite_dirty{false};
    uint64_t                      chase_composite_texture_id = 0;
};

// How long the user must hold the mouse button on a centroid dot
// before drag begins. Quick clicks (release inside this window) are
// pure selections. Tuned to "I meant to grab" without making the
// drag feel sluggish to the user who knows they want it.
inline constexpr double kCentroidDragHoldSeconds = 1.0;

// Convenience accessors. Caller must hold `state.mu` (or be the
// scan worker that owns publication of the Source). Returns
// nullptr if no source is loaded.
inline Source* ActiveSource(PanelState& state)
{
    if (state.active_source_index < 0) return nullptr;
    if (state.active_source_index >= static_cast<int>(state.sources.size())) {
        return nullptr;
    }
    return &state.sources[state.active_source_index];
}

inline const Source* ActiveSource(const PanelState& state)
{
    if (state.active_source_index < 0) return nullptr;
    if (state.active_source_index >= static_cast<int>(state.sources.size())) {
        return nullptr;
    }
    return &state.sources[state.active_source_index];
}

// Look up a source by its persistent ID. Returns nullptr if no
// source has that ID (typically means the source was removed since
// the LayerRef was captured).
inline Source* FindSourceById(PanelState& state, uint32_t source_id)
{
    for (auto& s : state.sources) {
        if (s.source_id == source_id) return &s;
    }
    return nullptr;
}
inline const Source* FindSourceById(const PanelState& state, uint32_t source_id)
{
    for (const auto& s : state.sources) {
        if (s.source_id == source_id) return &s;
    }
    return nullptr;
}

// Resolve a LayerRef to its LayerInfo in the current state. Returns
// nullptr if the source is gone or the layer was renamed (which
// changes its FNV hash).
inline LayerInfo* FindLayerByRef(PanelState& state, const LayerRef& ref)
{
    Source* src = FindSourceById(state, ref.source_id);
    if (!src) return nullptr;
    for (auto& L : src->layers) {
        if (L.fnv1a_hash == ref.fnv1a_hash) return &L;
    }
    return nullptr;
}
inline const LayerInfo* FindLayerByRef(const PanelState& state, const LayerRef& ref)
{
    const Source* src = FindSourceById(state, ref.source_id);
    if (!src) return nullptr;
    for (const auto& L : src->layers) {
        if (L.fnv1a_hash == ref.fnv1a_hash) return &L;
    }
    return nullptr;
}

// True if `ref` is a member of `bind`. O(N) over members; binds are
// small so this is fine.
inline bool BindContains(const Bind& bind, const LayerRef& ref)
{
    for (const auto& m : bind.members) if (m == ref) return true;
    return false;
}

// Returns the bind a layer belongs to, or nullptr if free-standing.
// A layer can belong to at most one bind (binds partition lights).
inline const Bind* BindOfLayer(const PanelState& state, const LayerRef& ref)
{
    for (const auto& b : state.binds) {
        if (BindContains(b, ref)) return &b;
    }
    return nullptr;
}

// Returns the tags a layer belongs to. Each layer can be in many
// tags. Pointers are stable as long as the tag vector isn't mutated.
inline std::vector<const Tag*> TagsOfLayer(const PanelState& state, const LayerRef& ref)
{
    std::vector<const Tag*> out;
    for (const auto& t : state.tags) {
        for (const auto& m : t.members) {
            if (m == ref) { out.push_back(&t); break; }
        }
    }
    return out;
}

// Look up a tag by ID. nullptr if it was removed.
inline const Tag* FindTagById(const PanelState& state, uint32_t tag_id)
{
    for (const auto& t : state.tags) {
        if (t.tag_id == tag_id) return &t;
    }
    return nullptr;
}

// Active chase shortcut. Returns nullptr when no chase is selected.
inline Chase* ActiveChase(PanelState& state)
{
    if (state.active_chase_index < 0) return nullptr;
    if (state.active_chase_index >= static_cast<int>(state.chases.size())) {
        return nullptr;
    }
    return &state.chases[state.active_chase_index];
}
inline const Chase* ActiveChase(const PanelState& state)
{
    if (state.active_chase_index < 0) return nullptr;
    if (state.active_chase_index >= static_cast<int>(state.chases.size())) {
        return nullptr;
    }
    return &state.chases[state.active_chase_index];
}

// Loop length in frames for a Random-scatter chase at `fps`.
inline int ScatterLoopFrames(const Chase& chase, float fps)
{
    // No std::max/std::min in this header: <windows.h> (pulled in
    // before panel_state.h by some TUs) defines max/min macros.
    const float fps_eff = (fps < 1.f) ? 1.f : fps;
    const long f = std::lround(chase.loop_seconds * fps_eff);
    return static_cast<int>(f < 1 ? 1 : f);
}

// True when the active source is an animation/sequence — chases run
// as seamless loops over the source's own duration instead of the
// shift-in-time "from black" model used for stills.
inline bool ChaseLoopMode(const PanelState& state)
{
    const Source* src = ActiveSource(state);
    return src && src->animation && src->frame_count > 1;
}

// Seamless-loop length in frames for this chase. Animation source:
// EXACTLY the source duration (frame_count) — the show requirement,
// so the chase loop and the scene animation stay phase-locked.
// Otherwise the scatter's own loop_seconds*fps (still-image scatter).
inline int ChaseLoopFrames(const Chase& chase, const PanelState& state,
                           float fps)
{
    const Source* src = ActiveSource(state);
    if (src && src->animation && src->frame_count > 1)
        return src->frame_count;
    return ScatterLoopFrames(chase, fps);
}

// Regenerate a scatter chase's hit list deterministically from
// (random_seed, scatter_density, loop_seconds, fps, eligible set).
// Shared by the UI (per-frame, for the preview) AND the AE builder
// (so a Random comp is fully populated even when its tab was never
// opened / after a session load / via Build-all — otherwise the
// comp would be empty/black). Per light: one jittered hit per
// stratified time bucket + a per-light phase, so a light never
// self-stacks and coverage stays even. Binds share each hit time.
inline void RegenerateScatter(Chase& chase, const PanelState& state,
                              float fps)
{
    chase.scatter.clear();
    const Source* src = ActiveSource(state);
    if (!src) return;
    const int loop_frames = ChaseLoopFrames(chase, state, fps);
    const int density = (chase.scatter_density < 1) ? 1
                                                    : chase.scatter_density;

    struct Lead { LayerRef repr; std::vector<LayerRef> members; };
    std::vector<Lead> leads;
    std::vector<uint32_t> bind_seen;
    for (const auto& L : src->layers) {
        if (!L.included) continue;
        LayerRef ref{ src->source_id, L.fnv1a_hash };
        if (!chase.tag_filter.empty()) {
            bool in_any = false;
            for (uint32_t tid : chase.tag_filter) {
                if (const Tag* t = FindTagById(state, tid)) {
                    for (const auto& m : t->members)
                        if (m == ref) { in_any = true; break; }
                }
                if (in_any) break;
            }
            if (!in_any) continue;
        }
        if (const Bind* b = BindOfLayer(state, ref)) {
            bool seen = false;
            for (uint32_t id : bind_seen)
                if (id == b->bind_id) { seen = true; break; }
            if (!seen) {
                bind_seen.push_back(b->bind_id);
                leads.push_back({ ref, b->members });
            }
        } else {
            leads.push_back({ ref, { ref } });
        }
    }
    if (leads.empty()) return;

    const float bucket = static_cast<float>(loop_frames) /
                         static_cast<float>(density);
    for (const auto& lead : leads) {
        uint32_t s = chase.random_seed ^
            (lead.repr.fnv1a_hash * 2654435761u + 0x9E3779B9u);
        std::mt19937 rng(s ? s : 1u);
        std::uniform_real_distribution<float> u01(0.f, 1.f);
        const float phase = u01(rng) * static_cast<float>(loop_frames);
        for (int k = 0; k < density; ++k) {
            float t = bucket * (static_cast<float>(k) + u01(rng)) + phase;
            t = std::fmod(t, static_cast<float>(loop_frames));
            if (t < 0.f) t += static_cast<float>(loop_frames);
            for (const auto& m : lead.members)
                chase.scatter.push_back(ScatterHit{ m, t });
        }
    }
}

// In-place sort. Layer fields are all pre-computed on scan, so any
// sort mode is a pure comparator change — no re-scanning needed.
// `reverse` flips the result. `seed` only matters for Random.
//
// `state` and `source_id` are only consulted by modes that look at
// per-layer external context (tag membership, position overrides):
// TagName, EffectiveX, EffectiveY. Pass nullptr for state and the
// sort will still work for all other modes.
inline void SortLayers(std::vector<LayerInfo>& layers,
                       PanelState::SortMode mode,
                       bool reverse,
                       uint32_t seed,
                       const PanelState* state,
                       uint32_t source_id)
{
    auto angle = [](const LayerInfo& L) {
        return std::atan2(L.cy - 0.5f, L.cx - 0.5f);
    };
    auto dist2 = [](const LayerInfo& L) {
        const float dx = L.cx - 0.5f, dy = L.cy - 0.5f;
        return dx * dx + dy * dy;
    };
    auto effective_xy = [&](const LayerInfo& L, float& x, float& y) {
        x = L.cx; y = L.cy;
        if (!state) return;
        for (const auto& po : state->position_overrides) {
            if (po.layer.source_id == source_id &&
                po.layer.fnv1a_hash == L.fnv1a_hash)
            {
                x = po.cx; y = po.cy;
                return;
            }
        }
    };
    auto first_tag_name = [&](const LayerInfo& L) -> std::string {
        if (!state) return {};
        std::string best;
        for (const auto& t : state->tags) {
            for (const auto& m : t.members) {
                if (m.source_id == source_id &&
                    m.fnv1a_hash == L.fnv1a_hash)
                {
                    if (best.empty() || t.name < best) best = t.name;
                    break;
                }
            }
        }
        return best;
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
        case PanelState::SortMode::Random: {
            uint32_t ka = a.fnv1a_hash * 2654435761u + seed;
            uint32_t kb = b.fnv1a_hash * 2654435761u + seed;
            return ka < kb;
        }
        case PanelState::SortMode::Alphabetical:
            return a.display_name < b.display_name;
        case PanelState::SortMode::IncludedFirst:
            if (a.included != b.included) return a.included > b.included;
            return a.display_name < b.display_name;
        case PanelState::SortMode::TagName: {
            std::string ta = first_tag_name(a);
            std::string tb = first_tag_name(b);
            // Untagged at the end.
            if (ta.empty() != tb.empty()) return !ta.empty();
            if (ta != tb) return ta < tb;
            return a.display_name < b.display_name;
        }
        case PanelState::SortMode::EffectiveX: {
            float ax, ay, bx, by;
            effective_xy(a, ax, ay); effective_xy(b, bx, by);
            return ax < bx;
        }
        case PanelState::SortMode::EffectiveY: {
            float ax, ay, bx, by;
            effective_xy(a, ax, ay); effective_xy(b, bx, by);
            return ay < by;
        }
        }
        return false;
    };
    std::sort(layers.begin(), layers.end(), less_than);
    if (reverse) std::reverse(layers.begin(), layers.end());
}

// Backwards-compatible overload — for sort modes that don't need
// per-layer state context.
inline void SortLayers(std::vector<LayerInfo>& layers,
                       PanelState::SortMode mode,
                       bool reverse = false,
                       uint32_t seed = 0)
{
    SortLayers(layers, mode, reverse, seed, nullptr, 0);
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

// Deterministic distinct color per ID — paired with the contrast-
// text picker in the pill renderer so text is always legible on the
// chosen background. Hue cycled via golden-ratio rotation so
// successive ids land in maximally different hue regions.
inline uint32_t ColorForId(uint32_t id)
{
    const float h = std::fmod(static_cast<float>(id) * 0.61803398875f, 1.0f);
    const float s = 0.58f;
    const float l = 0.50f;
    auto channel = [](float p, float q, float t) {
        if (t < 0.f) t += 1.f;
        if (t > 1.f) t -= 1.f;
        if (t < 1.f / 6.f) return p + (q - p) * 6.f * t;
        if (t < 1.f / 2.f) return q;
        if (t < 2.f / 3.f) return p + (q - p) * (2.f / 3.f - t) * 6.f;
        return p;
    };
    const float q = (l < 0.5f) ? l * (1.f + s) : (l + s - l * s);
    const float p = 2.f * l - q;
    const int rc = static_cast<int>(channel(p, q, h + 1.f / 3.f) * 255.f + 0.5f);
    const int gc = static_cast<int>(channel(p, q, h) * 255.f + 0.5f);
    const int bc = static_cast<int>(channel(p, q, h - 1.f / 3.f) * 255.f + 0.5f);
    return 0xFF000000u
         | static_cast<uint32_t>(rc & 0xFF)
         | (static_cast<uint32_t>(gc & 0xFF) << 8)
         | (static_cast<uint32_t>(bc & 0xFF) << 16);
}

// Pull the "tag prefix" off a layer display name. Rule: trim a
// trailing run of digits, then trim a single separator. Empty result
// means no useful prefix.
//   Fire_01          -> Fire
//   Curtain_Col_017  -> Curtain_Col
//   World            -> ""
//   Foo01            -> Foo
inline std::string ExtractTagPrefix(const std::string& name)
{
    size_t end = name.size();
    while (end > 0 && std::isdigit(static_cast<unsigned char>(name[end - 1]))) --end;
    if (end == 0 || end == name.size()) return {};
    char c = name[end - 1];
    if (c == '_' || c == '.' || c == '-' || c == ' ') --end;
    if (end == 0) return {};
    return name.substr(0, end);
}

// Build (or extend) tags by parsing prefixes off layer names. Called
// automatically when a source finishes scanning so the user gets a
// useful tag set without an extra click. Idempotent against
// repeated calls — existing tags get new members added; duplicates
// are skipped. Takes its own lock, so call OUTSIDE state.mu.
inline void AutotagByName(PanelState* state)
{
    if (!state) return;
    std::lock_guard<std::mutex> lk(state->mu);
    if (state->active_source_index < 0 ||
        state->active_source_index >= static_cast<int>(state->sources.size()))
    {
        return;
    }
    Source& src = state->sources[state->active_source_index];

    std::vector<std::pair<std::string, std::vector<LayerRef>>> by_prefix;
    auto group_for = [&](const std::string& key)
        -> std::vector<LayerRef>&
    {
        for (auto& kv : by_prefix) if (kv.first == key) return kv.second;
        by_prefix.emplace_back(key, std::vector<LayerRef>{});
        return by_prefix.back().second;
    };
    for (const auto& L : src.layers) {
        std::string prefix = ExtractTagPrefix(L.display_name);
        if (prefix.empty()) continue;
        group_for(prefix).push_back(LayerRef{ src.source_id, L.fnv1a_hash });
    }

    int created = 0, updated = 0, members_added = 0;
    for (auto& kv : by_prefix) {
        const std::string& prefix = kv.first;
        std::vector<LayerRef>& members = kv.second;
        if (members.size() < 2) continue;
        Tag* tag = nullptr;
        for (auto& t : state->tags) if (t.name == prefix) { tag = &t; break; }
        if (!tag) {
            Tag nt;
            nt.tag_id = state->next_tag_id++;
            nt.name   = prefix;
            nt.color  = ColorForId(nt.tag_id);
            state->tags.push_back(std::move(nt));
            tag = &state->tags.back();
            ++created;
        } else {
            ++updated;
        }
        for (const auto& m : members) {
            bool already = false;
            for (const auto& em : tag->members) if (em == m) { already = true; break; }
            if (!already) { tag->members.push_back(m); ++members_added; }
        }
    }
    // Auto-tag UNIQUE-named layers too: any layer not absorbed into a
    // shared-prefix tag (its prefix group had <2 members, or it had
    // no usable prefix) gets its OWN single-member tag named exactly
    // after the layer. Otherwise unique lights (e.g. "Top Light Down")
    // would be unreachable from the wizard's tag filter — there is no
    // separate "untagged" bucket by design; every light is tagged.
    // Idempotent (re-run after each scan): reuses an existing
    // same-named tag and skips members already present.
    std::vector<uint32_t> covered;
    for (auto& kv : by_prefix) {
        if (kv.second.size() < 2) continue;
        for (const auto& m : kv.second) covered.push_back(m.fnv1a_hash);
    }
    for (const auto& L : src.layers) {
        bool is_covered = false;
        for (uint32_t h : covered)
            if (h == L.fnv1a_hash) { is_covered = true; break; }
        if (is_covered) continue;
        LayerRef ref{ src.source_id, L.fnv1a_hash };
        Tag* tag = nullptr;
        for (auto& t : state->tags)
            if (t.name == L.display_name) { tag = &t; break; }
        if (!tag) {
            Tag nt;
            nt.tag_id = state->next_tag_id++;
            nt.name   = L.display_name;
            nt.color  = ColorForId(nt.tag_id);
            state->tags.push_back(std::move(nt));
            tag = &state->tags.back();
            ++created;
        }
        bool already = false;
        for (const auto& em : tag->members)
            if (em == ref) { already = true; break; }
        if (!already) { tag->members.push_back(ref); ++members_added; }
    }

    if (created > 0 || members_added > 0) {
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "Auto-tagged: %d new tag(s), %d updated, %d member(s) added.",
                      created, updated, members_added);
        state->last_status = buf;
    }
}

// Returns the position to use for a layer, honoring any user
// override. Falls back to the scanned centroid (X or Y per the
// active sort mode).
inline float EffectiveLayerX(const PanelState& state, const LayerRef& ref,
                              const LayerInfo& L, SortMode mode)
{
    for (const auto& po : state.position_overrides) {
        if (po.layer == ref) return po.cx;
    }
    return LayerDisplayX(L, mode);
}
inline float EffectiveLayerY(const PanelState& state, const LayerRef& ref,
                              const LayerInfo& L, SortMode mode)
{
    for (const auto& po : state.position_overrides) {
        if (po.layer == ref) return po.cy;
    }
    return LayerDisplayY(L, mode);
}

// ===== Undo: capture / restore =======================================

inline UndoSnapshot CaptureUndoSnapshot(const PanelState& state)
{
    UndoSnapshot s;
    s.sources.reserve(state.sources.size());
    for (const auto& src : state.sources) {
        SourceSnap ss;
        ss.source_id = src.source_id;
        ss.layers.reserve(src.layers.size());
        for (const auto& L : src.layers) {
            ss.layers.push_back({ L.display_name, L.included });
        }
        s.sources.push_back(std::move(ss));
    }
    s.binds              = state.binds;
    s.tags               = state.tags;
    s.position_overrides = state.position_overrides;
    s.chases             = state.chases;
    s.sort_mode          = state.sort_mode;
    s.sort_reverse       = state.sort_reverse;
    s.random_seed        = state.random_seed;
    s.active_source_index = state.active_source_index;
    s.active_tab         = state.active_tab;
    s.active_chase_index = state.active_chase_index;
    s.chase_in_wizard    = state.chase_in_wizard;
    s.default_timing     = state.default_timing;
    s.hide_unchecked     = state.hide_unchecked;
    s.next_source_id     = state.next_source_id;
    s.next_bind_id       = state.next_bind_id;
    s.next_tag_id        = state.next_tag_id;
    s.next_chase_id      = state.next_chase_id;
    return s;
}

inline void RestoreUndoSnapshot(PanelState& state, const UndoSnapshot& s)
{
    // For each saved source, find the matching current Source by id
    // and rebuild its layers vector in the saved order, restoring
    // included flags. We hold onto the existing LayerInfo objects so
    // thumb_rgba / texture_id / centroid metrics stay attached.
    for (const auto& ss : s.sources) {
        Source* src = nullptr;
        for (auto& cur : state.sources) {
            if (cur.source_id == ss.source_id) { src = &cur; break; }
        }
        if (!src) continue;
        // Move current layers into a name-indexed map for O(1) lookup
        // while rebuilding.
        std::unordered_map<std::string, LayerInfo> by_name;
        by_name.reserve(src->layers.size());
        for (auto& L : src->layers) {
            by_name.emplace(L.display_name, std::move(L));
        }
        src->layers.clear();
        src->layers.reserve(ss.layers.size());
        for (const auto& isnap : ss.layers) {
            auto it = by_name.find(isnap.display_name);
            if (it != by_name.end()) {
                LayerInfo L = std::move(it->second);
                L.included = isnap.included;
                src->layers.push_back(std::move(L));
                by_name.erase(it);
            }
        }
        // Any layers we have now but the snapshot didn't (e.g. a
        // skipped-include happened after the snapshot was taken):
        // append them at the end so they aren't lost.
        for (auto& kv : by_name) {
            src->layers.push_back(std::move(kv.second));
        }
    }
    state.binds              = s.binds;
    state.tags               = s.tags;
    state.position_overrides = s.position_overrides;
    state.chases             = s.chases;
    state.sort_mode          = s.sort_mode;
    state.sort_reverse       = s.sort_reverse;
    state.random_seed        = s.random_seed;
    if (s.active_source_index >= 0 &&
        s.active_source_index < (int)state.sources.size())
    {
        state.active_source_index = s.active_source_index;
    }
    state.active_tab         = s.active_tab;
    state.active_chase_index = s.active_chase_index;
    state.chase_in_wizard    = s.chase_in_wizard;
    state.default_timing     = s.default_timing;
    state.hide_unchecked     = s.hide_unchecked;
    state.next_source_id     = s.next_source_id;
    state.next_bind_id       = s.next_bind_id;
    state.next_tag_id        = s.next_tag_id;
    state.next_chase_id      = s.next_chase_id;

    // Drop any drag state that was in flight when undo fired.
    state.drag_layer_hash = 0;
    state.drag_armed_layer_hash = 0;
}

inline bool PerformUndo(PanelState& state)
{
    if (state.undo_stack.empty()) return false;
    state.redo_stack.push_back(CaptureUndoSnapshot(state));
    if (state.redo_stack.size() > kMaxUndoStack) {
        state.redo_stack.erase(state.redo_stack.begin());
    }
    UndoSnapshot s = std::move(state.undo_stack.back());
    state.undo_stack.pop_back();
    RestoreUndoSnapshot(state, s);
    // Refresh the stable-state cache so the post-frame auto-snapshot
    // doesn't think the undo itself is a change worth recording.
    state.last_stable = CaptureUndoSnapshot(state);
    state.has_last_stable = true;
    return true;
}

inline bool PerformRedo(PanelState& state)
{
    if (state.redo_stack.empty()) return false;
    state.undo_stack.push_back(CaptureUndoSnapshot(state));
    if (state.undo_stack.size() > kMaxUndoStack) {
        state.undo_stack.erase(state.undo_stack.begin());
    }
    UndoSnapshot s = std::move(state.redo_stack.back());
    state.redo_stack.pop_back();
    RestoreUndoSnapshot(state, s);
    state.last_stable = CaptureUndoSnapshot(state);
    state.has_last_stable = true;
    return true;
}
