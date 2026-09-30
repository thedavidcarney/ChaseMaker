// Stage generation, templates, and envelope math.

#include "test_harness.h"
#include "test_helpers.h"

#include "chase_gen.h"

using namespace chase_gen;
using testing::IndexOfRef;
using testing::MakeState;

// ---- Chunks: N groups, sizes differ by at most one ------------------

TEST(chunks_split_31_lights_into_3_groups)
{
    auto st = MakeState(31);
    Chase c;
    c.sort_mode = SortMode::HotspotX;
    c.desired_stage_count = 3;
    RegenerateChaseStages(c, *st, c.desired_stage_count);

    CHECK_EQ((int)c.stages.size(), 3);
    CHECK_EQ((int)c.stages[0].members.size(), 11);
    CHECK_EQ((int)c.stages[1].members.size(), 10);
    CHECK_EQ((int)c.stages[2].members.size(), 10);
}

TEST(chunks_zero_means_one_light_per_stage)
{
    auto st = MakeState(12);
    Chase c;
    c.desired_stage_count = 0;
    RegenerateChaseStages(c, *st, c.desired_stage_count);

    CHECK_EQ((int)c.stages.size(), 12);
    for (const auto& s : c.stages) CHECK_EQ((int)s.members.size(), 1);
}

TEST(stages_follow_sort_order_and_reverse)
{
    auto st = MakeState(5);
    Chase c;
    c.sort_mode = SortMode::HotspotX;
    RegenerateChaseStages(c, *st, 0);
    CHECK_EQ(IndexOfRef(*st, c.stages.front().members[0]), 0);
    CHECK_EQ(IndexOfRef(*st, c.stages.back().members[0]), 4);

    c.sort_reverse = true;
    RegenerateChaseStages(c, *st, 0);
    CHECK_EQ(IndexOfRef(*st, c.stages.front().members[0]), 4);
    CHECK_EQ(IndexOfRef(*st, c.stages.back().members[0]), 0);
}

// ---- Center Out: mirrored pairs from the middle outward -------------

TEST(center_out_pairs_outward_from_middle)
{
    auto st = MakeState(5);
    Chase c;
    ApplyTemplateToChase(c, 2);            // Center Out
    CHECK(c.symmetric_pairs);
    RegenerateChaseStages(c, *st, 0);

    CHECK_EQ((int)c.stages.size(), 3);
    CHECK_EQ((int)c.stages[0].members.size(), 1);
    CHECK_EQ(IndexOfRef(*st, c.stages[0].members[0]), 2);
    CHECK_EQ((int)c.stages[1].members.size(), 2);
    CHECK_EQ(IndexOfRef(*st, c.stages[1].members[0]), 1);
    CHECK_EQ(IndexOfRef(*st, c.stages[1].members[1]), 3);
    CHECK_EQ(IndexOfRef(*st, c.stages[2].members[0]), 0);
    CHECK_EQ(IndexOfRef(*st, c.stages[2].members[1]), 4);
}

TEST(center_out_even_count_pairs_the_two_middle_lights)
{
    auto st = MakeState(4);
    Chase c;
    ApplyTemplateToChase(c, 2);
    RegenerateChaseStages(c, *st, 0);

    CHECK_EQ((int)c.stages.size(), 2);
    CHECK_EQ((int)c.stages[0].members.size(), 2);
    CHECK_EQ(IndexOfRef(*st, c.stages[0].members[0]), 1);
    CHECK_EQ(IndexOfRef(*st, c.stages[0].members[1]), 2);
}

// ---- Templates ------------------------------------------------------

TEST(three_step_template_is_three_chunks)
{
    Chase c;
    ApplyTemplateToChase(c, 3);
    CHECK_EQ(c.desired_stage_count, 3);
    CHECK(!c.symmetric_pairs);
    CHECK(!c.random_scatter);
}

TEST(hit_template_is_one_stage_one_cycle)
{
    auto st = MakeState(9);
    Chase c;
    ApplyTemplateToChase(c, 5);
    CHECK_EQ(c.desired_stage_count, 1);
    CHECK_EQ(c.loop_cycles, 1);
    RegenerateChaseStages(c, *st, c.desired_stage_count);
    CHECK_EQ((int)c.stages.size(), 1);
    CHECK_EQ((int)c.stages[0].members.size(), 9);
}

