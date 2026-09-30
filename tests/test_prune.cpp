// Orphaned references.
//
// The neon production session carries 143 tag members (of 293) pointing
// at source_ids that no longer exist: sources removed and re-added
// during the show mint fresh ids, and nothing ever cleaned up the old
// refs. They're inert, but they inflate every count the UI shows and a
// tag whose members are ALL orphaned looks usable and isn't.
//
// The dangerous mistake here would be over-pruning: if an EXR moves on
// disk and its scan comes back empty, deleting every reference into it
// would destroy the user's tags for good. So only a reference to a
// source that is not in the session at all may be removed.

#include "test_harness.h"
#include "test_helpers.h"

#include "chase_gen.h"
#include "panel_state.h"

using chase_gen::PruneOrphanedRefs;
using testing::MakeState;

TEST(pruning_drops_refs_whose_source_is_gone)
{
    auto st = MakeState(4);
    const uint32_t sid = st->sources[0].source_id;

    Tag t;
    t.tag_id = 1;
    t.name = "floor";
    t.members.push_back({ sid, st->sources[0].layers[0].fnv1a_hash });
    t.members.push_back({ 999, st->sources[0].layers[1].fnv1a_hash });  // dead
    t.members.push_back({ sid, st->sources[0].layers[2].fnv1a_hash });
    st->tags.push_back(t);

    const int removed = PruneOrphanedRefs(*st);
    CHECK_EQ(removed, 1);
    CHECK_EQ((int)st->tags.size(), 1);
    CHECK_EQ((int)st->tags[0].members.size(), 2);
    for (const auto& m : st->tags[0].members) CHECK_EQ(m.source_id, sid);
}

// The safety property: a source that is present but scanned empty (a
// moved or unreadable file) keeps every reference into it.
TEST(pruning_spares_a_source_that_merely_failed_to_scan)
{
    auto st = MakeState(3);
    const uint32_t sid = st->sources[0].source_id;

    Tag t;
    t.tag_id = 1;
    t.name = "wall";
    for (int i = 0; i < 3; ++i) {
        t.members.push_back({ sid, st->sources[0].layers[i].fnv1a_hash });
    }
    st->tags.push_back(t);

    st->sources[0].layers.clear();          // scan came back empty
    const int removed = PruneOrphanedRefs(*st);

    CHECK_EQ(removed, 0);
    CHECK_EQ((int)st->tags[0].members.size(), 3);
}

TEST(pruning_cleans_binds_and_chases_too)
{
    auto st = MakeState(4);
    const uint32_t sid = st->sources[0].source_id;

    Bind b;
    b.bind_id = 1;
    b.name = "joined";
    b.members.push_back({ sid, st->sources[0].layers[0].fnv1a_hash });
    b.members.push_back({ 777, 12345u });                            // dead
    st->binds.push_back(b);

    Chase c;
    c.chase_id = 1;
    c.manual_stages = true;
    ChaseStage s1;
    s1.members.push_back({ sid, st->sources[0].layers[1].fnv1a_hash });
    s1.members.push_back({ 888, 4242u });                            // dead
    c.stages.push_back(s1);
    c.scatter.push_back({ { sid, st->sources[0].layers[2].fnv1a_hash }, 4.f });
    c.scatter.push_back({ { 888, 4242u }, 9.f });                    // dead
    st->chases.push_back(c);

    const int removed = PruneOrphanedRefs(*st);
    CHECK_EQ(removed, 3);
    CHECK_EQ((int)st->binds[0].members.size(), 1);
    CHECK_EQ((int)st->chases[0].stages[0].members.size(), 1);
    CHECK_EQ((int)st->chases[0].scatter.size(), 1);
}

