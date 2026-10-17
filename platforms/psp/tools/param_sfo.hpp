#pragma once

// PARAM.SFO (PSF) reader: the key/value table every PSP disc and save carries (DISC_ID, DISC_VERSION, TITLE...).

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mhf::psp {

struct ParamSfo {
    std::map<std::string, std::string> strings;
    std::map<std::string, std::uint32_t> integers;

    // Returns nothing when `data` is not a well-formed PSF table.
    static std::optional<ParamSfo> parse(const std::vector<std::uint8_t> &data);

    [[nodiscard]] std::string string(const std::string &key) const {
        const auto it = strings.find(key);
        return it == strings.end() ? std::string{} : it->second;
    }
};

} // namespace mhf::psp
