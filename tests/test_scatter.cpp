// Random-scatter generation.
//
// The bar David set: it should still FEEL random, but "make sure ~4
// lights are on at a time" â€” no stretches where a clump of bright
// lights fires together and no dark holes. Placing each light's hits
// independently (the original scheme) gets the average right and the
// texture wrong, so these tests measure the lit-brightness curve, not
// just the hit count.

#include "test_harness.h"
#include "test_helpers.h"

#include <cmath>
#include <map>
#include <random>
#include <vector>

#include "chase_gen.h"

using namespace chase_gen;
using testing::MakeState;

namespace {

// A loop-mode session where brightness varies wildly: every 5th light
// is 12x the others. Clumping is glaring with a spread like this.
std::unique_ptr<PanelState> MakeUnevenState(int n, int loop_frames)
{
    auto st = MakeState(n);
    st->sources[0].frame_count = loop_frames;
    st->sources[0].animation   = true;
    for (int i = 0; i < n; ++i) {
        st->sources[0].layers[i].total = (i % 5 == 0) ? 12.0 : 1.0;
    }
    return st;
}

double BrightnessOf(const PanelState& st, const LayerRef& ref)
{
    if (const LayerInfo* L = FindLayerByRef(const_cast<PanelState&>(st), ref)) {
        return L->total;
    }
    return 0.0;
}

// Lit brightness at each frame of the loop, and its coefficient of
// variation (stddev / mean) â€” scale-free, so a lower number is a
// smoother chase regardless of how bright the scene is.
double LitBrightnessCV(const Chase& c, const PanelState& st, int loop_frames)
{
    std::vector<double> series(loop_frames, 0.0);
    for (const auto& h : c.scatter) {
        const double b = BrightnessOf(st, h.ref);
        for (int f = 0; f < loop_frames; ++f) {
            const float e = ScatterHitEnvelope((float)f, h.start_frame,
                                               (float)loop_frames, c.timing);
            if (e > 0.f) series[f] += b * e;
        }
    }
    double mean = 0.0;
    for (double v : series) mean += v;
    mean /= loop_frames;
    if (mean <= 0.0) return 0.0;
    double var = 0.0;
    for (double v : series) var += (v - mean) * (v - mean);
    var /= loop_frames;
    return std::sqrt(var) / mean;
}

// The original scheme, kept here as the baseline to beat: each light
// picks its own phase and jitters within its own buckets, with no
// coordination between lights.
void IndependentScatter(Chase& c, const PanelState& st, int loop_frames)
{
    c.scatter.clear();
    const int density = c.scatter_density < 1 ? 1 : c.scatter_density;
    const float bucket = (float)loop_frames / (float)density;
    for (const auto& L : st.sources[0].layers) {
        if (!L.included) continue;
        LayerRef ref{ st.sources[0].source_id, L.fnv1a_hash };
        uint32_t s = c.random_seed ^ (L.fnv1a_hash * 2654435761u + 0x9E3779B9u);
        std::mt19937 rng(s ? s : 1u);
        std::uniform_real_distribution<float> u01(0.f, 1.f);
        const float phase = u01(rng) * (float)loop_frames;
        for (int k = 0; k < density; ++k) {
            float t = bucket * ((float)k + u01(rng)) + phase;
            t = std::fmod(t, (float)loop_frames);
            if (t < 0.f) t += (float)loop_frames;
            c.scatter.push_back(ScatterHit{ ref, t });
        }
    }
}

Chase MakeScatterChase(int loop_frames, int density, uint32_t seed,
                       int n_lights, float lights_on)
{
    Chase c;
    c.random_scatter = true;
    c.random_seed = seed;
    // These tests are about WHERE the hits land, so the density is
    // given explicitly rather than derived from a lights-on target.
    c.scatter_density = density;
    ApplyHitPreset(c.timing, kDefaultHitPreset);
    // Hit length sized so the requested concurrency is reachable with
    // the density above â€” the placement is what is under test.
    const float hits = (float)n_lights * (float)density;
    if (hits > 0.f && loop_frames > 0) {
        c.timing.duration = lights_on * (float)loop_frames / hits;
        if (c.timing.duration < 2.f) c.timing.duration = 2.f;
        c.timing.attack = c.timing.duration / 6.f;
        if (c.timing.attack < 1.f) c.timing.attack = 1.f;
    }
    return c;
}

} // namespace

