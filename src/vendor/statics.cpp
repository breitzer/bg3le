// Recovers the engine's own static pointers, so a manager is read rather
// than searched for.
//
// Everything bg3le finds -- the stats manager, the module list, the
// template and prototype managers -- it finds by scanning the heap for
// something that looks right. That works, but only once the data exists,
// and it costs a scan. bg3se has neither problem: it pattern-matches the
// executable at startup to recover the addresses of the statics the engine
// keeps those pointers in, and from then on it just dereferences them.
//
// The Linux build gives no way to pattern-match for those statics -- of
// Larian's own code nothing carries a symbol, and its statics are
// anonymous -- but it does allow the inverse. Once a structure has been
// found by content, the static that points at it can be found by searching
// the executable's own writable data for its address. That static's offset
// within the image is a property of the build, not of the run, so it is
// recorded and reused: every later run reads the pointer directly, before
// anything has asked for it, with no scanning at all.
//
// The cache is keyed by the executable's build id, so a game patch
// invalidates it rather than silently resolving to the wrong offset.

#include <stdafx.h>

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../log.h"
#include "../mem.h"

namespace bg3le {

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace {

std::string const& exe_path() {
    static std::string path;
    if (!path.empty()) return path;

    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        path = buffer;
    }
    return path;
}

// The executable's own writable mappings -- .data and .bss -- which is
// where a static pointer lives. Nothing else is searched: a manager
// pointer is not in the heap, and searching everything would defeat the
// point of doing this at all.
std::vector<std::pair<unsigned long long, unsigned long long>>
writable_image_regions() {
    std::vector<std::pair<unsigned long long, unsigned long long>> out;
    if (exe_path().empty()) return out;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe_path().c_str()) == nullptr) continue;

        unsigned long long from = 0;
        unsigned long long to = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &from, &to, perms) != 3) {
            continue;
        }
        if (std::strchr(perms, 'w') == nullptr) continue;
        out.emplace_back(from, to);
    }
    std::fclose(maps);
    return out;
}

// The lowest address the executable is mapped at, so an offset recorded in
// one run means the same thing in the next.
std::uintptr_t image_base() {
    static std::uintptr_t base = 0;
    if (base != 0) return base;
    if (exe_path().empty()) return 0;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return 0;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe_path().c_str()) == nullptr) continue;
        unsigned long long from = 0;
        if (std::sscanf(line, "%llx", &from) != 1) continue;
        if (base == 0 || from < base) base = (std::uintptr_t)from;
    }
    std::fclose(maps);
    return base;
}

// Identifies the build, so a game patch invalidates the cache instead of
// resolving to an offset that has moved.
std::string build_key() {
    if (exe_path().empty()) return {};
    struct stat st{};
    if (stat(exe_path().c_str(), &st) != 0) return {};

    char key[64];
    std::snprintf(key, sizeof(key), "%llx-%llx",
                  (unsigned long long)st.st_size,
                  (unsigned long long)st.st_mtime);
    return key;
}

// What a search found, expressed so it can be recovered next run: the
// static's offset within the image, and how far past that pointer the
// thing we actually wanted sits.
//
// The delta matters because the searches land inside a manager, not at its
// base -- the stats search anchors on a run of treasure rarities partway
// through RPGStats -- while the static holds the base. Recording the gap
// means neither the struct's layout nor the manager's size has to be
// known.
// A path from a static to something bg3le found.
//
// One hop covers a manager a static points at directly. Two cover the
// common case: a static names an owning object, and the manager hangs off
// a member of it. RPGStats is reached that way -- eight statics were found
// within a megabyte below it and not one of them held on the next run,
// because none of them pointed at it; they pointed at neighbours in the
// same arena.
struct Entry {
    std::uint64_t Offset{0};          // of the static, within the image
    std::vector<std::uint64_t> Hops;  // member offsets, each dereferenced
    std::uint64_t Delta{0};           // from the last pointer to the target
};

// Resolution is: read the static, then for each hop add it and read again,
// then add the delta. An empty hop list is the simple case -- a static
// holding the manager directly.

struct Cache {
    bool Loaded{false};
    bool Dirty{false};
    std::string Path;
    // Several candidates per key, nearest first. Which static the engine
    // actually keeps the manager in is not decidable from one run -- any
    // static pointing into the same arena looks the same -- so all of them
    // are kept and the caller validates until one holds.
    std::map<std::string, std::vector<Entry>> Offsets;
};

