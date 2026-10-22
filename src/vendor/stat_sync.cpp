// stat:Sync() and Ext.Stats.Sync, as upstream's
// RPGStats::SyncWithPrototypeManager: a spell, status or interrupt prototype
// is reset the way bg3se's SyncStat resets it, then rebuilt by the engine's
// own Init from the stat. Adapted from bg3se's GameDefinitions/Stats/Stats.cpp,
// by Norbyte and the bg3se contributors -- thank you.
//
// None of the Init functions has a symbol. Their offsets come from the
// relocations the linker kept (tools/find-prototype-inits.py) and are checked
// before each call: the opening bytes must match, and where a function loads
// the RPGStats global, that global must hold the stats manager bg3le found.
// See reference/STAT-WRITES.md.

#include <stdafx.h>

#include <GameDefinitions/Stats/Stats.h>
#include <GameDefinitions/Stats/Prototype.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../log.h"
#include "../resolve.h"
#include "../targets.h"
#include "../mem.h"
#include "engine_containers.h"

namespace bg3le {
std::uintptr_t load_bias();
}

extern "C" void* bg3le_stats_manager();
extern "C" void* bg3le_stats_find(char const* name);
extern "C" char const* bg3le_stats_type(void const* object);
extern "C" int bg3le_stats_attr_index(void const* object, char const* wanted);
extern "C" bool bg3le_stats_attr_at(void const* object, std::size_t index,
                                    char const** nameOut, char const** typeNameOut,
                                    int* kindOut, int* rawOut);
extern "C" char const* bg3le_stats_attr_label(void const* object,
                                              std::size_t index, int raw);
extern "C" char const* bg3le_stats_attr_string(int raw);
extern "C" void* bg3le_prototype_find(int kind, char const* name);
extern "C" void* bg3le_prototype_map(int kind);
extern "C" void bg3le_prototype_added(int kind, char const* name, void* prototype);
extern "C" bool bg3le_meta_enum_label_value(char const* enumName,
                                           char const* label,
                                           std::uint64_t* value);

