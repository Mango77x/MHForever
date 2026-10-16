#include "base/version.hpp"

#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--version") {
            std::printf("%.*s\n", static_cast<int>(mhf::version().size()), mhf::version().data());
            return 0;
        }
    }
    std::printf("MH-Forever (codename) %.*s\n", static_cast<int>(mhf::version().size()), mhf::version().data());
    std::printf("Nothing to run yet. See docs/10-roadmap.md.\n");
    return 0;
}