Cache& cache() {
    static Cache c;
    return c;
}

std::string cache_path() {
    char const* home = std::getenv("HOME");
    const std::string build = build_key();
    if (home == nullptr || build.empty()) return {};

    const std::string dir = std::string(home) + "/.local/share/bg3le";
    mkdir(dir.c_str(), 0755);
    return dir + "/statics-" + build + ".txt";
}

void load_cache() {
    Cache& c = cache();
    if (c.Loaded) return;
    c.Loaded = true;
    c.Path = cache_path();
    if (c.Path.empty()) return;

    std::FILE* f = std::fopen(c.Path.c_str(), "r");
    if (f == nullptr) return;

    char line[4096];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char key[128];
        int consumed = 0;
        if (std::sscanf(line, "%127s%n", key, &consumed) != 1) continue;

        std::vector<Entry> entries;
        char const* at = line + consumed;
        for (;;) {
            // offset:hop,hop,...:delta, with the hops possibly empty.
            char field[256];
            int used = 0;
            if (std::sscanf(at, " %255s%n", field, &used) != 1) break;

            char* first = std::strchr(field, ':');
            char* last = std::strrchr(field, ':');
            if (first == nullptr || last == first) break;
            *first = '\0';
            *last = '\0';

            Entry entry;
            entry.Offset = std::strtoull(field, nullptr, 16);
            entry.Delta = std::strtoull(last + 1, nullptr, 16);
            char* rest = nullptr;
            for (char* hop = strtok_r(first + 1, ",", &rest); hop != nullptr;
                 hop = strtok_r(nullptr, ",", &rest)) {
                entry.Hops.push_back(std::strtoull(hop, nullptr, 16));
            }
            entries.push_back(std::move(entry));
            at += used;
        }
        if (!entries.empty()) c.Offsets[key] = std::move(entries);
    }
    std::fclose(f);
    logf("statics: %zu cached offsets from %s", c.Offsets.size(),
         c.Path.c_str());
}

void save_cache() {
    Cache& c = cache();
    if (!c.Dirty || c.Path.empty()) return;

    std::FILE* f = std::fopen(c.Path.c_str(), "w");
    if (f == nullptr) return;
    for (auto const& entry : c.Offsets) {
        std::fprintf(f, "%s", entry.first.c_str());
        for (Entry const& candidate : entry.second) {
            std::fprintf(f, " %llx:", (unsigned long long)candidate.Offset);
            for (std::size_t i = 0; i < candidate.Hops.size(); ++i) {
                std::fprintf(f, "%s%llx", i == 0 ? "" : ",",
                             (unsigned long long)candidate.Hops[i]);
            }
            std::fprintf(f, ":%llx", (unsigned long long)candidate.Delta);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    c.Dirty = false;
}

// One address holding a pointer that lands at or shortly below the
// `Which`th address asked about, `Hop` bytes below it.
struct Hit {
    std::uint64_t Addr{0};
    std::size_t Which{0};
    std::uint64_t Hop{0};
};

// Everything in the process pointing at or shortly below any of `wanted`,
// found in a single pass.
//
// One pass per level, not per candidate, is what makes a multi-hop search
// affordable: a level can hold thousands of addresses, and scanning
// gigabytes for each of them separately would take hours.
std::vector<Hit> holders_of_many(std::vector<std::uint64_t> const& wanted,
                                 std::uint64_t window, std::size_t limit) {
    std::vector<Hit> out;
    if (wanted.empty()) return out;

    // A hashed page filter, so the common case -- a word pointing nowhere
    // near anything wanted -- costs one bit test. The binary search below
    // is an order of magnitude too slow to run on a billion words.
    constexpr std::uint64_t kFilterBits = 1ull << 22;
    std::vector<std::uint64_t> filter(kFilterBits / 64, 0);
    auto const mark = [&](std::uint64_t page) {
        const std::uint64_t h = (page * 0x9e3779b97f4a7c15ull) >> 42;
        filter[h / 64] |= 1ull << (h % 64);
    };
    auto const marked = [&](std::uint64_t page) {
        const std::uint64_t h = (page * 0x9e3779b97f4a7c15ull) >> 42;
        return (filter[h / 64] & (1ull << (h % 64))) != 0;
    };

    for (std::uint64_t address : wanted) {
        for (std::uint64_t back = 0; back <= window; back += 4096) {
            mark((address - back) >> 12);
        }
        mark((address - window) >> 12);
    }

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;

    constexpr std::size_t kChunk = 1u << 20;
    static std::vector<unsigned char> block;
    block.resize(kChunk + 8);

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr
           && out.size() < limit) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long at = from; at < to && out.size() < limit;
             at += kChunk) {
            std::size_t want = (std::size_t)(to - at);
            if (want > kChunk + 8) want = kChunk + 8;
            const std::size_t got =
                safe_read_some((void const*)at, block.data(), want);
            if (got < 8) continue;
            scan_yield();

            for (std::size_t off = 0; off + 8 <= got && out.size() < limit;
                 off += 8) {
                std::uint64_t word = 0;
                std::memcpy(&word, block.data() + off, sizeof(word));
                if (word < 0x10000 || word > 0x800000000000ull) continue;
                if (!marked(word >> 12)) continue;

                auto it = std::lower_bound(wanted.begin(), wanted.end(),
                                           word);
                for (; it != wanted.end() && *it - word <= window; ++it) {
                    out.push_back(Hit{at + off,
                                      (std::size_t)(it - wanted.begin()),
                                      *it - word});
                    if (out.size() >= limit) break;
                }
            }
        }
    }
    std::fclose(maps);
    return out;
}

}  // namespace

