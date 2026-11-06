// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Minimal assertion helpers for the Boneyards screen tests.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace by_test {

inline int failures = 0;

inline void check(bool ok, const char* what, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++failures;
    }
}

inline void check_str(const char* got, const char* want, const char* what, int line) {
    if (got == nullptr || std::strcmp(got, want) != 0) {
        std::fprintf(
            stderr,
            "FAIL line %d: %s\n  got:  \"%s\"\n  want: \"%s\"\n",
            line,
            what,
            got != nullptr ? got : "(null)",
            want
        );
        ++failures;
    }
}

inline int finish(const char* name) {
    if (failures != 0) {
        std::fprintf(stderr, "%s: %d failure(s)\n", name, failures);
        return EXIT_FAILURE;
    }
    std::printf("%s: ok\n", name);
    return EXIT_SUCCESS;
}

} // namespace by_test

#define CHECK(expr) by_test::check((expr), #expr, __LINE__)
#define CHECK_STR(got, want) by_test::check_str((got), (want), #got, __LINE__)