TEST(random_template_switches_to_scatter_and_leaves_stages_alone)
{
    auto st = MakeState(6);
    Chase c;
    ApplyTemplateToChase(c, 4);
    CHECK(c.random_scatter);
    CHECK(c.scatter_density > 0.f);
    RegenerateChaseStages(c, *st, 0);
    CHECK(c.stages.empty());               // scatter owns the hits
}

// ---- Envelopes ------------------------------------------------------

TEST(stage_envelope_rises_peaks_and_falls)
{
    ChaseTiming t;
    t.duration = 10.f; t.attack = 2.f; t.hold = 0.f; t.step_duration = 4.f;

    CHECK_NEAR(StageEnvelope(0, 0.f,  t), 0.0, 1e-5);
    CHECK_NEAR(StageEnvelope(0, 1.f,  t), 0.5, 1e-5);
    CHECK_NEAR(StageEnvelope(0, 2.f,  t), 1.0, 1e-5);
    CHECK_NEAR(StageEnvelope(0, 6.f,  t), 0.5, 1e-5);
    CHECK_NEAR(StageEnvelope(0, 10.f, t), 0.0, 1e-5);   // past the hit
    CHECK_NEAR(StageEnvelope(0, -1.f, t), 0.0, 1e-5);   // before it
}

TEST(stage_envelope_hold_holds_the_peak)
{
    ChaseTiming t;
    t.duration = 10.f; t.attack = 2.f; t.hold = 4.f;

    CHECK_NEAR(StageEnvelope(0, 2.f,  t), 1.0, 1e-5);
    CHECK_NEAR(StageEnvelope(0, 4.f,  t), 1.0, 1e-5);   // plateau
    CHECK_NEAR(StageEnvelope(0, 6.f,  t), 1.0, 1e-5);   // plateau ends
    CHECK_NEAR(StageEnvelope(0, 10.f, t), 0.5, 1e-5);   // falling
    CHECK_NEAR(StageEnvelope(0, 14.f, t), 0.0, 1e-5);   // total = dur+hold
}

TEST(stage_envelope_steps_by_step_duration)
{
    ChaseTiming t;
    t.duration = 10.f; t.attack = 2.f; t.step_duration = 4.f;
    // Stage 3 starts at frame 12, so its peak lands at 14.
    CHECK_NEAR(StageEnvelope(3, 14.f, t), 1.0, 1e-5);
    CHECK_NEAR(StageEnvelope(3, 12.f, t), 0.0, 1e-5);
}

TEST(scatter_envelope_wraps_across_the_loop_point)
{
    ChaseTiming t;
    t.duration = 10.f; t.attack = 2.f; t.hold = 0.f;
    // A hit starting 4 frames before the end of a 100-frame loop is
    // still going at frame 0 of the next iteration.
    CHECK_NEAR(ScatterHitEnvelope(96.f, 96.f, 100.f, t), 0.0,  1e-5);
    CHECK_NEAR(ScatterHitEnvelope(98.f, 96.f, 100.f, t), 1.0,  1e-5);
    // 6 frames into a 10-frame hit that peaked at 2: (10-6)/8.
    CHECK_NEAR(ScatterHitEnvelope(2.f,  96.f, 100.f, t), 0.5,  1e-5);
    CHECK_NEAR(ScatterHitEnvelope(4.f,  96.f, 100.f, t), 0.25, 1e-5);
    CHECK_NEAR(ScatterHitEnvelope(6.f,  96.f, 100.f, t), 0.0,  1e-5);
}

// The loop-point stutter regression: the period must stay fractional.
// 179 frames over 8 cycles is 22.375 — truncating to 22 wraps the
// envelope early and the preview starts a phantom cycle before the
// loop boundary.
TEST(loop_envelope_is_seamless_at_a_fractional_period)
{
    auto st = MakeState(7);
    Chase c;
    c.loop_cycles = 8;
    c.timing.duration = 10.f;
    c.timing.attack = 2.f;
    RegenerateChaseStages(c, *st, 0);

    const int loop = 179;
    for (size_t si = 0; si < c.stages.size(); ++si) {
        for (float p = 0.f; p < 4.f; p += 0.5f) {
            const float a = ChaseStageEnvelopeAt(c, si, p, loop);
            const float b = ChaseStageEnvelopeAt(c, si, p + loop, loop);
            CHECK_NEAR(a, b, 1e-4);
        }
    }
}

