// A field table for every class bg3se describes, built from its own generated
// metadata.
//
// bg3se already knows the name, offset and type of every field of every
// component -- 3,071 classes and some 12,500 offset-based fields, in
// GameDefinitions/Generated/PropertyMaps.inl. Hand-writing an accessor per
// component throws all of that away.
//
// Its runtime property maps are not usable here, though. They are keyed by
// FixedString, which is a 32-bit index into the engine's global string table,
// and that table has no symbol and no entry point: ls::FixedString's methods
// are all inlined in the native build, so there is nothing to borrow the way
// the allocator was. The property getters also take a LifetimeHandle and push
// through bg3se's Lua State, which bg3le does not have.
//
// None of that is needed. PropertyMaps.inl is macro-driven, and upstream
// #defines and #undefs the macros around its own include of it, so the same
// file can be included again here with different definitions. The offsets come
// from offsetof and the types from decltype, so this is the compiler's own
// layout knowledge rather than a restatement of it -- the same reason the
// hand-written Health accessors were trustworthy, applied to everything at
// once.
//
// What comes out is a plain table: no FixedString, no Lua, no lifetimes. Field
// names are the string literals from the generated file, so they need no
// interning and outlive everything.
//
// The generated metadata and the definitions it describes are by Norbyte and
// the bg3se contributors (https://github.com/Norbyte/bg3se); only this
// re-expansion of it is ours. With thanks to them.

// stdafx.h first, then the dependency header, exactly as upstream's own
// translation unit orders them; the generated metadata relies on it.
#include <stdafx.h>

#include <Lua/Shared/Proxies/PropertyMapDependencies.h>

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../component_meta_abi.h"
#include "../log.h"
#include "../mem.h"

// From platform_linux.cpp; the self-test below needs an allocator to build an
// array with.
extern "C" bool bg3le_game_allocator_ready();
extern "C" bool bg3le_fixed_string_hash(std::uint32_t id, std::uint32_t* out);

