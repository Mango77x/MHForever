#include "launcher.hpp"

#include "hash.hpp"
#include "host/keymap.hpp"
#include "host/padmap.hpp"
#include "install/data_dir.hpp"
#include "install/installer.hpp"
#include "iso9660.hpp"
#include "param_sfo.hpp"
#include "platform.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
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
constexpr const char *kProgramSuffix = ".exe";
#else
constexpr const char *kProgramSuffix = "";
#endif

// One program per region: the recompiled code is the game's own, so it differs between releases. The European one keeps
// the plain name (mhfu), the others add the market (mhfu-us).
std::string game_program_name(const CatalogEntry &e) {
    std::string name = e.game;
    if (std::string(e.market) != "EU") {
        name += '-';
        for (const char c : std::string(e.market)) name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return name + kProgramSuffix;
}

// The copy of `game_id` the player chose (launcher.json "selectedDisc"), else the first supported one.
const Json *chosen_copy(const Json &data, const std::string &game_id, const CatalogEntry **entry_out) {
    const Json *first = nullptr;
    const CatalogEntry *first_entry = nullptr;
    for (const auto &g : data["games"].as_array()) {
        const CatalogEntry *e = nullptr;
        for (const auto &c : catalog()) {
            if (g["discId"].as_string() == c.disc_id) e = &c;
        }
        if (e == nullptr || game_id != e->game || std::string(e->status) == "planned") continue;
        if (g["discId"].as_string() == data["selectedDisc"].as_string()) {
            *entry_out = e;
            return &g;
        }
        if (first == nullptr) {
            first = &g;
            first_entry = e;
        }
    }
    *entry_out = first_entry;
    return first;
}

// The launcher's own settings: everything else is kept per copy of a game.
bool is_launcher_setting(const std::string &key) { return key == "copyImage" || key == "adhocServer" || key == "checkUpdates"; }

// What a copy of a game uses: its own settings, over what the launcher.json "settings" hold (the values every copy started
// from, and what a launcher.json from before the per-copy settings has).
Json settings_for(const Json &data, const std::string &disc_id) {
    Json s = data["settings"];
    const Json &own = data["gameSettings"][disc_id];
    if (own.is_object()) {
        for (const auto &[k, v] : own.as_object()) s[k] = v;
    }
    return s;
}

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
    s["fps"] = 30; // output rate: the game's own 30, or a multiple of it up to 180 (in-between pictures)
    s["copyImage"] = false; // keep a copy of the disc image in the data directory
    s["checkUpdates"] = true; // ask GitHub for a newer release when the launcher starts
    s["musicVolume"] = 100;
    s["sfxVolume"] = 100;
    // Keyboard bindings (docs/07-launcher-ux.md section 5). Values are UI Events codes, so they name the key's
    // position rather than the character printed on it, and they are the same on every keyboard layout.
    const host::KeyBindings defaults;
    Json keys;
    for (const std::string_view action : host::kKeyActions) {
        if (const std::string *key = host::key_binding(defaults, action)) keys[std::string(action)] = *key;
    }
    s["keys"] = keys;
    // Controller bindings: SDL gamepad names (docs/07-launcher-ux.md section 5), joined with '+' when several press the button.
    const host::PadBindings pad_defaults;
    Json pad;
    for (const std::string_view action : host::kPadActions) {
        if (const std::string *source = host::pad_binding(pad_defaults, action)) pad[std::string(action)] = *source;
    }
    s["pad"] = pad;
    s["adhocServer"] = ""; // PSP ad hoc server for online play ("host" or "host:port"); empty: offline
    return s;
}

// The bindings a launcher.json holds; actions it does not mention keep their default.
host::KeyBindings key_bindings_from(const Json &keys) {
    host::KeyBindings bindings;
    for (const std::string_view action : host::kKeyActions) {
        const Json &value = keys[action];
        if (!value.is_string()) continue;
        if (std::string *slot = host::key_binding(bindings, action)) *slot = value.as_string();
    }
    return bindings;
}

