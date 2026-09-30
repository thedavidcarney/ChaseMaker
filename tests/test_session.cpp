// Layer-name hashing (the EXRDemux contract) and session round-trips.

#include "test_harness.h"
#include "test_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#include "chase_gen.h"
#include "hash.h"
#include "panel_state.h"
#include "session_io.h"

// ---- FNV-1a: must match EXRDemux byte-for-byte ----------------------
//
// Expected values come from an independent implementation, not from
// this code. A silent drift here points every built layer at the wrong
// pass, so these are pinned rather than derived.

TEST(fnv1a32_matches_the_reference_values)
{
    CHECK_EQ(FNV1a32(""),                  2166136261u);
    CHECK_EQ(FNV1a32("a"),                 3826002220u);
    CHECK_EQ(FNV1a32("World"),             3714116915u);
    CHECK_EQ(FNV1a32("Curtain_001"),       2353201161u);
    CHECK_EQ(FNV1a32("Swag Highlight 12"),  536255450u);
}

TEST(fnv1a32_splits_into_the_hi_lo_params_exrdemux_reads)
{
    const uint32_t h = FNV1a32("Curtain_001");
    CHECK_EQ((h >> 16) & 0xFFFFu, 35907u);
    CHECK_EQ(h & 0xFFFFu, 9u);
}

TEST(fnv1a32_is_case_and_space_sensitive)
{
    CHECK(FNV1a32("Curtain_001") != FNV1a32("curtain_001"));
    CHECK(FNV1a32("Swag Highlight 12") != FNV1a32("SwagHighlight 12"));
}

// ---- Session round-trip ---------------------------------------------

namespace {

std::string TempSessionPath()
{
    const char* tmp = std::getenv("TEMP");
    if (!tmp || !*tmp) tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = ".";
    return std::string(tmp) + "/cm_test_session.chasemaker.json";
}

// Single-light still sources load synchronously (no scan worker), so a
// round-trip stays deterministic without touching real render files.
std::unique_ptr<PanelState> MakeStillState(int n)
{
    auto st = testing::MakeState(n);
    st->sources[0].path      = "C:/nonexistent/cm_test_light.png";
    st->sources[0].scan_path = st->sources[0].path;
    st->sources[0].is_movie  = true;
    return st;
}

} // namespace

TEST(session_round_trips_chase_authoring)
{
    auto st = MakeStillState(6);
    st->project_fps = 30.f;
    st->project_fps_user = true;
    st->dedupe_by_name = true;

    Chase c;
    c.chase_id = 1;
    c.name = "Bottom to Top";
    c.sort_mode = SortMode::CentroidY;
    c.sort_reverse = true;
    c.desired_stage_count = 3;
    c.loop_multiple = 2;
    c.loop_cycles = 4;
    c.loop_offset = 7;
    c.timing.duration = 22.f;
    c.timing.attack = 3.f;
    c.timing.hold = 5.f;
    c.timing.step_duration = 9.f;
    c.timing.opacity_floor = 12.f;
    st->chases.push_back(c);
    st->next_chase_id = 2;

    const std::string path = TempSessionPath();
    CHECK(session_io::WriteSession(st.get(), path));

    auto loaded = std::make_unique<PanelState>();
    CHECK(session_io::LoadSession(loaded.get(), path));

    CHECK_EQ((int)loaded->chases.size(), 1);
    if (loaded->chases.empty()) return;
    const Chase& g = loaded->chases[0];
    CHECK_EQ(g.name, std::string("Bottom to Top"));
    CHECK_EQ((int)g.sort_mode, (int)SortMode::CentroidY);
    CHECK(g.sort_reverse);
    CHECK_EQ(g.desired_stage_count, 3);
    CHECK_EQ(g.loop_multiple, 2);
    CHECK_EQ(g.loop_cycles, 4);
    CHECK_EQ(g.loop_offset, 7);
    CHECK_NEAR(g.timing.duration, 22.0, 1e-3);
    CHECK_NEAR(g.timing.attack, 3.0, 1e-3);
    CHECK_NEAR(g.timing.hold, 5.0, 1e-3);
    CHECK_NEAR(g.timing.step_duration, 9.0, 1e-3);
    CHECK_NEAR(g.timing.opacity_floor, 12.0, 1e-3);
    CHECK_NEAR(loaded->project_fps.load(), 30.0, 1e-3);
    CHECK(loaded->dedupe_by_name);

    std::remove(path.c_str());
}

