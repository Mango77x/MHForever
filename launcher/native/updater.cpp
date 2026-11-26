#include "updater.hpp"

#include "hash.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace mhf::launcher::update {

namespace fs = std::filesystem;

namespace {

constexpr char kBackupDir[] = ".mhforever-update-backup";
constexpr char kComplete[] = "complete";

// "v0.6.3-beta" -> {0, 6, 3}
std::vector<int> parts(std::string_view version) {
    if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) version.remove_prefix(1);
    version = version.substr(0, version.find_first_of("-+ "));
    std::vector<int> out;
    int value = 0;
    bool digits = false;
    for (const char c : version) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            digits = true;
        } else if (c == '.') {
            out.push_back(digits ? value : 0);
            value = 0;
            digits = false;
        } else {
            break;
        }
    }
    out.push_back(digits ? value : 0);
    return out;
}

bool ends_with(std::string_view text, std::string_view end) {
    return text.size() >= end.size() && text.substr(text.size() - end.size()) == end;
}

std::string lower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The system's curl or tar: both ship with Windows 10 and 11 (System32) and with every Linux desktop.
std::optional<fs::path> tool(const char *name) {
    std::error_code ec;
#if defined(_WIN32)
    const char *root = std::getenv("SystemRoot");
    const fs::path candidate = fs::path(root != nullptr ? root : "C:\\Windows") / "System32" / (std::string(name) + ".exe");
    if (fs::is_regular_file(candidate, ec)) return candidate;
#else
    for (const char *dir : {"/usr/bin", "/bin", "/usr/local/bin"}) {
        const fs::path candidate = fs::path(dir) / name;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
#endif
    return std::nullopt;
}

std::string read_text(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

// Runs a tool to its end. Returns an error message (with what the tool said), empty on success.
std::string run(const fs::path &program, const std::vector<std::string> &args, const fs::path &work,
                const std::function<void()> &while_running) {
    std::error_code ec;
    fs::create_directories(work, ec);
    const fs::path log = work / (platform::to_utf8(program.stem()) + ".log");
    std::string error;
    auto process = platform::Process::start(program, args, work, log, error);
    if (!process) return error;
    std::optional<int> code;
    while (!(code = process->poll())) {
        if (while_running) while_running();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    if (*code == 0) return {};
    const std::string said = read_text(log);
    return platform::to_utf8(program.stem()) + " failed (" + std::to_string(*code) + ")" + (said.empty() ? "" : ": " + said);
}

} // namespace

std::string archive_suffix() {
#if defined(_WIN32)
    return "windows-x64.zip";
#else
    return "linux-x64.tar.gz";
#endif
}

bool newer(std::string_view candidate, std::string_view current) {
    std::vector<int> a = parts(candidate), b = parts(current);
    const std::size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a > b;
}

std::optional<Release> pick(const Json &release, std::string_view suffix) {
    Release r;
    r.version = release["tag_name"].as_string();
    if (!r.version.empty() && (r.version.front() == 'v' || r.version.front() == 'V')) r.version.erase(0, 1);
    r.page = release["html_url"].as_string();
    if (r.version.empty() || !release["assets"].is_array()) return std::nullopt;
    for (const Json &asset : release["assets"].as_array()) {
        const std::string name = asset["name"].as_string();
        if (!ends_with(name, suffix)) continue;
        const std::string digest = lower(asset["digest"].as_string());
        if (digest.rfind("sha256:", 0) != 0 || digest.size() != 7 + 64) return std::nullopt;
        r.archive = name;
        r.archive_url = asset["browser_download_url"].as_string();
        r.archive_size = static_cast<std::uint64_t>(asset["size"].as_number());
        r.sha256 = digest.substr(7);
        const bool local = std::getenv("MHF_UPDATE_URL") != nullptr && r.archive_url.rfind("file://", 0) == 0; // tests
        if (r.archive_url.rfind("https://", 0) != 0 && !local) return std::nullopt;
        return r;
    }
    return std::nullopt;
}

std::string sha256_of(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Sha256 sha;
    std::vector<char> buffer(1 << 20);
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        if (in.gcount() > 0) sha.update(buffer.data(), static_cast<std::size_t>(in.gcount()));
    }
    const auto digest = sha.finish();
    return to_hex(digest.data(), digest.size());
}

std::string install(const fs::path &unpacked, const fs::path &install) {
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(unpacked, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec)) files.push_back(fs::relative(it->path(), unpacked, ec));
    }
    if (ec) return "Cannot read the unpacked update: " + ec.message();
    if (files.empty()) return "The update archive is empty.";
    // Reserve a directory on the same volume so a running Windows executable can be renamed.
    // An unmarked directory may contain the only copy of files from an interrupted update.
    const fs::path backup = install / kBackupDir;
    for (const fs::path &relative : files) {
        if (lower(platform::to_utf8(*relative.begin())) == kBackupDir) {
            return "The update archive contains the reserved backup directory.";
        }
    }
    fs::create_directories(install, ec);
    if (ec) return "Cannot open the installation directory: " + ec.message();
    if (!fs::create_directory(backup, ec)) {
        return "Cannot reserve the update backup directory " + platform::to_utf8(backup) +
               (ec ? ": " + ec.message() : ". A previous or active update still has backups there.");
    }

    struct Done {
        fs::path relative;
        bool had_old;
    };
    std::vector<Done> done;
    std::vector<fs::path> created_dirs;
    const auto undo = [&](std::string error) {
        bool restored = true;
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            const fs::path target = install / it->relative;
            std::error_code e;
            fs::remove(target, e);
            if (!e && it->had_old) fs::rename(backup / "files" / it->relative, target, e);
            if (e) restored = false;
        }
        for (auto it = created_dirs.rbegin(); it != created_dirs.rend(); ++it) {
            std::error_code e;
            fs::remove(*it, e); // removes only an empty directory
            if (e) restored = false;
        }
        if (restored) {
            std::error_code e;
            fs::remove_all(backup, e);
            if (!e) return error;
        }
        return error + " Automatic rollback or cleanup was incomplete. Check " + platform::to_utf8(backup) + ".";
    };

    std::sort(files.begin(), files.end());
    for (const fs::path &relative : files) {
        const fs::path target = install / relative;
        const fs::path old = backup / "files" / relative;
        fs::path parent = install;
        for (const auto &part : relative.parent_path()) {
            parent /= part;
            if (fs::create_directory(parent, ec)) created_dirs.push_back(parent);
            if (ec) return undo("Cannot write in " + platform::to_utf8(parent) + ": " + ec.message());
        }
        const fs::file_status status = fs::symlink_status(target, ec);
        if (ec && status.type() != fs::file_type::not_found) {
            return undo("Cannot check " + platform::to_utf8(relative) + ": " + ec.message());
        }
        const bool exists = fs::exists(status);
        if (exists) {
            if (!fs::is_regular_file(target, ec)) {
                return undo("Cannot replace " + platform::to_utf8(relative) + ": not a regular file.");
            }
            fs::create_directories(old.parent_path(), ec);
            if (!ec) fs::rename(target, old, ec);
            if (ec) return undo("Cannot replace " + platform::to_utf8(relative) + ": " + ec.message());
        }
        done.push_back({relative, exists});
        fs::copy_file(unpacked / relative, target, fs::copy_options::none, ec);
        if (ec) return undo("Cannot write " + platform::to_utf8(relative) + ": " + ec.message());
    }
    fs::create_directory(backup / kComplete, ec);
    if (ec) return undo("Cannot mark the update backup as complete: " + ec.message());
    return {};
}