// ---- Contract preserved from the original generator ------------------

TEST(scatter_fires_every_light_density_times)
{
    auto st = MakeUnevenState(20, 300);
    Chase c = MakeScatterChase(300, 4, 99u, 20, 3.f);
    RegenerateScatter(c, *st, 30.f);

    CHECK_EQ((int)c.scatter.size(), 20 * 4);
    std::map<uint32_t, int> per_light;
    for (const auto& h : c.scatter) per_light[h.ref.fnv1a_hash]++;
    CHECK_EQ((int)per_light.size(), 20);
    for (const auto& kv : per_light) CHECK_EQ(kv.second, 4);
}

TEST(scatter_stays_inside_the_loop_and_is_seed_stable)
{
    auto st = MakeUnevenState(16, 240);
    Chase a = MakeScatterChase(240, 3, 7u, 16, 3.f);
    Chase b = a;
    RegenerateScatter(a, *st, 30.f);
    RegenerateScatter(b, *st, 30.f);

    CHECK_EQ((int)a.scatter.size(), (int)b.scatter.size());
    for (size_t i = 0; i < a.scatter.size(); ++i) {
        CHECK_NEAR(a.scatter[i].start_frame, b.scatter[i].start_frame, 1e-6);
        CHECK(a.scatter[i].start_frame >= 0.f);
        CHECK(a.scatter[i].start_frame < 240.f);
    }

    Chase d = MakeScatterChase(240, 3, 8u, 16, 3.f);
    RegenerateScatter(d, *st, 30.f);
    bool differs = false;
    for (size_t i = 0; i < a.scatter.size() && i < d.scatter.size(); ++i) {
        if (a.scatter[i].start_frame != d.scatter[i].start_frame) {
            differs = true;
            break;
        }
    }
    CHECK(differs);
}

// A light must never stack on itself: its own hits stay spread across
// the loop rather than doubling up.
TEST(scatter_never_stacks_a_light_on_itself)
{
    auto st = MakeUnevenState(12, 360);
    const int density = 4;
    Chase c = MakeScatterChase(360, density, 31u, 12, 3.f);
    RegenerateScatter(c, *st, 30.f);

    std::map<uint32_t, std::vector<float>> times;
    for (const auto& h : c.scatter) times[h.ref.fnv1a_hash].push_back(h.start_frame);

    const float ideal_gap = 360.f / density;          // 90 frames
    for (auto& kv : times) {
        std::sort(kv.second.begin(), kv.second.end());
        for (size_t i = 1; i < kv.second.size(); ++i) {
            CHECK(kv.second[i] - kv.second[i - 1] > ideal_gap * 0.4f);
        }
    }
}

// The pattern must not repeat inside the loop. The old generator
// replayed one running order every round, so a density-5 Random was
// one 2-second clip looped five times (David, 2026-09-26: "it should
// be feeling like random lights are pulsing for the entire time").
// Measured as: how many back-to-back light pairs from one round show up
// back-to-back again in another round. A replay keeps nearly all of
// them; independent rounds keep almost none.
TEST(scatter_rounds_do_not_repeat_the_same_order)
{
    const int n = 24, loop = 300, density = 5;
    auto st = MakeUnevenState(n, loop);
    Chase c = MakeScatterChase(loop, density, 2026u, n, 3.f);
    RegenerateScatter(c, *st, 30.f);

    std::vector<std::pair<float, uint32_t>> hits;
    for (const auto& h : c.scatter) hits.push_back({ h.start_frame, h.ref.fnv1a_hash });
    std::sort(hits.begin(), hits.end());
    CHECK_EQ((int)hits.size(), n * density);

    std::map<std::pair<uint32_t, uint32_t>, int> pairs;
    for (size_t i = 1; i < hits.size(); ++i) {
        pairs[{ hits[i - 1].second, hits[i].second }]++;
    }
    int repeated = 0;
    for (const auto& kv : pairs) if (kv.second > 1) repeated += kv.second - 1;
    std::printf("    repeated neighbour pairs: %d of %d\n",
                repeated, (int)hits.size() - 1);
    // A replayed order repeats ~ (density-1) * n = 96 of them. Pure
    // chance alone gives ~13 here (119 pairs drawn from 24*23).
    CHECK(repeated < 30);
}