namespace bg3le {

using namespace bg3se;

// The name of a type, taken from the compiler's own function-name string.
//
// This is how a field's type gets recorded without its field table having to
// be complete at that point. Requiring completeness would mean a field could
// only be described if its type happened to be declared earlier in the
// generated file, which is not how that file is ordered. Instead both a
// class's own name and a field's type name come from here, so they are written
// by the same scheme and compare exactly, and a field type that turns out to
// have no table of its own simply fails to resolve at load.
template <class T>
constexpr std::string_view type_name() {
    std::string_view p = __PRETTY_FUNCTION__;
    const auto start = p.find("T = ") + 4;
    const auto end = p.rfind(']');
    return p.substr(start, end - start);
}

// std::array is how the engine stores the per-ability and per-skill tables,
// so it is worth recognising rather than reporting as unsupported.
template <class T>
struct ArrayTraits {
    static constexpr bool kIsArray = false;
    using Elem = void;
    static constexpr std::size_t kCount = 0;
};

template <class T, std::size_t N>
struct ArrayTraits<std::array<T, N>> {
    static constexpr bool kIsArray = true;
    using Elem = T;
    static constexpr std::size_t kCount = N;
};

// glm's vectors and quaternions are fixed runs of floats, and they carry
// everything positional in the engine -- Bound.Translate is a glm::vec3, so
// without this the position of anything in the world reads as unsupported.
// They are plain aggregates, so they read exactly as a fixed-extent array
// does.
template <class T>
struct GlmTraits {
    static constexpr bool kIsGlm = false;
    using Elem = void;
    static constexpr std::size_t kCount = 0;
};

template <glm::length_t N, class T, glm::qualifier Q>
struct GlmTraits<glm::vec<N, T, Q>> {
    static constexpr bool kIsGlm = true;
    using Elem = T;
    static constexpr std::size_t kCount = (std::size_t)N;
};

// A matrix, as upstream pushes one: its C*R floats in memory order.
template <glm::length_t C, glm::length_t R, class T, glm::qualifier Q>
struct GlmTraits<glm::mat<C, R, T, Q>> {
    static constexpr bool kIsGlm = true;
    using Elem = T;
    static constexpr std::size_t kCount = (std::size_t)C * (std::size_t)R;
};

template <class T, glm::qualifier Q>
struct GlmTraits<glm::qua<T, Q>> {
    static constexpr bool kIsGlm = true;
    using Elem = T;
    static constexpr std::size_t kCount = 4;
};

// bg3se's Array is the engine's dynamically sized array. Its length and buffer
// members are private, so they are reached through size() and data(), which
// are public and constexpr -- instantiated per field type below, so no member
// offset is ever guessed at.
template <class T>
struct VectorTraits {
    static constexpr bool kIsVector = false;
    using Elem = void;
};

template <class T>
struct VectorTraits<Array<T>> {
    static constexpr bool kIsVector = true;
    using Elem = T;
};

// A LegacyArray is an Array<T> behind a vtable; upstream resizes it through
// that base, and so does this.
template <class T>
struct LegacyArrayTraits {
    static constexpr bool kIs = false;
    using Elem = void;
};
template <class T>
struct LegacyArrayTraits<LegacyArray<T>> {
    static constexpr bool kIs = true;
    using Elem = T;
};

// Arrays written in place but never resized: StaticArray has no grow, and a
// std::vector's buffer belongs to whichever allocator its owner uses.
template <class T>
struct FixedVectorTraits {
    static constexpr bool kIs = false;
    using Elem = void;
};
template <class T>
struct FixedVectorTraits<StaticArray<T>> {
    static constexpr bool kIs = true;
    using Elem = T;
};
template <class T, class A>
struct FixedVectorTraits<std::vector<T, A>> {
    static constexpr bool kIs = true;
    using Elem = T;
};

template <class T>
struct BitArrayTraits {
    static constexpr bool kIs = false;
};
template <class TWord, unsigned Words>
struct BitArrayTraits<BitArray<TWord, Words>> {
    static constexpr bool kIs = true;
    static constexpr unsigned kBits = BitArray<TWord, Words>::NumBits;
    static_assert(kBits <= 0xffff);
};

// Queue, a ring buffer: read in order through operator[], never written.
template <class T>
struct QueueTraits {
    static constexpr bool kIs = false;
    using Elem = void;
};
template <class T>
struct QueueTraits<Queue<T>> {
    static constexpr bool kIs = true;
    using Elem = T;
};

// Copies n bytes of text that may not be readable.
bool copy_text(char const* data, std::size_t n, std::string* out) {
    if (n > (1u << 24)) return false;
    out->resize(n);
    return n == 0 || safe_read(data, out->data(), n);
}

// Text in its several forms, each read the way upstream pushes it.
template <class T>
struct TextTraits {
    static constexpr bool kIsText = false;
};
template <>
struct TextTraits<char const*> {
    static constexpr bool kIsText = true;
    static bool read(void const* field, std::string* out) {
        char const* text = nullptr;
        if (!safe_read(field, &text, sizeof(text)) || text == nullptr) return false;
        out->clear();
        char chunk[256];
        for (std::size_t at = 0; at < (1u << 20); at += sizeof(chunk)) {
            const std::size_t got = safe_read_some(text + at, chunk, sizeof(chunk));
            if (got == 0) return !out->empty();
            char const* end = (char const*)std::memchr(chunk, 0, got);
            out->append(chunk, end != nullptr ? (std::size_t)(end - chunk) : got);
            if (end != nullptr || got < sizeof(chunk)) return true;
        }
        return true;
    }
};
template <>
struct TextTraits<char*> : TextTraits<char const*> {};
template <>
struct TextTraits<std::string_view> {
    static constexpr bool kIsText = true;
    static bool read(void const* field, std::string* out) {
        auto const* v = static_cast<std::string_view const*>(field);
        return copy_text(v->data(), v->size(), out);
    }
};
template <class C>
struct TextTraits<LSBaseStringView<C>> {
    static constexpr bool kIsText = sizeof(C) == 1;
    static bool read(void const* field, std::string* out) {
        auto const* v = static_cast<LSBaseStringView<C> const*>(field);
        return copy_text((char const*)v->data(), v->size(), out);
    }
};
template <>
struct TextTraits<ScratchBuffer> {
    static constexpr bool kIsText = true;
    static bool read(void const* field, std::string* out) {
        auto const* v = static_cast<ScratchBuffer const*>(field);
        if (v->Buffer.Buffer == nullptr || v->Buffer.Meta.Origin == MemoryOrigin::None) {
            return false;
        }
        return copy_text((char const*)v->Buffer.Buffer, v->Buffer.Size, out);
    }
};
template <unsigned N>
struct TextTraits<Noesis::FixedString<N>> {
    static constexpr bool kIsText = true;
    static bool read(void const* field, std::string* out) {
        auto const* v = static_cast<Noesis::FixedString<N> const*>(field);
        return copy_text(v->Str(), v->Size(), out);
    }
};

// Whether a type is complete here, which a pointee need not be.
template <class T, class = void>
constexpr bool kComplete = false;
template <class T>
constexpr bool kComplete<T, std::void_t<decltype(sizeof(T))>> = true;

// CompactSet (and TrackedCompactSet, MiniCompactSet) is laid out as an
// Array is and reads the same way, through size() and data(). Its resize()
// sets the capacity rather than the length; compact_set_resize_thunk sets
// both.
template <class T>
struct CompactSetTraits {
    static constexpr bool kIsCompactSet = false;
    using Elem = void;
};

template <class T, class Allocator, bool StoreSize, class TSize>
struct CompactSetTraits<CompactSet<T, Allocator, StoreSize, TSize>> {
    static constexpr bool kIsCompactSet = true;
    using Elem = T;
};
// And the sets built on it, which add no members.
template <class T, class Allocator, bool StoreSize>
struct CompactSetTraits<Set<T, Allocator, StoreSize>> {
    static constexpr bool kIsCompactSet = true;
    using Elem = T;
};
template <class T, class Allocator, bool StoreSize>
struct CompactSetTraits<ObjectSet<T, Allocator, StoreSize>> {
    static constexpr bool kIsCompactSet = true;
    using Elem = T;
};
template <class T, class Allocator>
struct CompactSetTraits<PrimitiveSet<T, Allocator>> {
    static constexpr bool kIsCompactSet = true;
    using Elem = T;
};
template <class T, class Allocator>
struct CompactSetTraits<PrimitiveSmallSet<T, Allocator>> {
    static constexpr bool kIsCompactSet = true;
    using Elem = T;
};

template <class A>
std::size_t array_count_thunk(void const* container) {
    return (std::size_t)static_cast<A const*>(container)->size();
}

// A fresh container of count default elements, its header copied over the
// field's; the old buffer is left where it is.
template <class A>
bool array_resize_thunk(void* container, std::size_t count) {
    if (!bg3le_game_allocator_ready() || count > (1u << 20)) return false;
    alignas(A) unsigned char raw[sizeof(A)];
    auto* fresh = new (raw) A();
    fresh->resize((typename A::size_type)count);
    std::memcpy(container, raw, sizeof(A));
    return true;
}

template <class A>
bool legacy_array_resize_thunk(void* container, std::size_t count) {
    using E = typename LegacyArrayTraits<A>::Elem;
    return array_resize_thunk<Array<E>>(static_cast<Array<E>*>(static_cast<A*>(container)),
                                        count);
}

// Reallocate default-constructs every element up to the capacity, as
// Array::resize does; the old buffer is left where it is.
template <class S>
bool compact_set_resize_thunk(void* container, std::size_t count) {
    using TSize = decltype(S::Size);
    if (!bg3le_game_allocator_ready() || count > (1u << 20)
        || count > (std::size_t)std::numeric_limits<TSize>::max()) {
        return false;
    }
    alignas(S) unsigned char raw[sizeof(S)];
    auto* fresh = new (raw) S();
    fresh->Reallocate((TSize)count);
    fresh->Size = (TSize)count;
    std::memcpy(container, raw, sizeof(S));
    return true;
}

template <class Q>
std::size_t queue_count_thunk(void const* container) {
    return static_cast<Q const*>(container)->size();
}

template <class Q>
void* queue_elem_thunk(void const* container, std::size_t index) {
    auto* q = const_cast<Q*>(static_cast<Q const*>(container));
    if (index >= q->size()) return nullptr;
    return &(*q)[(typename Q::size_type)index];
}

template <class A>
void* array_data_thunk(void const* container) {
    // const is dropped deliberately: the same descriptor serves reads and
    // writes, and a write has a non-const component to begin with.
    return (void*)static_cast<A const*>(container)->data();
}

// A hash set keeps its elements in a contiguous key array, which keys() hands
// back, so it reads as an array with no extra machinery. Writing one would
// desynchronise the table's hashes from its keys, so these are marked
// read-only rather than left writable.
template <class T>
struct SetTraits {
    static constexpr bool kIsSet = false;
    using Elem = void;
};

template <class T>
struct SetTraits<HashSet<T>> {
    static constexpr bool kIsSet = true;
    using Elem = T;
};

// What a key is bucketed by.
//
// bg3se's HashMapHash for a FixedString is FixedString::GetHash(), which
// reads the string table entry's own hash -- through an engine function
// pointer bg3le does not have, so calling it jumps to zero. bg3le reads that
// table itself, so the hash comes from there instead; every other key type
// goes through bg3se's own hash unchanged.
//
// A failure here has to be a refusal rather than a substitute value. A wrong
// bucket does not fault, it makes the key unfindable, and a spell list that
// silently holds nothing is worse than one that refused to change.
template <class T>
bool hash_of(T const& key, std::uint64_t* out) {
    if constexpr (std::is_base_of_v<FixedStringBase, T>) {
        std::uint32_t hash = 0;
        if (!bg3le_fixed_string_hash(key.Index, &hash)) return false;
        *out = hash;
        return true;
    } else {
        *out = HashMapHash(key);
        return true;
    }
}

// Whether the set's existing table is the one our hash rule would have built.
//
// Not that the chains are identical -- insertion order decides those -- but
// that a lookup finds every key the set already holds, which is what the
// engine's own find_index does. If the engine buckets a key by something
// other than hash_of, this fails and the write is refused; nothing else in
// reach would catch that, because a wrong bucket reads as an absent spell
// rather than as a fault.
template <class S>
bool table_reproduces(S const* set) {
    using Elem = typename SetTraits<S>::Elem;

    auto const& keys = set->keys();
    auto const& nextIds = set->next_ids();
    auto const& hashKeys = set->hash_keys();
    const std::size_t buckets = hashKeys.size();
    if (buckets == 0) return keys.size() == 0;

    for (std::uint32_t k = 0; k < keys.size(); ++k) {
        std::uint64_t hash = 0;
        if (!hash_of<Elem>(keys[k], &hash)) return false;

        bool found = false;
        std::int32_t at = hashKeys[(std::uint32_t)(hash % buckets)];
        // Bounded by the key count: a chain longer than that is a cycle.
        for (std::uint32_t step = 0; at >= 0 && step <= keys.size(); ++step) {
            if ((std::uint32_t)at == k) {
                found = true;
                break;
            }
            if ((std::uint32_t)at >= nextIds.size()) return false;
            at = nextIds[at];
        }
        if (!found) return false;
    }
    return true;
}

template <class S>
std::size_t set_count_thunk(void const* container) {
    return (std::size_t)static_cast<S const*>(container)->keys().size();
}

template <class S>
void* set_data_thunk(void const* container) {
    return (void*)static_cast<S const*>(container)->keys().data();
}

// Replaces a set's contents, which is the only way to write one.
//
// A spell list is one of these -- HashSet<FixedString> -- so this is what a
// mod that adds or removes spells is doing.
//
// The algorithm is bg3se's ResizeHashMap and InsertToHashMap; only the stores
// are ours. Going through the container's own clear() and insert() took the
// game down, and there are two reasons it would. clear() fills the engine's
// HashKeys buffer in place, and clearing an Array hands the engine's buffer to
// operator delete -- and static data comes out of a .pak, so neither the
// buffer being writable nor its having come from the engine heap is something
// to assume. Fresh buffers plus a 48-byte header write touch nothing the
// engine allocated.
//
// The old buffers are deliberately left alone, for that second reason: bg3le
// did not allocate them and cannot know what did. Three small allocations per
// spell-list edit is a fair price for not passing a foreign pointer to free.
template <class S>
bool set_assign_thunk(void* container, void const* values,
                      std::size_t count) {
    using Elem = typename SetTraits<S>::Elem;
    auto* set = static_cast<S*>(container);
    auto const* items = static_cast<Elem const*>(values);

    if (!bg3le_game_allocator_ready()) return false;

    // Where the three members sit, from the compiler's own layout of bg3se's
    // HashSet, each checked against the offset a live SpellList.Spells dump
    // showed. A mismatch refuses rather than writing into the wrong member.
    auto* base = reinterpret_cast<char*>(set);
    const std::size_t hashAt =
        reinterpret_cast<char const*>(&set->hash_keys()) - base;
    const std::size_t nextAt =
        reinterpret_cast<char const*>(&set->next_ids()) - base;
    const std::size_t keysAt =
        reinterpret_cast<char const*>(&set->keys()) - base;
    if (hashAt != 0x00 || nextAt != 0x10 || keysAt != 0x20) return false;
    if (sizeof(S) != 0x30) return false;

    // And the set's bookkeeping has to agree with itself, because reading one
    // proves less than it looks: a lookup only ever touches Keys, so a spell
    // list reads perfectly whether or not the two members in front of it hold
    // what they should. One next-id per key, and at least as many buckets as
    // keys.
    constexpr std::size_t kSane = 1u << 24;
    const std::size_t keysNow = set->keys().size();
    const std::size_t nextsNow = set->next_ids().size();
    const std::size_t bucketsNow = set->hash_keys().size();

    if (keysNow > kSane || nextsNow > kSane || bucketsNow > kSane
        || nextsNow != keysNow || bucketsNow < keysNow || count > kSane) {
        logf("set write: refused: %zu keys, %zu next-ids, %zu buckets, %zu "
             "wanted", keysNow, nextsNow, bucketsNow, count);
        return false;
    }

    // And the table the engine built has to be the table our rule builds.
    if (!table_reproduces(set)) {
        logf("set write: refused: the engine's %zu buckets do not find its "
             "own %zu keys under the hash bg3le would use",
             bucketsNow, keysNow);
        return false;
    }

    // Every hash up front, so a key whose string the table cannot resolve
    // stops this before anything is written.
    std::vector<std::uint64_t> hashes(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (!hash_of<Elem>(items[i], &hashes[i])) {
            logf("set write: refused: no string-table hash for key %zu of %zu",
                 i, count);
            return false;
        }
    }

    const auto total = (std::uint32_t)count;
    const std::uint32_t buckets = GetNearestSmallMultiHashMapPrime(total + 2);
    const std::uint32_t slots = total > 0 ? total : 1;

    auto* keysBuf = (Elem*)GameAllocRaw(sizeof(Elem) * slots);
    auto* nextBuf = (std::int32_t*)GameAllocRaw(sizeof(std::int32_t) * slots);
    auto* hashBuf = (std::int32_t*)GameAllocRaw(sizeof(std::int32_t) * buckets);
    if (keysBuf == nullptr || nextBuf == nullptr || hashBuf == nullptr) {
        return false;
    }

    for (std::uint32_t i = 0; i < buckets; ++i) hashBuf[i] = -1;

    // bg3se's InsertToHashMap, key by key. A bucket holding no key encodes
    // the bucket it belongs to as -2 - bucket, which is how a chain ends.
    for (std::uint32_t i = 0; i < total; ++i) {
        new (keysBuf + i) Elem(items[i]);
        const auto bucket = (std::uint32_t)(hashes[i] % buckets);
        std::int32_t prev = hashBuf[bucket];
        if (prev < 0) prev = -2 - (std::int32_t)bucket;
        nextBuf[i] = prev;
        hashBuf[bucket] = (std::int32_t)i;
    }

    auto store32 = [base](std::size_t off, std::uint32_t value) {
        std::memcpy(base + off, &value, sizeof(value));
    };
    auto store64 = [base](std::size_t off, std::uint64_t value) {
        std::memcpy(base + off, &value, sizeof(value));
    };

    // Empty first, then filled: a reader that catches this mid-write sees a
    // set with no keys rather than one whose count outruns its buffer.
    store32(0x2c, 0);
    store64(0x00, (std::uint64_t)(std::uintptr_t)hashBuf);
    store32(0x08, buckets);
    store64(0x10, (std::uint64_t)(std::uintptr_t)nextBuf);
    store32(0x18, slots);
    store32(0x1c, total);
    store64(0x20, (std::uint64_t)(std::uintptr_t)keysBuf);
    store32(0x28, slots);
    store32(0x2c, total);
    return true;
}

// Replaces one of Larian's sixteen-byte strings.
//
// Through LSStringBase's own assign, so the inline and heap forms and the
// capacity flag are its business rather than restated here -- see
// vendor/bg3se/CoreLib/Base/LSString.h.
//
// The storage is zeroed first, which abandons the old heap buffer instead of
// freeing it. Deliberate, and for the same reason the hash-set rebuild leaves
// its old buffers alone: a string in static data was allocated by whatever
// loaded the .pak, and bg3le cannot know that operator delete is the right
// way to release it. A string's worth of memory per write is a fair price.
extern "C" bool bg3le_meta_lsstring_assign(void* address, char const* text,
                                           std::size_t length) {
    if (address == nullptr || text == nullptr) return false;
    // Only an allocation can fail, and only a long string needs one.
    if (length > STDString::InlineCapacity && !bg3le_game_allocator_ready()) {
        logf("string write: refused: %zu characters needs the engine heap and "
             "the allocator was not found", length);
        return false;
    }

    auto* str = static_cast<STDString*>(address);
    std::memset(str, 0, sizeof(STDString));
    str->assign(text, length);
    return true;
}

// std::optional is a value that may or may not be there, which is a different
// claim from one bg3le cannot read -- and reporting it as unsupported conflated
// the two. bg3se prints null for an empty one; saying "unsupported" instead
// would still be wrong when it is full.
//
// Read through has_value() and operator* rather than by guessing at where the
// engaged flag sits. The engine is built against libc++ and so is bg3le, which
// is already a requirement for std::string, so the layouts agree -- but going
// through the accessors means not depending on that here.
template <class T>
struct OptionalTraits {
    static constexpr bool kIsOptional = false;
    using Elem = void;
};

template <class T>
struct OptionalTraits<std::optional<T>> {
    static constexpr bool kIsOptional = true;
    using Elem = T;
};

template <class O>
std::size_t optional_count_thunk(void const* opt) {
    return static_cast<O const*>(opt)->has_value() ? 1 : 0;
}

template <class O>
void* optional_data_thunk(void const* opt) {
    auto const* o = static_cast<O const*>(opt);
    if (!o->has_value()) return nullptr;
    return (void*)&**o;
}

// Engages or clears an optional, through the type's own emplace() and
// reset(). Writing the flag directly would mean knowing where libc++ keeps
// it, and that is not the same place for every payload.
template <class O>
void optional_engage_thunk(void* opt, bool engaged) {
    auto* o = static_cast<O*>(opt);
    if (engaged) {
        if (!o->has_value()) o->emplace();
    } else {
        o->reset();
    }
}

// std::variant holds one of several types, and which one is known only at
// runtime -- so unlike every other container here its element cannot be
// described by a single type. It carries one descriptor per alternative
// instead, and the resolver picks by the active index.
//
// Read through index() and std::visit rather than by reaching into the
// discriminant, for the same reason optional goes through has_value().
template <class T>
struct VariantTraits {
    static constexpr bool kIsVariant = false;
};

// The engine's layout for a type this compiler does not lay out the same way.
//
// Everywhere else in this file the layout comes from the type itself --
// offsetof for a member, the container's own accessors for a length -- and
// that is right because the declaration and the engine agree. std::variant
// is the exception: its size and where it keeps its discriminant are the
// standard library's business, not bg3se's, and this libc++ does not agree
// with whatever built the game.
//
// So for those, the numbers are measured from the engine's own memory and
// written down here. See reference/REFERENCE-DIFFS.md for the measurement:
// a 52-character stats expression whose fifteen parameters wrote only their
// payload and their discriminant, leaving the pool's previous contents
// showing through the padding, which made the stride visible.
template <class T>
struct EngineLayout {
    static constexpr std::size_t Size = sizeof(T);
    // Where the discriminant is, or -1 for a type that has none.
    static constexpr int IndexAt = -1;
};

constexpr std::size_t round_up(std::size_t n, std::size_t to) {
    return (n + to - 1) / to * to;
}

// A variant, laid out the way the game's libc++ lays it out: the union,
// rounded to the alternatives' alignment, then the index in one byte, then
// padding to the same alignment. The payload is at the start whichever
// alternative is held.
//
// The game is built against libc++ ABI version 2. CMakeLists.txt gives this
// build the same one-byte index, which makes a flat variant compile to
// exactly this. What it cannot fix is nesting: this libc++ version pads a
// variant held inside another variant by eight bytes and the game's does
// not, which is why the size of an alternative is taken from its own engine
// layout rather than from sizeof. Two measurements below pin the rule.
template <class... Ts>
struct EngineLayout<std::variant<Ts...>> {
    static_assert(sizeof...(Ts) < 255, "a one-byte index holds 254");
    static constexpr std::size_t Align = std::max({alignof(Ts)...});
    static constexpr std::size_t UnionSize =
        round_up(std::max({EngineLayout<Ts>::Size...}), Align);
    static constexpr int IndexAt = (int)UnionSize;
    static constexpr std::size_t Size = round_up(UnionSize + 1, Align);
};

// An optional is its value then its flag, so it inherits any difference in
// its value's size.
template <class V>
struct EngineLayout<std::optional<V>> {
    static constexpr std::size_t Size =
        round_up(EngineLayout<V>::Size + 1, alignof(V));
    static constexpr int IndexAt = -1;
};

// A struct whose last member is a variant this libc++ lays out differently.
// Every member sits where offsetof says; only the struct's size differs, and
// that is what an array of them strides by. The static_assert is the
// assumption: a member added after this one would sit at a wrong offset,
// and should stop the build rather than be read from the wrong place.
#define BG3LE_ENGINE_SIZE_LAST_MEMBER(Type, Member)                           \
    template <>                                                               \
    struct EngineLayout<Type> {                                               \
        static_assert(offsetof(Type, Member) + sizeof(Type::Member)            \
                          == sizeof(Type),                                    \
                      #Member " must be the last member of " #Type);         \
        static constexpr std::size_t Size =                                   \
            round_up(offsetof(Type, Member)                                   \
                         + EngineLayout<decltype(Type::Member)>::Size,        \
                     alignof(Type));                                          \
        static constexpr int IndexAt = -1;                                    \
    };

// The three tools/meta-check.c finds. Each holds a variant nested inside a
// variant, the one case the compile flag cannot make agree.
BG3LE_ENGINE_SIZE_LAST_MEMBER(bg3se::GlobalConfigParameter, Value)
BG3LE_ENGINE_SIZE_LAST_MEMBER(bg3se::esv::spell_cast::PreviewSetRequest, Param)
BG3LE_ENGINE_SIZE_LAST_MEMBER(bg3se::esv::spell_cast::SystemEvent, Args)

// GlobalConfigParameter is the one that faulted: a root template holds an
// Array of them, and at 56 bytes rather than 48 the fifth element was read
// from inside the fourth.
static_assert(EngineLayout<bg3se::GlobalConfigParameter>::Size == 48);

// The two measurements the rule has to reproduce, taken from the engine's
// own memory -- see reference/REFERENCE-DIFFS.md. A 52-character stats
// expression whose fifteen parameters wrote only their payload and their
// discriminant showed Param's stride and index; "Axal" and "AORT" read as
// Variant2's index showed its width. Both decode to upstream's captured
// answers.
static_assert(EngineLayout<bg3se::StatsExpressionInternal::Param>::Size == 32);
static_assert(EngineLayout<bg3se::StatsExpressionInternal::Param>::IndexAt == 24);
static_assert(EngineLayout<bg3se::StatsExpressionInternal::Variant2>::Size == 24);
static_assert(EngineLayout<bg3se::StatsExpressionInternal::Variant2>::IndexAt == 16);

template <class V>
std::size_t variant_alternative_count();

// The discriminant read at the engine's offset rather than through index().
template <class V>
std::size_t variant_index_at_thunk(void const* v) {
    std::uint8_t held = 0;
    std::memcpy(&held, (char const*)v + EngineLayout<V>::IndexAt,
                sizeof(held));
    const std::size_t count = variant_alternative_count<V>();
    // Out of range is a valueless variant as far as a reader is concerned,
    // and saying so beats decoding the bytes as an alternative they are not.
    return held >= count ? (std::size_t)-1 : (std::size_t)held;
}

// The payload sits at the start, which is what leaves room for the
// discriminant after it.
template <class V>
void* variant_payload_thunk(void const* v) {
    return const_cast<void*>(v);
}

template <class V>
std::size_t variant_index_thunk(void const* v) {
    auto const* var = static_cast<V const*>(v);
    if (var->valueless_by_exception()) return (std::size_t)-1;
    return var->index();
}

template <class V>
void* variant_data_thunk(void const* v) {
    auto const* var = static_cast<V const*>(v);
    if (var->valueless_by_exception()) return nullptr;
    return std::visit([](auto const& held) { return (void*)&held; }, *var);
}

// A hash map keeps its keys and its values in two parallel contiguous runs,
// so slot i holds key i alongside value i -- which is what makes it
// presentable without hashing anything: iteration is a walk over both runs.
template <class T>
struct MapTraits {
    static constexpr bool kIsMap = false;
    using Key = void;
    using Value = void;
};

template <class K, class V>
struct MapTraits<HashMap<K, V>> {
    static constexpr bool kIsMap = true;
    using Key = K;
    using Value = V;
};

// LegacyMap and LegacyRefMap chain their entries off a bucket table, so an
// entry is reached by walking to it. A cursor per thread keeps walking them
// in order linear rather than quadratic.
template <class T>
struct LegacyMapTraits {
    static constexpr bool kIsLegacyMap = false;
    using Key = void;
    using Value = void;
};

template <class I>
struct LegacyMapTraits<LegacyMapBase<I>> {
    static constexpr bool kIsLegacyMap = true;
    using Key = typename LegacyMapBase<I>::KeyType;
    using Value = typename LegacyMapBase<I>::ValueType;
};

template <class M>
struct LegacyInternalsOf;
template <class I>
struct LegacyInternalsOf<LegacyMapBase<I>> {
    using type = I;
};

// The node of entry index. The table and the chains are read fault-
// tolerantly, since a map reached through a pointer may not be where the
// pointer says; the walk is kept per thread for the last map, so reading its
// entries in turn walks it once.
template <class M>
typename M::Node* legacy_map_node(void const* container, std::size_t index) {
    using I = typename LegacyInternalsOf<M>::type;
    using Node = typename M::Node;
    std::uint32_t hashSize = 0, count = 0;
    Node** table = nullptr;
    auto const* base = static_cast<char const*>(container);
    if (!safe_read(base + offsetof(I, HashSize), &hashSize, sizeof(hashSize))
        || !safe_read(base + offsetof(I, ItemCount), &count, sizeof(count))
        || !safe_read(base + offsetof(I, HashTable), &table, sizeof(table))
        || index >= count || count > (1u << 22) || hashSize > (1u << 22)) {
        return nullptr;
    }

    struct Walk {
        void const* Map = nullptr;
        std::uint32_t Count = 0;
        std::vector<Node*> Nodes;
    };
    thread_local Walk walk;
    if (walk.Map != container || walk.Count != count) {
        walk = Walk{container, count, {}};
        std::vector<Node*> buckets(hashSize);
        if (hashSize != 0
            && safe_read_some(table, buckets.data(), hashSize * sizeof(Node*))
                   != hashSize * sizeof(Node*)) {
            return nullptr;
        }
        for (Node* node : buckets) {
            for (std::uint32_t guard = 0; node != nullptr && guard <= count
                 && walk.Nodes.size() < count; ++guard) {
                walk.Nodes.push_back(node);
                Node* next = nullptr;
                if (!safe_read((char const*)node + offsetof(Node, Next), &next, sizeof(next))) {
                    break;
                }
                node = next;
            }
        }
    }
    return index < walk.Nodes.size() ? walk.Nodes[index] : nullptr;
}

template <class M>
std::size_t legacy_map_count_thunk(void const* container) {
    using I = typename LegacyInternalsOf<M>::type;
    std::uint32_t count = 0;
    safe_read((char const*)container + offsetof(I, ItemCount), &count, sizeof(count));
    return count;
}

template <class M>
void* legacy_map_value_thunk(void const* container, std::size_t index) {
    auto* node = legacy_map_node<M>(container, index);
    return node != nullptr ? (char*)node + offsetof(typename M::Node, Value) : nullptr;
}

template <class M>
void* legacy_map_key_thunk(void const* container, std::size_t index) {
    auto* node = legacy_map_node<M>(container, index);
    return node != nullptr ? (char*)node + offsetof(typename M::Node, Key) : nullptr;
}

// A HashMap's contents rebuilt into fresh buffers with one key added or
// taken out: bg3se's HashSet algorithm, our stores, the old buffers left
// alone. The layout is the set's (checked in set_assign_thunk) plus the
// values at +0x30, and the engine's table must reproduce under hash_of.
template <class M>
bool map_rebuild(void* container, void const* keyBytes, bool insert) {
    using K = typename MapTraits<M>::Key;
    using V = typename MapTraits<M>::Value;
    if constexpr (!std::is_default_constructible_v<V> || sizeof(M) != 0x40) {
        return false;
    } else {
        if (!bg3le_game_allocator_ready()) return false;
        auto* base = static_cast<char*>(container);
        struct Raw {
            std::int32_t* Hash; std::uint32_t Buckets; std::uint32_t Pad;
            std::int32_t* Next; std::uint32_t NextCap; std::uint32_t NextSize;
            unsigned char* Keys; std::uint32_t KeysCap; std::uint32_t KeysSize;
            unsigned char* Values; std::uint32_t ValuesCap; std::uint32_t ValuesSize;
        } m{};
        static_assert(sizeof(Raw) == 0x40);
        if (!safe_read(base, &m, sizeof(m))) return false;
        const std::uint32_t n = m.KeysSize;
        if (n > (1u << 24) || m.NextSize != n || (m.ValuesSize != 0 && m.ValuesSize != n)
            || (n > 0 && m.Buckets == 0)) {
            return false;
        }

        alignas(K) unsigned char keyRaw[sizeof(K)];
        std::memcpy(keyRaw, keyBytes, sizeof(K));
        K const& key = *reinterpret_cast<K const*>(keyRaw);
        std::uint64_t keyHash = 0;
        if (!hash_of<K>(key, &keyHash)) return false;

        std::vector<std::uint64_t> hashes(n);
        std::int32_t found = -1;
        for (std::uint32_t i = 0; i < n; ++i) {
            K const& k = *reinterpret_cast<K const*>(m.Keys + (std::size_t)i * sizeof(K));
            if (!hash_of<K>(k, &hashes[i])) return false;
            // Every key reachable from its bucket, or the table is not ours.
            bool reached = false;
            std::int32_t at = m.Hash[(std::uint32_t)(hashes[i] % m.Buckets)];
            for (std::uint32_t step = 0; at >= 0 && step <= n; ++step) {
                if ((std::uint32_t)at == i) { reached = true; break; }
                if ((std::uint32_t)at >= n) return false;
                at = m.Next[at];
            }
            if (!reached) return false;
            if (k == key) found = (std::int32_t)i;
        }
        if (insert ? found >= 0 : found < 0) return insert;

        // bg3se's removeAt moves the last entry into the hole.
        std::vector<std::uint32_t> order;
        for (std::uint32_t i = 0; i < n; ++i) order.push_back(i);
        if (!insert) {
            order[(std::uint32_t)found] = n - 1;
            order.pop_back();
        }
        const auto total = (std::uint32_t)(order.size() + (insert ? 1 : 0));
        const std::uint32_t slots = total > 0 ? total : 1;
        const std::uint32_t buckets = GetNearestSmallMultiHashMapPrime(total + 2);
        auto* keysBuf = (unsigned char*)GameAllocRaw(sizeof(K) * slots);
        auto* valuesBuf = (unsigned char*)GameAllocRaw(sizeof(V) * slots);
        auto* nextBuf = (std::int32_t*)GameAllocRaw(sizeof(std::int32_t) * slots);
        auto* hashBuf = (std::int32_t*)GameAllocRaw(sizeof(std::int32_t) * buckets);
        if (!keysBuf || !valuesBuf || !nextBuf || !hashBuf) return false;

        std::vector<std::uint64_t> newHashes;
        for (std::uint32_t j = 0; j < order.size(); ++j) {
            std::memcpy(keysBuf + (std::size_t)j * sizeof(K), m.Keys + (std::size_t)order[j] * sizeof(K), sizeof(K));
            std::memcpy(valuesBuf + (std::size_t)j * sizeof(V), m.Values + (std::size_t)order[j] * sizeof(V), sizeof(V));
            newHashes.push_back(hashes[order[j]]);
        }
        if (insert) {
            std::memcpy(keysBuf + (std::size_t)(total - 1) * sizeof(K), keyRaw, sizeof(K));
            new (valuesBuf + (std::size_t)(total - 1) * sizeof(V)) V();
            newHashes.push_back(keyHash);
        }
        for (std::uint32_t b = 0; b < buckets; ++b) hashBuf[b] = -1;
        for (std::uint32_t i = 0; i < total; ++i) {
            const auto bucket = (std::uint32_t)(newHashes[i] % buckets);
            std::int32_t prev = hashBuf[bucket];
            if (prev < 0) prev = -2 - (std::int32_t)bucket;
            nextBuf[i] = prev;
            hashBuf[bucket] = (std::int32_t)i;
        }

        // Empty first, then filled, as set_assign_thunk does.
        const std::uint32_t zero = 0;
        std::memcpy(base + 0x2c, &zero, 4);
        Raw grown{hashBuf, buckets, m.Pad, nextBuf, slots, total, keysBuf, slots, total,
                  valuesBuf, slots, m.ValuesSize == 0 ? 0u : total};
        std::memcpy(base, &grown, sizeof(grown));
        return true;
    }
}

template <class M>
bool map_insert_thunk(void* container, void const* key) {
    return map_rebuild<M>(container, key, true);
}

template <class M>
bool map_remove_thunk(void* container, void const* key) {
    return map_rebuild<M>(container, key, false);
}

// A LegacyMap only links a new node in, so bg3se's own get_or_insert will do.
template <class M>
bool legacy_map_insert_thunk(void* container, void const* keyBytes) {
    using K = typename LegacyMapTraits<M>::Key;
    using V = typename LegacyMapTraits<M>::Value;
    if constexpr (!std::is_default_constructible_v<V>) {
        return false;
    } else {
        if (!bg3le_game_allocator_ready()) return false;
        alignas(K) unsigned char keyRaw[sizeof(K)];
        std::memcpy(keyRaw, keyBytes, sizeof(K));
        return static_cast<M*>(container)->get_or_insert(*reinterpret_cast<K const*>(keyRaw)) != nullptr;
    }
}

template <class M>
std::size_t map_count_thunk(void const* container) {
    return (std::size_t)static_cast<M const*>(container)->size();
}

template <class M>
void* map_values_thunk(void const* container) {
    return (void*)static_cast<M const*>(container)->raw_values().data();
}

template <class M>
void* map_keys_thunk(void const* container) {
    return (void*)static_cast<M const*>(container)->keys().data();
}

// The field kinds bg3le can read and write without interpretation. Enums
// resolve to their underlying integer, which is how the engine stores them and
// how a script wants to see them.
template <class T>
constexpr FieldKind scalar_kind_of() {
    if constexpr (std::is_enum_v<T>) {
        return scalar_kind_of<std::underlying_type_t<T>>();
    }
    else if constexpr (std::is_same_v<T, bool>) return FieldKind::Bool;
    else if constexpr (std::is_same_v<T, float>) return FieldKind::Float;
    else if constexpr (std::is_same_v<T, double>) return FieldKind::Double;
    // Any integer, by size and signedness rather than by name: the vendored
    // headers use MSVC's __int64 and __int8, which are long long and char
    // here, neither of them the std:: type of that width.
    else if constexpr (std::is_integral_v<T>) {
        constexpr bool kSigned = std::is_signed_v<T>;
        if constexpr (sizeof(T) == 1) return kSigned ? FieldKind::Int8 : FieldKind::Uint8;
        else if constexpr (sizeof(T) == 2) return kSigned ? FieldKind::Int16 : FieldKind::Uint16;
        else if constexpr (sizeof(T) == 4) return kSigned ? FieldKind::Int32 : FieldKind::Uint32;
        else if constexpr (sizeof(T) == 8) return kSigned ? FieldKind::Int64 : FieldKind::Uint64;
        else return FieldKind::Unsupported;
    }
    else if constexpr (std::is_same_v<T, Guid>) return FieldKind::Guid;
    else if constexpr (std::is_same_v<T, EntityHandle>) return FieldKind::Entity;
    // A FixedString is a four-byte index, so it behaves as a scalar here even
    // though resolving it needs the engine's string table.
    else if constexpr (std::is_same_v<T, FixedString>) return FieldKind::FixedString;
    // Larian's string. Reported as a scalar because it is read in place,
    // like a FixedString -- it was Unsupported before, which is why every
    // reflected object's string fields, a template's Name among them, read
    // as "<unsupported>".
    else if constexpr (std::is_same_v<T, STDString>) return FieldKind::LSString;
    // Wrappers upstream pushes and gets as the value they hold, which is
    // first in each, so the bytes at the field's address are that value.
    else if constexpr (std::is_same_v<T, Path>) {
        static_assert(offsetof(Path, Name) == 0 && sizeof(Path) == sizeof(STDString));
        return FieldKind::LSString;
    }
    else if constexpr (std::is_same_v<T, NetId>) return FieldKind::Uint64;
    else if constexpr (std::is_same_v<T, UserId>) {
        return scalar_kind_of<decltype(UserId::Id)>();
    }
    else if constexpr (std::is_same_v<T, ComponentHandle>) {
        return FieldKind::ComponentHandle;
    }
    else if constexpr (std::is_same_v<T, bg3se::stats::ConditionId>) {
        return FieldKind::ConditionId;
    }
    else return FieldKind::Unsupported;
}

// An array of scalars is reported as one. A class type that is not a scalar in
// disguise is reported as a struct, and whether it can actually be traversed
// is decided at load by whether its type name resolves to a field table --
// so HashMap and DynamicArray land here too and simply fail to resolve, which
// is the honest answer until they are handled.
template <class T>
constexpr FieldKind kind_of() {
    // A fixed-extent array is one whatever its elements are: an element that
    // is a struct or another container is reached through the element
    // descriptor, and reportable_kind is what decides whether anything can be
    // done with it. Restricting this to scalar elements made
    // std::array<SomeStruct, N> unreadable -- which is how DiceValues, an
    // optional std::array of structs, stayed out of reach after the optional
    // itself worked.
    if constexpr (TextTraits<T>::kIsText) {
        return FieldKind::Text;
    } else if constexpr (std::is_same_v<T, Version>) {
        return FieldKind::Version;
    } else if constexpr (std::is_same_v<T, EntityOrVec3Variant>) {
        return FieldKind::EntityOrVec3;
    } else if constexpr (BitArrayTraits<T>::kIs) {
        return FieldKind::BitArray;
    } else if constexpr (ArrayTraits<T>::kIsArray) {
        return FieldKind::ScalarArray;
    } else if constexpr (GlmTraits<T>::kIsGlm) {
        if constexpr (scalar_kind_of<typename GlmTraits<T>::Elem>()
                      != FieldKind::Unsupported) {
            return FieldKind::ScalarArray;
        } else {
            return FieldKind::Unsupported;
        }
    } else if constexpr (VectorTraits<T>::kIsVector
                         || LegacyArrayTraits<T>::kIs
                         || FixedVectorTraits<T>::kIs
                         || QueueTraits<T>::kIs
                         || SetTraits<T>::kIsSet
                         || CompactSetTraits<T>::kIsCompactSet) {
        return FieldKind::DynArray;
    } else if constexpr (MapTraits<T>::kIsMap
                         || LegacyMapTraits<T>::kIsLegacyMap) {
        return FieldKind::Map;
    } else if constexpr (OptionalTraits<T>::kIsOptional) {
        return FieldKind::Optional;
    } else if constexpr (VariantTraits<T>::kIsVariant) {
        return FieldKind::Variant;
    } else if constexpr (scalar_kind_of<T>() != FieldKind::Unsupported) {
        return scalar_kind_of<T>();
    } else if constexpr (std::is_same_v<T, StatsExpressionRef>) {
        return FieldKind::Pointer;
    } else if constexpr (std::is_pointer_v<T>) {
        // To a described class, or to anything else that converts: a set, a
        // map, a string, another pointer.
        using P = std::remove_cv_t<std::remove_pointer_t<T>>;
        if constexpr (std::is_class_v<P> || (kComplete<P> && !std::is_void_v<P>
                                              && kind_of<P>() != FieldKind::Unsupported)) {
            return FieldKind::Pointer;
        } else {
            return FieldKind::Unsupported;
        }
    } else if constexpr (std::is_class_v<T>) {
        return FieldKind::Struct;
    } else {
        return FieldKind::Unsupported;
    }
}

// Not a field: records that a class also has the fields of another, named so
// it can be resolved at load rather than needing that class to be complete
// here.
constexpr FieldDesc inherit_field(char const* baseName) {
    FieldDesc f{};
    f.Name = baseName;
    f.Kind = FieldKind::Inherit;
    f.ElemKind = FieldKind::Unsupported;
    return f;
}

template <class T>
constexpr FieldDesc make_field(char const* name, std::size_t offset);

// A descriptor for a container's element type, so indexing a container can
// continue with the element's own accessors rather than with the field's.
template <class T>
inline constexpr FieldDesc kElementDesc = make_field<T>("(element)", 0);

// One descriptor per alternative, null-terminated. Declared here rather than
// with the other traits because it needs kElementDesc.
template <class... Ts>
struct VariantTraits<std::variant<Ts...>> {
    static constexpr bool kIsVariant = true;
    static inline constexpr FieldDesc const* kAlternatives[] = {
        &kElementDesc<Ts>..., nullptr};
    static constexpr std::size_t kCount = sizeof...(Ts);
};

template <class V>
std::size_t variant_alternative_count() {
    return VariantTraits<V>::kCount;
}

// Builds a field descriptor from its type. Having every kind decision here
// rather than spelled out in each macro means adding a kind is one edit.
// A root template's property: its value, and whether the template overrides
// the one it inherits. See CoreLib/Base/BaseUtilities.h.
template <class T>
struct OverrideableTraits {
    static constexpr bool kIsOverrideable = false;
    using Value = void;
};

template <class V>
struct OverrideableTraits<OverrideableProperty<V>> {
    static constexpr bool kIsOverrideable = true;
    using Value = V;
};

template <class T>
constexpr FieldDesc make_plain_field(char const* name, std::size_t offset);

template <class T>
constexpr FieldDesc make_field(char const* name, std::size_t offset) {
    // An OverrideableProperty is described as its Value, at the same offset,
    // because that is what upstream makes it: GetStaticTypeInfo hands back
    // the value type's information, and push, Serialize and MakeObjectRef
    // all go straight to .Value. So every read path here works unchanged --
    // the bytes at the field's address are a V.
    //
    // What is added is where the flag is, for the one place the wrapper
    // shows: upstream's setter is get<OverrideableProperty<V>>, which builds
    // {value, true}, so assigning the property marks it overridden.
    if constexpr (OverrideableTraits<T>::kIsOverrideable) {
        using V = typename OverrideableTraits<T>::Value;
        static_assert(offsetof(T, Value) == 0,
                      "OverrideableProperty keeps its Value first");
        // make_field rather than make_plain_field, so a value that is itself
        // presented as something else -- an EntityRef, say -- still is.
        FieldDesc inner = make_field<V>(name, offset);
        inner.OverrideFlagAt = (std::uint16_t)offsetof(T, IsOverridden);
        return inner;
    } else if constexpr (std::is_same_v<T, bg3se::ecs::EntityRef>) {
        // An entity and the world it belongs to. Upstream's push hands back
        // the entity for Handle, or nil if it is null, exactly as it does for
        // a bare EntityHandle; World never reaches Lua. So it reads as its
        // handle, at the same offset.
        //
        // Writing is where World matters; see bg3le_meta_after_write.
        static_assert(offsetof(bg3se::ecs::EntityRef, Handle) == 0,
                      "EntityRef keeps its Handle first");
        FieldDesc handle = make_plain_field<EntityHandle>(name, offset);
        handle.EntityWorldAt =
            (std::uint16_t)offsetof(bg3se::ecs::EntityRef, World);
        return handle;
    } else {
        return make_plain_field<T>(name, offset);
    }
}

// The field itself, marked so its enum's labels resolve as flag properties.
template <class T>
constexpr FieldDesc make_bitmask_field(char const* name, std::size_t offset) {
    FieldDesc f = make_field<T>(name, offset);
    f.BitmaskFlags = true;
    return f;
}

// A field's old name, still found by mods written against it.
template <class T>
constexpr FieldDesc make_legacy_field(char const* name, std::size_t offset) {
    FieldDesc f = make_field<T>(name, offset);
    f.Legacy = true;
    return f;
}

template <class T>
constexpr FieldDesc make_plain_field(char const* name, std::size_t offset) {
    FieldDesc f{};
    f.Name = name;
    f.Offset = (std::uint32_t)offset;
    f.Size = (std::uint16_t)EngineLayout<T>::Size;
    // Where this build's sizeof disagrees with the engine, a struct holding
    // the field has its later members at the wrong offsets too. Recorded so
    // tools/meta-check.c can list every one rather than waiting for a read to
    // go wrong.
    if constexpr (EngineLayout<T>::Size != sizeof(T)) {
        f.CompiledSize = (std::uint16_t)sizeof(T);
    }
    f.Kind = kind_of<T>();
    f.ElemKind = FieldKind::Unsupported;
    f.ElemCount = 0;
    f.ElemSize = 0;
    f.TypeName = nullptr;
    f.TypeNameLength = 0;
    f.ElemTypeName = nullptr;
    f.ElemTypeNameLength = 0;

    // Element description, shared by both array kinds. A struct element is
    // named so it can be descended into, exactly as a struct field is.
    auto describe_elements = [&f]<class E>() {
        f.ElemKind = scalar_kind_of<E>();
        f.ElemSize = (std::uint16_t)EngineLayout<E>::Size;
        f.ElemDesc = &kElementDesc<E>;
        if constexpr (std::is_class_v<E>) {
            f.ElemTypeName = type_name<E>().data();
            f.ElemTypeNameLength = (std::uint16_t)type_name<E>().size();
        }
    };

    if constexpr (ArrayTraits<T>::kIsArray) {
        using E = typename ArrayTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.ElemCount = (std::uint16_t)ArrayTraits<T>::kCount;
    } else if constexpr (GlmTraits<T>::kIsGlm) {
        using E = typename GlmTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.ElemCount = (std::uint16_t)GlmTraits<T>::kCount;
        f.IsVector = true;
    } else if constexpr (VectorTraits<T>::kIsVector) {
        using E = typename VectorTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &array_count_thunk<T>;
        f.Data = &array_data_thunk<T>;
        if constexpr (std::is_default_constructible_v<E>
                      && std::is_move_constructible_v<E>) {
            f.Resize = &array_resize_thunk<T>;
        }
    } else if constexpr (CompactSetTraits<T>::kIsCompactSet) {
        using E = typename CompactSetTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &array_count_thunk<T>;
        f.Data = &array_data_thunk<T>;
        if constexpr (std::is_default_constructible_v<E>
                      && std::is_move_constructible_v<E>) {
            f.Resize = &compact_set_resize_thunk<T>;
        }
    } else if constexpr (LegacyArrayTraits<T>::kIs) {
        using E = typename LegacyArrayTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &array_count_thunk<T>;
        f.Data = &array_data_thunk<T>;
        if constexpr (std::is_default_constructible_v<E>
                      && std::is_move_constructible_v<E>) {
            f.Resize = &legacy_array_resize_thunk<T>;
        }
    } else if constexpr (FixedVectorTraits<T>::kIs) {
        using E = typename FixedVectorTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &array_count_thunk<T>;
        f.Data = &array_data_thunk<T>;
    } else if constexpr (QueueTraits<T>::kIs) {
        using E = typename QueueTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &queue_count_thunk<T>;
        f.ElemAt = &queue_elem_thunk<T>;
        f.ReadOnly = true;
    } else if constexpr (BitArrayTraits<T>::kIs) {
        f.ElemCount = (std::uint16_t)BitArrayTraits<T>::kBits;
    } else if constexpr (SetTraits<T>::kIsSet) {
        using E = typename SetTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &set_count_thunk<T>;
        f.Data = &set_data_thunk<T>;
        f.Assign = &set_assign_thunk<T>;
        f.ReadOnly = true;
    } else if constexpr (OptionalTraits<T>::kIsOptional) {
        using E = typename OptionalTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &optional_count_thunk<T>;
        f.Data = &optional_data_thunk<T>;
        // Only where the payload can be default-constructed: engaging one
        // that cannot would have nothing to put in it.
        if constexpr (std::is_default_constructible_v<E>) {
            f.Engage = &optional_engage_thunk<T>;
        }
    } else if constexpr (VariantTraits<T>::kIsVariant) {
        f.Alternatives = VariantTraits<T>::kAlternatives;
        // The engine's layout, never index() or std::visit: those are this
        // libc++'s, and a nested variant is laid out differently by it.
        f.ActiveIndex = &variant_index_at_thunk<T>;
        f.Data = &variant_payload_thunk<T>;
    } else if constexpr (MapTraits<T>::kIsMap) {
        using K = typename MapTraits<T>::Key;
        using V = typename MapTraits<T>::Value;
        // The values are the elements; the keys are described separately.
        describe_elements.template operator()<V>();
        f.Count = &map_count_thunk<T>;
        f.Data = &map_values_thunk<T>;
        f.KeyData = &map_keys_thunk<T>;
        f.MapInsert = &map_insert_thunk<T>;
        f.MapRemove = &map_remove_thunk<T>;
        f.KeyKind = scalar_kind_of<K>();
        f.KeySize = (std::uint16_t)sizeof(K);
        if constexpr (std::is_enum_v<K>) {
            f.KeyTypeName = type_name<K>().data();
            f.KeyTypeNameLength = (std::uint16_t)type_name<K>().size();
        }
    } else if constexpr (TextTraits<T>::kIsText) {
        f.ReadText = &TextTraits<T>::read;
        f.ReadOnly = true;
    } else if constexpr (std::is_same_v<T, Version>
                         || std::is_same_v<T, EntityOrVec3Variant>) {
        f.ReadOnly = true;
    } else if constexpr (std::is_same_v<T, StatsExpressionRef>) {
        // A StatsExpressionRef is its StatsExpressionPooled*, and upstream
        // pushes the pooled expression.
        static_assert(sizeof(StatsExpressionRef) == sizeof(void*));
        f.TypeName = type_name<StatsExpressionPooled>().data();
        f.TypeNameLength = (std::uint16_t)type_name<StatsExpressionPooled>().size();
    } else if constexpr (std::is_pointer_v<T>) {
        // A described class is read as an object of its own (TypeName); the
        // rest through [0], with the pointee's own descriptor.
        using P = std::remove_cv_t<std::remove_pointer_t<T>>;
        if constexpr (kind_of<P>() == FieldKind::Struct) {
            f.TypeName = type_name<P>().data();
            f.TypeNameLength = (std::uint16_t)type_name<P>().size();
        }
        if constexpr (kComplete<P> && !std::is_abstract_v<P>) {
            describe_elements.template operator()<P>();
        }
    } else if constexpr (LegacyMapTraits<T>::kIsLegacyMap) {
        using K = typename LegacyMapTraits<T>::Key;
        using V = typename LegacyMapTraits<T>::Value;
        describe_elements.template operator()<V>();
        f.Count = &legacy_map_count_thunk<T>;
        f.ElemAt = &legacy_map_value_thunk<T>;
        f.KeyAt = &legacy_map_key_thunk<T>;
        f.MapInsert = &legacy_map_insert_thunk<T>;
        f.KeyKind = scalar_kind_of<K>();
        f.KeySize = (std::uint16_t)sizeof(K);
        if constexpr (std::is_enum_v<K>) {
            f.KeyTypeName = type_name<K>().data();
            f.KeyTypeNameLength = (std::uint16_t)type_name<K>().size();
        }
    } else if constexpr (std::is_class_v<T>) {
        f.TypeName = type_name<T>().data();
        f.TypeNameLength = (std::uint16_t)type_name<T>().size();
    } else if constexpr (std::is_enum_v<T>) {
        // An enum keeps its scalar kind, so it still reads and writes as an
        // integer; the type name is what lets the labels be found.
        f.TypeName = type_name<T>().data();
        f.TypeNameLength = (std::uint16_t)type_name<T>().size();
    }

    return f;
}

// Components carry two names, and both are wanted.
//
// EngineClass is the engine's own name ("eoc::HealthComponent"), which is what
// bg3le's symbol-table registry is keyed by. ComponentName is bg3se's short
// name ("Health"), which is what its Lua API exposes -- so carrying it means
// entity.Health resolves generically instead of through a hardcoded map.
//
// A resource declares EngineClass as well -- "eoc::ActionResourceTypes" for
// resource::ActionResource -- and bg3se's own static data binding reads it from
// exactly there, so this asks for the member rather than for componenthood.
// Gating it on IsComponentType left every resource with a null engine class,
// which is what made Ext.StaticData.Get fail before it reached a bank.
// A resource is recognised by ResourceManagerType, which only the static data
// types declare. Asking merely for an EngineClass member is too broad: 682
// other bg3se types -- systems, singletons -- declare one too, and pulling
// those in makes every count over "things with an engine name" mean something
// different from what it says.
template <class T, class = void>
constexpr bool kIsResourceType = false;
template <class T>
constexpr bool kIsResourceType<T, std::void_t<decltype(T::ResourceManagerType)>> =
    true;

template <class T>
constexpr char const* engine_class_of() {
    if constexpr (IsComponentType<T>) return T::EngineClass;
    else if constexpr (kIsResourceType<T>) return T::EngineClass;
    else return nullptr;
}

// Whether this is an ECS component, as opposed to a resource that merely also
// has an engine name. Separate so the component count stays a count of
// components.
template <class T>
constexpr bool is_component_class() {
    if constexpr (IsComponentType<T>) return true;
    else return false;
}

template <class T>
constexpr char const* component_name_of() {
    if constexpr (IsComponentType<T>) return T::ComponentName;
    else return nullptr;
}

// Whether the entity's page holds a pointer to the component rather than the
// component itself.
//
// These exist and they are not rare: 46 of the 586 components a live save
// carries are proxies, including BoundComponent, esv::Character, esv::Item and
// every esv trigger. Reading one as though it were inline reads the pointer's
// own bytes as the first fields, which is not a subtle kind of wrong -- and it
// was only caught by comparing every component's declared size against the
// size the engine recorded, because the engine records 8 for all of them.
template <class T>
constexpr bool is_proxy_component() {
    if constexpr (IsProxyComponentType<T>) return true;
    else return false;
}

// Whether the component lives in a per-storage pool rather than in the entity
// page. These are the transient event components; the engine keeps them keyed
// by entity in a pool of their own, so reading one through the page returns
// whatever is at that offset. bg3se records it on the component itself.
template <class T>
constexpr bool is_one_frame_component() {
    if constexpr (IsComponentType<T>) return T::OneFrame;
    else return false;
}

struct ClassFields {
    char const* Name;           // the C++ class name, what INHERIT refers to
    char const* ComponentName;  // bg3se's short name, or null
    char const* EngineClass;    // the engine's name, or null
    // The fully qualified type name, written by the same type_name<T>() that
    // writes a field's type name, so a nested field type resolves by an exact
    // compare rather than by guessing at qualification.
    std::string_view TypeName;
    FieldDesc const* Fields;
    // The size of the struct itself.
    std::size_t Size;
    // Whether the page holds a pointer to the component rather than the
    // component inline. For one of these the stride is a pointer and the
    // pointer has to be followed; see bg3le_meta_component_stride.
    bool IsProxy;
    // Whether the component lives in a per-storage pool instead of the entity
    // page, in which case the page path does not apply to it at all.
    bool IsOneFrame;
    // Whether this is an ECS component at all. A resource has an engine name
    // but no place in the entity world.
    bool IsComponent;
    // A static data resource's ExtResourceManagerType, or -1.
    std::int32_t ResourceType;
    // Whether upstream's property map has a Construct: default-constructible
    // and not a Noesis object (LuaObjectProxies.cpp's GetConstructor).
    bool IsConstructible;
    // Upstream's Construct and ProxyDestroy for a component (GetProxyDestructor).
    void (*Construct)(void*);
    void (*ProxyDestroy)(void**);
};

template <class T>
void construct_thunk(void* at) {
    new (at) T();
}

template <class T>
void proxy_destroy_thunk(void** at) {
    if (*at) {
        static_cast<T*>(*at)->~T();
        GameFree(*at);
        *at = nullptr;
    }
}

template <class T>
constexpr bool constructible() {
    return std::is_default_constructible_v<T> && !std::is_base_of_v<Noesis::BaseObject, T>
           && !std::is_base_of_v<Noesis::Interface, T>;
}

template <class T>
constexpr std::int32_t resource_type_of() {
    if constexpr (kIsResourceType<T>) {
        return (std::int32_t)T::ResourceManagerType;
    } else {
        return -1;
    }
}

template <class T>
struct FieldTable;

// An enum's labels, so a field holding one reads as a name rather than as a
// number. bg3se generates these the same macro-driven way it generates the
// property maps, so they come out the same way: by including the generated
// file with different macros.
//
// A bitmask is kept apart from a plain enum because they present differently
// -- bg3se renders a bitmask as the list of set flags, and matching that
// matters for scripts written against it.
struct EnumLabel {
    char const* Name;
    std::uint64_t Value;
};

struct EnumDesc {
    std::string_view TypeName;
    char const* Name;
    // The name a script writes. bg3se's generated metadata gives a namespaced
    // enum a Lua name of its own -- ecl::GameState is ClientGameState -- and
    // that is the only name Ext.Enums is reachable by, so it has to survive
    // the re-expansion rather than being replaced by the C++ spelling.
    char const* LuaName;
    bool IsBitmask;
    EnumLabel const* Labels;  // null-terminated
};

template <class T>
struct EnumTable;

}  // namespace bg3le

// ---------------------------------------------------------------------------
// The re-expansion.
//
// Only the offset-based macros produce entries. P_FUN, P_GETTER,
// P_FREE_GETTER, P_GETTER_SETTER and P_FALLBACK describe computed properties
// with no storage of their own, so there is no offset to record; P_BITMASK has
// one but needs the bit position too, and is left out until it is needed. All
// of them expand to nothing rather than to a wrong entry.
//
// offsetof on these classes is not standard -- most are not standard-layout --
// but it is what upstream uses to build the same table, and it is how every
// component offset in bg3le has been derived so far.
// ---------------------------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

#define GENERATING_PROPMAP

#define BEGIN_CLS_TN(cls, typeName, id)                                       \
    namespace bg3le {                                                         \
    template <> struct FieldTable<cls> {                                      \
        using ObjectType = cls;                                               \
        static constexpr char const* kName = #cls;                            \
        static constexpr char const* kComponentName =                         \
            component_name_of<cls>();                                         \
        static constexpr char const* kEngineClass = engine_class_of<cls>();   \
        static constexpr std::string_view kTypeName = type_name<cls>();       \
        static constexpr FieldDesc kFields[] = {

#define BEGIN_CLS(cls, id) BEGIN_CLS_TN(cls, cls, id)

// A class with no offset-based fields would otherwise declare a zero-length
// array, which is not valid C++, so every table is null-terminated.
#define END_CLS()                                                             \
            FieldDesc{},  /* the null terminator */                          \
        };                                                                    \
    };                                                                        \
    }

// Recorded as an entry rather than as a member, so it needs no surgery on the
// initialiser list and so multiple bases work. Resolved by name at load,
// because a class's table may be declared before its base's.
#define INHERIT(base)                                                         \
        inherit_field(#base),

#define PN(name, prop)                                                        \
        make_field<decltype(ObjectType::prop)>(                               \
            #name, offsetof(ObjectType, prop)),

#define P(prop) PN(prop, prop)
#define P_RO(prop) PN(prop, prop)
#define PN_RO(name, prop) PN(name, prop)
#define P_NOTIFY(prop, notify) PN(prop, prop)
// The old name too, as upstream keeps it: mods still use field_1 and the like.
#define P_RENAMED(prop, oldName)                                              \
        PN(prop, prop)                                                        \
        make_legacy_field<decltype(ObjectType::prop)>(                        \
            #oldName, offsetof(ObjectType, prop)),

#define P_BITMASK(prop)                                                       \
        make_bitmask_field<decltype(ObjectType::prop)>(                       \
            #prop, offsetof(ObjectType, prop)),
#define P_BITMASK_GETTER_SETTER(prop, getter, setter)
#define P_GETTER(name, fun)
#define P_FREE_GETTER(name, fun)
#define P_GETTER_SETTER(name, getter, setter)
#define P_FUN(name, fun)
#define P_FALLBACK(getter, setter, next)

#include <GameDefinitions/Generated/PropertyMaps.inl>

#undef GENERATING_PROPMAP
#undef BEGIN_CLS
#undef BEGIN_CLS_TN
#undef END_CLS
#undef INHERIT
#undef P
#undef PN
#undef P_RO
#undef PN_RO
#undef P_NOTIFY
#undef P_RENAMED
#undef P_BITMASK
#undef P_BITMASK_GETTER_SETTER
#undef P_GETTER
#undef P_FREE_GETTER
#undef P_GETTER_SETTER
#undef P_FUN
#undef P_FALLBACK

#pragma clang diagnostic pop

// ---------------------------------------------------------------------------
// The enum labels, from the same generated metadata.
//
// Two passes, as with the property maps: one to declare a table per enum, one
// to collect them. The type is in scope inside the expansion, so both the
// table and the field that refers to it are named by the same type_name<T>()
// and compare exactly.
// ---------------------------------------------------------------------------

#define BEGIN_ENUM_IMPL(cls, luaname, bitmask)                                \
    namespace bg3le {                                                         \
    template <> struct EnumTable<cls> {                                       \
        static constexpr std::string_view kTypeName = type_name<cls>();       \
        static constexpr char const* kName = #cls;                            \
        static constexpr char const* kLuaName = #luaname;                     \
        static constexpr bool kIsBitmask = bitmask;                           \
        static constexpr EnumLabel kLabels[] = {

#define BEGIN_ENUM(T, type, id) BEGIN_ENUM_IMPL(T, T, false)
#define BEGIN_BITMASK(T, type, id) BEGIN_ENUM_IMPL(T, T, true)
#define BEGIN_ENUM_NS(NS, T, luaName, type, id) \
    BEGIN_ENUM_IMPL(NS::T, luaName, false)
#define BEGIN_BITMASK_NS(NS, T, luaName, type, id) \
    BEGIN_ENUM_IMPL(NS::T, luaName, true)

#define EV(label, value) { #label, (std::uint64_t)(value) },

// Null-terminated, so a table with no values is still a valid array.
#define END_ENUM()                                                            \
            { nullptr, 0 },                                                   \
        };                                                                    \
    };                                                                        \
    }
#define END_ENUM_NS() END_ENUM()

#include <GameDefinitions/Generated/Enumerations.inl>
#include <GameDefinitions/Generated/ExternalEnumerations.inl>

#undef BEGIN_ENUM_IMPL
#undef BEGIN_ENUM
#undef BEGIN_BITMASK
#undef BEGIN_ENUM_NS
#undef BEGIN_BITMASK_NS
#undef EV
#undef END_ENUM
#undef END_ENUM_NS

namespace bg3le {
namespace {

template <class T>
inline constexpr EnumDesc kEnumDesc{
    EnumTable<T>::kTypeName,
    EnumTable<T>::kName,
    EnumTable<T>::kLuaName,
    EnumTable<T>::kIsBitmask,
    EnumTable<T>::kLabels,
};

constexpr EnumDesc const* kAllEnums[] = {
#define BEGIN_ENUM(T, type, id) &kEnumDesc<T>,
#define BEGIN_BITMASK(T, type, id) &kEnumDesc<T>,
#define BEGIN_ENUM_NS(NS, T, luaName, type, id) &kEnumDesc<NS::T>,
#define BEGIN_BITMASK_NS(NS, T, luaName, type, id) &kEnumDesc<NS::T>,
#define EV(label, value)
#define END_ENUM()
#define END_ENUM_NS()
#include <GameDefinitions/Generated/Enumerations.inl>
#include <GameDefinitions/Generated/ExternalEnumerations.inl>
#undef BEGIN_ENUM
#undef BEGIN_BITMASK
#undef BEGIN_ENUM_NS
#undef BEGIN_BITMASK_NS
#undef EV
#undef END_ENUM
#undef END_ENUM_NS
};

// Enums by their type_name<T>() spelling, the same index the class tables use.
std::unordered_map<std::string_view, EnumDesc const*>& by_enum_name() {
    static std::unordered_map<std::string_view, EnumDesc const*> map = [] {
        std::unordered_map<std::string_view, EnumDesc const*> m;
        m.reserve(std::size(kAllEnums));
        for (auto const* e : kAllEnums) m.emplace(e->TypeName, e);
        return m;
    }();
    return map;
}

}  // namespace
}  // namespace bg3le

namespace bg3le {
namespace {

template <class T>
inline constexpr ClassFields kClassFields{
    FieldTable<T>::kName,
    FieldTable<T>::kComponentName,
    FieldTable<T>::kEngineClass,
    FieldTable<T>::kTypeName,
    FieldTable<T>::kFields,
    sizeof(T),
    is_proxy_component<T>(),
    is_one_frame_component<T>(),
    is_component_class<T>(),
    resource_type_of<T>(),
    constructible<T>(),
    [] {
        if constexpr (constructible<T>() && !std::is_abstract_v<T>) return &construct_thunk<T>;
        else return (void (*)(void*))nullptr;
    }(),
    [] {
        if constexpr (is_proxy_component<T>() && std::is_default_constructible_v<T>
                      && !std::is_trivially_destructible_v<T>) {
            return &proxy_destroy_thunk<T>;
        } else {
            return (void (*)(void**))nullptr;
        }
    }(),
};

// Every class table, collected the way upstream collects its own.
constexpr ClassFields const* kAllClasses[] = {
#define DECLARE_CLS(id, ...) &kClassFields<__VA_ARGS__>,
#define DECLARE_CLS_FWD(id, cls) &kClassFields<cls>,
#define DECLARE_CLS_NS_FWD(id, ns, cls) &kClassFields<ns::cls>,
#define DECLARE_CLS_BARE_NS_FWD(id, ns, cls) &kClassFields<::ns::cls>,
#define DECLARE_STRUCT_BARE_NS_FWD(id, ns, cls) &kClassFields<::ns::cls>,
#include <GameDefinitions/Generated/PropertyMapNames.inl>
#undef DECLARE_CLS
#undef DECLARE_CLS_FWD
#undef DECLARE_CLS_NS_FWD
#undef DECLARE_CLS_BARE_NS_FWD
#undef DECLARE_STRUCT_BARE_NS_FWD
};

// Name -> table, built once. Both indexes are wanted: the engine name is what
// the ECS registry gives us, the class name is what INHERIT refers to.
std::unordered_map<std::string_view, ClassFields const*>& by_class_name() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        m.reserve(std::size(kAllClasses) * 2);
        for (auto const* cls : kAllClasses) m.emplace(cls->Name, cls);
        return m;
    }();
    return map;
}

// Resources by their ExtResourceManagerType label, which is how upstream's
// Ext.StaticData names them. Seventeen differ from the class name --
// "ColorDefinition" is resource::Color -- so the label is looked up rather
// than assumed.
std::unordered_map<std::string_view, ClassFields const*>& by_resource_label() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        auto e = by_enum_name().find(type_name<ExtResourceManagerType>());
        if (e == by_enum_name().end()) return m;
        for (auto const* cls : kAllClasses) {
            if (cls->ResourceType < 0) continue;
            for (auto const* l = e->second->Labels; l->Name != nullptr; ++l) {
                if (l->Value == (std::uint64_t)cls->ResourceType) {
                    m.emplace(l->Name, cls);
                    break;
                }
            }
        }
        return m;
    }();
    return map;
}

// Indexed under both names a component has, so callers can use whichever they
// have: the engine name from the ECS registry, or bg3se's short name from a
// script. They cannot collide -- one is namespace-qualified and the other is
// not.
std::unordered_map<std::string_view, ClassFields const*>& by_component_name() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        for (auto const* cls : kAllClasses) {
            if (cls->EngineClass != nullptr) m.emplace(cls->EngineClass, cls);
            if (cls->ComponentName != nullptr) m.emplace(cls->ComponentName, cls);
        }
        return m;
    }();
    return map;
}

// Walks a class and its bases for a field. Bases contribute at the same offset
// because bg3se's components inherit from empty tag bases, which is also why
// the offsets need no adjustment.
FieldDesc const* find_field(ClassFields const* cls, char const* name,
                            unsigned depth = 0) {
    if (cls == nullptr || depth > 8) return nullptr;

    for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
        if (f->Kind == FieldKind::Inherit) continue;
        if (std::strcmp(f->Name, name) == 0) return f;
    }

