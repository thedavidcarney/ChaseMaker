// The timing model, in the artist's terms.
//
// Two knobs, and each belongs to exactly one kind of chase:
//
//   Sweeps  — HOW LONG THE CLIP IS. The stagger between lights is
//             derived from it, so adding lights tightens the spacing
//             rather than lengthening the chase.
//   Random  — HOW MANY LIGHTS ARE LIT AT A TIME. The number of copies
//             of each light (density) is derived from it, against the
//             10-second loop convention.
//
// Both used to run the other way round: the panel asked for density
// and for a per-light step, and made the artist do the arithmetic that
// belongs here.

#include "test_harness.h"
#include "test_helpers.h"

#include <cmath>

#include "chase_gen.h"

using namespace chase_gen;
using testing::MakeState;

// ---- Sweeps: one knob, the length of the clip ------------------------

TEST(sweep_timing_fits_the_requested_clip_length)
{
    ChaseTiming base;                       // Hit 30: duration 30, attack 5
    // 20 lights across 4 seconds at 30fps = 120 frames end to end.
    const ChaseTiming t = FitSweepTiming(20, 0, 1, 30.f, base, 4.f);
    const float total = (20 - 1) * t.step_duration + t.duration;
    CHECK_NEAR(total, 120.0, 0.5);
}

// The property David asked for: more lights tightens the spacing, it
// does not make the chase run longer.
TEST(sweep_timing_keeps_its_length_as_lights_are_added)
{
    ChaseTiming base;
    const ChaseTiming few  = FitSweepTiming(10,  0, 1, 30.f, base, 4.f);
    const ChaseTiming many = FitSweepTiming(100, 0, 1, 30.f, base, 4.f);

    CHECK(many.step_duration < few.step_duration);   // tighter stagger
    const float t_few  = (10 - 1)  * few.step_duration  + few.duration;
    const float t_many = (100 - 1) * many.step_duration + many.duration;
    CHECK_NEAR(t_few, t_many, 1.0);                  // same clip length
    CHECK_NEAR(t_few, 120.0, 1.0);
}

TEST(sweep_timing_leaves_the_hit_envelope_alone)
{
    ChaseTiming base;
    base.duration = 60.f;                   // Hit 60
    base.attack   = 30.f;
    const ChaseTiming t = FitSweepTiming(30, 0, 1, 30.f, base, 4.f);
    CHECK_NEAR(t.duration, 60.0, 1e-4);     // the hit is the artist's
    CHECK_NEAR(t.attack, 30.0, 1e-4);
}

TEST(sweep_timing_scales_with_the_frame_rate)
{
    ChaseTiming base;
    const ChaseTiming a = FitSweepTiming(20, 0, 1, 60.f, base, 4.f);
    const float total = (20 - 1) * a.step_duration + a.duration;
    CHECK_NEAR(total, 240.0, 1.0);          // 4s at 60fps
}

TEST(sweep_timing_survives_a_hit_longer_than_the_clip)
{
    ChaseTiming base;
    base.duration = 90.f;
    base.attack = 45.f;
    // 30 lights, 1-second target, 3-second hits: impossible, but it
    // must still produce something playable.
    const ChaseTiming t = FitSweepTiming(30, 0, 1, 30.f, base, 1.f);
    CHECK(t.step_duration > 0.f);
    CHECK(t.duration > t.attack);
}

TEST(sweep_timing_handles_a_single_light)
{
    ChaseTiming base;
    const ChaseTiming t = FitSweepTiming(1, 0, 1, 30.f, base, 4.f);
    CHECK(t.step_duration > 0.f);
    CHECK_NEAR(t.duration, 30.0, 1e-4);
}

// In loop mode the scene's own loop dictates the spacing — the stages
// spread across it so the chase wraps seamlessly. The clip-length knob
// does not apply there.
TEST(sweep_timing_in_loop_mode_spreads_across_the_loop)
{
    ChaseTiming base;
    const ChaseTiming t = FitSweepTiming(60, 120, 1, 30.f, base, 4.f);
    CHECK_NEAR(t.step_duration, 2.0, 1e-4);          // 120 / 60
    const ChaseTiming t2 = FitSweepTiming(60, 120, 2, 30.f, base, 4.f);
    CHECK_NEAR(t2.step_duration, 1.0, 1e-4);         // 120 / (60 * 2)
}

// ---- Random: one knob, how many lights are lit -----------------------

