// The launcher's first program on Linux (MHForever). It links nothing but the C and C++ libraries, so it starts on every
// system, and it starts the launcher's window (MHForever-window), which needs WebKitGTK 4.1.
//
// When the system has WebKitGTK, the window uses it, as before. When it has not (SteamOS on the Steam Deck, whose system
// cannot be changed), the release carries one in lib/webkit (scripts/bundle_webkit_linux.py) and this program makes the
// window use it:
//   - lib/webkit holds WebKitGTK and the libraries it needs. Only those the system lacks are used: the program resolves
//     the window's libraries the way the dynamic loader does and links the missing ones into kRuntimeDir/lib, which goes
//     first in LD_LIBRARY_PATH. The system's own libraries (GTK, graphics drivers) always win.
//   - WebKit starts its helper programs (WebKitWebProcess, WebKitNetworkProcess) from a folder fixed when it was built.
//     The bundled copy has kRuntimeDir written there instead (same length), and kRuntimeDir links to the helpers here.
//   - the TLS module of GIO (for the wss:// and https:// the friends feature uses) is added only if the system has none.
// The game programs started from the window get the environment back as it was (platform.cpp, Process::start).
//
// MHF_WEBKIT=system or MHF_WEBKIT=bundled forces either WebKitGTK (to test the bundled one on a system that has its own).

#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

// Written into the bundled libwebkit2gtk-4.1.so.0 in place of its helpers' folder, which has the same length
// ("/usr/lib/x86_64-linux-gnu/webkit2gtk-4.1"). scripts/bundle_webkit_linux.py reads it from this line.
constexpr const char kRuntimeDir[] = "/tmp/.mhforever-webkit2gtk-4.1-helpers-1";
constexpr const char kWebKit[] = "libwebkit2gtk-4.1.so.0";
constexpr const char kJavaScriptCore[] = "libjavascriptcoregtk-4.1.so.0";

std::string self_dir() {
    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n <= 0) return ".";
    std::string path(buffer, static_cast<std::size_t>(n));
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? "." : path.substr(0, slash == 0 ? 1 : slash);
}

bool is_dir(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path) {
    struct stat st {};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::vector<std::string> list_dir(const std::string &path) {
    std::vector<std::string> names;
    if (DIR *d = opendir(path.c_str())) {
        while (const dirent *e = readdir(d)) {
            if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) names.emplace_back(e->d_name);
        }
        closedir(d);
    }
    return names;
}

// The libraries a 64-bit x86 ELF file asks for (DT_NEEDED), or nothing when it is not one (a 32-bit library in
// /usr/lib32 has the same name as the 64-bit one).
std::optional<std::vector<std::string>> needed_of(const std::string &path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    const auto read_at = [fd](void *to, std::size_t size, off_t at) {
        return pread(fd, to, size, at) == static_cast<ssize_t>(size);
    };
    std::optional<std::vector<std::string>> result;
    Elf64_Ehdr eh{};
    if (read_at(&eh, sizeof eh, 0) && std::memcmp(eh.e_ident, ELFMAG, SELFMAG) == 0 && eh.e_ident[EI_CLASS] == ELFCLASS64 &&
        eh.e_machine == EM_X86_64 && eh.e_phentsize == sizeof(Elf64_Phdr) && eh.e_phnum > 0 && eh.e_phnum < 256) {
        std::vector<Elf64_Phdr> ph(eh.e_phnum);
        if (read_at(ph.data(), ph.size() * sizeof(Elf64_Phdr), static_cast<off_t>(eh.e_phoff))) {
            result.emplace();
            const auto offset_of = [&](Elf64_Addr address) -> std::optional<off_t> {
                for (const auto &p : ph) {
                    if (p.p_type == PT_LOAD && address >= p.p_vaddr && address < p.p_vaddr + p.p_filesz) {
                        return static_cast<off_t>(p.p_offset + (address - p.p_vaddr));
                    }
                }
                return std::nullopt;
            };
            for (const auto &p : ph) {
                if (p.p_type != PT_DYNAMIC || p.p_filesz > (1u << 20)) continue;
                std::vector<Elf64_Dyn> dyn(p.p_filesz / sizeof(Elf64_Dyn));
                if (dyn.empty() || !read_at(dyn.data(), dyn.size() * sizeof(Elf64_Dyn), static_cast<off_t>(p.p_offset))) break;
                Elf64_Addr strtab = 0;
                std::vector<Elf64_Xword> names;
                for (const auto &d : dyn) {
                    if (d.d_tag == DT_NULL) break;
                    if (d.d_tag == DT_STRTAB) strtab = d.d_un.d_ptr;
                    if (d.d_tag == DT_NEEDED) names.push_back(d.d_un.d_val);
                }
                const auto base = offset_of(strtab);
                if (!base) break;
                for (const auto name : names) {
                    char text[256] = {};
                    const ssize_t got = pread(fd, text, sizeof text - 1, *base + static_cast<off_t>(name));
                    if (got > 0) result->emplace_back(text);
                }
                break;
            }
        }
    }
    close(fd);
    return result;
}

