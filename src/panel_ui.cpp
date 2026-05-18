#include "panel_ui.h"

#include "panel_state.h"
#include "exr_scan.h"   // IncludeSkippedLayer for the Skipped section

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"   // ImGui::TabItemButton, ImGuiInputTextFlags_*

namespace panel_ui {

namespace {

// ===== String / inclusion helpers =====================================

bool ContainsCI(const std::string& haystack, const char* needle)
{
    if (!needle || !*needle) return false;
    const size_t n = std::strlen(needle);
    if (haystack.size() < n) return false;
    auto lower = [](char c) { return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c))); };
    for (size_t i = 0; i + n <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < n; ++j) {
            if (lower(haystack[i + j]) != lower(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

int IncludedCount(const std::vector<LayerInfo>& layers)
{
    int n = 0;
    for (const auto& L : layers) if (L.included) ++n;
    return n;
}

int IndexOfHash(const std::vector<LayerInfo>& layers, uint32_t hash)
{
    if (hash == 0) return -1;
    for (int i = 0; i < (int)layers.size(); ++i) {
        if (layers[i].fnv1a_hash == hash) return i;
    }
    return -1;
}

int FullIndexOfIncluded(const std::vector<LayerInfo>& layers, int included_idx)
{
    int sub = 0;
    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
        if (!layers[i].included) continue;
        if (sub == included_idx) return i;
        ++sub;
    }
    return -1;
}

void TickPreview(PanelState* state, int n_included)
{
    if (n_included == 0) {
        state->preview_playing = false;
        state->preview_index = 0;
        state->preview_accum_ms = 0.f;
        return;
    }
    if (state->preview_index >= n_included) state->preview_index = 0;
    if (!state->preview_playing) {
        state->preview_accum_ms = 0.f;
        return;
    }
    const float dt_ms = ImGui::GetIO().DeltaTime * 1000.f;
    state->preview_accum_ms += dt_ms;
    const float step = std::max(20.f, state->preview_ms_step);
    int safety = 1000;
    while (state->preview_accum_ms >= step && safety-- > 0) {
        state->preview_index = (state->preview_index + 1) % n_included;
        state->preview_accum_ms -= step;
    }
    if (safety <= 0) state->preview_accum_ms = 0.f;
}

// Basename helper — strip directory component for compact source labels.
std::string Basename(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// ColorForId, ExtractTagPrefix, and AutotagByName moved into
// panel_state.h so exr_scan can trigger autotag automatically after
// a scan completes (no explicit button needed).

// ===== Frame snapshot =================================================

struct SourceSummary {
    uint32_t    source_id   = 0;
    std::string path;
    std::string scan_path;
    int         scan_frame  = 0;
    int         frame_count = 1;
    bool        animation   = false;
    int         image_width = 0;
    int         image_height = 0;
    size_t      layer_count = 0;
    size_t      skipped_count = 0;
    bool        is_active   = false;
};

struct FrameSnapshot {
    // Active source (the one whose layers the Staging tab table shows)
    uint32_t                       active_source_id = 0;
    std::string                    exr_path;
    int                            image_width = 0;
    int                            image_height = 0;
    std::vector<LayerInfo>         layers;       // working copy (UI mutates `included`)
    std::vector<SkippedLayer>      skipped;
    // Source list summary (for the Sources tab)
    std::vector<SourceSummary>     source_summaries;
    // Session-level data (deep-copied so the UI sees a consistent frame)
    std::vector<Bind>              binds;
    std::vector<Tag>               tags;
    std::vector<PositionOverride>  position_overrides;
    std::vector<Chase>             chases;
    int                            active_chase_index = -1;
    PanelTab                       active_tab = PanelTab::Sources;
    bool                           chase_in_wizard = false;
    bool                           hide_unchecked = false;
    std::string                    session_save_path;
    // Status fields
    std::string                    last_error;
    std::string                    last_status;
    bool                           scanning = false;
};

FrameSnapshot TakeSnapshot(PanelState* state)
{
    FrameSnapshot s;
    s.scanning        = state->scanning.load();
    std::lock_guard<std::mutex> lk(state->mu);
    s.active_tab         = state->active_tab;
    s.active_chase_index = state->active_chase_index;
    s.chase_in_wizard    = state->chase_in_wizard;
    s.hide_unchecked     = state->hide_unchecked;
    s.session_save_path  = state->session_save_path;
    s.last_error         = state->last_error;
    s.last_status        = state->last_status;
    if (const Source* src = ActiveSource(*state)) {
        s.active_source_id = src->source_id;
        s.exr_path         = src->path;
        s.image_width      = src->image_width;
        s.image_height     = src->image_height;
        s.layers           = src->layers;
        s.skipped          = src->skipped;
    }
    s.source_summaries.reserve(state->sources.size());
    for (const Source& src : state->sources) {
        SourceSummary ss;
        ss.source_id     = src.source_id;
        ss.path          = src.path;
        ss.scan_path     = src.scan_path;
        ss.scan_frame    = src.scan_frame;
        ss.frame_count   = src.frame_count;
        ss.animation     = src.animation;
        ss.image_width   = src.image_width;
        ss.image_height  = src.image_height;
        ss.layer_count   = src.layers.size();
        ss.skipped_count = src.skipped.size();
        ss.is_active     = (src.source_id == s.active_source_id);
        s.source_summaries.push_back(std::move(ss));
    }
    s.binds              = state->binds;
    s.tags               = state->tags;
    s.position_overrides = state->position_overrides;
    s.chases             = state->chases;
    return s;
}

// Push layer inclusion changes back into the active source. Matches by
// display_name so a concurrent scan that swapped layers simply ignores
// stale toggles. Also publishes any drag-reorder we recorded as a
// reordering of the active source's layers vector.
void PublishInclusionChanges(PanelState* state,
                             const std::vector<LayerInfo>& working)
{
    std::lock_guard<std::mutex> lk(state->mu);
    Source* src = ActiveSource(*state);
    if (!src) return;
    if (src->layers.size() != working.size()) return;
    for (size_t i = 0; i < working.size(); ++i) {
        if (src->layers[i].display_name == working[i].display_name) {
            src->layers[i].included = working[i].included;
        }
    }
}

// Reorder the active source's layers by display_name to match
// `desired_order`. Called when the user drags rows in the Staging tab.
void PublishLayerReorder(PanelState* state,
                         const std::vector<std::string>& desired_order)
{
    std::lock_guard<std::mutex> lk(state->mu);
    Source* src = ActiveSource(*state);
    if (!src) return;
    if (src->layers.size() != desired_order.size()) return;
    // Build name -> current-index map, then permute.
    std::unordered_map<std::string, size_t> idx;
    idx.reserve(src->layers.size());
    for (size_t i = 0; i < src->layers.size(); ++i) {
        idx.emplace(src->layers[i].display_name, i);
    }
    std::vector<LayerInfo> reordered;
    reordered.reserve(src->layers.size());
    for (const auto& name : desired_order) {
        auto it = idx.find(name);
        if (it == idx.end()) return;  // mismatch -> abort, don't corrupt
        reordered.push_back(std::move(src->layers[it->second]));
    }
    src->layers = std::move(reordered);
}

// ===== Centroid canvas =================================================

void DrawCentroidCanvas(const std::vector<LayerInfo>& layers,
                        uint32_t source_id,
                        const std::vector<PositionOverride>& overrides,
                        int image_w, int image_h,
                        int highlight_full_index,
                        int selected_index,
                        SortMode sort_mode,
                        float requested_height,
                        PanelState* state = nullptr,
                        bool allow_drag = true)
{
    const float pane_w = ImGui::GetContentRegionAvail().x;
    if (pane_w < 80.f) {
        ImGui::Dummy(ImVec2(0.f, requested_height));
        return;
    }
    const float aspect = (image_w > 0 && image_h > 0)
        ? static_cast<float>(image_w) / static_cast<float>(image_h)
        : 16.f / 9.f;
    float canvas_w = pane_w - 4.f;
    float canvas_h = canvas_w / aspect;
    if (canvas_h > requested_height) {
        canvas_h = requested_height;
        canvas_w = canvas_h * aspect;
    }
    canvas_w = std::max(80.f, canvas_w);
    canvas_h = std::max(40.f, canvas_h);

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 max(origin.x + canvas_w, origin.y + canvas_h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Interactive hit-test layer over the canvas. InvisibleButton
    // registers clicks + drag tracking without consuming the visual
    // area (we still draw the rect/dots manually below).
    bool hovered = false, active = false;
    if (state) {
        ImGui::InvisibleButton("centroid_canvas_hit",
                               ImVec2(canvas_w, canvas_h));
        hovered = ImGui::IsItemHovered();
        active  = ImGui::IsItemActive();
    }

    dl->AddRectFilled(origin, max, IM_COL32(20, 22, 26, 255), 4.f);
    dl->AddRect(origin, max, IM_COL32(70, 72, 78, 255), 4.f);
    const float midx = origin.x + canvas_w * 0.5f;
    const float midy = origin.y + canvas_h * 0.5f;
    dl->AddLine(ImVec2(origin.x, midy), ImVec2(max.x, midy),
                IM_COL32(48, 50, 56, 255));
    dl->AddLine(ImVec2(midx, origin.y), ImVec2(midx, max.y),
                IM_COL32(48, 50, 56, 255));

    auto override_pos = [&](const LayerInfo& L, float& ox, float& oy) -> bool {
        for (const auto& po : overrides) {
            if (po.layer.source_id == source_id &&
                po.layer.fnv1a_hash == L.fnv1a_hash) {
                ox = po.cx;
                oy = po.cy;
                return true;
            }
        }
        return false;
    };
    // effective_xy returns the position to render for a given layer.
    // When the layer is bound (and state is provided), all members of
    // the bind collapse to ONE position so the canvas shows a single
    // dot per bind. Position priority:
    //   1. Any bind member's position override
    //   2. Mean of bind members' scanned centroids
    //   3. Single-layer override
    //   4. Scanned centroid
    auto effective_xy = [&](const LayerInfo& L, float& dx, float& dy) {
        if (state) {
            LayerRef ref{ source_id, L.fnv1a_hash };
            if (const Bind* b = BindOfLayer(*state, ref)) {
                // Any-member override wins.
                for (const auto& m : b->members) {
                    for (const auto& po : overrides) {
                        if (po.layer == m) {
                            dx = po.cx; dy = po.cy;
                            return;
                        }
                    }
                }
                // Mean of member centroids.
                float sx = 0.f, sy = 0.f; int n = 0;
                for (const auto& m : b->members) {
                    const LayerInfo* mL = FindLayerByRef(*state, m);
                    if (mL) {
                        sx += LayerDisplayX(*mL, sort_mode);
                        sy += LayerDisplayY(*mL, sort_mode);
                        ++n;
                    }
                }
                if (n > 0) { dx = sx / n; dy = sy / n; return; }
            }
        }
        if (override_pos(L, dx, dy)) return;
        dx = LayerDisplayX(L, sort_mode);
        dy = LayerDisplayY(L, sort_mode);
    };

    // ---- Interaction: hold-delay drag-arm, then drag, then commit. ----
    if (state) {
        const ImGuiIO& io = ImGui::GetIO();
        const ImVec2 mp = ImGui::GetMousePos();
        const float mx_norm = std::min(1.f, std::max(0.f,
                                    (mp.x - origin.x) / canvas_w));
        const float my_norm = std::min(1.f, std::max(0.f,
                                    (mp.y - origin.y) / canvas_h));

        // Helper: find the closest dot under the cursor (or -1).
        auto hit_test_dot = [&]() -> int {
            int hit = -1;
            float best_d2 = 81.f;  // 9 px² hit radius
            for (int i = 0; i < (int)layers.size(); ++i) {
                float dx, dy;
                effective_xy(layers[i], dx, dy);
                const float sx = origin.x + dx * canvas_w;
                const float sy = origin.y + dy * canvas_h;
                const float ddx = sx - mp.x;
                const float ddy = sy - mp.y;
                const float d2 = ddx*ddx + ddy*ddy;
                if (d2 < best_d2) { best_d2 = d2; hit = i; }
            }
            return hit;
        };

        // Phase 1: idle → arm on mouse-down over a dot.
        // Selection happens IMMEDIATELY on mouse-down so the row
        // tint + canvas ring update without waiting for release.
        // The drag itself still requires holding for
        // kCentroidDragHoldSeconds to avoid accidental moves.
        if (state->drag_layer_hash == 0 &&
            state->drag_armed_layer_hash == 0 &&
            hovered &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            int hit = hit_test_dot();
            if (hit >= 0) {
                // Drag-to-reposition the centroid is a Staging-only
                // affordance (Staging is the master config; Chase mode
                // is for manual tweaks, not moving lights around). When
                // drags are disabled we still select on press so the
                // click-to-pin preview works in the chase editor.
                if (allow_drag) {
                    state->drag_armed_source_id  = source_id;
                    state->drag_armed_layer_hash = layers[hit].fnv1a_hash;
                    state->drag_armed_press_time = ImGui::GetTime();
                }
                // Plain-click-style selection on press: replace the
                // multi-select set with this single layer so the
                // visual + context-menu state matches the row-click
                // path's behavior.
                state->selected_hash       = layers[hit].fnv1a_hash;
                state->last_selected_hash  = layers[hit].fnv1a_hash;
                state->selected_hashes.clear();
                state->selected_hashes.push_back(layers[hit].fnv1a_hash);
            }
        }

        // Phase 2: armed → either drag (upgrade after hold) or end
        // (release before hold).
        if (state->drag_armed_layer_hash != 0) {
            const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
            const double held = ImGui::GetTime() - state->drag_armed_press_time;
            if (released) {
                // Released before hold threshold — selection already
                // happened on press; nothing else to commit.
                state->drag_armed_layer_hash = 0;
                state->drag_armed_source_id  = 0;
                (void)io;
            } else if (held >= kCentroidDragHoldSeconds) {
                // Hold threshold reached → enter drag.
                state->drag_layer_source_id = state->drag_armed_source_id;
                state->drag_layer_hash      = state->drag_armed_layer_hash;
                state->drag_cursor_x        = mx_norm;
                state->drag_cursor_y        = my_norm;
                state->drag_armed_layer_hash = 0;
                state->drag_armed_source_id  = 0;
            }
        }

        // Phase 3: actively dragging.
        if (state->drag_layer_hash != 0 && active) {
            state->drag_cursor_x = mx_norm;
            state->drag_cursor_y = my_norm;
        }
        if (state->drag_layer_hash != 0 &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Left))
        {
            LayerRef ref{ state->drag_layer_source_id, state->drag_layer_hash };
            std::lock_guard<std::mutex> lk(state->mu);
            // If the dragged layer is part of a bind, write the
            // override to ALL bind members so the whole bind moves
            // together. Otherwise it's a single-layer override.
            std::vector<LayerRef> targets;
            const Bind* b = BindOfLayer(*state, ref);
            if (b) {
                for (const auto& m : b->members) targets.push_back(m);
            } else {
                targets.push_back(ref);
            }
            for (const LayerRef& tgt : targets) {
                bool found = false;
                for (auto& po : state->position_overrides) {
                    if (po.layer == tgt) {
                        po.cx = state->drag_cursor_x;
                        po.cy = state->drag_cursor_y;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    state->position_overrides.push_back(
                        PositionOverride{ tgt, state->drag_cursor_x,
                                          state->drag_cursor_y, 0.f });
                }
            }
            state->drag_layer_hash = 0;
            state->drag_layer_source_id = 0;
        }
    }

    // ---- Draw the dots. Dragging dot follows the cursor. ----
    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
        const LayerInfo& L = layers[i];
        float dx, dy;
        if (state && state->drag_layer_hash == L.fnv1a_hash &&
            state->drag_layer_source_id == source_id)
        {
            dx = state->drag_cursor_x;
            dy = state->drag_cursor_y;
        } else {
            effective_xy(L, dx, dy);
        }
        const float x = origin.x + dx * canvas_w;
        const float y = origin.y + dy * canvas_h;
        if (!L.included) {
            dl->AddCircleFilled(ImVec2(x, y), 2.5f, IM_COL32(80, 80, 86, 200));
        } else {
            dl->AddCircleFilled(ImVec2(x, y), 3.0f, IM_COL32(220, 200, 80, 220));
        }
    }
    if (selected_index >= 0 && selected_index < static_cast<int>(layers.size())) {
        const LayerInfo& L = layers[selected_index];
        float dx, dy;
        if (state && state->drag_layer_hash == L.fnv1a_hash &&
            state->drag_layer_source_id == source_id)
        {
            dx = state->drag_cursor_x;
            dy = state->drag_cursor_y;
        } else {
            effective_xy(L, dx, dy);
        }
        const float x = origin.x + dx * canvas_w;
        const float y = origin.y + dy * canvas_h;
        dl->AddCircleFilled(ImVec2(x, y), 7.f, IM_COL32(120, 180, 255, 70));
        dl->AddCircle(ImVec2(x, y), 7.f, IM_COL32(120, 180, 255, 255), 0, 2.f);
    }
    if (highlight_full_index >= 0 &&
        highlight_full_index < static_cast<int>(layers.size())) {
        const LayerInfo& L = layers[highlight_full_index];
        float dx, dy;
        effective_xy(L, dx, dy);
        const float x = origin.x + dx * canvas_w;
        const float y = origin.y + dy * canvas_h;
        dl->AddCircleFilled(ImVec2(x, y), 8.f, IM_COL32(255, 240, 160, 90));
        dl->AddCircle(ImVec2(x, y), 8.f, IM_COL32(255, 240, 160, 255), 0, 2.f);
    }

    // When NOT interactive, still need to advance the cursor for
    // layout flow (InvisibleButton did it in the interactive path).
    if (!state) ImGui::Dummy(ImVec2(canvas_w, canvas_h));
}

void DrawPositionBar(float pos)
{
    constexpr float kBarHeight = 8.f;
    ImGui::Dummy(ImVec2(0.f, 2.f));
    const ImVec2 p_min = ImGui::GetCursorScreenPos();
    const float avail = std::max(80.f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p_max(p_min.x + avail, p_min.y + kBarHeight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p_min, p_max, IM_COL32(60, 60, 65, 255), 2.f);
    const float clamped = pos < 0.f ? 0.f : (pos > 1.f ? 1.f : pos);
    const float marker_x = p_min.x + clamped * avail;
    dl->AddRectFilled(ImVec2(marker_x - 1.f, p_min.y - 1.f),
                      ImVec2(marker_x + 1.f, p_max.y + 1.f),
                      IM_COL32(220, 200, 80, 255));
    ImGui::Dummy(ImVec2(avail, kBarHeight + 2.f));
}

float SortPosition(const LayerInfo& L, SortMode mode, double max_brightness)
{
    switch (mode) {
    case SortMode::CentroidX:  return L.cx;
    case SortMode::CentroidY:  return L.cy;
    case SortMode::HotspotX:   return L.cx_hot;
    case SortMode::HotspotY:   return L.cy_hot;
    case SortMode::Brightness:
        return max_brightness > 0.0
            ? static_cast<float>(L.total / max_brightness)
            : 0.f;
    case SortMode::RadialSweep: {
        float a = std::atan2(L.cy - 0.5f, L.cx - 0.5f);
        constexpr float kPi = 3.14159265f;
        return (a + kPi) / (2.f * kPi);
    }
    case SortMode::DistanceFromCenter: {
        float dx = L.cx - 0.5f, dy = L.cy - 0.5f;
        return std::sqrt(dx * dx + dy * dy) / std::sqrt(0.5f);
    }
    case SortMode::Random:
    case SortMode::Alphabetical:
    case SortMode::IncludedFirst:
    case SortMode::TagName:
    case SortMode::EffectiveX:
    case SortMode::EffectiveY:
        return -1.f;
    }
    return 0.f;
}

// ===== Layers table (Staging tab) =====================================

enum class TableCol : int {
    None = 0, Index, On, Src, Layer, Tags, X, Y, Z,
};

struct TableInteractionResult {
    int       clicked_row = -1;      // -1 if no click
    int       drag_from   = -1;
    int       drag_to     = -1;      // valid if drag_from >= 0
    bool      open_context_menu = false;
    int       context_menu_row = -1;
    TableCol  header_clicked_col = TableCol::None;
};

// Returns row interaction. Caller persists selection / drag results.
TableInteractionResult DrawLayersTable(std::vector<LayerInfo>& layers,
                                       uint32_t source_id,
                                       const std::vector<Bind>& binds,
                                       const std::vector<Tag>& tags,
                                       const std::vector<PositionOverride>& state_position_overrides,
                                       const std::vector<uint32_t>& selected_set,
                                       int highlight_full_index,
                                       int selected_index,
                                       SortMode sort_mode,
                                       bool show_source_col,
                                       bool hide_unchecked,
                                       float table_height)
{
    TableInteractionResult r;
    if (layers.empty()) return r;

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    double max_brightness = 0.0;
    for (const auto& L : layers) {
        if (L.total > max_brightness) max_brightness = L.total;
    }

    const int n_cols = show_source_col ? 8 : 7;
    if (!ImGui::BeginTable("layers", n_cols, kTableFlags,
                           ImVec2(0.f, table_height))) {
        return r;
    }
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#",   ImGuiTableColumnFlags_WidthFixed, 32.f);
    ImGui::TableSetupColumn("On",  ImGuiTableColumnFlags_WidthFixed, 28.f);
    if (show_source_col) {
        ImGui::TableSetupColumn("Src", ImGuiTableColumnFlags_WidthFixed, 64.f);
    }
    ImGui::TableSetupColumn("Layer",   ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("Tags",    ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("X",       ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("Y",       ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("Z",       ImGuiTableColumnFlags_WidthFixed, 48.f);
    // Manual headers so we can detect clicks on each column and use
    // them as sort shortcuts.
    auto header_with_click = [&](const char* label,
                                  TableCol col,
                                  const char* tooltip = nullptr)
    {
        ImGui::TableHeader(label);
        if (ImGui::IsItemClicked()) r.header_clicked_col = col;
        if (tooltip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tooltip);
    };
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    ImGui::TableNextColumn(); ImGui::TableHeader("#");
    ImGui::TableNextColumn();
    header_with_click("On", TableCol::On, "Click to sort included rows first");
    if (show_source_col) {
        ImGui::TableNextColumn();
        header_with_click("Src", TableCol::Src, "Click to sort by source");
    }
    ImGui::TableNextColumn();
    header_with_click("Layer", TableCol::Layer,
        "Click to sort alphabetically (toggle direction on re-click)");
    ImGui::TableNextColumn();
    header_with_click("Tags", TableCol::Tags,
        "Click to group by tag (alphabetical, untagged last)");
    ImGui::TableNextColumn();
    header_with_click("X", TableCol::X, "Click to sort by effective X");
    ImGui::TableNextColumn();
    header_with_click("Y", TableCol::Y, "Click to sort by effective Y");
    ImGui::TableNextColumn();
    header_with_click("Z", TableCol::Z, "Click to sort by Z (mostly 0)");

    // Build per-layer lookups for bind + tag membership.
    auto bind_for = [&](const LayerInfo& L) -> const Bind* {
        for (const auto& b : binds) {
            for (const auto& m : b.members) {
                if (m.source_id == source_id && m.fnv1a_hash == L.fnv1a_hash)
                    return &b;
            }
        }
        return nullptr;
    };
    auto tags_for = [&](const LayerInfo& L, std::vector<const Tag*>& out) {
        out.clear();
        for (const auto& t : tags) {
            for (const auto& m : t.members) {
                if (m.source_id == source_id && m.fnv1a_hash == L.fnv1a_hash) {
                    out.push_back(&t);
                    break;
                }
            }
        }
    };
    std::vector<const Tag*> tag_list;

    // Helper: pick high-contrast text color (black or white) for a
    // given background. Uses Rec.709 luminance; threshold tuned by eye.
    auto contrasting_text = [](ImU32 bg) -> ImU32 {
        int r = (bg >>  0) & 0xFF;
        int g = (bg >>  8) & 0xFF;
        int b = (bg >> 16) & 0xFF;
        float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;  // 0..255
        return (Y > 140.f) ? IM_COL32(20, 20, 20, 255)
                           : IM_COL32(240, 240, 240, 255);
    };

    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
        LayerInfo& L = layers[i];
        if (hide_unchecked && !L.included) continue;
        ImGui::TableNextRow();

        // Wrap the entire row in PushID(i) so every widget inside —
        // tag pills, checkbox, drag source, etc. — has a unique ID
        // even when their labels repeat across rows (e.g. tag "Fire"
        // pill rendered on many rows after Autotag by name).
        ImGui::PushID(i);

        const Bind* b = bind_for(L);
        // "Selected" = in the multi-select set (drives bulk actions).
        // The single-row "focus" (selected_index) gets a slightly
        // brighter tint to indicate it's the keyboard/anchor row.
        bool in_set = false;
        for (uint32_t h : selected_set) {
            if (h == L.fnv1a_hash) { in_set = true; break; }
        }
        if (i == highlight_full_index) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(120, 80, 30, 80));
        } else if (i == selected_index) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(40, 100, 150, 120));
        } else if (in_set) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(40, 80, 120, 90));
        } else if (b) {
            ImU32 c = b->color;
            ImU32 faded = (c & 0x00FFFFFFu) | 0x40000000u;
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, faded);
        } else if (!L.included) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(40, 40, 44, 120));
        }

        // # column with row-spanning Selectable for click + DnD.
        ImGui::TableNextColumn();
        const bool is_selected = (i == selected_index);
        if (ImGui::Selectable("##row", is_selected,
                ImGuiSelectableFlags_SpanAllColumns |
                ImGuiSelectableFlags_AllowOverlap)) {
            r.clicked_row = i;
        }
        if (ImGui::IsItemHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        {
            r.open_context_menu = true;
            r.context_menu_row = i;
        }
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("CM_ROW", &i, sizeof(int));
            ImGui::Text("Move: %s", L.display_name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CM_ROW")) {
                int src_i = *(const int*)payload->Data;
                if (src_i != i) {
                    r.drag_from = src_i;
                    r.drag_to   = i;
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        if (L.included) ImGui::Text("%d", i + 1);
        else            ImGui::TextDisabled("%d", i + 1);

        // On column
        ImGui::TableNextColumn();
        ImGui::Checkbox("##on", &L.included);

        // Source column (only when 2+ sources)
        if (show_source_col) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("#%u", source_id);
        }

        // Layer column (name + optional bind badge)
        ImGui::TableNextColumn();
        if (b) {
            ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                               "[%s] ", b->name.c_str());
            ImGui::SameLine(0.f, 4.f);
        }
        if (L.included) ImGui::TextUnformatted(L.display_name.c_str());
        else            ImGui::TextDisabled("%s", L.display_name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FNV-1a hash: 0x%08x\nTotal luminance: %.4g",
                              L.fnv1a_hash, L.total);
        }

        // Tags column (pills). Each pill is keyed by tag_id (PushID)
        // so identical labels across rows don't collide. Text color
        // is chosen for legibility against the pill background.
        ImGui::TableNextColumn();
        tags_for(L, tag_list);
        for (size_t ti = 0; ti < tag_list.size(); ++ti) {
            const Tag* t = tag_list[ti];
            const ImU32 bg_col = t->color;
            const ImU32 fg_col = contrasting_text(bg_col);
            const ImVec4 bg_v = ImGui::ColorConvertU32ToFloat4(bg_col);
            const ImVec4 fg_v = ImGui::ColorConvertU32ToFloat4(fg_col);
            ImGui::PushID(static_cast<int>(t->tag_id));
            ImGui::PushStyleColor(ImGuiCol_Button, bg_v);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_v);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_v);
            ImGui::PushStyleColor(ImGuiCol_Text, fg_v);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.f, 0.f));
            ImGui::SmallButton(t->name.c_str());
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
            ImGui::PopID();
            if (ti + 1 < tag_list.size()) ImGui::SameLine(0.f, 2.f);
        }

        // X / Y / Z columns. Reads through the same effective-position
        // logic the canvas uses so bound layers and overridden layers
        // display the same values you see on the dot.
        float dx = LayerDisplayX(L, sort_mode);
        float dy = LayerDisplayY(L, sort_mode);
        float dz = 0.f;
        // Single-layer override is the only place cz lives today.
        for (size_t poi = 0; poi < state_position_overrides.size(); ++poi) {
            const auto& po = state_position_overrides[poi];
            if (po.layer.source_id == source_id &&
                po.layer.fnv1a_hash == L.fnv1a_hash)
            {
                dx = po.cx; dy = po.cy; dz = po.cz;
                break;
            }
        }
        ImGui::TableNextColumn();
        if (L.included) ImGui::Text("%.2f", dx); else ImGui::TextDisabled("%.2f", dx);
        ImGui::TableNextColumn();
        if (L.included) ImGui::Text("%.2f", dy); else ImGui::TextDisabled("%.2f", dy);
        ImGui::TableNextColumn();
        if (L.included) ImGui::Text("%.2f", dz); else ImGui::TextDisabled("%.2f", dz);
        (void)max_brightness;
        ImGui::PopID();  // matches the PushID(i) at the top of the row
    }
    ImGui::EndTable();
    return r;
}

