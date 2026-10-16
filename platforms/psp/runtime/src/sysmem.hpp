#pragma once

// Guest memory partitions and block allocation (SysMemUserForUser, thread stacks, module images).

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace mhf::psp {

class SysMem {
public:
    enum class Placement { Low, High, Address };

    static constexpr std::uint32_t kKernelPartitionStart = 0x08000000u;
    static constexpr std::uint32_t kUserPartitionStart = 0x08800000u;
    static constexpr std::uint32_t kAlignment = 256u;

    explicit SysMem(std::uint32_t ram_end);

    // Allocates `size` bytes in partition 1 (kernel) or 2 (user). Returns the block uid and address.
    struct Block {
        int uid{};
        int partition{};
        std::uint32_t address{};
        std::uint32_t size{};
        std::string name;
    };
    std::optional<Block> allocate(int partition, const std::string &name, Placement placement, std::uint32_t size,
                                  std::uint32_t address_or_alignment = 0);
    bool free(int uid);
    [[nodiscard]] const Block *find(int uid) const;

    // Reserves an exact range (a loaded module image). Fails if it overlaps another block.
    std::optional<Block> reserve(int partition, const std::string &name, std::uint32_t address, std::uint32_t size);

    [[nodiscard]] std::uint32_t max_free(int partition) const;
    [[nodiscard]] std::uint32_t total_free(int partition) const;

    void set_next_uid(int *counter) { uid_counter_ = counter; }

private:
    struct Range {
        std::uint32_t start;
        std::uint32_t end;
    };
    [[nodiscard]] std::optional<Range> partition_range(int partition) const;
    [[nodiscard]] bool overlaps(std::uint32_t start, std::uint32_t end) const;

    std::uint32_t ram_end_;
    std::map<std::uint32_t, Block> blocks_; // by address
    int *uid_counter_{};
    int own_counter_{0x100};
};

} // namespace mhf::psp