// The seamless wrap counts too: a light that ends the loop must not
// immediately start it again.
TEST(scatter_never_stacks_a_light_across_the_wrap)
{
    const int n = 12, loop = 360, density = 4;
    auto st = MakeUnevenState(n, loop);
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        Chase c = MakeScatterChase(loop, density, seed, n, 3.f);
        RegenerateScatter(c, *st, 30.f);
        std::map<uint32_t, std::vector<float>> times;
        for (const auto& h : c.scatter) times[h.ref.fnv1a_hash].push_back(h.start_frame);
        const float ideal_gap = (float)loop / density;
        for (auto& kv : times) {
            std::sort(kv.second.begin(), kv.second.end());
            const float wrap = kv.second.front() + loop - kv.second.back();
            CHECK(wrap > ideal_gap * 0.4f);
            for (size_t i = 1; i < kv.second.size(); ++i) {
                CHECK(kv.second[i] - kv.second[i - 1] > ideal_gap * 0.4f);
            }
        }
    }
}

// ---- The new bar: an even lit-brightness curve ----------------------

TEST(scatter_smooths_brightness_better_than_independent_placement)
{
    const int n = 30, loop = 300, density = 4;
    auto st = MakeUnevenState(n, loop);

    double sum_new = 0.0, sum_old = 0.0;
    // 60, not 6: over six seeds the ratio swung by a couple of points
    // on RNG stream changes alone, straddling the 0.75 bar.
    const int trials = 60;
    for (int s = 0; s < trials; ++s) {
        Chase c = MakeScatterChase(loop, density, 1000u + s * 77u, n, 3.f);
        RegenerateScatter(c, *st, 30.f);
        sum_new += LitBrightnessCV(c, *st, loop);

        Chase o = MakeScatterChase(loop, density, 1000u + s * 77u, n, 3.f);
        IndependentScatter(o, *st, loop);
        sum_old += LitBrightnessCV(o, *st, loop);
    }
    const double cv_new = sum_new / trials;
    const double cv_old = sum_old / trials;
    std::printf("    lit-brightness CV: %.3f independent -> %.3f balanced"
                " (%.0f%% smoother)\n",
                cv_old, cv_new, (1.0 - cv_new / cv_old) * 100.0);

    CHECK(cv_new < cv_old);
    // Not a marginal win â€” the clumps should be substantially gone.
    CHECK(cv_new < cv_old * 0.75);
}

TEST(scatter_leaves_no_dark_holes)
{
    const int n = 24, loop = 240, density = 3;
    auto st = MakeUnevenState(n, loop);
    Chase c = MakeScatterChase(loop, density, 4242u, n, 3.f);
    RegenerateScatter(c, *st, 30.f);

    std::vector<int> lit(loop, 0);
    for (const auto& h : c.scatter) {
        for (int f = 0; f < loop; ++f) {
            if (ScatterHitEnvelope((float)f, h.start_frame, (float)loop,
                                   c.timing) > 0.f) {
                lit[f]++;
            }
        }
    }
    int longest_dark = 0, run = 0;
    for (int f = 0; f < loop; ++f) {
        if (lit[f] == 0) { ++run; if (run > longest_dark) longest_dark = run; }
        else run = 0;
    }
    // With 72 hits over 240 frames nothing should go dark for long.
    CHECK(longest_dark <= 3);
}

