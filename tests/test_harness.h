// Minimal test harness — no external deps (vcpkg manifest stays as-is).
// Register with TEST(name) { ... } and assert with CHECK*.

#pragma once

#include <cstdio>
#include <string>
#include <vector>

namespace th {

struct TestCase { const char* name; void (*fn)(); };
std::vector<TestCase>& Registry();
struct Reg { Reg(const char* n, void (*f)()); };

void Fail(const char* file, int line, const std::string& msg);
// Mark the running test as skipped (missing local fixtures, say) —
// remaining CHECKs still run but the case is never a failure.
void Skip(const std::string& reason);
std::string Str(double v);
std::string Str(const std::string& v);

} // namespace th

#define TEST(name)                                                   \
    static void name();                                              \
    static th::Reg th_reg_##name(#name, name);                       \
    static void name()

#define CHECK(cond)                                                  \
    do { if (!(cond)) th::Fail(__FILE__, __LINE__,                   \
        "CHECK failed: " #cond); } while (0)

#define CHECK_EQ(a, b)                                               \
    do { auto th_a = (a); auto th_b = (b);                           \
         if (!(th_a == th_b)) th::Fail(__FILE__, __LINE__,           \
            std::string("CHECK_EQ failed: " #a " == " #b            \
                        "\n      got: ") + th::Str(th_a) +           \
            "\n expected: " + th::Str(th_b)); } while (0)

#define CHECK_NEAR(a, b, eps)                                        \
    do { double th_a = (double)(a), th_b = (double)(b);              \
         double th_d = th_a - th_b; if (th_d < 0) th_d = -th_d;      \
         if (!(th_d <= (eps))) th::Fail(__FILE__, __LINE__,          \
            std::string("CHECK_NEAR failed: " #a " ~= " #b          \
                        "\n      got: ") + th::Str(th_a) +           \
            "\n expected: " + th::Str(th_b)); } while (0)
