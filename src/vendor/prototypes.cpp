// The spell, status, interrupt and passive prototype managers, which is
// what Ext.Stats.GetCachedSpell, GetCachedStatus, GetCachedInterrupt and
// GetCachedPassive read.
//
// A prototype is the engine's parsed form of a stat: the stats object says
// what the .txt file said, the prototype is what the engine actually runs.
// Neither manager has a symbol.
//
// Both are found the way the templates were -- by validating a candidate
// against its own contents rather than by recognising a container's shape.
// Each manager holds HashMap<FixedString, Prototype*>, and a prototype
// carries the same name the map is keyed by, so for a genuine map every
// entry satisfies
//
//     *(FixedString*)(Values[i] + nameOffset) == Keys[i]
//
// Eight consecutive entries agreeing on that is not something another map
// does by accident, and it is a statement about the data rather than about
// the layout.

#include <stdafx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include "../game_state.h"
#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" void* bg3le_stats_find(char const* name);
extern "C" char const* bg3le_stats_type(void const* object);
extern "C" void* bg3le_static_get(char const* key, std::size_t which);
extern "C" std::size_t bg3le_static_count(char const* key);
extern "C" void bg3le_static_confirm(char const* key, std::size_t which);
extern "C" bool bg3le_static_record_path(char const* key, void const* target,
                                         std::uint64_t first_window);
extern "C" bool bg3le_stats_is_loading();
extern "C" void bg3le_prototypes_debug_check_passives(void const* mapAt, char const* when);
extern "C" void bg3le_prototypes_debug_check_spells(char const* when);
extern "C" void bg3le_stats_debug_watch(char const* when, bool prototypes);

namespace {

char const* client_game_state_or_unknown() {
    char const* now = client_game_state();
    return now != nullptr ? now : "unknown";
}

// A bg3se HashMap: HashKeys, NextIds, Keys, then Values.
constexpr std::size_t kKeysBuffer = 32;
constexpr std::size_t kKeysCapacity = 40;
constexpr std::size_t kKeysSize = 44;
constexpr std::size_t kValuesBuffer = 48;
constexpr std::size_t kMapSize = 64;

// Array<FixedString>, so four bytes per key -- not the eight a
// RuntimeStringHandle takes.
constexpr std::size_t kKeyStride = 4;
constexpr std::size_t kValueStride = 8;   // Prototype*

constexpr std::uint32_t kNullFixedString = 0xffffffffu;

// How many entries have to agree before a candidate is believed, and how
// many a real manager holds at the least.
constexpr std::uint32_t kAgreeing = 8;
// Low enough for the smaller managers: the interrupt map holds hundreds
// where the spell map holds thousands.
constexpr std::uint32_t kMinEntries = 50;
constexpr std::uint32_t kMaxEntries = 1u << 20;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

struct Kind {
    char const* Name;        // for the log
    std::size_t NameOffset;  // where the prototype repeats its own name
};

// SpellPrototype opens with StatsObjectIndex and SpellTypeId, then SpellId;
// StatusPrototype with StatsObjectIndex and StatusId, then StatusName. Both
// repeat their name at the same offset, which is why the self-check alone
// cannot tell the two managers apart -- see kind_of below.
constexpr Kind kKinds[] = {
    {"spell", 8},
    {"status", 8},
    // Interrupts and passives are stored in their map rather than behind a
    // pointer, so what varies is the stride between them; the name still
    // has to match the key, which is what derives it.
    {"interrupt", 0},
    // Passives are a LegacyRefMap of chained nodes; see refmap_consistent.
    {"passive", 4},
};
constexpr std::size_t kSpell = 0;
constexpr std::size_t kStatus = 1;
constexpr std::size_t kInterrupt = 2;
constexpr std::size_t kPassive = 3;

// Which kind a validated map holds, from the stats its names belong to: a
// spell prototype is named after a SpellData stat and a status after a
// StatusData one. Without this the two maps were labelled in the order
// they happened to be found, and they came out swapped -- GetCachedSpell
// was looking spells up in the status map.
//
// Unanimity is required. A split vote means the map is neither, and
// guessing from a majority is how you end up with one wrong entry that
// nobody notices.
constexpr char const* kStatTypes[] = {"SpellData", "StatusData",
                                      "InterruptData", "PassiveData"};

int kind_of_names(std::uint32_t const* keys, std::size_t count) {
    std::size_t votes[std::size(kKinds)] = {};
    std::size_t asked = 0;

    constexpr std::size_t kVotes = 3;
    for (std::size_t i = 0; i < count && asked < kVotes; ++i) {
        if (keys[i] == 0 || keys[i] == kNullFixedString) continue;

        char const* name = bg3le_fixed_string(keys[i], nullptr);
        if (name == nullptr) continue;

        void* object = bg3le_stats_find(name);
        if (object == nullptr) continue;
        char const* type = bg3le_stats_type(object);
        if (type == nullptr) continue;

        ++asked;
        for (std::size_t k = 0; k < std::size(kKinds); ++k) {
            if (std::strcmp(type, kStatTypes[k]) == 0) ++votes[k];
        }
    }

    if (asked < kVotes) return -1;
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        if (votes[k] == asked) return (int)k;
    }
    return -1;
}