TEST(density_comes_from_how_many_lights_should_be_lit)
{
    // 12 lights, 1-second hits, the 10-second loop: 5 copies each.
    CHECK_NEAR(DensityForLightsOn(9.f, 12, 300, 45.f), 5.0, 1e-3);
    // 126 lights, same hit: about 3 copies each.
    CHECK_NEAR(DensityForLightsOn(57.f, 126, 300, 45.f), 3.0, 0.03);
}

// The point of the control: the same artistic request lands on very
// different densities depending on how big the rig is, which is the
// arithmetic the artist used to do by hand.
TEST(the_same_request_gives_different_densities_per_scene)
{
    const float small = DensityForLightsOn(3.f, 6,  300, 30.f);
    const float big   = DensityForLightsOn(3.f, 60, 300, 30.f);
    CHECK(small > big);
    CHECK(small > 0.f);
    CHECK(big > 0.f);
}

// Density goes BELOW one copy per light, and has to: on a 126-light rig
// with 30-frame hits, one copy of everything already means 12.6 lit at
// any instant, so "3 lit" is only reachable by letting most of the rig
// sit the loop out. This used to clamp at 1 and the artist had no way
// down. (David, testing: "the lowest setting is too dense.")
TEST(density_goes_below_one_copy_so_a_big_rig_can_be_sparse)
{
    const float d = DensityForLightsOn(3.f, 126, 300, 30.f);
    CHECK(d < 1.f);
    CHECK(d > 0.f);
    // 3 lit / 12.6 lit-per-copy
    CHECK_NEAR(d, 3.0 * 300.0 / (126.0 * 30.0), 1e-4);
    // Asking for nothing still leaves one light firing — an empty
    // scatter is not a chase.
    const float none = DensityForLightsOn(0.f, 20, 300, 30.f);
    CHECK(none > 0.f);
    CHECK_NEAR(none, 1.0 / 20.0, 1e-6);
}

TEST(density_handles_nonsense_without_dividing_by_zero)
{
    CHECK(DensityForLightsOn(3.f, 0, 300, 30.f) > 0.f);
    CHECK(DensityForLightsOn(3.f, 10, 0, 30.f) > 0.f);
    CHECK(DensityForLightsOn(3.f, 10, 300, 0.f) > 0.f);
}

// Round trip: ask for N lit, get a density, and the resulting scatter
// really does hold about N lit.
TEST(the_derived_density_delivers_the_requested_lights_on)
{
    auto st = MakeState(20);
    st->sources[0].frame_count = 300;         // 10s at 30fps
    st->sources[0].animation   = true;

    for (float want : { 3.f, 6.f }) {
        Chase c;
        c.random_scatter = true;
        c.random_seed = 4242u;
        ApplyHitPreset(c.timing, kDefaultHitPreset);        // Hit 30
        c.scatter_density =
            DensityForLightsOn(want, 20, 300, c.timing.duration);
        RegenerateScatter(c, *st, 30.f);

        double lit = 0.0;
        for (int f = 0; f < 300; ++f) {
            for (const auto& h : c.scatter) {
                if (ScatterHitEnvelope((float)f, h.start_frame, 300.f,
                                       c.timing) > 0.f) lit += 1.0;
            }
        }
        // Density is a whole number of copies, so the landing is
        // approximate by construction.
        CHECK_NEAR(lit / 300.0, want, want * 0.35);
    }
}

// ---- The hit presets, straight off Noah's Kbar buttons ---------------

TEST(hit_presets_match_the_kbar_files)
{
    CHECK_EQ(kHitPresetCount, 4);
    const float dur[] = { 15.f, 30.f, 60.f, 90.f };
    const float att[] = {  4.f,  5.f, 30.f, 45.f };
    for (int i = 0; i < kHitPresetCount; ++i) {
        CHECK_NEAR(kHitPresets[i].duration, dur[i], 1e-4);
        CHECK_NEAR(kHitPresets[i].attack,   att[i], 1e-4);
    }
}

TEST(the_default_hit_is_the_30_frame_preset)
{
    ChaseTiming t;
    ApplyHitPreset(t, kDefaultHitPreset);
    CHECK_NEAR(t.duration, 30.0, 1e-4);
    CHECK_NEAR(t.attack, 5.0, 1e-4);
    // And it is what a fresh ChaseTiming already is.
    const ChaseTiming fresh;
    CHECK_NEAR(fresh.duration, t.duration, 1e-4);
    CHECK_NEAR(fresh.attack, t.attack, 1e-4);
    CHECK_NEAR(fresh.gamma_baseline, 0.25, 1e-4);
    CHECK_NEAR(fresh.gamma_peak, 1.0, 1e-4);
}

