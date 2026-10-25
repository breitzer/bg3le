#include <algorithm>
#include <atomic>
#include <array>
#include <map>
#include "osi.h"

#include <cctype>
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstring>
#include <link.h>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "debug_server.h"
#include "hook.h"
#include "log.h"
#include "vendor/cache_lock.h"
#include "mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le::osi {
namespace {

using Thunk6 = long (*)(long, long, long, long, long, long);

Thunk6 g_call = nullptr;
Thunk6 g_query = nullptr;

// Descriptors are 64 bytes in the engine's pool; 128 leaves slack. Max
// observed arity is 11.
constexpr std::size_t kNodeSize = 128;
constexpr std::size_t kMaxParams = 16;

template <typename T>
T sym(const char* name) {
    return reinterpret_cast<T>(::dlsym(RTLD_NEXT, name));
}

struct Accessors {
    void (*set_type)(void*, unsigned short) = nullptr;
    void (*set_integer)(void*, int) = nullptr;
    void (*set_integer64)(void*, long) = nullptr;
    void (*set_float)(void*, float) = nullptr;
    void (*set_string)(void*, const char*) = nullptr;
    int (*get_integer)(const void*) = nullptr;
    long (*get_integer64)(const void*) = nullptr;
    float (*get_float)(const void*) = nullptr;
    const char* (*get_string)(const void*) = nullptr;

    bool ok() const {
        return set_type && set_integer && set_integer64 && set_float &&
               set_string && get_integer && get_integer64 && get_float &&
               get_string;
    }
};

const Accessors& accessors() {
    static Accessors a = [] {
        Accessors r;
        r.set_type = sym<decltype(r.set_type)>(
            "_ZN16COsiArgumentDesc7SetTypeE13TOsiValueType");
        r.set_integer = sym<decltype(r.set_integer)>(
            "_ZN16COsiArgumentDesc10SetIntegerEi");
        r.set_integer64 = sym<decltype(r.set_integer64)>(
            "_ZN16COsiArgumentDesc12SetInteger64El");
        r.set_float = sym<decltype(r.set_float)>("_ZN16COsiArgumentDesc8SetFloatEf");
        r.set_string = sym<decltype(r.set_string)>(
            "_ZN16COsiArgumentDesc12SetAnyStringEPKc");
        r.get_integer = sym<decltype(r.get_integer)>(
            "_ZNK16COsiArgumentDesc10GetIntegerEv");
        r.get_integer64 = sym<decltype(r.get_integer64)>(
            "_ZNK16COsiArgumentDesc12GetInteger64Ev");
        r.get_float = sym<decltype(r.get_float)>("_ZNK16COsiArgumentDesc8GetFloatEv");
        r.get_string = sym<decltype(r.get_string)>(
            "_ZNK16COsiArgumentDesc12GetAnyStringEv");
        return r;
    }();
    return a;
}

// Types 6+ are story enums; they are carried as strings or integers depending
// on what the caller supplied.
unsigned short wire_type(std::uint8_t declared, const Value& v) {
    if (declared >= 6) return v.type == kString ? kGuidString : kInteger;
    return declared;
}

void write(void* node, unsigned short type, const Value& v) {
    const Accessors& a = accessors();
    a.set_type(node, type);  // must precede any setter; it asserts on type
    switch (type) {
        case kInteger: a.set_integer(node, static_cast<int>(v.integer)); break;
        case kInteger64: a.set_integer64(node, static_cast<long>(v.integer)); break;
        case kReal: a.set_float(node, static_cast<float>(v.real)); break;
        case kString:
        case kGuidString: a.set_string(node, v.text.c_str()); break;
        default: break;
    }
}

Value read(const void* node, unsigned short type) {
    const Accessors& a = accessors();
    Value v;
    v.type = type;
    switch (type) {
        case kInteger: v.integer = a.get_integer(node); break;
        case kInteger64: v.integer = a.get_integer64(node); break;
        case kReal: v.real = a.get_float(node); break;
        case kString:
        case kGuidString: {
            // A null string, such as an output not yet set, is nil to Lua.
            const char* s = a.get_string(node);
            if (s != nullptr) v.text = s; else v.type = kNone;
            break;
        }
        default:
            // Story enums come back as whichever representation they use.
            v.integer = a.get_integer(node);
            break;
    }
    return v;
}

}  // namespace

namespace {

// Osiris keeps its function database in a hash of red-black trees reachable
// from a static in libOsiris. The layouts below are bg3se's, which were
// reversed against the Windows build -- the walk validates itself against the
// names we already enumerated rather than trusting them.
//
//   [libOsiris + function_db_holder()] -> holder
//   holder + 0x10                   -> TypeDb::HashSlot[1023], stride 0x18
//   HashSlot + 0x00                 -> item count (NOT a pointer)
//   HashSlot + 0x08                 -> TMap root node
//   TMapNode: Left +00, Parent +08, Right +10, Color +18, IsRoot +19,
//             Key(OsiString) +20, Value +38
//   OsiFunctionDef + 0x18           -> FunctionSignature*
//   FunctionSignature + 0x08        -> const char* Name
//   FunctionSignature + 0x18/+0x20  -> out-param bitmask / byte count
// Recorded for v4.76; osiris_globals() reads the live ones off COsiris::COsiris().
constexpr std::uintptr_t kFunctionDbHolder = 0x119d50;
constexpr std::uintptr_t kNodeDbHolder = 0x119eb8;
constexpr std::uintptr_t kDatabaseDbHolder = 0x119ec0;
constexpr std::uintptr_t kAdapterDbHolder = 0x119ec8;

// Inside OsiFunctionDef (bg3se's GameDefinitions/Osiris.h): VMT, three
// uint32s, the signature pointer at +0x18, a NodeRef, then FunctionType,
// Key[4] and OsiFunctionId. The NodeRef's width decides where Type and
// Key land, so both candidates are tried and the one whose key
// reproduces the engine's own function ids wins -- +0x24 does, for all
// 324 functions both sources list, which puts the node id at +0x20.
constexpr std::uintptr_t kTypeCandidates[] = {0x24, 0x28};

// OsiFunctionDef's NodeRef, immediately before FunctionType.
constexpr std::uintptr_t kNodeRef = 0x20;

// Inside a Node: its own id and the function it belongs to. A data node
// also holds the id of the database it stands for, at +0x18, which is
// what reading facts through a function will use.
constexpr std::uintptr_t kNodeId = 0x08;
constexpr std::uintptr_t kNodeFunction = 0x10;

// Inside a Database: the facts list header and its count.
constexpr std::uintptr_t kFactsHead = 0x10;
constexpr std::uintptr_t kFactsCount = 0x20;

// bg3se's OsirisFunctionHandle: the handle the dispatch handlers take is
// built from the key, and how depends on the function's type.
std::uint32_t function_handle(std::uint32_t type, std::uint32_t part2,
                              std::uint32_t functionId,
                              std::uint32_t part4) {
    std::uint32_t handle = (type & 7) | (part4 << 31);
    if (type < 4) {
        handle |= (functionId & 0x1ffffff) << 3;
    } else {
        handle |= ((functionId & 0x1ffff) << 3) | ((part2 & 0xff) << 20);
    }
    return handle;
}

constexpr std::size_t kBuckets = 1023;
constexpr std::size_t kSlotStride = 0x18;

int find_osiris(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name == nullptr) return 0;
    if (std::strstr(info->dlpi_name, "libOsiris.so") == nullptr) return 0;
    *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
    return 1;
}

// These offsets are unverified until the walk validates itself, so every
// dereference goes through the fault-tolerant reader: a wrong offset yields
// a failed read instead of taking the game down.
template <typename T>
bool peek(std::uintptr_t addr, T* out) {
    if (addr < 0x1000) return false;
    return safe_read(reinterpret_cast<const void*>(addr), out, sizeof(T));
}

// libOsiris' own definition: bg3le interposes some COsiris functions, and
// the executable's PLT can stand in for others under RTLD_DEFAULT.
std::uintptr_t osiris_symbol(const char* name) {
    void* lib = ::dlopen("libOsiris.so", RTLD_NOW | RTLD_NOLOAD);
    if (lib == nullptr) return 0;
    const auto at = reinterpret_cast<std::uintptr_t>(::dlsym(lib, name));
    ::dlclose(lib);
    return at;
}

// The globals the exported COsiris::COsiris() stores: the function db is its
// first store after the hash init loop (bound kBuckets * kSlotStride),
// the node, database and adapter lists are its only run of three adjacent
// stores. Recorded offsets stand in if the code differs; every use validates.
struct OsirisGlobals {
    std::uintptr_t FunctionDb = kFunctionDbHolder;
    std::uintptr_t NodeDb = kNodeDbHolder;
    std::uintptr_t DatabaseDb = kDatabaseDbHolder;
    std::uintptr_t AdapterDb = kAdapterDbHolder;
};

OsirisGlobals const& osiris_globals() {
    static const OsirisGlobals g = [] {
        OsirisGlobals out;
        std::uintptr_t base = 0;
        ::dl_iterate_phdr(find_osiris, &base);
        const auto ctor = osiris_symbol("_ZN7COsirisC1Ev");
        unsigned char code[0x800] = {};
        const std::size_t n = base == 0 || ctor == 0 ? 0 : safe_read_some((void const*)ctor, code, sizeof(code));
        const std::uint32_t bound = kBuckets * kSlotStride;
        std::size_t loop = 0;
        std::vector<std::pair<std::size_t, std::uintptr_t>> stores;  // mov [rip+disp32], r64
        for (std::size_t i = 0; i + 7 <= n; ++i) {
            std::int32_t disp = 0;
            std::memcpy(&disp, code + i + 3, 4);
            if ((code[i] == 0x48 || code[i] == 0x4c) && code[i + 1] == 0x89 && (code[i + 2] & 0xc7) == 0x05)
                stores.emplace_back(i, ctor + i + 7 + disp - base);
            if (loop == 0 && code[i] == 0x81 && (code[i + 1] & 0xf8) == 0xf8
                && std::memcmp(code + i + 2, &bound, 4) == 0)
                loop = i;
        }
        std::uintptr_t fdb = 0, nodes = 0, databases = 0, adapters = 0;
        int runs = 0;
        for (std::size_t k = 0; k < stores.size(); ++k) {
            if (fdb == 0 && loop != 0 && stores[k].first > loop) fdb = stores[k].second;
            if (k >= 2 && stores[k].second == stores[k - 1].second + 8
                && stores[k - 1].second == stores[k - 2].second + 8) {
                nodes = stores[k - 2].second;
                databases = stores[k - 1].second;
                adapters = stores[k].second;
                ++runs;
            }
        }
        if (fdb != 0 && runs == 1) {
            out = OsirisGlobals{fdb, nodes, databases, adapters};
            if (fdb != kFunctionDbHolder || nodes != kNodeDbHolder || databases != kDatabaseDbHolder
                || adapters != kAdapterDbHolder)
                logf("osiris: globals moved: functions +%#lx, nodes +%#lx, databases +%#lx, adapters +%#lx",
                     (unsigned long)fdb, (unsigned long)nodes, (unsigned long)databases,
                     (unsigned long)adapters);
        } else {
            statusf("WARNING: osiris: COsiris::COsiris() no longer shows its globals (ctor +%#lx, %zu bytes, "
                    "loop %#zx, %zu stores, %d runs); using v4.76 offsets",
                    (unsigned long)(ctor - base), n, loop, stores.size(), runs);
        }
        return out;
    }();
    return g;
}

std::uintptr_t function_db_holder() { return osiris_globals().FunctionDb; }

// The global the ctor stored first, then the window around the function db.
std::vector<std::uintptr_t> list_slots(std::uintptr_t direct, std::intptr_t window) {
    std::vector<std::uintptr_t> slots{direct};
    for (std::intptr_t delta = -window; delta <= window; delta += 8)
        if (function_db_holder() + delta != direct) slots.push_back(function_db_holder() + delta);
    return slots;
}

// One object, read in a single call and then parsed locally.
//
// Every field of these structures used to cost a process_vm_readv, and
// the walk touches four thousand of them: the signature walk was 1.75s
// of the level load, nearly all of it syscall overhead. A block read per
// object keeps the fault tolerance -- a wrong offset still yields a
// failed read rather than a segfault -- at a tenth of the calls.
template <std::size_t N>
struct Block {
    std::uintptr_t Base = 0;
    unsigned char Bytes[N] = {};
    bool Ok = false;

    explicit Block(std::uintptr_t base) : Base(base) {
        if (base < 0x1000) return;
        Ok = safe_read(reinterpret_cast<void const*>(base), Bytes, N);
    }

    template <typename T>
    T at(std::size_t off) const {
        T value{};
        if (off + sizeof(T) <= N) std::memcpy(&value, Bytes + off, sizeof(T));
        return value;
    }
};

// How many parameters are outputs, and which (bit i for parameter i). The
// signature's bitmask is MSB-first within each byte, as bg3se's isOutParam does.
int read_out_params(std::uintptr_t bits, std::uint32_t bytes, std::uint64_t* which) {
    *which = 0;
    if (bits == 0 || bytes == 0 || bytes > 64) return 0;

    unsigned char mask[64] = {};
    if (!safe_read(reinterpret_cast<void const*>(bits), mask, bytes)) {
        return -1;
    }

    int total = 0;
    for (std::uint32_t i = 0; i < bytes * 8; ++i) {
        if ((mask[i / 8] & (0x80 >> (i % 8))) == 0) continue;
        ++total;
        if (i < 64) *which |= std::uint64_t{1} << i;
    }
    return total;
}

// Type and handle for one Function object, with `at` the offset of the
// FunctionType field.
bool read_type_and_handle(std::uintptr_t def, std::uintptr_t at,
                          std::uint32_t* type, std::uint32_t* handle) {
    const Block<0x40> block(def);
    if (!block.Ok) return false;

    const auto kind = block.at<std::uint32_t>(at);
    if (kind == 0 || kind > 8) return false;

    std::uint32_t key[4] = {};
    for (int i = 0; i < 4; ++i) {
        key[i] = block.at<std::uint32_t>(at + 4 + i * 4);
    }
    *type = kind;
    *handle = function_handle(key[0], key[1], key[2], key[3]);
    return true;
}

// The declared parameter types, from the signature's parameter list.
//
// The list is circular and doubly linked: the header holds head, tail and
// a count, each node holds Next, Prev and the descriptor, and the type is
// the first sixteen bits of the descriptor. The nodes run in reverse
// declaration order, which is why the result is flipped.
std::vector<std::uint8_t> read_param_types(std::uintptr_t list) {
    std::vector<std::uint8_t> types;
    if (list < 0x1000) return types;

    const Block<0x20> header(list);
    if (!header.Ok) return types;
    const auto count = header.at<std::uint64_t>(0x18);
    if (count == 0 || count > 32) return types;

    std::uintptr_t node = header.at<std::uintptr_t>(0x08);
    for (std::uint64_t i = 0; i < count && node >= 0x1000; ++i) {
        const Block<0x18> entry(node);
        if (!entry.Ok) return {};
        types.push_back((std::uint8_t)entry.at<std::uint16_t>(0x10));
        node = entry.at<std::uintptr_t>(0x00);
    }
    if (types.size() != count) return {};

    std::reverse(types.begin(), types.end());
    return types;
}

// libc++ std::string from the twenty-four bytes of it already read. Only
// the long form needs another read, and most keys are short.
bool parse_osi_string(unsigned char const* bytes, std::string* out) {
    const std::uint8_t last = bytes[23];
    if ((last & 0x80) == 0) {
        const std::size_t len = last;
        if (len > 22) return false;
        out->assign(reinterpret_cast<char const*>(bytes), len);
        return true;
    }

    std::uintptr_t data = 0;
    std::uint64_t size = 0;
    std::memcpy(&data, bytes + 0x00, sizeof(data));
    std::memcpy(&size, bytes + 0x08, sizeof(size));
    if (data < 0x1000 || size == 0 || size > 512) return false;

    std::vector<char> buf(size + 1, 0);
    if (!safe_read(reinterpret_cast<void const*>(data), buf.data(), size)) {
        return false;
    }
    out->assign(buf.data(), size);
    return true;
}


