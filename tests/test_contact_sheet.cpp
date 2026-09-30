// The contact sheet: every generated chase previewing at once.
//
// Built as ONE tiled buffer rather than one texture per cell, so it
// rides the single composite texture both renderers already manage.
// That keeps the whole feature out of the panel-lifecycle code that
// caused the AE crash saga — worth protecting with tests, because the
// tiling is where a mistake would show up as cells drawing each
// other's contents.

#include "test_harness.h"
#include "test_helpers.h"

#include <cmath>
#include <vector>

#include "chase_gen.h"

using namespace chase_gen;
using testing::GiveThumbnails;
using testing::MakeState;
using testing::RectChannelMean;

namespace {

// A session of `n` lights with thumbnails, looping, plus `count`
// one-light chases: chase k fires only light k, always on.
std::unique_ptr<PanelState> MakeSheetState(int n, int count)
{
    auto st = MakeState(n);
    st->sources[0].frame_count = 100;
    st->sources[0].animation   = true;
    GiveThumbnails(*st);

    for (int k = 0; k < count; ++k) {
        Chase c;
        c.chase_id = (uint32_t)(k + 1);
        c.name = "chase";
        c.manual_stages = true;            // hand-built below
        ChaseStage stage;
        stage.members.push_back({ st->sources[0].source_id,
                                  st->sources[0].layers[k].fnv1a_hash });
        c.stages.push_back(stage);
        // Hold every light fully on for the whole loop so a cell's
        // contents don't depend on the playhead.
        c.timing.opacity_peak  = 100.f;
        c.timing.opacity_floor = 100.f;
        c.timing.gamma_peak     = 1.f;
        c.timing.gamma_baseline = 1.f;
        st->chases.push_back(c);
    }
    return st;
}

} // namespace

TEST(contact_sheet_lays_out_a_grid_that_fits_every_chase)
{
    auto st = MakeSheetState(6, 5);
    std::vector<uint8_t> rgba;
    const ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 32, 3, rgba);

    CHECK_EQ(lay.count, 5);
    CHECK_EQ(lay.cols, 3);
    CHECK_EQ(lay.rows, 2);                       // 5 cells over 3 columns
    CHECK_EQ(lay.cell_w, 32);
    CHECK(lay.cell_h > 0);
    CHECK_EQ(lay.sheet_w, lay.cols * lay.cell_w);
    CHECK_EQ(lay.sheet_h, lay.rows * lay.cell_h);
    CHECK_EQ((int)rgba.size(), lay.sheet_w * lay.sheet_h * 4);
}

TEST(contact_sheet_keeps_the_source_aspect_in_each_cell)
{
    auto st = MakeSheetState(3, 2);
    // Thumbnails are 8x6, so a 40px cell should be 30px tall.
    std::vector<uint8_t> rgba;
    const ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 40, 2, rgba);
    CHECK_EQ(lay.cell_w, 40);
    CHECK_EQ(lay.cell_h, 30);
}

// The mistake worth guarding: a cell drawing another cell's chase.
TEST(each_cell_holds_its_own_chase)
{
    auto st = MakeSheetState(3, 3);      // light 0 red, 1 green, 2 blue
    std::vector<uint8_t> rgba;
    const ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 3, rgba);
    CHECK_EQ(lay.count, 3);
    if (lay.count != 3) return;

    for (int k = 0; k < 3; ++k) {
        const int x0 = (k % lay.cols) * lay.cell_w;
        const int y0 = (k / lay.cols) * lay.cell_h;
        for (int ch = 0; ch < 3; ++ch) {
            const double mean = RectChannelMean(rgba, lay.sheet_w, x0, y0,
                                                lay.cell_w, lay.cell_h, ch);
            if (ch == k) CHECK(mean > 200.0);    // its own light is lit
            else         CHECK(mean < 20.0);     // and nothing else is
        }
    }
}

// Fewer chases than columns just narrows the grid — no empty gap.
TEST(the_grid_narrows_rather_than_leaving_a_gap)
{
    auto st = MakeSheetState(3, 2);
    std::vector<uint8_t> rgba;
    const ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 3, rgba);

    CHECK_EQ(lay.count, 2);
    CHECK_EQ(lay.cols, 2);
    CHECK_EQ(lay.rows, 1);
}

