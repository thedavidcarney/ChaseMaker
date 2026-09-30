#include "chase_gen.h"

#include "build_math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace chase_gen {

// ===== Chase preview compositing ======================================

// Defined later; the wrapped (seamless-loop) triangle envelope.
float ScatterHitEnvelope(float playhead, float start, float loop_frames,
                         const ChaseTiming& t);

// Envelope value (0..1) for a stage at the given playhead frame.
// Shape: linear ramp up over `attack` frames, hold at peak for
// `hold` frames, linear ramp down for `duration - attack` frames.
// Total hit length = duration + hold. Returns 0 outside the hit.
float StageEnvelope(int stage_idx, float playhead, const ChaseTiming& t)
{
    const float hold = std::max(0.f, t.hold);
    const float total = t.duration + hold;
    const float stage_start = stage_idx * t.step_duration;
    const float local_t = playhead - stage_start;
    if (local_t < 0.f || local_t >= total) return 0.f;
    const float att = std::max(0.001f, std::min(t.attack, t.duration - 0.001f));
    if (local_t < att) return local_t / att;                 // rising
    if (local_t < att + hold) return 1.f;                    // holding
    const float fall_len = std::max(0.001f, t.duration - att);
    return (total - local_t) / fall_len;                     // falling
}

// Total chase duration in frames: when the last stage's release ends.
float ChaseTotalDuration(const Chase& chase)
{
    if (chase.stages.empty()) return 0.f;
    return (chase.stages.size() - 1) * chase.timing.step_duration + chase.timing.duration;
}

// Per-stage envelope honoring loop-mode. In loop-mode (animation
// source) stages are evenly spaced across the seamless loop and the
// envelope wraps â€” no from-black ramp at t=0. `loop_frames <= 0`
// falls back to the legacy shift-in-time triangle (still images).
float ChaseStageEnvelopeAt(const Chase& chase, size_t si, float playhead,
                           int loop_frames)
{
    const size_t n = chase.stages.size();
    if (loop_frames > 0 && n > 0) {
        // `loop_cycles` repeats the chase pattern INSIDE the comp
        // loop (the inverse of loop_multiple, which makes the comp
        // longer). Cycles=2 = chase sweeps twice per loop. We
        // implement it by wrapping the envelope with a shorter
        // period = loop_frames / cycles â€” the triangle then tiles
        // `cycles` times automatically across [0, loop_frames).
        const int cy = chase.loop_cycles < 1 ? 1 : chase.loop_cycles;
        // FRACTIONAL period â€” must match the builder's
        // `loopL / cycles` (double) exactly. Integer truncation here
        // (e.g. 179/8 = 22 instead of 22.375) would make the envelope
        // wrap on a SHORTER period than the actual loop, causing the
        // preview to visibly start a new cycle a few frames before
        // the loop boundary (the "chase starts from left at frame
        // 176-177 then black at 178" jitter).
        const float period = static_cast<float>(loop_frames) /
                             static_cast<float>(cy);
        // Master offset shifts every stage's start time by the same
        // amount inside the period. Wrap is handled by
        // ScatterHitEnvelope's fmod, so order + cycle behavior is
        // preserved automatically.
        float off = static_cast<float>(chase.loop_offset);
        if (period > 0.f) {
            off = std::fmod(off, period);
            if (off < 0.f) off += period;
        } else {
            off = 0.f;
        }
        // Stages evenly spaced across the FULL cycle period.
        // step = period / n = loop_frames / (n * cycles) â€” the
        // distance between consecutive hits anywhere in the loop.
        // With env_d <= step the chase has "no tail" (each hit ends
        // before the next starts); with env_d > step hits overlap.
        // Either is a valid user choice â€” we just expose evenly-
        // spaced phases and let env_d control hit shape.
        //
        // Seamless loop is GUARANTEED by env_at's periodicity (with
        // period * cycles == loop_frames exactly), so frame
        // loop_frames-1 naturally flows into frame 0 of the next
        // iteration just like any other 1-frame transition. We
        // explicitly do NOT pin frame loop_frames-1 to env(0): that
        // would make AE play two consecutive frames with the same
        // value â€” a 2-frame freeze every loop iteration. The
        // natural-period sampling IS the seamless behavior.
        const float step = period / static_cast<float>(n);
        const float phase = static_cast<float>(si) * step + off;
        return ScatterHitEnvelope(playhead, phase, period, chase.timing);
    }
    return StageEnvelope(static_cast<int>(si), playhead, chase.timing);
}

// Loop-mode-aware total preview length in frames.
float ChaseTotalFrames(const Chase& chase, const PanelState& state, float fps)
{
    if (chase.random_scatter || ChaseWraps(chase, state))
        return static_cast<float>(ChaseLoopFrames(chase, state, fps));
    return ChaseTotalDuration(chase);
}

// Lighten-composite weighted contributions, matching the AE build
// (black solid + Lighten â€” per-channel max, NOT additive). Thumbnails
// are normalized to each layer's OWN peak so weak lights stay visible
// in the layer table; that lies about relative brightness, so here we
// undo it via thumb_peak and renormalize against `shared_peak` (the
// brightest participating layer). Result: overlapping lights no
// longer stack to white, and a dim fill reads dimmer than the key â€”
// as it will in the AE comp. Still an approximation (the real
// Exposure effect is a per-channel pow, not a linear scale), but
// close and ~free.
bool LightenComposite(const std::vector<CompositeContribution>& contribs,
                      int out_w, int out_h, float shared_peak,
                      std::vector<uint8_t>& out_rgba)
{
    if (out_w <= 0 || out_h <= 0) return false;
    const size_t n = static_cast<size_t>(out_w) * out_h;
    std::vector<float> acc(n * 3, 0.f);            // linear, Lighten max
    const float sp = shared_peak > 1e-6f ? shared_peak : 1.f;

    for (const auto& c : contribs) {
        const LayerInfo* L = c.L;
        if (!L || L->thumb_rgba.empty()) continue;
        if (L->thumb_w <= 0 || L->thumb_h <= 0) continue;
        if (c.k <= 0.f) continue;
        // thumb byte = sqrt(linear / thumb_peak) * 255, so
        // linear / shared_peak = (byte/255)^2 * thumb_peak/shared_peak.
        const float lp = L->thumb_peak > 0.f ? L->thumb_peak : 1.f;
        const float scale = (lp / sp) * c.k;
        const uint8_t* s = L->thumb_rgba.data();
        // Same-size fast path; otherwise nearest-neighbour resample
        // into the output canvas. Layers from different sources may
        // disagree on thumb_w/thumb_h (different scan-time
        // thumb_max_width, or different EXR aspect ratios). Dropping
        // those silently was hiding one half of e.g. Center Out
        // symmetric pairs whose partners lived in another source.
        // Nearest-neighbor is good enough â€” these are preview
        // thumbnails (~256 px), and centroids are normalized so the
        // sources cover the same image region in [0,1].
        if (L->thumb_w == out_w && L->thumb_h == out_h) {
            for (size_t i = 0; i < n; ++i) {
                for (int ch = 0; ch < 3; ++ch) {
                    const float u = s[i * 4 + ch] * (1.f / 255.f);
                    const float lin = u * u * scale;
                    float& a = acc[i * 3 + ch];
                    if (lin > a) a = lin;          // Lighten
                }
            }
        } else {
            const int sw = L->thumb_w;
            const int sh = L->thumb_h;
            for (int y = 0; y < out_h; ++y) {
                const int sy = (y * sh) / out_h;
                for (int x = 0; x < out_w; ++x) {
                    const int sx = (x * sw) / out_w;
                    const uint8_t* p = s + (sy * sw + sx) * 4;
                    const size_t di = static_cast<size_t>(y) * out_w + x;
                    for (int ch = 0; ch < 3; ++ch) {
                        const float u = p[ch] * (1.f / 255.f);
                        const float lin = u * u * scale;
                        float& a = acc[di * 3 + ch];
                        if (lin > a) a = lin;      // Lighten
                    }
                }
            }
        }
    }

    out_rgba.assign(n * 4, 0);
    for (size_t i = 0; i < n; ++i) {
        for (int ch = 0; ch < 3; ++ch) {
            float v = acc[i * 3 + ch];
            if (v < 0.f) v = 0.f; else if (v > 1.f) v = 1.f;
            int q = static_cast<int>(std::sqrt(v) * 255.f + 0.5f);
            out_rgba[i * 4 + ch] =
                static_cast<uint8_t>(q < 0 ? 0 : (q > 255 ? 255 : q));
        }
        out_rgba[i * 4 + 3] = 255;
    }
    return true;
}

