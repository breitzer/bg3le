// bg3le - Linux script extender shim for the native Baldur's Gate 3 build.
//
// Loaded via LD_PRELOAD.  Osiris entry points are plain PLT calls from the
// bg3 executable into libOsiris.so, so they are interposed by defining the
// same mangled symbols here and chaining with dlsym(RTLD_NEXT, ...).

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <sched.h>
#include <spawn.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <chrono>
#include <thread>
#include <mutex>
#include <ctime>
#include <unistd.h>

#include "ecs_types.h"
#include "ecs_world.h"
#include "elf_symbols.h"
#include "hook.h"
#include "resolve.h"
#include "targets.h"
#include "game_state.h"
namespace bg3le { void install_entity_trace_hook(); }
#include "savegame.h"
#include "debug_server.h"
#include "lua_host.h"
#include "version.h"
#include "osi.h"
#include "fast_alloc.h"

namespace bg3le {
void install_path_override_hook();
}
extern "C" void bg3le_note_game_server(void* server);
#include "stackdump.h"
#include "mem.h"
#include "log.h"
#include "net.h"
#include "console.h"

namespace bg3le {

// Defined in src/vendor/imgui_overlay.cpp, which needs bg3se headers this
// file does not include.
void imgui_overlay_start();


namespace {

SymbolTable g_symbols;
std::once_flag g_symbols_once;
std::once_flag g_init_struct_once;

void ensure_symbols();
void ensure_achievement_gate_patch();

// Defined in src/vendor/platform_linux.cpp.
extern "C" bool bg3le_fixed_string_intern(char const* text,
                                          unsigned int* out);
extern "C" int bg3le_stats_string_intern(char const* text);
extern "C" void* bg3le_stats_find(char const* wanted);
extern "C" void* bg3le_global_switches();
extern "C" std::size_t bg3le_stats_attr_count(void const* object);
extern "C" bool bg3le_stats_attr_at(void const* object, std::size_t index,
                                    char const** nameOut,
                                    char const** typeNameOut, int* kindOut,
                                    int* rawOut);
extern "C" bool bg3le_stats_attr_set(void const* object, std::size_t index,
                                     int raw);
extern "C" bool bg3le_install_game_allocator(void* alloc, void* free);
extern "C" void bg3le_corelib_strings_install();

// Points bg3se's GameAllocRaw/GameFree at the engine's own heap, so any bg3se
// container we grow allocates the way the engine does. Without this the two
// function pointers are null, because the Windows path that fills them in is a
// pattern scan we do not have. The mangled names are the engine's static
// global operator new and operator delete.
void install_game_allocator() {
    void* alloc = g_symbols.find("_Znwm");
    void* free = g_symbols.find("_ZdlPv");
    if (!bg3le_install_game_allocator(alloc, free)) {
        logf("game allocator: operator new/delete not found in the symbol "
             "table (new %p, delete %p); anything that would allocate through "
             "bg3se will refuse rather than run",
             alloc, free);
        return;
    }
    logf("game allocator: using the engine heap (new %p, delete %p)", alloc,
         free);
}

extern "C" void* bg3le_stats_manager();
extern "C" std::size_t bg3le_mods_count();
extern "C" void bg3le_mods_rescan();
extern "C" char const* bg3le_game_version();

// The game build bg3le's data layouts were established against -- see
// reference/version.txt. The API version is in version.h.
constexpr char const* kValidatedGameVersion = "v4.73.98.727";
extern "C" void bg3le_pak_modules_prewarm();
extern "C" bool bg3le_stat_origins_ready();
extern "C" bool bg3le_loca_ready();
extern "C" bool bg3le_templates_ready();
extern "C" bool bg3le_prototypes_ready();

// Finds the stats manager on a thread of our own.
//
// The search reads every writable region, and the console dispatches
// expressions onto the story thread -- so doing it on demand means the first
// Ext.Stats call stalls the game. Worse, the stats are parsed during load, so
// an early attempt fails for a reason that stops being true; this retries
// until it succeeds or the game has plainly finished loading without it.
//
// Detached on purpose: nothing waits on the result, and a failure only means
// Ext.Stats reports itself unavailable.
// Finds the structures Ext.* reads, on a thread of our own.
//
// Each of these is a scan of the process's own memory, and there are now
// six of them. Run eagerly on a fixed five-second cadence they were
// scanning gigabytes forty times over during startup, which left the game
// idle and unable to tick -- the debugger could not get a story thread at
// all. So: ordered by dependency, backed off between attempts, and given
// up on early.
//
// Detached on purpose: nothing waits on the result, and a failure only
// means the module in question reports itself unavailable.
// Every search, once, on the calling thread.
//
// This is the one that matters. A mod's load-time code calls Ext.Stats.Get
// or Ext.Loca.GetTranslatedString the moment it runs, and a background
// thread that is still looking cannot answer it -- the call fails, which
// is a wrong answer rather than a slow one. So the searches run to
// completion at the point where the engine's data exists and nothing has
// asked yet: after Osiris has loaded and before mod scripts do.
//
// bg3se has no equivalent problem because it does not search for data at
// all; it pattern-matches the executable once at startup to recover the
// addresses of the engine's static manager pointers, and then just
// dereferences them. Doing the same here is the right long-term answer --
// it would make all of this immediate instead of merely early.
void run_searches_now(char const* when) {
    scan_enable_on_this_thread();

    struct Step {
        char const* Name;
        bool (*Ready)();
    };
    // What a mod needs before its first line runs, and nothing more.
    //
    // Templates and prototypes used to be here too. They are the two
    // searches with no recorded pointer to resolve from, so they scan
    // every time -- fifteen seconds of story thread at level load, which
    // is exactly the stall this list exists to avoid. They keep running on
    // the warming thread and are ready a few seconds later; a mod that
    // asks for one at load time waits for it there instead.
    const Step steps[] = {
        {"paks", [] { bg3le_pak_modules_prewarm(); return true; }},
        {"loca", &bg3le_loca_ready},
        {"stats", [] { return bg3le_stats_manager() != nullptr; }},
        {"mods", [] { return bg3le_mods_count() > 0; }},
        {"origins", &bg3le_stat_origins_ready},
    };

    using clock = std::chrono::steady_clock;
    auto const elapsed = [](clock::time_point from) {
        return std::chrono::duration<double>(clock::now() - from).count();
    };

    const auto started = clock::now();
    for (Step const& step : steps) {
        const auto at = clock::now();
        const bool ok = step.Ready();
        logf("search(%s): %s %s in %.2fs", when, step.Name,
             ok ? "ready" : "NOT FOUND", elapsed(at));
    }
    logf("search(%s): all searches took %.2fs", when, elapsed(started));
}

void warm_stats_search() {
    // BG3LE_NO_WARM=1 skips every search, so a problem can be told apart
    // from the searches that look for the structures behind them.
    if (std::getenv("BG3LE_NO_WARM") != nullptr) {
        logf("warm: skipped (BG3LE_NO_WARM)");
        return;
    }

    std::thread([] {
        scan_enable_on_this_thread();

        // The archive index first, and on this thread: it is pure file
        // work, it does not depend on the game being up, and the only
        // other place it could happen is the story thread during level
        // load.
        bg3le_pak_modules_prewarm();

        struct Search {
            char const* Name;
            bool (*Ready)();
            bool Done;
            bool NeedsStats;
        };

        Search searches[] = {
            {"stats", [] { return bg3le_stats_manager() != nullptr; },
             false, false},
            {"mods", [] { return bg3le_mods_count() > 0; }, false, false},
            // Which mod defines a stat needs the load order first.
            {"origins", &bg3le_stat_origins_ready, false, false},
            // Reads the archives rather than memory, so it is cheap and
            // settles on the first attempt.
            // Reads the archives rather than memory, so it is cheap and
            // settles on the first attempt. The menu's version line is
            // rewritten once it has, since that needs the original text.
            {"loca", &bg3le_loca_ready, false, false},
            {"templates", &bg3le_templates_ready, false, false},
            // Classifying the prototype maps asks the stats what their
            // names are, so there is no point before stats is up.
            {"prototypes", &bg3le_prototypes_ready, false, true},
        };

        // The localisation comes first; the version line no longer waits
        // for it.
        //
        // bg3se writes that line as the client leaves GameState::LoadModule
        // -- before the menu is built. Having it queued behind five
        // whole-address-space searches put it minutes late, so the menu
        // came up with no line on it at all. Reading the .loca archives
        // takes about a second, so this costs nothing to do first.
        // The version line is reapplied for as long as this loop runs.
        // The pool the menu reads is not necessarily the one that exists
        // a few seconds after load, so patching once is not enough.
        std::this_thread::sleep_for(std::chrono::seconds(2));

        bool statsReady = false;

        // Five seconds, then ten, twenty and forty: a search that has not
        // succeeded by then is waiting on data the game has not built, and
        // rescanning every five seconds only takes cycles from the game.
        // Enough rounds for every search to get several turns: one
        // search runs per round, and one that needs another to finish
        // first has to wait for it. Six rounds meant the prototype search
        // never got a turn at all, and the first Lua call for it then ran
        // the scan on the story thread.
        int delay = 5;
        for (int attempt = 0; attempt < 120; ++attempt) {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            if (delay < 4) delay *= 2;

            bool all = true;
            for (Search& search : searches) {
                if (search.Done) continue;
                if (search.NeedsStats && !statsReady) {
                    all = false;
                    continue;
                }
                // One search per round, so a round cannot be six scans
                // back to back.
                search.Done = search.Ready();
                if (!search.Done) all = false;
                if (std::strcmp(search.Name, "stats") == 0) {
                    statsReady = search.Done;
                }
                break;
            }

            for (Search const& search : searches) {
                if (!search.Done) all = false;
            }
            // The version line is re-checked for as long as the loop
            // runs, so finishing early would stop maintaining it.
            if (all && attempt > 30) return;
        }

        for (Search const& search : searches) {
            if (!search.Done) {
                logf("warm: gave up looking for %s; the Ext.* functions "
                     "that need it will report themselves unavailable",
                     search.Name);
            }
        }
    }).detach();
}

template <typename Fn>
Fn next(const char* mangled) {
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, mangled));
}

