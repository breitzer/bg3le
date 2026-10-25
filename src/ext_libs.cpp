// The parts of Ext.Utils, Ext.IO, Ext.Timer and Ext.Debug that need more
// than Lua: clocks, the game's version, a GUID generator, and file access
// under the profile and data roots.
//
// Everything here mirrors the behaviour of its counterpart in bg3se's
// Lua/Libs, with the platform-specific halves replaced. Upstream's
// MicrosecTime is QueryPerformanceCounter against a counter taken at
// startup; this keeps the same shape with a steady_clock baseline.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <dirent.h>
#include <dlfcn.h>
#include <fnmatch.h>
#include <map>
#include <mutex>
#include <vector>

#include "lauxlib.h"
#include "lua.h"

#include "debug_server.h"
#include "game_files.h"
#include "log.h"
#include "pak.h"

namespace bg3le {
bool game_file_read(char const* relative, std::string* out);
bool engine_read_file(char const* relative, std::string* out);

namespace {

std::chrono::steady_clock::time_point const kStart =
    std::chrono::steady_clock::now();

// Defined with the archive scanning further down; a data read falls back
// to the mod archives, and the scan is what knows where they are.
std::vector<std::string>& mod_archives();
std::map<std::string, std::string>& mod_files();
void scan_mod_archives();

// The game's profile directory, PathRootType::UserProfile: Mods and
// PlayerProfiles are under it.
std::string profile_root() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::string(home) + "/.local/share/Larian Studios/Baldur's Gate 3";
}

// Where SaveFile writes and LoadFile reads by default: upstream's
// ToPath("/Script Extender", LocalAppData), so a mod's saved settings are
// where bg3se keeps them.
std::string extender_root() {
    const std::string profile = profile_root();
    return profile.empty() ? profile : profile + "/Script Extender";
}

// PathRootType::Data, the game's own install.
std::string data_root() {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    exe[n] = '\0';

    std::string path(exe);
    const std::size_t bin = path.rfind("/bin/");
    if (bin == std::string::npos) return {};
    return path.substr(0, bin) + "/Data";
}

// Upstream's IsSafeRelativePath, its refusals printed as its OsiError does:
// Windows' illegal filename characters ('*' allowed in a glob), no trailing
// space or dot, no traversal. Bytes from 0x80 are UTF-8 and pass.
bool safe_relative(char const* path, bool isGlob, bool allowEmpty) {
    auto refuse = [](std::string const& text) {
        debug_server_output(text.c_str(), 2);
        logf("IO: %s", text.c_str());
        return false;
    };
    const std::string quoted = std::string("'") + path + "'";
    if (path[0] == '\0') return allowEmpty || refuse("IO path cannot be empty");
    for (char const* c = path; *c != '\0'; ++c) {
        if ((unsigned char)*c < 0x20 || std::strchr("<>:\"|?", *c) != nullptr || (*c == '*' && !isGlob)) {
            return refuse("Illegal character in filename: " + quoted);
        }
    }
    const char last = path[std::strlen(path) - 1];
    if (last == ' ' || last == '.') return refuse("Path cannot end with space or dot: " + quoted);
    if (std::strstr(path, "..") != nullptr) return refuse("Path cannot contain traversal: " + quoted);
    return true;
}

// Refuses to leave the root it was given: a mod naming "../../.ssh/id_rsa"
// should not reach it, and upstream's script::LoadExternalFile checks the
// same way.
bool resolve_under(std::string const& root, char const* relative,
                   std::string* out) {
    if (root.empty() || relative == nullptr || !safe_relative(relative, false, false)) {
        return false;
    }
    *out = root + "/" + relative;
    return true;
}

bool make_parents(std::string const& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return true;

    std::string dir = path.substr(0, slash);
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        std::string const part = dir.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

}  // namespace

// ---- clocks ---------------------------------------------------------------

extern "C" int bg3le_ext_monotonic_time(lua_State* L) {
    using namespace std::chrono;
    lua_pushinteger(L, (lua_Integer)duration_cast<milliseconds>(
                           steady_clock::now().time_since_epoch()).count());
    return 1;
}

extern "C" int bg3le_ext_microsec_time(lua_State* L) {
    using namespace std::chrono;
    const auto since = steady_clock::now() - kStart;
    lua_pushnumber(L, (lua_Number)duration_cast<nanoseconds>(since).count()
                          / 1000.0);
    return 1;
}

extern "C" int bg3le_ext_clock_epoch(lua_State* L) {
    using namespace std::chrono;
    lua_pushinteger(L, (lua_Integer)duration_cast<seconds>(
                           system_clock::now().time_since_epoch()).count());
    return 1;
}

// Upstream's std::format("{0:%F} {0:%T}", system_clock::now()): UTC, and the
// seconds to Windows' 100 ns tick.
extern "C" int bg3le_ext_clock_time(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    std::tm parts{};
    gmtime_r(&ts.tv_sec, &parts);

    char text[64];
    const std::size_t n = std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
    std::snprintf(text + n, sizeof(text) - n, ".%07ld", ts.tv_nsec / 100);
    lua_pushstring(L, text);
    return 1;
}

// ---- identity -------------------------------------------------------------

extern "C" int bg3le_ext_generate_guid(lua_State* L) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint8_t bytes[16];
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t word = rng();
        std::memcpy(bytes + i, &word, 8);
    }
    // Version 4, variant 1, as Guid::Generate does.
    bytes[6] = (std::uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (std::uint8_t)((bytes[8] & 0x3f) | 0x80);

    char text[37];
    std::snprintf(text, sizeof(text),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                  bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    lua_pushstring(L, text);
    return 1;
}

// The game's version, in the form upstream reports.
//
// There is no PE version resource to read here, so it comes from the
// version string the binary carries, "4.1.1.7398727". Larian's last
// component packs the rest: 73 * 100000 + 98 * 1000 + 727 = 7398727, which
// is v4.73.98.727 -- exactly what the Windows extender reported for this
// same build in reference/utils-shape.txt.
// The game's version, scanned out of its own binary once. Exposed as a
// plain function as well so the startup banner can report it the way
// upstream's does, without going through Lua.
std::string const& game_version_text() {
    static std::string cached;
    static bool scanned = false;
    if (scanned) return cached;
    scanned = true;

    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return cached;
    exe[n] = '\0';

    std::FILE* f = std::fopen(exe, "rb");
    if (f == nullptr) return cached;

    // A bounded scan for the version literal rather than a full read of a
    // 300 MB binary.
    std::string window;
    std::vector<char> block(1u << 20);
    unsigned major = 0;
    unsigned a = 0;
    unsigned b = 0;
    unsigned packed = 0;
    bool found = false;
    while (!found) {
        const std::size_t got = std::fread(block.data(), 1, block.size(), f);
        if (got == 0) break;

        window.append(block.data(), got);
        for (std::size_t i = 0; i + 8 < window.size() && !found; ++i) {
            if (window[i] < '0' || window[i] > '9') continue;
            if (std::sscanf(window.c_str() + i, "%u.%u.%u.%u", &major, &a, &b,
                            &packed) != 4) {
                continue;
            }
            if (major == 4 && packed > 100000) found = true;
        }
        if (window.size() > (1u << 20)) {
            window.erase(0, window.size() - 64);
        }
    }
    std::fclose(f);
    if (!found) return cached;

    char text[64];
    std::snprintf(text, sizeof(text), "v%u.%u.%u.%u", major, packed / 100000,
                  (packed / 1000) % 100, packed % 1000);
    cached = text;
    return cached;
}

extern "C" char const* bg3le_game_version() {
    return game_version_text().c_str();
}

extern "C" int bg3le_ext_game_version(lua_State* L) {
    std::string const& version = game_version_text();
    if (version.empty()) return 0;
    lua_pushstring(L, version.c_str());
    return 1;
}

extern "C" int bg3le_ext_command_line(lua_State* L) {
    std::FILE* f = std::fopen("/proc/self/cmdline", "rb");
    if (f == nullptr) return 0;

    std::string all;
    char block[4096];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), f)) > 0) {
        all.append(block, got);
    }
    std::fclose(f);

    lua_newtable(L);
    int n = 0;
    std::size_t start = 0;
    while (start < all.size()) {
        const std::size_t end = all.find('\0', start);
        const std::string arg = all.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!arg.empty()) {
            lua_pushstring(L, arg.c_str());
            lua_rawseti(L, -2, ++n);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return 1;
}

