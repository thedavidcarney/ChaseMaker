#include "panel_ui.h"

#include "panel_state.h"

#include "imgui_internal.h"   // GetActiveID()

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"

namespace panel_ui {

namespace {

// ---- Helpers --------------------------------------------------------

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
    // Safety: cap iterations even if accum runs away on a stalled frame.
    int safety = 1000;
    while (state->preview_accum_ms >= step && safety-- > 0) {
        state->preview_index = (state->preview_index + 1) % n_included;
        state->preview_accum_ms -= step;
    }
    if (safety <= 0) state->preview_accum_ms = 0.f;
}

// Frame-local snapshot of the published state, plus a working copy of
// the layers vector that the UI can mutate (inclusion toggles). After
// drawing, we publish layer-inclusion changes back under the lock.
struct FrameSnapshot {
    std::string                exr_path;
    int                        image_width = 0;
    int                        image_height = 0;
    std::vector<LayerInfo>     layers;       // working copy
    std::vector<SkippedLayer>  skipped;
    std::string                last_error;
    std::string                last_status;
    bool                       scanning = false;
    bool                       sidecar_written = false;
};

FrameSnapshot TakeSnapshot(PanelState* state)
{
    FrameSnapshot s;
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

// Push layer inclusion changes (and only those — other fields aren't
// mutated by the UI) back into the canonical state. Matches entries by
// display_name, so a concurrent scan that swapped in a fresh layers
// vector simply ignores stale toggles.
void PublishInclusionChanges(PanelState* state,
                             const std::vector<LayerInfo>& working)
{
    std::lock_guard<std::mutex> lk(state->mu);
    if (state->layers.size() != working.size()) return;
    for (size_t i = 0; i < working.size(); ++i) {
        if (state->layers[i].display_name == working[i].display_name) {
            state->layers[i].included = working[i].included;
        }
    }
}

// ---- Centroid scatter canvas ---------------------------------------

void DrawCentroidCanvas(const std::vector<LayerInfo>& layers,
                        int image_w, int image_h,
                        int highlight_full_index,
                        float requested_height)
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

    dl->AddRectFilled(origin, max, IM_COL32(20, 22, 26, 255), 4.f);
    dl->AddRect(origin, max, IM_COL32(70, 72, 78, 255), 4.f);
    const float midx = origin.x + canvas_w * 0.5f;
    const float midy = origin.y + canvas_h * 0.5f;
    dl->AddLine(ImVec2(origin.x, midy), ImVec2(max.x, midy),
                IM_COL32(48, 50, 56, 255));
    dl->AddLine(ImVec2(midx, origin.y), ImVec2(midx, max.y),
                IM_COL32(48, 50, 56, 255));

    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
        const LayerInfo& L = layers[i];
        const float x = origin.x + L.cx * canvas_w;
        const float y = origin.y + L.cy * canvas_h;
        if (!L.included) {
            dl->AddCircleFilled(ImVec2(x, y), 2.5f, IM_COL32(80, 80, 86, 200));
        } else {
            dl->AddCircleFilled(ImVec2(x, y), 3.0f, IM_COL32(220, 200, 80, 220));
        }
    }
    if (highlight_full_index >= 0 &&
        highlight_full_index < static_cast<int>(layers.size())) {
        const LayerInfo& L = layers[highlight_full_index];
        const float x = origin.x + L.cx * canvas_w;
        const float y = origin.y + L.cy * canvas_h;
        dl->AddCircleFilled(ImVec2(x, y), 8.f, IM_COL32(255, 240, 160, 90));
        dl->AddCircle(ImVec2(x, y), 8.f, IM_COL32(255, 240, 160, 255), 0, 2.f);
    }

    ImGui::Dummy(ImVec2(canvas_w, canvas_h));
}