// The controller bindings a launcher.json holds; actions it does not mention keep their default.
host::PadBindings pad_bindings_from(const Json &pad) {
    host::PadBindings bindings;
    for (const std::string_view action : host::kPadActions) {
        const Json &value = pad[action];
        if (!value.is_string()) continue;
        if (std::string *slot = host::pad_binding(bindings, action)) *slot = value.as_string();
    }
    return bindings;
}

// "host" or "host:port": letters, digits, dots, dashes and underscores, and a port from 1 to 65535. Empty is "offline".
bool valid_server(const std::string &text) {
    if (text.empty()) return true;
    if (text.size() > 253) return false;
    const std::size_t colon = text.find(':');
    const std::string host = text.substr(0, colon);
    if (host.empty()) return false;
    if (!std::all_of(host.begin(), host.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' || c == '_'; })) {
        return false;
    }
    if (colon == std::string::npos) return true;
    const std::string port = text.substr(colon + 1);
    if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    const int value = std::stoi(port);
    return value >= 1 && value <= 65535;
}

// An empty string is a pad button the player left without a source; otherwise every part must be a known one. The stick
// is "left", "right" or empty.
bool valid_pad_value(std::string_view action, const std::string &value) {
    if (value.empty()) return true;
    if (action == "stick") return value == "left" || value == "right";
    std::size_t start = 0;
    int parts = 0;
    while (start <= value.size()) {
        const std::size_t plus = value.find('+', start);
        const std::string_view part = std::string_view(value).substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        if (++parts > 4 || std::find(host::kPadSources.begin(), host::kPadSources.end(), part) == host::kPadSources.end()) return false;
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    return true;
}

// An empty string is a button the player left without a key; anything else must look like a UI Events code.
bool valid_key_code(const std::string &code) {
    if (code.size() > 24) return false;
    return std::all_of(code.begin(), code.end(),
                       [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
}

} // namespace

Launcher::Launcher(fs::path base_dir)
    : Launcher(std::move(base_dir), install::user_data_dir(), install::user_saves_dir()) {}

Launcher::Launcher(fs::path base_dir, fs::path data_dir) : Launcher(std::move(base_dir), data_dir, data_dir / "saves") {}

Launcher::Launcher(fs::path base_dir, fs::path data_dir, fs::path saves_dir)
    : base_(std::move(base_dir)), file_(data_dir / "launcher.json"), data_dir_(std::move(data_dir)),
      default_saves_dir_(saves_dir), saves_dir_(std::move(saves_dir)), decryptor_(install::builtin_decryptor()) {
    load();
    if (const std::string custom = data_["savesFolder"].as_string(); !custom.empty()) saves_dir_ = platform::from_utf8(custom);
    import_game_controls();
}

Launcher::SavesFolderChange Launcher::set_saves_folder(const fs::path &folder) {
    std::lock_guard lock(mutex_);
    SavesFolderChange result;
    const fs::path target = folder.empty() ? default_saves_dir_ : folder;
    std::error_code ec;
    const bool same = fs::exists(target, ec) && fs::exists(saves_dir_, ec) && fs::equivalent(target, saves_dir_, ec);
    if (!same) {
        // A folder inside the current one would be copied into itself.
        const fs::path from = fs::weakly_canonical(saves_dir_, ec), to = fs::weakly_canonical(target, ec);
        if (std::mismatch(from.begin(), from.end(), to.begin(), to.end()).first == from.end()) {
            result.error = "That folder is inside the current saves folder. Choose one outside it.";
            return result;
        }
        fs::create_directories(target, ec);
        if (ec || !fs::is_directory(target, ec)) {
            result.error = "Cannot use that folder: " + (ec ? ec.message() : std::string("it is not a folder"));
            return result;
        }
        if (fs::is_directory(saves_dir_, ec)) {
            for (const auto &game : fs::directory_iterator(saves_dir_, ec)) {
                std::error_code item;
                if (!game.is_directory(item)) continue;
                const fs::path destination = target / game.path().filename();
                if (fs::exists(destination, item)) continue; // that game already has saves there: they stay as they are
                fs::copy(game.path(), destination, fs::copy_options::recursive, item);
                if (item) {
                    result.error = "Could not copy the saves of " + platform::to_utf8(game.path().filename()) + ": " + item.message() +
                                   ". Your saves are still in the old folder.";
                    return result;
                }
                ++result.copied;
            }
        }
    }
    if (folder.empty()) data_.object().erase("savesFolder");
    else data_["savesFolder"] = platform::to_utf8(target);
    saves_dir_ = target;
    save();
    result.changed = true;
    return result;
}

void Launcher::import_legacy_saves(const std::string &disc_id) {
    std::error_code ec;
    const fs::path old = base_ / "memstick";
    if (!fs::is_directory(old / "PSP" / "SAVEDATA", ec)) return;
    const fs::path stick = install::game_saves_dir(saves_dir_, disc_id);
    if (fs::exists(stick, ec) && fs::equivalent(old, stick, ec)) return;
    const std::string source = platform::to_utf8(fs::weakly_canonical(old, ec));
    const fs::path marker = stick / ".imported";
    {
        std::ifstream in(marker);
        for (std::string line; std::getline(in, line);) {
            if (line == source) return; // already imported from that folder
        }
    }
    fs::create_directories(stick, ec);
    if (ec) return;
    fs::copy(old, stick, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
    if (ec) return; // try again next time; nothing was overwritten
    std::ofstream(marker, std::ios::app) << source << '\n';
}

namespace {

std::string upper_ascii(std::string s) {
    for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// A PSP game id: four letters and five digits (ULES01213).
bool looks_like_game_id(const std::string &name) {
    if (name.size() < 9) return false;
    for (int i = 0; i < 4; ++i) {
        if (!std::isalpha(static_cast<unsigned char>(name[static_cast<std::size_t>(i)]))) return false;
    }
    for (int i = 4; i < 9; ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[static_cast<std::size_t>(i)]))) return false;
    }
    return true;
}

std::optional<psp::ParamSfo> read_save_sfo(const fs::path &dir) {
    std::ifstream in(dir / "PARAM.SFO", std::ios::binary);
    if (!in) return std::nullopt;
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return psp::ParamSfo::parse(bytes);
}

} // namespace

Launcher::SaveScan Launcher::scan_saves(const std::string &disc_id, const fs::path &source) const {
    SaveScan scan;
    std::error_code ec;
    const fs::path mine = install::game_saves_dir(saves_dir_, disc_id) / "PSP" / "SAVEDATA";
    const std::string wanted = upper_ascii(disc_id);
    std::set<std::string> seen, other_versions;
    const auto consider = [&](const fs::path &dir) {
        const auto sfo = read_save_sfo(dir);
        if (!sfo) return;
        const std::string name = platform::to_utf8(dir.filename());
        const std::string upper = upper_ascii(name);
        if (upper.compare(0, wanted.size(), wanted) == 0) {
            if (!seen.insert(upper).second) return;
            SaveCandidate c;
            c.folder = name;
            c.path = dir;
            c.title = sfo->string("SAVEDATA_TITLE");
            c.exists = fs::is_directory(mine / name, ec);
            scan.found.push_back(std::move(c));
        } else if (looks_like_game_id(upper) && upper_ascii(sfo->string("TITLE")).find("MONSTER HUNTER") != std::string::npos) {
            other_versions.insert(upper.substr(0, 9));
        }
    };
    consider(source);
    const fs::path roots[] = {source, source / "PSP" / "SAVEDATA", source / "SAVEDATA", source / "memstick" / "PSP" / "SAVEDATA"};
    for (const fs::path &root : roots) {
        if (!fs::is_directory(root, ec)) continue;
        for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) consider(it->path());
        }
    }
    if (scan.found.empty()) {
        if (!other_versions.empty()) {
            std::string ids;
            for (const auto &id : other_versions) ids += (ids.empty() ? "" : ", ") + id;
            scan.note = "Found a save of another version of the game (" + ids + "). Only saves of your own version (" + disc_id +
                        ") can be imported for now: each version encrypts its saves with a different key.";
        } else {
            scan.note = "No " + disc_id + " save found there. Choose the folder that holds your saves: the PSP/SAVEDATA folder of the "
                        "memory stick, or PPSSPP's memory stick folder.";
        }
    }
    return scan;
}