// ---- stall profiling ----
//
// Every profile so far caught gameplay, not the load. The stall begins
// immediately after Osiris finishes, and we are the only thing that knows
// when that is -- so start perf from here and capture exactly that window.
void start_stall_profile() {
    const char* opt = std::getenv("BG3LE_PERF");
    if (opt == nullptr || opt[0] != '1') return;

    char pid[32];
    std::snprintf(pid, sizeof(pid), "%d", (int)::getpid());

    // No call graph and a low rate: with ~35 threads, stack-walking at a few
    // hundred hertz costs more than the stall being measured. Self time at
    // 99Hz is enough to name the hot function.
    const char* argv[] = {"perf", "record", "-F", "99",
                          "-p",   pid,      "-o", "/tmp/bg3-stall.perf.data",
                          "--",   "sleep",  "70", nullptr};

    pid_t child = 0;
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
    const int rc = ::posix_spawnp(&child, "perf", nullptr, &attr,
                                  const_cast<char* const*>(argv), environ);
    ::posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        statusf("perf: could not start (%s)", std::strerror(rc));
        return;
    }
    std::thread([child] { int st = 0; ::waitpid(child, &st, 0); }).detach();
    statusf("perf: flat-sampling the load stall at 99Hz");
}

// ---- clock diagnostics ----
//
// Profiling the native build shows ~12% of all cycles in clock_gettime plus
// ~7% in the kernel, which is far too much for timekeeping and looks like a
// hot loop. Clock ids matter here: CLOCK_MONOTONIC and CLOCK_REALTIME are
// served by the vDSO in ~20ns, but CLOCK_*_CPUTIME_ID always enters the
// kernel and costs ~1us. Count calls per id to see which is being used.
// Enable with BG3LE_CLOCK_STATS=1.

std::atomic<unsigned long> g_clock_calls[16];
std::atomic<bool> g_clock_stats{false};

const char* clock_name(int id) {
    switch (id) {
        case CLOCK_REALTIME: return "REALTIME";
        case CLOCK_MONOTONIC: return "MONOTONIC";
        case CLOCK_PROCESS_CPUTIME_ID: return "PROCESS_CPUTIME (syscall)";
        case CLOCK_THREAD_CPUTIME_ID: return "THREAD_CPUTIME (syscall)";
        case CLOCK_MONOTONIC_RAW: return "MONOTONIC_RAW (syscall)";
        case CLOCK_REALTIME_COARSE: return "REALTIME_COARSE";
        case CLOCK_MONOTONIC_COARSE: return "MONOTONIC_COARSE";
        case CLOCK_BOOTTIME: return "BOOTTIME";
        default: return "other";
    }
}

// Workaround for the busy-wait. A thread calling the clock again within a
// microsecond is polling, not timing; once that repeats enough times, yield
// so the threads doing real work get the core. sched_yield is used rather
// than a sleep because it costs nothing when no one else is runnable -- this
// donates spare capacity instead of inserting delay.
std::atomic<bool> g_spin_backoff{false};
std::atomic<unsigned long> g_yields{0};

constexpr std::uint64_t kSpinGapNs = 1000;   // calls closer than this are polling
constexpr unsigned kSpinBeforeYield = 200;   // consecutive polls before yielding

void maybe_back_off(const struct timespec* ts) {
    thread_local std::uint64_t last_ns = 0;
    thread_local unsigned spins = 0;

    const std::uint64_t now =
        static_cast<std::uint64_t>(ts->tv_sec) * 1000000000ULL + ts->tv_nsec;
    if (now - last_ns < kSpinGapNs) {
        if (++spins >= kSpinBeforeYield) {
            spins = 0;
            g_yields.fetch_add(1, std::memory_order_relaxed);
            ::sched_yield();
        }
    } else {
        spins = 0;
    }
    last_ns = now;
}