// Build the composite RGBA8 buffer for the chase at the current
// playhead. Sized to match the first usable thumbnail; ignores
// thumbs whose dimensions don't match (mixed-source case; not yet
// implemented). Returns true on success.
bool BuildChaseComposite(const Chase& chase, const PanelState& state,
                         float playhead,
                         std::vector<uint8_t>& out_rgba,
                         int& out_w, int& out_h)
{
    out_w = 0; out_h = 0;
    // Shared peak over ALL referenced layers (not just active ones)
    // so brightness doesn't flicker as stages turn on/off.
    float shared_peak = 0.f;
    for (const auto& stage : chase.stages) {
        for (const auto& ref : stage.members) {
            const LayerInfo* L = FindLayerByRef(state, ref);
            if (!L) continue;
            if (out_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
                out_w = L->thumb_w; out_h = L->thumb_h;
            }
            if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
        }
    }
    if (out_w == 0 || out_h == 0) return false;

    const int loop_frames = ChaseWraps(chase, state)
        ? ChaseLoopFrames(chase, state, 0.f) : 0;
    std::vector<CompositeContribution> contribs;
    for (size_t si = 0; si < chase.stages.size(); ++si) {
        float env = ChaseStageEnvelopeAt(chase, si, playhead, loop_frames);
        float op_pk = chase.timing.opacity_peak;
        float op_fl = chase.timing.opacity_floor;
        if (op_fl < 0.f) op_fl = 0.f;
        if (op_fl > op_pk) op_fl = op_pk;
        // With a floor, a light at env=0 still sits at the floor, so we
        // can't skip inactive stages. Keep the fast path when floor=0.
        if (env <= 0.f && op_fl <= 0.f) continue;
        const float opacity = (op_fl + (op_pk - op_fl) * env) / 100.f;
        const float gamma_t = chase.timing.gamma_baseline +
            (chase.timing.gamma_peak - chase.timing.gamma_baseline) * env;
        const float k = opacity * gamma_t;
        if (k <= 0.f) continue;
        for (const auto& ref : chase.stages[si].members) {
            contribs.push_back({ FindLayerByRef(state, ref), k });
        }
    }
    return LightenComposite(contribs, out_w, out_h, shared_peak, out_rgba);
}

// Composite an explicit set of layer refs at full strength (no
// envelope). Used by the chase preview's click-to-pin: clicking a
// layer freezes the preview on just that light (or, if it sits in a
// multi-light stage, that whole stage). Same accumulation as
// BuildChaseComposite with k = 1.
bool BuildRefsComposite(const std::vector<LayerRef>& refs,
                        const PanelState& state,
                        std::vector<uint8_t>& out_rgba,
                        int& out_w, int& out_h)
{
    out_w = 0; out_h = 0;
    float shared_peak = 0.f;
    std::vector<CompositeContribution> contribs;
    for (const auto& ref : refs) {
        const LayerInfo* L = FindLayerByRef(state, ref);
        if (!L) continue;
        if (out_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
            out_w = L->thumb_w; out_h = L->thumb_h;
        }
        if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
        contribs.push_back({ L, 1.f });
    }
    if (out_w == 0 || out_h == 0) return false;
    return LightenComposite(contribs, out_w, out_h, shared_peak, out_rgba);
}