Launcher::SaveImport Launcher::import_saves(const std::string &disc_id, const std::vector<SaveCandidate> &chosen) {
    SaveImport result;
    std::error_code ec;
    const fs::path game_dir = install::game_saves_dir(saves_dir_, disc_id);
    const fs::path stick = game_dir / "PSP" / "SAVEDATA";
    const fs::path staging = game_dir / ".import";
    fs::create_directories(stick, ec);
    if (ec) {
        result.error = "Cannot create the saves folder: " + ec.message();
        return result;
    }
    const auto stamp = [] {
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &now);
#else
        localtime_r(&now, &tm);
#endif
        char text[32];
        std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &tm);
        return std::string(text);
    };
    for (const SaveCandidate &c : chosen) {
        const fs::path source = c.path;
        const std::string name = platform::to_utf8(fs::path(c.folder).filename());
        if (name.empty() || name == "." || name == ".." || !fs::is_regular_file(source / "PARAM.SFO", ec)) continue;
        // Copied next to the real place first (outside SAVEDATA, where the game would list it), then moved in.
        const fs::path staged = staging / name;
        fs::remove_all(staged, ec);
        fs::create_directories(staged, ec);
        for (fs::directory_iterator it(source, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string file = platform::to_utf8(it->path().filename());
            if (!it->is_regular_file(ec) || file == "Thumbs.db" || file == "desktop.ini") continue;
            fs::copy_file(it->path(), staged / it->path().filename(), fs::copy_options::overwrite_existing, ec);
            if (ec) break;
        }
        if (ec) {
            result.error = "Could not copy " + name + ": " + ec.message();
            fs::remove_all(staged, ec);
            return result;
        }
        const fs::path target = stick / name;
        fs::path kept;
        if (fs::exists(target, ec)) {
            if (result.backup.empty()) result.backup = game_dir / "backups" / stamp();
            fs::create_directories(result.backup, ec);
            kept = result.backup / name;
            fs::rename(target, kept, ec);
            if (ec) {
                result.error = "Could not keep a backup of your current save: " + ec.message();
                fs::remove_all(staged, ec);
                return result;
            }
        }
        fs::rename(staged, target, ec);
        if (ec) {
            result.error = "Could not put the save in place: " + ec.message();
            if (!kept.empty()) fs::rename(kept, target, ec); // give the player's own save back
            return result;
        }
        ++result.imported;
    }
    fs::remove_all(staging, ec);
    return result;
}

