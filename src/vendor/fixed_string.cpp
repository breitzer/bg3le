// Resolves a FixedString to its text, by finding the engine's global string
// table in memory.
//
// A FixedString is a 32-bit index into a table the engine builds at startup.
// Everything else in bg3le reads component data through layout that the
// compiler works out, but this one needs an address, and there is nothing to
// look up: ls::FixedString's methods are all inlined in the native build, so
// unlike the allocator there is no entry point to borrow, and the table itself
// has no symbol. It is a heap allocation reached through an anonymous global.
//
// It can be found by its shape instead. The table is an array of sub-tables,
// each carrying its own index, so eleven consecutive integers running 0..10 at
// a fixed stride identify it -- a coincidence of eleven ascending values at
// exactly the right spacing is not something a heap produces by accident. The
// candidate is then checked by resolving an entry and confirming the string's
// recorded length matches the bytes actually there.
//
// The layout and the index arithmetic below are bg3se's, from
// CoreLib/Base/BaseString.{h,inl} -- FixedStringBase::FindEntry. The walk is
// reimplemented here rather than called because FindEntry is protected and,
// more importantly, because it strides by sizeof(SubTable) as this build
// computes it, which is not the engine's: SubTable ends with a
// CRITICAL_SECTION, and the Linux stand-in for that is 48 bytes where the
// Windows type is 40. Every offset this needs sits before that member, so the
// offsets are reliable and only the stride is not -- and the stride is checked
// against the table rather than assumed.
//
// bg3se is by Norbyte and the bg3se contributors; only the search is ours.

#include <stdafx.h>

#include <CoreLib/Base/BaseString.h>

#include <chrono>
#include <cstdio>
#include <utility>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iterator>
#include <unordered_map>
#include <vector>
#include <string>
#include <mutex>

#include "../log.h"
#include "cache_lock.h"
#include "../mem.h"

namespace bg3le {
namespace {

using bg3se::FixedString;
using bg3se::GlobalStringTable;

using SubTable = GlobalStringTable::SubTable;
using Header = bg3se::FixedStringBase::Header;

// The engine's stride between sub-tables.
//
// Not sizeof(SubTable): see the file comment. bg3se's field names record the
// offsets they were found at -- field_1180 at 0x1180, field_11E8 at 0x11E8 --
// and the struct ends with three more qwords, which puts the engine's
// SubTable at 0x1200. Assumed here and then verified against eleven
// sub-tables, so a wrong value finds nothing rather than finding rubbish.
constexpr std::size_t kSubTableStride = 0x1200;

// Every offset the walk needs, and each one is below the CRITICAL_SECTION that
// makes the total size diverge, so these come from the struct itself.
static_assert(offsetof(SubTable, TableIndex) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, EntrySize) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, EntriesPerBucket) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, NumBuckets) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, Buckets) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, field_1100) == 0x1100 && offsetof(SubTable, field_1180) == 0x1180);
static_assert(offsetof(SubTable, CriticalSection) < kSubTableStride,
              "the stride has to leave room for everything before it");

// A FixedString is just the index, so a field holding one is four bytes.
static_assert(sizeof(FixedString) == sizeof(std::uint32_t));

constexpr std::size_t kSubTableCount =
    sizeof(GlobalStringTable::SubTables) / sizeof(GlobalStringTable::SubTable);

template <class T>
T read_at(void const* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, (char const*)base + offset, sizeof(T));
    return value;
}

void const* sub_table(void const* table, std::size_t index) {
    return (char const*)table + index * kSubTableStride;
}

// Whether a sub-table's own fields are self-consistent. Deliberately loose:
// this only has to reject the overwhelming majority of coincidences, and the
// string check afterwards is what actually confirms the find.
bool sub_table_plausible(void const* st) {
    const auto entrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
    const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
    const auto buckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
    const auto bucketPtr = read_at<void*>(st, offsetof(SubTable, Buckets));

    if (entrySize < sizeof(Header) + 1 || entrySize > 0x10000) return false;
    if (perBucket == 0 || perBucket > (1u << 24)) return false;
    if (buckets == 0 || buckets > (1u << 24)) return false;
    if (bucketPtr == nullptr) return false;
    return true;
}

// The address of a string, or null. The index arithmetic is bg3se's.
//
// The entry's own hash comes back too, for the caller that wants it: it is
// what the engine hashes a FixedString by, and bg3se's FixedString::GetHash
// reaches it through an engine function bg3le has no pointer for. The bytes
// are right here.
char const* resolve(void const* table, std::uint32_t id, std::uint32_t* length,
                    std::uint32_t* hash = nullptr) {
    if (table == nullptr || id == FixedString::NullIndex) return nullptr;

    const std::size_t subTableIdx = id & 0x0F;
    if (subTableIdx >= kSubTableCount) return nullptr;

    void const* st = sub_table(table, subTableIdx);
    const std::size_t bucketIdx = (id >> 4) & 0xffff;
    const std::size_t entryIdx = (id >> 20);

    const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
    const auto numBuckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
    if (bucketIdx >= numBuckets || entryIdx >= perBucket) return nullptr;

    const auto entrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
    auto** buckets = read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
    if (buckets == nullptr) return nullptr;

    std::uint8_t* bucket = nullptr;
    if (!safe_read(&buckets[bucketIdx], &bucket, sizeof(bucket))
        || bucket == nullptr) {
        return nullptr;
    }

    auto const* header = (Header const*)(bucket + entryIdx * entrySize);
    Header copy{};
    if (!safe_read(header, &copy, sizeof(copy))) return nullptr;
    if (copy.Length > entrySize) return nullptr;

    if (length != nullptr) *length = copy.Length;
    if (hash != nullptr) *hash = copy.Hash;
    return (char const*)(header + 1);
}