// Recompute the chase's stages from its sort + tag filter + the
// session's staged layers. Called once each frame from the chase
// editor so stages always reflect current state.
void RegenerateChaseStages(Chase& chase, const PanelState& state,
                           int stages_count_hint)
{
    // Random-scatter chases don't use the stage sequence at all â€”
    // they own `scatter` instead (see RegenerateScatter).
    if (chase.random_scatter) return;
    // Manual arrangement: the user owns `stages` (drag-reorder, weld,
    // split in the editor). Don't recompute â€” leave it as authored so
    // it survives the per-frame refresh and session round-trips.
    if (chase.manual_stages) return;
    chase.stages.clear();
    if (state.sources.empty()) return;
    // Gather candidates from EVERY source (multi-source chases pull
    // from the whole session). Tag filter is session-wide and
    // already keyed by (source_id, hash), so it works unchanged.
    const auto dedupe_keep = BuildDedupeKeepSet(state);
    const bool dedupe_on   = state.dedupe_by_name;
    std::vector<LayerRef> candidates;
    for (const Source& src : state.sources) {
        for (const auto& L : src.layers) {
            if (!L.included) continue;
            if (!DedupeKeeps(dedupe_keep, dedupe_on,
                             src.source_id, L.fnv1a_hash)) continue;
            LayerRef ref{ src.source_id, L.fnv1a_hash };
            if (!chase.tag_filter.empty()) {
                bool in_any = false;
                for (uint32_t tid : chase.tag_filter) {
                    if (const Tag* t = FindTagById(state, tid)) {
                        for (const auto& m : t->members) {
                            if (m == ref) { in_any = true; break; }
                        }
                        if (in_any) break;
                    }
                }
                if (!in_any) continue;
            }
            candidates.push_back(ref);
        }
    }
    // Coalesce bind members into one "logical light" FIRST, then sort.
    // A bind is two render passes of the same physical light, so it has
    // to be ordered by where the combined light actually sits â€” its
    // luminance-weighted centre â€” not by whichever member happened to
    // sort first. The bind expands into the stage as N LayerRefs so the
    // comp fires all the bound lights together on one envelope.
    struct Lead {
        LayerRef              repr;
        std::vector<LayerRef> members;
        // Combined metrics: luminance-weighted means, summed total.
        // For a lone light these are just its own values.
        float  cx = 0.f, cy = 0.f, cx_hot = 0.f, cy_hot = 0.f;
        float  peak_x = 0.f, peak_y = 0.f;
        double total = 0.0;
        const LayerInfo* info = nullptr;      // repr, for name/flags
    };
    std::vector<Lead> leads;
    std::unordered_set<uint32_t> bind_id_seen;
    leads.reserve(candidates.size());
    for (const auto& ref : candidates) {
        const Bind* b = BindOfLayer(state, ref);
        if (b) {
            if (!bind_id_seen.insert(b->bind_id).second) continue;  // dup
            Lead L;
            L.repr = ref;
            L.members = b->members;
            leads.push_back(std::move(L));
        } else {
            Lead L;
            L.repr = ref;
            L.members.push_back(ref);
            leads.push_back(std::move(L));
        }
    }

    for (Lead& L : leads) {
        L.info = FindLayerByRef(state, L.repr);
        double wsum = 0.0;
        double wx = 0.0, wy = 0.0, wxh = 0.0, wyh = 0.0;
        double wpx = 0.0, wpy = 0.0;
        for (const auto& m : L.members) {
            const LayerInfo* mi = FindLayerByRef(state, m);
            if (!mi) continue;
            // Weight by luminance so a bright pass dominates a faint
            // one; fall back to equal weights if nothing is scanned yet.
            const double w = (mi->total > 0.0) ? mi->total : 1.0;
            wsum += w;
            wx   += w * mi->cx;
            wy   += w * mi->cy;
            wxh  += w * mi->cx_hot;
            wyh  += w * mi->cy_hot;
            wpx  += w * mi->peak_x;
            wpy  += w * mi->peak_y;
            L.total += mi->total;
        }
        if (wsum > 0.0) {
            L.cx     = static_cast<float>(wx  / wsum);
            L.cy     = static_cast<float>(wy  / wsum);
            L.cx_hot = static_cast<float>(wxh / wsum);
            L.cy_hot = static_cast<float>(wyh / wsum);
            L.peak_x = static_cast<float>(wpx / wsum);
            L.peak_y = static_cast<float>(wpy / wsum);
        } else if (L.info) {
            L.cx = L.info->cx;         L.cy = L.info->cy;
            L.cx_hot = L.info->cx_hot; L.cy_hot = L.info->cy_hot;
            L.peak_x = L.info->peak_x; L.peak_y = L.info->peak_y;
        }
    }

    // Sort the logical lights per chase.sort_mode.
    auto less_by_mode = [&](const Lead& a, const Lead& b) {
        switch (chase.sort_mode) {
        case SortMode::CentroidX:        return a.cx     < b.cx;
        case SortMode::CentroidY:        return a.cy     < b.cy;
        case SortMode::HotspotX:         return a.cx_hot < b.cx_hot;
        case SortMode::HotspotY:         return a.cy_hot < b.cy_hot;
        case SortMode::PeakX:            return a.peak_x < b.peak_x;
        case SortMode::PeakY:            return a.peak_y < b.peak_y;
        case SortMode::Brightness:       return a.total  > b.total;
        case SortMode::RadialSweep: {
            const float angA = std::atan2(a.cy - 0.5f, a.cx - 0.5f);
            const float angB = std::atan2(b.cy - 0.5f, b.cx - 0.5f);
            return angA < angB;
        }
        case SortMode::DistanceFromCenter: {
            const float da = (a.cx - 0.5f) * (a.cx - 0.5f) +
                             (a.cy - 0.5f) * (a.cy - 0.5f);
            const float db = (b.cx - 0.5f) * (b.cx - 0.5f) +
                             (b.cy - 0.5f) * (b.cy - 0.5f);
            return da < db;
        }
        case SortMode::Random: {
            const uint32_t ka = a.repr.fnv1a_hash * 2654435761u +
                                chase.random_seed;
            const uint32_t kb = b.repr.fnv1a_hash * 2654435761u +
                                chase.random_seed;
            return ka < kb;
        }
        case SortMode::Alphabetical:
            if (!a.info || !b.info) return false;
            return a.info->display_name < b.info->display_name;
        case SortMode::IncludedFirst:
            if (!a.info || !b.info) return false;
            if (a.info->included != b.info->included)
                return a.info->included > b.info->included;
            return a.info->display_name < b.info->display_name;
        case SortMode::TagName:
        case SortMode::EffectiveX:
        case SortMode::EffectiveY:
            // Chase regen uses the layer's intrinsic centroid for
            // these â€” they're staging-tab-only sort modes in practice.
            if (!a.info || !b.info) return false;
            return a.info->display_name < b.info->display_name;
        }
        return false;
    };
    std::stable_sort(leads.begin(), leads.end(), less_by_mode);
    if (chase.sort_reverse) std::reverse(leads.begin(), leads.end());

    // Symmetric center-out: order is already set by sort_mode (the
    // "Center Out" template uses CentroidX = left->right). Build
    // stages from the middle outward â€” the center light(s) fire
    // alone, then each mirrored pair welds into one stage. For an
    // even count the two innermost lights share the first stage.
    // Lights 1..5 (L->R) => [3], [2,4], [1,5]. Ignores
    // desired_stage_count by design.
    if (chase.symmetric_pairs) {
        const int n = static_cast<int>(leads.size());
        const int lo = (n - 1) / 2;
        const int hi = n / 2;
        for (int k = 0; ; ++k) {
            const int i = lo - k;
            const int j = hi + k;
            if (i < 0 || j >= n) break;
            ChaseStage st;
            for (const auto& m : leads[i].members) st.members.push_back(m);
            if (j != i) {
                for (const auto& m : leads[j].members) st.members.push_back(m);
            }
            chase.stages.push_back(std::move(st));
        }
        return;
    }

    // CHUNKS model: split the leads into exactly `D` contiguous
    // groups, sizes as equal as possible (differ by at most one
    // light; the first `rem` groups get the extra). No remainder /
    // straggler, fully linear: D=3 over 31 lights -> [11][10][10].
    // stages_count_hint is the chunk COUNT; <=0 or >=n means one
    // light per stage (the default; auto-tracks N as lights change).
    const int n = static_cast<int>(leads.size());
    if (n <= 0) return;
    int D = stages_count_hint;
    if (D <= 0 || D >= n) {
        for (const auto& L : leads) {
            ChaseStage s;
            for (const auto& m : L.members) s.members.push_back(m);
            chase.stages.push_back(std::move(s));
        }
        return;
    }
    // TAG-BALANCED CHUNKS (3 Step). Contiguous blocks of the sort order
    // put whole fixture families in one chunk: with StringLights_A, _B
    // and _C the three chunks become "the A's", "the B's", "the C's",
    // which reads as three separate scenes rather than one scene
    // pulsing in three. Instead divide EACH auto-tag by D, so every
    // tag appears in every chunk. Within a tag the split stays
    // contiguous in the sort order, so a chunk is still a coherent
    // sweep position â€” just one taken across all the families at once.
    //
    // Uneven divisions are the interesting case: a tag of 5 over 3
    // chunks is 2/2/1, and WHICH chunk gets the short share decides
    // whether the chunks are evenly lit. Give the extras to whichever
    // chunk is dimmest so far, measured in summed luminance, and carry
    // that running total across tags â€” so a tag that had to shortchange
    // chunk 2 is compensated by the next tag that has one to spare.
    if (chase.tag_balanced_chunks) {
        // Group by auto-tag prefix, taken from the light's own name
        // rather than the session tag list: the same rule autotagging
        // uses, and it holds for lights whose tag was renamed or whose
        // chase is filtered to a subset. A light with no usable prefix
        // is its own group of one, which is what a singleton tag is.
        struct TagGroup {
            std::string       key;
            std::vector<int>  idx;      // lead indices, in sort order
            double            weight = 0.0;
        };
        std::vector<TagGroup> groups;
        for (int i = 0; i < n; ++i) {
            std::string key = leads[i].info
                ? ExtractTagPrefix(leads[i].info->display_name)
                : std::string();
            if (key.empty()) {
                // Unique-named light: its own group, keyed with a
                // character no tag prefix can contain so it can never
                // merge with a real one.
                key = std::string(1, '\x01') + std::to_string(i);
            }
            bool placed = false;
            for (auto& g : groups) {
                if (g.key == key) { g.idx.push_back(i); placed = true; break; }
            }
            if (!placed) groups.push_back(TagGroup{ key, { i }, 0.0 });
        }

        // Weigh lights by luminance when the scene has any. A session of
        // movie/still sources reloaded without AE has every total at
        // zero; there, weigh by count instead, which is the only
        // balance available.
        bool any_lum = false;
        for (const auto& L : leads) if (L.total > 0.0) { any_lum = true; break; }
        auto weight_of = [&](int li) -> double {
            return any_lum ? leads[li].total : 1.0;
        };
        auto x_of = [&](int li) -> double {
            return static_cast<double>(leads[li].cx);
        };

        double gx = 0.0, gw = 0.0;
        for (int i = 0; i < n; ++i) { gx += x_of(i); gw += weight_of(i); }
        gx = (n > 0) ? gx / n : 0.5;
        const double ideal = (D > 0) ? gw / D : gw;

        for (auto& g : groups) {
            for (int li : g.idx) g.weight += weight_of(li);
        }
        // Brightest family first. Greedy balancing is much better when
        // the big lumps are placed while there is still room to
        // compensate for them; the key breaks ties so the order â€” and
        // therefore the whole result â€” is reproducible.
        std::stable_sort(groups.begin(), groups.end(),
                         [](const TagGroup& a, const TagGroup& b) {
                             if (a.weight != b.weight) return a.weight > b.weight;
                             return a.key < b.key;
                         });

        // Deterministic per-(tag, rotation) jitter, so families whose
        // rotations score equally don't all take the first one. This is
        // the "randomly choose the start" part; `random_seed` moves it,
        // which is what Reseed on a 3 Step does.
        auto jitter = [&](const std::string& key, int r) -> double {
            uint32_t h = 2166136261u ^ chase.random_seed;
            for (char ch : key) {
                h ^= static_cast<uint8_t>(ch);
                h *= 16777619u;
            }
            h ^= static_cast<uint32_t>(r + 1);
            h *= 16777619u;
            return static_cast<double>(h) / 4294967296.0;
        };

        std::vector<std::vector<int>> bucket(D);
        std::vector<double> lit(D, 0.0);      // running weight per chunk
        std::vector<double> sumx(D, 0.0);
        std::vector<int>    cnt(D, 0);

        for (const auto& g : groups) {
            const int gn = (int)g.idx.size();
            // The family's own running order, divided as evenly as it
            // divides: slice i is its i'th third. Extras go to the
            // earlier slices â€” within a family that choice is
            // arbitrary, because the rotation below is what decides
            // where those slices actually land.
            const int gbase = gn / D;
            const int grem  = gn % D;
            std::vector<std::vector<int>> slice(D);
            int at = 0;
            for (int i = 0; i < D; ++i) {
                const int sz = gbase + (i < grem ? 1 : 0);
                for (int k = 0; k < sz && at < gn; ++k, ++at)
                    slice[i].push_back(g.idx[at]);
            }

            // ROTATION: slice i fires in chunk (i + r) % D. Every family
            // still appears in every chunk and still runs in its own
            // order â€” but it starts on a different beat, so the scene
            // stops reading as one left-to-right wipe cut into three
            // and becomes each section running its own 3 Step. For a
            // one-light family the rotation IS the chunk choice, which
            // is how a lone light finds the right place to sit.
            //
            // Which rotation: the one leaving the chunks most evenly
            // lit AND most evenly spread across the frame, so no chunk
            // is "the bright one" or "the left one".
            int best_r = 0;
            double best_score = 0.0;
            for (int r = 0; r < D; ++r) {
                std::vector<double> tl(lit), tx(sumx);
                std::vector<int>    tc(cnt);
                for (int i = 0; i < D; ++i) {
                    const int c2 = (i + r) % D;
                    for (int li : slice[i]) {
                        tl[c2] += weight_of(li);
                        tx[c2] += x_of(li);
                        tc[c2] += 1;
                    }
                }
                // Brightness: how far the worst chunk is from an even
                // share, normalized so the term means the same thing in
                // a 6-light scene and a 140-light one.
                double lum_term = 0.0;
                for (int c2 = 0; c2 < D; ++c2) {
                    const double d = std::fabs(tl[c2] - ideal);
                    if (d > lum_term) lum_term = d;
                }
                lum_term /= (ideal > 1e-9 ? ideal : 1.0);
                // Position: how far the worst chunk's centre of light
                // sits from the scene's. A chunk made only of the left
                // side scores badly here even when its brightness is
                // right â€” that is the "reads as left to right" failure.
                double pos_term = 0.0;
                for (int c2 = 0; c2 < D; ++c2) {
                    if (tc[c2] == 0) { pos_term = 1.0; continue; }
                    const double d = std::fabs(tx[c2] / tc[c2] - gx) * 2.0;
                    if (d > pos_term) pos_term = d;
                }
                const double score = lum_term + 0.6 * pos_term +
                                     1e-4 * jitter(g.key, r);
                if (r == 0 || score < best_score) {
                    best_score = score;
                    best_r = r;
                }
            }

            for (int i = 0; i < D; ++i) {
                const int c2 = (i + best_r) % D;
                for (int li : slice[i]) {
                    bucket[c2].push_back(li);
                    lit[c2]  += weight_of(li);
                    sumx[c2] += x_of(li);
                    cnt[c2]  += 1;
                }
            }
        }

        // Within a chunk, restore the chase's sort order. Families were
        // visited brightest-first, which is a placement order, not a
        // firing order.
        for (int c2 = 0; c2 < D; ++c2) {
            std::sort(bucket[c2].begin(), bucket[c2].end());
            ChaseStage cur;
            for (int li : bucket[c2])
                for (const auto& m : leads[li].members)
                    cur.members.push_back(m);
            if (!cur.members.empty()) chase.stages.push_back(std::move(cur));
        }
        return;
    }

    const int base = n / D;
    const int rem  = n % D;          // first `rem` chunks get base+1
    int idx = 0;
    for (int g = 0; g < D; ++g) {
        const int sz = base + (g < rem ? 1 : 0);
        ChaseStage cur;
        for (int k = 0; k < sz && idx < n; ++k, ++idx)
            for (const auto& m : leads[idx].members)
                cur.members.push_back(m);
        if (!cur.members.empty()) chase.stages.push_back(std::move(cur));
    }
}