std::string Launcher::disc_id_for(const std::string &game_id) {
    std::lock_guard lock(mutex_);
    const CatalogEntry *e = nullptr;
    return chosen_copy(data_, game_id, &e) != nullptr ? std::string(e->disc_id) : std::string();
}

bool Launcher::select_game(const std::string &disc_id) {
    std::lock_guard lock(mutex_);
    for (const auto &g : data_["games"].as_array()) {
        const CatalogEntry *e = entry_for(g["discId"].as_string());
        if (e == nullptr || disc_id != e->disc_id || std::string(e->status) == "planned") continue;
        data_["selectedDisc"] = disc_id;
        save();
        return true;
    }
    return false;
}

void Launcher::load() {
    // launcher.json (settings, library, identity) used to sit next to the executable, which an update (a new folder)
    // leaves behind. It lives in the per-user data folder now; an old one is copied there once, never deleted.
    {
        std::error_code ec;
        const fs::path old = base_ / "launcher.json";
        if (!fs::exists(file_, ec) && fs::is_regular_file(old, ec)) {
            fs::create_directories(file_.parent_path(), ec);
            fs::copy_file(old, file_, fs::copy_options::skip_existing, ec);
        }
    }
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
    if (!data_["gameSettings"].is_object()) data_["gameSettings"] = Json(Json::Object{});
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
    {
        std::error_code ec;
        fs::create_directories(file_.parent_path(), ec);
    }
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
    const CatalogEntry *shown = nullptr;
    chosen_copy(data_, "mhfu", &shown);
    s["settings"] = settings_for(data_, shown != nullptr ? shown->disc_id : "");
    s["keyDefaults"] = default_settings()["keys"]; // what "Restore defaults" puts back
    s["padDefaults"] = default_settings()["pad"];
    s["stats"] = data_["stats"];
    s["savesFolder"] = platform::to_utf8(saves_dir_);
    s["savesFolderCustom"] = !data_["savesFolder"].as_string().empty();
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
        games.array().push_back(out);
    }
    s["games"] = games;
    // Home, Settings and Play work on one copy of the game: the one the player chose, else the first playable.
    chosen_copy(data_, "mhfu", &playable);
    s["selectedDisc"] = playable != nullptr ? std::string(playable->disc_id) : std::string();
    Json exe;
    const fs::path found = playable ? find_executable(*playable) : fs::path{};
    exe["path"] = platform::to_utf8(found);
    exe["found"] = !found.empty() || playable == nullptr;
    s["executable"] = exe;
    return s;
}