TEST(session_round_trips_scatter_settings)
{
    auto st = MakeStillState(4);
    Chase c;
    c.chase_id = 1;
    c.name = "Random";
    c.random_scatter = true;
    c.random_seed = 424242u;
    c.scatter_density = 9;
    c.loop_seconds = 12.5f;
    st->chases.push_back(c);

    const std::string path = TempSessionPath();
    CHECK(session_io::WriteSession(st.get(), path));

    auto loaded = std::make_unique<PanelState>();
    CHECK(session_io::LoadSession(loaded.get(), path));

    CHECK_EQ((int)loaded->chases.size(), 1);
    if (loaded->chases.empty()) return;
    const Chase& g = loaded->chases[0];
    CHECK(g.random_scatter);
    CHECK_EQ(g.random_seed, 424242u);
    CHECK_EQ(g.scatter_density, 9);
    CHECK_NEAR(g.loop_seconds, 12.5, 1e-3);

    std::remove(path.c_str());
}

TEST(session_round_trips_tags_and_binds)
{
    auto st = MakeStillState(5);
    const uint32_t sid = st->sources[0].source_id;

    Tag t;
    t.tag_id = 1;
    t.name = "floor";
    t.members.push_back({ sid, st->sources[0].layers[0].fnv1a_hash });
    t.members.push_back({ sid, st->sources[0].layers[1].fnv1a_hash });
    st->tags.push_back(t);
    st->next_tag_id = 2;

    Bind b;
    b.bind_id = 1;
    b.name = "Split pass";
    b.members.push_back({ sid, st->sources[0].layers[2].fnv1a_hash });
    b.members.push_back({ sid, st->sources[0].layers[3].fnv1a_hash });
    st->binds.push_back(b);
    st->next_bind_id = 2;

    const std::string path = TempSessionPath();
    CHECK(session_io::WriteSession(st.get(), path));

    auto loaded = std::make_unique<PanelState>();
    CHECK(session_io::LoadSession(loaded.get(), path));

    CHECK_EQ((int)loaded->tags.size(), 1);
    CHECK_EQ((int)loaded->binds.size(), 1);
    if (loaded->tags.empty() || loaded->binds.empty()) return;
    CHECK_EQ(loaded->tags[0].name, std::string("floor"));
    CHECK_EQ((int)loaded->tags[0].members.size(), 2);
    CHECK_EQ(loaded->binds[0].name, std::string("Split pass"));
    CHECK_EQ((int)loaded->binds[0].members.size(), 2);
    CHECK_EQ(loaded->binds[0].members[0].fnv1a_hash,
             st->sources[0].layers[2].fnv1a_hash);

    std::remove(path.c_str());
}

// ===== User preferences ==============================================
//
// Zoom is a property of the PERSON, not of a scene: David imports a new
// scene every time he tests how the tool would be used, and re-picking
// the zoom each time is friction. So it lives in a prefs file next to
// no session at all.

TEST(zoom_preference_survives_a_restart)
{
    const std::string dir = std::string(CM_TEST_TMP) + "/prefs_a";
#ifdef _WIN32
    _mkdir(CM_TEST_TMP);
    _mkdir(dir.c_str());
    _putenv_s("CHASEMAKER_PREFS_DIR", dir.c_str());
#else
    mkdir(CM_TEST_TMP, 0755);
    mkdir(dir.c_str(), 0755);
    setenv("CHASEMAKER_PREFS_DIR", dir.c_str(), 1);
#endif

    std::remove((dir + "/prefs.json").c_str());
    {
        PanelState a;
        a.sheet_cols = 2;                 // zoomed right in
        session_io::SavePrefs(&a);
    }
    {
        // A brand-new panel, as after an AE restart: no session loaded,
        // nothing imported yet.
        PanelState b;
        CHECK_EQ(b.sheet_cols, 3);        // the built-in default
        session_io::LoadPrefs(&b);
        CHECK_EQ(b.sheet_cols, 2);
    }
}