// ===== Contact sheet ==================================================

namespace {

// Everything a preview needs for one chase at one moment: which layers
// are lit and how brightly, plus the natural pixel size to draw at.
// Shared by the single-chase previews and the contact sheet so all
// three agree on what a chase looks like.
bool GatherContributions(const Chase& chase, const PanelState& state,
                         float playhead, float fps,
                         std::vector<CompositeContribution>& contribs,
                         float& shared_peak, int& nat_w, int& nat_h)
{
    contribs.clear();
    shared_peak = 0.f;
    nat_w = 0; nat_h = 0;

    auto note = [&](const LayerInfo* L) {
        if (!L) return;
        if (nat_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
            nat_w = L->thumb_w;
            nat_h = L->thumb_h;
        }
        // Shared peak over ALL referenced layers (not just the lit
        // ones) so brightness doesn't flicker as lights turn on and off.
        if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
    };

    // Below this a light cannot move a Lighten max by even one 8-bit
    // step, so compositing it is pure cost.
    const float kNegligible = 1.f / 255.f;

    // Envelope value -> the linear multiplier the AE build will apply.
    auto weight = [&](float env) {
        float op_pk = chase.timing.opacity_peak;
        float op_fl = chase.timing.opacity_floor;
        if (op_fl < 0.f) op_fl = 0.f;
        if (op_fl > op_pk) op_fl = op_pk;
        // With a floor, a light at env=0 still sits at the floor, so an
        // inactive light can't simply be skipped.
        if (env <= 0.f && op_fl <= 0.f) return 0.f;
        const float opacity = (op_fl + (op_pk - op_fl) * env) / 100.f;
        const float gamma_t = chase.timing.gamma_baseline +
            (chase.timing.gamma_peak - chase.timing.gamma_baseline) * env;
        return opacity * gamma_t;
    };

    if (chase.random_scatter) {
        const int loop_frames = ChaseLoopFrames(chase, state, fps);
        for (const auto& hit : chase.scatter) note(FindLayerByRef(state, hit.ref));
        if (nat_w == 0 || nat_h == 0) return false;
        for (const auto& hit : chase.scatter) {
            const float env = ScatterHitEnvelope(playhead, hit.start_frame,
                                                 static_cast<float>(loop_frames),
                                                 chase.timing);
            const float k = weight(env);
            if (k <= kNegligible) continue;
            contribs.push_back({ FindLayerByRef(state, hit.ref), k });
        }
        return true;
    }

    const int loop_frames = ChaseWraps(chase, state)
        ? ChaseLoopFrames(chase, state, fps) : 0;
    for (const auto& stage : chase.stages) {
        for (const auto& ref : stage.members) note(FindLayerByRef(state, ref));
    }
    if (nat_w == 0 || nat_h == 0) return false;
    for (size_t si = 0; si < chase.stages.size(); ++si) {
        const float env = ChaseStageEnvelopeAt(chase, si, playhead, loop_frames);
        const float k = weight(env);
        if (k <= kNegligible) continue;
        for (const auto& ref : chase.stages[si].members) {
            contribs.push_back({ FindLayerByRef(state, ref), k });
        }
    }
    return true;
}

} // namespace

float LongestChaseFrames(const std::vector<Chase>& chases,
                         const PanelState& state, float fps)
{
    float longest = 0.f;
    for (const auto& c : chases) {
        const float f = ChaseTotalFrames(c, state, fps);
        if (f > longest) longest = f;
    }
    return longest;
}

