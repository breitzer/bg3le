// Finds the engine's stats manager, which is what Ext.Stats reads.
//
// Unlike the GUID resource manager, RPGStats has no ECS type index to
// fingerprint against -- the symbol table only carries eoc::RPGStatsComponent
// and esv::RPGStatsSystem, which are the ECS component and system, not this.
// What it does have is FixedString TreasureRarities[7], seven consecutive
// string indices whose text is known: Common, Unique, Uncommon, Rare, Epic,
// Legendary, Divine, in the order ItemDataRarity declares them. Seven
// consecutive indices resolving to those seven names in that order is not a
// coincidence, and bg3le already has the string table needed to read them.
//
// The object base is then that address minus the offset of TreasureRarities,
// and the guess is checked rather than trusted: Objects.Values has to hold a
// plausible buffer and a plausible count before the address is accepted. This
// matters because struct offsets in this codebase are not automatically
// portable -- the Linux CRITICAL_SECTION shim already makes our FixedString
// sub-table 0x1208 where Windows has 0x1200 -- so an offset that happens to
// be wrong has to fail loudly rather than return garbage.
//
// Reading a stat's attributes takes four lookups, because the values are
// stored apart from their names:
//
//   Object.ModifierListIndex -> RPGStats.ModifierLists   -> ModifierList
//   ModifierList.Attributes.Values[n]                    -> Modifier
//   Object.IndexedProperties[n]                          -> the raw int32
//   Modifier.EnumerationIndex -> RPGStats.ModifierValueLists -> RPGEnumeration
//
// The enumeration says how to read the int: ConstantInt and ConstantFloat are
// literal, FixedString and Guid are indices into their tables, and anything
// with labels is an enumeration or a flag set. That mapping is name-based in
// upstream too (RPGEnumeration::GetPropertyType compares against known type
// names), so it is reproduced here by comparing the resolved text.
//
// The types are used as declared rather than walked by hand: bg3se's headers
// describe RPGStats, Object, ModifierList, Modifier and RPGEnumeration, and
// CoreLib carries LegacyMap, so once the base address is known every access
// is ordinary member access.

#include <stdafx.h>

#include <GameDefinitions/Stats/Stats.h>
#include <GameDefinitions/Components/All.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <ctime>
#include <vector>

#include "ls_string.h"

#include "../log.h"
#include "cache_lock.h"
#include "../mem.h"
#include "engine_containers.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" void* bg3le_static_get(char const* key, std::size_t which);
extern "C" std::size_t bg3le_static_count(char const* key);
extern "C" void bg3le_static_confirm(char const* key, std::size_t which);
extern "C" bool bg3le_static_record_exact(char const* key,
                                          void const* pointer,
                                          std::uint64_t delta);
extern "C" bool bg3le_static_record(char const* key,
                                    void const* object);
extern "C" bool bg3le_static_record_path(char const* key,
                                         void const* target,
                                         std::uint64_t first_window);
extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace {

using bg3se::stats::Modifier;
using bg3se::stats::ModifierList;
static_assert(offsetof(ModifierList, Name) == 92);
using bg3se::stats::Object;
using bg3se::stats::RPGEnumeration;
using bg3se::stats::RPGStats;

// ItemDataRarity's first seven values, in declaration order, which is the
// order TreasureRarities stores them in.
constexpr char const* kRarities[] = {"Common",    "Unique", "Uncommon",
                                     "Rare",      "Epic",   "Legendary",
                                     "Divine"};
constexpr std::size_t kRarityCount =
    sizeof(kRarities) / sizeof(kRarities[0]);

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// The text of a FixedString, or null. FixedString is a 32-bit index into the
// engine's global string table.
char const* text_of(bg3se::FixedString const& fs) {
    return bg3le_fixed_string(fs.Index, nullptr);
}

extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out);
extern "C" void bg3le_fixed_string_forget_failures();
extern "C" void bg3le_fixed_string_forget_all();
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);

// The seven rarity indices, looked up once by name.
//
// Knowing the values turns the scan into a search for one exact 32-bit
// integer followed by a 28-byte compare. Guessing at plausible indices
// instead -- seven arithmetic tests at every four-byte offset -- took 114
// seconds; this is a plain memory scan.
bool rarity_indices(std::uint32_t* out) {
    for (std::size_t i = 0; i < kRarityCount; ++i) {
        if (!bg3le_fixed_string_index_of(kRarities[i], &out[i])) {
            // Not fatal, and it used to read as though it were: the
            // table is populated as the game loads, and the search runs
            // again later. It succeeds then -- 27,821 stats -- so saying
            // "stays unavailable" was wrong twice over.
            logf("stats: the string table has no entry for \"%s\" yet, so "
                 "the rarity fingerprint cannot be built; will look again "
                 "once the engine has populated it", kRarities[i]);
            return false;
        }
    }
    return true;
}

