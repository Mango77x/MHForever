// The launcher's entry point: a system web view showing the embedded UI, with `mhf_*` functions bound for it
// (launcher/ui/src/bridge.ts documents the protocol). Long work runs on worker threads and reports back through
// `window.__mhfEvent`.

#include "launcher.hpp"
#include "platform.hpp"

#include "webview/webview.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

extern const unsigned char kUiHtml[];
extern const std::size_t kUiHtmlSize;

namespace {

using mhf::launcher::Json;
using mhf::launcher::Launcher;
namespace platform = mhf::launcher::platform;
namespace fs = std::filesystem;

struct App {
    webview_t view{};
    std::unique_ptr<Launcher> launcher;
    std::atomic<bool> busy{false}; // a verification or a game session is running
    std::thread worker;

    // Runs `js` on the UI thread (safe from any thread).
    void eval(std::string js) {
        auto *payload = new std::string(std::move(js));
        webview_dispatch(
            view,
            [](webview_t w, void *arg) {
                std::unique_ptr<std::string> s(static_cast<std::string *>(arg));
                webview_eval(w, s->c_str());
            },
            payload);
    }
    void emit(const Json &event) { eval("window.__mhfEvent(" + event.dump() + ")"); }
    void emit_state() {
        Json e;
        e["type"] = "state";
        e["state"] = launcher->state();
        emit(e);
    }
    void run_in_background(std::function<void()> work) {
        if (worker.joinable()) worker.join();
        worker = std::thread(std::move(work));
    }
};

App g_app;

Json args_of(const char *req) {
    auto parsed = Json::parse(req ? req : "[]");
    return parsed && parsed->is_array() ? *parsed : Json(Json::Array{});
}

void reply(const char *id, const Json &value) { webview_return(g_app.view, id, 0, value.dump().c_str()); }

using Handler = void (*)(const char *id, const Json &args);

void bind(const char *name, Handler handler) {
    webview_bind(
        g_app.view, name,
        [](const char *id, const char *req, void *arg) {
            reinterpret_cast<Handler>(arg)(id, args_of(req));
        },
        reinterpret_cast<void *>(handler));
}

void *native_window() { return webview_get_window(g_app.view); }

void on_get_state(const char *id, const Json &) { reply(id, g_app.launcher->state()); }

void on_set_settings(const char *id, const Json &args) {
    g_app.launcher->set_settings(args.as_array().empty() ? Json() : args.as_array()[0]);
    reply(id, g_app.launcher->state());
}

void on_set_pseudonym(const char *id, const Json &args) {
    if (!args.as_array().empty()) g_app.launcher->set_pseudonym(args.as_array()[0].as_string());
    reply(id, g_app.launcher->state());
}

void on_add_game(const char *id, const Json &) {
    Json r;
    if (g_app.busy) {
        r["started"] = false;
        reply(id, r);
        return;
    }
    const auto file = platform::open_file(native_window(), "Choose your copy of the game", "PSP disc images",
                                          {"*.iso", "*.cso"});
    r["started"] = file.has_value();
    reply(id, r);
    if (!file) return;
    g_app.busy = true;
    const fs::path iso = *file;
    g_app.run_in_background([iso] {
        const std::string name = platform::to_utf8(iso.filename());
        auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        mhf::launcher::Progress progress{[&](const char *phase, double fraction) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last < std::chrono::milliseconds(60) && fraction < 1) return;
            last = now;
            Json e;
            e["type"] = "verify";
            e["phase"] = phase;
            e["progress"] = fraction;
            e["file"] = name;
            g_app.emit(e);
        }};
        auto result = g_app.launcher->add_game(iso, progress);
        Json e;
        if (result.game) {
            e["type"] = "verified";
            e["game"] = *result.game;
        } else {
            e["type"] = "verifyFailed";
            e["message"] = result.error;
        }
        g_app.emit(e);
        g_app.emit_state();
        g_app.busy = false;
    });
}

void on_remove_game(const char *id, const Json &args) {
    if (!args.as_array().empty()) g_app.launcher->remove_game(args.as_array()[0].as_string());
    reply(id, g_app.launcher->state());
}

void on_pick_executable(const char *id, const Json &) {
    if (const auto file = platform::open_file(native_window(), "Locate the decrypted executable (developer build)",
                                              "Executables", {"*.BIN", "*.bin", "*.elf", "*.prx"})) {
        g_app.launcher->set_executable(*file);
    }
    reply(id, g_app.launcher->state());
}

void on_copy_text(const char *id, const Json &args) {
    const bool ok = !args.as_array().empty() && platform::copy_to_clipboard(native_window(), args.as_array()[0].as_string());
    reply(id, Json(ok));
}

void on_export_identity(const char *id, const Json &) {
    Json r;
    r["saved"] = false;
    if (const auto file = platform::save_file(native_window(), "Back up my identity", "mhforever-identity.json")) {
        std::ofstream out(*file, std::ios::binary | std::ios::trunc);
        out << g_app.launcher->identity_backup().dump(true) << '\n';
        r["saved"] = static_cast<bool>(out);
    }
    reply(id, r);
}

