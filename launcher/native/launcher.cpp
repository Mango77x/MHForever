#include "launcher.hpp"

#include "hash.hpp"
#include "install/data_dir.hpp"
#include "install/installer.hpp"
#include "iso9660.hpp"
#include "param_sfo.hpp"
#include "platform.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>

namespace mhf::launcher {

namespace fs = std::filesystem;

const std::vector<CatalogEntry> &catalog() {
    static const std::vector<CatalogEntry> entries = {
#include "catalog.inc"
    };
    return entries;
}

namespace {

const char *region_name(const std::string &market) {
    if (market == "EU") return "Europe";
    if (market == "US") return "North America";
    if (market == "JP") return "Japan";
    return "Unknown region";
}

#if defined(_WIN32)
constexpr const char *kGameProgram = "mhfu.exe";
#else
constexpr const char *kGameProgram = "mhfu";
#endif

std::optional<std::string> sha1_file(const fs::path &path, std::uint64_t size, const Progress *progress) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    Sha1 sha;
    std::vector<char> buf(4u << 20);
    std::uint64_t done = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) break;
        sha.update(buf.data(), n);
        done += n;
        if (progress && progress->report && size > 0) progress->report("hashing", static_cast<double>(done) / static_cast<double>(size));
    }
    const auto digest = sha.finish();
    return to_hex(digest.data(), digest.size());
}

Json default_settings() {
    Json s;
    s["scale"] = 4;
    s["fullscreen"] = false;
    s["smoothTextures"] = false;
    s["copyImage"] = false; // keep a copy of the disc image in the data directory
    s["musicVolume"] = 100;
    s["sfxVolume"] = 100;
    return s;
}

} // namespace

Launcher::Launcher(fs::path base_dir) : Launcher(std::move(base_dir), install::user_data_dir()) {}

Launcher::Launcher(fs::path base_dir, fs::path data_dir)
    : base_(std::move(base_dir)), file_(base_ / "launcher.json"), data_dir_(std::move(data_dir)),
      decryptor_(install::builtin_decryptor()) {
    load();
}

void Launcher::load() {
    std::ifstream in(file_, std::ios::binary);
    if (in) {
        std::stringstream ss;
        ss << in.rdbuf();
        if (auto parsed = Json::parse(ss.str()); parsed && parsed->is_object()) data_ = std::move(*parsed);
    }
    if (!data_.is_object()) data_ = Json(Json::Object{});
    bool changed = false;
    if (data_["identitySeed"].as_string().size() != 64) {
        std::uint8_t seed[32];
        platform::random_bytes(seed, sizeof seed);
        data_["identitySeed"] = to_hex(seed, sizeof seed);
        changed = true;
    }
    if (data_["pseudonym"].as_string().empty()) {
        data_["pseudonym"] = "Hunter";
        changed = true;
    }
    Json settings = default_settings();
    for (const auto &[k, v] : data_["settings"].as_object()) settings[k] = v;
    data_["settings"] = settings;
    if (!data_["games"].is_array()) data_["games"] = Json(Json::Array{});
    if (!data_["stats"].is_object()) {
        Json stats;
        stats["secondsPlayed"] = 0;
        stats["sessions"] = 0;
        stats["lastPlayed"] = nullptr;
        data_["stats"] = stats;
    }
    if (changed) save();
}

void Launcher::save() {
    const fs::path tmp = file_.string() + ".tmp";
    bool written = false;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << data_.dump(true) << '\n';
        out.flush();
        out.close();
        written = static_cast<bool>(out);
    }
    std::error_code ec;
    if (!written) {
        // Full disk or no permission: keep the previous launcher.json (it holds the identity seed) as it is.
        if (fs::is_regular_file(tmp, ec)) fs::remove(tmp, ec);
        return;
    }
    fs::rename(tmp, file_, ec);
    if (!ec) return;
    // rename() normally replaces the old file. If this filesystem refuses, move the old one aside and put it back
    // when the new one cannot be placed.
    fs::path backup = file_;
    backup += ".old";
    fs::remove(backup, ec);
    fs::rename(file_, backup, ec);
    std::error_code move_ec;
    fs::rename(tmp, file_, move_ec);
    if (move_ec) {
        fs::rename(backup, file_, ec);
        fs::remove(tmp, ec);
        return;
    }
    fs::remove(backup, ec);
}

