# Emit a tiny header that bakes the current local time into a macro.
# Re-runs on every build (driven from CMakeLists via a custom target),
# so the resulting stamp reflects when this build was actually linked
# rather than when chase_maker.cpp was last edited.
string(TIMESTAMP NOW "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}" "// AUTO-GENERATED. Do not edit. Regenerated on every build.
#pragma once
#define CM_BUILD_STAMP \"${NOW}\"
")