TEST(loop_frames_is_an_exact_multiple_of_the_source)
{
    auto st = MakeState(4);
    st->sources[0].frame_count = 120;
    st->sources[0].animation   = true;

    Chase c;
    c.loop_multiple = 1;
    CHECK_EQ(ChaseLoopFrames(c, *st, 30.f), 120);
    c.loop_multiple = 3;
    CHECK_EQ(ChaseLoopFrames(c, *st, 30.f), 360);
}

// ---- Scatter --------------------------------------------------------

TEST(scatter_is_deterministic_for_a_seed)
{
    auto st = MakeState(10);
    st->sources[0].frame_count = 120;
    st->sources[0].animation   = true;

    Chase a;
    a.random_scatter = true;
    a.random_seed = 12345;
    a.scatter_density = 4;
    Chase b = a;
    RegenerateScatter(a, *st, 30.f);
    RegenerateScatter(b, *st, 30.f);

    CHECK(!a.scatter.empty());
    CHECK_EQ((int)a.scatter.size(), (int)b.scatter.size());
    for (size_t i = 0; i < a.scatter.size(); ++i) {
        CHECK_EQ(a.scatter[i].ref.fnv1a_hash, b.scatter[i].ref.fnv1a_hash);
        CHECK_NEAR(a.scatter[i].start_frame, b.scatter[i].start_frame, 1e-6);
    }

    Chase d = a;
    d.random_seed = 999;
    d.scatter.clear();
    RegenerateScatter(d, *st, 30.f);
    bool any_moved = false;
    for (size_t i = 0; i < a.scatter.size() && i < d.scatter.size(); ++i) {
        if (a.scatter[i].start_frame != d.scatter[i].start_frame) {
            any_moved = true;
            break;
        }
    }
    CHECK(any_moved);                      // a new seed reshuffles
}

TEST(scatter_hits_stay_inside_the_loop)
{
    auto st = MakeState(8);
    st->sources[0].frame_count = 90;
    st->sources[0].animation   = true;

    Chase c;
    c.random_scatter = true;
    c.random_seed = 7;
    c.scatter_density = 5;
    RegenerateScatter(c, *st, 30.f);

    CHECK(!c.scatter.empty());
    for (const auto& h : c.scatter) {
        CHECK(h.start_frame >= 0.f);
        CHECK(h.start_frame < 90.f);
    }
}

// ---- Joined lights sort by their combined position -------------------
//
// Two render passes welded into one light are ONE light: the sweep
// should reach it where it actually sits, not where whichever member
// happened to come first sits.

namespace {

// Weld the named layers of the synthetic source into one bind.
void JoinLights(PanelState& st, std::initializer_list<int> which)
{
    Bind b;
    b.bind_id = st.next_bind_id++;
    b.name = "joined";
    for (int i : which) {
        b.members.push_back({ st.sources[0].source_id,
                              st.sources[0].layers[i].fnv1a_hash });
    }
    st.binds.push_back(b);
}

} // namespace

TEST(joined_lights_sort_by_the_combined_centroid)
{
    // Lights at x = 0.0, 0.25, 0.5, 0.75, 1.0. Weld the two ends
    // together: their combined centre is 0.5, so the joined light must
    // sort into the MIDDLE, not at the far left where member 0 sits.
    auto st = MakeState(5);
    JoinLights(*st, { 0, 4 });

    Chase c;
    c.sort_mode = SortMode::CentroidX;
    RegenerateChaseStages(c, *st, 0);

    CHECK_EQ((int)c.stages.size(), 4);          // 3 loose + 1 joined
    if (c.stages.size() != 4) return;
    // Order should be 0.25, then the joined pair (0.5), then 0.5, 0.75
    // — i.e. the joined light is not first.
    CHECK((int)c.stages[0].members.size() == 1);
    CHECK_EQ(IndexOfRef(*st, c.stages[0].members[0]), 1);   // x = 0.25
    bool joined_is_first = c.stages[0].members.size() > 1;
    CHECK(!joined_is_first);

    // And it lands in the middle of the running order.
    int joined_at = -1;
    for (size_t i = 0; i < c.stages.size(); ++i) {
        if (c.stages[i].members.size() == 2) joined_at = (int)i;
    }
    CHECK(joined_at > 0);
    CHECK(joined_at < 3);
}