// A ragged last row is the real case: 5 chases over 3 columns leaves
// one cell with nothing behind it, and it must stay black rather than
// keeping whatever was in the buffer.
TEST(a_ragged_last_row_leaves_black_cells)
{
    auto st = MakeSheetState(6, 5);
    std::vector<uint8_t> rgba;
    const ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 3, rgba);

    CHECK_EQ(lay.count, 5);
    CHECK_EQ(lay.cols, 3);
    CHECK_EQ(lay.rows, 2);
    // Cell index 5 (row 1, column 2) has no chase.
    for (int ch = 0; ch < 3; ++ch) {
        const double mean = RectChannelMean(rgba, lay.sheet_w,
                                            2 * lay.cell_w, lay.cell_h,
                                            lay.cell_w, lay.cell_h, ch);
        CHECK_NEAR(mean, 0.0, 1e-9);
    }
}

TEST(contact_sheet_is_empty_without_chases)
{
    auto st = MakeSheetState(3, 0);
    std::vector<uint8_t> rgba;
    const ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 32, 3, rgba);
    CHECK_EQ(lay.count, 0);
    CHECK_EQ(lay.sheet_w, 0);
    CHECK(rgba.empty());
}

TEST(contact_sheet_columns_are_clamped_to_something_sane)
{
    auto st = MakeSheetState(4, 4);
    std::vector<uint8_t> rgba;
    // Nonsense column counts must not produce a zero-sized or
    // negative-dimension buffer.
    const ContactSheetLayout a =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 0, rgba);
    CHECK(a.cols >= 1);
    CHECK_EQ((int)rgba.size(), a.sheet_w * a.sheet_h * 4);

    const ContactSheetLayout b =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 99, rgba);
    CHECK(b.cols >= 1);
    CHECK_EQ(b.rows, 1);
    CHECK_EQ((int)rgba.size(), b.sheet_w * b.sheet_h * 4);
}

// One REAL timeline drives every cell: each chase is sampled at t
// modulo its own length. Chases of different durations must not be
// stretched to a common cycle — that would hide the very differences
// the grid exists to compare.
TEST(the_shared_transport_runs_on_one_real_timeline)
{
    auto st = MakeState(4);
    st->sources[0].frame_count = 100;
    st->sources[0].animation   = true;
    GiveThumbnails(*st);

    // Two sweeps: one pattern per loop, and one that runs twice.
    for (int k = 0; k < 2; ++k) {
        Chase c;
        c.chase_id = (uint32_t)(k + 1);
        c.name = "sweep";
        c.sort_mode = SortMode::HotspotX;
        c.loop_cycles = (k == 0) ? 1 : 2;
        c.timing.duration = 40.f;
        c.timing.attack = 6.f;
        RegenerateChaseStages(c, *st, 0);
        st->chases.push_back(c);
    }

    std::vector<uint8_t> a, b;
    BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 2, a);
    BuildContactSheet(st->chases, *st, testing::AllAt(12.f, (int)st->chases.size()), 30.f, 16, 2, b);
    CHECK_EQ((int)a.size(), (int)b.size());
    CHECK(a != b);                       // the transport actually moves

    // A chase repeats after ITS OWN length, not after a common cycle.
    std::vector<uint8_t> c;
    const float own = ChaseTotalFrames(st->chases[0], *st, 30.f);
    BuildContactSheet(st->chases, *st, testing::AllAt(own, (int)st->chases.size()), 30.f, 16, 2, c);
    CHECK(a == c);
}

