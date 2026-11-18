// Only region metadata: generates synthetic-test headers without reading game files.
#include "emitter.hpp"
#include <filesystem>
#include <string_view>

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    std::filesystem::create_directories(argv[2]);
    auto entries = mhf::psp::recomp::region_data_entries(argv[1]);
    if (std::string_view(argv[3]) == "unmapped") {
        for (auto &[name, address] : entries) address.reset();
    }
    mhf::psp::recomp::emit_region_data("mhfu", entries, argv[2]);
}