void DrawSkippedSection(PanelState* state, uint32_t source_id,
                        const std::vector<SkippedLayer>& skipped)
{
    if (skipped.empty()) return;
    std::string header = "Skipped (" + std::to_string(skipped.size()) +
                         ")  \xE2\x80\x94  click Include if a layer was "
                         "wrongly classified###skipped";
    if (ImGui::CollapsingHeader(header.c_str())) {
        for (size_t i = 0; i < skipped.size(); ++i) {
            const SkippedLayer& s = skipped[i];
            ImGui::PushID(static_cast<int>(i));
            // "all black" and "read failed" stay un-includable; their
            // payload data is genuinely useless (or unreadable). The
            // common false-positive case is substring matches like
            // "Crypto*" — those are include-able.
            const bool includable =
                (s.reason != "all black") &&
                (s.reason != "read failed") &&
                (s.reason != "not RGB-complete");
            ImGui::BeginDisabled(!includable || state == nullptr);
            if (ImGui::SmallButton("Include")) {
                exr_scan::IncludeSkippedLayer(state, source_id, s.display_name);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::Text("%s  \xE2\x80\x94  %s",
                        s.display_name.c_str(), s.reason.c_str());
            ImGui::PopID();
        }
    }
}

// ===== Sources tab ====================================================

void DrawSourcesTab(PanelState* state, const FrameSnapshot& snap)
{
    ImGui::TextWrapped("Drag EXR or PNG files (from Explorer/Finder, "
                       "or AE's Project panel) onto this panel to add "
                       "sources. Switch which source the Staging tab "
                       "focuses on with the Use buttons below.");
    ImGui::Spacing();

    if (snap.scanning) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.f),
                           "Scanning... %s", snap.last_status.c_str());
    } else if (!snap.last_error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.f),
                           "%s", snap.last_error.c_str());
    }
    (void)state;

    ImGui::Spacing();
    if (snap.source_summaries.empty()) {
        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::TextDisabled("No sources loaded yet. Drop an EXR/PNG anywhere on the panel.");
        return;
    }

    if (ImGui::BeginTable("sources_table", 7,
            ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_BordersV |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Active",  ImGuiTableColumnFlags_WidthFixed, 56.f);
        ImGui::TableSetupColumn("File",    ImGuiTableColumnFlags_WidthStretch, 4.f);
        ImGui::TableSetupColumn("Size",    ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("Layers",  ImGuiTableColumnFlags_WidthFixed, 70.f);
        ImGui::TableSetupColumn("Scan frame", ImGuiTableColumnFlags_WidthFixed, 130.f);
        ImGui::TableSetupColumn("Loop",    ImGuiTableColumnFlags_WidthFixed, 64.f);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 80.f);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < snap.source_summaries.size(); ++i) {
            const SourceSummary& ss = snap.source_summaries[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(ss.source_id));

            ImGui::TableNextColumn();
            if (ss.is_active) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.f), "ACTIVE");
            else {
                if (ImGui::SmallButton("Use")) {
                    std::lock_guard<std::mutex> lk(state->mu);
                    state->active_source_index = static_cast<int>(i);
                    // Don't bump scan_generation here — that would
                    // release the renderer's SRVs while other sources'
                    // LayerInfos still reference them. Per-layer
                    // texture_ids stay valid; new source's layers
                    // (texture_id == 0) get fresh SRVs lazily.
                }
            }

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Basename(ss.path).c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ss.path.c_str());

            ImGui::TableNextColumn();
            ImGui::Text("%d x %d", ss.image_width, ss.image_height);

            ImGui::TableNextColumn();
            ImGui::Text("%zu", ss.layer_count);
            if (ss.skipped_count > 0 && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Plus %zu skipped", ss.skipped_count);
            }

            // Scan-frame picker: re-scan a different frame of the
            // sequence in place. Only one frame is needed for centroid
            // / sort / thumbnails — the chase is the animation layered
            // on top, not the underlying scene motion.
            ImGui::TableNextColumn();
            if (ss.frame_count > 1) {
                const bool busy = snap.scanning;
                ImGui::BeginDisabled(busy || ss.scan_frame <= 0);
                if (ImGui::SmallButton("<")) {
                    exr_scan::StartScan(ss.path, state, /*append=*/true,
                                        ss.source_id, ss.scan_frame - 1);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::Text("%d/%d", ss.scan_frame + 1, ss.frame_count);
                if (ImGui::IsItemHovered() && !ss.scan_path.empty())
                    ImGui::SetTooltip("Scanned: %s", ss.scan_path.c_str());
                ImGui::SameLine();
                ImGui::BeginDisabled(busy ||
                                     ss.scan_frame >= ss.frame_count - 1);
                if (ImGui::SmallButton(">")) {
                    exr_scan::StartScan(ss.path, state, /*append=*/true,
                                        ss.source_id, ss.scan_frame + 1);
                }
                ImGui::EndDisabled();
            } else {
                ImGui::TextDisabled("single");
            }

            // Animation toggle: when on, chases from this source are
            // seamless loops over its duration (footage time-locked,
            // envelope sweeps). Auto-on for sequences; override here.
            ImGui::TableNextColumn();
            {
                bool anim = ss.animation;
                if (ImGui::Checkbox("##anim", &anim)) {
                    std::lock_guard<std::mutex> lk(state->mu);
                    if (Source* s = FindSourceById(*state, ss.source_id))
                        s->animation = anim;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        anim ? "Animation: seamless loop over %d frames "
                               "(footage time-locked)"
                             : "Treated as a still (shift-in-time chase)",
                        ss.frame_count);
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove")) {
                std::lock_guard<std::mutex> lk(state->mu);
                if (i < state->sources.size()) {
                    state->sources.erase(state->sources.begin() + i);
                    if (state->active_source_index >= (int)state->sources.size()) {
                        state->active_source_index = state->sources.empty() ? -1 : 0;
                    }
                    // Removed source's SRVs in the renderer cache
                    // leak until the next scan bumps the generation;
                    // small and rare in practice (few sources per
                    // session), tolerable for now.
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ===== Staging tab ====================================================

void DrawStagingTab(PanelState* state, FrameSnapshot& snap, float w, float h)
{
    if (snap.layers.empty()) {
        ImGui::Dummy(ImVec2(0.f, 8.f));
        ImGui::TextDisabled("Active source has no layers yet. Add an EXR in the Sources tab.");
        if (!snap.skipped.empty()) {
            DrawSkippedSection(state, snap.active_source_id, snap.skipped);
        }
        return;
    }
    const int included_now = IncludedCount(snap.layers);
    TickPreview(state, included_now);

    int highlight_full_index = -1;
    if (state->preview_playing && included_now > 0) {
        highlight_full_index = FullIndexOfIncluded(snap.layers,
                                                   state->preview_index);
    }
    const int selected_index = IndexOfHash(snap.layers, state->selected_hash);

    // Status line
    ImGui::Text("Image: %d x %d  •  %d/%zu included  •  %zu binds, %zu tags",
                snap.image_width, snap.image_height,
                included_now, snap.layers.size(),
                snap.binds.size(), snap.tags.size());

    // Filter row
    ImGui::SetNextItemWidth(180.f);
    ImGui::InputTextWithHint("##excludeFilter",
        "substring (e.g. fire)",
        state->exclude_filter, sizeof(state->exclude_filter));
    ImGui::SameLine();
    if (ImGui::Button("Exclude matching")) {
        for (auto& L : snap.layers) {
            if (ContainsCI(L.display_name, state->exclude_filter)) L.included = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Include matching")) {
        for (auto& L : snap.layers) {
            if (ContainsCI(L.display_name, state->exclude_filter)) L.included = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("All on"))  for (auto& L : snap.layers) L.included = true;
    ImGui::SameLine();
    if (ImGui::Button("All off")) for (auto& L : snap.layers) L.included = false;
    ImGui::SameLine();
    ImGui::Checkbox("Hide unchecked", &state->hide_unchecked);
    // Autotag-by-name now runs automatically when a source finishes
    // scanning (see exr_scan.cpp); no button needed.

    // Sort mode
    static const char* kSortLabels[] = {
        "Centroid (Left to Right)",
        "Centroid (Top to Bottom)",
        "Hotspot (Left to Right)",
        "Hotspot (Top to Bottom)",
        "Brightness (Brightest first)",
        "Radial Sweep (angle around center)",
        "Distance from Center (in to out)",
        "Random",
        "Alphabetical (A \xE2\x86\x92 Z)",
        "Included first",
        "Tag name (A \xE2\x86\x92 Z)",
        "Effective X (override-aware)",
        "Effective Y (override-aware)",
    };
    int sort_choice = static_cast<int>(state->sort_mode);
    ImGui::SetNextItemWidth(260.f);
    bool resort_now = false;
    if (ImGui::Combo("sort by", &sort_choice, kSortLabels,
                     IM_ARRAYSIZE(kSortLabels))) {
        state->sort_mode = static_cast<SortMode>(sort_choice);
        if (state->sort_mode == SortMode::Random) {
            std::random_device rd;
            state->random_seed = rd();
        }
        resort_now = true;
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("reverse", &state->sort_reverse)) resort_now = true;
    if (state->sort_mode == SortMode::Random) {
        ImGui::SameLine();
        if (ImGui::Button("Re-shuffle")) {
            std::random_device rd;
            state->random_seed = rd();
            resort_now = true;
        }
    }
    if (resort_now) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (Source* src = ActiveSource(*state)) {
            SortLayers(src->layers, state->sort_mode,
                       state->sort_reverse, state->random_seed,
                       state, src->source_id);
        }
    }

    // Preview controls
    const char* play_label = state->preview_playing ? "Pause preview" : "Play preview";
    if (ImGui::Button(play_label)) {
        state->preview_playing = !state->preview_playing;
        state->preview_accum_ms = 0.f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        state->preview_index = 0;
        state->preview_accum_ms = 0.f;
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.f);
    ImGui::SliderFloat("ms/light", &state->preview_ms_step,
                       20.f, 2000.f, "%.0f ms",
                       ImGuiSliderFlags_Logarithmic);

    ImGui::Separator();

    // ---- Table + canvas split pane ----
    const float pane_h = std::max(120.f, ImGui::GetContentRegionAvail().y - 40.f);
    const float canvas_w = std::min(360.f, std::max(160.f, w * 0.36f));
    const float table_w  = std::max(200.f,
                            ImGui::GetContentRegionAvail().x - canvas_w - 12.f);

    ImGui::BeginChild("table_pane", ImVec2(table_w, pane_h), false);
    const bool show_source_col = snap.source_summaries.size() > 1;
    TableInteractionResult ti = DrawLayersTable(
        snap.layers, snap.active_source_id, snap.binds, snap.tags,
        snap.position_overrides, state->selected_hashes,
        highlight_full_index, selected_index,
        state->sort_mode, show_source_col, snap.hide_unchecked,
        pane_h - 8.f);
    if (ti.clicked_row >= 0 && ti.clicked_row < (int)snap.layers.size()) {
        const uint32_t clicked_hash = snap.layers[ti.clicked_row].fnv1a_hash;
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyShift && state->last_selected_hash != 0) {
            // Range select from last_selected to this hash in the
            // current sorted order. Add (don't replace) so combining
            // Shift with Ctrl keeps prior selection.
            int a_idx = IndexOfHash(snap.layers, state->last_selected_hash);
            int b_idx = ti.clicked_row;
            if (a_idx > b_idx) std::swap(a_idx, b_idx);
            if (a_idx < 0) a_idx = b_idx;  // last_selected no longer present
            if (!io.KeyCtrl) state->selected_hashes.clear();
            for (int k = a_idx; k <= b_idx; ++k) {
                uint32_t h = snap.layers[k].fnv1a_hash;
                bool already = false;
                for (uint32_t e : state->selected_hashes) if (e == h) { already = true; break; }
                if (!already) state->selected_hashes.push_back(h);
            }
            state->selected_hash = clicked_hash;
        } else if (io.KeyCtrl) {
            // Toggle this row in the selection set.
            bool removed = false;
            for (auto it = state->selected_hashes.begin();
                 it != state->selected_hashes.end(); )
            {
                if (*it == clicked_hash) {
                    it = state->selected_hashes.erase(it);
                    removed = true;
                } else ++it;
            }
            if (!removed) state->selected_hashes.push_back(clicked_hash);
            state->selected_hash = clicked_hash;
            state->last_selected_hash = clicked_hash;
        } else {
            // Plain click — single-select.
            state->selected_hashes.clear();
            state->selected_hashes.push_back(clicked_hash);
            state->selected_hash = clicked_hash;
            state->last_selected_hash = clicked_hash;
        }
    }
    // Layer header click → alphabetical sort. Re-click toggles
    // direction.
    if (ti.header_clicked_col == TableCol::Layer) {
        if (state->sort_mode == SortMode::Alphabetical) {
            state->sort_reverse = !state->sort_reverse;
        } else {
            state->sort_mode = SortMode::Alphabetical;
            state->sort_reverse = false;
        }
        std::lock_guard<std::mutex> lk(state->mu);
        if (Source* src = ActiveSource(*state)) {
            SortLayers(src->layers, state->sort_mode,
                       state->sort_reverse, state->random_seed,
                       state, src->source_id);
        }
    }
    // Column header clicks for On / Tags / X / Y — toggle direction
    // on re-click of the same column, else set new mode.
    auto column_header_click = [&](SortMode m) {
        if (state->sort_mode == m) state->sort_reverse = !state->sort_reverse;
        else { state->sort_mode = m; state->sort_reverse = false; }
        std::lock_guard<std::mutex> lk(state->mu);
        if (Source* src = ActiveSource(*state)) {
            SortLayers(src->layers, state->sort_mode,
                       state->sort_reverse, state->random_seed,
                       state, src->source_id);
        }
    };
    if (ti.header_clicked_col == TableCol::On)   column_header_click(SortMode::IncludedFirst);
    if (ti.header_clicked_col == TableCol::Tags) column_header_click(SortMode::TagName);
    if (ti.header_clicked_col == TableCol::X)    column_header_click(SortMode::EffectiveX);
    if (ti.header_clicked_col == TableCol::Y)    column_header_click(SortMode::EffectiveY);
    // Apply drag-to-reorder.
    if (ti.drag_from >= 0 && ti.drag_to >= 0 &&
        ti.drag_from < (int)snap.layers.size() &&
        ti.drag_to   < (int)snap.layers.size())
    {
        LayerInfo moved = snap.layers[ti.drag_from];
        snap.layers.erase(snap.layers.begin() + ti.drag_from);
        // After erase, indices >= drag_from shifted by 1
        int target = ti.drag_to;
        if (ti.drag_from < ti.drag_to) target = ti.drag_to - 1;
        if (target < 0) target = 0;
        if (target > (int)snap.layers.size()) target = (int)snap.layers.size();
        snap.layers.insert(snap.layers.begin() + target, std::move(moved));
        std::vector<std::string> order;
        order.reserve(snap.layers.size());
        for (const auto& L : snap.layers) order.push_back(L.display_name);
        PublishLayerReorder(state, order);
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("canvas_pane", ImVec2(canvas_w, pane_h), false);
    // Preview layer pick. Priority:
    //   0. Currently-being-dragged layer (so the user sees what
    //      they're moving in image-space while dragging the dot)
    //   1. Playback-highlighted layer
    //   2. User selection
    //   3. First included
    int preview_layer = -1;
    if (state->drag_layer_hash != 0) {
        for (int i = 0; i < (int)snap.layers.size(); ++i) {
            if (snap.layers[i].fnv1a_hash == state->drag_layer_hash) {
                preview_layer = i; break;
            }
        }
    }
    if (preview_layer < 0) preview_layer = highlight_full_index;
    if (preview_layer < 0 && selected_index >= 0) preview_layer = selected_index;
    if (preview_layer < 0) {
        for (int i = 0; i < (int)snap.layers.size(); ++i) {
            if (snap.layers[i].included) { preview_layer = i; break; }
        }
        if (preview_layer < 0 && !snap.layers.empty()) preview_layer = 0;
    }

    if (preview_layer >= 0 && preview_layer < (int)snap.layers.size()) {
        const LayerInfo& L = snap.layers[preview_layer];
        ImGui::TextDisabled("preview: %s", L.display_name.c_str());
        const float pane_w = ImGui::GetContentRegionAvail().x;
        if (L.texture_id != 0 && L.thumb_w > 0 && L.thumb_h > 0) {
            const float aspect = float(L.thumb_h) / float(L.thumb_w);
            float tw = pane_w;
            float th = tw * aspect;
            const float max_h = pane_h * 0.55f;
            if (th > max_h) { th = max_h; tw = th / aspect; }
            const ImVec2 img_p_min = ImGui::GetCursorScreenPos();
            ImGui::Image((ImTextureID)L.texture_id, ImVec2(tw, th));
            // Mirror the drag cursor onto the thumbnail so the user
            // sees where they're placing the override in image space.
            if (state->drag_layer_hash == L.fnv1a_hash &&
                state->drag_layer_source_id == snap.active_source_id)
            {
                const float gx = img_p_min.x + state->drag_cursor_x * tw;
                const float gy = img_p_min.y + state->drag_cursor_y * th;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddCircleFilled(ImVec2(gx, gy), 7.f,
                                    IM_COL32(255, 240, 160, 110));
                dl->AddCircle(ImVec2(gx, gy), 7.f,
                              IM_COL32(255, 240, 160, 255), 0, 2.f);
                // Crosshair for precision.
                dl->AddLine(ImVec2(gx - 12.f, gy), ImVec2(gx - 4.f, gy),
                            IM_COL32(255, 240, 160, 200), 1.f);
                dl->AddLine(ImVec2(gx + 4.f, gy), ImVec2(gx + 12.f, gy),
                            IM_COL32(255, 240, 160, 200), 1.f);
                dl->AddLine(ImVec2(gx, gy - 12.f), ImVec2(gx, gy - 4.f),
                            IM_COL32(255, 240, 160, 200), 1.f);
                dl->AddLine(ImVec2(gx, gy + 4.f), ImVec2(gx, gy + 12.f),
                            IM_COL32(255, 240, 160, 200), 1.f);
            }
        } else {
            ImGui::Dummy(ImVec2(pane_w, 40.f));
            ImGui::TextDisabled("(thumbnail not ready)");
        }
    } else {
        ImGui::TextDisabled("preview: (no layers)");
    }
    ImGui::Separator();
    ImGui::TextDisabled("Centroid map  (yellow = included)");
    DrawCentroidCanvas(snap.layers, snap.active_source_id,
                       snap.position_overrides,
                       snap.image_width, snap.image_height,
                       highlight_full_index, selected_index,
                       state->sort_mode,
                       std::max(80.f, ImGui::GetContentRegionAvail().y - 8.f),
                       state);  // interactive: click-select + drag-to-override
    ImGui::EndChild();

    DrawSkippedSection(state, snap.active_source_id, snap.skipped);

    // ---- Context menu actions (bind, tag, override) ----
    // We track the right-clicked row and re-open the popup as a stable
    // window so multi-step menu items don't dismiss it prematurely.
    // If the right-click landed on a row that ISN'T currently in the
    // multi-select set, treat the right-click as a single-select on
    // that row (so the user sees the menu acting on the row they
    // clicked, not on a stale selection).
    if (ti.open_context_menu && ti.context_menu_row >= 0) {
        const uint32_t ctx_hash = snap.layers[ti.context_menu_row].fnv1a_hash;
        bool in_sel = false;
        for (uint32_t h : state->selected_hashes) {
            if (h == ctx_hash) { in_sel = true; break; }
        }
        if (!in_sel) {
            state->selected_hashes.clear();
            state->selected_hashes.push_back(ctx_hash);
            state->last_selected_hash = ctx_hash;
        }
        state->selected_hash = ctx_hash;
        ImGui::OpenPopup("row_actions");
    }
    if (ImGui::BeginPopup("row_actions")) {
        // Collect the active selection as a set of LayerRefs.
        std::vector<LayerRef> sel_refs;
        for (uint32_t h : state->selected_hashes) {
            sel_refs.push_back(LayerRef{ snap.active_source_id, h });
        }
        // Expand any bound member to include all its bind siblings —
        // tag / bind / override operations act on the whole bind.
        auto expand_to_bind_members = [&](std::vector<LayerRef>& v) {
            std::vector<LayerRef> out;
            std::unordered_set<uint64_t> seen;
            for (const auto& r : v) {
                if (const Bind* b = BindOfLayer(*state, r)) {
                    for (const auto& m : b->members) {
                        uint64_t key = (uint64_t(m.source_id) << 32) | m.fnv1a_hash;
                        if (seen.insert(key).second) out.push_back(m);
                    }
                } else {
                    uint64_t key = (uint64_t(r.source_id) << 32) | r.fnv1a_hash;
                    if (seen.insert(key).second) out.push_back(r);
                }
            }
            v = std::move(out);
        };

        const int N = static_cast<int>(sel_refs.size());
        if (N > 0) {
            // Header — show selection size + focus name.
            const LayerInfo* focus_L = nullptr;
            for (const auto& Li : snap.layers) {
                if (Li.fnv1a_hash == state->selected_hash) { focus_L = &Li; break; }
            }
            if (N == 1 && focus_L) {
                ImGui::TextDisabled("%s", focus_L->display_name.c_str());
            } else {
                ImGui::TextDisabled("%d rows selected", N);
            }
            ImGui::Separator();

            // ---- Bind submenu ----
            if (ImGui::BeginMenu("Bind...")) {
                ImGui::BeginDisabled(N < 2);
                if (ImGui::MenuItem("Bind selected together (new bind)")) {
                    std::lock_guard<std::mutex> lk(state->mu);
                    // De-duplicate and avoid making a bind that
                    // overlaps existing bind members; if any selected
                    // ref is already in a bind, pull its bind siblings
                    // out and absorb them into the new bind.
                    std::vector<LayerRef> all = sel_refs;
                    expand_to_bind_members(all);
                    // Remove members from any existing binds first.
                    for (auto& bex : state->binds) {
                        for (auto it = bex.members.begin(); it != bex.members.end(); ) {
                            bool drop = false;
                            for (const auto& nm : all) if (nm == *it) { drop = true; break; }
                            if (drop) it = bex.members.erase(it); else ++it;
                        }
                    }
                    // Discard now-empty binds.
                    for (auto it = state->binds.begin(); it != state->binds.end(); ) {
                        if (it->members.empty()) it = state->binds.erase(it); else ++it;
                    }
                    Bind nb;
                    nb.bind_id = state->next_bind_id++;
                    nb.name    = "Bind " + std::to_string(state->binds.size() + 1);
                    nb.color   = ColorForId(nb.bind_id);
                    nb.members = std::move(all);
                    state->binds.push_back(std::move(nb));
                }
                ImGui::EndDisabled();
                // Unbind: if any selected row belongs to a bind, allow
                // pulling those layers back out. If all selected rows
                // belong to the same single bind, this dissolves that
                // bind cleanly.
                bool any_bound = false;
                for (const auto& r : sel_refs) {
                    if (BindOfLayer(*state, r)) { any_bound = true; break; }
                }
                ImGui::BeginDisabled(!any_bound);
                if (ImGui::MenuItem("Unbind selected")) {
                    std::lock_guard<std::mutex> lk(state->mu);
                    for (auto& b : state->binds) {
                        for (auto it = b.members.begin(); it != b.members.end(); ) {
                            bool drop = false;
                            for (const auto& r : sel_refs) if (*it == r) { drop = true; break; }
                            if (drop) it = b.members.erase(it); else ++it;
                        }
                    }
                    for (auto it = state->binds.begin(); it != state->binds.end(); ) {
                        if (it->members.empty()) it = state->binds.erase(it); else ++it;
                    }
                }
                ImGui::EndDisabled();
                ImGui::EndMenu();
            }

            // ---- Tag submenu ----
            if (ImGui::BeginMenu("Tag...")) {
                static char new_tag_buf[64] = {};
                ImGui::InputTextWithHint("##newtag", "new tag name",
                                         new_tag_buf, sizeof(new_tag_buf));
                ImGui::SameLine();
                if (ImGui::Button("Create + assign")) {
                    if (new_tag_buf[0]) {
                        std::vector<LayerRef> all = sel_refs;
                        expand_to_bind_members(all);
                        Tag t;
                        t.tag_id = state->next_tag_id++;
                        t.name   = new_tag_buf;
                        t.color  = ColorForId(t.tag_id);
                        t.members = std::move(all);
                        std::lock_guard<std::mutex> lk(state->mu);
                        state->tags.push_back(std::move(t));
                        new_tag_buf[0] = 0;
                    }
                }
                ImGui::Separator();
                for (auto& t : state->tags) {
                    // Tristate: tag applies to all / some / none of the
                    // current selection (expanded to bind members).
                    std::vector<LayerRef> all = sel_refs;
                    expand_to_bind_members(all);
                    int in_count = 0;
                    for (const auto& r : all) {
                        for (const auto& m : t.members) {
                            if (m == r) { ++in_count; break; }
                        }
                    }
                    const char* mark = (in_count == (int)all.size())
                        ? "[\xE2\x9C\x93]"
                        : (in_count > 0 ? "[~]" : "[ ]");
                    char label[160];
                    std::snprintf(label, sizeof(label), "%s %s",
                                  mark, t.name.c_str());
                    if (ImGui::MenuItem(label)) {
                        std::lock_guard<std::mutex> lk(state->mu);
                        const bool all_in = (in_count == (int)all.size());
                        if (all_in) {
                            // Remove all selection from tag.
                            for (auto it = t.members.begin(); it != t.members.end(); ) {
                                bool drop = false;
                                for (const auto& r : all) if (*it == r) { drop = true; break; }
                                if (drop) it = t.members.erase(it); else ++it;
                            }
                        } else {
                            for (const auto& r : all) {
                                bool already = false;
                                for (const auto& m : t.members) if (m == r) { already = true; break; }
                                if (!already) t.members.push_back(r);
                            }
                        }
                    }
                }
                ImGui::EndMenu();
            }

            // ---- Position override (single-row only) ----
            if (N == 1 && focus_L) {
                LayerRef ref{ snap.active_source_id, focus_L->fnv1a_hash };
                if (ImGui::BeginMenu("Position override...")) {
                    PositionOverride* found = nullptr;
                    for (auto& po : state->position_overrides) {
                        if (po.layer == ref) { found = &po; break; }
                    }
                    float cx = found ? found->cx : LayerDisplayX(*focus_L, state->sort_mode);
                    float cy = found ? found->cy : LayerDisplayY(*focus_L, state->sort_mode);
                    float cz = found ? found->cz : 0.f;
                    bool changed = false;
                    ImGui::SetNextItemWidth(120.f);
                    if (ImGui::SliderFloat("X##ox", &cx, 0.f, 1.f, "%.3f")) changed = true;
                    ImGui::SetNextItemWidth(120.f);
                    if (ImGui::SliderFloat("Y##oy", &cy, 0.f, 1.f, "%.3f")) changed = true;
                    ImGui::SetNextItemWidth(120.f);
                    if (ImGui::SliderFloat("Z##oz", &cz, -1.f, 1.f, "%.3f")) changed = true;
                    if (changed) {
                        std::lock_guard<std::mutex> lk(state->mu);
                        if (!found) {
                            state->position_overrides.push_back(
                                PositionOverride{ ref, cx, cy, cz });
                        } else {
                            found->cx = cx; found->cy = cy; found->cz = cz;
                        }
                    }
                    if (found && ImGui::MenuItem("Clear override")) {
                        std::lock_guard<std::mutex> lk(state->mu);
                        for (auto it = state->position_overrides.begin();
                             it != state->position_overrides.end(); )
                        {
                            if (it->layer == ref) it = state->position_overrides.erase(it);
                            else ++it;
                        }
                    }
                    ImGui::EndMenu();
                }
            }
        }
        ImGui::EndPopup();
    }
}

// Middle-click-on-last-item reset. Use immediately after a slider /
// drag / input:
//   ImGui::SliderFloat(...); if (MiddleClickReset(value, default)) changed = true;
// Matches the AE "right-click param → reset" muscle memory but
// middle-click since right-click is already our row context menu.
template <typename T>
bool MiddleClickReset(T& value, T default_value)
{
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        value = default_value;
        return true;
    }
    return false;
}

// ===== Chase preview compositing ======================================

// Defined later; the wrapped (seamless-loop) triangle envelope.
float ScatterHitEnvelope(float playhead, float start, int loop_frames,
                         const ChaseTiming& t);

// Envelope value (0..1) for a stage at the given playhead frame.
// Linear triangle: rises from 0 at stage_start to 1 at attack, falls
// back to 0 at duration. Returns 0 when the stage is inactive.
float StageEnvelope(int stage_idx, float playhead, const ChaseTiming& t)
{
    const float stage_start = stage_idx * t.step_duration;
    const float local_t = playhead - stage_start;
    if (local_t < 0.f || local_t >= t.duration) return 0.f;
    const float dur = std::max(0.001f, t.duration);
    const float t01 = local_t / dur;
    float peak = std::max(0.001f, std::min(0.999f, t.attack / dur));
    if (t01 < peak) return t01 / peak;
    return (1.f - t01) / (1.f - peak);
}

// Total chase duration in frames: when the last stage's release ends.
float ChaseTotalDuration(const Chase& chase)
{
    if (chase.stages.empty()) return 0.f;
    return (chase.stages.size() - 1) * chase.timing.step_duration + chase.timing.duration;
}

// Per-stage envelope honoring loop-mode. In loop-mode (animation
// source) stages are evenly spaced across the seamless loop and the
// envelope wraps — no from-black ramp at t=0. `loop_frames <= 0`
// falls back to the legacy shift-in-time triangle (still images).
float ChaseStageEnvelopeAt(const Chase& chase, size_t si, float playhead,
                           int loop_frames)
{
    const size_t n = chase.stages.size();
    if (loop_frames > 0 && n > 0) {
        const float phase = static_cast<float>(si) *
            (static_cast<float>(loop_frames) / static_cast<float>(n));
        return ScatterHitEnvelope(playhead, phase, loop_frames, chase.timing);
    }
    return StageEnvelope(static_cast<int>(si), playhead, chase.timing);
}

// Loop-mode-aware total preview length in frames.
float ChaseTotalFrames(const Chase& chase, const PanelState& state, float fps)
{
    if (chase.random_scatter || ChaseLoopMode(state))
        return static_cast<float>(ChaseLoopFrames(chase, state, fps));
    return ChaseTotalDuration(chase);
}

// One weighted layer going into a composite. `k` is the envelope
// multiplier (opacity * gamma_at_t), applied linearly.
struct CompositeContribution { const LayerInfo* L; float k; };

// Lighten-composite weighted contributions, matching the AE build
// (black solid + Lighten — per-channel max, NOT additive). Thumbnails
// are normalized to each layer's OWN peak so weak lights stay visible
// in the layer table; that lies about relative brightness, so here we
// undo it via thumb_peak and renormalize against `shared_peak` (the
// brightest participating layer). Result: overlapping lights no
// longer stack to white, and a dim fill reads dimmer than the key —
// as it will in the AE comp. Still an approximation (the real
// Exposure effect is a per-channel pow, not a linear scale), but
// close and ~free.
bool LightenComposite(const std::vector<CompositeContribution>& contribs,
                      int out_w, int out_h, float shared_peak,
                      std::vector<uint8_t>& out_rgba)
{
    if (out_w <= 0 || out_h <= 0) return false;
    const size_t n = static_cast<size_t>(out_w) * out_h;
    std::vector<float> acc(n * 3, 0.f);            // linear, Lighten max
    const float sp = shared_peak > 1e-6f ? shared_peak : 1.f;

    for (const auto& c : contribs) {
        const LayerInfo* L = c.L;
        if (!L || L->thumb_rgba.empty()) continue;
        if (L->thumb_w != out_w || L->thumb_h != out_h) continue;
        if (c.k <= 0.f) continue;
        // thumb byte = sqrt(linear / thumb_peak) * 255, so
        // linear / shared_peak = (byte/255)^2 * thumb_peak/shared_peak.
        const float lp = L->thumb_peak > 0.f ? L->thumb_peak : 1.f;
        const float scale = (lp / sp) * c.k;
        const uint8_t* s = L->thumb_rgba.data();
        for (size_t i = 0; i < n; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const float u = s[i * 4 + ch] * (1.f / 255.f);
                const float lin = u * u * scale;
                float& a = acc[i * 3 + ch];
                if (lin > a) a = lin;              // Lighten
            }
        }
    }

    out_rgba.assign(n * 4, 0);
    for (size_t i = 0; i < n; ++i) {
        for (int ch = 0; ch < 3; ++ch) {
            float v = acc[i * 3 + ch];
            if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
            int q = static_cast<int>(std::sqrt(v) * 255.f + 0.5f);
            out_rgba[i * 4 + ch] =
                static_cast<uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
        }
        out_rgba[i * 4 + 3] = 255;
    }
    return true;
}

// Build the composite RGBA8 buffer for the chase at the current
// playhead. Sized to match the first usable thumbnail; ignores
// thumbs whose dimensions don't match (mixed-source case; not yet
// implemented). Returns true on success.
bool BuildChaseComposite(const Chase& chase, const PanelState& state,
                         float playhead,
                         std::vector<uint8_t>& out_rgba,
                         int& out_w, int& out_h)
{
    out_w = 0; out_h = 0;
    // Shared peak over ALL referenced layers (not just active ones)
    // so brightness doesn't flicker as stages turn on/off.
    float shared_peak = 0.f;
    for (const auto& stage : chase.stages) {
        for (const auto& ref : stage.members) {
            const LayerInfo* L = FindLayerByRef(state, ref);
            if (!L) continue;
            if (out_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
                out_w = L->thumb_w; out_h = L->thumb_h;
            }
            if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
        }
    }
    if (out_w == 0 || out_h == 0) return false;

    const int loop_frames = ChaseLoopMode(state)
        ? ChaseLoopFrames(chase, state, 0.f) : 0;
    std::vector<CompositeContribution> contribs;
    for (size_t si = 0; si < chase.stages.size(); ++si) {
        float env = ChaseStageEnvelopeAt(chase, si, playhead, loop_frames);
        if (env <= 0.f) continue;
        const float opacity = (chase.timing.opacity_peak / 100.f) * env;
        const float gamma_t = chase.timing.gamma_baseline +
            (chase.timing.gamma_peak - chase.timing.gamma_baseline) * env;
        const float k = opacity * gamma_t;
        if (k <= 0.f) continue;
        for (const auto& ref : chase.stages[si].members) {
            contribs.push_back({ FindLayerByRef(state, ref), k });
        }
    }
    return LightenComposite(contribs, out_w, out_h, shared_peak, out_rgba);
}

// Composite an explicit set of layer refs at full strength (no
// envelope). Used by the chase preview's click-to-pin: clicking a
// layer freezes the preview on just that light (or, if it sits in a
// multi-light stage, that whole stage). Same accumulation as
// BuildChaseComposite with k = 1.
bool BuildRefsComposite(const std::vector<LayerRef>& refs,
                        const PanelState& state,
                        std::vector<uint8_t>& out_rgba,
                        int& out_w, int& out_h)
{
    out_w = 0; out_h = 0;
    float shared_peak = 0.f;
    std::vector<CompositeContribution> contribs;
    for (const auto& ref : refs) {
        const LayerInfo* L = FindLayerByRef(state, ref);
        if (!L) continue;
        if (out_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
            out_w = L->thumb_w; out_h = L->thumb_h;
        }
        if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
        contribs.push_back({ L, 1.f });
    }
    if (out_w == 0 || out_h == 0) return false;
    return LightenComposite(contribs, out_w, out_h, shared_peak, out_rgba);
}

// ===== Chase JSON exporter ============================================

std::string JsonEscapeForChase(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

// Recompute the chase's stages from its sort + tag filter + the
// session's staged layers. Called once each frame from the chase
// editor so stages always reflect current state.
void RegenerateChaseStages(Chase& chase, const PanelState& state,
                           int stages_count_hint)
{
    // Random-scatter chases don't use the stage sequence at all —
    // they own `scatter` instead (see RegenerateScatter).
    if (chase.random_scatter) return;
    // Manual arrangement: the user owns `stages` (drag-reorder, weld,
    // split in the editor). Don't recompute — leave it as authored so
    // it survives the per-frame refresh and session round-trips.
    if (chase.manual_stages) return;
    chase.stages.clear();
    const Source* src = ActiveSource(state);
    if (!src) return;
    // Gather candidates from the active source filtered by tags.
    std::vector<LayerRef> candidates;
    for (const auto& L : src->layers) {
        if (!L.included) continue;
        LayerRef ref{ src->source_id, L.fnv1a_hash };
        if (!chase.tag_filter.empty()) {
            bool in_any = false;
            for (uint32_t tid : chase.tag_filter) {
                if (const Tag* t = FindTagById(state, tid)) {
                    for (const auto& m : t->members) {
                        if (m == ref) { in_any = true; break; }
                    }
                    if (in_any) break;
                }
            }
            if (!in_any) continue;
        }
        candidates.push_back(ref);
    }
    // Sort candidates per chase.sort_mode. Done over the LayerInfo
    // lookups, not a copy of LayerInfo, for clarity.
    auto less_by_mode = [&](const LayerRef& a, const LayerRef& b) {
        const LayerInfo* la = FindLayerByRef(state, a);
        const LayerInfo* lb = FindLayerByRef(state, b);
        if (!la || !lb) return false;
        switch (chase.sort_mode) {
        case SortMode::CentroidX:        return la->cx     < lb->cx;
        case SortMode::CentroidY:        return la->cy     < lb->cy;
        case SortMode::HotspotX:         return la->cx_hot < lb->cx_hot;
        case SortMode::HotspotY:         return la->cy_hot < lb->cy_hot;
        case SortMode::Brightness:       return la->total  > lb->total;
        case SortMode::RadialSweep: {
            float angA = std::atan2(la->cy - 0.5f, la->cx - 0.5f);
            float angB = std::atan2(lb->cy - 0.5f, lb->cx - 0.5f);
            return angA < angB;
        }
        case SortMode::DistanceFromCenter: {
            float da = (la->cx - 0.5f) * (la->cx - 0.5f) + (la->cy - 0.5f) * (la->cy - 0.5f);
            float db = (lb->cx - 0.5f) * (lb->cx - 0.5f) + (lb->cy - 0.5f) * (lb->cy - 0.5f);
            return da < db;
        }
        case SortMode::Random: {
            uint32_t ka = la->fnv1a_hash * 2654435761u + chase.random_seed;
            uint32_t kb = lb->fnv1a_hash * 2654435761u + chase.random_seed;
            return ka < kb;
        }
        case SortMode::Alphabetical:
            return la->display_name < lb->display_name;
        case SortMode::IncludedFirst:
            if (la->included != lb->included) return la->included > lb->included;
            return la->display_name < lb->display_name;
        case SortMode::TagName:
        case SortMode::EffectiveX:
        case SortMode::EffectiveY:
            // Chase regen uses the layer's intrinsic centroid for
            // these — they're staging-tab-only sort modes in practice.
            return la->display_name < lb->display_name;
        }
        return false;
    };
    std::sort(candidates.begin(), candidates.end(), less_by_mode);
    if (chase.sort_reverse) std::reverse(candidates.begin(), candidates.end());

    // Coalesce bind members into one "logical light" — each bind
    // appears once in the ordering, sorted by where its first member
    // landed. The bind expands into the stage as N LayerRefs (one
    // per member) so the chase comp fires all the bound lights
    // together with the same envelope.
    struct Lead {
        LayerRef              repr;
        std::vector<LayerRef> members;
    };
    std::vector<Lead> leads;
    std::unordered_set<uint32_t> bind_id_seen;
    leads.reserve(candidates.size());
    for (const auto& ref : candidates) {
        const Bind* b = BindOfLayer(state, ref);
        if (b) {
            if (bind_id_seen.insert(b->bind_id).second) {
                Lead L;
                L.repr = ref;
                L.members = b->members;
                leads.push_back(std::move(L));
            }
            // Else: this bind already represented; skip the dup.
        } else {
            Lead L;
            L.repr = ref;
            L.members.push_back(ref);
            leads.push_back(std::move(L));
        }
    }

    // Symmetric center-out: order is already set by sort_mode (the
    // "Center Out" template uses CentroidX = left->right). Build
    // stages from the middle outward — the center light(s) fire
    // alone, then each mirrored pair welds into one stage. For an
    // even count the two innermost lights share the first stage.
    // Lights 1..5 (L->R) => [3], [2,4], [1,5]. Ignores
    // desired_stage_count by design.
    if (chase.symmetric_pairs) {
        const int n = static_cast<int>(leads.size());
        const int lo = (n - 1) / 2;
        const int hi = n / 2;
        for (int k = 0; ; ++k) {
            const int i = lo - k;
            const int j = hi + k;
            if (i < 0 || j >= n) break;
            ChaseStage st;
            for (const auto& m : leads[i].members) st.members.push_back(m);
            if (j != i) {
                for (const auto& m : leads[j].members) st.members.push_back(m);
            }
            chase.stages.push_back(std::move(st));
        }
        return;
    }

    // Group leads into stages. desired_stage_count counts LOGICAL
    // lights, not individual layers, so a 3-stage chase with binds
    // still produces 3 stages.
    int per_stage = 1;
    if (stages_count_hint > 0 && (int)leads.size() > stages_count_hint) {
        per_stage = ((int)leads.size() + stages_count_hint - 1) / stages_count_hint;
    }
    ChaseStage cur;
    int placed = 0;
    for (const auto& L : leads) {
        for (const auto& m : L.members) cur.members.push_back(m);
        ++placed;
        if (placed >= per_stage) {
            chase.stages.push_back(std::move(cur));
            cur = ChaseStage{};
            placed = 0;
        }
    }
    if (!cur.members.empty()) chase.stages.push_back(std::move(cur));
}

// ===== Random-scatter preview ========================================
// ScatterLoopFrames + RegenerateScatter are shared inlines in
// panel_state.h (the AE builder regenerates too — see note there).

// Envelope (0..1) of a scattered hit at `playhead`, seamless-wrapped:
// a hit whose tail crosses the loop end re-enters at the start.
float ScatterHitEnvelope(float playhead, float start, int loop_frames,
                         const ChaseTiming& t)
{
    float local = std::fmod(playhead - start,
                            static_cast<float>(loop_frames));
    if (local < 0.f) local += static_cast<float>(loop_frames);
    if (local >= t.duration) return 0.f;
    const float dur  = std::max(0.001f, t.duration);
    const float t01  = local / dur;
    const float peak = std::max(0.001f, std::min(0.999f, t.attack / dur));
    if (t01 < peak) return t01 / peak;
    return (1.f - t01) / (1.f - peak);
}

// Composite a scatter chase at `playhead` (loop-relative frames).
// Same accumulation as BuildChaseComposite; per-hit wrapped envelope.
bool BuildScatterComposite(const Chase& chase, const PanelState& state,
                           float playhead, float fps,
                           std::vector<uint8_t>& out_rgba,
                           int& out_w, int& out_h)
{
    out_w = 0; out_h = 0;
    const int loop_frames = ChaseLoopFrames(chase, state, fps);
    float shared_peak = 0.f;
    for (const auto& hit : chase.scatter) {
        const LayerInfo* L = FindLayerByRef(state, hit.ref);
        if (!L) continue;
        if (out_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
            out_w = L->thumb_w; out_h = L->thumb_h;
        }
        if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
    }
    if (out_w == 0 || out_h == 0) return false;

    std::vector<CompositeContribution> contribs;
    for (const auto& hit : chase.scatter) {
        const float env = ScatterHitEnvelope(playhead, hit.start_frame,
                                              loop_frames, chase.timing);
        if (env <= 0.f) continue;
        const float opacity = (chase.timing.opacity_peak / 100.f) * env;
        const float gamma_t = chase.timing.gamma_baseline +
            (chase.timing.gamma_peak - chase.timing.gamma_baseline) * env;
        const float k = opacity * gamma_t;
        if (k <= 0.f) continue;
        contribs.push_back({ FindLayerByRef(state, hit.ref), k });
    }
    return LightenComposite(contribs, out_w, out_h, shared_peak, out_rgba);
}

// Build a JSON string describing a chase. The companion JSX executor
// will read this and build the AE comp. Format is intentionally
// human-readable.
std::string BuildChaseExportJson(const Chase& chase, const PanelState& state)
{
    std::string out;
    auto append = [&](const char* s) { out += s; };
    append("{\n");
    out += "  \"version\": 1,\n";
    out += "  \"name\": \"" + JsonEscapeForChase(chase.name) + "\",\n";
    out += "  \"sort_mode\": " + std::to_string((int)chase.sort_mode) + ",\n";
    out += "  \"sort_reverse\": ";
    out += chase.sort_reverse ? "true" : "false";
    out += ",\n";
    out += "  \"timing\": {\n";
    char tbuf[256];
    std::snprintf(tbuf, sizeof(tbuf),
        "    \"duration\": %.3f,\n"
        "    \"attack\": %.3f,\n"
        "    \"step_duration\": %.3f,\n"
        "    \"opacity_peak\": %.3f,\n"
        "    \"gamma_peak\": %.3f,\n"
        "    \"gamma_baseline\": %.3f\n",
        chase.timing.duration, chase.timing.attack,
        chase.timing.step_duration, chase.timing.opacity_peak,
        chase.timing.gamma_peak, chase.timing.gamma_baseline);
    out += tbuf;
    out += "  },\n";
    out += "  \"stages\": [\n";
    for (size_t si = 0; si < chase.stages.size(); ++si) {
        out += "    [\n";
        const auto& stage = chase.stages[si];
        for (size_t mi = 0; mi < stage.members.size(); ++mi) {
            const LayerRef& m = stage.members[mi];
            const LayerInfo* L = FindLayerByRef(state, m);
            const Source* src = FindSourceById(state, m.source_id);
            std::snprintf(tbuf, sizeof(tbuf),
                "      {\"source_id\": %u, \"layer_hash\": %u, "
                "\"source\": \"%s\", \"layer\": \"%s\"}%s\n",
                m.source_id, m.fnv1a_hash,
                src ? JsonEscapeForChase(Basename(src->path)).c_str() : "",
                L ? JsonEscapeForChase(L->display_name).c_str() : "",
                (mi + 1 < stage.members.size()) ? "," : "");
            out += tbuf;
        }
        out += (si + 1 < chase.stages.size()) ? "    ],\n" : "    ]\n";
    }
    out += "  ]\n";
    out += "}\n";
    return out;
}

// ===== Chase tab + wizard =============================================

const char* kChaseTemplateNames[] = {
    "Left to Right",
    "Top to Bottom",
    "Center Out",
    "3 Step Chase",
    "Random",
    "Custom",
};

void ApplyTemplateToChase(Chase& c, int template_idx)
{
    // Defaults: one light per stage, auto sort (template re-derives
    // stages from scratch, so any prior manual arrangement is
    // intentionally dropped). 3 Step / Center Out override below.
    c.desired_stage_count = 0;
    c.symmetric_pairs = false;
    c.manual_stages = false;
    c.random_scatter = false;
    switch (template_idx) {
    // Left→Right: Hotspot X (bright concentrated source, spill
    // ignored) is the default ordering basis for chases.
    case 0: c.sort_mode = SortMode::HotspotX;           c.sort_reverse = false; break;
    case 1: c.sort_mode = SortMode::CentroidY;          c.sort_reverse = false; break;
    case 2:
        // Center Out: order by HOTSPOT x (the bright concentrated
        // source, not the spill-influenced whole-layer centroid) so
        // the mirrored pairing is spatially symmetric, then weld
        // pairs outward from the middle. Even light counts pair the
        // two centermost together as stage 1 (see RegenerateChase
        // Stages' symmetric_pairs branch: lo=(n-1)/2, hi=n/2).
        c.sort_mode = SortMode::HotspotX;
        c.sort_reverse = false;
        c.symmetric_pairs = true;
        break;
    case 3:
        c.sort_mode = SortMode::HotspotX;
        c.sort_reverse = false;
        c.desired_stage_count = 3;
        break;
    case 4: {
        // Random = the stratified scatter generator (not a sort).
        c.random_scatter  = true;
        c.loop_seconds    = (c.loop_seconds > 0.f) ? c.loop_seconds : 10.0f;
        c.scatter_density = (c.scatter_density > 0) ? c.scatter_density : 5;
        std::random_device rd;
        c.random_seed = rd();
        break;
    }
    case 5: default: break; // custom — leave as-is
    }
}

void DrawWizardForChaseTab(PanelState* state, FrameSnapshot& snap, int chase_index)
{
    if (chase_index < 0 || chase_index >= (int)snap.chases.size()) return;
    Chase& cs = snap.chases[chase_index];

    ImGui::TextWrapped("Create a new chase. Pick a template, optional "
                       "tag filter, and the starting timing — you can "
                       "tweak everything afterward.");
    ImGui::Spacing();

    ImGui::SetNextItemWidth(220.f);
    ImGui::Combo("Template", &state->wizard_template_index,
                 kChaseTemplateNames, IM_ARRAYSIZE(kChaseTemplateNames));

    // Tag filter — multi-select. Empty selection = no filter (all
    // staged layers, including untagged). All / None reset buttons.
    auto wiz_has_tag = [&](uint32_t id) {
        for (uint32_t x : state->wizard_tag_filter) if (x == id) return true;
        return false;
    };
    ImGui::Text("Tag filter");
    ImGui::SameLine();
    ImGui::TextDisabled(state->wizard_tag_filter.empty()
        ? "(all layers — no filter)"
        : "(%d selected)", (int)state->wizard_tag_filter.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("All")) {
        state->wizard_tag_filter.clear();
        for (const auto& t : snap.tags) {
            state->wizard_tag_filter.push_back(t.tag_id);
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("None")) state->wizard_tag_filter.clear();
    ImGui::BeginChild("wizard_tag_list", ImVec2(260.f, 110.f), true);
    if (snap.tags.empty()) {
        ImGui::TextDisabled("(no tags — load a source / auto-tag)");
    }
    for (const auto& t : snap.tags) {
        bool sel = wiz_has_tag(t.tag_id);
        ImGui::PushID((int)t.tag_id);
        if (ImGui::Checkbox(t.name.c_str(), &sel)) {
            if (sel) {
                if (!wiz_has_tag(t.tag_id)) {
                    state->wizard_tag_filter.push_back(t.tag_id);
                }
            } else {
                for (auto it = state->wizard_tag_filter.begin();
                     it != state->wizard_tag_filter.end(); ++it) {
                    if (*it == t.tag_id) {
                        state->wizard_tag_filter.erase(it);
                        break;
                    }
                }
            }
        }
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Hit duration (frames)", &state->wizard_hit_duration,
                       1.f, 240.f, "%.0f");
    ImGui::SetNextItemWidth(220.f);
    ImGui::SliderFloat("Step duration (frames)", &state->wizard_step_duration,
                       0.5f, 60.f, "%.1f");

    ImGui::Spacing();
    if (ImGui::Button("Create chase", ImVec2(160.f, 0.f))) {
        // Mutate the real chase under lock.
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            Chase& real = state->chases[chase_index];
            // Smart naming: "<tag> — <template>" if a tag filter is
            // set, else just "<template>". The em-dash separates the
            // two without colliding with characters that aren't
            // filesystem-safe (the comp builder slugs `/`, `\`, etc.).
            std::string tmpl = kChaseTemplateNames[state->wizard_template_index];
            std::string base = tmpl;
            const auto& wtf = state->wizard_tag_filter;
            if (wtf.size() == 1) {
                for (const auto& t : state->tags) {
                    if (t.tag_id == wtf[0]) {
                        base = t.name + " \xE2\x80\x94 " + tmpl;
                        break;
                    }
                }
            } else if (wtf.size() > 1) {
                base = std::to_string(wtf.size()) + " tags \xE2\x80\x94 " + tmpl;
            }
            real.name = base;
            // Append " 2", " 3" if a chase already exists with this
            // exact name (e.g. user creates two "Fire — Left to Right").
            int dup = 0;
            for (const auto& other : state->chases) {
                if (&other != &real && other.name == real.name) ++dup;
            }
            if (dup > 0) real.name += " " + std::to_string(dup + 1);

            real.tag_filter = state->wizard_tag_filter;
            ApplyTemplateToChase(real, state->wizard_template_index);
            real.timing = state->default_timing;
            real.timing.duration = state->wizard_hit_duration;
            real.timing.step_duration = state->wizard_step_duration;
            state->default_timing.duration = state->wizard_hit_duration;
            state->default_timing.step_duration = state->wizard_step_duration;

            RegenerateChaseStages(real, *state, real.desired_stage_count);
            state->chase_in_wizard = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        std::lock_guard<std::mutex> lk(state->mu);
        // Delete the just-created chase.
        if (chase_index >= 0 && chase_index < (int)state->chases.size()) {
            state->chases.erase(state->chases.begin() + chase_index);
            state->active_chase_index = -1;
            state->active_tab = PanelTab::Staging;
            state->chase_in_wizard = false;
        }
    }
}

void DrawChaseEditor(PanelState* state, FrameSnapshot& snap, int chase_index,
                     float panel_w, float panel_h)
{
    if (chase_index < 0 || chase_index >= (int)snap.chases.size()) return;
    (void)panel_h;

    // ---- Always refresh stages from current config + staged set ----
    // Cheap (one sort + one grouping pass over the staged layers) and
    // keeps the chase reactive: change a tag in Staging, change sort
    // here, toggle inclusion — stages and composite stay in sync
    // without a manual "Regenerate" click.
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            Chase& real = state->chases[chase_index];
            RegenerateChaseStages(real, *state, real.desired_stage_count);
            if (chase_index < (int)snap.chases.size()) {
                snap.chases[chase_index].stages = real.stages;
                snap.chases[chase_index].desired_stage_count = real.desired_stage_count;
            }
        }
    }
    Chase& cs = snap.chases[chase_index];

    // ---- Header row: name + delete (with confirm) ----
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", cs.name.c_str());
    ImGui::SetNextItemWidth(260.f);
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            state->chases[chase_index].name = name_buf;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Build in AE")) {
        state->want_build_chase_index = chase_index;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Create an AE comp for this chase inside a 'ChaseMaker'\n"
            "folder at the project root (versioned _v02/_v03 if one\n"
            "already exists). One AE layer\n"
            "per stage member, with EXRDemux applied + hash params set\n"
            "to target the right EXR layer. Single AE undo step.\n"
            "\nKeyframes (Opacity + Exposure-Gamma envelopes) land in\n"
            "a follow-up pass; for now the layers are static.");
    }
    // Chase deletion lives on the tab's close (X) button now — see
    // the confirm modal driven by chase_pending_close_index after the
    // tab bar. (The old redundant "Delete chase..." button was
    // removed.)

    // ---- Manual-arrangement banner ----
    // When the user has hand-edited the stage list, the sort + stage
    // controls no longer drive it; surface that and offer a one-click
    // way back to auto.
    if (cs.manual_stages) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.30f, 1.f),
            "Manual arrangement \xE2\x80\x94 sort/stage controls disabled.");
        ImGui::SameLine();
        if (ImGui::Button("Reset to auto")) {
            std::lock_guard<std::mutex> lk(state->mu);
            if (chase_index < (int)state->chases.size()) {
                state->chases[chase_index].manual_stages = false;
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Discard the manual stage edits and rebuild\n"
                              "stages from the sort + stage settings.");
        }
    }

    // ---- Sort + stage grouping ----
    static const char* kSortLabels[] = {
        "Centroid X", "Centroid Y", "Hotspot X", "Hotspot Y",
        "Brightness", "Radial Sweep", "Distance from Center", "Random",
        "Alphabetical", "Included first", "Tag", "Effective X", "Effective Y",
    };
    ImGui::BeginDisabled(cs.manual_stages);
    int sort_choice = static_cast<int>(cs.sort_mode);
    bool sort_changed = false;
    ImGui::SetNextItemWidth(180.f);
    if (ImGui::Combo("Sort", &sort_choice, kSortLabels, IM_ARRAYSIZE(kSortLabels))) {
        sort_changed = true;
    }
    ImGui::SameLine();
    bool rev = cs.sort_reverse;
    if (ImGui::Checkbox("reverse", &rev)) sort_changed = true;
    ImGui::SameLine();
    // Stages slider max = number of LOGICAL lights (binds count as
    // one). Iterate current stages, treating each bind as one entry
    // regardless of member count.
    int total_eligible = 0;
    {
        std::unordered_set<uint32_t> binds_seen;
        for (const auto& stage : cs.stages) {
            for (const auto& m : stage.members) {
                const Bind* b = BindOfLayer(*state, m);
                if (b) {
                    if (binds_seen.insert(b->bind_id).second) ++total_eligible;
                } else {
                    ++total_eligible;
                }
            }
        }
    }
    int desired = cs.desired_stage_count;
    bool grouping_changed = false;
    ImGui::SetNextItemWidth(180.f);
    int max_stages = std::max(1, total_eligible);
    int display_desired = (desired <= 0) ? max_stages : desired;
    if (ImGui::SliderInt("Stages", &display_desired, 1, max_stages,
                         "%d", ImGuiSliderFlags_AlwaysClamp))
    {
        // Setting back to "one per stage" when slider hits N (the
        // implicit default) keeps the value as 0 internally so future
        // layer additions extend automatically.
        desired = (display_desired >= max_stages) ? 0 : display_desired;
        grouping_changed = true;
    }
    // Middle-click resets to "one light per stage" (desired=0).
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
    {
        desired = 0;
        display_desired = max_stages;
        grouping_changed = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("How many stages this chase has.\n"
                          "Max = N lights (one per stage).\n"
                          "Lower groups lights into chunks; e.g. 3 stages\n"
                          "with N=51 lights fires 17 lights per stage.\n"
                          "\nMiddle-click to reset to one-per-stage.");
    }
    ImGui::EndDisabled();

    // ---- Timing sliders (compact, two rows) ----
    // Middle-click any slider to reset it to its factory default.
    // Defaults match the ChaseTiming struct's in-class initialisers
    // and the original NWE Light Hit preset envelope shape.
    ImGui::Separator();
    ChaseTiming t = cs.timing;
    bool t_changed = false;
    const float slider_w = 150.f;
    ImGui::SetNextItemWidth(slider_w);
    if (ImGui::SliderFloat("Duration", &t.duration, 1.f, 240.f, "%.0f f")) t_changed = true;
    if (MiddleClickReset(t.duration, 30.0f)) t_changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(slider_w);
    if (ImGui::SliderFloat("Attack", &t.attack, 0.f, t.duration, "%.1f f")) t_changed = true;
    if (MiddleClickReset(t.attack, 5.0f)) t_changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(slider_w);
    const bool loop_mode = ChaseLoopMode(*state);
    if (loop_mode) {
        // Animation source: the loop length is the source duration and
        // step is derived (loop / stage count) so the chase stays
        // phase-locked to the scene. Step isn't user-editable here.
        const int   lf = ChaseLoopFrames(cs, *state, 0.f);
        const size_t n  = cs.stages.empty() ? 1 : cs.stages.size();
        const float derived_step = static_cast<float>(lf) /
                                   static_cast<float>(n);
        t.step_duration = derived_step;
        ImGui::BeginDisabled(true);
        float shown = derived_step;
        ImGui::SliderFloat("Step", &shown, 0.f, static_cast<float>(lf),
                           "%.2f f (auto)");
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "Seamless loop: %d-frame source \xC2\xB7 %zu stages \xC2\xB7 "
            "step %.2f f (loop divides the source exactly)",
            lf, n, derived_step);
    } else {
        if (ImGui::SliderFloat("Step", &t.step_duration, 0.5f, 60.f, "%.1f f")) t_changed = true;
        if (MiddleClickReset(t.step_duration, 4.0f)) t_changed = true;
    }
    // Opacity/gamma envelope tuned rarely — collapsed by default to
    // keep the common timing controls uncluttered.
    if (ImGui::TreeNode("Opacity / Gamma envelope")) {
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat("Opacity peak", &t.opacity_peak, 0.f, 100.f, "%.0f%%")) t_changed = true;
        if (MiddleClickReset(t.opacity_peak, 100.0f)) t_changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat("Gamma peak", &t.gamma_peak, 0.01f, 4.f, "%.2f")) t_changed = true;
        if (MiddleClickReset(t.gamma_peak, 1.0f)) t_changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(slider_w);
        if (ImGui::SliderFloat("Gamma base", &t.gamma_baseline, 0.01f, 4.f, "%.2f")) t_changed = true;
        if (MiddleClickReset(t.gamma_baseline, 0.25f)) t_changed = true;
        ImGui::TreePop();
    }

    if (sort_changed || t_changed || grouping_changed) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            Chase& real = state->chases[chase_index];
            real.sort_mode = static_cast<SortMode>(sort_choice);
            real.sort_reverse = rev;
            real.timing = t;
            real.desired_stage_count = desired;
            state->default_timing = t;
        }
    }

    // ---- Preview transport ----
    ImGui::Separator();
    const float fps = std::max(1.f, state->chase_preview_fps.load());
    const float total = ChaseTotalFrames(cs, *state, fps);
    if (state->chase_preview_frame > total) state->chase_preview_frame = 0.f;
    if (state->chase_preview_playing && total > 0.f) {
        state->chase_preview_frame += ImGui::GetIO().DeltaTime * fps;
        if (state->chase_preview_frame >= total) state->chase_preview_frame = 0.f;
    }
    const char* play_label = state->chase_preview_playing ? "Pause" : "Play";
    if (ImGui::Button(play_label)) state->chase_preview_playing = !state->chase_preview_playing;
    ImGui::SameLine();
    if (ImGui::Button("Reset")) state->chase_preview_frame = 0.f;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280.f);
    ImGui::SliderFloat("##scrub", &state->chase_preview_frame,
                       0.f, std::max(1.f, total), "frame %.1f");
    ImGui::SameLine();
    ImGui::TextDisabled("/ %.0f f (%.1f s @ %.0f fps)",
                        total, total / fps, fps);

    // ---- Build composite ----
    // Click-to-pin: while the preview is NOT playing and the user
    // has a layer selected (row or centroid click) that belongs to
    // this chase, freeze the preview on that layer's whole stage
    // (so a multi-light / bound stage shows all its members). Once
    // the user presses Play, the animated composite takes over.
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool built = false;
        if (!state->chase_preview_playing && state->selected_hash != 0) {
            for (const auto& st : cs.stages) {
                bool here = false;
                for (const auto& m : st.members) {
                    if (m.fnv1a_hash == state->selected_hash) { here = true; break; }
                }
                if (here) {
                    built = BuildRefsComposite(st.members, *state, rgba, w, h);
                    break;
                }
            }
        }
        if (!built) {
            built = BuildChaseComposite(cs, *state,
                                        state->chase_preview_frame, rgba, w, h);
        }
        if (built) {
            state->chase_composite_rgba = std::move(rgba);
            state->chase_composite_w = w;
            state->chase_composite_h = h;
            state->chase_composite_dirty = true;
        } else {
            state->chase_composite_rgba.clear();
            state->chase_composite_w = 0;
            state->chase_composite_h = 0;
            state->chase_composite_texture_id = 0;
        }
    }

    // ---- Identify active stages for highlighting ----
    std::vector<float> stage_envelopes(cs.stages.size(), 0.f);
    int   active_stage_top = -1;
    float active_top_env = 0.f;
    const int hl_loop_frames = ChaseLoopMode(*state)
        ? ChaseLoopFrames(cs, *state, fps) : 0;
    for (size_t si = 0; si < cs.stages.size(); ++si) {
        stage_envelopes[si] = ChaseStageEnvelopeAt(
            cs, si, state->chase_preview_frame, hl_loop_frames);
        if (stage_envelopes[si] > active_top_env) {
            active_top_env = stage_envelopes[si];
            active_stage_top = static_cast<int>(si);
        }
    }

    // ---- Split pane: stages table | composite preview + centroid map ----
    ImGui::Separator();
    const float pane_h = std::max(160.f, ImGui::GetContentRegionAvail().y - 8.f);
    const float right_w = std::min(420.f, std::max(200.f, panel_w * 0.42f));
    const float left_w  = std::max(220.f,
                            ImGui::GetContentRegionAvail().x - right_w - 12.f);

    // Stages table (left). One row per layer, Staging-style:
    // shift/ctrl multi-select, drag to reorder, right-click to
    // Bind / Unbind. A multi-light stage = a chase-local "bind"
    // (same UX + name as Staging, but it only welds *this* chase;
    // it is not a session bind). Any edit flips the chase to manual.
    ImGui::BeginChild("chase_stages", ImVec2(left_w, pane_h), false);
    ImGui::TextDisabled("Click to preview \xC2\xB7 shift/ctrl multi-select "
                        "\xC2\xB7 drag to reorder \xC2\xB7 right-click to "
                        "Bind \xC2\xB7 edits switch this chase to manual");

    // Flatten stages -> per-layer rows.
    struct FlatRow { LayerRef ref; int stage; bool first; int group; };
    std::vector<FlatRow> flat;
    for (size_t si = 0; si < cs.stages.size(); ++si) {
        const auto& mem = cs.stages[si].members;
        for (size_t mi = 0; mi < mem.size(); ++mi) {
            flat.push_back({ mem[mi], static_cast<int>(si),
                             mi == 0, static_cast<int>(mem.size()) });
        }
    }
    auto is_sel = [&](uint32_t h) {
        for (uint32_t x : state->selected_hashes) if (x == h) return true;
        return false;
    };

    // Deferred edits (applied under the lock after the table; the
    // table iterates the snap copy so mid-loop mutation would
    // invalidate indices). Refs, not indices, so they survive.
    bool      do_move = false;
    LayerRef  move_ref{}, before_ref{};
    bool      open_ctx = false;
    uint32_t  ctx_hash = 0;

    constexpr ImGuiTableFlags kTblFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("chase_stages_table", 3, kTblFlags,
                          ImVec2(0.f, pane_h - 8.f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthFixed, 48.f);
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn("Env", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableHeadersRow();
        for (int r = 0; r < (int)flat.size(); ++r) {
            const FlatRow& fr = flat[r];
            const float env = (fr.stage < (int)stage_envelopes.size())
                              ? stage_envelopes[fr.stage] : 0.f;
            const LayerInfo* L = FindLayerByRef(*state, fr.ref);
            ImGui::TableNextRow();
            if (env > 0.f) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    IM_COL32(120, 80, 30,
                             static_cast<int>(40 + 120 * env)));
            } else if (fr.group > 1) {
                // Faint stable stripe so a chase-local bind reads as
                // one contiguous block.
                ImU32 c = ColorForId(static_cast<uint32_t>(fr.stage) + 1);
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    (c & 0x00FFFFFFu) | 0x33000000u);
            }
            ImGui::TableNextColumn();
            ImGui::PushID(r);
            const uint32_t hh = fr.ref.fnv1a_hash;
            if (ImGui::Selectable("##crow", is_sel(hh),
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowOverlap))
            {
                const ImGuiIO& io = ImGui::GetIO();
                if (io.KeyCtrl) {
                    bool removed = false;
                    for (auto it = state->selected_hashes.begin();
                         it != state->selected_hashes.end(); ++it) {
                        if (*it == hh) {
                            state->selected_hashes.erase(it);
                            removed = true; break;
                        }
                    }
                    if (!removed) state->selected_hashes.push_back(hh);
                    state->last_selected_hash = hh;
                } else if (io.KeyShift && state->last_selected_hash != 0) {
                    int anchor = -1;
                    for (int k = 0; k < (int)flat.size(); ++k) {
                        if (flat[k].ref.fnv1a_hash ==
                            state->last_selected_hash) { anchor = k; break; }
                    }
                    if (anchor < 0) anchor = r;
                    int a = std::min(anchor, r), b = std::max(anchor, r);
                    state->selected_hashes.clear();
                    for (int k = a; k <= b; ++k) {
                        state->selected_hashes.push_back(
                            flat[k].ref.fnv1a_hash);
                    }
                } else {
                    state->selected_hashes.clear();
                    state->selected_hashes.push_back(hh);
                    state->last_selected_hash = hh;
                }
                state->selected_hash = hh;  // drives the preview pin
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            {
                open_ctx = true;
                ctx_hash = hh;
            }
            if (ImGui::BeginDragDropSource(
                    ImGuiDragDropFlags_SourceAllowNullID)) {
                ImGui::SetDragDropPayload("CM_CHROW", &r, sizeof(int));
                ImGui::Text("Move: %s",
                            L ? L->display_name.c_str() : "(missing)");
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p =
                        ImGui::AcceptDragDropPayload("CM_CHROW")) {
                    int from = *static_cast<const int*>(p->Data);
                    if (from >= 0 && from < (int)flat.size() && from != r) {
                        do_move    = true;
                        move_ref   = flat[from].ref;
                        before_ref = fr.ref;
                    }
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine();
            if (fr.first) ImGui::Text("%d", fr.stage + 1);
            else          ImGui::TextDisabled("\xC2\xB7");
            ImGui::PopID();
            ImGui::TableNextColumn();
            if (fr.group > 1) {
                ImU32 c = ColorForId(static_cast<uint32_t>(fr.stage) + 1)
                          | 0xFF000000u;
                ImGui::TextColored(ImColor(c).Value, "%s",
                    L ? L->display_name.c_str() : "(missing)");
            } else {
                ImGui::TextUnformatted(
                    L ? L->display_name.c_str() : "(missing)");
            }
            ImGui::TableNextColumn();
            DrawPositionBar(env);
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- Context menu (Bind / Unbind, Staging-style) ----
    if (open_ctx) {
        if (!is_sel(ctx_hash)) {
            state->selected_hashes.clear();
            state->selected_hashes.push_back(ctx_hash);
            state->last_selected_hash = ctx_hash;
        }
        state->selected_hash = ctx_hash;
        ImGui::OpenPopup("chase_row_actions");
    }
    bool do_bind = false, do_unbind = false;
    if (ImGui::BeginPopup("chase_row_actions")) {
        // Selected refs, in flat order, restricted to this chase.
        std::vector<LayerRef> selv;
        for (const auto& fr : flat) {
            if (is_sel(fr.ref.fnv1a_hash)) selv.push_back(fr.ref);
        }
        const int N = static_cast<int>(selv.size());
        // Is any selected layer in a multi-light (chase-local bind)
        // stage? Gates "Unbind".
        bool any_grouped = false;
        for (const auto& fr : flat) {
            if (fr.group > 1 && is_sel(fr.ref.fnv1a_hash)) {
                any_grouped = true; break;
            }
        }
        if (N == 1) {
            const LayerInfo* fL = FindLayerByRef(*state, selv[0]);
            ImGui::TextDisabled("%s",
                fL ? fL->display_name.c_str() : "(missing)");
        } else {
            ImGui::TextDisabled("%d layers selected", N);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Bind...")) {
            ImGui::BeginDisabled(N < 2);
            if (ImGui::MenuItem("Bind selected together (this chase)")) {
                do_bind = true;
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!any_grouped);
            if (ImGui::MenuItem("Unbind selected")) {
                do_unbind = true;
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Bind welds the selected lights into one\n"
                              "chase stage so they fire together \xE2\x80\x94 "
                              "only in this chase (not a session bind).");
        }
        ImGui::EndPopup();
    }

    // ---- Apply deferred edits (under lock) ----
    if (do_move || do_bind || do_unbind) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            Chase& real = state->chases[chase_index];
            std::vector<ChaseStage> ns = real.stages;
            auto in_list = [](const std::vector<LayerRef>& v,
                              const LayerRef& x) {
                for (const auto& e : v) if (e == x) return true;
                return false;
            };
            auto remove_refs = [&](const std::vector<LayerRef>& kill) {
                for (auto it = ns.begin(); it != ns.end(); ) {
                    auto& m = it->members;
                    for (auto mi = m.begin(); mi != m.end(); ) {
                        if (in_list(kill, *mi)) mi = m.erase(mi);
                        else ++mi;
                    }
                    if (m.empty()) it = ns.erase(it); else ++it;
                }
            };
            if (do_move) {
                std::vector<LayerRef> one{ move_ref };
                remove_refs(one);
                // Resolve the drop target's stage *after* removal so
                // the index is exact even if a stage emptied out.
                int pos = -1;
                for (int i = 0; i < (int)ns.size(); ++i) {
                    for (const auto& m : ns[i].members) {
                        if (m == before_ref) { pos = i; break; }
                    }
                    if (pos >= 0) break;
                }
                if (pos < 0 || pos > (int)ns.size()) pos = (int)ns.size();
                ChaseStage st; st.members.push_back(move_ref);
                ns.insert(ns.begin() + pos, std::move(st));
            } else if (do_bind) {
                std::vector<LayerRef> selv;
                for (const auto& fr : flat) {
                    if (is_sel(fr.ref.fnv1a_hash)) selv.push_back(fr.ref);
                }
                if (selv.size() >= 2) {
                    int pos = -1;
                    for (int i = 0; i < (int)ns.size(); ++i) {
                        for (const auto& m : ns[i].members) {
                            if (in_list(selv, m)) { pos = i; break; }
                        }
                        if (pos >= 0) break;
                    }
                    remove_refs(selv);
                    if (pos < 0 || pos > (int)ns.size()) {
                        pos = (int)ns.size();
                    }
                    ChaseStage st; st.members = selv;
                    ns.insert(ns.begin() + pos, std::move(st));
                }
            } else if (do_unbind) {
                std::vector<LayerRef> selv;
                for (const auto& fr : flat) {
                    if (is_sel(fr.ref.fnv1a_hash)) selv.push_back(fr.ref);
                }
                std::vector<ChaseStage> rebuilt;
                rebuilt.reserve(ns.size());
                for (auto& stg : ns) {
                    std::vector<LayerRef> kept, pulled;
                    for (const auto& m : stg.members) {
                        if (stg.members.size() > 1 && in_list(selv, m)) {
                            pulled.push_back(m);
                        } else {
                            kept.push_back(m);
                        }
                    }
                    if (!kept.empty()) {
                        ChaseStage k; k.members = std::move(kept);
                        rebuilt.push_back(std::move(k));
                    }
                    for (const auto& p : pulled) {
                        ChaseStage one; one.members.push_back(p);
                        rebuilt.push_back(std::move(one));
                    }
                }
                ns = std::move(rebuilt);
            }
            real.stages = std::move(ns);
            real.manual_stages = true;
        }
    }

    ImGui::SameLine();

    // Composite preview + centroid map (right)
    ImGui::BeginChild("chase_preview_pane", ImVec2(right_w, pane_h), false);
    const bool pinned = !state->chase_preview_playing &&
                        state->selected_hash != 0;
    ImGui::TextDisabled(pinned ? "Composite preview (pinned \xE2\x80\x94 "
                                 "click elsewhere or Play)"
                               : "Composite preview");
    if (state->chase_composite_texture_id != 0 &&
        state->chase_composite_w > 0 && state->chase_composite_h > 0)
    {
        const float pw = ImGui::GetContentRegionAvail().x;
        const float aspect = float(state->chase_composite_h) /
                             float(state->chase_composite_w);
        float w = pw;
        float h = w * aspect;
        const float max_h = pane_h * 0.55f;
        if (h > max_h) { h = max_h; w = h / aspect; }
        ImGui::Image((ImTextureID)state->chase_composite_texture_id,
                     ImVec2(w, h));
    } else {
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 60.f));
        ImGui::TextDisabled("(no composite — add layers or wait for thumbs)");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Centroid map  (yellow = active \xC2\xB7 ring = "
                        "selected \xC2\xB7 only this chase's lights)");
    // Only this chase's lights, not the whole staging set. `included`
    // = member of the active stage this frame (canvas draws those
    // yellow); the selected/pinned layer gets the ring.
    {
        std::unordered_set<uint32_t> chase_hashes;
        for (const auto& stg : cs.stages) {
            for (const auto& m : stg.members) {
                chase_hashes.insert(m.fnv1a_hash);
            }
        }
        std::unordered_set<uint32_t> active_hashes;
        if (active_stage_top >= 0 &&
            active_stage_top < (int)cs.stages.size()) {
            for (const auto& ref : cs.stages[active_stage_top].members) {
                active_hashes.insert(ref.fnv1a_hash);
            }
        }
        std::vector<LayerInfo> chase_layers;
        int sel_idx = -1;
        for (const auto& Lsrc : snap.layers) {
            if (!chase_hashes.count(Lsrc.fnv1a_hash)) continue;
            LayerInfo c = Lsrc;
            c.included = active_hashes.count(c.fnv1a_hash) != 0;
            if (c.fnv1a_hash == state->selected_hash) {
                sel_idx = static_cast<int>(chase_layers.size());
            }
            chase_layers.push_back(std::move(c));
        }
        DrawCentroidCanvas(chase_layers, snap.active_source_id,
                           snap.position_overrides,
                           snap.image_width, snap.image_height,
                           -1, sel_idx, cs.sort_mode,
                           std::max(80.f, ImGui::GetContentRegionAvail().y - 8.f),
                           state, /*allow_drag=*/false);
    }
    ImGui::EndChild();
}

