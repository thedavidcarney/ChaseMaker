// 3 Step's chunking. David's rule, in his words: "The 3 chunks should
// be chosen by each auto-tag dividing by 3. When there is an uneven
// division use brightness calculations to find out how to divide them
// to make it most evenly lit" — and, on the families: "if there's a
// StringLights_A, b and c I need those to be in all 3 chunks each."
//
// So a chunk is a slice taken ACROSS the fixture families, never one
// family entire.

#include "test_harness.h"
#include "test_helpers.h"

#include <map>
#include <string>

#include "chase_gen.h"
#include "panel_state.h"

using namespace chase_gen;

namespace {

// A session of named families: `families` maps a name prefix to how
// many lights it has. Lights are spread left-to-right within a family
// so the sort order inside a family is well defined.
std::unique_ptr<PanelState> MakeTaggedState(
    const std::vector<std::pair<std::string, int>>& families,
    const std::vector<double>* totals = nullptr)
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

    int idx = 0;
    for (const auto& f : families) {
        for (int i = 0; i < f.second; ++i) {
            LayerInfo L;
            char name[64];
            std::snprintf(name, sizeof(name), "%s_%03d", f.first.c_str(), i);
            L.display_name = name;
            L.fnv1a_hash   = FNV1a32(L.display_name);
            L.source_id    = src.source_id;
            const float t  = (f.second > 1)
                ? static_cast<float>(i) / (f.second - 1) : 0.5f;
            L.cx = L.cx_hot = L.peak_x = t;
            L.cy = L.cy_hot = L.peak_y = 0.5f;
            L.total    = (totals && idx < (int)totals->size())
                         ? (*totals)[idx] : 1.0;
            L.included = true;
            src.layers.push_back(std::move(L));
            ++idx;
        }
    }
    st->sources.push_back(std::move(src));
    return st;
}

// Which family each light in a stage belongs to.
std::map<std::string, int> FamilyCounts(const ChaseStage& stage,
                                        const PanelState& st)
{
    std::map<std::string, int> out;
    for (const auto& ref : stage.members) {
        if (const LayerInfo* L = FindLayerByRef(st, ref)) {
            out[ExtractTagPrefix(L->display_name)]++;
        }
    }
    return out;
}

double StageLuminance(const ChaseStage& stage, const PanelState& st)
{
    double sum = 0.0;
    for (const auto& ref : stage.members) {
        if (const LayerInfo* L = FindLayerByRef(st, ref)) sum += L->total;
    }
    return sum;
}

Chase MakeThreeStep(const PanelState& st)
{
    Chase c;
    ApplyTemplateToChase(c, 3);
    RegenerateChaseStages(c, st, c.desired_stage_count);
    return c;
}

} // namespace

TEST(the_three_step_template_is_always_three_tag_balanced_chunks)
{
    Chase c;
    ApplyTemplateToChase(c, 3);
    CHECK_EQ(c.desired_stage_count, 3);
    CHECK(c.tag_balanced_chunks);
    CHECK(c.loops);
}

// The headline case, straight off David's example.
TEST(every_family_appears_in_every_chunk)
{
    auto st = MakeTaggedState({ {"StringLights_A", 9},
                                {"StringLights_B", 9},
                                {"StringLights_C", 9} });
    const Chase c = MakeThreeStep(*st);

    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.size() != 3) return;
    for (const auto& stage : c.stages) {
        const auto fam = FamilyCounts(stage, *st);
        CHECK_EQ((int)fam.size(), 3);
        for (const auto& kv : fam) CHECK_EQ(kv.second, 3);
    }
}

// The failure this replaces: contiguous chunking over the whole sort
// order hands each chunk one whole family, which reads as three
// separate scenes rather than one scene pulsing in three.
TEST(contiguous_chunking_would_have_split_by_family_and_no_longer_does)
{
    auto st = MakeTaggedState({ {"Alpha", 6}, {"Beta", 6}, {"Gamma", 6} });

    Chase contiguous;
    ApplyTemplateToChase(contiguous, 3);
    contiguous.tag_balanced_chunks = false;
    contiguous.sort_mode = PanelState::SortMode::Alphabetical;
    RegenerateChaseStages(contiguous, *st, 3);
    // Sorted by name, the families are already blocks: chunk 1 is all
    // Alpha. That is exactly the behaviour being replaced.
    CHECK_EQ((int)FamilyCounts(contiguous.stages[0], *st).size(), 1);

    Chase balanced;
    ApplyTemplateToChase(balanced, 3);
    balanced.sort_mode = PanelState::SortMode::Alphabetical;
    RegenerateChaseStages(balanced, *st, 3);
    for (const auto& stage : balanced.stages) {
        CHECK_EQ((int)FamilyCounts(stage, *st).size(), 3);
    }
}

