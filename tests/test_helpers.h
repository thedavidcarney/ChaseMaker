// Synthetic PanelState builders for the tests.

#pragma once

#include <cstdio>
#include <memory>
#include <string>

#include "hash.h"
#include "panel_state.h"

namespace testing {

// A session with one still source holding `n` lights spread evenly
// left-to-right, named L00..Lnn. Hotspot == centroid unless a test
// moves them apart.
inline std::unique_ptr<PanelState> MakeState(int n)
{
    auto st = std::make_unique<PanelState>();
    Source src;
    src.source_id    = 1;
    src.path         = "synthetic.exr";
    src.scan_path    = src.path;
    src.frame_count  = 1;
    src.animation    = false;
    src.image_width  = 1920;
    src.image_height = 1080;
    for (int i = 0; i < n; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "L%02d", i);
        LayerInfo L;
        L.display_name = name;
        L.fnv1a_hash   = FNV1a32(L.display_name);
        L.source_id    = src.source_id;
        const float t  = (n > 1) ? static_cast<float>(i) / (n - 1) : 0.5f;
        L.cx = L.cx_hot = L.peak_x = t;
        L.cy = L.cy_hot = L.peak_y = 0.5f;
        L.total    = 1.0;
        L.included = true;
        src.layers.push_back(std::move(L));
    }
    st->sources.push_back(std::move(src));
    st->next_source_id = 2;
    return st;
}

// Give every light a solid-colour thumbnail so the preview compositor
// has something to work with. Light i lights up channel (i % 3), which
// makes it identifiable in a composited buffer.
inline void GiveThumbnails(PanelState& st, int w = 8, int h = 6)
{
    for (auto& src : st.sources) {
        for (size_t i = 0; i < src.layers.size(); ++i) {
            LayerInfo& L = src.layers[i];
            L.thumb_w = w;
            L.thumb_h = h;
            L.thumb_peak = 1.f;            // bytes encode sqrt(linear)
            L.thumb_rgba.assign((size_t)w * h * 4, 0);
            const int ch = (int)(i % 3);
            for (int p = 0; p < w * h; ++p) {
                L.thumb_rgba[(size_t)p * 4 + ch] = 255;
                L.thumb_rgba[(size_t)p * 4 + 3] = 255;
            }
        }
    }
}

// Average value of one channel over a rectangle of an RGBA buffer.
inline double RectChannelMean(const std::vector<uint8_t>& rgba, int stride_px,
                              int x0, int y0, int w, int h, int channel)
{
    double sum = 0.0;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            sum += rgba[((size_t)y * stride_px + x) * 4 + channel];
        }
    }
    const double n = (double)w * h;
    return (n > 0.0) ? sum / n : 0.0;
}


// One playhead per chase, all at the same time — the simple case for
// tests that only care about what a frame looks like.
inline std::vector<float> AllAt(float t, int n)
{
    return std::vector<float>(n < 0 ? 0 : (size_t)n, t);
}

// Index of the light a LayerRef points at, in MakeState order.
inline int IndexOfRef(const PanelState& st, const LayerRef& ref)
{
    const Source& src = st.sources[0];
    for (size_t i = 0; i < src.layers.size(); ++i) {
        if (src.layers[i].fnv1a_hash == ref.fnv1a_hash) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace testing