// Reads a pointer whose static bg3le has already recorded, or null.
//
// This is the fast path, and the whole point: no scan, no waiting, usable
// the moment the engine has filled the static in.
extern "C" void* bg3le_static_get(char const* key, std::size_t which) {
    load_cache();
    if (key == nullptr) return nullptr;

    auto it = cache().Offsets.find(key);
    if (it == cache().Offsets.end() || which >= it->second.size()) {
        return nullptr;
    }

    const std::uintptr_t base = image_base();
    if (base == 0) return nullptr;

    Entry const& entry = it->second[which];
    void* value = nullptr;
    if (!safe_read((void const*)(base + entry.Offset), &value,
                   sizeof(value))) {
        return nullptr;
    }
    if (value == nullptr) return nullptr;   // the engine has not filled it in

    for (std::uint64_t hop : entry.Hops) {
        void* next = nullptr;
        if (!safe_read((void const*)((char*)value + hop), &next,
                       sizeof(next))) {
            return nullptr;
        }
        if (next == nullptr) return nullptr;
        value = next;
    }
    return (void*)((char*)value + entry.Delta);
}

// How many candidates were recorded for a key.
extern "C" std::size_t bg3le_static_count(char const* key) {
    load_cache();
    if (key == nullptr) return 0;
    auto it = cache().Offsets.find(key);
    return it == cache().Offsets.end() ? 0 : it->second.size();
}

// Narrows a key to the candidate that worked, so later runs try it first
// and the rest are forgotten.
extern "C" void bg3le_static_confirm(char const* key, std::size_t which) {
    load_cache();
    if (key == nullptr) return;

    auto it = cache().Offsets.find(key);
    if (it == cache().Offsets.end() || which >= it->second.size()) return;
    if (it->second.size() == 1) return;

    Entry const kept = it->second[which];
    it->second.assign(1, kept);
    cache().Dirty = true;
    save_cache();
    logf("statics: %s confirmed at image+%#llx plus %#llx", key,
         (unsigned long long)kept.Offset, (unsigned long long)kept.Delta);
}