// A prefs file that is missing, empty or mangled must leave the
// defaults alone rather than failing anything.
TEST(bad_prefs_are_ignored_not_reported)
{
    const std::string dir = std::string(CM_TEST_TMP) + "/prefs_b";
#ifdef _WIN32
    _mkdir(CM_TEST_TMP);
    _mkdir(dir.c_str());
    _putenv_s("CHASEMAKER_PREFS_DIR", dir.c_str());
#else
    mkdir(CM_TEST_TMP, 0755);
    mkdir(dir.c_str(), 0755);
    setenv("CHASEMAKER_PREFS_DIR", dir.c_str(), 1);
#endif

    std::remove((dir + "/prefs.json").c_str());
    {   // nothing written yet
        PanelState a;
        session_io::LoadPrefs(&a);
        CHECK_EQ(a.sheet_cols, 3);
        CHECK(a.last_error.empty());
    }
    {   // garbage
        std::ofstream f(dir + "/prefs.json", std::ios::binary | std::ios::trunc);
        f << "{ this is not json";
    }
    {
        PanelState a;
        session_io::LoadPrefs(&a);
        CHECK_EQ(a.sheet_cols, 3);
        CHECK(a.last_error.empty());
    }
    {   // in range but absurd
        std::ofstream f(dir + "/prefs.json", std::ios::binary | std::ios::trunc);
        f << "{ \"sheet_cols\": 9000 }";
    }
    {
        PanelState a;
        session_io::LoadPrefs(&a);
        CHECK(a.sheet_cols >= 1 && a.sheet_cols <= 6);
    }
}

// The prefs directory may not exist yet on a fresh machine — the very
// first save has to create it, including intermediate levels (on macOS
// the path is ~/Library/Application Support/ChaseMaker).
TEST(the_first_save_creates_the_prefs_directory)
{
    const std::string dir =
        std::string(CM_TEST_TMP) + "/prefs_c/nested/ChaseMaker";
#ifdef _WIN32
    _mkdir(CM_TEST_TMP);
    _putenv_s("CHASEMAKER_PREFS_DIR", dir.c_str());
#else
    mkdir(CM_TEST_TMP, 0755);
    setenv("CHASEMAKER_PREFS_DIR", dir.c_str(), 1);
#endif
    // Nothing below CM_TEST_TMP/prefs_c exists yet.
    {
        PanelState a;
        a.sheet_cols = 5;
        session_io::SavePrefs(&a);
    }
    {
        PanelState b;
        session_io::LoadPrefs(&b);
        CHECK_EQ(b.sheet_cols, 5);
    }
}

// ===== New session ===================================================

TEST(new_session_clears_the_scene_but_keeps_the_person_s_settings)
{
    auto st = testing::MakeState(8);
    st->project_fps.store(24.f);
    st->project_fps_user.store(true);
    st->sheet_cols = 2;

    AutotagByName(st.get());
    chase_gen::GenerateStandardPack(*st, {}, 24.f);
    CHECK(!st->chases.empty());
    CHECK(!st->sources.empty());
    st->sheet_playheads.emplace_back(st->chases[0].chase_id, 12.f);
    st->sheet_unchecked.push_back(st->chases[0].chase_id);
    st->session_save_path = "somewhere/old.chasemaker.json";
    st->undo_stack.push_back(CaptureUndoSnapshot(*st));

    const int gen_before = st->scan_generation.load();
    session_io::NewSession(st.get());

    CHECK(st->sources.empty());
    CHECK(st->chases.empty());
    CHECK(st->tags.empty());
    CHECK(st->binds.empty());
    CHECK(st->sheet_playheads.empty());
    CHECK(st->sheet_unchecked.empty());
    CHECK(st->undo_stack.empty());
    CHECK(st->redo_stack.empty());
    CHECK(!st->has_last_stable);
    CHECK(st->session_save_path.empty());
    CHECK_EQ((int)st->active_chase_index, -1);
    CHECK_EQ((int)st->next_chase_id, 1);
    // Thumbnails from the old scene must not be drawn against the new
    // one: the renderer keys its cache off this.
    CHECK(st->scan_generation.load() > gen_before);

    // ...but how this person works is untouched.
    CHECK_EQ(st->sheet_cols, 2);
    CHECK_NEAR(st->project_fps.load(), 24.0, 1e-6);
    CHECK(st->project_fps_user.load());
}