void remove_old(const fs::path &install) {
    const fs::path backup = install / kBackupDir;
    const fs::path complete = backup / kComplete;
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(backup, ec)) ||
        !fs::is_directory(fs::symlink_status(complete, ec))) return;
    // Claim cleanup by removing the marker. A concurrent launcher must leave the directory alone.
    if (!fs::remove(complete, ec)) return;
    fs::remove_all(backup / "files", ec);
    if (!ec && fs::remove(backup, ec)) return;
    // For example, a previous launcher may still have one of the Windows files open.
    fs::create_directory(complete, ec);
}

std::string download(const std::string &url, const fs::path &to, std::uint64_t expected_size, const Progress &progress,
                     const fs::path &work) {
    const auto curl = tool("curl");
    if (!curl) return "curl was not found on this system.";
    std::error_code ec;
    fs::remove(to, ec);
    const std::vector<std::string> args = {"--fail", "--location", "--silent", "--show-error", "--retry", "2",
                                           "--connect-timeout", "20", "--user-agent", "MHForever-launcher",
                                           "--output", platform::to_utf8(to), url};
    return run(*curl, args, work, [&] {
        if (!progress || expected_size == 0) return;
        std::error_code e;
        const auto size = fs::file_size(to, e);
        if (!e) progress(std::min(1.0, static_cast<double>(size) / static_cast<double>(expected_size)));
    });
}