// No light is dropped and none is doubled, whatever the arithmetic.
TEST(tag_balanced_chunks_place_every_light_exactly_once)
{
    auto st = MakeTaggedState({ {"Aa", 7}, {"Bb", 5}, {"Cc", 1}, {"Dd", 4} });
    const Chase c = MakeThreeStep(*st);

    int total = 0;
    std::vector<uint32_t> seen;
    for (const auto& stage : c.stages) {
        for (const auto& ref : stage.members) {
            ++total;
            for (uint32_t h : seen) CHECK(h != ref.fnv1a_hash);
            seen.push_back(ref.fnv1a_hash);
        }
    }
    CHECK_EQ(total, 17);
}

// A family that doesn't divide by three still splits as evenly as it
// can — the spare light goes somewhere, not everywhere.
TEST(an_uneven_family_splits_within_one_light_of_even)
{
    auto st = MakeTaggedState({ {"Odd", 5} });
    const Chase c = MakeThreeStep(*st);

    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.size() != 3) return;
    int lo = 99, hi = 0;
    for (const auto& stage : c.stages) {
        const int n = (int)stage.members.size();
        if (n < lo) lo = n;
        if (n > hi) hi = n;
    }
    CHECK(hi - lo <= 1);
}

// The brightness half of the rule: when a family has a spare light,
// it goes to the chunk that is dimmest so far — so the chunks end up
// closer in total light than a naive "first chunks get the extra".
TEST(brightness_settles_the_uneven_split)
{
    // Two families of 4. The first is bright, the second dim. A naive
    // split gives chunk 1 the spare from BOTH families; balancing
    // gives the second family's spare to a chunk the first shortchanged.
    std::vector<double> totals = { 10, 10, 10, 10,   1, 1, 1, 1 };
    auto st = MakeTaggedState({ {"Bright", 4}, {"Dim", 4} }, &totals);
    const Chase c = MakeThreeStep(*st);
    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.size() != 3) return;

    double lo = 1e18, hi = 0.0;
    for (const auto& stage : c.stages) {
        const double L = StageLuminance(stage, *st);
        if (L < lo) lo = L;
        if (L > hi) hi = L;
    }
    // The perfectly even share is 44/3 = 14.67. Naive first-chunks-win
    // would put both spares (10 + 1) on chunk 1 for a spread of 11;
    // balancing must do better than that.
    CHECK(hi - lo < 11.0);
    std::printf("    3 Step chunk luminance spread: %.1f (lo %.1f hi %.1f)\n",
                hi - lo, lo, hi);
}

// Regenerating twice must give the same answer — a chase that
// reshuffled itself between identical regens could not be judged in a
// preview at all.
TEST(tag_balanced_chunks_are_deterministic)
{
    auto st = MakeTaggedState({ {"Aa", 7}, {"Bb", 5}, {"Cc", 4} });
    const Chase a = MakeThreeStep(*st);
    const Chase b = MakeThreeStep(*st);

    CHECK_EQ((int)a.stages.size(), (int)b.stages.size());
    for (size_t i = 0; i < a.stages.size() && i < b.stages.size(); ++i) {
        CHECK_EQ((int)a.stages[i].members.size(),
                 (int)b.stages[i].members.size());
        for (size_t j = 0; j < a.stages[i].members.size() &&
                           j < b.stages[i].members.size(); ++j) {
            CHECK(a.stages[i].members[j] == b.stages[i].members[j]);
        }
    }
}

// Lights with no family (unique names, the single-member-tag case)
// still spread across the chunks rather than piling into the first.
TEST(unique_named_lights_spread_across_the_chunks)
{
    auto st = std::make_unique<PanelState>();
    Source src;
    src.source_id = 1;
    src.path = "synthetic.exr";
    src.scan_path = src.path;
    src.frame_count = 1;
    src.image_width = 1920;
    src.image_height = 1080;
    const char* names[] = { "Key Light", "Fill", "Rim", "Practical",
                            "Bounce", "Kicker" };
    for (int i = 0; i < 6; ++i) {
        LayerInfo L;
        L.display_name = names[i];
        L.fnv1a_hash   = FNV1a32(L.display_name);
        L.source_id    = src.source_id;
        L.cx = L.cx_hot = L.peak_x = i / 5.f;
        L.cy = L.cy_hot = L.peak_y = 0.5f;
        L.total = 1.0;
        L.included = true;
        src.layers.push_back(std::move(L));
    }
    st->sources.push_back(std::move(src));

    const Chase c = MakeThreeStep(*st);
    CHECK_EQ((int)c.stages.size(), 3);
    for (const auto& stage : c.stages) CHECK_EQ((int)stage.members.size(), 2);
}