    for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
        if (f->Kind != FieldKind::Inherit) continue;
        auto it = by_class_name().find(f->Name);
        if (it == by_class_name().end()) continue;
        if (auto const* found = find_field(it->second, name, depth + 1)) {
            return found;
        }
    }
    return nullptr;
}

// Types by their type_name<T>() spelling, so a Struct field can be resolved to
// the table describing it. Only the types bg3se describes are in here; a field
// whose type it does not describe -- a HashMap, say -- simply misses.
std::unordered_map<std::string_view, ClassFields const*>& by_type_name() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        m.reserve(std::size(kAllClasses));
        for (auto const* cls : kAllClasses) m.emplace(cls->TypeName, cls);
        return m;
    }();
    return map;
}

// The table describing a Struct field's type, or null if bg3se does not
// describe it.
ClassFields const* struct_type_of(FieldDesc const* field) {
    if (field == nullptr
        || (field->Kind != FieldKind::Struct && field->Kind != FieldKind::Pointer)
        || field->TypeName == nullptr) {
        return nullptr;
    }
    auto it = by_type_name().find(
        std::string_view(field->TypeName, field->TypeNameLength));
    return it != by_type_name().end() ? it->second : nullptr;
}

// The table describing an array field's element type, or null.
ClassFields const* elem_type_of(FieldDesc const* field) {
    if (field == nullptr || field->ElemTypeName == nullptr) return nullptr;
    auto it = by_type_name().find(
        std::string_view(field->ElemTypeName, field->ElemTypeNameLength));
    return it != by_type_name().end() ? it->second : nullptr;
}