TEST(scatter_holds_the_requested_lights_on_average)
{
    const int n = 20, loop = 300, density = 4;
    auto st = MakeUnevenState(n, loop);
    Chase c = MakeScatterChase(loop, density, 5u, n, 3.f);
    RegenerateScatter(c, *st, 30.f);

    double lit = 0.0;
    for (int f = 0; f < loop; ++f) {
        for (const auto& h : c.scatter) {
            if (ScatterHitEnvelope((float)f, h.start_frame, (float)loop,
                                   c.timing) > 0.f) {
                lit += 1.0;
            }
        }
    }
    CHECK_NEAR(lit / loop, 3.0, 0.6);
}

TEST(scatter_still_looks_random_not_metronomic)
{
    const int n = 20, loop = 400, density = 3;
    auto st = MakeUnevenState(n, loop);
    Chase c = MakeScatterChase(loop, density, 11u, n, 3.f);
    RegenerateScatter(c, *st, 30.f);

    std::vector<float> t;
    for (const auto& h : c.scatter) t.push_back(h.start_frame);
    std::sort(t.begin(), t.end());
    std::vector<float> gaps;
    for (size_t i = 1; i < t.size(); ++i) gaps.push_back(t[i] - t[i - 1]);

    float mean = 0.f;
    for (float g : gaps) mean += g;
    mean /= (float)gaps.size();
    float var = 0.f;
    for (float g : gaps) var += (g - mean) * (g - mean);
    var /= (float)gaps.size();
    // Perfectly even spacing would be variance 0; real jitter must
    // survive the balancing pass.
    CHECK(std::sqrt(var) > mean * 0.15f);
}

// Diagnostic: how much of the remaining unevenness is brightness
// clumping (fixable by placement) versus the envelope phase noise that
// any scatter has (not fixable). Prints rather than asserts a target,
// so tuning the balancer has a reference point.
TEST(scatter_brightness_floor_diagnostic)
{
    const int n = 30, loop = 300, density = 4;

    auto flat = MakeState(n);
    flat->sources[0].frame_count = loop;
    flat->sources[0].animation   = true;
    for (int i = 0; i < n; ++i) flat->sources[0].layers[i].total = 1.0;

    auto uneven = MakeUnevenState(n, loop);

    double cv_flat = 0.0, cv_uneven = 0.0;
    const int trials = 6;
    for (int s = 0; s < trials; ++s) {
        Chase a = MakeScatterChase(loop, density, 300u + s * 31u, n, 3.f);
        RegenerateScatter(a, *flat, 30.f);
        cv_flat += LitBrightnessCV(a, *flat, loop);

        Chase b = MakeScatterChase(loop, density, 300u + s * 31u, n, 3.f);
        RegenerateScatter(b, *uneven, 30.f);
        cv_uneven += LitBrightnessCV(b, *uneven, loop);
    }
    cv_flat /= trials;
    cv_uneven /= trials;
    std::printf("    CV floor (equal brightness): %.3f   "
                "12x-spread scene: %.3f\n", cv_flat, cv_uneven);
    CHECK(cv_flat > 0.0);
}

// Placement must not depend on the chase's timing: dragging Duration
// or Hit Hold changes how long each hit lasts, never where the hits
// are. Otherwise the whole pattern reshuffles under the slider.
TEST(scatter_placement_is_independent_of_timing)
{
    auto st = MakeUnevenState(18, 270);

    Chase a = MakeScatterChase(270, 3, 616u, 18, 2.f);
    RegenerateScatter(a, *st, 30.f);

    Chase b = a;
    b.scatter.clear();
    b.timing.duration = a.timing.duration * 3.f;   // much longer hits
    b.timing.hold = 9.f;
    RegenerateScatter(b, *st, 30.f);

    CHECK_EQ((int)a.scatter.size(), (int)b.scatter.size());
    for (size_t i = 0; i < a.scatter.size() && i < b.scatter.size(); ++i) {
        CHECK_EQ(a.scatter[i].ref.fnv1a_hash, b.scatter[i].ref.fnv1a_hash);
        CHECK_NEAR(a.scatter[i].start_frame, b.scatter[i].start_frame, 1e-6);
    }
}