// Records where the engine keeps a pointer to an object bg3le has already
// found, so later runs can skip the search.
//
// Every candidate is checked by reading it back: the offset only counts if
// the word at it really holds this address. Several statics can hold the
// same pointer; the first is taken, and being wrong about which is
// harmless since they are interchangeable by definition.
namespace {

// The executable's writable data, copied once.
//
// It is a few megabytes, and every candidate check reads it -- doing that
// with a syscall per eight bytes is how the earlier searches came to stall
// the game, so it is read in bulk and matched in memory.
struct ImageData {
    std::vector<std::pair<unsigned long long, std::vector<unsigned char>>>
        Regions;
};

ImageData const& image_data() {
    static ImageData data;
    if (!data.Regions.empty()) return data;

    for (auto const& region : writable_image_regions()) {
        const std::size_t size = (std::size_t)(region.second - region.first);
        std::vector<unsigned char> bytes(size);
        const std::size_t got =
            safe_read_some((void const*)region.first, bytes.data(), size);
        if (got < 8) continue;
        bytes.resize(got);
        data.Regions.emplace_back(region.first, std::move(bytes));
    }
    return data;
}

// Every pointer-looking value in the image's writable data, sorted by
// value, with the offset it lives at.
//
// Sorted because the two-hop search asks the same question thousands of
// times -- "is there a static pointing just below this address?" -- and
// walking megabytes per question is what made the first attempt
// unaffordable, so it only had budget for 32 candidates and missed the
// real one. A binary search makes thousands affordable.
std::vector<std::pair<std::uint64_t, std::uint64_t>> const&
sorted_statics(std::uintptr_t base) {
    static std::vector<std::pair<std::uint64_t, std::uint64_t>> values;
    if (!values.empty()) return values;

    for (auto const& region : image_data().Regions) {
        unsigned char const* bytes = region.second.data();
        const std::size_t size = region.second.size();
        for (std::size_t off = 0; off + 8 <= size; off += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, bytes + off, sizeof(word));
            if (word < 0x10000 || word > 0x800000000000ull) continue;
            values.emplace_back(word, (region.first + off) - base);
        }
    }
    std::sort(values.begin(), values.end());
    logf("statics: %zu pointer-shaped words in the image's writable data",
         values.size());
    return values;
}

// Statics whose value points at or shortly below `wanted`.
void statics_pointing_near(std::uint64_t wanted, std::uint64_t window,
                           std::uintptr_t base,
                           std::vector<std::pair<std::uint64_t,
                                                 std::uint64_t>>* out) {
    auto const& values = sorted_statics(base);
    auto upper = std::upper_bound(
        values.begin(), values.end(),
        std::make_pair(wanted, (std::uint64_t)~0ull));

    while (upper != values.begin()) {
        --upper;
        if (wanted - upper->first >= window) break;
        // offset within the image, and how far past the value the target
        // sits.
        out->emplace_back(upper->second, wanted - upper->first);
    }
}

// Everything anywhere in the process that holds a pointer at or shortly
// below `wanted`. These are the candidate owning objects for a two-hop
// path: something points at them from a static.
std::vector<std::uint64_t> holders_of(std::uint64_t wanted,
                                      std::uint64_t window,
                                      std::size_t limit) {
    std::vector<std::uint64_t> out;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;

    constexpr std::size_t kChunk = 1u << 20;
    static std::vector<unsigned char> block;
    block.resize(kChunk + 8);

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr
           && out.size() < limit) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long at = from; at < to && out.size() < limit;
             at += kChunk) {
            std::size_t want = (std::size_t)(to - at);
            if (want > kChunk + 8) want = kChunk + 8;
            const std::size_t got =
                safe_read_some((void const*)at, block.data(), want);
            if (got < 8) continue;
            scan_yield();

            for (std::size_t off = 0; off + 8 <= got && out.size() < limit;
                 off += 8) {
                std::uint64_t word = 0;
                std::memcpy(&word, block.data() + off, sizeof(word));
                if (word == 0 || word > wanted) continue;
                if (wanted - word >= window) continue;
                out.push_back(at + off);
            }
        }
    }
    std::fclose(maps);
    return out;
}

}  // namespace

// Records a static that holds exactly `pointer`, with `delta` from it to
// what the caller actually wants.
//
// This is the version to prefer whenever the containing object's base is
// known. The windowed search below has to guess which of dozens of nearby
// statics is the right one -- for RPGStats it found 48 and every one was a
// neighbour in the same arena, identical-looking until the next run moved
// them. An exact match has no such ambiguity.
extern "C" bool bg3le_static_record_exact(char const* key,
                                          void const* pointer,
                                          std::uint64_t delta) {
    load_cache();
    if (key == nullptr || pointer == nullptr) return false;

    const std::uintptr_t base = image_base();
    if (base == 0) return false;

    const auto wanted = (std::uint64_t)(std::uintptr_t)pointer;
    std::vector<Entry> candidates;
    for (auto const& entry : sorted_statics(base)) {
        if (entry.first != wanted) continue;
        candidates.push_back(Entry{entry.second, {}, delta});
    }

    if (candidates.empty()) {
        logf("statics: no static holds %s's base exactly", key);
        return false;
    }

    cache().Offsets[key] = candidates;
    cache().Dirty = true;
    save_cache();
    logf("statics: %s is image+%#llx plus %#llx, matched exactly (%zu "
         "statics hold it)", key,
         (unsigned long long)candidates[0].Offset,
         (unsigned long long)delta, candidates.size());
    return true;
}