ContactSheetLayout BuildContactSheet(const std::vector<Chase>& chases,
                                     const PanelState& state,
                                     const std::vector<float>& playheads,
                                     float fps, int cell_w, int cols,
                                     std::vector<uint8_t>& out_rgba)
{
    ContactSheetLayout lay;
    out_rgba.clear();
    if (chases.empty() || cell_w < 1) return lay;

    // Cell aspect follows the source: find the first chase that has a
    // usable thumbnail behind it.
    int nat_w = 0, nat_h = 0;
    for (const auto& c : chases) {
        std::vector<CompositeContribution> probe;
        float peak = 0.f;
        if (GatherContributions(c, state, 0.f, fps, probe, peak, nat_w, nat_h) &&
            nat_w > 0 && nat_h > 0) {
            break;
        }
    }
    if (nat_w <= 0 || nat_h <= 0) return lay;

    lay.count  = static_cast<int>(chases.size());
    lay.cols   = (cols < 1) ? 1 : cols;
    if (lay.cols > lay.count) lay.cols = lay.count;
    lay.rows   = (lay.count + lay.cols - 1) / lay.cols;
    lay.cell_w = cell_w;
    lay.cell_h = static_cast<int>(std::lround(
        static_cast<double>(cell_w) * nat_h / nat_w));
    if (lay.cell_h < 1) lay.cell_h = 1;
    lay.sheet_w = lay.cols * lay.cell_w;
    lay.sheet_h = lay.rows * lay.cell_h;

    // One buffer for the whole grid: it uploads as a single texture and
    // each cell is drawn from its own sub-rectangle. Cells with no
    // chase behind them stay black.
    out_rgba.assign(static_cast<size_t>(lay.sheet_w) * lay.sheet_h * 4, 0);
    for (size_t i = 3; i < out_rgba.size(); i += 4) out_rgba[i] = 255;

    std::vector<CompositeContribution> contribs;
    std::vector<uint8_t> cell;
    for (int k = 0; k < lay.count; ++k) {
        const Chase& c = chases[k];
        // Each cell has its own transport: the chases in a set are
        // different lengths, so one shared position is meaningless.
        float total = ChaseTotalFrames(c, state, fps);
        if (total <= 0.f) total = 1.f;
        const float raw = (k < (int)playheads.size()) ? playheads[k] : 0.f;
        float playhead = std::fmod(raw, total);
        if (playhead < 0.f) playhead += total;

        float peak = 0.f;
        int cw = 0, chh = 0;
        if (!GatherContributions(c, state, playhead, fps, contribs, peak,
                                 cw, chh)) {
            continue;
        }
        // A cell this size can only show so much. When a saturated
        // chase lights most of the rig at once, compositing every one
        // of them costs real milliseconds and changes nothing visible,
        // so keep the strongest and drop the rest. Full-size previews
        // and the AE build are untouched by this.
        const size_t kMaxCellContribs = 16;
        if (contribs.size() > kMaxCellContribs) {
            std::partial_sort(contribs.begin(),
                              contribs.begin() + kMaxCellContribs,
                              contribs.end(),
                              [](const CompositeContribution& a,
                                 const CompositeContribution& b) {
                                  return a.k > b.k;
                              });
            contribs.resize(kMaxCellContribs);
        }
        if (!LightenComposite(contribs, lay.cell_w, lay.cell_h, peak, cell)) {
            continue;
        }

        const int x0 = (k % lay.cols) * lay.cell_w;
        const int y0 = (k / lay.cols) * lay.cell_h;
        for (int y = 0; y < lay.cell_h; ++y) {
            const size_t src_row = static_cast<size_t>(y) * lay.cell_w * 4;
            const size_t dst_row =
                (static_cast<size_t>(y0 + y) * lay.sheet_w + x0) * 4;
            std::memcpy(&out_rgba[dst_row], &cell[src_row],
                        static_cast<size_t>(lay.cell_w) * 4);
        }
    }
    return lay;
}

// ===== Random scatter =================================================

// Regenerate a scatter chase's hit list deterministically from
// (random_seed, scatter_density, loop length, eligible set). Shared by
// the UI (per-frame, for the preview) AND the AE builder, so a Random
// comp is fully populated even when its tab was never opened.
void RegenerateScatter(Chase& chase, const PanelState& state,
                              float fps)
{
    chase.scatter.clear();
    if (state.sources.empty()) return;
    const int loop_frames = ChaseLoopFrames(chase, state, fps);
    const float density = (chase.scatter_density > 0.f)
                          ? chase.scatter_density : 0.f;

    struct Lead { LayerRef repr; std::vector<LayerRef> members; };
    std::vector<Lead> leads;
    std::vector<uint32_t> bind_seen;
    // Iterate EVERY source so a chase pulls lights from the whole
    // session (e.g. one big multilayer source + a few single-layer
    // "extra" sequences). Sources are assumed to share resolution
    // and frame rate; loop length is still taken from the active
    // source (animation sources should all match per the user).
    const auto dedupe_keep = BuildDedupeKeepSet(state);
    const bool dedupe_on   = state.dedupe_by_name;
    for (const Source& src : state.sources) {
        for (const auto& L : src.layers) {
            if (!L.included) continue;
            if (!DedupeKeeps(dedupe_keep, dedupe_on,
                             src.source_id, L.fnv1a_hash)) continue;
            LayerRef ref{ src.source_id, L.fnv1a_hash };
            if (!chase.tag_filter.empty()) {
                bool in_any = false;
                for (uint32_t tid : chase.tag_filter) {
                    if (const Tag* t = FindTagById(state, tid)) {
                        for (const auto& m : t->members)
                            if (m == ref) { in_any = true; break; }
                    }
                    if (in_any) break;
                }
                if (!in_any) continue;
            }
            if (const Bind* b = BindOfLayer(state, ref)) {
                bool seen = false;
                for (uint32_t id : bind_seen)
                    if (id == b->bind_id) { seen = true; break; }
                if (!seen) {
                    bind_seen.push_back(b->bind_id);
                    leads.push_back({ ref, b->members });
                }
            } else {
                leads.push_back({ ref, { ref } });
            }
        }
    }
    if (leads.empty()) return;

    // ---- Placement -------------------------------------------------
    //
    // Every light fires `density` times, so the loop holds
    // n * density hits. Placing each light's hits independently (the
    // original scheme) gets the average right but the texture wrong:
    // bright lights land together by chance, leaving glare and holes.
    //
    // Instead, the loop is dealt in ROUNDS of one hit per light, and
    // each round gets its OWN running order, balanced so that bright
    // lights end up spaced apart. A single order replayed every round
    // (the previous scheme) read on screen as one short random clip
    // looped `density` times â€” David: "it should be feeling like
    // random lights are pulsing for the entire time." Density is only
    // how much material fills the 10 seconds, not a repeat count.
    //
    // Placement deliberately does NOT depend on chase.timing. Dragging
    // Duration or Hit Hold must not re-shuffle where every light fires
    // â€” the pattern would jump around under the slider.

    // Density is copies-per-light and may be FRACTIONAL. Above 1 every
    // light fires and some fire more than once; below 1 there are fewer
    // slots than lights, so some lights sit the loop out entirely.
    // That is the only way a big rig with a long hit gets sparse: with
    // 51 lights and a 90-frame hit over a 10-second loop, one copy of
    // everything already means ~15 lit at any instant, and the artist
    // needs to be able to ask for less than that.
    const int   n          = static_cast<int>(leads.size());
    const long  want_slots = std::lround(static_cast<double>(n) * density);
    const int   slots      = (want_slots < 1) ? 1 : static_cast<int>(want_slots);
    const float slot_width = static_cast<float>(loop_frames) /
                             static_cast<float>(slots);

    std::vector<double> bright(n, 0.0);
    for (int i = 0; i < n; ++i) {
        for (const auto& m : leads[i].members) {
            if (const LayerInfo* L = FindLayerByRef(state, m)) {
                bright[i] += L->total;
            }
        }
    }

    std::mt19937 rng(chase.random_seed ? chase.random_seed : 1u);
    std::uniform_real_distribution<float> u01(0.f, 1.f);

    // Balanced running order, by low-discrepancy placement.
    //
    // Rank the lights brightest-first, then drop them into slots along
    // the golden-ratio sequence: slot = frac(offset + k * 0.618) * n.
    // That sequence has the property we need â€” for EVERY k, the first
    // k entries are spread about as evenly over the loop as k points
    // can be. So the brightest light is far from the second brightest,
    // those two are far from the third, and so on down. Whatever the
    // brightness distribution, the heavy hitters end up apart.
    //
    // Two earlier attempts measured worse and are not worth retrying:
    // a greedy "pick the candidate that brings the trailing window
    // closest to average" (commits early, runs out of dim lights to
    // correct with), and dealing from brightness strata (a scene where
    // six lights are 12x the rest puts all six in one stratum, and
    // shuffling inside it lets them bunch in one half of the loop).
    //
    // Each round re-ranks with multiplicative noise on the brightness
    // (so near-equal lights trade places freely while a light several
    // times brighter still ranks near the top). That is what makes
    // every round a different order. The golden-ratio offset is shared
    // by all rounds on purpose: the SLOTS the brightest ranks land in
    // stay evenly spread across the whole loop, while WHICH light
    // takes them changes. A fresh offset per round measured worse â€”
    // bright lights bunched up where one round met the next.
    const double kGolden = 0.6180339887498949;
    std::normal_distribution<double> noise(0.0, 0.5);
    const double offset = u01(rng);
    auto balanced_order = [&]() {
        std::vector<double> key(n);
        for (int i = 0; i < n; ++i) key[i] = bright[i] * std::exp(noise(rng));
        std::vector<int> ranked(n);
        for (int i = 0; i < n; ++i) ranked[i] = i;
        std::shuffle(ranked.begin(), ranked.end(), rng);      // break ties
        std::stable_sort(ranked.begin(), ranked.end(),
                         [&](int a, int b) { return key[a] > key[b]; });
        std::vector<int> slot_of(n, -1);
        for (int k = 0; k < n; ++k) {
            double f = offset + kGolden * static_cast<double>(k);
            f -= std::floor(f);
            int p = static_cast<int>(f * n);
            if (p >= n) p = n - 1;
            while (slot_of[p] >= 0) p = (p + 1) % n;   // linear probe
            slot_of[p] = ranked[k];
        }
        return slot_of;
    };

    // Rounds run until the slots are used up. The last one may be
    // partial â€” with fewer slots than lights that is the ONLY round,
    // and the lights it does not reach simply don't fire this loop.
    // Which ones those are comes out of the golden-ratio placement, so
    // they are spread across the rig and across the brightness range
    // rather than being one corner of it; Reseed picks a different set.
    std::vector<int> seq;
    seq.reserve(slots);
    while (static_cast<int>(seq.size()) < slots) {
        const std::vector<int> round_order = balanced_order();
        for (int i = 0; i < n && static_cast<int>(seq.size()) < slots; ++i) {
            seq.push_back(round_order[i]);
        }
    }

    // A light must never stack on itself. Independent rounds can put a
    // light at the end of one round and the start of the next (or, via
    // the seamless wrap, the last round and the first), so repair any
    // pair closer than half a round by swapping with a nearby slot of
    // similar brightness â€” the balance the placement built survives.
    const int min_gap = (n / 2 < 1) ? 1 : n / 2;
    std::vector<std::vector<int>> pos_of(n);
    for (int s = 0; s < slots; ++s) pos_of[seq[s]].push_back(s);
    auto circ = [&](int a, int b) {
        int d = a > b ? a - b : b - a;
        return (slots - d < d) ? slots - d : d;
    };
    // Would `light` sitting at slot `at` be clear of its other hits?
    // `leaving` is the slot it moves out of (ignored in the check).
    auto clear_at = [&](int light, int at, int leaving) {
        for (int p : pos_of[light]) {
            if (p == leaving || p == at) continue;
            if (circ(p, at) < min_gap) return false;
        }
        return true;
    };
    for (int s = 0; s < slots; ++s) {
        const int a = seq[s];
        if (clear_at(a, s, s)) continue;
        int best = -1;
        double best_diff = 0.0;
        for (int d = 1; d <= n && d < slots; ++d) {
            for (int t : { s + d, s - d }) {
                const int u = ((t % slots) + slots) % slots;
                const int b = seq[u];
                if (b == a) continue;
                if (!clear_at(a, u, s) || !clear_at(b, s, u)) continue;
                const double diff = std::fabs(bright[a] - bright[b]);
                if (best < 0 || diff < best_diff) { best = u; best_diff = diff; }
            }
            if (best >= 0 && d >= 4) break;   // a few candidates is plenty
        }
        if (best < 0) continue;               // tiny rig: nothing better
        const int b = seq[best];
        for (int& p : pos_of[a]) if (p == s)    { p = best; break; }
        for (int& p : pos_of[b]) if (p == best) { p = s;    break; }
        std::swap(seq[s], seq[best]);
    }

    for (int slot = 0; slot < slots; ++slot) {
        const float jitter = (u01(rng) - 0.5f) * 0.7f;   // Â±0.35 slot
        float t = (static_cast<float>(slot) + 0.5f + jitter) * slot_width;
        t = std::fmod(t, static_cast<float>(loop_frames));
        if (t < 0.f) t += static_cast<float>(loop_frames);
        for (const auto& m : leads[seq[slot]].members) {
            chase.scatter.push_back(ScatterHit{ m, t });
        }
    }
}