// What a path resolved to.
//
// Address is only filled in when a base was supplied. It has to be, rather
// than an offset being enough, because crossing a dynamic array means
// following its buffer pointer -- the element does not live at a fixed offset
// from the component at all.
struct Resolved {
    FieldDesc Field{};      // synthesised for an element, copied for a field
    void* Address{nullptr};
    bool Ok{false};
};

// Whether a container header can be read. Its accessors read the header
// directly, and a container reached through a pointer taken from a struct
// whose layout is not the engine's can be anywhere.
bool header_readable(void const* at, std::size_t size) {
    unsigned char probe[64];
    const std::size_t n = size == 0 ? 8 : (size < sizeof(probe) ? size : sizeof(probe));
    return at != nullptr && safe_read(at, probe, n);
}

bool is_container_kind(FieldKind kind) {
    return kind == FieldKind::DynArray || kind == FieldKind::Map
           || kind == FieldKind::Optional || kind == FieldKind::Variant;
}

// A count no real container has, which means the header was not one.
constexpr std::size_t kMaxContainerCount = std::size_t(1) << 24;

// Resolves a path, which may name nested fields and index arrays:
//
//   "Hp"                     a field
//   "Transform.Translate"    a field of a nested struct
//   "Resources[2].Amount"    a field of an element of a dynamic array
//
// base may be null to resolve the type only, which is what listing fields and
// reporting kinds need; then Address stays null and an array index is still
// crossed, because the element type is known statically even when the element
// address is not.
Resolved resolve_path(ClassFields const* cls, char const* path, void* base) {
    Resolved out;
    if (cls == nullptr || path == nullptr) return out;

    std::string_view rest(path);
    void* address = base;

    // Bounded so a pathological path cannot spin; nothing nests near this
    // deep.
    for (unsigned step = 0; step < 16; ++step) {
        // A segment is a name, optionally followed by [index], and the path
        // continues after a dot.
        const auto dot = rest.find('.');
        std::string_view segment =
            dot == std::string_view::npos ? rest : rest.substr(0, dot);
        if (segment.empty()) return out;

        // A segment may carry more than one subscript, because indexing a
        // container can yield another one: a map of arrays reads as
        // "Resources[0][1]".
        std::string_view subscripts;
        if (const auto open = segment.find('['); open != std::string_view::npos) {
            if (segment.back() != ']') return out;
            subscripts = segment.substr(open);
            segment = segment.substr(0, open);
            if (segment.empty()) return out;
        }

        // find_field takes a NUL-terminated name; a path segment is not one.
        const std::string name(segment);
        FieldDesc const* field = find_field(cls, name.c_str());
        if (field == nullptr) return out;

        FieldDesc current = *field;
        if (address != nullptr) {
            address = (char*)address + field->Offset;
        }

        // Apply each subscript in turn, the element descriptor of one becoming
        // the container of the next.
        while (!subscripts.empty()) {
            if (subscripts.front() != '[') return out;
            const auto close = subscripts.find(']');
            if (close == std::string_view::npos) return out;

            if (address != nullptr && is_container_kind(current.Kind)
                && !header_readable(address, current.Size)) {
                return out;
            }

            const auto digits = subscripts.substr(1, close - 1);
            if (digits.empty()) return out;
            std::size_t index = 0;
            for (const char c : digits) {
                if (c < '0' || c > '9') return out;
                index = index * 10 + (std::size_t)(c - '0');
                if (index > 0xffffff) return out;  // absurd; refuse
            }
            subscripts = subscripts.substr(close + 1);

            // A variant is the one container whose element type is not fixed,
            // so it is handled before the size and descriptor checks the
            // others share.
            if (current.Kind == FieldKind::Variant) {
                if (current.Alternatives == nullptr
                    || current.ActiveIndex == nullptr
                    || current.Data == nullptr) {
                    return out;
                }

                std::size_t alternatives = 0;
                while (current.Alternatives[alternatives] != nullptr) {
                    ++alternatives;
                }
                if (index >= alternatives) return out;

                if (address != nullptr) {
                    // Only the alternative actually held resolves: the bytes
                    // are not any of the others, and reading them as though
                    // they were is exactly the sort of plausible nonsense
                    // worth refusing.
                    if (current.ActiveIndex(address) != index) return out;
                    void* held = current.Data(address);
                    if (held == nullptr) return out;
                    address = held;
                }

                const bool readOnlyVariant = current.ReadOnly;
                char const* variantName = current.Name;
                current = *current.Alternatives[index];
                current.Name = variantName;
                current.ReadOnly = current.ReadOnly || readOnlyVariant;
                continue;
            }

            if (current.ElemSize == 0 || current.ElemDesc == nullptr) return out;

            switch (current.Kind) {
            // What a pointer points at, when that is not an object read as one
            // of its own: index 0 only.
            case FieldKind::Pointer: {
                if (index != 0) return out;
                if (address != nullptr) {
                    void* target = nullptr;
                    if (!safe_read(address, &target, sizeof(target)) || target == nullptr) {
                        return out;
                    }
                    const auto at = (std::uintptr_t)target;
                    std::uint64_t probe = 0;
                    if (at < 0x10000 || at >= 0x800000000000ull || (at & 7) != 0
                        || !safe_read(target, &probe, sizeof(probe))) {
                        return out;
                    }
                    address = target;
                }
                break;
            }

            case FieldKind::ScalarArray:
                if (index >= current.ElemCount) return out;
                if (address != nullptr) {
                    address = (char*)address + index * current.ElemSize;
                }
                break;

            // A map indexes to its value, which is what makes the value side
            // reachable by slot; the key side is read separately, because a
            // path has no way to say "the key of this slot".
            case FieldKind::DynArray:
            case FieldKind::Map:
            case FieldKind::Optional: {
                if (current.Count == nullptr
                    || (current.Data == nullptr && current.ElemAt == nullptr)) {
                    return out;
                }
                if (address != nullptr) {
                    const std::size_t count = current.Count(address);
                    if (index >= count || count > kMaxContainerCount) return out;
                    if (current.ElemAt != nullptr) {
                        address = current.ElemAt(address, index);
                        if (address == nullptr) return out;
                        break;
                    }
                    void* data = current.Data(address);
                    if (data == nullptr) return out;
                    address = (char*)data + index * current.ElemSize;
                }
                break;
            }

            default:
                return out;  // not a container; nothing to index
            }

            // Continue with the element's own descriptor, which carries its
            // accessors if it is itself a container. Read-only propagates: an
            // element of a hash set is one of the keys its hashes were
            // computed from.
            const bool readOnly = current.ReadOnly;
            char const* fieldName = current.Name;
            current = *current.ElemDesc;
            current.Name = fieldName;
            current.ReadOnly = current.ReadOnly || readOnly;
        }

        if (dot == std::string_view::npos) {
            if (address != nullptr && is_container_kind(current.Kind)
                && (!header_readable(address, current.Size)
                    || (current.Count != nullptr
                        && current.Count(address) > kMaxContainerCount))) {
                return out;
            }
            out.Field = current;
            out.Address = address;
            out.Ok = true;
            return out;
        }

        cls = struct_type_of(&current);
        if (cls == nullptr) return out;  // cannot descend through this
        if (current.Kind == FieldKind::Pointer && address != nullptr) {
            void* target = nullptr;
            if (!safe_read(address, &target, sizeof(target)) || target == nullptr) {
                return out;
            }
            // Where an object could be, and readable there.
            const auto at = (std::uintptr_t)target;
            std::uint64_t probe = 0;
            if (at < 0x10000 || at >= 0x800000000000ull || (at & 7) != 0
                || !safe_read(target, &probe, sizeof(probe))) {
                return out;
            }
            address = target;
        }
        rest = rest.substr(dot + 1);
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The C interface bg3le uses.
// ---------------------------------------------------------------------------

// Looks a component up by its engine name, as it appears in the ECS registry
// ("eoc::HealthComponent"). Returns an opaque handle, or null if bg3se has no
// metadata for it.
// The declared type of the field at a path, for the same reason: a nested
// struct is an object with a type of its own, and a view over it should say
// which. Not NUL-terminated -- it is a slice of a compiler-generated name --
// so the length comes back too.
extern "C" bool bg3le_meta_type_name_at(void const* handle, char const* path,
                                        char const** name,
                                        std::uint16_t* length) {
    if (handle == nullptr || name == nullptr || length == nullptr) return false;
    *name = nullptr;
    *length = 0;

    auto const* cls = static_cast<ClassFields const*>(handle);
    if (path == nullptr || path[0] == '\0') {
        if (cls->Name == nullptr) return false;
        *name = cls->Name;
        *length = (std::uint16_t)std::strlen(cls->Name);
        return true;
    }

    const auto r = resolve_path(cls, path, nullptr);
    if (!r.Ok) return false;

    // A struct field carries its type; an array of structs carries its
    // element's, which is what an element view wants.
    char const* found = r.Field.TypeName;
    std::uint16_t len = r.Field.TypeNameLength;
    if (found == nullptr) {
        found = r.Field.ElemTypeName;
        len = r.Field.ElemTypeNameLength;
    }
    if (found == nullptr || len == 0) return false;

    *name = found;
    *length = len;
    return true;
}

extern "C" void const* bg3le_meta_component(char const* engineName) {
    if (engineName == nullptr) return nullptr;
    auto it = by_component_name().find(engineName);
    return it != by_component_name().end() ? it->second : nullptr;
}

// The declared size of a component, which is the stride bg3se uses to walk a
// component page. Without metadata there is no safe stride, hence 0.
extern "C" std::size_t bg3le_meta_component_size(void const* handle) {
    if (handle == nullptr) return 0;
    return static_cast<ClassFields const*>(handle)->Size;
}

// The stride the engine walks the component page with, which is what
// GetComponent multiplies the entity's slot by. For a proxy component that is
// a pointer, not the struct: the struct lives wherever the pointer says.
extern "C" std::size_t bg3le_meta_component_stride(void const* handle) {
    if (handle == nullptr) return 0;
    auto const* cls = static_cast<ClassFields const*>(handle);
    return cls->IsProxy ? sizeof(void*) : cls->Size;
}

extern "C" bool bg3le_meta_component_is_proxy(void const* handle) {
    if (handle == nullptr) return false;
    return static_cast<ClassFields const*>(handle)->IsProxy;
}

// Upstream's Construct, into zeroed memory as CreateComponentRaw leaves it.
extern "C" bool bg3le_meta_construct(void const* handle, void* at) {
    auto const* cls = static_cast<ClassFields const*>(handle);
    if (cls == nullptr || cls->Construct == nullptr || at == nullptr) return false;
    std::memset(at, 0, cls->Size);
    cls->Construct(at);
    return true;
}

// Fills a new component's slot as upstream's CreateComponentRaw does: a
// proxy's slot gets a pointer to a fresh object. The component, or null.
extern "C" void* bg3le_meta_construct_component(void const* handle, void* slot) {
    auto const* cls = static_cast<ClassFields const*>(handle);
    if (cls == nullptr || cls->Construct == nullptr || slot == nullptr) return nullptr;
    void* at = slot;
    if (cls->IsProxy) {
        at = GameAllocRaw(cls->Size);
        if (at == nullptr) return nullptr;
        *(void**)slot = at;
    }
    std::memset(at, 0, cls->Size);
    cls->Construct(at);
    return at;
}

extern "C" std::size_t bg3le_meta_class_size(void const* handle) {
    return handle != nullptr ? static_cast<ClassFields const*>(handle)->Size : 0;
}

extern "C" void* bg3le_meta_proxy_destroy(void const* handle) {
    return handle != nullptr ? (void*)static_cast<ClassFields const*>(handle)->ProxyDestroy : nullptr;
}

extern "C" bool bg3le_meta_class_constructible(void const* handle) {
    return handle != nullptr && static_cast<ClassFields const*>(handle)->IsConstructible;
}

extern "C" bool bg3le_meta_component_is_one_frame(void const* handle) {
    if (handle == nullptr) return false;
    return static_cast<ClassFields const*>(handle)->IsOneFrame;
}

namespace {

// The kind to report for a resolved field. A struct with no table behind it,
// and an array whose elements are structs with no table, cannot be acted on,
// so they are reported as unsupported rather than as something traversable.
std::uint8_t reportable_kind(FieldDesc const& field, unsigned depth = 0) {
    if (depth > 4) return (std::uint8_t)FieldKind::Unsupported;

    if (field.Kind == FieldKind::Struct && struct_type_of(&field) == nullptr) {
        return (std::uint8_t)FieldKind::Unsupported;
    }
    // A pointer is usable if what it points at is: a described class, or
    // anything its element descriptor can read.
    if (field.Kind == FieldKind::Pointer) {
        if (struct_type_of(&field) != nullptr) return (std::uint8_t)FieldKind::Pointer;
        if (field.ElemDesc != nullptr
            && reportable_kind(*field.ElemDesc, depth + 1)
                   != (std::uint8_t)FieldKind::Unsupported) {
            return (std::uint8_t)FieldKind::Pointer;
        }
        return (std::uint8_t)FieldKind::Unsupported;
    }

    if (field.Kind == FieldKind::Variant) {
        if (field.Alternatives == nullptr) {
            return (std::uint8_t)FieldKind::Unsupported;
        }
        // Usable if any alternative is: a variant of a readable type and an
        // unreadable one is still worth having, and which it holds is a
        // runtime question.
        for (auto const* const* alt = field.Alternatives; *alt != nullptr; ++alt) {
            if (reportable_kind(**alt, depth + 1)
                != (std::uint8_t)FieldKind::Unsupported) {
                return (std::uint8_t)FieldKind::Variant;
            }
        }
        return (std::uint8_t)FieldKind::Unsupported;
    }

    if (field.Kind == FieldKind::ScalarArray || field.Kind == FieldKind::DynArray
        || field.Kind == FieldKind::Map || field.Kind == FieldKind::Optional) {
        // A container is usable if its elements are: a scalar, a struct bg3se
        // describes, or another container. That last case is why this
        // recurses rather than testing the element fields directly -- a
        // HashMap<Guid, Array<Entry>> has no scalar element kind and no
        // element struct, but indexing it twice reaches an Entry, so reporting
        // it unsupported would hide a field that works.
        // A pointer element is usable only if what it points at is described,
        // which the element descriptor answers.
        if (field.ElemKind != FieldKind::Unsupported
            && field.ElemKind != FieldKind::Pointer) {
            return (std::uint8_t)field.Kind;
        }
        if (elem_type_of(&field) != nullptr) {
            return (std::uint8_t)field.Kind;
        }
        if (field.ElemDesc != nullptr
            && reportable_kind(*field.ElemDesc, depth + 1)
                   != (std::uint8_t)FieldKind::Unsupported) {
            return (std::uint8_t)field.Kind;
        }
        return (std::uint8_t)FieldKind::Unsupported;
    }

    return (std::uint8_t)field.Kind;
}

}  // namespace

// Resolves a field of a component by name or by dotted path, following base
// classes. Type information only -- no component instance, so a path may
// index an array (the element type is static) but the offset returned is not
// meaningful once it has, because a dynamic array's elements do not live at a
// fixed offset from the component. Use bg3le_meta_resolve to reach a value.
extern "C" bool bg3le_meta_field(void const* handle, char const* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount) {
    if (handle == nullptr || name == nullptr) return false;
    const auto r =
        resolve_path(static_cast<ClassFields const*>(handle), name, nullptr);
    if (!r.Ok) return false;
    *offset = r.Field.Offset;
    *size = r.Field.Size;
    *kind = reportable_kind(r.Field);
    *elemKind = (std::uint8_t)r.Field.ElemKind;
    *elemCount = r.Field.ElemCount;
    return true;
}

// Resolves a path against a live component and hands back the address of the
// value, following array buffers where the path indexes one.
extern "C" bool bg3le_meta_resolve(void const* handle, char const* path,
                                   void* component, void** address,
                                   std::uint8_t* kind, std::uint16_t* size,
                                   bool* readOnly) {
    *address = nullptr;
    *readOnly = false;
    if (handle == nullptr || path == nullptr || component == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;

    *address = r.Address;
    *kind = reportable_kind(r.Field);
    *size = r.Field.Size;
    *readOnly = r.Field.ReadOnly;
    return true;
}

// What a live hash set actually looks like, against what is known about
// it independently: the keys pointer and the key count both come from the
// container's own accessors, which reading a set has always used and which
// are therefore trustworthy. Everything else is read off the bytes.
//
// This is how the three members get located without taking a vendored
// header's word for their offsets.
extern "C" void bg3le_meta_set_dump(void const* handle, char const* path,
                                   void* component) {
    if (handle == nullptr || path == nullptr || component == nullptr) return;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                               component);
    if (!r.Ok || r.Address == nullptr) {
        logf("set dump: %s does not resolve", path);
        return;
    }
    if (r.Field.Count == nullptr || r.Field.Data == nullptr) {
        logf("set dump: %s is not a container", path);
        return;
    }

    void const* keys = r.Field.Data(r.Address);
    const std::size_t count = r.Field.Count(r.Address);
    logf("set dump: %s at %p, keys %p, %zu of them, element %u bytes", path,
         r.Address, keys, count, r.Field.ElemSize);

    for (std::size_t off = 0; off < 64; off += 8) {
        std::uint64_t word = 0;
        std::uint32_t lo = 0, hi = 0;
        std::memcpy(&word, (char const*)r.Address + off, sizeof(word));
        std::memcpy(&lo, (char const*)&word, sizeof(lo));
        std::memcpy(&hi, (char const*)&word + 4, sizeof(hi));
        logf("set dump:   +%02zx = %#018llx  (u32 %u, %u)%s", off,
             (unsigned long long)word, lo, hi,
             (void const*)word == keys ? "  <- keys buffer" : "");
    }
}

// Replaces a set's contents with the values given.
//
// elemSize is checked against the field's own, so a caller that has
// misunderstood the element type is refused rather than reinterpreting its
// bytes -- the difference between writing FixedStrings and writing
// something else four bytes wide.
extern "C" bool bg3le_meta_set_assign(void const* handle, char const* path,
                                      void* component, void const* values,
                                      std::size_t count,
                                      std::size_t elemSize) {
    if (handle == nullptr || path == nullptr || component == nullptr) {
        return false;
    }
    if (values == nullptr && count > 0) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;
    if (r.Field.Assign == nullptr) return false;
    if (elemSize != r.Field.ElemSize) return false;

    return r.Field.Assign(r.Address, values, count);
}

// Engages or clears an optional field, and hands back where its payload lives
// so the caller can write it.
//
// What makes an optional writable at all: the flag and the payload are one
// object as far as the type is concerned, and only the type knows where the
// flag is. Reading one never needed that -- has_value() and operator* are
// enough -- so this is the write side arriving late.
extern "C" char const* bg3le_meta_kind_name(std::uint8_t kind);

extern "C" bool bg3le_meta_optional_set(void const* handle, char const* path,
                                        void* component, bool engaged,
                                        void** payload, std::uint8_t* kind,
                                        std::uint8_t* elemKind,
                                        std::uint16_t* elemCount) {
    if (handle == nullptr || path == nullptr || component == nullptr) {
        return false;
    }

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) {
        logf("optional set: %s does not resolve", path);
        return false;
    }
    if (r.Field.Kind != FieldKind::Optional) {
        logf("optional set: %s is %s, not an optional", path,
             bg3le_meta_kind_name((std::uint8_t)r.Field.Kind));
        return false;
    }
    if (r.Field.Engage == nullptr || r.Field.Data == nullptr) {
        logf("optional set: %s has no %s accessor", path,
             r.Field.Engage == nullptr ? "engage" : "data");
        return false;
    }

    r.Field.Engage(r.Address, engaged);

    if (payload != nullptr) {
        *payload = engaged ? r.Field.Data(r.Address) : nullptr;
    }

    // The payload is described by the element descriptor, not by the
    // optional's own scalar fields: an optional<glm::vec2> holds something
    // that is itself an array of two floats, and the optional's ElemKind
    // says nothing about that. Writing through the wrong one is how the
    // engage succeeded and the write then quietly failed.
    FieldDesc const* inner =
        r.Field.ElemDesc != nullptr ? r.Field.ElemDesc : &r.Field;
    if (kind != nullptr) *kind = reportable_kind(*inner);
    if (elemKind != nullptr) *elemKind = (std::uint8_t)inner->ElemKind;
    if (elemCount != nullptr) *elemCount = inner->ElemCount;
    return true;
}

// Whether an optional field holds anything, and where. The read side of
// bg3le_meta_optional_set, and the same reason it needs a descriptor rather
// than an address: has_value() belongs to the type.
//
// Returns false if the path is not an optional at all; `engaged` says
// whether it holds a value.
extern "C" bool bg3le_meta_optional_get(void const* handle, char const* path,
                                        void* component, bool* engaged,
                                        void** payload, std::uint8_t* kind,
                                        std::uint8_t* elemKind,
                                        std::uint16_t* elemCount) {
    if (handle == nullptr || path == nullptr || component == nullptr) {
        return false;
    }

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;
    if (r.Field.Kind != FieldKind::Optional) return false;
    if (r.Field.Count == nullptr || r.Field.Data == nullptr) return false;

    const bool has = r.Field.Count(r.Address) != 0;
    if (engaged != nullptr) *engaged = has;
    if (payload != nullptr) *payload = has ? r.Field.Data(r.Address) : nullptr;

    FieldDesc const* inner =
        r.Field.ElemDesc != nullptr ? r.Field.ElemDesc : &r.Field;
    if (kind != nullptr) *kind = reportable_kind(*inner);
    if (elemKind != nullptr) *elemKind = (std::uint8_t)inner->ElemKind;
    if (elemCount != nullptr) *elemCount = inner->ElemCount;
    return true;
}

// A field's size as the engine lays it out and as this build compiles it,
// where they differ; `compiled` is zero where they agree. For
// tools/meta-check.c, which lists every disagreement.
extern "C" bool bg3le_meta_field_sizes(void const* handle, char const* name,
                                      std::uint16_t* engine,
                                      std::uint16_t* compiled,
                                      std::uint32_t* offset) {
    if (handle == nullptr || name == nullptr) return false;
    const auto r =
        resolve_path(static_cast<ClassFields const*>(handle), name, nullptr);
    if (!r.Ok) return false;
    if (engine != nullptr) *engine = r.Field.Size;
    if (compiled != nullptr) *compiled = r.Field.CompiledSize;
    if (offset != nullptr) *offset = r.Field.Offset;
    return true;
}

// What a write has to do beyond the bytes of the value, to match the setter
// upstream would have run. Called after every successful field write; a
// no-op for most fields.
//
// An OverrideableProperty is marked overridden, because upstream's setter is
// get<OverrideableProperty<T>>, which builds {value, true} -- but not by
// Ext.Types.Unserialize, whose Unserialize writes only the value. A member of
// one reached by a longer path is not marked either, matching upstream:
// that write goes through MakeObjectRef(&value->Value).
//
// An EntityRef is given a World if it has none. Upstream's get fills World
// with the calling context's world, on both paths, since EntityRef is a
// by-value type. bg3le keeps the world the engine already paired with the
// ref instead, and fills it only when empty -- with the server world, since
// every handle bg3le hands out is resolved there. Two reasons. These refs
// split 77 server, 77 client and 73 in effects and genome blueprints, so the
// object knows its world better than the caller does. And bg3le lets either
// context write server components, which upstream's per-context world cannot
// express. For a server-context write -- the common case -- the two agree.
extern "C" bool bg3le_meta_after_write(void const* handle, char const* path,
                                      void* component, bool unserializing,
                                      void* entityWorld) {
    if (handle == nullptr || path == nullptr || component == nullptr) {
        return false;
    }
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;

    if (r.Field.OverrideFlagAt != 0 && !unserializing) {
        const std::uint8_t overridden = 1;
        std::memcpy((char*)r.Address + r.Field.OverrideFlagAt, &overridden,
                    sizeof(overridden));
    }

    if (r.Field.EntityWorldAt != 0 && entityWorld != nullptr) {
        void* world = nullptr;
        char* at = (char*)r.Address + r.Field.EntityWorldAt;
        std::memcpy(&world, at, sizeof(world));
        if (world == nullptr) std::memcpy(at, &entityWorld, sizeof(void*));
    }
    return true;
}

// The current length of a dynamic array, and the element stride. Needs the
// component because the length is stored in the container.
// Status rather than a bool, because the caller has to be able to tell "this
// resolved and holds nothing" from "this did not resolve". Collapsing the two
// is how an unreadable container came to look like an empty one.
//
// 0 ok, 1 bad arguments, 2 the path does not resolve, 3 not a container,
// 4 a container with no length accessor.
extern "C" int bg3le_meta_array_length(void const* handle, char const* path,
                                       void* component, std::size_t* count,
                                       std::uint16_t* elemSize,
                                       std::uint8_t* elemKind) {
    *count = 0;
    if (handle == nullptr || path == nullptr || component == nullptr) return 1;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return 2;

    *elemSize = r.Field.ElemSize;
    *elemKind = (std::uint8_t)r.Field.ElemKind;

    if (r.Field.Kind == FieldKind::ScalarArray) {
        *count = r.Field.ElemCount;
        return 0;
    }
    if (r.Field.Kind != FieldKind::DynArray && r.Field.Kind != FieldKind::Map
        && r.Field.Kind != FieldKind::Optional) {
        return 3;
    }
    if (r.Field.Count == nullptr) return 4;

    *count = r.Field.Count(r.Address);
    return 0;
}

// Element kinds a whole-array assignment can write one by one. Anything else
// would be left default-constructed, a null pointer included.
static bool element_assignable(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: case FieldKind::Int8: case FieldKind::Uint8:
        case FieldKind::Int16: case FieldKind::Uint16: case FieldKind::Int32:
        case FieldKind::Uint32: case FieldKind::Int64: case FieldKind::Uint64:
        case FieldKind::Float: case FieldKind::Double: case FieldKind::Guid:
        case FieldKind::Entity: case FieldKind::FixedString:
        case FieldKind::LSString: case FieldKind::ComponentHandle:
            return true;
        default:
            return false;
    }
}

