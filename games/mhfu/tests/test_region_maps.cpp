// Tests the region maps in games/mhfu/regions (identification data in the repository, no game data): every map parses,
// the EU map gives the data entries of --true-weapon-values an address, its per-language format entries never share an
// address (the game program writes only where the loaded language's format is), and the releases that do not map them
// yet still declare them, so the game program builds for every release.

#include "emitter.hpp"

#include "check.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mhf::psp::recomp::RegionData;
using mhf::psp::recomp::region_data_entries;

std::vector<std::string> element_value_zero_entries() {
    std::vector<std::string> names;
    for (const char *language : {"en", "fr", "de", "es", "it"}) {
        names.push_back(std::string("mh_element_value_format_zero_") + language);
        names.push_back(std::string("mh_dual_element_value_zero_") + language);
    }
    return names;
}

RegionData entries_for(const std::string &disc_id) {
    return region_data_entries(fs::path(MHF_MHFU_REGIONS_DIR) / disc_id / "symbols.toml");
}

} // namespace

int main() {
    const auto zeros = element_value_zero_entries();

    const RegionData eu = entries_for("ULES01213");
    MHF_CHECK(eu.contains("mh_attack_display_multipliers") && eu.at("mh_attack_display_multipliers").has_value());
    std::set<std::uint32_t> addresses;
    for (const auto &name : zeros) {
        MHF_CHECK(eu.contains(name) && eu.at(name).has_value());
        if (eu.contains(name) && eu.at(name)) addresses.insert(*eu.at(name));
    }
    MHF_CHECK(addresses.size() == zeros.size());
    MHF_CHECK(eu.contains("mh_overlay_region_bases") && eu.at("mh_overlay_region_bases").has_value());

    for (const char *disc_id : {"ULUS10391", "ULJM05500"}) {
        const RegionData other = entries_for(disc_id);
        MHF_CHECK(other.contains("mh_attack_display_multipliers")); // declared, mapped or not
        for (const auto &name : zeros) MHF_CHECK(other.contains(name));
        MHF_CHECK(other.at("mh_overlay_region_bases").has_value());
    }

    return mhf::test::finish("mhfu_region_maps");
}