int kind_of(void const* keys, std::uint32_t count) {
    std::uint32_t sample[16] = {};
    const std::size_t n = count < std::size(sample) ? count : std::size(sample);
    if (!safe_read(keys, sample, n * sizeof(sample[0]))) return -1;
    return kind_of_names(sample, n);
}

// Where a harvested prototype sat in the engine's map, to check it is still there.
struct Slot {
    std::uint32_t Key;
    std::uint32_t Index;  // kNoIndex in a LegacyRefMap
};
constexpr std::uint32_t kNoIndex = 0xffffffffu;

// What changes when the engine rebuilds a map: its buffers and its count.
struct Signature {
    std::uint64_t Keys{0};    // a LegacyRefMap's bucket table
    std::uint64_t Values{0};
    std::uint32_t Count{0};
    bool operator==(Signature const&) const = default;
};

struct Table {
    std::unordered_map<std::string, std::uint64_t> ByName;
    std::unordered_map<std::string, Slot> Slots;
    std::vector<std::string> Order;
    std::uint64_t MapAt{0};  // the manager's map, for adding a prototype
    std::size_t Stride{0};   // between inline prototypes
    Signature Sig;
};

struct Prototypes {
    // Written by the warming thread and read by the story thread. The
    // flag is released after the tables are filled and acquired before
    // they are read, so a reader that sees it set sees the contents too --
    // without that the handoff is a data race and the reader can see an
    // empty map.
    std::atomic<bool> Built{false};
    // Per kind, so one that resolved is usable while another is still missing.
    std::atomic<bool> Ready[std::size(kKinds)]{};
    Table Kinds[std::size(kKinds)];
};

Prototypes& state() {
    static Prototypes p;
    return p;
}

// Whether every one of the first few entries is a prototype that names
// itself the way the map is keyed.
bool self_consistent(void const* keys, void const* values,
                     std::uint32_t count, std::size_t nameOffset) {
    const std::uint32_t probe = count < kAgreeing ? count : kAgreeing;
    for (std::uint32_t i = 0; i < probe; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) return false;
        if (key == 0 || key == kNullFixedString) return false;

        void const* prototype = nullptr;
        if (!read_as((char const*)values + i * kValueStride, &prototype)
            || prototype == nullptr) {
            return false;
        }

        std::uint32_t own = 0;
        if (!read_as((char const*)prototype + nameOffset, &own)) return false;
        if (own != key) return false;
    }
    return probe == kAgreeing;
}

// The stride between inline prototypes, derived rather than assumed:
// sizeof(InterruptPrototype) as this build computes it is not the
// engine's, as every other struct here has shown. The right stride is the
// one where every sampled entry names itself the way its key does; a
// wrong one disagrees on the first.
std::size_t inline_stride(void const* keys, void const* values,
                          std::uint32_t count, std::size_t nameOffset) {
    const std::uint32_t probe = count < kAgreeing ? count : kAgreeing;
    if (probe < kAgreeing) return 0;

    for (std::size_t stride = 8; stride <= 512; stride += 8) {
        bool ok = true;
        for (std::uint32_t i = 0; i < probe && ok; ++i) {
            std::uint32_t key = 0;
            if (!read_as((char const*)keys + i * kKeyStride, &key)
                || key == 0 || key == kNullFixedString) {
                ok = false;
                break;
            }
            std::uint32_t own = 0;
            if (!read_as((char const*)values + i * stride + nameOffset,
                         &own)) {
                ok = false;
                break;
            }
            ok = own == key;
        }
        if (ok) return stride;
    }
    return 0;
}

std::size_t harvest_inline(void const* keys, void const* values,
                           std::uint32_t count, std::size_t nameOffset,
                           std::size_t stride, Table* into) {
    std::size_t added = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) break;
        if (key == 0 || key == kNullFixedString) continue;

        auto const* at = (char const*)values + i * stride;
        std::uint32_t own = 0;
        if (!read_as(at + nameOffset, &own) || own != key) continue;

        char const* name = bg3le_fixed_string(key, nullptr);
        if (name == nullptr || name[0] == '\0') continue;

        if (into->ByName.emplace(name, (std::uint64_t)(std::uintptr_t)at)
                .second) {
            into->Slots.emplace(name, Slot{key, i});
            ++added;
        }
    }
    return added;
}

std::size_t harvest(void const* keys, void const* values,
                    std::uint32_t count, std::size_t nameOffset,
                    Table* into) {
    std::size_t added = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) break;
        if (key == 0 || key == kNullFixedString) continue;

        void const* prototype = nullptr;
        if (!read_as((char const*)values + i * kValueStride, &prototype)
            || prototype == nullptr) {
            continue;
        }

        // Checked per entry, not just on the sample: a map that passed the
        // probe still must not contribute an entry that disagrees.
        std::uint32_t own = 0;
        if (!read_as((char const*)prototype + nameOffset, &own)) continue;
        if (own != key) continue;

        char const* name = bg3le_fixed_string(key, nullptr);
        if (name == nullptr || name[0] == '\0') continue;

        if (into->ByName.emplace(name, (std::uint64_t)(std::uintptr_t)
                                           prototype).second) {
            into->Slots.emplace(name, Slot{key, i});
            ++added;
        }
    }
    return added;
}