// Finds a path of any length from a static in the image to `target`, by
// working backwards: what points at the target, what points at that, and
// so on until something in the image's own writable data does.
//
// This is the general case of the two searches above. A manager the engine
// does not keep a static pointer to is still reachable -- the engine has
// to get at it somehow -- just through an owning object, or a chain of
// them. Each level costs one pass over the process, so the depth is
// bounded; three hops is already further than the engine's own accessors
// tend to go.
extern "C" bool bg3le_static_record_path(char const* key,
                                         void const* target,
                                         std::uint64_t first_window) {
    load_cache();
    if (key == nullptr || target == nullptr) return false;

    const std::uintptr_t base = image_base();
    if (base == 0) return false;

    // Members sit close to the head of their object; a whole level of
    // slack here is thousands of false paths.
    constexpr std::uint64_t kMemberWindow = 1u << 13;
    constexpr std::size_t kMaxDepth = 4;
    constexpr std::size_t kFrontierLimit = 4096;
    constexpr std::size_t kPerDepth = 64;
    constexpr std::size_t kPathLimit = 256;

    struct Node {
        std::uint64_t Addr{0};
        std::vector<std::uint64_t> Hops;
        std::uint64_t Delta{0};
    };

    const auto wanted = (std::uint64_t)(std::uintptr_t)target;

    // The first level is wider than the rest: the searches land inside a
    // manager rather than at its base, so whatever points at it points
    // further back than a member offset.
    std::vector<Node> frontier;
    for (auto const& hit : holders_of_many({wanted}, first_window,
                                           kFrontierLimit)) {
        void* held = nullptr;
        if (!safe_read((void const*)hit.Addr, &held, sizeof(held))) continue;
        frontier.push_back(
            Node{hit.Addr, {}, wanted - (std::uint64_t)(std::uintptr_t)held});
    }
    logf("statics: %s has %zu holders to walk back from", key,
         frontier.size());

    std::unordered_set<std::uint64_t> seen;
    for (auto const& node : frontier) seen.insert(node.Addr);

    std::vector<Entry> candidates;
    for (std::size_t depth = 0; depth < kMaxDepth && !frontier.empty();
         ++depth) {
        // Anything in the image's own writable data is a static, and a
        // static is the end of the search.
        std::vector<Entry> atDepth;
        for (auto const& node : frontier) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> hits;
            statics_pointing_near(node.Addr, kMemberWindow, base, &hits);
            for (auto const& hit : hits) {
                Entry entry;
                entry.Offset = hit.first;
                entry.Hops.push_back(hit.second);
                entry.Hops.insert(entry.Hops.end(), node.Hops.begin(),
                                  node.Hops.end());
                entry.Delta = node.Delta;
                atDepth.push_back(std::move(entry));
            }
        }

        // The tightest offsets first, and only so many per depth. A
        // shallow path is not necessarily the real one -- eight statics
        // sit within a page of anything on the heap -- so the search does
        // not stop at the first depth that produces one; it keeps a
        // shortlist from every depth and lets the next run's validation
        // decide.
        const auto reach = [](Entry const& e) {
            std::uint64_t sum = e.Delta;
            for (std::uint64_t hop : e.Hops) sum += hop;
            return sum;
        };
        std::sort(atDepth.begin(), atDepth.end(),
                  [&](Entry const& a, Entry const& b) {
                      return reach(a) < reach(b);
                  });
        if (atDepth.size() > kPerDepth) atDepth.resize(kPerDepth);
        logf("statics: %s has %zu paths at depth %zu", key, atDepth.size(),
             depth + 1);
        candidates.insert(candidates.end(), atDepth.begin(), atDepth.end());

        if (depth + 1 == kMaxDepth) break;

        std::vector<std::uint64_t> addresses;
        addresses.reserve(frontier.size());
        for (auto const& node : frontier) addresses.push_back(node.Addr);
        std::sort(addresses.begin(), addresses.end());

        std::vector<Node> next;
        for (auto const& hit : holders_of_many(addresses, kMemberWindow,
                                               kFrontierLimit)) {
            if (!seen.insert(hit.Addr).second) continue;
            // The frontier was sorted, so find the node the hit names.
            Node const* owner = nullptr;
            for (auto const& node : frontier) {
                if (node.Addr == addresses[hit.Which]) {
                    owner = &node;
                    break;
                }
            }
            if (owner == nullptr) continue;

            Node node;
            node.Addr = hit.Addr;
            node.Hops.push_back(hit.Hop);
            node.Hops.insert(node.Hops.end(), owner->Hops.begin(),
                             owner->Hops.end());
            node.Delta = owner->Delta;
            next.push_back(std::move(node));
        }
        logf("statics: %s level %zu widened to %zu addresses", key,
             depth + 2, next.size());
        frontier = std::move(next);
    }

    if (candidates.empty()) {
        logf("statics: no chain from a static reaches %s", key);
        return false;
    }

    if (candidates.size() > kPathLimit) candidates.resize(kPathLimit);
    cache().Offsets[key] = candidates;
    cache().Dirty = true;
    save_cache();
    logf("statics: %s has %zu candidate paths, shortest %zu hops from "
         "image+%#llx", key, candidates.size(), candidates[0].Hops.size(),
         (unsigned long long)candidates[0].Offset);
    return true;
}