TEST(joined_lights_combine_brightness_for_a_brightness_sort)
{
    auto st = MakeState(4);
    // Two dim lights welded together outweigh a single middling one.
    st->sources[0].layers[0].total = 3.0;
    st->sources[0].layers[1].total = 3.0;
    st->sources[0].layers[2].total = 5.0;
    st->sources[0].layers[3].total = 1.0;
    JoinLights(*st, { 0, 1 });                      // combined 6.0

    Chase c;
    c.sort_mode = SortMode::Brightness;         // brightest first
    RegenerateChaseStages(c, *st, 0);

    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.empty()) return;
    CHECK_EQ((int)c.stages[0].members.size(), 2);   // the joined pair
}

TEST(joined_lights_weight_the_centroid_by_brightness)
{
    // A bright light at 0.0 welded to a faint one at 1.0 should sit
    // near the bright end, not at the midpoint.
    auto st = MakeState(5);
    st->sources[0].layers[0].total = 100.0;     // x = 0.0
    st->sources[0].layers[4].total = 1.0;       // x = 1.0
    JoinLights(*st, { 0, 4 });

    Chase c;
    c.sort_mode = SortMode::CentroidX;
    RegenerateChaseStages(c, *st, 0);

    CHECK(!c.stages.empty());
    if (c.stages.empty()) return;
    // Combined centre ~0.01, so the joined light leads the sweep.
    CHECK_EQ((int)c.stages[0].members.size(), 2);
}

TEST(unjoined_lights_are_unaffected_by_the_combining)
{
    auto st = MakeState(6);
    Chase c;
    c.sort_mode = SortMode::HotspotX;
    RegenerateChaseStages(c, *st, 0);

    CHECK_EQ((int)c.stages.size(), 6);
    for (int i = 0; i < 6; ++i) {
        CHECK_EQ(IndexOfRef(*st, c.stages[i].members[0]), i);
    }
}

// ---- Peak-pixel ordering ---------------------------------------------
//
// peak_x/peak_y were computed for every layer from the first scan and,
// until the light-centre control existed, nothing ever read them.

TEST(peak_sort_orders_by_the_brightest_pixel_not_the_centroid)
{
    auto st = MakeState(3);
    // Spread and peak disagree: light 0's mass is left but its hot
    // pixel is far right, and vice versa for light 2.
    st->sources[0].layers[0].cx = 0.1f; st->sources[0].layers[0].peak_x = 0.9f;
    st->sources[0].layers[1].cx = 0.5f; st->sources[0].layers[1].peak_x = 0.5f;
    st->sources[0].layers[2].cx = 0.9f; st->sources[0].layers[2].peak_x = 0.1f;

    Chase byCentroid;
    byCentroid.sort_mode = SortMode::CentroidX;
    RegenerateChaseStages(byCentroid, *st, 0);
    CHECK_EQ(IndexOfRef(*st, byCentroid.stages.front().members[0]), 0);

    Chase byPeak;
    byPeak.sort_mode = SortMode::PeakX;
    RegenerateChaseStages(byPeak, *st, 0);
    CHECK_EQ(IndexOfRef(*st, byPeak.stages.front().members[0]), 2);
    CHECK_EQ(IndexOfRef(*st, byPeak.stages.back().members[0]), 0);
}

TEST(peak_sort_works_on_the_vertical_axis)
{
    auto st = MakeState(3);
    st->sources[0].layers[0].peak_y = 0.8f;
    st->sources[0].layers[1].peak_y = 0.2f;
    st->sources[0].layers[2].peak_y = 0.5f;

    Chase c;
    c.sort_mode = SortMode::PeakY;
    RegenerateChaseStages(c, *st, 0);
    CHECK_EQ(IndexOfRef(*st, c.stages[0].members[0]), 1);
    CHECK_EQ(IndexOfRef(*st, c.stages[1].members[0]), 2);
    CHECK_EQ(IndexOfRef(*st, c.stages[2].members[0]), 0);
}

TEST(joined_lights_combine_their_peak_positions_too)
{
    auto st = MakeState(5);
    st->sources[0].layers[0].peak_x = 0.0f;
    st->sources[0].layers[4].peak_x = 1.0f;
    JoinLights(*st, { 0, 4 });                 // combined peak ~0.5

    Chase c;
    c.sort_mode = SortMode::PeakX;
    RegenerateChaseStages(c, *st, 0);
    CHECK(!c.stages.empty());
    if (c.stages.empty()) return;
    CHECK_EQ((int)c.stages[0].members.size(), 1);   // not the joined pair
}
