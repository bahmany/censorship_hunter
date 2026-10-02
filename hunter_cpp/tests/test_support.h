#pragma once
// Minimal assertion helpers shared by the Stage 4 unit tests (no external framework).
#include <cmath>
#include <iostream>
#include <string>

static int g_checks = 0, g_failures = 0, g_cases = 0, g_case_failed_start = 0;

#define T_CASE(name)                                                        \
    do { std::cout << "  " << (name) << " ... "; g_cases++;                 \
         g_case_failed_start = g_failures; } while (0)
#define T_END() do { std::cout << (g_failures == g_case_failed_start ? "ok" : "FAILED") << std::endl; } while (0)
#define CHECK(cond, msg)                                                    \
    do { g_checks++; if (!(cond)) { g_failures++;                           \
         std::cout << "\n    CHECK FAILED (" << __FILE__ << ":" << __LINE__ << "): " << (msg) << std::endl; } } while (0)
#define CHECK_NEAR(a, b, eps, msg) CHECK(std::fabs((double)(a) - (double)(b)) <= (eps), msg)
#define T_SUMMARY()                                                         \
    (std::cout << "cases=" << g_cases << " checks=" << g_checks             \
               << " failures=" << g_failures << std::endl, g_failures == 0 ? 0 : 1)