// Resizes a DynArray field for a whole-array assignment and returns where
// its elements now are; the caller writes them one by one.
extern "C" bool bg3le_meta_array_resize(void const* handle, char const* path,
                                        void* component, std::size_t count,
                                        void** data, std::uint16_t* elemSize,
                                        std::uint8_t* elemKind) {
    if (handle == nullptr || path == nullptr || component == nullptr) return false;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr || r.Field.Kind != FieldKind::DynArray
        || r.Field.ReadOnly || r.Field.Data == nullptr
        || (count != 0 && !element_assignable(r.Field.ElemKind)
            && r.Field.ElemTypeName == nullptr)) {
        return false;
    }
    if (r.Field.Resize == nullptr) {
        if (r.Field.Count == nullptr || r.Field.Count(r.Address) != count) return false;
    } else if (!r.Field.Resize(r.Address, count)) {
        return false;
    }
    *data = r.Field.Data(r.Address);
    *elemSize = r.Field.ElemSize;
    *elemKind = (std::uint8_t)r.Field.ElemKind;
    return true;
}

// Which alternative a variant currently holds, and how many it has.
//
// Needs the component, because the active one is a runtime fact. Returns false
// if the field is not a variant; sets active to the count when the variant is
// valueless, which no index can then match.
extern "C" bool bg3le_meta_variant_index(void const* handle, char const* path,
                                         void* component, std::size_t* active,
                                         std::size_t* count) {
    *active = 0;
    *count = 0;
    if (handle == nullptr || path == nullptr || component == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;
    if (r.Field.Kind != FieldKind::Variant) return false;
    if (r.Field.Alternatives == nullptr || r.Field.ActiveIndex == nullptr) {
        return false;
    }

    while (r.Field.Alternatives[*count] != nullptr) ++*count;

    const std::size_t live = r.Field.ActiveIndex(r.Address);
    *active = (live == (std::size_t)-1) ? *count : live;
    return true;
}

// The key of one slot of a map.
//
// Keys are read by slot rather than looked up, because a lookup would mean
// hashing a key built from Lua -- for every key type, with the engine's own
// hash. Slot i holds the key belonging to the value at the same index, so
// walking the slots pairs them up, and these maps are small enough for a
// caller to walk.
extern "C" bool bg3le_meta_map_key(void const* handle, char const* path,
                                   void* component, std::size_t index,
                                   void** address, std::uint8_t* kind,
                                   std::uint16_t* size) {
    *address = nullptr;
    if (handle == nullptr || path == nullptr || component == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;
    if (r.Field.Kind != FieldKind::Map) return false;
    if (r.Field.Count == nullptr
        || (r.Field.KeyData == nullptr && r.Field.KeyAt == nullptr)) {
        return false;
    }
    if (r.Field.KeySize == 0) return false;
    if (index >= r.Field.Count(r.Address)) return false;

    if (r.Field.KeyAt != nullptr) {
        *address = r.Field.KeyAt(r.Address, index);
    } else {
        void* keys = r.Field.KeyData(r.Address);
        if (keys == nullptr) return false;
        *address = (char*)keys + index * r.Field.KeySize;
    }
    if (*address == nullptr) return false;
    *kind = (std::uint8_t)r.Field.KeyKind;
    *size = r.Field.KeySize;
    return true;
}

// A map field's key: its kind, size and, for an enum, the enum's type name.
extern "C" bool bg3le_meta_map_key_info(void const* handle, char const* path,
                                        std::uint8_t* kind, std::uint16_t* size,
                                        char const** enumName, std::uint16_t* enumLength) {
    if (handle == nullptr || path == nullptr) return false;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path, nullptr);
    if (!r.Ok || r.Field.Kind != FieldKind::Map || r.Field.KeySize == 0) return false;
    *kind = (std::uint8_t)r.Field.KeyKind;
    *size = r.Field.KeySize;
    *enumName = r.Field.KeyTypeName;
    *enumLength = r.Field.KeyTypeNameLength;
    return true;
}

// Adds (insert) or removes a key of the map at path; false where bg3le cannot.
extern "C" bool bg3le_meta_map_edit(void const* handle, char const* path, void* component,
                                    void const* key, bool insert) {
    if (handle == nullptr || path == nullptr || component == nullptr || key == nullptr) return false;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path, component);
    if (!r.Ok || r.Address == nullptr || r.Field.Kind != FieldKind::Map) return false;
    auto op = insert ? r.Field.MapInsert : r.Field.MapRemove;
    return op != nullptr && op(r.Address, key);
}

