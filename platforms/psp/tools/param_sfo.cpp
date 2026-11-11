#include "param_sfo.hpp"

#include <algorithm>

namespace mhf::psp {

namespace {
std::uint32_t le32(const std::vector<std::uint8_t> &d, std::size_t at) {
    return static_cast<std::uint32_t>(d[at]) | static_cast<std::uint32_t>(d[at + 1]) << 8u |
           static_cast<std::uint32_t>(d[at + 2]) << 16u | static_cast<std::uint32_t>(d[at + 3]) << 24u;
}
std::uint16_t le16(const std::vector<std::uint8_t> &d, std::size_t at) {
    return static_cast<std::uint16_t>(d[at] | d[at + 1] << 8u);
}
} // namespace

std::optional<ParamSfo> ParamSfo::parse(const std::vector<std::uint8_t> &d) {
    if (d.size() < 20 || d[0] != 0 || d[1] != 'P' || d[2] != 'S' || d[3] != 'F') return std::nullopt;
    const std::uint32_t keys = le32(d, 8), data = le32(d, 12), count = le32(d, 16);
    if (count > 256 || 20u + static_cast<std::size_t>(count) * 16u > d.size() || keys > d.size() || data > d.size()) {
        return std::nullopt;
    }
    ParamSfo out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t e = 20u + static_cast<std::size_t>(i) * 16u;
        const std::size_t key_at = keys + le16(d, e);
        const std::uint16_t format = le16(d, e + 2);
        const std::uint32_t length = le32(d, e + 4);
        const std::size_t value_at = static_cast<std::size_t>(data) + le32(d, e + 12);
        if (key_at >= d.size() || value_at > d.size() || length > d.size() - value_at) return std::nullopt;
        std::string key;
        for (std::size_t k = key_at; k < d.size() && d[k] != 0; ++k) key.push_back(static_cast<char>(d[k]));
        if (format == 0x0404 && length >= 4) {
            out.integers[key] = le32(d, value_at);
        } else {
            // A NUL-terminated string (0x0204) runs to its NUL within its reserved space, as the PSP reads it: a patched
            // disc may write a longer value and keep the old length (FUComplete's TITLE: length 30, 44 characters).
            std::size_t size = length;
            const auto *begin = d.data() + value_at;
            if (format == 0x0204 && std::find(begin, begin + length, std::uint8_t{0}) == begin + length) {
                const std::size_t reserved = std::min<std::size_t>(le32(d, e + 8), d.size() - value_at);
                while (size < reserved && d[value_at + size] != 0) ++size;
            }
            std::string value(reinterpret_cast<const char *>(d.data() + value_at), size);
            while (!value.empty() && value.back() == '\0') value.pop_back();
            out.strings[key] = value;
        }
    }
    return out;
}

} // namespace mhf::psp
