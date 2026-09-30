// Sorting a scene that is spread across many source files.
//
// Staging sorts the ACTIVE source's layers. That was fine when a scene
// was one multilayer EXR, but a show rendered one clip per light is 138
// sources of one layer each — and sorting a single-element vector 138
// times does nothing at all. Picking "Left to Right" left the list in
// the order the files happened to load.

#include "test_harness.h"
#include "test_helpers.h"

#include <string>
#include <vector>

#include "chase_gen.h"
#include "hash.h"
#include "panel_state.h"

using chase_gen::SortAllSources;

namespace {

// One source per light, added in a deliberately scrambled order, the
// way a folder of per-light renders arrives.
std::unique_ptr<PanelState> MakeOneSourcePerLight(const std::vector<float>& xs)
{
    auto st = std::make_unique<PanelState>();
    uint32_t id = 1;
    for (size_t i = 0; i < xs.size(); ++i) {
        Source src;
        src.source_id = id++;
        char name[32];
        std::snprintf(name, sizeof(name), "clip%02d", (int)i);
        src.path = std::string(name) + ".mov";
        src.is_movie = true;
        LayerInfo L;
        L.display_name = name;
        L.fnv1a_hash = FNV1a32(L.display_name);
        L.source_id = src.source_id;
        L.cx = L.cx_hot = L.peak_x = xs[i];
        L.cy = L.cy_hot = L.peak_y = 1.f - xs[i];
        L.total = 1.0;
        src.layers.push_back(L);
        st->sources.push_back(std::move(src));
    }
    st->next_source_id = id;
    return st;
}

// The order the unified Staging table would show, as x positions.
std::vector<float> DisplayedX(const PanelState& st)
{
    std::vector<float> out;
    for (const auto& src : st.sources) {
        for (const auto& L : src.layers) out.push_back(L.cx_hot);
    }
    return out;
}

bool Ascending(const std::vector<float>& v)
{
    for (size_t i = 1; i < v.size(); ++i) if (v[i] < v[i - 1]) return false;
    return true;
}

} // namespace

TEST(sorting_orders_lights_across_one_source_per_light)
{
    auto st = MakeOneSourcePerLight({ 0.7f, 0.1f, 0.9f, 0.3f, 0.5f });
    CHECK(!Ascending(DisplayedX(*st)));            // scrambled to start

    SortAllSources(*st, SortMode::HotspotX, false, 0);
    CHECK(Ascending(DisplayedX(*st)));
}

TEST(sorting_across_sources_honours_reverse)
{
    auto st = MakeOneSourcePerLight({ 0.7f, 0.1f, 0.9f, 0.3f });
    SortAllSources(*st, SortMode::HotspotX, true, 0);

    std::vector<float> xs = DisplayedX(*st);
    for (size_t i = 1; i < xs.size(); ++i) CHECK(xs[i] <= xs[i - 1]);
}

TEST(sorting_across_sources_works_on_the_other_axis)
{
    // cy is 1-cx in the fixture, so a Y sort is the mirror of the X one.
    auto st = MakeOneSourcePerLight({ 0.7f, 0.1f, 0.9f, 0.3f });
    SortAllSources(*st, SortMode::CentroidY, false, 0);

    std::vector<float> xs = DisplayedX(*st);
    for (size_t i = 1; i < xs.size(); ++i) CHECK(xs[i] <= xs[i - 1]);
}

TEST(sorting_still_orders_within_a_single_multilayer_source)
{
    auto st = testing::MakeState(6);
    // Scramble the layers inside the one source.
    std::swap(st->sources[0].layers[0], st->sources[0].layers[4]);
    std::swap(st->sources[0].layers[1], st->sources[0].layers[5]);
    CHECK(!Ascending(DisplayedX(*st)));

    SortAllSources(*st, SortMode::HotspotX, false, 0);
    CHECK(Ascending(DisplayedX(*st)));
    CHECK_EQ((int)st->sources.size(), 1);
}

// Sources are addressed by id everywhere; the active one is held as an
// INDEX, so reordering the vector has to carry it along.
TEST(sorting_keeps_the_active_source_selected)
{
    auto st = MakeOneSourcePerLight({ 0.7f, 0.1f, 0.9f, 0.3f, 0.5f });
    st->active_source_index = 2;                   // the light at x = 0.9
    const uint32_t was_active = st->sources[2].source_id;

    SortAllSources(*st, SortMode::HotspotX, false, 0);

    CHECK(st->active_source_index >= 0);
    CHECK(st->active_source_index < (int)st->sources.size());
    CHECK_EQ(st->sources[st->active_source_index].source_id, was_active);
}