// What the walk keeps about one function in Osiris' own database.
struct DbEntry {
    int Outs = -1;
    std::uint64_t OutMask = 0;  // which parameters, as Function::out_mask
    std::uintptr_t Def = 0;  // the Function object behind the tree node
    std::vector<std::uint8_t> Types;
};

std::size_t g_visited = 0;

// How many entries the database held that carried no dispatch handle.
std::size_t g_story_functions = 0;

// Every function in one of Osiris' hash buckets.
//
// bg3se's TMapNode is { Left, Root, Right, Color, IsRoot, KeyValuePair },
// the key being a 24-byte string at +0x20 and the OsiFunctionDef* the
// value at +0x38. That much this build agrees with, and a bucket slot is
// { count, node, node } -- the count is what made the rest of this
// measurable: bucket 0 holds 19 entries, and the holder's own total at
// +0x5fe8 is 20,414.
//
// The node a bucket points at is not the root of its tree, so following
// Left and Right from there reaches a fraction of it: 3,748 entries in
// total, of which only 323 were among the 1,303 the engine's own mapping
// names. Two thirds of Osiris was invisible, and nothing said so --
// every entry that was found was correct.
//
// The middle link is the parent, so following all three reaches
// everything. Done indiscriminately that crawls the heap: eleven million
// addresses and ninety-two seconds of a level load. What bounds it is
// the key. A real node carries "Name/Arity" at +0x20, so a node is only
// expanded if it has one, and a stray pointer into the heap stops there
// instead of spreading.
bool node_key(Block<0x40> const& entry, std::string* key) {
    if (!parse_osi_string(entry.Bytes + 0x20, key) || key->empty()) {
        return false;
    }

    // "Name/Arity", and nothing else: the arity is digits, and the name
    // is the kind of identifier a story compiler emits.
    const std::size_t slash = key->rfind('/');
    if (slash == std::string::npos || slash == 0
        || slash + 1 >= key->size()) {
        return false;
    }
    for (std::size_t i = slash + 1; i < key->size(); ++i) {
        if (std::isdigit((unsigned char)(*key)[i]) == 0) return false;
    }
    for (std::size_t i = 0; i < slash; ++i) {
        const unsigned char c = (unsigned char)(*key)[i];
        if (std::isalnum(c) == 0 && c != '_') return false;
    }
    return true;
}

void visit_tree(std::uintptr_t bucket,
                std::unordered_map<std::string, DbEntry>* out,
                int /*depth*/, std::unordered_set<std::uintptr_t>* seen) {
    if (bucket < 0x1000) return;

    std::vector<std::uintptr_t> pending{bucket};
    while (!pending.empty()) {
        const std::uintptr_t node = pending.back();
        pending.pop_back();

        if (node < 0x1000) continue;
        if (!seen->insert(node).second) continue;
        ++g_visited;

        // One read for the whole tree node: the links, the key and the
        // pointer to the function.
        const Block<0x40> entry(node);
        if (!entry.Ok) continue;

        // No IsRoot check: bg3se's layout puts that flag at +0x19, and on
        // this build those padding bytes carry whatever the allocator left
        // there -- one node reads 'T'. Pruning on it discarded live
        // subtrees.
        std::string key;
        if (!node_key(entry, &key)) continue;

        const auto def = entry.at<std::uintptr_t>(0x38);
        if (def >= 0x1000) {
            const Block<0x28> function(def);
            const auto signature = function.Ok
                                       ? function.at<std::uintptr_t>(0x18)
                                       : 0;
            if (signature >= 0x1000) {
                const Block<0x28> sig(signature);
                if (sig.Ok) {
                    std::uint64_t which = 0;
                    const int outs = read_out_params(
                        sig.at<std::uintptr_t>(0x18),
                        sig.at<std::uint32_t>(0x20), &which);
                    if (outs >= 0) {
                        (*out)[key] = DbEntry{
                            outs, which, def,
                            read_param_types(sig.at<std::uintptr_t>(0x10))};
                    }
                }
            }
        }

        // Only a node that proved itself expands.
        pending.push_back(entry.at<std::uintptr_t>(0x00));  // Left
        pending.push_back(entry.at<std::uintptr_t>(0x08));  // Root/parent
        pending.push_back(entry.at<std::uintptr_t>(0x10));  // Right
    }
}

}  // namespace

// The last walk's results, so the story functions can be taken from it
// without walking the tree a second time.
std::unordered_map<std::string, DbEntry>& database() {
    static std::unordered_map<std::string, DbEntry> entries;
    return entries;
}

// Whether database() holds this story's function objects. Cleared whenever
// it is replaced: a second story load brought back unbound entries while
// this stayed set, and every story procedure read as missing.
bool g_defs_bound = false;
// The story key the signature cache was loaded under, to rewrite a stale one.
std::string g_cache_story;

// Osiris' node list, which is what a story-defined function is actually
// reached through: a procedure is run by inserting a tuple into its node,
// not by calling the engine's dispatch.
//
// OsirisStaticGlobals is a run of pointer slots in libOsiris' data, and
// the function database is one of them -- the one this file already
// uses. NodeDb is five slots further on, but rather than trust that, the
// window around it is searched for something shaped like
// TypedDb<Node> = { uint32 Size; std::vector<Node*> }: the vector's
// extent has to match the size, and the elements have to be objects with
// a vtable inside libOsiris.
struct NodeList {
    std::uintptr_t First = 0;  // the vector's first element
    std::uint32_t Count = 0;
};

NodeList g_nodes;
NodeList g_databases;

// Osiris' database list, found the same way and told apart from the node
// list by its contents: a Database holds its own one-based id in its first
// four bytes, so a sample of elements that agree with their index is the
// list, and anything else is not.
NodeList find_database_db(std::uintptr_t base) {
    // A wider window than the node list needed: the globals are a run of
    // pointer slots, but which run and in what order is this build's
    // business, not bg3se's Windows order.
    for (std::uintptr_t slot : list_slots(osiris_globals().DatabaseDb, 0x400)) {
        std::uintptr_t db = 0;
        if (!peek(base + slot, &db) || db < 0x1000) {
            continue;
        }

        std::uint32_t size = 0;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        if (!peek(db + 0x00, &size) || size < 16 || size > (1u << 22)) {
            continue;
        }
        if (!peek(db + 0x08, &begin) || !peek(db + 0x10, &end)) continue;
        if (begin < 0x1000 || end <= begin) continue;
        if ((end - begin) / 8 != size) continue;

        // Sparse lists are normal -- an unused slot is null -- so nulls are
        // skipped and only what is there has to agree.
        int agreed = 0;
        int disagreed = 0;
        for (std::uint32_t i = 0; i < size && agreed < 8 && disagreed == 0;
             ++i) {
            std::uintptr_t element = 0;
            if (!peek(begin + i * 8, &element) || element < 0x1000) continue;
            std::uint32_t id = 0;
            if (!peek(element + 0x00, &id)) break;
            if (id == i + 1) {
                ++agreed;
            } else {
                ++disagreed;
            }
        }
        if (agreed < 4 || disagreed > 0) {
            if (agreed > 0) {
                logf("osiris: candidate list at libOsiris+%#lx (%u entries) "
                     "had %d ids agree and %d disagree",
                     (unsigned long)slot, size, agreed, disagreed);
            }
            continue;
        }

        logf("osiris: database list at libOsiris+%#lx holds %u databases",
             (unsigned long)slot, size);
        g_databases = NodeList{begin, size};
        return g_databases;
    }
    logf("osiris: no database list found near the function database");
    return NodeList{};
}

// Osiris interns its strings, so a TypedValue holding one holds a handle
// into a string pool rather than a pointer: a fact reads as
// 0x256d01df940f035 where a heap address on this build looks like
// 0x52f40519ee0.
//
// COsiStringTable::GetStr is exported, and its body is the entire
// encoding -- eight instructions:
//
//   index = handle & 0x1fffff;        // the upper bits are never read
//   if (index == 0) return "";
//   return *(*(*this + 0x28) + index * 32);
//
// So a record is 32 bytes with the text pointer first, the record array
// hangs off the table at +0x28 with its end at +0x30, and `this` is a
// COsiStringTable** -- bg3se types the export the same way and calls it
// as GetString(*Globals.StringTable, handle). AddStr agrees: it keeps a
// refcount at record+0x18 and a free list of indices at +0x40/+0x48.
//
// bg3se reads the table from the first of ten globals that the COsiris
// constructor stores, and the same ten stores are here -- but this build
// embeds containers that Windows heap-allocates, which shifts the run and
// is why the earlier structural search, looking for the vector of
// COsiString that bg3se describes, found nothing. The slot is found by
// content instead, which the layout above makes cheap to test.
constexpr std::uint64_t kStringIndexMask = 0x1fffff;
constexpr std::uintptr_t kPoolRecords = 0x28;
constexpr std::uintptr_t kPoolRecordsEnd = 0x30;
constexpr std::size_t kRecordSize = 32;
constexpr std::uintptr_t kRecordRefs = 0x18;

// Only the table address is worth keeping: the record array is a vector,
// so interning a new string can reallocate it and both its address and
// its extent change underneath. Every access re-reads them, which is two
// loads and removes a whole class of stale-pointer bug -- interning a
// string and then failing to read it back is how that was noticed.
struct StringPool {
    std::uintptr_t Table = 0;  // what GetStr and AddStr want as `this`
    std::uintptr_t Records = 0;
    std::size_t Count = 0;
    std::uintptr_t Slot = 0;   // libOsiris' global the table was found in
    bool Indirect = false;     // the global holds Table rather than being it
};

StringPool g_strings;

// The table as libOsiris' global holds it now. Keeping the address read at
// the first search crashed AddStr after loading a second save.
std::uintptr_t table_now() {
    if (g_strings.Slot == 0) return 0;
    std::uintptr_t table = g_strings.Slot;
    if (g_strings.Indirect && (!peek(g_strings.Slot, &table) || table < 0x1000)) return 0;
    if (table != g_strings.Table) {
        logf("osiris: string table moved from %#lx to %#lx", (unsigned long)g_strings.Table,
             (unsigned long)table);
        g_strings.Table = table;
    }
    return table;
}

bool pool_now(std::uintptr_t* records, std::size_t* count) {
    const std::uintptr_t table = table_now();
    if (table == 0) return false;

    std::uintptr_t object = 0;
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    if (!peek(table, &object) || object < 0x1000) return false;
    if (!peek(object + kPoolRecords, &begin)) return false;
    if (!peek(object + kPoolRecordsEnd, &end)) return false;
    if (begin < 0x1000 || end <= begin) return false;

    *records = begin;
    *count = (end - begin) / kRecordSize;
    return true;
}

// A record's text: NUL-terminated printable ASCII. Osiris holds
// identifiers and GUIDs, so this is a tight test.
bool record_text(std::uintptr_t at, std::string* out) {
    if (at < 0x1000) return false;

    // Read in chunks that stop at page ends, so a short string near an
    // unmapped page still reads. A 72-byte window used to drop every name
    // of 72 characters or more, a tenth of DB_Dead.
    constexpr std::size_t kMaxText = 1024;
    std::string text;
    while (text.size() < kMaxText) {
        const std::uintptr_t cursor = at + text.size();
        const std::size_t room = 4096 - (cursor & 4095);
        char chunk[64] = {};
        const std::size_t n = room < sizeof(chunk) ? room : sizeof(chunk);
        if (!safe_read(reinterpret_cast<void const*>(cursor), chunk, n)) return false;
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char c = (unsigned char)chunk[i];
            if (c == 0) {
                if (text.empty() && i == 0) return false;
                text.append(chunk, i);
                if (out != nullptr) *out = std::move(text);
                return true;
            }
            if (c < 0x20 || c > 0x7e) return false;
        }
        text.append(chunk, n);
    }
    return false;
}

// Does this address behave as GetStr's `this`?
bool pool_from(std::uintptr_t table, StringPool* out) {
    std::uintptr_t object = 0;
    if (!peek(table, &object) || object < 0x1000) return false;

    std::uintptr_t records = 0;
    std::uintptr_t end = 0;
    if (!peek(object + kPoolRecords, &records)) return false;
    if (!peek(object + kPoolRecordsEnd, &end)) return false;
    if (records < 0x1000 || end <= records) return false;
    if ((end - records) % kRecordSize != 0) return false;

    const std::size_t count = (end - records) / kRecordSize;
    if (count < 64 || count > (kStringIndexMask + 1)) return false;

    // Index 0 is the empty string and holds no pointer, so sampling
    // starts at 1. A freed record is empty too; what decides it is that
    // nothing occupied is anything other than text.
    std::size_t text = 0;
    std::size_t other = 0;
    for (std::size_t i = 1; i < count && text < 64 && other == 0; ++i) {
        std::uintptr_t at = 0;
        if (!peek(records + i * kRecordSize, &at)) return false;
        if (at == 0) continue;
        if (record_text(at, nullptr)) {
            ++text;
        } else {
            ++other;
        }
    }
    if (text < 32 || other > 0) return false;

    *out = StringPool{table, records, count};
    return true;
}

// libOsiris' writable segment, which is where the global lives.
struct Segment {
    std::uintptr_t Begin = 0;
    std::uintptr_t End = 0;
};

int find_osiris_data(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name == nullptr) return 0;
    if (std::strstr(info->dlpi_name, "libOsiris.so") == nullptr) return 0;

    auto* out = static_cast<Segment*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        ElfW(Phdr) const& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD || (ph.p_flags & PF_W) == 0) continue;
        const std::uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        if (out->Begin == 0 || begin < out->Begin) out->Begin = begin;
        if (begin + ph.p_memsz > out->End) out->End = begin + ph.p_memsz;
    }
    return 1;
}

bool find_string_table() {
    if (g_strings.Records != 0) return true;

    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);

    Segment segment;
    ::dl_iterate_phdr(find_osiris_data, &segment);
    if (segment.Begin == 0 || segment.End <= segment.Begin) {
        logf("strings: libOsiris has no writable segment");
        return false;
    }

    for (std::uintptr_t at = segment.Begin; at + 8 <= segment.End; at += 8) {
        StringPool found;

        // The global holds a COsiStringTable**, so the slot is one
        // dereference away from what GetStr is passed. The slot itself is
        // tried as well, in case this build stores the object inline as it
        // does the node and database lists.
        std::uintptr_t indirect = 0;
        if (peek(at, &indirect) && indirect >= 0x1000
            && pool_from(indirect, &found)) {
            g_strings = found;
            g_strings.Slot = at;
            g_strings.Indirect = true;
        } else if (pool_from(at, &found)) {
            g_strings = found;
            g_strings.Slot = at;
        } else {
            continue;
        }

        logf("osiris: string table at libOsiris+%#lx holds %zu strings",
             (unsigned long)(at - base), g_strings.Count);
        return true;
    }

    logf("osiris: no string table found in libOsiris' data");
    return false;
}


NodeList find_node_db(std::uintptr_t base) {
    for (std::uintptr_t slot : list_slots(osiris_globals().NodeDb, 0x80)) {
        std::uintptr_t db = 0;
        if (!peek(base + slot, &db) || db < 0x1000) {
            continue;
        }

        std::uint32_t size = 0;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        if (!peek(db + 0x00, &size) || size < 16 || size > (1u << 22)) {
            continue;
        }
        if (!peek(db + 0x08, &begin) || !peek(db + 0x10, &end)) continue;
        if (begin < 0x1000 || end <= begin) continue;
        if ((end - begin) / 8 != size) continue;

        // Elements are objects, and an object here starts with a vtable
        // in the library that defines it.
        int checked = 0;
        for (std::uint32_t i = 1; i < size && checked < 4; ++i) {
            std::uintptr_t element = 0;
            if (!peek(begin + i * 8, &element) || element < 0x1000) continue;
            std::uintptr_t vmt = 0;
            if (!peek(element, &vmt) || vmt < 0x1000) return NodeList{};
            ++checked;
        }
        if (checked == 0) continue;

        logf("osiris: node list at libOsiris+%#lx holds %u nodes",
             (unsigned long)slot, size);
        g_nodes = NodeList{begin, size};
        return g_nodes;
    }
    logf("osiris: no node list found near the function database");
    return NodeList{};
}