// The Advanced editor's Chunks is the contiguous split and stays that
// way — the locked "Chunks = contiguous, sizes differ by at most one"
// decision is not overturned by 3 Step's rule.
TEST(plain_chunking_is_still_contiguous)
{
    auto st = MakeTaggedState({ {"Aa", 5}, {"Bb", 5} });
    Chase c;
    c.desired_stage_count = 2;
    c.sort_mode = PanelState::SortMode::Alphabetical;
    RegenerateChaseStages(c, *st, 2);
    CHECK(!c.tag_balanced_chunks);
    CHECK_EQ((int)c.stages.size(), 2);
    if (c.stages.size() != 2) return;
    // Alphabetical: the whole Aa family lands in the first chunk.
    CHECK_EQ((int)FamilyCounts(c.stages[0], *st).size(), 1);
}

// The clip-length knob asks "does this chase have a length of its
// own?", and it reads that off ChaseLoopFrames. A one-shot sweep on a
// still scene must answer 0 — returning the scatter default there is
// what hid the slider on Left to Right, Bottom to Top and Center Out.
TEST(a_one_shot_sweep_reports_no_loop_of_its_own)
{
    auto st = testing::MakeState(8);
    Chase sweep;
    ApplyTemplateToChase(sweep, 0);
    RegenerateChaseStages(sweep, *st, 0);
    CHECK(!ChaseWraps(sweep, *st));
    CHECK_EQ(ChaseLoopFrames(sweep, *st, 30.f), 0);

    Chase centre;
    ApplyTemplateToChase(centre, 2);
    RegenerateChaseStages(centre, *st, 0);
    CHECK_EQ(ChaseLoopFrames(centre, *st, 30.f), 0);

    // While the two that DO have a length still report one.
    Chase rnd;
    ApplyTemplateToChase(rnd, 4);
    CHECK(ChaseLoopFrames(rnd, *st, 30.f) > 0);

    Chase three = MakeThreeStep(*st);
    CHECK(ChaseLoopFrames(three, *st, 30.f) > 0);
}

// The degenerate case that matters in practice: a session reloaded
// without AE has no luminance at all, so every chunk ties on
// brightness. Balancing must fall back to light COUNT rather than
// handing every spare to the first chunk — 138 single-light sources
// with no metrics collapsed a 3 Step into one chunk.
TEST(chunks_still_balance_when_no_light_has_luminance)
{
    std::vector<double> zeros(150, 0.0);
    auto st = MakeTaggedState({ {"Aa", 1}, {"Bb", 1}, {"Cc", 1}, {"Dd", 1},
                                {"Ee", 1}, {"Ff", 1}, {"Gg", 1}, {"Hh", 1},
                                {"Ii", 1} }, &zeros);
    const Chase c = MakeThreeStep(*st);
    CHECK_EQ((int)c.stages.size(), 3);
    for (const auto& stage : c.stages) CHECK_EQ((int)stage.members.size(), 3);
}

// ===== Per-tag start beats ===========================================
//
// David: "I want the whole scene chasing with each section of tags
// being it's own 3 step and not necessarily starting on the left side.
// Randomly choose the start for each tag."
//
// Mechanically: a family's thirds still fire in order, but which chunk
// its FIRST third lands in varies per family. Every family still
// appears in every chunk.

namespace {

// Which chunk holds the given family's first light (i.e. the beat that
// family starts on). -1 if it isn't placed.
int StartChunkOf(const Chase& c, const PanelState& st, const std::string& fam)
{
    for (size_t s = 0; s < c.stages.size(); ++s) {
        for (const auto& ref : c.stages[s].members) {
            const LayerInfo* L = FindLayerByRef(st, ref);
            if (!L) continue;
            if (ExtractTagPrefix(L->display_name) != fam) continue;
            // The family's lights are named _000 upward; the first one
            // is the head of its first slice.
            if (L->display_name.size() >= 3 &&
                L->display_name.compare(L->display_name.size() - 3, 3, "000") == 0)
                return (int)s;
        }
    }
    return -1;
}

} // namespace