extern "C" int clock_gettime(clockid_t clk, struct timespec* ts) {
    static auto real = next<int (*)(clockid_t, struct timespec*)>("clock_gettime");
    if (real == nullptr) return -1;

    if (clk == CLOCK_MONOTONIC && g_spin_backoff.load(std::memory_order_relaxed)) {
        const int rc = real(clk, ts);
        maybe_back_off(ts);
        return rc;
    }

    if (g_clock_stats.load(std::memory_order_relaxed)) {
        const unsigned idx = static_cast<unsigned>(clk) < 16u
                                 ? static_cast<unsigned>(clk) : 15u;
        const unsigned long n =
            g_clock_calls[idx].fetch_add(1, std::memory_order_relaxed);

        // Report from one id only, so the log is not flooded.
        if (idx == 1 && (n % 20000000UL) == 0 && n > 0) {
            for (unsigned i = 0; i < 16; ++i) {
                const unsigned long c = g_clock_calls[i].load(std::memory_order_relaxed);
                if (c > 0) logf("clock: id=%u %-26s %lu calls", i, clock_name((int)i), c);
            }
        }
    }
    return real(clk, ts);
}

// ---- tick ----
//
// esv::GameServer::UpdateMessagesToSend flushes outbound network messages
// once per server tick, on the story thread, whether or not the story is
// busy. It has no direct call sites -- it is dispatched through a pointer
// table -- so hooking it is one aligned store.
//
// Slot and function from targets.h; hook_slot checks the one holds the other.
using UpdateMessagesProc = void (*)(void*);
UpdateMessagesProc g_orig_update_messages = nullptr;

void update_messages_hook(void* self) {
    bg3le_note_game_server(self);
    net_server_tick(self);
    debug_server_note_story_thread();
    debug_server_pump();
    lua_tick();
    ensure_achievement_gate_patch();
    if (g_orig_update_messages != nullptr) g_orig_update_messages(self);
}

void install_tick_hook() {
    void* original = nullptr;
    if (hook_slot(target::UpdateMessagesSlot(), target::UpdateMessagesFunc(),
                  reinterpret_cast<void*>(&update_messages_hook), &original)) {
        g_orig_update_messages = reinterpret_cast<UpdateMessagesProc>(original);
        statusf("Hooked server tick at the vtable slot in image+%#lx",
                (unsigned long)target::UpdateMessagesSlot());
    } else {
        statusf("WARNING: server tick hook refused; the prompt will stall "
                "unless a story is active");
    }
}

double now_s();

void ensure_symbols() {
    std::call_once(g_symbols_once, [] {
        // Upstream reports what its own startup cost ("Library startup took
        // 1252 ms"); ours is the symbol table and the ECS registry built
        // from it, so those are timed the same way.
        const double started = now_s();
        if (!g_symbols.load()) {
            logf("symbol table unavailable");
            return;
        }
        const double symbols = now_s();
        // Upstream opens with its build, the game's version and a verdict
        // on it. Same three facts, since the first question about any
        // extender is whether it matches the game it is loaded into.
        statusf("bg3le, bg3se v%d API, built on %s %s", kExtenderApiVersion,
                __DATE__, __TIME__);
        char const* version = bg3le_game_version();
        if (version != nullptr && version[0] != '\0') {
            const bool known =
                std::strcmp(version, kValidatedGameVersion) == 0;
            if (known) {
                statusf("Game version %s OK", version);
            } else {
                statusf("Game version %s -- bg3le's layouts were established "
                        "against %s", version, kValidatedGameVersion);
            }
        }
        statusf("bg3le attached to %s", g_symbols.path().c_str());
        statusf("Extender runtime log written to '%s'", log_path());
        statusf("Read %zu symbols from the executable's table "
                "(load bias 0x%lx)", g_symbols.count(), g_symbols.bias());

        // The engine names every ECS type index, so the whole registry comes
        // straight out of the symbol table.
        bg3le_corelib_strings_install();
        resolve_set_symbols(&g_symbols);
        lua_set_symbols(&g_symbols);
        const std::size_t types = ecs::load(g_symbols);
        statusf("ECS registry: %zu type indices (%zu components)", types,
                ecs::count(ecs::Context::Component));
        statusf("bg3le startup took %d ms (%d of it the symbol table)",
                (int)((now_s() - started) * 1000.0),
                (int)((symbols - started) * 1000.0));
        void* p = g_symbols.find(
            "_ZN2ls11TypeContextIN3esv4tags8_private23TagComponentTypeContextEE7m_StateE");
        logf("  sentinel esv TagComponentTypeContext::m_State -> %p", p);
        lua_init();
        debug_server_start();

        warm_stats_search();
        if (const char* e = std::getenv("BG3LE_CLOCK_STATS")) {
            g_clock_stats.store(e[0] == '1');
        }
        if (const char* e = std::getenv("BG3LE_SPIN_BACKOFF")) {
            g_spin_backoff.store(e[0] == '1');
            if (e[0] == '1') statusf("Spin backoff enabled (yield after %u polls)",
                                     kSpinBeforeYield);
        }
        install_tick_hook();
        ecs::install_container_capture();
        install_game_allocator();
        fast_alloc_install();
        install_path_override_hook();
    });
}

// Derived from the engine's own tables: a flat 24-byte record. Function ids
// always carry 0x80000000; param_types points into one contiguous byte array
// shared by all entries.
struct MappingInfo {
    const char* name;
    std::uint32_t id;
    std::uint32_t num_params;
    const std::uint8_t* param_types;
};
static_assert(sizeof(MappingInfo) == 24, "unexpected MappingInfo layout");

using GetMappings = long (*)(void*, MappingInfo**, unsigned*);
using FreeMappings = long (*)(void*, MappingInfo*, unsigned);

// Ids 0-5 are Osiris' built-in value types and are absent from the story's
// type table; 6+ are the story enums (CHARACTER, ITEM, FLAG, ...).
const char* base_type_name(std::uint8_t t) {
    switch (t) {
        case 0: return "NONE";
        case 1: return "INTEGER";
        case 2: return "INTEGER64";
        case 3: return "REAL";
        case 4: return "STRING";
        case 5: return "GUIDSTRING";
        default: return nullptr;
    }
}

std::string name_of(const MappingInfo& m) {
    char buf[256];
    return safe_cstr(m.name, buf, sizeof(buf)) ? std::string(buf) : std::string("?");
}

// TOsirisInitFunction is the callback table the game hands to Osiris. It
// should carry the dispatch entry used to invoke DIV functions, which is the
// call path Osi.* needs. Classify each word to find out what's in it.
void dump_init_struct(const void* init_fn) {
    std::uintptr_t words[16] = {};
    if (!safe_read(init_fn, words, sizeof(words))) {
        logf("init struct: unreadable at %p", init_fn);
        return;
    }

    logf("TOsirisInitFunction @ %p", init_fn);
    for (unsigned i = 0; i < 16; ++i) {
        const std::uintptr_t w = words[i];
        const std::size_t off = i * sizeof(void*);

        if (w == 0) {
            logf("  +%02zu = 0", off);
            continue;
        }

        Dl_info info{};
        if (::dladdr(reinterpret_cast<void*>(w), &info) != 0 && info.dli_fname != nullptr) {
            const char* base = std::strrchr(info.dli_fname, '/');
            logf("  +%02zu = 0x%016lx  %s+0x%lx  %s", off, (unsigned long)w,
                 base != nullptr ? base + 1 : info.dli_fname,
                 (unsigned long)(w - reinterpret_cast<std::uintptr_t>(info.dli_fbase)),
                 info.dli_sname != nullptr ? info.dli_sname : "");
            continue;
        }

        char text[64];
        if (safe_cstr(reinterpret_cast<const void*>(w), text, sizeof(text)) &&
            text[0] >= 0x20 && text[0] < 0x7f) {
            logf("  +%02zu = 0x%016lx  \"%s\"", off, (unsigned long)w, text);
        } else {
            logf("  +%02zu = 0x%016lx", off, (unsigned long)w);
        }
    }
}