namespace bg3le {
namespace {

using namespace bg3se;
using namespace bg3se::stats;

// The engine's layouts, from the loaders that allocate and fill them.
static_assert(sizeof(SpellPrototype) == 0x338);
static_assert(offsetof(SpellPrototype, SteerSpeedMultipler) == 0x330);
static_assert(sizeof(StatusPrototype) == 0x110);
static_assert(offsetof(StatusPrototype, Boosts) == 0xc0);
static_assert(sizeof(InterruptPrototype) == 0x1f0);
static_assert(offsetof(InterruptPrototype, Name) == 0);
static_assert(offsetof(Object, Name) == 0x20);
// The passive loader allocates 0x220-byte nodes: next, key, prototype.
static_assert(sizeof(PassivePrototype) == 0x210);
static_assert(offsetof(PassivePrototypeManager, Initialized) == 0x18);
static_assert(sizeof(Array<int>) == 16);

// The global every Init reads first: RPGStats, whose Objects array buffer
// is at +0xc8 -- the array bg3le found by content.
constexpr std::size_t kStatsInHolder = 0xc8;

struct InitFn {
    char const* Name;
    std::uintptr_t (*Offset)();  // targets.h
    unsigned char Bytes[36];
    std::size_t Length;
    std::size_t StatsDisp;   // offset of a disp32 that loads bg3le::target::StatsGlobal(), or 0
};

const InitFn kSpellInit = {
    "eoc::SpellPrototype::Init", &bg3le::target::SpellPrototypeInit,
    {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
     0x48, 0x81, 0xec, 0xe8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0x1d},
    20, 20};
const InitFn kStatusInit = {
    "eoc::StatusPrototype::Init", &bg3le::target::StatusPrototypeInit,
    {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
     0x48, 0x83, 0xec, 0x38, 0x4c, 0x8b, 0x2d},
    17, 17};
const InitFn kInterruptInit = {
    "eoc::InterruptPrototype::Init", &bg3le::target::InterruptPrototypeInit,
    {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
     0x48, 0x83, 0xec, 0x18, 0x8b, 0x07, 0x8b, 0x6e, 0x20, 0x49,
     0x89, 0xf6, 0x48, 0x89, 0xfb, 0x39, 0xe8},
    27, 0};

// The passive loader, which builds every passive missing from the manager's
// map inline; there is no PassivePrototype::Init on this build.
const InitFn kPassiveLoader = {
    "the passive loader", &bg3le::target::PassiveLoader,
    {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
     0x48, 0x81, 0xec, 0xb8, 0x00, 0x00, 0x00, 0x80, 0x7f, 0x18,
     0x00, 0x48, 0x89, 0xfb, 0x0f, 0x85, 0x1a, 0x14, 0x00, 0x00,
     0x4c, 0x8b, 0x3d},
    33, 33};

// The status loader's boost parse, which upstream calls ParseStaticBoosts:
// the parser, then the three functions of the callback it is handed.
const InitFn kBoostParse = {
    "the static boost parser", &bg3le::target::BoostParse,
    {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
     0x48, 0x83, 0xec, 0x58, 0x85, 0xf6, 0x74, 0x2d},
    18, 0};
const InitFn kBoostInvoke = {
    "the boost parse callback", &bg3le::target::BoostInvoke,
    {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53,
     0x48, 0x83, 0xec, 0x68, 0x48, 0x89, 0xfb, 0x48, 0x8b, 0x7f, 0x18},
    21, 0};
const InitFn kBoostCopy = {
    "the boost parse callback's copy", &bg3le::target::BoostCopy,
    {0x0f, 0x10, 0x46, 0x18, 0x48, 0x89, 0xd0, 0x0f, 0x11, 0x42, 0x18},
    11, 0};
const InitFn kBoostManage = {
    "the boost parse callback's manager", &bg3le::target::BoostManage,
    {0x48, 0x89, 0xd0, 0x48, 0x85, 0xd2, 0x75, 0x01, 0xc3},
    9, 0};

// The function's address if it is the one this build was read from.
void* verified(InitFn const& fn) {
    const std::uintptr_t bias = load_bias();
    // The bytes near its start; StatsDisp counts from where they matched.
    const std::uintptr_t at = code_near_n(fn.Offset(), fn.Bytes, fn.Length);
    if (at == 0) {
        logf("stat sync: %s was not found on this build", fn.Name);
        return nullptr;
    }
    auto const* code = reinterpret_cast<unsigned char const*>(bias + at);
    if (fn.StatsDisp != 0) {
        std::int32_t disp = 0;
        char const* holder = nullptr;
        void* stats = nullptr;
        const std::uintptr_t next = (std::uintptr_t)code + fn.StatsDisp + 4;
        if (!safe_read(code + fn.StatsDisp, &disp, sizeof(disp))
            || next + disp != bias + bg3le::target::StatsGlobal()
            || !safe_read((void const*)(bias + bg3le::target::StatsGlobal()), &holder, sizeof(holder))
            || holder == nullptr
            || !safe_read(holder + kStatsInHolder, &stats, sizeof(stats))
            || stats == nullptr || stats != bg3le_stats_manager()) {
            logf("stat sync: %s does not reach the stats manager bg3le found "
                 "(image+%#lx -> %p -> %p; the manager is %p)", fn.Name,
                 (unsigned long)(next + disp - bias), (void const*)holder, stats,
                 bg3le_stats_manager());
            return nullptr;
        }
    }
    return (void*)code;
}

// Empties an array without running destructors, which would release strings
// through engine calls bg3le does not have; the buffer stays the engine's.
template <class T>
void forget(Array<T>& a) {
    std::uint32_t zero = 0;
    std::memcpy((char*)&a + 12, &zero, sizeof(zero));
}

void reset_animation(SpellPrototypeAnimationData& a) {
    for (auto* part : {&a.Part0, &a.Part1, &a.Part3, &a.Part4, &a.Part5,
                       &a.Part6, &a.Part7, &a.Part8}) {
        std::memset(part->data(), 0xff, sizeof(*part));
    }
    forget(a.Part2);
    a.Flags = 0;
}

// A FixedString attribute's text, as upstream's GetFixedString reads it, or
// an enumeration's label.
char const* attr_label(void const* object, char const* name) {
    const int index = bg3le_stats_attr_index(object, name);
    if (index < 0) return nullptr;
    int raw = 0;
    if (!bg3le_stats_attr_at(object, (std::size_t)index, nullptr, nullptr, nullptr, &raw)) {
        return nullptr;
    }
    char const* text = bg3le_stats_attr_string(raw);
    return text != nullptr ? text : bg3le_stats_attr_label(object, (std::size_t)index, raw);
}

template <class E>
E enum_of(char const* enumName, char const* label) {
    std::uint64_t value = 0;
    if (label == nullptr || !bg3le_meta_enum_label_value(enumName, label, &value)) {
        return (E)0;
    }
    return (E)value;
}

char const* sync_spell(Object* object, SpellPrototype* proto) {
    auto* init = (void (*)(SpellPrototype*, FixedString const*))verified(kSpellInit);
    if (init == nullptr) return "the engine's SpellPrototype::Init is not where this build has it";

    proto->SpellTypeId = enum_of<SpellType>("SpellType", attr_label(object, "SpellType"));    forget(proto->UseCosts);
    forget(proto->RitualCosts);
    forget(proto->DualWieldingUseCosts);
    forget(proto->HitCostGroups);
    forget(proto->VariableUseCosts);
    forget(proto->VariableDualWieldingUseCosts);
    forget(proto->VariableRitualCosts);
    reset_animation(proto->SpellAnimation);
    reset_animation(proto->DualWieldingSpellAnimation);
    forget(proto->AlternativeCastTextEvents);
    forget(proto->ContainerSpells);
    forget(proto->Trajectories);
    proto->SpellFlags = (SpellFlags)0;
    proto->LineOfSightFlags = 0;
    proto->CinematicArenaFlags = 0;
    proto->WeaponTypes = 0;
    proto->AiFlags = 0;
    proto->RequirementEvents = 0;
    proto->IsWeaponAttack = false;

    init(proto, &object->Name);
    return nullptr;
}

// The callback the loader hands the boost parser, as it lays it out: a
// pointer to the implementation, which is the inline storage right after it.
struct BoostSink {
    void* Invoke;
    void* Copy;
    void* Manage;
    void* Scratch;
    Array<Guid>* Boosts;
    std::uint64_t Tail[2];
};
struct BoostFunction {
    BoostSink* Impl;
    BoostSink Sink;
};
static_assert(offsetof(BoostFunction, Sink) == 8);
using ManageProc = void (*)(void* self, void* storage, void* into);

// As the status loader parses a status's Boosts after Init: into the emptied
// array, through a scratch buffer released afterwards.
char const* parse_boosts(void const* object, Array<Guid>* boosts) {
    auto* parse = (void (*)(char const*, std::uint32_t, BoostFunction*))verified(kBoostParse);
    void* invoke = verified(kBoostInvoke);
    void* copy = verified(kBoostCopy);
    void* manage = verified(kBoostManage);
    if (parse == nullptr || invoke == nullptr || copy == nullptr || manage == nullptr) {
        return "the engine's boost parser is not where this build has it";
    }

    forget(*boosts);
    const int index = bg3le_stats_attr_index(object, "Boosts");
    int raw = 0;
    if (index < 0 || !bg3le_stats_attr_at(object, (std::size_t)index, nullptr,
                                          nullptr, nullptr, &raw)) {
        return nullptr;
    }
    char const* text = bg3le_stats_attr_string(raw);
    if (text == nullptr) return nullptr;

    alignas(16) unsigned char scratch[64] = {};
    BoostFunction fn{nullptr, {invoke, copy, manage, scratch, boosts, {0, 0}}};
    fn.Impl = &fn.Sink;
    parse(text, (std::uint32_t)std::strlen(text), &fn);
    if (fn.Impl != nullptr) ((ManageProc)fn.Impl->Manage)(fn.Impl, &fn.Sink, nullptr);
    void* held = nullptr;
    std::memcpy(&held, scratch, sizeof(held));
    if (held != nullptr) {
        ManageProc release = nullptr;
        std::memcpy(&release, (char*)held + 0x10, sizeof(release));
        release(held, scratch + 8, nullptr);
    }
    return nullptr;
}

char const* sync_status(Object* object, StatusPrototype* proto) {
    auto* init = (void (*)(StatusPrototype*, FixedString const*, std::uint8_t))
        verified(kStatusInit);
    if (init == nullptr) return "the engine's StatusPrototype::Init is not where this build has it";

    proto->StatusId = enum_of<StatusType>("StatusType", attr_label(object, "StatusType"));
    proto->StatusPropertyFlags = 0;
    proto->StatusGroups = 0;
    // The loader clears only bit 0 before Init; the rest are set by a later
    // pass that a sync does not repeat, so they are kept.
    proto->Flags &= ~1u;
    proto->RemoveEvents = 0;
    forget(proto->Boosts);

    init(proto, &object->Name, 0);
    return parse_boosts(object, &proto->Boosts);
}

char const* sync_interrupt(Object* object, InterruptPrototype* proto) {
    auto* init = (void (*)(InterruptPrototype*, Object*))verified(kInterruptInit);
    if (init == nullptr) return "the engine's InterruptPrototype::Init is not where this build has it";

    forget(proto->Costs);
    init(proto, object);
    return nullptr;
}

}  // namespace
}  // namespace bg3le