// Whether the first value names itself at nameOffset the way the first key
// does: the cheap test that lets an inline-valued map into the candidates,
// with the stride derived later by inline_stride.
bool inline_head(void const* keys, void const* values, std::size_t nameOffset) {
    std::uint32_t key = 0;
    std::uint32_t own = 0;
    return read_as(keys, &key) && key != 0 && key != kNullFixedString
           && read_as((char const*)values + nameOffset, &own) && own == key;
}

// A LegacyRefMap: ItemCount, HashSize, then a table of chains of
// MapNode { Next; Key; Value }, with the value 8-aligned after the key.
constexpr std::size_t kRefMapHashSize = 4;
constexpr std::size_t kRefMapTable = 8;
constexpr std::size_t kNodeKey = 8;
constexpr std::size_t kNodeValue = 16;

// Every node of the map, stopping at `limit`; false if the table is not
// readable. A chain longer than the item count is a cycle, not a map.
bool refmap_nodes(unsigned long long at, std::size_t limit,
                  std::vector<void const*>* out) {
    std::uint32_t count = 0;
    std::uint32_t hashSize = 0;
    std::uint64_t table = 0;
    auto const* head = (char const*)(std::uintptr_t)at;
    if (!read_as(head, &count) || !read_as(head + kRefMapHashSize, &hashSize)
        || !read_as(head + kRefMapTable, &table) || table == 0) {
        return false;
    }

    std::vector<std::uint64_t> buckets(256);
    for (std::uint32_t b = 0; b < hashSize && out->size() < limit;) {
        const std::uint32_t n = std::min<std::uint32_t>(256, hashSize - b);
        if (!safe_read((void const*)(std::uintptr_t)(table + b * 8ull),
                       buckets.data(), n * 8)) {
            return false;
        }
        for (std::uint32_t i = 0; i < n && out->size() < limit; ++i) {
            std::uint64_t node = buckets[i];
            for (std::uint32_t hops = 0; node != 0 && out->size() < limit;
                 ++hops) {
                if (hops > count) return false;
                out->push_back((void const*)(std::uintptr_t)node);
                if (!read_as((void const*)(std::uintptr_t)node, &node)) {
                    return false;
                }
            }
        }
        b += n;
    }
    return true;
}

// Whether the first few nodes hold prototypes that name themselves the way
// they are keyed, and the keys they carry.
bool refmap_consistent(unsigned long long at, std::size_t nameOffset,
                       std::vector<std::uint32_t>* keys) {
    std::vector<void const*> nodes;
    if (!refmap_nodes(at, kAgreeing, &nodes) || nodes.size() < kAgreeing) {
        return false;
    }
    for (void const* node : nodes) {
        std::uint32_t key = 0;
        std::uint32_t own = 0;
        if (!read_as((char const*)node + kNodeKey, &key) || key == 0
            || key == kNullFixedString
            || !read_as((char const*)node + kNodeValue + nameOffset, &own)
            || own != key) {
            return false;
        }
        keys->push_back(key);
    }
    return true;
}

std::size_t harvest_refmap(unsigned long long at, std::size_t nameOffset,
                           Table* into) {
    std::uint32_t count = 0;
    if (!read_as((void const*)(std::uintptr_t)at, &count)) return 0;

    std::vector<void const*> nodes;
    if (!refmap_nodes(at, count, &nodes)) return 0;

    std::size_t added = 0;
    for (void const* node : nodes) {
        std::uint32_t key = 0;
        std::uint32_t own = 0;
        if (!read_as((char const*)node + kNodeKey, &key)
            || !read_as((char const*)node + kNodeValue + nameOffset, &own)
            || own != key) {
            continue;
        }
        char const* name = bg3le_fixed_string(key, nullptr);
        if (name == nullptr || name[0] == '\0') continue;
        auto const value = (std::uint64_t)(std::uintptr_t)node + kNodeValue;
        if (into->ByName.emplace(name, value).second) {
            into->Slots.emplace(name, Slot{key, kNoIndex});
            ++added;
        }
    }
    return added;
}

// The executable's own mapping, where a vtable lives.
std::pair<std::uint64_t, std::uint64_t> image_span() {
    static std::pair<std::uint64_t, std::uint64_t> span{};
    if (span.second != 0) return span;

    char exe[512] = {};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return span;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return span;
    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe) == nullptr) continue;
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (std::sscanf(line, "%llx-%llx", &from, &to) != 2) continue;
        if (span.first == 0 || from < span.first) span.first = from;
        if (to > span.second) span.second = to;
    }
    std::fclose(maps);
    return span;
}

struct Candidate {
    void const* Keys{nullptr};
    void const* Values{nullptr};
    std::uint32_t Count{0};
    unsigned long long At{0};
    // Prototypes held in the map rather than behind a pointer.
    bool Inline{false};
};

