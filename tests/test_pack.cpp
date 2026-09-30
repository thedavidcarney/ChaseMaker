// The standard pack: one click makes the five chases David actually
// builds, scene-wide or scoped to a single tag.

#include "test_harness.h"
#include "test_helpers.h"

#include <string>

#include "chase_gen.h"

using namespace chase_gen;
using testing::MakeState;

namespace {

const Chase* FindChase(const PanelState& st, const std::string& name)
{
    for (const auto& c : st.chases) if (c.name == name) return &c;
    return nullptr;
}

// A session whose lights are split across two tags.
std::unique_ptr<PanelState> MakeTaggedState()
{
    auto st = MakeState(10);
    const uint32_t sid = st->sources[0].source_id;
    Tag floor_tag;
    floor_tag.tag_id = 1;
    floor_tag.name = "floor";
    for (int i = 0; i < 4; ++i) {
        floor_tag.members.push_back({ sid, st->sources[0].layers[i].fnv1a_hash });
    }
    Tag wall_tag;
    wall_tag.tag_id = 2;
    wall_tag.name = "wall";
    for (int i = 4; i < 10; ++i) {
        wall_tag.members.push_back({ sid, st->sources[0].layers[i].fnv1a_hash });
    }
    st->tags.push_back(floor_tag);
    st->tags.push_back(wall_tag);
    st->next_tag_id = 3;
    return st;
}

} // namespace

TEST(pack_creates_the_five_standard_chases)
{
    auto st = MakeState(12);
    const int n = GenerateStandardPack(*st, {}, 30.f);

    CHECK_EQ(n, 5);
    CHECK_EQ((int)st->chases.size(), 5);
    CHECK(FindChase(*st, "Left to Right") != nullptr);
    CHECK(FindChase(*st, "Bottom to Top") != nullptr);
    CHECK(FindChase(*st, "Center Out") != nullptr);
    CHECK(FindChase(*st, "3 Step Chase") != nullptr);
    CHECK(FindChase(*st, "Random") != nullptr);
}

// No reverse variants: David reverses on the server, so a separate
// comp for each direction is waste.
TEST(pack_has_no_reverse_variants)
{
    auto st = MakeState(12);
    GenerateStandardPack(*st, {}, 30.f);
    CHECK(FindChase(*st, "Right to Left") == nullptr);
    CHECK(FindChase(*st, "Top to Bottom") == nullptr);
}

TEST(pack_bottom_to_top_sorts_up_the_frame)
{
    auto st = MakeState(12);
    GenerateStandardPack(*st, {}, 30.f);
    const Chase* c = FindChase(*st, "Bottom to Top");
    CHECK(c != nullptr);
    if (!c) return;
    CHECK_EQ((int)c->sort_mode, (int)SortMode::CentroidY);
    CHECK(c->sort_reverse);
}

TEST(pack_center_out_and_three_step_keep_their_staging)
{
    auto st = MakeState(12);
    GenerateStandardPack(*st, {}, 30.f);

    const Chase* co = FindChase(*st, "Center Out");
    CHECK(co != nullptr);
    if (co) CHECK(co->symmetric_pairs);

    const Chase* ts = FindChase(*st, "3 Step Chase");
    CHECK(ts != nullptr);
    if (ts) CHECK_EQ(ts->desired_stage_count, 3);
}

TEST(pack_random_is_a_seeded_scatter)
{
    auto st = MakeState(12);
    GenerateStandardPack(*st, {}, 30.f);
    const Chase* r = FindChase(*st, "Random");
    CHECK(r != nullptr);
    if (!r) return;
    CHECK(r->random_scatter);
    CHECK(r->scatter_density > 0.f);
}

TEST(pack_stages_are_generated_not_left_empty)
{
    auto st = MakeState(12);
    GenerateStandardPack(*st, {}, 30.f);
    for (const auto& c : st->chases) {
        if (c.random_scatter) continue;
        CHECK(!c.stages.empty());
    }
}

// ---- Tags SCOPE the pack, they do not multiply it -------------------
//
// David, 2026-09-09: "if I select a bunch of tag groups, that means I
// want those included in the 5. Not that I want the 5 chases for each
// group." An earlier build made a pack per tag, which filled the panel
// with chases nobody asked for.