// The node a function runs through, by the id in OsiFunctionDef+0x20.
//
// Ids are one-based into the node vector, as bg3se's
// Nodes->Db.Elements[Node.Id - 1] has it. The node is only accepted if it
// agrees: a Node holds its own id at +0x08 and a pointer back to its
// function at +0x10, so a wrong list or a wrong offset shows up as a
// mismatch rather than as a call into the wrong object.
std::uintptr_t node_for(std::uintptr_t def) {
    if (g_nodes.First == 0) return 0;

    std::uint32_t id = 0;
    if (!peek(def + kNodeRef, &id) || id == 0 || id > g_nodes.Count) return 0;

    std::uintptr_t node = 0;
    if (!peek(g_nodes.First + (std::uintptr_t)(id - 1) * 8, &node)
        || node < 0x1000) {
        return 0;
    }

    std::uint32_t ownId = 0;
    std::uintptr_t function = 0;
    if (!peek(node + kNodeId, &ownId)
        || !peek(node + kNodeFunction, &function)) {
        return 0;
    }
    if (ownId != id || function != def) return 0;
    return node;
}

// One fact from each of the first few databases that have any.
//
// Database, as this build lays it out: its own id in the low half of the
// first eight bytes, the facts VMT, then a circular list of facts whose
// header points at itself when empty, with the count at +0x20. A fact
// node is {Next, Prev, TypedValue* Values, uint64 Width}, and a
// TypedValue is {value, uint16 TypeId, int8 Index, uint8 Flags} at
// sixteen bytes. Stored strings are interned: the value is a handle into
// one of Osiris' string pools, not a pointer.
void dump_database_facts() {
    if (g_databases.First == 0) return;

    int shown = 0;
    for (std::uint32_t i = 0; i < g_databases.Count && shown < 3; ++i) {
        std::uintptr_t db = 0;
        if (!peek(g_databases.First + (std::uintptr_t)i * 8, &db)
            || db < 0x1000) {
            continue;
        }

        std::uintptr_t head = 0;
        std::uint64_t count = 0;
        if (!peek(db + kFactsHead, &head) || !peek(db + kFactsCount, &count)) {
            continue;
        }
        if (count == 0 || count > (1u << 20) || head < 0x1000) continue;

        logf("dbfacts: database %u at %#lx holds %llu facts", i + 1,
             (unsigned long)db, (unsigned long long)count);

        std::uintptr_t values = 0;
        std::uint64_t width = 0;
        if (!peek(head + 0x10, &values) || !peek(head + 0x18, &width)
            || values < 0x1000 || width == 0 || width > 32) {
            logf("dbfacts:   first node %#lx does not read as a tuple",
                 (unsigned long)head);
            ++shown;
            continue;
        }

        for (std::uint64_t k = 0; k < width; ++k) {
            const std::uintptr_t tv = values + k * 16;
            std::uint64_t raw = 0;
            std::uint16_t type = 0;
            std::uint8_t flags = 0;
            if (!peek(tv + 0x00, &raw) || !peek(tv + 0x08, &type)
                || !peek(tv + 0x0b, &flags)) {
                break;
            }
            logf("dbfacts:   [%llu] type=%u flags=%#x value=%#llx",
                 (unsigned long long)k, type, flags,
                 (unsigned long long)raw);
        }
        ++shown;
    }
}

// ---- finding the string pool ----
//
// Osiris interns its strings, so a stored value is a handle. Reading one
// back, or building one for a procedure's argument, needs the pool that
// resolves it, and the pool is not laid out the way bg3se describes for
// Windows. So it is found from the other end: take a string Osiris
// certainly holds, find it in memory, and look at what points at it.

// Which mapping an address falls in, for telling heap from file-backed.
std::string mapping_of(std::uintptr_t at) {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return "maps unavailable";

    char line[1024];
    std::string found = "unmapped";
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (std::sscanf(line, "%llx-%llx", &from, &to) != 2) continue;
        if (at < from || at >= to) continue;
        found = line;
        while (!found.empty() && (found.back() == '\n' || found.back() == ' ')) {
            found.pop_back();
        }
        break;
    }
    std::fclose(maps);
    return found;
}

std::vector<std::uintptr_t> find_bytes(char const* needle) {
    std::vector<std::uintptr_t> out;
    const std::size_t length = std::strlen(needle) + 1;  // with the NUL

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;

    constexpr std::size_t kChunk = 1u << 20;
    std::vector<unsigned char> block(kChunk + 64);

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr
           && out.size() < 64) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long at = from; at < to && out.size() < 64;
             at += kChunk) {
            std::size_t want = (std::size_t)(to - at);
            if (want > block.size()) want = block.size();
            const std::size_t got =
                safe_read_some((void const*)at, block.data(), want);
            if (got < length) continue;
            scan_yield();

            for (std::size_t off = 0; off + length <= got; ++off) {
                if (std::memcmp(block.data() + off, needle, length) != 0) {
                    continue;
                }
                out.push_back((std::uintptr_t)(at + off));
                if (out.size() >= 64) break;
            }
        }
    }
    std::fclose(maps);
    return out;
}

std::vector<std::uintptr_t> find_pointers_to(std::uintptr_t wanted,
                                             std::size_t limit) {
    std::vector<std::uintptr_t> out;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;

    constexpr std::size_t kChunk = 1u << 20;
    std::vector<unsigned char> block(kChunk + 8);

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr
           && out.size() < limit) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long at = from; at < to && out.size() < limit;
             at += kChunk) {
            std::size_t want = (std::size_t)(to - at);
            if (want > block.size()) want = block.size();
            const std::size_t got =
                safe_read_some((void const*)at, block.data(), want);
            if (got < 8) continue;
            scan_yield();

            for (std::size_t off = 0; off + 8 <= got; off += 8) {
                std::uintptr_t word = 0;
                std::memcpy(&word, block.data() + off, sizeof(word));
                if (word != wanted) continue;
                out.push_back((std::uintptr_t)(at + off));
                if (out.size() >= limit) break;
            }
        }
    }
    std::fclose(maps);
    return out;
}

// Whether `at` holds a pointer to printable text.
bool slot_points_at_text(std::uintptr_t at) {
    std::uintptr_t str = 0;
    if (!peek(at, &str) || str < 0x1000) return false;

    char text[8] = {};
    if (!safe_read(reinterpret_cast<void const*>(str), text, sizeof(text))) {
        return false;
    }
    for (char ch : text) {
        if (ch == '\0') return true;
        if ((unsigned char)ch < 0x20 || (unsigned char)ch > 0x7e) return false;
    }
    return true;
}

// Candidate encodings for a string handle, given the text.
//
// Osiris stores a copy of the text per instance -- eleven copies of the
// host character's UUID are in memory, eight of them in one array of
// pointers -- so a handle is unlikely to be an index into a table of
// unique strings. The likelier shape is something derived from the text,
// which is testable: derive it every plausible way and look for the answer
// among the handles the databases actually hold.
struct Candidate {
    char const* Name;
    std::uint64_t Value;
};

std::vector<Candidate> handle_candidates(char const* text) {
    const std::size_t length = std::strlen(text);

    std::uint64_t fnv1a64 = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < length; ++i) {
        fnv1a64 ^= (unsigned char)text[i];
        fnv1a64 *= 0x100000001b3ull;
    }

    std::uint32_t fnv1a32 = 0x811c9dc5u;
    for (std::size_t i = 0; i < length; ++i) {
        fnv1a32 ^= (unsigned char)text[i];
        fnv1a32 *= 0x01000193u;
    }

    std::uint64_t djb2 = 5381;
    for (std::size_t i = 0; i < length; ++i) {
        djb2 = djb2 * 33 + (unsigned char)text[i];
    }

    std::uint32_t crc = 0xffffffffu;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= (unsigned char)text[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (~(crc & 1) + 1));
        }
    }
    crc = ~crc;

    // A GUID string also parses as two 64-bit halves, which is what
    // DivTools::ParseGuidString returns -- a handle could just be that.
    std::uint64_t high = 0;
    std::uint64_t low = 0;
    int digits = 0;
    for (std::size_t i = 0; i < length; ++i) {
        const char ch = text[i];
        int value = -1;
        if (ch >= '0' && ch <= '9') value = ch - '0';
        if (ch >= 'a' && ch <= 'f') value = ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') value = ch - 'A' + 10;
        if (value < 0) continue;
        if (digits < 16) {
            high = (high << 4) | (std::uint64_t)value;
        } else {
            low = (low << 4) | (std::uint64_t)value;
        }
        ++digits;
    }

    return {
        {"fnv1a64", fnv1a64},
        {"fnv1a32", fnv1a32},
        {"djb2", djb2},
        {"crc32", crc},
        {"guid high", high},
        {"guid low", low},
    };
}

// Every handle the databases hold, so a derived value can be looked for
// among them.
struct StoredValue {
    std::uint64_t Raw = 0;
    std::uint16_t Type = 0;
};

std::vector<StoredValue> stored_values(std::size_t limit) {
    std::vector<StoredValue> out;
    if (g_databases.First == 0) return out;

    for (std::uint32_t i = 0; i < g_databases.Count && out.size() < limit;
         ++i) {
        std::uintptr_t db = 0;
        if (!peek(g_databases.First + (std::uintptr_t)i * 8, &db)
            || db < 0x1000) {
            continue;
        }

        std::uintptr_t head = 0;
        std::uint64_t facts = 0;
        if (!peek(db + kFactsHead, &head) || !peek(db + kFactsCount, &facts)) {
            continue;
        }
        if (facts == 0 || facts > (1u << 20) || head < 0x1000) continue;

        std::uintptr_t node = head;
        for (std::uint64_t f = 0; f < facts && out.size() < limit; ++f) {
            std::uintptr_t values = 0;
            std::uint64_t width = 0;
            if (!peek(node + 0x10, &values) || !peek(node + 0x18, &width)
                || values < 0x1000 || width == 0 || width > 32) {
                break;
            }
            for (std::uint64_t k = 0; k < width && out.size() < limit; ++k) {
                StoredValue value;
                if (!peek(values + k * 16, &value.Raw)
                    || !peek(values + k * 16 + 8, &value.Type)) {
                    break;
                }
                out.push_back(value);
            }
            if (!peek(node + 0x00, &node) || node < 0x1000) break;
        }
    }
    return out;
}

std::vector<std::uint64_t> stored_handles(std::size_t limit) {
    std::vector<std::uint64_t> out;
    if (g_databases.First == 0) return out;

    for (std::uint32_t i = 0; i < g_databases.Count && out.size() < limit;
         ++i) {
        std::uintptr_t db = 0;
        if (!peek(g_databases.First + (std::uintptr_t)i * 8, &db)
            || db < 0x1000) {
            continue;
        }

        std::uintptr_t head = 0;
        std::uint64_t facts = 0;
        if (!peek(db + kFactsHead, &head) || !peek(db + kFactsCount, &facts)) {
            continue;
        }
        if (facts == 0 || facts > (1u << 20) || head < 0x1000) continue;

        std::uintptr_t node = head;
        for (std::uint64_t f = 0; f < facts && out.size() < limit; ++f) {
            std::uintptr_t values = 0;
            std::uint64_t width = 0;
            if (!peek(node + 0x10, &values) || !peek(node + 0x18, &width)
                || values < 0x1000 || width == 0 || width > 32) {
                break;
            }

            for (std::uint64_t k = 0; k < width && out.size() < limit; ++k) {
                std::uint64_t raw = 0;
                std::uint16_t type = 0;
                if (!peek(values + k * 16, &raw)
                    || !peek(values + k * 16 + 8, &type)) {
                    break;
                }
                // String and GuidString, and the story's own types alias
                // to one of them.
                if (type == 4 || type == 5 || type >= 6) out.push_back(raw);
            }

            if (!peek(node + 0x00, &node) || node < 0x1000) break;
        }
    }
    return out;
}

// The facts of one database, by the name of the function that stands for
// it, so a handle can be paired with text that is known by other means.
std::vector<std::uint64_t> handles_of(char const* key) {
    std::vector<std::uint64_t> out;

    auto entry = database().find(key);
    if (entry == database().end() || entry->second.Def == 0) return out;

    const std::uintptr_t node = node_for(entry->second.Def);
    if (node == 0) return out;

    std::uint32_t dbId = 0;
    if (!peek(node + 0x18, &dbId) || dbId == 0 || dbId > g_databases.Count) {
        return out;
    }

    std::uintptr_t db = 0;
    if (!peek(g_databases.First + (std::uintptr_t)(dbId - 1) * 8, &db)
        || db < 0x1000) {
        return out;
    }

    std::uintptr_t head = 0;
    std::uint64_t facts = 0;
    if (!peek(db + kFactsHead, &head) || !peek(db + kFactsCount, &facts)) {
        return out;
    }
    if (facts == 0 || facts > 4096 || head < 0x1000) return out;

    std::uintptr_t at = head;
    for (std::uint64_t f = 0; f < facts; ++f) {
        std::uintptr_t values = 0;
        std::uint64_t width = 0;
        if (!peek(at + 0x10, &values) || !peek(at + 0x18, &width)
            || values < 0x1000 || width == 0 || width > 32) {
            break;
        }
        for (std::uint64_t k = 0; k < width; ++k) {
            std::uint64_t raw = 0;
            if (!peek(values + k * 16, &raw)) break;
            out.push_back(raw);
        }
        if (!peek(at + 0x00, &at) || at < 0x1000) break;
    }
    return out;
}

// What the stored values look like, grouped by their declared type. The
// point is whether every type encodes a value the same way: a plain string
// and a GUID string may well not.
void report_value_shapes() {
    const std::vector<StoredValue> values = stored_values(1u << 17);
    logf("strings: %zu stored values", values.size());

    std::map<std::uint16_t, std::vector<std::uint64_t>> byType;
    for (StoredValue const& value : values) {
        auto& list = byType[value.Type];
        if (list.size() < 6) list.push_back(value.Raw);
    }

    std::map<std::uint16_t, std::size_t> counts;
    for (StoredValue const& value : values) ++counts[value.Type];

    for (auto const& entry : byType) {
        std::string shown;
        for (std::uint64_t raw : entry.second) {
            char one[48] = {};
            // Split the way the sequential handles suggested: a field at
            // bit 21 and up, and the twenty-one bits below it.
            std::snprintf(one, sizeof(one), "%#llx(%llu|%llu) ",
                          (unsigned long long)raw,
                          (unsigned long long)(raw >> 21),
                          (unsigned long long)(raw & 0x1fffff));
            shown += one;
        }
        logf("strings:   type %u: %zu values: %s", entry.first,
             counts[entry.first], shown.c_str());
    }
}

// A stored value only means a string handle if its type says so, and
// most types are aliases: the story declares CHARACTERGUID, the engine
// resolves that to GuidString. Osiris keeps the resolution in an array
// indexed by type id -- bg3se's OsiTypeDb::AliasInfo, {uint16 TypeId;
// uint16 AliasTypeId} -- and walks it until the id is String or
// GuidString.
//
// The array hangs off the type database, whose global is one of the ten
// the COsiris constructor stores, at an offset past a 1023-slot hash
// table that this build does not have to size the same way. Finding it
// directly is easier and self-proving: an array whose every entry holds
// its own index, for sixty-four entries running, with aliases that all
// resolve to a base type, is not something an arena produces by
// accident.
//
// Without it a type-1 integer value of 6 reads as the string at index 6
// and looks convincing -- "HealingSpiritHeal" -- which is how the need
// for this was noticed.
constexpr std::uint16_t kTypeString = 4;
constexpr std::uint16_t kTypeGuidString = 5;
constexpr std::uint16_t kTypeUndefined = 0x7f;

