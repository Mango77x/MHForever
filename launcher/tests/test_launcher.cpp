// Launcher core tests (no game data needed). With MHF_TEST_ISO set to the player's ULES01213 image, also checks that
// a real copy is identified and verified (label requires_game_data; never run in CI).

#include "hash.hpp"
#include "install/installer.hpp"
#include "json.hpp"
#include "launcher.hpp"
#include "param_sfo.hpp"
#include "platform.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

using namespace mhf::launcher;
namespace fs = std::filesystem;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                     \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static std::string sha1_of(const std::string &s) {
    Sha1 h;
    h.update(s.data(), s.size());
    const auto d = h.finish();
    return to_hex(d.data(), d.size());
}
static std::string sha256_of(const std::string &s) {
    Sha256 h;
    h.update(s.data(), s.size());
    const auto d = h.finish();
    return to_hex(d.data(), d.size());
}

int main(int argc, char **argv) {
    // The process test below starts this same program and expects it to return this code.
    if (argc > 2 && std::string(argv[1]) == "--exit-code") {
        std::printf("child ran\n");
        return std::atoi(argv[2]);
    }

    // Hashes: FIPS 180 test vectors.
    CHECK(sha1_of("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(sha1_of("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(sha1_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    CHECK(sha256_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_of(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

    // JSON round trip, escapes and UTF-8.
    auto parsed = Json::parse(R"({"a":[1,2.5,true,null,"x\"y\\zé\n"],"b":{"c":"ñ"}})");
    CHECK(parsed.has_value());
    if (parsed) {
        CHECK((*parsed)["a"].as_array().size() == 5);
        CHECK((*parsed)["a"].as_array()[4].as_string() == "x\"y\\z\xc3\xa9\n");
        CHECK((*parsed)["b"]["c"].as_string() == "\xc3\xb1");
        auto again = Json::parse(parsed->dump());
        CHECK(again.has_value() && again->dump() == parsed->dump());
    }
    CHECK(!Json::parse("{\"a\":}").has_value());
    CHECK(!Json::parse("[1,2").has_value());
    CHECK(!Json::parse(std::string(200, '[')).has_value()); // depth limit

    // PARAM.SFO parsing of a synthetic table.
    {
        std::vector<std::uint8_t> sfo(0x60, 0);
        sfo[1] = 'P'; sfo[2] = 'S'; sfo[3] = 'F';
        sfo[4] = 1;
        sfo[8] = 0x24;  // key table
        sfo[12] = 0x30; // data table
        sfo[16] = 1;    // one entry
        // entry: key offset 0, format 0x0204, length 10, max 16, data offset 0
        sfo[20] = 0; sfo[22] = 0x04; sfo[23] = 0x02; sfo[24] = 10; sfo[28] = 16;
        const char key[] = "DISC_ID";
        std::copy(key, key + 8, sfo.begin() + 0x24);
        const char value[] = "ULES01213";
        std::copy(value, value + 10, sfo.begin() + 0x30);
        auto p = mhf::psp::ParamSfo::parse(sfo);
        CHECK(p.has_value() && p->string("DISC_ID") == "ULES01213");
        sfo[16] = 200; // entries beyond the buffer
        CHECK(!mhf::psp::ParamSfo::parse(sfo).has_value());
    }

    // Catalog comes from games/*/regions/*.toml.
    bool found = false;
    for (const auto &e : catalog()) found |= std::string(e.disc_id) == "ULES01213" && !e.dumps.empty();
    CHECK(found);

    // State, settings clamping, identity persistence.
    const fs::path dir = fs::temp_directory_path() / "mhf_launcher_test";
    fs::remove_all(dir);
    fs::create_directories(dir);

    // Processes: the launcher starts the game in its own folder and collects its output. The child changes directory
    // before it starts the program, so a relative program path has to keep working (Linux, where the launcher is often
    // run from the folder it was unzipped in).
    {
        const fs::path base = dir / "process";
        fs::create_directories(base);
        const fs::path log = base / "child.log";
        std::error_code ec;
        fs::path program = fs::relative(fs::path(argv[0]), fs::current_path(), ec);
        if (ec) program = fs::path(argv[0]);
        std::string error;
        auto child = platform::Process::start(program, {"--exit-code", "7"}, base, log, error);
        CHECK(child.has_value());
        if (child) CHECK(child->wait() == 7);
        CHECK(error.empty());
        std::ifstream in(log);
        std::string line;
        std::getline(in, line);
        CHECK(line == "child ran");
    }

    std::string code;
    {
        Launcher l(dir, dir);
        Json s = l.state();
        code = s["friendCode"].as_string();
        CHECK(std::regex_match(code, std::regex("^[0-9A-HJKMNP-TV-Z]{4}(-[0-9A-HJKMNP-TV-Z]{4}){3}$")));
        CHECK(s["pseudonym"].as_string() == "Hunter");
        Json partial;
        partial["scale"] = 11;
        partial["musicVolume"] = -5;
        partial["fullscreen"] = true;
        partial["fps"] = 100; // snaps to a multiple of 30
        l.set_settings(partial);
        l.set_pseudonym("Rathalos\x01Slayer");
        s = l.state();
        CHECK(s["settings"]["scale"].as_number() == 10);
        CHECK(s["settings"]["musicVolume"].as_number() == 0);
        CHECK(s["settings"]["fullscreen"].as_bool());
        CHECK(s["settings"]["fps"].as_number() == 90);
        Json fast;
        fast["fps"] = 500;
        l.set_settings(fast);
        CHECK(l.state()["settings"]["fps"].as_number() == 180);
        Json slow;
        slow["fps"] = 5;
        l.set_settings(slow);
        CHECK(l.state()["settings"]["fps"].as_number() == 30);
        Json back;
        back["fps"] = 90;
        l.set_settings(back);
        CHECK(s["pseudonym"].as_string() == "RathalosSlayer");

        // Keyboard bindings: defaults, a change, a button left without a key, and input the launcher ignores.
        s = l.state();
        CHECK(s["settings"]["keys"]["south"].as_string() == "KeyK");
        CHECK(s["settings"]["keys"]["stick_up"].as_string() == "KeyW");
        Json keys;
        keys["south"] = "KeyZ";
        keys["select"] = "";                  // deliberately unbound
        keys["jump"] = "KeyP";                // not a button of the pad
        keys["north"] = std::string(40, 'X'); // not a key code
        keys["start"] = 5;                    // not a string
        Json edited;
        edited["keys"] = keys;
        l.set_settings(edited);
        const Json state = l.state(); // a copy: the members of a temporary would not outlive this line
        const Json &saved = state["settings"]["keys"];
        CHECK(saved["south"].as_string() == "KeyZ");
        CHECK(saved["select"].as_string().empty());
        CHECK(saved["north"].as_string() == "KeyI");
        CHECK(saved["start"].as_string() == "Enter");
        CHECK(!saved.has("jump"));

        // Controller bindings: defaults, a change with several sources, an unbound button, and input the launcher ignores.
        CHECK(state["settings"]["pad"]["south"].as_string() == "south");
        CHECK(state["settings"]["pad"]["left_shoulder"].as_string() == "left_shoulder+left_trigger");
        CHECK(state["padDefaults"]["select"].as_string() == "back");
        Json pad;
        pad["south"] = "dpad_up+right_trigger";
        pad["east"] = "";                // deliberately unbound
        pad["west"] = "banana";          // not a source
        pad["north"] = "south+nothing";  // one part is not a source
        pad["start"] = 5;                // not a string
        pad["stick"] = "right";
        pad["jump"] = "south";           // not a button of the pad
        Json pad_edit;
        pad_edit["pad"] = pad;
        l.set_settings(pad_edit);
        const Json pad_state = l.state();
        const Json &pad_saved = pad_state["settings"]["pad"];
        CHECK(pad_saved["south"].as_string() == "dpad_up+right_trigger");
        CHECK(pad_saved["east"].as_string().empty());
        CHECK(pad_saved["west"].as_string() == "west");
        CHECK(pad_saved["north"].as_string() == "north");
        CHECK(pad_saved["start"].as_string() == "start");
        CHECK(pad_saved["stick"].as_string() == "right");
        CHECK(!pad_saved.has("jump"));

        // What the player changed in the game's own menu comes back through a file the game writes.
        {
            std::ofstream(l.controls_file()) << "keymap=south=KeyZ,select=,west=KeyX,start=Space,jump=KeyP,north=" << std::string(40, 'Y')
                                             << "\npadmap=south=dpad_up+right_trigger,up=south,west=banana,stick=right\nunknown=1\n";
        }
        l.import_game_controls();
        const Json from_game = l.state();
        CHECK(from_game["settings"]["keys"]["west"].as_string() == "KeyX");
        CHECK(from_game["settings"]["keys"]["start"].as_string() == "Space");
        CHECK(from_game["settings"]["keys"]["north"].as_string() == "KeyI"); // not a key code: unchanged
        CHECK(from_game["settings"]["pad"]["up"].as_string() == "south");
        CHECK(from_game["settings"]["pad"]["west"].as_string() == "west");   // not a source: unchanged
        CHECK(from_game["settings"]["pad"]["stick"].as_string() == "right");
        CHECK(!fs::exists(l.controls_file())); // taken over: the file is gone

        auto plan = l.plan_launch("mhfu");
        CHECK(!plan.error.empty()); // no game files here
    }
    {
        Launcher again(dir, dir); // reloads launcher.json
        CHECK(again.state()["friendCode"].as_string() == code);
        CHECK(again.state()["settings"]["fullscreen"].as_bool());
        CHECK(again.state()["settings"]["fps"].as_number() == 90);
        CHECK(again.state()["settings"]["keys"]["south"].as_string() == "KeyZ");
        CHECK(again.state()["settings"]["keys"]["select"].as_string().empty());
        CHECK(again.state()["settings"]["keys"]["east"].as_string() == "KeyL");
        CHECK(again.state()["settings"]["pad"]["south"].as_string() == "dpad_up+right_trigger");
        CHECK(again.state()["settings"]["pad"]["stick"].as_string() == "right");
    }

    // launcher.json lives in the data folder; one left next to the executable by an older version is copied there once.
    {
        const fs::path base = dir / "config_base", data = dir / "config_data";
        fs::create_directories(base);
        std::ofstream(base / "launcher.json") << R"({"pseudonym": "OldHunter", "identitySeed": ")" << std::string(64, 'a')
                                              << R"("})";
        {
            Launcher l(base, data);
            CHECK(l.state()["pseudonym"].as_string() == "OldHunter");
            CHECK(fs::is_regular_file(data / "launcher.json"));
            CHECK(fs::is_regular_file(base / "launcher.json")); // the old file stays
            Json s;
            s["fullscreen"] = true;
            l.set_settings(s);
        }
        Launcher again(base, data); // the data folder's file wins from now on
        CHECK(again.state()["settings"]["fullscreen"].as_bool());
        CHECK(again.state()["pseudonym"].as_string() == "OldHunter");
    }

    // Saves live outside the launcher folder; the ones an older version left next to it are imported once, copied and
    // never overwritten or deleted.
    {
        const fs::path base = dir / "saves_base", data = dir / "saves_data";
        const fs::path old_save = base / "memstick" / "PSP" / "SAVEDATA" / "ULES01213SLOT1";
        fs::create_directories(old_save);
        std::ofstream(old_save / "DATA.BIN", std::ios::binary) << "old";
        Launcher l(base, data);
        CHECK(l.saves_dir() == data / "saves");
        l.import_legacy_saves("ULES01213");
        const fs::path stick = data / "saves" / "ULES01213"; // one memory stick per game
        const fs::path moved = stick / "PSP" / "SAVEDATA" / "ULES01213SLOT1" / "DATA.BIN";
        CHECK(fs::is_regular_file(moved));
        CHECK(fs::is_regular_file(old_save / "DATA.BIN")); // the old copy stays
        std::ofstream(moved, std::ios::binary) << "newer";  // played on after the import
        l.import_legacy_saves("ULES01213");                 // same source: nothing happens
        std::string text;
        std::ifstream(moved, std::ios::binary) >> text;
        CHECK(text == "newer");
        fs::remove(moved);
        Launcher again(base, data);
        again.import_legacy_saves("ULES01213");             // a deleted save does not come back
        CHECK(!fs::exists(moved));
        // A second old folder (another unzipped version) is imported too, without touching what exists.
        const fs::path base2 = dir / "saves_base2";
        fs::create_directories(base2 / "memstick" / "PSP" / "SAVEDATA" / "ULES01213SLOT2");
        std::ofstream(base2 / "memstick" / "PSP" / "SAVEDATA" / "ULES01213SLOT2" / "DATA.BIN") << "two";
        Launcher third(base2, data);
        third.import_legacy_saves("ULES01213");
        CHECK(fs::is_regular_file(stick / "PSP" / "SAVEDATA" / "ULES01213SLOT2" / "DATA.BIN"));
    }

    // Not a disc image.
    {
        std::ofstream(dir / "junk.iso") << "not an iso";
        Launcher l(dir, dir);
        auto r = l.add_game(dir / "junk.iso", {});
        CHECK(!r.game && !r.error.empty());
    }

    // Importing saves made by a PSP or PPSSPP: found by the disc id, other versions only mentioned, nothing of the
    // player's own save ever lost.
    {
        const auto sfo_with = [](const std::string &key, const std::string &value) {
            std::vector<std::uint8_t> v(0x80, 0);
            v[1] = 'P'; v[2] = 'S'; v[3] = 'F';
            v[4] = 1;
            v[8] = 0x24;  // key table
            v[12] = 0x40; // data table
            v[16] = 1;    // one entry
            v[22] = 0x04; v[23] = 0x02; // utf-8 text
            v[24] = static_cast<std::uint8_t>(value.size() + 1);
            v[28] = 0x30;
            std::copy(key.begin(), key.end(), v.begin() + 0x24);
            std::copy(value.begin(), value.end(), v.begin() + 0x40);
            return std::string(v.begin(), v.end());
        };
        const auto put = [](const fs::path &file, const std::string &content) {
            fs::create_directories(file.parent_path());
            std::ofstream(file, std::ios::binary) << content;
        };
        const auto read = [](const fs::path &file) {
            std::ifstream in(file, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        const fs::path base = dir / "saveimport";
        fs::remove_all(base);
        const fs::path stick = base / "from" / "PSP" / "SAVEDATA";
        put(stick / "ULES01213" / "PARAM.SFO", sfo_with("SAVEDATA_TITLE", "Game Data"));
        put(stick / "ULES01213" / "MHP2NDG.BIN", "encrypted data of the new save");
        put(stick / "ULES01213" / "ICON0.PNG", "icon");
        put(stick / "ULES01213" / "Thumbs.db", "junk");
        put(stick / "ULUS10391" / "PARAM.SFO", sfo_with("TITLE", "Monster Hunter Freedom Unite"));
        put(stick / "NPJH12345" / "PARAM.SFO", sfo_with("TITLE", "Another Game"));
        put(stick / "NOSFO" / "MHP2NDG.BIN", "no PARAM.SFO here");

        Launcher l(base / "app", base / "data", base / "saves");
        // The memory stick folder, its PSP/SAVEDATA folder and the save folder itself all lead to the save.
        for (const fs::path &source : {base / "from", stick, stick / "ULES01213"}) {
            const auto scan = l.scan_saves("ULES01213", source);
            CHECK(scan.found.size() == 1 && scan.found[0].folder == "ULES01213" && scan.found[0].title == "Game Data");
            CHECK(scan.note.empty() && !scan.found[0].exists);
        }
        // Only another version in the folder: nothing to import, and the player is told why.
        fs::create_directories(base / "us" / "PSP" / "SAVEDATA");
        fs::copy(stick / "ULUS10391", base / "us" / "PSP" / "SAVEDATA" / "ULUS10391", fs::copy_options::recursive);
        const auto us = l.scan_saves("ULES01213", base / "us");
        CHECK(us.found.empty() && us.note.find("ULUS10391") != std::string::npos);
        fs::create_directories(base / "empty");
        const auto nothing = l.scan_saves("ULES01213", base / "empty");
        CHECK(nothing.found.empty() && nothing.note.find("ULES01213") != std::string::npos);

        // The first import: the files arrive (not the junk), nothing is left in the staging folder.
        const auto found = l.scan_saves("ULES01213", base / "from").found;
        auto done = l.import_saves("ULES01213", found);
        const fs::path mine = base / "saves" / "ULES01213" / "PSP" / "SAVEDATA" / "ULES01213";
        CHECK(done.error.empty() && done.imported == 1 && done.backup.empty());
        CHECK(read(mine / "MHP2NDG.BIN") == "encrypted data of the new save" && fs::exists(mine / "ICON0.PNG"));
        CHECK(!fs::exists(mine / "Thumbs.db") && !fs::exists(base / "saves" / "ULES01213" / ".import"));
        CHECK(l.scan_saves("ULES01213", base / "from").found[0].exists);

        // The second one replaces it, and the old one is kept, not deleted.
        put(mine / "MHP2NDG.BIN", "the player's own save");
        done = l.import_saves("ULES01213", found);
        CHECK(done.error.empty() && done.imported == 1 && !done.backup.empty());
        CHECK(read(mine / "MHP2NDG.BIN") == "encrypted data of the new save");
        CHECK(read(done.backup / "ULES01213" / "MHP2NDG.BIN") == "the player's own save");

        // A folder name that tries to leave the saves folder, or a folder without PARAM.SFO, is ignored.
        Launcher::SaveCandidate bad;
        bad.folder = "..";
        bad.path = stick / "ULES01213";
        Launcher::SaveCandidate nosfo;
        nosfo.folder = "NOSFO";
        nosfo.path = stick / "NOSFO";
        done = l.import_saves("ULES01213", {bad, nosfo});
        CHECK(done.imported == 0 && done.error.empty() && !fs::exists(base / "saves" / "ULES01213" / "PSP" / "SAVEDATA" / "NOSFO"));

        // The disc id of the player's copy is looked up by game id (none added in this test launcher).
        CHECK(l.disc_id_for("mhfu").empty());
    }

    // A failed write must never destroy the saved state. Putting a directory where save() wants its temporary file makes
    // the write fail the same way a full disk does.
    {
        const fs::path blocked = dir / "blocked";
        fs::create_directories(blocked);
        std::string blocked_code;
        {
            Launcher l(blocked, blocked);
            blocked_code = l.state()["friendCode"].as_string();
        }
        fs::create_directories(blocked / "launcher.json.tmp");
        {
            Launcher l(blocked, blocked);
            l.set_pseudonym("Changed"); // save() cannot write
        }
        fs::remove_all(blocked / "launcher.json.tmp");
        CHECK(fs::is_regular_file(blocked / "launcher.json"));
        Launcher after(blocked, blocked);
        CHECK(after.state()["friendCode"].as_string() == blocked_code); // same identity, not a fresh one
        CHECK(after.state()["pseudonym"].as_string() == "Hunter");      // the previous file is intact
    }

    // The player's copy (requires game data).
    if (const char *iso = std::getenv("MHF_TEST_ISO")) {
        // Identification: a developer executable of the right size is already known, so only the image is checked.
        {
            fs::create_directories(dir / "exe");
            std::ofstream(dir / "exe" / "ULES01213_EBOOT.BIN", std::ios::binary) << std::string(1910384, '\0');
            Launcher l(dir, dir / "data_identify");
            double last = 0;
            auto r = l.add_game(iso, {[&](const char *, double f) { last = f; }});
            CHECK(r.game.has_value());
            if (r.game) {
                CHECK((*r.game)["discId"].as_string() == "ULES01213");
                CHECK((*r.game)["verification"].as_string() == "verified");
            }
            CHECK(last > 0.99);
            std::printf("identified %s\n", r.game ? r.game->dump().c_str() : r.error.c_str());
        }
        // Executable preparation: the encrypted executable on the disc must pass the identity gate of the catalog.
        // Without a decryptor in this build the only acceptable outcome is "unavailable", never "another release".
        {
            const CatalogEntry *entry = nullptr;
            for (const auto &e : catalog()) {
                if (std::string(e.disc_id) == "ULES01213") entry = &e;
            }
            CHECK(entry != nullptr);
            if (entry) {
                const fs::path data = dir / "data_prepare";
                const auto r = install::install_from_image(iso, *entry, data, install::InstallOptions{true, false},
                                                           install::builtin_decryptor(), {});
                if (install::builtin_decryptor()) {
                    CHECK(r.error == install::InstallError::None);
                    // Byte for byte the executable the recompiled code was generated from.
                    std::ifstream in(r.executable, std::ios::binary);
                    install::Bytes bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    CHECK(install::sha1_hex(bytes) == entry->dev_executable_sha1);
                } else {
                    CHECK(r.error == install::InstallError::PreparationUnavailable);
                }
                std::printf("preparation: %s\n", r.error == install::InstallError::None ? "prepared" : r.message.c_str());
            }
        }
    }

    fs::remove_all(dir);
    if (failures == 0) std::printf("launcher tests passed\n");
    return failures == 0 ? 0 : 1;
}
