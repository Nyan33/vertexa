// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal self-contained unit test harness (no external dependencies).
#pragma once

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace vxtest {

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> r;
    return r;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

struct Registrar {
    Registrar(const char* n, void (*f)()) { registry().push_back({n, f}); }
};

inline int runAll(int argc, char** argv)
{
    int failedCases = 0, ran = 0;
    for (const Case& c : registry()) {
        if (argc > 1 && std::strstr(c.name, argv[1]) == nullptr) continue;
        const int before = failures();
        c.fn();
        ++ran;
        const bool ok = failures() == before;
        if (!ok) ++failedCases;
        std::printf("[%s] %s\n", ok ? " OK " : "FAIL", c.name);
    }
    std::printf("\n%d/%d test cases passed\n", ran - failedCases, ran);
    return failedCases == 0 ? 0 : 1;
}

} // namespace vxtest

#define VX_TEST(name)                                                  \
    static void name();                                                \
    static vxtest::Registrar vx_registrar_##name(#name, name);         \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++vxtest::failures();                                                          \
        }                                                                                  \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                                              \
    do {                                                                                                   \
        const double vx_a = double(a), vx_b = double(b);                                                   \
        if (!(std::abs(vx_a - vx_b) <= double(eps))) {                                                     \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %.12g, %s = %.12g (eps %g)\n", __FILE__, \
                         __LINE__, #a, vx_a, #b, vx_b, double(eps));                                      \
            ++vxtest::failures();                                                                          \
        }                                                                                                  \
    } while (0)

#define VX_TEST_MAIN() \
    int main(int argc, char** argv) { return vxtest::runAll(argc, argv); }