// Identifies the managers by what they contain, not by where the header says
// they are.
//
// The first attempt trusted offsetof: it found the single rarity run, took
// offsetof(RPGStats, TreasureRarities) off it, and validated the result. That
// was wrong twice over. The offset is 800 in our build and 3648 in the
// engine's, so the struct differs -- and once the offset is unknown, scanning
// 1024 candidate offsets against a loose test (three non-null pointers with
// any size under a million, one resolvable string) accepts a coincidence.
// It did: the search reported success and Objects.size read back as zero.
//
// So nothing here uses a member offset. A CNamedElementManager begins with
// Array<T*>, which is a pointer, a capacity and a size; the stats array is
// the one whose elements are stat objects, and that is checked by resolving
// many of their names rather than one. Ten consecutive resolvable names out
// of a candidate array is not a coincidence.
// Whether seven consecutive indices are the seven rarity indices in some
// order.
bool is_rarity_permutation(std::uint32_t const* v,
                           std::uint32_t const* want) {
    bool seen[kRarityCount] = {};
    for (std::size_t i = 0; i < kRarityCount; ++i) {
        bool matched = false;
        for (std::size_t k = 0; k < kRarityCount; ++k) {
            if (seen[k] || v[i] != want[k]) continue;
            seen[k] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    return true;
}

struct ArrayRef {
    void const* Buffer{nullptr};
    std::uint32_t Size{0};
};

bool array_header_at(void const* at, ArrayRef* out) {
    std::uint32_t capacity = 0;
    if (!read_as((char const*)at + 0, &out->Buffer)) return false;
    if (!read_as((char const*)at + 8, &capacity)) return false;
    if (!read_as((char const*)at + 12, &out->Size)) return false;
    if (out->Buffer == nullptr) return false;
    if (out->Size == 0 || out->Size > capacity) return false;
    if (capacity > 4000000) return false;
    return true;
}

// How many of the first n elements are pointers to something with a
// resolvable, *distinct* FixedString at the given offset.
//
// Distinctness is the whole test. Without it this accepted an array of
// visual resources whose first twelve entries all resolved to
// "DEC_HAR_Fish_Small_A...Mesh_LOD1.1" -- the same pointer repeated -- and
// reported 15754 stats that were nothing of the kind. Any array of pointers
// to named objects passes "the names resolve"; stat names are unique, so
// requiring them to differ rejects the rest.
std::size_t named_elements(ArrayRef const& array, std::size_t nameOffset,
                           std::size_t n) {
    std::uint32_t seen[32];
    std::size_t ok = 0;
    for (std::size_t i = 0; i < n && i < array.Size && ok < 32; ++i) {
        void const* element = nullptr;
        if (!read_as((char const*)array.Buffer + i * sizeof(void*),
                     &element)) {
            break;
        }
        if (element == nullptr) break;
        bg3se::FixedString name{};
        if (!read_as((char const*)element + nameOffset, &name)) break;
        if (text_of(name) == nullptr) break;

        for (std::size_t k = 0; k < ok; ++k) {
            if (seen[k] == name.Index) return ok;   // a repeat: not stats
        }
        seen[ok++] = name.Index;
    }
    return ok;
}

// The stats array, found by content within a window around the rarity run.
//
// Object's own layout is as uncertain as RPGStats', so the offset of its Name
// is searched for too: whichever offset makes ten consecutive elements
// resolve is the right one, and it is logged so the drift is on record.
bool find_objects(unsigned long long runAddr, ArrayRef* out,
                  std::size_t* nameOffsetOut, void const** headerOut) {
    constexpr std::size_t kWindow = 16384;   // either side of the run
    constexpr std::size_t kProbe = 10;       // elements that must resolve
    constexpr std::size_t kMaxNameOffset = 128;

    const unsigned long long lo =
        runAddr > kWindow ? runAddr - kWindow : 0;
    const unsigned long long hi = runAddr + kWindow;

    // Diagnostic: what array-like headers are actually near the run. Logged
    // because guessing which constraint is too strict wastes a run each time.
    std::size_t headers = 0;
    std::size_t biggest = 0;
    for (unsigned long long at = lo; at + 16 <= hi; at += 8) {
        ArrayRef probe{};
        if (array_header_at((void const*)at, &probe)) {
            ++headers;
            if (probe.Size > biggest) biggest = probe.Size;
        }
    }
    logf("stats: %zu array-like headers within %zu bytes of the run, largest "
         "%zu entries", headers, (std::size_t)kWindow, biggest);

    for (unsigned long long at = lo; at + 16 <= hi; at += 8) {
        ArrayRef array{};
        if (!array_header_at((void const*)at, &array)) continue;
        // BG3 ships thousands of stats; a smaller array is something else.
        if (array.Size < 1000) continue;

        for (std::size_t nameOff = 0; nameOff <= kMaxNameOffset;
             nameOff += 4) {
            if (named_elements(array, nameOff, kProbe) < kProbe) continue;
            *out = array;
            *nameOffsetOut = nameOff;
            *headerOut = (void const*)at;
            logf("stats: stats array at %#llx, %u entries, Object::Name at "
                 "+%zu (header says +%zu)", at, array.Size, nameOff,
                 (std::size_t)offsetof(Object, Name));
            return true;
        }
    }
    return false;
}

// What the search establishes. No struct offsets survive into this: every
// one of these was derived by looking at the memory, because RPGStats' own
// offsets are wrong for this build (TreasureRarities at 800 here, 3648 in the
// engine).
struct Found {
    ArrayRef Objects{};
    // Where the Objects array's header lives. The engine grows and rebuilds
    // the array while mods load, so the buffer and size are re-read from here
    // rather than kept from the moment it was found.
    void const* ObjectsHeader{nullptr};
    std::size_t NameOffset{0};          // Object::Name

    ArrayRef Lists{};                   // RPGStats::ModifierLists
    std::size_t ListNameOffset{0};      // ModifierList::Name
    std::size_t ModifierNameOffset{0};  // Modifier::Name
    std::size_t AttrsOffset{0};         // ModifierList::Attributes

    ArrayRef ValueLists{};              // RPGStats::ModifierValueLists
    std::size_t ValueNameOffset{0};     // RPGEnumeration::Name

    std::size_t PropsOffset{0};         // Object::IndexedProperties
    std::size_t ListIndexOffset{0};     // Object::ModifierListIndex
    ArrayRef Strings{};                 // RPGStats::FixedStrings
    ArrayRef Floats{};                  // RPGStats::Floats
    ArrayRef Guids{};                   // RPGStats::GUIDs
    ArrayRef Int64s{};                  // RPGStats::Int64s
    ArrayRef TranslatedStrings{};       // RPGStats::TranslatedStrings
    ArrayRef Conditions{};              // RPGStats::Conditions
    // Where that array's header lives, which reading never needed: an
    // attribute write has to see the capacity and hand out a slot from it.
    void const* ConditionsHeader{nullptr};
    void const* StringsHeader{nullptr};     // likewise, for FixedStrings
    void const* Int64sHeader{nullptr};      // and for Int64s
    void const* FloatsHeader{nullptr};      // and for Floats
    void const* GuidsHeader{nullptr};       // and for GUIDs
    void const* TranslatedHeader{nullptr};  // and for TranslatedStrings
    bool Attributes{false};             // whether all of the above landed
    void const* First{nullptr};         // Objects[0], to notice a rebuild
    std::uint32_t ModuleLoads{0};       // g_module_loads when found
};

Found& state() {
    static Found f;
    return f;
}

// Bumped whenever the engine rebuilds its stats, which it does when a save's
// module list differs from the load order. Every cache keyed by an engine
// address or pool slot clears on it: a rebuilt modifier list can land where
// another list was, and a stale name map then says Conditions is not an
// attribute of an interrupt.
std::uint32_t g_generation = 0;

// Module loads finished, from the game state (bg3le_stats_module_loaded): the
// engine rebuilds its stats then, and can reuse the same allocations, so
// Objects[0] moving does not show every rebuild.
std::atomic<std::uint32_t> g_module_loads{0};

template <class F>
void clear_on_rebuild(std::uint32_t& seen, F clear) {
    if (seen != g_generation) {
        clear();
        seen = g_generation;
    }
}

// A pointer array containing an element with a specific name.
//
// This is the test that cannot be faked. The modifier value lists must
// contain an entry called "ConstantInt", because that is the type name the
// engine compares against when deciding how to read an attribute; the
// modifier lists must contain one called "Weapon". An array of unrelated
// named objects will not.
bool array_contains_name(ArrayRef const& array, std::size_t nameOffset,
                         char const* wanted, std::size_t limit) {
    for (std::size_t i = 0; i < array.Size && i < limit; ++i) {
        void const* element = nullptr;
        if (!read_as((char const*)array.Buffer + i * sizeof(void*),
                     &element)) {
            return false;
        }
        if (element == nullptr) continue;
        bg3se::FixedString name{};
        if (!read_as((char const*)element + nameOffset, &name)) continue;
        char const* text = text_of(name);
        if (text != nullptr && std::strcmp(text, wanted) == 0) return true;
    }
    return false;
}

// Finds a pointer array near the run whose elements carry a given name at
// some offset, identified by a name it must contain.
bool find_named_array(unsigned long long runAddr, char const* mustContain,
                      std::size_t minSize, std::size_t maxSize,
                      std::size_t maxNameOffset, ArrayRef* out,
                      std::size_t* nameOffsetOut, char const* what) {
    constexpr std::size_t kWindow = 16384;
    const unsigned long long lo = runAddr > kWindow ? runAddr - kWindow : 0;
    const unsigned long long hi = runAddr + kWindow;

    for (unsigned long long at = lo; at + 16 <= hi; at += 8) {
        ArrayRef array{};
        if (!array_header_at((void const*)at, &array)) continue;
        if (array.Size < minSize || array.Size > maxSize) continue;

        for (std::size_t nameOff = 0; nameOff <= maxNameOffset;
             nameOff += 4) {
            // Distinct names first, for the same reason as the stats array.
            if (named_elements(array, nameOff, 8) < 8) continue;
            if (!array_contains_name(array, nameOff, mustContain,
                                     array.Size)) {
                continue;
            }
            *out = array;
            *nameOffsetOut = nameOff;
            logf("stats: %s at %#llx, %u entries, name at +%zu (contains "
                 "\"%s\")", what, at, array.Size, nameOff, mustContain);
            return true;
        }
    }
    logf("stats: no %s near the run (wanted an array of %zu-%zu entries "
         "containing \"%s\")", what, minSize, maxSize, mustContain);
    return false;
}

// Modifier::Name, found by requiring a list's attributes to have distinct
// resolvable names.
bool find_modifier_name_offset(ArrayRef const& lists, std::size_t* out,
                               std::size_t* attrsOffsetOut) {
    // The attribute array's offset is searched for, not assumed. Assuming
    // zero failed: a ModifierList starts with a vtable pointer, which our
    // header does not declare, so the array actually begins at +8. The
    // reconstructed layout agrees with the name landing at +92 --
    // VMT(8) + Array(16) + HashMap(64) + int32(4).
    for (std::size_t i = 0; i < lists.Size && i < 16; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)lists.Buffer + i * sizeof(void*), &list)) {
            continue;
        }
        if (list == nullptr) continue;

        for (std::size_t attrsOff = 0; attrsOff <= 32; attrsOff += 8) {
            ArrayRef attrs{};
            if (!array_header_at((char const*)list + attrsOff, &attrs)) {
                continue;
            }
            if (attrs.Size < 8) continue;

            for (std::size_t off = 0; off <= 64; off += 4) {
                if (named_elements(attrs, off, 8) < 8) continue;
                *out = off;
                *attrsOffsetOut = attrsOff;
                logf("stats: Modifier::Name at +%zu, ModifierList::Attributes "
                     "at +%zu (%u attributes on list %zu)", off, attrsOff,
                     attrs.Size, i);
                return true;
            }
        }
    }
    // Diagnostic: what the first list's attribute array actually looks like.
    for (std::size_t i = 0; i < lists.Size && i < 2; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)lists.Buffer + i * sizeof(void*), &list)
            || list == nullptr) {
            continue;
        }
        ArrayRef attrs{};
        const bool header = array_header_at(list, &attrs);
        logf("stats:   list %zu at %p: header=%d size=%u buffer=%p", i, list,
             header ? 1 : 0, header ? attrs.Size : 0,
             header ? attrs.Buffer : nullptr);
        if (!header) {
            std::uint64_t words[4] = {};
            if (read_as(list, &words)) {
                logf("stats:     first words %#lx %#lx %#lx %#lx", words[0],
                     words[1], words[2], words[3]);
            }
            continue;
        }
        void const* first = nullptr;
        if (read_as(attrs.Buffer, &first) && first != nullptr) {
            std::uint32_t w[8] = {};
            if (read_as(first, &w)) {
                logf("stats:     modifier[0] at %p: %u %u %u %u %u %u %u %u",
                     first, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }
    logf("stats: could not place Modifier::Name; attributes stay unavailable");
    return false;
}

// The value lists upstream treats as flag sets rather than plain
// enumerations (RPGEnumeration::IsFlagType). A flags value is an index into
// the Int64s pool whose contents are a bitmask, not a label index, which is
// why an exact label lookup finds nothing for them.
// RPGEnumeration::IsFlagType, copied whole. Seven of these were missing,
// which made StatusGroups report the raw int 1564 instead of
// {SG_Light, SG_Surface} and AuraFlags report the label "None" instead of
// an empty set.
constexpr char const* kFlagTypes[] = {
    "AttributeFlags",      "WeaponFlags",       "ResistanceFlags",
    "PassiveFlags",        "SpellFlagList",     "StatusEvent",
    "StatusPropertyFlags", "ProficiencyGroupFlags",
    "CinematicArenaFlags", "LineOfSightFlags",  "SpellCategoryFlags",
    "StatsFunctorContext", "StatusGroupFlags",  "InterruptContext",
    "InterruptContextScope", "InterruptDefaultValue",
    "InterruptFlagsList",  "AuraFlags",         "AbilityFlags"};
constexpr std::size_t kFlagTypeCount =
    sizeof(kFlagTypes) / sizeof(kFlagTypes[0]);

bool is_flag_type(char const* name) {
    if (name == nullptr) return false;
    for (std::size_t i = 0; i < kFlagTypeCount; ++i) {
        if (std::strcmp(name, kFlagTypes[i]) == 0) return true;
    }
    return false;
}

// Strings the value pool must contain, and a list of names cannot.
constexpr char const* kDiceStrings[] = {"1d8", "1d6", "1d10", "2d6"};
constexpr std::size_t kDiceCount =
    sizeof(kDiceStrings) / sizeof(kDiceStrings[0]);

// Whether a pool of FixedString indices holds any of the given strings.
bool pool_contains_any(ArrayRef const& pool, char const* const* wanted,
                       std::size_t count) {
    std::uint32_t ids[8];
    std::size_t n = 0;
    for (std::size_t i = 0; i < count && n < 8; ++i) {
        std::uint32_t id = 0;
        if (bg3le_fixed_string_index_of(wanted[i], &id)) ids[n++] = id;
    }
    if (n == 0) {
        static bool said = false;
        if (!said) {
            said = true;
            logf("stats: none of the dice strings are in the string table, so "
                 "the value pool cannot be identified that way");
        }
        return false;
    }

    const std::size_t limit = pool.Size < 200000 ? pool.Size : 200000;
    for (std::size_t i = 0; i < limit; ++i) {
        std::uint32_t entry = 0;
        if (!read_as((char const*)pool.Buffer + i * sizeof(std::uint32_t),
                     &entry)) {
            return false;
        }
        for (std::size_t k = 0; k < n; ++k) {
            if (entry == ids[k]) return true;
        }
    }
    return false;
}

// RPGStats::FixedStrings, the pool a FixedString attribute indexes into.
//
// An attribute's raw int is not a global string index. It is a position in
// this pool, which is why "Damage" read back as 2303 -- decoded as a string
// index its sub-table nibble is 15, and only 11 exist -- and why every unset
// attribute resolved to "Version64", the string at index 0.
//
// The header puts FixedStrings immediately after TreasureRarities, so the
// rarity run locates it: a CompactSet of uint32 string indices that resolve.
bool find_string_pool(unsigned long long runAddr, ArrayRef* out,
                      unsigned long long* poolAddrOut) {
    // Both directions, and wide. Searching only forwards found nothing: the
    // header has FixedStrings just after TreasureRarities, but the engine's
    // member order plainly differs -- the modifier lists sit *below* the run
    // in memory, not above it.
    constexpr std::size_t kWindow = 16384;
    constexpr std::size_t kProbe = 8;
    const unsigned long long lo = runAddr > kWindow ? runAddr - kWindow : 0;
    const unsigned long long hi = runAddr + kWindow;

    for (unsigned long long at = lo; at + 16 <= hi; at += 4) {
        ArrayRef pool{};
        if (!array_header_at((void const*)at, &pool)) continue;
        // The pool holds every string the stats files mention, so it is
        // large; a small array of resolvable indices is something else.
        if (pool.Size < 256) continue;

        // No structural pre-filter. Requiring the first entries to resolve
        // and differ rejected the real pool on its first run -- a pool may
        // hold empty or repeated slots -- while happily accepting the stat
        // name array, which has neither. The dice test below is the only
        // thing that actually distinguishes them, so it decides alone.

        // Distinctness is not enough here. The Objects manager keeps a
        // NameToHandle map whose key array is 15754 resolvable, distinct
        // string indices -- the stat names -- and matching it made every
        // attribute read back as a stat name ("Damage" came out as
        // "Interrupt_BardicInspiration_SavingThrow_d8", unset ones as
        // "Target_MainHandAttack", which is stat zero).
        //
        // So the test is positive rather than structural: the value pool has
        // to contain dice notation, because that is what weapon damage is
        // written as. A list of stat names does not.
        if (!pool_contains_any(pool, kDiceStrings, kDiceCount)) {
            logf("stats:   candidate pool at %#llx rejected: %u resolvable "
                 "entries but no dice notation", at, pool.Size);
            continue;
        }

        *out = pool;
        *poolAddrOut = at;
        logf("stats: string pool at %#llx (%+lld from the run), %u entries",
             at, (long long)(at - runAddr), pool.Size);
        return true;
    }
    logf("stats: no string pool found after the rarity run; FixedString "
         "attributes will report their raw index");
    return false;
}

// Whether an array reads as RPGStats::TranslatedStrings: sixteen-byte
// entries opening with a FixedString that resolves to a loca handle, which
// is an "h" followed by thirty-six hex-and-g characters.
bool plausible_handles(ArrayRef const& a) {
    if (a.Size < 4) return false;
    std::size_t good = 0;
    std::size_t checked = 0;
    for (std::size_t i = 0; i < 16 && i < a.Size; ++i) {
        std::uint32_t index = 0;
        if (!read_as((char const*)a.Buffer + i * 16, &index)) return false;
        ++checked;
        char const* text = bg3le_fixed_string(index, nullptr);
        if (text != nullptr && text[0] == 'h' && std::strlen(text) == 37) {
            ++good;
        }
    }
    return checked > 0 && good * 2 >= checked;
}

// RPGStats::Conditions, an array of Larian strings holding the condition
// expressions that Conditions, TargetConditions and UseConditions index
// into.
//
// It sits far past the pools -- several maps and a lock later -- and none of
// those have a size that can be confirmed from the headers, so it is found
// by content instead of position: an array whose entries read as strings
// that look like conditions, "Character() and Enemy() and not Dead()".
void find_conditions(unsigned long long poolAddr, Found* f) {
    auto plausible_conditions = [](ArrayRef const& a) {
        if (a.Size < 16) return false;
        std::size_t calls = 0;
        std::size_t readable = 0;
        for (std::size_t i = 0; i < 24 && i < a.Size; ++i) {
            std::string text;
            if (!read_ls_string((char const*)a.Buffer + i * 16, &text)) {
                return false;
            }
            ++readable;
            // A condition is a boolean expression over predicates, so the
            // overwhelming majority carry a call.
            if (text.find('(') != std::string::npos
                && text.find(')') != std::string::npos) {
                ++calls;
            }
        }
        return readable >= 16 && calls * 2 >= readable;
    };

    // A bounded walk: the member order puts Conditions after the pools, and
    // a window rather than a whole-memory scan keeps a wrong match from
    // being possible at all.
    constexpr std::size_t kWindow = 4096;
    for (std::size_t off = 64; off <= kWindow; off += 8) {
        ArrayRef candidate{};
        if (!array_header_at((void const*)(poolAddr + off), &candidate)) {
            continue;
        }
        if (!plausible_conditions(candidate)) continue;

        f->Conditions = candidate;
        f->ConditionsHeader = (void const*)(poolAddr + off);

        // The capacity is quoted because whether a mod can add a condition
        // depends on it: an expression it builds at runtime is not in the
        // pool, and the only place to put one without taking ownership of
        // the array away from the engine is the slack past the end.
        std::uint32_t capacity = 0;
        read_as((char const*)f->ConditionsHeader + 8, &capacity);
        logf("stats: condition pool at pool+%zu, %u entries, %u capacity "
             "(%u spare)", off, candidate.Size, capacity,
             capacity > candidate.Size ? capacity - candidate.Size : 0);
        return;
    }
    logf("stats: no condition pool found; Conditions attributes report "
         "nothing");
}

// RPGStats::Floats and RPGStats::GUIDs, the pools the other indexed
// attribute kinds point into.
//
// The header's member order held for the string pool -- it landed exactly
// where TreasureRarities plus its padding predicted -- so the same order is
// used here: FixedStrings, Int64s, GUIDs, Floats. Each is a 16-byte array
// header, so the candidates are a short walk forward, and each is checked
// against what its contents should look like rather than accepted on
// position alone.
void find_value_pools(unsigned long long poolAddr, Found* f) {
    // Floats: finite, and not a block of zeroes or garbage exponents.
    auto plausible_floats = [](ArrayRef const& a) {
        if (a.Size < 8) return false;
        std::size_t sane = 0;
        std::size_t nonzero = 0;
        for (std::size_t i = 0; i < 16 && i < a.Size; ++i) {
            float v = 0.0f;
            if (!read_as((char const*)a.Buffer + i * sizeof(float), &v)) {
                return false;
            }
            const float mag = v < 0 ? -v : v;
            if (v == v && mag < 1e9f) ++sane;      // v == v rejects NaN
            if (v != 0.0f) ++nonzero;
        }
        return sane >= 16 && nonzero >= 2;
    };

    // Guids: 16 bytes each, and a real one is not all zeroes.
    auto plausible_guids = [](ArrayRef const& a) {
        if (a.Size < 4) return false;
        std::size_t nonzero = 0;
        for (std::size_t i = 0; i < 8 && i < a.Size; ++i) {
            std::uint64_t w[2] = {};
            if (!read_as((char const*)a.Buffer + i * 16, &w)) return false;
            if (w[0] != 0 || w[1] != 0) ++nonzero;
        }
        return nonzero >= 4;
    };

    // At the positions the member order predicts, not the first thing that
    // passes. Scanning forward for "the first plausible guid array" picked
    // pool+16, which is Int64s: an array of pointers read sixteen bytes at a
    // time looks exactly like non-zero guids. The order is
    // FixedStrings, Int64s, GUIDs, Floats, and floats landing at +48 on the
    // first run is what confirms it.
    // Int64s comes first in the member order, and it is what flag values
      // index into -- upstream's Object::GetFlags reads
      // GetStats()->GetInt64(index) and treats the result as a bitmask.
    constexpr std::size_t kInt64sAt = 16;
    constexpr std::size_t kGuidsAt = 32;
    constexpr std::size_t kFloatsAt = 48;

    ArrayRef int64s{};
    if (array_header_at((void const*)(poolAddr + kInt64sAt), &int64s)
        && int64s.Size > 0) {
        f->Int64s = int64s;
        f->Int64sHeader = (void const*)(poolAddr + kInt64sAt);
        logf("stats: int64 pool at pool+%zu, %u entries", kInt64sAt,
             int64s.Size);
    }

    ArrayRef guids{};
    if (array_header_at((void const*)(poolAddr + kGuidsAt), &guids)
        && plausible_guids(guids)) {
        f->Guids = guids;
        f->GuidsHeader = (void const*)(poolAddr + kGuidsAt);
        logf("stats: guid pool at pool+%zu, %u entries", kGuidsAt,
             guids.Size);
    }

    ArrayRef floats{};
    if (array_header_at((void const*)(poolAddr + kFloatsAt), &floats)
        && plausible_floats(floats)) {
        f->Floats = floats;
        f->FloatsHeader = (void const*)(poolAddr + kFloatsAt);
        logf("stats: float pool at pool+%zu, %u entries", kFloatsAt,
             floats.Size);
    }
    if (f->Floats.Buffer == nullptr) {
        logf("stats: no float pool found; Float attributes report their "
             "pool index");
    }
    if (f->Guids.Buffer == nullptr) {
        logf("stats: no guid pool found; GUID attributes report their "
             "pool index");
    }

    // TranslatedStrings follows Floats in the member order. An entry is a
    // RuntimeStringHandle pair -- sixteen bytes, the handle's FixedString
    // first -- and a handle resolves to text like
    // "h5fafec24g30d5g425cg952cga9c53752059c", which is what confirms it
    // rather than the position.
    constexpr std::size_t kTranslatedAt = 64;
    ArrayRef translated{};
    if (array_header_at((void const*)(poolAddr + kTranslatedAt), &translated)
        && plausible_handles(translated)) {
        f->TranslatedStrings = translated;
        f->TranslatedHeader = (void const*)(poolAddr + kTranslatedAt);
        logf("stats: translated string pool at pool+%zu, %u entries",
             kTranslatedAt, translated.Size);
    } else {
        logf("stats: no translated string pool found; TranslatedString "
             "attributes report nothing");
    }

    find_conditions(poolAddr, f);
}

// Object::IndexedProperties and Object::ModifierListIndex, derived from the
// one relationship that has to hold: an object's value count equals the
// number of attributes in its modifier list.
//
// This is self-validating, which matters because these two offsets sit after
// members whose size cannot be confirmed from the headers. A pair of offsets
// that agrees across many objects is right; nothing else would.
bool find_object_offsets(Found const& f, std::size_t* propsOut,
                         std::size_t* indexOut) {
    constexpr std::size_t kSamples = 24;
    constexpr std::size_t kMaxProps = 64;
    constexpr std::size_t kMaxIndex = 512;

    // Vector<int32_t> is a begin/end pair, so the count is the byte span
    // divided by four.
    auto vector_count = [](void const* at, std::size_t* countOut) {
        void const* begin = nullptr;
        void const* end = nullptr;
        if (!read_as((char const*)at + 0, &begin)) return false;
        if (!read_as((char const*)at + 8, &end)) return false;
        if (begin == nullptr || end < begin) return false;
        const std::size_t bytes =
            (std::size_t)((char const*)end - (char const*)begin);
        if (bytes % 4 != 0 || bytes > (1u << 20)) return false;
        *countOut = bytes / 4;
        return true;
    };

    auto list_attr_count = [&f](std::uint32_t listIndex,
                                std::size_t* countOut) {
        if (listIndex >= f.Lists.Size) return false;
        void const* list = nullptr;
        if (!read_as((char const*)f.Lists.Buffer + listIndex * sizeof(void*),
                     &list)) {
            return false;
        }
        if (list == nullptr) return false;
        ArrayRef attrs{};
        if (!array_header_at((char const*)list + f.AttrsOffset, &attrs)) {
            return false;
        }
        *countOut = attrs.Size;
        return true;
    };

    for (std::size_t props = 0; props <= kMaxProps; props += 8) {
        for (std::size_t idx = 0; idx <= kMaxIndex; idx += 4) {
            std::size_t agreed = 0;
            std::size_t tried = 0;

            for (std::size_t i = 0; i < f.Objects.Size && tried < kSamples;
                 ++i) {
                void const* obj = nullptr;
                if (!read_as((char const*)f.Objects.Buffer + i * sizeof(void*),
                             &obj)) {
                    break;
                }
                if (obj == nullptr) continue;
                ++tried;

                std::size_t valueCount = 0;
                if (!vector_count((char const*)obj + props, &valueCount)) {
                    break;
                }
                std::uint32_t listIndex = 0;
                if (!read_as((char const*)obj + idx, &listIndex)) break;

                std::size_t attrCount = 0;
                if (!list_attr_count(listIndex, &attrCount)) break;
                if (valueCount != attrCount || valueCount == 0) break;
                ++agreed;
            }

            if (tried >= kSamples && agreed == tried) {
                *propsOut = props;
                *indexOut = idx;
                logf("stats: Object::IndexedProperties at +%zu, "
                     "ModifierListIndex at +%zu (agreed on %zu objects)",
                     props, idx, agreed);
                return true;
            }
        }
    }
    logf("stats: no offsets made an object's value count match its modifier "
         "list's attribute count; attributes stay unavailable");
    return false;
}

// Everything Ext.Stats needs, from the address of the treasure rarity run.
//
// Split out so it can be driven either by the scan below or by the
// pointer a previous run recorded -- see src/vendor/statics.cpp. The
// validation is the same either way, so a stale cache fails here rather
// than being believed.
bool build_from_run(unsigned long long run) {
    ArrayRef objects{};
    std::size_t nameOffset = 0;
    void const* header = nullptr;
    if (!find_objects(run, &objects, &nameOffset, &header)) return false;

    Found& f = state();
    f.Objects = objects;
    f.ObjectsHeader = header;
    f.NameOffset = nameOffset;
    logf("stats: %u stats via the rarity run at %#llx", objects.Size, run);

    // Attributes need three more things, each identified by content. Any
    // of them missing leaves enumeration working and attributes reporting
    // themselves unavailable.
    const bool lists = find_named_array(
        run, "Weapon", 4, 4096, 128, &f.Lists, &f.ListNameOffset,
        "modifier lists");
    unsigned long long poolAddr = 0;
    if (find_string_pool(run, &f.Strings, &poolAddr)) {
        f.StringsHeader = (void const*)poolAddr;
        find_value_pools(poolAddr, &f);
    }
    const bool values = find_named_array(
        run, "ConstantInt", 4, 65536, 32, &f.ValueLists, &f.ValueNameOffset,
        "modifier value lists");
    bool offsets = false;
    if (lists) {
        offsets = find_modifier_name_offset(f.Lists, &f.ModifierNameOffset,
                                            &f.AttrsOffset)
                  && find_object_offsets(f, &f.PropsOffset,
                                         &f.ListIndexOffset);
    }
    f.Attributes = lists && values && offsets;
    logf("stats: attributes %s", f.Attributes ? "available" : "unavailable");
    return true;
}

bool search_for_stats() {
    // The pointer a previous run recorded, if this build has been seen
    // before: no scan, and available the moment the engine has filled the
    // static in. The rarity permutation is checked first, so a cache that
    // no longer means anything is discarded rather than trusted.
    std::uint32_t want[kRarityCount] = {};
    const std::size_t candidates = bg3le_static_count("stats.rarities");
    if (candidates > 0 && rarity_indices(want)) {
        for (std::size_t i = 0; i < candidates; ++i) {
            void* cached = bg3le_static_get("stats.rarities", i);
            if (cached == nullptr) continue;

            std::uint32_t at[kRarityCount] = {};
            if (!safe_read(cached, at, sizeof(at))) continue;
            if (!is_rarity_permutation(at, want)) continue;
            if (!build_from_run((unsigned long long)(std::uintptr_t)cached)) {
                continue;
            }

            logf("stats: found without scanning, from recorded static %zu "
                 "of %zu", i + 1, candidates);
            bg3le_static_confirm("stats.rarities", i);
            return true;
        }
        logf("stats: none of the %zu recorded statics holds the rarity run; "
             "scanning", candidates);
    }

    if (bg3le_fixed_string(1, nullptr) == nullptr) {
        logf("stats: the string table is not available, so the rarity "
             "fingerprint cannot be read; Ext.Stats stays unavailable");
        return false;
    }

    if (!rarity_indices(want)) return false;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kRunBytes = kRarityCount * sizeof(std::uint32_t);
    constexpr std::size_t kChunk = 1u << 20;

    static std::vector<unsigned char> block;
    block.resize(kChunk + kRunBytes);

    char line[512];
    std::size_t regions = 0;
    std::size_t scanned = 0;
    std::size_t hits = 0;

    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;
        ++regions;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t span = (std::size_t)(to - base);
            if (span > kChunk + kRunBytes) span = kChunk + kRunBytes;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), span);
            if (got < kRunBytes) continue;
            scan_yield();
            scanned += got;

            const std::size_t last = got - kRunBytes;
            for (std::size_t off = 0; off <= last; off += 4) {
                auto const* v = (std::uint32_t const*)(block.data() + off);
                // Any ordering, not the enum's.
                //
                // Requiring declaration order found a run reliably -- and it
                // was the ItemDataRarity label table, which stores them in
                // exactly that order. Around it sat 173 arrays whose largest
                // held 101 entries, nothing like the thousands of stats.
                // TreasureRarities has no reason to use the enum's order, so
                // the test is now set membership.
                if (!is_rarity_permutation(v, want)) continue;
                ++hits;

                if (!build_from_run(base + off)) continue;
                std::fclose(maps);
                logf("stats: found by scanning %zu bytes over %zu regions",
                     scanned, regions);
                // Recorded so the next run reads the engine's own pointer
                // instead of scanning for it.
                //
                // The base is computable rather than guessed: the run is
                // RPGStats::TreasureRarities, and this build puts it 3,648
                // bytes into the object -- established when the stats
                // search was written, and the reason none of bg3se's own
                // member offsets could be used directly. So a static
                // holding that address exactly is the pointer the engine
                // keeps, with no window and no ambiguity.
                constexpr std::uint64_t kRaritiesInStats = 3648;
                const unsigned long long run = base + off;
                if (run > kRaritiesInStats
                    && !bg3le_static_record_exact(
                           "stats.rarities",
                           (void const*)(run - kRaritiesInStats),
                           kRaritiesInStats)) {
                    // No exact match, so either the engine keeps no
                    // static pointer to RPGStats or that member offset is
                    // wrong for this build. Either way, walk backwards
                    // from the run itself until a static is reached.
                    //
                    // The windowed search is not worth trying here: it
                    // found 48 statics within a megabyte below RPGStats
                    // and every one was a neighbour in the same arena,
                    // which moved on the next run.
                    constexpr std::uint64_t kManagerWindow = 1u << 16;
                    bg3le_static_record_path("stats.rarities",
                                             (void const*)run,
                                             kManagerWindow);
                }
                return true;
            }
        }
    }

    std::fclose(maps);
    if (hits != 0) {
        logf("stats: found %zu rarity runs but no stats array near any of "
             "them; Ext.Stats stays unavailable", hits);
    } else {
        logf("stats: no rarity run found (scanned %zu bytes over %zu "
             "regions); the stats may not be parsed yet", scanned, regions);
    }
    return false;
}

