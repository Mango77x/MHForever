#include "fuc_config.hpp"

#include "iso9660.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

namespace mhf::launcher::fuc {

namespace fs = std::filesystem;

namespace {

// Offsets of the options in CONFIG.BIN (FUCTool res/config.json).
constexpr std::size_t kSaveRegion = 0x11;
constexpr std::size_t kTrueRaw = 0x12;
constexpr std::size_t kDosAudio = 0x13;
constexpr std::size_t kFieldOfView = 0x14; // two bytes
constexpr std::size_t kCamera = 0x16;
constexpr std::size_t kMinimap = 0x17;
constexpr std::size_t kHornTweak = 0x18;
constexpr std::size_t kLaoEarlyKill = 0x19;
constexpr std::size_t kFullChest = 0x1A;
constexpr std::size_t kDrinkBuff = 0x1B;

// The field of view's two bytes for each choice (0.872, 0.935, 0.997, 1.120 as the high half of a float).
constexpr std::uint8_t kFieldOfViewHigh[] = {0x5F, 0x6F, 0x7F, 0x8F};
constexpr std::uint8_t kFieldOfViewLow = 0x3F;

std::uint8_t flag(bool on) { return on ? 1 : 0; }

} // namespace

bool apply(std::vector<std::uint8_t> &config, const Options &options) {
    if (config.size() < kConfigSize) return false;
    config[kSaveRegion] = static_cast<std::uint8_t>(std::clamp(options.save_region, 0, 2));
    config[kTrueRaw] = flag(options.true_raw);
    config[kDosAudio] = flag(options.dos_audio);
    config[kFieldOfView] = kFieldOfViewHigh[std::clamp(options.fov, 0, 3)];
    config[kFieldOfView + 1] = kFieldOfViewLow;
    config[kCamera] = static_cast<std::uint8_t>(std::clamp(options.camera, 0, 4));
    config[kMinimap] = 0;
    config[kHornTweak] = flag(options.horn_tweak);
    config[kLaoEarlyKill] = flag(options.lao_early_kill);
    config[kFullChest] = flag(options.full_chest);
    config[kDrinkBuff] = flag(options.drink_buff);
    return true;
}

fs::path save_folder(const fs::path &stick) { return stick / "PSP" / "SAVEDATA" / "FUCDAT"; }

std::string prepare(const fs::path &iso, const fs::path &stick, const Options &options) {
    const fs::path folder = save_folder(stick);
    const fs::path config_path = folder / "CONFIG.BIN";
    std::error_code ec;
    if (!fs::is_regular_file(config_path, ec)) {
        // The first start: FUComplete's own files, as the game would copy them from its disc.
        try {
            psp::IsoImage image(iso);
            bool found = false;
            for (const auto &entry : image.entries()) {
                const std::string prefix = "/FUC/";
                if (entry.is_directory || entry.path.rfind(prefix, 0) != 0) continue;
                const fs::path target = folder / fs::path(entry.path.substr(prefix.size()));
                fs::create_directories(target.parent_path(), ec);
                const auto bytes = image.read_file(entry, 0, entry.size);
                std::ofstream out(target, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!out) return "FUComplete's files could not be written to " + folder.string() + ".";
                found = true;
            }
            if (!found) return "This copy has no FUComplete files. Add the image again from the Library.";
        } catch (const std::exception &) {
            return "Your FUComplete copy could not be read. Add it again from the Library.";
        }
    }
    std::vector<std::uint8_t> config;
    {
        std::ifstream in(config_path, std::ios::binary);
        config.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (!apply(config, options)) return "FUComplete's settings file (" + config_path.string() + ") is damaged.";
    std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(config.data()), static_cast<std::streamsize>(config.size()));
    if (!out) return "FUComplete's settings could not be saved to " + config_path.string() + ".";
    return {};
}

} // namespace mhf::launcher::fuc