constexpr std::size_t kTypeCount = 128;  // ids are 7 bits

// Copied out of the engine's table once rather than read through a
// pointer per value: a fact row asks about every column, and the answer
// cannot change while a story is loaded.
std::array<std::uint16_t, kTypeCount> g_alias{};
bool g_alias_ready = false;
std::uintptr_t g_alias_at = 0;  // where they were read from, for the log

// Resolve as the engine does, with a bound: a cycle in the table would
// otherwise spin.
std::uint16_t resolve_alias(std::uint16_t type) {
    if (!g_alias_ready) return type;

    for (int step = 0; step < 16; ++step) {
        if (type <= kTypeGuidString || type == kTypeUndefined) return type;
        if (type >= kTypeCount) return type;

        const std::uint16_t next = g_alias[type];
        if (next == type || next == 0) return type;
        type = next;
    }
    return type;
}

bool is_string_type(std::uint16_t type) {
    const std::uint16_t base = resolve_alias(type);
    return base == kTypeString || base == kTypeGuidString;
}

// A story type's base, as upstream's ResolveAlias gives it, or a GUID string
// when nothing says otherwise.
unsigned short slot_type(std::uint8_t declared) {
    if (declared < 6) return declared;
    const std::uint16_t base = resolve_alias(declared);
    return base >= 1 && base <= kTypeGuidString ? base : kGuidString;
}

// A node typed with its declared story type, read by its base. The number
// getters assert on the exact base type, so those values are read where
// SetInteger and SetFloat put them; GetAnyString resolves the alias itself.
Value read_declared(const void* node, std::uint8_t declared) {
    const unsigned short base = slot_type(declared);
    if (declared < 6 || base == kString || base == kGuidString) return read(node, base);
    Value v;
    v.type = base;
    char const* at = static_cast<char const*>(node) + 8;
    if (base == kInteger) {
        std::int32_t i = 0;
        std::memcpy(&i, at, sizeof(i));
        v.integer = i;
    } else if (base == kInteger64) {
        std::int64_t i = 0;
        std::memcpy(&i, at, sizeof(i));
        v.integer = i;
    } else if (base == kReal) {
        float f = 0;
        std::memcpy(&f, at, sizeof(f));
        v.real = f;
    }
    return v;
}

// Learned from what the story stores, not from the engine's table.
//
// bg3se resolves an alias through OsiTypeDb::Aliases, an array of
// {uint16 TypeId; uint16 AliasTypeId} indexed by type id. Nothing of that
// shape is in this process: a scan of every writable mapping for
// sixty-four consecutive entries holding their own index finds one array
// of {index, 1} pairs, which is something else -- taking it made every
// aliased type an integer, and a GUID argument to a procedure silently
// became 0. It looked right because the two type ids one naturally checks,
// 4 and 5, are base types that resolve without consulting the table.
//
// What the story stores answers the question directly and needs no
// structure at all. A stored string is a handle, and a handle's bits above
// the low 21 are never zero -- consecutive facts differ by 0x200001 -- so
// a column whose values all carry those bits and resolve to text is a
// string, and one whose values are all under 2^21 is an integer. The two
// do not overlap: a genuine small integer has nothing above bit 21, and a
// handle to even the first pool record does.
//
// String or GUID string is decided by the text: Osiris wants to know which
// when interning, and a GUID string ends in a uuid.
constexpr std::size_t kAliasEvidence = 4;

// Defined below, with the rest of the pool reading; wanted here because
// deciding a type means trying to read a string through it.
bool string_of(std::uint64_t handle, std::string* out);

bool guid_looking(std::string const& text) {
    if (text.size() < 36) return false;

    char const* at = text.c_str() + text.size() - 36;
    for (int i = 0; i < 36; ++i) {
        const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash) {
            if (at[i] != '-') return false;
        } else if (std::isxdigit((unsigned char)at[i]) == 0) {
            return false;
        }
    }
    return true;
}

// Does this value look like a handle to text, rather than a number?
bool handle_looking(std::uint64_t raw, std::string* text) {
    if ((raw >> 21) == 0) return false;
    return string_of(raw, text) && !text->empty();
}

void learn_aliases() {
    if (g_alias_ready) return;

    struct Evidence {
        std::size_t Handles = 0;
        std::size_t Numbers = 0;
        std::size_t Guids = 0;
    };
    std::map<std::uint16_t, Evidence> seen;

    for (StoredValue const& value : stored_values(1u << 16)) {
        if (value.Type <= kTypeGuidString || value.Type >= kTypeCount) {
            continue;  // a base type says nothing about aliases
        }
        if (value.Raw == 0) continue;

        Evidence& evidence = seen[value.Type];
        std::string text;
        if (handle_looking(value.Raw, &text)) {
            ++evidence.Handles;
            if (guid_looking(text)) ++evidence.Guids;
        } else if ((value.Raw >> 21) == 0) {
            ++evidence.Numbers;
        }
    }

    std::size_t strings = 0;
    std::size_t guids = 0;
    std::size_t numbers = 0;
    std::size_t unclear = 0;
    for (auto const& entry : seen) {
        Evidence const& evidence = entry.second;
        const std::size_t total = evidence.Handles + evidence.Numbers;
        if (total < kAliasEvidence) {
            ++unclear;
            continue;
        }

        if (evidence.Handles > evidence.Numbers * 4) {
            const bool guid = evidence.Guids * 2 >= evidence.Handles;
            g_alias[entry.first] = guid ? kTypeGuidString : kTypeString;
            if (guid) {
                ++guids;
            } else {
                ++strings;
            }
        } else if (evidence.Numbers > evidence.Handles * 4) {
            g_alias[entry.first] = kInteger;
            ++numbers;
        } else {
            ++unclear;
        }
    }

    if (strings + guids + numbers == 0) {
        logf("osiris: the story stores nothing that says what its own types "
             "are; aliased columns will be read by the shape of each value");
        return;
    }

    g_alias_ready = true;
    logf("osiris: learned %zu type aliases from the story's own facts "
         "(%zu guid strings, %zu strings, %zu integers; %zu with too little "
         "evidence, decided per value)",
         strings + guids + numbers, guids, strings, numbers, unclear);
}

// The story's own type declarations, from the header Osiris writes while it
// loads: alias_type {NAME, id, base} and enum_type {NAME, id, ...}, an enum
// being an integer. Covers the types no fact stores, which learning misses.
void read_story_header_types() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return;
    const std::string path = std::string(home)
        + "/.local/share/Larian Studios/Baldur's Gate 3/Temp/Story/story_header.div";
    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return;

    std::size_t aliases = 0;
    std::size_t enums = 0;
    char line[4096];
    char name[256];
    unsigned int id = 0;
    unsigned int base = 0;
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        if (std::sscanf(line, "alias_type {%255[^,], %u, %u}", name, &id, &base) == 3) {
            if (id < kTypeCount && base > 0 && base < kTypeCount) {
                g_alias[id] = (std::uint16_t)base;
                ++aliases;
            }
        } else if (std::sscanf(line, "enum_type {%255[^,], %u,", name, &id) == 2) {
            if (id < kTypeCount) {
                g_alias[id] = kInteger;
                ++enums;
            }
        } else if (std::strncmp(line, "query ", 6) == 0 || std::strncmp(line, "call ", 5) == 0) {
            break;  // the type declarations come first
        }
    }
    std::fclose(f);
    if (aliases + enums == 0) return;
    g_alias_ready = true;
    logf("osiris: %zu type aliases and %zu enum types from the story header", aliases, enums);
}

// A value whose type says nothing, read by its own shape. Used for an
// aliased type that appears in no fact anywhere, so nothing could be
// learned about it.
Value value_by_shape(std::uint64_t raw) {
    Value value;
    std::string text;
    if (handle_looking(raw, &text)) {
        value.type = kString;
        value.text = std::move(text);
    } else {
        value.type = kInteger;
        value.integer = (std::int32_t)(std::uint32_t)raw;
    }
    return value;
}

// Whether the alias of this type is known at all. An aliased type that
// appears in no fact anywhere is not, and a caller has to fall back to
// the shape of the value it was given rather than write a zero.
bool type_known(std::uint16_t type) {
    if (type <= kTypeGuidString) return true;
    return type < kTypeCount && g_alias[type] != 0;
}

// GetStr, done by reading rather than calling: the encoding is known, and
// a read cannot take the game down on a handle the pool has since freed.
bool string_of(std::uint64_t handle, std::string* out) {
    std::uintptr_t records = 0;
    std::size_t count = 0;
    if (!pool_now(&records, &count)) return false;

    const std::uint64_t index = handle & kStringIndexMask;
    if (index == 0) {
        if (out != nullptr) out->clear();
        return true;
    }
    if (index >= count) return false;

    std::uintptr_t at = 0;
    if (!peek(records + (std::uintptr_t)index * kRecordSize, &at)) return false;
    return record_text(at, out);
}

// How many references the pool holds for a handle. Worth reading because
// interning is meant to bump it: a round trip that returns the same index
// and one more reference is the whole layout confirmed.
std::uint32_t string_refs(std::uint64_t handle) {
    std::uintptr_t records = 0;
    std::size_t count = 0;
    if (!pool_now(&records, &count)) return 0;

    const std::uint64_t index = handle & kStringIndexMask;
    if (index == 0 || index >= count) return 0;

    std::uint32_t refs = 0;
    peek(records + (std::uintptr_t)index * kRecordSize + kRecordRefs, &refs);
    return refs;
}

// AddStr, which has to be a call: it allocates through Osiris' own
// allocator, keeps the hash map that makes interning idempotent, and
// refcounts the record. Called on the story thread, as bg3se does.
using AddStrProc = std::uint64_t (*)(void*, char const*, bool);

AddStrProc add_str() {
    static AddStrProc proc = reinterpret_cast<AddStrProc>(
        ::dlsym(RTLD_DEFAULT, "_ZN15COsiStringTable6AddStrEPKcb"));
    return proc;
}

// The empty string is index 0 of the table, so 0 is a valid handle for "".
std::uint64_t intern_string(char const* text, bool guid) {
    if (text == nullptr) return 0;

    // A table whose record array does not read is not one to hand AddStr.
    std::uintptr_t records = 0;
    std::size_t count = 0;
    if (!pool_now(&records, &count)) return 0;

    AddStrProc proc = add_str();
    if (proc == nullptr) return 0;

    return proc(reinterpret_cast<void*>(g_strings.Table), text, guid);
}

// The Function objects, bound on demand.
//
// A cached signature carries names, arities, types and out-parameter
// counts, but not the address of the Function object behind each one:
// that is a heap pointer, different every run, and caching it would be
// wrong rather than merely stale. Calling a story function needs it --
// the node a tuple is inserted into is reached through it -- so the tree
// walk still has to happen, just not during the level load. The first
// call pays the ~0.4s; a session where no mod calls Osiris never does.
void save_cached_signatures(char const* story);

bool bind_defs() {
    if (g_defs_bound) return true;

    // The signature walk fills these in as it goes, so when it ran this
    // run there is nothing to do: walking a second time cost half a
    // second of the level load before this check existed.
    for (auto const& entry : database()) {
        if (entry.second.Def != 0) {
            g_defs_bound = true;
            return true;
        }
    }

    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);
    if (base == 0) return false;

    std::uintptr_t holder = 0;
    if (!peek(base + function_db_holder(), &holder) || holder < 0x1000) {
        return false;
    }

    std::unordered_map<std::string, DbEntry> live;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        const std::uintptr_t slot = holder + 0x10 + i * kSlotStride;
        std::unordered_set<std::uintptr_t> seen;
        for (std::uintptr_t off = 0x00; off <= 0x08; off += 0x08) {
            std::uintptr_t map = 0;
            if (!peek(slot + off, &map)) continue;
            visit_tree(map, &live, 0, &seen);
        }
    }
    if (live.empty()) {
        logf("osiris: no function objects found; story calls stay unavailable");
        return false;
    }

    // The cache wins on types -- it was verified against the engine's own
    // mapping -- so only the pointer is taken from the walk.
    std::size_t added = 0;
    for (auto const& entry : live) {
        auto it = database().find(entry.first);
        if (it == database().end()) {
            database().emplace(entry.first, entry.second);
            ++added;
            continue;
        }
        it->second.Def = entry.second.Def;
        if (it->second.Types.empty()) it->second.Types = entry.second.Types;
    }
    // The cache key (the story version) is the same for every mod set, so a
    // cache can be another story's: what this one lacks is dropped.
    std::size_t gone = 0;
    for (auto it = database().begin(); it != database().end();) {
        if (live.count(it->first) == 0) {
            it = database().erase(it);
            ++gone;
        } else {
            ++it;
        }
    }

    g_defs_bound = true;
    logf("osiris: bound %zu function objects (%zu the cache did not have, %zu it had that the story does not)",
         live.size(), added, gone);
    if ((added != 0 || gone != 0) && !g_cache_story.empty()) {
        save_cached_signatures(g_cache_story.c_str());
    }
    return true;
}

// Which class each node is, by its vtable.
//
// Running a procedure means calling Node::InsertTuple, and that is a
// virtual: bg3se puts it at +0x50 of the node vtable, counting from a
// Destroy slot. That offset cannot be carried over, because the Itanium
// ABI spends two slots on destructors where MSVC spends one, so every
// slot after the first shifts by eight. Nothing here names those methods,
// so the slot has to be established rather than assumed.
//
// This is the first half: the vtable each kind of node actually uses,
// with an example function for each, so the proc node class is known by
// what uses it rather than by its position in a list.
void report_node_vtables() {
    if (g_nodes.First == 0 || !bind_defs()) return;

    std::map<std::uintptr_t, std::size_t> counts;
    for (std::uint32_t i = 0; i < g_nodes.Count; ++i) {
        std::uintptr_t node = 0;
        if (!peek(g_nodes.First + (std::uintptr_t)i * 8, &node)
            || node < 0x1000) {
            continue;
        }
        std::uintptr_t vmt = 0;
        if (!peek(node, &vmt) || vmt < 0x1000) continue;
        ++counts[vmt];
    }

    // An example name per vtable, which is what tells the classes apart:
    // a DB_ function's node is a database node, and a procedure's is not.
    std::map<std::uintptr_t, std::string> examples;
    for (auto const& entry : database()) {
        if (entry.second.Def == 0) continue;
        const std::uintptr_t node = node_for(entry.second.Def);
        if (node == 0) continue;
        std::uintptr_t vmt = 0;
        if (!peek(node, &vmt) || vmt < 0x1000) continue;

        std::string& shown = examples[vmt];
        // Prefer a name that says what the node is for.
        if (shown.empty() || (shown.rfind("DB_", 0) != 0
                              && entry.first.rfind("DB_", 0) == 0)) {
            shown = entry.first;
        }
    }

    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);
    for (auto const& entry : counts) {
        logf("nodes: vtable libOsiris+%#lx used by %zu nodes, e.g. %s",
             (unsigned long)(entry.first - base), entry.second,
             examples.count(entry.first) != 0
                 ? examples[entry.first].c_str() : "(no named function)");
    }
}