// The table's two large entries (+08, +16) are the likely Call/Query
// handlers Osiris invokes for DIV functions; the rest are small thunks.
// Rather than guess, hand Osiris a copy with those two wrapped and learn the
// signature from real traffic.
//
// Wrappers take six longs so they forward correctly whatever the true arity
// is: SysV passes the first six integer args in registers, and the callee
// reads only what it needs. Opt in with BG3LE_WRAP_DIV=1.
using Thunk6 = long (*)(long, long, long, long, long, long);

Thunk6 g_real_call = nullptr;
Thunk6 g_real_query = nullptr;

// Cross-check the raw bytes of a live COsiArgumentDesc against Osiris' own
// exported accessors, so the layout is read off the engine rather than guessed.
void test_requery(unsigned id, void* args);
void test_integer_sum();

void dump_arg_desc(const void* desc, unsigned id, const char* kind) {
    auto type_of = next<int (*)(const void*)>("_ZNK16COsiArgumentDesc13GetOpaqueTypeEv");
    auto is_str = next<bool (*)(const void*)>("_ZNK16COsiArgumentDesc12IsStringTypeEv");
    auto get_int = next<int (*)(const void*)>("_ZNK16COsiArgumentDesc10GetIntegerEv");
    auto get_str = next<const char* (*)(const void*)>(
        "_ZNK16COsiArgumentDesc12GetAnyStringEv");
    auto src_ptr = next<const void* (*)(const void*, bool)>(
        "_ZNK16COsiArgumentDesc13GetDataSrcPtrEb");
    auto data_size = next<unsigned long (*)(const void*, bool)>(
        "_ZNK16COsiArgumentDesc11GetDataSizeEb");

    logf("%s id=0x%08x chain from %p", kind, id, desc);

    const void* node = desc;
    for (unsigned n = 0; n < 8 && node != nullptr; ++n) {
        std::uintptr_t w[12] = {};
        if (!safe_read(node, w, sizeof(w))) {
            logf("  node[%u] @ %p unreadable", n, node);
            break;
        }

        const int type = type_of != nullptr ? type_of(node) : -1;
        const bool str = is_str != nullptr && is_str(node);
        logf("  node[%u] @ %p type=%d isString=%d", n, node, type, (int)str);
        for (unsigned i = 0; i < 12; ++i)
            logf("      +%02zu = 0x%016lx", i * sizeof(void*), (unsigned long)w[i]);

        // Where does the value actually live? This is what constructing one needs.
        if (src_ptr != nullptr) logf("      GetDataSrcPtr(false) = %p", src_ptr(node, false));
        auto dst_ptr = next<void* (*)(void*)>("_ZN16COsiArgumentDesc13GetDataDstPtrEv");
        if (dst_ptr != nullptr)
            logf("      GetDataDstPtr()      = %p", dst_ptr(const_cast<void*>(node)));
        if (data_size != nullptr) logf("      GetDataSize(false)   = %lu", data_size(node, false));

        if (str && get_str != nullptr) {
            char buf[128];
            const char* v = get_str(node);
            logf("      GetAnyString() = \"%s\"",
                 safe_cstr(v, buf, sizeof(buf)) ? buf : "<unreadable>");
        } else if (!str && get_int != nullptr) {
            logf("      GetInteger()   = %d", get_int(node));
        }

        node = reinterpret_cast<const void*>(w[0]);  // suspected NextParam
    }
}

// ISteamUserStats vtable hook state (installed later, once the game first
// asks for the interface -- see "Steam achievement diagnostics" below).
using SetAchievementFn = bool (*)(void*, const char*);
std::atomic<SetAchievementFn> g_real_set_achievement{nullptr};
std::atomic<bool> g_user_stats_vtable_patched{false};

// EnableAchievements: ls::ModuleSettings::IsModded, patched to return false
// as bg3se's patch makes it. Not the per-module "is official" predicate it
// calls (image+0x37675f0): new games use that too, and with it forced true
// they started with the base modules only.
// target::IsModded(), resolved before the first patch and kept, since the patch changes its bytes.
constexpr unsigned char kIsModdedBytes[11] = {
    0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x50};
constexpr unsigned char kIsModdedPatch[3] = {0x31, 0xc0, 0xc3};  // xor eax,eax; ret

// Off with BG3LE_ACHIEVEMENTS=0, or "EnableAchievements": false in
// ScriptExtenderSettings.json next to the binary (default on).
bool achievement_patch_disabled() {
    static const bool disabled = [] {
        const char* e = std::getenv("BG3LE_ACHIEVEMENTS");
        if (e != nullptr && std::strcmp(e, "0") == 0) {
            logf("EnableAchievements: BG3LE_ACHIEVEMENTS=0, predicate left unpatched");
            return true;
        }
        char exe[4096];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return false;
        exe[n] = '\0';
        std::string dir(exe);
        const std::size_t slash = dir.rfind('/');
        if (slash != std::string::npos) dir.erase(slash);
        const bool on = settings_flag(dir, "EnableAchievements", true);
        if (!on) logf("EnableAchievements: disabled in ScriptExtenderSettings.json");
        return !on;
    }();
    return disabled;
}

// LD_PRELOAD also lands in steam-launch-wrapper and reaper.
// Only the real game binary is patched.
bool host_is_game() {
    static const bool game = [] {
        char exe[4096];
        const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n <= 0) return false;
        exe[n] = '\0';
        const char* base = std::strrchr(exe, '/');
        base = base != nullptr ? base + 1 : exe;
        const bool ok = std::strcmp(base, "bg3") == 0;
        if (!ok) logf("host is %s, not bg3 -- bg3le stays inactive", base);
        return ok;
    }();
    return game;
}

// Idempotent: applies once, then only re-checks the bytes.
void ensure_achievement_gate_patch() {
    if (!host_is_game() || achievement_patch_disabled()) return;
    if (bytes_match(target::IsModded(), kIsModdedPatch, sizeof(kIsModdedPatch))) {
        return;
    }
    static bool first = true;
    if (patch_bytes(target::IsModded(), kIsModdedBytes, sizeof(kIsModdedBytes),
                    kIsModdedPatch, sizeof(kIsModdedPatch))) {
        logf("EnableAchievements: %s IsModded at 0x%lx",
             first ? "patched" : "re-applied", (unsigned long)target::IsModded());
    } else if (first) {
        logf("WARNING: EnableAchievements IsModded patch refused -- "
             "achievements will stay mod-blocked");
    }
    first = false;
}