// A short chase and a long one must not be forced into step: at a time
// that is a whole number of short-chase cycles, the short cell repeats
// while the long one has moved on.
TEST(chases_of_different_lengths_run_at_their_own_speeds)
{
    auto st = MakeState(3);
    GiveThumbnails(*st);                 // stills: no loop

    Chase quick;                         // ~30 frames
    quick.chase_id = 1;
    quick.manual_stages = true;
    ChaseStage s1;
    s1.members.push_back({ st->sources[0].source_id,
                           st->sources[0].layers[0].fnv1a_hash });
    quick.stages.push_back(s1);
    quick.timing.duration = 30.f;
    quick.timing.attack = 5.f;
    quick.timing.step_duration = 30.f;

    Chase slow = quick;                  // ~300 frames
    slow.chase_id = 2;
    slow.stages.clear();
    ChaseStage s2;
    s2.members.push_back({ st->sources[0].source_id,
                           st->sources[0].layers[1].fnv1a_hash });
    slow.stages.push_back(s2);
    slow.timing.duration = 300.f;
    slow.timing.attack = 50.f;
    slow.timing.step_duration = 300.f;

    st->chases.push_back(quick);
    st->chases.push_back(slow);

    const float q = ChaseTotalFrames(st->chases[0], *st, 30.f);
    const float sl = ChaseTotalFrames(st->chases[1], *st, 30.f);
    CHECK(sl > q * 5.f);                 // genuinely different lengths

    std::vector<uint8_t> at0, atq;
    ContactSheetLayout lay =
        BuildContactSheet(st->chases, *st, testing::AllAt(0.f, (int)st->chases.size()), 30.f, 16, 2, at0);
    BuildContactSheet(st->chases, *st, testing::AllAt(q, (int)st->chases.size()), 30.f, 16, 2, atq);

    // After exactly one short cycle: cell 0 is back where it started,
    // cell 1 is not.
    const double q0_a = RectChannelMean(at0, lay.sheet_w, 0, 0,
                                        lay.cell_w, lay.cell_h, 0);
    const double q0_b = RectChannelMean(atq, lay.sheet_w, 0, 0,
                                        lay.cell_w, lay.cell_h, 0);
    const double s1_a = RectChannelMean(at0, lay.sheet_w, lay.cell_w, 0,
                                        lay.cell_w, lay.cell_h, 1);
    const double s1_b = RectChannelMean(atq, lay.sheet_w, lay.cell_w, 0,
                                        lay.cell_w, lay.cell_h, 1);
    CHECK_NEAR(q0_a, q0_b, 1.0);         // short chase looped back
    CHECK(std::fabs(s1_a - s1_b) > 1.0); // long chase kept going
}

TEST(longest_chase_frames_spans_the_whole_set)
{
    auto st = MakeSheetState(3, 0);
    Chase a; a.chase_id = 1; a.manual_stages = true;
    a.timing.duration = 20.f; a.timing.step_duration = 20.f;
    ChaseStage sa;
    sa.members.push_back({ st->sources[0].source_id,
                           st->sources[0].layers[0].fnv1a_hash });
    a.stages.push_back(sa);

    Chase b = a; b.chase_id = 2; b.timing.duration = 90.f;

    st->chases.push_back(a);
    st->chases.push_back(b);
    const float longest = LongestChaseFrames(st->chases, *st, 30.f);
    CHECK_NEAR(longest, ChaseTotalFrames(st->chases[1], *st, 30.f), 1e-3);
    CHECK(longest >= ChaseTotalFrames(st->chases[0], *st, 30.f));
}

// The sheet reports one cell per chase it was GIVEN — nothing about
// the list a caller happens to be iterating alongside it.
//
// That distinction crashed AE (David, 2026-09-09): the toolbar's
// "Generate chases" draws before the Review tab, so a click appended to
// the live chase list mid-frame while the tab still held the frame's
// snapshot. The sheet was built from the live list, so its cell count
// ran past the snapshot the cell loop was indexing. The tab now clamps;
// this pins the contract that made clamping necessary.
TEST(the_sheet_reports_a_cell_for_every_chase_it_was_given)
{
    auto st = testing::MakeState(6);
    testing::GiveThumbnails(*st, 8, 8);

    std::vector<Chase> chases;
    for (int n = 1; n <= 7; ++n) {
        Chase c;
        c.chase_id = (uint32_t)n;
        chase_gen::ApplyTemplateToChase(c, 0);
        chase_gen::RegenerateChaseStages(c, *st, 0);
        chases.push_back(std::move(c));

        std::vector<uint8_t> rgba;
        const ContactSheetLayout lay = chase_gen::BuildContactSheet(
            chases, *st, testing::AllAt(0.f, (int)chases.size()), 30.f,
            32, 3, rgba);
        CHECK_EQ(lay.count, (int)chases.size());
        // And the grid really does have room for every one of them.
        CHECK(lay.cols * lay.rows >= lay.count);
    }
}