// ===== Standard pack ==================================================

namespace {

struct PackEntry {
    const char* name;
    int         template_index;   // index into kChaseTemplateNames
    bool        reverse;          // applied AFTER the template
};

// Left to Right, Bottom to Top, Center Out, 3 Step, Random. Bottom to
// Top is the Top-to-Bottom template reversed; no separate reverse
// comps are emitted (the server handles direction).
const PackEntry kStandardPack[] = {
    { "Left to Right", 0, false },
    { "Bottom to Top", 1, true  },
    { "Center Out",    2, false },
    { "3 Step Chase",  3, false },
    { "Random",        4, false },
};

bool NameTaken(const PanelState& state, const std::string& name)
{
    for (const auto& c : state.chases) if (c.name == name) return true;
    return false;
}

// "Left to Right" -> "Left to Right 2" -> "Left to Right 3" ...
std::string UniqueChaseName(const PanelState& state, const std::string& base)
{
    if (!NameTaken(state, base)) return base;
    for (int n = 2; n < 1000; ++n) {
        std::string candidate = base + " " + std::to_string(n);
        if (!NameTaken(state, candidate)) return candidate;
    }
    return base;
}

} // namespace

void SortAllSources(PanelState& state, SortMode mode, bool reverse,
                    uint32_t seed)
{
    if (state.sources.empty()) return;

    const uint32_t active_id =
        (state.active_source_index >= 0 &&
         state.active_source_index < (int)state.sources.size())
        ? state.sources[state.active_source_index].source_id : 0;

    for (auto& src : state.sources) {
        SortLayers(src.layers, mode, reverse, seed, &state, src.source_id);
    }

    // Order the sources by their now-leading light. This mirrors
    // SortLayers' ordering rather than calling it: running a sort on a
    // two-element vector and reading off the winner is NOT a strict
    // weak ordering (it answers "true" both ways for equal keys), which
    // is undefined behaviour inside std::sort.
    auto leading_less = [&](const Source& a, const Source& b) {
        // A source with nothing in it has no position; park it at the
        // end rather than letting it interleave.
        if (a.layers.empty() || b.layers.empty()) {
            return !a.layers.empty() && b.layers.empty();
        }
        const LayerInfo& la = a.layers.front();
        const LayerInfo& lb = b.layers.front();
        switch (mode) {
        case SortMode::CentroidX:  if (la.cx     != lb.cx)     return la.cx     < lb.cx;     break;
        case SortMode::CentroidY:  if (la.cy     != lb.cy)     return la.cy     < lb.cy;     break;
        case SortMode::HotspotX:   if (la.cx_hot != lb.cx_hot) return la.cx_hot < lb.cx_hot; break;
        case SortMode::HotspotY:   if (la.cy_hot != lb.cy_hot) return la.cy_hot < lb.cy_hot; break;
        case SortMode::PeakX:      if (la.peak_x != lb.peak_x) return la.peak_x < lb.peak_x; break;
        case SortMode::PeakY:      if (la.peak_y != lb.peak_y) return la.peak_y < lb.peak_y; break;
        case SortMode::Brightness: if (la.total  != lb.total)  return la.total  > lb.total;  break;
        case SortMode::RadialSweep: {
            const float aa = std::atan2(la.cy - 0.5f, la.cx - 0.5f);
            const float ab = std::atan2(lb.cy - 0.5f, lb.cx - 0.5f);
            if (aa != ab) return aa < ab;
            break;
        }
        case SortMode::DistanceFromCenter: {
            const float da = (la.cx - 0.5f) * (la.cx - 0.5f) +
                             (la.cy - 0.5f) * (la.cy - 0.5f);
            const float db = (lb.cx - 0.5f) * (lb.cx - 0.5f) +
                             (lb.cy - 0.5f) * (lb.cy - 0.5f);
            if (da != db) return da < db;
            break;
        }
        case SortMode::Random: {
            const uint32_t ka = la.fnv1a_hash * 2654435761u + seed;
            const uint32_t kb = lb.fnv1a_hash * 2654435761u + seed;
            if (ka != kb) return ka < kb;
            break;
        }
        default:
            // Name-based and staging-only modes: order by the leading
            // light's name.
            if (la.display_name != lb.display_name) {
                return la.display_name < lb.display_name;
            }
            break;
        }
        // Equal on the sort key: fall back to something total and
        // stable so the ordering stays well defined.
        return a.source_id < b.source_id;
    };
    std::stable_sort(state.sources.begin(), state.sources.end(),
        [&](const Source& a, const Source& b) {
            return reverse ? leading_less(b, a) : leading_less(a, b);
        });

    if (active_id != 0) {
        for (size_t i = 0; i < state.sources.size(); ++i) {
            if (state.sources[i].source_id == active_id) {
                state.active_source_index = (int)i;
                break;
            }
        }
    }
}