void probe_string_pool(char const* text) {
    report_node_vtables();
    report_value_shapes();
    if (!find_string_table()) return;

    // Resolution, on values the engine put there itself: one sample of
    // every type that carries a string.
    const std::vector<StoredValue> values = stored_values(1u << 17);
    std::map<std::uint16_t, std::size_t> shown;
    std::size_t resolved = 0;
    std::size_t failed = 0;
    for (StoredValue const& value : values) {
        if (!is_string_type(value.Type)) continue;

        std::string held;
        if (!string_of(value.Raw, &held) || held.empty()) {
            ++failed;
            continue;
        }
        ++resolved;
        if (shown[value.Type]++ < 2) {
            logf("strings: type %u %#llx -> \"%s\"", value.Type,
                 (unsigned long long)value.Raw, held.c_str());
        }
    }
    logf("strings: %zu of %zu string-typed values resolve to text", resolved,
         resolved + failed);

    // And interning, which has to be idempotent: a string the pool
    // already holds comes back as the handle it already has. That is the
    // round trip a procedure argument needs.
    for (StoredValue const& value : values) {
        if (!is_string_type(value.Type)) continue;

        std::string held;
        if (!string_of(value.Raw, &held) || held.empty()) continue;

        const bool guid = resolve_alias(value.Type) == kTypeGuidString;
        const std::uint32_t before = string_refs(value.Raw);
        const std::uint64_t again = intern_string(held.c_str(), guid);
        logf("strings: intern(\"%s\", guid=%d) -> %#llx: index %llu, wanted "
             "%llu; refs %u -> %u", held.c_str(), guid ? 1 : 0,
             (unsigned long long)again,
             (unsigned long long)(again & kStringIndexMask),
             (unsigned long long)(value.Raw & kStringIndexMask), before,
             string_refs(again));
        break;
    }

    if (text == nullptr || std::strlen(text) < 8) return;

    const std::uint64_t handle = intern_string(text, std::strlen(text) > 30);
    std::string back;
    string_of(handle, &back);
    logf("strings: intern(\"%s\") -> %#llx -> \"%s\"", text,
         (unsigned long long)handle, back.c_str());
}

// Which offset holds FunctionType, decided once by agreement with the
// engine's mapping.
std::uintptr_t& type_offset() {
    static std::uintptr_t at = 0;
    return at;
}

// Where the signature cache lives, alongside the static-pointer cache.
std::string cache_path(char const* story) {
    char const* home = std::getenv("HOME");
    if (home == nullptr || story == nullptr || story[0] == '\0') return {};

    std::string safe;
    for (char const* at = story; *at != '\0'; ++at) {
        safe += (std::isalnum((unsigned char)*at) != 0) ? *at : '-';
    }
    return std::string(home) + "/.local/share/bg3le/osiris-" + safe + ".txt";
}

// A store without this line predates the FunctionType offset, and is walked again.
constexpr char kStoreFormat[] = "# format 3\n";

// name/arity outs outmask type,type,...
bool load_cached_signatures(char const* story,
                            std::vector<Function>* functions,
                            std::size_t* applied) {
    const std::string path = cache_path(story);
    if (path.empty()) return false;

    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return false;

    std::unordered_map<std::string, DbEntry> loaded;
    char line[1024];
    if (std::fgets(line, sizeof(line), f) == nullptr
        || std::strcmp(line, kStoreFormat) != 0) {
        std::fclose(f);
        logf("osiris: %s is from an older bg3le; walking again", path.c_str());
        return false;
    }
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::size_t story = 0;
        if (std::sscanf(line, "# story %zu", &story) == 1) {
            g_story_functions = story;
            continue;
        }

        unsigned long typeAt = 0;
        if (std::sscanf(line, "# typeat %lx", &typeAt) == 1) {
            type_offset() = typeAt;
            continue;
        }

        unsigned int type = 0;
        unsigned int alias = 0;
        if (std::sscanf(line, "# alias %u %u", &type, &alias) == 2) {
            if (type < kTypeCount) {
                g_alias[type] = (std::uint16_t)alias;
                g_alias_ready = true;
            }
            continue;
        }

        char key[512] = {};
        int outs = 0;
        unsigned long long which = 0;
        char types[256] = {};
        const int got = std::sscanf(line, "%511s %d %llx %255s", key, &outs, &which, types);
        if (got < 3) continue;

        DbEntry entry;
        entry.Outs = outs;
        entry.OutMask = which;
        if (got == 4) {
            char* rest = nullptr;
            for (char* at = strtok_r(types, ",", &rest); at != nullptr;
                 at = strtok_r(nullptr, ",", &rest)) {
                entry.Types.push_back((std::uint8_t)std::strtoul(at, nullptr, 10));
            }
        }
        loaded.emplace(key, std::move(entry));
    }
    std::fclose(f);
    if (loaded.empty()) return false;

    std::size_t hits = 0;
    for (Function& fn : *functions) {
        auto it = loaded.find(fn.name + "/" + std::to_string(fn.params.size()));
        if (it == loaded.end()) continue;
        fn.out_params = it->second.Outs;
        fn.out_mask = it->second.OutMask;
        ++hits;
    }
    if (hits == 0) return false;

    database() = std::move(loaded);
    g_defs_bound = false;
    *applied = hits;
    logf("osiris: %zu signatures from %s, no walk needed", database().size(),
         path.c_str());
    return true;
}

void save_cached_signatures(char const* story) {
    const std::string path = cache_path(story);
    if (path.empty()) return;

    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        ::mkdir(path.substr(0, slash).c_str(), 0755);
    }

    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return;
    // First line: how many of these the story defines itself. Counted while
    // walking, and reported on a cached run too -- otherwise the line about
    // uncallable procedures appears on the first run and vanishes on the
    // second, which reads like something changed.
    std::fputs(kStoreFormat, f);
    std::fprintf(f, "# story %zu\n", g_story_functions);
    std::fprintf(f, "# typeat %lx\n", (unsigned long)type_offset());
    // The type aliases, which decide whether a stored value is a string.
    // Scanning for the engine's table costs a pass over every writable
    // mapping, so once is enough per story.
    if (g_alias_ready) {
        for (std::size_t i = 0; i < kTypeCount; ++i) {
            if (g_alias[i] == 0) continue;
            std::fprintf(f, "# alias %zu %u\n", i, g_alias[i]);
        }
    }
    for (auto const& entry : database()) {
        std::fprintf(f, "%s %d %llx", entry.first.c_str(), entry.second.Outs,
                     (unsigned long long)entry.second.OutMask);
        for (std::size_t i = 0; i < entry.second.Types.size(); ++i) {
            std::fprintf(f, "%s%u", i == 0 ? " " : ",",
                         (unsigned)entry.second.Types[i]);
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    logf("osiris: wrote %zu signatures to %s", database().size(),
         path.c_str());
}

std::size_t load_out_param_counts(std::vector<Function>* functions,
                                  char const* story, bool* cached) {
    const CacheLock lock(osiris_cache_lock());
    if (cached != nullptr) *cached = false;
    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);
    if (base == 0) {
        logf("osiris: libOsiris.so not found; cannot read signatures");
        return 0;
    }

    // The node and database lists are wanted either way, and finding them
    // is a handful of reads rather than a walk.
    find_node_db(base);
    find_database_db(base);
    find_string_table();

    g_cache_story = story != nullptr ? story : "";
    std::size_t fromStore = 0;
    if (load_cached_signatures(story, functions, &fromStore)) {
        if (cached != nullptr) *cached = true;
        // The cache carries the aliases too; only fall back to searching
        // for the engine's table if it did not.
        learn_aliases();
        read_story_header_types();
        return fromStore;
    }

    learn_aliases();
    read_story_header_types();

    std::uintptr_t holder = 0;
    if (!peek(base + function_db_holder(), &holder) || holder < 0x1000) {
        logf("osiris: function db holder unreadable");
        return 0;
    }

    // One-shot structural dump: the walk found nothing, so look at what is
    // actually there instead of guessing another offset.
    if (std::getenv("BG3LE_DUMP_DB") != nullptr) {
        logf("db: libOsiris base 0x%lx, holder 0x%lx", (unsigned long)base,
             (unsigned long)holder);

        std::uint32_t a = 0, b = 0;
        peek(holder + 0x5fe8, &a);
        peek(holder + 0xc018, &b);
        logf("db: counts at +0x5fe8=%u +0xc018=%u (1303 would confirm the holder)",
             a, b);

        int shown = 0;
        for (std::size_t i = 0; i < kBuckets && shown < 4; ++i) {
            std::uintptr_t w[3] = {};
            const std::uintptr_t slot = holder + 0x10 + i * kSlotStride;
            if (!peek(slot + 0x00, &w[0]) || w[0] == 0) continue;
            peek(slot + 0x08, &w[1]);
            peek(slot + 0x10, &w[2]);
            logf("db: bucket[%zu] @0x%lx = %016lx %016lx %016lx", i,
                 (unsigned long)slot, (unsigned long)w[0], (unsigned long)w[1],
                 (unsigned long)w[2]);

            for (int which = 0; which < 2; ++which) {
                const std::uintptr_t node = which == 0 ? w[0] : w[1];
                if (node < 0x1000) continue;
                std::uintptr_t q[10] = {};
                for (int k = 0; k < 10; ++k) peek(node + k * 8, &q[k]);
                logf("db:   node%d @0x%lx", which, (unsigned long)node);
                for (int k = 0; k < 10; ++k) {
                    char text[96];
                    const bool str = q[k] >= 0x1000 &&
                        safe_cstr(reinterpret_cast<const void*>(q[k]), text,
                                  sizeof(text)) && text[0] >= 0x20 && text[0] < 0x7f;
                    logf("db:     +%02d = %016lx%s%s", k * 8, (unsigned long)q[k],
                         str ? "  -> " : "", str ? text : "");
                }
            }
            ++shown;
        }
    }

    std::unordered_map<std::string, DbEntry>& by_name = database();
    by_name.clear();
    g_defs_bound = false;
    g_visited = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        const std::uintptr_t slot = holder + 0x10 + i * kSlotStride;
        std::unordered_set<std::uintptr_t> seen;
        for (std::uintptr_t off = 0x00; off <= 0x08; off += 0x08) {
            std::uintptr_t map = 0;
            if (!peek(slot + off, &map)) continue;
            visit_tree(map, &by_name, 0, &seen);
        }
    }

    // Functions the story never references have no entry here; those keep the
    // caller-decides fallback. bg3se has the same limitation -- it reports
    // "Attempted to call an unbound Osiris function" for them.
    std::size_t applied = 0;
    for (Function& fn : *functions) {
        auto it = by_name.find(fn.name + "/" + std::to_string(fn.params.size()));
        if (it != by_name.end()) {
            fn.out_params = it->second.Outs;
            fn.out_mask = it->second.OutMask;
            ++applied;
        }
    }

    // Where FunctionType and the key sit inside the Function object.
    // OsiFunctionDef puts a NodeRef between the signature pointer and
    // them, and its width decides the offset, so both candidates are
    // tried and the one whose key reproduces the engine's own function
    // ids wins. Story-defined functions -- the procedures and user
    // queries mods call -- are in this database but not in the engine's
    // mapping, and that handle is the only way to reach them.
    std::uintptr_t bestAt = 0;
    std::size_t bestHits = 0;
    for (std::uintptr_t at : kTypeCandidates) {
        std::size_t hits = 0;
        for (Function const& fn : *functions) {
            auto it = by_name.find(fn.name + "/"
                                   + std::to_string(fn.params.size()));
            if (it == by_name.end() || it->second.Def == 0) continue;
            std::uint32_t type = 0;
            std::uint32_t handle = 0;
            if (!read_type_and_handle(it->second.Def, at, &type, &handle)) {
                continue;
            }
            if (handle == fn.id) ++hits;
        }
        logf("osiris: FunctionType at +%#lx reproduces %zu of %zu ids",
             (unsigned long)at, hits, applied);
        if (hits > bestHits) {
            bestHits = hits;
            bestAt = at;
        }
    }
    type_offset() = bestHits > applied / 2 ? bestAt : 0;

    // How many of these the story defines itself, counted here so the cache
    // can carry it: story_functions() works it out again later, but the
    // cache is written before that runs.
    std::unordered_set<std::string> mapped;
    for (Function const& fn : *functions) {
        mapped.insert(fn.name + "/" + std::to_string(fn.params.size()));
    }
    g_story_functions = 0;
    for (auto const& entry : by_name) {
        if (mapped.count(entry.first) != 0 || entry.second.Def == 0) continue;
        std::uint32_t type = 0;
        std::uint32_t handle = 0;
        if (type_offset() == 0
            || !read_type_and_handle(entry.second.Def, type_offset(), &type,
                                     &handle)
            || handle == 0) {
            ++g_story_functions;
        }
    }

    save_cached_signatures(story);
    // One known signature, dumped, so the parameter type list can be read
    // off rather than guessed at. BG3LE_DUMP_SIG=1.
    if (std::getenv("BG3LE_DUMP_SIG") != nullptr) {
        for (Function const& fn : *functions) {
            if (fn.params.size() < 2) continue;
            // One with mixed parameter types, so the decoding can be
            // checked rather than merely fitted.
            bool mixed = false;
            for (std::uint8_t t : fn.params) {
                if (t != fn.params[0]) mixed = true;
            }
            if (!mixed) continue;
            auto it = by_name.find(fn.name + "/"
                                   + std::to_string(fn.params.size()));
            if (it == by_name.end() || it->second.Def == 0) continue;

            std::uintptr_t signature = 0;
            if (!peek(it->second.Def + 0x18, &signature)) continue;
            std::string types;
            for (std::uint8_t t : fn.params) {
                types += std::to_string((int)t) + " ";
            }
            logf("sig: %s/%zu id=%u def=0x%lx signature=0x%lx types: %s",
                 fn.name.c_str(), fn.params.size(), fn.id,
                 (unsigned long)it->second.Def, (unsigned long)signature,
                 types.c_str());
            for (std::size_t off = 0; off < 0x40; off += 8) {
                std::uintptr_t word = 0;
                if (!peek(signature + off, &word)) break;
                logf("sig:   +%02zx = %016lx", off, (unsigned long)word);
            }
            // The parameter list at +0x10: its own words, then the chain.
            std::uintptr_t list = 0;
            if (peek(signature + 0x10, &list) && list >= 0x1000) {
                for (std::size_t off = 0; off < 0x28; off += 8) {
                    std::uintptr_t word = 0;
                    if (!peek(list + off, &word)) break;
                    logf("sig: list+%02zx = %016lx", off,
                         (unsigned long)word);
                }
                std::uintptr_t node = 0;
                peek(list + 0x08, &node);
                for (int step = 0; step < 4 && node >= 0x1000; ++step) {
                    std::uintptr_t words[5] = {};
                    for (int k = 0; k < 5; ++k) peek(node + k * 8, &words[k]);
                    logf("sig: node%d @0x%lx = %016lx %016lx %016lx %016lx "
                         "%016lx", step, (unsigned long)node,
                         (unsigned long)words[0], (unsigned long)words[1],
                         (unsigned long)words[2], (unsigned long)words[3],
                         (unsigned long)words[4]);
                    node = words[0];
                }
            }

            // And the first words behind each pointer-looking field.
            for (std::size_t off = 0; off < 0x40; off += 8) {
                std::uintptr_t word = 0;
                if (!peek(signature + off, &word)) break;
                if (word < 0x10000 || word > 0x800000000000ull) continue;
                std::uintptr_t inner[4] = {};
                for (int k = 0; k < 4; ++k) peek(word + k * 8, &inner[k]);
                logf("sig:   [+%02zx] -> %016lx %016lx %016lx %016lx", off,
                     (unsigned long)inner[0], (unsigned long)inner[1],
                     (unsigned long)inner[2], (unsigned long)inner[3]);
            }
            break;
        }
    }

    // Does the decoded parameter list agree with the mapping the engine
    // already gave us? If it does for every function both sources know,
    // it can be trusted for the ones only the database knows.
    std::size_t typed = 0;
    std::size_t typedWrong = 0;
    for (Function const& fn : *functions) {
        auto it = by_name.find(fn.name + "/"
                               + std::to_string(fn.params.size()));
        if (it == by_name.end() || it->second.Types.empty()) continue;
        if (it->second.Types == fn.params) {
            ++typed;
        } else {
            ++typedWrong;
        }
    }
    logf("osiris: parameter types agree for %zu functions, differ for %zu",
         typed, typedWrong);

    // The database counts itself, so say whether the walk agrees with it.
    // This is the check that was missing: every entry the old walk found
    // was correct, it simply found a fifth of them, and nothing in the
    // output could have told you.
    std::uint32_t declared = 0;
    peek(holder + 0x5fe8, &declared);
    if (declared > 0) {
        logf("osiris: signature walk visited %zu nodes and found %zu of the "
             "%u entries the database says it holds, matching %zu of %zu "
             "functions the engine maps",
             g_visited, by_name.size(), declared, applied, functions->size());
    } else {
        logf("osiris: signature walk visited %zu nodes, found %zu entries, "
             "matched %zu of %zu functions", g_visited, by_name.size(),
             applied, functions->size());
    }
    return applied;
}

