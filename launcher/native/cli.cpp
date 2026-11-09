#include "cli.hpp"

#include "launcher.hpp"
#include "platform.hpp"
#include "updater.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <thread>

namespace fs = std::filesystem;

namespace mhf::launcher::cli {
namespace {

#ifndef MHF_VERSION_STRING
#define MHF_VERSION_STRING "dev"
#endif

// The settings `--set` changes (the others, such as the controls, are changed in the game's own menu).
constexpr const char *kBoolSettings[] = {"fullscreen", "smoothTextures", "trueWeaponValues", "fastSupplyBox",
                                         "smithyBrowse", "smithyLayout", "analogCamera", "hornSongs"};
constexpr const char *kNumberSettings[] = {"scale", "fps", "hudSize", "musicVolume", "sfxVolume",
                                           "analogCameraTurnSpeed", "analogCameraTiltSpeed"};

void usage() {
    std::fprintf(stderr,
                 "MHForever without a window.\n\n"
                 "  --install <image.iso> [--in-place | --copy]   add your copy of the game (once)\n"
                 "  --list                                        your copies and their settings\n"
                 "  --set <name>=<value>... [--disc <disc id>]    change a copy's settings\n"
                 "        fullscreen, smoothTextures, trueWeaponValues, fastSupplyBox, smithyBrowse, smithyLayout:\n"
                 "        true or false\n"
                 "        scale 1-10, fps 30-180, hudSize 50-100, musicVolume 0-100, sfxVolume 0-100\n"
                 "  --play [<disc id>]                            start the game and wait until it ends\n"
                 "  --update                                      update MHForever\n"
                 "  --version\n\n"
                 "The copy used is the one chosen in the launcher, else the first one that can be played; a disc id such as\n"
                 "ULES01213 (Europe), ULUS10391 (North America), ULJM05500 (Japan) or ULJM05500-FUC (FUComplete)\n"
                 "picks another.\n");
}

bool contains(const char *const *list, std::size_t count, const std::string &name) {
    return std::any_of(list, list + count, [&](const char *item) { return name == item; });
}

// `--install <image.iso> [--in-place | --copy]`: the same setup as "Add my copy", without the window.
int install(const std::vector<std::string> &args) {
    fs::path iso;
    std::optional<bool> copy;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--install") continue;
        if (args[i] == "--in-place") copy = false;
        else if (args[i] == "--copy") copy = true;
        else if (!args[i].empty() && args[i][0] != '-') iso = platform::from_utf8(args[i]);
        else {
            std::fprintf(stderr, "unknown option: %s\n", args[i].c_str());
            return 2;
        }
    }
    if (iso.empty()) {
        std::fprintf(stderr, "usage: MHForever --install <image.iso> [--in-place | --copy]\n");
        return 2;
    }
    Launcher launcher(platform::executable_dir());
    if (copy) {
        Json settings;
        settings["copyImage"] = *copy;
        launcher.set_settings(settings);
    }
    mhf::launcher::Progress progress{[](const char *phase, double fraction) {
        std::fprintf(stderr, "\r%-10s %3d%%", phase, static_cast<int>(fraction * 100));
        if (fraction >= 1) std::fputc('\n', stderr);
    }};
    const auto result = launcher.add_game(iso, progress);
    if (!result.game) {
        std::fprintf(stderr, "error: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("%s (%s) is ready. Image: %s\n", (*result.game)["title"].as_string().c_str(),
                (*result.game)["discId"].as_string().c_str(), (*result.game)["isoPath"].as_string().c_str());
    return 0;
}

// `--update`: the same update as the launcher's "Update now", without the window.
int update() {
    Launcher launcher(platform::executable_dir());
    const fs::path work = launcher.data_dir() / "update";
    update::remove_old(platform::executable_dir());
    std::string error;
    const auto release = update::check(work, MHF_VERSION_STRING, error);
    if (!release) {
        if (!error.empty()) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        std::printf("MHForever %s is the latest version.\n", MHF_VERSION_STRING);
        return 0;
    }
    std::printf("Updating MHForever %s to %s (%s)\n", MHF_VERSION_STRING, release->version.c_str(), release->archive.c_str());
    error = update::apply(*release, work, platform::executable_dir(), [](const char *phase, double fraction) {
        std::fprintf(stderr, "\r%-12s %3d%%", phase, static_cast<int>(fraction * 100));
        if (fraction >= 1.0) std::fputc('\n', stderr);
    });
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s is installed. Start MHForever again.\n", release->version.c_str());
    return 0;
}

// Picks the copy the other commands work on; false (and a message) when `disc_id` is not in the library.
bool choose_copy(Launcher &launcher, const std::string &disc_id) {
    if (disc_id.empty() || launcher.select_game(disc_id)) return true;
    std::fprintf(stderr, "error: %s is not in your library (see --list)\n", disc_id.c_str());
    return false;
}

void print_copies(Launcher &launcher) {
    const Json state = launcher.state();
    const Json &games = state["games"];
    if (games.as_array().empty()) {
        std::printf("No copy of the game yet: add yours with --install <image.iso>\n");
        return;
    }
    const Json &s = state["settings"];
    for (const auto &g : games.as_array()) {
        const bool chosen = g["discId"].as_string() == state["selectedDisc"].as_string();
        std::printf("%s %s  %s, %s\n    %s\n", chosen ? "*" : " ", g["discId"].as_string().c_str(), g["title"].as_string().c_str(),
                    g["region"].as_string().c_str(), g["isoPath"].as_string().c_str());
    }
    std::printf("\nSettings of the copy marked *:\n");
    for (const char *name : kNumberSettings) {
        if (s[name].is_number()) std::printf("  %-18s %d\n", name, static_cast<int>(s[name].as_number()));
    }
    for (const char *name : kBoolSettings) {
        if (s[name].is_bool()) std::printf("  %-18s %s\n", name, s[name].as_bool() ? "true" : "false");
    }
}

// `--list`.
int list() {
    Launcher launcher(platform::executable_dir());
    print_copies(launcher);
    return 0;
}

// `--set name=value ... [--disc <disc id>]`.
int set(const std::vector<std::string> &args) {
    Json partial;
    std::string disc_id;
    bool any = false;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string &a = args[i];
        if (a == "--set") continue;
        if (a == "--disc" && i + 1 < args.size()) {
            disc_id = args[++i];
            continue;
        }
        const auto eq = a.find('=');
        if (eq == std::string::npos || eq == 0) {
            std::fprintf(stderr, "error: expected name=value, got '%s'\n", a.c_str());
            return 2;
        }
        const std::string name = a.substr(0, eq), value = a.substr(eq + 1);
        if (contains(kBoolSettings, std::size(kBoolSettings), name)) {
            if (value == "true" || value == "on" || value == "yes" || value == "1") partial[name] = true;
            else if (value == "false" || value == "off" || value == "no" || value == "0") partial[name] = false;
            else {
                std::fprintf(stderr, "error: %s is true or false\n", name.c_str());
                return 2;
            }
        } else if (contains(kNumberSettings, std::size(kNumberSettings), name)) {
            char *end = nullptr;
            const long number = std::strtol(value.c_str(), &end, 10);
            if (value.empty() || *end != '\0') {
                std::fprintf(stderr, "error: %s is a whole number\n", name.c_str());
                return 2;
            }
            partial[name] = static_cast<double>(number);
        } else {
            std::fprintf(stderr, "error: unknown setting '%s'\n", name.c_str());
            return 2;
        }
        any = true;
    }
    if (!any) {
        usage();
        return 2;
    }
    Launcher launcher(platform::executable_dir());
    if (launcher.state()["games"].as_array().empty()) {
        std::fprintf(stderr, "error: add your copy of the game first (--install <image.iso>)\n");
        return 1;
    }
    if (!choose_copy(launcher, disc_id)) return 2;
    launcher.set_settings(partial); // clamps every value to what the game accepts
    print_copies(launcher);
    return 0;
}

// `--play [<disc id>]`: starts the game as the launcher's Play button does, and stays until it ends (a launcher entry in
// Steam's game mode needs the program it started to keep running). What the player changes in the game's menu is kept.
int play(const std::vector<std::string> &args) {
    std::string disc_id;
    for (std::size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--play") continue;
        if (!args[i].empty() && args[i][0] != '-') disc_id = args[i];
        else {
            std::fprintf(stderr, "unknown option: %s\n", args[i].c_str());
            return 2;
        }
    }
    Launcher launcher(platform::executable_dir());
    if (!choose_copy(launcher, disc_id)) return 2;
    auto plan = launcher.plan_launch("mhfu");
    if (!plan.error.empty()) {
        std::fprintf(stderr, "error: %s\n", plan.error.c_str());
        return 1;
    }
    std::string error;
    auto process = platform::Process::start(plan.program, plan.args, plan.cwd, plan.log, error);
    if (!process) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::fprintf(stderr, "Playing. The game's log is %s\n", platform::to_utf8(plan.log).c_str());
    const auto started = std::chrono::steady_clock::now();
    auto looked = started;
    int code = 0;
    while (true) {
        if (auto exited = process->poll()) {
            code = *exited;
            break;
        }
        if (std::chrono::steady_clock::now() - looked > std::chrono::seconds(2)) {
            looked = std::chrono::steady_clock::now();
            std::error_code ec;
            if (fs::exists(launcher.controls_file(), ec)) launcher.import_game_controls();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
    if (code != 0 && seconds < 10) {
        std::fprintf(stderr, "The game closed right away (code %d). Its log is in %s\n", code, platform::to_utf8(plan.log).c_str());
        return 1;
    }
    launcher.import_game_controls();
    if (seconds >= 5) launcher.record_session(seconds);
    return 0;
}

} // namespace

bool handles(const std::string &first_argument) {
    return first_argument == "--install" || first_argument == "--update" || first_argument == "--list" ||
           first_argument == "--set" || first_argument == "--play" || first_argument == "--version" ||
           first_argument == "--help" || first_argument == "-h";
}

int run(const std::vector<std::string> &args) {
    if (args.size() < 2) {
        usage();
        return 2;
    }
    const std::string &mode = args[1];
    if (mode == "--install") return install(args);
    if (mode == "--update") return update();
    if (mode == "--list") return list();
    if (mode == "--set") return set(args);
    if (mode == "--play") return play(args);
    if (mode == "--version") {
        std::printf("MHForever %s\n", MHF_VERSION_STRING);
        return 0;
    }
    usage();
    return mode == "--help" || mode == "-h" ? 0 : 2;
}

} // namespace mhf::launcher::cli