// A validated prototype map is rare; this only exists so a pathological
// process cannot make the classification pass unbounded either.
constexpr std::size_t kMaxCandidates = 64;

// How many of them are actually classified, largest first.
constexpr std::size_t kClassify = 6;

// The key a path from a static to one kind's manager is recorded under.
std::string static_key(std::size_t k) {
    return std::string("prototypes.") + kKinds[k].Name;
}

// The map at `at`, validated as kind `k`'s manager and harvested. Shared
// between the scan and a recorded static so neither adopts what the other
// would reject.
bool read_signature(unsigned long long at, std::size_t k, Signature* out) {
    auto const* head = (char const*)(std::uintptr_t)at;
    if (at == 0) return false;
    if (k == kPassive) {
        return read_as(head, &out->Count) && read_as(head + kRefMapTable, &out->Keys);
    }
    return read_as(head + kKeysSize, &out->Count) && read_as(head + kKeysBuffer, &out->Keys)
           && read_as(head + kValuesBuffer, &out->Values);
}

bool harvest_map(unsigned long long at, std::size_t k, Table* into) {
    if (!read_signature(at, k, &into->Sig)) return false;
    if (k == kPassive) {
        std::vector<std::uint32_t> keys;
        if (!refmap_consistent(at, kKinds[k].NameOffset, &keys)
            || kind_of_names(keys.data(), keys.size()) != (int)k) {
            return false;
        }
        into->MapAt = at;
        return harvest_refmap(at, kKinds[k].NameOffset, into) > 0;
    }

    std::uint32_t capacity = 0;
    std::uint32_t count = 0;
    std::uint64_t keysWord = 0;
    std::uint64_t valuesWord = 0;
    auto const* head = (char const*)(std::uintptr_t)at;
    if (!read_as(head + kKeysCapacity, &capacity)
        || !read_as(head + kKeysSize, &count)
        || !read_as(head + kKeysBuffer, &keysWord)
        || !read_as(head + kValuesBuffer, &valuesWord)) {
        return false;
    }
    if (count < kMinEntries || count > kMaxEntries || count > capacity) {
        return false;
    }

    auto const* keys = (void const*)(std::uintptr_t)keysWord;
    auto const* values = (void const*)(std::uintptr_t)valuesWord;

    if (k == kInterrupt) {
        const std::size_t stride =
            inline_stride(keys, values, count, kKinds[k].NameOffset);
        if (stride == 0 || kind_of(keys, count) != (int)k) return false;
        if (harvest_inline(keys, values, count, kKinds[k].NameOffset, stride, into) == 0) {
            return false;
        }
        into->MapAt = at;
        into->Stride = stride;
        return true;
    }

    if (!self_consistent(keys, values, count, kKinds[k].NameOffset)) {
        return false;
    }
    // Which kind still has to be confirmed rather than taken from the key
    // the path was recorded under: the spell and status managers look
    // identical, and adopting one as the other would be silent.
    if (kind_of(keys, count) != (int)k) return false;
    if (harvest(keys, values, count, kKinds[k].NameOffset, into) == 0) return false;
    into->MapAt = at;
    return true;
}

std::mutex& tables_lock();

bool all_ready() {
    for (auto const& ready : state().Ready) {
        if (!ready.load(std::memory_order_acquire)) return false;
    }
    return true;
}

// Makes one kind's table live, under the lock lookups of the others hold.
void publish(std::size_t k, Table&& table) {
    table.Order.clear();
    table.Order.reserve(table.ByName.size());
    for (auto const& entry : table.ByName) table.Order.push_back(entry.first);
    const auto mapAt = (void const*)(std::uintptr_t)table.MapAt;
    {
        const std::lock_guard<std::mutex> held(tables_lock());
        state().Kinds[k] = std::move(table);
        state().Ready[k].store(true, std::memory_order_release);
    }
    if (k == kPassive) bg3le_prototypes_debug_check_passives(mapAt, "first found");
    if (all_ready()) {
        bg3le_prototypes_debug_check_spells("all prototypes found");
        bg3le_stats_debug_watch("all prototypes found", true);
    }
}

// What a recorded manager holds now, for the log when it is not usable yet.
std::string describe_map(unsigned long long at, std::size_t k) {
    auto const* head = (char const*)(std::uintptr_t)at;
    char text[160];
    if (k == kPassive) {
        std::uint32_t items = 0;
        std::uint32_t buckets = 0;
        if (!read_as(head, &items) || !read_as(head + 4, &buckets)) return "unreadable";
        std::snprintf(text, sizeof(text), "%u items in %u buckets", items, buckets);
        return text;
    }
    std::uint32_t capacity = 0;
    std::uint32_t count = 0;
    std::uint64_t keys = 0;
    if (!read_as(head + kKeysCapacity, &capacity) || !read_as(head + kKeysSize, &count)
        || !read_as(head + kKeysBuffer, &keys)) {
        return "unreadable";
    }
    const int kind = count > 0 ? kind_of((void const*)(std::uintptr_t)keys, count) : -1;
    std::snprintf(text, sizeof(text), "%u of %u entries, names %s", count, capacity,
                  kind < 0 ? "unclassified" : kKinds[kind].Name);
    return text;
}