// Every function the database names, whether or not it can be called.
//
// story_functions returns only the ones reachable through the engine's
// dispatch, because its caller binds them. This is for describing them: a
// procedure and a user query have no dispatch handle and are still what a mod
// author wants annotations for, so the kind comes from the function object and
// the id is left at nought when there is none.
std::vector<Function> all_functions() {
    const CacheLock lock(osiris_cache_lock());

    std::vector<Function> out;
    if (!bind_defs()) return out;
    out.reserve(database().size());

    for (auto const& entry : database()) {
        if (entry.second.Def == 0) continue;

        const std::size_t slash = entry.first.rfind('/');
        if (slash == std::string::npos || slash == 0) continue;
        const std::size_t arity = (std::size_t)std::strtoul(
            entry.first.c_str() + slash + 1, nullptr, 10);
        if (entry.second.Types.size() != arity) continue;

        std::uint32_t type = 0;
        std::uint32_t handle = 0;
        if (type_offset() != 0) {
            read_type_and_handle(entry.second.Def, type_offset(), &type,
                                 &handle);
        }

        Function fn;
        fn.name = entry.first.substr(0, slash);
        // The kind lives in the low three bits of the id, and a function with
        // no dispatch handle still has a kind: carry it in an id of its own so
        // Function::kind() answers either way.
        fn.id = handle != 0 ? handle : (type & 7u);
        fn.params = entry.second.Types;
        fn.out_params = entry.second.Outs;
        fn.out_mask = entry.second.OutMask;
        out.push_back(std::move(fn));
    }

    std::sort(out.begin(), out.end(), [](Function const& a, Function const& b) {
        if (a.name != b.name) return a.name < b.name;
        return a.params.size() < b.params.size();
    });
    return out;
}

std::vector<Function> story_functions(std::vector<Function> const& known) {
    const CacheLock lock(osiris_cache_lock());
    std::unordered_set<std::string> seen;
    for (Function const& fn : known) {
        seen.insert(fn.name + "/" + std::to_string(fn.params.size()));
    }

    std::vector<Function> out;
    std::size_t already = 0;
    std::size_t noId = 0;
    std::size_t kinds[9] = {};
    for (auto const& entry : database()) {
        if (seen.count(entry.first) != 0) {
            ++already;
            continue;
        }
        if (entry.second.Def == 0) continue;

        // "Name/Arity" is the database's key; the arity is checked against
        // the parameter list rather than trusted.
        const std::size_t slash = entry.first.rfind('/');
        if (slash == std::string::npos || slash == 0) continue;
        const std::string name = entry.first.substr(0, slash);
        const std::size_t arity = (std::size_t)std::strtoul(
            entry.first.c_str() + slash + 1, nullptr, 10);
        if (entry.second.Types.size() != arity) continue;

        std::uint32_t type = 0;
        std::uint32_t handle = 0;
        if (type_offset() == 0
            || !read_type_and_handle(entry.second.Def, type_offset(), &type,
                                     &handle)
            || handle == 0) {
            ++noId;
            continue;
        }

        Function fn;
        fn.name = name;
        fn.id = handle;
        fn.params = entry.second.Types;
        fn.out_params = entry.second.Outs;
        fn.out_mask = entry.second.OutMask;
        kinds[type < 9 ? type : 0]++;
        if (type == kProc && kinds[kProc] <= 3) {
            logf("osiris: procedure %s/%zu handle=%#x", fn.name.c_str(),
                 fn.params.size(), fn.id);
        }
        if (type == kEvent) continue;  // the game raises these
        out.push_back(std::move(fn));
    }
    // The ones with no handle are the story's own: a procedure, a
    // database and a user query carry an empty key, because they are not
    // called through the engine's dispatch at all -- Osiris runs them by
    // inserting a tuple into their node. Reaching those needs the node
    // list, which bg3le does not have yet; until then they are counted
    // rather than bound, so nothing claims to call what it cannot.
    // How many of the story's own functions can be reached through their
    // node, which is what calling them will need. Checked by agreement:
    // the node has to name the same id and point back at the same
    // function.
    std::size_t withNodes = 0;
    if (type_offset() != 0) {
        for (auto const& entry : database()) {
            if (entry.second.Def == 0) continue;
            if (seen.count(entry.first) != 0) continue;
            if (node_for(entry.second.Def) != 0) ++withNodes;
        }
        logf("osiris: %zu of the story's functions resolve to a node that "
             "agrees about its id and its function", withNodes);
    }

    // Databases that hold facts, with one fact each, under
    // BG3LE_DUMP_DBFACTS=1. This is what established the layout below, and
    // what will confirm it again after a game patch.
    if (std::getenv("BG3LE_DUMP_DBFACTS") != nullptr) dump_database_facts();

    // On a cached run the entries carry no Function pointer, so nothing
    // reaches the handle check and noId counts nothing: the count comes
    // from the cache instead. Overwriting it with zero made the line say
    // something different on the second run than on the first.
    if (noId != 0) g_story_functions = noId;
    logf("osiris: database holds %zu entries: %zu the engine already maps, "
         "%zu callable through the dispatch, %zu the story defines itself "
         "(reached by tuple insert, resolved on first use)",
         database().size(), already, out.size(), g_story_functions);
    return out;
}

namespace {

// ---------------------------------------------------------------------------
// Running a story-defined function.
//
// A procedure a mod calls carries no dispatch handle, so the DIV boundary
// cannot reach it: the story's own functions are run by putting a tuple
// into the Rete node that stands for them. bg3se does the same thing --
// node->InsertTuple(&tuple) -- but its slot number and its structure
// layouts are MSVC's, and neither transfers. Both came from the engine's
// own code instead: COsiris::Event is exported, and raising an event is
// exactly this operation.
//
// What that function does, in order:
//
//   1. builds a COsipParameterList on the stack from its COsiArgumentDesc
//   2. finds the OsiFunctionDef by id in the function database
//   3. reads the node id from the def, takes the node out of the node
//      list, and calls the node's vtable slot at +0x68 with the list
//   4. destroys the list
//
// So +0x68 is the slot, and it is not +0x50: the Itanium ABI spends two
// vtable slots on destructors where MSVC spends one, and this Osiris has
// virtuals bg3se's list does not. Guessing from bg3se's offset would have
// called PushDownTupleDelete.
//
// The structures, read off the same two functions:
//
//   COsipParameterList  +0x00 vtable
//                       +0x08 last node, and the sentinel's own address
//                       +0x10 first node
//                       +0x18 count
//   node                +0x00 prev, +0x08 next, +0x10 TypedValue*
//   TypedValue          +0x00 value, +0x08 type, +0x0a index, +0x0b flags
//
// The sentinel is the +0x08 field itself: both pointers start out holding
// its address, so the first insertion writes the first node through what
// is nominally the sentinel's next pointer, which is the +0x10 field. The
// engine's six stores are mirrored exactly rather than reasoned about.
//
// Nothing here is allocated from Osiris' allocator, unlike the engine's
// own path: the tuple lives for one call and the values are copied by
// whatever consumes them -- which is why bg3se can free its own straight
// after. Interned strings are released afterwards all the same.
constexpr std::uintptr_t kInsertTuple = 0x68;

// And retracting one is +0x70, from the same standard of evidence: the
// rule-action dispatcher branches on the action's insert/delete flag --
// `cmpb $0x0,0x10(%r13)` -- and the two arms log " [add fact]" and
// " [delete fact]" before calling +0x68 and +0x70 respectively, both with
// the same node and the same parameter list. bg3se's spacing would have
// suggested +0x78.
constexpr std::uintptr_t kDeleteTuple = 0x70;

struct TupleNode;

struct alignas(8) TypedValueRec {
    std::uint64_t Value = 0;
    std::uint16_t Type = 0;
    std::int8_t Index = -1;
    std::uint8_t Flags = 0x02;  // TypedValue, as the engine initialises it
    std::uint32_t Unused = 0;
};
static_assert(sizeof(TypedValueRec) == 16, "TypedValue is 16 bytes");

struct TupleNode {
    TupleNode* Prev = nullptr;
    TupleNode* Next = nullptr;
    TypedValueRec* Item = nullptr;
};
static_assert(sizeof(TupleNode) == 24, "tuple node is 24 bytes");

struct ParameterList {
    void* Vtable = nullptr;
    TupleNode* Last = nullptr;
    TupleNode* First = nullptr;
    std::uint64_t Count = 0;

    // The +0x08 field doubles as the head sentinel.
    TupleNode* sentinel() { return reinterpret_cast<TupleNode*>(&Last); }

    void init(void* vtable) {
        Vtable = vtable;
        Last = sentinel();
        First = sentinel();
        Count = 0;
    }

    void append(TupleNode* node, TypedValueRec* value) {
        node->Prev = nullptr;
        node->Item = value;
        node->Next = sentinel();

        TupleNode* prev = Last;
        node->Prev = prev;
        prev->Next = node;
        Last = node;
        ++Count;
    }
};
static_assert(sizeof(ParameterList) == 32, "parameter list is 32 bytes");

// The mangled name of a class, from its vtable: one slot back is the
// typeinfo, and the typeinfo's second field is the name. Used to check
// that a node is the class its function claims, rather than trusting an
// offset.
std::string class_of(std::uintptr_t vtable) {
    // Nine node classes share a hundred and fifty thousand nodes, so the
    // answer is kept: asking once per function cost four reads each and
    // showed up in the level load.
    static std::unordered_map<std::uintptr_t, std::string> known;
    const CacheLock lock(osiris_cache_lock());
    auto cached = known.find(vtable);
    if (cached != known.end()) return cached->second;

    std::uintptr_t info = 0;
    std::uintptr_t name = 0;
    std::string out;
    if (peek(vtable - 8, &info) && info >= 0x1000 && peek(info + 8, &name)
        && name >= 0x1000) {
        record_text(name, &out);
    }

    known.emplace(vtable, out);
    return out;
}

// libOsiris' whole mapped image, for searching inside it.
struct Image {
    std::uintptr_t Begin = 0;
    std::uintptr_t End = 0;
};

int find_osiris_image(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name == nullptr) return 0;
    if (std::strstr(info->dlpi_name, "libOsiris.so") == nullptr) return 0;

    auto* out = static_cast<Image*>(data);
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        ElfW(Phdr) const& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        const std::uintptr_t begin = info->dlpi_addr + ph.p_vaddr;
        if (out->Begin == 0 || begin < out->Begin) out->Begin = begin;
        if (begin + ph.p_memsz > out->End) out->End = begin + ph.p_memsz;
    }
    return 1;
}

// Every qword in libOsiris' image that equals `wanted`.
std::vector<std::uintptr_t> image_pointers_to(Image const& image,
                                              std::uintptr_t wanted,
                                              std::size_t limit) {
    std::vector<std::uintptr_t> out;
    for (std::uintptr_t at = image.Begin; at + 8 <= image.End && out.size() < limit;
         at += 8) {
        std::uintptr_t word = 0;
        if (peek(at, &word) && word == wanted) out.push_back(at);
    }
    return out;
}

// The vtable of a class named in the image, found by content: the mangled
// name is a string in the image, the typeinfo points at it eight bytes
// in, and the vtable points at the typeinfo eight bytes back. Used for
// the tuple class, whose vtable the engine puts in every list it builds.
std::uintptr_t vtable_named(char const* mangled) {
    Image image;
    ::dl_iterate_phdr(find_osiris_image, &image);
    if (image.Begin == 0) return 0;

    const std::size_t length = std::strlen(mangled);
    std::uintptr_t name = 0;
    for (std::uintptr_t at = image.Begin; at + length + 1 <= image.End; ++at) {
        char window[64] = {};
        if (length + 1 > sizeof(window)) return 0;
        if (!peek(at, &window)) {
            at += 0xfff;  // an unmapped hole; skip past it
            continue;
        }
        if (std::memcmp(window, mangled, length + 1) == 0) {
            name = at;
            break;
        }
    }
    if (name == 0) return 0;

    for (std::uintptr_t info : image_pointers_to(image, name, 8)) {
        // The name sits at typeinfo+8.
        for (std::uintptr_t vtable : image_pointers_to(image, info - 8, 8)) {
            return vtable + 8;
        }
    }
    return 0;
}

std::uintptr_t tuple_vtable() {
    static std::uintptr_t found = vtable_named("18COsipParameterList");
    return found;
}

using InsertProc = void (*)(void*, void*);
using RemoveStrProc = void (*)(void*, std::uint64_t);

RemoveStrProc remove_str() {
    static RemoveStrProc proc = reinterpret_cast<RemoveStrProc>(
        ::dlsym(RTLD_DEFAULT, "_ZN15COsiStringTable9RemoveStrE16COsiStringHandle"));
    return proc;
}

void release_string(std::uint64_t handle) {
    if (table_now() == 0 || (handle & kStringIndexMask) == 0) return;

    RemoveStrProc proc = remove_str();
    if (proc != nullptr) proc(reinterpret_cast<void*>(g_strings.Table), handle);
}

// Is this really a function, rather than one of the many slots that are a
// bare `ret` or a constant? A wrong slot number is the one mistake here
// that takes the game down, so the target is looked at before it is
// called.
bool plausible_method(std::uintptr_t at) {
    unsigned char head[4] = {};
    if (!peek(at, &head)) return false;

    if (head[0] == 0xc3) return false;                       // ret
    if (head[0] == 0xb0 && head[2] == 0xc3) return false;    // mov $x,%al; ret
    if (head[0] == 0x31 && head[1] == 0xc0) return false;    // xor %eax,%eax
    return true;
}

// The exported COsiris::Event raises an event by inserting into its node:
// it must still call [reg+kInsertTuple]. Delete, the next slot, is only
// called from an unexported dispatcher, so it rests on the class checks.
bool tuple_slots_ok() {
    static const bool ok = [] {
        const auto event = osiris_symbol("_ZN7COsiris5EventEjP16COsiArgumentDesc");
        unsigned char code[0x800] = {};
        const std::size_t n = event == 0 ? 0 : safe_read_some((void const*)event, code, sizeof(code));
        for (std::size_t i = 0; i + 3 <= n; ++i) {
            if (code[i] == 0xcc && code[i + 1] == 0xcc) break;
            const unsigned char modrm = code[i + 1];
            if (code[i] == 0xff && (modrm & 0xf8) == 0x50 && modrm != 0x54 && code[i + 2] == kInsertTuple)
                return true;  // call *disp8(%reg)
        }
        statusf("WARNING: osiris: COsiris::Event no longer calls slot %#lx; tuple inserts are off",
                (unsigned long)kInsertTuple);
        return false;
    }();
    return ok;
}

