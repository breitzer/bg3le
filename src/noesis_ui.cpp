// The game's Noesis, reached by symbol.
//
// Upstream links Noesis.dll; here Noesis is compiled into the executable and
// every one of its functions is a local symbol in .symtab, so they are called
// through addresses resolved by mangled name. The UI root is the content of
// the one Noesis::View, which hands itself over the first time the game calls
// its per-frame Update: that vtable slot, and its IView thunk, are hooked.

#include "noesis_ui.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "elf_symbols.h"
#include "hook.h"
#include "log.h"
#include "mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);
extern "C" std::size_t bg3le_noesis_resolve(void* (*find)(char const*));
extern "C" void bg3le_ui_run_deferred();

namespace bg3le {
namespace {

const SymbolTable* g_symbols = nullptr;

void* resolve(char const* mangled) {
    return g_symbols != nullptr ? g_symbols->find(mangled) : nullptr;
}

// Noesis::View's address point: the vtable symbol plus offset-to-top and
// typeinfo.
std::uintptr_t view_vtable() {
    void* vt = resolve("_ZTVN6Noesis4ViewE");
    return vt != nullptr ? reinterpret_cast<std::uintptr_t>(vt) + 16 : 0;
}

bool is_view(void* object) {
    std::uintptr_t vt = 0;
    return object != nullptr && safe_read(object, &vt, sizeof(vt))
        && vt == view_vtable();
}

// The IView subobject's offset, from the thunk's mangled name.
constexpr std::size_t kIViewOffset = 16;

// View::Update(double) returns whether a render is needed; passed through.
using UpdateProc = std::uint64_t (*)(void*, double);
UpdateProc g_update = nullptr;
UpdateProc g_update_thunk = nullptr;
std::atomic<void*> g_view{nullptr};
bool g_hooked = false;

void saw_view(void* view) {
    void* expected = nullptr;
    if (g_view.compare_exchange_strong(expected, view)) {
        logf("noesis: View at %p", view);
    } else if (expected != view && !is_view(expected)) {
        g_view.store(view);  // the old one is gone
        logf("noesis: View replaced, now at %p", view);
    }
}

std::uint64_t update_hook(void* view, double time) {
    saw_view(view);
    bg3le_ui_run_deferred();
    return g_update(view, time);
}

std::uint64_t update_thunk_hook(void* iview, double time) {
    saw_view(static_cast<char*>(iview) - kIViewOffset);
    bg3le_ui_run_deferred();
    return g_update_thunk(iview, time);
}

// Hooks the View vtable slot that holds `function`.
bool hook_view_slot(char const* function, void* replacement, UpdateProc* original) {
    const std::uintptr_t vt = view_vtable();
    auto target = reinterpret_cast<std::uintptr_t>(resolve(function));
    if (vt == 0 || target == 0) return false;
    // The View vtable and its IView part: 99 entries.
    for (std::size_t i = 0; i < 99; ++i) {
        std::uintptr_t entry = 0;
        const std::uintptr_t slot = vt + i * 8;
        if (!safe_read(reinterpret_cast<void*>(slot), &entry, sizeof(entry))) break;
        if (entry != target) continue;
        void* previous = nullptr;
        if (!hook_slot(slot - load_bias(), target - load_bias(), replacement, &previous)) {
            return false;
        }
        *original = reinterpret_cast<UpdateProc>(previous);
        return true;
    }
    return false;
}

// Fallback for when the hooks could not go in: a one-off scan for the vtable.
void* scan_for_view() {
    const std::uintptr_t want = view_vtable();
    if (want == 0) return nullptr;
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;
    constexpr std::size_t kChunk = 1u << 20;
    static unsigned char block[kChunk];
    char line[512];
    void* found = nullptr;
    while (found == nullptr && std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0, to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;
        for (unsigned long long base = from; base < to && found == nullptr;
             base += kChunk) {
            const std::size_t want_n = (std::size_t)(to - base) < kChunk
                ? (std::size_t)(to - base) : kChunk;
            const std::size_t got = safe_read_some((void const*)base, block, want_n);
            for (std::size_t off = 0; off + 8 <= got; off += 8) {
                std::uintptr_t v;
                std::memcpy(&v, block + off, 8);
                if (v == want) {
                    found = reinterpret_cast<void*>(base + off);
                    break;
                }
            }
        }
    }
    std::fclose(maps);
    return found;
}

}  // namespace

void noesis_set_symbols(const SymbolTable* symbols) {
    g_symbols = symbols;
    const std::size_t missing = bg3le_noesis_resolve(
        [](char const* name) -> void* { return resolve(name); });
    if (missing != 0) logf("noesis: %zu forwarded functions unresolved", missing);

    g_hooked = hook_view_slot("_ZN6Noesis4View6UpdateEd",
                              reinterpret_cast<void*>(&update_hook), &g_update)
        && hook_view_slot("_ZThn16_N6Noesis4View6UpdateEd",
                          reinterpret_cast<void*>(&update_thunk_hook), &g_update_thunk);
    if (!g_hooked) logf("noesis: View::Update not hooked; Ext.UI will scan for the View");
}

bool noesis_update_hooked() { return g_hooked; }

void* noesis_view() {
    void* view = g_view.load();
    if (is_view(view)) return view;
    if (g_hooked) return nullptr;  // no UI yet

    static void* scanned = nullptr;
    if (!is_view(scanned)) {
        scanned = scan_for_view();
        if (scanned != nullptr) logf("noesis: View at %p (scanned)", scanned);
    }
    return scanned;
}

void* noesis_root() {
    using GetContent = void* (*)(void const*);
    static auto get = reinterpret_cast<GetContent>(
        resolve("_ZNK6Noesis4View10GetContentEv"));
    void* view = noesis_view();
    return view != nullptr && get != nullptr ? get(view) : nullptr;
}

}  // namespace bg3le

// A symbol of the executable by mangled name, or null (Ext.UI's boxing).
extern "C" void* bg3le_noesis_symbol(char const* mangled) {
    return bg3le::resolve(mangled);
}