// Where the dynamic loader looks: LD_LIBRARY_PATH, the folders of /etc/ld.so.conf (and what it includes), the defaults.
std::vector<std::string> library_dirs() {
    std::vector<std::string> dirs;
    if (const char *env = std::getenv("LD_LIBRARY_PATH")) {
        std::string list = env;
        for (std::size_t start = 0; start <= list.size();) {
            const auto end = std::min(list.find(':', start), list.size());
            // Not the bundled libraries of an earlier start (a launcher restarting itself after an update).
            if (end > start && list.compare(start, std::strlen(kRuntimeDir), kRuntimeDir) != 0) {
                dirs.push_back(list.substr(start, end - start));
            }
            start = end + 1;
        }
    }
    std::set<std::string> seen_conf;
    const auto read_conf = [&](auto &&self, const std::string &file) -> void {
        if (!seen_conf.insert(file).second || seen_conf.size() > 64) return;
        std::ifstream in(file);
        for (std::string line; std::getline(in, line);) {
            if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.pop_back();
            std::size_t first = line.find_first_not_of(" \t");
            if (first == std::string::npos) continue;
            line = line.substr(first);
            if (line.rfind("include", 0) == 0 && line.size() > 7 && (line[7] == ' ' || line[7] == '\t')) {
                std::string pattern = line.substr(line.find_first_not_of(" \t", 7));
                if (!pattern.empty() && pattern[0] != '/') pattern = "/etc/" + pattern;
                glob_t g{};
                if (glob(pattern.c_str(), 0, nullptr, &g) == 0) {
                    for (std::size_t i = 0; i < g.gl_pathc; ++i) self(self, g.gl_pathv[i]);
                }
                globfree(&g);
            } else if (line[0] == '/') {
                dirs.push_back(line);
            }
        }
    };
    read_conf(read_conf, "/etc/ld.so.conf");
    for (const char *d : {"/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu", "/lib64", "/usr/lib64", "/lib", "/usr/lib"}) {
        dirs.emplace_back(d);
    }
    return dirs;
}

std::optional<std::string> find_on_system(const std::string &soname, const std::vector<std::string> &dirs) {
    for (const auto &dir : dirs) {
        const std::string path = dir + "/" + soname;
        if (is_file(path) && needed_of(path)) return path;
    }
    return std::nullopt;
}

// "libxml2.so.2" -> "libxml2": two sonames with the same stem are two versions of one library.
std::string stem_of(const std::string &soname) {
    const auto so = soname.find(".so");
    return so == std::string::npos ? soname : soname.substr(0, so);
}

bool system_has_tls_module() {
    for (const char *dir : {"/usr/lib/gio/modules", "/usr/lib64/gio/modules", "/usr/lib/x86_64-linux-gnu/gio/modules"}) {
        for (const char *module : {"libgiognutls.so", "libgioopenssl.so"}) {
            if (is_file(std::string(dir) + "/" + module)) return true;
        }
    }
    return false;
}

// Which of the bundled libraries the window needs on this system: the ones the system lacks, WebKitGTK and its
// JavaScriptCore always (they come as a pair), and a system library that needs another version of a bundled one
// (SteamOS's libxslt asks for libxml2.so.16 while the bundled WebKit brings libxml2.so.2: both would be loaded).
std::set<std::string> bundled_needed(const std::string &bundle, const std::vector<std::string> &roots,
                                     std::vector<std::string> &unresolved) {
    const auto dirs = library_dirs();
    std::map<std::string, std::string> chosen; // soname -> file
    std::set<std::string> from_bundle;
    std::deque<std::string> queue;
    const auto push_needed_of = [&](const std::string &file) {
        if (const auto needed = needed_of(file)) queue.insert(queue.end(), needed->begin(), needed->end());
    };
    for (const auto &root : roots) push_needed_of(root);
    const auto resolve = [&] {
        while (!queue.empty()) {
            const std::string soname = queue.front();
            queue.pop_front();
            if (chosen.count(soname) != 0) continue;
            const std::string in_bundle = bundle + "/" + soname;
            const bool pinned = soname == kWebKit || soname == kJavaScriptCore;
            std::optional<std::string> file;
            if (!pinned) file = find_on_system(soname, dirs);
            if (!file && is_file(in_bundle)) {
                file = in_bundle;
                from_bundle.insert(soname);
            }
            if (!file) {
                unresolved.push_back(soname);
                chosen[soname] = "";
                continue;
            }
            chosen[soname] = *file;
            push_needed_of(*file);
        }
    };
    resolve();
    for (bool changed = true; changed;) {
        changed = false;
        std::set<std::string> stems;
        for (const auto &soname : from_bundle) stems.insert(stem_of(soname));
        for (auto &[soname, file] : chosen) {
            if (file.empty() || from_bundle.count(soname) != 0 || !is_file(bundle + "/" + soname)) continue;
            const auto needed = needed_of(file);
            if (!needed) continue;
            for (const auto &n : *needed) {
                if (stems.count(stem_of(n)) != 0 && from_bundle.count(n) == 0) {
                    file = bundle + "/" + soname;
                    from_bundle.insert(soname);
                    push_needed_of(file);
                    changed = true;
                    break;
                }
            }
        }
        resolve();
    }
    return from_bundle;
}