// One argument as the TypedValue Osiris wants for its declared type.
// Strings are interned, and the handles collected for the caller to release.
bool encode_arg(char const* key, std::size_t i, std::uint16_t declared,
                Value const& arg, TypedValueRec* out,
                std::vector<std::uint64_t>* interned, std::string* why) {
    TypedValueRec& value = *out;
    // kNone is how the caller says nil, which on a retract means
    // "any value in this column". The engine wants a cleared value
    // for that -- no type and IsValid off -- not an absent one.
    if (arg.type == kNone) {
        value.Type = kNone;
        value.Flags = 0x02;
        return true;
    }

    value.Type = declared;
    value.Flags = 0x02 | 0x08;  // TypedValue | IsValid

    // An aliased type that appears in no fact anywhere could not be
    // learned, so what the caller passed decides. Better than writing
    // a zero, which is what a wrong guess produces and what sends a
    // mod author looking in the wrong place.
    std::uint16_t base = resolve_alias(declared);
    if (!type_known(declared)) {
        base = arg.type == kString
                   ? (guid_looking(arg.text) ? kTypeGuidString
                                                 : kTypeString)
                   : arg.type;
    } else if ((base == kTypeString || base == kTypeGuidString)
               != (arg.type == kString)) {
        if (why != nullptr) {
            *why = std::string("argument ") + std::to_string(i + 1)
                   + " of " + key + " is declared type "
                   + std::to_string(declared) + ", which wants a "
                   + ((base == kTypeString || base == kTypeGuidString)
                          ? "string" : "number");
        }
        return false;
    }

    switch (base) {
    case kTypeString:
    case kTypeGuidString: {
        const std::uint64_t handle = intern_string(
            arg.text.c_str(), base == kTypeGuidString);
        if (handle == 0 && !arg.text.empty()) {
            if (why != nullptr) {
                *why = std::string("argument ") + std::to_string(i + 1) + " of " + key
                       + " could not be interned";
            }
            return false;
        }
        if (handle != 0) interned->push_back(handle);
        value.Value = handle;
        break;
    }
    case kReal: {
        const float real = (float)arg.real;
        std::memcpy(&value.Value, &real, sizeof(real));
        break;
    }
    case kInteger64:
        value.Value = (std::uint64_t)arg.integer;
        break;
    default: {
        const std::int32_t narrow = (std::int32_t)arg.integer;
        std::memcpy(&value.Value, &narrow, sizeof(narrow));
        break;
    }
    }

    return true;
}

// A TypedValue read back as the type it was declared with.
Value decode_value(std::uint64_t raw, std::uint16_t type) {
    Value value;
    value.type = type;
    if (!type_known(type)) return value_by_shape(raw);
    switch (resolve_alias(type)) {
    case kTypeString:
    case kTypeGuidString:
        value.type = kString;
        string_of(raw, &value.text);
        break;
    case kReal: {
        float real = 0.f;
        std::memcpy(&real, &raw, sizeof(real));
        value.type = kReal;
        value.real = real;
        break;
    }
    case kInteger64:
        value.type = kInteger64;
        value.integer = (std::int64_t)raw;
        break;
    default:
        value.type = kInteger;
        value.integer = (std::int32_t)(std::uint32_t)raw;
        break;
    }
    return value;
}

// Put a tuple into a story function's node, which is what running it
// means. `args` are already in the function's declared order.
Status insert_tuple(char const* key, std::vector<Value> const& args,
                    std::uintptr_t slot, std::string* why) {
    auto fail = [why](char const* text) {
        if (why != nullptr) *why = text;
        return Status::kUnavailable;
    };

    if (!bind_defs()) return fail("Osiris' function database is unreadable");
    if (!find_string_table()) return fail("Osiris' string pool was not found");

    auto entry = database().find(key);
    if (entry == database().end() || entry->second.Def == 0) {
        return fail("no such story function");
    }
    if (entry->second.Types.size() != args.size()) {
        if (why != nullptr) {
            *why = "expected " + std::to_string(entry->second.Types.size())
                   + " arguments, got " + std::to_string(args.size());
        }
        return Status::kUnavailable;
    }

    const std::uintptr_t node = node_for(entry->second.Def);
    if (node == 0) return fail("the function has no node");

    std::uintptr_t vtable = 0;
    if (!peek(node, &vtable) || vtable < 0x1000) return fail("node has no vtable");

    // Only the two classes that hold tuples: a procedure or event node,
    // and a database node. Anything else is a rule or a condition, and
    // inserting into one is not what a caller means.
    const std::string cls = class_of(vtable);
    if (cls != "10CReteEvent" && cls != "9CReteFact") {
        if (why != nullptr) *why = "node is a " + cls + ", which holds no tuple";
        return Status::kUnavailable;
    }

    std::uintptr_t target = 0;
    if (!tuple_slots_ok() || !peek(vtable + slot, &target) || !plausible_method(target)) {
        return fail("the node's tuple slot does not hold a function");
    }

    void* listVtable = reinterpret_cast<void*>(tuple_vtable());
    if (listVtable == nullptr) return fail("the tuple class was not found");

    // Everything the call needs lives on the stack for the duration.
    std::vector<TypedValueRec> values(args.size());
    std::vector<TupleNode> nodes(args.size());
    std::vector<std::uint64_t> interned;

    ParameterList list;
    list.init(listVtable);

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (!encode_arg(key, i, entry->second.Types[i], args[i], &values[i],
                        &interned, why)) {
            for (std::uint64_t held : interned) release_string(held);
            return Status::kUnavailable;
        }
        list.append(&nodes[i], &values[i]);
    }

    reinterpret_cast<InsertProc>(target)(reinterpret_cast<void*>(node), &list);

    // The engine copies what it keeps, so the references taken above are
    // ours to give back.
    for (std::uint64_t held : interned) release_string(held);
    return Status::kHandled;
}
// ---------------------------------------------------------------------------
// Calling a user query: QRY_Foo(...).
//
// Upstream's OsiUserQuery, with the layouts read off this libOsiris:
// CReteOsiQuery and CReteFact both implement IsValid at vtable +0x28 as
// IsValid(SmallTuple*, adapterId), the SmallTuple being eight inline
// TypedValues (or a pointer to more) then size at +0x80 and capacity at
// +0x84. The adapter maps tuple columns to the node's variables; an
// identity one, with no constants, passes the tuple through as it is.
constexpr std::uintptr_t kIsValid = 0x28;

// Inside an Adapter: libc++'s std::map<u8,u8> VarToColumnMaps at +0x08
// (its size at +0x18), vector<int8> ColumnToVarMaps at +0x20, and the
// constants SmallTuple at +0x38.
constexpr std::uintptr_t kAdapterMapSize = 0x18;
constexpr std::uintptr_t kAdapterColumns = 0x20;
constexpr std::uintptr_t kAdapterConstantsSize = 0x38 + 0x80;

struct SmallTupleRec {
    union {
        TypedValueRec Inline[8];
        TypedValueRec* Values;
    };
    std::uint32_t Size = 0;
    std::uint32_t Capacity = 8;

    SmallTupleRec() : Inline{} {}
    TypedValueRec* data() { return Capacity > 8 ? Values : Inline; }
};
static_assert(sizeof(SmallTupleRec) == 0x88, "SmallTuple is 0x88 bytes");

using IsValidProc = bool (*)(void*, SmallTupleRec*, std::uint32_t);

// The identity adapter for each column count, per adapter list.
std::uint32_t identity_adapter(std::size_t columns) {
    static std::uintptr_t cachedFor = 0;
    static std::uint32_t cachedSize = 0;
    static std::unordered_map<std::size_t, std::uint32_t> ids;

    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);
    std::uintptr_t db = 0;
    std::uint32_t size = 0;
    std::uintptr_t begin = 0;
    if (base == 0 || !peek(base + osiris_globals().AdapterDb, &db) || db < 0x1000
        || !peek(db, &size) || !peek(db + 0x08, &begin) || begin < 0x1000
        || size == 0 || size > (1u << 20)) {
        return 0;
    }

    if (db != cachedFor || size != cachedSize) {
        ids.clear();
        cachedFor = db;
        cachedSize = size;
        for (std::uint32_t i = 0; i < size; ++i) {
            std::uintptr_t adapter = 0;
            std::uint32_t id = 0;
            if (!peek(begin + (std::uintptr_t)i * 8, &adapter) || adapter < 0x1000
                || !peek(adapter, &id) || id != i + 1) {
                continue;
            }
            std::uint32_t constants = 0;
            std::uint64_t mapped = 0;
            std::uintptr_t first = 0;
            std::uintptr_t last = 0;
            if (!peek(adapter + kAdapterConstantsSize, &constants) || constants != 0
                || !peek(adapter + kAdapterMapSize, &mapped)
                || !peek(adapter + kAdapterColumns, &first)
                || !peek(adapter + kAdapterColumns + 8, &last)) {
                continue;
            }
            // The vector can be wider than the mapping; upstream checks
            // only the mapped columns.
            const std::size_t width = (std::size_t)mapped;
            if (last - first < width || width > kMaxParams || ids.count(width) != 0) {
                continue;
            }
            std::int8_t map[kMaxParams] = {};
            if (width > 0 && !safe_read(reinterpret_cast<void const*>(first), map, width)) {
                continue;
            }
            bool identity = true;
            for (std::size_t c = 0; c < width && identity; ++c) {
                identity = map[c] == (std::int8_t)c;
            }
            if (identity) ids.emplace(width, id);
        }
        logf("osiris: %zu identity adapters among %u", ids.size(), size);
    }

    auto found = ids.find(columns);
    return found == ids.end() ? 0 : found->second;
}

// Which of a function's parameters are outputs, from its signature's
// bitmask, MSB-first as upstream's isOutParam reads it.
bool out_mask(std::uintptr_t def, std::size_t count, std::vector<bool>* out) {
    std::uintptr_t signature = 0;
    std::uintptr_t bits = 0;
    std::uint32_t bytes = 0;
    if (!peek(def + 0x18, &signature) || signature < 0x1000
        || !peek(signature + 0x18, &bits) || !peek(signature + 0x20, &bytes)) {
        return false;
    }
    out->assign(count, false);
    if (bits == 0 || bytes == 0) return true;
    unsigned char mask[64] = {};
    if (bytes > sizeof(mask)
        || !safe_read(reinterpret_cast<void const*>(bits), mask, bytes)) {
        return false;
    }
    for (std::size_t i = 0; i < count && i / 8 < bytes; ++i) {
        (*out)[i] = (mask[i / 8] & (0x80 >> (i % 8))) != 0;
    }
    return true;
}

// The user query declared with this many IN parameters.
DbEntry const* user_query_entry(char const* name, std::size_t inputs,
                                std::uintptr_t* node, std::vector<bool>* outs) {
    const std::string prefix = std::string(name) + "/";
    for (auto const& entry : database()) {
        if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
        if (entry.second.Def == 0) continue;
        const std::uintptr_t n = node_for(entry.second.Def);
        std::uintptr_t vtable = 0;
        if (n == 0 || !peek(n, &vtable) || class_of(vtable) != "13CReteOsiQuery") {
            continue;
        }
        std::vector<bool> mask;
        if (!out_mask(entry.second.Def, entry.second.Types.size(), &mask)) continue;
        std::size_t ins = 0;
        for (bool o : mask) ins += o ? 0 : 1;
        if (ins != inputs) continue;
        *node = n;
        *outs = std::move(mask);
        return &entry.second;
    }
    return nullptr;
}

}  // namespace

std::vector<Signature> signatures(char const* name) {
    std::vector<Signature> out;
    const CacheLock lock(osiris_cache_lock());
    if (name == nullptr || !bind_defs() || type_offset() == 0) return out;
    const std::string prefix = std::string(name) + "/";
    for (auto const& entry : database()) {
        if (entry.first.compare(0, prefix.size(), prefix) != 0 || entry.second.Def == 0) continue;
        std::uint32_t type = 0;
        std::uint32_t handle = 0;
        if (!read_type_and_handle(entry.second.Def, type_offset(), &type, &handle)) continue;
        Signature sig;
        sig.arity = (std::uint32_t)entry.second.Types.size();
        sig.type = type;
        sig.inputs = sig.arity - (std::uint32_t)std::max(entry.second.Outs, 0);
        out.push_back(sig);
    }
    std::sort(out.begin(), out.end(),
              [](Signature const& a, Signature const& b) { return a.arity < b.arity; });
    return out;
}

bool declared_inputs(char const* name, std::size_t inputs, std::vector<std::uint16_t>* types) {
    const CacheLock lock(osiris_cache_lock());
    if (name == nullptr || !bind_defs()) return false;
    const std::string prefix = std::string(name) + "/";
    for (auto const& entry : database()) {
        if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
        const std::size_t arity = (std::size_t)std::strtoul(entry.first.c_str() + prefix.size(), nullptr, 10);
        const std::size_t outs = (std::size_t)std::max(entry.second.Outs, 0);
        if (outs > arity || arity - outs != inputs) continue;
        types->clear();
        const bool typed = entry.second.Types.size() == arity;
        for (std::size_t i = 0; i < arity; ++i) {
            if (i < 64 && ((entry.second.OutMask >> i) & 1) != 0) continue;
            types->push_back(typed ? resolve_alias(entry.second.Types[i]) : 0);
        }
        return true;
    }
    return false;
}

Status query(char const* name, std::vector<Value> const& inputs,
             std::vector<Value>* outputs, std::string* why) {
    const CacheLock lock(osiris_cache_lock());
    auto fail = [why](char const* text) {
        if (why != nullptr) *why = text;
        return Status::kUnavailable;
    };
    if (!bind_defs()) return fail("Osiris' function database is unreadable");
    if (!find_string_table()) return fail("Osiris' string pool was not found");

    std::uintptr_t node = 0;
    std::vector<bool> outs;
    DbEntry const* entry = user_query_entry(name, inputs.size(), &node, &outs);
    if (entry == nullptr) {
        return fail("the story declares no user query with that many IN arguments");
    }
    const std::size_t n = entry->Types.size();
    if (n > kMaxParams) return fail("too many parameters");

    const std::uint32_t adapter = identity_adapter(n);
    if (adapter == 0) return fail("Osiris has no identity adapter for that many columns");

    std::uintptr_t vtable = 0;
    std::uintptr_t target = 0;
    if (!peek(node, &vtable) || !peek(vtable + kIsValid, &target)
        || !plausible_method(target)) {
        return fail("the node's IsValid slot does not hold a function");
    }

    SmallTupleRec tuple;
    std::vector<TypedValueRec> heap;
    if (n > 8) {
        heap.resize(n);
        tuple.Values = heap.data();
        tuple.Capacity = (std::uint32_t)n;
    }
    TypedValueRec* values = tuple.data();
    std::vector<std::uint64_t> interned;
    std::size_t in = 0;
    for (std::size_t i = 0; i < n; ++i) {
        values[i] = TypedValueRec{};
        if (!outs[i]
            && !encode_arg(name, i, entry->Types[i], inputs[in++], &values[i],
                           &interned, why)) {
            for (std::uint64_t held : interned) release_string(held);
            return Status::kUnavailable;
        }
        values[i].Index = (std::int8_t)i;
    }
    tuple.Size = (std::uint32_t)n;

    const bool valid = reinterpret_cast<IsValidProc>(target)(
        reinterpret_cast<void*>(node), &tuple, adapter);

    for (std::size_t i = 0; i < n; ++i) {
        if (!outs[i]) continue;
        TypedValueRec const& v = values[i];
        const bool has = valid && (v.Flags & 0x08) != 0;
        const std::uint16_t type = v.Type != kNone ? v.Type : entry->Types[i];
        if (outputs != nullptr) outputs->push_back(has ? decode_value(v.Value, type) : Value{});
        // The engine's copy into the tuple took a reference, as upstream's
        // ~TypedValue gives back.
        if (has) {
            const std::uint16_t base = resolve_alias(type);
            if (base == kTypeString || base == kTypeGuidString) release_string(v.Value);
        }
    }
    for (std::uint64_t held : interned) release_string(held);
    return valid ? Status::kHandled : Status::kRejected;
}