// The managers a previous run recorded a path to, with no scanning.
//
// Each kind is published as soon as it resolves and the rest are tried again
// on later calls, at most twice a second. A manager the engine has not filled
// yet fails the check, and dropping the kinds that had resolved with it left
// Sync and GetCached* unavailable until the warming thread's scan.
bool build_from_statics() {
    if (all_ready()) return true;

    // Any thread may call this; one resolves at a time.
    static std::mutex resolving;
    const std::unique_lock<std::mutex> held(resolving, std::try_to_lock);
    if (!held.owns_lock()) return false;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(500)) return false;
    last = now;

    static std::string reasons[std::size(kKinds)];
    static unsigned said[std::size(kKinds)] = {};
    std::size_t kinds = 0;
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        if (state().Ready[k].load(std::memory_order_acquire)) {
            ++kinds;
            continue;
        }
        const std::string key = static_key(k);
        const std::size_t count = bg3le_static_count(key.c_str());
        std::string why = count == 0 ? "no recorded path" : "";
        for (std::size_t i = 0; i < count; ++i) {
            void* at = bg3le_static_get(key.c_str(), i);
            if (at == nullptr) continue;
            Table found;
            if (!harvest_map((unsigned long long)(std::uintptr_t)at, k, &found)) {
                if (why.empty()) why = describe_map((unsigned long long)(std::uintptr_t)at, k);
                continue;
            }
            logf("prototypes: %zu %s prototypes at %p without scanning, "
                 "from recorded static %zu of %zu",
                 found.ByName.size(), kKinds[k].Name, at, i, count);
            bg3le_static_confirm(key.c_str(), i);
            publish(k, std::move(found));
            ++kinds;
            why.clear();
            break;
        }
        // Debug: what the first recorded path held, each time that changes.
        if (!why.empty() && why != reasons[k] && said[k] < 20) {
            ++said[k];
            reasons[k] = why;
            logf("prototypes: %s manager not usable from its recorded path yet (%s; game state %s%s)",
                 kKinds[k].Name, why.c_str(), client_game_state_or_unknown(),
                 bg3le_stats_is_loading() ? ", inside RPGStats::Load" : "");
        }
    }

    static std::size_t reported = ~std::size_t{0};
    if (kinds < std::size(kKinds)) {
        if (kinds != reported) {
            reported = kinds;
            logf("prototypes: %zu of %zu managers resolved from a recorded path; "
                 "the rest are tried again and scanned for", kinds, std::size(kKinds));
        }
        return false;
    }
    state().Built.store(true, std::memory_order_release);
    return true;
}