// Points `link` at `target`, replacing what was there in one step (another launcher may be starting at the same time).
bool place_link(const std::string &target, const std::string &link) {
    char current[4096];
    const ssize_t n = readlink(link.c_str(), current, sizeof current - 1);
    if (n > 0 && std::string(current, static_cast<std::size_t>(n)) == target) return true;
    const std::string temporary = link + ".new" + std::to_string(getpid());
    unlink(temporary.c_str());
    return symlink(target.c_str(), temporary.c_str()) == 0 && rename(temporary.c_str(), link.c_str()) == 0;
}

bool own_private_dir(const std::string &path, std::string &error) {
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create " + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
        error = path + " belongs to another user";
        return false;
    }
    chmod(path.c_str(), 0700);
    return true;
}

// kRuntimeDir: the helpers (what the bundled WebKit starts) and lib/, the bundled libraries this system needs.
bool prepare_runtime(const std::string &bundle, const std::set<std::string> &libraries, std::string &error) {
    const std::string helpers = bundle + "/webkit2gtk-4.1";
    if (!own_private_dir(kRuntimeDir, error) || !own_private_dir(std::string(kRuntimeDir) + "/lib", error)) return false;
    for (const auto &name : list_dir(helpers)) {
        if (!place_link(helpers + "/" + name, std::string(kRuntimeDir) + "/" + name)) {
            error = "cannot link " + name + " in " + kRuntimeDir + ": " + std::strerror(errno);
            return false;
        }
    }
    const std::string lib = std::string(kRuntimeDir) + "/lib";
    for (const auto &name : libraries) {
        if (!place_link(bundle + "/" + name, lib + "/" + name)) {
            error = "cannot link " + name + " in " + lib + ": " + std::strerror(errno);
            return false;
        }
    }
    for (const auto &name : list_dir(lib)) { // from an earlier start: what this system no longer needs
        struct stat st {};
        if (libraries.count(name) == 0 && lstat((lib + "/" + name).c_str(), &st) == 0 && S_ISLNK(st.st_mode)) {
            unlink((lib + "/" + name).c_str());
        }
    }
    return true;
}

// Says what went wrong in a window when the desktop has a dialog program (SteamOS has both), and on the terminal.
void tell(const std::string &message) {
    std::fprintf(stderr, "MHForever: %s\n", message.c_str());
    if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) return;
    const pid_t pid = fork();
    if (pid == 0) {
        execlp("zenity", "zenity", "--error", "--title=MHForever", "--no-markup", ("--text=" + message).c_str(),
               static_cast<char *>(nullptr));
        execlp("kdialog", "kdialog", "--title", "MHForever", "--error", message.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    if (pid > 0) waitpid(pid, nullptr, 0);
}

// Sets `name` for the window; the programs the window starts get it back as it came (platform.cpp, Process::start):
// MHF_RESTORE_ENV lists the names changed, MHF_SYSTEM_<name> holds the value each had (absent: it was not set).
void set_env_for_window(const char *name, const std::string &value) {
    const std::string saved_as = std::string("MHF_SYSTEM_") + name;
    const char *changed = std::getenv("MHF_RESTORE_ENV");
    std::string list = changed != nullptr ? changed : "";
    if ((":" + list + ":").find(std::string(":") + name + ":") == std::string::npos) { // the first change keeps the original
        if (const char *old = std::getenv(name)) setenv(saved_as.c_str(), old, 1);
        list = list.empty() ? name : list + ":" + name;
        setenv("MHF_RESTORE_ENV", list.c_str(), 1);
    }
    if (value.empty()) {
        unsetenv(name);
    } else {
        setenv(name, value.c_str(), 1);
    }
}

std::string join_path(const std::string &first, const std::string &rest) {
    return first.empty() ? rest : rest.empty() ? first : first + ":" + rest;
}

// Steam starts a non-Steam shortcut with the libraries of its own runtime first in LD_LIBRARY_PATH: older builds of
// what GTK and WebKit need. The window does without them (the game gets them back, as Steam meant).
std::string without_steam_runtime(const char *list) {
    std::string kept;
    if (list == nullptr) return kept;
    const std::string all = list;
    for (std::size_t start = 0; start <= all.size();) {
        const auto end = std::min(all.find(':', start), all.size());
        const std::string entry = all.substr(start, end - start);
        const bool steam = entry.find("/steam-runtime") != std::string::npos ||
                           entry.find("/ubuntu12_32") != std::string::npos ||
                           entry.find("/ubuntu12_64") != std::string::npos;
        if (!entry.empty() && !steam) kept = join_path(kept, entry);
        start = end + 1;
    }
    return kept;
}

// The commands of the launcher without a window (cli.cpp, handles()): they need no WebKitGTK.
bool is_cli_command(const char *arg) {
    for (const char *c : {"--install", "--update", "--list", "--set", "--font", "--texture-pack", "--play", "--version",
                          "--help", "-h"}) {
        if (std::strcmp(arg, c) == 0) return true;
    }
    return false;
}

} // namespace

