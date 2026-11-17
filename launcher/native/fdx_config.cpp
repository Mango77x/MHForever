#include "fdx_config.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

namespace mhf::launcher::fdx {

namespace fs = std::filesystem;

namespace {

// Where Freedom DX's code reads each option (source/ULES00318). The drink buff's byte sits between the Felyne skills'
// and the Dos bonuses', as its config editor lists them.
constexpr std::size_t kHoldToGather = 0x10;
constexpr std::size_t kTrueRaw = 0x11;
constexpr std::size_t kLaoEarlyKill = 0x12;
constexpr std::size_t kMinimap = 0x13; // percent, 50..100
constexpr std::size_t kSnsDebuff = 0x14;
constexpr std::size_t kFileReplacer = 0x15;
constexpr std::size_t kFelyneSkills = 0x16;
constexpr std::size_t kDrinkBuff = 0x17;
constexpr std::size_t kDosBonus = 0x18;
constexpr std::size_t kInstantSupply = 0x19;
constexpr std::size_t kFieldOfView = 0x1A;
constexpr std::size_t kCamera = 0x1B;
constexpr std::size_t kOfflineTreasure = 0x1C;

std::uint8_t flag(bool on) { return on ? 1 : 0; }

} // namespace

bool apply(std::vector<std::uint8_t> &config, const Options &options) {
    if (config.size() < kConfigSize) return false;
    config[kHoldToGather] = flag(options.hold_to_gather);
    config[kTrueRaw] = flag(options.true_raw);
    config[kLaoEarlyKill] = flag(options.lao_early_kill);
    config[kMinimap] = 100;
    config[kSnsDebuff] = flag(options.sns_debuff);
    config[kFileReplacer] = flag(options.file_replacer);
    config[kFelyneSkills] = flag(options.felyne_skills);
    config[kDrinkBuff] = flag(options.drink_buff);
    config[kDosBonus] = static_cast<std::uint8_t>(std::clamp(options.dos_bonus, 0, 2));
    config[kInstantSupply] = flag(options.instant_supply);
    config[kFieldOfView] = static_cast<std::uint8_t>(std::clamp(options.fov, 0, 255));
    config[kCamera] = static_cast<std::uint8_t>(std::clamp(options.camera, 0, 4));
    config[kOfflineTreasure] = flag(options.offline_treasure);
    return true;
}

fs::path save_folder(const fs::path &stick) { return stick / "PSP" / "SAVEDATA" / "FDXDAT"; }

std::string prepare(const fs::path &stick, const Options &options) {
    std::error_code ec;
    const fs::path folder = save_folder(stick);
    fs::create_directories(folder, ec);
    if (ec) return "Could not make Freedom DX's folder on the memory stick: " + ec.message();
    const fs::path file = folder / "CONFIG.BIN";
    std::vector<std::uint8_t> config;
    {
        std::ifstream in(file, std::ios::binary);
        if (in) config.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (config.size() < kConfigSize) {
        // None yet (or too short): a new one, with the version Freedom DX's own file starts with.
        config.assign(kConfigSize, 0);
        const char version[] = "1.8.6";
        std::memcpy(config.data(), version, sizeof version - 1);
    }
    apply(config, options);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(config.data()), static_cast<std::streamsize>(config.size()));
    if (!out) return "Could not write Freedom DX's options (" + file.string() + ").";
    return {};
}

} // namespace mhf::launcher::fdx
