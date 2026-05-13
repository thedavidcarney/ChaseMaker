#include "panel_ui.h"

#include "panel_state.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#include "imgui.h"

namespace panel_ui {

namespace {

// Pull a stable snapshot of the mutable fields under the state lock,
// so the rest of the frame draws against consistent data while the
// worker thread continues to mutate the source.
struct StateSnapshot {
    std::string                exr_path;
    int                        image_width = 0;
    int                        image_height = 0;
    std::vector<LayerInfo>     layers;
    std::vector<SkippedLayer>  skipped;
    std::string                last_error;
    std::string                last_status;
    bool                       scanning = false;
    bool                       sidecar_written = false;
};

StateSnapshot Snapshot(PanelState* state)
{
    StateSnapshot s;
    s.scanning        = state->scanning.load();
    s.sidecar_written = state->sidecar_written.load();
    std::lock_guard<std::mutex> lk(state->mu);
    s.exr_path     = state->exr_path;
    s.image_width  = state->image_width;
    s.image_height = state->image_height;
    s.layers       = state->layers;
    s.skipped      = state->skipped;
    s.last_error   = state->last_error;
    s.last_status  = state->last_status;
    return s;
}

void DrawCentroidBar(float cx)
{
    // 1D horizontal bar with a marker at cx, drawn inside the current
    // table cell. Uses the foreground draw list of the current window
    // so it composes with ImGui's clipping.
    constexpr float kBarHeight = 8.f;
    ImGui::Dummy(ImVec2(0.f, 2.f));
    const ImVec2 p_min = ImGui::GetCursorScreenPos();
    const float avail = std::max(80.f, ImGui::GetContentRegionAvail().x);
    const ImVec2 p_max(p_min.x + avail, p_min.y + kBarHeight);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p_min, p_max, IM_COL32(60, 60, 65, 255), 2.f);

    const float clamped = cx < 0.f ? 0.f : (cx > 1.f ? 1.f : cx);
    const float marker_x = p_min.x + clamped * avail;
    dl->AddRectFilled(ImVec2(marker_x - 1.f, p_min.y - 1.f),
                      ImVec2(marker_x + 1.f, p_max.y + 1.f),
                      IM_COL32(220, 200, 80, 255));

    ImGui::Dummy(ImVec2(avail, kBarHeight + 2.f));
}

void DrawLayersTable(const std::vector<LayerInfo>& layers)
{
    if (layers.empty()) return;

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("layers", 5, kTableFlags,
                           ImVec2(0.f, ImGui::GetContentRegionAvail().y - 30.f))) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 32.f);
    ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("cx", ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("cy", ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("position", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();

    for (size_t i = 0; i < layers.size(); ++i) {
        const LayerInfo& L = layers[i];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%zu", i + 1);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(L.display_name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FNV-1a hash: 0x%08x\nTotal luminance: %.4g",
                              L.fnv1a_hash, L.total);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", L.cx);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", L.cy);
        ImGui::TableNextColumn();
        DrawCentroidBar(L.cx);
    }
    ImGui::EndTable();
}

void DrawSkippedSection(const std::vector<SkippedLayer>& skipped)
{
    if (skipped.empty()) return;
    std::string header = "Skipped (" + std::to_string(skipped.size()) + ")###skipped";
    if (ImGui::CollapsingHeader(header.c_str())) {
        for (const auto& s : skipped) {
            ImGui::BulletText("%s  —  %s",
                              s.display_name.c_str(), s.reason.c_str());
        }
    }
}

} // namespace

void RenderFrame(PanelState* state, float w, float h, void* host_view,
                 const char* backend_label)
{
    if (!state) return;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("ChaseMakerRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    StateSnapshot snap = Snapshot(state);

    // ---- Header: pick + path ----
    bool clicked_pick = false;
    {
        ImGui::BeginDisabled(snap.scanning);
        clicked_pick = ImGui::Button("Open EXR...");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (snap.exr_path.empty()) {
            ImGui::TextDisabled("(no file)");
        } else {
            ImGui::TextWrapped("%s", snap.exr_path.c_str());
        }
    }

    if (clicked_pick && !snap.scanning) {
        // Defer the actual dialog to after the frame — see
        // PanelState::want_pick_exr for the reentrancy rationale.
        state->want_pick_exr = true;
    }
    (void)host_view;

    // ---- Status line / image size ----
    ImGui::Separator();
    if (snap.image_width > 0 && snap.image_height > 0) {
        ImGui::Text("Image: %d x %d  •  %zu layer%s scanned  •  %s",
                    snap.image_width, snap.image_height,
                    snap.layers.size(), snap.layers.size() == 1 ? "" : "s",
                    backend_label ? backend_label : "");
    } else {
        ImGui::Text("Panel: %.0f x %.0f  •  %s",
                    w, h, backend_label ? backend_label : "");
    }

    if (snap.scanning) {
        ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.f),
                           "Scanning... %s", snap.last_status.c_str());
    } else if (!snap.last_error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.f),
                           "%s", snap.last_error.c_str());
    } else if (!snap.last_status.empty()) {
        ImGui::TextDisabled("%s", snap.last_status.c_str());
    }

    // ---- Sidecar write button ----
    if (!snap.layers.empty() && !snap.scanning) {
        if (ImGui::Button("Write luminosity sidecar")) {
            state->want_write_sidecar = true;
        }
        ImGui::SameLine();
        if (snap.sidecar_written) {
            ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1.f), "saved");
        } else {
            ImGui::TextDisabled(".luminosity.json next to the EXR");
        }
    }

    ImGui::Separator();

    // ---- Layers table ----
    DrawLayersTable(snap.layers);

    // ---- Skipped section ----
    DrawSkippedSection(snap.skipped);

    ImGui::End();
}

} // namespace panel_ui
