// Launcher core tests (no game data needed). With MHF_TEST_ISO set to the player's ULES01213 image, also checks that
// a real copy is identified and verified (label requires_game_data; never run in CI).

#include "hash.hpp"
#include "install/installer.hpp"
#include "json.hpp"
#include "launcher.hpp"
#include "param_sfo.hpp"

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

int main() {
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
    std::string code;
    {
        Launcher l(dir);
        Json s = l.state();
        code = s["friendCode"].as_string();
        CHECK(std::regex_match(code, std::regex("^[0-9A-HJKMNP-TV-Z]{4}(-[0-9A-HJKMNP-TV-Z]{4}){3}$")));
        CHECK(s["pseudonym"].as_string() == "Hunter");
        Json partial;
        partial["scale"] = 9;
        partial["musicVolume"] = -5;
        partial["fullscreen"] = true;
        l.set_settings(partial);
        l.set_pseudonym("Rathalos\x01Slayer");
        s = l.state();
        CHECK(s["settings"]["scale"].as_number() == 4);
        CHECK(s["settings"]["musicVolume"].as_number() == 0);
        CHECK(s["settings"]["fullscreen"].as_bool());
        CHECK(s["pseudonym"].as_string() == "RathalosSlayer");
        auto plan = l.plan_launch("mhfu");
        CHECK(!plan.error.empty()); // no game files here
    }
    {
        Launcher again(dir); // reloads launcher.json
        CHECK(again.state()["friendCode"].as_string() == code);
        CHECK(again.state()["settings"]["fullscreen"].as_bool());
    }

    // Not a disc image.
    {
        std::ofstream(dir / "junk.iso") << "not an iso";
        Launcher l(dir);
        auto r = l.add_game(dir / "junk.iso", {});
        CHECK(!r.game && !r.error.empty());
    }

    // A failed write must never destroy the saved state. Putting a directory where save() wants its temporary file makes
    // the write fail the same way a full disk does.
    {
        const fs::path blocked = dir / "blocked";
        fs::create_directories(blocked);
        std::string blocked_code;
        {
            Launcher l(blocked);
            blocked_code = l.state()["friendCode"].as_string();
        }
        fs::create_directories(blocked / "launcher.json.tmp");
        {
            Launcher l(blocked);
            l.set_pseudonym("Changed"); // save() cannot write
        }
        fs::remove_all(blocked / "launcher.json.tmp");
        CHECK(fs::is_regular_file(blocked / "launcher.json"));
        Launcher after(blocked);
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