// Every engine call, from the story or from Lua, passes through here so a
// watched one reaches its listeners before and after it runs, as upstream's
// CallPreHook and CallPostHook. An unwatched call costs one flag test.
long listening_call(long a, long b, long c, long d, long e, long f) {
    const auto id = static_cast<std::uint32_t>(a);
    const bool watched = osi::call_watched(id);
    if (watched) osi::fire_call(id, reinterpret_cast<const void*>(b), "before");
    const long rc = g_real_call != nullptr ? g_real_call(a, b, c, d, e, f) : 0;
    if (watched) osi::fire_call(id, reinterpret_cast<const void*>(b), "after");
    return rc;
}

long call_wrapper(long a, long b, long c, long d, long e, long f) {
    static unsigned long seen = 0;
    if (++seen <= 10) logf("DIV Call  arg0=0x%lx arg1=0x%lx", a, b);
    if (seen <= 3) dump_arg_desc(reinterpret_cast<const void*>(b),
                                 (unsigned)a, "DIV Call ");
    return listening_call(a, b, c, d, e, f);
}

// Engine queries too: upstream's QueryWrapper fires the same hooks since v33.
long listening_query(long a, long b, long c, long d, long e, long f) {
    const auto id = static_cast<std::uint32_t>(a);
    const bool watched = osi::call_watched(id);
    if (watched) osi::fire_call(id, reinterpret_cast<const void*>(b), "before");
    const long rc = g_real_query != nullptr ? g_real_query(a, b, c, d, e, f) : 0;
    if (watched) osi::fire_call(id, reinterpret_cast<const void*>(b), "after");
    return rc;
}

long query_wrapper(long a, long b, long c, long d, long e, long f) {
    static unsigned long seen = 0;
    if (++seen <= 10) logf("DIV Query arg0=0x%lx arg1=0x%lx", a, b);
    if (seen <= 3) dump_arg_desc(reinterpret_cast<const void*>(b),
                                 (unsigned)a, "DIV Query");
    static std::once_flag once;
    if ((unsigned)a == 0x8000113au || (unsigned)a == 0x800019f2u)
        std::call_once(once, [a, b] {
            test_requery((unsigned)a, reinterpret_cast<void*>(b));
        });
    return listening_query(a, b, c, d, e, f);
}

// Returns the table to pass on: either our patched copy or the original.
void* maybe_wrap_div_table(void* init_fn) {
    static std::uintptr_t copy[32];
    if (!safe_read(init_fn, copy, sizeof(copy))) {
        logf("DIV table unreadable, passing through");
        return init_fn;
    }

    // Always record the true originals first.
    g_real_call = reinterpret_cast<Thunk6>(copy[1]);
    g_real_query = reinterpret_cast<Thunk6>(copy[2]);

    // osi::invoke() bypasses the DIV table, so it goes through the
    // listening wrappers directly; the story reaches them through the table.
    osi::set_handlers(reinterpret_cast<void*>(&listening_call),
                      reinterpret_cast<void*>(&listening_query));
    copy[1] = reinterpret_cast<std::uintptr_t>(&listening_call);
    copy[2] = reinterpret_cast<std::uintptr_t>(&listening_query);

    const char* opt = std::getenv("BG3LE_WRAP_DIV");
    if (opt == nullptr || opt[0] != '1') {
        return copy;
    }

    copy[1] = reinterpret_cast<std::uintptr_t>(&call_wrapper);
    copy[2] = reinterpret_cast<std::uintptr_t>(&query_wrapper);
    osi::set_handlers(reinterpret_cast<void*>(copy[1]),
                      reinterpret_cast<void*>(copy[2]));
    logf("DIV wrap: active (call=%p query=%p)", (void*)g_real_call, (void*)g_real_query);
    return copy;
}

double g_story_ready_at = 0.0;

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// First attempt at invoking Osiris ourselves. IntegerSum is pure arithmetic
// with a checkable answer, so a wrong layout shows up as a bad result rather
// than as damage to a save. Opt in with BG3LE_TEST_CALL=1.
// SetInteger asserts the descriptor is already typed -- its disassembly
// compares the uint16 at +16 against 1 and otherwise raises "Trying to set
// integer parameter while type is different", which routes to the game's
// assert thunk and aborts. SetType must come first; it is a two-instruction
// store of the type at +16 and a zero of the value at +08.
void test_integer_sum() {
    const char* opt = std::getenv("BG3LE_TEST_CALL");
    if (opt == nullptr || opt[0] != '1') return;
    logf("lua test: Osi.IntegerSum(2, 3) ->");
    lua_run("local s = Osi.IntegerSum(2, 3); return tostring(s)");
    logf("lua test: Osi.GetModuleVersion('GustavX') ->");
    lua_run("return table.concat({Osi.GetModuleVersion('GustavX')}, '.')");
}

// Building a descriptor from zeroed memory crashes inside SetInteger, so
// prove invocation a different way: re-issue a query the engine just made,
// reusing its own descriptor list. Exists/IsSummon are pure predicates, so
// calling one twice is harmless, and nothing has to be constructed.
void test_requery(unsigned id, void* args) {
    const char* opt = std::getenv("BG3LE_TEST_CALL");
    if (opt == nullptr || opt[0] != '1') return;
    if (g_real_query == nullptr) return;

    // Exists(GUIDSTRING, INTEGER) and IsSummon(GUIDSTRING, INTEGER).
    if (id != 0x8000113au && id != 0x800019f2u) return;

    auto get_int = next<int (*)(const void*)>("_ZNK16COsiArgumentDesc10GetIntegerEv");
    std::uintptr_t next_node = 0;
    if (!safe_read(args, &next_node, sizeof(next_node)) || next_node == 0) return;
    const void* out = reinterpret_cast<const void*>(next_node);

    const int before = get_int != nullptr ? get_int(out) : -1;
    logf("requery: id=0x%08x out before = %d; re-invoking with engine's own args ...",
         id, before);
    long rc = g_real_query(static_cast<long>(id), reinterpret_cast<long>(args),
                           0, 0, 0, 0);
    logf("requery: rc=%ld out after = %d", rc, get_int != nullptr ? get_int(out) : -1);
}

// Whichever of InitGame / the first Event happens first does the work.
void dump_osiris_api(void* self);

std::atomic<bool> g_story_ready{false};

// The story work runs once per session, not once per process: when the
// client unloads a session (src/game_state.cpp) the Lua states are rebuilt
// and the next story's work binds Osi and loads the mods again.
std::mutex g_story_mutex;
std::atomic<bool> g_story_done{false};

void dump_once(void* self) {
    if (g_story_done.load(std::memory_order_acquire)) return;
    const std::lock_guard<std::mutex> held(g_story_mutex);
    if (g_story_done.load(std::memory_order_relaxed)) return;
    {
        ensure_symbols();
        fast_alloc_report("level load");
        if (g_story_ready_at > 0.0) {
            statusf("Level load took %.1fs after Osiris finished (%lu spin yields)",
                    now_s() - g_story_ready_at,
                    g_yields.load(std::memory_order_relaxed));
        }
        dump_osiris_api(self);
        test_integer_sum();
        g_story_ready.store(true);
    }
    g_story_done.store(true, std::memory_order_release);
}

}  // namespace

// Whether a session's story has been worked (Osi bound, mods loaded).
bool story_ready() { return g_story_ready.load(); }

