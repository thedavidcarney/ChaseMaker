// Per-layer luminance metrics — the numbers every sort mode orders by.
// Fed synthetic float planes, so the expected centroid is known exactly
// rather than eyeballed off a render.

#include "test_harness.h"

#include <vector>

#include "exr_scan.h"
#include "panel_state.h"

namespace {

struct Frame {
    int w = 0, h = 0;
    std::vector<float> r, g, b;

    Frame(int w_, int h_) : w(w_), h(h_),
        r((size_t)w_ * h_, 0.f), g((size_t)w_ * h_, 0.f),
        b((size_t)w_ * h_, 0.f) {}

    void Set(int x, int y, float v)
    {
        const size_t i = (size_t)y * w + x;
        r[i] = g[i] = b[i] = v;
    }
    void Box(int x0, int y0, int x1, int y1, float v)
    {
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) Set(x, y, v);
    }
    LayerInfo Analyze(int thumb_max_w = 64) const
    {
        LayerInfo out;
        exr_scan::AnalyzeFramePixels(r, g, b, w, h, thumb_max_w, out);
        return out;
    }
};

} // namespace

TEST(single_bright_pixel_lands_on_its_own_coordinates)
{
    Frame f(64, 32);
    f.Set(16, 8, 1.f);
    const LayerInfo L = f.Analyze();

    CHECK_NEAR(L.cx, 16.0 / 63.0, 1e-5);
    CHECK_NEAR(L.cy,  8.0 / 31.0, 1e-5);
    CHECK_NEAR(L.peak_x, 16.0 / 63.0, 1e-5);
    CHECK_NEAR(L.peak_y,  8.0 / 31.0, 1e-5);
    CHECK_NEAR(L.cx_hot, L.cx, 1e-5);
    CHECK_NEAR(L.cy_hot, L.cy, 1e-5);
}

TEST(two_equal_lights_average_to_the_midpoint)
{
    Frame f(65, 33);
    f.Set(0, 16, 1.f);
    f.Set(64, 16, 1.f);
    const LayerInfo L = f.Analyze();

    CHECK_NEAR(L.cx, 0.5, 1e-5);
    CHECK_NEAR(L.cy, 0.5, 1e-5);
}

// The distinction the sort modes hang on: a broad dim spill on the
// left plus a small bright core on the right. The whole-layer centroid
// gets dragged toward the spill; the hotspot centroid stays on the core.
TEST(hotspot_ignores_spill_that_drags_the_centroid)
{
    Frame f(101, 21);
    f.Box(0, 0, 40, 20, 0.05f);            // broad dim spill, left
    f.Box(88, 9, 92, 11, 1.0f);            // small bright core, right
    const LayerInfo L = f.Analyze();

    CHECK(L.cx < 0.6f);                    // pulled left by the spill
    CHECK(L.cx_hot > 0.85f);               // stays on the core
    CHECK(L.cx_hot > L.cx);
    CHECK_NEAR(L.cx_hot, 90.0 / 100.0, 0.02);
    CHECK_NEAR(L.peak_x, 88.0 / 100.0, 0.05);
}

TEST(negative_pixels_are_clamped_not_subtracted)
{
    Frame a(33, 33);
    a.Set(8, 16, 1.f);
    const LayerInfo clean = a.Analyze();

    Frame b(33, 33);
    b.Set(8, 16, 1.f);
    b.Set(24, 16, -5.f);                   // negative lobe, right side
    const LayerInfo dirty = b.Analyze();

    CHECK_NEAR(dirty.cx, clean.cx, 1e-5);
    CHECK(dirty.total >= 0.0);
}

TEST(total_luminance_sums_the_frame)
{
    Frame f(10, 10);
    f.Box(0, 0, 9, 9, 0.5f);
    const LayerInfo L = f.Analyze();
    // Rec.709 weights sum to 1, so a flat grey of 0.5 over 100 px.
    CHECK_NEAR(L.total, 50.0, 1e-3);
}

TEST(all_black_frame_reports_no_light)
{
    Frame f(16, 16);
    const LayerInfo L = f.Analyze();
    CHECK_NEAR(L.total, 0.0, 1e-9);
}

TEST(thumbnail_is_downsampled_within_the_cap)
{
    Frame f(200, 100);
    f.Box(10, 10, 40, 40, 1.f);
    const LayerInfo L = f.Analyze(64);

    CHECK(L.thumb_w > 0);
    CHECK(L.thumb_h > 0);
    CHECK(L.thumb_w <= 64);
    CHECK_EQ((int)L.thumb_rgba.size(), L.thumb_w * L.thumb_h * 4);
    CHECK(L.thumb_peak > 0.f);
}
