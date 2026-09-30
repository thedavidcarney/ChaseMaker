// Delivery comp names.
//
// David's convention (2026-09-09): "USC_CatTowers_LeftToRight_v1" —
// prefix, then the chase, lowercase v, no leading zeros. With Split
// Lightgroups on, the per-group comps take another underscore-separated
// part: "USC_CatTowers_LeftToRight_Spotlights_v1".
//
// Only the name COMPACTION is testable here; the versioning and the
// split live in ae_build.cpp, which needs a real AE.

#include "test_harness.h"

#include <string>

#include "chase_gen.h"

using chase_gen::CompactExportName;

TEST(a_chase_name_loses_its_spaces_and_keeps_its_words)
{
    CHECK_EQ(CompactExportName("Left to Right"), std::string("LeftToRight"));
    CHECK_EQ(CompactExportName("Bottom to Top"), std::string("BottomToTop"));
    CHECK_EQ(CompactExportName("Center Out"),    std::string("CenterOut"));
    CHECK_EQ(CompactExportName("Random"),        std::string("Random"));
}

// The template names are the ones that matter, and one of them starts
// with a digit.
TEST(a_name_starting_with_a_digit_survives)
{
    CHECK_EQ(CompactExportName("3 Step Chase"), std::string("3StepChase"));
}

// Tag names go through the same rule, since they become a name part.
TEST(punctuation_and_runs_of_separators_collapse)
{
    CHECK_EQ(CompactExportName("Spot lights"),    std::string("SpotLights"));
    CHECK_EQ(CompactExportName("Curtain_Col"),    std::string("CurtainCol"));
    CHECK_EQ(CompactExportName("Swag-Highlight"), std::string("SwagHighlight"));
    CHECK_EQ(CompactExportName("A  B   C"),       std::string("ABC"));
    CHECK_EQ(CompactExportName("MG.Curtain"),     std::string("MGCurtain"));
}

// Existing capitals are left alone — "MGCurtain" must not become
// "Mgcurtain".
TEST(existing_capitals_are_preserved)
{
    CHECK_EQ(CompactExportName("MGCurtain"),      std::string("MGCurtain"));
    CHECK_EQ(CompactExportName("StageDownlight"), std::string("StageDownlight"));
    CHECK_EQ(CompactExportName("cylinder0_6"),    std::string("Cylinder06"));
}

// Leading and trailing separators must not produce a stray underscore
// or an empty first word.
TEST(edge_shapes_do_not_produce_stray_separators)
{
    CHECK_EQ(CompactExportName("  Left to Right  "),
             std::string("LeftToRight"));
    CHECK_EQ(CompactExportName("_"), std::string(""));
    CHECK_EQ(CompactExportName(""),  std::string(""));
    CHECK_EQ(CompactExportName("   "), std::string(""));
}

// The whole assembled name, the way the builder assembles it.
//
// NO separator between the artist's prefix and the chase — the prefix
// already carries its own underscores. David, correcting me
// 2026-09-10: "there should be no underscore between our provided name
// and the chase so it should be USC_Curtains3StepChase_v1". The
// LIGHTGROUP part is underscore-separated.
TEST(the_delivery_name_reads_as_david_wrote_it)
{
    const std::string prefix = "USC_Curtains";
    const std::string chase  = CompactExportName("3 Step Chase");

    CHECK_EQ(prefix + chase, std::string("USC_Curtains3StepChase"));
    CHECK_EQ(prefix + chase + "_v1",
             std::string("USC_Curtains3StepChase_v1"));

    const std::string group = CompactExportName("Spot lights");
    CHECK_EQ(prefix + chase + "_" + group + "_v1",
             std::string("USC_Curtains3StepChase_SpotLights_v1"));
}
