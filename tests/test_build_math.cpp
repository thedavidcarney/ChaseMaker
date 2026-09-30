// The two calculations that decide what a built comp actually looks
// like. Both have bitten in production, and neither was covered.

#include "test_harness.h"

#include "build_math.h"
#include "chase_gen.h"
#include "panel_state.h"

using build_math::FramesToTime;
using build_math::RatioFromFps;
using build_math::TimeRatio;
using build_math::TimeValue;
using build_math::WrappedEnvelope;

namespace {

double Seconds(const TimeValue& t)
{
    return (t.scale > 0) ? (double)t.value / (double)t.scale : 0.0;
}

} // namespace

// ---- frames -> A_Time ------------------------------------------------

TEST(frames_to_time_is_exact_on_whole_frames)
{
    const TimeRatio fps{ 30, 1 };
    CHECK_NEAR(Seconds(FramesToTime(0.0,  fps)), 0.0, 1e-9);
    CHECK_NEAR(Seconds(FramesToTime(30.0, fps)), 1.0, 1e-9);
    CHECK_NEAR(Seconds(FramesToTime(45.0, fps)), 1.5, 1e-9);
}

// The subframe-precision regression: at fps {30,1} the naive
// conversion rounded `value` to whole frames, so 59.53 and 60.0 landed
// on the same A_Time. Every corner of a wrapped envelope then
// collapsed onto one frame and the chase shape was destroyed.
TEST(frames_to_time_keeps_subframe_positions_apart)
{
    const TimeRatio fps{ 30, 1 };
    const TimeValue a = FramesToTime(59.53, fps);
    const TimeValue b = FramesToTime(60.0,  fps);
    CHECK(a.value != b.value);
    CHECK(Seconds(a) < Seconds(b));
    CHECK_NEAR(Seconds(a), 59.53 / 30.0, 1e-4);
}

TEST(frames_to_time_resolves_the_corners_of_one_hit)
{
    // A fractional stage step puts every corner off the frame grid;
    // all four must stay distinct or the envelope flattens.
    const TimeRatio fps{ 30, 1 };
    const double corners[] = { 22.375, 24.375, 28.375, 32.375 };
    long seen[4];
    for (int i = 0; i < 4; ++i) seen[i] = FramesToTime(corners[i], fps).value;
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) CHECK(seen[i] != seen[j]);
    }
}

TEST(frames_to_time_handles_ntsc_rates)
{
    const TimeRatio ntsc{ 30000, 1001 };
    CHECK_NEAR(Seconds(FramesToTime(30.0, ntsc)), 30.0 * 1001.0 / 30000.0, 1e-9);
    const TimeValue a = FramesToTime(59.53, ntsc);
    const TimeValue b = FramesToTime(60.0,  ntsc);
    CHECK(a.value != b.value);
}

TEST(frames_to_time_survives_a_nonsense_rate)
{
    const TimeValue t = FramesToTime(10.0, TimeRatio{ 0, 1 });
    CHECK_EQ((int)t.value, 0);
    CHECK(t.scale > 0);                       // never divides by zero
}

// ---- fps -> exact ratio ---------------------------------------------

TEST(ratio_from_fps_preserves_the_ntsc_rates)
{
    CHECK_EQ((int)RatioFromFps(23.976).num, 24000);
    CHECK_EQ((int)RatioFromFps(23.976).den, 1001);
    CHECK_EQ((int)RatioFromFps(29.97).num,  30000);
    CHECK_EQ((int)RatioFromFps(29.97).den,  1001);
    CHECK_EQ((int)RatioFromFps(59.94).num,  60000);
    CHECK_EQ((int)RatioFromFps(59.94).den,  1001);
}

TEST(ratio_from_fps_rounds_ordinary_rates_to_whole_numbers)
{
    CHECK_EQ((int)RatioFromFps(30.0).num, 30);
    CHECK_EQ((int)RatioFromFps(30.0).den, 1);
    CHECK_EQ((int)RatioFromFps(24.0).num, 24);
    CHECK_EQ((int)RatioFromFps(25.0).num, 25);
    CHECK_EQ((int)RatioFromFps(60.0).num, 60);
}