// Confirms a candidate by resolving something out of it and checking the
// string against its own recorded length. A structure that merely looks like
// the table will not survive this.
bool table_resolves_a_string(void const* table) {
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto numBuckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        for (std::uint32_t bucket = 0; bucket < numBuckets && bucket < 64; ++bucket) {
            for (std::uint32_t entry = 0; entry < perBucket && entry < 8; ++entry) {
                const std::uint32_t id =
                    (std::uint32_t)sub | (bucket << 4) | (entry << 20);
                std::uint32_t length = 0;
                char const* str = resolve(table, id, &length);
                if (str == nullptr || length == 0 || length > 512) continue;

                char buf[513];
                if (!safe_read(str, buf, length + 1)) continue;
                if (buf[length] != '\0') continue;

                bool printable = true;
                for (std::uint32_t i = 0; i < length; ++i) {
                    if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e) {
                        printable = false;
                        break;
                    }
                }
                if (!printable) continue;

                buf[length] = '\0';
                logf("string table: resolved id %#x to \"%s\"", id, buf);
                return true;
            }
        }
    }
    return false;
}

// Eleven consecutive sub-table indices at the expected stride.
bool looks_like_table(void const* candidate, std::size_t available) {
    const std::size_t span = (kSubTableCount - 1) * kSubTableStride
                             + offsetof(SubTable, TableIndex) + sizeof(std::int32_t);
    if (available < span) return false;

    for (std::size_t k = 0; k < kSubTableCount; ++k) {
        const auto index = read_at<std::int32_t>(
            sub_table(candidate, k), offsetof(SubTable, TableIndex));
        if (index != (std::int32_t)k) return false;
    }
    return true;
}

void* g_table = nullptr;
bool g_searched = false;

}  // namespace

// Whether a /proc/self/maps line describes a region worth scanning, and its
// bounds.
//
// Exported so it can be checked against real map lines without a game. The
// first version of this skipped two fields before looking for the pathname,
// but the format is
//
//   address perms offset dev inode pathname
//
// so it landed on the inode, saw a digit, concluded every region was
// file-backed, and scanned nothing at all -- reporting "not found" for a table
// it had never looked for. The logging caught that; a test would have caught
// it sooner.
//
// Anonymous writable mappings only, which is not a guess about where the table
// lives: bg3se reaches it through a GlobalStringTable**, so the variable holds
// a pointer and the table itself is a heap allocation. Two other reasons agree.
// The scan reads directly rather than through safe_read, because safe_read is
// a process_vm_readv syscall and this makes hundreds of millions of probes --
// and a direct read of a file-backed page can raise SIGBUS if the file has
// been truncated, where an anonymous page cannot. So the fast path is also the
// safe one.
extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to) {
    char perms[8] = {0};
    int consumed = 0;
    if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", from, to, perms,
                    &consumed) < 3) {
        return false;
    }
    if (perms[0] != 'r' || perms[1] != 'w') return false;
    if (*to <= *from) return false;

    char const* path = line + consumed;
    while (*path == ' ') ++path;

    // No pathname at all, or one of the kernel's bracketed names. [heap] and
    // [stack] are anonymous and fine to read; the two below are not ordinary
    // memory.
    if (*path == '\0' || *path == '\n') return true;
    if (*path != '[') return false;
    if (std::strncmp(path, "[vvar", 5) == 0) return false;
    if (std::strncmp(path, "[vsyscall", 9) == 0) return false;
    return true;
}

namespace {

bool scannable_region(char const* line, unsigned long long* from,
                      unsigned long long* to) {
    return bg3le_scannable_region(line, from, to);
}

// Scans every writable region, in address order.
void* search_for_table() {
    // Logged rather than left as a claim in the comment above: this is the
    // divergence that makes sizeof(SubTable) unusable as the stride.
    logf("string table: searching with stride %#zx; this build computes "
         "sizeof(SubTable) as %#zx, %zu bytes larger, because the Linux "
         "critical-section stand-in is wider than the Windows type",
         kSubTableStride, sizeof(SubTable),
         sizeof(SubTable) - kSubTableStride);

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;

    char line[512];
    std::size_t regions = 0;
    std::size_t scanned = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!scannable_region(line, &from, &to)) continue;

        ++regions;
        const std::size_t size = (std::size_t)(to - from);
        if (size < kSubTableCount * kSubTableStride) continue;
        scanned += size;

        for (unsigned long long addr = from; addr + kSubTableStride <= to;
             addr += 8) {
            void const* candidate = (void const*)addr;
            if (!looks_like_table(candidate, (std::size_t)(to - addr))) continue;
            if (!sub_table_plausible(candidate)) continue;
            if (!table_resolves_a_string(candidate)) continue;

            std::fclose(maps);
            logf("string table: found at %p (scanned %zu bytes over %zu "
                 "regions)", candidate, scanned, regions);
            return (void*)candidate;
        }
    }

    std::fclose(maps);
    logf("string table: not found (scanned %zu bytes over %zu regions); "
         "FixedString fields stay unreadable", scanned, regions);
    return nullptr;
}

}  // namespace

