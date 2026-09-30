// Which passes are lights and which are scenery.
//
// The EXR scanner has always kept World / Ambient / HDRI / Image /
// Alpha / cryptomattes out of the chase order. Single-light sources
// (a folder of .mov or .png passes, one file per light) never got that
// treatment — a World.mov came in as an ordinary light — which is
// exactly the sort of thing the setup screen promises to have right.

#include "test_harness.h"
#include "test_helpers.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "chase_gen.h"
#include "exr_scan.h"
#include "panel_state.h"
#include "session_io.h"

using exr_scan::EnvironmentHint;
using exr_scan::SkipReason;

// ---- The exact list, unchanged ---------------------------------------

TEST(skip_reason_catches_the_known_non_lights)
{
    CHECK(!SkipReason("World").empty());
    CHECK(!SkipReason("world").empty());
    CHECK(!SkipReason("HDRI").empty());
    CHECK(!SkipReason("Ambient").empty());
    CHECK(!SkipReason("Image").empty());
    CHECK(!SkipReason("Alpha").empty());
    CHECK(!SkipReason("CryptoObject00").empty());
    CHECK(!SkipReason("crypto_material").empty());
}

TEST(skip_reason_leaves_real_lights_alone)
{
    CHECK(SkipReason("Curtain_001").empty());
    CHECK(SkipReason("Swag Highlight 12").empty());
    CHECK(SkipReason("cylinder0_6_0020").empty());
    // Names that merely CONTAIN a keyword are lights until the user
    // says otherwise — pass names are arbitrary and over-filtering
    // silently drops real fixtures.
    CHECK(SkipReason("World_Light").empty());
    CHECK(SkipReason("Ambient_Fill").empty());
    CHECK(SkipReason("HDRI_Sky_001").empty());
}

// ---- Suggestions, never applied automatically ------------------------

TEST(environment_hint_flags_the_near_misses)
{
    CHECK(!EnvironmentHint("World_Light").empty());
    CHECK(!EnvironmentHint("Ambient_Fill").empty());
    CHECK(!EnvironmentHint("HDRI_Sky_001").empty());
    CHECK(!EnvironmentHint("scene_environment_pass").empty());
}

TEST(environment_hint_stays_quiet_for_ordinary_lights)
{
    CHECK(EnvironmentHint("Curtain_001").empty());
    CHECK(EnvironmentHint("Swag Highlight 12").empty());
    CHECK(EnvironmentHint("cylinder0_6_0020").empty());
    CHECK(EnvironmentHint("Uplight_04").empty());
}

// An exact match is already excluded, so it isn't also a "suggestion".
TEST(environment_hint_does_not_repeat_the_skip_list)
{
    CHECK(EnvironmentHint("World").empty());
    CHECK(EnvironmentHint("Ambient").empty());
    CHECK(EnvironmentHint("HDRI").empty());
}

// ---- Single-light sources honour the same list -----------------------

namespace {

// Adding a still/movie source builds its placeholder from the filename
// alone — no file access — so this runs without any fixtures.
std::unique_ptr<PanelState> AddStill(const char* filename)
{
    auto st = std::make_unique<PanelState>();
    exr_scan::AddSourcePath(std::string("C:/nowhere/") + filename, st.get());
    return st;
}

} // namespace

TEST(single_light_source_named_world_is_not_staged_as_a_light)
{
    auto st = AddStill("World.png");
    CHECK_EQ((int)st->sources.size(), 1);
    if (st->sources.empty()) return;
    CHECK_EQ((int)st->sources[0].layers.size(), 1);
    if (st->sources[0].layers.empty()) return;
    CHECK_EQ(st->sources[0].layers[0].display_name, std::string("World"));
    CHECK(!st->sources[0].layers[0].included);
}

TEST(single_light_ambient_and_crypto_clips_are_excluded_too)
{
    for (const char* f : { "Ambient.mov", "HDRI.png", "CryptoObject00.png" }) {
        auto st = AddStill(f);
        if (st->sources.empty() || st->sources[0].layers.empty()) {
            CHECK(false);
            continue;
        }
        CHECK(!st->sources[0].layers[0].included);
    }
}

TEST(ordinary_single_light_clips_stay_included)
{
    for (const char* f : { "Curtain_001.mov", "cylinder7_12_0020.png",
                           "Ambient_Fill.png" }) {
        auto st = AddStill(f);
        if (st->sources.empty() || st->sources[0].layers.empty()) {
            CHECK(false);
            continue;
        }
        CHECK(st->sources[0].layers[0].included);
    }
}

// An excluded single-light source must not vanish: it still appears as
// a source with a light the user can tick back on in Staging.
TEST(excluded_single_light_source_is_still_listed)
{
    auto st = AddStill("World.mov");
    CHECK_EQ((int)st->sources.size(), 1);
    if (st->sources.empty()) return;
    CHECK(st->sources[0].is_movie);
    CHECK_EQ((int)st->sources[0].layers.size(), 1);
    CHECK(st->sources[0].skipped.empty());   // excluded, not hidden
}

TEST(excluded_lights_drive_no_chases)
{
    auto st = AddStill("World.png");
    exr_scan::AddSourcePath("C:/nowhere/Curtain_001.png", st.get());
    exr_scan::AddSourcePath("C:/nowhere/Curtain_002.png", st.get());

    CHECK_EQ((int)st->sources.size(), 3);
    CHECK_EQ(chase_gen::CountEligibleLights(*st, {}), 2);
}