// The client is unloading a session (src/game_state.cpp). True if one had
// got as far as its story, in which case the story work is re-armed for the
// next one and the caller rebuilds the Lua states.
bool note_session_ended() {
    if (!g_story_ready.exchange(false)) return false;
    const std::lock_guard<std::mutex> held(g_story_mutex);
    g_story_done.store(false, std::memory_order_release);
    return true;
}

namespace {

void dump_osiris_api(void* self) {
    // Every phase here runs on the story thread between Osiris finishing
    // and the level being up, so each one is time the player waits. They
    // are timed individually because "level load got slower" is otherwise
    // a guess -- and twice now the cause has been something added here.
    const double storyStarted = now_s();
    double mark = storyStarted;
    auto const phase = [&mark](char const* what) {
        const double took = now_s() - mark;
        mark = now_s();
        if (took >= 0.05) statusf("story: %s took %.2fs", what, took);
    };

    auto gen = next<long (*)(void*)>("_ZN7COsiris20GenerateFunctionListEv");
    if (gen != nullptr) gen(self);  // mappings are generated on demand
    phase("GenerateFunctionList");

    auto get_types = next<GetMappings>("_ZN7COsiris15GetTypeMappingsEPP11MappingInfoPj");
    auto get_funcs = next<GetMappings>("_ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj");
    if (get_types == nullptr || get_funcs == nullptr) {
        logf("osiris: mapping getters unresolved");
        return;
    }

    MappingInfo* types = nullptr;
    unsigned type_count = 0;
    get_types(self, &types, &type_count);

    std::unordered_map<std::uint32_t, std::string> type_names;
    for (unsigned i = 0; i < type_count; ++i) type_names[types[i].id] = name_of(types[i]);

    MappingInfo* funcs = nullptr;
    unsigned func_count = 0;
    get_funcs(self, &funcs, &func_count);
    statusf("StoryLoaded(): %u Osiris functions, %u types", func_count, type_count);
    // The engine's settings object is up by now; the overlay reads its language.
    bg3le_global_switches();

    char path[4096];
    const char* out = std::getenv("BG3LE_OSI_DUMP");
    if (out != nullptr) {
        std::snprintf(path, sizeof(path), "%s", out);
    } else {
        std::snprintf(path, sizeof(path), "/tmp/bg3le-osi.%d.txt", (int)::getpid());
    }
    std::FILE* f = std::fopen(path, "w");
    if (f != nullptr) {
        for (const auto& t : type_names) std::fprintf(f, "type %u %s\n", t.first, t.second.c_str());
        for (unsigned i = 0; i < func_count; ++i) {
            const MappingInfo& m = funcs[i];
            std::fprintf(f, "func 0x%08x %s(", m.id, name_of(m).c_str());
            for (unsigned p = 0; p < m.num_params; ++p) {
                std::uint8_t t = 0;
                safe_read(m.param_types + p, &t, 1);
                std::fprintf(f, "%s", p ? ", " : "");
                auto it = type_names.find(t);
                if (it != type_names.end()) {
                    std::fprintf(f, "%s", it->second.c_str());
                } else if (const char* b = base_type_name(t)) {
                    std::fprintf(f, "%s", b);
                } else {
                    std::fprintf(f, "?%u", t);
                }
            }
            std::fprintf(f, ")\n");
        }
        std::fclose(f);
        logf("osiris: wrote %s", path);
    }

    std::vector<osi::Function> bindable;
    bindable.reserve(func_count);
    for (unsigned i = 0; i < func_count; ++i) {
        const MappingInfo& m = funcs[i];
        osi::Function fn;
        fn.name = name_of(m);
        fn.id = m.id;
        fn.params.reserve(m.num_params);
        for (unsigned p = 0; p < m.num_params; ++p) {
            std::uint8_t t = 0;
            safe_read(m.param_types + p, &t, 1);
            fn.params.push_back(t);
        }
        bindable.push_back(std::move(fn));
    }
    // Osiris knows which parameters are outputs; without this the split is
    // inferred from how many arguments the caller passed.
    // The story's version identifies the compiled story, and the function
    // database is a property of it, so the signature walk's answer can be
    // cached under it rather than repeated at every level load.
    char storyVersion[128] = {};
    auto version = next<long (*)(void const*, char*)>(
        "_ZNK7COsiris21GetStoryVersionStringEPc");
    if (version != nullptr) version(self, storyVersion);

    bool signaturesCached = false;
    const std::size_t typed = osi::load_out_param_counts(
        &bindable, storyVersion, &signaturesCached);
    statusf("Osiris signatures: out-params for %zu of %zu functions, %s",
            typed, bindable.size(),
            signaturesCached ? "from the store for this story"
                             : "by walking the function database");
    phase("the signature walk");

    // The story's own procedures, user queries and databases. The engine's
    // mapping lists only what the engine implements; everything a goal
    // script declares -- Proc_CharacterFullRestore and the rest of what
    // mods actually call -- lives in Osiris' function database instead.
    std::vector<osi::Function> const story = osi::story_functions(bindable);
    phase("reading the story's own functions");
    if (!story.empty()) {
        statusf("Osiris: %zu story-defined functions (procedures, queries, "
                "databases)", story.size());
        bindable.insert(bindable.end(), story.begin(), story.end());
    } else if (osi::story_function_count() > 0) {
        // These carry no dispatch handle, so they are not bound here: they
        // are run by inserting a tuple into their node, and each resolves
        // the first time its name is used. Said because the count is worth
        // knowing, not because anything is missing.
        statusf("Osiris: %zu story-defined functions (procedures, events and "
                "databases), run by node insertion and resolved on first use",
                osi::story_function_count());
    }

    if (!achievement_patch_disabled() &&
        bytes_match(target::IsModded(), kIsModdedPatch, sizeof(kIsModdedPatch))) {
        // Same point at which bg3se reports it.
        statusf("Modded achievements enabled");
    }

    lua_bind_osi(bindable);
    phase("binding Osi.*");

    // Before the mods, not after: whatever they ask for on load has to be
    // there already.
    //
    // The mod list is re-resolved first: the manager the main menu had is
    // not necessarily the one a loaded game uses. That goes through the
    // recorded pointers, not a scan -- forcing a scan here cost fifteen
    // seconds of level load.
    bg3le_mods_rescan();
    run_searches_now("story");
    phase("the searches");

    lua_load_mods();  // after Osi, so a mod's load-time code can call it

    // BG3LE_PROBE_INTERN=1: place one FixedString during the level load and
    // touch nothing else. Writing a string attribute during a load sends
    // the engine into a grind that the same write, made from the console
    // after the load, does not -- so this asks whether placing the entry is
    // what does it, with no pool slot and no attribute involved.
    // 1 places a string-table entry, 2 also takes a pool slot, 3 also
    // points a stat's attribute at it. Each step is the previous one plus
    // one thing, which is how the step that upsets the engine gets named.
    char const* probeIntern = std::getenv("BG3LE_PROBE_INTERN");
    if (probeIntern != nullptr) {
        const int level = std::atoi(probeIntern);
        char const* text =
            "IF(IsClericCantrip()):DamageBonus(max(0, WisdomModifier));"
            "IF(SpellId('Target_TollTheDead')):DamageBonus(max(0, "
            "WisdomModifier))";

        unsigned int id = 0;
        const bool placed = bg3le_fixed_string_intern(text, &id);
        logf("strings: probe level %d: intern -> %s, id %#x", level,
             placed ? "placed" : "refused", id);

        if (level >= 2) {
            const int slot = bg3le_stats_string_intern(text);
            logf("strings: probe level %d: pool slot -> %d", level, slot);

            if (level >= 3 && slot > 0) {
                void const* object = bg3le_stats_find("PotentSpellcasting");
                bool written = false;
                if (object != nullptr) {
                    const std::size_t count =
                        bg3le_stats_attr_count(object);
                    for (std::size_t i = 0; i < count; ++i) {
                        char const* name = nullptr;
                        if (!bg3le_stats_attr_at(object, i, &name, nullptr,
                                                 nullptr, nullptr)
                            || name == nullptr
                            || std::strcmp(name, "Boosts") != 0) {
                            continue;
                        }
                        written = bg3le_stats_attr_set(object, i, slot);
                        break;
                    }
                }
                logf("strings: probe level %d: attribute write -> %s", level,
                     written ? "done" : "not done");
            }
        }
    }

    // BG3LE_PROBE_STRINGS=1: work back from a string Osiris certainly holds
    // to the pool that interns it. The host character's UUID comes back
    // through the DIV boundary as plain text, and the same string is in
    // Osiris' own storage as a handle.
    if (std::getenv("BG3LE_PROBE_STRINGS") != nullptr) {
        std::string host;
        std::string error;
        lua_eval("return Osi.GetHostCharacter()", &host, &error);
        if (!error.empty()) {
            logf("strings: GetHostCharacter failed: %s", error.c_str());
        } else {
            logf("strings: host character is %s", host.c_str());
            osi::probe_strings(host.c_str());
        }
    }
    phase("loading mods");

    const double total = now_s() - storyStarted;
    if (total >= 0.05) {
        statusf("story: bg3le used %.2fs of the level load", total);
    }

    auto free_types = next<FreeMappings>("_ZN7COsiris16FreeTypeMappingsEP11MappingInfoj");
    auto free_funcs = next<FreeMappings>("_ZN7COsiris20FreeFunctionMappingsEP11MappingInfoj");
    if (free_types != nullptr) free_types(self, types, type_count);
    if (free_funcs != nullptr) free_funcs(self, funcs, func_count);
}

}  // namespace
}  // namespace bg3le