extern "C" bool bg3le_static_record(char const* key, void const* object) {
    load_cache();
    if (key == nullptr || object == nullptr) return false;
    if (cache().Offsets.count(key) != 0) return true;

    const std::uintptr_t base = image_base();
    if (base == 0) return false;

    // The static holds a pointer at or before what was found. A manager
    // is not megabytes wide, so a window keeps an unrelated pointer that
    // happens to sit below it from being mistaken for one -- and the
    // closest candidate is taken, which is the innermost enclosing object.
    // Wide enough for a manager the size of RPGStats, whose rarity run
    // sits 55KB into it.
    constexpr std::uint64_t kWindow = 1u << 20;
    constexpr std::size_t kMaxCandidates = 24;

    const auto wanted = (std::uint64_t)(std::uintptr_t)object;
    std::vector<Entry> candidates;

    std::vector<std::pair<std::uint64_t, std::uint64_t>> direct;
    statics_pointing_near(wanted, kWindow, base, &direct);
    for (auto const& hit : direct) {
        candidates.push_back(Entry{hit.first, {}, hit.second});
    }

    // Two-hop paths as well, always -- not only when no static points at
    // the object directly. The single-hop candidates are mostly
    // neighbours: RPGStats sits in an arena with other allocations, and a
    // static pointing at one of those looks identical until the next run
    // moves them relative to each other. Two-hop paths are the ones that
    // survive, so they are recorded alongside and the caller validates.
    for (std::uint64_t holder : holders_of(wanted, kWindow, 20000)) {
        std::uint64_t owner = 0;
        if (!safe_read((void const*)(std::uintptr_t)holder, &owner,
                       sizeof(owner))) {
            continue;
        }

        // A member of an owning object, so the static points close by --
        // a narrow window here keeps the coincidences down.
        constexpr std::uint64_t kMemberWindow = 1u << 14;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> reaching;
        statics_pointing_near(holder, kMemberWindow, base, &reaching);
        for (auto const& hop : reaching) {
            candidates.push_back(
                Entry{hop.first, {hop.second}, wanted - owner});
            if (candidates.size() >= kMaxCandidates * 4) break;
        }
        if (candidates.size() >= kMaxCandidates * 4) break;
    }

    if (!candidates.empty()) {
        // Nearest first: the innermost enclosing object is the likeliest,
        // and the caller stops at the first that validates.
        // Two-hop paths first, then by proximity: a path through an
        // owning object survives a restart, a neighbour does not.
        std::stable_sort(candidates.begin(), candidates.end(),
                         [](Entry const& a, Entry const& b) {
                             if (a.Hops.empty() != b.Hops.empty()) {
                                 return !a.Hops.empty();
                             }
                             return a.Delta < b.Delta;
                         });
        if (candidates.size() > kMaxCandidates * 2) {
            candidates.resize(kMaxCandidates * 2);
        }

        cache().Offsets[key] = candidates;
        cache().Dirty = true;
        save_cache();
        logf("statics: %s has %zu candidate statics, nearest at image+%#llx "
             "plus %#llx", key, candidates.size(),
             (unsigned long long)candidates[0].Offset,
             (unsigned long long)candidates[0].Delta);
        return true;
    }

    logf("statics: nothing in the image points at %s; it will keep being "
         "searched for", key);
    return false;
}

}  // namespace bg3le
