// Crash-/hang-resilient load-path tracing.
//
// Diagnostic only: ChaseMaker intermittently hangs while AE loads the
// plugin on the first launch after a rebuild. There's no debugger on
// the dev box, so instead we drop timestamped milestones along the
// load path. Each call opens-appends-flushes-closes the file so the
// last line on disk is always the last milestone reached *before* a
// hang — point the next investigation straight at that phase.
//
// Path: <temp>/chasemaker_load.log  (TEMP/TMP on Windows, TMPDIR else).
// Appends across launches; every process writes a SESSION banner so a
// hung run can be diffed against a clean one. Cheap (milestone calls
// only, never the render hot loop) — safe to leave in.

#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// Diagnostic-only header: getenv/fopen are deliberate (portable,
// no Win32 dep) and the inputs are our own constants — silence
// MSVC's C4996 "use the _s variant" noise just for this file.
#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable : 4996)
#endif

namespace cm_diag {

inline std::mutex& log_mu()
{
    static std::mutex m;
    return m;
}

inline long long ms_since_start()
{
    using namespace std::chrono;
    static const steady_clock::time_point t0 = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - t0).count();
}

inline const char* log_path()
{
    static const std::string p = [] {
        const char* base = nullptr;
#ifdef _WIN32
        base = std::getenv("TEMP");
        if (!base) base = std::getenv("TMP");
#else
        base = std::getenv("TMPDIR");
#endif
        std::string dir = (base && *base) ? base : ".";
        while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\')) {
            dir.pop_back();
        }
        return dir + "/chasemaker_load.log";
    }();
    return p.c_str();
}

inline void logf(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lk(log_mu());
    std::FILE* f = std::fopen(log_path(), "a");
    if (!f) return;
    const unsigned tid = static_cast<unsigned>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::fprintf(f, "[+%7lldms t%08x] ", ms_since_start(), tid);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f, fmt, ap);
    va_end(ap);
    std::fputc('\n', f);
    std::fflush(f);
    std::fclose(f);
}

} // namespace cm_diag

#ifdef _MSC_VER
#  pragma warning(pop)
#endif

#define CM_DIAG_LOG(...) ::cm_diag::logf(__VA_ARGS__)