TEST(pack_scoped_to_a_tag_filters_to_it)
{
    auto st = MakeTaggedState();
    const int n = GenerateStandardPack(*st, { 1u }, 30.f);   // "floor"

    CHECK_EQ(n, 5);
    CHECK_EQ((int)st->chases.size(), 5);
    const Chase* c = FindChase(*st, "Left to Right");
    CHECK(c != nullptr);
    if (!c) return;
    CHECK_EQ((int)c->tag_filter.size(), 1);
    CHECK_EQ(c->tag_filter[0], 1u);
    // Only the four tagged lights make it into the stages.
    int members = 0;
    for (const auto& s : c->stages) members += (int)s.members.size();
    CHECK_EQ(members, 4);
}

// The names stay plain whatever the scope, and that is load-bearing:
// regenerating with a narrower scope has to match the previous set BY
// NAME for CarryOverChaseTweaks to keep the timing.
TEST(a_scoped_pack_still_uses_the_plain_template_names)
{
    auto st = MakeTaggedState();
    GenerateStandardPack(*st, { 1u }, 30.f);
    for (const char* n : { "Left to Right", "Bottom to Top", "Center Out",
                           "3 Step Chase", "Random" }) {
        CHECK(FindChase(*st, n) != nullptr);
    }
}

TEST(pack_scene_wide_uses_every_light)
{
    auto st = MakeTaggedState();
    GenerateStandardPack(*st, {}, 30.f);
    const Chase* c = FindChase(*st, "Left to Right");
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c->tag_filter.empty());
    int members = 0;
    for (const auto& s : c->stages) members += (int)s.members.size();
    CHECK_EQ(members, 10);
}

// Several groups at once: FIVE chases built from the union of their
// lights, not five per group. This is the case that produced "tons of
// chases".
TEST(several_tags_make_one_pack_from_their_union)
{
    auto st = MakeTaggedState();
    const int n = GenerateStandardPack(*st, { 1u, 2u }, 30.f);

    CHECK_EQ(n, 5);
    CHECK_EQ((int)st->chases.size(), 5);
    const Chase* c = FindChase(*st, "Left to Right");
    CHECK(c != nullptr);
    if (!c) return;
    CHECK_EQ((int)c->tag_filter.size(), 2);
    // floor (4) + wall (6), every light in one running order.
    int members = 0;
    for (const auto& s : c->stages) members += (int)s.members.size();
    CHECK_EQ(members, 10);
}

// Narrowing from the whole scene to a couple of groups keeps the names
// stable, so the timing carries over — the Include/Exclude round trip.
TEST(narrowing_the_scope_still_carries_the_timing_over)
{
    auto st = MakeTaggedState();
    GenerateStandardPack(*st, {}, 30.f);
    for (auto& c : st->chases) ApplyHitPreset(c.timing, 3);   // Hit 90
    const std::vector<Chase> before = st->chases;

    st->chases.clear();
    const int n = GenerateStandardPack(*st, { 1u }, 30.f);
    CHECK_EQ(n, 5);
    CHECK_EQ(CarryOverChaseTweaks(*st, before, 30.f), 5);

    for (const auto& c : st->chases) {
        CHECK_NEAR(c.timing.duration, 90.0, 1e-3);
        CHECK_EQ((int)c.tag_filter.size(), 1);   // ...and it really narrowed
    }
}

TEST(pack_generated_twice_disambiguates_names)
{
    auto st = MakeState(6);
    GenerateStandardPack(*st, {}, 30.f);
    GenerateStandardPack(*st, {}, 30.f);

    CHECK_EQ((int)st->chases.size(), 10);
    CHECK(FindChase(*st, "Left to Right") != nullptr);
    CHECK(FindChase(*st, "Left to Right 2") != nullptr);
}

TEST(pack_chase_ids_are_unique)
{
    auto st = MakeState(6);
    GenerateStandardPack(*st, {}, 30.f);
    GenerateStandardPack(*st, {}, 30.f);
    for (size_t i = 0; i < st->chases.size(); ++i) {
        for (size_t j = i + 1; j < st->chases.size(); ++j) {
            CHECK(st->chases[i].chase_id != st->chases[j].chase_id);
        }
    }
}

// ---- Timing ---------------------------------------------------------