extern char** environ;

using namespace bg3le;

// ---- Osiris interposition ----

extern "C" long _ZN7COsiris8InitGameEv(void* self) {
    static auto real = next<long (*)(void*)>("_ZN7COsiris8InitGameEv");
    logf("COsiris::InitGame() self=%p", self);
    long rc = real != nullptr ? real(self) : 0;
    dump_once(self);
    return rc;
}

extern "C" long _ZN7COsiris20RegisterDIVFunctionsEP19TOsirisInitFunction(
    void* self, void* init_fn) {
    static auto real = next<long (*)(void*, void*)>(
        "_ZN7COsiris20RegisterDIVFunctionsEP19TOsirisInitFunction");
    logf("COsiris::RegisterDIVFunctions() self=%p init=%p", self, init_fn);
    ensure_symbols();  // game is initialised by now; its allocator is usable
    std::call_once(g_init_struct_once, [init_fn] { dump_init_struct(init_fn); });
    void* table = maybe_wrap_div_table(init_fn);
    ensure_achievement_gate_patch();
    return real != nullptr ? real(self, table) : 0;
}

extern "C" long _ZN7COsiris5EventEjP16COsiArgumentDesc(
    void* self, unsigned event_id, void* args) {
    static auto real = next<long (*)(void*, unsigned, void*)>(
        "_ZN7COsiris5EventEjP16COsiArgumentDesc");

    debug_server_note_story_thread();  // Osiris runs on the story thread
    dump_once(self);  // first event means the story is up
    debug_server_pump();

    static unsigned long seen = 0;
    if (++seen <= 5) logf("COsiris::Event(%u) args=%p", event_id, args);
    return real != nullptr ? real(self, event_id, args) : 0;
}

// ---- pump ----
//
// COsiris::Event only fires when the story is active, so an idle game
// starves the request queue. NoStoryLoaded is a trivial const query the game
// imports; instrument its rate to see whether it ticks regularly.

extern "C" long _ZNK7COsiris13NoStoryLoadedEv(void* self) {
    static auto real = next<long (*)(void*)>("_ZNK7COsiris13NoStoryLoadedEv");

    static std::atomic<unsigned long> calls{0};
    static double last = 0.0;

    const unsigned long n = ++calls;
    const double t = now_s();
    if (last == 0.0) last = t;
    if (t - last >= 5.0) {
        logf("pump: NoStoryLoaded %.1f calls/s", n / (t - last));
        calls.store(0);
        last = t;
    }

    debug_server_pump();
    return real != nullptr ? real(self) : 0;
}

// ---- story load timing ----
//
// A 72s stall sits between story registration and the first Osiris event on
// the native build but not under Proton. These four are the story-loading
// entry points, so timing them localises it.

extern "C" long _ZN7COsiris4LoadER12COsiSmartBuf(void* self, void* buf) {
    static auto real = next<long (*)(void*, void*)>("_ZN7COsiris4LoadER12COsiSmartBuf");
    debug_server_note_story_thread();
    // The engine calls this more than once per session -- the base story and
    // then the save's own -- so the line says which, rather than looking
    // like the same event logged twice.
    static int loads = 0;
    const int which = ++loads;
    double t0 = now_s();
    long rc = real != nullptr ? real(self, buf) : 0;
    // A new session's story arrives after the client unloaded the last one,
    // which rebuilt the Lua states. One arriving without that is not
    // expected; it keeps the old state and says so once.
    if (g_story_ready.load()) {
        static std::once_flag told;
        std::call_once(told, [] {
            statusf("bg3le: this session is using the Lua state and mod "
                    "scripts from the previous one; restart the game when "
                    "changing saves");
        });
    }

    // bg3se reports the node count here too; it is the size of the story
    // the game just loaded, and a useful thing to see change.
    if (osi::node_count() > 0) {
        statusf("COsiris::Load #%d: story loaded in %.2fs, %zu nodes", which,
                now_s() - t0, osi::node_count());
    } else {
        statusf("COsiris::Load #%d: story loaded in %.2fs", which,
                now_s() - t0);
    }

    g_story_ready_at = now_s();
    start_stall_profile();

    // Sample mid-stall: every thread is parked, so this should show what on.
    if (const char* e = std::getenv("BG3LE_STACKDUMP")) {
        if (e[0] == '1') {
            schedule_stack_dump(20.0, "mid level load");
            schedule_stack_dump(40.0, "mid level load");
        }
    }
    return rc;
}

extern "C" long _ZN7COsiris7CompileEPKwS1_(void* self, const wchar_t* a, const wchar_t* b) {
    static auto real = next<long (*)(void*, const wchar_t*, const wchar_t*)>(
        "_ZN7COsiris7CompileEPKwS1_");
    double t0 = now_s();
    long rc = real != nullptr ? real(self, a, b) : 0;
    logf("COsiris::Compile took %.2fs", now_s() - t0);
    return rc;
}