TEST(ratio_from_fps_falls_back_rather_than_returning_zero)
{
    CHECK(RatioFromFps(0.0).num > 0);
    CHECK(RatioFromFps(-5.0).num > 0);
}

// ---- The envelope both the comp and the preview use -----------------

TEST(wrapped_envelope_rises_holds_and_falls)
{
    // attack 2, duration 10, hold 4 -> total 14.
    CHECK_NEAR(WrappedEnvelope(0.0, 0.0,  100.0, 2.0, 10.0, 4.0), 0.0, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 1.0,  100.0, 2.0, 10.0, 4.0), 0.5, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 2.0,  100.0, 2.0, 10.0, 4.0), 1.0, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 5.0,  100.0, 2.0, 10.0, 4.0), 1.0, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 6.0,  100.0, 2.0, 10.0, 4.0), 1.0, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 10.0, 100.0, 2.0, 10.0, 4.0), 0.5, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 14.0, 100.0, 2.0, 10.0, 4.0), 0.0, 1e-9);
}

// Seamlessness is periodicity and nothing else — no pinned last frame.
TEST(wrapped_envelope_is_exactly_periodic)
{
    const double period = 22.375;             // deliberately fractional
    for (double t = 0.0; t < period; t += 0.7) {
        const double a = WrappedEnvelope(3.5, t, period, 2.0, 8.0, 1.0);
        const double b = WrappedEnvelope(3.5, t + period, period, 2.0, 8.0, 1.0);
        const double c = WrappedEnvelope(3.5, t + 4 * period, period, 2.0, 8.0, 1.0);
        CHECK_NEAR(a, b, 1e-12);
        CHECK_NEAR(a, c, 1e-12);
    }
}

TEST(wrapped_envelope_has_no_duplicate_frame_at_the_loop_point)
{
    // The removed "fudge keyframe" made the last frame of the loop
    // equal the first, which played as a two-frame freeze every time
    // round. A hit straddling the boundary must keep moving through it
    // exactly like any other pair of neighbouring frames.
    const double period = 60.0;
    const double phase  = 50.0;               // still lit at the wrap
    const double a = WrappedEnvelope(phase, 59.0, period, 4.0, 40.0, 0.0);
    const double b = WrappedEnvelope(phase, 60.0, period, 4.0, 40.0, 0.0);
    const double c = WrappedEnvelope(phase, 61.0, period, 4.0, 40.0, 0.0);
    CHECK(a > 0.0 && b > 0.0 && c > 0.0);     // genuinely lit across it
    CHECK(a != b);
    CHECK(b != c);
    // And the steps either side of the boundary are the same size — no
    // stall, no jump.
    CHECK_NEAR(a - b, b - c, 1e-9);
}

TEST(wrapped_envelope_refuses_degenerate_inputs)
{
    CHECK_NEAR(WrappedEnvelope(0.0, 5.0, 0.0,  2.0, 10.0, 0.0), 0.0, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 5.0, -1.0, 2.0, 10.0, 0.0), 0.0, 1e-9);
    CHECK_NEAR(WrappedEnvelope(0.0, 5.0, 50.0, 2.0,  0.0, 0.0), 0.0, 1e-9);
    // An attack longer than the hit must not produce a negative or
    // runaway value.
    const double v = WrappedEnvelope(0.0, 3.0, 50.0, 99.0, 10.0, 0.0);
    CHECK(v >= 0.0);
    CHECK(v <= 1.0);
}

// The drift that actually shipped: the builder and the preview each had
// their own copy of this. They share one now, and this pins it.
TEST(the_preview_envelope_matches_the_builders)
{
    ChaseTiming t;
    t.duration = 17.5f;
    t.attack   = 3.25f;
    t.hold     = 2.5f;

    const double period = 22.375;
    for (double p = 0.0; p < 45.0; p += 0.25) {
        const float preview = chase_gen::ScatterHitEnvelope(
            (float)p, 5.5f, (float)period, t);
        const double builder = WrappedEnvelope(5.5, p, period,
                                               t.attack, t.duration, t.hold);
        CHECK_NEAR(preview, builder, 1e-4);
    }
}