bool build() {
    struct { Table Kinds[std::size(kKinds)]; } found{};
    unsigned long long at[std::size(kKinds)] = {};
    std::vector<Candidate> candidates;
    std::vector<unsigned long long> refmaps;
    const auto image = image_span();

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kChunk = 1u << 20;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kMapSize);

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kMapSize) want = kChunk + kMapSize;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            scan_yield();
            if (got < kMapSize) continue;

            for (std::size_t off = 0; off + kMapSize <= got; off += 8) {
                // The passive manager: a vtable into the executable, then
                // a LegacyRefMap. Shape from the block, contents after.
                if (off >= 8 && refmaps.size() < kMaxCandidates) {
                    std::uint64_t vmt = 0;
                    std::uint32_t items = 0;
                    std::uint32_t buckets = 0;
                    std::uint64_t table = 0;
                    std::memcpy(&vmt, block.data() + off - 8, sizeof(vmt));
                    std::memcpy(&items, block.data() + off, sizeof(items));
                    std::memcpy(&buckets, block.data() + off + 4,
                                sizeof(buckets));
                    std::memcpy(&table, block.data() + off + 8, sizeof(table));
                    std::vector<std::uint32_t> names;
                    if (vmt >= image.first && vmt < image.second
                        && items >= kMinEntries && items <= kMaxEntries
                        && buckets != 0 && buckets <= kMaxEntries
                        && table > 0x10000 && table < 0x800000000000ull
                        && (table & 7) == 0
                        && refmap_consistent(base + off,
                                             kKinds[kPassive].NameOffset,
                                             &names)) {
                        refmaps.push_back(base + off);
                    }
                }

                // From the block, no syscalls: the counts have to be in
                // range before anything is dereferenced.
                std::uint32_t capacity = 0;
                std::uint32_t count = 0;
                std::memcpy(&capacity, block.data() + off + kKeysCapacity,
                            sizeof(capacity));
                std::memcpy(&count, block.data() + off + kKeysSize,
                            sizeof(count));
                if (count < kMinEntries || count > kMaxEntries
                    || count > capacity) {
                    continue;
                }

                // A real HashMap's three internal arrays agree with one
                // another, and that can be checked from the block without
                // a single read. It has to be: the count test alone left
                // tens of thousands of candidates, each costing two dozen
                // process_vm_readv calls, and that volume takes the mmap
                // lock often enough to leave the game blocked on its own
                // allocations -- idle rather than busy, and unable to
                // tick.
                std::uint64_t hashKeys = 0;
                std::uint32_t hashKeysSize = 0;
                std::uint64_t nextIds = 0;
                std::uint32_t nextIdsCapacity = 0;
                std::uint32_t nextIdsSize = 0;
                std::uint64_t keysWord = 0;
                std::uint64_t valuesWord = 0;
                std::memcpy(&hashKeys, block.data() + off, sizeof(hashKeys));
                std::memcpy(&hashKeysSize, block.data() + off + 8,
                            sizeof(hashKeysSize));
                std::memcpy(&nextIds, block.data() + off + 16,
                            sizeof(nextIds));
                std::memcpy(&nextIdsCapacity, block.data() + off + 24,
                            sizeof(nextIdsCapacity));
                std::memcpy(&nextIdsSize, block.data() + off + 28,
                            sizeof(nextIdsSize));
                std::memcpy(&keysWord, block.data() + off + kKeysBuffer,
                            sizeof(keysWord));
                std::memcpy(&valuesWord, block.data() + off + kValuesBuffer,
                            sizeof(valuesWord));

                // NextIds runs parallel to Keys, so their extents match.
                if (nextIdsSize != count || nextIdsCapacity != capacity) {
                    continue;
                }
                // The bucket table is sized for the entries it holds.
                if (hashKeysSize < count / 2 || hashKeysSize > count * 4) {
                    continue;
                }
                // And all four buffers are heap pointers.
                auto plausible = [](std::uint64_t p) {
                    return p > 0x10000 && p < 0x800000000000ull
                           && (p & 7) == 0;
                };
                if (!plausible(hashKeys) || !plausible(nextIds)
                    || !plausible(keysWord) || !plausible(valuesWord)) {
                    continue;
                }

                auto const* keys = (void const*)(std::uintptr_t)keysWord;
                auto const* values = (void const*)(std::uintptr_t)valuesWord;

                // Every prototype names itself at the same offset, so
                // one check decides whether this is a prototype map at
                // all. Which kind it is costs stat lookups, so it is
                // settled after the scan rather than inside it: asking
                // per candidate meant a linear walk of 15,754 stats for
                // each one, which starved the story thread until the
                // debugger could not get a tick.
                bool isInline = false;
                if (!self_consistent(keys, values, count,
                                     kKinds[kSpell].NameOffset)) {
                    if (!inline_head(keys, values,
                                     kKinds[kInterrupt].NameOffset)) {
                        continue;
                    }
                    isInline = true;
                }
                if (candidates.size() < kMaxCandidates) {
                    candidates.push_back(Candidate{keys, values, count,
                                                   base + off, isInline});
                }
            }
        }
    }
    std::fclose(maps);

    // Now the stat lookups. Only the largest few candidates, because a
    // real manager holds thousands of entries and each lookup is a linear
    // walk of every stat -- asking all of them would be millions of reads
    // for an answer two maps decide.
    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& a, Candidate const& b) {
                  return a.Count > b.Count;
              });
    // Every candidate, before the pointer pass narrows to the largest
    // few: the passive and interrupt managers hold hundreds where the
    // spell and status ones hold thousands, so truncating first left them
    // out entirely. The inline test costs reads rather than stat lookups,
    // so it can afford the whole list.
    std::vector<Candidate> const inlineCandidates = candidates;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                    [](Candidate const& c) { return c.Inline; }),
                     candidates.end());
    if (candidates.size() > kClassify) candidates.resize(kClassify);

    for (Candidate const& candidate : candidates) {
        // Both found: stop. Classifying a leftover costs a linear walk of
        // every stat per key, and doing that for the remaining candidates
        // put over two minutes between finding the maps and publishing
        // them -- long enough that a caller in between saw nothing.
        if (!found.Kinds[kSpell].ByName.empty()
            && !found.Kinds[kStatus].ByName.empty()) {
            break;
        }

        const int kind = kind_of(candidate.Keys, candidate.Count);
        if (kind < 0) continue;
        if (!found.Kinds[kind].ByName.empty()) continue;

        const std::size_t added =
            harvest(candidate.Keys, candidate.Values, candidate.Count,
                    kKinds[kind].NameOffset, &found.Kinds[kind]);
        if (added > 0) {
            at[kind] = candidate.At;
            found.Kinds[kind].MapAt = candidate.At;
            logf("prototypes: %zu %s prototypes at %#llx", added,
                 kKinds[kind].Name, candidate.At);
        }
    }

    // The inline-valued managers. Same candidates, different value
    // layout: the prototype sits in the map rather than behind a pointer,
    // so the stride is derived and the name still has to match the key.
    for (Candidate const& candidate : inlineCandidates) {
        constexpr std::size_t k = kInterrupt;
        if (!candidate.Inline || !found.Kinds[k].ByName.empty()) continue;

        const std::size_t stride = inline_stride(
            candidate.Keys, candidate.Values, candidate.Count,
            kKinds[k].NameOffset);
        if (stride == 0) continue;
        if (kind_of(candidate.Keys, candidate.Count) != (int)k) continue;

        const std::size_t added = harvest_inline(
            candidate.Keys, candidate.Values, candidate.Count,
            kKinds[k].NameOffset, stride, &found.Kinds[k]);
        if (added > 0) {
            at[k] = candidate.At;
            found.Kinds[k].MapAt = candidate.At;
            found.Kinds[k].Stride = stride;
            logf("prototypes: %zu %s prototypes at %#llx, stride %zu",
                 added, kKinds[k].Name, candidate.At, stride);
        }
    }

    // The passive manager's chained map, confirmed by the stats its names
    // belong to.
    for (unsigned long long refmap : refmaps) {
        constexpr std::size_t k = kPassive;
        if (!found.Kinds[k].ByName.empty()) break;

        std::vector<std::uint32_t> names;
        if (!refmap_consistent(refmap, kKinds[k].NameOffset, &names)
            || kind_of_names(names.data(), names.size()) != (int)k) {
            continue;
        }
        const std::size_t added =
            harvest_refmap(refmap, kKinds[k].NameOffset, &found.Kinds[k]);
        if (added > 0) {
            at[k] = refmap;
            found.Kinds[k].MapAt = refmap;
            logf("prototypes: %zu %s prototypes at %#llx", added,
                 kKinds[k].Name, refmap);
        }
    }

    std::size_t total = 0;
    for (auto const& table : found.Kinds) total += table.ByName.size();
    if (total == 0) {
        logf("prototypes: this scan found no prototype manager (game state %s); "
             "they are looked for again on the next lookup",
             client_game_state_or_unknown());
        return false;
    }

    std::size_t sizes[std::size(kKinds)] = {};
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        sizes[k] = found.Kinds[k].ByName.size();
        read_signature(found.Kinds[k].MapAt, k, &found.Kinds[k].Sig);
    }

    // Filled first, published second; a kind already resolved from its
    // recorded path keeps that table.
    Prototypes& live = state();
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        if (sizes[k] == 0 || live.Ready[k].load(std::memory_order_acquire)) continue;
        publish(k, std::move(found.Kinds[k]));
    }
    live.Built.store(true, std::memory_order_release);

    // Recorded so the next run reads the engine's own pointer. The window
    // is one map: the scan lands on the map itself, and whatever owns it
    // points at the object the map is a member of.
    constexpr std::uint64_t kOwnerWindow = 1u << 12;
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        if (at[k] == 0) continue;
        bg3le_static_record_path(static_key(k).c_str(),
                                 (void const*)(std::uintptr_t)at[k],
                                 kOwnerWindow);
    }

    logf("prototypes: %zu spells, %zu statuses, %zu interrupts and %zu "
         "passives found by the scan",
         sizes[kSpell], sizes[kStatus], sizes[kInterrupt], sizes[kPassive]);
    return true;
}