TEST(pack_sweep_timing_is_fitted_to_the_scene_not_left_at_defaults)
{
    auto st = MakeState(60);
    st->sources[0].frame_count = 120;
    st->sources[0].animation   = true;
    GenerateStandardPack(*st, {}, 30.f);

    const Chase* c = FindChase(*st, "Left to Right");
    CHECK(c != nullptr);
    if (!c) return;
    // Loop mode: the scene's 120-frame loop divided by 60 lights.
    CHECK_NEAR(c->timing.step_duration, 2.0, 1e-3);
    // The hit envelope is the artist's Hit preset and is NOT derived
    // from the light count.
    CHECK_NEAR(c->timing.duration, 30.0, 1e-3);
    CHECK_NEAR(c->timing.attack, 5.0, 1e-3);
}

// The knob for a sweep is how long the clip runs; adding lights
// tightens the spacing instead of dragging the chase out.
TEST(pack_sweeps_keep_their_clip_length_whatever_the_light_count)
{
    auto few = MakeState(10);
    auto many = MakeState(80);
    GenerateStandardPack(*few, {}, 30.f);
    GenerateStandardPack(*many, {}, 30.f);

    const Chase* a = FindChase(*few, "Left to Right");
    const Chase* b = FindChase(*many, "Left to Right");
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    if (!a || !b) return;
    CHECK(b->timing.step_duration < a->timing.step_duration);

    const float ta = (a->stages.size() - 1) * a->timing.step_duration
                     + a->timing.duration;
    const float tb = (b->stages.size() - 1) * b->timing.step_duration
                     + b->timing.duration;
    CHECK_NEAR(ta, tb, 1.5);
    CHECK_NEAR(ta, 4.0 * 30.0, 1.5);          // the 4-second default
}

// 3 Step is three chunks whatever the light count, and its spacing is
// its own default rather than anything the scene or the hit dictates.
TEST(pack_three_step_is_three_chunks_on_the_default_spacing)
{
    auto st = MakeState(60);
    st->sources[0].frame_count = 120;
    st->sources[0].animation   = true;
    GenerateStandardPack(*st, {}, 30.f, 1.f);

    const Chase* c = FindChase(*st, "3 Step Chase");
    CHECK(c != nullptr);
    if (!c) return;
    CHECK_EQ((int)c->stages.size(), 3);
    CHECK_NEAR(c->timing.step_duration, kDefaultChunkStepFrames, 1e-3);
    CHECK(c->timing.duration > c->timing.step_duration);
}

// Random's knob is how many lights are lit; the copies per light are
// derived from it against the loop.
TEST(pack_random_density_comes_from_the_lights_on_target)
{
    auto st = MakeState(10);
    st->sources[0].frame_count = 300;          // the 10-second loop
    st->sources[0].animation   = true;
    GenerateStandardPack(*st, {}, 30.f, 4.f);

    const Chase* c = FindChase(*st, "Random");
    CHECK(c != nullptr);
    if (!c) return;
    CHECK(c->scatter_density > 0.f);
    const float got = ScatterLightsOn(c->scatter_density, 10, 300,
                                      c->timing.duration);
    CHECK_NEAR(got, 4.0, 1.5);
    // And the hit envelope was left alone.
    CHECK_NEAR(c->timing.duration, 30.0, 1e-3);
}

TEST(pack_is_a_noop_without_lights)
{
    auto st = std::make_unique<PanelState>();
    const int n = GenerateStandardPack(*st, {}, 30.f);
    CHECK_EQ(n, 0);
    CHECK(st->chases.empty());
}

// ===== The Include/Exclude round trip ================================
//
// Generate, tune the hits and lengths in Review, notice a light that
// shouldn't be in the set, go back and untick it, generate again. The
// tuning has to survive that — David, 2026-09-09: "it should preserve
// the settings and timing changes that were done in the review tab."

TEST(regenerating_keeps_the_timing_tuned_in_review)
{
    auto st = MakeState(12);
    AutotagByName(st.get());
    CHECK_EQ(GenerateStandardPack(*st, {}, 30.f), 5);

    // Tune, the way the Review cells do.
    for (auto& c : st->chases) {
        ApplyHitPreset(c.timing, 3);           // Hit 90
        c.timing.step_duration = 17.f;
        c.timing.hold = 4.f;
        if (c.random_scatter) c.scatter_density = 0.75f;
        c.sort_mode = PanelState::SortMode::PeakY;
    }
    const std::vector<Chase> before = st->chases;

    // A light turns out not to belong.
    st->sources[0].layers[0].included = false;

    st->chases.clear();
    CHECK_EQ(GenerateStandardPack(*st, {}, 30.f), 5);
    const int kept = CarryOverChaseTweaks(*st, before, 30.f);
    CHECK_EQ(kept, 5);

    for (const auto& c : st->chases) {
        CHECK_NEAR(c.timing.duration, 90.0, 1e-3);
        CHECK_NEAR(c.timing.attack, 45.0, 1e-3);
        CHECK_NEAR(c.timing.step_duration, 17.0, 1e-3);
        CHECK_NEAR(c.timing.hold, 4.0, 1e-3);
        CHECK(c.sort_mode == PanelState::SortMode::PeakY);
        if (c.random_scatter) CHECK_NEAR(c.scatter_density, 0.75, 1e-4);
    }
}

