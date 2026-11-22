#include "game_files.hpp"
#include "data_bin.hpp"
#include "iso9660.hpp"

#include <fstream>
#include <stdexcept>

namespace mhf::mhfu {
std::optional<std::vector<std::vector<std::uint8_t>>> read_game_files(const std::filesystem::path &iso_path,
                                                                      const std::filesystem::path &executable,
                                                                      std::span<const std::uint32_t> ids, std::uint32_t limit) {
    try {
        std::ifstream file(executable, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0 || file.tellg() > 16 * 1024 * 1024) return {};
        std::vector<std::uint8_t> exe(static_cast<std::size_t>(file.tellg()));
        file.seekg(0);
        if (!file.read(reinterpret_cast<char *>(exe.data()), static_cast<std::streamsize>(exe.size()))) return {};
        psp::IsoImage iso(iso_path);
        const auto entry = iso.find("/PSP_GAME/USRDIR/DATA.BIN");
        if (!entry) return {};
        const auto data = DataBin::open(
            [&](std::uint64_t offset, std::size_t size) {
                if (size > limit) throw std::runtime_error("game file read exceeds limit");
                return iso.read_file(*entry, offset, size);
            },
            entry->size, find_substitution_table_candidates(exe));
        if (!data) return {};
        std::vector<std::vector<std::uint8_t>> out;
        for (const auto id : ids) {
            if (id >= data->files().size() || data->files()[id].size > limit) return {};
            out.push_back(data->read(id));
        }
        return out;
    } catch (const std::exception &) {
        return {}; // an unsupported or missing input: the caller keeps what the game shows
    }
}
} // namespace mhf::mhfu