// Caches success permanently and failure only briefly.
//
// Caching a failure forever would be wrong: the stats are parsed during load,
// so a search that runs before that finishes fails for a reason that stops
// being true -- the first attempt in practice fails on "Epic" not yet being
// in the string table. Retrying without a cooldown would be worse, since
// every call would rescan memory.
// Re-reads the Objects array from its header, at most every 100 ms unless
// forced: 27,821 entries were seen mid-load where the finished array holds
// fewer, and the stale tail pointed at objects the engine had let go.
// True when it re-read.
bool refresh_objects(bool force) {
    Found& f = state();
    if (f.ObjectsHeader == nullptr) return false;
    static std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (!force && now - last < std::chrono::milliseconds(100)) return false;
    last = now;
    ArrayRef fresh{};
    if (array_header_at(f.ObjectsHeader, &fresh)) f.Objects = fresh;
    return true;
}

void const* first_object(Found const& f) {
    void const* first = nullptr;
    if (f.Objects.Buffer == nullptr || f.Objects.Size == 0) return nullptr;
    read_as(f.Objects.Buffer, &first);
    return first;
}

bool ready() {
    Found& f = state();
    static std::time_t lastAttempt = 0;
    if (f.Objects.Buffer != nullptr && f.ModuleLoads != g_module_loads.load()) {
        logf("stats: the engine reloaded its modules; finding the stats again");
        ++g_generation;
        f = Found{};
        lastAttempt = 0;
    }
    if (f.Objects.Buffer != nullptr) {
        if (!refresh_objects(false)) return true;
        // A rebuild re-creates every object, so the first one moves; bg3le's
        // own Create only appends.
        void const* first = first_object(f);
        if (f.First == nullptr) f.First = first;
        if (first == nullptr || first == f.First) return true;
        logf("stats: the engine rebuilt its stats; finding them again");
        ++g_generation;
        f = Found{};
        lastAttempt = 0;
    }

    // A module load finishing ends the cooldown: a search during the load fails.
    static std::uint32_t attemptLoads = 0;
    const std::time_t now = std::time(nullptr);
    if (lastAttempt != 0 && now - lastAttempt < 10
        && attemptLoads == g_module_loads.load()) {
        return false;
    }
    lastAttempt = now;
    attemptLoads = g_module_loads.load();

    const auto started = std::clock();
    const bool ok = search_for_stats();
    const double ms =
        1000.0 * (double)(std::clock() - started) / (double)CLOCKS_PER_SEC;
    logf("stats: search took %.0f ms", ms);
    if (ok) {
        state().First = first_object(state());
        state().ModuleLoads = g_module_loads.load();
    }
    return ok;
}

void const* object_at(std::size_t index) {
    if (!ready()) return nullptr;
    Found const& f = state();
    if (index >= f.Objects.Size) return nullptr;
    void const* element = nullptr;
    if (!read_as((char const*)f.Objects.Buffer + index * sizeof(void*),
                 &element)) {
        return nullptr;
    }
    return element;
}

// Object's own members, past the indexed properties.
//
// The member order gives these directly once the container sizes are known:
// StaticArray and Array are both sixteen bytes here, so HashSet is
// forty-eight and HashMap sixty-four. Counting from Name at +32 puts
// ModifierListIndex at +228, which is exactly where it was derived
// independently -- that agreement is what says the rest of the walk is
// right.
//
//   +40  Functors        HashMap<FixedString, Array<FunctorGroup>>
//   +104 RollConditions  HashMap<FixedString, Array<RollCondition>>
//   +168 AIFlags         FixedString
//   +176 Requirements    Array<Requirement>
constexpr std::size_t kObjectRollConditions = 104;
constexpr std::size_t kObjectAIFlags = 168;
constexpr std::size_t kObjectRequirements = 176;
static_assert(offsetof(bg3se::stats::Object, Requirements) == kObjectRequirements,
              "Object::Requirements where the live walk put it");

// The walk is only trusted while it still agrees with the build: where it
// puts ModifierListIndex against where bg3le found it at runtime. A field of
// the walk, or null when they differ (which read_as and write_bytes refuse).
constexpr std::size_t kObjectListIndexByWalk = 228;
char* walked(void const* object, std::size_t offset) {
    if (object == nullptr || state().ListIndexOffset != kObjectListIndexByWalk) return nullptr;
    return (char*)object + offset;
}

// A bg3se HashMap: HashKeys, NextIds, Keys, then Values. Looking a key up
// means walking the buckets upstream, but Keys and Values are parallel
// arrays, so a linear walk finds the same entry without reimplementing the
// hash -- and these maps hold a handful of entries.
struct HashMapRef {
    void const* Keys{nullptr};
    void const* Values{nullptr};
    std::uint32_t Count{0};
};

bool read_hash_map(void const* at, HashMapRef* out) {
    constexpr std::size_t kKeysBuffer = 32;
    constexpr std::size_t kKeysSize = 44;
    constexpr std::size_t kValuesBuffer = 48;

    void const* keys = nullptr;
    void const* values = nullptr;
    std::uint32_t count = 0;
    if (!read_as((char const*)at + kKeysBuffer, &keys)
        || !read_as((char const*)at + kKeysSize, &count)
        || !read_as((char const*)at + kValuesBuffer, &values)) {
        return false;
    }
    if (count > 4096) return false;
    if (count != 0 && (keys == nullptr || values == nullptr)) return false;

    out->Keys = keys;
    out->Values = values;
    out->Count = count;
    return true;
}