bool ready() {
    if (state().Built.load(std::memory_order_acquire)) return true;

    // Resolving a recorded pointer is not a scan, so any thread may do it.
    if (build_from_statics()) return true;

    // Only the warming thread scans; see mem.h.
    if (!scan_allowed()) return false;

    static int attempts = 0;
    if (attempts >= 40) return false;
    ++attempts;
    return build();
}

// Client and server Lua both look prototypes up, and a lookup can re-read a table.
std::mutex& tables_lock() {
    static std::mutex m;
    return m;
}

// Whether a kind's table is live, trying its recorded path again if not.
bool kind_ready(std::size_t k) {
    std::atomic<bool> const& live = state().Ready[k];
    if (live.load(std::memory_order_acquire)) return true;
    if (!build_from_statics() && !live.load(std::memory_order_acquire)) ready();
    return live.load(std::memory_order_acquire);
}

// The table for a kind once found, or null. Checked before the lock is
// taken: the first build can scan for seconds, and publishing takes it.
Table* table_of(int kind) {
    if (kind < 0 || (std::size_t)kind >= std::size(kKinds) || !kind_ready((std::size_t)kind)) {
        return nullptr;
    }
    return &state().Kinds[kind];
}

// Reads a kind's table again from the engine's map, which the engine
// rebuilds when a save loads: every prototype the old table points at is
// freed then. The manager may have moved with it, so its recorded path is
// tried as well. On failure the table is emptied rather than left stale, and
// the kind is looked for again from its recorded path on the next call.
bool refresh(std::size_t k) {
    Table& table = state().Kinds[k];
    Table fresh;
    bool ok = harvest_map(table.MapAt, k, &fresh);
    const std::string key = static_key(k);
    for (std::size_t i = 0, n = bg3le_static_count(key.c_str()); !ok && i < n; ++i) {
        fresh = Table{};
        void* at = bg3le_static_get(key.c_str(), i);
        ok = at != nullptr && harvest_map((unsigned long long)(std::uintptr_t)at, k, &fresh);
    }
    if (!ok) {
        logf("prototypes: the engine rebuilt its %s prototypes and they could not be "
             "read again (%s; game state %s); retrying from the recorded path",
             kKinds[k].Name, describe_map(table.MapAt, k).c_str(), client_game_state_or_unknown());
        table = Table{};
        state().Ready[k].store(false, std::memory_order_release);
        return false;
    }
    fresh.Order.reserve(fresh.ByName.size());
    for (auto const& entry : fresh.ByName) fresh.Order.push_back(entry.first);
    logf("prototypes: the engine rebuilt its %s prototypes; read %zu again (was %zu; game state %s)",
         kKinds[k].Name, fresh.ByName.size(), table.ByName.size(), client_game_state_or_unknown());
    table = std::move(fresh);
    if (k == kPassive) {
        bg3le_prototypes_debug_check_passives((void const*)(std::uintptr_t)table.MapAt,
                                              "after the engine rebuilt them");
    }
    return true;
}