void DrawCentroidBar(float cx)
{
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

void DrawLayersTable(std::vector<LayerInfo>& layers,
                     int highlight_full_index,
                     float table_height)
{
    if (layers.empty()) return;

    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersV |
        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_ScrollY;

    if (!ImGui::BeginTable("layers", 6, kTableFlags,
                           ImVec2(0.f, table_height))) {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#",   ImGuiTableColumnFlags_WidthFixed, 32.f);
    ImGui::TableSetupColumn("On",  ImGuiTableColumnFlags_WidthFixed, 28.f);
    ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn("cx",  ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("cy",  ImGuiTableColumnFlags_WidthFixed, 48.f);
    ImGui::TableSetupColumn("position", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();

    for (int i = 0; i < static_cast<int>(layers.size()); ++i) {
        LayerInfo& L = layers[i];
        ImGui::TableNextRow();

        if (i == highlight_full_index) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(120, 80, 30, 80));
        } else if (!L.included) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   IM_COL32(40, 40, 44, 120));
        }

        ImGui::TableNextColumn();
        if (L.included) ImGui::Text("%d", i + 1);
        else            ImGui::TextDisabled("%d", i + 1);

        ImGui::TableNextColumn();
        ImGui::PushID(i);
        ImGui::Checkbox("##on", &L.included);
        ImGui::PopID();

        ImGui::TableNextColumn();
        if (L.included) ImGui::TextUnformatted(L.display_name.c_str());
        else            ImGui::TextDisabled("%s", L.display_name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("FNV-1a hash: 0x%08x\nTotal luminance: %.4g",
                              L.fnv1a_hash, L.total);
        }

        ImGui::TableNextColumn();
        if (L.included) ImGui::Text("%.3f", L.cx);
        else            ImGui::TextDisabled("%.3f", L.cx);

        ImGui::TableNextColumn();
        if (L.included) ImGui::Text("%.3f", L.cy);
        else            ImGui::TextDisabled("%.3f", L.cy);

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
    (void)host_view;
    if (w < 1.f) w = 1.f;
    if (h < 1.f) h = 1.f;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("ChaseMakerRoot", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus);

    // ---- Keyboard-input diagnostic strip ----
    // Surfaces whether ImGui's IO sees keys at all and whether any
    // ImGui widget is currently asking for text input. ActiveID
    // non-zero means some widget is "claimed" (e.g. focused
    // InputText); it should clear when you click outside.
    {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::TextDisabled(
            "kbd: WantText=%d WantCapture=%d  active=0x%08x  focus=%d  recent: %s",
            (int)io.WantTextInput, (int)io.WantCaptureKeyboard,
            (unsigned)ImGui::GetActiveID(), (int)io.AppFocusLost ? 0 : 1,
            io.InputQueueCharacters.Size > 0 ? "yes" :
                (ImGui::IsKeyDown(ImGuiKey_A) || ImGui::IsKeyDown(ImGuiKey_Space) ||
                 ImGui::IsKeyDown(ImGuiKey_Enter) ? "down" : "(none)"));
    }

    FrameSnapshot snap = TakeSnapshot(state);

    const int included_now = IncludedCount(snap.layers);
    TickPreview(state, included_now);

    int highlight_full_index = -1;
    if (state->preview_playing && included_now > 0) {
        highlight_full_index = FullIndexOfIncluded(snap.layers,
                                                   state->preview_index);
    }

    // ---- Header + pick button ----
    ImGui::BeginDisabled(snap.scanning);
    if (ImGui::Button("Open EXR...")) state->want_pick_exr = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (snap.exr_path.empty()) {
        ImGui::TextDisabled("(no file)");
    } else {
        ImGui::TextWrapped("%s", snap.exr_path.c_str());
    }

    // ---- Status line ----
    ImGui::Separator();
    if (snap.image_width > 0 && snap.image_height > 0) {
        ImGui::Text("Image: %d x %d  •  %d/%zu included  •  %s",
                    snap.image_width, snap.image_height,
                    included_now, snap.layers.size(),
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

    if (!snap.layers.empty()) {
        ImGui::Separator();

        // ---- Exclude filter row ----
        ImGui::SetNextItemWidth(200.f);
        ImGui::InputTextWithHint("##excludeFilter",
            "substring (e.g. fire)",
            state->exclude_filter, sizeof(state->exclude_filter));
        ImGui::SameLine();
        if (ImGui::Button("Exclude matching")) {
            for (auto& L : snap.layers) {
                if (ContainsCI(L.display_name, state->exclude_filter)) {
                    L.included = false;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Include matching")) {
            for (auto& L : snap.layers) {
                if (ContainsCI(L.display_name, state->exclude_filter)) {
                    L.included = true;
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("All on")) {
            for (auto& L : snap.layers) L.included = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("All off")) {
            for (auto& L : snap.layers) L.included = false;
        }

        // ---- Preview controls ----
        ImGui::Separator();
        const char* play_label = state->preview_playing
            ? "Pause preview" : "Play preview";
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
        ImGui::SetNextItemWidth(160.f);
        ImGui::SliderFloat("ms/light", &state->preview_ms_step,
                           20.f, 2000.f, "%.0f ms",
                           ImGuiSliderFlags_Logarithmic);
        if (state->preview_playing && included_now > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("now: %d / %d",
                                state->preview_index + 1, included_now);
        }

        ImGui::Separator();

        const float pane_h = std::max(120.f, ImGui::GetContentRegionAvail().y - 40.f);
        const float canvas_w = std::min(360.f, std::max(160.f, w * 0.40f));
        const float table_w  = std::max(200.f,
                                ImGui::GetContentRegionAvail().x - canvas_w - 12.f);

        ImGui::BeginChild("table_pane", ImVec2(table_w, pane_h), false);
        DrawLayersTable(snap.layers, highlight_full_index, pane_h - 8.f);
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("canvas_pane", ImVec2(canvas_w, pane_h), false);
        ImGui::TextDisabled("Centroid map  (yellow = included)");
        DrawCentroidCanvas(snap.layers, snap.image_width, snap.image_height,
                           highlight_full_index, pane_h - 30.f);
        ImGui::EndChild();
    }

    DrawSkippedSection(snap.skipped);

    ImGui::End();

    // Publish any inclusion changes back to the canonical state.
    if (!snap.layers.empty()) {
        PublishInclusionChanges(state, snap.layers);
    }
}

} // namespace panel_ui