// ...while the MEMBERSHIP is genuinely new. Keeping the old stage list
// would defeat the whole point of going back to trim the lights.
TEST(regenerating_drops_the_light_that_was_excluded)
{
    auto st = MakeState(10);
    AutotagByName(st.get());
    GenerateStandardPack(*st, {}, 30.f);
    const std::vector<Chase> before = st->chases;

    const uint32_t gone = st->sources[0].layers[3].fnv1a_hash;
    st->sources[0].layers[3].included = false;

    st->chases.clear();
    GenerateStandardPack(*st, {}, 30.f);
    CarryOverChaseTweaks(*st, before, 30.f);

    for (const auto& c : st->chases) {
        for (const auto& s : c.stages) {
            for (const auto& m : s.members) CHECK(m.fnv1a_hash != gone);
        }
        for (const auto& h : c.scatter) CHECK(h.ref.fnv1a_hash != gone);
        // And the set really did shrink.
        int members = 0;
        for (const auto& s : c.stages) members += (int)s.members.size();
        if (!c.random_scatter && !c.symmetric_pairs && c.desired_stage_count == 0)
            CHECK_EQ(members, 9);
    }
}

// A carried sort basis has to rebuild the running order, not just sit
// in the field — copying it on after generation would leave the stages
// ordered by the template's default.
TEST(a_carried_sort_basis_actually_reorders_the_stages)
{
    auto st = MakeState(8);
    // Spread the lights on Y so a Y sort differs from an X sort.
    for (int i = 0; i < (int)st->sources[0].layers.size(); ++i) {
        st->sources[0].layers[i].cy = st->sources[0].layers[i].cy_hot =
            st->sources[0].layers[i].peak_y =
                1.f - (float)i / 7.f;           // reversed against x
    }
    AutotagByName(st.get());
    GenerateStandardPack(*st, {}, 30.f);

    std::vector<Chase> before = st->chases;
    for (auto& c : before) {
        if (c.random_scatter || c.symmetric_pairs) continue;
        c.sort_mode = PanelState::SortMode::CentroidY;
        c.sort_reverse = false;
    }

    st->chases.clear();
    GenerateStandardPack(*st, {}, 30.f);
    CarryOverChaseTweaks(*st, before, 30.f);

    for (const auto& c : st->chases) {
        if (c.random_scatter || c.symmetric_pairs) continue;
        if (c.desired_stage_count > 0) continue;
        CHECK(!c.stages.empty());
        if (c.stages.empty()) continue;
        // Sorted by Y ascending, the first stage is the light whose y
        // is smallest — the LAST one in x order.
        const LayerInfo* first =
            FindLayerByRef(*st, c.stages.front().members[0]);
        CHECK(first != nullptr);
        if (first) CHECK_NEAR(first->cy, 0.0, 1e-3);
    }
}

// A chase with no counterpart in the previous set is left exactly as
// generated — adding a tag scope must not inherit another chase's hit.
TEST(a_brand_new_chase_is_not_given_someone_elses_settings)
{
    auto st = MakeState(9);
    AutotagByName(st.get());
    GenerateStandardPack(*st, {}, 30.f);
    std::vector<Chase> before = st->chases;
    for (auto& c : before) ApplyHitPreset(c.timing, 3);   // Hit 90

    st->chases.clear();
    GenerateStandardPack(*st, {}, 30.f);
    // Pretend one scope is new by renaming it out of the match set.
    st->chases[2].name = "Something Else";
    const ChaseTiming fresh = st->chases[2].timing;
    const int kept = CarryOverChaseTweaks(*st, before, 30.f);

    CHECK_EQ(kept, 4);
    CHECK_NEAR(st->chases[2].timing.duration, fresh.duration, 1e-4);
    CHECK_NEAR(st->chases[2].timing.attack, fresh.attack, 1e-4);
}