// The slot in a map keyed by attribute name, or -1.
int hash_map_slot(void const* map, char const* name) {
    HashMapRef m{};
    if (name == nullptr || !read_hash_map(map, &m)) return -1;

    for (std::uint32_t i = 0; i < m.Count; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)m.Keys + i * sizeof(std::uint32_t), &key)) {
            return -1;
        }
        char const* text = bg3le_fixed_string(key, nullptr);
        if (text != nullptr && std::strcmp(text, name) == 0) return (int)i;
    }
    return -1;
}

}  // namespace

// ---- the C surface Ext.Stats is built on ----

// After bg3le itself has grown the Objects array.
extern "C" void bg3le_stats_objects_changed() {
    const CacheLock lock(stats_cache_lock());
    refresh_objects(true);
}

// A modifier list's handle (its index in RPGStats::ModifierLists), or -1.
extern "C" int bg3le_stats_list_handle(char const* listName) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (listName == nullptr || !ready() || f.Lists.Buffer == nullptr) return -1;
    for (std::uint32_t i = 0; i < f.Lists.Size; ++i) {
        void const* list = nullptr;
        std::uint32_t id = 0;
        if (!read_as((char const*)f.Lists.Buffer + i * sizeof(void*), &list) || list == nullptr
            || !read_as((char const*)list + f.ListNameOffset, &id)) {
            continue;
        }
        char const* text = bg3le_fixed_string(id, nullptr);
        if (text != nullptr && std::strcmp(text, listName) == 0) return (int)i;
    }
    return -1;
}

extern "C" void* bg3le_stats_manager() {
    const CacheLock lock(stats_cache_lock());
    return ready() ? (void*)state().Objects.Buffer : nullptr;
}

extern "C" std::size_t bg3le_stats_count() {
    const CacheLock lock(stats_cache_lock());
    return ready() ? state().Objects.Size : 0;
}

// Whether the stats are new since StatsLoaded last fired, and records them as
// fired if so. Upstream fires it only when the engine loads stats; a save
// load keeps them, and firing again let 5eSpells append to the same Boosts
// on every load until one outgrew the string table. A reload re-creates
// every Object, so the first one's address is the identity.
// Set once RPGStats::Load is hooked: the hook fires StatsLoaded, as upstream's does.
std::atomic<bool> g_stats_load_hooked{false};

extern "C" void bg3le_stats_load_hooked() { g_stats_load_hooked = true; }

// Inside RPGStats::Load, StatsLoaded included.
std::atomic<bool> g_stats_loading{false};

extern "C" void bg3le_stats_loading(bool loading) { g_stats_loading = loading; }
extern "C" bool bg3le_stats_is_loading() { return g_stats_loading.load(); }

extern "C" bool bg3le_stats_take_loaded(bool client) {
    if (g_stats_load_hooked.load()) return false;
    const CacheLock lock(stats_cache_lock());
    if (!ready()) return false;
    static void const* fired[2] = {nullptr, nullptr};  // per context
    static std::uint32_t firedLoads[2] = {~0u, ~0u};
    void const* first = object_at(0);
    void const*& last = fired[client ? 1 : 0];
    std::uint32_t& lastLoads = firedLoads[client ? 1 : 0];
    const std::uint32_t loads = g_module_loads.load();
    if (first == nullptr || (first == last && loads == lastLoads)) return false;
    if (last != nullptr) {
        logf("stats: the engine rebuilt its stats (first object %p, was %p; module load %u)",
             first, last, loads);
    }
    last = first;
    lastLoads = loads;
    return true;
}

// A module load finished (the game state left LoadModule): the stats were rebuilt.
extern "C" void bg3le_stats_module_loaded() {
    ++g_module_loads;
}

extern "C" void* bg3le_stats_at(std::size_t index) {
    const CacheLock lock(stats_cache_lock());
    return (void*)object_at(index);
}

// The raw FixedString id of a stat's Name, for the diagnostic that asks
// whether it still resolves to the same text.
extern "C" bool bg3le_stats_name_id(void const* object, std::uint32_t* out) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || out == nullptr) return false;
    return read_as((char const*)object + state().NameOffset, out);
}

extern "C" char const* bg3le_stats_name(void const* object) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || !ready()) return nullptr;
    bg3se::FixedString name{};
    if (!read_as((char const*)object + state().NameOffset, &name)) {
        return nullptr;
    }
    return text_of(name);
}

// Linear, like the resource bank lookup: the engine's hash for a FixedString
// key is its own, and a wrong hash misses silently where a scan either finds
// the name or does not.
// Every stat by name, built once.
//
// This used to be a linear scan, and the comment above Ext.Stats.GetStats
// said what that costs: a mod that walks the stats and fetches each one by
// name turns 27,821 lookups into 27,821 scans of 27,821 objects, two
// system calls an element. That is five hundred million reads, and it is
// what 5eSpells was doing for six minutes at a stretch with nothing to
// show for it.
//
// Built from one read of the pointer array and one name read per object --
// forty milliseconds, once -- and rebuilt if the array's size changes,
// which is the only way its contents can, since a stat object does not
// move once the manager holds it.
std::unordered_map<std::string, void const*> const& stats_by_name();

// Both indexes, at file scope so bg3le_stats_appended can extend them.
struct NameIndex {
    std::unordered_map<std::string, void const*> byName;
    std::uint32_t builtFor = 0;
    std::uint32_t gen = ~0u;
};
struct ListIndex {
    std::unordered_map<std::string, std::vector<char const*>> byList;
    std::uint32_t builtFor = 0;
    std::uint32_t gen = ~0u;
};
NameIndex g_name_index;
ListIndex g_list_index;

std::unordered_map<std::string, void const*> const& stats_by_name() {
    auto& byName = g_name_index.byName;
    auto& builtFor = g_name_index.builtFor;
    clear_on_rebuild(g_name_index.gen, [&] { byName.clear(); builtFor = 0; });

    const std::uint32_t size = state().Objects.Size;
    if (!byName.empty() && builtFor == size) return byName;

    byName.clear();
    builtFor = size;

    // Nothing this index read before is trusted again, because this only runs
    // when the stats array has grown -- the engine is still parsing, and a
    // name resolved mid-parse can be the bytes that were there before the
    // engine wrote it. Forgetting failures alone left one such name cached
    // for the session; see bg3le_fixed_string_forget_all.
    bg3le_fixed_string_forget_all();

    std::vector<void const*> all(size);
    const std::size_t got =
        safe_read_some(state().Objects.Buffer, all.data(),
                       (std::size_t)size * sizeof(void*)) / sizeof(void*);

    byName.reserve(got);
    for (std::size_t i = 0; i < got; ++i) {
        if (all[i] == nullptr) continue;
        char const* name = bg3le_stats_name(all[i]);
        if (name == nullptr || name[0] == '\0') continue;
        byName.emplace(name, all[i]);
    }

    logf("stats: %zu stats indexed by name", byName.size());
    return byName;
}

extern "C" char const* bg3le_stats_type(void const* object);

// Stat names, by the modifier list they belong to, built once.
//
// Ext.Stats.GetStats walked all 27,821 objects and asked each one for its
// name and its list -- six system calls an element, a quarter of a second
// a call. A mod calls it once per spell school, or once per loop, and
// 5eSpells never got past the section that does: it spent minutes there
// without reaching a single stat write.
//
// The empty key holds every name, in array order, which is the order
// upstream returns them in.
std::unordered_map<std::string, std::vector<char const*>> const&
stats_names_by_list() {
    auto& byList = g_list_index.byList;
    auto& builtFor = g_list_index.builtFor;
    clear_on_rebuild(g_list_index.gen, [&] { byList.clear(); builtFor = 0; });

    const std::uint32_t size = state().Objects.Size;
    if (!byList.empty() && builtFor == size) return byList;

    byList.clear();
    builtFor = size;

    // Nothing this index read before is trusted again, because this only runs
    // when the stats array has grown -- the engine is still parsing, and a
    // name resolved mid-parse can be the bytes that were there before the
    // engine wrote it. Forgetting failures alone left one such name cached
    // for the session; see bg3le_fixed_string_forget_all.
    bg3le_fixed_string_forget_all();

    std::vector<void const*> all(size);
    const std::size_t got =
        safe_read_some(state().Objects.Buffer, all.data(),
                       (std::size_t)size * sizeof(void*)) / sizeof(void*);

    // Only the object a lookup by name will actually return.
    //
    // Names are not unique across modifier lists: one animation path is
    // carried by both a Character and a SpellData stat, and the two
    // indexes disagreed about it -- Ext.Stats.GetStats("SpellData")
    // listed the name while Ext.Stats.Get(name) handed back the
    // Character, whose modifier list has no TargetConditions. A mod
    // walking every spell and reading one got nil, which upstream never
    // returns for a condition.
    //
    // So a name is filed under a list only when that list's object is the
    // one the name resolves to. Everything GetStats hands out can then be
    // read as a member of the list it came from.
    auto const& byName = stats_by_name();

    std::vector<char const*>& every = byList[""];
    every.reserve(got);
    for (std::size_t i = 0; i < got; ++i) {
        if (all[i] == nullptr) continue;
        char const* name = bg3le_stats_name(all[i]);
        if (name == nullptr || name[0] == '\0') continue;

        auto resolves = byName.find(name);
        if (resolves == byName.end() || resolves->second != all[i]) continue;

        every.push_back(name);

        char const* list = bg3le_stats_type(all[i]);
        if (list != nullptr && list[0] != '\0') byList[list].push_back(name);
    }

    logf("stats: %zu names indexed across %zu modifier lists", every.size(),
         byList.size() - 1);
    return byList;
}

// bg3le's own Create appended `object`: extend the indexes by that one entry
// rather than letting the size change rebuild them. A rebuild per created stat
// was most of UAWarCaster's 17-second StatsLoaded (two creates per spell).
extern "C" void bg3le_stats_appended(void const* object) {
    const CacheLock lock(stats_cache_lock());
    if (!ready() || object == nullptr) return;
    const std::uint32_t size = state().Objects.Size;
    char const* name = bg3le_stats_name(object);
    if (name == nullptr || name[0] == '\0') return;

    NameIndex& n = g_name_index;
    if (n.gen == g_generation && !n.byName.empty() && n.builtFor + 1 == size) {
        n.byName.emplace(name, object);
        n.builtFor = size;
    }
    ListIndex& l = g_list_index;
    if (l.gen == g_generation && !l.byList.empty() && l.builtFor + 1 == size
        && n.builtFor == size) {
        l.byList[""].push_back(name);
        char const* list = bg3le_stats_type(object);
        if (list != nullptr && list[0] != '\0') l.byList[list].push_back(name);
        l.builtFor = size;
    }
}

extern "C" std::size_t bg3le_stats_names_count(char const* list) {
    const CacheLock lock(stats_cache_lock());
    if (!ready()) return 0;
    auto const& byList = stats_names_by_list();
    auto found = byList.find(list == nullptr ? "" : list);
    return found == byList.end() ? 0 : found->second.size();
}

// Every name of a list, handed to `each` under one lock and one lookup:
// Ext.Stats.GetStats asks for thousands.
extern "C" std::size_t bg3le_stats_names_each(char const* list,
                                              void (*each)(void*, char const*),
                                              void* context) {
    const CacheLock lock(stats_cache_lock());
    if (!ready()) return 0;
    auto const& byList = stats_names_by_list();
    auto found = byList.find(list == nullptr ? "" : list);
    if (found == byList.end()) return 0;
    for (char const* name : found->second) each(context, name);
    return found->second.size();
}

extern "C" char const* bg3le_stats_names_at(char const* list,
                                            std::size_t index) {
    const CacheLock lock(stats_cache_lock());
    if (!ready()) return nullptr;
    auto const& byList = stats_names_by_list();
    auto found = byList.find(list == nullptr ? "" : list);
    if (found == byList.end() || index >= found->second.size()) return nullptr;
    return found->second[index];
}

extern "C" void* bg3le_stats_find(char const* wanted) {
    const CacheLock lock(stats_cache_lock());
    if (wanted == nullptr || !ready()) return nullptr;

    auto const& byName = stats_by_name();
    auto found = byName.find(wanted);
    return found == byName.end() ? nullptr : (void*)found->second;
}

// ---- attributes ----
//
// Four lookups, because the values are stored apart from their names:
//
//   Object[ListIndexOffset]        -> index into ModifierLists
//   ModifierList.Attributes[n]     -> Modifier, which carries the name
//   Object.IndexedProperties[n]    -> the raw int32
//   Modifier.EnumerationIndex      -> RPGEnumeration, which says how to read
//
// Every offset here was derived by looking at memory, not taken from the
// headers; see the search above for why.

namespace {

void const* list_for_uncached(void const* object);

// Kept per object: its list is fixed when it is made, and the two reads this
// costs were a third of every attribute read (EasyCheat's spell pass).
void const* list_for(void const* object) {
    static std::unordered_map<void const*, void const*> byObject;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { byObject.clear(); });
    auto known = byObject.find(object);
    if (known != byObject.end()) return known->second;
    void const* list = list_for_uncached(object);
    if (list != nullptr) byObject.emplace(object, list);
    return list;
}

void const* list_for_uncached(void const* object) {
    Found const& f = state();
    if (!f.Attributes || object == nullptr) return nullptr;

    std::uint32_t index = 0;
    if (!read_as((char const*)object + f.ListIndexOffset, &index)) {
        return nullptr;
    }
    if (index >= f.Lists.Size) return nullptr;

    void const* list = nullptr;
    if (!read_as((char const*)f.Lists.Buffer + index * sizeof(void*),
                 &list)) {
        return nullptr;
    }
    return list;
}

// The modifiers of one modifier list, read in one go and kept.
//
// Every attribute of every stat goes through here, and reading the pointer
// one at a time cost a system call each: a stat with two hundred
// attributes paid two hundred, and a mod that walks a few hundred stats
// paid tens of thousands. There are nine lists in the whole game and they
// do not change while it runs, so each is read once.
std::vector<void const*> const* modifiers_of(void const* list) {
    static std::unordered_map<void const*, std::vector<void const*>> byList;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { byList.clear(); });
    auto known = byList.find(list);
    if (known != byList.end()) return &known->second;

    ArrayRef attrs{};
    if (!array_header_at((char const*)list + state().AttrsOffset, &attrs)) {
        return nullptr;
    }

    std::vector<void const*> all(attrs.Size);
    const std::size_t got =
        safe_read_some(attrs.Buffer, all.data(), all.size() * sizeof(void*))
        / sizeof(void*);
    all.resize(got);
    if (got == 0) return nullptr;

    return &byList.emplace(list, std::move(all)).first->second;
}

void const* modifier_at(void const* object, std::size_t index) {
    void const* list = list_for(object);
    if (list == nullptr) return nullptr;

    std::vector<void const*> const* mods = modifiers_of(list);
    if (mods == nullptr || index >= mods->size()) return nullptr;
    return (*mods)[index];
}