// ===== Random-scatter editor =========================================
// Separate from DrawChaseEditor: scatter chases have no stages / sort
// / manual arrangement, so a dedicated screen is cleaner than
// threading `if (random_scatter)` through the stage UI.
void DrawScatterEditor(PanelState* state, FrameSnapshot& snap,
                       int chase_index, float panel_w, float panel_h)
{
    if (chase_index < 0 || chase_index >= (int)snap.chases.size()) return;
    (void)panel_h;
    const float fps = std::max(1.f, state->chase_preview_fps.load());

    // Per-frame regen so the scatter tracks param / staging changes.
    {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            Chase& real = state->chases[chase_index];
            RegenerateScatter(real, *state, fps);
            if (chase_index < (int)snap.chases.size()) {
                snap.chases[chase_index].scatter = real.scatter;
            }
        }
    }
    Chase& cs = snap.chases[chase_index];

    // ---- Header: name + Build in AE ----
    char name_buf[128];
    std::snprintf(name_buf, sizeof(name_buf), "%s", cs.name.c_str());
    ImGui::SetNextItemWidth(260.f);
    if (ImGui::InputText("Name", name_buf, sizeof(name_buf))) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size())
            state->chases[chase_index].name = name_buf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Build in AE")) state->want_build_chase_index = chase_index;
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Build a seamless looping comp: every eligible light gets\n"
            "Density copies scattered across the loop, each with the\n"
            "hit envelope. Loops cleanly at the loop length.");
    }

    ImGui::TextColored(ImVec4(0.55f, 0.75f, 1.f, 1.f),
        "Random scatter \xE2\x80\x94 stratified (every light used; copies "
        "spread evenly, never self-stacking).");

    // ---- Scatter params ----
    bool changed = false;
    float loop_s = cs.loop_seconds;
    int   dens   = cs.scatter_density;
    int   seed_i = static_cast<int>(cs.random_seed);
    ImGui::SetNextItemWidth(180.f);
    if (ImGui::SliderFloat("Loop length", &loop_s, 1.f, 60.f, "%.1f s"))
        changed = true;
    if (MiddleClickReset(loop_s, 10.0f)) changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::SliderInt("Density", &dens, 1, 12, "%d /light")) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies of each light spread across the loop.");
    ImGui::SetNextItemWidth(160.f);
    if (ImGui::InputInt("Seed", &seed_i)) changed = true;
    ImGui::SameLine();
    if (ImGui::Button("Reseed")) {
        std::random_device rd;
        seed_i = static_cast<int>(rd());
        changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d lights \xC2\xB7 %zu hits \xC2\xB7 %d frames",
        (cs.scatter.empty() || dens <= 0)
            ? 0 : (int)(cs.scatter.size() / std::max(1, dens)),
        cs.scatter.size(), ChaseLoopFrames(cs, *state, fps));

    // ---- Hit envelope (shared ChaseTiming; no step in scatter) ----
    ImGui::Separator();
    ChaseTiming t = cs.timing;
    bool t_changed = false;
    const float sw = 150.f;
    ImGui::SetNextItemWidth(sw);
    if (ImGui::SliderFloat("Duration", &t.duration, 1.f, 240.f, "%.0f f")) t_changed = true;
    if (MiddleClickReset(t.duration, 30.0f)) t_changed = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(sw);
    if (ImGui::SliderFloat("Attack", &t.attack, 0.f, t.duration, "%.1f f")) t_changed = true;
    if (MiddleClickReset(t.attack, 5.0f)) t_changed = true;
    // Opacity/gamma envelope tuned rarely — collapsed by default to
    // keep the common timing controls uncluttered.
    if (ImGui::TreeNode("Opacity / Gamma envelope")) {
        ImGui::SetNextItemWidth(sw);
        if (ImGui::SliderFloat("Opacity peak", &t.opacity_peak, 0.f, 100.f, "%.0f%%")) t_changed = true;
        if (MiddleClickReset(t.opacity_peak, 100.0f)) t_changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sw);
        if (ImGui::SliderFloat("Gamma peak", &t.gamma_peak, 0.01f, 4.f, "%.2f")) t_changed = true;
        if (MiddleClickReset(t.gamma_peak, 1.0f)) t_changed = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sw);
        if (ImGui::SliderFloat("Gamma base", &t.gamma_baseline, 0.01f, 4.f, "%.2f")) t_changed = true;
        if (MiddleClickReset(t.gamma_baseline, 0.25f)) t_changed = true;
        ImGui::TreePop();
    }

    if (changed || t_changed) {
        std::lock_guard<std::mutex> lk(state->mu);
        if (chase_index < (int)state->chases.size()) {
            Chase& real = state->chases[chase_index];
            real.loop_seconds   = std::max(0.1f, loop_s);
            real.scatter_density = std::max(1, dens);
            real.random_seed    = static_cast<uint32_t>(seed_i);
            real.timing         = t;
            state->default_timing = t;
        }
    }

    // ---- Preview transport (loops over the scatter length) ----
    ImGui::Separator();
    const float total = static_cast<float>(ChaseLoopFrames(cs, *state, fps));
    if (state->chase_preview_frame > total) state->chase_preview_frame = 0.f;
    if (state->chase_preview_playing && total > 0.f) {
        state->chase_preview_frame += ImGui::GetIO().DeltaTime * fps;
        if (state->chase_preview_frame >= total)
            state->chase_preview_frame = std::fmod(state->chase_preview_frame,
                                                   total);
    }
    if (ImGui::Button(state->chase_preview_playing ? "Pause" : "Play"))
        state->chase_preview_playing = !state->chase_preview_playing;
    ImGui::SameLine();
    if (ImGui::Button("Reset")) state->chase_preview_frame = 0.f;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(280.f);
    ImGui::SliderFloat("##scrub", &state->chase_preview_frame,
                       0.f, std::max(1.f, total), "frame %.1f");
    ImGui::SameLine();
    ImGui::TextDisabled("/ %.0f f (%.1f s @ %.0f fps, loops)",
                        total, total / fps, fps);

    // ---- Composite (pin single clicked light, else scatter) ----
    {
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        bool built = false;
        if (!state->chase_preview_playing && state->selected_hash != 0) {
            for (const auto& hit : cs.scatter) {
                if (hit.ref.fnv1a_hash == state->selected_hash) {
                    built = BuildRefsComposite({ hit.ref }, *state, rgba, w, h);
                    break;
                }
            }
        }
        if (!built) {
            built = BuildScatterComposite(cs, *state,
                state->chase_preview_frame, fps, rgba, w, h);
        }
        if (built) {
            state->chase_composite_rgba = std::move(rgba);
            state->chase_composite_w = w;
            state->chase_composite_h = h;
            state->chase_composite_dirty = true;
        } else {
            state->chase_composite_rgba.clear();
            state->chase_composite_w = 0;
            state->chase_composite_h = 0;
            state->chase_composite_texture_id = 0;
        }
    }

    // ---- Split pane: per-light hit list | preview + centroid map ----
    ImGui::Separator();
    const float pane_h = std::max(160.f, ImGui::GetContentRegionAvail().y - 8.f);
    const float right_w = std::min(420.f, std::max(200.f, panel_w * 0.42f));
    const float left_w  = std::max(220.f,
                            ImGui::GetContentRegionAvail().x - right_w - 12.f);

    // Aggregate hits per light (for the table) + active set this frame.
    const int loop_frames = ChaseLoopFrames(cs, *state, fps);
    std::unordered_map<uint32_t, int> hit_count;
    std::unordered_set<uint32_t> active_hashes;
    for (const auto& hit : cs.scatter) {
        hit_count[hit.ref.fnv1a_hash]++;
        if (ScatterHitEnvelope(state->chase_preview_frame, hit.start_frame,
                               loop_frames, cs.timing) > 0.f)
            active_hashes.insert(hit.ref.fnv1a_hash);
    }

    ImGui::BeginChild("scatter_list", ImVec2(left_w, pane_h), false);
    ImGui::TextDisabled("Click a light to pin it in the preview");
    constexpr ImGuiTableFlags kF =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("scatter_tbl", 3, kF, ImVec2(0.f, pane_h - 8.f))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Light", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, 44.f);
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, 32.f);
        ImGui::TableHeadersRow();
        // First-occurrence order in the scatter list == source layer
        // order (RegenerateScatter walks src->layers in order).
        std::vector<uint32_t> seen;
        for (const auto& hit : cs.scatter) {
            uint32_t hh = hit.ref.fnv1a_hash;
            bool dup = false;
            for (uint32_t x : seen) if (x == hh) { dup = true; break; }
            if (dup) continue;
            seen.push_back(hh);
            const LayerInfo* L = FindLayerByRef(*state, hit.ref);
            ImGui::TableNextRow();
            if (active_hashes.count(hh))
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(120, 80, 30, 110));
            ImGui::TableNextColumn();
            ImGui::PushID((int)hh);
            bool sel = (state->selected_hash == hh);
            if (ImGui::Selectable(L ? L->display_name.c_str() : "(missing)",
                    sel, ImGuiSelectableFlags_SpanAllColumns)) {
                state->selected_hash = hh;
                state->last_selected_hash = hh;
                state->selected_hashes.clear();
                state->selected_hashes.push_back(hh);
            }
            ImGui::PopID();
            ImGui::TableNextColumn();
            ImGui::Text("%d", hit_count[hh]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(active_hashes.count(hh) ? "*" : "");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("scatter_preview", ImVec2(right_w, pane_h), false);
    const bool pinned = !state->chase_preview_playing &&
                        state->selected_hash != 0;
    ImGui::TextDisabled(pinned ? "Preview (pinned \xE2\x80\x94 click "
                                 "elsewhere or Play)" : "Composite preview");
    if (state->chase_composite_texture_id != 0 &&
        state->chase_composite_w > 0 && state->chase_composite_h > 0) {
        const float pw = ImGui::GetContentRegionAvail().x;
        const float aspect = float(state->chase_composite_h) /
                             float(state->chase_composite_w);
        float w = pw, h = w * aspect;
        const float max_h = pane_h * 0.55f;
        if (h > max_h) { h = max_h; w = h / aspect; }
        ImGui::Image((ImTextureID)state->chase_composite_texture_id,
                     ImVec2(w, h));
    } else {
        ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, 60.f));
        ImGui::TextDisabled("(no composite — wait for thumbs)");
    }
    ImGui::Separator();
    ImGui::TextDisabled("Centroid map  (yellow = on this frame \xC2\xB7 "
                        "ring = selected)");
    {
        std::unordered_set<uint32_t> chase_hashes;
        for (const auto& hit : cs.scatter)
            chase_hashes.insert(hit.ref.fnv1a_hash);
        std::vector<LayerInfo> chase_layers;
        int sel_idx = -1;
        for (const auto& Lsrc : snap.layers) {
            if (!chase_hashes.count(Lsrc.fnv1a_hash)) continue;
            LayerInfo c = Lsrc;
            c.included = active_hashes.count(c.fnv1a_hash) != 0;
            if (c.fnv1a_hash == state->selected_hash)
                sel_idx = static_cast<int>(chase_layers.size());
            chase_layers.push_back(std::move(c));
        }
        DrawCentroidCanvas(chase_layers, snap.active_source_id,
                           snap.position_overrides,
                           snap.image_width, snap.image_height,
                           -1, sel_idx, SortMode::CentroidX,
                           std::max(80.f, ImGui::GetContentRegionAvail().y - 8.f),
                           state, /*allow_drag=*/false);
    }
    ImGui::EndChild();
}

