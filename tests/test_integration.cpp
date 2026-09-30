// End-to-end checks against the real saved sessions in testdata/:
// the Wall_Curtains reference EXR and the 138-light neon-hallway show.
//
// Those assets are gitignored (large renders, and the repo is public),
// so every test here SKIPS cleanly when they aren't on the machine
// rather than failing.

#include "test_harness.h"
#include "test_helpers.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include "chase_gen.h"
#include "panel_state.h"
#include "session_io.h"

#ifndef CM_TESTDATA_DIR
#define CM_TESTDATA_DIR ""
#endif

using namespace chase_gen;

namespace {

std::string TestDataPath(const char* rel)
{
    const std::string root = CM_TESTDATA_DIR;
    if (root.empty()) return {};
    return root + "/" + rel;
}

bool Exists(const std::string& p)
{
    if (p.empty()) return false;
    if (FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

// Loads a testdata session, or returns null after marking the test
// skipped.
std::unique_ptr<PanelState> LoadFixture(const char* rel)
{
    const std::string path = TestDataPath(rel);
    if (!Exists(path)) {
        th::Skip(std::string("missing ") + rel);
        return nullptr;
    }
    auto st = std::make_unique<PanelState>();
    if (!session_io::LoadSession(st.get(), path)) {
        th::Skip(std::string("could not load ") + rel);
        return nullptr;
    }
    // Sessions record absolute source paths, so on another machine
    // (the Mac) the session loads but none of its renders resolve.
    if (st->sources.empty() || !Exists(st->sources[0].path)) {
        th::Skip(std::string("source files not found for ") + rel);
        return nullptr;
    }
    return st;
}

int TotalLayers(const PanelState& st)
{
    int n = 0;
    for (const auto& s : st.sources) n += (int)s.layers.size();
    return n;
}

} // namespace

// ---- Wall_Curtains: a real multilayer EXR through the real scanner --

TEST(curtains_session_loads_and_scans_its_lightgroups)
{
    auto st = LoadFixture("curtains/session.chasemaker.json");
    if (!st) return;

    CHECK_EQ((int)st->sources.size(), 1);
    if (st->sources.empty()) return;
    // 51 lightgroups in the reference scene; allow for scanner-level
    // skips without pinning an exact count.
    CHECK(TotalLayers(*st) > 40);
    CHECK_EQ((int)st->chases.size(), 3);
    CHECK(st->sources[0].image_width > 0);
    CHECK(st->sources[0].image_height > 0);
}

// World / Image / Alpha / cryptomattes must never become chase lights.
TEST(curtains_environment_passes_are_skipped_not_staged)
{
    auto st = LoadFixture("curtains/session.chasemaker.json");
    if (!st) return;
    if (st->sources.empty()) return;

    for (const auto& L : st->sources[0].layers) {
        CHECK(L.display_name != "World");
        CHECK(L.display_name != "Image");
        CHECK(L.display_name != "Alpha");
        CHECK(L.display_name.find("Crypto") == std::string::npos);
    }
    CHECK(!st->sources[0].skipped.empty());
}

TEST(curtains_layers_have_usable_centroids)
{
    auto st = LoadFixture("curtains/session.chasemaker.json");
    if (!st) return;
    if (st->sources.empty()) return;

    int spread = 0;
    for (const auto& L : st->sources[0].layers) {
        CHECK(L.cx >= 0.f && L.cx <= 1.f);
        CHECK(L.cy >= 0.f && L.cy <= 1.f);
        CHECK(L.total > 0.0);
        if (L.cx < 0.4f || L.cx > 0.6f) ++spread;
    }
    // A wall of curtain lights must not all collapse to the middle.
    CHECK(spread > 10);
}

TEST(curtains_generates_a_pack_with_distinct_stage_orders)
{
    auto st = LoadFixture("curtains/session.chasemaker.json");
    if (!st) return;

    const int before = (int)st->chases.size();
    const int made = GenerateStandardPack(*st, {}, 30.f);
    CHECK_EQ(made, 5);
    CHECK_EQ((int)st->chases.size(), before + 5);

    const Chase* ltr = nullptr;
    const Chase* btt = nullptr;
    for (const auto& c : st->chases) {
        if (c.name == "Left to Right") ltr = &c;
        if (c.name == "Bottom to Top") btt = &c;
    }
    CHECK(ltr != nullptr);
    CHECK(btt != nullptr);
    if (!ltr || !btt) return;
    CHECK(!ltr->stages.empty());
    CHECK(!btt->stages.empty());
    // Different axes must not produce the same running order.
    CHECK(!(ltr->stages.front().members[0] == btt->stages.front().members[0]) ||
          !(ltr->stages.back().members[0] == btt->stages.back().members[0]));
}

// ---- Neon hallway: 138 single-light stills, the multi-source case ---

TEST(neon_session_loads_every_single_light_source)
{
    auto st = LoadFixture("neon/NeonHallwayComps_v1.chasemaker.json");
    if (!st) return;

    CHECK_EQ((int)st->sources.size(), 138);
    CHECK_EQ((int)st->chases.size(), 12);
    CHECK(st->tags.size() > 100);
    for (const auto& s : st->sources) {
        CHECK(s.is_movie);                 // AE-decoded single-light
        CHECK_EQ((int)s.layers.size(), 1);
    }
}

TEST(neon_pack_covers_all_138_lights)
{
    auto st = LoadFixture("neon/NeonHallwayComps_v1.chasemaker.json");
    if (!st) return;

    // The session already ships a hand-built "Left to Right", so the
    // generated chases have to be found by position, not by name.
    const int before = (int)st->chases.size();
    const int made = GenerateStandardPack(*st, {}, 30.f);
    CHECK_EQ(made, 5);
    if (made != 5) return;

    const Chase& sweep = st->chases[before];      // Left to Right
    int members = 0;
    for (const auto& s : sweep.stages) members += (int)s.members.size();
    CHECK_EQ(members, 138);
    CHECK_EQ((int)sweep.stages.size(), 138);      // one light per stage
}

// The workflow that cost twelve trips through the wizard: the same
// family again, scoped to one tag.
TEST(neon_per_tag_pack_scopes_to_that_tag)
{
    auto st = LoadFixture("neon/NeonHallwayComps_v1.chasemaker.json");
    if (!st) return;
    if (st->tags.empty()) return;

    const Tag& t = st->tags[0];
    // Not t.members.size(): this production session carries tag members
    // pointing at source_ids that no longer exist (sources removed and
    // re-added during the show mint new ids and nothing prunes the old
    // refs). Only the resolvable ones can drive a chase.
    const int tagged = CountEligibleLights(*st, { t.tag_id });
    CHECK(tagged >= 1);
    const int before = (int)st->chases.size();
    const int made = GenerateStandardPack(*st, { t.tag_id }, 30.f);
    CHECK_EQ(made, 5);
    if (made != 5) return;

    const Chase& c = st->chases[before];
    // The plain template name, whatever collision suffix it picked up —
    // the scope narrows the MEMBERSHIP, it does not rename the chase.
    CHECK(c.name.find("Left to Right") == 0);
    CHECK_EQ((int)c.tag_filter.size(), 1);
    int members = 0;
    for (const auto& s : c.stages) members += (int)s.members.size();
    CHECK_EQ(members, tagged);
}

TEST(neon_pack_timing_is_fitted_to_the_real_scene)
{
    auto st = LoadFixture("neon/NeonHallwayComps_v1.chasemaker.json");
    if (!st) return;

    const int before = (int)st->chases.size();
    const int made = GenerateStandardPack(*st, {}, 30.f);
    if (made != 5) return;

    for (int i = before; i < before + made; ++i) {
        const Chase& c = st->chases[i];
        // Whatever the scene, a generated chase must be playable.
        CHECK(c.timing.step_duration > 0.f);
        CHECK(c.timing.duration > c.timing.attack);
        if (c.desired_stage_count > 0) {
            // A chunked chase derives its hit so the chunks overlap;
            // it has no hit of its own to preserve.
            CHECK(c.timing.duration > c.timing.step_duration);
        } else {
            // Everything else keeps the artist's Hit preset — 138
            // lights must not change how long one light stays lit.
            CHECK_NEAR(c.timing.duration, 30.0, 1e-3);
            CHECK_NEAR(c.timing.attack, 5.0, 1e-3);
        }

        if (c.random_scatter) {
            CHECK(c.scatter_density > 0.f);
        } else if (c.desired_stage_count > 0) {
            // A chunked chase's length falls out of its hit, not out
            // of a clip-length target.
            const float total = (c.stages.size() - 1) * c.timing.step_duration
                                + c.timing.duration;
            CHECK(total > c.timing.duration);
        } else {
            // Stills, so the sweep is fitted to the 4-second default.
            const float total = (c.stages.size() - 1) * c.timing.step_duration
                                + c.timing.duration;
            CHECK_NEAR(total, 4.0 * 30.0, 2.0);
        }
    }
}

TEST(contact_sheet_keeps_up_with_the_transport)
{
    auto st = LoadFixture("curtains/session.chasemaker.json");
    if (!st) return;

    GenerateStandardPack(*st, {}, 30.f);

    std::vector<uint8_t> rgba;
    ContactSheetLayout lay;
    const int frames = 20;
    const auto t0 = std::chrono::steady_clock::now();
    for (int f = 0; f < frames; ++f) {
        lay = BuildContactSheet(st->chases, *st, testing::AllAt(f / (float)frames, (int)st->chases.size()), 30.f,
                                256, 3, rgba);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / frames;

    // Guard against measuring nothing: the sheet must actually have
    // composited cells with pixels in them.
    CHECK(lay.count > 0);
    CHECK(lay.sheet_w > 0);
    CHECK_EQ((int)rgba.size(), lay.sheet_w * lay.sheet_h * 4);
    bool any_lit = false;
    for (size_t i = 0; i < rgba.size() && !any_lit; i += 4) {
        if (rgba[i] > 8 || rgba[i + 1] > 8 || rgba[i + 2] > 8) any_lit = true;
    }
    CHECK(any_lit);

    std::printf("    contact sheet: %.1f ms/frame, %d cells, %d lights\n",
                ms, lay.count, TotalLayers(*st));
    CHECK(ms < 33.0);
}

// The neon session ships with 143 of 293 tag members pointing at
// source_ids that no longer exist. Loading should quietly clean those
// up — while leaving every live reference alone.
TEST(neon_session_load_prunes_its_stale_references)
{
    auto st = LoadFixture("neon/NeonHallwayComps_v1.chasemaker.json");
    if (!st) return;

    int members = 0;
    for (const auto& t : st->tags) members += (int)t.members.size();
    CHECK(members > 0);

    // Every surviving member must resolve to a source in the session.
    for (const auto& t : st->tags) {
        for (const auto& m : t.members) {
            CHECK(FindSourceById(*st, m.source_id) != nullptr);
        }
    }
    // Pruning again finds nothing left to do.
    CHECK_EQ(PruneOrphanedRefs(*st), 0);
    std::printf("    neon tags after load: %d members across %d tags\n",
                members, (int)st->tags.size());
}

// What a 3 Step actually looks like on the reference scene: which
// families autotag found, how the chunks divide, and which beat each
// family starts on. Prints rather than asserts a shape — the point is
// to be able to see the real answer without opening AE.
TEST(curtains_three_step_divides_its_families)
{
    auto st = LoadFixture("curtains/session.chasemaker.json");
    if (!st) return;

    Chase c;
    ApplyTemplateToChase(c, 3);
    RegenerateChaseStages(c, *st, c.desired_stage_count);
    CHECK_EQ((int)c.stages.size(), 3);
    if (c.stages.size() != 3) return;

    std::map<std::string, std::vector<int>> per_family;   // counts per chunk
    std::vector<double> lum(3, 0.0);
    int placed = 0;
    for (size_t s = 0; s < c.stages.size(); ++s) {
        for (const auto& ref : c.stages[s].members) {
            const LayerInfo* L = FindLayerByRef(*st, ref);
            if (!L) continue;
            std::string fam = ExtractTagPrefix(L->display_name);
            if (fam.empty()) fam = "(unique)";
            auto& v = per_family[fam];
            if (v.empty()) v.assign(3, 0);
            v[s]++;
            lum[s] += L->total;
            ++placed;
        }
    }
    std::printf("    curtains 3 Step: %d lights, %d families\n",
                placed, (int)per_family.size());
    for (const auto& kv : per_family) {
        std::printf("      %-20s %2d / %2d / %2d\n", kv.first.c_str(),
                    kv.second[0], kv.second[1], kv.second[2]);
    }
    const double lo = std::min(lum[0], std::min(lum[1], lum[2]));
    const double hi = std::max(lum[0], std::max(lum[1], lum[2]));
    std::printf("      chunk light: %.0f / %.0f / %.0f  (spread %.1f%%)\n",
                lum[0], lum[1], lum[2],
                (hi > 0.0) ? 100.0 * (hi - lo) / hi : 0.0);

    // Every family that has three or more lights must reach all three
    // chunks — that is the property the whole rule exists for.
    for (const auto& kv : per_family) {
        if (kv.first == "(unique)") continue;
        const int total = kv.second[0] + kv.second[1] + kv.second[2];
        if (total < 3) continue;
        CHECK(kv.second[0] > 0 && kv.second[1] > 0 && kv.second[2] > 0);
    }
}
