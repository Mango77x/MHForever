#include "iso9660.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace mhf::psp {

namespace {

std::uint32_t read_le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::string upper(std::string_view s) {
    std::string out{s};
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

constexpr int kMaxDepth = 16;

} // namespace

IsoImage::IsoImage(const std::filesystem::path& path) : file_(path, std::ios::binary) {
    if (!file_) {
        throw std::runtime_error("cannot open image: " + path.string());
    }
    file_.seekg(0, std::ios::end);
    image_size_ = static_cast<std::uint64_t>(file_.tellg());

    const auto pvd = read_bytes(16ull * kSectorSize, kSectorSize);
    if (pvd[0] != 1 || std::string_view(reinterpret_cast<const char*>(pvd.data() + 1), 5) != "CD001") {
        throw std::runtime_error("not an ISO9660 image (no primary volume descriptor)");
    }
    const std::uint8_t* root = pvd.data() + 156;
    walk_directory(read_le32(root + 2), read_le32(root + 10), "", 0);
}

void IsoImage::walk_directory(std::uint32_t lba, std::uint32_t size, const std::string& prefix, int depth) {
    if (depth > kMaxDepth) {
        throw std::runtime_error("ISO directory tree too deep");
    }
    const auto data = read_bytes(static_cast<std::uint64_t>(lba) * kSectorSize, size);
    std::size_t pos = 0;
    while (pos < data.size()) {
        const std::uint8_t record_length = data[pos];
        if (record_length == 0) {
            // Records never cross sector boundaries; skip to the next sector.
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (record_length < 34 || pos + record_length > data.size()) {
            throw std::runtime_error("malformed ISO directory record");
        }
        const std::uint8_t* rec = data.data() + pos;
        const std::uint8_t name_length = rec[32];
        if (33u + name_length > record_length) {
            throw std::runtime_error("malformed ISO directory record name");
        }
        pos += record_length;

        std::string name(reinterpret_cast<const char*>(rec + 33), name_length);
        if (name_length == 1 && (name[0] == '\0' || name[0] == '\1')) {
            continue; // "." and ".."
        }
        if (const auto semicolon = name.find(';'); semicolon != std::string::npos) {
            name.resize(semicolon);
        }
        Entry entry;
        entry.path = prefix + "/" + name;
        entry.lba = read_le32(rec + 2);
        entry.size = read_le32(rec + 10);
        entry.is_directory = (rec[25] & 0x02) != 0;
        entries_.push_back(entry);
        if (entry.is_directory) {
            walk_directory(entry.lba, entry.size, entry.path, depth + 1);
        }
    }
}

std::optional<IsoImage::Entry> IsoImage::find(std::string_view path) const {
    const std::string wanted = upper(path);
    for (const auto& entry : entries_) {
        if (upper(entry.path) == wanted) {
            return entry;
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> IsoImage::read_bytes(std::uint64_t offset, std::size_t size) {
    if (offset > image_size_ || size > image_size_ - offset) {
        throw std::runtime_error("read past the end of the image");
    }
    std::vector<std::uint8_t> buffer(size);
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    if (!file_) {
        throw std::runtime_error("I/O error while reading the image");
    }
    return buffer;
}

std::vector<std::uint8_t> IsoImage::read_file(const Entry& entry, std::uint64_t offset, std::size_t size) {
    if (offset > entry.size || size > entry.size - offset) {
        throw std::runtime_error("read past the end of " + entry.path);
    }
    return read_bytes(static_cast<std::uint64_t>(entry.lba) * kSectorSize + offset, size);
}

} // namespace mhf::psp