// ---- files ----------------------------------------------------------------

// Ext.IO.LoadFile(path[, context]) where context is "user" or "data".
extern "C" int bg3le_ext_load_file(lua_State* L) {
    char const* relative = luaL_checkstring(L, 1);
    char const* context = lua_isnoneornil(L, 2) ? "user"
                                                : luaL_checkstring(L, 2);

    std::string root;
    if (std::strcmp(context, "user") == 0) {
        root = extender_root();
    } else if (std::strcmp(context, "data") == 0) {
        root = data_root();
    } else {
        return luaL_error(L, "Unknown file loading context: %s", context);
    }

    std::string path;
    if (!resolve_under(root, relative, &path)) return 0;

    // Upstream reads this context through the engine's FileReader, which
    // sees what the engine sees and honours path overrides.
    if (std::strcmp(context, "data") == 0) {
        std::string contents;
        if (bg3le::engine_read_file(relative, &contents)) {
            lua_pushlstring(L, contents.data(), contents.size());
            return 1;
        }
    }

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) {
        // Not on disk. Upstream reads this context through the engine's
        // virtual file system, which has the mod archives mounted, so a
        // packed mod's file is found there -- Mod Configuration Menu
        // reads every other mod's blueprint that way.
        if (std::strcmp(context, "data") != 0) return 0;
        std::string contents;
        // A mod's archive first, then the game's own, as the engine's file
        // system layers them.
        if (!bg3le::mod_file_read(relative, &contents)
            && !bg3le::game_file_read(relative, &contents)) {
            return 0;
        }
        lua_pushlstring(L, contents.data(), contents.size());
        return 1;
    }

    std::string contents;
    char block[65536];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), f)) > 0) {
        contents.append(block, got);
    }
    std::fclose(f);

    lua_pushlstring(L, contents.data(), contents.size());
    return 1;
}