int main(int argc, char **argv) {
    const std::string dir = self_dir();
    const std::string window = dir + "/MHForever-window";
    const std::string cli = dir + "/MHForever-cli";
    std::vector<char *> args(argv, argv + argc);
    args.push_back(nullptr);

    if (argc > 1 && is_cli_command(argv[1]) && is_file(cli)) {
        args[0] = const_cast<char *>("MHForever-cli");
        execv(cli.c_str(), args.data());
    }

    // A launcher restarting itself after an update inherits nothing of this: platform.cpp gave it the original back.
    const char *library_path = std::getenv("LD_LIBRARY_PATH");
    const std::string system_path = without_steam_runtime(library_path);
    if (library_path != nullptr && system_path != library_path) {
        set_env_for_window("LD_LIBRARY_PATH", system_path);
    }

    const std::string bundle = dir + "/lib/webkit";
    const char *forced = std::getenv("MHF_WEBKIT");
    const auto dirs = library_dirs();
    const bool system_webkit = find_on_system(kWebKit, dirs) && find_on_system(kJavaScriptCore, dirs);
    bool use_bundle = is_dir(bundle) && !system_webkit;
    if (forced != nullptr && std::strcmp(forced, "system") == 0) use_bundle = false;
    if (forced != nullptr && std::strcmp(forced, "bundled") == 0) use_bundle = is_dir(bundle);

    if (!use_bundle && !system_webkit) {
        tell(is_file(cli) ? "The launcher's window needs WebKitGTK 4.1 (libwebkit2gtk-4.1), which this system lacks. "
                            "MHForever-cli, next to MHForever, installs and starts the game without it: run it with --help."
                          : "The launcher's window needs WebKitGTK 4.1 (libwebkit2gtk-4.1), which this system lacks.");
        return 1;
    }
    if (use_bundle) {
        std::vector<std::string> roots = {window};
        for (const auto &name : list_dir(bundle + "/webkit2gtk-4.1")) roots.push_back(bundle + "/webkit2gtk-4.1/" + name);
        roots.push_back(bundle + "/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so");
        const std::string tls = bundle + "/gio/libgiognutls.so";
        const bool add_tls = !system_has_tls_module() && is_file(tls);
        if (add_tls) roots.push_back(tls);
        std::vector<std::string> unresolved;
        const auto libraries = bundled_needed(bundle, roots, unresolved);
        for (const auto &name : unresolved) std::fprintf(stderr, "MHForever: %s was not found\n", name.c_str());
        std::string error;
        if (!prepare_runtime(bundle, libraries, error)) {
            tell("The launcher's window could not prepare its web view (" + error + ").");
            return 1;
        }
        set_env_for_window("LD_LIBRARY_PATH", join_path(std::string(kRuntimeDir) + "/lib", system_path));
        if (add_tls) {
            const char *modules = std::getenv("GIO_EXTRA_MODULES");
            set_env_for_window("GIO_EXTRA_MODULES", join_path(bundle + "/gio", modules != nullptr ? modules : ""));
        }
        if (std::getenv("MHF_LAUNCHER_DEBUG") != nullptr) {
            std::fprintf(stderr, "MHForever: bundled WebKitGTK, %zu libraries from %s\n", libraries.size(), bundle.c_str());
            for (const auto &name : libraries) std::fprintf(stderr, "  %s\n", name.c_str());
        }
    }
    args[0] = const_cast<char *>("MHForever"); // the program's name for GTK (the window's class, the desktop entry)
    execv(window.c_str(), args.data());
    tell("Could not start " + window + " (" + std::strerror(errno) + ").");
    return 1;
}
