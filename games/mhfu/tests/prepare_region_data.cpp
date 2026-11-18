// Only region metadata: generates synthetic-test headers without reading game files.
#include "emitter.hpp"
#include <filesystem>

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    std::filesystem::create_directories(argv[2]);
    const auto entries = mhf::psp::recomp::region_data_entries(argv[1]);
    mhf::psp::recomp::emit_region_data("mhfu", entries, argv[2]);
}