// Finds the table on first use and caches the answer.
//
// Deliberately lazy: the engine builds the table during startup, so a search
// at load time would find nothing and cache that.
extern "C" void* bg3le_string_table() {
    const CacheLock lock(string_cache_lock());
    if (!g_searched) {
        g_searched = true;
        g_table = search_for_table();
    }
    return g_table;
}

// The index of a string, by its text, or false if the table does not hold it.
//
// This exists so a scan can look for one exact 32-bit value instead of
// guessing at plausible indices. The stats search first tried the latter:
// seven plausibility tests at every four-byte offset of every writable
// region, which took 114 seconds. Searching for a known index is a plain
// memory compare.
//
// Walks buckets rather than entries. Resolving an entry costs a
// process_vm_readv each, and a table of this size has millions of them, so
// each bucket is read whole and searched locally -- one syscall per bucket.
// Text to id, from an index built once.
//
// A FixedString compares by id, so anything that has to put a *named*
// string where the engine will compare it -- a spell in a spell list --
// needs the id the engine already has for that text. A second entry with
// the same characters is a different string as far as the engine is
// concerned.
//
// Finding it used to mean walking every bucket of every sub-table, about a
// million and a half entries, at eighty milliseconds a call. So the walk
// happens once and what it collects is kept: the hash of each string
// against its id, sorted, which is twelve bytes an entry rather than the
// text itself. A lookup hashes, binary-searches, and proves each candidate
// by resolving it and comparing, so a hash collision costs a comparison
// rather than a wrong answer.
std::uint64_t text_hash(char const* text, std::size_t length) {
    std::uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a
    for (std::size_t i = 0; i < length; ++i) {
        h ^= (unsigned char)text[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

// How much of a bucket to ask for at a time. Small enough that one
// unreadable page costs a piece rather than the rest of the bucket.
constexpr std::size_t kReadChunk = 8192;

using TextIndex = std::vector<std::pair<std::uint64_t, std::uint32_t>>;

// How much table there is to index, which is what tells a stale index from
// a current one.
//
// The engine allocates buckets as it interns, so the table grows all
// through a session -- 262,425 entries while a level was loading, over a
// million by the time it had. Building the index once was therefore
// building it from whatever existed at the first lookup, and a string
// interned afterwards could not be found: the intern path then made a
// second entry for text the engine already had, which is the one outcome
// that matters, because a FixedString compares by id.
std::uint64_t table_extent(void const* table) {
    std::uint64_t extent = 0;
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;
        extent += (std::uint64_t)
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        extent <<= 1;
    }
    return extent;
}

// What the extent misses: a string added to a bucket the engine already has.
// field_1100 counts a sub-table's entries in use and field_1180 moves with it;
// "Epic" arriving that way left the stats unfound for a whole load.
std::uint64_t table_usage(void const* table) {
    std::uint64_t usage = 0;
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;
        usage = usage * 1000003u + read_at<std::uint32_t>(st, offsetof(SubTable, field_1100));
        usage = usage * 1000003u + read_at<std::uint64_t>(st, offsetof(SubTable, field_1180));
    }
    return usage;
}

// fresh also rebuilds when strings were added without the table growing.
TextIndex const& text_index(bool fresh = false) {
    static TextIndex index;
    static std::uint64_t builtFor = 0;
    static std::uint64_t builtUsage = 0;

    void const* table = bg3le_string_table();
    if (table == nullptr) return index;

    const std::uint64_t extent = table_extent(table);
    const std::uint64_t usage = fresh ? table_usage(table) : builtUsage;
    if (!index.empty() && extent == builtFor && usage == builtUsage) return index;

    index.clear();
    builtFor = extent;
    builtUsage = table_usage(table);

    // What the walk rejects, and why. The first version of this index
    // collected a fifth of the table and the lookups it was built for
    // silently missed, so the reasons are counted rather than assumed.
    std::size_t seen = 0, freed = 0, tooLong = 0, truncated = 0, unterm = 0;
    std::size_t skippedTables = 0, skippedBuckets = 0;

    std::vector<unsigned char> bucketBuf;
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) {
            ++skippedTables;
            continue;
        }

        const auto entrySize =
            read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        const auto numBuckets =
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        const auto perBucket =
            read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        auto** buckets =
            read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
        if (buckets == nullptr || entrySize == 0 || perBucket == 0) continue;

        const std::size_t span =
            (std::size_t)perBucket * (std::size_t)entrySize;
        if (span == 0 || span > (64u << 20)) continue;
        bucketBuf.resize(span);

        for (std::uint32_t b = 0; b < numBuckets; ++b) {
            std::uint8_t* bucket = nullptr;
            if (!safe_read(&buckets[b], &bucket, sizeof(bucket))
                || bucket == nullptr) {
                ++skippedBuckets;
                continue;
            }
            // In pieces, and the buffer is cleared first so a piece
            // that cannot be read leaves zeroes rather than the last
            // bucket's bytes -- a zero header is skipped below as free.
            //
            // A single read of the whole bucket is what the first version
            // did, and process_vm_readv returns a partial count when it
            // reaches a page it cannot read, so the parse loop stopped
            // there and abandoned the rest. The index came out with
            // 262,425 of the table's 1,211,260 entries, just over a
            // fifth, and a lookup for a string that was plainly there
            // missed. Nothing said so: a short read is not an error.
            std::fill(bucketBuf.begin(), bucketBuf.end(), (unsigned char)0);
            for (std::size_t off = 0; off < span; off += kReadChunk) {
                const std::size_t want = std::min(kReadChunk, span - off);
                if (safe_read_some(bucket + off, bucketBuf.data() + off,
                                   want) < want) {
                    ++skippedBuckets;
                }
            }

            for (std::uint32_t e = 0; e < perBucket; ++e) {
                const std::size_t at = (std::size_t)e * (std::size_t)entrySize;
                if (at + sizeof(Header) + 1 > span) break;
                ++seen;

                auto const* header = (Header const*)(bucketBuf.data() + at);
                if (header->RefCount == 0 || header->Length == 0) {
                    ++freed;
                    continue;
                }
                if (header->Length > entrySize - sizeof(Header)) {
                    ++tooLong;
                    continue;
                }
                if (at + sizeof(Header) + header->Length + 1 > span) {
                    ++truncated;
                    continue;
                }

                char const* text = (char const*)(header + 1);
                if (text[header->Length] != '\0') {
                    ++unterm;
                    continue;
                }

                index.emplace_back(text_hash(text, header->Length),
                                   (std::uint32_t)sub | (b << 4) | (e << 20));
            }
        }
    }

    std::sort(index.begin(), index.end());
    logf("string table: %zu strings indexed by text, out of %zu entries "
         "walked (%zu free, %zu over-long, %zu truncated, %zu unterminated; "
         "%zu sub-tables and %zu buckets skipped)",
         index.size(), seen, freed, tooLong, truncated, unterm,
         skippedTables, skippedBuckets);
    return index;
}

