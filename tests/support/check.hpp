#pragma once

// Minimal dependency-free test helpers shared by all MH-Forever test executables.

#include <cstdio>
#include <cstdlib>

namespace mhf::test {

inline int& failure_count() {
    static int failures = 0;
    return failures;
}

inline int finish(const char* suite) {
    if (failure_count() == 0) {
        std::printf("%s: all checks passed\n", suite);
        return EXIT_SUCCESS;
    }
    std::printf("%s: %d check(s) failed\n", suite, failure_count());
    return EXIT_FAILURE;
}

} // namespace mhf::test

#define MHF_CHECK(expr)                                                                  \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            std::printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #expr);         \
            ++::mhf::test::failure_count();                                              \
        }                                                                                \
    } while (0)