// RPGStats, if the global still holds the manager bg3le found.
extern "C" void* bg3le_rpgstats() {
    const std::uintptr_t bias = bg3le::load_bias();
    char* rpg = nullptr;
    void* objects = nullptr;
    if (bg3le::target::StatsGlobal() == 0
        || !bg3le::safe_read((void const*)(bias + bg3le::target::StatsGlobal()), &rpg, sizeof(rpg))
        || rpg == nullptr
        || !bg3le::safe_read(rpg + bg3le::kStatsInHolder, &objects, sizeof(objects))
        || objects == nullptr || objects != bg3le_stats_manager()) {
        return nullptr;
    }
    return rpg;
}

namespace bg3le {
namespace {

static_assert(offsetof(SpellPrototypeManager, SpellNames) - offsetof(SpellPrototypeManager, Spells) == 0x80);
static_assert(offsetof(StatusPrototypeManager, StatusNames) - offsetof(StatusPrototypeManager, Statuses) == 0x40);

// Upstream's new-prototype branch of SyncStat: a default prototype, synced,
// then added to the manager's map and name list.
template <class P>
P* fresh_prototype() {
    auto* p = (P*)bg3se::GameAllocRaw(sizeof(P));
    if (p == nullptr) return nullptr;
    std::memset((void*)p, 0, sizeof(P));
    new (p) P;
    return p;
}

char const* add_spell(Object* object, char const* name) {
    auto* map = (char*)bg3le_prototype_map(0);
    if (map == nullptr) return "the spell prototype manager is not located";
    auto* proto = fresh_prototype<SpellPrototype>();
    if (proto == nullptr) return "out of memory";
    if (char const* err = sync_spell(object, proto)) return err;
    std::uint32_t id = 0;
    std::memcpy(&id, &object->Name, sizeof(id));
    if (!fs_map_insert<void*>(map, id, proto)) return "the spell map does not hash the way bg3le reads it";
    array_append<std::uint32_t>(map + 0x80, id);
    bg3le_prototype_added(0, name, proto);
    return nullptr;
}

char const* add_status(Object* object, char const* name) {
    auto* map = (char*)bg3le_prototype_map(1);
    if (map == nullptr) return "the status prototype manager is not located";
    auto* proto = fresh_prototype<StatusPrototype>();
    if (proto == nullptr) return "out of memory";
    if (char const* err = sync_status(object, proto)) return err;
    std::uint32_t id = 0;
    std::memcpy(&id, &object->Name, sizeof(id));
    if (!fs_map_insert<void*>(map, id, proto)) return "the status map does not hash the way bg3le reads it";
    array_append<std::uint32_t>(map + 0x40, id);
    bg3le_prototype_added(1, name, proto);
    return nullptr;
}

// Interrupts are held in the map itself, so the default one goes in first
// and is synced where it lands, as upstream's add_key then SyncStat do.
char const* add_interrupt(Object* object, char const* name) {
    auto* map = (char*)bg3le_prototype_map(2);
    if (map == nullptr) return "the interrupt prototype manager is not located";
    struct Raw { alignas(InterruptPrototype) unsigned char Bytes[sizeof(InterruptPrototype)]; } raw;
    std::memset(raw.Bytes, 0, sizeof(raw.Bytes));
    new (raw.Bytes) InterruptPrototype;
    std::uint32_t id = 0;
    std::memcpy(&id, &object->Name, sizeof(id));
    void* slot = nullptr;
    if (!fs_map_insert<Raw>(map, id, raw, &slot) || slot == nullptr) {
        return "the interrupt map does not hash the way bg3le reads it";
    }
    if (char const* err = sync_interrupt(object, (InterruptPrototype*)slot)) return err;
    bg3le_prototype_added(2, name, slot);
    return nullptr;
}

// A passive's node in the manager's chained map.
struct PassiveNode {
    PassiveNode* Next;
    std::uint32_t Key;
    std::uint32_t Pad;
    PassivePrototype Value;
};
static_assert(sizeof(PassiveNode) == 0x220);

// RefMapInternals, whose members LegacyRefMap inherits privately.
struct RawRefMap {
    std::uint32_t ItemCount;
    std::uint32_t HashSize;
    PassiveNode** HashTable;
};
static_assert(sizeof(RawRefMap) == sizeof(PassivePrototypeManager::Passives));

RawRefMap& raw_map(PassivePrototypeManager* mgr) {
    return *reinterpret_cast<RawRefMap*>(&mgr->Passives);
}

PassiveNode** passive_bucket(PassivePrototypeManager* mgr, std::uint32_t key) {
    RawRefMap& map = raw_map(mgr);
    if (map.HashSize == 0 || map.HashTable == nullptr) return nullptr;
    return &map.HashTable[key % map.HashSize];
}

bool passive_unlink(PassivePrototypeManager* mgr, PassiveNode* node) {
    PassiveNode** link = passive_bucket(mgr, node->Key);
    for (; link != nullptr && *link != nullptr; link = &(*link)->Next) {
        if (*link == node) {
            *link = node->Next;
            --raw_map(mgr).ItemCount;
            return true;
        }
    }
    return false;
}

PassiveNode* passive_node(PassivePrototypeManager* mgr, std::uint32_t key) {
    PassiveNode** link = passive_bucket(mgr, key);
    PassiveNode* node = link != nullptr ? *link : nullptr;
    while (node != nullptr && node->Key != key) node = node->Next;
    return node;
}

// Upstream resets a passive and calls PassivePrototype::Init. Here the
// loader builds it again: the old node leaves the map, the loader adds a
// fresh one, and its prototype moves into the old node so the address holds.
char const* sync_passive(Object* object, char const* name) {
    auto* loader = (void (*)(PassivePrototypeManager*))verified(kPassiveLoader);
    if (loader == nullptr) return "the engine's passive loader is not where this build has it";
    auto* map = (char*)bg3le_prototype_map(3);
    if (map == nullptr) return "the passive prototype manager is not located";
    auto* mgr = (PassivePrototypeManager*)(map - offsetof(PassivePrototypeManager, Passives));

    std::uint32_t key = 0;
    std::memcpy(&key, &object->Name, sizeof(key));
    PassiveNode* old = passive_node(mgr, key);
    if (old != nullptr && !passive_unlink(mgr, old)) return "the passive map does not chain the way bg3le reads it";

    mgr->Initialized = false;
    loader(mgr);
    PassiveNode* fresh = passive_node(mgr, key);
    if (fresh == nullptr) {
        if (old != nullptr) {
            PassiveNode** link = passive_bucket(mgr, key);
            old->Next = *link;
            *link = old;
            ++raw_map(mgr).ItemCount;
        }
        return "the passive loader did not rebuild the passive";
    }
    if (old == nullptr) {
        bg3le_prototype_added(3, name, &fresh->Value);
        return nullptr;
    }

    // What the old prototype held is left, not freed.
    passive_unlink(mgr, fresh);
    std::memcpy((void*)&old->Value, (void const*)&fresh->Value, sizeof(PassivePrototype));
    PassiveNode** link = passive_bucket(mgr, key);
    old->Next = *link;
    *link = old;
    ++raw_map(mgr).ItemCount;
    return nullptr;
}

}  // namespace
}  // namespace bg3le