// The hash the engine stores with a string, by id.
//
// A FixedString in a hash set is bucketed by this rather than by its id, and
// nothing else can stand in for it: the hash is FNV-1a over the text, but it
// is the engine's copy that the engine's own lookups use, so it is the copy
// that is read.
extern "C" bool bg3le_fixed_string_hash(std::uint32_t id,
                                        std::uint32_t* out) {
    const CacheLock lock(string_cache_lock());
    if (out == nullptr) return false;
    // Upstream's GetHash: the null string hashes to 0. RPGStats'
    // TreasureCategories keys its unnamed first category that way.
    if (id == bg3se::FixedStringBase::NullIndex) {
        *out = 0;
        return true;
    }

    void* table = bg3le_string_table();
    if (table == nullptr) return false;

    std::uint32_t hash = 0;
    if (resolve(table, id, nullptr, &hash) == nullptr) return false;

    *out = hash;
    return true;
}

extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out) {
    const CacheLock lock(string_cache_lock());
    if (wanted == nullptr) return false;
    if (bg3le_string_table() == nullptr) return false;

    const std::size_t wantLen = std::strlen(wanted);
    const std::uint64_t hash = text_hash(wanted, wantLen);

    auto find = [&](TextIndex const& index) {
        auto at = std::lower_bound(
            index.begin(), index.end(),
            std::pair<std::uint64_t, std::uint32_t>{hash, 0});
        for (; at != index.end() && at->first == hash; ++at) {
            std::uint32_t length = 0;
            char const* text = bg3le_fixed_string(at->second, &length);
            if (text == nullptr || length != wantLen) continue;
            if (std::memcmp(text, wanted, wantLen) != 0) continue;

            if (out != nullptr) *out = at->second;
            return true;
        }
        return false;
    };
    if (find(text_index())) return true;

    // A miss may be a string added since the build. Rebuilding is a walk of
    // the whole table, so at most once a second.
    static std::chrono::steady_clock::time_point lastRetry{};
    const auto now = std::chrono::steady_clock::now();
    if (now - lastRetry < std::chrono::seconds(1)) return false;
    lastRetry = now;
    return find(text_index(true));
}

// Rebuilds the index now if strings were added since it was built.
extern "C" void bg3le_fixed_string_reindex() {
    const CacheLock lock(string_cache_lock());
    if (bg3le_string_table() != nullptr) text_index(true);
}

// The text of a FixedString index, or null. length may be null.
// Resolved once per id. The text lives in the engine's own entry and stays
// put for as long as the entry does, so the pointer keeps.
struct Known {
    char const* Text;
    std::uint32_t Length;
};

std::unordered_map<std::uint32_t, Known>& resolved_strings() {
    static std::unordered_map<std::uint32_t, Known> known;
    return known;
}

// Copies of the entries the pool does not terminate. A deque rather than a
// vector because the cache holds pointers into these and they must not move.
std::deque<std::string>& owned_strings() {
    static std::deque<std::string> owned;
    return owned;
}

// Forgets the ids that did not resolve.
//
// Keeping failures is what stops an unset field costing three system calls
// every time it is read. It is wrong for anything built once and kept: an
// id that failed early -- before its bucket was allocated, say -- would
// stay failed, so a stat whose name resolved to nothing would be missing
// from an index for the rest of the session. That is not hypothetical, it
// made Ext.Stats.Get("PotentSpellcasting") return nil. An index builder
// calls this first and resolves afresh.
// Everything, successes included, for a caller about to re-read a set of ids
// it has seen before.
//
// A success can be premature. The engine interns a stat's name as it parses
// the stats, and an id read before its entry was written resolves to whatever
// the memory held -- on one run a stat's Name came back as
// "ls::TranslatedStringRepository::s_HandleUnknown", a symbol string, on an
// object that was otherwise a perfectly good SpellData stat with 204
// attributes and a valid Using. Caching that made it permanent for the
// session, and 5eSpells walks every SpellData stat and reads an attribute off
// each, so one bad name broke the mod.
//
// Intermittent, and it has not been caught in the act -- the stats array is a
// different size on every load, so the index lands at a different point in the
// parse. Ext._Internal.StatsNameRecheck is there to settle it when it happens
// again: it resolves an id fresh and prints that beside what the cache holds.
//
// The pointers already handed out stay valid: the cache holds either a pointer
// into the engine's own entry, which does not move, or a copy owned by
// owned_strings(), which is never cleared.
extern "C" void bg3le_fixed_string_forget_all() {
    const CacheLock lock(string_cache_lock());
    resolved_strings().clear();
}