void DrawChaseTab(PanelState* state, FrameSnapshot& snap, int chase_index,
                  float panel_w, float panel_h)
{
    if (snap.chase_in_wizard) {
        DrawWizardForChaseTab(state, snap, chase_index);
    } else if (chase_index >= 0 && chase_index < (int)snap.chases.size() &&
               snap.chases[chase_index].random_scatter) {
        DrawScatterEditor(state, snap, chase_index, panel_w, panel_h);
    } else {
        DrawChaseEditor(state, snap, chase_index, panel_w, panel_h);
    }
}

// ===== Session toolbar ================================================

void DrawSessionToolbar(PanelState* state, const FrameSnapshot& snap)
{
    if (ImGui::Button("Save session")) state->want_save_session = true;
    ImGui::SameLine();
    if (ImGui::Button("Load session")) state->want_load_session = true;
    ImGui::SameLine();
    if (!snap.session_save_path.empty()) {
        ImGui::TextDisabled("Session: %s", Basename(snap.session_save_path).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", snap.session_save_path.c_str());
    } else {
        ImGui::TextDisabled("Session: (unsaved)");
    }
    ImGui::SameLine(0.f, 20.f);
    ImGui::BeginDisabled(snap.chases.empty());
    if (ImGui::Button("Build all chases")) {
        state->want_build_all_chases = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Run the dumb-comps builder for every chase\n"
                          "in this session. One AE undo step covers\n"
                          "the whole build.");
    }
    ImGui::EndDisabled();
    {
        std::string status;
        bool exrdemux_missing = false;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            status = state->last_build_status;
            exrdemux_missing = state->last_build_exrdemux_missing;
        }
        if (exrdemux_missing) {
            // Comps DID build — but without EXRDemux they render
            // wrong. Loud, actionable, points at the Save button
            // that's right above this line.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.62f, 0.18f, 1.f));
            ImGui::TextWrapped(
                "EXRDemux not installed: the comps were created, but the "
                "light layers have NO hash-driven selection and will "
                "render wrong. Click \"Save session\" above, install "
                "\"tdcarney EXRDemux\", restart AE, then \"Load session\" "
                "and Build again. (%s)", status.c_str());
            ImGui::PopStyleColor();
        } else if (!status.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", status.c_str());
        }
    }
}

} // namespace

