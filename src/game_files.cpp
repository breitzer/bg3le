// Reading the game's own data files out of its archives.
//
// See game_files.h for why this exists rather than calling the engine.

#include "game_files.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "log.h"
#include "pak.h"

namespace bg3le {

namespace {

std::mutex g_lock;

// Every archive under the install, highest priority first, and which
// archive holds each path. Built once: the game's own archives hold half a
// million entries between them, and pak.cpp deliberately does not cache a
// list that large, so asking it per lookup would decode them again each
// time.
struct Index {
    bool Built{false};
    std::map<std::string, std::string> Files;
};

Index& index() {
    static Index it;
    return it;
}

std::vector<std::string> paks_in(std::string const& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return out;

    while (dirent* e = readdir(d)) {
        std::string const name = e->d_name;
        if (name.size() < 5) continue;
        if (name.compare(name.size() - 4, 4, ".pak") != 0) continue;
        out.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// The archives the engine mounts, in the order it resolves them.
std::vector<std::string> archives() {
    std::string const& root = game_data_root();
    if (root.empty()) return {};

    std::vector<std::string> all = paks_in(root);
    for (std::string const& pak : paks_in(root + "/Localization")) {
        all.push_back(pak);
    }

    // Stable, so two archives of equal priority keep their name order.
    std::stable_sort(all.begin(), all.end(),
                     [](std::string const& a, std::string const& b) {
                         unsigned pa = 0;
                         unsigned pb = 0;
                         pak_priority(a.c_str(), &pa);
                         pak_priority(b.c_str(), &pb);
                         return pa > pb;
                     });
    return all;
}

void build() {
    Index& it = index();
    if (it.Built) return;
    it.Built = true;

    std::size_t archiveCount = 0;
    for (std::string const& pak : archives()) {
        const bool listed = pak_list(pak.c_str(), [&](char const* name) {
            // First writer wins, and the archives arrive highest priority
            // first, so this is the engine's own precedence.
            it.Files.emplace(name, pak);
        });
        if (listed) ++archiveCount;
    }

    logf("game files: indexed %zu paths in %zu archives", it.Files.size(),
         archiveCount);
}

bool safe(char const* relative) {
    if (relative == nullptr || relative[0] == '\0') return false;
    if (relative[0] == '/') return false;
    return std::strstr(relative, "..") == nullptr;
}

// A loose file under Data/, which overrides the archives.
bool read_loose(char const* relative, std::string* out) {
    std::string const& root = game_data_root();
    if (root.empty()) return false;

    std::FILE* f = std::fopen((root + "/" + relative).c_str(), "rb");
    if (f == nullptr) return false;

    std::string body;
    char buf[64 * 1024];
    while (const std::size_t got = std::fread(buf, 1, sizeof(buf), f)) {
        body.append(buf, got);
    }
    const bool ok = std::ferror(f) == 0;
    std::fclose(f);
    if (!ok) return false;

    if (out != nullptr) *out = std::move(body);
    return true;
}

}  // namespace

std::string const& game_data_root() {
    static std::string const root = [] {
        char exe[4096];
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return std::string();
        exe[n] = '\0';

        std::string path(exe);
        const std::size_t bin = path.rfind("/bin/");
        if (bin == std::string::npos) return std::string();
        return path.substr(0, bin) + "/Data";
    }();
    return root;
}

bool game_file_read(char const* relative, std::string* out) {
    if (!safe(relative)) return false;
    if (read_loose(relative, out)) return true;

    std::string pak;
    {
        const std::lock_guard<std::mutex> held(g_lock);
        build();
        auto const in = index().Files.find(relative);
        if (in == index().Files.end()) return false;
        pak = in->second;
    }

    bool found = false;
    pak_read(
        pak.c_str(),
        [&](char const* name) { return std::strcmp(name, relative) == 0; },
        [&](char const*, char const* data, std::size_t size) {
            if (out != nullptr) out->assign(data, size);
            found = true;
        });
    return found;
}

bool game_file_exists(char const* relative) {
    if (!safe(relative)) return false;
    if (read_loose(relative, nullptr)) return true;

    const std::lock_guard<std::mutex> held(g_lock);
    build();
    return index().Files.find(relative) != index().Files.end();
}

std::vector<std::string> game_files_under(std::string const& prefix) {
    std::vector<std::string> out;
    const std::lock_guard<std::mutex> held(g_lock);
    build();
    auto const& files = index().Files;
    for (auto it = files.lower_bound(prefix);
         it != files.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it) {
        out.push_back(it->first);
    }
    return out;
}

}  // namespace bg3le