extern "C" void bg3le_fixed_string_forget_failures() {
    const CacheLock lock(string_cache_lock());
    auto& known = resolved_strings();
    for (auto it = known.begin(); it != known.end();) {
        it = it->second.Text == nullptr ? known.erase(it) : std::next(it);
    }
}

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length) {
    const CacheLock lock(string_cache_lock());
    void* table = bg3le_string_table();
    if (table == nullptr) return nullptr;

    // Resolved once per id. The text lives in the engine's own entry and
    // stays put for as long as the entry does, so the pointer keeps.
    // Worth caching because resolving walks the sub-table, the bucket
    // array and the header -- three reads -- and reading a stat asks for
    // a couple of hundred strings, most of them the same ones over and
    // over.
    auto& known = resolved_strings();
    auto found = known.find(index);
    if (found != known.end()) {
        if (length != nullptr) *length = found->second.Length;
        return found->second.Text;
    }

    std::uint32_t got = 0;
    char const* text = resolve(table, index, &got);

    // Only successes are kept. Keeping failures was worth three quarters
    // of the extender's CPU when the expensive lookups above it were
    // linear scans -- an unset field cost three reads every time it was
    // read -- and it was wrong: an id can fail because its bucket has not
    // been allocated yet, and a cached failure made that permanent. A
    // stat's name would resolve to nothing, so the stat was missing from
    // every index built afterwards, and a mod reading it got nil.
    //
    // With the lookups indexed the saving is not needed. If it is ever
    // needed again it has to expire, not persist.
    if (text == nullptr) return nullptr;

    // The entry's own length is authoritative, and most callers hand this
    // straight to strlen or to lua_pushstring. An entry whose text is not
    // terminated at Length runs into the one after it in the pool, and the
    // result looks like a real name with another string glued to the end:
    // ELEMENTALAFFINITY_ACID came back as
    // "ELEMENTALAFFINITY_ACIDTARGETAOE_IF(HasPassive(..." -- eight of 24,828
    // stat names, enough to break a mod that walks them all and reads an
    // attribute off each.
    //
    // So an unterminated entry is copied and terminated once, and the copy
    // is what the cache holds. The engine's own bytes are never written, and
    // a terminated entry -- which is nearly all of them -- still hands back
    // the pointer into the pool.
    // The terminator is read with safe_read rather than indexed: it is one
    // byte past what the entry declares, and an entry at the end of a bucket
    // could put it on a page that is not there. A read that fails counts as
    // unterminated, which copies -- the safe answer either way.
    char terminator = 1;
    const bool readable =
        safe_read(text + got, &terminator, sizeof(terminator));
    if (!readable || terminator != '\0') {
        auto& owned = owned_strings();
        owned.emplace_back(text, got);
        text = owned.back().c_str();
    }

    known.emplace(index, Known{text, got});

    if (length != nullptr) *length = got;
    return text;
}


// The text an id resolves to now, ignoring the cache, alongside what the
// cache holds.
//
// Diagnostic. A stat whose Name id resolves to something that is not a stat
// name -- a symbol, on one run -- is either a cache that went stale or a
// resolve that was wrong from the start, and those want different fixes. This
// tells them apart: if `fresh` and `cached` differ, the cache is stale.
extern "C" bool bg3le_fixed_string_recheck(std::uint32_t id,
                                           char const** cached,
                                           char const** fresh) {
    const CacheLock lock(string_cache_lock());
    if (cached == nullptr || fresh == nullptr) return false;

    void* table = bg3le_string_table();
    if (table == nullptr) return false;

    *cached = nullptr;
    auto& known = resolved_strings();
    auto found = known.find(id);
    if (found != known.end()) *cached = found->second.Text;

    std::uint32_t got = 0;
    *fresh = resolve(table, id, &got);
    return true;
}

// A FixedString for text the game does not already hold.
//
// Needed because a string attribute on a stat holds an index into a pool
// of FixedString ids, so a mod assigning one -- 5eSpells appends to
// PotentSpellcasting.Boosts -- needs an id for text that has never
// existed.
//
// The sub-tables are length classes: entry sizes 48 through 2080, each
// holding a 24-byte header and the text. Free entries are kept on a list
// threaded through the headers' NextFreeIndex, and field_1180 is its
// head.
//
// That is not a guess. Every sub-table's field_1180 has its own index in
// the low four bits -- 0 through 10, all eleven of them -- which is the
// bottom of a FixedString id, and the rest decodes the same way ids do:
// sub-table in the low four bits, bucket in the next sixteen, entry above
// that, with a counter in the high thirty-two that the engine bumps to
// keep two threads from popping the same entry. For sub-table 6 the head
// reads bucket 172 of 173 and entry 25 of 292, which are in range and
// would not be if the reading were wrong.
//
// So an entry is taken the way the engine takes one: pop the head and put
// what it pointed at in its place -- with a compare-and-swap, because
// that counter is the signature of a tagged lock-free stack and the
// engine is interning on other threads the whole time a level loads.
// Popping with a plain load and store worked perfectly with the game
// sitting idle and lost the race every time during a load, which is what
// sent the engine into its grind the second time.
//
// The first version of this did something else -- it took the entry at
// field_1100, which is a count of live entries rather than a high-water
// mark, so that entry was somewhere in the middle of the allocated region
// and, being free, was on this list. Writing over it cut the chain. The
// engine then handed the same entry out again (a stat's Boosts read back
// as a Gustav animation path) and spent the rest of the level load at
// 250% CPU, which is what walking a severed free list looks like from
// outside.
//
// The refcount is set high enough never to reach zero, so the entry never
// returns to the list and the id a mod holds stays its own.
//
// Nothing is added to the hash map the engine interns through, which is a
// deliberate limitation: the map's layout is not established here, and the
// only consequence is that the engine interning the same text later makes
// its own second entry -- which is what the table looks like anyway when a
// string arrives twice before either copy is released. Text the engine
// already holds is found first and its own id handed back, so a duplicate
// only ever happens for text that did not exist.
constexpr std::uint32_t kInternRefCount = 0x100000;

extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out);

extern "C" bool bg3le_fixed_string_create(char const* text, std::uint32_t* out);

extern "C" bool bg3le_fixed_string_intern(char const* text,
                                          std::uint32_t* out) {
    if (text == nullptr || out == nullptr) return false;
    // The engine's own CreateFromString, when it was found, gives the one id
    // every other path gets: the text index below is built once, so text
    // first created after it was otherwise interned twice under two ids.
    if (bg3le_fixed_string_create(text, out)) return true;

    const CacheLock lock(string_cache_lock());

    // What has already been interned here, so the same text asked for
    // twice costs nothing.
    static std::unordered_map<std::string, std::uint32_t> ours;
    auto known = ours.find(text);
    if (known != ours.end()) {
        *out = known->second;
        return true;
    }

    // Then the engine's own table. This was skipped while the only way to
    // search it was a walk of every bucket -- eighty milliseconds a call,
    // which turned a mod's stats pass into minutes -- and the cost of
    // skipping it was a second entry for text the engine already had. That
    // matters more than it sounds: a FixedString compares by id, so a
    // duplicate is a different string to everything that compares them.
    // The index makes the search a binary search, so it happens again.
    if (bg3le_fixed_string_index_of(text, out)) {
        ours.emplace(text, *out);
        return true;
    }

    void const* table = bg3le_string_table();
    if (table == nullptr) return false;

    const std::size_t length = std::strlen(text);
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto entrySize =
            read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        if (entrySize <= sizeof(Header) + 1) continue;
        if (length + 1 > entrySize - sizeof(Header)) continue;  // next class

        const auto perBucket =
            read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        const auto numBuckets =
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        auto** buckets =
            read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
        if (perBucket == 0 || buckets == nullptr) continue;

        auto* headAt = (std::uint64_t*)((char*)st + 0x1180);
        std::uint8_t* target = nullptr;
        std::uint32_t id = 0;
        std::uint32_t bucketIdx = 0;
        std::uint32_t entryIdx = 0;

        // Pop, competing with the engine's own threads. Everything up to
        // the exchange is a read, so losing simply means going round
        // again.
        constexpr int kAttempts = 64;
        bool popped = false;
        for (int attempt = 0; attempt < kAttempts && !popped; ++attempt) {
            std::uint64_t head = 0;
            __atomic_load(headAt, &head, __ATOMIC_ACQUIRE);

            id = (std::uint32_t)(head & 0xffffffffu);
            if ((id & 0x0f) != sub) {
                logf("string table: sub-table %zu has a free list head of "
                     "%#llx, whose sub-table nibble is %u; not touching it",
                     sub, (unsigned long long)head, id & 0x0f);
                return false;
            }

            bucketIdx = (id >> 4) & 0xffff;
            entryIdx = id >> 20;
            if (bucketIdx >= numBuckets || entryIdx >= perBucket) {
                logf("string table: sub-table %zu's free list head names "
                     "bucket %u of %u and entry %u of %u, which is out of "
                     "range; not touching it", sub, bucketIdx, numBuckets,
                     entryIdx, perBucket);
                return false;
            }

            std::uint8_t* bucket = nullptr;
            if (!safe_read(&buckets[bucketIdx], &bucket, sizeof(bucket))
                || bucket == nullptr) {
                return false;
            }
            target = bucket + (std::size_t)entryIdx * entrySize;

            // It has to read as free, or this is not the free list.
            Header current{};
            if (!safe_read(target, &current, sizeof(current))) return false;
            if (current.RefCount != 0 || current.Length != 0) {
                // Lost the race between the load and this read: whoever
                // won has filled it in. Try again.
                continue;
            }

            std::uint64_t next = current.NextFreeIndex;
            popped = __atomic_compare_exchange(headAt, &head, &next, false,
                                               __ATOMIC_ACQ_REL,
                                               __ATOMIC_ACQUIRE);
        }

        if (!popped) {
            logf("string table: could not take an entry from sub-table "
                 "%zu's free list in %d attempts; the engine is holding it",
                 sub, kAttempts);
            return false;
        }

        Header header{};
        header.Hash = 0;  // the hash map is not touched; see above
        header.RefCount = kInternRefCount;
        header.Length = (std::uint32_t)length;
        header.Id = 1;  // what every live entry in this table reads
        header.NextFreeIndex = 0;

        std::memcpy(target, &header, sizeof(header));
        std::memcpy(target + sizeof(Header), text, length + 1);

        // Proved by reading it back the way everything else reads one.
        std::uint32_t got = 0;
        char const* back = resolve(table, id, &got);
        if (back == nullptr || got != length
            || std::strcmp(back, text) != 0) {
            logf("string table: wrote \"%.48s\" as id %#x but it reads back "
                 "as %s; the entry is off the free list and refcounted, so "
                 "nothing reuses it", text, id,
                 back == nullptr ? "nothing" : back);
            return false;
        }

        static std::size_t said = 0;
        if (++said <= 3) {
            logf("string table: interned %zu bytes as id %#x in sub-table "
                 "%zu (bucket %u entry %u, off the free list): \"%.64s%s\"",
                 length, id, sub, bucketIdx, entryIdx, text,
                 length > 64 ? "..." : "");
        }

        ours.emplace(text, id);
        *out = id;
        return true;
    }

    logf("string table: no sub-table takes %zu bytes of text", length);
    return false;
}