// Whether the field at path is a hash set: the one container with Assign.
extern "C" bool bg3le_meta_is_set(void const* handle, char const* path) {
    if (handle == nullptr || path == nullptr) return false;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                nullptr);
    return r.Ok && r.Field.Kind == FieldKind::DynArray && r.Field.Assign != nullptr;
}

// Whether the field at path is a glm vector or matrix.
extern "C" bool bg3le_meta_is_vector(void const* handle, char const* path) {
    if (handle == nullptr || path == nullptr) return false;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                nullptr);
    return r.Ok && r.Field.IsVector;
}

// The label of an enum-typed map key, as upstream pushes one. False for a key
// that is not an enum, a bitmask, or a value with no label.
extern "C" bool bg3le_meta_map_key_label(void const* handle, char const* path,
                                         std::uint64_t raw,
                                         char const** label) {
    *label = nullptr;
    if (handle == nullptr || path == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                nullptr);
    if (!r.Ok || r.Field.Kind != FieldKind::Map
        || r.Field.KeyTypeName == nullptr) {
        return false;
    }

    auto it = by_enum_name().find(
        std::string_view(r.Field.KeyTypeName, r.Field.KeyTypeNameLength));
    if (it == by_enum_name().end() || it->second->IsBitmask) return false;

    for (auto const* l = it->second->Labels; l->Name != nullptr; ++l) {
        if (l->Value == raw) {
            *label = l->Name;
            return true;
        }
    }
    return false;
}

