#include "platform.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shobjidl.h>
#else
#include <fcntl.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !defined(_WIN32)
// The application icon, embedded by launcher/embed.cmake from assets/branding/icon-256.png.
extern const unsigned char kAppIconPng[];
extern const std::size_t kAppIconPngSize;
#endif

namespace mhf::launcher::platform {

namespace fs = std::filesystem;

fs::path from_utf8(const std::string &s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

std::string to_utf8(const fs::path &p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char *>(u.data()), u.size());
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string &s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Quotes one argument for CreateProcess (the rules of CommandLineToArgvW).
std::wstring quote(const std::wstring &arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
        } else if (c == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
            continue;
        } else {
            slashes = 0;
        }
        out.push_back(c);
    }
    out.append(slashes, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring filter_string(const std::string &name, const std::vector<std::string> &patterns) {
    std::wstring joined;
    for (const auto &p : patterns) {
        if (!joined.empty()) joined += L";";
        joined += widen(p);
    }
    std::wstring f = widen(name) + L" (" + joined + L")";
    f.push_back(L'\0');
    f += joined;
    f.push_back(L'\0');
    f += L"All files (*.*)";
    f.push_back(L'\0');
    f += L"*.*";
    f.push_back(L'\0');
    f.push_back(L'\0');
    return f;
}

} // namespace

fs::path executable_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    while (true) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

std::optional<fs::path> open_file(void *parent, const std::string &title, const std::string &filter_name,
                                  const std::vector<std::string> &patterns) {
    wchar_t file[4096] = {};
    const std::wstring filter = filter_string(filter_name, patterns);
    const std::wstring wtitle = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = static_cast<HWND>(parent);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return fs::path(file);
}

std::optional<fs::path> open_folder(void *parent, const std::string &title) {
    // The UI thread already runs COM (the web view needs it); this only makes sure.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IFileOpenDialog *dialog = nullptr;
    std::optional<fs::path> result;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        const std::wstring wtitle = widen(title);
        dialog->SetTitle(wtitle.c_str());
        if (SUCCEEDED(dialog->Show(static_cast<HWND>(parent)))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    result = fs::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

std::optional<fs::path> save_file(void *parent, const std::string &title, const std::string &default_name) {
    wchar_t file[4096] = {};
    const std::wstring name = widen(default_name);
    wcsncpy(file, name.c_str(), std::size(file) - 1);
    const std::wstring wtitle = widen(title);
    const wchar_t filter[] = L"Identity backup (*.json)\0*.json\0\0";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = static_cast<HWND>(parent);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = static_cast<DWORD>(std::size(file));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.lpstrDefExt = L"json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return fs::path(file);
}

bool copy_to_clipboard(void *parent, const std::string &utf8) {
    const std::wstring w = widen(utf8);
    if (!OpenClipboard(static_cast<HWND>(parent))) return false;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    bool ok = false;
    if (mem != nullptr) {
        auto *dst = static_cast<wchar_t *>(GlobalLock(mem));
        std::memcpy(dst, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
        if (!ok) GlobalFree(mem);
    }
    CloseClipboard();
    return ok;
}

void random_bytes(std::uint8_t *out, std::size_t size) {
    BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

void style_window(void *window) {
    HWND hwnd = static_cast<HWND>(window);
    if (hwnd == nullptr) return;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    const COLORREF caption = RGB(0x0b, 0x0c, 0x10);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof caption);
    HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (icon != nullptr) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
    }
}

void show_error(const std::string &title, const std::string &message) {
    MessageBoxW(nullptr, widen(message).c_str(), widen(title).c_str(), MB_OK | MB_ICONERROR);
}

std::optional<Process> Process::start(const fs::path &program, const std::vector<std::string> &args, const fs::path &cwd,
                                      const fs::path &log_file, std::string &error) {
    std::wstring cmd = quote(program.wstring());
    for (const auto &a : args) cmd += L" " + quote(widen(a));
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(log_file.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    if (log != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = log;
        si.hStdError = log;
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, log != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW,
                                   nullptr, cwd.wstring().c_str(), &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        error = "Windows could not start " + to_utf8(program.filename()) + " (error " + std::to_string(GetLastError()) + ").";
        return std::nullopt;
    }
    CloseHandle(pi.hThread);
    Process p;
    p.handle_ = pi.hProcess;
    return p;
}

Process::Process(Process &&other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
Process &Process::operator=(Process &&other) noexcept {
    std::swap(handle_, other.handle_);
    return *this;
}
Process::~Process() {
    if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
}

int Process::wait() {
    WaitForSingleObject(static_cast<HANDLE>(handle_), INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    return static_cast<int>(code);
}

std::optional<int> Process::poll() {
    if (WaitForSingleObject(static_cast<HANDLE>(handle_), 0) != WAIT_OBJECT_0) return std::nullopt;
    DWORD code = 0;
    GetExitCodeProcess(static_cast<HANDLE>(handle_), &code);
    return static_cast<int>(code);
}

#else // POSIX (Linux, Steam Deck)

fs::path executable_dir() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path() : self.parent_path();
}

std::optional<fs::path> open_file(void *parent, const std::string &title, const std::string &filter_name,
                                  const std::vector<std::string> &patterns) {
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(title.c_str(), parent ? GTK_WINDOW(parent) : nullptr,
                                                               GTK_FILE_CHOOSER_ACTION_OPEN, "_Open", "_Cancel");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, filter_name.c_str());
    for (const auto &p : patterns) {
        gtk_file_filter_add_pattern(filter, p.c_str());
        std::string upper = p;
        for (auto &c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        gtk_file_filter_add_pattern(filter, upper.c_str());
    }
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all);
    std::optional<fs::path> result;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (name != nullptr) {
            result = fs::path(name);
            g_free(name);
        }
    }
    g_object_unref(dialog);
    return result;
}

std::optional<fs::path> open_folder(void *parent, const std::string &title) {
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(title.c_str(), parent ? GTK_WINDOW(parent) : nullptr,
                                                               GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "_Choose", "_Cancel");
    std::optional<fs::path> result;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (name != nullptr) {
            result = fs::path(name);
            g_free(name);
        }
    }
    g_object_unref(dialog);
    return result;
}

std::optional<fs::path> save_file(void *parent, const std::string &title, const std::string &default_name) {
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(title.c_str(), parent ? GTK_WINDOW(parent) : nullptr,
                                                               GTK_FILE_CHOOSER_ACTION_SAVE, "_Save", "_Cancel");
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), default_name.c_str());
    std::optional<fs::path> result;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char *name = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (name != nullptr) {
            result = fs::path(name);
            g_free(name);
        }
    }
    g_object_unref(dialog);
    return result;
}