// What the sub-tables look like, and whether an entry can be taken from
// one the way the engine takes one. BG3LE_DUMP_STRINGTABLE=1.
//
// The question this answers is how to get a FixedString for text the game
// does not already hold, which is what a mod needs when it assigns a
// string attribute it built at runtime. The header carries a
// NextFreeIndex, so each sub-table keeps a free list, and taking an entry
// off it is what the engine itself does -- unlike writing into unused
// space, which the engine would later hand to someone else.
//
// Header.Id is the self-check: an entry records the id it is reachable by,
// so a computed id can be proved against the entry it lands on before
// anything is written.
extern "C" void bg3le_fixed_string_dump() {
    const CacheLock lock(string_cache_lock());
    void const* table = bg3le_string_table();
    if (table == nullptr) {
        logf("string table: not located, nothing to dump");
        return;
    }

    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto entrySize =
            read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        const auto perBucket =
            read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        const auto numBuckets =
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        const auto f1100 = read_at<std::uint32_t>(st, 0x1100);
        const auto f1180 = read_at<std::uint64_t>(st, 0x1180);
        auto** buckets =
            read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));

        std::size_t live = 0;
        for (std::uint32_t b = 0; b < numBuckets; ++b) {
            std::uint8_t* bucket = nullptr;
            if (safe_read(&buckets[b], &bucket, sizeof(bucket))
                && bucket != nullptr) {
                ++live;
            }
        }

        logf("string table: sub %zu entrySize %llu perBucket %u buckets %u "
             "(%zu allocated) field_1100 %u field_1180 %#llx", sub,
             (unsigned long long)entrySize, perBucket, numBuckets, live,
             f1100, (unsigned long long)f1180);

        // Is field_1180 the head of the free list? Two readings are
        // plausible -- an entry index, or a pointer to the entry -- and
        // each is checked by whether what it lands on reads as free.
        if (f1180 != 0) {
            const std::uint64_t asIndex = f1180;
            if (asIndex < (std::uint64_t)perBucket * numBuckets) {
                std::uint8_t* bucket = nullptr;
                if (safe_read(&buckets[asIndex / perBucket], &bucket,
                              sizeof(bucket))
                    && bucket != nullptr) {
                    Header h{};
                    if (safe_read(bucket + (asIndex % perBucket) * entrySize,
                                  &h, sizeof(h))) {
                        logf("string table:   as an index it is entry %llu: "
                             "refs %u len %u next %#llx",
                             (unsigned long long)asIndex, h.RefCount,
                             h.Length, (unsigned long long)h.NextFreeIndex);
                    }
                }
            }

            Header h{};
            if (safe_read((void const*)(std::uintptr_t)f1180, &h,
                          sizeof(h))) {
                logf("string table:   as a pointer it reads refs %u len %u "
                     "next %#llx", h.RefCount, h.Length,
                     (unsigned long long)h.NextFreeIndex);
            }
        }

        // One live entry, to confirm Header.Id is the id it is found by.
        for (std::uint32_t b = 0; b < numBuckets && b < 4; ++b) {
            std::uint8_t* bucket = nullptr;
            if (!safe_read(&buckets[b], &bucket, sizeof(bucket))
                || bucket == nullptr) {
                continue;
            }
            for (std::uint32_t e = 0; e < perBucket && e < 4; ++e) {
                Header h{};
                if (!safe_read(bucket + (std::size_t)e * entrySize, &h,
                               sizeof(h))) {
                    continue;
                }
                const std::uint32_t id =
                    (std::uint32_t)sub | (b << 4) | (e << 20);
                char text[64] = {};
                safe_read(bucket + (std::size_t)e * entrySize + sizeof(Header),
                          text, sizeof(text) - 1);
                logf("string table:   id %#x -> hash %#x refs %u len %u "
                     "Header.Id %#x nextFree %#llx \"%.32s\"", id, h.Hash,
                     h.RefCount, h.Length, h.Id,
                     (unsigned long long)h.NextFreeIndex, text);
            }
            break;
        }
    }
}