// Ext.IO.SaveFile(path, contents), and AppendFile through the same path.
extern "C" int bg3le_ext_save_file(lua_State* L) {
    char const* relative = luaL_checkstring(L, 1);
    std::size_t length = 0;
    char const* contents = luaL_checklstring(L, 2, &length);
    const bool append = lua_toboolean(L, 3) != 0;

    std::string path;
    if (!resolve_under(extender_root(), relative, &path)
        || !make_parents(path)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    std::FILE* f = std::fopen(path.c_str(), append ? "ab" : "wb");
    if (f == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const bool ok = length == 0 || std::fwrite(contents, 1, length, f) == length;
    std::fclose(f);

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

namespace {

void collect_files(std::string const& dir, std::string const& prefix, char const* glob,
                   bool recursive, std::vector<std::string>* out) {
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return;
    while (dirent* entry = readdir(d)) {
        const std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        const std::string full = dir + "/" + name;
        // A symlinked directory is not entered, as a recursive_directory_iterator
        // does not by default, so a link loop cannot recurse forever.
        struct stat st{};
        if (lstat(full.c_str(), &st) != 0) continue;
        const bool link = S_ISLNK(st.st_mode);
        if (link && stat(full.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (recursive && !link) collect_files(full, prefix + name + "/", glob, true, out);
        } else if (fnmatch(glob, name.c_str(), 0) == 0) {
            out->push_back(prefix + name);
        }
    }
    closedir(d);
}

}  // namespace

// Ext.IO.FindFiles(path, glob[, context[, recursive[, checkPackedFiles]]]),
// upstream's script::FindFiles: files under the root's path matching glob,
// named relative to that path. "user" is the profile root, not Script Extender.
extern "C" int bg3le_ext_find_files(lua_State* L) {
    char const* relative = luaL_checkstring(L, 1);
    char const* glob = luaL_checkstring(L, 2);
    char const* context = lua_isnoneornil(L, 3) ? "user" : luaL_checkstring(L, 3);
    const bool recursive = lua_toboolean(L, 4) != 0;
    const bool packed = lua_isnoneornil(L, 5) || lua_toboolean(L, 5) != 0;

    std::string root;
    if (std::strcmp(context, "user") == 0) {
        root = profile_root();
    } else if (std::strcmp(context, "data") == 0) {
        root = data_root();
    } else {
        return luaL_error(L, "Unknown file loading context: %s", context);
    }

    lua_newtable(L);
    // Upstream's checks: an empty path is the root of the user context only.
    if (root.empty() || !safe_relative(relative, false, std::strcmp(context, "user") == 0)
        || !safe_relative(glob, true, false)) {
        return 1;
    }
    std::string base(relative);
    while (!base.empty() && base.back() == '/') base.pop_back();
    while (!base.empty() && base.front() == '/') base.erase(0, 1);

    std::vector<std::string> names;
    collect_files(base.empty() ? root : root + "/" + base, "", glob, recursive, &names);
    if (packed && std::strcmp(context, "data") == 0) {
        // The engine's file system has the mods' archives and the game's own mounted.
        const std::string prefix = base.empty() ? std::string() : base + "/";
        auto consider = [&](std::string const& name) {
            if (name.compare(0, prefix.size(), prefix) != 0) return;
            const std::string rest = name.substr(prefix.size());
            const std::size_t slash = rest.rfind('/');
            if (slash != std::string::npos && !recursive) return;
            const std::string file = slash == std::string::npos ? rest : rest.substr(slash + 1);
            if (fnmatch(glob, file.c_str(), 0) == 0) names.push_back(rest);
        };
        scan_mod_archives();
        for (auto const& entry : mod_files()) consider(entry.first);
        for (std::string const& name : bg3le::game_files_under(prefix)) consider(name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushlstring(L, names[i].data(), names[i].size());
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

// Ext._Internal.WriteDataFile(relative, contents) -> path, or nil and why.
//
// Under the game's Data directory rather than the profile, which is where
// upstream puts the IDE helpers: a mod's own source tree is what an editor
// has open. Ext.IO.SaveFile deliberately cannot write here, and this is not
// a general-purpose writer either -- the caller is the helper generator, and
// the same climb-out refusal applies.
extern "C" int bg3le_ext_write_data_file(lua_State* L) {
    char const* relative = luaL_checkstring(L, 1);
    std::size_t length = 0;
    char const* contents = luaL_checklstring(L, 2, &length);

    std::string path;
    if (!resolve_under(data_root(), relative, &path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s does not resolve under the Data directory",
                        relative);
        return 2;
    }
    if (!make_parents(path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not create the directories for %s",
                        path.c_str());
        return 2;
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not open %s for writing", path.c_str());
        return 2;
    }
    const bool ok =
        length == 0 || std::fwrite(contents, 1, length, f) == length;
    std::fclose(f);

    if (!ok) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not write %s", path.c_str());
        return 2;
    }
    lua_pushstring(L, path.c_str());
    return 1;
}

// ---- misc -----------------------------------------------------------------

extern "C" int bg3le_ext_memory_usage(lua_State* L) {
    // Upstream reports the extender's own allocation total. bg3le does not
    // track one, so this is Lua's heap, which is what a mod asking about
    // memory usage from Lua is generally after.
    const int kb = lua_gc(L, LUA_GCCOUNT, 0);
    lua_pushinteger(L, (lua_Integer)kb * 1024);
    return 1;
}

extern "C" int bg3le_ext_show_error(lua_State* L) {
    char const* message = luaL_checkstring(L, 1);
    logf("Ext.Utils.ShowError: %s", message);
    std::fprintf(stderr, "bg3le: %s\n", message);
    return 0;
}

extern "C" bool bg3le_imgui_show_error(char const* title, char const* message);

// Ext.Utils.ShowErrorAndExitGame: upstream's ShowStartupError with exit set --
// the message in a dialog, then the game ends when it is dismissed. The
// dialog is the overlay's (src/vendor/imgui_overlay.cpp).
extern "C" int bg3le_ext_show_error_and_exit(lua_State* L) {
    char const* message = luaL_checkstring(L, 1);
    logf("Ext.Utils.ShowErrorAndExitGame: %s", message);
    std::fprintf(stderr, "bg3le: %s\n", message);
    if (bg3le_imgui_show_error("Script Extender", message)) {
        // It shows next frame and ends the game when dismissed. Nothing after
        // this call runs, as nothing would after upstream's.
        return luaL_error(L, "%s (the game closes when this is dismissed)", message);
    }
    // run-native.sh's headless runs have no screen of their own, and SDL
    // would put the box on the desktop's instead.
    char const* headless = std::getenv("HEADLESS");
    if (!(headless != nullptr && std::strcmp(headless, "1") == 0)) {
        // Only without the overlay: SDL's own box, which looks its age.
        using ShowProc = int (*)(std::uint32_t, char const*, char const*, void*);
        auto show = reinterpret_cast<ShowProc>(
            ::dlsym(RTLD_DEFAULT, "SDL_ShowSimpleMessageBox"));
        constexpr std::uint32_t kSdlMessageBoxError = 0x10;
        if (show != nullptr) show(kSdlMessageBoxError, "Script Extender", message, nullptr);
    }
    std::_Exit(1);
}


// ---- mod archives ---------------------------------------------------------
//
// Mods ship their Lua inside a .pak, so the loader has to read one. The
// alternative -- loose files only -- means none of an installed mod set
// runs, which is most of what a script extender is for.

namespace {

struct PakModule {
    std::string Pak;   // path of the archive it was found in
    std::string Name;  // the folder under Mods/ inside the archive
    std::string Uuid;
    // Straight out of the mod's own meta.lsx, so a mod the engine has not
    // loaded can still be described.
    std::string ModName;
    std::string Author;
    std::string Description;
    std::string Version;
};

// Where installed mods live. The native build reads both: the profile's
// Mods directory, which is the Windows AppData location's counterpart, and
// the install's own Data/Mods, which is where the game keeps its unpacked
// modules and where a Linux install is usually told to put mod paks.
std::vector<std::string> mods_roots() {
    std::vector<std::string> roots;
    const std::string profile = profile_root();
    if (!profile.empty()) roots.push_back(profile + "/Mods");
    const std::string data = data_root();
    if (!data.empty()) roots.push_back(data + "/Mods");
    return roots;
}

// The module name in "Mods/<name>/ScriptExtender/Config.json", or empty.
std::string module_of_config(char const* entry) {
    static char const* const kPrefix = "Mods/";
    static char const* const kSuffix = "/ScriptExtender/Config.json";
    const std::size_t prefix = std::strlen(kPrefix);
    const std::size_t suffix = std::strlen(kSuffix);
    const std::size_t len = std::strlen(entry);
    if (len <= prefix + suffix) return {};
    if (std::strncmp(entry, kPrefix, prefix) != 0) return {};
    if (std::strcmp(entry + len - suffix, kSuffix) != 0) return {};

    const std::string name(entry + prefix, len - prefix - suffix);
    // One level only: "Mods/A/B/ScriptExtender/Config.json" is not a module.
    if (name.find('/') != std::string::npos) return {};
    return name;
}

// One attribute of the meta's ModuleInfo node.
std::string meta_attribute(std::string const& meta, char const* id) {
    const std::size_t info = meta.find("id=\"ModuleInfo\"");
    if (info == std::string::npos) return {};

    const std::string needle = std::string("id=\"") + id + "\"";
    const std::size_t at = meta.find(needle, info);
    if (at == std::string::npos) return {};
    const std::size_t value = meta.find("value=\"", at);
    if (value == std::string::npos) return {};
    const std::size_t from = value + 7;
    const std::size_t to = meta.find('"', from);
    if (to == std::string::npos) return {};
    return meta.substr(from, to - from);
}

// Every archive in the profile's Mods directory that carries a script
// extender module, scanned once.
std::vector<std::string>& mod_archives() {
    static std::vector<std::string> archives;
    return archives;
}

// Which archive holds a given file, for the paths a mod is likely to ask
// for by name. Only the Mods/ tree is indexed: that is where a mod keeps
// its blueprints and configuration, it is a few thousand entries across
// every installed mod, and indexing Public/ as well would be a hundred
// times the memory for files nothing reads this way.
std::map<std::string, std::string>& mod_files() {
    static std::map<std::string, std::string> files;
    return files;
}

std::vector<PakModule> const& pak_modules();

// Builds the archive index if it has not been built yet.
void scan_mod_archives() { (void)pak_modules(); }

std::vector<PakModule> const& pak_modules() {
    static std::vector<PakModule> modules;
    static bool scanned = false;
    if (scanned) return modules;
    scanned = true;

    std::vector<std::pair<std::string, std::string>> paks;  // root, name
    for (std::string const& root : mods_roots()) {
        DIR* dir = opendir(root.c_str());
        if (dir == nullptr) continue;
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.size() < 5
                || name.compare(name.size() - 4, 4, ".pak") != 0) {
                continue;
            }
            paks.emplace_back(root, name);
        }
        closedir(dir);
    }
    std::sort(paks.begin(), paks.end());

    for (auto const& entry : paks) {
        std::string const& root = entry.first;
        std::string const& pak = entry.second;
        const std::string path = root + "/" + pak;
        mod_archives().push_back(path);
        pak_list(path.c_str(), [&](char const* name) {
            if (std::strncmp(name, "Mods/", 5) != 0) return;
            mod_files().emplace(name, path);
        });
        std::vector<std::string> names;
        std::map<std::string, std::string> metas;
        pak_read(
            path.c_str(),
            [](char const* entry) {
                const std::size_t len = std::strlen(entry);
                const bool meta = len >= 8
                                  && std::strcmp(entry + len - 8, "meta.lsx")
                                         == 0;
                return meta || !module_of_config(entry).empty();
            },
            [&](char const* entry, char const* data, std::size_t size) {
                const std::string module = module_of_config(entry);
                if (!module.empty()) {
                    names.push_back(module);
                    return;
                }
                metas.emplace(entry, std::string(data, size));
            });

        for (std::string const& name : names) {
            PakModule module;
            module.Pak = path;
            module.Name = name;
            auto it = metas.find("Mods/" + name + "/meta.lsx");
            if (it != metas.end()) {
                std::string const& meta = it->second;
                module.Uuid = meta_attribute(meta, "UUID");
                module.ModName = meta_attribute(meta, "Name");
                module.Author = meta_attribute(meta, "Author");
                module.Description = meta_attribute(meta, "Description");
                module.Version = meta_attribute(meta, "Version64");
                if (module.Version.empty()) {
                    module.Version = meta_attribute(meta, "Version");
                }
            }
            modules.push_back(std::move(module));
        }
    }

    logf("mods: %zu script extender modules in %zu archives",
         modules.size(), paks.size());
    return modules;
}

}  // namespace

// The load order the player wrote, from modsettings.lsx.
//
// Not a substitute for the engine's list, which is what upstream uses and
// what bg3le uses when it has one. It is the fallback for the case where
// the engine has loaded no add-on at all: the mods are installed, the
// player has enabled them, and their scripts would otherwise never run.
// Parsed once per version of the file: mods ask through IsModLoaded, and
// Mod Configuration Menu asks a few hundred times as the menu comes up.
extern "C" int bg3le_ext_mod_settings_order(lua_State* L) {
    const std::string root = profile_root();
    if (root.empty()) return 0;

    const std::string path =
        root + "/PlayerProfiles/Public/modsettings.lsx";
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return 0;

    static std::mutex lock;
    static std::vector<std::string> uuids;
    static struct timespec seenTime{};
    static off_t seenSize = -1;
    std::lock_guard<std::mutex> held(lock);

    if (st.st_size != seenSize || st.st_mtim.tv_sec != seenTime.tv_sec
        || st.st_mtim.tv_nsec != seenTime.tv_nsec) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (f == nullptr) return 0;
        std::string text;
        char block[65536];
        std::size_t got = 0;
        while ((got = std::fread(block, 1, sizeof(block), f)) > 0) {
            text.append(block, got);
        }
        std::fclose(f);

        uuids.clear();
        std::size_t at = 0;
        for (;;) {
            // Each entry is a ModuleShortDesc; its UUID is the only field the
            // caller needs, and the attribute name is unambiguous within one.
            const std::size_t entry = text.find("<node id=\"ModuleShortDesc\"", at);
            if (entry == std::string::npos) break;
            const std::size_t end = text.find("</node>", entry);
            const std::size_t uuid = text.find("id=\"UUID\"", entry);
            if (uuid == std::string::npos || (end != std::string::npos && uuid > end)) {
                at = entry + 1;
                continue;
            }
            const std::size_t value = text.find("value=\"", uuid);
            if (value == std::string::npos) break;
            const std::size_t from = value + 7;
            const std::size_t to = text.find('"', from);
            if (to == std::string::npos) break;
            uuids.emplace_back(text.data() + from, to - from);
            at = end == std::string::npos ? to : end;
        }
        seenSize = st.st_size;
        seenTime = st.st_mtim;
    }

    lua_createtable(L, (int)uuids.size(), 0);
    int index = 1;
    for (std::string const& uuid : uuids) {
        lua_pushlstring(L, uuid.data(), uuid.size());
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// Builds the archive index ahead of time, so the story thread does not pay
// for it during level load: reading 57 file lists is seconds of work, and
// mod loading happens at the worst possible moment for it.
extern "C" void bg3le_pak_modules_prewarm() { (void)pak_modules(); }

// Ext._Internal.PakModules() -> { {Pak=, Name=, Uuid=}, ... }
extern "C" int bg3le_ext_pak_modules(lua_State* L) {
    auto const& modules = pak_modules();
    lua_createtable(L, (int)modules.size(), 0);
    int index = 1;
    for (PakModule const& module : modules) {
        lua_createtable(L, 0, 3);
        lua_pushstring(L, module.Pak.c_str());
        lua_setfield(L, -2, "Pak");
        lua_pushstring(L, module.Name.c_str());
        lua_setfield(L, -2, "Name");
        lua_pushstring(L, module.Uuid.c_str());
        lua_setfield(L, -2, "Uuid");
        lua_pushstring(L, module.ModName.empty() ? module.Name.c_str()
                                                 : module.ModName.c_str());
        lua_setfield(L, -2, "ModName");
        lua_pushstring(L, module.Author.c_str());
        lua_setfield(L, -2, "Author");
        lua_pushstring(L, module.Description.c_str());
        lua_setfield(L, -2, "Description");
        lua_pushstring(L, module.Version.c_str());
        lua_setfield(L, -2, "Version");
        lua_rawseti(L, -2, index++);
    }
    return 1;
}

// Ext._Internal.PakRead(pak, entry) -> string or nil. The archive is named
// by file name, not path, so a mod cannot read outside the Mods directory.
extern "C" int bg3le_ext_pak_read(lua_State* L) {
    char const* pak = luaL_checkstring(L, 1);
    char const* entry = luaL_checkstring(L, 2);

    // Only an archive the scan itself reported, so a mod cannot name a
    // path of its own and read anything on disk.
    bool known = false;
    for (PakModule const& module : pak_modules()) {
        if (module.Pak == pak) {
            known = true;
            break;
        }
    }
    if (!known) return 0;

    std::string contents;
    bool found = false;
    pak_read(
        pak,
        [&](char const* name) { return std::strcmp(name, entry) == 0; },
        [&](char const*, char const* data, std::size_t size) {
            contents.assign(data, size);
            found = true;
        });
    if (!found) return 0;

    lua_pushlstring(L, contents.data(), contents.size());
    return 1;
}

// A file under Mods/ in an installed mod's archive, which the engine's
// virtual file system would have mounted.
bool mod_file_read(char const* relative, std::string* out) {
    scan_mod_archives();
    auto const in = mod_files().find(relative);
    if (in == mod_files().end()) return false;

    bool found = false;
    pak_read(
        in->second.c_str(),
        [&](char const* name) { return std::strcmp(name, relative) == 0; },
        [&](char const*, char const* data, std::size_t size) {
            out->assign(data, size);
            found = true;
        });
    return found;
}

}  // namespace bg3le
