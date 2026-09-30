// The arithmetic behind the AE build, with no AE SDK in sight.
//
// These two calculations decide what the built comp actually looks
// like, and both have caused production bugs:
//
//  * frame -> A_Time conversion silently lost subframe precision, which
//    collapsed every corner of a wrapped envelope onto the same integer
//    frame and destroyed the chase shape;
//  * the builder and the preview each had their own copy of the
//    envelope, and they drifted — one wrapped on an integer period, the
//    other on a fractional one, so the preview showed a phantom cycle
//    near the loop point that the comp did not have.
//
// So they live here, in plain types, with the builder and the preview
// both delegating to them and the tests able to reach them.

#pragma once

#include <cmath>

namespace build_math {

// Mirrors AE's A_Ratio / A_Time without dragging in the SDK.
struct TimeRatio { long  num = 30; unsigned long den = 1; };
struct TimeValue { long  value = 0; unsigned long scale = 1; };

// Subframe resolution multiplier. A_Time is an exact rational
// value/scale, so "frames at fps" is frames*den / num — but `value` is
// an integer, and at fps = {30,1} that rounds every fractional frame to
// a whole one. Scaling BOTH value and scale by 1000 keeps ~1ms of
// resolution at 30fps while staying far inside A_long's range for any
// realistic comp length. NTSC rates ({30000,1001}) were always fine;
// the plain integer rates were not.
inline constexpr long kSubframeScale = 1000;

inline TimeValue FramesToTime(double frames, TimeRatio fps)
{
    TimeValue t;
    if (fps.num <= 0) {
        t.value = 0;
        t.scale = 1;
        return t;
    }
    t.value = static_cast<long>(frames * fps.den * kSubframeScale + 0.5);
    t.scale = static_cast<unsigned long>(fps.num) * kSubframeScale;
    return t;
}

// A floating fps -> an exact rational, preserving the NTSC rates.
inline TimeRatio RatioFromFps(double fps)
{
    if (fps <= 0.01)                    return TimeRatio{ 24, 1 };
    if (std::fabs(fps - 23.976) < 0.01) return TimeRatio{ 24000, 1001 };
    if (std::fabs(fps - 29.97)  < 0.01) return TimeRatio{ 30000, 1001 };
    if (std::fabs(fps - 59.94)  < 0.01) return TimeRatio{ 60000, 1001 };
    return TimeRatio{ static_cast<long>(fps + 0.5), 1 };
}

// The wrapped hit envelope: rise over `attack`, hold at the peak for
// `hold`, fall for the rest of `duration`, zero outside. `t` and
// `phase` are frames; the shape repeats every `period` frames.
//
// Seamlessness comes from this periodicity alone — frame period-1 and
// frame 0 are ordinary neighbours in continuous time. Do NOT "help" by
// pinning the last frame to the first: that plays two identical frames
// back to back, which is a visible freeze every loop.
//
// `period` must be the SAME fractional value the caller uses elsewhere
// (loop_frames / cycles, not an integer division of it).
inline double WrappedEnvelope(double phase, double t, double period,
                              double attack, double duration, double hold)
{
    if (period <= 0.0) return 0.0;
    const double held  = (hold > 0.0) ? hold : 0.0;
    const double total = duration + held;
    if (total <= 0.0) return 0.0;

    double local = std::fmod(t - phase, period);
    if (local < 0.0) local += period;
    if (local >= total) return 0.0;

    double att = attack;
    if (att < 0.001)              att = 0.001;
    if (att > duration - 0.001)   att = duration - 0.001;
    if (att < 0.001)              att = 0.001;

    if (local < att)         return local / att;          // rising
    if (local < att + held)  return 1.0;                  // holding
    double fall_len = duration - att;
    if (fall_len < 0.001) fall_len = 0.001;
    return (total - local) / fall_len;                    // falling
}

} // namespace build_math
