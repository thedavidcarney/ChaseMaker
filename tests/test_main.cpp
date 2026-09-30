#include "test_harness.h"

#include <cstdio>
#include <cstring>

namespace th {

static int         g_case_failures = 0;
static bool        g_case_skipped  = false;
static std::string g_skip_reason;

std::vector<TestCase>& Registry()
{
    static std::vector<TestCase> r;
    return r;
}

Reg::Reg(const char* n, void (*f)()) { Registry().push_back({ n, f }); }

void Fail(const char* file, int line, const std::string& msg)
{
    ++g_case_failures;
    const char* base = std::strrchr(file, '/');
    const char* base2 = std::strrchr(file, '\\');
    if (base2 > base) base = base2;
    std::printf("    %s:%d  %s\n", base ? base + 1 : file, line, msg.c_str());
}

void Skip(const std::string& reason)
{
    g_case_skipped = true;
    g_skip_reason = reason;
}

std::string Str(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}
std::string Str(const std::string& v) { return "\"" + v + "\""; }

} // namespace th

int main(int argc, char** argv)
{
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    int run = 0, failed = 0, skipped = 0;
    for (const auto& t : th::Registry()) {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        th::g_case_failures = 0;
        th::g_case_skipped = false;
        th::g_skip_reason.clear();
        t.fn();
        if (th::g_case_skipped) {
            ++skipped;
            std::printf("skip  %s (%s)\n", t.name, th::g_skip_reason.c_str());
        } else if (th::g_case_failures) {
            ++failed;
            std::printf("FAIL  %s\n", t.name);
        } else {
            std::printf("ok    %s\n", t.name);
        }
    }
    std::printf("\n%d test%s, %d failed", run, run == 1 ? "" : "s", failed);
    if (skipped) std::printf(", %d skipped", skipped);
    std::printf("\n");
    return failed ? 1 : 0;
}
