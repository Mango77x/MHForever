#include "hle.hpp"

#include "check.hpp"

#include <filesystem>
#include <string>

namespace {

using mhf::psp::hle::resolve_under_root;
const std::filesystem::path kRoot = "memstick";

} // namespace

int main() {
    // Plain paths stay under the root, whatever the separator.
    MHF_CHECK(resolve_under_root(kRoot, "/PSP/SAVEDATA/x") == kRoot / "PSP" / "SAVEDATA" / "x");
    MHF_CHECK(resolve_under_root(kRoot, "\\PSP\\x") == kRoot / "PSP" / "x");
    MHF_CHECK(resolve_under_root(kRoot, "/a/./b//c") == kRoot / "a" / "b" / "c");
    MHF_CHECK(resolve_under_root(kRoot, "") == kRoot);
    MHF_CHECK(resolve_under_root(kRoot, "/a/../b") == kRoot / "b");

    // Leaving the root is refused.
    MHF_CHECK(!resolve_under_root(kRoot, "/..").has_value());
    MHF_CHECK(!resolve_under_root(kRoot, "/a/../../b").has_value());

    // Drive letters and NTFS streams are refused on every platform (on Windows "C:" would replace the root).
    MHF_CHECK(!resolve_under_root(kRoot, "/C:/Windows/win.ini").has_value());
    MHF_CHECK(!resolve_under_root(kRoot, "/c:").has_value());
    MHF_CHECK(!resolve_under_root(kRoot, "/save/file.bin:stream").has_value());
    MHF_CHECK(!resolve_under_root(kRoot, "/a/b|c").has_value());
    MHF_CHECK(!resolve_under_root(kRoot, "/a/b?").has_value());
    return mhf::test::finish("test_hle_io");
}