void Launcher::import_game_controls() {
    std::error_code ec;
    if (!fs::is_regular_file(controls_file(), ec)) return;
    Json keys, pad;
    {
        std::ifstream in(controls_file());
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::size_t equals = line.find('=');
            if (equals == std::string::npos) continue;
            const std::string name = line.substr(0, equals), text = line.substr(equals + 1);
            if (name == "keymap") {
                const host::KeyBindings parsed = host::parse_keymap(text);
                for (const std::string_view action : host::kKeyActions) {
                    if (const std::string *key = host::key_binding(parsed, action)) keys[std::string(action)] = *key;
                }
            } else if (name == "padmap") {
                const host::PadBindings parsed = host::parse_padmap(text);
                for (const std::string_view action : host::kPadActions) {
                    if (const std::string *source = host::pad_binding(parsed, action)) pad[std::string(action)] = *source;
                }
            }
        }
    }
    Json partial;
    partial["keys"] = keys;
    partial["pad"] = pad;
    {
        std::lock_guard lock(mutex_);
        const CatalogEntry *entry = nullptr;
        chosen_copy(data_, "mhfu", &entry);
        // The game that wrote the file is the one that was started, even if the Library shows another copy by now.
        apply_settings(!launched_disc_.empty() ? launched_disc_ : (entry != nullptr ? std::string(entry->disc_id) : std::string()), partial);
    }
    fs::remove(controls_file(), ec);
}

void Launcher::set_settings(const Json &partial) {
    std::lock_guard lock(mutex_);
    const CatalogEntry *entry = nullptr;
    chosen_copy(data_, "mhfu", &entry);
    apply_settings(entry != nullptr ? entry->disc_id : "", partial);
}

