#include "install/data_dir.hpp"

#include <cstdlib>
#include <optional>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#endif

namespace mhf::launcher::install {

namespace fs = std::filesystem;

namespace {

constexpr const char *kAppName = "MHForever";

std::optional<fs::path> env_path(const char *name) {
#if defined(_WIN32)
    std::wstring wide_name;
    for (const char *p = name; *p != '\0'; ++p) wide_name.push_back(static_cast<wchar_t>(*p));
    DWORD n = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wide_name.c_str(), value.data(), n);
    value.resize(n);
    if (value.empty()) return std::nullopt;
    return fs::path(value);
#else
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0') return std::nullopt;
    return fs::path(value);
#endif
}

} // namespace

fs::path user_data_dir() {
    if (const auto override_dir = env_path("MHF_DATA_DIR")) return *override_dir;
#if defined(_WIN32)
    if (const auto appdata = env_path("APPDATA")) return *appdata / kAppName;
    if (const auto profile = env_path("USERPROFILE")) return *profile / "AppData" / "Roaming" / kAppName;
#else
    if (const auto xdg = env_path("XDG_DATA_HOME")) return *xdg / kAppName;
    if (const auto home = env_path("HOME")) return *home / ".local" / "share" / kAppName;
#endif
    // Last resort (no home directory at all): next to wherever we run, as an absolute path.
    std::error_code ec;
    const fs::path here = fs::current_path(ec);
    return (ec ? fs::path(".") : here) / "data";
}

fs::path user_saves_dir() {
    if (const auto override_dir = env_path("MHF_SAVES_DIR")) return *override_dir;
#if defined(_WIN32)
    PWSTR documents = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr) {
        const fs::path folder(documents);
        CoTaskMemFree(documents);
        if (!folder.empty()) return folder / kAppName / "saves";
    } else if (documents != nullptr) {
        CoTaskMemFree(documents);
    }
#endif
    return user_data_dir() / "saves";
}

fs::path game_dir(const fs::path &data_dir, std::string_view disc_id) { return data_dir / "games" / std::string(disc_id); }
fs::path executable_path(const fs::path &data_dir, std::string_view disc_id) { return game_dir(data_dir, disc_id) / "executable.elf"; }
fs::path image_path(const fs::path &data_dir, std::string_view disc_id) { return game_dir(data_dir, disc_id) / "image.iso"; }

} // namespace mhf::launcher::install