// Friend code: 80 bits of SHA-256(identity seed) in Crockford base32, XXXX-XXXX-XXXX-XXXX. Provisional until online
// play derives it from the Ed25519 public key (docs/08-multiplayer.md §4.1).
std::string Launcher::friend_code() const {
    const std::string seed_hex = data_["identitySeed"].as_string();
    Sha256 sha;
    sha.update(seed_hex.data(), seed_hex.size());
    const auto d = sha.finish();
    static const char *alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string code;
    std::uint64_t acc = 0;
    int bits = 0;
    std::size_t byte = 0;
    while (code.size() < 16) {
        if (bits < 5) {
            acc = (acc << 8) | d[byte++];
            bits += 8;
        }
        code.push_back(alphabet[(acc >> (bits - 5)) & 31]);
        bits -= 5;
    }
    return code.substr(0, 4) + "-" + code.substr(4, 4) + "-" + code.substr(8, 4) + "-" + code.substr(12, 4);
}

const CatalogEntry *Launcher::entry_for(const std::string &disc_id) const {
    for (const auto &e : catalog()) {
        if (disc_id == e.disc_id) return &e;
    }
    return nullptr;
}

fs::path Launcher::find_executable(const CatalogEntry &entry) {
    std::vector<fs::path> candidates;
    const std::string configured = data_["executablePath"].as_string();
    if (!configured.empty()) candidates.push_back(platform::from_utf8(configured));
    candidates.push_back(install::executable_path(data_dir_, entry.disc_id)); // prepared by the installer
    const std::string name = std::string(entry.disc_id) + "_EBOOT.BIN";
    candidates.push_back(base_ / ".." / "exe" / name);
    candidates.push_back(base_ / "exe" / name);
    candidates.push_back(base_ / name);
    for (const auto &g : data_["games"].as_array()) {
        if (g["discId"].as_string() != entry.disc_id) continue;
        const fs::path iso = platform::from_utf8(g["isoPath"].as_string());
        candidates.push_back(iso.parent_path() / name);
        candidates.push_back(iso.parent_path() / ".." / "exe" / name);
    }
    for (const auto &c : candidates) {
        std::error_code ec;
        if (!fs::is_regular_file(c, ec)) continue;
        if (entry.dev_executable_size != 0 && fs::file_size(c, ec) != entry.dev_executable_size) continue;
        return fs::weakly_canonical(c, ec);
    }
    return {};
}

Json Launcher::state() {
    std::lock_guard lock(mutex_);
    Json s;
    s["version"] = MHF_VERSION_STRING;
#if defined(_WIN32)
    s["platform"] = "windows";
#else
    s["platform"] = "linux";
#endif
    s["pseudonym"] = data_["pseudonym"];
    s["friendCode"] = friend_code();
    s["settings"] = data_["settings"];
    s["stats"] = data_["stats"];
    Json games(Json::Array{});
    const CatalogEntry *playable = nullptr;
    for (const auto &g : data_["games"].as_array()) {
        const CatalogEntry *e = entry_for(g["discId"].as_string());
        if (e == nullptr) continue;
        Json out;
        out["id"] = e->game;
        out["title"] = e->title;
        out["region"] = region_name(e->market);
        out["discId"] = e->disc_id;
        out["version"] = e->version;
        out["isoPath"] = g["isoPath"];
        out["verification"] = g["verification"];
        out["supported"] = std::string(e->status) != "planned";
        if (out["supported"].as_bool() && playable == nullptr) playable = e;
        games.array().push_back(out);
    }
    s["games"] = games;
    Json exe;
    const fs::path found = playable ? find_executable(*playable) : fs::path{};
    exe["path"] = platform::to_utf8(found);
    exe["found"] = !found.empty() || playable == nullptr;
    s["executable"] = exe;
    return s;
}

void Launcher::set_settings(const Json &partial) {
    std::lock_guard lock(mutex_);
    Json &s = data_["settings"];
    const auto num = [&](const char *key, double lo, double hi) {
        if (partial[key].is_number()) s[key] = std::clamp(std::round(partial[key].as_number()), lo, hi);
    };
    num("scale", 1, 4);
    num("musicVolume", 0, 100);
    num("sfxVolume", 0, 100);
    for (const char *key : {"fullscreen", "smoothTextures", "copyImage"}) {
        if (partial[key].is_bool()) s[key] = partial[key];
    }
    save();
}