// A Text field's text. *present is false for one that reads as nil -- a null
// C string, a buffer with nothing behind it.
extern "C" bool bg3le_meta_read_text(void const* handle, char const* path, void* base,
                                     char const** data, std::size_t* size, bool* present) {
    thread_local std::string held;
    *present = false;
    if (handle == nullptr || path == nullptr || base == nullptr) return false;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path, base);
    if (!r.Ok || r.Address == nullptr || r.Field.Kind != FieldKind::Text
        || r.Field.ReadText == nullptr) {
        return false;
    }
    held.clear();
    *present = r.Field.ReadText(r.Address, &held);
    *data = held.data();
    *size = held.size();
    return true;
}

// The class a pointer field points at, by the name bg3le_meta_class takes,
// so what it points at can be read as an object of its own.
extern "C" char const* bg3le_meta_path_class(void const* handle, char const* path) {
    if (handle == nullptr || path == nullptr) return nullptr;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path, nullptr);
    if (!r.Ok) return nullptr;
    auto const* cls = struct_type_of(&r.Field);
    return cls != nullptr ? cls->Name : nullptr;
}

// Whether a path names a struct bg3se describes -- which may have no fields
// of its own, so an empty listing is not by itself a failure.
extern "C" bool bg3le_meta_path_is_struct(void const* handle, char const* path) {
    if (handle == nullptr) return false;
    if (path == nullptr || path[0] == '\0') return true;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path, nullptr);
    return r.Ok && struct_type_of(&r.Field) != nullptr;
}

// The legacy names (P_RENAMED's old ones) of the struct at path, bases
// included, which the listing below leaves out. Returns the number written.
extern "C" std::size_t bg3le_meta_legacy_at(void const* handle,
                                            char const* path,
                                            char const** names,
                                            std::uint8_t* kinds,
                                            std::size_t capacity) {
    if (handle == nullptr) return 0;
    auto const* cls = static_cast<ClassFields const*>(handle);
    if (path != nullptr && path[0] != '\0') {
        const auto r = resolve_path(cls, path, nullptr);
        if (!r.Ok) return 0;
        cls = struct_type_of(&r.Field);
        if (cls == nullptr) return 0;
    }

    std::size_t n = 0;
    std::vector<ClassFields const*> pending{cls};
    while (!pending.empty() && n < capacity) {
        auto const* c = pending.back();
        pending.pop_back();
        for (auto const* f = c->Fields; f->Name != nullptr && n < capacity; ++f) {
            if (f->Kind == FieldKind::Inherit) {
                auto it = by_class_name().find(f->Name);
                if (it != by_class_name().end()) pending.push_back(it->second);
                continue;
            }
            if (!f->Legacy) continue;
            names[n] = f->Name;
            kinds[n] = reportable_kind(*f);
            ++n;
        }
    }
    return n;
}

// Enumerates a component's own fields, base classes included, for listing a
// component from Lua. Returns the number written.
// path may be null or empty for the component itself, or a dotted path to a
// nested struct, so a script can list what an inner struct offers the same way
// it lists a component.
extern "C" std::size_t bg3le_meta_fields_at(void const* handle,
                                            char const* path,
                                            char const** names,
                                            std::uint8_t* kinds,
                                            std::size_t capacity) {
    if (handle == nullptr) return 0;

    auto const* cls = static_cast<ClassFields const*>(handle);
    if (path != nullptr && path[0] != '\0') {
        const auto r = resolve_path(cls, path, nullptr);
        if (!r.Ok) return 0;
        // Either a nested struct, or an element of an array of structs; both
        // resolve to a field whose type has a table.
        cls = struct_type_of(&r.Field);
        if (cls == nullptr) return 0;
    }

    std::size_t n = 0;
    std::vector<ClassFields const*> pending{cls};

    while (!pending.empty() && n < capacity) {
        auto const* cls = pending.back();
        pending.pop_back();
        for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
            if (f->Kind == FieldKind::Inherit) {
                auto it = by_class_name().find(f->Name);
                if (it != by_class_name().end()) pending.push_back(it->second);
                continue;
            }
            if (f->Legacy) continue;
            if (n >= capacity) break;
            names[n] = f->Name;
            // A Struct whose type bg3se does not describe cannot be descended
            // into, so it is reported as unsupported rather than as a struct.
            // That keeps the reported kind something a caller can act on.
            kinds[n] = reportable_kind(*f);
            ++n;
        }
    }
    return n;
}

extern "C" std::size_t bg3le_meta_fields(void const* handle,
                                         char const** names,
                                         std::uint8_t* kinds,
                                         std::size_t capacity) {
    return bg3le_meta_fields_at(handle, nullptr, names, kinds, capacity);
}

// How many classes carry metadata, for the startup log.
// Walks a real component holding a real dynamic array, so the container
// accessors and the indexed path walk are exercised on genuine memory rather
// than inferred from the tables.
//
// Everything the static tables say can be checked from outside (see
// tools/meta-check.c), but following a dynamic array's buffer cannot: it needs
// an instance. Building one here is the only way to test that without the
// game.
//
// Pushing onto the array allocates through bg3se, so the caller has to install
// an allocator first. In the test harness that is malloc, which is safe
// precisely because this memory is never handed to the engine.
//
// Returns the number of failed checks, and writes a line per failure.
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);
extern "C" bool bg3le_meta_enum_label(void const* handle, char const* path,
                                      std::size_t index, char const** label,
                                      std::uint64_t* value, bool* isBitmask);

extern "C" int bg3le_meta_selftest() {
    int failures = 0;
    auto fail = [&failures](char const* what) {
        bg3le::logf("meta selftest: FAIL %s", what);
        ++failures;
    };

    if (!bg3le_game_allocator_ready()) {
        fail("no allocator installed; cannot build a test array");
        return failures;
    }

    ActionResourceEventsOneFrameComponent component;
    ActionResourceSetValueRequest a{};
    a.Amount = 11.5;
    a.OldAmount = 1.0;
    ActionResourceSetValueRequest b{};
    b.Amount = 22.25;
    b.OldAmount = 2.0;
    component.Events.push_back(a);
    component.Events.push_back(b);

    auto const* meta = static_cast<ClassFields const*>(
        bg3le_meta_component("eoc::ActionResourceEventsOneFrameComponent"));
    if (meta == nullptr) {
        fail("no metadata for ActionResourceEventsOneFrameComponent");
        return failures;
    }

    // The length has to come from the container, not from the table.
    std::size_t count = 0;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    if (bg3le_meta_array_length(meta, "Events", &component, &count, &elemSize,
                                &elemKind) != 0) {
        fail("Events has no array length");
    } else {
        if (count != 2) fail("Events length is not 2");
        if (elemSize != sizeof(ActionResourceSetValueRequest)) {
            fail("Events element stride does not match the element type");
        }
    }

    // Indexing the array and then descending into the element struct.
    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, "Events[1].Amount", &component, &address,
                            &kind, &size, &readOnly)) {
        fail("Events[1].Amount does not resolve");
    } else if (address != &component.Events[1].Amount) {
        fail("Events[1].Amount resolved to the wrong address");
    } else if (kind != (std::uint8_t)FieldKind::Double) {
        fail("Events[1].Amount is not reported as a double");
    } else if (*(double*)address != 22.25) {
        fail("Events[1].Amount does not read back what was written");
    }

    if (bg3le_meta_resolve(meta, "Events[0].OldAmount", &component, &address,
                           &kind, &size, &readOnly)
        && address != &component.Events[0].OldAmount) {
        fail("Events[0].OldAmount resolved to the wrong address");
    }

    // Past the end has to fail rather than run off the buffer.
    if (bg3le_meta_resolve(meta, "Events[2].Amount", &component, &address,
                           &kind, &size, &readOnly)) {
        fail("Events[2] resolved despite the array holding two elements");
    }

    // Descending into a dynamic array without indexing it is not meaningful
    // and must fail rather than read the container's own bytes as a struct.
    if (bg3le_meta_resolve(meta, "Events.Amount", &component, &address, &kind,
                           &size, &readOnly)) {
        fail("Events.Amount resolved without an index");
    }

    // A write through a resolved address has to land in the component.
    if (bg3le_meta_resolve(meta, "Events[0].Amount", &component, &address,
                           &kind, &size, &readOnly)) {
        *(double*)address = 99.5;
        if (component.Events[0].Amount != 99.5) {
            fail("a write through a resolved address did not land");
        }
    } else {
        fail("Events[0].Amount does not resolve");
    }

    // A hash set reads as an array of its keys, and has to report itself
    // read-only: its elements are the keys the table's hashes were computed
    // from, so writing one in place would desynchronise the two.
    SummonContainerComponent summons;
    summons.Characters.insert(EntityHandle((std::uint64_t)0x1234));
    summons.Characters.insert(EntityHandle((std::uint64_t)0x5678));

    auto const* summonMeta = static_cast<ClassFields const*>(
        bg3le_meta_component("eoc::summon::ContainerComponent"));
    if (summonMeta == nullptr) {
        fail("no metadata for the summon container component");
    } else {
        std::size_t setCount = 0;
        std::uint16_t setElemSize = 0;
        std::uint8_t setElemKind = 0;
        if (bg3le_meta_array_length(summonMeta, "Characters", &summons,
                                    &setCount, &setElemSize, &setElemKind)
            != 0) {
            fail("Characters has no array length");
        } else if (setCount != 2) {
            fail("Characters length is not 2");
        }

        if (!bg3le_meta_resolve(summonMeta, "Characters[0]", &summons, &address,
                                &kind, &size, &readOnly)) {
            fail("Characters[0] does not resolve");
        } else {
            if (!readOnly) fail("a hash set element is not reported read-only");
            if (address != summons.Characters.keys().data()) {
                fail("Characters[0] is not the first key");
            }
        }
    }

    // A map, which is the deepest walk: a HashMap<Guid, Array<...>> means
    // indexing the map to a value that is itself an array, indexing that, and
    // then descending into the element struct. That is the whole chain
    // "Resources[0][1].Amount" exercised on real memory, and it is also the
    // shape LenonTweaks needs.
    ActionResourcesComponent resources;
    const auto key = Guid{0x1122334455667788ull, 0x99aabbccddeeff00ull};
    Array<ActionResourceEntry> entries;
    ActionResourceEntry e0{};
    e0.Amount = 3.5;
    e0.MaxAmount = 4.0;
    ActionResourceEntry e1{};
    e1.Amount = 7.25;
    e1.MaxAmount = 8.0;
    entries.push_back(e0);
    entries.push_back(e1);
    resources.Resources.set(key, entries);

    auto const* resMeta = static_cast<ClassFields const*>(
        bg3le_meta_component("eoc::ActionResourcesComponent"));
    if (resMeta == nullptr) {
        fail("no metadata for the action resources component");
    } else {
        std::size_t mapCount = 0;
        std::uint16_t mapElemSize = 0;
        std::uint8_t mapElemKind = 0;
        if (bg3le_meta_array_length(resMeta, "Resources", &resources,
                                    &mapCount, &mapElemSize, &mapElemKind)
            != 0) {
            fail("Resources has no length");
        } else if (mapCount != 1) {
            fail("Resources length is not 1");
        }

        // The key run, read by slot.
        if (!bg3le_meta_map_key(resMeta, "Resources", &resources, 0, &address,
                                &kind, &size)) {
            fail("Resources has no key at slot 0");
        } else {
            if (kind != (std::uint8_t)FieldKind::Guid) {
                fail("the Resources key is not reported as a Guid");
            }
            if (*(Guid*)address != key) fail("the Resources key does not match");
        }

        // A key past the end has to fail rather than run off the run.
        if (bg3le_meta_map_key(resMeta, "Resources", &resources, 1, &address,
                               &kind, &size)) {
            fail("Resources returned a key at slot 1 of a one-entry map");
        }

        // Map -> value array -> element -> field, in one path.
        if (!bg3le_meta_resolve(resMeta, "Resources[0][1].Amount", &resources,
                                &address, &kind, &size, &readOnly)) {
            fail("Resources[0][1].Amount does not resolve");
        } else if (address != &resources.Resources.values()[0][1].Amount) {
            fail("Resources[0][1].Amount resolved to the wrong address");
        } else if (*(double*)address != 7.25) {
            fail("Resources[0][1].Amount does not read back what was written");
        }

        if (!bg3le_meta_resolve(resMeta, "Resources[0][0].MaxAmount",
                                &resources, &address, &kind, &size,
                                &readOnly)) {
            fail("Resources[0][0].MaxAmount does not resolve");
        } else if (*(double*)address != 4.0) {
            fail("Resources[0][0].MaxAmount does not read back");
        }

        // Indexing past the end of the inner array, and past the map.
        if (bg3le_meta_resolve(resMeta, "Resources[0][2].Amount", &resources,
                               &address, &kind, &size, &readOnly)) {
            fail("the inner array indexed past its two entries");
        }
        if (bg3le_meta_resolve(resMeta, "Resources[1][0].Amount", &resources,
                               &address, &kind, &size, &readOnly)) {
            fail("the map indexed past its one entry");
        }

        // The write the mod actually wants: top an entry up to its maximum.
        if (bg3le_meta_resolve(resMeta, "Resources[0][0].Amount", &resources,
                               &address, &kind, &size, &readOnly)) {
            *(double*)address = 4.0;
            if (resources.Resources.values()[0][0].Amount != 4.0) {
                fail("a write into a map's array element did not land");
            }
        } else {
            fail("Resources[0][0].Amount does not resolve");
        }
    }

    // Guid formatting, pinned two ways.
    //
    // Against a literal, because the byte order is not guessable: the last
    // eight bytes are pairwise swapped as well as the first three groups, and
    // getting that wrong produced a UUID that looked entirely plausible.
    //
    // And as a round trip, because UuidToHandle parses what this formats -- a
    // UUID read out of a component has to be handed straight back.
    {
        const auto known = Guid{0x1122334455667788ull, 0x99aabbccddeeff00ull};
        char text[40];
        if (!bg3le_meta_format_guid(&known, text, sizeof(text))) {
            fail("formatting a Guid did not fit its buffer");
        } else if (std::strcmp(text, "55667788-3344-1122-ff00-ddeebbcc99aa")
                   != 0) {
            bg3le::logf("meta selftest: FAIL Guid formatted as %s", text);
            ++failures;
        }

        const auto parsed = Guid::ParseGuidString(text);
        if (!parsed.has_value()) {
            fail("a formatted Guid does not parse back");
        } else if (*parsed != known) {
            fail("a Guid does not survive a format and parse round trip");
        }

        // Too small a buffer has to be refused rather than truncated, since a
        // truncated UUID would still look like one.
        char small[8];
        if (bg3le_meta_format_guid(&known, small, sizeof(small))) {
            fail("formatting a Guid into a short buffer was allowed");
        }
    }

    // An optional, empty and then full. Empty has to be distinguishable from
    // unreadable: bg3se prints null for an empty one, and reporting it as
    // unsupported conflated "there is nothing here" with "I cannot read this".
    {
        auto const* resMeta3 = static_cast<ClassFields const*>(
            bg3le_meta_component("eoc::ActionResourcesComponent"));
        std::size_t held = 0;
        std::uint16_t sz = 0;
        std::uint8_t ek = 0;

        // The entries above were default-constructed, so DiceValues is empty.
        if (bg3le_meta_array_length(resMeta3, "Resources[0][0].DiceValues",
                                    &resources, &held, &sz, &ek) != 0) {
            fail("an empty optional has no length");
        } else if (held != 0) {
            fail("an empty optional does not report as empty");
        }

        // Fill it; the same field has to report one and read back.
        std::array<ActionResourceDiceValue, 7> dice{};
        dice[0].Amount = 3.0;
        dice[0].MaxAmount = 6.0;
        resources.Resources.values()[0][0].DiceValues = dice;

        if (bg3le_meta_array_length(resMeta3, "Resources[0][0].DiceValues",
                                    &resources, &held, &sz, &ek) != 0) {
            fail("a full optional has no length");
        } else if (held != 1) {
            fail("a full optional does not report as holding one");
        }

        if (!bg3le_meta_resolve(resMeta3,
                                "Resources[0][0].DiceValues[0][0].Amount",
                                &resources, &address, &kind, &size,
                                &readOnly)) {
            fail("cannot reach through a full optional");
        } else if (*(double*)address != 3.0) {
            fail("a value read through an optional does not match");
        }

        // Past the single slot must fail, as for any container.
        if (bg3le_meta_resolve(resMeta3, "Resources[0][0].DiceValues[1]",
                               &resources, &address, &kind, &size,
                               &readOnly)) {
            fail("an optional indexed past its single slot resolved");
        }
    }

    // A variant, in both of its alternatives. The point of the checks is that
    // only the one actually held resolves: the bytes are not the others, and
    // reading them as though they were gives a number that looks like a value.
    {
        SummonLifetimeComponent lifetime;
        auto const* lifeMeta = static_cast<ClassFields const*>(
            bg3le_meta_component("eoc::summon::LifetimeComponent"));
        std::size_t activeAlt = 0;
        std::size_t altCount = 0;

        lifetime.Lifetime = (std::uint8_t)7;
        if (!bg3le_meta_variant_index(lifeMeta, "Lifetime", &lifetime,
                                      &activeAlt, &altCount)) {
            fail("Lifetime is not reported as a variant");
        } else {
            if (altCount != 2) fail("Lifetime does not have two alternatives");
            if (activeAlt != 0) fail("the uint8 alternative is not active");
        }

        if (!bg3le_meta_resolve(lifeMeta, "Lifetime[0]", &lifetime, &address,
                                &kind, &size, &readOnly)) {
            fail("the live alternative does not resolve");
        } else if (*(std::uint8_t*)address != 7) {
            fail("the live alternative does not read back");
        }

        // The alternative that is not held has to refuse.
        if (bg3le_meta_resolve(lifeMeta, "Lifetime[1]", &lifetime, &address,
                               &kind, &size, &readOnly)) {
            fail("an alternative that is not held resolved");
        }

        // Switch it and the answers swap over.
        lifetime.Lifetime = 2.5f;
        if (!bg3le_meta_variant_index(lifeMeta, "Lifetime", &lifetime,
                                      &activeAlt, &altCount)) {
            fail("Lifetime stopped being a variant");
        } else if (activeAlt != 1) {
            fail("the float alternative is not active after assignment");
        }
        if (!bg3le_meta_resolve(lifeMeta, "Lifetime[1]", &lifetime, &address,
                                &kind, &size, &readOnly)) {
            fail("the float alternative does not resolve");
        } else if (*(float*)address != 2.5f) {
            fail("the float alternative does not read back");
        }
        if (bg3le_meta_resolve(lifeMeta, "Lifetime[0]", &lifetime, &address,
                               &kind, &size, &readOnly)) {
            fail("the uint8 alternative resolved while the float was held");
        }
        if (bg3le_meta_resolve(lifeMeta, "Lifetime[2]", &lifetime, &address,
                               &kind, &size, &readOnly)) {
            fail("an alternative past the end resolved");
        }
    }

    // Enum labels, checked against what bg3se prints on Windows for the same
    // save: ReplenishType came back as 2 and 8 here where bg3se showed
    // ["Default"] and ["Rest"]. It is a bitmask, so the labels are flags.
    {
        auto const* resMeta2 = static_cast<ClassFields const*>(
            bg3le_meta_component("eoc::ActionResourcesComponent"));
        char const* label = nullptr;
        std::uint64_t value = 0;
        bool isBitmask = false;

        bool foundDefault = false;
        bool foundRest = false;
        for (std::size_t i = 0;
             bg3le_meta_enum_label(resMeta2, "Resources[0][0].ReplenishType", i,
                                   &label, &value, &isBitmask);
             ++i) {
            if (std::strcmp(label, "Default") == 0 && value == 0x02) {
                foundDefault = true;
            }
            if (std::strcmp(label, "Rest") == 0 && value == 0x08) {
                foundRest = true;
            }
        }
        if (!isBitmask) fail("ReplenishType is not reported as a bitmask");
        if (!foundDefault) fail("ReplenishType has no Default = 2");
        if (!foundRest) fail("ReplenishType has no Rest = 8");

        // A field that is not an enum must report none, rather than the
        // labels of whatever happens to share its integer kind.
        if (bg3le_meta_enum_label(meta, "Events[0].Amount", 0, &label, &value,
                                  &isBitmask)) {
            fail("a non-enum field reported enum labels");
        }
    }

    if (failures == 0) {
        bg3le::logf("meta selftest: the container walks behave");
    }
    return failures;
}