extern "C" long _ZN7COsiris5MergeEPKw(void* self, const wchar_t* a) {
    static auto real = next<long (*)(void*, const wchar_t*)>("_ZN7COsiris5MergeEPKw");
    double t0 = now_s();
    statusf("COsiris::Merge: started");
    long rc = real != nullptr ? real(self, a) : 0;
    statusf("COsiris::Merge: finished in %.2fs", now_s() - t0);
    return rc;
}

extern "C" long _ZN7COsiris12PrepareMergeEPKw(void* self, const wchar_t* a) {
    static auto real = next<long (*)(void*, const wchar_t*)>("_ZN7COsiris12PrepareMergeEPKw");
    double t0 = now_s();
    long rc = real != nullptr ? real(self, a) : 0;
    logf("COsiris::PrepareMerge took %.2fs", now_s() - t0);
    return rc;
}

// ---- Steam achievement diagnostics ----
//
// bg3's own PLT has zero relocations against
// SteamAPI_ISteamUserStats_SetAchievement/StoreStats (confirmed via
// `readelf -r`) -- it is a normal C++ Steamworks SDK consumer, not a flat-API
// one. It gets its ISteamUserStats* through
// SteamInternal_FindOrCreateUserInterface(hSteamUser, "STEAMUSERSTATS_..."]
// (this symbol *is* PLT-imported, many call sites) and then calls straight
// through the interface's vtable. libsteam_api.so's own
// SteamAPI_ISteamUserStats_SetAchievement is just `jmp [rax+0x38]` on that
// same object (confirmed by disassembly), i.e. slot index 7 -- so that is the
// slot to patch, not a symbol to interpose.

namespace {

bool hooked_set_achievement(void* self, const char* name) {
    logf("ISteamUserStats::SetAchievement(%p, \"%s\")", self,
         name != nullptr ? name : "(null)");
    // Every thread's stack, which is how the engine's own gate was located
    // in the first place -- but far too much to log on a live call.
    if (std::getenv("BG3LE_DUMP_ACHIEVEMENT_STACKS") != nullptr) {
        dump_all_thread_stacks("SetAchievement call");
    }
    auto real = g_real_set_achievement.load();
    bool rc = real != nullptr ? real(self, name) : false;
    logf("ISteamUserStats::SetAchievement -> %s", rc ? "true" : "false");
    return rc;
}

// vtable is a shared, effectively-static table (one C++ class, one set of
// thunks) -- patch it once, the first time an ISteamUserStats interface
// pointer is seen, rather than on every FindOrCreateUserInterface call.
void maybe_hook_user_stats_vtable(void* iface) {
    if (iface == nullptr) return;
    bool expected = false;
    if (!g_user_stats_vtable_patched.compare_exchange_strong(expected, true)) return;

    void** vtable = *reinterpret_cast<void***>(iface);
    void** slot = vtable + 7;  // +0x38 -- SetAchievement, per the flat-API thunk

    const long page = ::sysconf(_SC_PAGESIZE);
    const auto addr = reinterpret_cast<std::uintptr_t>(slot);
    auto* page_start = reinterpret_cast<void*>(addr & ~(std::uintptr_t)(page - 1));
    const std::size_t span = (addr + sizeof(void*)) - (std::uintptr_t)page_start;
    if (::mprotect(page_start, span, PROT_READ | PROT_WRITE) != 0) {
        logf("steam hook: mprotect failed for ISteamUserStats vtable %p", vtable);
        g_user_stats_vtable_patched.store(false);
        return;
    }
    g_real_set_achievement.store(reinterpret_cast<SetAchievementFn>(*slot));
    *slot = reinterpret_cast<void*>(&hooked_set_achievement);
    ::mprotect(page_start, span, PROT_READ);

    logf("steam hook: ISteamUserStats vtable %p, SetAchievement slot -> %p (was %p)",
         vtable, (void*)&hooked_set_achievement, (void*)g_real_set_achievement.load());
}

}  // namespace

extern "C" void* SteamInternal_FindOrCreateUserInterface(int32_t hSteamUser,
                                                          const char* version) {
    static auto real = next<void* (*)(int32_t, const char*)>(
        "SteamInternal_FindOrCreateUserInterface");
    void* iface = real != nullptr ? real(hSteamUser, version) : nullptr;
    logf("SteamInternal_FindOrCreateUserInterface(\"%s\") -> %p",
         version != nullptr ? version : "(null)", iface);
    // Case-insensitive: the real constant is all-caps
    // ("STEAMUSERSTATS_INTERFACE_VERSIONxxx"), unlike this match string.
    if (version != nullptr && ::strcasestr(version, "UserStats") != nullptr) {
        maybe_hook_user_stats_vtable(iface);
    }
    return iface;
}

// ---- entry point ----

// The game writes a ModCrashSanityCheck directory in the profile while it
// runs and removes it on a clean exit; finding it at startup is how it
// decides the last run crashed, and it disables mods when it does. bg3se
// removes it for the same reason (CleanupSanityCheck in
// ScriptExtenderClient.cpp), and bg3le kills the game often enough in
// testing to leave it behind every time.
void cleanup_sanity_check() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return;

    // BG3LE_KEEP_SANITY_CHECK=1 leaves it alone, which is how the effect
    // was attributed: with the marker in place the engine loads 14
    // modules, without it 69.
    char const* keep = std::getenv("BG3LE_KEEP_SANITY_CHECK");
    if (keep != nullptr && keep[0] == '1') {
        logf("Kept ModCrashSanityCheck (BG3LE_KEEP_SANITY_CHECK=1)");
        return;
    }

    const std::string path = std::string(home)
                             + "/.local/share/Larian Studios/Baldur's Gate 3"
                             + "/ModCrashSanityCheck";
    if (::rmdir(path.c_str()) == 0) {
        logf("Removed ModCrashSanityCheck");
    }
}

__attribute__((constructor)) static void bg3le_init() {
    log_init();
    logf("bg3le loaded");
    if (bg3le::log_previous_crash()[0] != '\0') {
        logf("the previous run crashed; its log is %s", bg3le::log_previous_crash());
    }
    // LD_PRELOAD reaches every process the game starts -- the console's
    // launcher, the crash reporter -- and engine offsets mean nothing there.
    if (!bg3le::host_is_game()) return;

    cleanup_sanity_check();
    // Before main and the fork, so load caches see it.
    bg3le::ensure_achievement_gate_patch();

    // Before the launch's save is read, so its PersistentVars are seen.
    bg3le::install_savegame_hook();
    // Before the module loads, so its exit is seen (the menu line, client mods).
    bg3le::install_game_state_hook();
    bg3le::install_entity_trace_hook();

    // BG3LE_STACKDUMP_AT=<seconds>: every thread's stack, that long after load.
    if (const char* at = std::getenv("BG3LE_STACKDUMP_AT")) {
        bg3le::schedule_stack_dump(std::atof(at), "BG3LE_STACKDUMP_AT");
    }

    // Before the game creates its Vulkan instance, which is what the
    // overlay's first hook is on. Off with BG3LE_IMGUI=0.
    bg3le::imgui_overlay_start();

    // Symbol loading is deferred to the first Osiris callback: allocating
    // here runs before the game's allocator exists.
    logf("waiting for game init");
}
