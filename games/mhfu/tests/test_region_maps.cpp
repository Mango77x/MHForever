// Tests the region maps in games/mhfu/regions (identification data in the repository, no game data): every map parses,
// every release maps the data entries of --true-weapon-values for each of its languages, a map's per-language format
// entries never share an address (the game program writes only where the loaded language's format is), and every map
// declares every language's names, so the game program builds for every release. Every map also lists the functions
// games/mhfu/tools/prepare.py hooks (mhf_recomp stops when a hooked name is missing) and every instruction patch the
// game program switches on, each one readable.

#include "emitter.hpp"
#include "mini_toml.hpp"

#include "check.hpp"

#include <filesystem>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using mhf::psp::recomp::RegionData;
using mhf::psp::recomp::region_data_entries;

std::vector<std::string> element_value_zero_entries(std::initializer_list<const char *> languages) {
    std::vector<std::string> names;
    for (const char *language : languages) {
        names.push_back(std::string("mh_element_value_format_zero_") + language);
        names.push_back(std::string("mh_dual_element_value_zero_") + language);
    }
    return names;
}

RegionData entries_for(const std::string &disc_id) {
    return region_data_entries(fs::path(MHF_MHFU_REGIONS_DIR) / disc_id / "symbols.toml");
}

// The release maps the multipliers and both formats of each of its languages, at different addresses, and declares
// every language's names.
void check_release(const std::string &disc_id, const std::vector<std::string> &own,
                   const std::vector<std::string> &all, bool camera_mapped = true) {
    const RegionData entries = entries_for(disc_id);
    MHF_CHECK(entries.contains("mh_attack_display_multipliers") &&
              entries.at("mh_attack_display_multipliers").has_value());
    std::set<std::uint32_t> addresses;
    for (const auto &name : own) {
        MHF_CHECK(entries.contains(name) && entries.at(name).has_value());
        if (entries.contains(name) && entries.at(name)) addresses.insert(*entries.at(name));
    }
    MHF_CHECK(addresses.size() == own.size());
    for (const auto &name : all) MHF_CHECK(entries.contains(name));
    MHF_CHECK(entries.contains("mh_overlay_region_bases") && entries.at("mh_overlay_region_bases").has_value());

    const fs::path map_path = fs::path(MHF_MHFU_REGIONS_DIR) / disc_id / "symbols.toml";
    const auto map = mhf::psp::recomp::toml::parse_file(map_path);
    const auto functions = map.tables.find("functions");
    MHF_CHECK(functions != map.tables.end());
    if (functions != map.tables.end()) {
        for (const char *hooked :
             {"mh_overlay_prepare", "mh_supply_box_update", "mh_smithy_create_list_update",
              "mh_smithy_improve_list_update", "mh_smithy_panel_draw", "mh_smithy_model_update"}) {
            MHF_CHECK(mhf::psp::recomp::toml::get(functions->second, hooked).has_value());
        }
    }
    // --smithy-browse and --smithy-layout: their nine patches, all in the village overlay, the two Info functions and
    // the layout tables. --analog-camera: its four call patches, in the camera's d-pad code in the executable (not
    // mapped for every release: then none).
    std::set<std::string> smithy_patches, camera_patches;
    for (const auto &p : mhf::psp::recomp::region_patches(map_path)) {
        if (p.patch.name.starts_with("mh_smithy_")) {
            MHF_CHECK(p.module == "lobby_task.ovl");
            smithy_patches.insert(p.patch.name);
        } else {
            MHF_CHECK(p.module == "executable" && p.patch.kind == mhf::psp::recomp::InstructionPatch::Kind::Call);
            camera_patches.insert(p.patch.name);
        }
    }
    MHF_CHECK(smithy_patches.size() == 9);
    MHF_CHECK(camera_patches == (camera_mapped ? std::set<std::string>({"mh_camera_buttons_copied",
                                                                        "mh_camera_tilt_preset_chosen",
                                                                        "mh_camera_turn_buttons_test",
                                                                        "mh_camera_turn_amount_chosen"})
                                               : std::set<std::string>{}));
    for (const char *table : {"mh_smithy_panel_layout", "mh_smithy_side_layout", "mh_smithy_question_box",
                              "mh_smithy_skill_info_hint", "mh_smithy_model_scissor", "mh_smithy_panel_draw_entry",
                              "mh_pad_input"}) {
        MHF_CHECK(entries.contains(table) && entries.at(table).has_value());
    }
    MHF_CHECK(entries.contains("mh_smithy_create_info_setup") && entries.at("mh_smithy_create_info_setup").has_value());
    MHF_CHECK(entries.contains("mh_smithy_improve_info_setup") && entries.at("mh_smithy_improve_info_setup").has_value());
}

} // namespace

int main() {
    const auto european = element_value_zero_entries({"en", "fr", "de", "es", "it"});
    const auto japanese = element_value_zero_entries({"ja"});
    const auto all = element_value_zero_entries({"en", "fr", "de", "es", "it", "ja"});

    check_release("ULES01213", european, all);
    check_release("ULUS10391", european, all);
    check_release("ULJM05500", japanese, all);
    // FUComplete keeps the Japanese layout: the same map, plus the words it rewrites at run time ([guards]). The analog
    // camera too: FUComplete rewrites a word of the camera function the call patches are in, which then runs in the
    // interpreter, and the interpreter runs call patches as recompiled code does.
    check_release("ULJM05500-FUC", japanese, all);
    {
        const fs::path fuc = fs::path(MHF_MHFU_REGIONS_DIR) / "ULJM05500-FUC" / "symbols.toml";
        std::set<std::string> modules;
        for (const auto &g : mhf::psp::recomp::region_guards(fuc)) modules.insert(g.module);
        MHF_CHECK(modules == std::set<std::string>({"executable", "arcade_task.ovl", "game_task.ovl", "lobby_task.ovl",
                                                    "option_task.ovl"}));
        MHF_CHECK(mhf::psp::recomp::region_guards(fs::path(MHF_MHFU_REGIONS_DIR) / "ULJM05500" / "symbols.toml").empty());
    }

    return mhf::test::finish("mhfu_region_maps");
}
