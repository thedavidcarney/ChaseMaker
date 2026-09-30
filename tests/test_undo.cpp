// Undo / redo over the panel's auto-snapshot loop.
//
// The panel records an undo step at the end of any frame where the
// authoring state differs from the last stable snapshot (panel_ui.cpp,
// RenderFrame). Chase editors also regenerate derived caches (stages,
// scatter) every frame. These tests drive that same loop headlessly so
// multi-step undo is checked without AE.

#include "test_harness.h"
#include "test_helpers.h"

#include "chase_gen.h"

using namespace chase_gen;

namespace {

// One panel frame: the editor's per-frame regen, then the end-of-frame
// auto-snapshot exactly as RenderFrame does it.
void Frame(PanelState& st)
{
    const float fps = 30.f;
    if (st.active_chase_index >= 0 &&
        st.active_chase_index < (int)st.chases.size())
    {
        Chase& c = st.chases[st.active_chase_index];
        if (c.random_scatter) RegenerateScatter(c, st, fps);
        else RegenerateChaseStages(c, st, c.desired_stage_count);
    }
    UndoSnapshot cur = CaptureUndoSnapshot(st);
    if (st.has_last_stable && !(st.last_stable == cur)) {
        st.undo_stack.push_back(st.last_stable);
        st.redo_stack.clear();
    }
    st.last_stable = std::move(cur);
    st.has_last_stable = true;
}

std::unique_ptr<PanelState> SessionWithChases()
{
    auto st = testing::MakeState(16);
    Chase sweep;
    sweep.chase_id = 1;
    sweep.name = "Sweep";
    sweep.desired_stage_count = 16;
    st->chases.push_back(sweep);

    Chase rnd;
    rnd.chase_id = 2;
    rnd.name = "Random";
    rnd.random_scatter = true;
    rnd.random_seed = 42u;
    rnd.scatter_density = 2.f;
    st->chases.push_back(rnd);
    st->next_chase_id = 3;
    st->active_tab = PanelTab::Chase;
    st->active_chase_index = 1;
    Frame(*st);
    Frame(*st);
    return st;
}

} // namespace

TEST(undo_walks_back_several_edits_on_a_random_chase)
{
    auto st = SessionWithChases();
    const float densities[] = { 3.f, 4.f, 5.f };
    for (float d : densities) {
        st->chases[1].scatter_density = d;
        Frame(*st);
        Frame(*st);                         // idle frames change nothing
    }
    CHECK_EQ((int)st->undo_stack.size(), 3);

    const float expect[] = { 4.f, 3.f, 2.f };
    for (float e : expect) {
        st->session_dirty = false;
        CHECK(PerformUndo(*st));
        // The .aep copy of the session must learn about the undo.
        CHECK(st->session_dirty.load());
        Frame(*st);
        Frame(*st);
        CHECK_NEAR(st->chases[1].scatter_density, e, 1e-6);
    }
    // And forward again.
    for (float e : densities) {
        CHECK(PerformRedo(*st));
        Frame(*st);
        CHECK_NEAR(st->chases[1].scatter_density, e, 1e-6);
    }
}

// Moving between tabs and chases is not an edit: it must not use up a
// Ctrl+Z. Undo still returns you to where the edit was made.
TEST(switching_tabs_is_not_an_undo_step)
{
    auto st = SessionWithChases();
    const size_t base = st->undo_stack.size();

    st->chases[1].scatter_density = 4.f;       // an edit on Random
    Frame(*st);
    st->active_tab = PanelTab::Review;         // then just look around
    Frame(*st);
    st->active_tab = PanelTab::Chase;
    st->active_chase_index = 0;
    Frame(*st);
    st->active_tab = PanelTab::Sources;
    Frame(*st);
    CHECK_EQ((int)(st->undo_stack.size() - base), 1);

    CHECK(PerformUndo(*st));
    Frame(*st);
    CHECK_NEAR(st->chases[1].scatter_density, 2.f, 1e-6);
    CHECK(st->active_tab == PanelTab::Chase);
    CHECK_EQ(st->active_chase_index, 1);
}

// The stuck-Ctrl+Z loop: a Staging edit leaves a chase's stage cache
// stale; opening the chase regenerates it. That regen must not be an
// undo step, and undoing must not be undone by the next frame's regen
// (which used to push a fresh step and wipe redo).
TEST(undo_is_not_stuck_behind_a_regenerated_cache)
{
    auto st = SessionWithChases();
    st->active_chase_index = 0;
    Frame(*st);
    const size_t base = st->undo_stack.size();

    st->active_tab = PanelTab::Staging;
    st->sources[0].layers[3].included = false;   // edit 1
    Frame(*st);
    st->sources[0].layers[5].included = false;   // edit 2
    Frame(*st);
    st->active_tab = PanelTab::Chase;            // open the chase: regen
    Frame(*st);
    Frame(*st);
    CHECK_EQ((int)(st->undo_stack.size() - base), 2);

    CHECK(PerformUndo(*st));
    Frame(*st);
    Frame(*st);
    CHECK(st->sources[0].layers[5].included);
    CHECK(!st->sources[0].layers[3].included);
    CHECK_EQ((int)st->redo_stack.size(), 1);     // not wiped by the regen

    CHECK(PerformUndo(*st));
    Frame(*st);
    CHECK(st->sources[0].layers[3].included);
    CHECK_EQ((int)st->redo_stack.size(), 2);
}

TEST(undo_walks_back_several_edits_on_a_sweep)
{
    auto st = SessionWithChases();
    st->active_chase_index = 0;
    Frame(*st);
    const size_t base = st->undo_stack.size();
    const float before = st->chases[0].timing.step_duration;
    const float steps[] = { 2.f, 4.f, 8.f };
    for (float step : steps) {
        st->chases[0].timing.step_duration = step;
        Frame(*st);
    }
    CHECK_EQ((int)(st->undo_stack.size() - base), 3);
    const float expect[] = { 4.f, 2.f, before };
    for (float e : expect) {
        CHECK(PerformUndo(*st));
        Frame(*st);
        CHECK_NEAR(st->chases[0].timing.step_duration, e, 1e-6);
    }
}