// The failure David reported: "It's reading as left to right in 3
// chunks." That happens when every family starts on chunk 1 — the
// whole scene sweeps together. At least one family must start
// somewhere else.
TEST(families_do_not_all_start_on_the_same_beat)
{
    auto st = MakeTaggedState({ {"Aa", 6}, {"Bb", 6}, {"Cc", 6},
                                {"Dd", 6}, {"Ee", 6}, {"Ff", 6} });
    const Chase c = MakeThreeStep(*st);

    std::map<int, int> starts;
    for (const char* fam : { "Aa", "Bb", "Cc", "Dd", "Ee", "Ff" }) {
        const int s = StartChunkOf(c, *st, fam);
        CHECK(s >= 0);
        starts[s]++;
    }
    // More than one distinct start beat across the six families.
    CHECK((int)starts.size() > 1);
    std::printf("    3 Step start beats: ");
    for (const auto& kv : starts) std::printf("[%d]x%d ", kv.first, kv.second);
    std::printf("\n");
}

// Rotating the start must not cost the property that got us here: a
// family is still spread across all three chunks.
TEST(rotating_the_start_keeps_every_family_in_every_chunk)
{
    auto st = MakeTaggedState({ {"Aa", 9}, {"Bb", 9}, {"Cc", 9},
                                {"Dd", 9} });
    const Chase c = MakeThreeStep(*st);
    CHECK_EQ((int)c.stages.size(), 3);
    for (const auto& stage : c.stages) {
        const auto fam = FamilyCounts(stage, *st);
        CHECK_EQ((int)fam.size(), 4);
        for (const auto& kv : fam) CHECK_EQ(kv.second, 3);
    }
}

// A different seed can move the start beats; the same seed cannot.
TEST(reseeding_can_move_the_start_beats)
{
    auto st = MakeTaggedState({ {"Aa", 4}, {"Bb", 4}, {"Cc", 4}, {"Dd", 4},
                                {"Ee", 4}, {"Ff", 4}, {"Gg", 4}, {"Hh", 4} });
    auto starts_for = [&](uint32_t seed) {
        Chase c;
        ApplyTemplateToChase(c, 3);
        c.random_seed = seed;
        RegenerateChaseStages(c, *st, c.desired_stage_count);
        std::string out;
        for (const char* f : { "Aa", "Bb", "Cc", "Dd",
                               "Ee", "Ff", "Gg", "Hh" }) {
            out += std::to_string(StartChunkOf(c, *st, f));
        }
        return out;
    };
    const std::string a = starts_for(1);
    CHECK_EQ(a, starts_for(1));           // same seed, same answer

    bool moved = false;
    for (uint32_t s = 2; s < 40 && !moved; ++s) {
        if (starts_for(s) != a) moved = true;
    }
    CHECK(moved);
}

// A single-light family has no thirds to divide — the start beat IS
// its chunk. Several of them must not all land on the same one.
TEST(single_light_families_are_placed_across_the_chunks)
{
    auto st = MakeTaggedState({ {"Big", 9},
                                {"Lone1", 1}, {"Lone2", 1}, {"Lone3", 1},
                                {"Lone4", 1}, {"Lone5", 1}, {"Lone6", 1} });
    const Chase c = MakeThreeStep(*st);
    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.size() != 3) return;

    std::map<int, int> where;
    for (const char* f : { "Lone1", "Lone2", "Lone3",
                           "Lone4", "Lone5", "Lone6" }) {
        const int s = StartChunkOf(c, *st, f);
        CHECK(s >= 0);
        where[s]++;
    }
    CHECK_EQ((int)where.size(), 3);         // all three chunks used
    for (const auto& kv : where) CHECK(kv.second <= 3);
}

// A bright family and a dim one must not stack on the same beat: the
// chunks are still balanced by light after the rotation is chosen.
TEST(start_beats_keep_the_chunks_evenly_lit)
{
    // Four families of 3. Two are ten times brighter than the others.
    std::vector<double> totals = { 10, 10, 10,   10, 10, 10,
                                    1,  1,  1,    1,  1,  1 };
    auto st = MakeTaggedState({ {"BrightA", 3}, {"BrightB", 3},
                                {"DimA", 3}, {"DimB", 3} }, &totals);
    const Chase c = MakeThreeStep(*st);
    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.size() != 3) return;

    double lo = 1e18, hi = 0.0;
    for (const auto& stage : c.stages) {
        const double L = StageLuminance(stage, *st);
        if (L < lo) lo = L;
        if (L > hi) hi = L;
    }
    // 66 units over 3 chunks divides exactly; each family divides by 3
    // exactly too, so a balanced answer is reachable and must be found.
    CHECK_NEAR(lo, 22.0, 1e-6);
    CHECK_NEAR(hi, 22.0, 1e-6);
}