std::string unpack(const fs::path &archive, const fs::path &into, const fs::path &work) {
    const auto tar = tool("tar");
    if (!tar) return "tar was not found on this system.";
    std::error_code ec;
    fs::remove_all(into, ec);
    fs::create_directories(into, ec);
    if (ec) return "Cannot create " + platform::to_utf8(into) + ": " + ec.message();
    return run(*tar, {"-xf", platform::to_utf8(archive), "-C", platform::to_utf8(into)}, work, {});
}

std::string latest_release_url() {
    const char *test = std::getenv("MHF_UPDATE_URL");
    return test != nullptr && *test != '\0' ? std::string(test) : std::string(kLatestRelease);
}

std::optional<Release> check(const fs::path &work, std::string_view current, std::string &error) {
    const fs::path json = work / "latest.json";
    error = download(latest_release_url(), json, 0, {}, work);
    if (!error.empty()) return std::nullopt;
    const auto parsed = Json::parse(read_text(json));
    if (!parsed) {
        error = "GitHub's answer could not be read.";
        return std::nullopt;
    }
    auto release = pick(*parsed, archive_suffix());
    if (!release || !newer(release->version, current)) return std::nullopt;
    return release;
}

std::string apply(const Release &release, const fs::path &work, const fs::path &install, const Phase &phase) {
    const fs::path archive = work / release.archive, unpacked = work / "unpacked";
    std::error_code ec;
    phase("downloading", 0.0);
    if (std::string error = download(release.archive_url, archive, release.archive_size,
                                     [&](double f) { phase("downloading", f); }, work);
        !error.empty()) {
        return "The download failed. " + error;
    }
    phase("verifying", 1.0);
    if (sha256_of(archive) != release.sha256) {
        fs::remove(archive, ec);
        return "The download does not match the release (its SHA-256 differs). Nothing was changed.";
    }
    phase("installing", 1.0);
    if (std::string error = unpack(archive, unpacked, work); !error.empty()) {
        return "The update could not be unpacked. Nothing was changed. " + error;
    }
    // The archive holds one folder, MHForever-<version>.
    fs::path top = unpacked;
    std::vector<fs::path> entries;
    for (const auto &entry : fs::directory_iterator(unpacked, ec)) entries.push_back(entry.path());
    if (entries.size() == 1 && fs::is_directory(entries.front(), ec)) top = entries.front();
    if (std::string error = update::install(top, install); !error.empty()) {
        return error + " The new version can be downloaded from its release page.";
    }
    fs::remove(archive, ec);
    fs::remove_all(unpacked, ec);
    return {};
}

} // namespace mhf::launcher::update