int PruneOrphanedRefs(PanelState& state)
{
    std::vector<uint32_t> live;
    live.reserve(state.sources.size());
    for (const auto& src : state.sources) live.push_back(src.source_id);
    return PruneOrphanedRefs(state, live);
}

int PruneOrphanedRefs(PanelState& state,
                      const std::vector<uint32_t>& live_source_ids)
{
    std::unordered_set<uint32_t> live_sources(live_source_ids.begin(),
                                              live_source_ids.end());

    int removed = 0;
    auto orphaned = [&](const LayerRef& r) {
        return live_sources.find(r.source_id) == live_sources.end();
    };
    auto sweep = [&](std::vector<LayerRef>& members) {
        const size_t before = members.size();
        members.erase(std::remove_if(members.begin(), members.end(), orphaned),
                      members.end());
        removed += static_cast<int>(before - members.size());
    };

    for (auto& t : state.tags)  sweep(t.members);
    for (auto& b : state.binds) sweep(b.members);

    for (auto& c : state.chases) {
        for (auto& st : c.stages) sweep(st.members);
        // A stage with nothing left in it would still occupy a slot in
        // the running order, stretching the chase with a silent gap.
        c.stages.erase(std::remove_if(c.stages.begin(), c.stages.end(),
                                      [](const ChaseStage& s) {
                                          return s.members.empty();
                                      }),
                       c.stages.end());
        const size_t before = c.scatter.size();
        c.scatter.erase(std::remove_if(c.scatter.begin(), c.scatter.end(),
                                       [&](const ScatterHit& h) {
                                           return orphaned(h.ref);
                                       }),
                        c.scatter.end());
        removed += static_cast<int>(before - c.scatter.size());
    }

    // Position overrides are keyed the same way.
    {
        const size_t before = state.position_overrides.size();
        state.position_overrides.erase(
            std::remove_if(state.position_overrides.begin(),
                           state.position_overrides.end(),
                           [&](const PositionOverride& p) {
                               return orphaned(p.layer);
                           }),
            state.position_overrides.end());
        removed += static_cast<int>(before - state.position_overrides.size());
    }
    return removed;
}

// ===== Export naming ==================================================

std::string CompactExportName(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    bool at_word_start = true;
    for (char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out += at_word_start ? static_cast<char>(std::toupper(u)) : c;
            at_word_start = false;
        } else {
            // Any run of non-alphanumerics is one word break.
            at_word_start = !out.empty();
        }
    }
    return out;
}

int CarryOverChaseTweaks(PanelState& state,
                         const std::vector<Chase>& previous, float fps)
{
    if (previous.empty()) return 0;
    int matched = 0;

    for (Chase& c : state.chases) {
        const Chase* was = nullptr;
        for (const Chase& p : previous) {
            if (p.name == c.name) { was = &p; break; }
        }
        if (!was) continue;
        ++matched;

        // What the artist tuned, and nothing else. Identity and
        // membership stay as freshly generated â€” a chase that just lost
        // three lights must not get the old stage list back.
        c.timing              = was->timing;
        c.scatter_density     = was->scatter_density;
        c.random_seed         = was->random_seed;
        c.sort_mode           = was->sort_mode;
        c.sort_reverse        = was->sort_reverse;
        c.desired_stage_count = was->desired_stage_count;
        c.loop_seconds        = was->loop_seconds;
        c.loop_multiple       = was->loop_multiple;
        c.loop_cycles         = was->loop_cycles;
        c.loop_offset         = was->loop_offset;

        // Rebuild against the carried settings: the sort basis decides
        // the running order and the density decides the scatter, so
        // neither survives being copied on after the fact.
        if (c.random_scatter) {
            RegenerateScatter(c, state, fps);
        } else {
            RegenerateChaseStages(c, state, c.desired_stage_count);
        }
    }
    return matched;
}

int CountEligibleLights(const PanelState& state,
                        const std::vector<uint32_t>& tag_filter)
{
    // One probe through the real stage generator, so inclusion,
    // dedupe, tag filtering and bind coalescing are all counted
    // exactly as a built chase would see them.
    Chase probe;
    probe.tag_filter = tag_filter;
    probe.sort_mode  = SortMode::HotspotX;
    RegenerateChaseStages(probe, state, 0);
    return static_cast<int>(probe.stages.size());
}

float AchievedLightsOn(const ChaseTiming& t)
{
    if (t.step_duration <= 0.f) return 0.f;
    return (t.duration + (t.hold > 0.f ? t.hold : 0.f)) / t.step_duration;
}

int GenerateStandardPack(PanelState& state,
                         const std::vector<uint32_t>& tag_ids, float fps,
                         float lights_on, float sweep_seconds)
{
    // The tags SCOPE one pack; they do not multiply it. Checking three
    // tags means "build the five chases out of those three families'
    // lights", not "build five chases per family" â€” David, 2026-09-09:
    // "if I select a bunch of tag groups, that means I want those
    // included in the 5. Not that I want the 5 chases for each group."
    // An empty list is the whole scene.
    const std::vector<uint32_t>& tag_filter = tag_ids;

    // Names stay plain whatever the scope, and that is load-bearing:
    // regenerating after narrowing the scope has to match the previous
    // set BY NAME for the timing to carry over (CarryOverChaseTweaks).
    // Qualifying the name with a tag would silently break that.
    const std::string prefix;

    const int n_lights = CountEligibleLights(state, tag_filter);
    if (n_lights < 1) return 0;

    // Outside loop mode a sweep has no loop to fit into and its length
    // is the artist's knob instead.
    const Chase loop_probe;                  // defaults: loop_multiple 1
    const int stage_loop_frames =
        SessionAnimates(state) ? ChaseLoopFrames(loop_probe, state, fps) : 0;

    int made = 0;
    for (const PackEntry& e : kStandardPack) {
        Chase c;
        c.chase_id   = state.next_chase_id++;
        c.name       = UniqueChaseName(state, prefix + e.name);
        c.tag_filter = tag_filter;
        ApplyTemplateToChase(c, e.template_index);
        c.sort_reverse = e.reverse;          // after the template resets it

        // The hit envelope is the same choice for every chase in the
        // pack â€” the artist's Hit preset â€” and neither knob touches it.
        c.timing = state.default_timing;

        if (c.random_scatter) {
            // Random's knob is how many lights are lit at a time; the
            // number of copies per light follows from it, against the
            // scatter's own loop.
            const int scatter_loop = ChaseLoopFrames(c, state, fps);
            c.scatter_density = DensityForLightsOn(lights_on, n_lights,
                                                   scatter_loop,
                                                   c.timing.duration);
            RegenerateScatter(c, state, fps);
        } else {
            RegenerateChaseStages(c, state, c.desired_stage_count);
            if (c.desired_stage_count > 0) {
                // Chunked (3 Step): the chunks overlap so nothing goes
                // dark between them, and the hit length is derived
                // rather than chosen.
                c.timing = FitChunkedTiming(static_cast<int>(c.stages.size()),
                                            stage_loop_frames, c.loop_cycles,
                                            fps, c.timing, sweep_seconds);
            } else {
                // A sweep's knob is how long the clip runs; the stagger
                // between lights falls out of it.
                c.timing = FitSweepTiming(static_cast<int>(c.stages.size()),
                                          stage_loop_frames, c.loop_cycles,
                                          fps, c.timing, sweep_seconds);
            }
        }

        state.chases.push_back(std::move(c));
        ++made;
    }
    return made;
}

// ===== Hit presets ====================================================

void ApplyHitPreset(ChaseTiming& t, int index)
{
    if (index < 0 || index >= kHitPresetCount) return;
    // Length and shape only. Pacing (step) and look (opacity, gamma)
    // are separate decisions and must survive changing the hit.
    t.duration = kHitPresets[index].duration;
    t.attack   = kHitPresets[index].attack;
}

int NearestHitPreset(const ChaseTiming& t)
{
    int best = kDefaultHitPreset;
    float best_err = -1.f;
    for (int i = 0; i < kHitPresetCount; ++i) {
        const float err = std::fabs(kHitPresets[i].duration - t.duration);
        if (best_err < 0.f || err < best_err) {
            best_err = err;
            best = i;
        }
    }
    return best;
}

// ===== Fitting the timing =============================================