TEST(sorting_preserves_every_source_and_layer)
{
    auto st = MakeOneSourcePerLight({ 0.7f, 0.1f, 0.9f, 0.3f, 0.5f });
    SortAllSources(*st, SortMode::HotspotX, false, 0);

    CHECK_EQ((int)st->sources.size(), 5);
    std::vector<uint32_t> ids;
    for (const auto& s : st->sources) {
        CHECK_EQ((int)s.layers.size(), 1);
        ids.push_back(s.source_id);
    }
    for (uint32_t want = 1; want <= 5; ++want) {
        bool found = false;
        for (uint32_t got : ids) if (got == want) found = true;
        CHECK(found);
    }
}

TEST(sorting_a_brightness_order_across_sources)
{
    auto st = MakeOneSourcePerLight({ 0.1f, 0.2f, 0.3f, 0.4f });
    st->sources[0].layers[0].total = 5.0;
    st->sources[1].layers[0].total = 90.0;
    st->sources[2].layers[0].total = 1.0;
    st->sources[3].layers[0].total = 40.0;

    SortAllSources(*st, SortMode::Brightness, false, 0);   // brightest first

    std::vector<double> totals;
    for (const auto& s : st->sources) totals.push_back(s.layers[0].total);
    for (size_t i = 1; i < totals.size(); ++i) CHECK(totals[i] <= totals[i - 1]);
}

TEST(sorting_an_empty_session_is_a_noop)
{
    auto st = std::make_unique<PanelState>();
    SortAllSources(*st, SortMode::HotspotX, false, 0);
    CHECK(st->sources.empty());
}

TEST(sorting_tolerates_a_source_with_no_layers)
{
    auto st = MakeOneSourcePerLight({ 0.6f, 0.2f });
    Source empty;
    empty.source_id = 99;
    empty.path = "unreadable.exr";
    st->sources.push_back(empty);

    SortAllSources(*st, SortMode::HotspotX, false, 0);
    CHECK_EQ((int)st->sources.size(), 3);
    // A source with nothing in it sorts to the end rather than
    // interleaving with real lights.
    CHECK_EQ(st->sources.back().source_id, 99u);
}

// Two lights at exactly the same position must not make the comparator
// contradict itself — an ordering that answers "before" both ways is
// undefined behaviour inside std::sort.
TEST(sorting_is_well_defined_when_lights_share_a_position)
{
    auto st = MakeOneSourcePerLight({ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f });
    SortAllSources(*st, SortMode::HotspotX, false, 0);
    CHECK_EQ((int)st->sources.size(), 6);

    // Deterministic: the same input sorts the same way twice.
    auto again = MakeOneSourcePerLight({ 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f });
    SortAllSources(*again, SortMode::HotspotX, false, 0);
    for (size_t i = 0; i < st->sources.size(); ++i) {
        CHECK_EQ(st->sources[i].source_id, again->sources[i].source_id);
    }
}

// The cross-source ordering has to agree with the within-source one, or
// a mixed scene would read inconsistently.
TEST(cross_source_order_agrees_with_within_source_order)
{
    const std::vector<float> xs = { 0.8f, 0.1f, 0.55f, 0.3f, 0.95f, 0.42f };

    // Same lights, once as one source and once as one source per light.
    auto many = MakeOneSourcePerLight(xs);
    auto one = std::make_unique<PanelState>();
    {
        Source src;
        src.source_id = 1;
        for (size_t i = 0; i < xs.size(); ++i) {
            char name[32];
            std::snprintf(name, sizeof(name), "clip%02d", (int)i);
            LayerInfo L;
            L.display_name = name;
            L.fnv1a_hash = FNV1a32(L.display_name);
            L.source_id = 1;
            L.cx = L.cx_hot = L.peak_x = xs[i];
            L.cy = L.cy_hot = L.peak_y = 1.f - xs[i];
            L.total = 1.0;
            src.layers.push_back(L);
        }
        one->sources.push_back(std::move(src));
    }

    SortAllSources(*many, SortMode::HotspotX, false, 0);
    SortAllSources(*one, SortMode::HotspotX, false, 0);

    const std::vector<float> a = DisplayedX(*many);
    const std::vector<float> b = DisplayedX(*one);
    CHECK_EQ((int)a.size(), (int)b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) {
        CHECK_NEAR(a[i], b[i], 1e-6);
    }
}