void Launcher::set_pseudonym(const std::string &name) {
    std::string clean;
    for (const char c : name) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7F) clean.push_back(c);
    }
    if (clean.size() > 48) clean.resize(48); // UTF-8 bytes; the UI limits to 16 characters
    if (clean.empty()) return;
    std::lock_guard lock(mutex_);
    data_["pseudonym"] = clean;
    save();
}

Launcher::AddResult Launcher::add_game(const fs::path &iso, const Progress &progress) {
    AddResult result;
    std::error_code ec;
    const std::uint64_t size = fs::file_size(iso, ec);
    if (ec) {
        result.error = "The file could not be read.";
        return result;
    }
    if (progress.report) progress.report("reading", 0);
    std::string disc_id, version;
    try {
        psp::IsoImage image(iso);
        const auto sfo_entry = image.find("/PSP_GAME/PARAM.SFO");
        if (!sfo_entry) throw std::runtime_error("no PARAM.SFO");
        const auto sfo = psp::ParamSfo::parse(image.read_file(*sfo_entry, 0, sfo_entry->size));
        if (!sfo) throw std::runtime_error("bad PARAM.SFO");
        disc_id = sfo->string("DISC_ID");
        version = sfo->string("DISC_VERSION");
    } catch (const std::exception &) {
        result.error = "This file is not a PSP disc image. Choose the .iso you made from your own UMD.";
        return result;
    }
    const CatalogEntry *entry = entry_for(disc_id);
    if (entry == nullptr) {
        result.error = "This disc (" + disc_id + ") is not one MHForever can run.";
        return result;
    }
    bool size_known = false;
    for (const auto &d : entry->dumps) size_known |= d.size == size;
    std::string verification = "unverified";
    if (size_known) {
        const auto sha1 = sha1_file(iso, size, &progress);
        if (!sha1) {
            result.error = "The file could not be read.";
            return result;
        }
        for (const auto &d : entry->dumps) {
            if (*sha1 == d.sha1) verification = "verified";
        }
    } else if (progress.report) {
        progress.report("hashing", 1);
    }
    // Set the game up from the image: prepare its executable (unless a usable one is already known, as in developer
    // builds) and keep a copy of the image when the player chose to. Planned regions are only recognised.
    fs::path stored_iso = iso;
    if (std::string(entry->status) != "planned") {
        bool have_executable = false;
        bool copy_image = false;
        {
            std::lock_guard lock(mutex_);
            have_executable = !find_executable(*entry).empty();
            copy_image = data_["settings"]["copyImage"].as_bool();
        }
        install::InstallOptions options;
        options.prepare_executable = !have_executable;
        options.copy_image = copy_image;
        if (options.prepare_executable || options.copy_image) {
            auto installed = install::install_from_image(iso, *entry, data_dir_, options, decryptor_, progress);
            if (installed.error != install::InstallError::None) {
                result.error = installed.message;
                return result;
            }
            stored_iso = installed.image;
        }
    }
    {
        std::lock_guard lock(mutex_);
        auto &games = data_["games"].array();
        games.erase(std::remove_if(games.begin(), games.end(),
                                   [&](const Json &g) { return g["discId"].as_string() == entry->disc_id; }),
                    games.end());
        Json g;
        g["discId"] = entry->disc_id;
        g["isoPath"] = platform::to_utf8(fs::weakly_canonical(stored_iso, ec));
        g["verification"] = verification;
        games.insert(games.begin(), g);
        save();
    }
    Json out;
    out["id"] = entry->game;
    out["title"] = entry->title;
    out["region"] = region_name(entry->market);
    out["discId"] = entry->disc_id;
    out["version"] = version.empty() ? std::string(entry->version) : version;
    out["isoPath"] = platform::to_utf8(stored_iso);
    out["verification"] = verification;
    out["supported"] = std::string(entry->status) != "planned";
    result.game = out;
    return result;
}