ChaseTiming FitSweepTiming(int n_stages, int loop_frames, int cycles,
                           float fps, ChaseTiming base,
                           float sweep_seconds)
{
    if (n_stages < 1) return base;

    const int   cy      = (cycles < 1) ? 1 : cycles;
    const float fps_eff = (fps < 1.f) ? 1.f : fps;

    float step;
    if (loop_frames > 0) {
        // Loop mode: the scene's loop owns the spacing. Stages spread
        // evenly across it so the chase wraps seamlessly.
        step = static_cast<float>(loop_frames) /
               (static_cast<float>(n_stages) * static_cast<float>(cy));
    } else {
        // One knob: the length of the clip. The chase runs from the
        // first light starting to the last light finishing, so the
        // stagger covers (target - hit) across the gaps BETWEEN lights.
        const float target = (sweep_seconds > 0.f ? sweep_seconds : 1.f)
                             * fps_eff;
        const int gaps = (n_stages > 1) ? (n_stages - 1) : 1;
        step = (target - base.duration) / static_cast<float>(gaps);
    }

    // A quarter frame is already past useful; below that there are more
    // lights than the clip can articulate.
    if (step < 0.25f) step = 0.25f;
    base.step_duration = step;
    return base;
}

ChaseTiming FitChunkedTiming(int n_stages, int loop_frames, int cycles,
                             float fps, ChaseTiming base,
                             float clip_seconds)
{
    (void)n_stages; (void)loop_frames; (void)cycles;
    (void)fps;      (void)clip_seconds;

    // The hit is the artist's Hit preset and is left exactly as chosen.
    // The spacing is a flat default rather than anything derived from
    // the hit: David's number, "next after 10 f". It is the per-scene
    // knob from there, and a hit change no longer resets it.
    base.step_duration = kDefaultChunkStepFrames;
    return base;
}

float DensityForLightsOn(float lights_on, int n_lights, int loop_frames,
                         float hit_frames)
{
    if (n_lights < 1 || loop_frames < 1 || hit_frames <= 0.f) return 1.f;
    const float want = (lights_on > 0.f) ? lights_on : 0.f;
    const float d = want * static_cast<float>(loop_frames) /
                    (static_cast<float>(n_lights) * hit_frames);
    // Density is fractional, and below 1 it means some lights sit the
    // loop out â€” the only way a big rig with a long hit gets sparse.
    // The floor is one light firing: a scatter with nothing in it is
    // not a chase.
    const float floor_d = 1.f / static_cast<float>(n_lights);
    return (d < floor_d) ? floor_d : d;
}

float ScatterLightsOn(float density, int n_lights, int loop_frames,
                      float hit_frames)
{
    if (loop_frames < 1) return 0.f;
    // Mirror the generator: what is actually placed is a whole number
    // of slots, so report the lights-on that count really produces
    // rather than the un-rounded ideal.
    const long want = std::lround(static_cast<double>(n_lights) * density);
    const float slots = static_cast<float>(want < 1 ? 1 : want);
    return slots * hit_frames / static_cast<float>(loop_frames);
}

// ===== Random-scatter preview ========================================
// ScatterLoopFrames + RegenerateScatter are shared inlines in
// panel_state.h (the AE builder regenerates too â€” see note there).

// Envelope (0..1) of a scattered hit at `playhead`, seamless-wrapped:
// a hit whose tail crosses the loop end re-enters at the start.
float ScatterHitEnvelope(float playhead, float start, float loop_frames,
                         const ChaseTiming& t)
{
    // Delegates to the same implementation the AE builder uses, so a
    // preview and the comp it builds cannot disagree about the shape.
    return static_cast<float>(build_math::WrappedEnvelope(
        start, playhead, loop_frames, t.attack, t.duration, t.hold));
}

// Composite a scatter chase at `playhead` (loop-relative frames).
// Same accumulation as BuildChaseComposite; per-hit wrapped envelope.
bool BuildScatterComposite(const Chase& chase, const PanelState& state,
                           float playhead, float fps,
                           std::vector<uint8_t>& out_rgba,
                           int& out_w, int& out_h)
{
    out_w = 0; out_h = 0;
    const int loop_frames = ChaseLoopFrames(chase, state, fps);
    float shared_peak = 0.f;
    for (const auto& hit : chase.scatter) {
        const LayerInfo* L = FindLayerByRef(state, hit.ref);
        if (!L) continue;
        if (out_w == 0 && L->thumb_w > 0 && L->thumb_h > 0) {
            out_w = L->thumb_w; out_h = L->thumb_h;
        }
        if (L->thumb_peak > shared_peak) shared_peak = L->thumb_peak;
    }
    if (out_w == 0 || out_h == 0) return false;

    std::vector<CompositeContribution> contribs;
    for (const auto& hit : chase.scatter) {
        const float env = ScatterHitEnvelope(playhead, hit.start_frame,
                                             static_cast<float>(loop_frames),
                                             chase.timing);
        float op_pk = chase.timing.opacity_peak;
        float op_fl = chase.timing.opacity_floor;
        if (op_fl < 0.f) op_fl = 0.f;
        if (op_fl > op_pk) op_fl = op_pk;
        // With a floor every hit holds the light at >= floor, so don't
        // skip troughs; Lighten max-combines duplicate floor contribs.
        if (env <= 0.f && op_fl <= 0.f) continue;
        const float opacity = (op_fl + (op_pk - op_fl) * env) / 100.f;
        const float gamma_t = chase.timing.gamma_baseline +
            (chase.timing.gamma_peak - chase.timing.gamma_baseline) * env;
        const float k = opacity * gamma_t;
        if (k <= 0.f) continue;
        contribs.push_back({ FindLayerByRef(state, hit.ref), k });
    }
    return LightenComposite(contribs, out_w, out_h, shared_peak, out_rgba);
}

void ApplyTemplateToChase(Chase& c, int template_idx)
{
    // Defaults: 0 chunks = one light per stage (auto), auto sort
    // (template re-derives stages from scratch, so any prior manual
    // arrangement is intentionally dropped). 3 Step (= 3 chunks) /
    // Center Out override below.
    c.desired_stage_count = 0;   // 0 = one light per stage (auto)
    c.tag_balanced_chunks = false;
    c.symmetric_pairs = false;
    c.manual_stages = false;
    c.random_scatter = false;
    switch (template_idx) {
    // Leftâ†’Right: Hotspot X (bright concentrated source, spill
    // ignored) is the default ordering basis for chases.
    case 0: c.sort_mode = SortMode::HotspotX;           c.sort_reverse = false; break;
    case 1: c.sort_mode = SortMode::CentroidY;          c.sort_reverse = false; break;
    case 2:
        // Center Out: order by HOTSPOT x (the bright concentrated
        // source, not the spill-influenced whole-layer centroid) so
        // the mirrored pairing is spatially symmetric, then weld
        // pairs outward from the middle. Even light counts pair the
        // two centermost together as stage 1 (see RegenerateChase
        // Stages' symmetric_pairs branch: lo=(n-1)/2, hi=n/2).
        c.sort_mode = SortMode::HotspotX;
        c.sort_reverse = false;
        c.symmetric_pairs = true;
        break;
    case 3:
        // 3 Step: always three chunks, and it REPEATS rather than
        // ending in black â€” the chunks keep firing and the last
        // tail wraps round to the start.
        c.sort_mode = SortMode::HotspotX;
        c.sort_reverse = false;
        c.desired_stage_count = 3;
        c.loops = true;
        c.tag_balanced_chunks = true;
        break;
    case 4: {
        // Random = the stratified scatter generator (not a sort).
        c.random_scatter  = true;
        c.loop_seconds    = (c.loop_seconds > 0.f) ? c.loop_seconds : 10.0f;
        c.scatter_density = (c.scatter_density > 0.f) ? c.scatter_density : 5.f;
        std::random_device rd;
        c.random_seed = rd();
        break;
    }
    case 5:
        // Hit: one stage with ALL lights firing together exactly
        // once. In loop-mode the envelope sits at phase 0 and the
        // wrapped triangle is 0 outside [0, hit_duration), so it
        // plays once at the start then stays black for the rest of
        // the loop â€” "black to black", no looping/wrapping in the
        // animation. Cycles forced to 1 so a cycle setting carried
        // over from another template doesn't repeat the hit.
        c.sort_mode = SortMode::HotspotX;
        c.sort_reverse = false;
        c.desired_stage_count = 1;     // 1 chunk = all lights in one stage
        c.loop_cycles = 1;             // exactly one hit per loop
        break;
    case 6: default: break; // custom â€” leave as-is
    }
}

} // namespace chase_gen