// Ids restart, so a fresh scene doesn't inherit the last one's
// numbering — a chase in the new session must not collide with a
// playhead cached against the old one.
TEST(new_session_restarts_ids_without_stale_playheads)
{
    auto st = testing::MakeState(6);
    AutotagByName(st.get());
    chase_gen::GenerateStandardPack(*st, {}, 30.f);
    const uint32_t old_id = st->chases.empty() ? 0 : st->chases[0].chase_id;
    st->sheet_playheads.emplace_back(old_id, 40.f);

    session_io::NewSession(st.get());
    CHECK(st->sheet_playheads.empty());
    CHECK_EQ((int)st->next_chase_id, 1);

    // Import a scene into the cleared session, the way a new test round
    // starts. Ids restart, so the first chase reuses the old id — and
    // that is exactly why the cached playheads had to go with it.
    auto fresh = testing::MakeState(6);
    st->sources.push_back(fresh->sources[0]);
    AutotagByName(st.get());
    chase_gen::GenerateStandardPack(*st, {}, 30.f);
    CHECK(!st->chases.empty());
    if (st->chases.empty()) return;
    CHECK_EQ((int)st->chases[0].chase_id, (int)old_id);
    for (const auto& kv : st->sheet_playheads) {
        CHECK(kv.first != old_id);
    }
}

// ===== Serialize / load as TEXT ======================================
//
// The AE project stores the session as an item comment, so the round
// trip has to work without a file in it at all. LoadSessionText is the
// same loader the file path uses, minus where the bytes came from.

TEST(a_session_round_trips_through_text_with_no_file)
{
    auto st = testing::MakeState(6);
    AutotagByName(st.get());
    chase_gen::GenerateStandardPack(*st, {}, 30.f);
    st->export_prefix = "USC_Curtains";
    st->export_split_groups = true;
    const int chases = (int)st->chases.size();
    const int tags   = (int)st->tags.size();

    const std::string text = session_io::SerializeSession(st.get());
    CHECK(!text.empty());

    PanelState back;
    CHECK(session_io::LoadSessionText(&back, text, std::string()));
    CHECK_EQ((int)back.chases.size(), chases);
    CHECK_EQ((int)back.tags.size(), tags);
    CHECK_EQ(back.export_prefix, std::string("USC_Curtains"));
    CHECK(back.export_split_groups);
    // No file was involved, so there is no save path to remember.
    CHECK(back.session_save_path.empty());
}

// The comment is user-visible in AE's Project panel, so someone WILL
// eventually edit or truncate it. That has to read as "no session
// here", not as a crash or a half-loaded state.
TEST(mangled_session_text_is_reported_not_fatal)
{
    auto st = testing::MakeState(4);
    chase_gen::GenerateStandardPack(*st, {}, 30.f);
    const std::string good = session_io::SerializeSession(st.get());
    CHECK(good.size() > 40);

    for (const std::string bad : {
             std::string("not json at all"),
             good.substr(0, good.size() / 2),      // truncated
             std::string("{}"),
             std::string(""),
         }) {
        PanelState s2;
        session_io::LoadSessionText(&s2, bad, std::string());
        // Whatever it decides, it must not invent chases out of junk.
        if (!bad.empty() && bad != "{}") CHECK(s2.chases.empty());
    }
}

// The export prefix travels with the session, so reopening a project
// offers the same name it was exported under last time.
TEST(the_export_prefix_survives_a_round_trip)
{
    auto st = testing::MakeState(3);
    st->export_prefix = "USC_CatTowers";
    const std::string text = session_io::SerializeSession(st.get());

    PanelState back;
    session_io::LoadSessionText(&back, text, std::string());
    CHECK_EQ(back.export_prefix, std::string("USC_CatTowers"));
}