// ===== Fractional density ============================================
//
// Below one copy per light there are fewer slots than lights, so some
// lights sit the loop out. That is the ONLY way a large rig with a long
// hit gets sparse â€” David, testing at Hit 90: "the lowest setting is
// too dense."

TEST(density_below_one_lights_only_some_of_the_rig)
{
    auto st = testing::MakeState(30);
    st->sources[0].frame_count = 300;
    st->sources[0].animation   = true;

    Chase c;
    c.random_scatter = true;
    c.random_seed    = 777u;
    c.scatter_density = 0.4f;               // 12 of 30 lights
    RegenerateScatter(c, *st, 30.f);

    std::vector<uint32_t> seen;
    for (const auto& h : c.scatter) {
        bool known = false;
        for (uint32_t x : seen) if (x == h.ref.fnv1a_hash) known = true;
        if (!known) seen.push_back(h.ref.fnv1a_hash);
    }
    CHECK_EQ((int)c.scatter.size(), 12);
    CHECK_EQ((int)seen.size(), 12);         // each fires exactly once
}

// The lights that do fire are spread across the loop, not bunched into
// the fraction of it they would have occupied at full density.
TEST(a_sparse_scatter_still_covers_the_whole_loop)
{
    auto st = testing::MakeState(40);
    st->sources[0].frame_count = 300;
    st->sources[0].animation   = true;

    Chase c;
    c.random_scatter = true;
    c.random_seed    = 99u;
    c.scatter_density = 0.25f;              // 10 of 40
    RegenerateScatter(c, *st, 30.f);
    CHECK_EQ((int)c.scatter.size(), 10);
    if (c.scatter.empty()) return;

    float lo = 1e9f, hi = -1e9f;
    for (const auto& h : c.scatter) {
        if (h.start_frame < lo) lo = h.start_frame;
        if (h.start_frame > hi) hi = h.start_frame;
    }
    // Ten hits over a 300-frame loop: the span must be most of it.
    CHECK(hi - lo > 200.f);
}

// Above one copy nothing changed: every light fires, more than once.
TEST(density_above_one_still_covers_every_light)
{
    auto st = testing::MakeState(12);
    st->sources[0].frame_count = 300;
    st->sources[0].animation   = true;

    Chase c;
    c.random_scatter = true;
    c.random_seed    = 5u;
    c.scatter_density = 3.f;
    RegenerateScatter(c, *st, 30.f);
    CHECK_EQ((int)c.scatter.size(), 36);

    for (const auto& L : st->sources[0].layers) {
        int hits = 0;
        for (const auto& h : c.scatter)
            if (h.ref.fnv1a_hash == L.fnv1a_hash) ++hits;
        CHECK_EQ(hits, 3);
    }
}

// A fractional density that isn't a round number of slots still lands
// somewhere sensible, and never on an empty chase.
TEST(fractional_density_rounds_to_whole_slots)
{
    auto st = testing::MakeState(10);
    st->sources[0].frame_count = 300;
    st->sources[0].animation   = true;

    struct Case { float d; int slots; };
    const Case cases[] = {
        { 0.14f,  1 },      // rounds to 1
        { 0.5f,   5 },
        { 1.5f,  15 },
        { 0.001f, 1 },      // floored, never empty
    };
    for (const auto& tc : cases) {
        Chase c;
        c.random_scatter = true;
        c.random_seed    = 11u;
        c.scatter_density = tc.d;
        RegenerateScatter(c, *st, 30.f);
        CHECK_EQ((int)c.scatter.size(), tc.slots);
    }
}