std::string tail_of_log(const fs::path &log, std::size_t lines) {
    std::ifstream in(log);
    std::vector<std::string> all;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) all.push_back(line);
    }
    std::string out;
    for (std::size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); ++i) out += all[i] + "\n";
    return out;
}

void on_play(const char *id, const Json &args) {
    Json r;
    if (g_app.busy || args.as_array().empty()) {
        r["started"] = false;
        reply(id, r);
        return;
    }
    r["started"] = true;
    reply(id, r);
    g_app.busy = true;
    const std::string game = args.as_array()[0].as_string();
    g_app.run_in_background([game] {
        const auto phase = [](const char *p) {
            Json e;
            e["type"] = "launch";
            e["phase"] = p;
            g_app.emit(e);
        };
        const auto fail = [](const std::string &message) {
            Json e;
            e["type"] = "launchFailed";
            e["message"] = message;
            g_app.emit(e);
            g_app.busy = false;
        };
        phase("checking");
        auto plan = g_app.launcher->plan_launch(game);
        if (!plan.error.empty()) return fail(plan.error);
        std::this_thread::sleep_for(std::chrono::milliseconds(350)); // let the step animate
        std::string error;
        auto process = platform::Process::start(plan.program, plan.args, plan.cwd, plan.log, error);
        if (!process) return fail(error);
        phase("starting");
        const auto started = std::chrono::steady_clock::now();
        bool running = false;
        int code = 0;
        while (true) {
            if (auto exited = process->poll()) {
                code = *exited;
                break;
            }
            if (!running && std::chrono::steady_clock::now() - started > std::chrono::milliseconds(1500)) {
                running = true;
                phase("running");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        if (code != 0 && seconds < 10) {
            return fail("The game closed right away (code " + std::to_string(code) + "). Its log is in " +
                        platform::to_utf8(plan.log) + ".\n\n" + tail_of_log(plan.log, 4));
        }
        if (seconds >= 5) g_app.launcher->record_session(seconds);
        Json e;
        e["type"] = "exited";
        e["code"] = code;
        e["seconds"] = static_cast<std::int64_t>(seconds);
        g_app.emit(e);
        g_app.emit_state();
        g_app.busy = false;
    });
}

// `MHForever --install <image.iso> [--in-place | --copy]`: the same setup as "Add my copy", without the window.
int run_install_cli(const std::vector<std::string> &args) {
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

#if defined(_WIN32)
// A window-subsystem program has no console of its own: borrow the one it was started from.
void attach_parent_console() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
}
#endif

} // namespace

#if defined(_WIN32)
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Paint the window in the launcher's ground colour from the first frame (no white flash).
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", L"FF0B0C10");
    const bool debug = GetEnvironmentVariableW(L"MHF_LAUNCHER_DEBUG", nullptr, 0) > 0;
    {
        // WinMain leaves the CRT's __wargv unset, so read the wide command line directly.
        int wargc = 0;
        LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
        if (wargv != nullptr) {
            std::vector<std::string> args;
            for (int i = 0; i < wargc; ++i) args.push_back(platform::to_utf8(fs::path(wargv[i])));
            LocalFree(wargv);
            if (args.size() > 1 && args[1] == "--install") {
                attach_parent_console();
                return run_install_cli(args);
            }
        }
    }
#else
int main(int argc, char **argv) {
    const bool debug = std::getenv("MHF_LAUNCHER_DEBUG") != nullptr;
    if (argc > 1 && std::string(argv[1]) == "--install") return run_install_cli(std::vector<std::string>(argv, argv + argc));
#endif
    g_app.launcher = std::make_unique<Launcher>(platform::executable_dir());
    g_app.view = webview_create(debug ? 1 : 0, nullptr);
    if (g_app.view == nullptr) {
#if defined(_WIN32)
        platform::show_error("MHForever",
                             "The launcher needs the Microsoft Edge WebView2 Runtime, which is part of Windows 11 and "
                             "most Windows 10 installations.\n\nInstall it from "
                             "https://developer.microsoft.com/microsoft-edge/webview2/ and start the launcher again.");
#else
        platform::show_error("MHForever", "The launcher needs WebKitGTK (libwebkit2gtk-4.1).");
#endif
        return 1;
    }
    webview_set_title(g_app.view, "MHForever");
    webview_set_size(g_app.view, 960, 640, WEBVIEW_HINT_MIN);
    webview_set_size(g_app.view, 1280, 800, WEBVIEW_HINT_NONE);
    platform::style_window(native_window());

    bind("mhf_getState", on_get_state);
    bind("mhf_setSettings", on_set_settings);
    bind("mhf_setPseudonym", on_set_pseudonym);
    bind("mhf_addGame", on_add_game);
    bind("mhf_removeGame", on_remove_game);
    bind("mhf_pickExecutable", on_pick_executable);
    bind("mhf_play", on_play);
    bind("mhf_copyText", on_copy_text);
    bind("mhf_exportIdentity", on_export_identity);

    webview_set_html(g_app.view, std::string(reinterpret_cast<const char *>(kUiHtml), kUiHtmlSize).c_str());
    webview_run(g_app.view);
    if (g_app.worker.joinable()) g_app.worker.detach(); // a running game keeps going after the launcher closes
    webview_destroy(g_app.view);
    return 0;
}