// Re-including an auto-excluded scenery clip has to survive a save and
// reload — otherwise the scanner's default silently overrules the user
// every time the session opens.
TEST(re_including_an_excluded_light_survives_a_session_round_trip)
{
    auto st = AddStill("World.png");
    exr_scan::AddSourcePath("C:/nowhere/Curtain_001.png", st.get());
    CHECK(!st->sources[0].layers[0].included);

    st->sources[0].layers[0].included = true;      // user ticks it back on

    const char* tmp = std::getenv("TEMP");
    const std::string path =
        std::string(tmp && *tmp ? tmp : ".") + "/cm_skiplist_test.chasemaker.json";
    CHECK(session_io::WriteSession(st.get(), path));

    auto loaded = std::make_unique<PanelState>();
    CHECK(session_io::LoadSession(loaded.get(), path));
    CHECK_EQ((int)loaded->sources.size(), 2);
    if (loaded->sources.size() != 2) return;
    CHECK_EQ((int)loaded->sources[0].layers.size(), 1);
    if (loaded->sources[0].layers.empty()) return;
    CHECK(loaded->sources[0].layers[0].included);
    CHECK_EQ(chase_gen::CountEligibleLights(*loaded, {}), 2);

    std::remove(path.c_str());
}

TEST(an_excluded_light_stays_excluded_across_a_round_trip)
{
    auto st = AddStill("World.png");
    exr_scan::AddSourcePath("C:/nowhere/Curtain_001.png", st.get());

    const char* tmp = std::getenv("TEMP");
    const std::string path =
        std::string(tmp && *tmp ? tmp : ".") + "/cm_skiplist_test2.chasemaker.json";
    CHECK(session_io::WriteSession(st.get(), path));

    auto loaded = std::make_unique<PanelState>();
    CHECK(session_io::LoadSession(loaded.get(), path));
    if (loaded->sources.empty() || loaded->sources[0].layers.empty()) return;
    CHECK(!loaded->sources[0].layers[0].included);
    CHECK_EQ(chase_gen::CountEligibleLights(*loaded, {}), 1);

    std::remove(path.c_str());
}

// ===== Blender's built-in passes =====================================
//
// From USC_CatTowers (David, testing 2026-09-09): Blender writes its
// built-in passes into the view-layer part, so they arrive prefixed —
// "ViewLayer.Combined", not "Combined". Matching only the whole string
// let every one of them through into Staging as if it were a light.

TEST(view_layer_prefixed_passes_are_skipped)
{
    const char* names[] = {
        "ViewLayer.Noisy Image",
        "ViewLayer.Denoising Albedo",
        "ViewLayer.Combined_Ambient",
        "ViewLayer.Combined",
        "ViewLayer.Image",
        "ViewLayer.Alpha",
        "ViewLayer.World",
    };
    for (const char* n : names) {
        const std::string why = exr_scan::SkipReason(n);
        CHECK(!why.empty());
        if (why.empty()) std::printf("      leaked: %s\n", n);
    }
}

// The denoiser writes Normal and Depth alongside Albedo from the same
// setting; they are data, not light, for the same reason.
TEST(the_other_denoise_passes_are_skipped_too)
{
    CHECK(!exr_scan::SkipReason("ViewLayer.Denoising Normal").empty());
    CHECK(!exr_scan::SkipReason("ViewLayer.Denoising Depth").empty());
    CHECK(!exr_scan::SkipReason("ViewLayer.Denoising Specular Albedo").empty());
    CHECK(!exr_scan::SkipReason("ViewLayer.Denoising Roughness").empty());
    CHECK(!exr_scan::SkipReason("Denoising Specular Albedo").empty());
    CHECK(!exr_scan::SkipReason("Denoising Roughness").empty());
}

// Compound scenery names. "Combined_Ambient" is caught on its
// underscore-separated tokens, NOT by a substring search — pass names
// are arbitrary and a substring rule would eat real fixtures.
// "Combined_Ambient" is caught because it carries a BUILT-IN PASS
// token, not because it contains "ambient". The near-miss policy is
// unchanged and deliberate: names that merely contain a scenery word
// get a suggestion the artist confirms, never an automatic exclusion,
// because a pass name is an arbitrary string.
TEST(a_beauty_pass_qualified_by_scenery_is_skipped)
{
    CHECK(!exr_scan::SkipReason("Combined_Ambient").empty());
    CHECK(!exr_scan::SkipReason("Combined_World").empty());
    CHECK(!exr_scan::SkipReason("Noisy Image_Ambient").empty());

    // Still only suggestions — see skip_reason_leaves_real_lights_alone.
    CHECK(exr_scan::SkipReason("Ambient_Fill").empty());
    CHECK(exr_scan::SkipReason("World_Light").empty());
    // And a lightgroup that happens to start with the word is a light.
    CHECK(exr_scan::SkipReason("CombinedFixture_A").empty());
    CHECK(exr_scan::SkipReason("Combined_CatTower_01").empty());
}

// A light that merely happens to sit in a dotted part name must
// survive. Only the known pass names are matched, never the prefix.
TEST(a_lightgroup_in_a_named_part_is_not_skipped)
{
    CHECK(exr_scan::SkipReason("ViewLayer.CatTower_003").empty());
    CHECK(exr_scan::SkipReason("ViewLayer.StringLights_A_01").empty());
    CHECK(exr_scan::SkipReason("Tower.Uplight_01").empty());
}

// Anything the skip list now catches must not ALSO be offered as a
// scenery suggestion — that would be the panel asking about something
// it already handled.
TEST(skipped_passes_produce_no_scenery_hint)
{
    const char* names[] = {
        "ViewLayer.Combined_Ambient", "ViewLayer.World",
        "ViewLayer.Noisy Image",      "ViewLayer.Denoising Albedo",
    };
    for (const char* n : names) {
        CHECK(exr_scan::EnvironmentHint(n).empty());
    }
}
