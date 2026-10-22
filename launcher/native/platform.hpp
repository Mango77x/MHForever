#pragma once

// Operating-system services the launcher needs: paths, file dialogs, clipboard, processes, randomness.
// Windows uses Win32; Linux uses zenity/kdialog for dialogs and xclip/wl-copy for the clipboard when available.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mhf::launcher::platform {

std::filesystem::path executable_dir();

// Native file dialogs, modal to `parent` (a native window handle, may be null). `filter_name`/`patterns` like
// "Disc images" / {"*.iso"}. Return nothing when cancelled.
std::optional<std::filesystem::path> open_file(void *parent, const std::string &title, const std::string &filter_name,
                                               const std::vector<std::string> &patterns);
std::optional<std::filesystem::path> save_file(void *parent, const std::string &title, const std::string &default_name);

bool copy_to_clipboard(void *parent, const std::string &utf8);

void random_bytes(std::uint8_t *out, std::size_t size);

// Window chrome: dark title bar and minimum size (Windows), application icon/name where supported.
void style_window(void *window);
void show_error(const std::string &title, const std::string &message);

class Process {
public:
    // Starts `program` with `args` in `cwd`; stdout and stderr go to `log_file`. No console window on Windows.
    static std::optional<Process> start(const std::filesystem::path &program, const std::vector<std::string> &args,
                                        const std::filesystem::path &cwd, const std::filesystem::path &log_file,
                                        std::string &error);
    Process(Process &&other) noexcept;
    Process &operator=(Process &&other) noexcept;
    Process(const Process &) = delete;
    Process &operator=(const Process &) = delete;
    ~Process();

    // Blocks until the process ends; returns its exit code.
    int wait();
    // Exit code if it has already ended.
    std::optional<int> poll();

private:
    Process() = default;
#if defined(_WIN32)
    void *handle_{};
#else
    int pid_{-1};
#endif
};

std::filesystem::path from_utf8(const std::string &s);
std::string to_utf8(const std::filesystem::path &p);

} // namespace mhf::launcher::platform