extern "C" std::size_t bg3le_meta_class_count() { return std::size(kAllClasses); }

// Formats a Guid the way the engine spells it.
//
// Not open-coded on the bg3le side, because the byte order is not the obvious
// one: the first three groups are little-endian words, as a Microsoft GUID is,
// but so are the last eight bytes, pairwise. Hand-rolling it produced
// 8411-0cc7dfacdcfc where the engine writes 1184-c70cacdffcdc -- a UUID that
// looked entirely plausible and was wrong. Going through bg3se's own ToString
// also keeps this the exact inverse of the ParseGuidString that UuidToHandle
// relies on, so a UUID read out of a component can be handed straight back.
//
// Writes at most capacity bytes including the terminator, and returns false if
// it would not fit.
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity) {
    if (bytes == nullptr || out == nullptr || capacity == 0) return false;

    const auto text = static_cast<Guid const*>(bytes)->ToString();
    if (text.size() + 1 > capacity) return false;

    std::memcpy(out, text.c_str(), text.size() + 1);
    return true;
}

// The i'th class, for sweeping the whole set -- listing the components a
// script can reach, or measuring how much of them converts.
extern "C" void const* bg3le_meta_class_at(std::size_t index) {
    if (index >= std::size(kAllClasses)) return nullptr;
    return kAllClasses[index];
}

// The bg3se name of a reflected class.
//
// A component is reachable by three names -- its engine class, bg3se's short
// name, and the class name from the generated metadata -- and only the last is
// what the type registry is keyed by. Ext.Types.GetAllTypes lists these, and
// Ext.Types.GetObjectType has to return one of them or GetTypeInfo on its
// answer finds nothing.
extern "C" char const* bg3le_meta_class_name(void const* handle) {
    if (handle == nullptr) return nullptr;
    return static_cast<ClassFields const*>(handle)->Name;
}

extern "C" std::size_t bg3le_meta_component_count() {
    std::size_t n = 0;
    for (auto const* cls : kAllClasses) {
        if (cls->IsComponent && cls->EngineClass != nullptr) ++n;
    }
    return n;
}

// The labels of an enum-typed field.
//
// Reported separately from the field's kind, which stays the underlying
// integer, so an enum still reads and writes as a number for anything that
// wants one. index walks the labels; a bitmask is told apart because bg3se
// renders one as the list of set flags and matching that keeps scripts
// written against it working.
//
// Returns false once index runs past the end, or immediately if the field is
// not an enum.
namespace {

// A P_BITMASK field of cls or its bases with a label called name.
bool find_bitflag(ClassFields const* cls, char const* name,
                  FieldDesc const** field, std::uint64_t* mask,
                  unsigned depth = 0) {
    if (cls == nullptr || depth > 8) return false;
    for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
        if (f->Kind == FieldKind::Inherit) {
            auto it = by_class_name().find(f->Name);
            if (it != by_class_name().end()
                && find_bitflag(it->second, name, field, mask, depth + 1)) {
                return true;
            }
            continue;
        }
        if (!f->BitmaskFlags || f->TypeName == nullptr) continue;
        auto it = by_enum_name().find(
            std::string_view(f->TypeName, f->TypeNameLength));
        if (it == by_enum_name().end()) continue;
        for (auto const* l = it->second->Labels; l->Name != nullptr; ++l) {
            if (std::strcmp(l->Name, name) == 0) {
                *field = f;
                *mask = l->Value;
                return true;
            }
        }
    }
    return false;
}

}  // namespace

// P_BITMASK's flag properties: name is a label of a flags field's enum, and
// the property is that bit. Address and size are the flags field's.
extern "C" bool bg3le_meta_bitflag(void const* handle, char const* name,
                                   void* object, void** address,
                                   std::uint16_t* size, std::uint64_t* mask) {
    if (handle == nullptr || name == nullptr || object == nullptr) return false;
    FieldDesc const* field = nullptr;
    if (!find_bitflag(static_cast<ClassFields const*>(handle), name, &field,
                      mask)) {
        return false;
    }
    *address = static_cast<char*>(object) + field->Offset;
    *size = field->Size;
    return true;
}

// The descriptor naming a field's enum: the field's own, or an optional's payload.
static FieldDesc const* enum_desc(FieldDesc const& field) {
    if (field.TypeName != nullptr) return &field;
    if (field.Kind == FieldKind::Optional && field.ElemDesc != nullptr
        && field.ElemDesc->TypeName != nullptr) {
        return field.ElemDesc;
    }
    return nullptr;
}

extern "C" bool bg3le_meta_enum_label(void const* handle, char const* path,
                                      std::size_t index, char const** label,
                                      std::uint64_t* value, bool* isBitmask) {
    *label = nullptr;
    if (handle == nullptr || path == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                nullptr);
    FieldDesc const* desc = r.Ok ? enum_desc(r.Field) : nullptr;
    if (desc == nullptr) return false;

    auto it = by_enum_name().find(
        std::string_view(desc->TypeName, desc->TypeNameLength));
    if (it == by_enum_name().end()) return false;

    *isBitmask = it->second->IsBitmask;
    auto const* labels = it->second->Labels;
    for (std::size_t i = 0; i < index; ++i) {
        if (labels[i].Name == nullptr) return false;
    }
    if (labels[index].Name == nullptr) return false;

    *label = labels[index].Name;
    *value = labels[index].Value;
    return true;
}

// The name of a field kind.
//
// The single source of truth for these, because they were duplicated in
// lua_host.cpp and inserting a kind into the middle of the enum silently
// renumbered everything after it -- which broke the checks that asserted on
// numbers, and would have mislabelled every kind after the insertion had the
// two lists ever disagreed.
extern "C" char const* bg3le_meta_kind_name(std::uint8_t kind) {
    switch ((FieldKind)kind) {
        case FieldKind::Bool: return "boolean";
        case FieldKind::Float: return "float";
        case FieldKind::Double: return "double";
        case FieldKind::Int8: return "int8";
        case FieldKind::Uint8: return "uint8";
        case FieldKind::Int16: return "int16";
        case FieldKind::Uint16: return "uint16";
        case FieldKind::Int32: return "int32";
        case FieldKind::Uint32: return "uint32";
        case FieldKind::Int64: return "int64";
        case FieldKind::Uint64: return "uint64";
        case FieldKind::Guid: return "guid";
        case FieldKind::Entity: return "entity";
        case FieldKind::FixedString: return "string";
        case FieldKind::LSString: return "string";
        case FieldKind::ScalarArray: return "array";
        case FieldKind::Struct: return "struct";
        case FieldKind::DynArray: return "array";
        case FieldKind::Map: return "map";
        case FieldKind::Optional: return "optional";
        case FieldKind::Variant: return "variant";
        case FieldKind::Inherit: return "inherit";
        case FieldKind::ComponentHandle: return "handle";
        case FieldKind::ConditionId: return "condition";
        case FieldKind::Pointer: return "pointer";
        case FieldKind::Text: return "string";
        case FieldKind::Version: return "version";
        case FieldKind::EntityOrVec3: return "entityorvec3";
        case FieldKind::BitArray: return "bitarray";
        default: return "unsupported";
    }
}

// How many enums carry labels, for the startup log.
// The enum registry, for Ext.Enums: every enum bg3se describes, by index,
// and each one's labels and values.
//
// bg3le already decodes an enum-typed *field* through
// bg3le_meta_enum_label, which asks by the path of the field that has the
// type. Ext.Enums asks by the type itself, and nothing reached the registry
// that way.
// The Lua name of the enum a field holds (DiceSizeId), or null.
extern "C" char const* bg3le_meta_enum_lua_name(void const* handle, char const* path) {
    if (handle == nullptr || path == nullptr) return nullptr;
    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path, nullptr);
    FieldDesc const* desc = r.Ok ? enum_desc(r.Field) : nullptr;
    if (desc == nullptr) return nullptr;
    auto it = by_enum_name().find(std::string_view(desc->TypeName, desc->TypeNameLength));
    if (it == by_enum_name().end()) return nullptr;
    return it->second->LuaName != nullptr ? it->second->LuaName : it->second->Name;
}

extern "C" char const* bg3le_meta_enum_at(std::size_t index,
                                          bool* isBitmask) {
    if (index >= std::size(kAllEnums)) return nullptr;
    auto const* e = kAllEnums[index];
    if (isBitmask != nullptr) *isBitmask = e->IsBitmask;
    // The Lua name, which is what a script writes: ClientGameState rather
    // than ecl::GameState.
    return e->LuaName != nullptr ? e->LuaName : e->Name;
}

// One label of an enum named by its Lua name, by index. False past the end,
// which is how a caller knows to stop.
extern "C" bool bg3le_meta_enum_value_at(char const* enumName,
                                         std::size_t index,
                                         char const** label,
                                         std::uint64_t* value) {
    if (enumName == nullptr || label == nullptr || value == nullptr) {
        return false;
    }

    // Indexed by the C++ type name, and asked for by the Lua name; they
    // differ only for the namespaced ones, so both are tried.
    EnumDesc const* found = nullptr;
    auto it = by_enum_name().find(enumName);
    if (it != by_enum_name().end()) {
        found = it->second;
    } else {
        for (auto const* e : kAllEnums) {
            if (e->LuaName != nullptr
                && std::strcmp(e->LuaName, enumName) == 0) {
                found = e;
                break;
            }
            if (e->Name != nullptr && std::strcmp(e->Name, enumName) == 0) {
                found = e;
                break;
            }
        }
    }
    if (found == nullptr || found->Labels == nullptr) return false;

    std::size_t at = 0;
    for (auto const* l = found->Labels; l->Name != nullptr; ++l, ++at) {
        if (at != index) continue;
        *label = l->Name;
        *value = l->Value;
        return true;
    }
    return false;
}

// The value of one label, by name. What Ext.IMGUI needs to accept an enum
// argument the way upstream does: as a name, not only as a number.
extern "C" bool bg3le_meta_enum_label_value(char const* enumName,
                                           char const* label,
                                           std::uint64_t* value) {
    if (enumName == nullptr || label == nullptr || value == nullptr) {
        return false;
    }

    std::size_t at = 0;
    char const* name = nullptr;
    std::uint64_t found = 0;
    while (bg3le_meta_enum_value_at(enumName, at++, &name, &found)) {
        if (name != nullptr && std::strcmp(name, label) == 0) {
            *value = found;
            return true;
        }
    }
    return false;
}

extern "C" std::size_t bg3le_meta_enum_count() { return std::size(kAllEnums); }

// Parses a GUID the way the engine spells it, which is the inverse of
// bg3le_meta_format_guid -- both go through bg3se so they stay inverses.
extern "C" bool bg3le_meta_parse_guid(char const* text, void* out) {
    if (text == nullptr || out == nullptr) return false;
    const auto parsed = Guid::ParseGuidString(text);
    if (!parsed.has_value()) return false;
    std::memcpy(out, &*parsed, sizeof(Guid));
    return true;
}

// A class by its C++ name, for the things bg3se describes that are not
// components -- a static data resource, for instance. The same handle works
// with every field call, since those only ever needed a class and a base
// address; it was reaching the address that was entity-specific.
extern "C" void const* bg3le_meta_class(char const* className) {
    if (className == nullptr) return nullptr;
    auto it = by_class_name().find(className);
    if (it != by_class_name().end()) return it->second;

    // A script names a resource the way bg3se's Lua API does, by the
    // ExtResourceManagerType label: "ActionResource", not
    // "resource::ActionResource".
    it = by_resource_label().find(className);
    return it != by_resource_label().end() ? it->second : nullptr;
}

// The engine's name for a component, so a caller who looked the component up
// by bg3se's short name can still reach bg3le's symbol-table index, which is
// keyed by the engine name.
extern "C" char const* bg3le_meta_engine_class(void const* handle) {
    if (handle == nullptr) return nullptr;
    return static_cast<ClassFields const*>(handle)->EngineClass;
}

// bg3se's short name, which is what a script writes as entity.<Name>.
extern "C" char const* bg3le_meta_short_name(void const* handle) {
    if (handle == nullptr) return nullptr;
    return static_cast<ClassFields const*>(handle)->ComponentName;
}

}  // namespace bg3le