// What a modifier says about the attribute it describes: its name, the
// enumeration its values are read through, and the kind that follows from
// that. Fixed for the run, and shared by every stat on the same list, so
// this is worth remembering rather than re-reading per attribute per stat.
// An attribute name is an identifier: this is the most it can be, and
// anything longer is a pointer that has stopped meaning what it did.
constexpr std::size_t kMaxAttrName = 128;

struct ModifierMeta {
    char const* Name{nullptr};
    char const* TypeName{nullptr};
    int Kind{13};
    void const* Enumeration{nullptr};
};

ModifierMeta const* meta_of(void const* modifier);
void const* enumeration_for(void const* modifier);
int property_type(void const* enumeration);

// One object's indexed properties, read in one go.
//
// A one-entry cache, because that is the access pattern: everything that
// reads a stat reads all of its attributes in a row. bg3le_stats_attr_set
// drops it, so a write is never served a stale copy.
struct PropertyCache {
    void const* Object{nullptr};
    void const* Begin{nullptr};
    std::vector<std::int32_t> Values;
};

PropertyCache& property_cache() {
    static PropertyCache cache;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { cache = PropertyCache{}; });
    return cache;
}

std::vector<std::int32_t> const* properties_of(void const* object) {
    PropertyCache& cache = property_cache();

    // The same object's again without a read: its bounds only change through
    // bg3le_stats_attr_set, which drops this cache.
    if (object == cache.Object) return &cache.Values;

    void const* bounds[2] = {nullptr, nullptr};
    auto const* props = (char const*)object + state().PropsOffset;
    if (!safe_read(props, bounds, sizeof(bounds))) return nullptr;
    void const* begin = bounds[0];
    void const* end = bounds[1];
    if (begin == nullptr || end < begin) return nullptr;

    const std::size_t count =
        (std::size_t)((char const*)end - (char const*)begin) / 4;

    // Bounded. The count comes from two pointers read out of the object,
    // and if either is not what it should be the answer can be enormous --
    // a vector of it then throws, and an exception out of here is caught
    // by the interpreter and reported with whatever is on its stack. No
    // stat has thousands of attributes; the largest modifier list in the
    // game has a few hundred.
    constexpr std::size_t kMaxProperties = 4096;
    if (count > kMaxProperties) {
        cache.Object = nullptr;
        return nullptr;
    }

    std::vector<std::int32_t> all(count);
    const std::size_t got =
        safe_read_some(begin, all.data(), count * sizeof(std::int32_t))
        / sizeof(std::int32_t);
    all.resize(got);

    cache.Object = object;
    cache.Begin = begin;
    cache.Values = std::move(all);
    return &cache.Values;
}

ModifierMeta const* meta_of(void const* modifier) {
    static std::unordered_map<void const*, ModifierMeta> byModifier;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { byModifier.clear(); });
    auto known = byModifier.find(modifier);
    if (known != byModifier.end()) return &known->second;

    Found const& f = state();
    bg3se::FixedString modName{};
    if (!read_as((char const*)modifier + f.ModifierNameOffset, &modName)) {
        return nullptr;
    }

    ModifierMeta meta;
    meta.Name = text_of(modName);

    // Not cached if the name did not resolve: this is kept for the run,
    // and an attribute whose name is missing is an attribute no caller can
    // reach by name. Nor if it is not a bounded identifier, which is what
    // a name that has stopped pointing at a live string entry looks like.
    if (meta.Name == nullptr) return nullptr;
    if (::strnlen(meta.Name, kMaxAttrName) >= kMaxAttrName) return nullptr;
    meta.Enumeration = enumeration_for(modifier);
    meta.Kind = property_type(meta.Enumeration);
    if (meta.Enumeration != nullptr) {
        bg3se::FixedString enName{};
        meta.TypeName =
            read_as((char const*)meta.Enumeration + f.ValueNameOffset, &enName)
                ? text_of(enName)
                : nullptr;
    }
    return &byModifier.emplace(modifier, meta).first->second;
}

// The enumeration a modifier's value should be read through. EnumerationIndex
// is the modifier's first member.
void const* enumeration_for(void const* modifier) {
    Found const& f = state();
    if (modifier == nullptr || f.ValueLists.Buffer == nullptr) return nullptr;
    std::int32_t index = 0;
    if (!read_as(modifier, &index)) return nullptr;
    if (index < 0 || (std::size_t)index >= f.ValueLists.Size) return nullptr;
    void const* en = nullptr;
    if (!read_as((char const*)f.ValueLists.Buffer + (std::size_t)index
                     * sizeof(void*), &en)) {
        return nullptr;
    }
    return en;
}

// Upstream decides this by comparing the enumeration's name against known
// type names, so the same comparison is made here on the resolved text.
// Values are RPGEnumerationType in bg3se's declaration order.
int property_type(void const* enumeration) {
    if (enumeration == nullptr) return 13;                       // Unknown
    bg3se::FixedString name{};
    if (!read_as((char const*)enumeration + state().ValueNameOffset, &name)) {
        return 13;
    }
    char const* text = text_of(name);
    if (text == nullptr) return 13;

    if (std::strcmp(text, "ConstantInt") == 0) return 0;         // Int
    if (std::strcmp(text, "ConstantFloat") == 0) return 2;       // Float
    if (std::strcmp(text, "FixedString") == 0
        || std::strcmp(text, "StatusIDs") == 0) return 3;        // FixedString
    if (std::strcmp(text, "Guid") == 0) return 6;                // GUID
    if (std::strcmp(text, "StatsFunctors") == 0) return 7;
    if (std::strcmp(text, "Conditions") == 0
        || std::strcmp(text, "TargetConditions") == 0
        || std::strcmp(text, "UseConditions") == 0) return 8;
    if (std::strcmp(text, "RollConditions") == 0) return 9;
    if (std::strcmp(text, "Requirements") == 0) return 10;
    if (std::strcmp(text, "MemorizationRequirements") == 0) return 11;
    if (std::strcmp(text, "TranslatedString") == 0) return 12;
    return is_flag_type(text) ? 5 : 4;         // Flags or Enumeration
}

}  // namespace

// The stat this one inherits from, or null.
//
// Using sits immediately before ModifierListIndex in the header, and that
// offset was derived, so this one comes for free: the value is an index into
// the same stats array, or -1.
extern "C" char const* bg3le_stats_using(void const* object) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (!f.Attributes || object == nullptr || f.ListIndexOffset < 4) {
        return nullptr;
    }
    std::int32_t index = 0;
    if (!read_as((char const*)object + f.ListIndexOffset - 4, &index)) {
        return nullptr;
    }
    if (index < 0 || (std::size_t)index >= f.Objects.Size) return nullptr;
    void const* parent = nullptr;
    if (!read_as((char const*)f.Objects.Buffer
                     + (std::size_t)index * sizeof(void*), &parent)) {
        return nullptr;
    }
    return bg3le_stats_name(parent);
}

// A value list by name, for the enumeration helpers.
void const* value_list_named(char const* name) {
    Found const& f = state();
    if (name == nullptr || f.ValueLists.Buffer == nullptr) return nullptr;

    for (std::uint32_t i = 0; i < f.ValueLists.Size; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)f.ValueLists.Buffer + i * sizeof(void*),
                     &list)) {
            continue;
        }
        if (list == nullptr) continue;

        bg3se::FixedString listName{};
        if (!read_as((char const*)list + f.ValueNameOffset, &listName)) {
            continue;
        }
        char const* text = text_of(listName);
        if (text != nullptr && std::strcmp(text, name) == 0) return list;
    }
    return nullptr;
}

// The label/value map of a value list, which follows its name.
bg3se::LegacyMap<bg3se::FixedString, std::int32_t> const* value_map(
    void const* list) {
    return (bg3se::LegacyMap<bg3se::FixedString, std::int32_t> const*)
        ((char const*)list + state().ValueNameOffset + 8);
}


// Ext.Stats.EnumIndexToLabel(enumeration, index)
extern "C" char const* bg3le_stats_enum_label(char const* enumeration,
                                              int index) {
    const CacheLock lock(stats_cache_lock());
    void const* list = value_list_named(enumeration);
    if (list == nullptr) return nullptr;

    for (auto const& pair : *value_map(list)) {
        if (pair.Value == index) return text_of(pair.Key);
    }
    return nullptr;
}

// Ext.Stats.EnumLabelToIndex(enumeration, label)
extern "C" bool bg3le_stats_enum_index(char const* enumeration,
                                       char const* label, int* out) {
    const CacheLock lock(stats_cache_lock());
    void const* list = value_list_named(enumeration);
    if (list == nullptr || label == nullptr) return false;

    for (auto const& pair : *value_map(list)) {
        char const* text = text_of(pair.Key);
        if (text != nullptr && std::strcmp(text, label) == 0) {
            if (out != nullptr) *out = pair.Value;
            return true;
        }
    }
    return false;
}

// The attribute names a modifier list declares, which is what
// Ext.Stats.GetModifierAttributes reports.
extern "C" std::size_t bg3le_stats_list_attr_count(char const* listName) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (!f.Attributes || listName == nullptr) return 0;

    for (std::uint32_t i = 0; i < f.Lists.Size; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)f.Lists.Buffer + i * sizeof(void*),
                     &list) || list == nullptr) {
            continue;
        }
        bg3se::FixedString name{};
        if (!read_as((char const*)list + f.ListNameOffset, &name)) continue;
        char const* text = text_of(name);
        if (text == nullptr || std::strcmp(text, listName) != 0) continue;

        ArrayRef attrs{};
        if (!array_header_at((char const*)list + f.AttrsOffset, &attrs)) {
            return 0;
        }
        return attrs.Size;
    }
    return 0;
}

extern "C" bool bg3le_stats_list_attr_at(char const* listName,
                                         std::size_t index,
                                         char const** nameOut,
                                         char const** typeOut) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (!f.Attributes || listName == nullptr) return false;

    for (std::uint32_t i = 0; i < f.Lists.Size; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)f.Lists.Buffer + i * sizeof(void*),
                     &list) || list == nullptr) {
            continue;
        }
        bg3se::FixedString name{};
        if (!read_as((char const*)list + f.ListNameOffset, &name)) continue;
        char const* text = text_of(name);
        if (text == nullptr || std::strcmp(text, listName) != 0) continue;

        ArrayRef attrs{};
        if (!array_header_at((char const*)list + f.AttrsOffset, &attrs)
            || index >= attrs.Size) {
            return false;
        }

        void const* modifier = nullptr;
        if (!read_as((char const*)attrs.Buffer + index * sizeof(void*),
                     &modifier) || modifier == nullptr) {
            return false;
        }

        bg3se::FixedString attrName{};
        if (!read_as((char const*)modifier + f.ModifierNameOffset,
                     &attrName)) {
            return false;
        }
        if (nameOut != nullptr) *nameOut = text_of(attrName);

        if (typeOut != nullptr) {
            void const* en = enumeration_for(modifier);
            bg3se::FixedString typeName{};
            if (en != nullptr
                && read_as((char const*)en + f.ValueNameOffset, &typeName)) {
                *typeOut = text_of(typeName);
            } else {
                *typeOut = nullptr;
            }
        }
        return true;
    }
    return false;
}

// Where ModifierListIndex sits on an Object, as derived from the value
// counts. stats_functors.cpp checks this against the property maps before
// trusting the rest of the member walk.
extern "C" std::size_t bg3le_stats_list_index_offset() {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    return f.Attributes ? f.ListIndexOffset : 0;
}

// The index of the modifier list this stat uses, or -1.
extern "C" int bg3le_stats_list_index(void const* object) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (!f.Attributes || object == nullptr) return -1;
    std::uint32_t index = 0;
    if (!read_as((char const*)object + f.ListIndexOffset, &index)) return -1;
    return (int)index;
}

extern "C" char const* bg3le_stats_type(void const* object) {
    const CacheLock lock(stats_cache_lock());
    void const* list = list_for(object);
    if (list == nullptr) return nullptr;
    bg3se::FixedString name{};
    if (!read_as((char const*)list + state().ListNameOffset, &name)) {
        return nullptr;
    }
    return text_of(name);
}

extern "C" std::size_t bg3le_stats_attr_count(void const* object) {
    const CacheLock lock(stats_cache_lock());
    void const* list = list_for(object);
    if (list == nullptr) return 0;
    ArrayRef attrs{};
    if (!array_header_at((char const*)list + state().AttrsOffset, &attrs)) {
        return 0;
    }
    return attrs.Size;
}

// Which index an attribute has, by name.
//
// A property of the modifier list rather than of the stat -- every weapon
// shares one, every spell another, and there are nine in the game -- so
// the map is built once per list. Scanning instead cost a walk of a
// couple of hundred attributes for every field a mod read, which is what
// made reading a stat lazily no cheaper than reading all of it.
extern "C" int bg3le_stats_attr_index(void const* object,
                                      char const* wanted) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || wanted == nullptr) return -1;

    void const* list = list_for(object);
    if (list == nullptr) return -1;

    static std::unordered_map<void const*,
                              std::unordered_map<std::string, int>> byList;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { byList.clear(); });
    auto known = byList.find(list);
    if (known == byList.end()) {
        std::unordered_map<std::string, int> names;
        std::vector<void const*> const* mods = modifiers_of(list);
        if (mods != nullptr) {
            for (std::size_t i = 0; i < mods->size(); ++i) {
                ModifierMeta const* meta = meta_of((*mods)[i]);
                if (meta == nullptr || meta->Name == nullptr) continue;

                // Bounded, because the name is a pointer into the engine's
                // own string entry and an attribute name is a short
                // identifier. Building a std::string from it unbounded
                // reads until it finds a NUL, and on one Armor object that
                // ran far enough to throw length_error -- which Lua, built
                // as C++ here, caught and reported using whatever was on
                // its stack. The error a mod saw was the single word
                // "Shield", the name it had asked for.
                const std::size_t length = ::strnlen(meta->Name, kMaxAttrName);
                if (length == 0 || length >= kMaxAttrName) continue;
                names.emplace(std::string(meta->Name, length), (int)i);
            }
        }
        known = byList.emplace(list, std::move(names)).first;
    }

    auto found = known->second.find(wanted);
    return found == known->second.end() ? -1 : found->second;
}

extern "C" bool bg3le_stats_attr_at(void const* object, std::size_t index,
                                    char const** nameOut,
                                    char const** typeNameOut, int* kindOut,
                                    int* rawOut) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (!f.Attributes || object == nullptr) return false;

    void const* mod = modifier_at(object, index);
    if (mod == nullptr) return false;

    ModifierMeta const* meta = meta_of(mod);
    if (meta == nullptr) return false;

    // The attribute's position is its index into the object's values. An
    // object the engine made at runtime can carry fewer values than its
    // modifier list has attributes; a missing one reads as unset (-1), so it
    // takes the same default upstream's getters give an unset value.
    std::vector<std::int32_t> const* values = properties_of(object);
    if (values == nullptr) return false;

    if (nameOut != nullptr) *nameOut = meta->Name;
    if (typeNameOut != nullptr) *typeNameOut = meta->TypeName;
    if (kindOut != nullptr) *kindOut = meta->Kind;
    if (rawOut != nullptr) *rawOut = index < values->size() ? (*values)[index] : -1;
    return true;
}