TEST(applying_a_hit_preset_leaves_the_rest_of_the_timing_alone)
{
    ChaseTiming t;
    t.step_duration = 7.f;
    t.opacity_peak = 80.f;
    t.gamma_baseline = 0.4f;
    ApplyHitPreset(t, 2);                     // Hit 60
    CHECK_NEAR(t.duration, 60.0, 1e-4);
    CHECK_NEAR(t.attack, 30.0, 1e-4);
    CHECK_NEAR(t.step_duration, 7.0, 1e-4);   // pacing is separate
    CHECK_NEAR(t.opacity_peak, 80.0, 1e-4);
    CHECK_NEAR(t.gamma_baseline, 0.4, 1e-4);
}

TEST(the_current_hit_can_be_named_back)
{
    ChaseTiming t;
    ApplyHitPreset(t, 3);
    CHECK_EQ(NearestHitPreset(t), 3);
    ApplyHitPreset(t, 0);
    CHECK_EQ(NearestHitPreset(t), 0);
    // A hand-edited length reports the closest preset rather than
    // pretending to be exact.
    t.duration = 34.f;
    CHECK_EQ(NearestHitPreset(t), 1);
}

// ---- Chunked chases: the hit drives everything -----------------------
//
// David: "We use the same hit timings, so give us the dropdown for
// that. Then the next one will trigger as the previous one is fading
// out." The moment a hit starts fading is its attack, so the step IS
// the attack and there is no separate length to set.

TEST(chunked_step_is_the_hits_attack)
{
    ChaseTiming base;                        // Hit 30: duration 30, attack 5
    const ChaseTiming t = FitChunkedTiming(3, 0, 1, 30.f, base, 4.f);
    CHECK_NEAR(t.step_duration, 10.0, 1e-4); // David's default, flat
    CHECK_NEAR(t.duration, 30.0, 1e-4);      // the preset, untouched
    CHECK_NEAR(t.attack, 5.0, 1e-4);
}

// The hit and the spacing are INDEPENDENT knobs. Picking a longer hit
// makes each chunk last longer; it does not move the beat the next one
// arrives on. (An earlier build derived the step from the hit's attack;
// David replaced that with a flat default he tweaks per scene.)
TEST(chunked_spacing_is_independent_of_the_hit)
{
    for (int i = 0; i < kHitPresetCount; ++i) {
        ChaseTiming base;
        ApplyHitPreset(base, i);
        const ChaseTiming t = FitChunkedTiming(3, 0, 1, 30.f, base, 4.f);
        CHECK_NEAR(t.step_duration, kDefaultChunkStepFrames, 1e-4);
        CHECK_NEAR(t.duration, kHitPresets[i].duration, 1e-4);
        CHECK_NEAR(t.attack, kHitPresets[i].attack, 1e-4);
    }
}

TEST(chunked_length_falls_out_of_the_hit_and_the_spacing)
{
    ChaseTiming base;
    ApplyHitPreset(base, 2);                 // Hit 60
    const ChaseTiming t = FitChunkedTiming(3, 0, 1, 30.f, base, 4.f);
    // (chunks - 1) * step + hit = 2*10 + 60
    const float total = 2 * t.step_duration + t.duration;
    CHECK_NEAR(total, 80.0, 1e-3);
}

// The next chunk is up before the previous one is out — nothing ever
// goes fully dark inside the chase.
TEST(chunked_timing_never_goes_fully_dark)
{
    auto st = MakeState(9);
    Chase c;
    c.sort_mode = SortMode::HotspotX;
    c.desired_stage_count = 3;
    RegenerateChaseStages(c, *st, 3);
    CHECK_EQ((int)c.stages.size(), 3);
    c.timing = FitChunkedTiming(3, 0, 1, 30.f, c.timing, 4.f);

    const float total = 2 * c.timing.step_duration + c.timing.duration;
    for (float f = 0.5f; f < total - 0.5f; f += 0.5f) {
        float lit = 0.f;
        for (size_t si = 0; si < c.stages.size(); ++si) {
            lit += ChaseStageEnvelopeAt(c, si, f, 0);
        }
        CHECK(lit > 0.f);
    }
}