// A stage left with no members would fire nothing but still take a slot
// in the running order, stretching the chase with a silent gap.
TEST(pruning_removes_stages_it_empties)
{
    auto st = MakeState(3);
    const uint32_t sid = st->sources[0].source_id;

    Chase c;
    c.chase_id = 1;
    c.manual_stages = true;
    ChaseStage live;
    live.members.push_back({ sid, st->sources[0].layers[0].fnv1a_hash });
    ChaseStage dead;
    dead.members.push_back({ 999, 1u });
    c.stages.push_back(live);
    c.stages.push_back(dead);
    st->chases.push_back(c);

    PruneOrphanedRefs(*st);
    CHECK_EQ((int)st->chases[0].stages.size(), 1);
    CHECK_EQ((int)st->chases[0].stages[0].members.size(), 1);
}

// An emptied tag is kept, not deleted: it is the user's named group,
// and it costs nothing to leave in place (the pack popup already greys
// out a scope with no live lights).
TEST(pruning_keeps_a_tag_it_empties)
{
    auto st = MakeState(2);
    Tag t;
    t.tag_id = 1;
    t.name = "all gone";
    t.members.push_back({ 999, 1u });
    t.members.push_back({ 999, 2u });
    st->tags.push_back(t);

    const int removed = PruneOrphanedRefs(*st);
    CHECK_EQ(removed, 2);
    CHECK_EQ((int)st->tags.size(), 1);
    CHECK(st->tags[0].members.empty());
    CHECK_EQ(chase_gen::CountEligibleLights(*st, { 1u }), 0);
}

TEST(pruning_a_clean_session_changes_nothing)
{
    auto st = MakeState(5);
    const uint32_t sid = st->sources[0].source_id;
    Tag t;
    t.tag_id = 1;
    t.name = "fine";
    for (int i = 0; i < 5; ++i) {
        t.members.push_back({ sid, st->sources[0].layers[i].fnv1a_hash });
    }
    st->tags.push_back(t);

    CHECK_EQ(PruneOrphanedRefs(*st), 0);
    CHECK_EQ((int)st->tags[0].members.size(), 5);
}

TEST(pruning_an_empty_session_is_a_noop)
{
    auto st = std::make_unique<PanelState>();
    CHECK_EQ(PruneOrphanedRefs(*st), 0);
}

// The load-path hazard, stated as a test: a source whose file has moved
// produces NO Source at all (the scan throws and only records an
// error), so "not in state.sources" cannot mean "not part of this
// project". Loading passes the ids the session file listed, and a
// source that merely failed to load keeps its references.
TEST(a_source_that_failed_to_load_keeps_its_references)
{
    auto st = MakeState(3);
    const uint32_t present = st->sources[0].source_id;
    const uint32_t failed_to_load = 42;      // in the file, never scanned

    Tag t;
    t.tag_id = 1;
    t.name = "spans both";
    t.members.push_back({ present, st->sources[0].layers[0].fnv1a_hash });
    t.members.push_back({ failed_to_load, 111u });
    t.members.push_back({ 999, 222u });      // genuinely not in the project
    st->tags.push_back(t);

    // The session file listed both sources, so both are live.
    const int removed = PruneOrphanedRefs(*st, { present, failed_to_load });
    CHECK_EQ(removed, 1);
    CHECK_EQ((int)st->tags[0].members.size(), 2);

    bool kept_failed = false;
    for (const auto& m : st->tags[0].members) {
        if (m.source_id == failed_to_load) kept_failed = true;
    }
    CHECK(kept_failed);
}

// ...whereas an explicit cleanup, with the user looking at the session,
// judges by what is actually loaded.
TEST(explicit_cleanup_judges_by_what_is_loaded)
{
    auto st = MakeState(2);
    Tag t;
    t.tag_id = 1;
    t.name = "mixed";
    t.members.push_back({ st->sources[0].source_id,
                          st->sources[0].layers[0].fnv1a_hash });
    t.members.push_back({ 42, 111u });
    st->tags.push_back(t);

    CHECK_EQ(PruneOrphanedRefs(*st), 1);
    CHECK_EQ((int)st->tags[0].members.size(), 1);
}
