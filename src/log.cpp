#include "log.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <pthread.h>
#include <string>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

namespace bg3le {
namespace {
std::FILE* g_log = nullptr;
std::mutex g_mutex;
char g_path[4096] = {0};
char g_marker[4096] = {0};
std::string g_previous_crash;

// Game logs kept in the logs directory, newest first.
constexpr std::size_t kKeepLogs = 10;
}

const char* log_path() { return g_path; }
const char* log_previous_crash() { return g_previous_crash.c_str(); }

namespace {

// Escape sequences belong on a terminal, not in a log file. Mods colour
// their own output -- Mod Configuration Menu writes 24-bit SGR codes -- and
// a log full of them is hard to read and worse to quote: paste it anywhere
// that drops the escape byte and the digits stay behind as text, which is
// where "[38;2;0;255;255;48;2;12;12;12m" in a bug report comes from.
void write_without_escapes(std::FILE* f, char const* text,
                           std::size_t length) {
    std::size_t at = 0;
    while (at < length) {
        if (text[at] != '\x1b') {
            std::fputc(text[at++], f);
            continue;
        }

        // CSI: ESC [ parameters intermediates final. Anything else escape-led
        // is two bytes, and an unterminated one ends the line.
        ++at;
        if (at < length && text[at] == '[') {
            ++at;
            while (at < length && (unsigned char)text[at] >= 0x20
                   && (unsigned char)text[at] <= 0x3f) {
                ++at;
            }
            if (at < length) ++at;  // the final byte
        } else if (at < length) {
            ++at;
        }
    }
}

}  // namespace

namespace {

bool host_is_bg3() {
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';
    const char* base = std::strrchr(exe, '/');
    return std::strcmp(base != nullptr ? base + 1 : exe, "bg3") == 0;
}

void make_dirs(std::string const& path) {
    for (std::size_t at = 1; at <= path.size(); ++at) {
        if (at == path.size() || path[at] == '/') ::mkdir(path.substr(0, at).c_str(), 0755);
    }
}

// <install>/logs beside <install>/lib, where install.py puts the library;
// otherwise the installer's default, $XDG_DATA_HOME/bg3le/logs.
std::string logs_dir() {
    Dl_info dl{};
    if (::dladdr(reinterpret_cast<void*>(&logs_dir), &dl) != 0 && dl.dli_fname != nullptr) {
        std::string lib(dl.dli_fname);
        const std::size_t slash = lib.rfind('/');
        if (slash != std::string::npos && slash >= 4 && lib.compare(slash - 4, 4, "/lib") == 0) {
            return lib.substr(0, slash - 4) + "/logs";
        }
    }
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg != nullptr && xdg[0] == '/') return std::string(xdg) + "/bg3le/logs";
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return "";
    return std::string(home) + "/.local/share/bg3le/logs";
}

void prune_logs(std::string const& dir) {
    std::vector<std::string> logs;
    if (DIR* d = ::opendir(dir.c_str())) {
        while (dirent* e = ::readdir(d)) {
            const std::size_t len = std::strlen(e->d_name);
            if (std::strncmp(e->d_name, "bg3le-", 6) == 0 && len > 4
                && std::strcmp(e->d_name + len - 4, ".log") == 0) {
                logs.emplace_back(e->d_name);
            }
        }
        ::closedir(d);
    }
    // Names start with the date, so they sort oldest first.
    std::sort(logs.begin(), logs.end());
    for (std::size_t i = 0; i + kKeepLogs < logs.size(); ++i) {
        ::unlink((dir + "/" + logs[i]).c_str());
    }
}

// Left by the crash handler; read and cleared by the next launch.
void take_crash_marker() {
    std::FILE* f = std::fopen(g_marker, "r");
    if (f == nullptr) return;
    char line[4096] = {0};
    if (std::fgets(line, sizeof(line), f) != nullptr) {
        line[std::strcspn(line, "\n")] = '\0';
        if (line[0] != '\0') g_previous_crash = std::string(g_marker, std::strrchr(g_marker, '/') + 1) + line;
    }
    std::fclose(f);
    ::unlink(g_marker);
}

}  // namespace

void log_init() {
    // Every process in the Steam runtime launch chain preloads us, so each
    // needs its own file or they truncate each other.
    const char* base = std::getenv("BG3LE_LOG");
    if (base != nullptr && base[0] != '\0') {
        std::snprintf(g_path, sizeof(g_path), "%s.%d", base, (int)::getpid());
        g_log = std::fopen(g_path, "w");
        return;
    }

    // By default only the game logs: the launch chain's shells and helpers
    // would only say bg3le stays inactive in them.
    if (!host_is_bg3()) return;
    const std::string dir = logs_dir();
    if (dir.empty()) return;
    make_dirs(dir);

    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    std::snprintf(g_path, sizeof(g_path), "%s/bg3le-%s-%d.log", dir.c_str(), stamp,
                  (int)::getpid());
    std::snprintf(g_marker, sizeof(g_marker), "%s/last-crash", dir.c_str());
    take_crash_marker();
    g_log = std::fopen(g_path, "w");
    prune_logs(dir);
}

void log_mark_crash() {
    if (g_marker[0] == '\0' || g_path[0] == '\0') return;
    const int fd = ::open(g_marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    const char* name = std::strrchr(g_path, '/');
    name = name != nullptr ? name + 1 : g_path;
    (void)!::write(fd, name, std::strlen(name));
    (void)!::write(fd, "\n", 1);
    ::close(fd);
}

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_log == nullptr) return;

    // Formatted first so the escapes can be taken out. A line longer than
    // the buffer -- a component dump, say -- gets one sized for it rather
    // than being truncated.
    char stack[2048];
    char* text = stack;
    std::vector<char> heap;

    va_list ap;
    va_start(ap, fmt);
    va_list measure;
    va_copy(measure, ap);
    int length = std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);

    if (length >= (int)sizeof(stack)) {
        heap.resize((std::size_t)length + 1);
        std::vsnprintf(heap.data(), heap.size(), fmt, measure);
        text = heap.data();
    }
    va_end(measure);
    if (length < 0) return;

    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    std::fprintf(g_log, "[%6ld.%03ld] ", ts.tv_sec, ts.tv_nsec / 1000000);
    write_without_escapes(g_log, text, (std::size_t)length);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

const char* thread_tag() {
    static thread_local char tag[48] = {};
    if (tag[0] == 0) {
        char name[16] = {};
        ::pthread_getname_np(::pthread_self(), name, sizeof(name));
        std::snprintf(tag, sizeof(tag), "tid %ld %s", (long)::syscall(SYS_gettid), name);
    }
    return tag;
}
}  // namespace bg3le