// A chunk fires while its predecessor is still on its way down.
TEST(each_chunk_starts_while_the_last_is_still_fading)
{
    ChaseTiming base;
    const ChaseTiming t = FitChunkedTiming(3, 0, 1, 30.f, base, 4.f);
    Chase c;
    c.timing = t;
    c.manual_stages = true;
    for (int i = 0; i < 3; ++i) c.stages.push_back(ChaseStage{});

    // At the instant chunk 1 starts, chunk 0 is past its peak and
    // still lit.
    const float when = t.step_duration + 0.01f;
    const float prev = ChaseStageEnvelopeAt(c, 0, when, 0);
    CHECK(prev > 0.f);
    CHECK(prev < 1.f);                       // already coming down
    // And later still, more than one is up.
    int most = 0;
    const float total = 2 * t.step_duration + t.duration;
    for (float f = 0.5f; f < total; f += 0.5f) {
        int on = 0;
        for (size_t si = 0; si < 3; ++si) {
            if (ChaseStageEnvelopeAt(c, si, f, 0) > 0.f) ++on;
        }
        if (on > most) most = on;
    }
    CHECK(most >= 2);
}

// ---- 3 Step loops instead of fading to black -------------------------
//
// David: "3 step is not a 10 second loop. Like the other non-random
// chases its duration is determined by the hit length and the amount of
// overlap. The difference with this one from those is that it loops
// instead of fading to black." The chunks keep firing `step` apart, so
// one cycle is chunks * step, and the hit outlasting the step is what
// makes the last tail wrap round to the start.

TEST(the_three_step_template_loops)
{
    Chase c;
    ApplyTemplateToChase(c, 3);             // 3 Step
    CHECK(c.loops);
    CHECK_EQ(c.desired_stage_count, 3);
    // The sweeps do not.
    Chase s;
    ApplyTemplateToChase(s, 0);
    CHECK(!s.loops);
}

TEST(a_looping_chunked_chase_cycles_on_its_own_spacing)
{
    auto st = MakeState(9);                 // stills: no session loop
    Chase c;
    ApplyTemplateToChase(c, 3);
    RegenerateChaseStages(c, *st, 3);
    c.timing = FitChunkedTiming(3, 0, 1, 30.f, c.timing, 4.f);

    // One cycle is chunks * step, NOT the 10-second scatter loop.
    CHECK(ChaseWraps(c, *st));
    CHECK_EQ(ChaseLoopFrames(c, *st, 30.f),
             (int)std::lround(c.timing.step_duration * 3.f));
    CHECK(ChaseLoopFrames(c, *st, 30.f) < 300);      // not 10 seconds
}

// The tail wraps: the pattern is continuous across the loop point
// rather than going dark and starting again.
TEST(a_looping_chunked_chase_wraps_its_tail_to_the_start)
{
    auto st = MakeState(9);
    Chase c;
    ApplyTemplateToChase(c, 3);
    RegenerateChaseStages(c, *st, 3);
    c.timing = FitChunkedTiming(3, 0, 1, 30.f, c.timing, 4.f);
    const int loop = ChaseLoopFrames(c, *st, 30.f);

    // Something is lit at every frame of the cycle, including the seam.
    for (int f = 0; f < loop; ++f) {
        float lit = 0.f;
        for (size_t si = 0; si < c.stages.size(); ++si) {
            lit += ChaseStageEnvelopeAt(c, si, (float)f, loop);
        }
        CHECK(lit > 0.f);
    }
    // And it is genuinely periodic — frame 0 and frame `loop` match.
    for (size_t si = 0; si < c.stages.size(); ++si) {
        CHECK_NEAR(ChaseStageEnvelopeAt(c, si, 0.f, loop),
                   ChaseStageEnvelopeAt(c, si, (float)loop, loop), 1e-4);
    }
}

TEST(a_one_shot_sweep_still_ends_in_black)
{
    auto st = MakeState(9);
    Chase c;
    ApplyTemplateToChase(c, 0);             // Left to Right
    RegenerateChaseStages(c, *st, 0);
    c.timing = FitSweepTiming((int)c.stages.size(), 0, 1, 30.f, c.timing, 4.f);
    CHECK(!ChaseWraps(c, *st));
    // Past the last release there is nothing lit.
    const float total = (c.stages.size() - 1) * c.timing.step_duration
                        + c.timing.duration;
    float lit = 0.f;
    for (size_t si = 0; si < c.stages.size(); ++si) {
        lit += ChaseStageEnvelopeAt(c, si, total + 1.f, 0);
    }
    CHECK_NEAR(lit, 0.0, 1e-6);
}
