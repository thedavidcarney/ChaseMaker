#include "exr_scan.h"

#include "hash.h"
#include "panel_state.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <ImfStringAttribute.h>
#include <ImathBox.h>

namespace exr_scan {

namespace {

// Rec.709 luma weights — same convention as the Python reference.
constexpr float kR709 = 0.2126f;
constexpr float kG709 = 0.7152f;
constexpr float kB709 = 0.0722f;

std::string LowerCopy(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// Apply the "X.X" -> "X" doubled-prefix dedup that EXRDemux's
// DisplayNameFromArgbChannel performs. The scanner already strips the
// trailing .R/.G/.B per channel, so this acts on the layer-key level:
//   "World.World"       -> "World"
//   "LightGroup_001"    -> "LightGroup_001"     (no doubled prefix)
//   "Foo.Bar"           -> "Foo.Bar"            (different halves)
std::string DedupDoubledPrefix(const std::string& key)
{
    size_t dot = key.find_last_of('.');
    if (dot == std::string::npos) return key;
    std::string left = key.substr(0, dot);
    std::string right = key.substr(dot + 1);
    if (left == right) return left;
    return key;
}

// Returns a non-empty reason string if this layer should be skipped
// from chase ordering by default. Empty = include.
//
// Names skipped here aren't pure beauty/lightgroup passes — they're
// either non-luminance data (cryptomattes, hash-encoded), the full-
// frame beauty pass (Image/Alpha — bright everywhere, useless for
// per-light positioning), or environment/ambient passes (World /
// Ambient / HDRI — broad scene illumination rather than a single
// directional light). Matches the disable-by-default list in
// EXRDemux's SplitAndSortPassesToPrecomps.jsx.
std::string SkipReason(const std::string& display)
{
    std::string low = LowerCopy(display);
    if (low.find("crypto") != std::string::npos) return "cryptomatte";
    if (low == "image"  || low == "alpha")       return "beauty/alpha";
    if (low == "world"  || low == "hdri")        return "environment";
    if (low == "ambient")                        return "ambient";
    return {};
}

struct ChannelLookup {
    int   part = -1;
    std::string in_file_name;  // the raw channel name within the part
};

struct RgbGroup {
    std::string   layer_key;     // raw layer key before X.X dedup
    std::string   display_name;  // after dedup
    ChannelLookup r, g, b;
    bool          complete = false;
};

// Walk every part / every channel and group channels into per-layer
// R/G/B triples. Channels are grouped by:
//   - last-dot prefix of the channel name (e.g. "World.R" -> "World")
//   - or, if the channel has no dot, by the part's "name" attribute
std::vector<RgbGroup> BuildRgbGroups(Imf::MultiPartInputFile& file)
{
    std::map<std::string, RgbGroup> by_key;

    for (int p = 0; p < file.parts(); ++p) {
        const Imf::Header& hdr = file.header(p);

        std::string part_name;
        if (hdr.hasName()) {
            part_name = hdr.name();
        } else if (auto* a = hdr.findTypedAttribute<Imf::StringAttribute>("name")) {
            part_name = a->value();
        }

        const Imf::ChannelList& channels = hdr.channels();
        for (auto it = channels.begin(); it != channels.end(); ++it) {
            std::string raw = it.name();

            // Decompose the channel name into (layer_key, sub) where sub
            // is the trailing "R"/"G"/"B"/"A"/"Y"/etc. token.
            std::string layer_key;
            std::string sub;
            size_t dot = raw.find_last_of('.');
            if (dot != std::string::npos) {
                layer_key = raw.substr(0, dot);
                sub = raw.substr(dot + 1);
            } else {
                layer_key = part_name.empty() ? raw : part_name;
                sub = raw;
            }

            // Normalize R/G/B detection (rare lowercase variants).
            std::string sub_upper = sub;
            std::transform(sub_upper.begin(), sub_upper.end(), sub_upper.begin(),
                [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (sub_upper != "R" && sub_upper != "G" && sub_upper != "B") {
                continue;  // ignore A, Y, Z, custom suffixes for centroid
            }

            // Disambiguate by part index so two parts with the same
            // layer_key don't collide. We still display the dedup'd
            // layer_key (most common case: parts have unique names).
            std::string map_key = std::to_string(p) + "::" + layer_key;
            RgbGroup& grp = by_key[map_key];
            if (grp.layer_key.empty()) {
                grp.layer_key = layer_key;
                grp.display_name = DedupDoubledPrefix(layer_key);
            }
            ChannelLookup lookup{ p, raw };
            if (sub_upper == "R") grp.r = lookup;
            else if (sub_upper == "G") grp.g = lookup;
            else if (sub_upper == "B") grp.b = lookup;
        }
    }

    std::vector<RgbGroup> out;
    out.reserve(by_key.size());
    for (auto& [_, grp] : by_key) {
        grp.complete = (grp.r.part >= 0 && grp.g.part >= 0 && grp.b.part >= 0);
        out.push_back(std::move(grp));
    }
    return out;
}

// Read R+G+B for one layer as float32 buffers. Returns false on any
// OpenEXR / I/O failure.
bool ReadRgbPart(Imf::MultiPartInputFile& file, const RgbGroup& grp,
                 int& w_out, int& h_out,
                 std::vector<float>& r_buf,
                 std::vector<float>& g_buf,
                 std::vector<float>& b_buf)
{
    Imf::InputPart input_part(file, grp.r.part);
    const Imath::Box2i dw = file.header(grp.r.part).dataWindow();
    const int w = dw.max.x - dw.min.x + 1;
    const int h = dw.max.y - dw.min.y + 1;
    if (w <= 0 || h <= 0) return false;

    r_buf.assign(static_cast<size_t>(w) * h, 0.0f);
    g_buf.assign(static_cast<size_t>(w) * h, 0.0f);
    b_buf.assign(static_cast<size_t>(w) * h, 0.0f);

    // OpenEXR slice base must point at image coord (0,0). Our buffers
    // are indexed by (y - dw.min.y) * w + (x - dw.min.x), so the base
    // is `data - (dw.min.x + dw.min.y * w) * sizeof(float)`.
    const ptrdiff_t origin_offset =
        -(static_cast<ptrdiff_t>(dw.min.x) +
          static_cast<ptrdiff_t>(dw.min.y) * w) *
        static_cast<ptrdiff_t>(sizeof(float));

    Imf::FrameBuffer fb;
    fb.insert(grp.r.in_file_name.c_str(), Imf::Slice(
        Imf::FLOAT,
        reinterpret_cast<char*>(r_buf.data()) + origin_offset,
        sizeof(float), sizeof(float) * w));
    fb.insert(grp.g.in_file_name.c_str(), Imf::Slice(
        Imf::FLOAT,
        reinterpret_cast<char*>(g_buf.data()) + origin_offset,
        sizeof(float), sizeof(float) * w));
    fb.insert(grp.b.in_file_name.c_str(), Imf::Slice(
        Imf::FLOAT,
        reinterpret_cast<char*>(b_buf.data()) + origin_offset,
        sizeof(float), sizeof(float) * w));

    input_part.setFrameBuffer(fb);
    input_part.readPixels(dw.min.y, dw.max.y);

    w_out = w;
    h_out = h;
    return true;
}

// Box-filter downsample R/G/B float buffers into a normalized RGBA8
// thumbnail. Aspect-preserving — height scales from `target_max_w`.
// We normalize by the layer's own peak channel so weak lights remain
// visible at preview scale; this trades off realistic relative
// intensities for "can the user see the spatial position at all".
void GenerateThumbnail(const std::vector<float>& r,
                       const std::vector<float>& g,
                       const std::vector<float>& b,
                       int w, int h,
                       int target_max_w,
                       std::vector<uint8_t>& out_rgba,
                       int& out_w, int& out_h)
{
    if (target_max_w < 1) target_max_w = 1;
    out_w = std::min(target_max_w, w);
    if (out_w < 1) out_w = 1;
    out_h = std::max(1, h * out_w / std::max(1, w));

    // Find max channel value for per-layer normalization. Negative
    // pixels (float ringing) are clamped to 0.
    float max_v = 0.f;
    const size_t n = static_cast<size_t>(w) * h;
    for (size_t i = 0; i < n; ++i) {
        float rv = std::max(0.f, r[i]);
        float gv = std::max(0.f, g[i]);
        float bv = std::max(0.f, b[i]);
        if (rv > max_v) max_v = rv;
        if (gv > max_v) max_v = gv;
        if (bv > max_v) max_v = bv;
    }
    const float inv_max = max_v > 0.f ? 1.0f / max_v : 0.f;

    out_rgba.assign(static_cast<size_t>(out_w) * out_h * 4, 0);

    // For each output pixel, average the corresponding box of inputs.
    for (int oy = 0; oy < out_h; ++oy) {
        const int y0 = (oy * h) / out_h;
        const int y1 = std::max(y0 + 1, ((oy + 1) * h) / out_h);
        for (int ox = 0; ox < out_w; ++ox) {
            const int x0 = (ox * w) / out_w;
            const int x1 = std::max(x0 + 1, ((ox + 1) * w) / out_w);

            double sum_r = 0, sum_g = 0, sum_b = 0;
            int count = 0;
            for (int yi = y0; yi < y1; ++yi) {
                const size_t row = static_cast<size_t>(yi) * w;
                for (int xi = x0; xi < x1; ++xi) {
                    sum_r += std::max(0.f, r[row + xi]);
                    sum_g += std::max(0.f, g[row + xi]);
                    sum_b += std::max(0.f, b[row + xi]);
                    ++count;
                }
            }
            if (count > 0) {
                sum_r /= count;
                sum_g /= count;
                sum_b /= count;
            }
            // Per-layer normalize, then gamma 2.0 (sqrt) as a cheap
            // linear -> sRGB-ish encoding.
            float fr = static_cast<float>(sum_r) * inv_max;
            float fg = static_cast<float>(sum_g) * inv_max;
            float fb = static_cast<float>(sum_b) * inv_max;
            if (fr < 0.f) fr = 0.f; else if (fr > 1.f) fr = 1.f;
            if (fg < 0.f) fg = 0.f; else if (fg > 1.f) fg = 1.f;
            if (fb < 0.f) fb = 0.f; else if (fb > 1.f) fb = 1.f;
            const auto pack = [](float v) {
                int n = static_cast<int>(std::sqrt(v) * 255.f + 0.5f);
                return static_cast<uint8_t>(n < 0 ? 0 : (n > 255 ? 255 : n));
            };
            const size_t oi = (static_cast<size_t>(oy) * out_w + ox) * 4;
            out_rgba[oi + 0] = pack(fr);
            out_rgba[oi + 1] = pack(fg);
            out_rgba[oi + 2] = pack(fb);
            out_rgba[oi + 3] = 255;
        }
    }
}

// Metrics computed from one pass over the R/G/B buffers:
//   - total luminance
//   - whole-layer luminance-weighted centroid (cx, cy)
//   - hotspot centroid: luminance-weighted centroid restricted to
//     pixels at or above 50% of the peak luminance. Snaps to the
//     bright concentrated source and ignores soft spill.
//   - single brightest pixel position + value
//
// Two passes total: first finds peak, second accumulates the
// thresholded centroid alongside the whole-layer one. Each pass is
// O(w*h) and memory-bandwidth-limited, so doing them together is
// barely more expensive than the single centroid we used to do.
struct ScanMetrics {
    double total = 0.0;
    float  cx = 0.f, cy = 0.f;
    float  cx_hot = 0.f, cy_hot = 0.f;
    float  peak_x = 0.f, peak_y = 0.f;
    float  peak_lum = 0.f;
};

ScanMetrics ComputeMetrics(const std::vector<float>& r,
                           const std::vector<float>& g,
                           const std::vector<float>& b,
                           int w, int h)
{
    ScanMetrics m;
    if (w <= 1 || h <= 1) return m;

    // First pass: peak luminance + whole-layer centroid + total.
    double sum_L = 0.0, sum_Lx = 0.0, sum_Ly = 0.0;
    float  peak = 0.f;
    int    peak_ix = 0, peak_iy = 0;

    for (int y = 0; y < h; ++y) {
        double row_sum = 0.0, row_sum_x = 0.0;
        const size_t row = static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float lum = kR709 * r[row + x] + kG709 * g[row + x] + kB709 * b[row + x];
            if (lum < 0.f) lum = 0.f;
            row_sum   += lum;
            row_sum_x += static_cast<double>(lum) * x;
            if (lum > peak) { peak = lum; peak_ix = x; peak_iy = y; }
        }
        sum_L  += row_sum;
        sum_Lx += row_sum_x;
        sum_Ly += row_sum * y;
    }

    m.total    = sum_L;
    m.peak_lum = peak;
    m.peak_x   = static_cast<float>(peak_ix) / std::max(1, w - 1);
    m.peak_y   = static_cast<float>(peak_iy) / std::max(1, h - 1);

    if (sum_L > 0.0) {
        m.cx = static_cast<float>(sum_Lx / sum_L / std::max(1, w - 1));
        m.cy = static_cast<float>(sum_Ly / sum_L / std::max(1, h - 1));
    }

    // Second pass: hotspot centroid (>= 50% of peak). Falls back to
    // the peak pixel if too few qualifying pixels exist.
    if (peak > 0.f) {
        const float thresh = peak * 0.5f;
        double hs_L = 0.0, hs_Lx = 0.0, hs_Ly = 0.0;
        for (int y = 0; y < h; ++y) {
            const size_t row = static_cast<size_t>(y) * w;
            for (int x = 0; x < w; ++x) {
                float lum = kR709 * r[row + x] + kG709 * g[row + x] + kB709 * b[row + x];
                if (lum < thresh) continue;
                hs_L  += lum;
                hs_Lx += static_cast<double>(lum) * x;
                hs_Ly += static_cast<double>(lum) * y;
            }
        }
        if (hs_L > 0.0) {
            m.cx_hot = static_cast<float>(hs_Lx / hs_L / std::max(1, w - 1));
            m.cy_hot = static_cast<float>(hs_Ly / hs_L / std::max(1, h - 1));
        } else {
            m.cx_hot = m.peak_x;
            m.cy_hot = m.peak_y;
        }
    } else {
        m.cx_hot = m.cx;
        m.cy_hot = m.cy;
    }
    return m;
}

void DoScan(const std::string& path, PanelState* state,
            bool append, uint32_t fixed_source_id)
{
    try {
        Imf::MultiPartInputFile file(path.c_str());

        {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_status = "Enumerating layers...";
        }

        std::vector<RgbGroup> groups = BuildRgbGroups(file);

        // Use the data window of part 0 as the headline image size.
        int img_w = 0, img_h = 0;
        if (file.parts() > 0) {
            const Imath::Box2i dw = file.header(0).dataWindow();
            img_w = dw.max.x - dw.min.x + 1;
            img_h = dw.max.y - dw.min.y + 1;
        }

        std::vector<LayerInfo> layers;
        std::vector<SkippedLayer> skipped;
        layers.reserve(groups.size());

        std::vector<float> r_buf, g_buf, b_buf;
        size_t scanned_idx = 0;
        const size_t scan_total = groups.size();

        for (const auto& grp : groups) {
            ++scanned_idx;
            {
                std::lock_guard<std::mutex> lk(state->mu);
                state->last_status =
                    "Scanning " + std::to_string(scanned_idx) +
                    "/" + std::to_string(scan_total) + ": " + grp.display_name;
            }

            if (!grp.complete) {
                skipped.push_back({grp.display_name, "not RGB-complete"});
                continue;
            }
            if (std::string reason = SkipReason(grp.display_name); !reason.empty()) {
                skipped.push_back({grp.display_name, reason});
                continue;
            }

            int w = 0, h = 0;
            if (!ReadRgbPart(file, grp, w, h, r_buf, g_buf, b_buf)) {
                skipped.push_back({grp.display_name, "read failed"});
                continue;
            }

            ScanMetrics m = ComputeMetrics(r_buf, g_buf, b_buf, w, h);
            if (m.total <= 0.0) {
                skipped.push_back({grp.display_name, "all black"});
                continue;
            }

            LayerInfo info;
            info.display_name = grp.display_name;
            info.fnv1a_hash   = FNV1a32(grp.display_name);
            info.cx           = m.cx;
            info.cy           = m.cy;
            info.cx_hot       = m.cx_hot;
            info.cy_hot       = m.cy_hot;
            info.peak_x       = m.peak_x;
            info.peak_y       = m.peak_y;
            info.peak_lum     = m.peak_lum;
            info.total        = m.total;

            // Thumbnail downsample from the same R/G/B buffers we
            // just centroided. Snapshot the user-requested width
            // once outside the per-layer hot loop so a slider tweak
            // mid-scan doesn't make heights inconsistent across rows.
            int tw = 0, th = 0;
            int target_w = 256;
            {
                std::lock_guard<std::mutex> lk(state->mu);
                target_w = state->thumb_max_width;
            }
            GenerateThumbnail(r_buf, g_buf, b_buf, w, h, target_w,
                              info.thumb_rgba, tw, th);
            info.thumb_w = tw;
            info.thumb_h = th;

            layers.push_back(std::move(info));
        }

        // Initial sort uses the current UI sort settings so a re-scan
        // doesn't surprise the user with a different order. The UI
        // can re-sort live without re-scanning.
        PanelState::SortMode mode;
        bool reverse;
        uint32_t seed;
        {
            std::lock_guard<std::mutex> lk(state->mu);
            mode    = state->sort_mode;
            reverse = state->sort_reverse;
            seed    = state->random_seed;
        }
        SortLayers(layers, mode, reverse, seed);

        {
            std::lock_guard<std::mutex> lk(state->mu);
            Source src;
            if (fixed_source_id != 0) {
                src.source_id = fixed_source_id;
                if (state->next_source_id <= fixed_source_id) {
                    state->next_source_id = fixed_source_id + 1;
                }
            } else {
                src.source_id = state->next_source_id++;
            }
            src.path         = path;
            src.image_width  = img_w;
            src.image_height = img_h;
            src.layers       = std::move(layers);
            src.skipped      = std::move(skipped);
            if (!append) {
                state->sources.clear();
            }
            state->sources.push_back(std::move(src));
            state->active_source_index = (int)state->sources.size() - 1;
            state->last_error.clear();
            state->last_status  = "Scan complete.";
        }
        // Auto-tag-by-name once the source is published. Idempotent
        // against repeated calls (existing tags get new members added,
        // not duplicated). Locks state.mu internally, so call here
        // outside our own publish lock.
        AutotagByName(state);
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error  = std::string("EXR scan failed: ") + e.what();
        state->last_status = "Scan failed.";
        if (!append) {
            state->sources.clear();
            state->active_source_index = -1;
        }
    }
}

} // namespace

void StartScan(const std::string& path, PanelState* state,
               bool append, uint32_t source_id)
{
    if (!state) return;
    if (state->scanning.exchange(true)) {
        return;  // already scanning, ignore re-entry
    }
    // Bump generation BEFORE the worker starts mutating state so
    // the platform renderer can drop any previous-scan textures on
    // its next frame before new thumbnails start arriving.
    state->scan_generation.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_status = "Opening EXR...";
        state->last_error.clear();
    }
    std::thread([state, path, append, source_id]() {
        DoScan(path, state, append, source_id);
        state->scanning = false;
    }).detach();
}

bool IncludeSkippedLayer(PanelState* state, uint32_t source_id,
                         const std::string& display_name)
{
    if (!state) return false;

    std::string exr_path;
    int target_thumb_width = 384;
    {
        std::lock_guard<std::mutex> lk(state->mu);
        const Source* src = FindSourceById(*state, source_id);
        if (!src) {
            state->last_error = "Source not found.";
            return false;
        }
        exr_path = src->path;
        target_thumb_width = state->thumb_max_width;
    }
    if (exr_path.empty()) return false;

    try {
        Imf::MultiPartInputFile file(exr_path.c_str());
        std::vector<RgbGroup> groups = BuildRgbGroups(file);

        const RgbGroup* match = nullptr;
        for (const auto& g : groups) {
            if (g.display_name == display_name) { match = &g; break; }
        }
        if (!match) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error = "Layer not found in EXR: " + display_name;
            return false;
        }
        if (!match->complete) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error = "Layer is not RGB-complete: " + display_name;
            return false;
        }