// For an enumeration-typed attribute, the label matching the raw value. The
// enumeration maps label to value, so this is a reverse lookup over it.
extern "C" char const* bg3le_stats_attr_label(void const* object,
                                              std::size_t index, int raw) {
    const CacheLock lock(stats_cache_lock());
    void const* mod = modifier_at(object, index);
    void const* en = enumeration_for(mod);
    if (en == nullptr) return nullptr;

    // RPGEnumeration is Name then Values, so the map follows the name.
    auto const* map = (bg3se::LegacyMap<bg3se::FixedString, std::int32_t> const*)
        ((char const*)en + state().ValueNameOffset + 8);
    for (auto const& pair : *map) {
        if (pair.Value == raw) return text_of(pair.Key);
    }
    return nullptr;
}

// A flag set's labels, joined by semicolons.
//
// Straight from upstream's Object::GetFlags: the attribute's raw value is an
// index into the Int64s pool, whose entry is a pointer to the mask, and a
// label belongs in the set when bit (value - 1) is set. The minus one is the
// part no amount of staring at the numbers would have produced -- decomposing
// the raw value as a bitmask over the label values gave a longsword
// proficient in clubs and light armour.
extern "C" bool bg3le_stats_attr_flags(void const* object, std::size_t index,
                                       int raw, char* out,
                                       std::size_t capacity) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (out == nullptr || capacity == 0) return false;
    out[0] = '\0';
    if (raw <= 0 || f.Int64s.Buffer == nullptr) return false;
    if ((std::size_t)raw >= f.Int64s.Size) return false;

    std::int64_t const* slot = nullptr;
    if (!read_as((char const*)f.Int64s.Buffer + (std::size_t)raw
                     * sizeof(void*), &slot)) {
        return false;
    }
    if (slot == nullptr) return false;
    std::uint64_t mask = 0;
    if (!read_as(slot, &mask)) return false;

    void const* mod = modifier_at(object, index);
    void const* en = enumeration_for(mod);
    if (en == nullptr) return false;

    auto const* map = (bg3se::LegacyMap<bg3se::FixedString, std::int32_t> const*)
        ((char const*)en + f.ValueNameOffset + 8);

    std::size_t used = 0;
    for (auto const& pair : *map) {
        // Value zero is an entry like "None = 0" and never belongs in a set.
        if (pair.Value == 0) continue;
        if ((mask & (1ull << (pair.Value - 1))) == 0) continue;
        char const* label = text_of(pair.Key);
        if (label == nullptr) continue;

        const std::size_t len = std::strlen(label);
        const std::size_t need = used == 0 ? len : len + 1;
        if (used + need + 1 > capacity) break;
        if (used != 0) out[used++] = ';';
        std::memcpy(out + used, label, len);
        used += len;
        out[used] = '\0';
    }
    // True even when nothing is set: upstream returns an empty array for a
    // flag attribute with no bits, which is not the same as no value.
    return true;
}

// A Float attribute's value, from the float pool.
extern "C" bool bg3le_stats_attr_float(int raw, double* out) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    // Slot zero is the unset slot, not a value: RPGStats::GetFloat and every
    // other pool accessor tests `attributeId > 0`. Accepting it here is what
    // made unset floats read back as 0.0 where upstream reports nothing.
    if (raw <= 0 || f.Floats.Buffer == nullptr) return false;
    if ((std::size_t)raw >= f.Floats.Size) return false;
    float v = 0.0f;
    if (!read_as((char const*)f.Floats.Buffer + (std::size_t)raw
                     * sizeof(float), &v)) {
        return false;
    }
    if (out != nullptr) *out = (double)v;
    return true;
}

// A GUID attribute's value, formatted the way the engine writes one.
extern "C" bool bg3le_stats_attr_guid(int raw, char* out,
                                      std::size_t capacity) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (raw <= 0 || f.Guids.Buffer == nullptr) return false;
    if ((std::size_t)raw >= f.Guids.Size) return false;
    std::uint8_t bytes[16] = {};
    if (!read_as((char const*)f.Guids.Buffer + (std::size_t)raw * 16,
                 &bytes)) {
        return false;
    }
    return bg3le_meta_format_guid(bytes, out, capacity);
}

// A FixedString attribute's text: the raw value indexes RPGStats' own pool,
// not the global string table.
extern "C" char const* bg3le_stats_attr_string(int raw) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (raw <= 0 || f.Strings.Buffer == nullptr) return nullptr;
    if ((std::size_t)raw >= f.Strings.Size) return nullptr;

    // The pool slot's id, kept: slots are only ever appended to, and the
    // id behind one does not change.
    static std::unordered_map<int, std::uint32_t> known;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { known.clear(); });
    auto found = known.find(raw);
    if (found == known.end()) {
        std::uint32_t id = 0;
        if (!read_as((char const*)f.Strings.Buffer
                         + (std::size_t)raw * sizeof(std::uint32_t), &id)) {
            return nullptr;
        }
        found = known.emplace(raw, id).first;
    }
    return bg3le_fixed_string(found->second, nullptr);
}

// A TranslatedString attribute's loca handle, e.g.
// "h5fafec24g30d5g425cg952cga9c53752059c".
extern "C" char const* bg3le_stats_attr_translated(int raw) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (raw <= 0 || f.TranslatedStrings.Buffer == nullptr) return nullptr;
    if ((std::size_t)raw >= f.TranslatedStrings.Size) return nullptr;

    std::uint32_t index = 0;
    if (!read_as((char const*)f.TranslatedStrings.Buffer
                     + (std::size_t)raw * 16, &index)) {
        return nullptr;
    }
    return bg3le_fixed_string(index, nullptr);
}

// A Conditions attribute's expression text.

// ---------------------------------------------------------------------------
// Writing an attribute.
//
// An attribute is one int32 in the object's indexed properties, so the
// write itself is a single store into memory this file already reads. What
// takes care is the value: most kinds are an index into one of RPGStats'
// pools, and a value a mod builds at runtime is not in one.
//
// A value a mod builds goes at the end of the pool: into the array's spare
// capacity, or, when there is none, into a fresh buffer from the engine's
// own operator new -- the allocator the engine frees these arrays with --
// with the old buffer left in place, since the engine may still be reading
// it. See pool_slot.
//
// The size is raised to include the entry, which the first version of this
// did not do -- on the theory that an entry the engine does not count is
// an entry it will never destruct. That was wrong in the way that matters:
// an index past the size is out of range to every reader, this file's
// included, so the attribute read back empty. Writing a condition
// therefore *cleared* it, and an interrupt with no condition is an
// interrupt that always fires.
std::size_t& conditions_taken() {
    static std::size_t taken = 0;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { taken = 0; });
    return taken;
}

std::size_t& strings_taken() {
    static std::size_t taken = 0;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { taken = 0; });
    return taken;
}

extern "C" bool bg3le_fixed_string_intern(char const* text,
                                          std::uint32_t* out);
extern "C" char const* bg3le_stats_attr_condition(int raw);
extern "C" bool bg3le_game_allocator_ready();
extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out);
extern "C" bool bg3le_meta_parse_guid(char const* text, void* out);

bool write_bytes(void* at, void const* from, std::size_t size) {
    // The pools are ordinary heap, not the read-only image, so there is no
    // protection to change -- but a wrong address must not take the game
    // down, so the target is proved readable first.
    unsigned char probe[1] = {};
    if (!safe_read(at, probe, sizeof(probe))) return false;
    std::memcpy(at, from, size);
    return true;
}

// A sixteen-byte Larian string holding this text, built in place.
//
// Fifteen characters or fewer live inside the sixteen bytes and need
// nothing else. Longer ones need a buffer from the engine's heap: the entry
// sits inside the pool, and the engine frees its buffer when it resets the
// stats for a load (a malloc'd one crashed it there).
bool build_ls_string(void* at, char const* text) {
    const std::size_t length = std::strlen(text);
    unsigned char raw[16] = {};

    if (length <= 15) {
        std::memcpy(raw, text, length);
        raw[15] = (unsigned char)length;
        return write_bytes(at, raw, sizeof(raw));
    }

    if (length > (1u << 20) || !bg3le_game_allocator_ready()) return false;
    char* owned = (char*)bg3se::GameAllocRaw(length + 1);
    if (owned == nullptr) return false;
    std::memcpy(owned, text, length + 1);

    const std::uint64_t buffer = (std::uint64_t)(std::uintptr_t)owned;
    const std::uint32_t size = (std::uint32_t)length;
    const std::uint32_t capacity = (std::uint32_t)length | 0x80000000u;
    std::memcpy(raw + 0, &buffer, sizeof(buffer));
    std::memcpy(raw + 8, &size, sizeof(size));
    std::memcpy(raw + 12, &capacity, sizeof(capacity));

    // On failure the buffer is abandoned rather than freed (never free engine memory).
    return write_bytes(at, raw, sizeof(raw));
}

// The pool index for a condition expression: the one it already has, or a
// slack slot, or -1.
// The next free slot of one of RPGStats' pools, an engine Array whose header
// is { Buffer, Capacity, Size }. A full array moves to a fresh engine
// allocation half as large again, as push_back would grow it -- but the old
// buffer is abandoned rather than freed: the engine may still be reading it,
// and bg3le never frees engine memory. Returns false if there is no room
// and none can be made.
bool pool_slot(void const* header, ArrayRef const& ref, std::size_t stride,
               char const* what, std::uint32_t* slot) {
    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    std::uint32_t used = 0;
    if (!read_as(header, &buffer) || !read_as((char const*)header + 8, &capacity)
        || !read_as((char const*)header + 12, &used)) {
        return false;
    }
    if (used < capacity) {
        *slot = used;
        return true;
    }
    if (!bg3le_game_allocator_ready()) return false;

    const std::uint32_t grown = capacity + capacity / 2 + 16;
    void* fresh = bg3se::GameAllocRaw((std::size_t)grown * stride);
    if (fresh == nullptr) return false;
    std::memset(fresh, 0, (std::size_t)grown * stride);
    if (used != 0
        && safe_read_some((void const*)buffer, fresh, (std::size_t)used * stride)
               != (std::size_t)used * stride) {
        return false;  // the fresh block is abandoned too
    }

    const auto pointer = (std::uint64_t)(std::uintptr_t)fresh;
    write_bytes((void*)header, &pointer, sizeof(pointer));
    write_bytes((void*)((char const*)header + 8), &grown, sizeof(grown));
    const_cast<ArrayRef&>(ref).Buffer = fresh;
    logf("stats: the %s pool was full at %u; moved it to a buffer of %u and "
         "left the old one in place", what, capacity, grown);
    *slot = used;
    return true;
}

extern "C" int bg3le_stats_condition_intern(char const* text) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (text == nullptr || f.Conditions.Buffer == nullptr
        || f.ConditionsHeader == nullptr) {
        return -1;
    }

    // What has already been written, so the same expression twice costs
    // nothing. Searching the pool itself instead reads five thousand
    // Larian strings, most of them through a pointer, and a mod that
    // writes forty conditions paid for it forty times.
    static std::unordered_map<std::string, int> ours;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { ours.clear(); });
    auto known = ours.find(text);
    if (known != ours.end()) return known->second;

    // The next slot is the array's own size, read live. Adding a count of
    // our writes to a size those writes had already advanced skipped a
    // slot per write, and one more each time.
    std::uint32_t next = 0;
    if (!pool_slot(f.ConditionsHeader, f.Conditions, 16, "condition", &next)) {
        return -1;
    }
    const std::size_t slot = next;

    if (!build_ls_string((void*)((char*)f.Conditions.Buffer + slot * 16),
                         text)) {
        return -1;
    }
    // Now inside the array, so everything that reads it can see it.
    const std::uint32_t size = (std::uint32_t)(slot + 1);
    write_bytes((void*)((char*)f.ConditionsHeader + 12), &size, sizeof(size));
    const_cast<ArrayRef&>(f.Conditions).Size = size;

    ++conditions_taken();
    ours.emplace(text, (int)slot);
    bg3le_stats_attr_condition((int)slot);  // into the read cache too

    // Truncated, and quiet after the first few: these run to six hundred
    // characters and a mod writes dozens.
    static std::size_t said = 0;
    if (++said <= 3) {
        logf("stats: condition written to pool slot %zu (%zu written): "
             "\"%.64s%s\"", slot, conditions_taken(), text,
             std::strlen(text) > 64 ? "..." : "");
    }
    return (int)slot;
}

// Debug: what the stats hold right now, for the StatsLoaded log lines.
extern "C" void bg3le_stats_debug_summary(char const* when) {
    const CacheLock lock(stats_cache_lock());
    if (!ready()) {
        logf("stats debug: %s: the stats are not found (module load %u, generation %u)", when,
             g_module_loads.load(), g_generation);
        return;
    }
    Found const& f = state();
    std::size_t spells = 0, statuses = 0, passives = 0, interrupts = 0;
    for (std::size_t i = 0; i < f.Objects.Size; ++i) {
        void const* object = nullptr;
        if (!read_as((char const*)f.Objects.Buffer + i * sizeof(void*), &object) || object == nullptr) continue;
        char const* type = bg3le_stats_type(object);
        if (type == nullptr) continue;
        if (std::strcmp(type, "SpellData") == 0) ++spells;
        else if (std::strcmp(type, "StatusData") == 0) ++statuses;
        else if (std::strcmp(type, "PassiveData") == 0) ++passives;
        else if (std::strcmp(type, "InterruptData") == 0) ++interrupts;
    }
    std::uint32_t conditions = 0;
    if (f.ConditionsHeader != nullptr) read_as((char const*)f.ConditionsHeader + 12, &conditions);
    logf("stats debug: %s: %zu stats (%zu spells, %zu statuses, %zu passives, %zu interrupts); "
         "condition pool %u, %zu written by bg3le; first object %p; module load %u, generation %u",
         when, (std::size_t)f.Objects.Size, spells, statuses, passives, interrupts, conditions,
         conditions_taken(), first_object(f), g_module_loads.load(), g_generation);
}

