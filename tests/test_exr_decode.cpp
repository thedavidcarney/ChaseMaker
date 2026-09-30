// Real-render decode check through the real scanner.
//
// OpenEXR 3.4's DWA decoder fails every chunk after the first when one
// decode pipeline is reused (what the single-threaded C++ reader does),
// which skipped every layer of a Blender DWAB render as
// "decoder error [DWAB]". A synthetic DWAB file did NOT reproduce it,
// so this runs against a real render named by CHASEMAKER_TEST_EXR and
// skips when that isn't set (renders aren't in the public repo).

#include "test_harness.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "exr_scan.h"
#include "panel_state.h"

TEST(real_exr_scans_with_no_decoder_errors)
{
    const char* path = std::getenv("CHASEMAKER_TEST_EXR");
    if (!path || !*path) {
        th::Skip("CHASEMAKER_TEST_EXR not set");
        return;
    }

    PanelState st;
    const auto t0 = std::chrono::steady_clock::now();
    exr_scan::StartScan(path, &st, false);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int i = 0; i < 60000 && st.scanning.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const double secs = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(!st.scanning.load());

    CHECK_EQ((int)st.sources.size(), 1);
    if (st.sources.empty()) return;
    const Source& src = st.sources[0];
    int decoder_errors = 0;
    for (const auto& s : src.skipped)
        if (s.reason.find("decoder") != std::string::npos) ++decoder_errors;
    std::printf("    %d lights, %d skipped (%d decoder errors), %.1f s\n",
                (int)src.layers.size(), (int)src.skipped.size(),
                decoder_errors, secs);
    CHECK_EQ(decoder_errors, 0);
    // Optional per-light dump, for diffing two reader implementations.
    if (const char* dump = std::getenv("CHASEMAKER_TEST_DUMP")) {
        if (FILE* f = std::fopen(dump, "w")) {
            for (const auto& L : src.layers)
                std::fprintf(f, "%s %.9g %.9g %.9g %.9g %.9g %.9g %.17g\n",
                             L.display_name.c_str(), L.cx, L.cy, L.cx_hot,
                             L.cy_hot, L.peak_x, L.peak_y, L.total);
            std::fclose(f);
        }
    }
    CHECK(!src.layers.empty());
    for (const auto& L : src.layers) {
        CHECK(L.total > 0.0);
        CHECK(L.cx >= 0.f && L.cx <= 1.f);
        CHECK(L.cy >= 0.f && L.cy <= 1.f);
    }
}
