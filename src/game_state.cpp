// Client game state, for what upstream's ecl::ScriptExtender does with it:
// the menu line and the client's mod bootstraps as the game leaves
// LoadModule, GameStateChanged, and a client tick every frame.
//
// - The state is EoCClient->GameStateMachine->State, as upstream reads it.
// - LoadModule's Exit is ecl::GameStateLoadModule's vtable slot 7.
// - The per-frame call is the client's state-queue update, which EoCClient's
//   update calls once a frame; upstream's ticks come from the same place.
// - ecl::GameStateMachine::SetTargetState (it logs "ecl::GameStateQueued:
//   %s") is hooked only to log what is queued.

#include "game_state.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <string>

#include "hook.h"
#include "log.h"
#include "resolve.h"
#include "lua_host.h"
#include "mem.h"
#include "net.h"
#include "stackdump.h"

extern "C" void bg3le_stats_module_loaded();
extern "C" void bg3le_templates_invalidate();
extern "C" void bg3le_stats_load_hooked();
extern "C" void bg3le_stats_loading(bool loading);
extern "C" void bg3le_path_override_clear();
#include "targets.h"

namespace bg3le {

void translated_string_show_version(char const* suffix);
bool note_session_ended();
void plugins_dispatch_frame();  // src/plugins.cpp

namespace {

// Addresses from targets.h: SetTargetState; ecl::GameStateLoadModule's vtable
// slot 7 (Exit) and what it holds (slot 4 is Enter, which starts the load; 5
// and 6 run every frame until it is done); ecl::GameStateMachine::Update, which
// runs the current state's per-frame slot and switches to the next queued state
// once a frame; and the state names, char const* names[35] in .data.rel.ro.
constexpr unsigned char kMachineUpdatePrologue[] = {
    0x41, 0x56, 0x53, 0x50, 0x80, 0x7f, 0x08, 0x00, 0x75, 0x08};
// names[] holds 35 on v4.76. The cap only bounds the index: each entry is
// checked to be a short printable name before it is used, so a table that
// changed length reads "Unknown", never a stray pointer.
constexpr int kStateLimit = 64;

constexpr std::size_t kStateId = 0x10;

// ecl::EoCClient* (target::EoCClient) and its GameStateMachine {CurrentState,
// State}: upstream's GetClientState. At +0x88 here, a field earlier than on Windows.
constexpr std::size_t kClientMachine = 0x88;
constexpr std::size_t kMachineState = 0x08;

using SetTargetStateProc = void (*)(void*, void*);
SetTargetStateProc g_set_target_state = nullptr;
// State methods may return a result -- Enter returns whether it succeeded,
// and dropping it made the engine reject the load order -- so arguments and
// result pass straight through.
using StateProc = std::uint64_t (*)(void*, void*, void*, void*);
StateProc g_load_module_exit = nullptr;
std::atomic<bool> g_left_load_module{false};

// The GameTime each side's update is handed, laid out as bg3se::GameTime.
// Upstream's timers, Tick and Ext.Timer.GameTime all run on it.
// TODO: both builds lay it out {double Time; float DeltaTime; float DeltaTime2;
// int32 Ticks}, so upstream's Ticks is +0x0c's float bits and its Unknown holds
// the real tick count. Kept as upstream reads it until bg3se fixes the struct.
struct EngineTime {
    double Time;
    float DeltaTime;
    std::int32_t Ticks;
    double Unknown;
};
static_assert(sizeof(EngineTime) == 24);

struct EngineClock {
    std::mutex lock;
    EngineTime time{};
    bool hooked = false;
    bool seen = false;
};
EngineClock g_clocks[2];  // client, server

void note_engine_time(EngineClock& clock, void const* time, char const* side) {
    if (time == nullptr) return;
    EngineTime now;
    std::memcpy(&now, time, sizeof(now));
    bool first = false;
    {
        const std::lock_guard<std::mutex> held(clock.lock);
        first = !clock.seen;
        clock.time = now;
        clock.seen = true;
    }
    if (first) logf("gametime: the %s's clock reads %.3f s, delta %.4f s", side, now.Time, now.DeltaTime);
}

// Upstream's client tick is a post-hook on GameStateMachine::Update, which
// is inlined here; bg3le ticks later in this call, so the time is noted on entry.
StateProc g_client_app_update = nullptr;
std::uint64_t client_app_update_hook(void* app, void* time, void* c, void* d) {
    note_engine_time(g_clocks[0], time, "client");
    return g_client_app_update(app, time, c, d);
}

// Upstream's server tick is a pre-hook here.
StateProc g_server_machine_update = nullptr;
std::uint64_t server_machine_update_hook(void* machine, void* time, void* c, void* d) {
    note_engine_time(g_clocks[1], time, "server");
    return g_server_machine_update(machine, time, c, d);
}

char const* state_name(std::uint32_t state) {
    if (state >= kStateLimit || target::StateNames() == 0) return "Unknown";
    auto* const* names = reinterpret_cast<char const* const*>(load_bias() + target::StateNames());
    char const* name = nullptr;
    char text[48];
    if (!safe_read(&names[state], &name, sizeof(name)) || name == nullptr
        || !safe_cstr(name, text, sizeof(text)) || text[0] == '\0') {
        return "Unknown";
    }
    for (char const* c = text; *c != '\0'; ++c) {
        if (*c < 0x20 || *c > 0x7e) return "Unknown";
    }
    return name;
}

bool state_id(void* state, std::uint32_t* out) {
    return state != nullptr
        && safe_read(static_cast<char*>(state) + kStateId, out, sizeof(*out));
}

// ecl::ScriptExtender::ShowVersionNumber, queued until the copyright
// string is in the repository.
void show_version_number() {
    std::string text = "\r\nbg3le loaded, Script Extender v32 API, built on " __DATE__
                       " " __TIME__ ".";
    if (log_previous_crash()[0] != '\0') {
        text += "\r\nThe last run crashed. Its log is ";
        text += log_previous_crash();
    }
    translated_string_show_version(text.c_str());
}

std::uint64_t load_module_exit_hook(void* state, void* a, void* b, void* c) {
    const std::uint64_t result = g_load_module_exit(state, a, b, c);
    if (!g_left_load_module.exchange(true)) {
        logf("gamestate: client left LoadModule");
        install_crash_handler();
        show_version_number();
        lua_load_client_scripts();
    }
    return result;
}

StateProc g_machine_update = nullptr;

// Every frame is a client tick, and a state that differs from the last
// frame's is GameStateChanged -- upstream compares around its update call.
std::uint64_t machine_update_hook(void* machine, void* a, void* b, void* c) {
    static std::atomic<bool> first{true};
    if (first.exchange(false)) logf("gamestate: client frames are ticking");
    const std::uint64_t result = g_machine_update(machine, a, b, c);
    net_client_tick();

    static char const* last = nullptr;
    char const* now = client_game_state();
    if (now != nullptr && last != nullptr && now != last) {
        logf("gamestate: client %s -> %s", last, now);
        // Upstream's client resets on UnloadSession and loads again leaving
        // LoadMenu; its server resets there too and loads at the next
        // LoadSession, which is the story work here.
        // Upstream's client ResetExtensionState drops the path overrides here.
        if (std::strcmp(now, "UnloadSession") == 0) bg3le_path_override_clear();
        if (std::strcmp(now, "UnloadSession") == 0 && note_session_ended()) {
            logf("gamestate: session unloaded; rebuilding the Lua states");
            lua_reset(false);
        }
        if (std::strcmp(last, "LoadModule") == 0) {
            bg3le_stats_module_loaded();
            bg3le_templates_invalidate();
        }
        // The engine frees its root templates from here until the load is done.
        if (std::strcmp(now, "UnloadModule") == 0) bg3le_templates_invalidate();
        if (std::strcmp(last, "LoadMenu") == 0) {
            show_version_number();
            lua_load_client_scripts();
        }
        char const* from = last;
        last = now;
        lua_client_tick(from, now);
    } else {
        if (now != nullptr) last = now;
        lua_client_tick(nullptr, nullptr);
    }
    plugins_dispatch_frame();
    return result;
}

using StatsLoadProc = std::uint64_t (*)(void*, void*, void*, void*);
StatsLoadProc g_stats_load = nullptr;

// RPGStats::Load(paths), on the worker the client schedules it on.
std::uint64_t stats_load_hook(void* paths, void* a, void* b, void* c) {
    static std::atomic<unsigned> loads{0};
    const unsigned load = ++loads;
    char thread[32] = "?";
    pthread_getname_np(pthread_self(), thread, sizeof(thread));
    char const* client = client_game_state();
    char const* server = server_game_state();
    logf("stats: RPGStats::Load starts (load #%u on thread %s; client %s, server %s)", load, thread,
         client != nullptr ? client : "unknown", server != nullptr ? server : "unknown");
    lua_module_load_started();
    bg3le_stats_loading(true);
    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t result = g_stats_load(paths, a, b, c);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    logf("stats: RPGStats::Load done (load #%u, %.2f s)", load, secs);
    bg3le_stats_module_loaded();
    lua_stats_loaded();
    bg3le_stats_loading(false);
    return result;
}

void set_target_state_hook(void* machine, void* state) {
    std::uint32_t to = 0;
    if (state_id(state, &to)) {
        void* vtable = *static_cast<void**>(state);
        logf("gamestate: client queued %s (vtable image+%#lx)", state_name(to),
             (unsigned long)(reinterpret_cast<std::uintptr_t>(vtable) - load_bias()));
    }
    g_set_target_state(machine, state);
}

}  // namespace

// The last GameTime one side's update was handed: 0 if that update is not
// hooked, 1 before it first runs, 2 with *time, *delta, *ticks and *unknown set.
extern "C" int bg3le_engine_time(bool client, double* time, float* delta, int* ticks,
                                 double* unknown) {
    EngineClock& clock = g_clocks[client ? 0 : 1];
    const std::lock_guard<std::mutex> held(clock.lock);
    if (!clock.hooked) return 0;
    if (!clock.seen) return 1;
    *time = clock.time.Time;
    *delta = clock.time.DeltaTime;
    *ticks = clock.time.Ticks;
    *unknown = clock.time.Unknown;
    return 2;
}

char const* client_state_name() {
    char const* state = client_game_state();
    return state != nullptr ? state : "unknown";
}

char const* client_game_state() {
    void* client = nullptr;
    void* machine = nullptr;
    std::uint32_t state = 0;
    if (target::EoCClient() == 0
        || !safe_read(reinterpret_cast<void*>(load_bias() + target::EoCClient()), &client,
                   sizeof(client))
        || client == nullptr
        || !safe_read(static_cast<char*>(client) + kClientMachine, &machine,
                      sizeof(machine))
        || machine == nullptr
        || !safe_read(static_cast<char*>(machine) + kMachineState, &state,
                      sizeof(state))) {
        return nullptr;
    }
    return state_name(state);
}

// esv::GameState, in upstream's order (Enumerations/Engine.inl).
constexpr char const* kServerStates[] = {
    "Unknown", "Uninitialized", "Init", "Idle", "Exit", "LoadLevel",
    "LoadModule", "LoadSession", "UnloadLevel", "UnloadModule",
    "UnloadSession", "Sync", "Paused", "Running", "Save", "Disconnect",
    "BuildStory", "ReloadStory"};
// EoCServer::GameStateMachine, and its State, as upstream lays them out;
// checked live (Running reads 13).
constexpr std::size_t kServerMachine = 0xa0;
constexpr std::size_t kServerMachineState = 0x10;

char const* server_game_state() {
    void* server = nullptr;
    void* machine = nullptr;
    std::uint32_t state = 0;
    if (target::EoCServer() == 0
        || !safe_read(reinterpret_cast<void*>(load_bias() + target::EoCServer()), &server,
                      sizeof(server))
        || server == nullptr
        || !safe_read(static_cast<char*>(server) + kServerMachine, &machine,
                      sizeof(machine))
        || machine == nullptr
        || !safe_read(static_cast<char*>(machine) + kServerMachineState, &state,
                      sizeof(state))
        || state >= sizeof(kServerStates) / sizeof(kServerStates[0])) {
        return nullptr;
    }
    return kServerStates[state];
}

void install_game_state_hook() {
    void* original = nullptr;
    if (hook_slot(target::LoadModuleExitSlot(), target::LoadModuleExit(),
                  reinterpret_cast<void*>(&load_module_exit_hook), &original)) {
        g_load_module_exit = reinterpret_cast<StateProc>(original);
    } else {
        logf("gamestate: LoadModule's Exit is not where it was; the menu line "
             "and early client mods are off");
    }

    const std::uintptr_t machine_update = target::MachineUpdate();
    if (code_near(machine_update, kMachineUpdatePrologue) != 0
        && hook_call_sites(machine_update,
                           reinterpret_cast<void*>(&machine_update_hook),
                           &original) > 0) {
        g_machine_update = reinterpret_cast<StateProc>(original);
    } else {
        logf("gamestate: ecl::GameStateMachine::Update not found; the client "
             "ticks with the server");
    }

    if (hook_slot(target::ClientAppUpdateSlot(), target::ClientAppUpdate(),
                  reinterpret_cast<void*>(&client_app_update_hook), &original)) {
        g_client_app_update = reinterpret_cast<StateProc>(original);
        const std::lock_guard<std::mutex> held(g_clocks[0].lock);
        g_clocks[0].hooked = true;
    } else {
        logf("gametime: ecl::EoCClient::Update not found; client timers run on the monotonic clock");
    }
    const std::uintptr_t server_update = target::ServerMachineUpdate();
    if (server_update != 0
        && hook_call_sites(server_update, reinterpret_cast<void*>(&server_machine_update_hook),
                           &original) > 0) {
        g_server_machine_update = reinterpret_cast<StateProc>(original);
        const std::lock_guard<std::mutex> held(g_clocks[1].lock);
        g_clocks[1].hooked = true;
    } else {
        logf("gametime: esv::GameStateMachine::Update not found; server timers run on the monotonic clock");
    }

    const std::uintptr_t stats_load = target::StatsLoad();
    if (stats_load != 0
        && hook_call_sites(stats_load, reinterpret_cast<void*>(&stats_load_hook),
                           &original) > 0) {
        g_stats_load = reinterpret_cast<StatsLoadProc>(original);
        bg3le_stats_load_hooked();
    } else {
        logf("gamestate: RPGStats::Load not found; StatsLoaded fires at story load");
    }

    // Its prologue loads a global rip-relatively, so the bytes differ in every
    // build; the target's pattern wildcards that.
    const std::uintptr_t set_target_state = target::SetTargetState();
    if (set_target_state == 0) {
        logf("gamestate: ecl::GameStateMachine::SetTargetState not found");
        return;
    }
    if (hook_call_sites(set_target_state,
                        reinterpret_cast<void*>(&set_target_state_hook),
                        &original) > 0) {
        g_set_target_state = reinterpret_cast<SetTargetStateProc>(original);
    }
}

}  // namespace bg3le