bool copy_to_clipboard(void *, const std::string &utf8) {
    GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clipboard, utf8.c_str(), static_cast<gint>(utf8.size()));
    gtk_clipboard_store(clipboard);
    return true;
}

void random_bytes(std::uint8_t *out, std::size_t size) {
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (urandom) urandom.read(reinterpret_cast<char *>(out), static_cast<std::streamsize>(size));
    if (urandom && urandom.gcount() == static_cast<std::streamsize>(size)) return;
    std::random_device fallback; // no /dev/urandom (an unusual container): the standard device still works
    for (std::size_t i = 0; i < size; ++i) out[i] = static_cast<std::uint8_t>(fallback());
}

void style_window(void *window) {
    // The name and icon the desktop shows for the window (WM_CLASS, the task bar, alt-tab).
    g_set_prgname("MHForever");
    g_set_application_name("MHForever");
    if (window == nullptr) return;
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    if (gdk_pixbuf_loader_write(loader, kAppIconPng, static_cast<gsize>(kAppIconPngSize), nullptr)) {
        gdk_pixbuf_loader_close(loader, nullptr);
        if (GdkPixbuf *icon = gdk_pixbuf_loader_get_pixbuf(loader)) gtk_window_set_icon(GTK_WINDOW(window), icon);
    }
    g_object_unref(loader);
}

void show_error(const std::string &title, const std::string &message) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
    if (gtk_init_check(nullptr, nullptr)) {
        GtkWidget *d = gtk_message_dialog_new(nullptr, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "%s",
                                              message.c_str());
        gtk_window_set_title(GTK_WINDOW(d), title.c_str());
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    }
}

std::optional<Process> Process::start(const fs::path &program, const std::vector<std::string> &args, const fs::path &cwd,
                                      const fs::path &log_file, std::string &error) {
    // The child changes to `cwd` before it starts the program, so a relative program must be resolved here.
    std::error_code ec;
    const fs::path absolute = fs::absolute(program, ec);
    std::vector<std::string> argv_store;
    argv_store.push_back((ec ? program : absolute).string());
    argv_store.insert(argv_store.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &a : argv_store) argv.push_back(a.data());
    argv.push_back(nullptr);

    // A pipe tells us why exec failed: the child can only report it through its exit code otherwise.
    int report[2] = {-1, -1};
    if (pipe(report) != 0) {
        error = "Could not start the game process (" + std::string(std::strerror(errno)) + ").";
        return std::nullopt;
    }
    if (fcntl(report[1], F_SETFD, FD_CLOEXEC) != 0) {
        close(report[0]);
        close(report[1]);
        error = "Could not start the game process (" + std::string(std::strerror(errno)) + ").";
        return std::nullopt;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        const int failed = errno;
        close(report[0]);
        close(report[1]);
        error = "Could not start the game process (" + std::string(std::strerror(failed)) + ").";
        return std::nullopt;
    }
    if (pid == 0) {
        close(report[0]);
        const auto report_failure = [&](int code) {
            const ssize_t ignored = write(report[1], &code, sizeof code);
            (void)ignored;
        };
        if (chdir(cwd.c_str()) != 0) {
            report_failure(errno);
            _exit(126);
        }
        const int fd = open(log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        execv(argv[0], argv.data());
        report_failure(errno);
        _exit(127);
    }
    close(report[1]);
    int child_error = 0;
    ssize_t read_bytes = 0;
    do {
        read_bytes = read(report[0], &child_error, sizeof child_error);
    } while (read_bytes < 0 && errno == EINTR);
    close(report[0]);
    if (read_bytes == static_cast<ssize_t>(sizeof child_error)) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        error = "Could not start " + to_utf8(program.filename()) + " (" + std::string(std::strerror(child_error)) + ").";
        return std::nullopt;
    }
    Process p;
    p.pid_ = pid;
    return p;
}

Process::Process(Process &&other) noexcept : pid_(other.pid_) { other.pid_ = -1; }
Process &Process::operator=(Process &&other) noexcept {
    std::swap(pid_, other.pid_);
    return *this;
}
Process::~Process() = default;

int Process::wait() {
    int status = 0;
    while (waitpid(pid_, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

std::optional<int> Process::poll() {
    int status = 0;
    const pid_t r = waitpid(pid_, &status, WNOHANG);
    if (r == 0) return std::nullopt;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

#endif

} // namespace mhf::launcher::platform