// The pool index for a FixedString attribute's text.
//
// Two steps, because the pool holds string-table ids rather than text: get
// an id for the text, then a slot in the pool that holds it. A slot the
// pool already has is free; otherwise one comes out of the array's spare
// capacity, on the same terms as a condition.
extern "C" int bg3le_stats_string_intern(char const* text) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (text == nullptr || f.Strings.Buffer == nullptr
        || f.StringsHeader == nullptr) {
        return -1;
    }

    std::uint32_t id = 0;
    if (!bg3le_fixed_string_intern(text, &id)) return -1;

    // Where each id already sits, read once. The pool is four bytes per
    // entry, so the whole thing is one read of a hundred and twenty
    // kilobytes -- worth doing, because reusing a slot leaves the spare
    // capacity for ids that need it.
    static std::unordered_map<std::uint32_t, int> slots;
    static bool mapped = false;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { slots.clear(); mapped = false; });
    if (!mapped) {
        mapped = true;
        std::vector<std::uint32_t> all(f.Strings.Size);
        const std::size_t got = safe_read_some(
            f.Strings.Buffer, all.data(),
            all.size() * sizeof(std::uint32_t)) / sizeof(std::uint32_t);
        for (std::size_t i = 1; i < got; ++i) {
            slots.emplace(all[i], (int)i);
        }
        logf("stats: %zu of the string pool's %u slots indexed for writing",
             slots.size(), f.Strings.Size);
    }

    auto known = slots.find(id);
    if (known != slots.end()) return known->second;

    std::uint32_t next = 0;
    if (!pool_slot(f.StringsHeader, f.Strings, sizeof(std::uint32_t), "string",
                   &next)) {
        return -1;
    }
    const std::size_t slot = next;  // live, as for conditions

    if (!write_bytes((void*)((char*)f.Strings.Buffer
                             + slot * sizeof(std::uint32_t)),
                     &id, sizeof(id))) {
        return -1;
    }
    const std::uint32_t size = (std::uint32_t)(slot + 1);
    write_bytes((void*)((char*)f.StringsHeader + 12), &size, sizeof(size));
    const_cast<ArrayRef&>(f.Strings).Size = size;

    ++strings_taken();
    slots.emplace(id, (int)slot);

    static std::size_t said = 0;
    if (++said <= 3) {
        logf("stats: string id %#x written to pool slot %zu (%zu written)",
             id, slot, strings_taken());
    }
    return (int)slot;
}

// The pool index for a flag set's mask, as upstream's
// RPGStats::GetOrCreateInt64: each entry is a pointer to an int64 allocated
// on the engine's heap. A slot already holding the mask is reused;
// otherwise one comes out of the array's spare capacity, on the same terms
// as a condition.
extern "C" int bg3le_stats_int64_intern(std::int64_t value) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (f.Int64s.Buffer == nullptr || f.Int64sHeader == nullptr) return -1;

    static std::unordered_map<std::int64_t, int> slots;
    static bool mapped = false;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { slots.clear(); mapped = false; });
    if (!mapped) {
        mapped = true;
        std::vector<std::uint64_t> cells(f.Int64s.Size);
        const std::size_t got = safe_read_some(
            f.Int64s.Buffer, cells.data(), cells.size() * sizeof(std::uint64_t));
        // Slot 0 reads as "no flags", so it is never handed out.
        for (std::size_t i = 1; i < got / sizeof(std::uint64_t); ++i) {
            std::int64_t mask = 0;
            if (cells[i] != 0 && read_as((void const*)cells[i], &mask)) {
                slots.emplace(mask, (int)i);
            }
        }
    }
    auto known = slots.find(value);
    if (known != slots.end()) return known->second;

    std::uint32_t used = 0;
    if (!pool_slot(f.Int64sHeader, f.Int64s, sizeof(void*), "int64", &used)
        || !bg3le_game_allocator_ready()) {
        return -1;
    }

    auto* cell = bg3se::GameAlloc<std::int64_t>();
    *cell = value;
    const auto pointer = (std::uint64_t)(std::uintptr_t)cell;
    if (!write_bytes((void*)((char*)f.Int64s.Buffer + used * sizeof(pointer)),
                     &pointer, sizeof(pointer))) {
        return -1;
    }
    const std::uint32_t size = used + 1;
    write_bytes((void*)((char*)f.Int64sHeader + 12), &size, sizeof(size));
    const_cast<ArrayRef&>(f.Int64s).Size = size;
    slots.emplace(value, (int)used);
    return (int)used;
}

// The pool index for a float attribute's value, as upstream's
// RPGStats::GetOrCreateFloat. A slot already holding the same bits is
// reused -- a pool entry is never changed once written, so sharing one is
// the same as owning one.
extern "C" int bg3le_stats_float_intern(float value) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (f.Floats.Buffer == nullptr || f.FloatsHeader == nullptr) return -1;

    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    static std::unordered_map<std::uint32_t, int> slots;
    static bool mapped = false;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { slots.clear(); mapped = false; });
    if (!mapped) {
        mapped = true;
        std::vector<std::uint32_t> all(f.Floats.Size);
        const std::size_t got = safe_read_some(
            f.Floats.Buffer, all.data(), all.size() * sizeof(std::uint32_t));
        for (std::size_t i = 1; i < got / sizeof(std::uint32_t); ++i) {
            slots.emplace(all[i], (int)i);
        }
    }
    auto known = slots.find(bits);
    if (known != slots.end()) return known->second;

    std::uint32_t slot = 0;
    if (!pool_slot(f.FloatsHeader, f.Floats, sizeof(float), "float", &slot)) {
        return -1;
    }
    if (!write_bytes((void*)((char const*)f.Floats.Buffer
                             + slot * sizeof(float)),
                     &value, sizeof(value))) {
        return -1;
    }
    const std::uint32_t size = slot + 1;
    write_bytes((void*)((char const*)f.FloatsHeader + 12), &size, sizeof(size));
    const_cast<ArrayRef&>(f.Floats).Size = size;
    slots.emplace(bits, (int)slot);
    return (int)slot;
}

// Object::Requirements, one entry at a time. Upstream presents each as
// { Requirement, Not, Param }, with Param the tag's GUID for a Tag
// requirement and the integer otherwise.
extern "C" int bg3le_stats_requirement_count(void const* object) {
    const CacheLock lock(stats_cache_lock());
    std::uint32_t size = 0;
    if (object == nullptr
        || !read_as(walked(object, kObjectRequirements + 12), &size)) {
        return -1;
    }
    return size > 4096 ? -1 : (int)size;
}

extern "C" bool bg3le_stats_requirement_at(void const* object, int index,
                                           std::uint32_t* id,
                                           std::int32_t* intParam,
                                           unsigned char* tag, bool* negated) {
    const CacheLock lock(stats_cache_lock());
    const int count = bg3le_stats_requirement_count(object);
    if (count < 0 || index < 0 || index >= count) return false;

    void const* buffer = nullptr;
    if (!read_as(walked(object, kObjectRequirements), &buffer)
        || buffer == nullptr) {
        return false;
    }
    bg3se::stats::Requirement r{};
    if (!safe_read((char const*)buffer + (std::size_t)index * sizeof(r), &r,
                   sizeof(r))) {
        return false;
    }
    *id = (std::uint32_t)r.RequirementId;
    *intParam = r.IntParam;
    std::memcpy(tag, &r.TagParam, 16);
    *negated = r.Not;
    return true;
}

// Replaces Object::Requirements, in place if the array has room and in a
// fresh engine allocation otherwise; the old buffer is left, as always.
struct RequirementIn {
    std::uint32_t Id;
    std::int32_t IntParam;
    unsigned char Tag[16];
    bool Not;
};

extern "C" bool bg3le_stats_requirements_set(void const* object,
                                             RequirementIn const* entries,
                                             std::size_t count) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || count > 4096) return false;
    auto* array = walked(object, kObjectRequirements);
    if (array == nullptr) return false;

    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    if (!read_as(array, &buffer) || !read_as(array + 8, &capacity)) return false;
    if (count > capacity || buffer == 0) {
        if (count == 0) {
            const std::uint32_t empty = 0;
            return write_bytes(array + 12, &empty, sizeof(empty));
        }
        if (!bg3le_game_allocator_ready()) return false;
        void* fresh = bg3se::GameAllocRaw(count * sizeof(bg3se::stats::Requirement));
        if (fresh == nullptr) return false;
        buffer = (std::uint64_t)(std::uintptr_t)fresh;
        const auto cap = (std::uint32_t)count;
        write_bytes(array, &buffer, sizeof(buffer));
        write_bytes(array + 8, &cap, sizeof(cap));
    }

    for (std::size_t i = 0; i < count; ++i) {
        bg3se::stats::Requirement r{};
        r.RequirementId = (bg3se::RequirementType)entries[i].Id;
        r.IntParam = entries[i].IntParam;
        std::memcpy(&r.TagParam, entries[i].Tag, 16);
        r.Not = entries[i].Not;
        if (!write_bytes((void*)(std::uintptr_t)(buffer + i * sizeof(r)), &r,
                         sizeof(r))) {
            return false;
        }
    }
    const auto size = (std::uint32_t)count;
    return write_bytes(array + 12, &size, sizeof(size));
}

// The pool index for a TranslatedString attribute, as upstream's
// SetTranslatedString(TranslatedString::FromString(text)): "handle" or
// "handle;version", with the argument string left as a default-constructed
// one -- the unknown handle, version 0.
extern "C" int bg3le_stats_translated_intern(char const* text) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (text == nullptr || f.TranslatedStrings.Buffer == nullptr
        || f.TranslatedHeader == nullptr) {
        return -1;
    }

    std::string handle = text;
    std::uint16_t version = 0;
    if (auto sep = handle.find(';'); sep != std::string::npos) {
        version = (std::uint16_t)std::atoi(handle.c_str() + sep + 1);
        handle.resize(sep);
    }

    std::uint32_t handleId = 0;
    std::uint32_t unknownId = 0;
    if ((!bg3le_fixed_string_index_of(handle.c_str(), &handleId)
         && !bg3le_fixed_string_intern(handle.c_str(), &handleId))
        || !bg3le_fixed_string_index_of(
            "ls::TranslatedStringRepository::s_HandleUnknown", &unknownId)) {
        return -1;
    }

    struct Entry {
        std::uint32_t Handle;
        std::uint16_t Version;
        std::uint16_t Pad0;
        std::uint32_t Argument;
        std::uint16_t ArgumentVersion;
        std::uint16_t Pad1;
    };
    static_assert(sizeof(Entry) == sizeof(bg3se::TranslatedString));
    const Entry entry{handleId, version, 0, unknownId, 0, 0};

    static std::unordered_map<std::uint64_t, int> ours;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { ours.clear(); });
    const std::uint64_t key = ((std::uint64_t)handleId << 16) | version;
    if (auto known = ours.find(key); known != ours.end()) return known->second;

    std::uint32_t slot = 0;
    if (!pool_slot(f.TranslatedHeader, f.TranslatedStrings, sizeof(Entry),
                   "translated string", &slot)
        || !write_bytes((void*)((char const*)f.TranslatedStrings.Buffer
                                + slot * sizeof(Entry)),
                        &entry, sizeof(entry))) {
        return -1;
    }
    const std::uint32_t size = slot + 1;
    write_bytes((void*)((char const*)f.TranslatedHeader + 12), &size,
                sizeof(size));
    const_cast<ArrayRef&>(f.TranslatedStrings).Size = size;
    ours.emplace(key, (int)slot);
    return (int)slot;
}

// Object::AIFlags, which upstream's SetString assigns directly rather than
// through the pool or the enumeration.
extern "C" bool bg3le_stats_ai_flags_set(void const* object, char const* text) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || text == nullptr || !state().Attributes) return false;
    std::uint32_t id = 0;
    if (!bg3le_fixed_string_index_of(text, &id)
        && !bg3le_fixed_string_intern(text, &id)) {
        return false;
    }
    char* at = walked(object, kObjectAIFlags);
    return at != nullptr && write_bytes(at, &id,
                       sizeof(id));
}

// The pool index for a GUID attribute's value, as upstream's SetGuid. A slot
// already holding the GUID is reused. -1 for text that is not a GUID, -2 if
// there is no room.
extern "C" int bg3le_stats_guid_intern(char const* text) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    unsigned char guid[16] = {};
    if (text == nullptr || !bg3le_meta_parse_guid(text, guid)) return -1;
    if (f.Guids.Buffer == nullptr || f.GuidsHeader == nullptr) return -2;

    static std::unordered_map<std::string, int> slots;
    static bool mapped = false;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { slots.clear(); mapped = false; });
    if (!mapped) {
        mapped = true;
        std::vector<unsigned char> all((std::size_t)f.Guids.Size * 16);
        const std::size_t got = safe_read_some(f.Guids.Buffer, all.data(),
                                               all.size());
        for (std::size_t i = 1; i < got / 16; ++i) {
            slots.emplace(std::string((char const*)&all[i * 16], 16), (int)i);
        }
    }
    const std::string key((char const*)guid, 16);
    auto known = slots.find(key);
    if (known != slots.end()) return known->second;

    std::uint32_t slot = 0;
    if (!pool_slot(f.GuidsHeader, f.Guids, 16, "guid", &slot)) return -2;
    if (!write_bytes((void*)((char const*)f.Guids.Buffer + slot * 16), guid,
                     sizeof(guid))) {
        return -2;
    }
    const std::uint32_t size = slot + 1;
    write_bytes((void*)((char const*)f.GuidsHeader + 12), &size, sizeof(size));
    const_cast<ArrayRef&>(f.Guids).Size = size;
    slots.emplace(key, (int)slot);
    return (int)slot;
}

// Replaces a RollConditions attribute, as upstream's SetRollConditions:
// each (name, expression) pair becomes { Name, Conditions = <pooled> }.
// SetString's form is one pair named "Default", and "" is none. The array
// is rewritten in place, or moved to a fresh buffer if it is too small; a
// stat with no entry for the attribute gets one, as upstream's map insert.
// 0 on success; 1 if the entry could not be added, 2 if a condition could
// not be pooled, 3 otherwise.
extern "C" int bg3le_stats_roll_set(void const* object, char const* attribute,
                                    char const* const* names,
                                    char const* const* texts,
                                    std::size_t count) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || attribute == nullptr || !state().Attributes
        || count > 256) {
        return 3;
    }
    auto const* map = walked(object, kObjectRollConditions);
    if (map == nullptr) return 3;
    const int slot = hash_map_slot(map, attribute);

    std::vector<std::int32_t> entries;
    for (std::size_t i = 0; i < count; ++i) {
        const int condition = bg3le_stats_condition_intern(texts[i]);
        if (condition < 0) return 2;
        std::uint32_t name = 0;
        if (!bg3le_fixed_string_index_of(names[i], &name)
            && !bg3le_fixed_string_intern(names[i], &name)) {
            return 3;
        }
        entries.push_back((std::int32_t)name);
        entries.push_back(condition);
    }

    if (slot < 0) {
        if (count == 0) return 0;
        std::uint32_t key = 0;
        if (!bg3le_game_allocator_ready()
            || (!bg3le_fixed_string_index_of(attribute, &key)
                && !bg3le_fixed_string_intern(attribute, &key))) {
            return 1;
        }
        void* fresh = bg3se::GameAllocRaw(count * 8);
        if (fresh == nullptr) return 1;
        std::memcpy(fresh, entries.data(), entries.size() * sizeof(std::int32_t));
        struct { void* Buffer; std::uint32_t Capacity; std::uint32_t Size; } header{
            fresh, (std::uint32_t)count, (std::uint32_t)count};
        return fs_map_insert(const_cast<char*>(map), key, header) ? 0 : 1;
    }
    HashMapRef m{};
    if (!read_hash_map(map, &m)) return 3;
    auto* array = (char*)m.Values + (std::size_t)slot * 16;

    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    if (!read_as(array, &buffer) || !read_as(array + 8, &capacity)) return 3;
    if (count > 0 && (buffer == 0 || capacity < count)) {
        if (!bg3le_game_allocator_ready()) return 3;
        void* fresh = bg3se::GameAllocRaw(count * 8);
        if (fresh == nullptr) return 3;
        buffer = (std::uint64_t)(std::uintptr_t)fresh;
        const auto cap = (std::uint32_t)count;
        write_bytes(array, &buffer, sizeof(buffer));
        write_bytes(array + 8, &cap, sizeof(cap));
    }
    if (count > 0
        && !write_bytes((void*)(std::uintptr_t)buffer, entries.data(),
                        entries.size() * sizeof(std::int32_t))) {
        return 3;
    }
    const auto size = (std::uint32_t)count;
    return write_bytes(array + 12, &size, sizeof(size)) ? 0 : 3;
}