void RenderFrame(PanelState* state, float w, float h, void* host_view,
                 const char* backend_label)
{
    if (!state) return;
    (void)host_view;
    if (w < 1.f) w = 1.f;
    if (h < 1.f) h = 1.f;

    // Keyboard shortcuts must run BEFORE we capture the frame's snap
    // so an undo this frame surfaces in the panel immediately. Skip
    // while an InputText has focus so Ctrl+Z inside a text field
    // (where the user expects text-edit undo) isn't hijacked.
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool typing = io.WantTextInput;
        if (io.KeyCtrl && !typing) {
            std::lock_guard<std::mutex> lk(state->mu);
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (io.KeyShift) PerformRedo(*state);
                else             PerformUndo(*state);
            } else if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
                PerformRedo(*state);
            }
        }
    }

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("ChaseMakerRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // Spacebar play/pause — only when our panel hierarchy is focused
    // and no text input is active (so typing a space in a name field
    // doesn't toggle preview). Routes to the staging preview or the
    // chase preview based on the active tab.
    {
        const ImGuiIO& io = ImGui::GetIO();
        const bool window_focused =
            ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        if (window_focused && !io.WantTextInput &&
            ImGui::IsKeyPressed(ImGuiKey_Space, false))
        {
            if (state->active_tab == PanelTab::Staging) {
                state->preview_playing = !state->preview_playing;
                state->preview_accum_ms = 0.f;
            } else if (state->active_tab == PanelTab::Chase) {
                state->chase_preview_playing = !state->chase_preview_playing;
            }
        }
    }

    FrameSnapshot snap = TakeSnapshot(state);

    DrawSessionToolbar(state, snap);
    ImGui::Separator();
    ImGui::TextDisabled("Panel: %.0f x %.0f  •  %s",
                        w, h, backend_label ? backend_label : "");
    ImGui::Spacing();

    // Tab bar: Sources | Staging | chase tabs... | +
    if (ImGui::BeginTabBar("CMTabBar",
            ImGuiTabBarFlags_AutoSelectNewTabs |
            ImGuiTabBarFlags_FittingPolicyScroll))
    {
        if (ImGui::BeginTabItem("Sources")) {
            state->active_tab = PanelTab::Sources;
            DrawSourcesTab(state, snap);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Staging")) {
            state->active_tab = PanelTab::Staging;
            DrawStagingTab(state, snap, w, h);
            ImGui::EndTabItem();
        }
        for (int ci = 0; ci < (int)snap.chases.size(); ++ci) {
            char label[160];
            std::snprintf(label, sizeof(label), "%s###chase_%u",
                          snap.chases[ci].name.c_str(),
                          snap.chases[ci].chase_id);
            // Tint each chase tab with its own stable color so the
            // chase tabs read as a distinct group from Sources /
            // Staging (which keep the default theme) and from each
            // other. Same ColorForId palette as binds/tags.
            ImVec4 base = ImColor(ColorForId(snap.chases[ci].chase_id)).Value;
            auto tint = [&](float a) { return ImVec4(base.x, base.y,
                                                     base.z, a); };
            ImGui::PushStyleColor(ImGuiCol_Tab,                tint(0.40f));
            ImGui::PushStyleColor(ImGuiCol_TabHovered,         tint(0.80f));
            ImGui::PushStyleColor(ImGuiCol_TabSelected,        tint(0.70f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmed,          tint(0.28f));
            ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,  tint(0.50f));
            bool open = true;
            const bool visible = ImGui::BeginTabItem(label, &open);
            ImGui::PopStyleColor(5);
            if (visible) {
                state->active_tab = PanelTab::Chase;
                state->active_chase_index = ci;
                DrawChaseTab(state, snap, ci, w, h);
                ImGui::EndTabItem();
            }
            // Close (X) doesn't delete outright — it arms the
            // confirmation modal drawn after the tab bar.
            if (!open) state->chase_pending_close_index = ci;
        }
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                       ImGuiTabItemFlags_NoTooltip))
        {
            std::lock_guard<std::mutex> lk(state->mu);
            Chase nc;
            nc.chase_id = state->next_chase_id++;
            nc.name = "New chase";
            nc.timing = state->default_timing;
            state->chases.push_back(nc);
            state->active_chase_index = (int)state->chases.size() - 1;
            state->active_tab = PanelTab::Chase;
            state->chase_in_wizard = true;
            // Reset wizard form
            state->wizard_template_index = 0;
            state->wizard_tag_filter.clear();
            state->wizard_step_duration = state->default_timing.step_duration;
            state->wizard_hit_duration = state->default_timing.duration;
        }
        ImGui::EndTabBar();
    }

    // ---- Chase delete confirmation (armed by a tab's close X) ----
    if (state->chase_pending_close_index >= 0) {
        ImGui::OpenPopup("confirm_delete_chase");
    }
    if (ImGui::BeginPopupModal("confirm_delete_chase", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
        const int idx = state->chase_pending_close_index;
        const char* nm = (idx >= 0 && idx < (int)snap.chases.size())
            ? snap.chases[idx].name.c_str() : "";
        ImGui::Text("Delete chase \"%s\"?\nThis cannot be undone.", nm);
        ImGui::Spacing();
        if (ImGui::Button("Delete", ImVec2(120.f, 0.f))) {
            {
                std::lock_guard<std::mutex> lk(state->mu);
                if (idx >= 0 && idx < (int)state->chases.size()) {
                    state->chases.erase(state->chases.begin() + idx);
                    if (state->active_chase_index >=
                        (int)state->chases.size())
                    {
                        state->active_chase_index = -1;
                    }
                    if (state->chases.empty()) {
                        state->active_tab = PanelTab::Staging;
                    }
                }
            }
            state->chase_pending_close_index = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.f, 0.f))) {
            state->chase_pending_close_index = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    ImGui::End();

    if (!snap.layers.empty()) {
        PublishInclusionChanges(state, snap.layers);
    }

    // ---- Auto-snapshot for undo ----
    // Once per "no widget actively held" frame, compare the current
    // authoring state to the previous stable snapshot. If something
    // changed (a slider deactivated, a button click resolved, a drag
    // released), push the prior state onto the undo stack so Ctrl+Z
    // takes us back to where we were just before this change.
    if (!ImGui::IsAnyItemActive()) {
        std::lock_guard<std::mutex> lk(state->mu);
        UndoSnapshot cur = CaptureUndoSnapshot(*state);
        if (state->has_last_stable) {
            if (!(state->last_stable == cur)) {
                state->undo_stack.push_back(state->last_stable);
                if (state->undo_stack.size() > kMaxUndoStack) {
                    state->undo_stack.erase(state->undo_stack.begin());
                }
                state->redo_stack.clear();
            }
        }
        state->last_stable = std::move(cur);
        state->has_last_stable = true;
    }
}

} // namespace panel_ui