        std::vector<float> r_buf, g_buf, b_buf;
        int w = 0, h = 0;
        if (!ReadRgbPart(file, *match, w, h, r_buf, g_buf, b_buf)) {
            std::lock_guard<std::mutex> lk(state->mu);
            state->last_error = "Read failed for layer: " + display_name;
            return false;
        }

        ScanMetrics m = ComputeMetrics(r_buf, g_buf, b_buf, w, h);

        LayerInfo info;
        info.display_name = display_name;
        info.fnv1a_hash   = FNV1a32(display_name);
        info.cx           = m.cx;
        info.cy           = m.cy;
        info.cx_hot       = m.cx_hot;
        info.cy_hot       = m.cy_hot;
        info.peak_x       = m.peak_x;
        info.peak_y       = m.peak_y;
        info.peak_lum     = m.peak_lum;
        info.total        = m.total;
        int tw = 0, th = 0;
        GenerateThumbnail(r_buf, g_buf, b_buf, w, h, target_thumb_width,
                          info.thumb_rgba, tw, th);
        info.thumb_w = tw;
        info.thumb_h = th;

        std::lock_guard<std::mutex> lk(state->mu);
        Source* src = FindSourceById(*state, source_id);
        if (!src) return false;
        // Remove from skipped (match by display_name).
        for (auto it = src->skipped.begin(); it != src->skipped.end(); ) {
            if (it->display_name == display_name) it = src->skipped.erase(it);
            else ++it;
        }
        src->layers.push_back(std::move(info));
        // Re-sort with the active mode so the new row lands in the
        // expected position.
        SortLayers(src->layers, state->sort_mode,
                   state->sort_reverse, state->random_seed);
        state->last_status = "Included '" + display_name + "' from skipped list.";
        state->last_error.clear();
        return true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(state->mu);
        state->last_error = std::string("Include failed: ") + e.what();
        return false;
    }
}

} // namespace exr_scan