// FixedString::IncRef for the vendored code, which copies FixedStrings into
// its own maps -- a texture's GUID, a font's name -- and upstream counts each
// copy with the engine. Without that the engine freed entries still in use:
// an icon atlas's texture GUID came back as a different GUID at release, and
// every widget holding it failed to find its texture. The entry is pinned
// instead of counted -- raised to the interning refcount the first time, so
// it is never freed -- because decrementing correctly would mean freeing
// entries the way the engine does. The number of distinct strings the
// vendored code holds bounds what that keeps.
extern "C" void bg3le_fixed_string_pin(std::uint32_t index) {
    if (index == bg3se::FixedStringBase::NullIndex) return;
    static std::atomic<void const*> cached{nullptr};
    void const* table = cached.load(std::memory_order_acquire);
    if (table == nullptr) {
        table = bg3le_string_table();
        if (table == nullptr) return;
        cached.store(table, std::memory_order_release);
    }

    // The engine's own table, read directly: this runs on every copy.
    const std::size_t sub = index & 0x0F;
    if (sub >= kSubTableCount) return;
    void const* st = sub_table(table, sub);
    const std::size_t bucket = (index >> 4) & 0xffff;
    const std::size_t entry = index >> 20;
    const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
    const auto numBuckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
    const auto entrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
    auto** buckets = read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
    if (bucket >= numBuckets || entry >= perBucket || buckets == nullptr) return;
    std::uint8_t* base = buckets[bucket];
    if (base == nullptr) return;

    auto* header = reinterpret_cast<Header*>(base + entry * entrySize);
    auto* refs = reinterpret_cast<std::atomic<std::uint32_t>*>(&header->RefCount);
    if (refs->load(std::memory_order_relaxed) < kInternRefCount) {
        refs->fetch_add(kInternRefCount, std::memory_order_relaxed);
    }
}

// The longest text the engine's table holds, or 0 if not known yet. Each
// sub-table is a size class; past the last one the engine's CreateFromString
// computes a sub-table that does not exist and crashes on what it reads.
extern "C" std::size_t bg3le_fixed_string_max_length() {
    static std::atomic<std::size_t> cached{0};
    const std::size_t known = cached.load(std::memory_order_relaxed);
    if (known != 0) return known;

    void const* table = bg3le_string_table();
    if (table == nullptr) return 0;
    std::size_t longest = 0;
    for (std::size_t i = 0; i < kSubTableCount; ++i) {
        void const* st = sub_table(table, i);
        if (read_at<std::int32_t>(st, offsetof(SubTable, TableIndex)) != (std::int32_t)i) break;
        const auto entrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        if (entrySize > sizeof(Header) + 1) longest = entrySize - sizeof(Header) - 1;
    }
    cached.store(longest, std::memory_order_relaxed);
    return longest;
}

// Tripwire for a stray write into the sub-table headers
// (BG3LE_STRTAB_TRIPWIRE=1): true the first time damage is seen, logged
// with `where`. Legitimate bucket-array moves are logged but not damage.
extern "C" bool bg3le_strtab_check(char const* where) {
    struct Fields {
        std::int32_t TableIndex;
        std::uint64_t EntrySize;
        std::uint32_t EntriesPerBucket;
        std::uint32_t NumBuckets;
        std::uint64_t Buckets;
    };
    static std::mutex mutex;
    static Fields last[16];
    static std::size_t count = 0;
    static bool reported = false;
    static void const* table = nullptr;

    const std::lock_guard<std::mutex> lock(mutex);
    if (reported) return false;
    if (table == nullptr) {
        table = bg3le_string_table();
        if (table == nullptr) return false;
    }

    auto read = [](void const* st) {
        Fields f{};
        f.TableIndex = read_at<std::int32_t>(st, offsetof(SubTable, TableIndex));
        f.EntrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        f.EntriesPerBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        f.NumBuckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        f.Buckets = read_at<std::uint64_t>(st, offsetof(SubTable, Buckets));
        return f;
    };

    if (count == 0) {
        while (count < 16) {
            const Fields f = read(sub_table(table, count));
            if (f.TableIndex != (std::int32_t)count) break;
            last[count++] = f;
        }
        logf("strtab tripwire: armed over %zu sub-tables at %p", count, table);
        return false;
    }

    for (std::size_t i = 0; i < count; ++i) {
        const Fields f = read(sub_table(table, i));
        const Fields& was = last[i];
        const bool pointerOk = f.Buckets >= 0x10000 && f.Buckets < 0x800000000000ull
                               && (f.Buckets & 7) == 0;
        const bool damaged = f.TableIndex != (std::int32_t)i || !pointerOk
                             || f.EntrySize != was.EntrySize
                             || f.EntriesPerBucket != was.EntriesPerBucket
                             || f.NumBuckets > 0x10000 || f.NumBuckets < was.NumBuckets;
        if (damaged) {
            logf("strtab tripwire: sub-table %zu damaged, seen at %s: index %d, entry size %#lx "
                 "(was %#lx), per bucket %u (was %u), buckets %u (was %u), bucket array %#lx "
                 "(was %#lx)",
                 i, where, f.TableIndex, (unsigned long)f.EntrySize,
                 (unsigned long)was.EntrySize, f.EntriesPerBucket, was.EntriesPerBucket,
                 f.NumBuckets, was.NumBuckets, (unsigned long)f.Buckets,
                 (unsigned long)was.Buckets);
            reported = true;
            return true;
        }
        if (f.Buckets != was.Buckets || f.NumBuckets != was.NumBuckets) {
            logf("strtab tripwire: sub-table %zu grew at %s: buckets %u -> %u, array %#lx -> %#lx",
                 i, where, was.NumBuckets, f.NumBuckets, (unsigned long)was.Buckets,
                 (unsigned long)f.Buckets);
        }
        last[i] = f;
    }
    return false;
}

}  // namespace bg3le