void Launcher::remove_game(const std::string &id) {
    std::lock_guard lock(mutex_);
    auto &games = data_["games"].array();
    std::vector<std::string> removed; // disc ids whose installed data goes with them
    for (const auto &g : games) {
        const CatalogEntry *e = entry_for(g["discId"].as_string());
        if (e != nullptr && id == e->game) removed.emplace_back(e->disc_id);
    }
    games.erase(std::remove_if(games.begin(), games.end(),
                               [&](const Json &g) {
                                   const CatalogEntry *e = entry_for(g["discId"].as_string());
                                   return e == nullptr || id == e->game;
                               }),
                games.end());
    for (const auto &disc_id : removed) {
        std::error_code ec;
        fs::remove_all(install::game_dir(data_dir_, disc_id), ec); // only our own folder; never the player's image
    }
    save();
}

bool Launcher::set_executable(const fs::path &path) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path, ec);
    if (ec) return false;
    for (const auto &e : catalog()) {
        if (e.dev_executable_size == 0 || e.dev_executable_size != size) continue;
        const auto sha1 = sha1_file(path, size, nullptr);
        if (sha1 && *sha1 == e.dev_executable_sha1) {
            data_["executablePath"] = platform::to_utf8(fs::weakly_canonical(path, ec));
            save();
            return true;
        }
    }
    return false;
}

Launcher::LaunchPlan Launcher::plan_launch(const std::string &game_id) {
    std::lock_guard lock(mutex_);
    LaunchPlan plan;
    plan.cwd = base_;
    plan.program = base_ / kGameProgram;
    plan.log = base_ / "game.log";
    std::error_code ec;
    if (!fs::is_regular_file(plan.program, ec)) {
        plan.error = std::string("The game files are missing: ") + kGameProgram + " should be next to the launcher.";
        return plan;
    }
    const Json *game = nullptr;
    const CatalogEntry *entry = nullptr;
    for (const auto &g : data_["games"].as_array()) {
        const CatalogEntry *e = entry_for(g["discId"].as_string());
        if (e != nullptr && game_id == e->game && std::string(e->status) != "planned") {
            game = &g;
            entry = e;
            break;
        }
    }
    if (game == nullptr) {
        plan.error = "Add your copy of the game first.";
        return plan;
    }
    const fs::path iso = platform::from_utf8((*game)["isoPath"].as_string());
    if (!fs::is_regular_file(iso, ec)) {
        plan.error = "Your copy is no longer where it was (" + platform::to_utf8(iso) +
                     "). Add it again from the Library.";
        return plan;
    }
    const fs::path exe = find_executable(*entry);
    if (exe.empty()) {
        plan.error = "The decrypted executable was not found (developer build). Locate it in Settings.";
        return plan;
    }
    const Json &s = data_["settings"];
    const auto num = [&](const char *key) { return std::to_string(static_cast<int>(s[key].as_number())); };
    plan.args = {"--iso", platform::to_utf8(iso), "--exe", platform::to_utf8(exe), "--memstick",
                 platform::to_utf8(base_ / "memstick"), "--window", "--scale", num("scale"), "--name",
                 data_["pseudonym"].as_string(), "--music-volume", num("musicVolume"), "--sfx-volume", num("sfxVolume")};
    if (s["fullscreen"].as_bool()) plan.args.push_back("--fullscreen");
    if (s["smoothTextures"].as_bool()) plan.args.push_back("--smooth-textures");
    fs::create_directories(base_ / "memstick", ec);
    return plan;
}

void Launcher::record_session(std::int64_t seconds) {
    std::lock_guard lock(mutex_);
    Json &stats = data_["stats"];
    stats["secondsPlayed"] = static_cast<std::int64_t>(stats["secondsPlayed"].as_number()) + seconds;
    stats["sessions"] = static_cast<std::int64_t>(stats["sessions"].as_number()) + 1;
    stats["lastPlayed"] = static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    save();
}

Json Launcher::identity_backup() {
    std::lock_guard lock(mutex_);
    Json b;
    b["format"] = "mhforever-identity";
    b["version"] = 1;
    b["pseudonym"] = data_["pseudonym"];
    b["identitySeed"] = data_["identitySeed"];
    b["friendCode"] = friend_code();
    b["warning"] = "Keep this file private: it is your identity. Anyone with it can pretend to be you.";
    return b;
}

} // namespace mhf::launcher