// One int32 into the object's indexed properties, which is what an
// attribute is.
extern "C" bool bg3le_stats_attr_set(void const* object, std::size_t index,
                                     int raw);

// Copies one stat object's attributes onto another, the way upstream's
// Object::CopyFrom does: it refuses across modifier lists, then assigns
// AIFlags and every IndexedProperties entry.
//
// The indexed properties are the whole of a stat's scalar surface -- every
// integer, enumeration, condition, GUID and string attribute is one raw
// int32 in that array -- so copying them element-wise is the same assignment
// upstream makes, not an approximation of it.
//
// A functor-typed attribute is an indexed property too -- its raw int is a
// handle into the engine's compiled table -- so copying the array carries the
// same reference upstream's property loop carries.
//
// Object::Functors and Object::RollConditions, the two maps upstream copies
// after the property loop, are carried by bg3le_stats_copy_rest.
extern "C" bool bg3le_stats_copy_from(void const* dest, void const* source,
                                      std::size_t* carried,
                                      std::size_t* total) {
    const CacheLock lock(stats_cache_lock());
    if (carried != nullptr) *carried = 0;
    if (total != nullptr) *total = 0;
    if (dest == nullptr || source == nullptr) return false;

    // Upstream's first check, and for its reason: two objects of different
    // modifier lists index their properties differently, so a copy across
    // them would assign every attribute to the wrong name.
    const int destList = bg3le_stats_list_index(dest);
    const int sourceList = bg3le_stats_list_index(source);
    if (destList < 0 || sourceList < 0 || destList != sourceList) {
        logf("stats: refusing to copy across modifier lists (%d into %d)",
             sourceList, destList);
        return false;
    }

    std::vector<std::int32_t> const* from = properties_of(source);
    if (from == nullptr) return false;
    // Copied out before writing: properties_of keeps one object's values, and
    // the write below invalidates that cache.
    const std::vector<std::int32_t> values = *from;

    std::vector<std::int32_t> const* to = properties_of(dest);
    if (to == nullptr) return false;
    const std::size_t n = std::min(values.size(), to->size());

    std::size_t written = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (bg3le_stats_attr_set(dest, i, values[i])) ++written;
    }
    if (carried != nullptr) *carried = written;

    // AIFlags is a FixedString on the object rather than an indexed
    // property, which is why upstream assigns it separately.
    std::uint32_t flags = 0;
    char* flagsAt = walked(dest, kObjectAIFlags);
    if (flagsAt != nullptr && read_as(walked(source, kObjectAIFlags), &flags)) {
        std::memcpy(flagsAt, &flags, sizeof(flags));
    }

    if (total != nullptr) *total = n;
    logf("stats: copied %zu of %zu indexed properties", written, n);
    return written == n;
}

extern "C" bool bg3le_stats_attr_set(void const* object, std::size_t index,
                                     int raw) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (!f.Attributes || object == nullptr) return false;

    void const* begin = nullptr;
    void const* end = nullptr;
    auto const* props = (char const*)object + f.PropsOffset;
    if (!read_as(props + 0, &begin)) return false;
    if (!read_as(props + 8, &end)) return false;
    if (begin == nullptr || end < begin) return false;

    const std::size_t count =
        (std::size_t)((char const*)end - (char const*)begin) / 4;
    if (index >= count) return false;

    const std::int32_t value = raw;
    if (!write_bytes((void*)((char*)begin + index * sizeof(std::int32_t)),
                     &value, sizeof(value))) {
        return false;
    }

    // The reader keeps one object's values, and this just changed them.
    if (property_cache().Object == object) property_cache().Object = nullptr;
    return true;
}

extern "C" char const* bg3le_stats_attr_condition(int raw) {
    const CacheLock lock(stats_cache_lock());
    Found const& f = state();
    if (raw <= 0 || f.Conditions.Buffer == nullptr) return nullptr;
    if ((std::size_t)raw >= f.Conditions.Size) return nullptr;

    // Kept rather than re-read. A condition is a Larian string, so
    // reading one means a read for the header and another through its
    // pointer, and whole families of stats share the same expression --
    // a mod that walks every spell asks for the same handful thousands of
    // times. Entries are only ever appended, so an index that has been
    // read once cannot change.
    static std::unordered_map<int, std::string> known;
    static std::uint32_t gen = ~0u;
    clear_on_rebuild(gen, [&] { known.clear(); });
    auto found = known.find(raw);
    if (found != known.end()) return found->second.c_str();

    std::string text;
    if (!read_ls_string((char const*)f.Conditions.Buffer
                            + (std::size_t)raw * 16, &text)) {
        return nullptr;
    }
    return known.emplace(raw, std::move(text)).first->second.c_str();
}

// Object::AIFlags, which upstream reads off the object rather than out of
// the string pool -- Object::GetString special-cases the AIFlags type.
// Reading the pool instead reported "CanNotUse" on a spell whose AIFlags is
// empty.
extern "C" char const* bg3le_stats_ai_flags(void const* object) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || !state().Attributes) return nullptr;
    std::uint32_t index = 0;
    if (!read_as(walked(object, kObjectAIFlags), &index)) return nullptr;
    char const* text = bg3le_fixed_string(index, nullptr);
    return text != nullptr ? text : "";
}

// How many roll conditions an attribute carries, and the n-th one's text
// key and expression. Upstream reports the attribute as a table keyed by
// the roll condition's name.
extern "C" int bg3le_stats_roll_condition_count(void const* object,
                                                char const* attribute) {
    const CacheLock lock(stats_cache_lock());
    if (object == nullptr || !state().Attributes) return -1;
    auto const* map = walked(object, kObjectRollConditions);
    if (map == nullptr) return -1;
    const int slot = hash_map_slot(map, attribute);
    if (slot < 0) return -1;

    HashMapRef m{};
    if (!read_hash_map(map, &m)) return -1;

    // Values[slot] is an Array<RollCondition>.
    std::uint32_t size = 0;
    if (!read_as((char const*)m.Values + (std::size_t)slot * 16 + 12,
                 &size)) {
        return -1;
    }
    return size > 4096 ? -1 : (int)size;
}

extern "C" bool bg3le_stats_roll_condition_at(void const* object,
                                              char const* attribute,
                                              int index,
                                              char const** nameOut,
                                              char const** textOut) {
    const CacheLock lock(stats_cache_lock());
    const int count = bg3le_stats_roll_condition_count(object, attribute);
    if (count < 0 || index < 0 || index >= count) return false;

    auto const* map = walked(object, kObjectRollConditions);
    if (map == nullptr) return false;
    const int slot = hash_map_slot(map, attribute);
    HashMapRef m{};
    if (slot < 0 || !read_hash_map(map, &m)) return false;

    void const* buffer = nullptr;
    if (!read_as((char const*)m.Values + (std::size_t)slot * 16, &buffer)
        || buffer == nullptr) {
        return false;
    }

    // RollCondition is { FixedString Name; ConditionId Conditions }, and
    // ConditionId is one int32 indexing the condition pool.
    auto const* entry = (char const*)buffer + (std::size_t)index * 8;
    std::uint32_t name = 0;
    std::int32_t condition = 0;
    if (!read_as(entry, &name) || !read_as(entry + 4, &condition)) {
        return false;
    }

    if (nameOut != nullptr) {
        char const* text = bg3le_fixed_string(name, nullptr);
        *nameOut = text != nullptr ? text : "";
    }
    if (textOut != nullptr) *textOut = bg3le_stats_attr_condition(condition);
    return true;
}


// ---- structure edits: upstream's AddEnumerationValue and AddAttribute ----

// A value list's index in ModifierValueLists, or -1.
int value_list_index(char const* name) {
    Found const& f = state();
    void const* wanted = value_list_named(name);
    if (wanted == nullptr) return -1;
    for (std::uint32_t i = 0; i < f.ValueLists.Size; ++i) {
        void const* list = nullptr;
        if (read_as((char const*)f.ValueLists.Buffer + i * sizeof(void*), &list) && list == wanted) {
            return (int)i;
        }
    }
    return -1;
}

// Upstream's AddEnumerationValue: the label, valued at the count before it.
// RPGEnumeration::Values is a LegacyMap; the new node goes at the head of its
// bucket, whose rule -- string index or string hash, modulo the table size --
// is read off the nodes already there rather than assumed.
extern "C" bool bg3le_stats_enum_add(char const* typeName, char const* label,
                                     int* valueOut, char const** err) {
    const CacheLock lock(stats_cache_lock());
    static std::string why;
    auto fail = [&](std::string const& message) {
        why = message;
        *err = why.c_str();
        return false;
    };
    if (!ready()) return fail("the stats manager is not up yet");
    void const* list = value_list_named(typeName);
    if (list == nullptr) return fail(std::string("No such stats value type: ") + typeName);
    if (property_type(list) != 4) {
        return fail(std::string("Stats value type is not an enumeration: ") + typeName);
    }

    struct Map { std::uint32_t HashSize; void** HashTable; std::uint32_t ItemCount; };
    struct Node { Node* Next; std::uint32_t Key; std::int32_t Value; };
    auto* map = (Map*)((char*)list + state().ValueNameOffset + 8);
    Map m{};
    if (!read_as(map, &m) || m.HashSize == 0 || m.HashSize > (1u << 20) || m.HashTable == nullptr) {
        return fail("the value list's map could not be read");
    }

    std::uint32_t id = 0;
    if (!bg3le_fixed_string_index_of(label, &id) && !bg3le_fixed_string_intern(label, &id)) {
        return fail("the label could not be interned");
    }

    std::vector<void*> heads(m.HashSize);
    if (!safe_read(m.HashTable, heads.data(), heads.size() * sizeof(void*))) {
        return fail("the value list's map could not be read");
    }
    bool byIndex = true, byHash = true;
    std::uint32_t seen = 0;
    for (std::uint32_t b = 0; b < m.HashSize; ++b) {
        auto* node = (Node*)heads[b];
        for (int guard = 0; node != nullptr && guard < 4096; ++guard) {
            Node copy{};
            if (!read_as(node, &copy)) return fail("the value list's map could not be read");
            if (copy.Key == id) {
                // Upstream's message names the type, not the label.
                return fail(std::string("Stats value type already has a value named '") + typeName + "'");
            }
            std::uint32_t hash = 0;
            if (copy.Key % m.HashSize != b) byIndex = false;
            if (!bg3le_fixed_string_hash(copy.Key, &hash) || hash % m.HashSize != b) byHash = false;
            ++seen;
            node = copy.Next;
        }
    }
    std::uint32_t hash = 0;
    std::uint32_t bucket = 0;
    if (seen > 0 && byIndex) {
        bucket = id % m.HashSize;
    } else if (seen > 0 && byHash && bg3le_fixed_string_hash(id, &hash)) {
        bucket = hash % m.HashSize;
    } else {
        return fail("the value list's buckets follow no rule bg3le can reproduce");
    }

    auto* node = (Node*)bg3se::GameAllocRaw(sizeof(Node));
    if (node == nullptr) return fail("out of memory");
    node->Next = (Node*)heads[bucket];
    node->Key = id;
    node->Value = (std::int32_t)m.ItemCount;
    m.HashTable[bucket] = node;
    map->ItemCount = m.ItemCount + 1;
    *valueOut = node->Value;
    return true;
}

// Upstream's AddAttribute, which it allows only before any stats object
// exists; after that, its two messages.
extern "C" bool bg3le_stats_attr_add(char const* listName, char const* modifierName,
                                     char const* typeName, char const** err) {
    static std::string why;
    auto fail = [&](std::string const& message) {
        why = message;
        *err = why.c_str();
        return false;
    };
    {
        const CacheLock lock(stats_cache_lock());
        if (ready() && state().Objects.Size > 0) {
            return fail("It is not safe to modify stats types after stats data files were loaded!\n"
                        "(Try using the StatsStructureLoaded event)");
        }
    }
    const int list = bg3le_stats_list_handle(listName);
    if (list < 0) return fail(std::string("No such modifier list: ") + listName);
    for (std::size_t i = 0, n = bg3le_stats_list_attr_count(listName); i < n; ++i) {
        char const* existing = nullptr;
        if (bg3le_stats_list_attr_at(listName, i, &existing, nullptr) && existing != nullptr
            && std::strcmp(existing, modifierName) == 0) {
            return fail(std::string("Modifier list already has an attribute named '") + modifierName + "'");
        }
    }
    const CacheLock lock(stats_cache_lock());
    const int valueList = value_list_index(typeName);
    if (valueList < 0) return fail(std::string("No such stats value type: ") + typeName);

    Found const& f = state();
    void const* listObject = nullptr;
    if (!read_as((char const*)f.Lists.Buffer + (std::size_t)list * sizeof(void*), &listObject)
        || listObject == nullptr) {
        return fail("the modifier list could not be read");
    }
    std::uint32_t nameId = 0;
    if (!bg3le_fixed_string_index_of(modifierName, &nameId)
        && !bg3le_fixed_string_intern(modifierName, &nameId)) {
        return fail("the attribute name could not be interned");
    }

    using Manager = bg3se::stats::CNamedElementManager<bg3se::stats::Modifier>;
    auto* modifier = (bg3se::stats::Modifier*)bg3se::GameAllocRaw(sizeof(bg3se::stats::Modifier));
    if (modifier == nullptr) return fail("out of memory");
    std::memset((void*)modifier, 0, sizeof(*modifier));
    modifier->EnumerationIndex = valueList;
    modifier->LevelMapIndex = -1;
    std::memcpy(&modifier->Name, &nameId, sizeof(nameId));

    auto* manager = (char*)listObject + f.AttrsOffset - offsetof(Manager, Values);
    std::int32_t next = 0;
    if (!read_as(manager + offsetof(Manager, NextHandle), &next)
        || !array_append<void*>(manager + offsetof(Manager, Values), modifier)
        || !fs_map_insert<std::int32_t>(manager + offsetof(Manager, NameToHandle), nameId, next)) {
        return fail("the modifier list's attributes could not be extended");
    }
    const std::int32_t grown = next + 1;
    std::memcpy(manager + offsetof(Manager, NextHandle), &grown, sizeof(grown));
    return true;
}

}  // namespace bg3le