// Rebuilds the prototype of the named stat. Returns nullptr on success, or
// when the stat has no prototype to rebuild (upstream does nothing there
// either); otherwise why it could not.
extern "C" bool bg3le_stats_is_loading();

extern "C" char const* bg3le_stats_sync(char const* name) {
    using namespace bg3le;
    auto* object = (bg3se::stats::Object*)bg3le_stats_find(name);
    if (object == nullptr) return "no such stat";
    // The engine builds every prototype from the stats once RPGStats::Load returns.
    if (bg3le_stats_is_loading()) {
        // Debug: who syncs during the load, and how often.
        static std::atomic<unsigned> said{0};
        const unsigned n = ++said;
        if (n <= 20 || n % 500 == 0) {
            logf("stats debug: Sync(%s) inside RPGStats::Load, left to the engine's prototype build (%u so far)",
                 name, n);
        }
        return nullptr;
    }
    char const* type = bg3le_stats_type(object);
    if (type == nullptr) return nullptr;

    if (std::strcmp(type, "SpellData") == 0) {
        auto* proto = (bg3se::stats::SpellPrototype*)bg3le_prototype_find(0, name);
        if (proto == nullptr) return add_spell(object, name);
        return sync_spell(object, proto);
    }
    if (std::strcmp(type, "StatusData") == 0) {
        auto* proto = (bg3se::stats::StatusPrototype*)bg3le_prototype_find(1, name);
        if (proto == nullptr) return add_status(object, name);
        return sync_status(object, proto);
    }
    if (std::strcmp(type, "InterruptData") == 0) {
        auto* proto = (bg3se::stats::InterruptPrototype*)bg3le_prototype_find(2, name);
        if (proto == nullptr) return add_interrupt(object, name);
        return sync_interrupt(object, proto);
    }
    if (std::strcmp(type, "PassiveData") == 0) {
        return sync_passive(object, name);
    }
    return nullptr;
}