namespace {
// ---------------------------------------------------------------------------
// Watching the story: Ext.Osiris.RegisterListener.
//
// A listener fires when a procedure runs, an event is raised, or a fact
// goes into or out of a database -- which are all the same operation, the
// tuple insert above. So the two slots that operation goes through are
// replaced in the two node classes that use them, and the replacement
// fires the listeners and calls what was there.
//
// bg3se does exactly this (NodeHooks.cpp patches InsertTuple and
// DeleteTuple for the Database and Proc classes), and the same two slot
// numbers this file already established apply. The vtables belong to
// libOsiris rather than to the executable, so there is no link-time
// offset to check the slot against; what stands in for that check is the
// class name behind the vtable, read from its typeinfo.
//
// Installed only once a mod actually registers a listener. Until then
// every node keeps the engine's own pointers.
using NodeTupleProc = void (*)(void*, void*);

struct Hooked {
    NodeTupleProc Insert = nullptr;
    NodeTupleProc Delete = nullptr;
};

std::unordered_map<std::uintptr_t, Hooked> g_hooked;  // by vtable
TriggerFn g_trigger = nullptr;
bool g_watching = false;

// Def pointer -> "name/arity", so a node can be named without searching.
std::unordered_map<std::uintptr_t, std::string>& def_names() {
    static std::unordered_map<std::uintptr_t, std::string> names;
    return names;
}

// The values in a parameter list the engine is about to act on.
std::vector<Value> tuple_values(void* tuple) {
    std::vector<Value> out;
    auto* list = static_cast<ParameterList*>(tuple);
    if (list == nullptr) return out;

    TupleNode const* sentinel = reinterpret_cast<TupleNode const*>(&list->Last);
    TupleNode const* at = list->First;
    for (std::size_t i = 0; i < kMaxParams && at != nullptr && at != sentinel;
         ++i) {
        TypedValueRec record{};
        if (!peek(reinterpret_cast<std::uintptr_t>(at->Item), &record)) break;

        if (!type_known(record.Type)) {
            out.push_back(value_by_shape(record.Value));
            at = at->Next;
            continue;
        }

        Value value;
        value.type = record.Type;
        switch (resolve_alias(record.Type)) {
        case kTypeString:
        case kTypeGuidString:
            value.type = kString;
            string_of(record.Value, &value.text);
            break;
        case kReal: {
            float real = 0.f;
            std::memcpy(&real, &record.Value, sizeof(real));
            value.type = kReal;
            value.real = real;
            break;
        }
        case kInteger64:
            value.type = kInteger64;
            value.integer = (std::int64_t)record.Value;
            break;
        default:
            value.type = kInteger;
            value.integer = (std::int32_t)(std::uint32_t)record.Value;
            break;
        }
        out.push_back(std::move(value));
        at = at->Next;
    }
    return out;
}

// What the node stands for, as "name/arity", or empty if it is not one
// bg3le named.
//
// By value, not by pointer into the map: this runs on whichever engine thread
// inserted the tuple, and the map is built on the server thread.
std::string node_name(void* node) {
    std::uintptr_t def = 0;
    if (!peek(reinterpret_cast<std::uintptr_t>(node) + 0x10, &def)
        || def == 0) {
        return {};
    }

    const CacheLock lock(osiris_cache_lock());
    auto found = def_names().find(def);
    return found == def_names().end() ? std::string{} : found->second;
}

void fire(void* node, void* tuple, char const* before, char const* after,
          NodeTupleProc original) {
    const std::string key = node_name(node);
    if (key.empty() || g_trigger == nullptr) {
        if (original != nullptr) original(node, tuple);
        return;
    }

    const std::size_t slash = key.rfind('/');
    const std::string name = key.substr(0, slash);
    const std::size_t arity =
        (std::size_t)std::strtoul(key.c_str() + slash + 1, nullptr, 10);

    const std::vector<Value> values = tuple_values(tuple);
    g_trigger(name.c_str(), arity, before, values);
    if (original != nullptr) original(node, tuple);
    g_trigger(name.c_str(), arity, after, values);
}

// Two function pointers, copied out rather than pointed at, for the reason
// node_name is.
Hooked hooked_for(void* node) {
    std::uintptr_t vtable = 0;
    if (!peek(reinterpret_cast<std::uintptr_t>(node), &vtable)) return {};

    const CacheLock lock(osiris_cache_lock());
    auto found = g_hooked.find(vtable);
    return found == g_hooked.end() ? Hooked{} : found->second;
}

void watched_insert(void* node, void* tuple) {
    fire(node, tuple, "before", "after", hooked_for(node).Insert);
}

void watched_delete(void* node, void* tuple) {
    fire(node, tuple, "beforeDelete", "afterDelete", hooked_for(node).Delete);
}

bool install_node_hooks() {
    if (g_nodes.First == 0 || !bind_defs()) return false;

    // Naming a node needs the reverse of the function database. Rebuilt on
    // every call: a story reload gives every def a new address, and the hooks
    // stay installed across it.
    {
        const CacheLock lock(osiris_cache_lock());
        def_names().clear();
        for (auto const& entry : database()) {
            if (entry.second.Def != 0) def_names()[entry.second.Def] = entry.first;
        }
    }
    if (g_watching) return true;

    // The two classes that hold tuples, found from the nodes that use
    // them rather than from an address written down here.
    std::unordered_map<std::uintptr_t, std::string> classes;
    for (std::uint32_t i = 0; i < g_nodes.Count && classes.size() < 2; ++i) {
        std::uintptr_t node = 0;
        if (!peek(g_nodes.First + (std::uintptr_t)i * 8, &node)
            || node < 0x1000) {
            continue;
        }
        std::uintptr_t vtable = 0;
        if (!peek(node, &vtable) || vtable < 0x1000) continue;

        const std::string cls = class_of(vtable);
        if (cls != "10CReteEvent" && cls != "9CReteFact") continue;
        classes[vtable] = cls;
    }
    if (classes.size() != 2) {
        logf("osiris: only %zu of the two tuple node classes found; "
             "listeners stay inactive", classes.size());
        return false;
    }
    if (!tuple_slots_ok()) return false;

    for (auto const& entry : classes) {
        Hooked hooked;
        auto** insert = reinterpret_cast<void**>(entry.first + kInsertTuple);
        auto** remove = reinterpret_cast<void**>(entry.first + kDeleteTuple);

        void* wasInsert = nullptr;
        void* wasDelete = nullptr;
        if (!hook_pointer(insert, reinterpret_cast<void*>(&watched_insert),
                          &wasInsert)
            || !hook_pointer(remove, reinterpret_cast<void*>(&watched_delete),
                             &wasDelete)) {
            logf("osiris: could not make %s's vtable writable; listeners "
                 "stay inactive", entry.second.c_str());
            return false;
        }

        hooked.Insert = reinterpret_cast<NodeTupleProc>(wasInsert);
        hooked.Delete = reinterpret_cast<NodeTupleProc>(wasDelete);
        g_hooked[entry.first] = hooked;
        logf("osiris: watching %s (insert %p, delete %p)",
             entry.second.c_str(), wasInsert, wasDelete);
    }

    g_watching = true;
    return true;
}

}  // namespace

// An Osiris type resolved to one of the five built-in ones.
//
// The story declares its own types as aliases of these, so anything that has
// to name a type -- the IDE helpers, for one -- needs the base rather than the
// declared id.
std::uint16_t base_type(std::uint16_t declared) {
    const CacheLock lock(osiris_cache_lock());
    return resolve_alias(declared);
}

std::size_t story_function_count() { return g_story_functions; }

std::size_t node_count() { return g_nodes.Count; }

void probe_strings(char const* text) {
    const CacheLock lock(osiris_cache_lock());
    probe_string_pool(text);
}

Status insert(char const* key, std::vector<Value> const& args,
              std::string* why) {
    const CacheLock lock(osiris_cache_lock());
    return insert_tuple(key, args, kInsertTuple, why);
}

Status remove(char const* key, std::vector<Value> const& args,
              std::string* why) {
    const CacheLock lock(osiris_cache_lock());
    return insert_tuple(key, args, kDeleteTuple, why);
}

void set_trigger_sink(TriggerFn fn) { g_trigger = fn; }

namespace {

// Engine calls a listener watches, by dispatch handle: what upstream's
// CallPreHook and CallPostHook fire for.
struct WatchedCall {
    std::string name;
    std::vector<std::uint8_t> params;
};

std::unordered_map<std::uint32_t, WatchedCall>& watched_calls() {
    static std::unordered_map<std::uint32_t, WatchedCall> calls;
    return calls;
}

// A listener registers from either context's thread; the story thread reads.
std::mutex& watched_lock() {
    static std::mutex lock;
    return lock;
}

std::atomic<bool> g_any_watched_call{false};

}  // namespace

bool watch_call(Function const& fn) {
    // Builtin queries as well since upstream v33.
    if ((fn.kind() != kCall && fn.kind() != kQuery) || (fn.id >> 3) == 0) return false;
    {
        const std::lock_guard<std::mutex> held(watched_lock());
        watched_calls()[fn.id] = WatchedCall{fn.name, fn.params};
    }
    g_any_watched_call.store(true, std::memory_order_release);
    return true;
}

bool call_watched(std::uint32_t id) {
    if (!g_any_watched_call.load(std::memory_order_acquire)) return false;
    const std::lock_guard<std::mutex> held(watched_lock());
    return watched_calls().count(id) != 0;
}

void fire_call(std::uint32_t id, void const* args, char const* event) {
    if (g_trigger == nullptr) return;
    WatchedCall call;
    {
        const std::lock_guard<std::mutex> held(watched_lock());
        auto found = watched_calls().find(id);
        if (found == watched_calls().end()) return;
        call = found->second;
    }

    std::vector<Value> values;
    void const* node = args;
    for (std::size_t i = 0; i < call.params.size() && node != nullptr; ++i) {
        const std::uint8_t declared = call.params[i];
        values.push_back(read_declared(node, declared));
        void const* next = nullptr;
        if (!peek((std::uintptr_t)node, &next)) break;
        node = next;
    }
    g_trigger(call.name.c_str(), call.params.size(), event, values);
}

bool watch_story_triggers() {
    const CacheLock lock(osiris_cache_lock());
    return install_node_hooks();
}

// Does the story define a function by this name, and is it a database?
//
// Asked by the Lua side when a name misses, so story functions resolve on
// demand rather than being bound during the level load: the pointers
// cannot be cached across runs, and walking Osiris' database to get them
// costs half a second that a session which never calls one should not
// pay. bg3se resolves its Osi.* the same way, through a metatable.
bool story_function(char const* name, bool* is_database, std::string* real,
                    bool* is_query) {
    if (is_query != nullptr) *is_query = false;
    const CacheLock lock(osiris_cache_lock());
    if (name == nullptr || !bind_defs()) return false;

    std::string prefix = std::string(name) + "/";

    // Case-insensitively if the exact name is not there, as the engine's
    // own names resolve: mods write Proc_CharacterFullRestore where the
    // story declares PROC_CharacterFullRestore, and upstream answers with
    // a compatibility warning rather than a nil.
    bool exact = false;
    for (auto const& entry : database()) {
        if (entry.first.compare(0, prefix.size(), prefix) == 0) {
            exact = true;
            break;
        }
    }
    if (!exact) {
        for (auto const& entry : database()) {
            const std::size_t slash = entry.first.rfind('/');
            if (slash == std::string::npos || slash != prefix.size() - 1) {
                continue;
            }
            if (::strncasecmp(entry.first.c_str(), name, slash) != 0) continue;
            prefix = entry.first.substr(0, slash + 1);
            break;
        }
    }
    if (real != nullptr) prefix.substr(0, prefix.size() - 1).swap(*real);

    bool found = false;
    bool database_only = true;
    for (auto const& entry : database()) {
        if (entry.first.compare(0, prefix.size(), prefix) != 0) continue;
        if (entry.second.Def == 0) continue;

        const std::uintptr_t node = node_for(entry.second.Def);
        if (node == 0) continue;
        std::uintptr_t vtable = 0;
        if (!peek(node, &vtable) || vtable < 0x1000) continue;

        const std::string cls = class_of(vtable);
        if (cls == "13CReteOsiQuery") {
            if (is_query != nullptr) *is_query = true;
            found = true;
            database_only = false;
            continue;
        }
        if (cls != "10CReteEvent" && cls != "9CReteFact") continue;

        found = true;
        if (cls != "9CReteFact") database_only = false;
    }

    if (found && is_database != nullptr) *is_database = database_only;
    return found;
}


// The facts a story database holds, as text where the column is a string
// type. Reading these is the other half of what a database is for: a mod
// asks DB_Foo:Get(...) far more often than it inserts.
bool facts(char const* key, std::vector<std::vector<Value>>* rows) {
    const CacheLock lock(osiris_cache_lock());
    if (rows == nullptr || !bind_defs()) return false;
    if (g_databases.First == 0) return false;

    auto entry = database().find(key);
    if (entry == database().end() || entry->second.Def == 0) return false;

    const std::uintptr_t node = node_for(entry->second.Def);
    if (node == 0) return false;

    std::uint32_t dbId = 0;
    if (!peek(node + 0x18, &dbId) || dbId == 0 || dbId > g_databases.Count) {
        return false;
    }

    std::uintptr_t db = 0;
    if (!peek(g_databases.First + (std::uintptr_t)(dbId - 1) * 8, &db)
        || db < 0x1000) {
        return false;
    }

    std::uintptr_t head = 0;
    std::uint64_t count = 0;
    if (!peek(db + kFactsHead, &head) || !peek(db + kFactsCount, &count)) {
        return false;
    }
    if (head < 0x1000 || count > (1u << 20)) return false;

    std::uintptr_t at = head;
    for (std::uint64_t f = 0; f < count; ++f) {
        std::uintptr_t values = 0;
        std::uint64_t width = 0;
        if (!peek(at + 0x10, &values) || !peek(at + 0x18, &width)
            || values < 0x1000 || width == 0 || width > kMaxParams) {
            break;
        }

        std::vector<Value> row;
        row.reserve((std::size_t)width);
        for (std::uint64_t k = 0; k < width; ++k) {
            std::uint64_t raw = 0;
            std::uint16_t type = 0;
            if (!peek(values + k * 16, &raw)
                || !peek(values + k * 16 + 8, &type)) {
                break;
            }

            row.push_back(decode_value(raw, type));
        }
        rows->push_back(std::move(row));

        if (!peek(at + 0x00, &at) || at < 0x1000) break;
    }
    return true;
}

void set_handlers(void* call, void* query) {
    g_call = reinterpret_cast<Thunk6>(call);
    g_query = reinterpret_cast<Thunk6>(query);
}

bool ready() { return g_call != nullptr && g_query != nullptr && accessors().ok(); }

Status invoke(const Function& fn, const std::vector<Value>& inputs,
              std::vector<Value>* outputs) {
    const CacheLock lock(osiris_cache_lock());
    if (!ready()) return Status::kUnavailable;
    if (fn.kind() == kEvent) return Status::kUnavailable;  // the game raises these
    if (fn.params.size() > kMaxParams) return Status::kUnavailable;
    if (inputs.size() > fn.params.size()) return Status::kUnavailable;

    alignas(16) unsigned char storage[kMaxParams][kNodeSize];
    std::memset(storage, 0, sizeof(storage));

    const std::size_t n = fn.params.size();
    std::size_t next = 0;  // the next input to place
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t declared = fn.params[i];
        if (!fn.is_output(i, inputs.size()) && next < inputs.size()) {
            write(storage[i], wire_type(declared, inputs[next]), inputs[next]);
            ++next;
        } else {
            // An output: given its declared type, as upstream's query call
            // does, value left cleared.
            accessors().set_type(storage[i], declared);
        }
        void* nxt = (i + 1 < n) ? static_cast<void*>(storage[i + 1]) : nullptr;
        std::memcpy(storage[i], &nxt, sizeof(nxt));  // NextParam at +00
    }
    if (next != inputs.size()) return Status::kUnavailable;

    Thunk6 handler = fn.is_query() ? g_query : g_call;
    long rc = handler(static_cast<long>(fn.id),
                      n > 0 ? reinterpret_cast<long>(storage[0]) : 0, 0, 0, 0, 0);
    if ((rc & 0xff) == 0) return Status::kRejected;

    if (outputs != nullptr) {
        for (std::size_t i = 0; i < n; ++i) {
            if (!fn.is_output(i, inputs.size())) continue;
            const std::uint8_t declared = fn.params[i];
            outputs->push_back(read_declared(storage[i], declared));
        }
    }
    return Status::kHandled;
}

}  // namespace bg3le::osi