bool signature_current(Table const& table, std::size_t k) {
    Signature now;
    return read_signature(table.MapAt, k, &now) && now == table.Sig;
}

// Whether the engine's map still holds `at` for `name` where it was harvested.
bool still_held(Table const& table, std::size_t k, std::string const& name, std::uint64_t at) {
    if (!signature_current(table, k)) return false;
    auto slot = table.Slots.find(name);
    if (slot == table.Slots.end()) return true;  // one bg3le added; the signature covers it
    std::uint32_t key = 0;
    if (slot->second.Index == kNoIndex) {
        return read_as((char const*)(std::uintptr_t)(at - kNodeValue + kNodeKey), &key)
               && key == slot->second.Key;
    }
    if (!read_as((char const*)(std::uintptr_t)table.Sig.Keys + slot->second.Index * kKeyStride, &key)
        || key != slot->second.Key) {
        return false;
    }
    if (k == kInterrupt) return table.Sig.Values + slot->second.Index * table.Stride == at;
    std::uint64_t value = 0;
    return read_as((char const*)(std::uintptr_t)table.Sig.Values + slot->second.Index * kValueStride,
                   &value)
           && value == at;
}

// The table, read again first if the engine has rebuilt its map since.
// Called under the tables lock, so not through table_of, which may publish.
Table* current_table(int kind) {
    Table* table = &state().Kinds[kind];
    if (table->MapAt != 0 && !signature_current(*table, (std::size_t)kind)) {
        refresh((std::size_t)kind);
    }
    return table;
}

}  // namespace

extern "C" bool bg3le_prototypes_ready() { return ready(); }

// kind: 0 spell, 1 status.
extern "C" void* bg3le_prototype_find(int kind, char const* name) {
    if (table_of(kind) == nullptr || name == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(tables_lock());
    Table* table = current_table(kind);

    auto it = table->ByName.find(name);
    if (it == table->ByName.end()) return nullptr;
    if (!still_held(*table, (std::size_t)kind, it->first, it->second)) {
        if (!refresh((std::size_t)kind)) return nullptr;
        it = table->ByName.find(name);
        if (it == table->ByName.end()) return nullptr;
    }
    return (void*)(std::uintptr_t)it->second;
}

// The manager's map for a kind, or 0.
extern "C" void* bg3le_prototype_map(int kind) {
    if (table_of(kind) == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(tables_lock());
    Table* table = current_table(kind);
    return (void*)(std::uintptr_t)table->MapAt;
}

// A prototype bg3le has just added to the manager, which moved the map's
// count and perhaps its buffers.
extern "C" void bg3le_prototype_added(int kind, char const* name, void* prototype) {
    if (table_of(kind) == nullptr || name == nullptr) return;
    const std::lock_guard<std::mutex> held(tables_lock());
    Table& table = state().Kinds[kind];
    if (table.ByName.emplace(name, (std::uint64_t)(std::uintptr_t)prototype).second) {
        table.Order.push_back(name);
    }
    read_signature(table.MapAt, (std::size_t)kind, &table.Sig);
}

extern "C" std::size_t bg3le_prototype_count(int kind) {
    if (table_of(kind) == nullptr) return 0;
    const std::lock_guard<std::mutex> held(tables_lock());
    return current_table(kind)->ByName.size();
}

// Test hook: points one cached entry at garbage shaped like freed memory, so
// the next lookup has to notice and read the table again. Touches bg3le's
// index only, never the engine's.
extern "C" bool bg3le_prototype_test_stale(int kind, char const* name) {
    if (table_of(kind) == nullptr || name == nullptr) return false;
    const std::lock_guard<std::mutex> held(tables_lock());
    Table& table = state().Kinds[kind];
    auto it = table.ByName.find(name);
    if (it == table.ByName.end()) return false;
    static std::uint32_t garbage[4096];
    for (auto& word : garbage) word = 0x558b;  // a string id in sub-table 11, which does not exist
    it->second = (std::uint64_t)(std::uintptr_t)garbage;
    logf("prototypes: test: %s %s now points at garbage", kKinds[kind].Name, name);
    return true;
}

// Copied out: a refresh on another thread replaces the table. Not checked
// against the engine per name; bg3le_prototype_count, called first, did that.
extern "C" char const* bg3le_prototype_name_at(int kind, std::size_t index) {
    Table* table = table_of(kind);
    if (table == nullptr) return nullptr;
    const std::lock_guard<std::mutex> held(tables_lock());
    if (index >= table->Order.size()) return nullptr;
    thread_local std::string name;
    name = table->Order[index];
    return name.c_str();
}

}  // namespace bg3le