void Launcher::apply_settings(const std::string &disc_id, const Json &partial) {
    Json &launcher_settings = data_["settings"];
    // The copy's settings start as what the launcher.json "settings" hold, so nobody loses what they had set.
    Json *own = nullptr;
    if (!disc_id.empty()) {
        Json &all = data_["gameSettings"];
        if (!all[disc_id].is_object()) {
            Json seed = launcher_settings;
            for (const auto &[k, v] : launcher_settings.as_object()) {
                if (is_launcher_setting(k)) seed.object().erase(k);
            }
            all[disc_id] = seed;
        }
        own = &all[disc_id];
    }
    Json &s = own != nullptr ? *own : launcher_settings;
    const auto num = [&](const char *key, double lo, double hi) {
        if (partial[key].is_number()) s[key] = std::clamp(std::round(partial[key].as_number()), lo, hi);
    };
    num("scale", 1, 10); // whole multiples of the PSP's 480x272, as PPSSPP offers
    if (partial["fps"].is_number()) { // whole multiples of the game's 30 fps, 30..180
        s["fps"] = std::clamp(std::round(partial["fps"].as_number() / 30.0), 1.0, 6.0) * 30.0;
    }
    num("musicVolume", 0, 100);
    num("sfxVolume", 0, 100);
    for (const char *key : {"fullscreen", "smoothTextures"}) {
        if (partial[key].is_bool()) s[key] = partial[key];
    }
    if (partial["copyImage"].is_bool()) launcher_settings["copyImage"] = partial["copyImage"];
    if (partial["checkUpdates"].is_bool()) launcher_settings["checkUpdates"] = partial["checkUpdates"];
    if (partial["adhocServer"].is_string() && valid_server(partial["adhocServer"].as_string())) launcher_settings["adhocServer"] = partial["adhocServer"];
    if (partial["keys"].is_object()) {
        Json &keys = s["keys"];
        for (const auto &[action, value] : partial["keys"].as_object()) {
            const bool known = std::any_of(host::kKeyActions.begin(), host::kKeyActions.end(),
                                           [&](std::string_view name) { return action == name; });
            if (!known || !value.is_string()) continue;
            const std::string code = value.as_string();
            if (!valid_key_code(code)) continue;
            keys[action] = code;
        }
    }
    if (partial["pad"].is_object()) {
        Json &pad = s["pad"];
        for (const auto &[action, value] : partial["pad"].as_object()) {
            const bool known = std::any_of(host::kPadActions.begin(), host::kPadActions.end(),
                                           [&](std::string_view name) { return action == name; });
            if (!known || !value.is_string() || !valid_pad_value(action, value.as_string())) continue;
            pad[action] = value.as_string();
        }
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
        if (e != nullptr && (id == e->game || id == e->disc_id)) removed.emplace_back(e->disc_id);
    }
    games.erase(std::remove_if(games.begin(), games.end(),
                               [&](const Json &g) {
                                   const CatalogEntry *e = entry_for(g["discId"].as_string());
                                   return e == nullptr || id == e->game || id == e->disc_id;
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
    plan.log = base_ / "game.log";
    std::error_code ec;
    const CatalogEntry *entry = nullptr;
    const Json *game = chosen_copy(data_, game_id, &entry);
    if (game == nullptr) {
        plan.error = "Add your copy of the game first.";
        return plan;
    }
    const std::string program = game_program_name(*entry);
    plan.program = base_ / program;
    if (!fs::is_regular_file(plan.program, ec)) {
        plan.error = "The game files are missing: " + program + " should be next to the launcher.";
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
    const Json s = settings_for(data_, entry->disc_id);
    launched_disc_ = entry->disc_id;
    const auto num = [&](const char *key) { return std::to_string(static_cast<int>(s[key].as_number())); };
    const fs::path stick = install::game_saves_dir(saves_dir_, entry->disc_id);
    import_legacy_saves(entry->disc_id);
    plan.args = {"--iso", platform::to_utf8(iso), "--exe", platform::to_utf8(exe), "--memstick",
                 platform::to_utf8(stick), "--window", "--scale", num("scale"), "--name",
                 data_["pseudonym"].as_string(), "--music-volume", num("musicVolume"), "--sfx-volume", num("sfxVolume")};
    if (s["fullscreen"].as_bool()) plan.args.push_back("--fullscreen");
    if (s["smoothTextures"].as_bool()) plan.args.push_back("--smooth-textures");
    if (s["fps"].as_number() > 30) {
        plan.args.push_back("--fps");
        plan.args.push_back(num("fps"));
    }
    // Always sent, so the game cannot fall back to defaults that differ from what the launcher shows.
    plan.args.push_back("--keymap");
    plan.args.push_back(host::format_keymap(key_bindings_from(s["keys"])));
    plan.args.push_back("--padmap");
    plan.args.push_back(host::format_padmap(pad_bindings_from(s["pad"])));
    plan.args.push_back("--controls-file"); // where the game writes what the player changes in its menu
    plan.args.push_back(platform::to_utf8(controls_file()));
    if (const std::string server = s["adhocServer"].as_string(); !server.empty()) {
        // Online play: the server, and a virtual MAC that is the same on every launch (a locally administered address made
        // from the identity, which never leaves this computer).
        Sha256 sha;
        const std::string material = "mhf-adhoc-mac" + data_["identitySeed"].as_string();
        sha.update(material.data(), material.size());
        const auto digest = sha.finish();
        char mac[18];
        std::snprintf(mac, sizeof mac, "02:%02X:%02X:%02X:%02X:%02X", digest[0], digest[1], digest[2], digest[3], digest[4]);
        plan.args.push_back("--adhoc-server");
        plan.args.push_back(server);
        plan.args.push_back("--adhoc-mac");
        plan.args.push_back(mac);
    }
    fs::create_directories(stick, ec);
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
