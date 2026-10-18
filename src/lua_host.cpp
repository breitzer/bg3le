#include "lua_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <exception>
#include <deque>
#include <set>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <optional>
#include <string>
#include <utility>

#include "debug_server.h"
#include "ecs_types.h"
#include "ecs_world.h"
#include "mem.h"
#include "game_state.h"
#include "noesis_ui.h"
#include "hook.h"
#include "savegame.h"
#include "stats_sets.h"
#include "log.h"
#include "net.h"
#include "targets.h"
#include "vendor/ls_string.h"
#include "vendor/mods.h"

// Norbyte's Lua fork is compiled as C++, as bg3se compiles it, so these must
// not be wrapped in extern "C" or the symbols will not match.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace bg3le {
namespace {

// Two Lua states, because the game is two contexts in one process and a
// mod's BootstrapServer.lua and BootstrapClient.lua are meant not to see
// each other: upstream runs an esv and an ecl ExtensionState, each with
// its own Lua, its own Mods table and its own Ext.
//
// g_lua is whichever one the current call is running in. Everything below
// works on "the current state", so the context is switched at the few
// entry points -- a tick, a mod load, an evaluation from the console --
// rather than threaded through six hundred call sites.
//
// The client context runs on the client's thread and the server's on the
// story thread, as upstream's do, so "current" is per thread and falls back
// to the server; each state has a lock, taken by InContext, as upstream's
// LuaServerPin/LuaClientPin take theirs.
thread_local lua_State* t_lua = nullptr;
lua_State* g_server_lua = nullptr;
lua_State* g_client_lua = nullptr;
std::recursive_mutex g_server_lock;
std::recursive_mutex g_client_lock;
#define g_lua (t_lua != nullptr ? t_lua : g_server_lua)

bool in_client_state() { return g_lua != nullptr && g_lua == g_client_lua; }

// Ext._Internal.IsClientState() -- what Ext.IsClient()/IsServer() report,
// since one prelude builds both states.
int l_is_client_state(lua_State* L) {
    lua_pushboolean(L, in_client_state() ? 1 : 0);
    return 1;
}
// Ext._Internal.SavedPersistentVars() -- {modId = json} from the last save
// read. TakeSavedPersistentVars() is the same once per read, else nil.
void push_saved_vars(lua_State* L,
                     std::vector<std::pair<std::string, std::string>> const& vars) {
    lua_createtable(L, 0, static_cast<int>(vars.size()));
    for (auto const& [mod, json] : vars) {
        lua_pushlstring(L, json.data(), json.size());
        lua_setfield(L, -2, mod.c_str());
    }
}

// Ext._Internal.GameState() -- this context's state machine, as upstream's
// Ext.Utils.GetGameState reports; nil where it is not known.
int l_game_state(lua_State* L) {
    char const* state = in_client_state() ? client_game_state() : server_game_state();
    if (state == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, state);
    }
    return 1;
}

int l_saved_persistent_vars(lua_State* L) {
    push_saved_vars(L, saved_persistent_vars());
    return 1;
}

// Ext._Internal.TakeSavedExtras() -- {user = {{owner, name, type, value}},
// mod = ..., timers = {{frozen, repeat, paused, handler, args}}}, once per
// save read, else nil.
void push_saved_variables(lua_State* L, std::vector<SavedVariable> const& vars) {
    lua_createtable(L, (int)vars.size(), 0);
    for (std::size_t i = 0; i < vars.size(); ++i) {
        SavedVariable const& v = vars[i];
        lua_createtable(L, 4, 0);
        lua_pushstring(L, v.Owner.c_str());
        lua_rawseti(L, -2, 1);
        lua_pushstring(L, v.Name.c_str());
        lua_rawseti(L, -2, 2);
        lua_pushinteger(L, v.Type);
        lua_rawseti(L, -2, 3);
        switch (v.Type) {
        case 5: lua_pushboolean(L, v.Bool ? 1 : 0); break;
        case 1: lua_pushinteger(L, (lua_Integer)v.Int); break;
        case 2: lua_pushnumber(L, v.Num); break;
        default: lua_pushlstring(L, v.Str.data(), v.Str.size()); break;
        }
        lua_rawseti(L, -2, 4);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
}

int l_take_saved_extras(lua_State* L) {
    SaveExtras extras;
    if (!take_saved_extras(&extras)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    push_saved_variables(L, extras.User);
    lua_setfield(L, -2, "user");
    push_saved_variables(L, extras.Mod);
    lua_setfield(L, -2, "mod");
    lua_createtable(L, (int)extras.Timers.size(), 0);
    for (std::size_t i = 0; i < extras.Timers.size(); ++i) {
        SavedTimer const& t = extras.Timers[i];
        lua_createtable(L, 5, 0);
        lua_pushnumber(L, t.Frozen);
        lua_rawseti(L, -2, 1);
        lua_pushnumber(L, t.Repeat);
        lua_rawseti(L, -2, 2);
        lua_pushboolean(L, t.Paused ? 1 : 0);
        lua_rawseti(L, -2, 3);
        lua_pushstring(L, t.Handler.c_str());
        lua_rawseti(L, -2, 4);
        lua_pushlstring(L, t.Args.data(), t.Args.size());
        lua_rawseti(L, -2, 5);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "timers");
    return 1;
}

int l_take_saved_persistent_vars(lua_State* L) {
    std::vector<std::pair<std::string, std::string>> vars;
    if (!take_saved_persistent_vars(&vars)) {
        lua_pushnil(L);
        return 1;
    }
    push_saved_vars(L, vars);
    return 1;
}

const SymbolTable* g_symbols = nullptr;

// Bound functions must outlive the closures that reference them, and the
// storage must not move once pointers are handed to Lua.
std::vector<osi::Function> g_functions;

// Osiris overloads by arity: ApplyStatus is declared with three, four and
// five parameters, and they are three different functions. One Lua name
// covers all of them, so a name maps to the group and the call picks the
// member whose input count matches -- which is what bg3se's name resolver
// does. Binding one function per name kept whichever came last and
// refused every other arity.
//
// A deque, because the closures hold pointers to the groups and Lua is
// handed them as they are built.
std::deque<std::vector<const osi::Function*>> g_overloads;

// Runs something in one context and puts the previous one back.
//
// Nested use is fine and happens: a client script can be loaded while the
// server context is the current one.
// Which context to enter. The state is read only once its lock is held: a
// reset on another thread swaps both states while holding both locks, and a
// pointer read before waiting for it is a freed state by the time it runs.
enum class Side { Server, Client };

class InContext {
public:
    explicit InContext(Side side)
        : was_(t_lua),
          lock_(side == Side::Client ? g_client_lock : g_server_lock),
          state_(side == Side::Client ? g_client_lua : g_server_lua) {
        if (state_ != nullptr) t_lua = state_;
    }
    ~InContext() { t_lua = was_; }

    InContext(InContext const&) = delete;
    InContext& operator=(InContext const&) = delete;

    // Whether the context has a state to run in.
    explicit operator bool() const { return state_ != nullptr; }
    lua_State* state() const { return state_; }

private:
    lua_State* was_;
    std::lock_guard<std::recursive_mutex> lock_;
    lua_State* state_;
};

bool to_value(lua_State* L, int idx, osi::Value* out) {
    switch (lua_type(L, idx)) {
        case LUA_TSTRING:
            out->type = osi::kString;
            out->text = lua_tostring(L, idx);
            return true;
        case LUA_TBOOLEAN:
            out->type = osi::kInteger;
            out->integer = lua_toboolean(L, idx);
            return true;
        case LUA_TNUMBER:
            out->integer = (std::int64_t)lua_tonumber(L, idx);
            out->real = lua_tonumber(L, idx);
            out->type = lua_isinteger(L, idx) ? osi::kInteger : osi::kReal;
            if (lua_isinteger(L, idx)) out->integer = lua_tointeger(L, idx);
            return true;
        default:
            return false;
    }
}

void push_value(lua_State* L, const osi::Value& v) {
    switch (v.type) {
        case osi::kString:
        case osi::kGuidString: lua_pushstring(L, v.text.c_str()); break;
        case osi::kReal: lua_pushnumber(L, v.real); break;
        default: lua_pushinteger(L, v.integer); break;
    }
}

// Osi.Name(...) for a function the story defines itself.
//
// These have no dispatch handle, so they are run by inserting a tuple
// into their node. The name is the upvalue rather than a resolved
// function, because Osiris keys by name and arity both: the same name can
// be declared with two different signatures, and which one is meant is
// decided by how many arguments arrive.
int osi_story_dispatch(lua_State* L) {
    char const* name = lua_tostring(L, lua_upvalueindex(1));
    const bool method = lua_toboolean(L, lua_upvalueindex(2)) != 0;
    const int first = method ? 2 : 1;
    const int argc = lua_gettop(L) - first + 1;

    std::vector<osi::Value> args;
    args.reserve(argc > 0 ? argc : 0);
    for (int i = first; i <= lua_gettop(L); ++i) {
        osi::Value v;
        if (!to_value(L, i, &v)) {
            return luaL_error(L, "Osi.%s: argument %d has unsupported type %s",
                              name, i - first + 1, luaL_typename(L, i));
        }
        args.push_back(std::move(v));
    }

    const std::string key = std::string(name) + "/" + std::to_string(argc);
    std::string why;
    const osi::Status status = osi::insert(key.c_str(), args, &why);
    if (status != osi::Status::kHandled) {
        return luaL_error(L, "Osi.%s: %s", name, why.c_str());
    }
    return 0;
}

// Osi.QRY_Name(inputs...) for a user query: its OUT values, nils when it
// fails, or a boolean when it has none -- upstream's OsiUserQuery.
int osi_story_query(lua_State* L) {
    char const* name = lua_tostring(L, lua_upvalueindex(1));
    const int argc = lua_gettop(L);

    std::vector<osi::Value> args;
    args.reserve(argc);
    for (int i = 1; i <= argc; ++i) {
        osi::Value v;
        if (!to_value(L, i, &v)) {
            return luaL_error(L, "Osi.%s: argument %d has unsupported type %s",
                              name, i, luaL_typename(L, i));
        }
        args.push_back(std::move(v));
    }

    std::vector<osi::Value> outputs;
    std::string why;
    const osi::Status status = osi::query(name, args, &outputs, &why);
    if (status == osi::Status::kUnavailable) {
        return luaL_error(L, "Osi.%s: %s", name, why.c_str());
    }
    if (outputs.empty()) {
        lua_pushboolean(L, status == osi::Status::kHandled);
        return 1;
    }
    for (osi::Value const& v : outputs) {
        if (v.type == osi::kNone) {
            lua_pushnil(L);
        } else {
            push_value(L, v);
        }
    }
    return (int)outputs.size();
}

// Does a fact match the filter the caller gave? A nil argument is a
// wildcard, and a GUID compares on its last thirty-six characters, as
// bg3se's MatchTuple does: the story writes "Name_<uuid>" where a caller
// has only the uuid.
bool row_matches(lua_State* L, int first, std::vector<osi::Value> const& row) {
    for (std::size_t i = 0; i < row.size(); ++i) {
        const int idx = first + (int)i;
        if (idx > lua_gettop(L) || lua_isnil(L, idx)) continue;

        switch (row[i].type) {
        case osi::kString:
        case osi::kGuidString: {
            char const* want = lua_tostring(L, idx);
            if (want == nullptr) return false;
            std::string const& held = row[i].text;
            const std::size_t wantLen = std::strlen(want);
            if (wantLen >= 36 && held.size() >= 36) {
                if (::strcasecmp(held.c_str() + held.size() - 36,
                                 want + wantLen - 36) != 0) {
                    return false;
                }
            } else if (::strcasecmp(held.c_str(), want) != 0) {
                return false;
            }
            break;
        }
        case osi::kReal:
            if (std::fabs(row[i].real - lua_tonumber(L, idx)) > 0.00001) {
                return false;
            }
            break;
        default:
            if (row[i].integer != lua_tointeger(L, idx)) return false;
            break;
        }
    }
    return true;
}

// DB_Name:Get(...) -- the facts that match, as an array of arrays.
int osi_db_get(lua_State* L) {
    char const* name = lua_tostring(L, lua_upvalueindex(1));

    // The arity is the filter's length, which for Get is however many
    // arguments were passed -- including the nils.
    const int argc = lua_gettop(L) - 1;
    const std::string key = std::string(name) + "/" + std::to_string(argc);

    std::vector<std::vector<osi::Value>> rows;
    if (!osi::facts(key.c_str(), &rows)) {
        return luaL_error(L, "Osi.%s:Get: the database could not be read",
                          name);
    }

    lua_createtable(L, (int)rows.size(), 0);
    int kept = 0;
    for (std::vector<osi::Value> const& row : rows) {
        if (!row_matches(L, 2, row)) continue;

        lua_createtable(L, (int)row.size(), 0);
        for (std::size_t i = 0; i < row.size(); ++i) {
            push_value(L, row[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_rawseti(L, -2, ++kept);
    }
    return 1;
}

// DB_Name:Delete(...) -- retracts every fact that matches, with nil as a
// wildcard for a column, as upstream has it.
int osi_db_delete(lua_State* L) {
    char const* name = lua_tostring(L, lua_upvalueindex(1));
    const int argc = lua_gettop(L) - 1;

    std::vector<osi::Value> args;
    args.reserve(argc > 0 ? argc : 0);
    for (int i = 2; i <= lua_gettop(L); ++i) {
        osi::Value v;
        if (lua_isnil(L, i)) {
            v.type = osi::kNone;  // any value in this column
        } else if (!to_value(L, i, &v)) {
            return luaL_error(L, "Osi.%s:Delete: argument %d has unsupported "
                              "type %s", name, i - 1, luaL_typename(L, i));
        }
        args.push_back(std::move(v));
    }

    const std::string key = std::string(name) + "/" + std::to_string(argc);
    std::string why;
    if (osi::remove(key.c_str(), args, &why) != osi::Status::kHandled) {
        return luaL_error(L, "Osi.%s:Delete: %s", name, why.c_str());
    }
    return 0;
}

// Where osi.cpp sends a story trigger. Runs inside the engine's own call,
// on Osiris' thread, so a failing handler is logged rather than thrown:
// unwinding out of here would unwind through Osiris.
void osiris_trigger(char const* name, std::size_t arity, char const* event,
                    std::vector<osi::Value> const& values) {
    if (g_lua == nullptr) return;

    lua_getglobal(g_lua, "Ext");
    if (!lua_istable(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    lua_getfield(g_lua, -1, "_Internal");
    lua_remove(g_lua, -2);
    lua_getfield(g_lua, -1, "FireOsirisListener");
    lua_remove(g_lua, -2);
    if (!lua_isfunction(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }

    lua_pushstring(g_lua, name);
    lua_pushinteger(g_lua, (lua_Integer)arity);
    lua_pushstring(g_lua, event);
    for (osi::Value const& value : values) push_value(g_lua, value);

    if (lua_pcall(g_lua, 3 + (int)values.size(), 0, 0) != LUA_OK) {
        logf("lua: Osiris listener dispatch failed: %s",
             lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
}

// Ext._Internal.WatchOsiris() -- starts watching the story's tuple
// operations, which is what a listener needs. Called when the first
// listener registers rather than at load: it patches two vtable slots and
// builds a reverse index of Osiris' function database.
int l_watch_osiris(lua_State* L) {
    osi::set_trigger_sink(&osiris_trigger);
    lua_pushboolean(L, osi::watch_story_triggers() ? 1 : 0);
    return 1;
}

// Ext._Internal.WatchOsirisCall(name, arity) -> whether it is an engine call,
// which listeners see through the DIV call handler rather than a node.
int l_watch_osiris_call(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto arity = static_cast<std::size_t>(luaL_checkinteger(L, 2));
    osi::set_trigger_sink(&osiris_trigger);
    bool watched = false;
    for (osi::Function const& fn : g_functions) {
        if (fn.name == name && fn.params.size() == arity) {
            watched = osi::watch_call(fn) || watched;
        }
    }
    lua_pushboolean(L, watched ? 1 : 0);
    return 1;
}

// Osi.Name for a story-defined function, built the first time the name is
// asked for.
//
// These are not bound during the level load. Their Function objects are
// heap pointers that cannot be cached between runs, and recovering them
// means walking Osiris' whole function database -- half a second that a
// session where no mod calls a procedure should not pay. bg3se resolves
// its Osi.* through a metatable for the same reason; a name that is never
// used costs nothing.
// Ext._Internal.OsiIdeHelpers(builtinOnly) -> the annotation text.
//
// Upstream's DoGenerateIdeHelpers, over the same data: one LuaLS annotation
// block and one stub definition per Osiris function, under Osi. and -- for
// the engine's own functions, which are also globals -- under the bare name
// as well. Out-parameters become @return rather than @param, and a query with
// none gets the boolean upstream gives it.
//
// CRLF because upstream writes CRLF, so the output can be diffed against the
// real extender's.
const char* osi_lua_type(std::uint8_t declared) {
    switch (osi::base_type(declared)) {
        case osi::kInteger:
        case osi::kInteger64: return "integer";
        case osi::kReal:      return "number";
        case osi::kString:    return "string";
        case osi::kGuidString: return "string GUID";
        default:              return "any";
    }
}

bool osi_is_builtin(osi::Function const& fn) {
    switch (fn.kind()) {
        case osi::kEvent:
        case osi::kCall:
        case osi::kQuery:
        case osi::kSysCall:
        case osi::kSysQuery: return true;
        default: return false;
    }
}

void osi_helpers_for(osi::Function const& fn, std::string* out) {
    // Which parameters the engine fills in. Unknown means none, the same
    // reading the call path takes.
    const std::size_t total = fn.params.size();
    const std::size_t outs =
        fn.out_params >= 0 ? std::min((std::size_t)fn.out_params, total) : 0;
    const std::size_t ins = total - outs;

    std::string comment;
    std::size_t arg = 0;
    for (std::size_t i = 0; i < total; ++i) {
        if (fn.is_output(i, ins)) continue;
        comment += "--- @param arg";
        comment += std::to_string(++arg);
        comment += " ";
        comment += osi_lua_type(fn.params[i]);
        comment += "\r\n";
    }

    if (outs > 0) {
        for (std::size_t i = 0; i < total; ++i) {
            if (!fn.is_output(i, ins)) continue;
            comment += "--- @return ";
            comment += osi_lua_type(fn.params[i]);
            comment += "\r\n";
        }
    } else if (fn.is_query()) {
        comment += "--- @return boolean Did the query succeed?\r\n";
    }

    std::string defn = fn.name;
    defn += " = function (";
    for (std::size_t i = 0; i < ins; ++i) {
        defn += "arg";
        defn += std::to_string(i + 1);
        if (i + 1 < ins) defn += ", ";
    }
    defn += ") end\r\n\r\n";

    *out += comment;
    *out += "Osi.";
    *out += defn;

    if (osi_is_builtin(fn)) {
        *out += comment;
        *out += defn;
    }
}

// Ext._Internal.PostToOtherContext(channel, payload, userId) -> true
//
// The two Lua contexts are two states in one process, and the tick drives
// both from the same thread, so a message crosses by being queued in the
// other state rather than by riding the game's connection. Upstream's
// messages go over the extender's protobuf channel because on Windows the
// two contexts may be two machines; single-player is one process either way,
// and this is the same delivery a mod sees.
//
// Queued rather than delivered here. A send happens inside the sender's own
// tick, and calling the other side's handler from that point would let a
// reply re-enter the sender mid-call; the other context drains its queue on
// its next tick, which is when a real message would have arrived.
// Ext._Internal.HasOtherContext() -> whether there is a second Lua state.
//
// A client context only exists once the client side has started, and a mod
// that sends before then should reach its own listeners rather than nothing.
// Whether a reset has been asked for.
//
// Performed on the tick rather than where it is asked for: Ext.Debug.Reset
// is called from Lua, and closing the state you are executing in takes the
// process with it. Upstream's is asynchronous for the same reason, and fires
// ResetCompleted when it is done.
bool g_reset_pending = false;

// And whether the post-reset events are still owed. They are fired on the
// tick after the reload rather than at the end of it, which is where
// upstream's post-reset callback lands too: a mod's bootstrap may defer its
// own setup to a tick, and a ResetCompleted handler that runs before that
// finds a half-built mod.
bool g_reset_events_pending = false;

int l_request_reset(lua_State* L) {
    g_reset_pending = true;
    lua_pushboolean(L, 1);
    return 1;
}

extern "C" std::size_t bg3le_meta_enum_count();
extern "C" char const* bg3le_meta_enum_at(std::size_t index, bool* isBitmask);
extern "C" bool bg3le_meta_enum_value_at(char const* enumName,
                                         std::size_t index,
                                         char const** label,
                                         std::uint64_t* value);

// Ext._Internal.EnumCount() -> how many enums bg3se describes.
int l_enum_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_meta_enum_count());
    return 1;
}

// Ext._Internal.EnumAt(index) -> name, isBitmask
int l_enum_at(lua_State* L) {
    const auto index = (std::size_t)luaL_checkinteger(L, 1);
    bool isBitmask = false;
    char const* name = bg3le_meta_enum_at(index, &isBitmask);
    if (name == nullptr) return 0;
    lua_pushstring(L, name);
    lua_pushboolean(L, isBitmask ? 1 : 0);
    return 2;
}

// Ext._Internal.EnumValueAt(name, index) -> label, value
int l_enum_value_at(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto index = (std::size_t)luaL_checkinteger(L, 2);

    char const* label = nullptr;
    std::uint64_t value = 0;
    if (!bg3le_meta_enum_value_at(name, index, &label, &value)) return 0;

    lua_pushstring(L, label);
    lua_pushinteger(L, (lua_Integer)value);
    return 2;
}

// ---- Ext.Audio; src/vendor/audio.cpp ----
extern "C" bool bg3le_audio_ready();
extern "C" bool bg3le_audio_builtin(char const* name, unsigned player, std::uint64_t* out);
extern "C" bool bg3le_audio_set_switch(std::uint64_t obj, char const* group, char const* state);
extern "C" bool bg3le_audio_set_state(char const* group, char const* state);
extern "C" bool bg3le_audio_set_rtpc(std::uint64_t obj, char const* name, float value, bool bypass);
extern "C" float bg3le_audio_get_rtpc(std::uint64_t obj, char const* name);
extern "C" void bg3le_audio_reset_rtpc(std::uint64_t obj, char const* name);
extern "C" void bg3le_audio_stop(bool all, std::uint64_t obj);
extern "C" void bg3le_audio_pause_all(bool pause);
extern "C" bool bg3le_audio_post_event(std::uint64_t obj, char const* name, float position);
extern "C" bool bg3le_audio_event(char const* name, bool load);
extern "C" bool bg3le_audio_play_external(std::uint64_t obj, char const* event, char const* path,
                                          std::uint8_t codec, float position);
extern "C" bool bg3le_audio_bank(char const* name, int op);

std::uint64_t audio_object(lua_State* L, int index) {
    return (std::uint64_t)luaL_checkinteger(L, index);
}

// Ext._Internal.Audio(op, ...) -- one entry per upstream function; the
// prelude resolves sound objects to their ids first.
int l_audio(lua_State* L) {
    const std::string op = luaL_checkstring(L, 1);
    if (!bg3le_audio_ready()) return luaL_error(L, "Sound manager is not available!");
    if (op == "Builtin") {
        std::uint64_t id = 0;
        if (!bg3le_audio_builtin(luaL_checkstring(L, 2), (unsigned)luaL_checkinteger(L, 3), &id)) return 0;
        lua_pushinteger(L, (lua_Integer)id);
    } else if (op == "SetSwitch") {
        lua_pushboolean(L, bg3le_audio_set_switch(audio_object(L, 2), luaL_checkstring(L, 3), luaL_checkstring(L, 4)));
    } else if (op == "SetState") {
        lua_pushboolean(L, bg3le_audio_set_state(luaL_checkstring(L, 2), luaL_checkstring(L, 3)));
    } else if (op == "SetRTPC") {
        lua_pushboolean(L, bg3le_audio_set_rtpc(audio_object(L, 2), luaL_checkstring(L, 3),
                                                (float)luaL_checknumber(L, 4), lua_toboolean(L, 5) != 0));
    } else if (op == "GetRTPC") {
        lua_pushnumber(L, bg3le_audio_get_rtpc(audio_object(L, 2), luaL_checkstring(L, 3)));
    } else if (op == "ResetRTPC") {
        bg3le_audio_reset_rtpc(audio_object(L, 2), luaL_checkstring(L, 3));
        return 0;
    } else if (op == "Stop") {
        const bool all = lua_isnoneornil(L, 2);
        bg3le_audio_stop(all, all ? 0 : audio_object(L, 2));
        return 0;
    } else if (op == "PauseAllSounds" || op == "ResumeAllSounds") {
        bg3le_audio_pause_all(op == "PauseAllSounds");
        return 0;
    } else if (op == "PostEvent") {
        lua_pushboolean(L, bg3le_audio_post_event(audio_object(L, 2), luaL_checkstring(L, 3),
                                                  (float)luaL_optnumber(L, 4, 0.0)));
    } else if (op == "LoadEvent" || op == "UnloadEvent") {
        lua_pushboolean(L, bg3le_audio_event(luaL_checkstring(L, 2), op == "LoadEvent"));
    } else if (op == "PlayExternalSound") {
        lua_pushboolean(L, bg3le_audio_play_external(audio_object(L, 2), luaL_checkstring(L, 3),
                                                     luaL_checkstring(L, 4), (std::uint8_t)luaL_checkinteger(L, 5),
                                                     (float)luaL_optnumber(L, 6, 0.0)));
    } else if (op == "LoadBank" || op == "UnloadBank" || op == "PrepareBank" || op == "UnprepareBank") {
        const int code = op == "LoadBank" ? 0 : op == "PrepareBank" ? 2 : 1;
        lua_pushboolean(L, bg3le_audio_bank(luaL_checkstring(L, 2), code));
    } else {
        return luaL_error(L, "bad audio op %s", op.c_str());
    }
    return 1;
}

// Ext._Internal.InjectKey(down, scancode, modifiers) -- src/sdl_forward.cpp
extern "C" void bg3le_sdl_inject_key(bool down, int scancode, int modifiers);
int l_inject_key(lua_State* L) {
    bg3le_sdl_inject_key(lua_toboolean(L, 1) != 0, (int)luaL_checkinteger(L, 2),
                         (int)luaL_optinteger(L, 3, 0));
    return 0;
}

int l_has_other_context(lua_State* L) {
    lua_State* other = (L == g_client_lua) ? g_server_lua : g_client_lua;
    lua_pushboolean(L, other != nullptr ? 1 : 0);
    return 1;
}

// Messages for a context, taken on that context's own thread: entering the
// other state from here would cross threads. They come from the other
// context in this process, or from a peer through src/net.cpp.
std::mutex g_net_mutex;
std::vector<NetInbound> g_to_server;
std::vector<NetInbound> g_to_client;

// Ext._Internal.PostToOtherContext(channel, payload, userId, module,
//                                  requestId, replyId, binary) -> bool
int l_post_to_other_context(lua_State* L) {
    const bool fromClient = (L == g_client_lua);
    if ((fromClient ? g_server_lua : g_client_lua) == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    NetInbound m;
    m.channel = luaL_checkstring(L, 1);
    std::size_t length = 0;
    const char* payload = lua_tolstring(L, 2, &length);
    m.hasPayload = payload != nullptr;
    if (payload != nullptr) m.payload.assign(payload, length);
    m.user = luaL_optinteger(L, 3, 0);
    if (lua_isstring(L, 4)) m.module = lua_tostring(L, 4);
    m.requestId = (std::uint32_t)luaL_optinteger(L, 5, 0);
    m.replyId = (std::uint32_t)luaL_optinteger(L, 6, 0);
    m.binary = lua_toboolean(L, 7) != 0;
    {
        const std::lock_guard<std::mutex> lock(g_net_mutex);
        (fromClient ? g_to_server : g_to_client).push_back(std::move(m));
    }
    lua_pushboolean(L, 1);
    return 1;
}

void push_net_var(lua_State* L, NetUserVar const& v) {
    lua_createtable(L, 0, 6);
    lua_pushboolean(L, v.module);
    lua_setfield(L, -2, "Module");
    lua_pushstring(L, v.guid.c_str());
    lua_setfield(L, -2, "Guid");
    lua_pushlstring(L, v.key.data(), v.key.size());
    lua_setfield(L, -2, "Key");
    static char const* const kinds[] = {"null", "bool", "int", "double", "string", "json", "binary"};
    lua_pushstring(L, kinds[(int)v.kind]);
    lua_setfield(L, -2, "Kind");
    switch (v.kind) {
    case NetUserVar::Kind::Bool: lua_pushboolean(L, v.b); break;
    case NetUserVar::Kind::Int: lua_pushinteger(L, (lua_Integer)v.i); break;
    case NetUserVar::Kind::Double: lua_pushnumber(L, v.d); break;
    case NetUserVar::Kind::String:
    case NetUserVar::Kind::Json:
    case NetUserVar::Kind::Binary: lua_pushlstring(L, v.s.data(), v.s.size()); break;
    default: lua_pushnil(L); break;
    }
    lua_setfield(L, -2, "Value");
}

// Ext._Internal.TakeNetMessages() -> {{Kind, Channel, Payload, Module, User,
//                                      RequestId, ReplyId, Binary, Vars}, ...}
int l_take_net_messages(lua_State* L) {
    std::vector<NetInbound> taken;
    {
        const std::lock_guard<std::mutex> lock(g_net_mutex);
        taken.swap(L == g_client_lua ? g_to_client : g_to_server);
    }
    lua_createtable(L, static_cast<int>(taken.size()), 0);
    int i = 0;
    for (auto const& m : taken) {
        lua_createtable(L, 0, 9);
        lua_pushstring(L, m.kind == NetInbound::Kind::UserVars ? "vars"
                          : m.kind == NetInbound::Kind::ResetLua ? "reset" : "post");
        lua_setfield(L, -2, "Kind");
        lua_pushlstring(L, m.channel.data(), m.channel.size());
        lua_setfield(L, -2, "Channel");
        if (m.hasPayload) {
            lua_pushlstring(L, m.payload.data(), m.payload.size());
            lua_setfield(L, -2, "Payload");
        }
        if (!m.module.empty()) {
            lua_pushlstring(L, m.module.data(), m.module.size());
            lua_setfield(L, -2, "Module");
        }
        lua_pushinteger(L, (lua_Integer)m.user);
        lua_setfield(L, -2, "User");
        lua_pushinteger(L, (lua_Integer)m.requestId);
        lua_setfield(L, -2, "RequestId");
        lua_pushinteger(L, (lua_Integer)m.replyId);
        lua_setfield(L, -2, "ReplyId");
        lua_pushboolean(L, m.binary ? 1 : 0);
        lua_setfield(L, -2, "Binary");
        if (!m.vars.empty()) {
            lua_createtable(L, (int)m.vars.size(), 0);
            int j = 0;
            for (auto const& v : m.vars) {
                push_net_var(L, v);
                lua_rawseti(L, -2, ++j);
            }
            lua_setfield(L, -2, "Vars");
        }
        lua_rawseti(L, -2, ++i);
    }
    return 1;
}

int osi_ide_helpers(lua_State* L) {
    const bool builtinOnly = lua_toboolean(L, 1) != 0;

    // Everything the database names, not just what is bound: a procedure and
    // a user query have no dispatch handle, so they never reach g_functions,
    // and they are exactly what a mod author wants annotations for.
    std::vector<osi::Function> functions = osi::all_functions();
    if (functions.empty()) functions = g_functions;

    // The kind comes out of the function object, at an offset only a full
    // signature walk recovers -- and this story's signatures came from the
    // cache, so there was no walk and every kind read as nought. The bound
    // list carries the right id for all 1,303 engine functions whatever
    // happened, so it fills in what the database could not say. Without this
    // no function counted as a builtin and none got its global name.
    std::unordered_map<std::string, std::uint32_t> boundIds;
    boundIds.reserve(g_functions.size());
    for (osi::Function const& fn : g_functions) {
        boundIds.emplace(fn.name + "/" + std::to_string(fn.params.size()),
                         fn.id);
    }
    for (osi::Function& fn : functions) {
        if (fn.id != 0) continue;
        auto found =
            boundIds.find(fn.name + "/" + std::to_string(fn.params.size()));
        if (found != boundIds.end()) fn.id = found->second;
    }

    std::string out;
    out.reserve(0x20000);

    for (osi::Function const& fn : functions) {
        if (builtinOnly && !osi_is_builtin(fn)) continue;
        osi_helpers_for(fn, &out);
    }

    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

int osi_story_lookup(lua_State* L) {
    char const* asked = luaL_checkstring(L, 1);

    bool isDatabase = false;
    bool isQuery = false;
    std::string spelling;
    if (!osi::story_function(asked, &isDatabase, &spelling, &isQuery)) {
        lua_pushnil(L);
        return 1;
    }

    // The story's own spelling is what the database is keyed by, so that
    // is what the closure carries. The warning is upstream's.
    char const* name = spelling.empty() ? asked : spelling.c_str();
    if (spelling != asked) {
        logf("lua: COMPATIBILITY WARNING: Osiris symbol '%s' referenced "
             "using incorrect case; the correct name is '%s'", asked, name);
    }

    if (isQuery) {
        lua_pushstring(L, name);
        lua_pushcclosure(L, osi_story_query, 1);
        return 1;
    }

    if (!isDatabase) {
        lua_pushstring(L, name);
        lua_pushboolean(L, 0);
        lua_pushcclosure(L, osi_story_dispatch, 2);
        return 1;
    }

    // A database reads as well as writes: DB_Foo(...) inserts,
    // DB_Foo:Get(...) queries.
    lua_createtable(L, 0, 2);
    lua_pushstring(L, name);
    lua_pushcclosure(L, osi_db_get, 1);
    lua_setfield(L, -2, "Get");
    lua_pushstring(L, name);
    lua_pushcclosure(L, osi_db_delete, 1);
    lua_setfield(L, -2, "Delete");

    lua_createtable(L, 0, 1);
    lua_pushstring(L, name);
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, osi_story_dispatch, 2);
    lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
    return 1;
}

// Osi.Name(inputs...) -- whatever the caller omits is treated as an output,
// matching how the engine's own argument lists are shaped.
// How many arguments a caller supplies for this declaration.
int expected_inputs(const osi::Function& fn) {
    if (fn.out_params < 0) return -1;  // not known; the caller's count decides
    return (int)fn.params.size() - fn.out_params;
}

int osi_dispatch(lua_State* L) {
    const auto* group = static_cast<std::vector<const osi::Function*>*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    const int argc = lua_gettop(L);

    // With out-param counts recovered from Osiris, the input count is known
    // exactly, so the declaration is chosen by it. A name with one
    // declaration behaves as before; one with several -- ApplyStatus has
    // three -- now answers to each of them.
    const osi::Function* fn = nullptr;
    for (const osi::Function* candidate : *group) {
        if (expected_inputs(*candidate) == argc) {
            fn = candidate;
            break;
        }
    }
    if (fn == nullptr) {
        // Without out-param counts the caller's own count decides the
        // split, as it did before they were recovered.
        for (const osi::Function* candidate : *group) {
            if (expected_inputs(*candidate) < 0
                && (std::size_t)argc <= candidate->params.size()) {
                fn = candidate;
                break;
            }
        }
    }
    char const* wanted = group->empty() ? "?" : (*group)[0]->name.c_str();
    if (fn == nullptr) {
        // The engine's mapping holds one declaration per name, but the
        // story declares its own arities of the same name: ApplyStatus is
        // in the mapping with five parameters and in Osiris' database with
        // three, four and five, the extra two being the story's own. A
        // count the mapping does not have is theirs, so it goes the way
        // every story function goes -- a tuple into its node.
        const std::string key =
            std::string(wanted) + "/" + std::to_string(argc);
        std::vector<osi::Value> args;
        args.reserve(argc > 0 ? argc : 0);
        bool convertible = true;
        for (int i = 1; i <= argc; ++i) {
            osi::Value v;
            if (!to_value(L, i, &v)) {
                convertible = false;
                break;
            }
            args.push_back(std::move(v));
        }

        std::string why;
        if (convertible
            && osi::insert(key.c_str(), args, &why) == osi::Status::kHandled) {
            return 0;
        }

        // The story has this form; it was the arguments that failed.
        if (convertible && why != "no such story function") {
            return luaL_error(L, "Osi.%s: the story's %d-argument form could not be called: %s",
                              wanted, argc, why.c_str());
        }

        std::string counts;
        for (const osi::Function* candidate : *group) {
            const int wants = expected_inputs(*candidate);
            if (wants < 0) continue;
            if (!counts.empty()) counts += " or ";
            counts += std::to_string(wants);
        }
        return luaL_error(L,
            "Incorrect number of IN arguments for '%s'; expected %s, got %d, "
            "and the story declares no %d-argument form (%s)",
            wanted, counts.empty() ? "none" : counts.c_str(), argc, argc,
            why.empty() ? "not convertible" : why.c_str());
    }

    std::vector<osi::Value> inputs;
    inputs.reserve(argc);
    for (int i = 1; i <= argc; ++i) {
        osi::Value v;
        if (!to_value(L, i, &v)) {
            return luaL_error(L, "Osi.%s: argument %d has unsupported type %s",
                              fn->name.c_str(), i, luaL_typename(L, i));
        }
        inputs.push_back(std::move(v));
    }

    std::vector<osi::Value> outputs;
    const osi::Status status = osi::invoke(*fn, inputs, &outputs);

    if (status == osi::Status::kUnavailable) {
        return luaL_error(L, "Osi.%s could not be invoked (Osiris not ready?)",
                          fn->name.c_str());
    }

    // Matches bg3se: a procedure yields nothing, a query with no outputs
    // yields the success flag, and a query with outputs yields one value per
    // output -- all nil when the engine answered false. Returning a bare nil
    // for both "answered false" and "could not call" conflated a normal
    // answer with an error.
    if (fn->kind() == osi::kCall) return 0;

    const int out_count = fn->out_params >= 0
                              ? fn->out_params
                              : static_cast<int>(fn->params.size() - inputs.size());
    if (out_count == 0) {
        lua_pushboolean(L, status == osi::Status::kHandled);
        return 1;
    }

    if (status == osi::Status::kHandled) {
        for (const osi::Value& v : outputs) push_value(L, v);
    } else {
        for (int i = 0; i < out_count; ++i) lua_pushnil(L);
    }
    return out_count;
}

// Output goes to the attached debugger client as well as the log, which is
// what makes the remote prompt useful. Severity rides in an upvalue so
// Ext.Log.Print/PrintWarning/PrintError share one implementation.
int l_log(lua_State* L) {
    const int severity = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    std::string out;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) out += "\t";
        out += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    debug_server_output(out.c_str(), severity);
    logf("lua: %s", out.c_str());
    return 0;
}

// Raw memory access, so structure walks can be prototyped from the live
// console instead of rebuilding and reloading a save for every guess.
// Reads are fault-tolerant: a wrong address returns nil, it does not crash
// the game.
int l_module_base(lua_State* L) {
    const char* want = luaL_checkstring(L, 1);
    struct Ctx { const char* want; std::uintptr_t base; } ctx{want, 0};
    ::dl_iterate_phdr(
        [](struct dl_phdr_info* info, std::size_t, void* data) {
            auto* c = static_cast<Ctx*>(data);
            const char* name = info->dlpi_name;
            if (c->want[0] == '\0') {
                if (name == nullptr || name[0] == '\0') {
                    c->base = info->dlpi_addr;
                    return 1;
                }
                return 0;
            }
            if (name != nullptr && std::strstr(name, c->want) != nullptr) {
                c->base = info->dlpi_addr;
                return 1;
            }
            return 0;
        },
        &ctx);
    if (ctx.base == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(ctx.base));
    return 1;
}

int l_peek(lua_State* L) {
    const auto addr = static_cast<std::uintptr_t>(luaL_checkinteger(L, 1));
    const int width = static_cast<int>(luaL_optinteger(L, 2, 8));
    std::uint64_t value = 0;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return luaL_error(L, "Peek width must be 1, 2, 4 or 8");
    }
    if (!safe_read(reinterpret_cast<const void*>(addr), &value, width)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

// Ext._Internal.MoveableSet(address, what, {x, y, z[, w]}) -> true; what is
// 0 translate, 1 rotate, 2 scale (src/vendor/moveable.cpp).
extern "C" bool bg3le_moveable_set(void* object, int what, float const* v);
int l_moveable_set(lua_State* L) {
    auto* object = reinterpret_cast<void*>(static_cast<std::uintptr_t>(luaL_checkinteger(L, 1)));
    const int what = static_cast<int>(luaL_checkinteger(L, 2));
    luaL_checktype(L, 3, LUA_TTABLE);
    float v[4] = {0, 0, 0, 1};
    const int n = what == 1 ? 4 : 3;
    for (int i = 0; i < n; ++i) {
        lua_geti(L, 3, i + 1);
        v[i] = static_cast<float>(luaL_checknumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, bg3le_moveable_set(object, what, v) ? 1 : 0);
    return 1;
}

// Ext._Internal.ItemCreateCacheTemplate(handle, component) -> the item's cache
// template's address, or nil (src/vendor/cache_template.cpp).
extern "C" void* bg3le_item_create_cache_template(void* container, void* item,
                                                  std::uint64_t entity);
void* component_pointer(std::uint64_t handle, const char* name, void const** meta);
void* server_container();
int l_item_create_cache_template(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    void const* meta = nullptr;
    void* item = component_pointer(handle, name, &meta);
    void* tmpl = item != nullptr
        ? bg3le_item_create_cache_template(server_container(), item, handle) : nullptr;
    if (tmpl == nullptr) return 0;
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(tmpl)));
    return 1;
}

int l_peek_string(lua_State* L) {
    const auto addr = static_cast<std::uintptr_t>(luaL_checkinteger(L, 1));
    char buf[512];
    if (!safe_cstr(reinterpret_cast<const void*>(addr), buf, sizeof(buf))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    return 1;
}

int l_monotonic_ms(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushinteger(L, (lua_Integer)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return 1;
}

int l_microsec(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushinteger(L, (lua_Integer)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    return 1;
}

int l_clock_time(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    lua_pushnumber(L, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    return 1;
}

// The project root, derived from our own .so path: it lives in <root>/build/.
// Mod discovery uses it to find <root>/mods without hardcoding a path.
int l_extender_root(lua_State* L) {
    ::Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(&l_extender_root), &info) == 0 ||
        info.dli_fname == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    std::string path(info.dli_fname);
    for (int up = 0; up < 2; ++up) {  // strip the filename, then build/
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            lua_pushnil(L);
            return 1;
        }
        path.erase(slash);
    }
    lua_pushstring(L, path.c_str());
    return 1;
}

// Mod discovery needs to enumerate directories, which Lua cannot do without
// shelling out. Returns names only, with "." and ".." dropped.
int l_list_dir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    DIR* dir = ::opendir(path);
    if (dir == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, std::strerror(errno));
        return 2;
    }
    lua_newtable(L);
    int n = 0;
    while (dirent* e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        lua_pushstring(L, e->d_name);
        lua_rawseti(L, -2, ++n);
    }
    ::closedir(dir);
    return 1;
}

// Norbyte's Lua fork expects the host to install these before anything can
// allocate, because its GC calls them unconditionally; luaL_newstate alone
// leaves them null and the first sweep jumps through a null pointer. bg3se
// installs its own in LuaStateWrapper. bg3le creates no LUA_TCPPOBJECT
// values, so only the allocator and the string-cache pair ever run.
void* cpp_alloc(lua_State*, std::size_t size) { return std::malloc(size); }
void cpp_free(lua_State*, void* block, std::size_t) { std::free(block); }
void cpp_finalize(lua_State*, void*) {}
void* cpp_canonicalize(lua_State*, void* val) { return val; }
CMetatable* cpp_get_metatable(lua_State*, void*, unsigned long long) { return nullptr; }
CMetatable* cpp_get_light_metatable(lua_State*, unsigned long long,
                                    unsigned long long) { return nullptr; }
void cache_string(lua_State*, TString*) {}
void release_string(lua_State*, TString*) {}

// Address of an engine symbol by mangled name. The counterpart to Peek: with
// both, a structure can be walked from the prompt without a rebuild.
int l_symbol_addr(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (g_symbols == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    void* addr = g_symbols->find(name);
    if (addr == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(addr)));
    return 1;
}

// The engine's ECS type index for a component, read live from the static the
// engine assigns at startup. This is the foundation Ext.Entity needs.
int l_component_index(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto idx = ecs::index_of(ecs::Context::Component, name);
    if (!idx.has_value()) {
        // Try the one-frame registry before giving up; a handful of components
        // are registered there instead.
        const auto one_frame = ecs::index_of(ecs::Context::OneFrameComponent, name);
        if (!one_frame.has_value()) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, *one_frame);
        lua_pushstring(L, "one-frame");
        return 2;
    }
    lua_pushinteger(L, *idx);
    return 1;
}

// The captured EntityStorageContainer, for verifying the capture works
// before anything is built on it.
int l_ecs_storage(lua_State* L) {
    void* p = ecs::container();
    if (p == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(p)));
    return 1;
}

// Reads the captured container's Storages array, which bg3se says is its
// first member: Array<EntityStorageData*> = {buf, size}. One call is enough to
// tell whether the capture is a real container or a coincidence.
int l_ecs_dump(lua_State* L) {
    void* c = ecs::container();
    if (c == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "no entity lookup has happened yet");
        return 2;
    }

    std::uintptr_t buf = 0;
    std::uint32_t size = 0;
    const auto base = reinterpret_cast<const char*>(c);
    if (!safe_read(base, &buf, sizeof(buf)) ||
        !safe_read(base + sizeof(buf), &size, sizeof(size))) {
        lua_pushnil(L);
        lua_pushstring(L, "container is not readable");
        return 2;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)reinterpret_cast<std::uintptr_t>(c));
    lua_setfield(L, -2, "Container");
    lua_pushinteger(L, (lua_Integer)buf);
    lua_setfield(L, -2, "StoragesBuf");
    lua_pushinteger(L, (lua_Integer)size);
    lua_setfield(L, -2, "StoragesCount");

    // A handful of storage pointers, to show the array holds pointers rather
    // than noise.
    lua_newtable(L);
    const std::uint32_t show = size < 6 ? size : 6;
    for (std::uint32_t i = 0; i < show; ++i) {
        std::uintptr_t entry = 0;
        if (!safe_read(reinterpret_cast<const char*>(buf) + i * sizeof(entry),
                       &entry, sizeof(entry))) {
            break;
        }
        lua_pushinteger(L, (lua_Integer)entry);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "Storages");
    return 1;
}

// Implemented in src/vendor/entity_bridge.cpp against bg3se's ECS layout.
extern "C" bool bg3le_entity_health(void* container, std::uint64_t handle,
                                    std::uint16_t componentIndex,
                                    std::int32_t* hp, std::int32_t* maxHp);

extern "C" void bg3le_entity_probe(void* container, std::uint64_t handle,
                                   std::uint16_t componentIndex,
                                   std::int32_t* storageIndex, void** storage,
                                   void** component);
extern "C" bool bg3le_mark_component_changed(void* container, std::uint64_t handle,
                                             std::uint16_t componentIndex);
extern "C" bool bg3le_set_health(void* container, std::uint64_t handle,
                                 std::uint16_t componentIndex, std::int32_t hp,
                                 bool setMax);
extern "C" void bg3le_world_probe(void* container, void** world,
                                  void** replication, std::int32_t* poolCount,
                                  bool* storageMatches, bool* queriesMatch);
extern "C" std::int32_t bg3le_replicate_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t replicationTypeIndex,
                                                  std::uint32_t qword,
                                                  std::uint64_t flags);

extern "C" bool bg3le_container_is_server(void* container);
extern "C" bool bg3le_game_allocator_ready();
extern "C" bool bg3le_fixed_string_intern(char const* text,
                                          unsigned int* out);
extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            unsigned int* out);
extern "C" char const* bg3le_meta_class_name(void const* handle);
extern "C" bool bg3le_meta_type_name_at(void const* handle, char const* path,
                                        char const** name,
                                        std::uint16_t* length);
extern "C" void* bg3le_global_switches();
#include "vendor/imgui_args.h"

extern "C" void bg3le_imgui_status(bool* wanted, bool* started,
                                   bool* initialized);
extern "C" std::uint64_t bg3le_imgui_new_window(char const* name);
extern "C" std::uint64_t bg3le_imgui_add(std::uint64_t parent,
                                         char const* kind,
                                         ImguiArg const* args,
                                         std::size_t count);
extern "C" bool bg3le_imgui_call(std::uint64_t handle, char const* name,
                                 ImguiArg const* args, std::size_t count,
                                 ImguiArg* out);
extern "C" std::size_t bg3le_imgui_children(std::uint64_t handle,
                                            std::uint64_t* out,
                                            std::size_t capacity);
extern "C" void* bg3le_imgui_object(std::uint64_t handle,
                                    char const** typeName);
extern "C" bool bg3le_imgui_destroy(std::uint64_t handle);
extern "C" void bg3le_imgui_enable_demo(bool enabled);
extern "C" void bg3le_imgui_frame_stats(std::uint64_t* frames, int* vertices,
                                        int* lists, int* drawnFrames);
extern "C" void bg3le_imgui_watch(char const* window, char const* item);
extern "C" bool bg3le_imgui_window_geometry(float* out, std::size_t count);
extern "C" bool bg3le_imgui_hovered(char* name, std::size_t size,
                                    unsigned* hoveredId, unsigned* activeId,
                                    unsigned* navId, unsigned* watchedId);
extern "C" bool bg3le_imgui_load_font(char const* name, char const* path,
                                      float size);
extern "C" void bg3le_imgui_set_ui_scale(float scale);
extern "C" void bg3le_imgui_set_font_scale(float scale);
extern "C" bool bg3le_imgui_viewport_size(int* width, int* height);
extern "C" void bg3le_imgui_hold_mouse(float x, float y, bool holding);
extern "C" void bg3le_imgui_click_at(float x, float y, int button);
extern "C" void bg3le_imgui_input_state(float* mouseX, float* mouseY,
                                        float* displayW, float* displayH,
                                        bool* mouseDown, bool* wantMouse,
                                        bool* hoveredWindow,
                                        bool* navNoHover);
extern "C" std::uint32_t bg3le_imgui_set_callback(std::uint64_t handle,
                                                  char const* name,
                                                  void* state);
extern "C" bool bg3le_imgui_clear_callback(std::uint64_t handle,
                                           char const* name);
// Upstream's binary JSON reader and writer, src/vendor/json_binary.cpp.
extern "C" int bg3le_json_binary_encode(lua_State* L);
extern "C" int bg3le_json_binary_decode(lua_State* L);
// Ext.UI's C side, src/vendor/bg3le_noesis_lua.inl.
extern "C" void bg3le_ui_register(lua_State* L);
extern "C" void bg3le_ui_reset();
extern "C" bool bg3le_imgui_take_event(void* state, std::uint32_t* id,
                                       std::uint64_t* widget,
                                       std::uint8_t* argKind, bool* argBool,
                                       int* argInt, std::uint64_t* argWidget,
                                       float* argVec, int* argIVec,
                                       char const** argString);
extern "C" bool bg3le_stats_name_id(void const* object, std::uint32_t* out);
extern "C" bool bg3le_fixed_string_recheck(std::uint32_t id,
                                           char const** cached,
                                           char const** fresh);
extern "C" bool bg3le_stats_copy_from(void const* dest, void const* source,
                                      std::size_t* carried,
                                      std::size_t* total);
extern "C" bool bg3le_meta_lsstring_assign(void* address, char const* text,
                                           std::size_t length);

// The component field tables, from src/vendor/component_meta.cpp.
extern "C" void const* bg3le_meta_component(const char* engineName);
extern "C" std::size_t bg3le_meta_component_size(void const* handle);
extern "C" std::size_t bg3le_meta_component_stride(void const* handle);
extern "C" bool bg3le_meta_component_is_proxy(void const* handle);
extern "C" bool bg3le_meta_component_is_one_frame(void const* handle);
extern "C" void* bg3le_entity_one_frame_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t componentIndex);
extern "C" bool bg3le_meta_field(void const* handle, const char* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount);
extern "C" std::size_t bg3le_meta_fields(void const* handle, const char** names,
                                         std::uint8_t* kinds,
                                         std::size_t capacity);
extern "C" std::size_t bg3le_meta_legacy_at(void const* handle,
                                            char const* path,
                                            char const** names,
                                            std::uint8_t* kinds,
                                            std::size_t capacity);
extern "C" std::size_t bg3le_meta_fields_at(void const* handle,
                                            const char* path,
                                            const char** names,
                                            std::uint8_t* kinds,
                                            std::size_t capacity);
extern "C" bool bg3le_meta_resolve(void const* handle, const char* path,
                                   void* component, void** address,
                                   std::uint8_t* kind, std::uint16_t* size,
                                   bool* readOnly);
extern "C" int bg3le_meta_array_length(void const* handle, const char* path,
                                       void* component, std::size_t* count,
                                       std::uint16_t* elemSize,
                                       std::uint8_t* elemKind);
extern "C" bool bg3le_meta_variant_index(void const* handle,
                                         const char* path, void* component,
                                         std::size_t* active,
                                         std::size_t* count);
extern "C" bool bg3le_meta_map_key(void const* handle, const char* path,
                                   void* component, std::size_t index,
                                   void** address, std::uint8_t* kind,
                                   std::uint16_t* size);
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);
// From src/vendor/fixed_string.cpp.
extern "C" const char* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" void* bg3le_string_table();
extern "C" bool bg3le_meta_enum_label(void const* handle, const char* path,
                                      std::size_t index, const char** label,
                                      std::uint64_t* value, bool* isBitmask);
extern "C" bool bg3le_meta_array_resize(void const* handle, char const* path,
                                        void* component, std::size_t count,
                                        void** data, std::uint16_t* elemSize,
                                        std::uint8_t* elemKind);
extern "C" bool bg3le_meta_bitflag(void const* handle, char const* name,
                                   void* object, void** address,
                                   std::uint16_t* size, std::uint64_t* mask);
extern "C" std::size_t bg3le_meta_enum_count();
extern "C" std::size_t bg3le_meta_class_count();
extern "C" std::size_t bg3le_meta_component_count();
extern "C" const char* bg3le_meta_engine_class(void const* handle);
extern "C" const char* bg3le_meta_short_name(void const* handle);
extern "C" std::size_t bg3le_entities_collect(void* container,
                                              int componentIndex,
                                              std::uint64_t* out,
                                              std::size_t max);
extern "C" void* bg3le_entity_component(void* container, std::uint64_t handle,
                                        std::uint16_t componentIndex,
                                        std::size_t componentSize);
extern "C" void* bg3le_entity_pending_component(void* container, std::uint64_t handle,
                                                std::uint16_t type, bool* immediate);

// The container belonging to the server world.
//
// Both worlds come through the capture thunk and the order is not ours to
// choose, so the slots are sorted out here by asking which one has replication
// buffers. Server-side script runs against server state: reading the client's
// copy of a component gives a value that looks right but is a replica, and
// writing it is overwritten on the next update.
//
// Falls back to whatever was captured first if neither slot identifies as the
// server yet, so nothing that used to work stops working before the second
// container has been seen.
void* server_container() {
    if (bg3le_container_is_server(ecs::container())) return ecs::container();
    if (bg3le_container_is_server(ecs::container_alt())) return ecs::container_alt();
    return ecs::container();
}

// The client world's container: the captured one that is not the server's,
// once the two have been told apart.
void* client_container() {
    void* server = server_container();
    if (!bg3le_container_is_server(server)) return nullptr;
    void* other = ecs::container() != server ? ecs::container() : ecs::container_alt();
    return other != server ? other : nullptr;
}

extern "C" void* bg3le_context_container(bool client);

// The world the running context reads, as upstream's GetEntityWorld(): the
// client context the client's EntityWorld, the server context the server's,
// each taken from its EoC object, so it is there from the main menu on. The
// captured containers stand in only if that lookup fails.
void* world_container() {
    const bool client = g_lua != nullptr && g_lua == g_client_lua;
    if (void* own = bg3le_context_container(client)) return own;
    static std::atomic<bool> warned[2] = {false, false};
    void* captured = client && client_container() != nullptr ? client_container() : server_container();
    if (captured != nullptr && !warned[client ? 1 : 0].exchange(true)) {
        logf("ecs: the %s EntityWorld was not found from its EoC object; using a captured container",
             client ? "client" : "server");
    }
    return captured;
}

// Accepts either name a component goes by and yields the engine's.
//
// bg3se describes 1,071 components and the symbol table has rather more, so a
// name it has no metadata for is passed through unchanged. That keeps the raw
// engine names working for the components bg3se has not mapped, which is the
// only way to reach them at all.
const char* engine_name_of(const char* name) {
    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) return name;
    const char* engine = bg3le_meta_engine_class(meta);
    return engine != nullptr ? engine : name;
}

// Resolves a component name to its engine index, trying the one-frame registry
// as a fallback.
std::optional<std::int32_t> component_index(const char* name) {
    if (auto i = ecs::index_of(ecs::Context::Component, name)) return i;

    // A one-frame component's engine index carries 0x8000, which is what
    // ecs::IsOneFrame tests for. The two registries are numbered
    // independently, so without the flag a one-frame index silently collides
    // with an unrelated inline component -- which is what put 17 of them in
    // SizeAudit's mismatch list. Those were never size disagreements; they
    // were comparisons against whichever inline component shared the number.
    if (auto i = ecs::index_of(ecs::Context::OneFrameComponent, name)) {
        return *i | 0x8000;
    }
    return std::nullopt;
}

// Generic component access, driven by bg3se's field tables rather than by a
// hand-written accessor per component.
//
// The type decode stays here rather than in the bridge so that Lua and bg3se
// remain in separate translation units: the bridge hands back a raw component
// pointer, and the offset and kind come from the metadata.
//
// Mirrors bg3le::FieldKind in src/component_meta_abi.h.
enum class FieldKind : std::uint8_t {
    Unsupported = 0, Bool, Float, Double, Int8, Uint8, Int16, Uint16,
    Int32, Uint32, Int64, Uint64, Guid, Entity, FixedString, LSString,
    ScalarArray, Struct, DynArray, Map, Optional, Variant, Inherit,
    ComponentHandle, ConditionId, Pointer, Text, Version, EntityOrVec3,
    BitArray,
};

extern "C" const char* bg3le_meta_kind_name(std::uint8_t kind);

// Named in one place, in src/vendor/component_meta.cpp, because these were
// duplicated and inserting a kind mid-enum renumbered everything after it.
const char* field_kind_name(FieldKind kind) {
    return bg3le_meta_kind_name((std::uint8_t)kind);
}

// The stride of a scalar kind, used to walk a fixed-extent array. Zero for
// anything that is not a scalar, which stops an array of them being walked.
std::size_t field_kind_size(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: case FieldKind::Int8: case FieldKind::Uint8:
            return 1;
        case FieldKind::Int16: case FieldKind::Uint16:
            return 2;
        case FieldKind::Float: case FieldKind::Int32: case FieldKind::Uint32:
            return 4;
        case FieldKind::Double: case FieldKind::Int64: case FieldKind::Uint64:
        case FieldKind::Entity: case FieldKind::ComponentHandle:
        case FieldKind::Pointer:
            return 8;
        case FieldKind::FixedString: case FieldKind::ConditionId:
            return 4;
        case FieldKind::Guid:
            return 16;
        default:
            return 0;
    }
}

// Resolves an entity's component to a live pointer, using the metadata's size
// as the page stride and the symbol table's index as the type.
// name may be either the engine name or bg3se's short name; the index is
// always looked up under the engine name, which the metadata supplies.
void* component_pointer(std::uint64_t handle, const char* name,
                        void const** meta) {
    *meta = bg3le_meta_component(name);
    if (*meta == nullptr) return nullptr;

    const char* engineName = bg3le_meta_engine_class(*meta);
    if (engineName == nullptr) return nullptr;

    const auto index = component_index(engineName);
    if (!index.has_value()) return nullptr;

    // A one-frame component is not in the entity page at all: the engine keeps
    // it in a per-storage pool keyed by entity. Reading one through the page
    // returns whatever is at that offset, which is what the one-frame entries
    // in SizeAudit were -- not a wrong struct, a wrong mechanism.
    const auto type = static_cast<std::uint16_t>(*index);
    void* slot = nullptr;
    if (bg3le_meta_component_is_one_frame(*meta)) {
        slot = bg3le_entity_one_frame_component(world_container(), handle, type);
        if (slot != nullptr) return slot;
    } else {
        // The stride, not the struct size. For a proxy component the page holds a
        // pointer and the struct lives wherever it points, so passing the struct
        // size would stride the page wrongly and then read the pointer's own bytes
        // as the first fields. 46 of the components a live save carries are
        // proxies, so this is not an edge case.
        slot = bg3le_entity_component(world_container(), handle, type,
                                      bg3le_meta_component_stride(*meta));
    }
    if (slot == nullptr) {
        // Not committed yet; the immediate cache holds the component itself.
        bool immediate = false;
        slot = bg3le_entity_pending_component(world_container(), handle, type, &immediate);
        if (slot == nullptr || immediate) return slot;
    }

    if (bg3le_meta_component_is_proxy(*meta)) {
        void* target = nullptr;
        if (!safe_read(slot, &target, sizeof(target))) return nullptr;
        return target;
    }
    return slot;
}

extern "C" char const* bg3le_stats_attr_condition(int raw);

// Pushes a field, read through safe_read so a stale handle yields nil rather
// than a fault.
extern "C" bool bg3le_meta_read_text(void const* handle, char const* path, void* base,
                                     char const** data, std::size_t* size, bool* present);

// A Text field: a string, or nil, as upstream pushes it.
int push_text(lua_State* L, void const* meta, char const* path, void* base) {
    char const* data = nullptr;
    std::size_t size = 0;
    bool present = false;
    if (!bg3le_meta_read_text(meta, path, base, &data, &size, &present)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s cannot be read", path);
        return 2;
    }
    if (present) {
        lua_pushlstring(L, data, size);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

bool push_field(lua_State* L, const void* address, FieldKind kind,
                FieldKind elemKind = FieldKind::Unsupported,
                std::uint16_t elemCount = 0) {
    std::uint64_t raw = 0;
    switch (kind) {
        case FieldKind::ScalarArray: {
            // One-based, as Lua tables are. The stride comes from the element
            // kind, and a partial read fails the whole field rather than
            // returning a short table.
            const std::size_t stride = field_kind_size(elemKind);
            if (stride == 0) return false;
            lua_createtable(L, elemCount, 0);
            for (std::uint16_t i = 0; i < elemCount; ++i) {
                if (!push_field(L, (const char*)address + i * stride,
                                elemKind)) {
                    lua_pop(L, 1);
                    return false;
                }
                lua_rawseti(L, -2, i + 1);
            }
            return true;
        }
        case FieldKind::Bool:
            if (!safe_read(address, &raw, 1)) return false;
            lua_pushboolean(L, (int)(raw & 0xff));
            return true;
        case FieldKind::Int8:
            if (!safe_read(address, &raw, 1)) return false;
            lua_pushinteger(L, (std::int8_t)raw);
            return true;
        case FieldKind::Uint8:
            if (!safe_read(address, &raw, 1)) return false;
            lua_pushinteger(L, (std::uint8_t)raw);
            return true;
        case FieldKind::Int16:
            if (!safe_read(address, &raw, 2)) return false;
            lua_pushinteger(L, (std::int16_t)raw);
            return true;
        case FieldKind::Uint16:
            if (!safe_read(address, &raw, 2)) return false;
            lua_pushinteger(L, (std::uint16_t)raw);
            return true;
        case FieldKind::Int32:
            if (!safe_read(address, &raw, 4)) return false;
            lua_pushinteger(L, (std::int32_t)raw);
            return true;
        case FieldKind::Uint32:
            if (!safe_read(address, &raw, 4)) return false;
            lua_pushinteger(L, (std::uint32_t)raw);
            return true;
        case FieldKind::Int64:
        case FieldKind::Entity:
            if (!safe_read(address, &raw, 8)) return false;
            lua_pushinteger(L, (lua_Integer)(std::int64_t)raw);
            return true;
        case FieldKind::Uint64:
            if (!safe_read(address, &raw, 8)) return false;
            lua_pushinteger(L, (lua_Integer)raw);
            return true;
        case FieldKind::ComponentHandle:
            // Upstream's push: the handle's bits, or nil for NullHandle.
            if (!safe_read(address, &raw, 8)) return false;
            if (raw == 0xFFC0000000000000ull) lua_pushnil(L);
            else lua_pushinteger(L, (lua_Integer)raw);
            return true;
        case FieldKind::Version: {
            // Upstream's push: {major, minor, revision, build}.
            if (!safe_read(address, &raw, 8)) return false;
            lua_createtable(L, 4, 0);
            lua_pushinteger(L, (lua_Integer)(raw >> 55));
            lua_rawseti(L, -2, 1);
            lua_pushinteger(L, (lua_Integer)((raw >> 47) & 0xff));
            lua_rawseti(L, -2, 2);
            lua_pushinteger(L, (lua_Integer)((raw >> 31) & 0xffff));
            lua_rawseti(L, -2, 3);
            lua_pushinteger(L, (lua_Integer)(raw & 0x7fffffff));
            lua_rawseti(L, -2, 4);
            return true;
        }
        case FieldKind::BitArray: {
            // Upstream's push: one boolean per bit.
            std::vector<unsigned char> bits((elemCount + 7) / 8);
            if (!safe_read(address, bits.data(), bits.size())) return false;
            lua_createtable(L, elemCount, 0);
            for (std::uint16_t i = 0; i < elemCount; ++i) {
                lua_pushboolean(L, (bits[i / 8] >> (i % 8)) & 1);
                lua_rawseti(L, -2, i + 1);
            }
            return true;
        }
        case FieldKind::EntityOrVec3: {
            // Upstream's push: the position when the type byte is set, else
            // the entity (its handle here; the prelude makes it one).
            unsigned char bytes[13] = {};
            if (!safe_read(address, bytes, sizeof(bytes))) return false;
            if (bytes[12] != 0) {
                float v[3];
                std::memcpy(v, bytes, sizeof(v));
                lua_createtable(L, 3, 0);
                for (int i = 0; i < 3; ++i) {
                    lua_pushnumber(L, v[i]);
                    lua_rawseti(L, -2, i + 1);
                }
            } else {
                std::memcpy(&raw, bytes, 8);
                if (raw == 0xFFC0000000000000ull) lua_pushnil(L);
                else lua_pushinteger(L, (lua_Integer)raw);
            }
            return true;
        }
        case FieldKind::Pointer: {
            // The target's address, which the prelude follows; nil for null.
            // One that could not be an object, or cannot be read, is not
            // handed out -- it is a stale pointer or a layout that is not the
            // engine's, and following it would read garbage.
            if (!safe_read(address, &raw, 8)) return false;
            if (raw == 0) {
                lua_pushnil(L);
                return true;
            }
            std::uint64_t probe = 0;
            if (raw < 0x10000 || raw >= 0x800000000000ull || (raw & 7) != 0
                || !safe_read((void const*)raw, &probe, sizeof(probe))) {
                return false;
            }
            lua_pushinteger(L, (lua_Integer)raw);
            return true;
        }
        case FieldKind::ConditionId: {
            // Upstream's push: ConditionId::Get's text, or "" for none.
            std::int32_t id = -1;
            if (!safe_read(address, &id, 4)) return false;
            char const* text = id < 0 ? nullptr : bg3le_stats_attr_condition(id);
            lua_pushstring(L, text != nullptr ? text : "");
            return true;
        }
        case FieldKind::Float: {
            float f = 0;
            if (!safe_read(address, &f, 4)) return false;
            lua_pushnumber(L, f);
            return true;
        }
        case FieldKind::Double: {
            double d = 0;
            if (!safe_read(address, &d, 8)) return false;
            lua_pushnumber(L, d);
            return true;
        }
        case FieldKind::LSString: {
            std::string text;
            if (!read_ls_string(address, &text)) return false;
            lua_pushlstring(L, text.data(), text.size());
            return true;
        }
        case FieldKind::FixedString: {
            // The index is meaningless on its own, so an unresolved one is
            // reported rather than pushed as a number: a bare integer would
            // read as a value and it is not one.
            std::uint32_t index = 0;
            if (!safe_read(address, &index, 4)) return false;
            // The null FixedString is the empty string to Lua, not nil: that
            // is upstream's push, which sends anything falsy -- Index ==
            // NullIndex -- to "". It matters beyond looks, since a mod
            // testing `x ~= ""` gets the opposite answer from nil, and a key
            // holding nil does not exist in the table at all. Reading these
            // as nil left twenty-six of a character template's fields absent.
            if (index == 0xffffffffu) {
                lua_pushliteral(L, "");
                return true;
            }
            std::uint32_t length = 0;
            const char* text = bg3le_fixed_string(index, &length);
            if (text == nullptr) {
                lua_pushfstring(L,
                    bg3le_string_table() == nullptr
                        ? "<string table not found>"
                        : "<unresolved string %d>", (int)index);
                return true;
            }
            char buf[513];
            if (length > sizeof(buf) - 1) length = sizeof(buf) - 1;
            if (!safe_read(text, buf, length)) {
                lua_pushstring(L, "<unreadable string>");
                return true;
            }
            lua_pushlstring(L, buf, length);
            return true;
        }
        case FieldKind::Guid: {
            // Formatted by bg3se rather than here. The byte order is not the
            // obvious one -- see bg3le_meta_format_guid -- and open-coding it
            // produced a UUID that looked right and was not.
            std::uint8_t b[16];
            if (!safe_read(address, b, sizeof(b))) return false;
            char text[40];
            if (!bg3le_meta_format_guid(b, text, sizeof(text))) return false;
            lua_pushstring(L, text);
            return true;
        }
        default:
            return false;
    }
}

extern "C" bool bg3le_meta_parse_guid(const char* text, void* out);

// An entity is a full userdata holding its handle, with the "EntityProxy"
// metatable, as upstream's entities are userdata. False for anything else.
bool entity_proxy_handle(lua_State* L, int index, std::uint64_t* handle) {
    if (lua_type(L, index) != LUA_TUSERDATA || !lua_getmetatable(L, index)) {
        return false;
    }
    lua_getfield(L, -1, "__name");
    const bool isEntity = lua_isstring(L, -1)
                          && std::strcmp(lua_tostring(L, -1), "EntityProxy") == 0;
    lua_pop(L, 2);
    if (!isEntity) return false;
    std::memcpy(handle, lua_touserdata(L, index), sizeof(*handle));
    return true;
}

// Writes a field, converting the Lua value the way upstream's get<T> does.
//
// This used to refuse GUIDs and entity handles on purpose, as "not something
// a mod should be doing by accident". Upstream writes both, so the refusal
// was a gap rather than a safeguard: a mod written against it failed here.
// What upstream does insist on is the type -- a GUID must be a string that
// parses, an entity must be an entity or nil -- and so does this.
bool write_field(lua_State* L, int index, void* address, FieldKind kind,
                 FieldKind elemKind = FieldKind::Unsupported,
                 std::uint16_t elemCount = 0) {
    switch (kind) {
        case FieldKind::Guid: {
            // luaL_checklstring and Guid::ParseGuidString, as upstream's
            // do_get<Guid>, with its message for one that does not parse.
            std::size_t length = 0;
            const char* text = luaL_checklstring(L, index, &length);
            unsigned char guid[16] = {};
            if (!bg3le_meta_parse_guid(text, guid)) {
                luaL_error(L, "Param %d: not a valid GUID value: '%s'", index,
                           text);
                return false;
            }
            std::memcpy(address, guid, sizeof(guid));
            return true;
        }
        case FieldKind::Entity: {
            // nil is the null handle; anything else has to be an entity.
            // Upstream takes it out of an entity proxy and rejects anything
            // else, a bare number included -- a handle's bits are not
            // something to type in.
            std::uint64_t handle = 0xFFC0000000000000ull;  // NullHandle
            if (!lua_isnil(L, index) && !entity_proxy_handle(L, index, &handle)) {
                luaL_error(L, "Param %d: expected an entity or nil, got %s",
                           index, luaL_typename(L, index));
                return false;
            }
            std::memcpy(address, &handle, sizeof(handle));
            return true;
        }
        case FieldKind::ComponentHandle: {
            // Upstream's get: nil is NullHandle, anything else an integer.
            std::uint64_t handle = 0xFFC0000000000000ull;
            if (!lua_isnil(L, index)) handle = (std::uint64_t)lua_tointeger(L, index);
            std::memcpy(address, &handle, sizeof(handle));
            return true;
        }
        case FieldKind::ConditionId:
            luaL_error(L, "Setting ConditionId values is not supported");
            return false;
        case FieldKind::BitArray: {
            if (!lua_istable(L, index)) return false;
            std::vector<unsigned char> bits((elemCount + 7) / 8);
            for (std::uint16_t i = 0; i < elemCount; ++i) {
                lua_rawgeti(L, index, i + 1);
                if (lua_toboolean(L, -1)) bits[i / 8] |= (unsigned char)(1u << (i % 8));
                lua_pop(L, 1);
            }
            std::memcpy(address, bits.data(), bits.size());
            return true;
        }
        case FieldKind::ScalarArray: {
            const std::size_t stride = field_kind_size(elemKind);
            if (stride == 0 || !lua_istable(L, index)) return false;
            // Written element-wise so a short table leaves the rest alone
            // rather than zeroing it.
            for (std::uint16_t i = 0; i < elemCount; ++i) {
                lua_rawgeti(L, index, i + 1);
                if (!lua_isnil(L, -1)) {
                    if (!write_field(L, lua_gettop(L),
                                     (char*)address + i * stride, elemKind)) {
                        lua_pop(L, 1);
                        return false;
                    }
                }
                lua_pop(L, 1);
            }
            return true;
        }
        case FieldKind::Bool: {
            const std::uint8_t v = lua_toboolean(L, index) != 0 ? 1 : 0;
            std::memcpy(address, &v, 1);
            return true;
        }
        case FieldKind::Int8: case FieldKind::Uint8: {
            const auto v = (std::uint8_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 1);
            return true;
        }
        case FieldKind::Int16: case FieldKind::Uint16: {
            const auto v = (std::uint16_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 2);
            return true;
        }
        case FieldKind::Int32: case FieldKind::Uint32: {
            const auto v = (std::uint32_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 4);
            return true;
        }
        case FieldKind::Int64: case FieldKind::Uint64: {
            const auto v = (std::uint64_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 8);
            return true;
        }
        case FieldKind::Float: {
            const auto v = (float)luaL_checknumber(L, index);
            std::memcpy(address, &v, 4);
            return true;
        }
        case FieldKind::Double: {
            const double v = luaL_checknumber(L, index);
            std::memcpy(address, &v, 8);
            return true;
        }
        case FieldKind::FixedString: {
            // The id the engine already holds for that text, or a new entry
            // for text it does not. The old id's reference is not released:
            // bg3le did not take it and the engine may still be holding it
            // elsewhere, and an over-count keeps a string alive where an
            // under-count frees one out from under a reader.
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            if (text == nullptr) return false;

            // Interned through the engine first (see bg3le_fixed_string_intern),
            // so a string created after startup gets the id its readers use.
            std::uint32_t id = 0;
            if (!bg3le_fixed_string_intern(text, &id)) return false;
            std::memcpy(address, &id, sizeof(id));
            return true;
        }
        case FieldKind::LSString: {
            std::size_t length = 0;
            const char* text = lua_tolstring(L, index, &length);
            if (text == nullptr) return false;
            return bg3le_meta_lsstring_assign(address, text, length);
        }
        default:
            return false;
    }
}

// A copy of the table at `index`, pushed, with its values in the order
// upstream's iterate() visits them: integer keys ascending, then the rest.
// Holes close up, as they do there; walking 1..#t stopped at the first nil.
int push_packed(lua_State* L, int index) {
    index = lua_absindex(L, index);
    std::vector<lua_Integer> keys;
    bool others = false;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_isinteger(L, -2)) keys.push_back(lua_tointeger(L, -2));
        else others = true;
        lua_pop(L, 1);
    }
    std::sort(keys.begin(), keys.end());
    lua_createtable(L, (int)keys.size(), 0);
    const int out = lua_gettop(L);
    lua_Integer n = 0;
    for (lua_Integer key : keys) {
        lua_rawgeti(L, index, key);
        lua_rawseti(L, out, ++n);
    }
    if (others) {
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (!lua_isinteger(L, -2)) {
                lua_pushvalue(L, -1);
                lua_rawseti(L, out, ++n);
            }
            lua_pop(L, 1);
        }
    }
    return out;
}

// A whole array assigned from a Lua list, as upstream's Array setter does:
// the array becomes that many elements, each written as an element is.
bool assign_array(lua_State* L, int index, void const* meta, const char* path,
                  void* base, FieldKind kind) {
    if (kind != FieldKind::DynArray || !lua_istable(L, index)) return false;
    const int packed = push_packed(L, index);
    struct Pop {
        lua_State* L;
        int at;
        ~Pop() { lua_remove(L, at); }
    } pop{L, packed};
    index = packed;
    const lua_Integer n = luaL_len(L, index);
    void* data = nullptr;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    if (n < 0 || !bg3le_meta_array_resize(meta, path, base, (std::size_t)n,
                                          &data, &elemSize, &elemKind)) {
        return false;
    }
    for (lua_Integer i = 0; i < n; ++i) {
        lua_geti(L, index, i + 1);
        write_field(L, lua_gettop(L), static_cast<char*>(data) + i * elemSize,
                    (FieldKind)elemKind);
        lua_pop(L, 1);
    }
    return true;
}

// An enum field assigned by label, as upstream allows: the label, or for a
// bitmask a list of labels, becomes its value in place. Anything else is left
// for write_field to take or reject.
void enum_arg_in_place(lua_State* L, int index, void const* meta,
                       const char* path) {
    const char* label = nullptr;
    std::uint64_t value = 0;
    bool isBitmask = false;
    index = lua_absindex(L, index);
    // An EnumValue stands for its label, a BitfieldValue for its value.
    if (lua_type(L, index) == LUA_TUSERDATA && lua_getmetatable(L, index)) {
        if (lua_getfield(L, -1, "__bg3leEnumLabel") == LUA_TSTRING) {
            lua_replace(L, index);
            lua_pop(L, 1);
        } else if (lua_getfield(L, -2, "__bg3leBitfieldValue") == LUA_TNUMBER) {
            lua_replace(L, index);
            lua_pop(L, 2);
            return;
        } else {
            lua_pop(L, 3);
        }
    }
    const int type = lua_type(L, index);
    if ((type != LUA_TSTRING && type != LUA_TTABLE)
        || !bg3le_meta_enum_label(meta, path, 0, &label, &value, &isBitmask)) {
        return;
    }
    auto lookup = [&](const char* wanted, std::uint64_t* out) {
        for (std::size_t i = 0;
             bg3le_meta_enum_label(meta, path, i, &label, &value, &isBitmask);
             ++i) {
            if (std::strcmp(label, wanted) == 0) {
                *out = value;
                return true;
            }
        }
        return false;
    };

    std::uint64_t result = 0;
    if (type == LUA_TSTRING) {
        if (!lookup(lua_tostring(L, index), &result)) {
            luaL_error(L, "'%s' is not a label of %s", lua_tostring(L, index), path);
        }
    } else {
        if (!isBitmask) return;
        const lua_Integer n = luaL_len(L, index);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, index, i);
            std::uint64_t bit = 0;
            if (!lua_isstring(L, -1) || !lookup(lua_tostring(L, -1), &bit)) {
                luaL_error(L, "'%s' is not a label of %s",
                           luaL_tolstring(L, -1, nullptr), path);
            }
            result |= bit;
            lua_pop(L, 1);
        }
    }
    lua_pushinteger(L, (lua_Integer)result);
    lua_replace(L, index);
}

// A fixed array of enums assigned by labels: each element converted as a
// field of that enum takes it, into a copy that replaces the argument.
void enum_elements_in_place(lua_State* L, int index, void const* meta,
                            const char* path, std::uint16_t count) {
    index = lua_absindex(L, index);
    const std::string element = std::string(path) + "[0]";
    const char* label = nullptr;
    std::uint64_t value = 0;
    bool isBitmask = false;
    if (!lua_istable(L, index)
        || !bg3le_meta_enum_label(meta, element.c_str(), 0, &label, &value, &isBitmask)) {
        return;
    }
    lua_createtable(L, count, 0);
    for (int i = 1; i <= count; ++i) {
        lua_geti(L, index, i);
        enum_arg_in_place(L, lua_gettop(L), meta, element.c_str());
        lua_seti(L, -2, i);
    }
    lua_replace(L, index);
}

// Pushes an enum-typed field as its label, or a bitmask as the list of set
// flags -- which is how bg3se presents them, and scripts are written against
// that. Returns false if the field is not an enum, leaving the caller to push
// the raw integer.
//
// A value with no matching label is pushed as the number, so an unmapped bit
// is visible rather than dropped.
extern "C" char const* bg3le_meta_enum_lua_name(void const* handle, char const* path);

// An enum value as upstream pushes it: an EnumValue (Ext._Internal.EnumValue
// in the prelude), or the label where the prelude has none yet.
void push_enum_value(lua_State* L, void const* meta, const char* path,
                     const char* label, std::uint64_t value) {
    char const* name = bg3le_meta_enum_lua_name(meta, path);
    const int top = lua_gettop(L);
    if (name != nullptr && lua_getglobal(L, "Ext") == LUA_TTABLE
        && lua_getfield(L, -1, "_Internal") == LUA_TTABLE
        && lua_getfield(L, -1, "EnumValue") == LUA_TFUNCTION) {
        lua_pushstring(L, name);
        lua_pushstring(L, label);
        lua_pushinteger(L, (lua_Integer)value);
        lua_call(L, 3, 1);
        lua_replace(L, top + 1);
        lua_settop(L, top + 1);
        return;
    }
    lua_settop(L, top);
    lua_pushstring(L, label);
}

bool push_enum(lua_State* L, void const* meta, const char* path,
               std::uint64_t raw) {
    const char* label = nullptr;
    std::uint64_t value = 0;
    bool isBitmask = false;
    if (!bg3le_meta_enum_label(meta, path, 0, &label, &value, &isBitmask)) {
        return false;
    }

    if (!isBitmask) {
        for (std::size_t i = 0;
             bg3le_meta_enum_label(meta, path, i, &label, &value, &isBitmask);
             ++i) {
            if (value == raw) {
                push_enum_value(L, meta, path, label, value);
                return true;
            }
        }
        lua_pushinteger(L, (lua_Integer)raw);
        return true;
    }

    // A BitfieldValue (Ext._Internal.BitfieldValue in the prelude), as
    // upstream's push_bitfield_value; unnamed bits stay in its value.
    if (char const* name = bg3le_meta_enum_lua_name(meta, path)) {
        const int top = lua_gettop(L);
        if (lua_getglobal(L, "Ext") == LUA_TTABLE
            && lua_getfield(L, -1, "_Internal") == LUA_TTABLE
            && lua_getfield(L, -1, "BitfieldValue") == LUA_TFUNCTION) {
            lua_pushstring(L, name);
            lua_pushinteger(L, (lua_Integer)raw);
            lua_call(L, 2, 1);
            lua_replace(L, top + 1);
            lua_settop(L, top + 1);
            return true;
        }
        lua_settop(L, top);
    }

    lua_newtable(L);
    int n = 0;
    std::uint64_t matched = 0;
    for (std::size_t i = 0;
         bg3le_meta_enum_label(meta, path, i, &label, &value, &isBitmask);
         ++i) {
        if (value != 0 && (raw & value) == value) {
            lua_pushstring(L, label);
            lua_rawseti(L, -2, ++n);
            matched |= value;
        }
    }
    // Bits the table does not name are dropped, because upstream drops
    // them: a functor whose Flags byte is 112 reports an empty array there,
    // since FunctorFlags only names 1, 2 and 4. Appending the leftover as a
    // number kept the information but put an element in the array that no
    // mod written against bg3se expects. Logged once instead, so it is not
    // lost either.
    if ((raw & ~matched) != 0) {
        static std::uint64_t reported = 0;
        if ((raw & ~matched & ~reported) != 0) {
            reported |= raw & ~matched;
            logf("meta: %s has bits %#llx that its enumeration does not "
                 "name", path, (unsigned long long)(raw & ~matched));
        }
    }
    return true;
}

extern "C" bool bg3le_meta_optional_get(void const* handle, char const* path,
                                        void* component, bool* engaged,
                                        void** payload, std::uint8_t* kind,
                                        std::uint8_t* elemKind,
                                        std::uint16_t* elemCount);

// Reading an optional field: its value, or nil if it holds none.
//
// Like the write, this needs the field's descriptor rather than an address,
// because has_value() belongs to the type. Returns false if the path is not
// an optional, so the caller can fall through to the ordinary read.
bool read_optional(lua_State* L, void const* meta, char const* path,
                   void* base) {
    bool engaged = false;
    void* payload = nullptr;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_optional_get(meta, path, base, &engaged, &payload, &kind,
                                 &elemKind, &elemCount)) {
        return false;
    }

    if (!engaged || payload == nullptr) {
        lua_pushnil(L);
        return true;
    }
    if (!push_field(L, payload, (FieldKind)kind, (FieldKind)elemKind,
                    elemCount)) {
        lua_pushnil(L);
    }
    return true;
}

// Ext._Internal.GetField(handle, component, path)
//
// path may name a field, a field of a nested struct, or an element of an
// array: "Hp", "Transform.Translate", "Events[0].Amount". Resolving it is the
// C side's job, because an array element does not live at a fixed offset from
// the component -- its address is behind the container's own buffer pointer.
int l_get_field(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for %s", name);
        return 2;
    }
    if (component == nullptr) {
        lua_pushnil(L);
        return 1;  // the entity simply does not have this component
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                            &readOnly)) {
        // P_BITMASK: a flag of a flags field reads as a boolean.
        std::uint64_t mask = 0;
        std::uint64_t raw = 0;
        if (bg3le_meta_bitflag(meta, path, component, &address, &size, &mask)
            && size <= 8 && safe_read(address, &raw, size)) {
            lua_pushboolean(L, (raw & mask) == mask ? 1 : 0);
            return 1;
        }
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }

    // Element kind and count only matter for a fixed-extent array, which
    // push_field turns into a table; the dynamic ones are proxied in Lua.
    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    // An enum keeps its underlying integer kind, so it is read as a number
    // and then rendered as a label.
    const std::size_t width = field_kind_size((FieldKind)kind);
    if (width != 0 && width <= 8) {
        std::uint64_t raw = 0;
        if (safe_read(address, &raw, width)
            && push_enum(L, meta, path, raw)) {
            return 1;
        }
    }

    if ((FieldKind)kind == FieldKind::Optional
        && read_optional(L, meta, path, component)) {
        return 1;
    }
    if ((FieldKind)kind == FieldKind::Text) return push_text(L, meta, path, component);

    if (!push_field(L, address, (FieldKind)kind, (FieldKind)elemKind,
                    elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is of an unsupported kind (%s)", name, path,
                        field_kind_name((FieldKind)kind));
        return 2;
    }
    return 1;
}

extern "C" bool bg3le_meta_optional_set(void const* handle, char const* path,
                                        void* component, bool engaged,
                                        void** payload, std::uint8_t* kind,
                                        std::uint8_t* elemKind,
                                        std::uint16_t* elemCount);

extern "C" bool bg3le_meta_after_write(void const* handle, char const* path,
                                      void* component, bool unserializing,
                                      void* entityWorld);
extern "C" void* bg3le_entity_world(void* container);
void* server_container();

// Writing an optional field: engage it and write the payload, or clear it.
//
// Separate from write_field because engaging one needs the field's own
// descriptor -- only the type knows where libc++ keeps the flag -- and
// write_field has an address and a kind, not a descriptor.
//
// Returns false with a message pushed if it did not work.
bool write_optional(lua_State* L, int index, void const* meta,
                    char const* path, void* base) {
    if (lua_isnoneornil(L, index)) {
        return bg3le_meta_optional_set(meta, path, base, false, nullptr,
                                       nullptr, nullptr, nullptr);
    }

    void* payload = nullptr;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_optional_set(meta, path, base, true, &payload, &kind,
                                 &elemKind, &elemCount)) {
        return false;
    }
    if (payload == nullptr) return false;

    return write_field(L, index, payload, (FieldKind)kind,
                       (FieldKind)elemKind, elemCount);
}

// Ext._Internal.SetField(handle, component, path, value)
int l_set_field(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                            &readOnly)) {
        // P_BITMASK: assigning a flag sets or clears its bit.
        std::uint64_t mask = 0;
        std::uint64_t raw = 0;
        if (bg3le_meta_bitflag(meta, path, component, &address, &size, &mask)
            && size <= 8 && safe_read(address, &raw, size)) {
            raw = lua_toboolean(L, 4) ? (raw | mask) : (raw & ~mask);
            std::memcpy(address, &raw, size);
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }
    if (readOnly) {
        lua_pushnil(L);
        lua_pushfstring(L,
            "%s.%s is read-only (a hash set's keys, a queue, or a value "
            "upstream only reads)", name, path);
        return 2;
    }

    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    if ((FieldKind)kind == FieldKind::Optional) {
        if (!write_optional(L, 4, meta, path, component)) {
            lua_pushnil(L);
            lua_pushfstring(L, "%s.%s is an optional bg3le cannot write",
                            name, path);
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    if (assign_array(L, 4, meta, path, component, (FieldKind)kind)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    enum_arg_in_place(L, 4, meta, path);
    if ((FieldKind)kind == FieldKind::ScalarArray) {
        enum_elements_in_place(L, 4, meta, path, elemCount);
    }
    if (!write_field(L, 4, address, (FieldKind)kind, (FieldKind)elemKind,
                     elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not writable (%s)", name, path,
                        field_kind_name((FieldKind)kind));
        return 2;
    }
    // Whatever upstream's setter does beyond the value: an
    // OverrideableProperty marked overridden, an empty EntityRef given a
    // world. A no-op for any other field.
    bg3le_meta_after_write(meta, path, component, false,
                           bg3le_entity_world(world_container()));
    lua_pushboolean(L, 1);
    return 1;
}

// Map edits: the key at `index` written as the map's key type, then added or
// removed. Shared by MapEdit (a component) and ObjectMapEdit (an object).
extern "C" bool bg3le_meta_map_key_info(void const* handle, char const* path,
                                        std::uint8_t* kind, std::uint16_t* size,
                                        char const** enumName, std::uint16_t* enumLength);
extern "C" bool bg3le_meta_map_edit(void const* handle, char const* path, void* component,
                                    void const* key, bool insert);
extern "C" bool bg3le_meta_enum_label_value(char const* enumName, char const* label,
                                           std::uint64_t* value);
extern "C" void const* bg3le_meta_class(char const* className);
int map_edit(lua_State* L, void const* meta, void* base, char const* path, int keyIndex,
             bool insert) {
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    char const* enumName = nullptr;
    std::uint16_t enumLength = 0;
    if (!bg3le_meta_map_key_info(meta, path, &kind, &size, &enumName, &enumLength)
        || size > 32) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a map bg3le can edit", path);
        return 2;
    }
    alignas(16) unsigned char key[32] = {};
    bool written = false;
    if (enumName != nullptr && lua_type(L, keyIndex) == LUA_TSTRING) {
        std::uint64_t value = 0;
        if (bg3le_meta_enum_label_value(std::string(enumName, enumLength).c_str(),
                                        lua_tostring(L, keyIndex), &value)) {
            std::memcpy(key, &value, size);
            written = true;
        }
    }
    if (!written && !write_field(L, keyIndex, key, (FieldKind)kind, (FieldKind)0, 0)) {
        lua_pushnil(L);
        lua_pushfstring(L, "the key is not a %s", field_kind_name((FieldKind)kind));
        return 2;
    }
    if (!bg3le_meta_map_edit(meta, path, base, key, insert)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s could not %s the key", path, insert ? "add" : "remove");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.MapEdit(handle, component, path, key, insert) -> true, or nil and why
int l_map_edit(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }
    return map_edit(L, meta, component, luaL_checkstring(L, 3), 4, lua_toboolean(L, 5) != 0);
}

// Ext._Internal.ObjectMapEdit(address, class, path, key, insert) -> true, or nil and why
int l_object_map_edit(lua_State* L) {
    auto* base = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    void const* meta = bg3le_meta_class(luaL_checkstring(L, 2));
    if (meta == nullptr || base == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "no field metadata for this class");
        return 2;
    }
    return map_edit(L, meta, base, luaL_checkstring(L, 3), 4, lua_toboolean(L, 5) != 0);
}

// Entity component edits and this frame's command-buffer changes, as
// upstream's EntityProxyMetatable methods.
extern "C" void* bg3le_entity_component_add(void* container, std::uint64_t handle,
                                            std::uint16_t type, std::uint16_t inlineSize,
                                            void* dtor, bool immediate);
extern "C" bool bg3le_entity_component_remove(void* container, std::uint64_t handle,
                                              std::uint16_t type, std::uint16_t inlineSize,
                                              void* dtor, bool immediate);
extern "C" int bg3le_entity_ecb_changes(void* container, std::uint64_t handle,
                                        std::uint16_t* types, void** components,
                                        bool (*isProxy)(std::uint16_t), int max,
                                        std::uint32_t* flags);
extern "C" void* bg3le_meta_construct_component(void const* handle, void* slot);
extern "C" void* bg3le_meta_proxy_destroy(void const* handle);

// The component's meta and type index, for a name upstream's ExtComponentType
// accepts; one-frame components are not created or removed this way.
bool component_type(char const* name, void const** meta, std::uint16_t* type) {
    *meta = bg3le_meta_component(name);
    if (*meta == nullptr || bg3le_meta_component_is_one_frame(*meta)) return false;
    const char* engineName = bg3le_meta_engine_class(*meta);
    if (engineName == nullptr) return false;
    const auto index = component_index(engineName);
    if (!index.has_value()) return false;
    *type = (std::uint16_t)*index;
    return true;
}

// Ext._Internal.EntityComponentAdd(handle, name, immediate) -> address, class, or nil
int l_entity_component_add(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    void const* meta = nullptr;
    std::uint16_t type = 0;
    if (!component_type(luaL_checkstring(L, 2), &meta, &type)) return 0;
    const bool proxy = bg3le_meta_component_is_proxy(meta);
    void* slot = bg3le_entity_component_add(
        world_container(), handle, type, (std::uint16_t)bg3le_meta_component_stride(meta),
        proxy ? bg3le_meta_proxy_destroy(meta) : nullptr, lua_toboolean(L, 3) != 0);
    void* component = slot != nullptr ? bg3le_meta_construct_component(meta, slot) : nullptr;
    if (component == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)component);
    lua_pushstring(L, bg3le_meta_class_name(meta));
    return 2;
}

// Ext._Internal.EntityComponentRemove(handle, name, immediate) -> bool
int l_entity_component_remove(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    void const* meta = nullptr;
    std::uint16_t type = 0;
    if (!component_type(luaL_checkstring(L, 2), &meta, &type)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const bool proxy = bg3le_meta_component_is_proxy(meta);
    lua_pushboolean(L, bg3le_entity_component_remove(
        world_container(), handle, type, (std::uint16_t)bg3le_meta_component_stride(meta),
        proxy ? bg3le_meta_proxy_destroy(meta) : nullptr, lua_toboolean(L, 3) != 0));
    return 1;
}

// Ext._Internal.EntityEcbChanges(handle) -> { {Name, Address or true or false}, ... },
// flags; nil without a change record. Unmapped component types are skipped.
int l_entity_ecb_changes(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    constexpr int kMax = 512;
    std::uint16_t types[kMax];
    void* components[kMax];
    std::uint32_t flags = 0;
    auto shortName = [](std::uint16_t type) -> char const* {
        auto name = ecs::name_of(ecs::Context::Component, type);
        if (!name) return nullptr;
        void const* meta = bg3le_meta_component(name->c_str());
        return meta != nullptr ? bg3le_meta_short_name(meta) : nullptr;
    };
    const int n = bg3le_entity_ecb_changes(
        world_container(), handle, types, components,
        [](std::uint16_t type) {
            auto name = ecs::name_of(ecs::Context::Component, type);
            void const* meta = name ? bg3le_meta_component(name->c_str()) : nullptr;
            return meta != nullptr && bg3le_meta_component_is_proxy(meta);
        },
        kMax, &flags);
    if (n < 0) return 0;
    lua_createtable(L, n, 0);
    int at = 0;
    for (int i = 0; i < n; ++i) {
        char const* name = shortName(types[i]);
        if (name == nullptr) continue;
        lua_createtable(L, 3, 0);
        lua_pushstring(L, name);
        lua_rawseti(L, -2, 1);
        {
            auto engine = ecs::name_of(ecs::Context::Component, types[i]);
            void const* meta = engine ? bg3le_meta_component(engine->c_str()) : nullptr;
            char const* cls = meta != nullptr ? bg3le_meta_class_name(meta) : nullptr;
            if (cls != nullptr) {
                lua_pushstring(L, cls);
                lua_rawseti(L, -2, 3);
            }
        }
        if (components[i] == nullptr) lua_pushboolean(L, 0);
        else if (components[i] == (void*)1) lua_pushboolean(L, 1);
        else lua_pushinteger(L, (lua_Integer)(std::uintptr_t)components[i]);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, ++at);
    }
    lua_pushinteger(L, flags);
    return 2;
}

// Ext._Internal.EntityNetId(handle) -> the entity's NetId, or nil
extern "C" bool bg3le_entity_net_id(void* container, std::uint64_t handle, bool server,
                                    std::uint64_t* out);
int l_entity_net_id(lua_State* L) {
    std::uint64_t id = 0;
    if (!bg3le_entity_net_id(world_container(), static_cast<std::uint64_t>(luaL_checkinteger(L, 1)),
                             !in_client_state(), &id)) {
        return 0;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

// Ext._Internal.NetIdEntity(netId) -> the entity's handle, or nil
extern "C" bool bg3le_net_id_entity(void* container, std::uint64_t netId, bool server,
                                    std::uint64_t* out);
int l_net_id_entity(lua_State* L) {
    std::uint64_t handle = 0;
    if (!bg3le_net_id_entity(world_container(), static_cast<std::uint64_t>(luaL_checkinteger(L, 1)),
                             !in_client_state(), &handle)) {
        return 0;
    }
    lua_pushinteger(L, (lua_Integer)handle);
    return 1;
}

// Ext._Internal.UiObject(what, playerId) -> address, class; upstream's
// GetStateMachine, GetCursorControl, GetDragDrop and GetPickingHelper.
extern "C" void* bg3le_ls_resource_manager();
extern "C" void* bg3le_ui_state_machine(void* resourceManager);
extern "C" void* bg3le_cursor_control();
extern "C" void* bg3le_drag_drop(void* container, std::uint16_t playerId);
extern "C" void* bg3le_picking_helper(void* system, std::uint16_t playerIndex);
extern "C" bool bg3le_system_probe(void* container, std::int32_t index, void** system,
                                   std::int32_t* ownIndex, void** update, std::uint32_t* count);
std::optional<std::int32_t> system_index(char const* name);
int l_ui_object(lua_State* L) {
    if (!in_client_state()) return 0;
    const std::string what = luaL_checkstring(L, 1);
    const auto player = (std::uint16_t)luaL_optinteger(L, 2, 1);
    void* at = nullptr;
    char const* cls = nullptr;
    if (what == "StateMachine") {
        at = bg3le_ui_state_machine(bg3le_ls_resource_manager());
        cls = "ui::UIStateMachine";
    } else if (what == "CursorControl") {
        at = bg3le_cursor_control();
        cls = "ecl::CursorControl";
    } else if (what == "DragDrop") {
        at = bg3le_drag_drop(world_container(), player);
        cls = "ecl::PlayerDragData";
    } else if (what == "PickingHelper") {
        const auto index = system_index("ecl::PickingHelperManager");
        void* system = nullptr;
        void* update = nullptr;
        std::int32_t own = -1;
        std::uint32_t count = 0;
        if (index.has_value()
            && bg3le_system_probe(world_container(), *index, &system, &own, &update, &count)) {
            at = bg3le_picking_helper(system, player);
        }
        cls = "ecl::PlayerPickingHelper";
    }
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    lua_pushstring(L, cls);
    return 2;
}

// Ext._Internal.FieldInfo(component, path) -> kind, elemKind, elemCount
//
// Type information only, so it needs no entity. A dynamic array's length is
// not type information -- see ArrayInfo.
int l_field_info(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* path = luaL_checkstring(L, 2);

    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) return 0;

    std::uint32_t offset = 0;
    std::uint16_t size = 0;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_field(meta, path, &offset, &size, &kind, &elemKind,
                          &elemCount)) {
        return 0;
    }

    lua_pushstring(L, field_kind_name((FieldKind)kind));
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    lua_pushinteger(L, elemCount);
    return 3;
}

// Ext._Internal.ArrayInfo(handle, component, path) -> count, elementKind
//
// Needs the entity, because a dynamic array's length lives in the container
// rather than in the metadata. An element whose type is a struct bg3se
// describes is reported as "struct", so the caller knows to descend by path
// rather than to expect a value.
int l_array_info(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    std::size_t count = 0;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    const int status = bg3le_meta_array_length(meta, path, component, &count,
                                               &elemSize, &elemKind);
    if (status != 0) {
        static const char* const reasons[] = {
            "",
            "bad arguments",
            "the path does not resolve",
            "it is not a container",
            "it is a container with no length accessor",
        };
        lua_pushnil(L);
        lua_pushfstring(L, "cannot size %s.%s: %s", name, path,
                        (status >= 1 && status <= 4) ? reasons[status]
                                                     : "unknown error");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)count);
    // The element kind as it actually is, which for anything that is not a
    // scalar is "unsupported": the field tables only record a scalar kind for
    // elements. This used to answer "struct" instead, reasoning that a
    // non-scalar element must be one -- and then an element that was itself a
    // container got walked as a struct and raised. Nothing dispatches on this
    // any more, since read_path asks about the element's own path, so it can
    // simply be accurate.
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    return 2;
}

extern "C" void const* bg3le_meta_class(const char* className);
extern "C" char const* bg3le_meta_class_name(void const* handle);
extern "C" std::size_t bg3le_meta_class_count();
extern "C" void const* bg3le_meta_class_at(std::size_t index);
extern "C" void* bg3le_resource_get(std::int32_t typeIndex, void const* guid,
                                    std::size_t resourceSize);
extern "C" std::size_t bg3le_resource_count(std::int32_t typeIndex);
extern "C" bool bg3le_resource_guid_at(std::int32_t typeIndex, std::size_t i,
                                       void* guidOut);
// Ext.Stats. The manager is found by fingerprint like the resource manager;
// see src/vendor/stats.cpp for why the attribute values need four lookups
// rather than an offset.
// Ext.Mod. The module list is found from the base module's constant
// uuid; see src/vendor/mods.cpp.
extern "C" std::size_t bg3le_mods_count();
extern "C" char const* bg3le_mods_uuid_at(std::size_t index);
extern "C" void* bg3le_mods_at(std::size_t index);
extern "C" void* bg3le_mods_find(char const* uuid);
extern "C" std::size_t bg3le_mods_manager_dump(
    void (*report)(void* ctx, char const* key, unsigned long long header,
                   std::size_t count, void const* buffer, bool chosen),
    void* ctx);
extern "C" char const* bg3le_mods_manager_uuid_at(unsigned long long header,
                                                  std::size_t index);
extern "C" void* bg3le_mods_base();
extern "C" std::size_t bg3le_mods_available_count();
extern "C" bool bg3le_stat_origin(char const* name, char const** modId,
                                  char const** originalModId);
extern "C" void* bg3le_mods_available_at(std::size_t index);
extern "C" bool bg3le_mod_info(void const* module, bg3le::ModInfo* out);
extern "C" std::size_t bg3le_mod_list_count(void const* module, int list);
extern "C" bool bg3le_mod_list_at(void const* module, int list,
                                  std::size_t index, bg3le::ModShortDesc* out);
extern "C" std::size_t bg3le_mods_settings_count();
extern "C" bool bg3le_mods_settings_at(std::size_t index,
                                       bg3le::ModShortDesc* out);

extern "C" void* bg3le_stats_manager();
extern "C" std::size_t bg3le_stats_count();
extern "C" void* bg3le_stats_at(std::size_t index);
extern "C" char const* bg3le_stats_name(void const* object);
extern "C" void* bg3le_stats_find(char const* name);
extern "C" char const* bg3le_stats_type(void const* object);
extern "C" char const* bg3le_stats_using(void const* object);
extern "C" int bg3le_stats_list_index(void const* object);
extern "C" std::size_t bg3le_stats_attr_count(void const* object);
extern "C" bool bg3le_stats_attr_at(void const* object, std::size_t index,
                                    char const** nameOut,
                                    char const** typeNameOut, int* kindOut,
                                    int* rawOut);
extern "C" char const* bg3le_stats_attr_label(void const* object,
                                              std::size_t index, int raw);
extern "C" char const* bg3le_stats_attr_string(int raw);
extern "C" char const* bg3le_stats_attr_translated(int raw);
extern "C" char const* bg3le_loca_get(char const* handle);
extern "C" bool bg3le_loca_set(char const* handle, char const* text);
extern "C" bool bg3le_stats_attr_set(void const* object, std::size_t index,
                                     int raw);
extern "C" int bg3le_stats_condition_intern(char const* text);
extern "C" int bg3le_stats_attr_index(void const* object,
                                      char const* wanted);
extern "C" std::size_t bg3le_stats_names_count(char const* list);
extern "C" char const* bg3le_stats_names_at(char const* list,
                                            std::size_t index);
extern "C" void bg3le_fixed_string_dump();
extern "C" int bg3le_stats_string_intern(char const* text);
extern "C" void bg3le_meta_set_dump(void const* handle, char const* path,
                                   void* component);
extern "C" bool bg3le_meta_set_assign(void const* handle,
                                      char const* path, void* component,
                                      void const* values,
                                      std::size_t count,
                                      std::size_t elemSize);
extern "C" std::size_t bg3le_loca_count();
extern "C" char const* bg3le_loca_handle_at(std::size_t index);
extern "C" std::size_t bg3le_templates_count();
extern "C" char const* bg3le_templates_id_at(std::size_t index);
extern "C" void* bg3le_templates_find(char const* id);
extern "C" char const* bg3le_templates_type(char const* id);
extern "C" void* bg3le_templates_in(int source, char const* id, char const** type);
extern "C" bool bg3le_templates_list(int source,
                                     void (*each)(void* ctx, char const* id, void* at,
                                                  char const* type),
                                     void* ctx);
extern "C" void* bg3le_prototype_find(int kind, char const* name);
extern "C" std::size_t bg3le_prototype_count(int kind);
extern "C" char const* bg3le_prototype_name_at(int kind, std::size_t index);
extern "C" char const* bg3le_stats_attr_condition(int raw);
extern "C" char const* bg3le_stats_ai_flags(void const* object);
extern "C" char const* bg3le_stats_enum_label(char const* enumeration,
                                              int index);
extern "C" bool bg3le_stats_enum_index(char const* enumeration,
                                       char const* label, int* out);
extern "C" std::size_t bg3le_stats_list_attr_count(char const* listName);
extern "C" bool bg3le_stats_list_attr_at(char const* listName,
                                         std::size_t index,
                                         char const** nameOut,
                                         char const** typeOut);
extern "C" int bg3le_stats_functor_groups(void const* object,
                                          char const* attribute);
extern "C" bool bg3le_stats_functor_group_at(void const* object,
                                             char const* attribute,
                                             int index,
                                             char const** textKeyOut,
                                             void** functorsOut);
extern "C" int bg3le_stats_functor_count(void const* functors);
extern "C" void* bg3le_stats_functor_at(void const* functors, int index);
extern "C" char const* bg3le_stats_functor_class(void const* functor);
extern "C" void* bg3le_stats_object_expression(void const* object,
                                               char const* className,
                                               char const* field);
extern "C" int bg3le_ext_monotonic_time(lua_State* L);
extern "C" int bg3le_ext_microsec_time(lua_State* L);
extern "C" int bg3le_ext_clock_epoch(lua_State* L);
extern "C" int bg3le_ext_clock_time(lua_State* L);
extern "C" int bg3le_ext_generate_guid(lua_State* L);
extern "C" int bg3le_ext_game_version(lua_State* L);
extern "C" int bg3le_ext_command_line(lua_State* L);
extern "C" int bg3le_ext_load_file(lua_State* L);
extern "C" int bg3le_ext_pak_modules(lua_State* L);
extern "C" int bg3le_ext_mod_settings_order(lua_State* L);
extern "C" int bg3le_ext_show_error_and_exit(lua_State* L);
extern "C" int bg3le_json_parse(lua_State* L);
extern "C" int bg3le_ext_pak_read(lua_State* L);
extern "C" int bg3le_ext_plugin_list(lua_State* L);
extern "C" int bg3le_ext_plugin_settings(lua_State* L);
extern "C" int bg3le_ext_plugin_get(lua_State* L);
extern "C" int bg3le_ext_plugin_set(lua_State* L);
extern "C" int bg3le_ext_save_file(lua_State* L);
extern "C" int bg3le_ext_write_data_file(lua_State* L);
extern "C" int bg3le_ext_memory_usage(lua_State* L);
extern "C" int bg3le_ext_show_error(lua_State* L);
extern "C" int bg3le_math_add(lua_State* L);
extern "C" int bg3le_math_sub(lua_State* L);
extern "C" int bg3le_math_mul(lua_State* L);
extern "C" int bg3le_math_div(lua_State* L);
extern "C" int bg3le_math_reflect(lua_State* L);
extern "C" int bg3le_math_angle(lua_State* L);
extern "C" int bg3le_math_cross(lua_State* L);
extern "C" int bg3le_math_distance(lua_State* L);
extern "C" int bg3le_math_dot(lua_State* L);
extern "C" int bg3le_math_length(lua_State* L);
extern "C" int bg3le_math_normalize(lua_State* L);
extern "C" int bg3le_math_perpendicular(lua_State* L);
extern "C" int bg3le_math_project(lua_State* L);
extern "C" int bg3le_math_determinant(lua_State* L);
extern "C" int bg3le_math_inverse(lua_State* L);
extern "C" int bg3le_math_transpose(lua_State* L);
extern "C" int bg3le_math_outer_product(lua_State* L);
extern "C" int bg3le_math_rotate(lua_State* L);
extern "C" int bg3le_math_translate(lua_State* L);
extern "C" int bg3le_math_scale(lua_State* L);
extern "C" int bg3le_math_extract_euler_angles(lua_State* L);
extern "C" int bg3le_math_build_from_euler_angles3(lua_State* L);
extern "C" int bg3le_math_build_from_euler_angles4(lua_State* L);
extern "C" int bg3le_math_decompose(lua_State* L);
extern "C" int bg3le_math_extract_axis_angle(lua_State* L);
extern "C" int bg3le_math_build_from_axis_angle3(lua_State* L);
extern "C" int bg3le_math_build_from_axis_angle4(lua_State* L);
extern "C" int bg3le_math_build_rotation3(lua_State* L);
extern "C" int bg3le_math_build_rotation4(lua_State* L);
extern "C" int bg3le_math_build_translation(lua_State* L);
extern "C" int bg3le_math_build_scale(lua_State* L);
extern "C" int bg3le_math_quat_from_euler(lua_State* L);
extern "C" int bg3le_math_quat_from_to_rotation(lua_State* L);
extern "C" int bg3le_math_quat_dot(lua_State* L);
extern "C" int bg3le_math_quat_slerp(lua_State* L);
extern "C" int bg3le_math_quat_to_mat3(lua_State* L);
extern "C" int bg3le_math_quat_to_mat4(lua_State* L);
extern "C" int bg3le_math_mat3_to_quat(lua_State* L);
extern "C" int bg3le_math_mat4_to_quat(lua_State* L);
extern "C" int bg3le_math_quat_normalize(lua_State* L);
extern "C" int bg3le_math_quat_inverse(lua_State* L);
extern "C" int bg3le_math_quat_rotate(lua_State* L);
extern "C" int bg3le_math_quat_rotate_axis_angle(lua_State* L);
extern "C" int bg3le_math_quat_length(lua_State* L);
extern "C" int bg3le_math_quat_mul(lua_State* L);
extern "C" int bg3le_math_random(lua_State* L);
extern "C" int bg3le_math_round(lua_State* L);
extern "C" int bg3le_math_fract(lua_State* L);
extern "C" int bg3le_math_trunc(lua_State* L);
extern "C" int bg3le_math_sign(lua_State* L);
extern "C" int bg3le_math_clamp(lua_State* L);
extern "C" int bg3le_math_smoothstep(lua_State* L);
extern "C" int bg3le_math_lerp(lua_State* L);
extern "C" int bg3le_math_asin(lua_State* L);
extern "C" int bg3le_math_acos(lua_State* L);
extern "C" int bg3le_math_atan(lua_State* L);
extern "C" int bg3le_math_atan2(lua_State* L);
extern "C" int bg3le_math_is_nan(lua_State* L);
extern "C" int bg3le_math_is_inf(lua_State* L);
extern "C" char const* bg3le_stats_expression_code(void const* pooled);
extern "C" void bg3le_stats_expression_dump(void const* pooled);
extern "C" bool bg3le_stats_expression_refcount(void const* pooled,
                                                int* out);
extern "C" int bg3le_stats_roll_condition_count(void const* object,
                                                char const* attribute);
extern "C" bool bg3le_stats_roll_condition_at(void const* object,
                                              char const* attribute,
                                              int index,
                                              char const** nameOut,
                                              char const** textOut);
extern "C" bool bg3le_stats_attr_float(int raw, double* out);
extern "C" bool bg3le_stats_attr_guid(int raw, char* out,
                                      std::size_t capacity);
extern "C" bool bg3le_stats_attr_flags(void const* object,
                                       std::size_t index, int raw,
                                       char* out,
                                       std::size_t capacity);

extern "C" void* bg3le_resource_manager();
extern "C" std::size_t bg3le_resource_bank_count();
extern "C" bool bg3le_resource_bank_at(std::size_t i, std::int32_t* typeIndex,
                                       void** bank);

// A field call's subject: a class, and the address its fields are relative to.
//
// The field machinery never needed an entity -- only these two. An entity and
// a component resolve to a subject; a static data resource already is one,
// which is what makes the same code serve both without a second copy of it.
struct Subject {
    void const* Meta{nullptr};
    void* Base{nullptr};
};

// From an address and a class name, as a static data resource arrives.
bool subject_from_object(lua_State* L, int addressIdx, int classIdx,
                         Subject* out, const char** className) {
    *className = luaL_checkstring(L, classIdx);
    out->Base = (void*)(std::uintptr_t)luaL_checkinteger(L, addressIdx);
    out->Meta = bg3le_meta_class(*className);
    if (out->Meta == nullptr || out->Base == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for class %s", *className);
        return false;
    }
    return true;
}

// Ext._Internal.ObjectFields(class [, path]) -> { field = kind, ... }
extern "C" bool bg3le_meta_path_is_struct(void const* handle, char const* path);

// Gives the field table on top a metatable answering upstream's legacy names
// (field_1 for Controller): indexing finds them, pairs and next do not.
void attach_legacy_names(lua_State* L, void const* meta, const char* path) {
    constexpr std::size_t kMax = 64;
    const char* names[kMax];
    std::uint8_t kinds[kMax];
    const std::size_t n = bg3le_meta_legacy_at(meta, path, names, kinds, kMax);
    if (n == 0) return;
    lua_createtable(L, 0, 1);
    lua_createtable(L, 0, (int)n);
    for (std::size_t i = 0; i < n; ++i) {
        lua_pushstring(L, field_kind_name((FieldKind)kinds[i]));
        lua_setfield(L, -2, names[i]);
    }
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
}

int l_object_fields(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    const char* path = luaL_optstring(L, 2, nullptr);

    void const* meta = bg3le_meta_class(className);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for class %s", className);
        return 2;
    }

    constexpr std::size_t kMax = 512;
    const char* names[kMax];
    std::uint8_t kinds[kMax];
    const std::size_t count =
        bg3le_meta_fields_at(meta, path, names, kinds, kMax);
    if (count == 0 && path != nullptr && path[0] != '\0'
        && !bg3le_meta_path_is_struct(meta, path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s cannot be traversed", className, path);
        return 2;
    }

    lua_newtable(L);
    for (std::size_t i = 0; i < count; ++i) {
        lua_pushstring(L, field_kind_name((FieldKind)kinds[i]));
        lua_setfield(L, -2, names[i]);
    }
    attach_legacy_names(L, meta, path);
    return 1;
}

// Ext._Internal.ObjectFieldInfo(class, path) -> kind, elemKind, elemCount
int l_object_field_info(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    const char* path = luaL_checkstring(L, 2);

    void const* meta = bg3le_meta_class(className);
    if (meta == nullptr) return 0;

    std::uint32_t offset = 0;
    std::uint16_t size = 0;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_field(meta, path, &offset, &size, &kind, &elemKind,
                          &elemCount)) {
        return 0;
    }

    lua_pushstring(L, field_kind_name((FieldKind)kind));
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    lua_pushinteger(L, elemCount);
    return 3;
}

// Ext._Internal.ObjectGetField(address, class, path)
int l_object_get_field(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(subject.Meta, path, subject.Base, &address, &kind,
                            &size, &readOnly)) {
        // P_BITMASK: a flag of a flags field reads as a boolean.
        std::uint64_t mask = 0;
        std::uint64_t raw = 0;
        if (bg3le_meta_bitflag(subject.Meta, path, subject.Base, &address,
                               &size, &mask)
            && size <= 8 && safe_read(address, &raw, size)) {
            lua_pushboolean(L, (raw & mask) == mask ? 1 : 0);
            return 1;
        }
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", className, path);
        return 2;
    }

    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(subject.Meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    const std::size_t width = field_kind_size((FieldKind)kind);
    if (width != 0 && width <= 8) {
        std::uint64_t raw = 0;
        if (safe_read(address, &raw, width)
            && push_enum(L, subject.Meta, path, raw)) {
            return 1;
        }
    }

    if ((FieldKind)kind == FieldKind::Optional
        && read_optional(L, subject.Meta, path, subject.Base)) {
        return 1;
    }
    if ((FieldKind)kind == FieldKind::Text) {
        return push_text(L, subject.Meta, path, subject.Base);
    }

    if (!push_field(L, address, (FieldKind)kind, (FieldKind)elemKind,
                    elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is of an unsupported kind (%s)", className,
                        path, field_kind_name((FieldKind)kind));
        return 2;
    }
    return 1;
}

// Ext._Internal.SetDump(address, class, path) -- the container's bytes
// beside what its own accessors say, to the log. How the three members of
// a hash set were located.
int l_set_dump(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    bg3le_meta_set_dump(subject.Meta, luaL_checkstring(L, 3), subject.Base);
    return 0;
}

// A set of anything but FixedStrings -- GUIDs, entities, integers -- written
// as a field of that kind is, into a buffer handed over whole.
int assign_scalar_set(lua_State* L, Subject const& subject,
                      const char* className, const char* path, int tableIdx,
                      FieldKind elemKind) {
    const std::size_t stride = field_kind_size(elemKind);
    if (stride == 0 || elemKind == FieldKind::Pointer
        || elemKind == FieldKind::ConditionId) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is a set of %s, which bg3le cannot write",
                        className, path, field_kind_name(elemKind));
        return 2;
    }
    tableIdx = push_packed(L, tableIdx);
    const lua_Integer count = luaL_len(L, tableIdx);
    std::vector<unsigned char> bytes(stride * (std::size_t)(count > 0 ? count : 0));
    // An enum's labels are taken, as a field of one takes them.
    const std::string element = std::string(path) + "[0]";
    std::size_t kept = 0;
    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, tableIdx, i);
        enum_arg_in_place(L, lua_gettop(L), subject.Meta, element.c_str());
        unsigned char* at = &bytes[kept * stride];
        const bool ok = write_field(L, lua_gettop(L), at, elemKind);
        lua_pop(L, 1);
        if (!ok) {
            lua_pushnil(L);
            lua_pushfstring(L, "%s.%s: entry %d is not a %s", className, path,
                            (int)i, field_kind_name(elemKind));
            return 2;
        }
        // A key given twice is one key, as upstream's insert makes it.
        bool seen = false;
        for (std::size_t j = 0; j < kept && !seen; ++j) {
            seen = std::memcmp(&bytes[j * stride], at, stride) == 0;
        }
        if (!seen) ++kept;
    }
    if (!bg3le_meta_set_assign(subject.Meta, path, subject.Base,
                               kept == 0 ? nullptr : bytes.data(), kept, stride)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not a set bg3le can replace", className,
                        path);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Replaces a hash set of FixedStrings, which is what a spell list is.
// Elements cannot be written one at a time -- the table's hashes would
// still point at the old keys -- so the set is rebuilt whole.
//
// Each name has to become the id the engine already holds for that text: a
// FixedString compares by id, so a fresh entry for the same characters is a
// different string to everything that looks at it. Text the engine does not
// have is interned, which is the case for a spell a mod has added. That
// happens here rather than in the thunk, because interning is the part that
// can fail and failing before anything is written is what keeps a
// half-replaced set from existing.
//
// Shared by the component and the static-data routes, which differ only in
// how the base address is found.
int assign_set(lua_State* L, Subject const& subject, const char* className,
               const char* path, int tableIdx) {
    luaL_checktype(L, tableIdx, LUA_TTABLE);

    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(subject.Meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);
    if ((FieldKind)elemKind != FieldKind::FixedString) {
        return assign_scalar_set(L, subject, className, path, tableIdx,
                                 (FieldKind)elemKind);
    }

    std::vector<unsigned int> ids;
    tableIdx = push_packed(L, tableIdx);
    const lua_Integer count = luaL_len(L, tableIdx);
    ids.reserve((std::size_t)(count > 0 ? count : 0));

    for (lua_Integer i = 1; i <= count; ++i) {
        lua_rawgeti(L, tableIdx, i);
        char const* name = lua_tostring(L, -1);
        if (name == nullptr) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushfstring(L, "%s.%s: entry %d is not a string", className,
                            path, (int)i);
            return 2;
        }

        unsigned int id = 0;
        if (!bg3le_fixed_string_index_of(name, &id)
            && !bg3le_fixed_string_intern(name, &id)) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushfstring(L, "%s.%s: no string-table entry for %s and one "
                            "could not be made", className, path, name);
            return 2;
        }
        // A key given twice is one key, as upstream's insert makes it.
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        lua_pop(L, 1);
    }

    if (!bg3le_meta_set_assign(subject.Meta, path, subject.Base,
                               ids.empty() ? nullptr : ids.data(), ids.size(),
                               sizeof(unsigned int))) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not a set of FixedStrings that bg3le can "
                        "replace", className, path);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.ObjectSetSet(address, class, path, { name, ... }) -> true
int l_object_set_set(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    return assign_set(L, subject, className, luaL_checkstring(L, 3), 4);
}

// Ext._Internal.SetSet(handle, component, path, { name, ... }) -> true
//
// The same replacement against a component rather than a resource. A
// component holds hash sets too -- a tag list, for one -- and without this
// they were readable and not writable.
int l_set_set(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    Subject subject;
    subject.Base = component_pointer(handle, name, &subject.Meta);
    if (subject.Meta == nullptr || subject.Base == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }
    return assign_set(L, subject, name, path, 4);
}

// Ext._Internal.ObjectSetField(address, class, path, value[, unserializing])
//   -> true
//
// The same write SetField makes on a component, against an address that
// arrived some other way -- a static data resource, for one. Reading one
// of those has worked from the start; writing had no entry point at all,
// which meant Ext.StaticData handed back a snapshot and a mod editing it
// changed nothing. 5eSpells' whole job is editing spell lists.
int l_object_set_field(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(subject.Meta, path, subject.Base, &address, &kind,
                            &size, &readOnly)) {
        // P_BITMASK: assigning a flag sets or clears its bit.
        std::uint64_t mask = 0;
        std::uint64_t raw = 0;
        if (bg3le_meta_bitflag(subject.Meta, path, subject.Base, &address,
                               &size, &mask)
            && size <= 8 && safe_read(address, &raw, size)) {
            raw = lua_toboolean(L, 4) ? (raw | mask) : (raw & ~mask);
            std::memcpy(address, &raw, size);
            lua_pushboolean(L, 1);
            return 1;
        }
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", className, path);
        return 2;
    }
    if (readOnly) {
        lua_pushnil(L);
        lua_pushfstring(L,
            "%s.%s is read-only (a hash set's keys, a queue, or a value "
            "upstream only reads)", className, path);
        return 2;
    }

    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(subject.Meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    if ((FieldKind)kind == FieldKind::Optional) {
        if (!write_optional(L, 4, subject.Meta, path, subject.Base)) {
            lua_pushnil(L);
            lua_pushfstring(L, "%s.%s is an optional bg3le cannot write",
                            className, path);
            return 2;
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    if (assign_array(L, 4, subject.Meta, path, subject.Base, (FieldKind)kind)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    enum_arg_in_place(L, 4, subject.Meta, path);
    if ((FieldKind)kind == FieldKind::ScalarArray) {
        enum_elements_in_place(L, 4, subject.Meta, path, elemCount);
    }
    if (!write_field(L, 4, address, (FieldKind)kind, (FieldKind)elemKind,
                     elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not writable (%s)", className, path,
                        field_kind_name((FieldKind)kind));
        return 2;
    }
    // Upstream's setter for an OverrideableProperty builds {value, true}, so
    // an assignment marks it overridden -- but its Unserialize writes only
    // the Value and leaves the flag alone, and Ext.Types.Unserialize says
    // which it is. A no-op for any other field.
    const bool unserializing = lua_toboolean(L, 5) != 0;
    bg3le_meta_after_write(subject.Meta, path, subject.Base, unserializing,
                           bg3le_entity_world(world_container()));
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.ObjectArrayInfo(address, class, path) -> count, elementKind
int l_object_array_info(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    std::size_t count = 0;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    const int status = bg3le_meta_array_length(subject.Meta, path, subject.Base,
                                               &count, &elemSize, &elemKind);
    if (status != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot size %s.%s (status %d)", className, path,
                        status);
        return 2;
    }

    // An implausible count means the size was read from something that is
    // not this array's size, and walking it would read millions of
    // elements that are not there. Said once per class and path, because
    // it is a metadata problem worth fixing rather than a passing error.
    constexpr std::size_t kSane = 1u << 20;
    if (count > kSane) {
        static std::set<std::string> said;
        const std::string what = std::string(className) + "." + path;
        if (said.insert(what).second) {
            logf("meta: %s reports %zu elements, which is not a size; "
                 "treating it as empty", what.c_str(), count);
        }
        lua_pushinteger(L, 0);
        lua_pushstring(L, field_kind_name((FieldKind)elemKind));
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)count);
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    return 2;
}

// Ext._Internal.ObjectVariantIndex(address, class, path) -> active, count
int l_object_variant_index(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    std::size_t active = 0;
    std::size_t count = 0;
    if (!bg3le_meta_variant_index(subject.Meta, path, subject.Base, &active,
                                  &count)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not a variant", className, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)active);
    lua_pushinteger(L, (lua_Integer)count);
    return 2;
}

extern "C" bool bg3le_meta_map_key_label(void const* handle, char const* path,
                                         std::uint64_t raw,
                                         char const** label);

// A map key as upstream pushes it: an enum as its label, anything else as
// its value. The kind follows it, so Lua can make an entity of an entity key.
bool push_map_key(lua_State* L, void const* meta, char const* path,
                  void const* address, FieldKind kind) {
    const std::size_t width = field_kind_size(kind);
    std::uint64_t raw = 0;
    char const* label = nullptr;
    if (width != 0 && width <= 8 && safe_read(address, &raw, width)
        && bg3le_meta_map_key_label(meta, path, raw, &label)) {
        lua_pushstring(L, label);
    } else if (!push_field(L, address, kind)) {
        return false;
    }
    lua_pushstring(L, field_kind_name(kind));
    return true;
}

// Ext._Internal.ObjectMapKey(address, class, path, index) -> key, kind
int l_object_map_key(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);
    const auto index = (std::size_t)luaL_checkinteger(L, 4);

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    if (!bg3le_meta_map_key(subject.Meta, path, subject.Base, index, &address,
                            &kind, &size)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s has no key at slot %d", className, path,
                        (int)index);
        return 2;
    }

    if (!push_map_key(L, subject.Meta, path, address, (FieldKind)kind)) {
        lua_pushnil(L);
        lua_pushfstring(L, "the keys of %s.%s are of an unsupported kind (%s)",
                        className, path, field_kind_name((FieldKind)kind));
        return 2;
    }
    return 2;
}

// Ext._Internal.ResourceGet(class, guid) -> address
//
// The chain: the class names the resource type, its EngineClass names the
// static data type, the symbol table gives that type's index, the manager
// gives the bank for the index, and the bank maps the GUID to the resource.
// Every link but the manager was already in place.
// A resource class name's engine class and static data type index.
bool resource_type(lua_State* L, char const* className, char const** engineClass,
                   std::int32_t* typeIndex) {
    void const* meta = bg3le_meta_class(className);
    *engineClass = meta != nullptr ? bg3le_meta_engine_class(meta) : nullptr;
    if (*engineClass == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a resource type", className);
        return false;
    }
    const auto index = ecs::index_of(ecs::Context::ImmutableData, *engineClass);
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a registered static data type", *engineClass);
        return false;
    }
    *typeIndex = (std::int32_t)*index;
    return true;
}

// Ext._Internal.StaticDataCreate(class, guid) -> address, or nil and why
extern "C" void* bg3le_static_data_create(char const* engineClass, std::int32_t typeIndex,
                                          void const* guid16, char const** why);
int l_static_data_create(lua_State* L) {
    char const* engineClass = nullptr;
    std::int32_t typeIndex = -1;
    if (!resource_type(L, luaL_checkstring(L, 1), &engineClass, &typeIndex)) return 2;
    std::uint8_t guid[16];
    if (!bg3le_meta_parse_guid(luaL_checkstring(L, 2), guid)) {
        lua_pushnil(L);
        lua_pushstring(L, "not a GUID");
        return 2;
    }
    char const* why = nullptr;
    void* at = bg3le_static_data_create(engineClass, typeIndex, guid, &why);
    if (at == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.StaticDataBankCall(class, clear) -> true, or nil and why
extern "C" bool bg3le_static_data_bank_call(std::int32_t typeIndex, bool clear, char const** why);
int l_static_data_bank_call(lua_State* L) {
    char const* engineClass = nullptr;
    std::int32_t typeIndex = -1;
    if (!resource_type(L, luaL_checkstring(L, 1), &engineClass, &typeIndex)) return 2;
    char const* why = nullptr;
    if (!bg3le_static_data_bank_call(typeIndex, lua_toboolean(L, 2) != 0, &why)) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_resource_get(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    const char* guidText = luaL_checkstring(L, 2);

    void const* meta = bg3le_meta_class(className);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no metadata for resource class %s", className);
        return 2;
    }

    const char* engineClass = bg3le_meta_engine_class(meta);
    if (engineClass == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s has no engine class, so no static data type",
                        className);
        return 2;
    }

    const auto typeIndex =
        ecs::index_of(ecs::Context::ImmutableData, engineClass);
    if (!typeIndex.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a registered static data type",
                        engineClass);
        return 2;
    }

    std::uint8_t guid[16];
    if (!bg3le_meta_parse_guid(guidText, guid)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a GUID", guidText);
        return 2;
    }

    void* resource =
        bg3le_resource_get(*typeIndex, guid, bg3le_meta_component_size(meta));
    if (resource == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no %s with that GUID; the bank holds %d",
                        className, (int)bg3le_resource_count(*typeIndex));
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)resource);
    return 1;
}

// Ext._Internal.ResourceGuids(class) -> { guid, ... }
int l_resource_guids(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);

    void const* meta = bg3le_meta_class(className);
    const char* engineClass =
        meta != nullptr ? bg3le_meta_engine_class(meta) : nullptr;
    if (engineClass == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no static data type for %s", className);
        return 2;
    }

    const auto typeIndex =
        ecs::index_of(ecs::Context::ImmutableData, engineClass);
    if (!typeIndex.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a registered static data type",
                        engineClass);
        return 2;
    }

    const std::size_t count = bg3le_resource_count(*typeIndex);
    lua_createtable(L, (int)count, 0);
    int written = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t guid[16];
        if (!bg3le_resource_guid_at(*typeIndex, i, guid)) continue;
        char text[40];
        if (!bg3le_meta_format_guid(guid, text, sizeof(text))) continue;
        lua_pushstring(L, text);
        lua_rawseti(L, -2, ++written);
    }
    return 1;
}

// ---- Ext.Mod ----

// Ext._Internal.ModCount() -> n
int l_mod_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_mods_count());
    return 1;
}

// Ext._Internal.ModUuidAt(index) -> uuid string
int l_mod_uuid_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    char const* uuid = bg3le_mods_uuid_at(i);
    if (uuid == nullptr) return 0;
    lua_pushstring(L, uuid);
    return 1;
}

// Ext._Internal.ModAt(index) -> address, and ModFind(uuid) -> address.
// Ext.Mod takes the module by address so a lookup is paid for once.
int l_mod_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* module = bg3le_mods_at(i);
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

// Ext._Internal.ModManagers() -> { {Key, Header, Count, Buffer, Chosen}, ... }
//
// Both mod managers and what each holds right now. The load order is not
// fixed for the run -- one reached 69 modules during a level load and 43 by
// the end of it -- so which one bg3le adopted is a question with a different
// answer at different moments, and this is how to see it rather than infer
// it.
struct ManagerRow {
    lua_State* L;
    int Index;
};

void manager_row(void* ctx, char const* key, unsigned long long header,
                 std::size_t count, void const* buffer, bool chosen) {
    auto* out = static_cast<ManagerRow*>(ctx);
    lua_State* L = out->L;

    lua_createtable(L, 0, 5);
    lua_pushstring(L, key);
    lua_setfield(L, -2, "Key");
    lua_pushinteger(L, (lua_Integer)header);
    lua_setfield(L, -2, "Header");
    lua_pushinteger(L, (lua_Integer)count);
    lua_setfield(L, -2, "Count");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)buffer);
    lua_setfield(L, -2, "Buffer");
    lua_pushboolean(L, chosen ? 1 : 0);
    lua_setfield(L, -2, "Chosen");
    lua_rawseti(L, -2, out->Index++);
}

int l_mod_managers(lua_State* L) {
    lua_newtable(L);
    ManagerRow out{L, 1};
    bg3le_mods_manager_dump(&manager_row, &out);
    return 1;
}

// Ext._Internal.ModManagerUuidAt(header, index) -> uuid
int l_mod_manager_uuid_at(lua_State* L) {
    const auto header =
        (unsigned long long)luaL_checkinteger(L, 1);
    const auto index = (std::size_t)luaL_checkinteger(L, 2);
    char const* uuid = bg3le_mods_manager_uuid_at(header, index);
    if (uuid == nullptr) return 0;
    lua_pushstring(L, uuid);
    return 1;
}

int l_mod_find(lua_State* L) {
    void* module = bg3le_mods_find(luaL_checkstring(L, 1));
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

// Ext._Internal.ModAvailableCount() / ModAvailableAt(index)
int l_mod_available_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_mods_available_count());
    return 1;
}

int l_mod_available_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* module = bg3le_mods_available_at(i);
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

int l_mod_base(lua_State* L) {
    void* module = bg3le_mods_base();
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

void push_version(lua_State* L, std::uint32_t const v[4]) {
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        lua_pushinteger(L, (lua_Integer)v[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

void set_string(lua_State* L, char const* key, char const* value) {
    lua_pushstring(L, value != nullptr ? value : "");
    lua_setfield(L, -2, key);
}

// Ext._Internal.ModInfo(address) -> the ModuleInfo table
//
// Keys and shapes follow reference/mod-shape.txt exactly; a mod written
// against bg3se reads mod.Info.Directory and has to find it here.
int l_mod_info(lua_State* L) {
    auto const* module =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    bg3le::ModInfo info{};
    if (!bg3le_mod_info(module, &info)) return 0;

    lua_createtable(L, 0, 16);
    set_string(L, "ModuleUUID", info.ModuleUUIDString);
    set_string(L, "ModuleUUIDString", info.ModuleUUIDString);
    set_string(L, "Name", info.Name);
    set_string(L, "Directory", info.Directory);
    set_string(L, "Hash", info.Hash);
    set_string(L, "Author", info.Author);
    set_string(L, "Description", info.Description);
    set_string(L, "StartLevelName", info.StartLevelName);
    set_string(L, "MenuLevelName", info.MenuLevelName);
    set_string(L, "LobbyLevelName", info.LobbyLevelName);
    set_string(L, "CharacterCreationLevelName",
               info.CharacterCreationLevelName);
    set_string(L, "PhotoBoothLevelName", info.PhotoBoothLevelName);

    push_version(L, info.ModVersion);
    lua_setfield(L, -2, "ModVersion");
    push_version(L, info.PublishVersion);
    lua_setfield(L, -2, "PublishVersion");

    lua_pushinteger(L, (lua_Integer)info.NumPlayers);
    lua_setfield(L, -2, "NumPlayers");
    lua_pushinteger(L, (lua_Integer)info.FileSize);
    lua_setfield(L, -2, "FileSize");
    lua_pushinteger(L, (lua_Integer)info.PublishHandle);
    lua_setfield(L, -2, "PublishHandle");
    return 1;
}

void push_short_desc(lua_State* L, bg3le::ModShortDesc const& desc) {
    lua_createtable(L, 0, 7);
    set_string(L, "ModuleUUID", desc.ModuleUUIDString);
    set_string(L, "ModuleUUIDString", desc.ModuleUUIDString);
    set_string(L, "Name", desc.Name);
    set_string(L, "Folder", desc.Folder);
    set_string(L, "Hash", desc.Hash);
    push_version(L, desc.ModVersion);
    lua_setfield(L, -2, "ModVersion");
    push_version(L, desc.PublishVersion);
    lua_setfield(L, -2, "PublishVersion");
    lua_pushinteger(L, (lua_Integer)desc.PublishHandle);
    lua_setfield(L, -2, "PublishHandle");
}

// Ext._Internal.ModList(address, which) -> { ModuleShortDesc, ... }
// which is 0 Dependencies, 1 ModConflicts, 2 Addons.
int l_mod_list(lua_State* L) {
    auto const* module =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const int which = (int)luaL_checkinteger(L, 2);

    const std::size_t n = bg3le_mod_list_count(module, which);
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        bg3le::ModShortDesc desc{};
        if (!bg3le_mod_list_at(module, which, i, &desc)) break;
        push_short_desc(L, desc);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.ModSettings() -> { ModuleShortDesc, ... }
//
// ModManager::Settings.Mods, which is the mod list the session has rather
// than the load order: for a loaded save it is what the save recorded, and
// it omits the base modules the load order carries.
int l_mod_settings(lua_State* L) {
    const std::size_t n = bg3le_mods_settings_count();
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        bg3le::ModShortDesc desc{};
        if (!bg3le_mods_settings_at(i, &desc)) break;
        push_short_desc(L, desc);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.StatsNameRecheck(addr) -> id, cached, fresh
//
// Diagnostic, for a stat whose Name resolves to text that is not a stat name.
// If the cached and fresh answers differ, the string cache went stale.
int l_stats_name_recheck(lua_State* L) {
    auto const* object =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);

    std::uint32_t id = 0;
    if (!bg3le_stats_name_id(object, &id)) return 0;

    char const* cached = nullptr;
    char const* fresh = nullptr;
    if (!bg3le_fixed_string_recheck(id, &cached, &fresh)) return 0;

    lua_pushinteger(L, (lua_Integer)id);
    if (cached != nullptr) {
        lua_pushstring(L, cached);
    } else {
        lua_pushnil(L);
    }
    if (fresh != nullptr) {
        lua_pushstring(L, fresh);
    } else {
        lua_pushnil(L);
    }
    return 3;
}

// A widget handle is index | salt << 24 | type << 56, and the first window
// gets every one of those as zero -- Window is the first object type and a
// fresh pool slot starts at salt zero. So zero is a real handle and only
// InvalidHandle means failure.
constexpr std::uint64_t kImguiInvalidHandle = 0xffffffffffffffffull;

// Ext._Internal.ImguiNewWindow(name) -> handle
int l_imgui_new_window(lua_State* L) {
    const auto handle = bg3le_imgui_new_window(luaL_checkstring(L, 1));
    if (handle == kImguiInvalidHandle) return 0;
    lua_pushinteger(L, (lua_Integer)handle);
    return 1;
}

// Lua's own values, read into the form the widget methods take.
//
// A vector arrives as a table of numbers, a widget as one carrying a Handle,
// and nil is an argument the caller left out -- which is exactly what
// upstream's std::optional parameters mean. Strings point into the Lua stack,
// so nothing here outlives the call that reads it.
void read_imgui_args(lua_State* L, int first, ImguiArg* out,
                     std::size_t* count) {
    *count = 0;
    const int top = lua_gettop(L);

    for (int at = first; at <= top && *count < kImguiMaxArgs; ++at) {
        ImguiArg& arg = out[(*count)++];
        arg = ImguiArg{};

        switch (lua_type(L, at)) {
        case LUA_TBOOLEAN:
            arg.Kind = kImguiArgBool;
            arg.Bool = lua_toboolean(L, at) != 0;
            break;

        case LUA_TNUMBER:
            arg.Kind = lua_isinteger(L, at) ? kImguiArgInt : kImguiArgNumber;
            arg.Int = (int)lua_tointeger(L, at);
            arg.Number = lua_tonumber(L, at);
            break;

        case LUA_TSTRING:
            arg.Kind = kImguiArgText;
            arg.Text = lua_tostring(L, at);
            break;

        case LUA_TUSERDATA: {
            // A widget (userdata, as upstream's) answers Handle through __index;
            // other bg3le objects (entities) answer it too, so check the type.
            bool widget = false;
            if (lua_getmetatable(L, at)) {
                // An EnumValue or BitfieldValue is its number, as upstream takes it.
                if (lua_getfield(L, -1, "__bg3leEnumValue") == LUA_TNUMBER
                    || (lua_pop(L, 1), lua_getfield(L, -1, "__bg3leBitfieldValue"))
                           == LUA_TNUMBER) {
                    arg.Kind = kImguiArgInt;
                    arg.Int = (int)lua_tointeger(L, -1);
                    arg.Number = lua_tonumber(L, -1);
                    lua_pop(L, 2);
                    break;
                }
                lua_pop(L, 1);
                lua_getfield(L, -1, "__name");
                char const* name = lua_tostring(L, -1);
                widget = name != nullptr && std::strcmp(name, "ImguiHandle") == 0;
                lua_pop(L, 2);
            }
            if (widget) {
                lua_getfield(L, at, "Handle");
                arg.Kind = kImguiArgHandle;
                arg.Handle = (std::uint64_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
            }
            break;
        }

        case LUA_TTABLE: {
            // A vector; or a widget table, if it carries a handle.
            lua_getfield(L, at, "Handle");
            if (lua_isinteger(L, -1)) {
                arg.Kind = kImguiArgHandle;
                arg.Handle = (std::uint64_t)lua_tointeger(L, -1);
                lua_pop(L, 1);
                break;
            }
            lua_pop(L, 1);

            int written = 0;
            for (int i = 1; i <= 4; ++i) {
                lua_rawgeti(L, at, i);
                if (!lua_isnumber(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                arg.Vec[written++] = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
            }

            switch (written) {
            case 2: arg.Kind = kImguiArgVec2; break;
            case 3: arg.Kind = kImguiArgVec3; break;
            case 4: arg.Kind = kImguiArgVec4; break;
            default: arg.Kind = kImguiArgNone; break;
            }
            break;
        }

        default:
            // nil, and anything else, is "not passed".
            arg.Kind = kImguiArgNone;
            break;
        }
    }
}

// The result of a method, as the value Lua should see plus whether it is a
// widget handle the caller has to wrap.
int push_imgui_result(lua_State* L, ImguiArg const& value) {
    switch (value.Kind) {
    case kImguiArgBool:
        lua_pushboolean(L, value.Bool ? 1 : 0);
        lua_pushboolean(L, 0);
        return 2;

    case kImguiArgInt:
        lua_pushinteger(L, value.Int);
        lua_pushboolean(L, 0);
        return 2;

    case kImguiArgNumber:
        lua_pushnumber(L, value.Number);
        lua_pushboolean(L, 0);
        return 2;

    case kImguiArgText:
        lua_pushstring(L, value.Text != nullptr ? value.Text : "");
        lua_pushboolean(L, 0);
        return 2;

    case kImguiArgVec2:
    case kImguiArgVec3:
    case kImguiArgVec4: {
        const int width = value.Kind == kImguiArgVec2   ? 2
                          : value.Kind == kImguiArgVec3 ? 3
                                                        : 4;
        lua_createtable(L, width, 0);
        for (int i = 0; i < width; ++i) {
            lua_pushnumber(L, value.Vec[i]);
            lua_rawseti(L, -2, i + 1);
        }
        lua_pushboolean(L, 0);
        return 2;
    }

    case kImguiArgHandle:
        if (value.Handle == kImguiInvalidHandle) {
            lua_pushnil(L);
            lua_pushboolean(L, 0);
            return 2;
        }
        lua_pushinteger(L, (lua_Integer)value.Handle);
        lua_pushboolean(L, 1);
        return 2;

    default:
        lua_pushnil(L);
        lua_pushboolean(L, 0);
        return 2;
    }
}

// Ext._Internal.ImguiAdd(parent, kind, ...) -> handle
int l_imgui_add(lua_State* L) {
    const auto parent = (std::uint64_t)luaL_checkinteger(L, 1);
    char const* kind = luaL_checkstring(L, 2);

    ImguiArg args[kImguiMaxArgs];
    std::size_t count = 0;
    read_imgui_args(L, 3, args, &count);

    const auto handle = bg3le_imgui_add(parent, kind, args, count);
    if (handle == kImguiInvalidHandle) return 0;
    lua_pushinteger(L, (lua_Integer)handle);
    return 1;
}

// Ext._Internal.ImguiCall(handle, name, ...) -> ok, value, isWidget
int l_imgui_call(lua_State* L) {
    const auto handle = (std::uint64_t)luaL_checkinteger(L, 1);
    char const* name = luaL_checkstring(L, 2);

    ImguiArg args[kImguiMaxArgs];
    std::size_t count = 0;
    read_imgui_args(L, 3, args, &count);

    ImguiArg result{};
    if (!bg3le_imgui_call(handle, name, args, count, &result)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, 1);
    return 1 + push_imgui_result(L, result);
}

// Ext._Internal.ImguiChildren(handle) -> array of handles
int l_imgui_children(lua_State* L) {
    const auto handle = (std::uint64_t)luaL_checkinteger(L, 1);

    const std::size_t total = bg3le_imgui_children(handle, nullptr, 0);
    std::vector<std::uint64_t> children(total);
    const std::size_t got =
        total == 0 ? 0 : bg3le_imgui_children(handle, children.data(), total);

    lua_createtable(L, (int)std::min(got, total), 0);
    for (std::size_t i = 0; i < total && i < got; ++i) {
        lua_pushinteger(L, (lua_Integer)children[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.ImguiObject(handle) -> address, className
//
// What the field machinery needs to read or write a widget's properties: the
// same pair a static data resource is read through. Resolved per access
// rather than kept, because a widget lives in a pool that can move it.
int l_imgui_object(lua_State* L) {
    char const* typeName = nullptr;
    void* at = bg3le_imgui_object((std::uint64_t)luaL_checkinteger(L, 1),
                                  &typeName);
    if (at == nullptr) return 0;

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    // bg3se's own short name for the type; the field tables are keyed by the
    // qualified one, which Ext._Internal.ClassName resolves.
    lua_pushstring(L, typeName != nullptr ? typeName : "");
    return 2;
}

int l_imgui_destroy(lua_State* L) {
    lua_pushboolean(
        L, bg3le_imgui_destroy((std::uint64_t)luaL_checkinteger(L, 1)) ? 1 : 0);
    return 1;
}

// Ext._Internal.ImguiFrameStats() -> frames, vertices, drawLists
//
// What the last drawn frame produced. A widget tree that is attached but not
// rendering looks exactly like one that is, until you count the geometry.
int l_imgui_frame_stats(lua_State* L) {
    std::uint64_t frames = 0;
    int vertices = 0;
    int lists = 0;
    int drawn = 0;
    bg3le_imgui_frame_stats(&frames, &vertices, &lists, &drawn);
    lua_pushinteger(L, (lua_Integer)frames);
    lua_pushinteger(L, vertices);
    lua_pushinteger(L, lists);
    lua_pushinteger(L, drawn);
    return 4;
}

// Ext._Internal.ImguiTiming() -> frames, averageUs, maxUs of the overlay's
// per-frame Update since the last call.
extern "C" void bg3le_imgui_timing(std::uint64_t* frames, std::uint64_t* averageUs,
                                   std::uint64_t* maxUs);
int l_imgui_timing(lua_State* L) {
    std::uint64_t frames = 0, average = 0, max = 0;
    bg3le_imgui_timing(&frames, &average, &max);
    lua_pushinteger(L, (lua_Integer)frames);
    lua_pushinteger(L, (lua_Integer)average);
    lua_pushinteger(L, (lua_Integer)max);
    return 3;
}

// Ext._Internal.ImguiSetCallback(handle, name) -> id or nil
//
// The calling state is what identifies the context, so an event a client
// mod registered is never handed to the server's Lua.
int l_imgui_set_callback(lua_State* L) {
    const auto handle = (std::uint64_t)luaL_checkinteger(L, 1);
    const auto id =
        bg3le_imgui_set_callback(handle, luaL_checkstring(L, 2), (void*)L);
    if (id == 0) return 0;
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

int l_imgui_clear_callback(lua_State* L) {
    const auto handle = (std::uint64_t)luaL_checkinteger(L, 1);
    lua_pushboolean(
        L, bg3le_imgui_clear_callback(handle, luaL_checkstring(L, 2)) ? 1 : 0);
    return 1;
}

// Ext._Internal.ImguiTakeEvent() -> id, widgetHandle, argument
//
// Nothing if the queue holds nothing for this context. The argument is
// whatever the event carries, already in Lua's own types.
int l_imgui_take_event(lua_State* L) {
    std::uint32_t id = 0;
    std::uint64_t widget = 0;
    std::uint8_t kind = 0;
    bool argBool = false;
    int argInt = 0;
    std::uint64_t argWidget = 0;
    float argVec[4] = {0, 0, 0, 0};
    int argIVec[4] = {0, 0, 0, 0};
    char const* argString = nullptr;

    if (!bg3le_imgui_take_event((void*)L, &id, &widget, &kind, &argBool,
                                &argInt, &argWidget, argVec, argIVec,
                                &argString)) {
        return 0;
    }

    lua_pushinteger(L, (lua_Integer)id);
    lua_pushinteger(L, (lua_Integer)widget);

    // Kept in step with ArgKind in src/vendor/imgui_events.cpp.
    switch (kind) {
    case 1:
        lua_pushboolean(L, argBool ? 1 : 0);
        break;
    case 2:
        lua_pushinteger(L, argInt);
        break;
    case 3:
        lua_pushinteger(L, (lua_Integer)argWidget);
        break;
    case 4:
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; ++i) {
            lua_pushnumber(L, argVec[i]);
            lua_rawseti(L, -2, i + 1);
        }
        break;
    case 5:
        lua_createtable(L, 4, 0);
        for (int i = 0; i < 4; ++i) {
            lua_pushinteger(L, argIVec[i]);
            lua_rawseti(L, -2, i + 1);
        }
        break;
    case 6:
        lua_pushstring(L, argString != nullptr ? argString : "");
        break;
    default:
        lua_pushnil(L);
        break;
    }
    return 3;
}

// Ext._Internal.ImguiInputState() -> mouseX, mouseY, width, height, down
int l_imgui_input_state(lua_State* L) {
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;
    bool down = false;
    bool wantMouse = false;
    bool hoveredWindow = false;
    bool navNoHover = false;
    bg3le_imgui_input_state(&x, &y, &w, &h, &down, &wantMouse, &hoveredWindow,
                            &navNoHover);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, w);
    lua_pushnumber(L, h);
    lua_pushboolean(L, down ? 1 : 0);
    lua_pushboolean(L, wantMouse ? 1 : 0);
    lua_pushboolean(L, hoveredWindow ? 1 : 0);
    lua_pushboolean(L, navNoHover ? 1 : 0);
    return 8;
}

// Ext._Internal.ImguiMouseMove(x, y) -- or no arguments to let go
//
// What the overlay needs to be driven without a mouse: a test, or a mod
// automating its own UI. imgui sees these exactly as it sees the real ones.
//
// The position is held rather than sent once, because the SDL backend sets
// it from the real mouse every frame and would otherwise put it straight
// back.
// Ext._Internal.ImguiWindowGeometry(name)
//   -> x, y, w, h, contentMinX, contentMinY, contentMaxX, contentMaxY,
//      fontSize, firstItemX, firstItemY, contentReachX, contentReachY
int l_imgui_window_geometry(lua_State* L) {
    float out[33] = {};
    if (!bg3le_imgui_window_geometry(out, 33)) return 0;

    // Lua guarantees a C function only twenty free stack slots, and this
    // returns more than that; pushing past them corrupts the stack, which
    // took down the game rather than raising anything.
    luaL_checkstack(L, (int)std::size(out), "Ext._Internal.ImguiWindowGeometry");
    for (float value : out) lua_pushnumber(L, value);
    return (int)std::size(out);
}

// Ext.IMGUI.LoadFont(name, path, size) -> bool
int l_imgui_load_font(lua_State* L) {
    lua_pushboolean(L, bg3le_imgui_load_font(luaL_checkstring(L, 1),
                                             luaL_optstring(L, 2, ""),
                                             (float)luaL_checknumber(L, 3))
                           ? 1
                           : 0);
    return 1;
}

// Ext._Internal.ImguiFontInfo(name) -> "missing" | "unloaded" | "loaded", size
extern "C" int bg3le_imgui_font_info(char const* name, float* size);
int l_imgui_font_info(lua_State* L) {
    float size = 0;
    const int state = bg3le_imgui_font_info(luaL_checkstring(L, 1), &size);
    lua_pushstring(L, state == 2 ? "loaded" : state == 1 ? "unloaded" : "missing");
    lua_pushnumber(L, size);
    return 2;
}

int l_imgui_set_ui_scale(lua_State* L) {
    bg3le_imgui_set_ui_scale((float)luaL_checknumber(L, 1));
    return 0;
}

int l_imgui_set_font_scale(lua_State* L) {
    bg3le_imgui_set_font_scale((float)luaL_checknumber(L, 1));
    return 0;
}

// Ext.IMGUI.GetViewportSize() -> {width, height}
int l_imgui_viewport_size(lua_State* L) {
    int width = 0;
    int height = 0;
    if (!bg3le_imgui_viewport_size(&width, &height)) return 0;
    lua_createtable(L, 2, 0);
    lua_pushinteger(L, width);
    lua_rawseti(L, -2, 1);
    lua_pushinteger(L, height);
    lua_rawseti(L, -2, 2);
    return 1;
}

// Ext._Internal.ImguiWatch(window[, item])
//
// Which window's layout the next drawn frame should record, and which item
// within it to compute an id for. Read back with ImguiWindowGeometry and
// ImguiHovered; nothing reads imgui live, because the console asks from one
// thread and the frame is drawn on another.
int l_imgui_watch(lua_State* L) {
    bg3le_imgui_watch(luaL_optstring(L, 1, ""), luaL_optstring(L, 2, ""));
    return 0;
}

// Ext._Internal.ImguiHovered() -> window, hoveredId, activeId, navId, itemId
int l_imgui_hovered(lua_State* L) {
    char name[128] = {};
    unsigned hoveredId = 0;
    unsigned activeId = 0;
    unsigned navId = 0;
    unsigned watchedId = 0;
    if (!bg3le_imgui_hovered(name, sizeof(name), &hoveredId, &activeId,
                             &navId, &watchedId)) {
        return 0;
    }
    lua_pushstring(L, name);
    lua_pushinteger(L, (lua_Integer)hoveredId);
    lua_pushinteger(L, (lua_Integer)activeId);
    lua_pushinteger(L, (lua_Integer)navId);
    lua_pushinteger(L, (lua_Integer)watchedId);
    return 5;
}

int l_imgui_mouse_move(lua_State* L) {
    if (lua_isnoneornil(L, 1)) {
        bg3le_imgui_hold_mouse(0, 0, false);
        return 0;
    }
    bg3le_imgui_hold_mouse((float)luaL_checknumber(L, 1),
                           (float)luaL_checknumber(L, 2), true);
    return 0;
}

// Ext._Internal.ImguiClickAt(x, y[, button]) -- a press and a release
//
// Queued, and applied one change per frame: two button changes in one frame
// are one click to a widget, so a sweep that sent them all at once would
// register once.
int l_imgui_click_at(lua_State* L) {
    bg3le_imgui_click_at((float)luaL_checknumber(L, 1),
                         (float)luaL_checknumber(L, 2),
                         (int)luaL_optinteger(L, 3, 0));
    return 0;
}

int l_imgui_enable_demo(lua_State* L) {
    bg3le_imgui_enable_demo(lua_toboolean(L, 1) != 0);
    return 0;
}

// Ext._Internal.ImguiStatus() -> wanted, started, initialized
int l_imgui_status(lua_State* L) {
    bool wanted = false;
    bool started = false;
    bool initialized = false;
    bg3le_imgui_status(&wanted, &started, &initialized);
    lua_pushboolean(L, wanted ? 1 : 0);
    lua_pushboolean(L, started ? 1 : 0);
    lua_pushboolean(L, initialized ? 1 : 0);
    return 3;
}

// Ext._Internal.GlobalSwitches() -> address
//
// ls::GlobalSwitches has no symbol; src/vendor/global_switches.cpp finds it by
// its own contents and verifies the base before reporting it.
// Ext._Internal.InputManager() -> address of ls::gInputManager's object.
// Found through upstream's anchor (the keyboard_WhiteBoxing.json path is
// loaded beside it); its two CRITICAL_SECTIONs are 48 bytes here, as the
// compiled layout has them.
int l_input_manager(lua_State* L) {
    void* manager = nullptr;
    if (bg3le::target::InputManager() == 0
        || !safe_read(reinterpret_cast<void*>(load_bias() + bg3le::target::InputManager()),
                   &manager, sizeof(manager))
        || manager == nullptr) {
        return 0;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)manager);
    return 1;
}

int l_global_switches(lua_State* L) {
    void* at = bg3le_global_switches();
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.StatsCopyFrom(destAddr, sourceName) -> carried, total
extern "C" bool bg3le_stats_copy_rest(void* dest, void const* source);
int l_stats_copy_from(lua_State* L) {
    auto const* dest = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const char* from = luaL_checkstring(L, 2);

    void* source = bg3le_stats_find(from);
    if (source == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no stat named %s", from);
        return 2;
    }

    std::size_t carried = 0;
    std::size_t total = 0;
    const bool copied = bg3le_stats_copy_from(dest, source, &carried, &total);
    // Upstream copies the maps, requirements and combo sets after the
    // properties; a refusal across modifier lists stops before them.
    if (copied && !bg3le_stats_copy_rest(const_cast<void*>(dest), source)) {
        lua_pushnil(L);
        lua_pushstring(L, "copied the properties, but not the functor or roll condition maps");
        return 2;
    }
    if (!copied) {
        lua_pushnil(L);
        lua_pushfstring(L,
            "copied %d of %d properties; the two stats are probably of "
            "different modifier lists, which upstream refuses too",
            (int)carried, (int)total);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)carried);
    lua_pushinteger(L, (lua_Integer)total);
    return 2;
}

// Ext._Internal.StatsAttrTranslated(raw) -> loca handle
int l_stats_attr_translated(lua_State* L) {
    char const* text =
        bg3le_stats_attr_translated((int)luaL_checkinteger(L, 1));
    if (text == nullptr) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.StatsAttrCondition(raw) -> condition expression
int l_stats_attr_condition(lua_State* L) {
    char const* text =
        bg3le_stats_attr_condition((int)luaL_checkinteger(L, 1));
    if (text == nullptr) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.ExpressionDump(pooled) -- diagnostics only.
int l_expression_dump(lua_State* L) {
    bg3le_stats_expression_dump(
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1));
    return 0;
}

// Ext._Internal.ObjectExpression(address, class, field)
//   -> pooled address, code, refcount
int l_object_expression(lua_State* L) {
    auto const* object =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    void* pooled = bg3le_stats_object_expression(
        object, luaL_checkstring(L, 2), luaL_checkstring(L, 3));
    if (pooled == nullptr) return 0;

    char const* code = bg3le_stats_expression_code(pooled);
    int refCount = 0;
    if (code == nullptr || !bg3le_stats_expression_refcount(pooled,
                                                            &refCount)) {
        return 0;
    }

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)pooled);
    lua_pushstring(L, code);
    lua_pushinteger(L, refCount);
    return 3;
}

// Ext._Internal.ExpressionAt(pooled) -> code, refCount
int l_expression_at(lua_State* L) {
    auto* pooled = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* code = bg3le_stats_expression_code(pooled);
    int refCount = 0;
    if (code == nullptr || !bg3le_stats_expression_refcount(pooled, &refCount)) return 0;
    lua_pushstring(L, code);
    lua_pushinteger(L, refCount);
    return 2;
}

// Ext._Internal.StatsEnumLabel(enumeration, index) -> label
int l_stats_enum_label(lua_State* L) {
    char const* label = bg3le_stats_enum_label(luaL_checkstring(L, 1),
                                               (int)luaL_checkinteger(L, 2));
    if (label == nullptr) return 0;
    lua_pushstring(L, label);
    return 1;
}

// Ext._Internal.StatsEnumIndex(enumeration, label) -> index
int l_stats_enum_index(lua_State* L) {
    int value = 0;
    if (!bg3le_stats_enum_index(luaL_checkstring(L, 1),
                                luaL_checkstring(L, 2), &value)) {
        return 0;
    }
    lua_pushinteger(L, value);
    return 1;
}

// Ext._Internal.StatsListAttrs(modifierList) -> { {name, type}, ... }
int l_stats_list_attrs(lua_State* L) {
    char const* list = luaL_checkstring(L, 1);
    const std::size_t count = bg3le_stats_list_attr_count(list);
    if (count == 0) return 0;

    lua_createtable(L, (int)count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        char const* name = nullptr;
        char const* type = nullptr;
        if (!bg3le_stats_list_attr_at(list, i, &name, &type)) continue;
        if (name == nullptr) continue;

        lua_createtable(L, 0, 2);
        lua_pushstring(L, name);
        lua_setfield(L, -2, "Name");
        lua_pushstring(L, type != nullptr ? type : "");
        lua_setfield(L, -2, "Type");
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.PrototypeFind(kind, name) -> address; 0 spell, 1 status.
int l_prototype_find(lua_State* L) {
    void* at = bg3le_prototype_find((int)luaL_checkinteger(L, 1),
                                    luaL_checkstring(L, 2));
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.PrototypeNames(kind) -> { name, ... }
int l_prototype_names(lua_State* L) {
    const int kind = (int)luaL_checkinteger(L, 1);
    const std::size_t count = bg3le_prototype_count(kind);
    lua_createtable(L, (int)count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        char const* name = bg3le_prototype_name_at(kind, i);
        if (name == nullptr) break;
        lua_pushstring(L, name);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.PrototypeCount(kind) -> how many, without listing them
int l_prototype_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_prototype_count((int)luaL_checkinteger(L, 1)));
    return 1;
}

extern "C" bool bg3le_prototype_test_stale(int kind, char const* name);

// Ext._Internal.PrototypeTestStale(kind, name) -> whether the entry was found (a test hook)
int l_prototype_test_stale(lua_State* L) {
    lua_pushboolean(L, bg3le_prototype_test_stale((int)luaL_checkinteger(L, 1),
                                                  luaL_checkstring(L, 2)));
    return 1;
}

// Ext._Internal.TemplateFind(id) -> address, engine type name
int l_template_find(lua_State* L) {
    char const* id = luaL_checkstring(L, 1);
    void* at = bg3le_templates_find(id);
    if (at == nullptr) return 0;

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    char const* type = bg3le_templates_type(id);
    if (type != nullptr) {
        lua_pushstring(L, type);
    } else {
        lua_pushnil(L);
    }
    return 2;
}

// Ext._Internal.TemplateFindIn(source, id) -> address, engine type name,
// from the level's local templates (1), the cache (2) or the level's cache (3)
int l_template_find_in(lua_State* L) {
    char const* type = nullptr;
    void* at = bg3le_templates_in((int)luaL_checkinteger(L, 1), luaL_checkstring(L, 2), &type);
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    if (type != nullptr) {
        lua_pushstring(L, type);
    } else {
        lua_pushnil(L);
    }
    return 2;
}

// Ext._Internal.TemplatesIn(source) -> {id = address}, {id = engine type},
// or nothing when that manager is not there
int l_templates_in(lua_State* L) {
    const int source = (int)luaL_checkinteger(L, 1);
    lua_newtable(L);
    lua_newtable(L);
    auto each = [](void* ctx, char const* id, void* at, char const* type) {
        auto* S = static_cast<lua_State*>(ctx);
        lua_pushinteger(S, (lua_Integer)(std::uintptr_t)at);
        lua_setfield(S, -3, id);
        if (type != nullptr) {
            lua_pushstring(S, type);
            lua_setfield(S, -2, id);
        }
    };
    if (!bg3le_templates_list(source, each, L)) return 0;
    return 2;
}

// Ext._Internal.LevelDataManager() -> address, or nothing
extern "C" void* bg3le_level_data_manager();
int l_level_data_manager(lua_State* L) {
    void* at = bg3le_level_data_manager();
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.LevelAddPersistentTemplate(parent, sub, instance) -> count, or nil and why
extern "C" std::uint32_t bg3le_level_add_persistent_template(char const* parent,
                                                              char const* subLevel,
                                                              char const* instance,
                                                              char const** why);
int l_level_add_persistent_template(lua_State* L) {
    char const* why = nullptr;
    const std::uint32_t count = bg3le_level_add_persistent_template(
        luaL_checkstring(L, 1), luaL_checkstring(L, 2), luaL_checkstring(L, 3), &why);
    if (count == 0) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushinteger(L, count);
    return 1;
}

// Ext._Internal.PhysicsQuery(op, client, 11 floats, type, include, exclude,
// context) -> hit, address; or nothing without a physics scene
extern "C" int bg3le_physics_query(int op, bool client, float const* v, std::uint32_t type,
                                   std::uint32_t include, std::uint32_t exclude, int context,
                                   void** out);
int l_physics_query(lua_State* L) {
    float v[11];
    for (int i = 0; i < 11; ++i) v[i] = (float)luaL_checknumber(L, 3 + i);
    void* out = nullptr;
    const int hit = bg3le_physics_query(
        (int)luaL_checkinteger(L, 1), lua_toboolean(L, 2) != 0, v,
        (std::uint32_t)luaL_checkinteger(L, 14), (std::uint32_t)luaL_checkinteger(L, 15),
        (std::uint32_t)luaL_checkinteger(L, 16), (int)luaL_checkinteger(L, 17), &out);
    if (hit < 0) return 0;
    lua_pushboolean(L, hit);
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)out);
    return 2;
}

// Ext._Internal.AiEntitiesOnTile(client, x, y, z) -> {handle...}
extern "C" std::size_t bg3le_ai_entities_on_tile(bool client, float const* v,
                                                 std::uint64_t* out, std::size_t cap);
int l_ai_entities_on_tile(lua_State* L) {
    const float v[3] = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                        (float)luaL_checknumber(L, 4)};
    std::vector<std::uint64_t> handles(64);
    std::size_t n = bg3le_ai_entities_on_tile(lua_toboolean(L, 1) != 0, v, handles.data(), handles.size());
    if (n > handles.size()) {
        handles.resize(n);
        n = bg3le_ai_entities_on_tile(lua_toboolean(L, 1) != 0, v, handles.data(), handles.size());
    }
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n && i < handles.size(); ++i) {
        lua_pushinteger(L, (lua_Integer)handles[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.AiTileInfo(client, x, y, z) -> address, or nothing
extern "C" void* bg3le_ai_tile_info(bool client, float const* v);
int l_ai_tile_info(lua_State* L) {
    const float v[3] = {(float)luaL_checknumber(L, 2), (float)luaL_checknumber(L, 3),
                        (float)luaL_checknumber(L, 4)};
    void* at = bg3le_ai_tile_info(lua_toboolean(L, 1) != 0, v);
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.AiHeightsAt(client, x, z) -> {height...}
extern "C" std::size_t bg3le_ai_heights_at(bool client, float x, float z, float* out,
                                           std::size_t cap);
int l_ai_heights_at(lua_State* L) {
    float heights[64];
    const std::size_t n = bg3le_ai_heights_at(lua_toboolean(L, 1) != 0, (float)luaL_checknumber(L, 2),
                                              (float)luaL_checknumber(L, 3), heights, 64);
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n && i < 64; ++i) {
        lua_pushnumber(L, heights[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.AiPathCreate(client) -> address, or nil and why
extern "C" void* bg3le_ai_path_create(bool client, char const** why);
int l_ai_path_create(lua_State* L) {
    char const* why = nullptr;
    void* at = bg3le_ai_path_create(lua_toboolean(L, 1) != 0, &why);
    if (at == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.AiPathFree(client, address)
extern "C" void bg3le_ai_path_free(bool client, void* at);
int l_ai_path_free(lua_State* L) {
    bg3le_ai_path_free(lua_toboolean(L, 1) != 0, (void*)(std::uintptr_t)luaL_checkinteger(L, 2));
    return 0;
}

// Ext._Internal.AiPathById(client, id) -> address, or nothing
extern "C" void* bg3le_ai_path_by_id(bool client, std::uint32_t id);
int l_ai_path_by_id(lua_State* L) {
    void* at = bg3le_ai_path_by_id(lua_toboolean(L, 1) != 0, (std::uint32_t)luaL_checkinteger(L, 2));
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.AiPathSearch(client, address) -> goal found, or nil and why
extern "C" int bg3le_ai_path_search(bool client, void* at, char const** why);
int l_ai_path_search(lua_State* L) {
    char const* why = nullptr;
    const int found = bg3le_ai_path_search(lua_toboolean(L, 1) != 0,
                                           (void*)(std::uintptr_t)luaL_checkinteger(L, 2), &why);
    if (found < 0) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushboolean(L, found);
    return 1;
}

extern "C" void* bg3le_resource_bank(std::int32_t typeIndex);

// The ClassDescription bank, as upstream hands to surface actions and functor
// contexts; null when the static data is not up.
void* class_description_bank() {
    void const* meta = bg3le_meta_class("ClassDescription");
    const char* engineClass = meta != nullptr ? bg3le_meta_engine_class(meta) : nullptr;
    if (engineClass == nullptr) return nullptr;
    auto index = ecs::index_of(ecs::Context::ImmutableData, engineClass);
    return index ? bg3le_resource_bank((std::int32_t)*index) : nullptr;
}

// Ext._Internal.FunctorParams(type) -> address, or nothing
extern "C" void* bg3le_functor_params(int type, void* classDescriptions);
int l_functor_params(lua_State* L) {
    void* at = bg3le_functor_params((int)luaL_checkinteger(L, 1), class_description_bank());
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.FunctorsExecute(functors, context, single) -> true, or nil and why
extern "C" bool bg3le_functors_execute(void* functors, void* context, void* world, char const** why);
extern "C" bool bg3le_functor_execute(void* functor, void* context, void* world, char const** why);
int l_functors_execute(lua_State* L) {
    auto* functors = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    auto* context = (void*)(std::uintptr_t)luaL_checkinteger(L, 2);
    char const* why = nullptr;
    const bool ok = lua_toboolean(L, 3)
                        ? bg3le_functor_execute(functors, context, server_container(), &why)
                        : bg3le_functors_execute(functors, context, server_container(), &why);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.SurfaceActionCreate(type) -> address, or nil and why
extern "C" void* bg3le_surface_action_create(int type, void* classDescriptions, char const** why);
int l_surface_action_create(lua_State* L) {
    char const* why = nullptr;
    void* at = bg3le_surface_action_create((int)luaL_checkinteger(L, 1), class_description_bank(), &why);
    if (at == nullptr) {
        lua_pushnil(L);
        if (why == nullptr) return 1;
        lua_pushstring(L, why);
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.SurfaceActionExecute(address) -> true, or nil and why
extern "C" bool bg3le_surface_action_execute(void* at, char const** why);
int l_surface_action_execute(lua_State* L) {
    char const* why = nullptr;
    if (!bg3le_surface_action_execute((void*)(std::uintptr_t)luaL_checkinteger(L, 1), &why)) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.AiPathsActive(client) -> {address...}
extern "C" std::size_t bg3le_ai_paths_active(bool client, void** out, std::size_t cap);
int l_ai_paths_active(lua_State* L) {
    void* paths[256];
    const std::size_t n = bg3le_ai_paths_active(lua_toboolean(L, 1) != 0, paths, 256);
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n && i < 256; ++i) {
        lua_pushinteger(L, (lua_Integer)(std::uintptr_t)paths[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.TemplateIds() -> every template id
int l_template_ids(lua_State* L) {
    const std::size_t count = bg3le_templates_count();
    lua_createtable(L, (int)count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        char const* id = bg3le_templates_id_at(i);
        if (id == nullptr) break;
        lua_pushstring(L, id);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.Loca(handle) -> text
int l_loca_get(lua_State* L) {
    char const* text = bg3le_loca_get(luaL_checkstring(L, 1));
    if (text == nullptr) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.LocaSet(handle, text) -> bool
int l_loca_set(lua_State* L) {
    char const* handle = luaL_checkstring(L, 1);
    char const* text = luaL_checkstring(L, 2);
    const bool ok = bg3le_loca_set(handle, text);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// Ext._Internal.LocaKeys() -> every handle the repository holds
int l_loca_keys(lua_State* L) {
    const std::size_t count = bg3le_loca_count();
    lua_createtable(L, (int)count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        char const* handle = bg3le_loca_handle_at(i);
        if (handle == nullptr) break;
        lua_pushstring(L, handle);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.AllEntities([component]) -> { handle, ... }
//
// Sized from a first pass so the second cannot overrun, which matters
// because entities come and go between the two on a live world.
int l_all_entities(lua_State* L) {
    // The calling context's world, the one every other read here uses.
    // Enumerating ecs::container() instead handed back handles from
    // whichever world was captured first -- valid there, rejected by the
    // component readers, and so entirely unreadable.
    void* container = world_container();
    if (container == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "the ECS container has not been captured yet");
        return 2;
    }

    int component = -1;
    if (!lua_isnoneornil(L, 1)) {
        char const* name = luaL_checkstring(L, 1);

        // Either spelling, as everywhere else that names a component: the
        // index is registered under the engine name, and the metadata
        // maps bg3se's short one onto it.
        void const* meta = bg3le_meta_component(name);
        char const* engineName =
            meta != nullptr ? bg3le_meta_engine_class(meta) : name;

        const auto index = component_index(engineName);
        if (!index.has_value()) {
            lua_pushnil(L);
            lua_pushfstring(L, "no component named %s", name);
            return 2;
        }
        component = (int)*index;
    }

    const std::size_t count =
        bg3le_entities_collect(container, component, nullptr, 0);
    std::vector<std::uint64_t> handles(count);
    const std::size_t got = bg3le_entities_collect(
        container, component, handles.data(), handles.size());

    const std::size_t n = got < count ? got : count;
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        lua_pushinteger(L, (lua_Integer)handles[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.ComponentTypeNames() -> every component bg3le can read.
int l_component_type_names(lua_State* L) {
    const std::size_t count = bg3le_meta_class_count();
    lua_newtable(L);

    int n = 0;
    for (std::size_t i = 0; i < count; ++i) {
        void const* cls = bg3le_meta_class_at(i);
        if (cls == nullptr) continue;
        char const* engine = bg3le_meta_engine_class(cls);
        char const* name = bg3le_meta_class_name(cls);
        if (engine == nullptr || name == nullptr) continue;

        lua_pushstring(L, name);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// Ext._Internal.ClassName(name) -> the reflected class name.
//
// A component answers to three names and only one of them keys the type
// registry, so a view has to be told which it is before it can report a type.
// A name may be both a component's and a class's ("Origin" is a component
// and a static data resource); a caller holding a class name says so, and
// the class is looked up first.
void const* meta_by_name(const char* name, bool classFirst) {
    void const* meta = classFirst ? bg3le_meta_class(name) : bg3le_meta_component(name);
    if (meta == nullptr) meta = classFirst ? bg3le_meta_component(name) : bg3le_meta_class(name);
    return meta;
}

// Ext._Internal.ClassName(name[, classFirst])
int l_class_name(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    void const* meta = meta_by_name(name, lua_toboolean(L, 2) != 0);
    if (meta == nullptr) return 0;

    char const* className = bg3le_meta_class_name(meta);
    if (className == nullptr) return 0;
    lua_pushstring(L, className);
    return 1;
}

// Ext._Internal.TypeNameAt(class, path) -> the declared type of that field.
//
// A nested struct is an object with a type of its own, and a view over one
// should say which -- Ext.Types.GetObjectType on entity.Transform.Translate
// has to name a type GetTypeInfo can then find.
// Ext._Internal.TypeNameAt(class, path[, classFirst])
int l_type_name_at(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* path = luaL_optstring(L, 2, "");

    void const* meta = meta_by_name(name, lua_toboolean(L, 3) != 0);
    if (meta == nullptr) return 0;

    char const* found = nullptr;
    std::uint16_t length = 0;
    if (!bg3le_meta_type_name_at(meta, path, &found, &length)) return 0;

    lua_pushlstring(L, found, length);
    return 1;
}

// Ext._Internal.TypeNames() -> every reflected class name, for Ext.Types.
int l_type_names(lua_State* L) {
    const std::size_t count = bg3le_meta_class_count();
    lua_createtable(L, (int)count, 0);

    int n = 0;
    for (std::size_t i = 0; i < count; ++i) {
        void const* cls = bg3le_meta_class_at(i);
        char const* name = cls != nullptr ? bg3le_meta_class_name(cls)
                                          : nullptr;
        if (name == nullptr) continue;
        lua_pushstring(L, name);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// Ext._Internal.TypeIsComponent(class) -> engine component name, or nil
int l_type_component(lua_State* L) {
    void const* meta = bg3le_meta_class(luaL_checkstring(L, 1));
    if (meta == nullptr) return 0;
    char const* engine = bg3le_meta_engine_class(meta);
    if (engine == nullptr) return 0;
    lua_pushstring(L, engine);
    return 1;
}

// Ext._Internal.StatsFunctorGroups(address, attribute)
//   -> { {TextKey, Functors}, ... } where Functors is a list of
//      {address, class} pairs for the prelude to read reflectively.
//
// nil when the attribute carries no functors, which upstream reports as
// null rather than as an empty list.
int l_stats_functor_groups(lua_State* L) {
    auto const* object =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* attribute = luaL_checkstring(L, 2);

    const int groups = bg3le_stats_functor_groups(object, attribute);
    if (groups < 0) return 0;

    lua_createtable(L, groups, 0);
    for (int g = 0; g < groups; ++g) {
        char const* textKey = nullptr;
        void* functors = nullptr;
        if (!bg3le_stats_functor_group_at(object, attribute, g, &textKey,
                                          &functors)) {
            continue;
        }

        lua_createtable(L, 0, 2);
        lua_pushstring(L, textKey != nullptr ? textKey : "");
        lua_setfield(L, -2, "TextKey");

        const int count = bg3le_stats_functor_count(functors);
        lua_createtable(L, count < 0 ? 0 : count, 0);
        for (int i = 0; i < count; ++i) {
            void* functor = bg3le_stats_functor_at(functors, i);
            char const* className = bg3le_stats_functor_class(functor);
            if (functor == nullptr || className == nullptr) continue;

            lua_createtable(L, 0, 2);
            lua_pushinteger(L, (lua_Integer)(std::uintptr_t)functor);
            lua_setfield(L, -2, "Address");
            lua_pushstring(L, className);
            lua_setfield(L, -2, "Class");
            lua_rawseti(L, -2, i + 1);
        }
        lua_setfield(L, -2, "Functors");

        lua_rawseti(L, -2, g + 1);
    }
    return 1;
}

// Ext._Internal.FunctorsAdd(set, type) -> address, class; or nil and why
extern "C" void* bg3le_functors_add(void* functors, int type, char const** why);
extern "C" bool bg3le_functors_remove(void* functors, void* functor);
int l_functors_add(lua_State* L) {
    char const* why = nullptr;
    void* at = bg3le_functors_add((void*)(std::uintptr_t)luaL_checkinteger(L, 1),
                                  (int)luaL_checkinteger(L, 2), &why);
    if (at == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    char const* className = bg3le_stats_functor_class(at);
    if (className == nullptr) return 1;
    lua_pushstring(L, className);
    return 2;
}
int l_functors_remove(lua_State* L) {
    lua_pushboolean(L, bg3le_functors_remove((void*)(std::uintptr_t)luaL_checkinteger(L, 1),
                                             (void*)(std::uintptr_t)luaL_checkinteger(L, 2)));
    return 1;
}

// Ext._Internal.FunctorsList(address) -> {address, class, address, class...}
int l_functors_list(lua_State* L) {
    auto const* functors = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const int count = bg3le_stats_functor_count(functors);
    lua_createtable(L, count < 0 ? 0 : count * 2, 0);
    int n = 0;
    for (int i = 0; i < count; ++i) {
        void* functor = bg3le_stats_functor_at(functors, i);
        char const* className = bg3le_stats_functor_class(functor);
        if (functor == nullptr || className == nullptr) continue;
        lua_pushinteger(L, (lua_Integer)(std::uintptr_t)functor);
        lua_rawseti(L, -2, ++n);
        lua_pushstring(L, className);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// Ext._Internal.StatsAIFlags(address) -> the object's AIFlags
int l_stats_ai_flags(lua_State* L) {
    auto const* object =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* text = bg3le_stats_ai_flags(object);
    if (text == nullptr) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.StatsRollConditions(address, attribute)
//   -> { [name] = condition, ... }, or nil when the attribute has none
int l_stats_roll_conditions(lua_State* L) {
    auto const* object =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* attribute = luaL_checkstring(L, 2);

    const int count = bg3le_stats_roll_condition_count(object, attribute);
    if (count < 0) return 0;

    lua_createtable(L, 0, count);
    for (int i = 0; i < count; ++i) {
        char const* name = nullptr;
        char const* text = nullptr;
        if (!bg3le_stats_roll_condition_at(object, attribute, i, &name,
                                           &text)) {
            continue;
        }
        // Upstream skips a roll condition whose expression does not
        // resolve, rather than storing an empty one.
        if (name == nullptr || text == nullptr) continue;
        lua_pushstring(L, text);
        lua_setfield(L, -2, name);
    }
    return 1;
}

// Ext._Internal.StatOrigin(name) -> modId, originalModId
//
// Not a field on the stat: which mod defines an entry comes from the
// archives. See src/vendor/stat_origins.cpp.
int l_stat_origin(lua_State* L) {
    char const* name = luaL_checkstring(L, 1);
    char const* modId = nullptr;
    char const* originalModId = nullptr;
    if (!bg3le_stat_origin(name, &modId, &originalModId)) return 0;

    if (modId != nullptr) {
        lua_pushstring(L, modId);
    } else {
        lua_pushnil(L);
    }
    if (originalModId != nullptr) {
        lua_pushstring(L, originalModId);
    } else {
        lua_pushnil(L);
    }
    return 2;
}

// ---- Ext.Stats ----
//
// The manager is found by fingerprint (src/vendor/stats.cpp). Attribute
// values are stored apart from their names, so one attribute takes a name, a
// type and a raw int, and the decoding of that int depends on the type. These
// entry points hand the pieces to Lua and the prelude assembles them, which
// keeps the C side free of policy about how a stat should look.

// Ext._Internal.StatsCount() -> n
int l_stats_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_stats_count());
    return 1;
}

// Ext._Internal.StatsTakeLoaded() -> whether StatsLoaded is owed
extern "C" bool bg3le_stats_take_loaded(bool client);
extern "C" void bg3le_stats_module_loaded();
int l_stats_take_loaded(lua_State* L) {
    lua_pushboolean(L, bg3le_stats_take_loaded(in_client_state()) ? 1 : 0);
    return 1;
}

// ---- Ext.Stats' submodules; src/vendor/stats_sets.cpp ----
namespace sets = bg3le::stats_sets;

sets::Kind check_kind(lua_State* L, int index) {
    const auto k = luaL_checkinteger(L, index);
    if (k < 0 || k > (lua_Integer)sets::Kind::NameGroup) luaL_error(L, "bad stats set kind %d", (int)k);
    return (sets::Kind)k;
}

// Ext._Internal.StatsSetAll(kind) -> {addresses}, type name
int l_stats_set_all(lua_State* L) {
    const auto kind = check_kind(L, 1);
    const auto items = sets::all(kind);
    lua_createtable(L, (int)items.size(), 0);
    for (std::size_t i = 0; i < items.size(); ++i) {
        lua_pushinteger(L, (lua_Integer)(std::uintptr_t)items[i]);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_pushstring(L, sets::type_name(kind));
    return 2;
}

// Ext._Internal.StatsSetGet(kind, name) -> address, type name; or nil
int l_stats_set_get(lua_State* L) {
    const auto kind = check_kind(L, 1);
    void* at = sets::get(kind, luaL_checkstring(L, 2));
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    lua_pushstring(L, sets::type_name(kind));
    return 2;
}

// Ext._Internal.StatsSetCreate(kind, name) -> address, type name; or nil, why
int l_stats_set_create(lua_State* L) {
    const auto kind = check_kind(L, 1);
    std::string why;
    void* at = sets::create(kind, luaL_checkstring(L, 2), &why);
    if (at == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, why.c_str());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    lua_pushstring(L, sets::type_name(kind));
    return 2;
}

void set_int(lua_State* L, char const* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}
void set_bool(lua_State* L, char const* key, bool v) {
    lua_pushboolean(L, v ? 1 : 0);
    lua_setfield(L, -2, key);
}
void set_str(lua_State* L, char const* key, std::string const& v) {
    lua_pushlstring(L, v.data(), v.size());
    lua_setfield(L, -2, key);
}

// Ext._Internal.StatsTreasureTableRead(name) -> upstream's serialized table, or nil
int l_stats_treasure_table_read(lua_State* L) {
    sets::Table t;
    if (!sets::read_table(luaL_checkstring(L, 1), &t)) { lua_pushnil(L); return 1; }
    const auto rarities = sets::rarities();
    lua_newtable(L);
    set_str(L, "Name", t.Name);
    set_int(L, "MinLevel", t.MinLevel);
    set_int(L, "MaxLevel", t.MaxLevel);
    set_bool(L, "IgnoreLevelDiff", t.IgnoreLevelDiff);
    set_bool(L, "UseTreasureGroupContainers", t.UseTreasureGroupContainers);
    set_bool(L, "CanMerge", t.CanMerge);
    lua_createtable(L, (int)t.SubTables.size(), 0);
    for (std::size_t s = 0; s < t.SubTables.size(); ++s) {
        auto const& sub = t.SubTables[s];
        lua_newtable(L);
        set_int(L, "TotalCount", sub.TotalCount);
        set_int(L, "StartLevel", sub.StartLevel);
        set_int(L, "EndLevel", sub.EndLevel);
        lua_createtable(L, (int)sub.Categories.size(), 0);
        for (std::size_t c = 0; c < sub.Categories.size(); ++c) {
            auto const& cat = sub.Categories[c];
            lua_newtable(L);
            set_int(L, "Frequency", cat.Frequency);
            if (!cat.Name.empty()) set_str(L, cat.IsTable ? "TreasureTable" : "TreasureCategory", cat.Name);
            for (std::size_t r = 0; r < rarities.size() && r < 7; ++r) {
                if (!rarities[r].empty()) set_int(L, rarities[r].c_str(), cat.Frequencies[r]);
            }
            lua_rawseti(L, -2, (lua_Integer)c + 1);
        }
        lua_setfield(L, -2, "Categories");
        lua_createtable(L, (int)sub.DropCounts.size(), 0);
        for (std::size_t d = 0; d < sub.DropCounts.size(); ++d) {
            lua_newtable(L);
            set_int(L, "Chance", sub.DropCounts[d].Chance);
            set_int(L, "Amount", sub.DropCounts[d].Amount);
            lua_rawseti(L, -2, (lua_Integer)d + 1);
        }
        lua_setfield(L, -2, "DropCounts");
        lua_rawseti(L, -2, (lua_Integer)s + 1);
    }
    lua_setfield(L, -2, "SubTables");
    return 1;
}

// Ext._Internal.StatsTreasureCategoryRead(name) -> upstream's serialized table, or nil
int l_stats_treasure_category_read(lua_State* L) {
    sets::Category c;
    if (!sets::read_category(luaL_checkstring(L, 1), &c)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    set_str(L, "Category", c.Name);
    lua_createtable(L, (int)c.Items.size(), 0);
    for (std::size_t i = 0; i < c.Items.size(); ++i) {
        auto const& item = c.Items[i];
        lua_newtable(L);
        set_str(L, "Name", item.Name);
        set_int(L, "Priority", item.Priority);
        set_int(L, "MinAmount", item.MinAmount);
        set_int(L, "MaxAmount", item.MaxAmount);
        set_int(L, "ActPart", item.ActPart);
        set_int(L, "Unique", item.Unique);
        set_int(L, "MinLevel", item.MinLevel);
        set_int(L, "MaxLevel", item.MaxLevel);
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    lua_setfield(L, -2, "Items");
    return 1;
}

// A field of the table on top of the stack, as upstream's serializer reads
// it: an optional one falls back to its default, a required one raises.
lua_Integer field_int(lua_State* L, char const* key, bool required, lua_Integer fallback) {
    lua_getfield(L, -1, key);
    lua_Integer v = fallback;
    if (lua_isnil(L, -1)) {
        if (required) luaL_error(L, "Missing required property '%s'", key);
    } else {
        v = luaL_checkinteger(L, -1);
    }
    lua_pop(L, 1);
    return v;
}
bool field_bool(lua_State* L, char const* key, bool fallback) {
    lua_getfield(L, -1, key);
    const bool v = lua_isnil(L, -1) ? fallback : lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
std::string field_str(lua_State* L, char const* key, bool required) {
    lua_getfield(L, -1, key);
    std::string v;
    if (lua_isnil(L, -1)) {
        if (required) luaL_error(L, "Missing required property '%s'", key);
    } else {
        v = luaL_checkstring(L, -1);
    }
    lua_pop(L, 1);
    return v;
}
// Calls fn for each element of the array field, with that element on top.
template <class Fn>
void field_each(lua_State* L, char const* key, Fn fn) {
    lua_getfield(L, -1, key);
    if (lua_isnil(L, -1)) luaL_error(L, "Missing required property '%s'", key);
    luaL_checktype(L, -1, LUA_TTABLE);
    const lua_Integer n = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, -1, i);
        luaL_checktype(L, -1, LUA_TTABLE);
        fn();
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

// Ext._Internal.StatsTreasureTableUpdate(table) -> true, or nil, why
int l_stats_treasure_table_update(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const auto rarities = sets::rarities();
    lua_pushvalue(L, 1);
    sets::Table t;
    t.Name = field_str(L, "Name", true);
    t.MinLevel = (int)field_int(L, "MinLevel", false, 0);
    t.MaxLevel = (int)field_int(L, "MaxLevel", false, 0);
    t.IgnoreLevelDiff = field_bool(L, "IgnoreLevelDiff", false);
    t.UseTreasureGroupContainers = field_bool(L, "UseTreasureGroupContainers", false);
    t.CanMerge = field_bool(L, "CanMerge", false);
    field_each(L, "SubTables", [&] {
        sets::SubTable sub;
        sub.TotalCount = (int)field_int(L, "TotalCount", true, 0);
        sub.StartLevel = (int)field_int(L, "StartLevel", false, 0);
        sub.EndLevel = (int)field_int(L, "EndLevel", false, 0);
        field_each(L, "Categories", [&] {
            sets::CategoryRef c;
            c.Frequency = (int)field_int(L, "Frequency", false, 1);
            const std::string table = field_str(L, "TreasureTable", false);
            c.IsTable = !table.empty();
            c.Name = c.IsTable ? table : field_str(L, "TreasureCategory", false);
            for (std::size_t r = 0; r < rarities.size() && r < 7; ++r) {
                if (!rarities[r].empty()) {
                    c.Frequencies[r] = (std::uint16_t)field_int(L, rarities[r].c_str(), false, 0);
                }
            }
            sub.Categories.push_back(std::move(c));
        });
        field_each(L, "DropCounts", [&] {
            sets::DropCount d;
            d.Chance = (int)field_int(L, "Chance", true, 0);
            d.Amount = (int)field_int(L, "Amount", true, 0);
            sub.DropCounts.push_back(d);
        });
        t.SubTables.push_back(std::move(sub));
    });
    lua_pop(L, 1);

    std::string why;
    if (!sets::update_table(t, &why)) {
        return luaL_error(L, "%s", why.c_str());
    }
    return 0;
}

// Ext._Internal.StatsTreasureCategoryUpdate(name, table)
int l_stats_treasure_category_update(lua_State* L) {
    char const* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_pushvalue(L, 2);
    sets::Category c;
    c.Name = name;
    field_each(L, "Items", [&] {
        sets::CategoryItem item;
        item.Name = field_str(L, "Name", true);
        item.Priority = (int)field_int(L, "Priority", false, 1);
        item.MinAmount = (int)field_int(L, "MinAmount", false, 1);
        item.MaxAmount = (int)field_int(L, "MaxAmount", false, 1);
        item.ActPart = (int)field_int(L, "ActPart", true, 0);
        item.Unique = (int)field_int(L, "Unique", false, 0);
        item.MinLevel = (int)field_int(L, "MinLevel", false, 0);
        item.MaxLevel = (int)field_int(L, "MaxLevel", false, 0);
        c.Items.push_back(std::move(item));
    });
    lua_pop(L, 1);

    std::string why;
    if (!sets::update_category(name, c, &why)) {
        return luaL_error(L, "%s", why.c_str());
    }
    return 0;
}

// Ext._Internal.StatsNameAt(index) -> name
int l_stats_name_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* obj = bg3le_stats_at(i);
    if (obj == nullptr) return 0;
    char const* name = bg3le_stats_name(obj);
    if (name == nullptr) return 0;
    lua_pushstring(L, name);
    return 1;
}

// Ext._Internal.StatsAt(index) -> address
//
// Filtering needs the address without paying for a name lookup: StatsFind is
// a linear scan, so using it per stat would be quadratic.
int l_stats_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* obj = bg3le_stats_at(i);
    if (obj == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)obj);
    return 1;
}

// Ext._Internal.StatsFind(name) -> address
// Ext._Internal.Env(name) -> the environment variable, or nil. So a
// question of the form "is this bg3le's doing?" can be answered from
// outside the process without a rebuild.
int l_env(lua_State* L) {
    char const* value = std::getenv(luaL_checkstring(L, 1));
    if (value == nullptr) return 0;
    lua_pushstring(L, value);
    return 1;
}

// Ext._Internal.StringTableDump() -- the sub-table shapes and one entry
// each, to the log. A diagnostic, kept because what it establishes -- how
// an entry is allocated -- is what a string attribute write depends on.
int l_string_table_dump(lua_State*) {
    bg3le_fixed_string_dump();
    return 0;
}

// Ext._Internal.StatsNames([modifierList]) -> { name, ... }
//
// From an index built once rather than by walking every stat and asking
// each one what it is. Ext.Stats.GetStats is called in loops, and doing it
// the other way cost a quarter of a second a call.
extern "C" std::size_t bg3le_stats_names_each(char const* list,
                                              void (*each)(void*, char const*),
                                              void* context);

int l_stats_names(lua_State* L) {
    char const* list = lua_isnoneornil(L, 1) ? nullptr : luaL_checkstring(L, 1);
    const std::size_t count = bg3le_stats_names_count(list);

    struct Fill {
        lua_State* L;
        int Kept;
    } fill{L, 0};
    lua_createtable(L, (int)count, 0);
    bg3le_stats_names_each(list, [](void* context, char const* name) {
        auto* f = static_cast<Fill*>(context);
        if (name == nullptr) return;
        lua_pushstring(f->L, name);
        lua_rawseti(f->L, -2, ++f->Kept);
    }, &fill);
    return 1;
}

// Ext._Internal.StatsAttrFind(addr, name) -> index, kind, typeName
//
// For writing, which needs to know where an attribute lives. Looked up
// when a write happens rather than recorded for every attribute of every
// stat that is read: doing the latter built a table per attribute, three
// hundred thousand of them across a mod's stats pass, and the garbage
// made Ext.Stats.Get cost five times more by the six thousandth stat.
int l_stats_attr_find(lua_State* L) {
    auto addr = (std::uintptr_t)luaL_checkinteger(L, 1);
    char const* wanted = luaL_checkstring(L, 2);

    // Caught rather than let out. Lua is built as C++ here, so an
    // exception escaping a C function is caught by the interpreter and
    // reported with whatever is on its stack -- which for this function is
    // the name being looked up, so a mod reading Armor.Shield saw an error
    // whose entire message was "Shield". Nothing about that says where it
    // came from.
    auto const* object = (void const*)addr;
    int index = -1;
    try {
        index = bg3le_stats_attr_index(object, wanted);
    } catch (std::exception const& e) {
        logf("stats: looking up attribute %s threw %s", wanted, e.what());
        return 0;
    } catch (...) {
        logf("stats: looking up attribute %s threw", wanted);
        return 0;
    }
    if (index < 0) return 0;

    char const* typeName = nullptr;
    int kind = 0;
    try {
        bg3le_stats_attr_at(object, (std::size_t)index, nullptr, &typeName,
                            &kind, nullptr);
    } catch (std::exception const& e) {
        logf("stats: reading attribute %s threw %s", wanted, e.what());
        return 0;
    } catch (...) {
        logf("stats: reading attribute %s threw", wanted);
        return 0;
    }

    lua_pushinteger(L, index);
    lua_pushinteger(L, kind);
    if (typeName != nullptr) {
        lua_pushstring(L, typeName);
    } else {
        lua_pushnil(L);
    }
    return 3;
}

// Ext._Internal.StatsAttrSet(addr, index, raw) -> bool
int l_stats_attr_set(lua_State* L) {
    auto addr = (std::uintptr_t)luaL_checkinteger(L, 1);
    const auto index = (std::size_t)luaL_checkinteger(L, 2);
    const int raw = (int)luaL_checkinteger(L, 3);
    lua_pushboolean(L,
                    bg3le_stats_attr_set((void const*)addr, index, raw) ? 1 : 0);
    return 1;
}

// Ext._Internal.FixedStringIntern(text) -> id, or nil
//
// The string-table half on its own, with nothing written to any stat.
// Splitting the two is what tells "placing an entry upsets the engine"
// apart from "an attribute pointing at a new pool slot does".
int l_fixed_string_intern(lua_State* L) {
    unsigned int id = 0;
    if (!bg3le_fixed_string_intern(luaL_checkstring(L, 1), &id)) return 0;
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

// Ext._Internal.StatsStringIntern(text) -> pool index, or nil
int l_stats_string_intern(lua_State* L) {
    const int index = bg3le_stats_string_intern(luaL_checkstring(L, 1));
    if (index < 0) return 0;
    lua_pushinteger(L, index);
    return 1;
}

// Ext._Internal.StatsConditionIntern(text) -> pool index, or nil
int l_stats_condition_intern(lua_State* L) {
    const int index = bg3le_stats_condition_intern(luaL_checkstring(L, 1));
    if (index < 0) return 0;
    lua_pushinteger(L, index);
    return 1;
}

extern "C" int bg3le_stats_int64_intern(std::int64_t value);
extern "C" int bg3le_stats_float_intern(float value);
extern "C" int bg3le_stats_guid_intern(char const* text);
extern "C" int bg3le_stats_translated_intern(char const* text);
extern "C" bool bg3le_stats_ai_flags_set(void const* object, char const* text);

// Ext._Internal.StatsTranslatedIntern(text) -> pool index, or nil
int l_stats_translated_intern(lua_State* L) {
    const int index = bg3le_stats_translated_intern(luaL_checkstring(L, 1));
    if (index < 0) return 0;
    lua_pushinteger(L, index);
    return 1;
}

// Ext._Internal.StatsAIFlagsSet(address, text) -> bool
int l_stats_ai_flags_set(lua_State* L) {
    auto const* object = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, bg3le_stats_ai_flags_set(object, luaL_checkstring(L, 2)));
    return 1;
}
extern "C" int bg3le_stats_roll_set(void const* object, char const* attribute,
                                    char const* const* names,
                                    char const* const* texts,
                                    std::size_t count);

struct RequirementIn {
    std::uint32_t Id;
    std::int32_t IntParam;
    unsigned char Tag[16];
    bool Not;
};
extern "C" int bg3le_stats_requirement_count(void const* object);
extern "C" bool bg3le_stats_requirement_at(void const* object, int index,
                                           std::uint32_t* id,
                                           std::int32_t* intParam,
                                           unsigned char* tag, bool* negated);
extern "C" bool bg3le_stats_requirements_set(void const* object,
                                             RequirementIn const* entries,
                                             std::size_t count);
extern "C" bool bg3le_meta_enum_label_value(char const* enumName,
                                           char const* label,
                                           std::uint64_t* value);

// RequirementType::Tag, whose Param is a GUID rather than an integer.
std::uint32_t requirement_tag() {
    static const std::uint32_t tag = [] {
        std::uint64_t value = 0;
        return bg3le_meta_enum_label_value("RequirementType", "Tag", &value)
                   ? (std::uint32_t)value
                   : 0xffffffffu;
    }();
    return tag;
}

// Ext._Internal.StatsRequirements(address)
//   -> { { Requirement, Not, Param }, ... }, as upstream serialises them
int l_stats_requirements(lua_State* L) {
    auto const* object = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const int count = bg3le_stats_requirement_count(object);
    if (count < 0) return 0;

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        std::uint32_t id = 0;
        std::int32_t intParam = 0;
        unsigned char tag[16] = {};
        bool negated = false;
        if (!bg3le_stats_requirement_at(object, i, &id, &intParam, tag,
                                        &negated)) {
            break;
        }
        lua_createtable(L, 0, 3);
        const char* label = nullptr;
        std::uint64_t value = 0;
        bool named = false;
        for (std::size_t at = 0;
             bg3le_meta_enum_value_at("RequirementType", at, &label, &value);
             ++at) {
            if (value == id) {
                lua_pushstring(L, label);
                named = true;
                break;
            }
        }
        if (!named) lua_pushinteger(L, id);
        lua_setfield(L, -2, "Requirement");
        lua_pushboolean(L, negated);
        lua_setfield(L, -2, "Not");
        if (id == requirement_tag()) {
            char text[40];
            if (bg3le_meta_format_guid(tag, text, sizeof(text))) {
                lua_pushstring(L, text);
            } else {
                lua_pushnil(L);
            }
        } else {
            lua_pushinteger(L, intParam);
        }
        lua_setfield(L, -2, "Param");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// Ext._Internal.StatsRequirementsSet(address, { { Requirement, Not, Param },
//   ... }) -> true, or nil and a reason
int l_stats_requirements_set(lua_State* L) {
    auto const* object = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    push_packed(L, 2);
    lua_replace(L, 2);
    const lua_Integer n = luaL_len(L, 2);
    std::vector<RequirementIn> entries((std::size_t)(n > 0 ? n : 0));

    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        if (!lua_istable(L, -1)) {
            lua_pushnil(L);
            lua_pushfstring(L, "requirement %d is not a table", (int)i);
            return 2;
        }
        RequirementIn& r = entries[(std::size_t)(i - 1)];
        r = RequirementIn{};

        lua_getfield(L, -1, "Requirement");
        std::uint64_t id = 0;
        if (lua_isinteger(L, -1)) {
            id = (std::uint64_t)lua_tointeger(L, -1);
        } else if (!lua_isstring(L, -1)
                   || !bg3le_meta_enum_label_value("RequirementType",
                                                   lua_tostring(L, -1), &id)) {
            lua_pushnil(L);
            lua_pushfstring(L, "requirement %d: %s is not a RequirementType",
                            (int)i, luaL_tolstring(L, -1, nullptr));
            return 2;
        }
        lua_pop(L, 1);
        r.Id = (std::uint32_t)id;

        lua_getfield(L, -1, "Not");
        r.Not = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);

        lua_getfield(L, -1, "Param");
        if (r.Id == requirement_tag()) {
            r.IntParam = -1;
            const char* text = lua_tostring(L, -1);
            if (text == nullptr || !bg3le_meta_parse_guid(text, r.Tag)) {
                lua_pushnil(L);
                lua_pushfstring(L, "requirement %d: Param is not a GUID",
                                (int)i);
                return 2;
            }
        } else {
            r.IntParam = (std::int32_t)luaL_optinteger(L, -1, 0);
        }
        lua_pop(L, 2);
    }

    if (!bg3le_stats_requirements_set(object, entries.data(), entries.size())) {
        lua_pushnil(L);
        lua_pushstring(L, "the requirement array could not be written");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.StatsGuidIntern(text) -> pool index, or nil and a reason
int l_stats_guid_intern(lua_State* L) {
    const int index = bg3le_stats_guid_intern(luaL_checkstring(L, 1));
    if (index >= 0) {
        lua_pushinteger(L, index);
        return 1;
    }
    lua_pushnil(L);
    lua_pushstring(L, index == -1 ? "not a GUID" : "no room in the GUID pool");
    return 2;
}

// Ext._Internal.StatsRollSet(address, attribute, value) -> true, or nil and
// a reason. value is an expression (one "Default" entry, none for "") or a
// table of name = expression, as upstream's setter takes either.
int l_stats_roll_set(lua_State* L) {
    auto const* object = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const char* attribute = luaL_checkstring(L, 2);
    std::vector<std::string> names;
    std::vector<std::string> texts;
    if (lua_istable(L, 3)) {
        lua_pushnil(L);
        while (lua_next(L, 3) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isstring(L, -1)) {
                names.emplace_back(lua_tostring(L, -2));
                texts.emplace_back(lua_tostring(L, -1));
            }
            lua_pop(L, 1);
        }
    } else {
        const char* text = luaL_checkstring(L, 3);
        if (*text != '\0') {
            names.emplace_back("Default");
            texts.emplace_back(text);
        }
    }
    std::vector<char const*> namePtrs;
    std::vector<char const*> textPtrs;
    for (std::size_t i = 0; i < names.size(); ++i) {
        namePtrs.push_back(names[i].c_str());
        textPtrs.push_back(texts[i].c_str());
    }
    const int status = bg3le_stats_roll_set(object, attribute, namePtrs.data(),
                                            textPtrs.data(), names.size());
    if (status == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    static const char* const why[] = {
        "", "the roll condition entry could not be added to the stat",
        "the condition could not be added to the condition pool",
        "the roll condition array could not be written"};
    lua_pushnil(L);
    lua_pushstring(L, why[status >= 1 && status <= 3 ? status : 3]);
    return 2;
}

// Ext._Internal.StatsFloatIntern(value) -> pool index, or nil
int l_stats_float_intern(lua_State* L) {
    const int index = bg3le_stats_float_intern(
        static_cast<float>(luaL_checknumber(L, 1)));
    if (index < 0) return 0;
    lua_pushinteger(L, index);
    return 1;
}

// Ext._Internal.StatsInt64Intern(mask) -> pool index, or nil
int l_stats_int64_intern(lua_State* L) {
    const int index = bg3le_stats_int64_intern(
        static_cast<std::int64_t>(luaL_checkinteger(L, 1)));
    if (index < 0) return 0;
    lua_pushinteger(L, index);
    return 1;
}

// Ext._Internal.StringKeyFind(key) -> the TranslatedString's address, or nil
extern "C" void* bg3le_string_key_find(char const* key);
int l_string_key_find(lua_State* L) {
    void* value = bg3le_string_key_find(luaL_checkstring(L, 1));
    if (value == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)value);
    return 1;
}

// Ext._Internal.StringKeys() -> {key = address}, or nil without the manager
extern "C" std::size_t bg3le_string_keys(void (*each)(void*, char const*, void*),
                                         void* context);
int l_string_keys(lua_State* L) {
    lua_newtable(L);
    const std::size_t n = bg3le_string_keys(
        [](void* context, char const* key, void* value) {
            auto* L = static_cast<lua_State*>(context);
            lua_pushinteger(L, (lua_Integer)(std::uintptr_t)value);
            lua_setfield(L, -2, key);
        },
        L);
    if (n == 0) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

// Ext._Internal.StringKeySet(key, handle) -> boolean
extern "C" bool bg3le_string_key_set(char const* key, char const* handle);
int l_string_key_set(lua_State* L) {
    lua_pushboolean(L, bg3le_string_key_set(luaL_checkstring(L, 1),
                                            luaL_checkstring(L, 2)));
    return 1;
}

// Ext._Internal.TypeInfoAt(name) -> the registry's TypeInformation address, or nil
extern "C" void const* bg3le_type_info(char const* name);
int l_type_info_at(lua_State* L) {
    void const* at = bg3le_type_info(luaL_checkstring(L, 1));
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.TypeInfoRef(address) -> a TypeInformationRef's target, or nil
extern "C" void const* bg3le_type_info_ref(void const* ref);
int l_type_info_ref(lua_State* L) {
    void const* at = bg3le_type_info_ref((void const*)(std::uintptr_t)luaL_checkinteger(L, 1));
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

struct Bg3leTypeInfo {
    char const* TypeName;
    char const* NativeName;
    char const* Kind;
    void const* KeyType;
    void const* ElementType;
    void const* ParentType;
    char const* ModuleRole;
    char const* ComponentName;
    char const* SystemName;
    bool HasWildcardProperties;
    bool VarargParams;
    bool VarargsReturn;
    bool IsBitfield;
    bool IsBuiltin;
};
extern "C" bool bg3le_type_info_fields(void const* at, Bg3leTypeInfo* out);
extern "C" void bg3le_type_info_each(void const* at, int which,
                                     void (*each)(void*, char const*, void const*, unsigned long long),
                                     void* user);
extern "C" void bg3le_type_info_names(void (*each)(void*, char const*), void* user);

// Ext._Internal.TypeInfoFields(address) -> the scalars, refs as addresses
int l_type_info_fields(lua_State* L) {
    Bg3leTypeInfo f{};
    if (!bg3le_type_info_fields((void const*)(std::uintptr_t)luaL_checkinteger(L, 1), &f)) return 0;
    lua_newtable(L);
    // An empty FixedString is "" upstream, not nil.
    auto text = [L](char const* key, char const* value) {
        lua_pushstring(L, value != nullptr ? value : "");
        lua_setfield(L, -2, key);
    };
    auto ref = [L](char const* key, void const* value) {
        if (value == nullptr) return;
        lua_pushinteger(L, (lua_Integer)(std::uintptr_t)value);
        lua_setfield(L, -2, key);
    };
    auto flag = [L](char const* key, bool value) {
        lua_pushboolean(L, value);
        lua_setfield(L, -2, key);
    };
    text("TypeName", f.TypeName);
    text("NativeName", f.NativeName);
    text("Kind", f.Kind);
    ref("KeyType", f.KeyType);
    ref("ElementType", f.ElementType);
    ref("ParentType", f.ParentType);
    text("ModuleRole", f.ModuleRole);
    text("ComponentName", f.ComponentName);
    text("SystemName", f.SystemName);
    flag("HasWildcardProperties", f.HasWildcardProperties);
    flag("VarargParams", f.VarargParams);
    flag("VarargsReturn", f.VarargsReturn);
    flag("IsBitfield", f.IsBitfield);
    flag("IsBuiltin", f.IsBuiltin);
    return 1;
}

// Ext._Internal.TypeInfoEach(address, which) -> {key = address} for Members
// (0) and Methods (1), {label = value} for EnumValues (2), {address, ...}
// for Params (3) and ReturnValues (4)
int l_type_info_each(lua_State* L) {
    auto const* at = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const int which = (int)luaL_checkinteger(L, 2);
    lua_newtable(L);
    bg3le_type_info_each(
        at, which,
        [](void* user, char const* key, void const* type, unsigned long long value) {
            auto* S = static_cast<lua_State*>(user);
            if (key == nullptr) {
                lua_pushinteger(S, (lua_Integer)(std::uintptr_t)type);
                lua_rawseti(S, -2, luaL_len(S, -2) + 1);
                return;
            }
            if (type != nullptr) {
                lua_pushinteger(S, (lua_Integer)(std::uintptr_t)type);
            } else {
                lua_pushinteger(S, (lua_Integer)value);
            }
            lua_setfield(S, -2, key);
        },
        L);
    return 1;
}

// Ext._Internal.TypeInfoNames() -> every registered type name
int l_type_info_names(lua_State* L) {
    lua_newtable(L);
    bg3le_type_info_names(
        [](void* user, char const* name) {
            auto* S = static_cast<lua_State*>(user);
            if (name == nullptr) return;
            lua_pushstring(S, name);
            lua_rawseti(S, -2, luaL_len(S, -2) + 1);
        },
        L);
    return 1;
}

// Ext._Internal.StatsManagerAddress() -> RPGStats, or nil
extern "C" void* bg3le_rpgstats();
int l_stats_manager_address(lua_State* L) {
    void* at = bg3le_rpgstats();
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}

// Ext._Internal.StatsExtraGet(name) -> RPGStats::ExtraData's value, or nil
extern "C" bool bg3le_stats_extra_get(char const* name, float* out);
int l_stats_extra_get(lua_State* L) {
    float value = 0;
    if (!lua_isstring(L, 1) || !bg3le_stats_extra_get(lua_tostring(L, 1), &value)) return 0;
    lua_pushnumber(L, value);
    return 1;
}

// Ext._Internal.StatsExtraSet(name, value) -> bool
extern "C" bool bg3le_stats_extra_set(char const* name, float value);
int l_stats_extra_set(lua_State* L) {
    lua_pushboolean(L, bg3le_stats_extra_set(luaL_checkstring(L, 1), (float)luaL_checknumber(L, 2)));
    return 1;
}

// Ext._Internal.StatsExtraAll() -> {name = value}
extern "C" void bg3le_stats_extra_each(void (*each)(void*, char const*, float), void* user);
int l_stats_extra_all(lua_State* L) {
    lua_newtable(L);
    bg3le_stats_extra_each(
        [](void* user, char const* name, float value) {
            auto* S = static_cast<lua_State*>(user);
            if (name == nullptr) return;
            lua_pushnumber(S, value);
            lua_setfield(S, -2, name);
        },
        L);
    return 1;
}

// Ext._Internal.PathOverrideAdd(path, override) -> bool;
// PathOverrideGet(path) -> the absolute override, or nil
extern "C" bool bg3le_path_override_add(char const* path, char const* overridePath);
extern "C" char const* bg3le_path_override_get(char const* path);
int l_path_override_add(lua_State* L) {
    lua_pushboolean(L, bg3le_path_override_add(luaL_checkstring(L, 1), luaL_checkstring(L, 2)));
    return 1;
}
int l_path_override_get(lua_State* L) {
    char const* to = bg3le_path_override_get(luaL_checkstring(L, 1));
    if (to == nullptr) return 0;
    lua_pushstring(L, to);
    return 1;
}

// Ext._Internal.BuiltinFile(path) -> the builtin script's text, or nil
extern "C" char const* bg3le_builtin_lua(char const* path, std::size_t* size);
int l_builtin_file(lua_State* L) {
    std::size_t size = 0;
    char const* text = bg3le_builtin_lua(luaL_checkstring(L, 1), &size);
    if (text == nullptr) return 0;
    lua_pushlstring(L, text, size);
    return 1;
}

// Ext._Internal.SettingsFlag(key, default) -> ScriptExtenderSettings.json's
extern "C" bool bg3le_settings_flag(char const* key, bool fallback);
int l_settings_flag(lua_State* L) {
    lua_pushboolean(L, bg3le_settings_flag(luaL_checkstring(L, 1),
                                           lua_toboolean(L, 2) != 0));
    return 1;
}

// Ext._Internal.EngineTime(client) -> Time, DeltaTime, Ticks, Unknown of that
// side's last engine update; false before the first, nil if it is not hooked.
extern "C" int bg3le_engine_time(bool client, double* time, float* delta, int* ticks,
                                 double* unknown);
int l_engine_time(lua_State* L) {
    double time = 0.0, unknown = 0.0;
    float delta = 0.0f;
    int ticks = 0;
    switch (bg3le_engine_time(lua_toboolean(L, 1) != 0, &time, &delta, &ticks, &unknown)) {
    case 0: lua_pushnil(L); return 1;
    case 1: lua_pushboolean(L, 0); return 1;
    }
    lua_pushnumber(L, time);
    lua_pushnumber(L, delta);
    lua_pushinteger(L, ticks);
    lua_pushnumber(L, unknown);
    return 4;
}

// Ext._Internal.StatsCreate(name, modifierList) -> address, or nil and why
extern "C" void* bg3le_stats_create(char const* name, char const* listName, char const** err);
int l_stats_create(lua_State* L) {
    char const* err = nullptr;
    void* object = bg3le_stats_create(luaL_checkstring(L, 1), luaL_checkstring(L, 2), &err);
    if (object == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, err != nullptr ? err : "stat creation failed");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)object);
    return 1;
}

// Ext._Internal.StatsEnumAdd(type, label) -> value, or nil and why
extern "C" bool bg3le_stats_enum_add(char const* typeName, char const* label, int* valueOut,
                                     char const** err);
int l_stats_enum_add(lua_State* L) {
    int value = 0;
    char const* err = nullptr;
    if (!bg3le_stats_enum_add(luaL_checkstring(L, 1), luaL_checkstring(L, 2), &value, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err != nullptr ? err : "failed");
        return 2;
    }
    lua_pushinteger(L, value);
    return 1;
}

// Ext._Internal.StatsAttrAdd(list, name, type) -> true, or false and why
extern "C" bool bg3le_stats_attr_add(char const* listName, char const* modifierName,
                                     char const* typeName, char const** err);
int l_stats_attr_add(lua_State* L) {
    char const* err = nullptr;
    const bool ok = bg3le_stats_attr_add(luaL_checkstring(L, 1), luaL_checkstring(L, 2),
                                         luaL_checkstring(L, 3), &err);
    lua_pushboolean(L, ok);
    if (ok) return 1;
    lua_pushstring(L, err != nullptr ? err : "failed");
    return 2;
}

// Ext._Internal.EntityCreate() -> handle, or nil and why
extern "C" std::uint64_t bg3le_entity_create(void* container);
extern "C" bool bg3le_entity_destroy(void* container, std::uint64_t handle);
extern "C" int bg3le_engine_thread_index();
int l_entity_create(lua_State* L) {
    if (bg3le_engine_thread_index() < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "this thread has no engine thread index, so no command buffer of its own");
        return 2;
    }
    const std::uint64_t handle = bg3le_entity_create(world_container());
    if (handle == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "the entity world is not available");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)handle);
    return 1;
}

// Ext._Internal.EntityDestroy(entity) -> boolean
int l_entity_destroy(lua_State* L) {
    std::uint64_t handle = 0;
    if (!entity_proxy_handle(L, 1, &handle)) {
        return luaL_error(L, "Ext.Entity.Destroy expects an entity");
    }
    lua_pushboolean(L, bg3le_entity_destroy(world_container(), handle));
    return 1;
}

// Ext._Internal.TraceSetup(ecb, immediate, replication, modifications, {index...})
extern "C" bool bg3le_trace_setup(void* container, bool ecb, bool immediate, bool replication,
                                  bool modifications, std::uint16_t const* exclude,
                                  std::size_t excludeCount);
extern "C" bool bg3le_trace_enable(void* container, bool enable);
extern "C" void* bg3le_trace_get(void* container);
extern "C" void bg3le_trace_clear(void* container);
int l_trace_setup(lua_State* L) {
    std::vector<std::uint16_t> exclude;
    if (lua_istable(L, 5)) {
        const lua_Integer n = luaL_len(L, 5);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, 5, i);
            if (lua_isinteger(L, -1)) exclude.push_back((std::uint16_t)lua_tointeger(L, -1));
            lua_pop(L, 1);
        }
    }
    lua_pushboolean(L, bg3le_trace_setup(world_container(), lua_toboolean(L, 1), lua_toboolean(L, 2),
                                          lua_toboolean(L, 3), lua_toboolean(L, 4), exclude.data(),
                                          exclude.size()));
    return 1;
}
int l_trace_enable(lua_State* L) {
    lua_pushboolean(L, bg3le_trace_enable(world_container(), lua_toboolean(L, 1)));
    return 1;
}
int l_trace_get(lua_State* L) {
    void* at = bg3le_trace_get(world_container());
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}
int l_trace_clear(lua_State* L) {
    bg3le_trace_clear(world_container());
    return 0;
}

// Ext._Internal.StatsSetFunctors(address, attribute, text) -> true, or nil and why
extern "C" bool bg3le_stats_set_functors(void* object, char const* attribute, char const* value,
                                         char const** why);
int l_stats_set_functors(lua_State* L) {
    char const* why = nullptr;
    if (!bg3le_stats_set_functors((void*)(std::uintptr_t)luaL_checkinteger(L, 1),
                                  luaL_checkstring(L, 2), luaL_checkstring(L, 3), &why)) {
        lua_pushnil(L);
        lua_pushstring(L, why != nullptr ? why : "failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.StatsComboGet(address, which) -> {names}; which 0 is
// ComboProperties, 1 ComboCategories
extern "C" int bg3le_stats_combo_get(void const* object, int which,
                                     void (*each)(void*, char const*), void* user);
int l_stats_combo_get(lua_State* L) {
    auto const* object = (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    lua_newtable(L);
    bg3le_stats_combo_get(object, (int)luaL_checkinteger(L, 2),
                          [](void* user, char const* name) {
                              auto* S = static_cast<lua_State*>(user);
                              lua_pushstring(S, name != nullptr ? name : "");
                              lua_rawseti(S, -2, luaL_len(S, -2) + 1);
                          },
                          L);
    return 1;
}

// Ext._Internal.StatsComboSet(address, which, {names}) -> bool
extern "C" bool bg3le_stats_combo_set(void* object, int which, char const* const* names, int count);
int l_stats_combo_set(lua_State* L) {
    auto* object = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const int which = (int)luaL_checkinteger(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);
    push_packed(L, 3);
    lua_replace(L, 3);
    const lua_Integer n = luaL_len(L, 3);
    std::vector<char const*> names;
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        names.push_back(luaL_checkstring(L, -1));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, bg3le_stats_combo_set(object, which, names.data(), (int)names.size()));
    return 1;
}

// Ext._Internal.StatsSplitGroups(text) -> {TextKey = text}, or nil
extern "C" bool bg3le_stats_split_groups(char const* value,
                                         void (*each)(void*, char const*, char const*, std::size_t),
                                         void* user);
int l_stats_split_groups(lua_State* L) {
    char const* text = luaL_checkstring(L, 1);
    lua_newtable(L);
    const bool ok = bg3le_stats_split_groups(
        text,
        [](void* user, char const* key, char const* group, std::size_t size) {
            auto* S = static_cast<lua_State*>(user);
            lua_pushlstring(S, group, size);
            lua_setfield(S, -2, key != nullptr ? key : "");
        },
        L);
    if (!ok) lua_pushnil(L);
    return 1;
}

// Ext._Internal.StatSync(name) -> true, or nil and why
extern "C" char const* bg3le_stats_sync(char const* name);
int l_stat_sync(lua_State* L) {
    char const* err = bg3le_stats_sync(luaL_checkstring(L, 1));
    if (err != nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, err);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_stats_find(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* obj = bg3le_stats_find(name);
    if (obj == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no stat named %s (the manager holds %d)", name,
                        (int)bg3le_stats_count());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)obj);
    return 1;
}

// Ext._Internal.StatsType(address) -> modifier list name
int l_stats_type(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* type = bg3le_stats_type(obj);
    if (type == nullptr) return 0;
    lua_pushstring(L, type);
    return 1;
}

// Ext._Internal.StatsUsing(address) -> parent stat name
int l_stats_using(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* name = bg3le_stats_using(obj);
    if (name == nullptr) return 0;
    lua_pushstring(L, name);
    return 1;
}

// Ext._Internal.StatsListIndex(address) -> index
int l_stats_list_index(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, bg3le_stats_list_index(obj));
    return 1;
}

// Ext._Internal.StatsAttrCount(address) -> n
int l_stats_attr_count(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)bg3le_stats_attr_count(obj));
    return 1;
}

// Ext._Internal.StatsAttrAt(address, index) -> name, typeName, kind, raw
int l_stats_attr_at(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const auto i = (std::size_t)luaL_checkinteger(L, 2);

    char const* name = nullptr;
    char const* typeName = nullptr;
    int kind = 0;
    int raw = 0;
    if (!bg3le_stats_attr_at(obj, i, &name, &typeName, &kind, &raw)) {
        return 0;
    }

    if (name != nullptr) lua_pushstring(L, name);
    else lua_pushnil(L);
    if (typeName != nullptr) lua_pushstring(L, typeName);
    else lua_pushnil(L);
    lua_pushinteger(L, kind);
    lua_pushinteger(L, raw);
    return 4;
}

// Ext._Internal.StatsAttrLabel(address, index, raw) -> label
int l_stats_attr_label(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const auto i = (std::size_t)luaL_checkinteger(L, 2);
    const int raw = (int)luaL_checkinteger(L, 3);
    char const* label = bg3le_stats_attr_label(obj, i, raw);
    if (label == nullptr) return 0;
    lua_pushstring(L, label);
    return 1;
}

// Ext._Internal.StatsAttrString(raw) -> text
int l_stats_attr_string(lua_State* L) {
    const int raw = (int)luaL_checkinteger(L, 1);
    char const* text = bg3le_stats_attr_string(raw);
    if (text == nullptr) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.StatsAttrFloat(raw) -> number
int l_stats_attr_float(lua_State* L) {
    const int raw = (int)luaL_checkinteger(L, 1);
    double v = 0.0;
    if (!bg3le_stats_attr_float(raw, &v)) return 0;
    lua_pushnumber(L, v);
    return 1;
}

// Ext._Internal.StatsAttrFlags(address, index, raw) -> "A;B;C"
int l_stats_attr_flags(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const auto i = (std::size_t)luaL_checkinteger(L, 2);
    const int raw = (int)luaL_checkinteger(L, 3);
    char text[1024];
    if (!bg3le_stats_attr_flags(obj, i, raw, text, sizeof(text))) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.StatsAttrGuid(raw) -> guid string
int l_stats_attr_guid(lua_State* L) {
    const int raw = (int)luaL_checkinteger(L, 1);
    char text[40];
    if (!bg3le_stats_attr_guid(raw, text, sizeof(text))) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.ResourceBanks() -> { Manager, Banks = { {...}, ... } }
//
// Every bank the resource manager holds, with the name the symbol table gives
// its type index. That naming is the check on the search: the manager is found
// by looking for a table whose keys are all static data indices bg3le already
// knows, so if the names come back as real resource types rather than as gaps,
// the structure found is the right one.
int l_resource_banks(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)bg3le_resource_manager());
    lua_setfield(L, -2, "Manager");
    lua_pushinteger(L, (lua_Integer)ecs::count(ecs::Context::ImmutableData));
    lua_setfield(L, -2, "RegisteredTypes");

    const std::size_t count = bg3le_resource_bank_count();
    lua_createtable(L, (int)count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t typeIndex = 0;
        void* bank = nullptr;
        if (!bg3le_resource_bank_at(i, &typeIndex, &bank)) break;

        lua_newtable(L);
        lua_pushinteger(L, typeIndex);
        lua_setfield(L, -2, "TypeIndex");
        const auto name = ecs::name_of(ecs::Context::ImmutableData, typeIndex);
        if (name.has_value()) {
            lua_pushstring(L, name->c_str());
        } else {
            lua_pushstring(L, "<not in the registry>");
        }
        lua_setfield(L, -2, "Name");
        lua_pushinteger(L, (lua_Integer)(std::uintptr_t)bank);
        lua_setfield(L, -2, "Bank");
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "Banks");
    return 1;
}

// Ext._Internal.VariantIndex(handle, component, path) -> active, count
//
// active is zero-based, and equals count when the variant holds nothing.
// std::variant only reaches that state if a move threw, so it should not
// happen -- but it is representable, so it is reported rather than conflated
// with holding alternative zero.
int l_variant_index(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    std::size_t active = 0;
    std::size_t count = 0;
    if (!bg3le_meta_variant_index(meta, path, component, &active, &count)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not a variant", name, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)active);
    lua_pushinteger(L, (lua_Integer)count);
    return 2;
}

// Ext._Internal.MapKey(handle, component, path, index) -> key, kind
//
// index is zero-based, matching the slot the value at the same index occupies,
// so walking the slots pairs keys with values.
int l_map_key(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);
    const auto index = (std::size_t)luaL_checkinteger(L, 4);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    if (!bg3le_meta_map_key(meta, path, component, index, &address, &kind,
                            &size)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s has no key at slot %d", name, path,
                        (int)index);
        return 2;
    }

    if (!push_map_key(L, meta, path, address, (FieldKind)kind)) {
        lua_pushnil(L);
        lua_pushfstring(L,
            "the keys of %s.%s are of an unsupported kind (%s); the values are "
            "still reachable by slot", name, path,
            field_kind_name((FieldKind)kind));
        return 2;
    }
    return 2;
}

extern "C" int bg3le_component_engine_size(void* container,
                                           std::uint16_t componentIndex);
extern "C" void const* bg3le_meta_class_at(std::size_t index);
extern "C" char const* bg3le_meta_class_name(void const* handle);
extern "C" std::size_t bg3le_meta_class_count();

// Ext._Internal.SizeAudit() -> { Checked, Matched, Mismatches = {...} }
//
// Compares every component's declared struct size against the size the engine
// recorded for it. A mismatch means every read of that component is
// misaligned, because the size is the stride GetComponent multiplies the
// entity's slot by -- so the entity at index 0 reads correctly and the rest do
// not. That is worth knowing across all of them at once rather than one
// surprising result at a time.
//
// Components no live entity carries are skipped: the engine only records a
// size where a storage holds one.
int l_size_audit(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);  // the mismatch list

    std::size_t checked = 0;
    std::size_t matched = 0;
    std::size_t mismatches = 0;

    const std::size_t classes = bg3le_meta_class_count();
    for (std::size_t i = 0; i < classes; ++i) {
        void const* cls = bg3le_meta_class_at(i);
        if (cls == nullptr) continue;
        const char* engineName = bg3le_meta_engine_class(cls);
        if (engineName == nullptr) continue;

        const auto index = component_index(engineName);
        if (!index.has_value()) continue;

        const int engineSize = bg3le_component_engine_size(
            server_container(), static_cast<std::uint16_t>(*index));
        if (engineSize < 0) continue;  // no live entity has it

        ++checked;
        const auto declared = (int)bg3le_meta_component_stride(cls);
        if (declared == engineSize) {
            ++matched;
            continue;
        }

        ++mismatches;
        lua_newtable(L);
        lua_pushstring(L, engineName);
        lua_setfield(L, -2, "Component");
        lua_pushinteger(L, declared);
        lua_setfield(L, -2, "Declared");
        lua_pushinteger(L, engineSize);
        lua_setfield(L, -2, "Engine");
        lua_rawseti(L, -2, (int)mismatches);
    }

    lua_setfield(L, -2, "Mismatches");
    lua_pushinteger(L, (lua_Integer)checked);
    lua_setfield(L, -2, "Checked");
    lua_pushinteger(L, (lua_Integer)matched);
    lua_setfield(L, -2, "Matched");
    return 1;
}

// Ext._Internal.FieldAddress(handle, component, path) -> address, size
//
// For probing a field whose layout is in doubt. A container's length and
// buffer are read through the engine's own accessors, which is right only if
// bg3se's idea of the container's shape matches the engine's -- and a set the
// engine populated is the only thing that can settle that. Pair this with
// Peek, or use FieldBytes.
int l_field_address(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                            &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)address);
    lua_pushinteger(L, size);
    return 2;
}

// Ext._Internal.ObjectFieldAddress(address, class, path)
//   -> address, size
//
// The same as FieldAddress, for an object reached by address rather than a
// component reached by entity handle. What settles a layout that is in
// doubt: `StatsExpressionPooled.Params` reads as one element holding a
// number where upstream reports two and a string, and deciding between a
// wrong field offset and a wrong variant stride needs the bytes rather than
// another reading of the declaration.
int l_object_field_address(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(subject.Meta, path, subject.Base, &address, &kind,
                            &size, &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", className, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)address);
    lua_pushinteger(L, size);
    return 2;
}

// Ext._Internal.FieldBytes(handle, component, path [, count])
//
// The field's raw bytes as hex, grouped in eights, so a struct's shape can be
// read off directly. Defaults to the field's own size.
int l_field_bytes(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    // An empty path means the component itself, which is how to see every
    // field's bytes at once -- the only way to check a container's shape
    // against what bg3se believes it to be.
    void* address = component;
    std::uint8_t kind = 0;
    std::uint16_t size = (std::uint16_t)bg3le_meta_component_size(meta);
    bool readOnly = false;
    if (path[0] != '\0'
        && !bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                               &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }

    const auto want = (std::size_t)luaL_optinteger(L, 4, size);
    const std::size_t count = want > 256 ? 256 : want;

    std::string out;
    for (std::size_t i = 0; i < count; i += 8) {
        std::uint64_t word = 0;
        const std::size_t n = (count - i) < 8 ? (count - i) : 8;
        char line[64];
        if (!safe_read((const char*)address + i, &word, n)) {
            std::snprintf(line, sizeof(line), "+%02zx unreadable\n", i);
        } else {
            std::snprintf(line, sizeof(line), "+%02zx %016llx\n", i,
                          (unsigned long long)word);
        }
        out += line;
    }

    lua_pushstring(L, out.c_str());
    return 1;
}

// Ext._Internal.ComponentFields(name [, path]) ->{ field = kind, ... }, size
//
// path names a nested struct, so an inner struct lists the same way a
// component does.
int l_component_fields(lua_State* L) {
    const char* engineName = luaL_checkstring(L, 1);
    const char* path = luaL_optstring(L, 2, nullptr);
    void const* meta = bg3le_meta_component(engineName);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for %s", engineName);
        return 2;
    }

    constexpr std::size_t kMax = 512;
    const char* names[kMax];
    std::uint8_t kinds[kMax];
    const std::size_t n = bg3le_meta_fields_at(meta, path, names, kinds, kMax);
    if (n == 0 && path != nullptr && path[0] != '\0'
        && !bg3le_meta_path_is_struct(meta, path)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s cannot be traversed", engineName, path);
        return 2;
    }

    lua_newtable(L);
    for (std::size_t i = 0; i < n; ++i) {
        lua_pushstring(L, field_kind_name((FieldKind)kinds[i]));
        lua_setfield(L, -2, names[i]);
    }
    attach_legacy_names(L, meta, path);
    lua_pushinteger(L, (lua_Integer)bg3le_meta_component_size(meta));
    return 2;
}

// Ext.Entity primitives. The Lua-visible object model is assembled in the
// prelude on top of these, so field access and assignment stay in one place.
int l_entity_get_health(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const auto index = component_index("eoc::HealthComponent");
    if (!index.has_value()) return 0;

    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    if (!bg3le_entity_health(world_container(), handle,
                             static_cast<std::uint16_t>(*index), &hp, &maxHp)) {
        return 0;
    }
    lua_pushinteger(L, hp);
    lua_pushinteger(L, maxHp);
    return 2;
}

int l_entity_set_health(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const auto hp = static_cast<std::int32_t>(luaL_checkinteger(L, 2));
    const bool setMax = lua_toboolean(L, 3) != 0;
    const auto index = component_index("eoc::HealthComponent");
    if (!index.has_value()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, bg3le_set_health(world_container(), handle,
                                        static_cast<std::uint16_t>(*index), hp,
                                        setMax));
    return 1;
}

int l_entity_mark_changed(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));
    const auto index = component_index(name);
    if (!index.has_value()) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "%s is not a registered component", name);
        return 2;
    }
    lua_pushboolean(L, bg3le_mark_component_changed(
        server_container(), handle, static_cast<std::uint16_t>(*index)));
    return 1;
}

// Marking a component changed only tells the server. Replication is a second,
// separate registry: a component has a ReplicatedTypeContext index alongside
// its ComponentTypeIdContext one, and setting that entity's flags in the
// matching pool is what sends the value to the client.
int l_entity_replicate(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));

    const auto index = ecs::index_of(ecs::Context::Replication, name);
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a replicated component", name);
        return 2;
    }

    // Whole-component replication unless flags and qword are given, as
    // upstream's Replicate and SetReplicationFlags.
    const auto flags = lua_isnoneornil(L, 3)
                           ? ~static_cast<std::uint64_t>(0)
                           : static_cast<std::uint64_t>(luaL_checkinteger(L, 3));
    const auto qword = static_cast<std::uint32_t>(luaL_optinteger(L, 4, 0));
    const auto status = bg3le_replicate_component(
        server_container(), handle, static_cast<std::uint16_t>(*index), qword,
        flags);
    if (status == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }

    static const char* const messages[] = {
        "",
        "the entity storage container has not been captured yet",
        "could not recover the entity world from the container",
        "this world has no replication buffers; only the server replicates",
        "the replication type index is outside the replication pool array",
        "could not add the entity to the replication pool",
        "the engine allocator is not installed, and this would have allocated",
    };
    static_assert(sizeof(messages) / sizeof(messages[0]) == 7,
                  "keep in step with bg3le_replicate_component status codes");
    lua_pushnil(L);
    lua_pushfstring(L, "could not replicate %s: %s", name,
                    (status >= 1 && status <= 5) ? messages[status] : "unknown error");
    return 2;
}

// Diagnostic for the world recovery: reports the derived pointers and whether
// the two independent cross-checks hold.
// Pushes one captured slot as a table. Both are reported, because which of
// them is the server is the whole question.
void push_world_probe(lua_State* L, void* container) {
    void* world = nullptr;
    void* replication = nullptr;
    std::int32_t poolCount = -1;
    bool storageMatches = false;
    bool queriesMatch = false;
    bg3le_world_probe(container, &world, &replication, &poolCount,
                      &storageMatches, &queriesMatch);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)container);
    lua_setfield(L, -2, "Container");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)world);
    lua_setfield(L, -2, "World");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)replication);
    lua_setfield(L, -2, "Replication");
    lua_pushinteger(L, poolCount);
    lua_setfield(L, -2, "ReplicationPools");
    lua_pushboolean(L, storageMatches);
    lua_setfield(L, -2, "StorageMatches");
    lua_pushboolean(L, queriesMatch);
    lua_setfield(L, -2, "QueriesMatch");
    lua_pushboolean(L, bg3le_container_is_server(container));
    lua_setfield(L, -2, "IsServer");
}

int l_world_probe(lua_State* L) {
    lua_newtable(L);
    push_world_probe(L, ecs::container());
    lua_setfield(L, -2, "Slot1");
    push_world_probe(L, ecs::container_alt());
    lua_setfield(L, -2, "Slot2");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)server_container());
    lua_setfield(L, -2, "ServerContainer");
    lua_pushinteger(L, (lua_Integer)ecs::count(ecs::Context::Replication));
    lua_setfield(L, -2, "ReplicatedTypes");
    lua_pushboolean(L, bg3le_game_allocator_ready());
    lua_setfield(L, -2, "AllocatorReady");
    return 1;
}

// Whether an entity carries a component at all, so the proxy can report a
// missing component as nil rather than as an error.
int l_entity_has_component(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));
    const auto index = component_index(name);
    if (!index.has_value()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::int32_t storageIndex = -1;
    void* storage = nullptr;
    void* component = nullptr;
    bg3le_entity_probe(world_container(), handle,
                       static_cast<std::uint16_t>(*index), &storageIndex,
                       &storage, &component);
    if (component == nullptr) {
        bool immediate = false;
        component = bg3le_entity_pending_component(
            world_container(), handle, static_cast<std::uint16_t>(*index), &immediate);
    }
    lua_pushboolean(L, component != nullptr);
    return 1;
}

extern "C" bool bg3le_entity_alive(void* container, std::uint64_t handle);
extern "C" std::size_t bg3le_entity_component_types(void* container,
                                                    std::uint64_t handle,
                                                    std::uint16_t* out,
                                                    std::size_t max);
extern "C" std::size_t bg3le_entity_changed_types(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t* out,
                                                  std::size_t max);
extern "C" bool bg3le_entity_was_changed(void* container, std::uint64_t handle,
                                         std::uint16_t componentIndex);
extern "C" bool bg3le_entity_replication_flags(void* container,
                                               std::uint64_t handle,
                                               std::uint16_t replicationTypeIndex,
                                               std::uint32_t qword,
                                               std::uint64_t* flags);

// Ext._Internal.NewEntityProxy(handle, metatable) -> the entity's userdata
int l_new_entity_proxy(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    auto* block = static_cast<std::uint64_t*>(lua_newuserdata(L, sizeof(handle)));
    *block = handle;
    lua_pushvalue(L, 2);
    lua_setmetatable(L, -2);
    return 1;
}

extern "C" bool bg3le_meta_is_vector(void const* handle, char const* path);

// Ext._Internal.IsHashSet(component or class, path) -> boolean
extern "C" bool bg3le_meta_is_set(void const* handle, char const* path);
int l_is_hash_set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) meta = bg3le_meta_class(name);
    lua_pushboolean(L, bg3le_meta_is_set(meta, luaL_checkstring(L, 2)));
    return 1;
}

// Ext._Internal.EnumKey(component or class, path, value) -> the value, with
// an enum label turned into its number, so a set compares either spelling.
int l_enum_key(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* path = luaL_checkstring(L, 2);
    lua_settop(L, 3);
    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) meta = bg3le_meta_class(name);
    if (meta != nullptr && lua_type(L, 3) == LUA_TSTRING) {
        const char* wanted = lua_tostring(L, 3);
        const char* label = nullptr;
        std::uint64_t value = 0;
        bool isBitmask = false;
        for (std::size_t i = 0;
             bg3le_meta_enum_label(meta, path, i, &label, &value, &isBitmask); ++i) {
            if (std::strcmp(label, wanted) == 0) {
                lua_pushinteger(L, (lua_Integer)value);
                return 1;
            }
        }
    }
    return 1;
}

// Ext._Internal.IsVector(class or component, path) -> whether the field is a
// glm vector or matrix, which reads as a plain table

int l_is_vector(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) meta = bg3le_meta_class(name);
    lua_pushboolean(L, bg3le_meta_is_vector(meta, luaL_checkstring(L, 2)));
    return 1;
}

// Ext._Internal.NewObjectProxy(metatable) -> a userdata behind that
// metatable, which is what upstream's object, array and map proxies are.
// Upstream's LightObjectProxyMetatable::ToString: "Type (pointer)", with
// MSVC's %p. The pointer is the engine object's, from a "p:<hex>" identity.
int object_proxy_tostring(lua_State* L) {
    char const* name = "userdata";
    unsigned long long at = (unsigned long long)(std::uintptr_t)lua_topointer(L, 1);
    if (lua_getmetatable(L, 1)) {
        if (lua_getfield(L, -1, "__name") == LUA_TSTRING) name = lua_tostring(L, -1);
        lua_pop(L, 1);
        if (lua_getfield(L, -1, "__bg3leIdentity") == LUA_TSTRING) {
            std::sscanf(lua_tostring(L, -1), "p:%llx", &at);
        }
        lua_pop(L, 1);
    }
    char text[256];
    std::snprintf(text, sizeof(text), "%s (%016llX)", name, at);
    lua_pushstring(L, text);
    return 1;
}

// getmetatable as Norbyte's Lua fork has it: one of bg3le's objects answers
// with its type name ("EntityProxy" for an entity), as upstream's C++
// objects do -- FocusCore's character test compares against that. Anything
// else, the stock behaviour.
int l_getmetatable(lua_State* L) {
    luaL_checkany(L, 1);
    if (!lua_getmetatable(L, 1)) {
        lua_pushnil(L);
        return 1;
    }
    if (lua_type(L, 1) == LUA_TUSERDATA) {
        const bool proxy = lua_getfield(L, -1, "__bg3leProxy") == LUA_TBOOLEAN
                           && lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (proxy && lua_getfield(L, -1, "__name") == LUA_TSTRING) return 1;
        if (proxy) lua_pop(L, 1);
    }
    luaL_getmetafield(L, 1, "__metatable");
    return 1;
}

int l_new_object_proxy(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Marks it for getmetatable, which names it as upstream's fork does.
    lua_pushboolean(L, 1);
    lua_setfield(L, 1, "__bg3leProxy");
    if (lua_getfield(L, 1, "__tostring") == LUA_TNIL) {
        lua_pushcfunction(L, object_proxy_tostring);
        lua_setfield(L, 1, "__tostring");
    }
    lua_pop(L, 1);
    lua_newuserdata(L, 1);
    lua_pushvalue(L, 1);
    lua_setmetatable(L, -2);
    return 1;
}

// Ext._Internal.EntityProxyHandle(value) -> handle, or nil for a non-entity
int l_entity_proxy_handle(lua_State* L) {
    std::uint64_t handle = 0;
    if (!entity_proxy_handle(L, 1, &handle)) return 0;
    lua_pushinteger(L, (lua_Integer)handle);
    return 1;
}

// Ext._Internal.EntityAlive(handle) -> whether the entity has a storage
int l_entity_alive(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, bg3le_entity_alive(world_container(), handle));
    return 1;
}

// Ext._Internal.EntityComponentNames(handle, changedOnly)
//   -> { engine name, ... }, { engine name = short name or false, ... }
int l_entity_component_names(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const bool changedOnly = lua_toboolean(L, 2) != 0;

    std::vector<std::uint16_t> types(512);
    auto fetch = changedOnly ? bg3le_entity_changed_types
                             : bg3le_entity_component_types;
    std::size_t n = fetch(world_container(), handle, types.data(), types.size());
    if (n > types.size()) {
        types.resize(n);
        n = fetch(world_container(), handle, types.data(), types.size());
    }

    lua_createtable(L, (int)n, 0);
    lua_createtable(L, 0, (int)n);
    int at = 0;
    for (std::size_t i = 0; i < n && i < types.size(); ++i) {
        // One-frame indices carry 0x8000; see component_index.
        const bool oneFrame = (types[i] & 0x8000) != 0;
        auto name = ecs::name_of(oneFrame ? ecs::Context::OneFrameComponent
                                          : ecs::Context::Component,
                                 types[i] & 0x7fff);
        if (!name) continue;
        lua_pushstring(L, name->c_str());
        lua_rawseti(L, -3, ++at);
        void const* meta = bg3le_meta_component(name->c_str());
        const char* shortName = meta ? bg3le_meta_short_name(meta) : nullptr;
        if (shortName != nullptr) lua_pushstring(L, shortName);
        else lua_pushboolean(L, 0);
        lua_setfield(L, -2, name->c_str());
    }
    return 2;
}

extern "C" std::size_t bg3le_registered_component_types(void* container,
                                                        std::uint16_t* out,
                                                        std::size_t max);

extern "C" void bg3le_component_callbacks_probe(void* container,
                                                std::uint16_t componentIndex);
extern "C" bool bg3le_component_events_watch(void* container,
                                             std::uint16_t componentIndex);
extern "C" std::size_t bg3le_component_events_take(void* world,
                                                   std::uint64_t* entities,
                                                   std::uint16_t* types,
                                                   std::uint8_t* kinds,
                                                   std::size_t max);

// Ext._Internal.WatchComponentEvents(component) -> bool
int l_watch_component_events(lua_State* L) {
    const auto index = component_index(engine_name_of(luaL_checkstring(L, 1)));
    lua_pushboolean(L, index && !(*index & 0x8000)
                           && bg3le_component_events_watch(
                               world_container(),
                               static_cast<std::uint16_t>(*index)));
    return 1;
}

extern "C" bool bg3le_system_hook(void* container, std::int32_t index, bool client);
extern "C" bool bg3le_system_probe(void* container, std::int32_t index, void** system,
                                   std::int32_t* ownIndex, void** update,
                                   std::uint32_t* count);

// bg3se's ExtSystemType labels and the engine class each names.
struct SystemName {
    char const* Label;
    char const* Engine;
};
constexpr SystemName kSystemNames[] = {
#include "vendor/system_names.inc"
};

// The engine class a bg3se system label names, or the name itself.
char const* system_class(char const* name) {
    for (auto const& known : kSystemNames) {
        if (std::strcmp(known.Label, name) == 0) return known.Engine;
    }
    return name;
}

// A system's index, by bg3se's label or the engine's own name.
std::optional<std::int32_t> system_index(char const* name) {
    char const* engine = name;
    for (auto const& known : kSystemNames) {
        if (std::strcmp(known.Label, name) == 0) {
            engine = known.Engine;
            break;
        }
    }
    const auto index = ecs::index_of(ecs::Context::System, engine);
    if (!index.has_value()) return std::nullopt;
    return (std::int32_t)*index;
}

// Ext._Internal.HookSystem(name) -> index, or nil and why
int l_hook_system(lua_State* L) {
    char const* name = luaL_checkstring(L, 1);
    const auto index = system_index(name);
    if (!index.has_value()
        || !bg3le_system_hook(world_container(), *index, in_client_state())) {
        lua_pushnil(L);
        lua_pushfstring(L, "System %s not registered", name);
        return 2;
    }
    lua_pushinteger(L, *index);
    return 1;
}

// Ext._Internal.ClassConstructible(name) -> boolean
extern "C" bool bg3le_meta_class_constructible(void const* handle);
int l_class_constructible(lua_State* L) {
    void const* meta = bg3le_meta_class(luaL_checkstring(L, 1));
    if (meta == nullptr) meta = bg3le_meta_component(luaL_checkstring(L, 1));
    lua_pushboolean(L, bg3le_meta_class_constructible(meta));
    return 1;
}

// Ext._Internal.DialogManager() -> address, or nil
//
// Upstream's GetDialogManager: esv::DialogSystem's GameInterface.DialogManager,
// server only. On this build the pointer is at +0x1e8 of the system (bg3se's
// declared layout puts it 16 bytes later), so it is checked before it is
// trusted: a DialogManager's FlagDescriptions (+0x30) maps each flag kind's
// name to a description that carries the same name.
int l_dialog_manager(lua_State* L) {
    if (in_client_state()) return 0;
    const auto index = system_index("esv::DialogSystem");
    void* system = nullptr;
    void* update = nullptr;
    std::int32_t own = -1;
    std::uint32_t count = 0;
    if (!index.has_value()
        || !bg3le_system_probe(world_container(), *index, &system, &own, &update, &count)
        || system == nullptr) {
        return 0;
    }
    char* manager = nullptr;
    if (!safe_read((char const*)system + 0x1e8, &manager, sizeof(manager)) || manager == nullptr) {
        return 0;
    }
    struct RefMap { std::uint32_t ItemCount, HashSize; void** HashTable; } flags{};
    if (!safe_read(manager + 0x30, &flags, sizeof(flags)) || flags.HashSize == 0
        || flags.HashSize > 4096 || flags.ItemCount == 0 || flags.HashTable == nullptr) {
        return 0;
    }
    std::uint32_t agreeing = 0;
    for (std::uint32_t b = 0; b < flags.HashSize; ++b) {
        char* node = nullptr;
        if (!safe_read(flags.HashTable + b, &node, sizeof(node))) return 0;
        for (int guard = 0; node != nullptr && guard < 64; ++guard) {
            std::uint32_t key = 0, name = 0;
            char* value = nullptr;
            if (!safe_read(node + 8, &key, sizeof(key))
                || !safe_read(node + 16, &value, sizeof(value)) || value == nullptr
                || !safe_read(value, &name, sizeof(name)) || name != key) {
                return 0;
            }
            ++agreeing;
            if (!safe_read(node, &node, sizeof(node))) return 0;
        }
    }
    if (agreeing != flags.ItemCount) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)manager);
    return 1;
}

// Ext._Internal.SystemProbe(name) -> { Index, System, OwnIndex, Update, Count }
int l_system_probe(lua_State* L) {
    const auto index = system_index(luaL_checkstring(L, 1));
    if (!index.has_value()) return 0;
    void* system = nullptr;
    void* update = nullptr;
    std::int32_t own = -1;
    std::uint32_t count = 0;
    const bool ok = bg3le_system_probe(world_container(), *index, &system, &own, &update, &count);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, *index);
    lua_setfield(L, -2, "Index");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)system);
    lua_setfield(L, -2, "System");
    lua_pushinteger(L, own);
    lua_setfield(L, -2, "OwnIndex");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)update);
    lua_setfield(L, -2, "Update");
    lua_pushinteger(L, count);
    lua_setfield(L, -2, "Count");
    lua_pushboolean(L, ok);
    lua_setfield(L, -2, "Ok");
    lua_pushstring(L, system_class(luaL_checkstring(L, 1)));
    lua_setfield(L, -2, "Class");
    return 1;
}

extern "C" void* bg3le_resource_bank_get(std::uint32_t type, std::uint32_t key);
extern "C" long bg3le_resource_bank_keys(std::uint32_t type, std::uint32_t* out,
                                         std::size_t max);
extern "C" bool bg3le_fixed_string_index_of(char const* wanted, std::uint32_t* out);
extern "C" char const* bg3le_fixed_string(std::uint32_t index, std::uint32_t* length);

// Ext._Internal.ResourceBankGet(type, id) -> address, or nil
int l_resource_bank_get(lua_State* L) {
    const auto type = (std::uint32_t)luaL_checkinteger(L, 1);
    std::uint32_t key = 0;
    void* found = nullptr;
    if (bg3le_fixed_string_index_of(luaL_checkstring(L, 2), &key)) {
        found = bg3le_resource_bank_get(type, key);
    }
    if (found == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)found);
    return 1;
}

// Ext._Internal.ResourceBankKeys(type) -> { id, ... }, or nil
int l_resource_bank_keys(lua_State* L) {
    const auto type = (std::uint32_t)luaL_checkinteger(L, 1);
    const long count = bg3le_resource_bank_keys(type, nullptr, 0);
    if (count < 0) return 0;
    std::vector<std::uint32_t> keys((std::size_t)count);
    bg3le_resource_bank_keys(type, keys.data(), keys.size());
    lua_createtable(L, (int)keys.size(), 0);
    int at = 0;
    for (std::uint32_t key : keys) {
        std::uint32_t length = 0;
        char const* text = bg3le_fixed_string(key, &length);
        if (text == nullptr) continue;
        lua_pushlstring(L, text, length);
        lua_rawseti(L, -2, ++at);
    }
    return 1;
}

extern "C" void* bg3le_boost_prototype(char const* guid);

// Ext._Internal.BoostPrototype(guid) -> address, or nil
int l_boost_prototype(lua_State* L) {
    void* found = bg3le_boost_prototype(luaL_checkstring(L, 1));
    if (found == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)found);
    return 1;
}

extern "C" void* bg3le_texture_atlas_map();
extern "C" void* bg3le_icon_atlas(char const* icon);
extern "C" void* bg3le_icon_uvs(char const* icon);

// Ext._Internal.TextureAtlasMap() / IconAtlas(icon) / IconUVs(icon) -> address
int push_address_or_nothing(lua_State* L, void* at) {
    if (at == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)at);
    return 1;
}
int l_texture_atlas_map(lua_State* L) {
    return push_address_or_nothing(L, bg3le_texture_atlas_map());
}
int l_icon_atlas(lua_State* L) {
    return push_address_or_nothing(L, bg3le_icon_atlas(luaL_checkstring(L, 1)));
}
int l_icon_uvs(lua_State* L) {
    return push_address_or_nothing(L, bg3le_icon_uvs(luaL_checkstring(L, 1)));
}

extern "C" long bg3le_resource_sources(std::int32_t typeIndex,
                                       void (*each)(void* context, void const* mod,
                                                    void const* guids, std::uint32_t count),
                                       void* context);
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out, std::size_t size);

// Ext._Internal.ResourceSources(type) -> { [modGuid] = { guid, ... } }, or nil
int l_resource_sources(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    void const* meta = bg3le_meta_class(className);
    const char* engineClass = meta != nullptr ? bg3le_meta_engine_class(meta) : nullptr;
    const auto typeIndex = engineClass != nullptr
        ? ecs::index_of(ecs::Context::ImmutableData, engineClass) : std::nullopt;
    if (!typeIndex.has_value()) return 0;

    lua_newtable(L);
    const long mods = bg3le_resource_sources(*typeIndex,
        [](void* context, void const* mod, void const* guids, std::uint32_t count) {
            auto* L = static_cast<lua_State*>(context);
            char text[64];
            if (!bg3le_meta_format_guid(mod, text, sizeof(text))) return;
            lua_createtable(L, (int)count, 0);
            for (std::uint32_t i = 0; i < count; ++i) {
                char id[64];
                if (!bg3le_meta_format_guid((char const*)guids + i * 16, id, sizeof(id))) continue;
                lua_pushstring(L, id);
                lua_rawseti(L, -2, (int)i + 1);
            }
            lua_setfield(L, -2, text);
        }, L);
    if (mods < 0) {
        lua_pop(L, 1);
        return 0;
    }
    return 1;
}

extern "C" char const* bg3le_meta_path_class(void const* handle, char const* path);

// Ext._Internal.PointeeClass(component or class, path, isClass) -> class name
int l_pointee_class(lua_State* L) {
    char const* owner = luaL_checkstring(L, 1);
    char const* path = luaL_checkstring(L, 2);
    void const* meta = lua_toboolean(L, 3) ? bg3le_meta_class(owner)
                                           : bg3le_meta_component(owner);
    char const* name = bg3le_meta_path_class(meta, path);
    if (name == nullptr) return 0;
    lua_pushstring(L, name);
    return 1;
}

// Ext._Internal.TakeComponentEvents()
//   -> { { handle, component short name, "create" | "destroy" }, ... }
int l_take_component_events(lua_State* L) {
    constexpr std::size_t kBatch = 4096;
    static std::uint64_t entities[kBatch];
    static std::uint16_t types[kBatch];
    static std::uint8_t kinds[kBatch];
    const std::size_t n = bg3le_component_events_take(
        bg3le_entity_world(world_container()), entities, types, kinds, kBatch);
    lua_createtable(L, (int)n, 0);
    int at = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto name = ecs::name_of(ecs::Context::Component, types[i]);
        if (!name) continue;
        void const* meta = bg3le_meta_component(name->c_str());
        const char* shortName = meta ? bg3le_meta_short_name(meta) : nullptr;
        lua_createtable(L, 3, 0);
        lua_pushinteger(L, (lua_Integer)entities[i]);
        lua_rawseti(L, -2, 1);
        lua_pushstring(L, shortName != nullptr ? shortName : name->c_str());
        lua_rawseti(L, -2, 2);
        lua_pushstring(L, kinds[i] == 1 ? "create" : "destroy");
        lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, ++at);
    }
    return 1;
}

extern "C" void bg3le_replication_watch(std::uint16_t replicationTypeIndex);
extern "C" std::size_t bg3le_replication_changes(void* container,
                                                 std::uint64_t* entities,
                                                 std::uint16_t* types,
                                                 std::uint64_t* fields,
                                                 std::size_t max);

// Ext._Internal.WatchReplication(component) -> whether it replicates
int l_watch_replication(lua_State* L) {
    const auto index = ecs::index_of(ecs::Context::Replication,
                                     engine_name_of(luaL_checkstring(L, 1)));
    if (index) bg3le_replication_watch(static_cast<std::uint16_t>(*index));
    lua_pushboolean(L, index.has_value());
    return 1;
}

// Ext._Internal.TakeReplicationChanges()
//   -> { { handle, component short name, fields }, ... }
int l_take_replication_changes(lua_State* L) {
    constexpr std::size_t kBatch = 4096;
    static std::uint64_t entities[kBatch];
    static std::uint16_t types[kBatch];
    static std::uint64_t fields[kBatch];
    const std::size_t n = bg3le_replication_changes(server_container(), entities,
                                                    types, fields, kBatch);
    lua_createtable(L, (int)n, 0);
    int at = 0;
    for (std::size_t i = 0; i < n; ++i) {
        auto name = ecs::name_of(ecs::Context::Replication, types[i]);
        if (!name) continue;
        void const* meta = bg3le_meta_component(name->c_str());
        const char* shortName = meta ? bg3le_meta_short_name(meta) : nullptr;
        lua_createtable(L, 3, 0);
        lua_pushinteger(L, (lua_Integer)entities[i]);
        lua_rawseti(L, -2, 1);
        lua_pushstring(L, shortName != nullptr ? shortName : name->c_str());
        lua_rawseti(L, -2, 2);
        lua_pushinteger(L, (lua_Integer)fields[i]);
        lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, ++at);
    }
    return 1;
}

// Ext._Internal.ComponentShortName(name) -> upstream's name for a component
int l_component_short_name(lua_State* L) {
    void const* meta = bg3le_meta_component(luaL_checkstring(L, 1));
    const char* shortName = meta ? bg3le_meta_short_name(meta) : nullptr;
    if (shortName == nullptr) return 0;
    lua_pushstring(L, shortName);
    return 1;
}

// Ext._Internal.ComponentCallbacksProbe(component) -- diagnostics only.
int l_component_callbacks_probe(lua_State* L) {
    const auto index = component_index(engine_name_of(luaL_checkstring(L, 1)));
    if (index) {
        bg3le_component_callbacks_probe(server_container(),
                                        static_cast<std::uint16_t>(*index));
    }
    return 0;
}

// Ext._Internal.RegisteredComponentTypes()
//   -> { { engine name, one-frame, short name or false }, ... }
int l_registered_component_types(lua_State* L) {
    std::vector<std::uint16_t> types(4096);
    std::size_t n = bg3le_registered_component_types(world_container(),
                                                     types.data(), types.size());
    if (n > types.size()) {
        types.resize(n);
        n = bg3le_registered_component_types(world_container(), types.data(),
                                             types.size());
    }

    lua_createtable(L, (int)n, 0);
    int at = 0;
    for (std::size_t i = 0; i < n && i < types.size(); ++i) {
        const bool oneFrame = (types[i] & 0x8000) != 0;
        auto name = ecs::name_of(oneFrame ? ecs::Context::OneFrameComponent
                                          : ecs::Context::Component,
                                 types[i] & 0x7fff);
        if (!name) continue;
        lua_createtable(L, 3, 0);
        lua_pushstring(L, name->c_str());
        lua_rawseti(L, -2, 1);
        lua_pushboolean(L, oneFrame);
        lua_rawseti(L, -2, 2);
        void const* meta = bg3le_meta_component(name->c_str());
        const char* shortName = meta ? bg3le_meta_short_name(meta) : nullptr;
        if (shortName != nullptr) lua_pushstring(L, shortName);
        else lua_pushboolean(L, 0);
        lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, ++at);
    }
    return 1;
}

// Ext._Internal.EntityWasChanged(handle, component) -> bool
int l_entity_was_changed(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const auto index = component_index(engine_name_of(luaL_checkstring(L, 2)));
    lua_pushboolean(L, index && bg3le_entity_was_changed(
                               server_container(), handle,
                               static_cast<std::uint16_t>(*index)));
    return 1;
}

// Ext._Internal.EntityReplicationFlags(handle, component, qword) -> flags
int l_entity_replication_flags(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));
    const auto qword = static_cast<std::uint32_t>(luaL_optinteger(L, 3, 0));
    std::uint64_t flags = 0;
    if (auto index = ecs::index_of(ecs::Context::Replication, name)) {
        bg3le_entity_replication_flags(server_container(), handle,
                                       static_cast<std::uint16_t>(*index),
                                       qword, &flags);
    }
    lua_pushinteger(L, (lua_Integer)flags);
    return 1;
}

extern "C" std::uint64_t bg3le_uuid_to_handle(void* container,
                                              std::uint16_t mappingIndex,
                                              char const* uuid);

// UUID string -> EntityHandle, via ls::uuid::ToHandleMappingComponent. This is
// what lets Osiris UUIDs, which is all a mod ever has, reach the ECS.
int l_uuid_to_handle(lua_State* L) {
    const char* uuid = luaL_checkstring(L, 1);
    const auto index = ecs::index_of(ecs::Context::Component,
                                     "ls::uuid::ToHandleMappingComponent");
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "ls::uuid::ToHandleMappingComponent has no index");
        return 2;
    }

    const std::uint64_t handle = bg3le_uuid_to_handle(
        world_container(), static_cast<std::uint16_t>(*index), uuid);
    if (handle == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "UUID not found in the mapping");
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(handle));
    return 1;
}

extern "C" std::uint64_t bg3le_find_entity_with(void* container,
                                                std::uint16_t componentIndex,
                                                std::uint32_t* storagesSeen);

// Reports each step of the walk, so a null says where it stopped rather than
// just that it did.
int l_entity_probe(lua_State* L) {
    const auto index = ecs::index_of(ecs::Context::Component, "eoc::HealthComponent");
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "eoc::HealthComponent has no index");
        return 2;
    }

    // Default to an entity that actually carries the component, rather than
    // whatever the engine happened to look up last.
    std::uint32_t seen = 0;
    auto handle = static_cast<std::uint64_t>(luaL_optinteger(L, 1, 0));
    bool found = false;
    if (handle == 0) {
        handle = bg3le_find_entity_with(world_container(),
                                        static_cast<std::uint16_t>(*index), &seen);
        found = true;
    }

    std::int32_t storageIndex = -1;
    void* storage = nullptr;
    void* component = nullptr;
    bg3le_entity_probe(world_container(), handle,
                       static_cast<std::uint16_t>(*index), &storageIndex,
                       &storage, &component);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)*index);
    lua_setfield(L, -2, "HealthIndex");
    lua_pushinteger(L, (lua_Integer)handle);
    lua_setfield(L, -2, "Handle");
    if (found) {
        lua_pushinteger(L, (lua_Integer)seen);
        lua_setfield(L, -2, "StoragesScanned");
    }
    lua_pushinteger(L, storageIndex);
    lua_setfield(L, -2, "StorageIndex");
    lua_pushinteger(L, (lua_Integer)reinterpret_cast<std::uintptr_t>(storage));
    lua_setfield(L, -2, "Storage");
    lua_pushinteger(L, (lua_Integer)reinterpret_cast<std::uintptr_t>(component));
    lua_setfield(L, -2, "Component");

    if (component != nullptr) {
        std::int32_t hp = 0;
        std::int32_t maxHp = 0;
        if (bg3le_entity_health(world_container(), handle,
                                static_cast<std::uint16_t>(*index), &hp, &maxHp)) {
            lua_pushinteger(L, hp);
            lua_setfield(L, -2, "Hp");
            lua_pushinteger(L, maxHp);
            lua_setfield(L, -2, "MaxHp");
        }
    }
    return 1;
}

// The most recent EntityHandle the engine looked up, so component access can
// be tested before UUID -> handle exists.
int l_last_entity(lua_State* L) {
    const unsigned long long h = ecs::last_entity();
    if (h == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(h));
    return 1;
}

// End to end: entity handle -> storage -> component -> field. Uses the
// component index bg3le reads from the symbol table and the container it
// captured, walked by bg3se's own implementation.
int l_entity_health(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(
        luaL_optinteger(L, 1, static_cast<lua_Integer>(ecs::last_entity())));
    if (handle == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no entity handle available yet");
        return 2;
    }

    const auto index = ecs::index_of(ecs::Context::Component, "eoc::HealthComponent");
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "eoc::HealthComponent has no index yet");
        return 2;
    }

    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    if (!bg3le_entity_health(world_container(), handle,
                             static_cast<std::uint16_t>(*index), &hp, &maxHp)) {
        lua_pushnil(L);
        lua_pushstring(L, "entity has no Health component");
        return 2;
    }

    lua_newtable(L);
    lua_pushinteger(L, hp);
    lua_setfield(L, -2, "Hp");
    lua_pushinteger(L, maxHp);
    lua_setfield(L, -2, "MaxHp");
    return 1;
}

int l_ecs_counts(lua_State* L) {
    lua_newtable(L);
    const std::pair<ecs::Context, const char*> contexts[] = {
        {ecs::Context::Component, "Component"},
        {ecs::Context::OneFrameComponent, "OneFrameComponent"},
        {ecs::Context::System, "System"},
        {ecs::Context::Replication, "Replication"},
        {ecs::Context::ImmutableData, "ImmutableData"},
        {ecs::Context::Unchecked, "Unchecked"},
    };
    for (const auto& [ctx, label] : contexts) {
        lua_pushinteger(L, (lua_Integer)ecs::count(ctx));
        lua_setfield(L, -2, label);
    }
    return 1;
}

void register_log(lua_State* L, const char* name, int severity) {
    lua_pushinteger(L, severity);
    lua_pushcclosure(L, l_log, 1);
    lua_setfield(L, -2, name);
}

}  // namespace

// A message for a context, from a peer (src/net.cpp); drained on its tick.
void lua_net_deliver(bool server, NetInbound message) {
    const std::lock_guard<std::mutex> lock(g_net_mutex);
    (server ? g_to_server : g_to_client).push_back(std::move(message));
}

bool lua_state_is_client(lua_State* L) { return L != nullptr && L == g_client_lua; }

void build_state(bool client);

// Upstream's SandboxStartup.lua, then what upstream never opens: io, os,
// package and utf8 (LuaBinding.cpp), with its BuiltinLibrary.lua require.
void run_sandbox() {
    std::size_t size = 0;
    char const* text = bg3le_builtin_lua("SandboxStartup.lua", &size);
    if (text == nullptr) {
        logf("lua: the builtin SandboxStartup.lua is missing");
    } else if ((luaL_loadbuffer(g_lua, text, size, "=builtin://SandboxStartup.lua")
                || lua_pcall(g_lua, 0, 0, 0)) != LUA_OK) {
        logf("lua: SandboxStartup.lua failed: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
    static const char kAfter[] = R"LUA(
io, os, package, utf8 = nil, nil, nil, nil
require = function(name)
  if string.sub(name, -4) == ".lua" then return Ext.Require(name) end
  return Ext.Require((string.gsub(name, "%.", "/")) .. ".lua")
end
)LUA";
    if ((luaL_loadbuffer(g_lua, kAfter, std::strlen(kAfter), "=bg3le sandbox")
         || lua_pcall(g_lua, 0, 0, 0)) != LUA_OK) {
        logf("lua: sandbox failed: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
}

void lua_init() {
    if (g_server_lua != nullptr) return;

    build_state(false);
    build_state(true);

    t_lua = nullptr;
    logf("lua: server and client contexts built");
}

// BG3LE_STRTAB_TRIPWIRE=1: the engine string table is checked after every C
// function Lua calls, and the first damage is logged with the call and stack.
extern "C" bool bg3le_strtab_check(char const* where);

bool strtab_tripwire_on() {
    static const bool on = std::getenv("BG3LE_STRTAB_TRIPWIRE") != nullptr;
    return on;
}

void strtab_tripwire_hook(lua_State* L, lua_Debug* ar) {
    if (ar->event != LUA_HOOKRET) return;
    if (lua_getinfo(L, "nS", ar) == 0 || ar->what == nullptr || std::strcmp(ar->what, "C") != 0) {
        return;
    }
    char where[160];
    std::snprintf(where, sizeof(where), "return from C function %s (%s)",
                  ar->name != nullptr ? ar->name : "?",
                  L == g_client_lua ? "client" : "server");
    if (bg3le_strtab_check(where)) {
        luaL_traceback(L, L, "strtab tripwire: Lua stack", 0);
        logf("%s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

void build_state(bool client) {
    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        logf("lua: luaL_newstate failed for the %s context",
             client ? "client" : "server");
        return;
    }
    if (strtab_tripwire_on()) {
        bg3le_strtab_check("state build");
        lua_sethook(L, strtab_tripwire_hook, LUA_MASKRET, 0);
    }

    // Recorded before the build, so anything the prelude asks about the
    // context during it gets the right answer.
    t_lua = L;
    if (client) {
        g_client_lua = L;
    } else {
        g_server_lua = L;
    }
    lua_setup_cppobjects(g_lua, &cpp_alloc, &cpp_free, &cpp_get_light_metatable,
                         &cpp_get_metatable, &cpp_finalize, &cpp_canonicalize);
    lua_setup_strcache(g_lua, &cache_string, &release_string);
    luaL_openlibs(g_lua);
    lua_pushcfunction(g_lua, l_getmetatable);
    lua_setglobal(g_lua, "getmetatable");
    // Ext.Log and Ext.Json are pure Lua/C and need no engine reflection, so
    // the helpers mods actually use every day can be compatible now. Shapes
    // and aliases follow BG3SE's BuiltinLibrary.lua.
    lua_newtable(g_lua);                       // Ext
    lua_newtable(g_lua);                       // Ext.Log
    register_log(g_lua, "Print", 0);
    register_log(g_lua, "PrintWarning", 1);
    register_log(g_lua, "PrintError", 2);
    lua_setfield(g_lua, -2, "Log");

    lua_newtable(g_lua);                       // Ext.Timer
    lua_pushcfunction(g_lua, l_monotonic_ms);
    lua_setfield(g_lua, -2, "MonotonicTime");
    lua_pushcfunction(g_lua, l_microsec);
    lua_setfield(g_lua, -2, "MicrosecTime");
    lua_pushcfunction(g_lua, l_clock_time);
    lua_setfield(g_lua, -2, "ClockTime");
    lua_setfield(g_lua, -2, "Timer");

    lua_newtable(g_lua);                       // Ext._Internal
    lua_pushcfunction(g_lua, l_module_base);
    lua_setfield(g_lua, -2, "ModuleBase");
    lua_pushcfunction(g_lua, l_peek);
    lua_setfield(g_lua, -2, "Peek");
    lua_pushcfunction(g_lua, l_peek_string);
    lua_setfield(g_lua, -2, "PeekString");
    lua_pushcfunction(g_lua, l_moveable_set);
    lua_setfield(g_lua, -2, "MoveableSet");
    lua_pushcfunction(g_lua, l_item_create_cache_template);
    lua_setfield(g_lua, -2, "ItemCreateCacheTemplate");
    lua_pushcfunction(g_lua, l_list_dir);
    lua_setfield(g_lua, -2, "ListDir");
    lua_pushcfunction(g_lua, l_extender_root);
    lua_setfield(g_lua, -2, "ExtenderRoot");
    lua_pushcfunction(g_lua, l_game_state);
    lua_setfield(g_lua, -2, "GameState");
    lua_pushcfunction(g_lua, l_saved_persistent_vars);
    lua_setfield(g_lua, -2, "SavedPersistentVars");
    lua_pushcfunction(g_lua, l_take_saved_extras);
    lua_setfield(g_lua, -2, "TakeSavedExtras");
    lua_pushcfunction(g_lua, l_take_saved_persistent_vars);
    lua_setfield(g_lua, -2, "TakeSavedPersistentVars");
    lua_pushcfunction(g_lua, l_component_index);
    lua_setfield(g_lua, -2, "ComponentIndex");
    lua_pushcfunction(g_lua, l_ecs_counts);
    lua_setfield(g_lua, -2, "EcsCounts");
    lua_pushcfunction(g_lua, l_symbol_addr);
    lua_setfield(g_lua, -2, "SymbolAddr");
    lua_pushcfunction(g_lua, l_ecs_storage);
    lua_setfield(g_lua, -2, "EcsStorage");
    lua_pushcfunction(g_lua, l_ecs_dump);
    lua_setfield(g_lua, -2, "EcsDump");
    lua_pushcfunction(g_lua, l_last_entity);
    lua_setfield(g_lua, -2, "LastEntity");
    lua_pushcfunction(g_lua, l_entity_health);
    lua_setfield(g_lua, -2, "EntityHealth");
    lua_pushcfunction(g_lua, l_entity_probe);
    lua_setfield(g_lua, -2, "EntityProbe");
    lua_pushcfunction(g_lua, l_uuid_to_handle);
    lua_setfield(g_lua, -2, "UuidToHandle");
    lua_pushcfunction(g_lua, l_entity_get_health);
    lua_setfield(g_lua, -2, "GetHealth");
    lua_pushcfunction(g_lua, l_entity_set_health);
    lua_setfield(g_lua, -2, "SetHealth");
    lua_pushcfunction(g_lua, l_entity_mark_changed);
    lua_setfield(g_lua, -2, "MarkChanged");
    lua_pushcfunction(g_lua, l_component_callbacks_probe);
    lua_setfield(g_lua, -2, "ComponentCallbacksProbe");
    lua_pushcfunction(g_lua, l_watch_component_events);
    lua_setfield(g_lua, -2, "WatchComponentEvents");
    lua_pushcfunction(g_lua, l_pointee_class);
    lua_setfield(g_lua, -2, "PointeeClass");
    lua_pushcfunction(g_lua, l_resource_sources);
    lua_setfield(g_lua, -2, "ResourceSources");
    lua_pushcfunction(g_lua, l_texture_atlas_map);
    lua_setfield(g_lua, -2, "TextureAtlasMap");
    lua_pushcfunction(g_lua, l_icon_atlas);
    lua_setfield(g_lua, -2, "IconAtlas");
    lua_pushcfunction(g_lua, l_icon_uvs);
    lua_setfield(g_lua, -2, "IconUVs");
    lua_pushcfunction(g_lua, l_boost_prototype);
    lua_setfield(g_lua, -2, "BoostPrototype");
    lua_pushcfunction(g_lua, l_resource_bank_get);
    lua_setfield(g_lua, -2, "ResourceBankGet");
    lua_pushcfunction(g_lua, l_resource_bank_keys);
    lua_setfield(g_lua, -2, "ResourceBankKeys");
    lua_pushcfunction(g_lua, l_hook_system);
    lua_setfield(g_lua, -2, "HookSystem");
    lua_pushcfunction(g_lua, l_system_probe);
    lua_setfield(g_lua, -2, "SystemProbe");
    lua_pushcfunction(g_lua, l_dialog_manager);
    lua_setfield(g_lua, -2, "DialogManager");
    lua_pushcfunction(g_lua, l_class_constructible);
    lua_setfield(g_lua, -2, "ClassConstructible");
    lua_pushcfunction(g_lua, l_take_component_events);
    lua_setfield(g_lua, -2, "TakeComponentEvents");
    lua_pushcfunction(g_lua, l_component_short_name);
    lua_setfield(g_lua, -2, "ComponentShortName");
    lua_pushcfunction(g_lua, l_watch_replication);
    lua_setfield(g_lua, -2, "WatchReplication");
    lua_pushcfunction(g_lua, l_take_replication_changes);
    lua_setfield(g_lua, -2, "TakeReplicationChanges");
    lua_pushcfunction(g_lua, l_registered_component_types);
    lua_setfield(g_lua, -2, "RegisteredComponentTypes");
    lua_pushcfunction(g_lua, l_entity_alive);
    lua_setfield(g_lua, -2, "EntityAlive");
    lua_pushcfunction(g_lua, l_new_entity_proxy);
    lua_setfield(g_lua, -2, "NewEntityProxy");
    lua_pushcfunction(g_lua, l_new_object_proxy);
    lua_setfield(g_lua, -2, "NewObjectProxy");
    lua_pushcfunction(g_lua, l_is_vector);
    lua_setfield(g_lua, -2, "IsVector");
    lua_pushcfunction(g_lua, l_is_hash_set);
    lua_setfield(g_lua, -2, "IsHashSet");
    lua_pushcfunction(g_lua, l_enum_key);
    lua_setfield(g_lua, -2, "EnumKey");
    lua_pushcfunction(g_lua, l_entity_proxy_handle);
    lua_setfield(g_lua, -2, "EntityProxyHandle");
    lua_pushcfunction(g_lua, l_entity_component_names);
    lua_setfield(g_lua, -2, "EntityComponentNames");
    lua_pushcfunction(g_lua, l_entity_was_changed);
    lua_setfield(g_lua, -2, "EntityWasChanged");
    lua_pushcfunction(g_lua, l_entity_replication_flags);
    lua_setfield(g_lua, -2, "EntityReplicationFlags");
    lua_pushcfunction(g_lua, l_entity_replicate);
    lua_setfield(g_lua, -2, "Replicate");
    lua_pushcfunction(g_lua, l_world_probe);
    lua_setfield(g_lua, -2, "WorldProbe");
    lua_pushcfunction(g_lua, osi_ide_helpers);
    lua_setfield(g_lua, -2, "OsiIdeHelpers");
    lua_pushcfunction(g_lua, l_post_to_other_context);
    lua_setfield(g_lua, -2, "PostToOtherContext");
    lua_pushcfunction(g_lua, l_take_net_messages);
    lua_setfield(g_lua, -2, "TakeNetMessages");
    lua_pushcfunction(g_lua, l_has_other_context);
    lua_setfield(g_lua, -2, "HasOtherContext");
    lua_pushcfunction(g_lua, l_request_reset);
    lua_setfield(g_lua, -2, "RequestReset");
    lua_pushcfunction(g_lua, l_net_info);
    lua_setfield(g_lua, -2, "NetInfo");
    lua_pushcfunction(g_lua, l_net_post);
    lua_setfield(g_lua, -2, "NetPost");
    lua_pushcfunction(g_lua, l_net_sync_vars);
    lua_setfield(g_lua, -2, "NetSyncVars");
    lua_pushcfunction(g_lua, l_net_force_remote);
    lua_setfield(g_lua, -2, "NetForceRemote");
    lua_pushcfunction(g_lua, l_inject_key);
    lua_setfield(g_lua, -2, "InjectKey");
    lua_pushcfunction(g_lua, l_audio);
    lua_setfield(g_lua, -2, "Audio");
    lua_pushcfunction(g_lua, l_enum_count);
    lua_setfield(g_lua, -2, "EnumCount");
    lua_pushcfunction(g_lua, l_enum_at);
    lua_setfield(g_lua, -2, "EnumAt");
    lua_pushcfunction(g_lua, l_enum_value_at);
    lua_setfield(g_lua, -2, "EnumValueAt");
    lua_pushcfunction(g_lua, osi_story_lookup);
    lua_setfield(g_lua, -2, "StoryFunction");
    lua_pushcfunction(g_lua, l_is_client_state);
    lua_setfield(g_lua, -2, "IsClientState");
    lua_pushcfunction(g_lua, l_watch_osiris);
    lua_setfield(g_lua, -2, "WatchOsiris");
    lua_pushcfunction(g_lua, l_watch_osiris_call);
    lua_setfield(g_lua, -2, "WatchOsirisCall");
    lua_pushcfunction(g_lua, l_get_field);
    lua_setfield(g_lua, -2, "GetField");
    lua_pushcfunction(g_lua, l_set_field);
    lua_setfield(g_lua, -2, "SetField");
    lua_pushcfunction(g_lua, l_component_fields);
    lua_setfield(g_lua, -2, "ComponentFields");
    lua_pushcfunction(g_lua, l_field_info);
    lua_setfield(g_lua, -2, "FieldInfo");
    lua_pushcfunction(g_lua, l_array_info);
    lua_setfield(g_lua, -2, "ArrayInfo");
    lua_pushcfunction(g_lua, l_object_set_field);
    lua_setfield(g_lua, -2, "ObjectSetField");
    lua_pushcfunction(g_lua, l_object_set_set);
    lua_setfield(g_lua, -2, "ObjectSetSet");
    lua_pushcfunction(g_lua, l_set_set);
    lua_setfield(g_lua, -2, "SetSet");
    lua_pushcfunction(g_lua, l_set_dump);
    lua_setfield(g_lua, -2, "SetDump");
    lua_pushcfunction(g_lua, l_map_key);
    lua_setfield(g_lua, -2, "MapKey");
    lua_pushcfunction(g_lua, l_variant_index);
    lua_setfield(g_lua, -2, "VariantIndex");
    lua_pushcfunction(g_lua, l_mod_count);
    lua_setfield(g_lua, -2, "ModCount");
    lua_pushcfunction(g_lua, l_mod_uuid_at);
    lua_setfield(g_lua, -2, "ModUuidAt");
    lua_pushcfunction(g_lua, l_mod_at);
    lua_setfield(g_lua, -2, "ModAt");
    lua_pushcfunction(g_lua, l_mod_find);
    lua_setfield(g_lua, -2, "ModFind");
    lua_pushcfunction(g_lua, l_mod_managers);
    lua_setfield(g_lua, -2, "ModManagers");
    lua_pushcfunction(g_lua, l_mod_settings);
    lua_setfield(g_lua, -2, "ModSettings");
    lua_pushcfunction(g_lua, l_mod_manager_uuid_at);
    lua_setfield(g_lua, -2, "ModManagerUuidAt");
    lua_pushcfunction(g_lua, l_mod_base);
    lua_setfield(g_lua, -2, "ModBase");
    lua_pushcfunction(g_lua, l_mod_available_count);
    lua_setfield(g_lua, -2, "ModAvailableCount");
    lua_pushcfunction(g_lua, l_mod_available_at);
    lua_setfield(g_lua, -2, "ModAvailableAt");
    lua_pushcfunction(g_lua, l_mod_info);
    lua_setfield(g_lua, -2, "ModInfo");
    lua_pushcfunction(g_lua, l_mod_list);
    lua_setfield(g_lua, -2, "ModList");
    lua_pushcfunction(g_lua, l_stats_copy_from);
    lua_setfield(g_lua, -2, "StatsCopyFrom");
    lua_pushcfunction(g_lua, l_global_switches);
    lua_setfield(g_lua, -2, "GlobalSwitches");
    lua_pushcfunction(g_lua, l_input_manager);
    lua_setfield(g_lua, -2, "InputManager");
    lua_pushcfunction(g_lua, l_imgui_status);
    lua_setfield(g_lua, -2, "ImguiStatus");
    lua_pushcfunction(g_lua, l_imgui_new_window);
    lua_setfield(g_lua, -2, "ImguiNewWindow");
    lua_pushcfunction(g_lua, l_imgui_add);
    lua_setfield(g_lua, -2, "ImguiAdd");
    lua_pushcfunction(g_lua, l_imgui_call);
    lua_setfield(g_lua, -2, "ImguiCall");
    lua_pushcfunction(g_lua, l_imgui_children);
    lua_setfield(g_lua, -2, "ImguiChildren");
    lua_pushcfunction(g_lua, l_imgui_object);
    lua_setfield(g_lua, -2, "ImguiObject");
    lua_pushcfunction(g_lua, l_imgui_destroy);
    lua_setfield(g_lua, -2, "ImguiDestroy");
    lua_pushcfunction(g_lua, l_imgui_enable_demo);
    lua_setfield(g_lua, -2, "ImguiEnableDemo");
    lua_pushcfunction(g_lua, l_imgui_frame_stats);
    lua_setfield(g_lua, -2, "ImguiFrameStats");
    lua_pushcfunction(g_lua, l_imgui_set_callback);
    lua_setfield(g_lua, -2, "ImguiSetCallback");
    lua_pushcfunction(g_lua, l_imgui_timing);
    lua_setfield(g_lua, -2, "ImguiTiming");
    lua_pushcfunction(g_lua, l_imgui_clear_callback);
    lua_setfield(g_lua, -2, "ImguiClearCallback");
    lua_pushcfunction(g_lua, l_imgui_take_event);
    lua_setfield(g_lua, -2, "ImguiTakeEvent");
    bg3le_ui_register(g_lua);
    lua_pushcfunction(g_lua, bg3le_json_binary_encode);
    lua_setfield(g_lua, -2, "JsonBinaryEncode");
    lua_pushcfunction(g_lua, bg3le_json_binary_decode);
    lua_setfield(g_lua, -2, "JsonBinaryDecode");
    lua_pushcfunction(g_lua, bg3le_json_parse);
    lua_setfield(g_lua, -2, "JsonParse");
    lua_pushcfunction(g_lua, l_imgui_input_state);
    lua_setfield(g_lua, -2, "ImguiInputState");
    lua_pushcfunction(g_lua, l_imgui_window_geometry);
    lua_setfield(g_lua, -2, "ImguiWindowGeometry");
    lua_pushcfunction(g_lua, l_imgui_hovered);
    lua_setfield(g_lua, -2, "ImguiHovered");
    lua_pushcfunction(g_lua, l_imgui_watch);
    lua_setfield(g_lua, -2, "ImguiWatch");
    lua_pushcfunction(g_lua, l_imgui_load_font);
    lua_setfield(g_lua, -2, "ImguiLoadFont");
    lua_pushcfunction(g_lua, l_imgui_font_info);
    lua_setfield(g_lua, -2, "ImguiFontInfo");
    lua_pushcfunction(g_lua, l_imgui_set_ui_scale);
    lua_setfield(g_lua, -2, "ImguiSetUIScale");
    lua_pushcfunction(g_lua, l_imgui_set_font_scale);
    lua_setfield(g_lua, -2, "ImguiSetFontScale");
    lua_pushcfunction(g_lua, l_imgui_viewport_size);
    lua_setfield(g_lua, -2, "ImguiViewportSize");
    lua_pushcfunction(g_lua, l_imgui_mouse_move);
    lua_setfield(g_lua, -2, "ImguiMouseMove");
    lua_pushcfunction(g_lua, l_imgui_click_at);
    lua_setfield(g_lua, -2, "ImguiClickAt");
    lua_pushcfunction(g_lua, l_stats_name_recheck);
    lua_setfield(g_lua, -2, "StatsNameRecheck");
    lua_pushcfunction(g_lua, l_stats_attr_translated);
    lua_setfield(g_lua, -2, "StatsAttrTranslated");
    lua_pushcfunction(g_lua, l_stats_attr_condition);
    lua_setfield(g_lua, -2, "StatsAttrCondition");
    lua_pushcfunction(g_lua, l_expression_dump);
    lua_setfield(g_lua, -2, "ExpressionDump");
    lua_pushcfunction(g_lua, l_object_expression);
    lua_setfield(g_lua, -2, "ObjectExpression");
    lua_pushcfunction(g_lua, l_expression_at);
    lua_setfield(g_lua, -2, "ExpressionAt");
    lua_pushcfunction(g_lua, l_stats_enum_label);
    lua_setfield(g_lua, -2, "StatsEnumLabel");
    lua_pushcfunction(g_lua, l_stats_enum_index);
    lua_setfield(g_lua, -2, "StatsEnumIndex");
    lua_pushcfunction(g_lua, l_stats_list_attrs);
    lua_setfield(g_lua, -2, "StatsListAttrs");
    lua_pushcfunction(g_lua, l_prototype_find);
    lua_setfield(g_lua, -2, "PrototypeFind");
    lua_pushcfunction(g_lua, l_prototype_names);
    lua_setfield(g_lua, -2, "PrototypeNames");
    lua_pushcfunction(g_lua, l_prototype_count);
    lua_setfield(g_lua, -2, "PrototypeCount");
    lua_pushcfunction(g_lua, l_prototype_test_stale);
    lua_setfield(g_lua, -2, "PrototypeTestStale");
    lua_pushcfunction(g_lua, l_template_find);
    lua_setfield(g_lua, -2, "TemplateFind");
    lua_pushcfunction(g_lua, l_template_ids);
    lua_setfield(g_lua, -2, "TemplateIds");
    lua_pushcfunction(g_lua, l_template_find_in);
    lua_setfield(g_lua, -2, "TemplateFindIn");
    lua_pushcfunction(g_lua, l_templates_in);
    lua_setfield(g_lua, -2, "TemplatesIn");
    lua_pushcfunction(g_lua, l_level_data_manager);
    lua_setfield(g_lua, -2, "LevelDataManager");
    lua_pushcfunction(g_lua, l_physics_query);
    lua_setfield(g_lua, -2, "PhysicsQuery");
    lua_pushcfunction(g_lua, l_ai_entities_on_tile);
    lua_setfield(g_lua, -2, "AiEntitiesOnTile");
    lua_pushcfunction(g_lua, l_ai_tile_info);
    lua_setfield(g_lua, -2, "AiTileInfo");
    lua_pushcfunction(g_lua, l_ai_heights_at);
    lua_setfield(g_lua, -2, "AiHeightsAt");
    lua_pushcfunction(g_lua, l_ai_path_create);
    lua_setfield(g_lua, -2, "AiPathCreate");
    lua_pushcfunction(g_lua, l_ai_path_free);
    lua_setfield(g_lua, -2, "AiPathFree");
    lua_pushcfunction(g_lua, l_ai_path_by_id);
    lua_setfield(g_lua, -2, "AiPathById");
    lua_pushcfunction(g_lua, l_ai_paths_active);
    lua_setfield(g_lua, -2, "AiPathsActive");
    lua_pushcfunction(g_lua, l_ai_path_search);
    lua_setfield(g_lua, -2, "AiPathSearch");
    lua_pushcfunction(g_lua, l_surface_action_create);
    lua_setfield(g_lua, -2, "SurfaceActionCreate");
    lua_pushcfunction(g_lua, l_surface_action_execute);
    lua_setfield(g_lua, -2, "SurfaceActionExecute");
    lua_pushcfunction(g_lua, l_functor_params);
    lua_setfield(g_lua, -2, "FunctorParams");
    lua_pushcfunction(g_lua, l_functors_list);
    lua_setfield(g_lua, -2, "FunctorsList");
    lua_pushcfunction(g_lua, l_functors_add);
    lua_setfield(g_lua, -2, "FunctorsAdd");
    lua_pushcfunction(g_lua, l_functors_remove);
    lua_setfield(g_lua, -2, "FunctorsRemove");
    lua_pushcfunction(g_lua, l_static_data_create);
    lua_setfield(g_lua, -2, "StaticDataCreate");
    lua_pushcfunction(g_lua, l_static_data_bank_call);
    lua_setfield(g_lua, -2, "StaticDataBankCall");
    lua_pushcfunction(g_lua, l_functors_execute);
    lua_setfield(g_lua, -2, "FunctorsExecute");
    lua_pushcfunction(g_lua, l_level_add_persistent_template);
    lua_setfield(g_lua, -2, "LevelAddPersistentTemplate");
    lua_pushcfunction(g_lua, l_loca_get);
    lua_setfield(g_lua, -2, "Loca");
    lua_pushcfunction(g_lua, l_loca_keys);
    lua_setfield(g_lua, -2, "LocaKeys");
    lua_pushcfunction(g_lua, l_loca_set);
    lua_setfield(g_lua, -2, "LocaSet");
    lua_pushcfunction(g_lua, l_env);
    lua_setfield(g_lua, -2, "Env");
    lua_pushcfunction(g_lua, l_string_table_dump);
    lua_setfield(g_lua, -2, "StringTableDump");
    lua_pushcfunction(g_lua, l_stats_names);
    lua_setfield(g_lua, -2, "StatsNames");
    lua_pushcfunction(g_lua, l_stats_attr_find);
    lua_setfield(g_lua, -2, "StatsAttrFind");
    lua_pushcfunction(g_lua, l_stats_attr_set);
    lua_setfield(g_lua, -2, "StatsAttrSet");
    lua_pushcfunction(g_lua, l_stats_condition_intern);
    lua_setfield(g_lua, -2, "StatsConditionIntern");
    lua_pushcfunction(g_lua, l_stats_int64_intern);
    lua_setfield(g_lua, -2, "StatsInt64Intern");
    lua_pushcfunction(g_lua, l_stats_float_intern);
    lua_setfield(g_lua, -2, "StatsFloatIntern");
    lua_pushcfunction(g_lua, l_stats_guid_intern);
    lua_setfield(g_lua, -2, "StatsGuidIntern");
    lua_pushcfunction(g_lua, l_stats_translated_intern);
    lua_setfield(g_lua, -2, "StatsTranslatedIntern");
    lua_pushcfunction(g_lua, l_stats_ai_flags_set);
    lua_setfield(g_lua, -2, "StatsAIFlagsSet");
    lua_pushcfunction(g_lua, l_stats_combo_get);
    lua_setfield(g_lua, -2, "StatsComboGet");
    lua_pushcfunction(g_lua, l_stats_combo_set);
    lua_setfield(g_lua, -2, "StatsComboSet");
    lua_pushcfunction(g_lua, l_stats_split_groups);
    lua_setfield(g_lua, -2, "StatsSplitGroups");
    lua_pushcfunction(g_lua, l_stats_roll_set);
    lua_setfield(g_lua, -2, "StatsRollSet");
    lua_pushcfunction(g_lua, l_stats_requirements);
    lua_setfield(g_lua, -2, "StatsRequirements");
    lua_pushcfunction(g_lua, l_stats_requirements_set);
    lua_setfield(g_lua, -2, "StatsRequirementsSet");
    lua_pushcfunction(g_lua, l_fixed_string_intern);
    lua_setfield(g_lua, -2, "FixedStringIntern");
    lua_pushcfunction(g_lua, l_stats_string_intern);
    lua_setfield(g_lua, -2, "StatsStringIntern");
    lua_pushcfunction(g_lua, l_all_entities);
    lua_setfield(g_lua, -2, "AllEntities");
    lua_pushcfunction(g_lua, l_component_type_names);
    lua_setfield(g_lua, -2, "ComponentTypeNames");
    lua_pushcfunction(g_lua, l_type_names);
    lua_setfield(g_lua, -2, "TypeNames");
    lua_pushcfunction(g_lua, l_class_name);
    lua_setfield(g_lua, -2, "ClassName");
    lua_pushcfunction(g_lua, l_type_name_at);
    lua_setfield(g_lua, -2, "TypeNameAt");
    lua_pushcfunction(g_lua, l_type_component);
    lua_setfield(g_lua, -2, "TypeIsComponent");
    lua_pushcfunction(g_lua, l_stats_functor_groups);
    lua_setfield(g_lua, -2, "StatsFunctorGroups");
    lua_pushcfunction(g_lua, l_stats_ai_flags);
    lua_setfield(g_lua, -2, "StatsAIFlags");
    lua_pushcfunction(g_lua, l_stats_roll_conditions);
    lua_setfield(g_lua, -2, "StatsRollConditions");
    lua_pushcfunction(g_lua, l_stat_origin);
    lua_setfield(g_lua, -2, "StatOrigin");
    lua_pushcfunction(g_lua, l_stats_take_loaded);
    lua_setfield(g_lua, -2, "StatsTakeLoaded");
    lua_pushcfunction(g_lua, l_stats_set_all);
    lua_setfield(g_lua, -2, "StatsSetAll");
    lua_pushcfunction(g_lua, l_stats_set_get);
    lua_setfield(g_lua, -2, "StatsSetGet");
    lua_pushcfunction(g_lua, l_stats_set_create);
    lua_setfield(g_lua, -2, "StatsSetCreate");
    lua_pushcfunction(g_lua, l_stats_treasure_table_read);
    lua_setfield(g_lua, -2, "StatsTreasureTableRead");
    lua_pushcfunction(g_lua, l_stats_treasure_category_read);
    lua_setfield(g_lua, -2, "StatsTreasureCategoryRead");
    lua_pushcfunction(g_lua, l_stats_treasure_table_update);
    lua_setfield(g_lua, -2, "StatsTreasureTableUpdate");
    lua_pushcfunction(g_lua, l_stats_treasure_category_update);
    lua_setfield(g_lua, -2, "StatsTreasureCategoryUpdate");
    lua_pushcfunction(g_lua, l_stats_count);
    lua_setfield(g_lua, -2, "StatsCount");
    lua_pushcfunction(g_lua, l_stats_name_at);
    lua_setfield(g_lua, -2, "StatsNameAt");
    lua_pushcfunction(g_lua, l_stats_at);
    lua_setfield(g_lua, -2, "StatsAt");
    lua_pushcfunction(g_lua, l_stats_find);
    lua_setfield(g_lua, -2, "StatsFind");
    lua_pushcfunction(g_lua, l_stat_sync);
    lua_setfield(g_lua, -2, "StatSync");
    lua_pushcfunction(g_lua, l_stats_create);
    lua_setfield(g_lua, -2, "StatsCreate");
    lua_pushcfunction(g_lua, l_entity_create);
    lua_setfield(g_lua, -2, "EntityCreate");
    lua_pushcfunction(g_lua, l_stats_set_functors);
    lua_setfield(g_lua, -2, "StatsSetFunctors");
    lua_pushcfunction(g_lua, l_trace_setup);
    lua_setfield(g_lua, -2, "TraceSetup");
    lua_pushcfunction(g_lua, l_trace_enable);
    lua_setfield(g_lua, -2, "TraceEnable");
    lua_pushcfunction(g_lua, l_trace_get);
    lua_setfield(g_lua, -2, "TraceGet");
    lua_pushcfunction(g_lua, l_trace_clear);
    lua_setfield(g_lua, -2, "TraceClear");
    lua_pushcfunction(g_lua, l_entity_destroy);
    lua_setfield(g_lua, -2, "EntityDestroy");
    lua_pushcfunction(g_lua, l_stats_enum_add);
    lua_setfield(g_lua, -2, "StatsEnumAdd");
    lua_pushcfunction(g_lua, l_stats_attr_add);
    lua_setfield(g_lua, -2, "StatsAttrAdd");
    lua_pushcfunction(g_lua, l_type_info_fields);
    lua_setfield(g_lua, -2, "TypeInfoFields");
    lua_pushcfunction(g_lua, l_type_info_each);
    lua_setfield(g_lua, -2, "TypeInfoEach");
    lua_pushcfunction(g_lua, l_type_info_names);
    lua_setfield(g_lua, -2, "TypeInfoNames");
    lua_pushcfunction(g_lua, l_ui_object);
    lua_setfield(g_lua, -2, "UiObject");
    lua_pushcfunction(g_lua, l_entity_net_id);
    lua_setfield(g_lua, -2, "EntityNetId");
    lua_pushcfunction(g_lua, l_net_id_entity);
    lua_setfield(g_lua, -2, "NetIdEntity");
    lua_pushcfunction(g_lua, l_entity_component_add);
    lua_setfield(g_lua, -2, "EntityComponentAdd");
    lua_pushcfunction(g_lua, l_entity_component_remove);
    lua_setfield(g_lua, -2, "EntityComponentRemove");
    lua_pushcfunction(g_lua, l_entity_ecb_changes);
    lua_setfield(g_lua, -2, "EntityEcbChanges");
    lua_pushcfunction(g_lua, l_map_edit);
    lua_setfield(g_lua, -2, "MapEdit");
    lua_pushcfunction(g_lua, l_object_map_edit);
    lua_setfield(g_lua, -2, "ObjectMapEdit");
    lua_pushcfunction(g_lua, l_path_override_add);
    lua_setfield(g_lua, -2, "PathOverrideAdd");
    lua_pushcfunction(g_lua, l_path_override_get);
    lua_setfield(g_lua, -2, "PathOverrideGet");
    lua_pushcfunction(g_lua, l_stats_extra_get);
    lua_setfield(g_lua, -2, "StatsExtraGet");
    lua_pushcfunction(g_lua, l_stats_extra_set);
    lua_setfield(g_lua, -2, "StatsExtraSet");
    lua_pushcfunction(g_lua, l_stats_extra_all);
    lua_setfield(g_lua, -2, "StatsExtraAll");
    lua_pushcfunction(g_lua, l_stats_manager_address);
    lua_setfield(g_lua, -2, "StatsManagerAddress");
    lua_pushcfunction(g_lua, l_type_info_at);
    lua_setfield(g_lua, -2, "TypeInfoAt");
    lua_pushcfunction(g_lua, l_type_info_ref);
    lua_setfield(g_lua, -2, "TypeInfoRef");
    lua_pushcfunction(g_lua, l_builtin_file);
    lua_setfield(g_lua, -2, "BuiltinFile");
    lua_pushcfunction(g_lua, l_string_key_find);
    lua_setfield(g_lua, -2, "StringKeyFind");
    lua_pushcfunction(g_lua, l_string_keys);
    lua_setfield(g_lua, -2, "StringKeys");
    lua_pushcfunction(g_lua, l_string_key_set);
    lua_setfield(g_lua, -2, "StringKeySet");
    lua_pushcfunction(g_lua, l_settings_flag);
    lua_setfield(g_lua, -2, "SettingsFlag");
    lua_pushcfunction(g_lua, l_engine_time);
    lua_setfield(g_lua, -2, "EngineTime");
    lua_pushcfunction(g_lua, l_stats_type);
    lua_setfield(g_lua, -2, "StatsType");
    lua_pushcfunction(g_lua, l_stats_using);
    lua_setfield(g_lua, -2, "StatsUsing");
    lua_pushcfunction(g_lua, l_stats_list_index);
    lua_setfield(g_lua, -2, "StatsListIndex");
    lua_pushcfunction(g_lua, l_stats_attr_count);
    lua_setfield(g_lua, -2, "StatsAttrCount");
    lua_pushcfunction(g_lua, l_stats_attr_at);
    lua_setfield(g_lua, -2, "StatsAttrAt");
    lua_pushcfunction(g_lua, l_stats_attr_label);
    lua_setfield(g_lua, -2, "StatsAttrLabel");
    lua_pushcfunction(g_lua, l_stats_attr_string);
    lua_setfield(g_lua, -2, "StatsAttrString");
    lua_pushcfunction(g_lua, l_stats_attr_float);
    lua_setfield(g_lua, -2, "StatsAttrFloat");
    lua_pushcfunction(g_lua, l_stats_attr_guid);
    lua_setfield(g_lua, -2, "StatsAttrGuid");
    lua_pushcfunction(g_lua, l_stats_attr_flags);
    lua_setfield(g_lua, -2, "StatsAttrFlags");
    lua_pushcfunction(g_lua, l_resource_banks);
    lua_setfield(g_lua, -2, "ResourceBanks");
    lua_pushcfunction(g_lua, l_resource_get);
    lua_setfield(g_lua, -2, "ResourceGet");
    lua_pushcfunction(g_lua, l_resource_guids);
    lua_setfield(g_lua, -2, "ResourceGuids");
    lua_pushcfunction(g_lua, l_object_fields);
    lua_setfield(g_lua, -2, "ObjectFields");
    lua_pushcfunction(g_lua, l_object_field_info);
    lua_setfield(g_lua, -2, "ObjectFieldInfo");
    lua_pushcfunction(g_lua, l_object_get_field);
    lua_setfield(g_lua, -2, "ObjectGetField");
    lua_pushcfunction(g_lua, l_object_array_info);
    lua_setfield(g_lua, -2, "ObjectArrayInfo");
    lua_pushcfunction(g_lua, l_object_variant_index);
    lua_setfield(g_lua, -2, "ObjectVariantIndex");
    lua_pushcfunction(g_lua, l_object_map_key);
    lua_setfield(g_lua, -2, "ObjectMapKey");
    lua_pushcfunction(g_lua, l_object_field_address);
    lua_setfield(g_lua, -2, "ObjectFieldAddress");
    lua_pushcfunction(g_lua, l_field_address);
    lua_setfield(g_lua, -2, "FieldAddress");
    lua_pushcfunction(g_lua, l_field_bytes);
    lua_setfield(g_lua, -2, "FieldBytes");
    lua_pushcfunction(g_lua, l_size_audit);
    lua_setfield(g_lua, -2, "SizeAudit");
    lua_pushcfunction(g_lua, l_entity_has_component);
    lua_setfield(g_lua, -2, "HasComponent");
    lua_setfield(g_lua, -2, "_Internal");

    // Clocks, identity and file access for Ext.Utils, Ext.IO and
    // Ext.Timer. Parked under _Internal; the prelude arranges them into the
    // modules and names bg3se uses. See src/ext_libs.cpp.
    lua_getfield(g_lua, -1, "_Internal");
    lua_pushcfunction(g_lua, bg3le_ext_monotonic_time);
    lua_setfield(g_lua, -2, "MonotonicTime");
    lua_pushcfunction(g_lua, bg3le_ext_microsec_time);
    lua_setfield(g_lua, -2, "MicrosecTime");
    lua_pushcfunction(g_lua, bg3le_ext_clock_epoch);
    lua_setfield(g_lua, -2, "ClockEpoch");
    lua_pushcfunction(g_lua, bg3le_ext_clock_time);
    lua_setfield(g_lua, -2, "ClockTime");
    lua_pushcfunction(g_lua, bg3le_ext_generate_guid);
    lua_setfield(g_lua, -2, "GenerateGuid");
    lua_pushcfunction(g_lua, bg3le_ext_game_version);
    lua_setfield(g_lua, -2, "GameVersion");
    lua_pushcfunction(g_lua, bg3le_ext_command_line);
    lua_setfield(g_lua, -2, "GetCommandLineParams");
    lua_pushcfunction(g_lua, bg3le_ext_load_file);
    lua_setfield(g_lua, -2, "LoadFile");
    lua_pushcfunction(g_lua, bg3le_ext_save_file);
    lua_setfield(g_lua, -2, "SaveFile");
    lua_pushcfunction(g_lua, bg3le_ext_write_data_file);
    lua_setfield(g_lua, -2, "WriteDataFile");
    lua_pushcfunction(g_lua, bg3le_ext_memory_usage);
    lua_setfield(g_lua, -2, "GetMemoryUsage");
    lua_pushcfunction(g_lua, bg3le_ext_show_error);
    lua_setfield(g_lua, -2, "ShowError");
    lua_pushcfunction(g_lua, bg3le_ext_show_error_and_exit);
    lua_setfield(g_lua, -2, "ShowErrorAndExit");
    lua_pushcfunction(g_lua, bg3le_ext_pak_modules);
    lua_setfield(g_lua, -2, "PakModules");
    lua_pushcfunction(g_lua, bg3le_ext_mod_settings_order);
    lua_setfield(g_lua, -2, "ModSettingsOrder");
    lua_pushcfunction(g_lua, bg3le_ext_pak_read);
    lua_setfield(g_lua, -2, "PakRead");
    // Native plugins (src/plugins.cpp); the prelude makes them Ext.Plugins.
    lua_pushcfunction(g_lua, bg3le_ext_plugin_list);
    lua_setfield(g_lua, -2, "PluginList");
    lua_pushcfunction(g_lua, bg3le_ext_plugin_settings);
    lua_setfield(g_lua, -2, "PluginSettings");
    lua_pushcfunction(g_lua, bg3le_ext_plugin_get);
    lua_setfield(g_lua, -2, "PluginGet");
    lua_pushcfunction(g_lua, bg3le_ext_plugin_set);
    lua_setfield(g_lua, -2, "PluginSet");
    lua_pop(g_lua, 1);

    // ---- Ext.Math ----
    //
    // A module of its own rather than entries under _Internal: these are
    // the public functions themselves, taking and returning the plain Lua
    // values bg3se uses -- a vector is an array of numbers, a matrix a flat
    // column-major array. See src/vendor/math.cpp.
    lua_createtable(g_lua, 0, 59);
    lua_pushcfunction(g_lua, bg3le_math_add);
    lua_setfield(g_lua, -2, "Add");
    lua_pushcfunction(g_lua, bg3le_math_sub);
    lua_setfield(g_lua, -2, "Sub");
    lua_pushcfunction(g_lua, bg3le_math_mul);
    lua_setfield(g_lua, -2, "Mul");
    lua_pushcfunction(g_lua, bg3le_math_div);
    lua_setfield(g_lua, -2, "Div");
    lua_pushcfunction(g_lua, bg3le_math_reflect);
    lua_setfield(g_lua, -2, "Reflect");
    lua_pushcfunction(g_lua, bg3le_math_angle);
    lua_setfield(g_lua, -2, "Angle");
    lua_pushcfunction(g_lua, bg3le_math_cross);
    lua_setfield(g_lua, -2, "Cross");
    lua_pushcfunction(g_lua, bg3le_math_distance);
    lua_setfield(g_lua, -2, "Distance");
    lua_pushcfunction(g_lua, bg3le_math_dot);
    lua_setfield(g_lua, -2, "Dot");
    lua_pushcfunction(g_lua, bg3le_math_length);
    lua_setfield(g_lua, -2, "Length");
    lua_pushcfunction(g_lua, bg3le_math_normalize);
    lua_setfield(g_lua, -2, "Normalize");
    lua_pushcfunction(g_lua, bg3le_math_perpendicular);
    lua_setfield(g_lua, -2, "Perpendicular");
    lua_pushcfunction(g_lua, bg3le_math_project);
    lua_setfield(g_lua, -2, "Project");
    lua_pushcfunction(g_lua, bg3le_math_determinant);
    lua_setfield(g_lua, -2, "Determinant");
    lua_pushcfunction(g_lua, bg3le_math_inverse);
    lua_setfield(g_lua, -2, "Inverse");
    lua_pushcfunction(g_lua, bg3le_math_transpose);
    lua_setfield(g_lua, -2, "Transpose");
    lua_pushcfunction(g_lua, bg3le_math_outer_product);
    lua_setfield(g_lua, -2, "OuterProduct");
    lua_pushcfunction(g_lua, bg3le_math_rotate);
    lua_setfield(g_lua, -2, "Rotate");
    lua_pushcfunction(g_lua, bg3le_math_translate);
    lua_setfield(g_lua, -2, "Translate");
    lua_pushcfunction(g_lua, bg3le_math_scale);
    lua_setfield(g_lua, -2, "Scale");
    lua_pushcfunction(g_lua, bg3le_math_extract_euler_angles);
    lua_setfield(g_lua, -2, "ExtractEulerAngles");
    lua_pushcfunction(g_lua, bg3le_math_build_from_euler_angles3);
    lua_setfield(g_lua, -2, "BuildFromEulerAngles3");
    lua_pushcfunction(g_lua, bg3le_math_build_from_euler_angles4);
    lua_setfield(g_lua, -2, "BuildFromEulerAngles4");
    lua_pushcfunction(g_lua, bg3le_math_decompose);
    lua_setfield(g_lua, -2, "Decompose");
    lua_pushcfunction(g_lua, bg3le_math_extract_axis_angle);
    lua_setfield(g_lua, -2, "ExtractAxisAngle");
    lua_pushcfunction(g_lua, bg3le_math_build_from_axis_angle3);
    lua_setfield(g_lua, -2, "BuildFromAxisAngle3");
    lua_pushcfunction(g_lua, bg3le_math_build_from_axis_angle4);
    lua_setfield(g_lua, -2, "BuildFromAxisAngle4");
    lua_pushcfunction(g_lua, bg3le_math_build_rotation3);
    lua_setfield(g_lua, -2, "BuildRotation3");
    lua_pushcfunction(g_lua, bg3le_math_build_rotation4);
    lua_setfield(g_lua, -2, "BuildRotation4");
    lua_pushcfunction(g_lua, bg3le_math_build_translation);
    lua_setfield(g_lua, -2, "BuildTranslation");
    lua_pushcfunction(g_lua, bg3le_math_build_scale);
    lua_setfield(g_lua, -2, "BuildScale");
    lua_pushcfunction(g_lua, bg3le_math_quat_from_euler);
    lua_setfield(g_lua, -2, "QuatFromEuler");
    lua_pushcfunction(g_lua, bg3le_math_quat_from_to_rotation);
    lua_setfield(g_lua, -2, "QuatFromToRotation");
    lua_pushcfunction(g_lua, bg3le_math_quat_dot);
    lua_setfield(g_lua, -2, "QuatDot");
    lua_pushcfunction(g_lua, bg3le_math_quat_slerp);
    lua_setfield(g_lua, -2, "QuatSlerp");
    lua_pushcfunction(g_lua, bg3le_math_quat_to_mat3);
    lua_setfield(g_lua, -2, "QuatToMat3");
    lua_pushcfunction(g_lua, bg3le_math_quat_to_mat4);
    lua_setfield(g_lua, -2, "QuatToMat4");
    lua_pushcfunction(g_lua, bg3le_math_mat3_to_quat);
    lua_setfield(g_lua, -2, "Mat3ToQuat");
    lua_pushcfunction(g_lua, bg3le_math_mat4_to_quat);
    lua_setfield(g_lua, -2, "Mat4ToQuat");
    lua_pushcfunction(g_lua, bg3le_math_quat_normalize);
    lua_setfield(g_lua, -2, "QuatNormalize");
    lua_pushcfunction(g_lua, bg3le_math_quat_inverse);
    lua_setfield(g_lua, -2, "QuatInverse");
    lua_pushcfunction(g_lua, bg3le_math_quat_rotate);
    lua_setfield(g_lua, -2, "QuatRotate");
    lua_pushcfunction(g_lua, bg3le_math_quat_rotate_axis_angle);
    lua_setfield(g_lua, -2, "QuatRotateAxisAngle");
    lua_pushcfunction(g_lua, bg3le_math_quat_length);
    lua_setfield(g_lua, -2, "QuatLength");
    lua_pushcfunction(g_lua, bg3le_math_quat_mul);
    lua_setfield(g_lua, -2, "QuatMul");
    lua_pushcfunction(g_lua, bg3le_math_random);
    lua_setfield(g_lua, -2, "Random");
    lua_pushcfunction(g_lua, bg3le_math_round);
    lua_setfield(g_lua, -2, "Round");
    lua_pushcfunction(g_lua, bg3le_math_fract);
    lua_setfield(g_lua, -2, "Fract");
    lua_pushcfunction(g_lua, bg3le_math_trunc);
    lua_setfield(g_lua, -2, "Trunc");
    lua_pushcfunction(g_lua, bg3le_math_sign);
    lua_setfield(g_lua, -2, "Sign");
    lua_pushcfunction(g_lua, bg3le_math_clamp);
    lua_setfield(g_lua, -2, "Clamp");
    lua_pushcfunction(g_lua, bg3le_math_smoothstep);
    lua_setfield(g_lua, -2, "Smoothstep");
    lua_pushcfunction(g_lua, bg3le_math_lerp);
    lua_setfield(g_lua, -2, "Lerp");
    lua_pushcfunction(g_lua, bg3le_math_asin);
    lua_setfield(g_lua, -2, "Asin");
    lua_pushcfunction(g_lua, bg3le_math_acos);
    lua_setfield(g_lua, -2, "Acos");
    lua_pushcfunction(g_lua, bg3le_math_atan);
    lua_setfield(g_lua, -2, "Atan");
    lua_pushcfunction(g_lua, bg3le_math_atan2);
    lua_setfield(g_lua, -2, "Atan2");
    lua_pushcfunction(g_lua, bg3le_math_is_nan);
    lua_setfield(g_lua, -2, "IsNaN");
    lua_pushcfunction(g_lua, bg3le_math_is_inf);
    lua_setfield(g_lua, -2, "IsInf");
    lua_setfield(g_lua, -2, "Math");


    lua_setglobal(g_lua, "Ext");

    static const char kPrelude[] = R"LUA(
-- Upstream's sandbox takes these from mods; bg3le's own code keeps them.
Ext._Internal.RawLoad = load
Ext._Internal.SetHook = debug.sethook
Ext._Internal.OpenFile = io.open
Ext._Internal.Getenv = os.getenv
-- The real metatable: getmetatable names bg3le's objects, as upstream's does.
Ext._Internal.RawGetMetatable = debug.getmetatable

-- Upstream's EnumValue (Lua/Shared/Proxies/LuaEnumValue.inl): Label, Value
-- and EnumName, the label as its string, equal to and ordered against its
-- label, its number or another value of the enum, and its label as a table
-- key (the Lua fork's canonicalize_key). One per enum value, made on first
-- read. Its __name is the enum's, which getmetatable and the type queries
-- report.
do
  local made, values = {}, {}

  -- The enum's label -> value, from the same tables Ext.Enums is built from.
  local function labels_of(name)
    local t = values[name]
    if t ~= nil then return t end
    t = {}
    local i = 0
    while true do
      local label, value = Ext._Internal.EnumValueAt(name, i)
      if label == nil then break end
      t[label] = value
      i = i + 1
    end
    values[name] = t
    return t
  end

  -- What another operand is as a value of `name`, or nil.
  local function as_value(name, other)
    local t = type(other)
    if t == "number" then return other end
    if t == "string" then return labels_of(name)[other] end
    if t == "userdata" then
      local meta = Ext._Internal.RawGetMetatable(other)
      if type(meta) == "table" and meta.__bg3leEnumName == name then
        return meta.__bg3leEnumValue
      end
    end
    return nil
  end

  local function operands(a, b)
    local meta = Ext._Internal.RawGetMetatable(a)
    if type(meta) ~= "table" or meta.__bg3leEnumName == nil then
      meta = Ext._Internal.RawGetMetatable(b)
    end
    local name = meta.__bg3leEnumName
    return as_value(name, a), as_value(name, b)
  end

  function Ext._Internal.EnumValue(name, label, value)
    local byValue = made[name]
    if byValue == nil then
      byValue = {}
      made[name] = byValue
    end
    local hit = byValue[value]
    if hit ~= nil then return hit end

    hit = Ext._Internal.NewObjectProxy({
      __name = name,
      __bg3leEnumName = name,
      __bg3leEnumLabel = label,
      __bg3leEnumValue = value,
      __index = function(_, key)
        if key == "Label" then return label end
        if key == "Value" then return value end
        if key == "EnumName" then return name end
        error("Enum values have no property named '" .. tostring(key) .. "'", 2)
      end,
      __tostring = function() return label end,
      __eq = function(a, b)
        local x, y = operands(a, b)
        return x ~= nil and x == y
      end,
      __lt = function(a, b)
        local x, y = operands(a, b)
        return x ~= nil and y ~= nil and x < y
      end,
      __le = function(a, b)
        local x, y = operands(a, b)
        return x ~= nil and y ~= nil and x <= y
      end,
    })
    byValue[value] = hit
    return hit
  end

  -- The label of an EnumValue, or nil for anything else.
  function Ext._Internal.EnumLabelOf(v)
    if type(v) ~= "userdata" then return nil end
    local meta = Ext._Internal.RawGetMetatable(v)
    return type(meta) == "table" and meta.__bg3leEnumLabel or nil
  end
end

-- Upstream's BitfieldValue (Lua/Shared/Proxies/LuaBitfieldValue.inl): a label
-- indexes it as a boolean and a position as the n-th set label; __Labels,
-- __Value and __EnumName; its length, iteration, tostring, & | ~ and
-- equality. One per value, made on first read; __name is the bitfield's.
do
  local made, infos = {}, {}

  -- Declaration order, label -> value, single bit -> label, every bit named.
  local function info_of(name)
    local info = infos[name]
    if info ~= nil then return info end
    info = {order = {}, byLabel = {}, byBit = {}, allowed = 0}
    local i = 0
    while true do
      local label, value = Ext._Internal.EnumValueAt(name, i)
      if label == nil then break end
      info.order[#info.order + 1] = label
      info.byLabel[label] = value
      if info.byBit[value] == nil then info.byBit[value] = label end
      info.allowed = info.allowed | value
      i = i + 1
    end
    infos[name] = info
    return info
  end

  -- The n-th set bit of the named flags, lowest first.
  local function bit_at(info, value, n)
    local v, i = value & info.allowed, 0
    while v ~= 0 do
      local low = v & -v
      i = i + 1
      if i == n then return low end
      v = v & ~low
    end
    return nil
  end

  -- Another operand as a value of `name`, as upstream's get_bitfield_value.
  local function operand(name, other, mask)
    local info = info_of(name)
    local v
    if math.type(other) == "integer" then
      if (other & ~info.allowed) == 0 then v = other
      elseif mask then v = other & info.allowed end
    elseif type(other) == "string" then
      v = info.byLabel[other]
    elseif type(other) == "table" then
      v = 0
      for _, label in pairs(other) do
        local bit = info.byLabel[label]
        if bit == nil then v = nil break end
        v = v | bit
      end
    elseif type(other) == "userdata" then
      local meta = Ext._Internal.RawGetMetatable(other)
      if type(meta) == "table" and meta.__bg3leBitfieldName == name then
        v = meta.__bg3leBitfieldValue
      end
    end
    if v == nil then
      error(string.format("not a valid '%s' bitfield value: %s", name, tostring(other)), 3)
    end
    return v
  end

  -- Either operand may be the bitfield.
  local function binary(a, b, op, mask)
    local meta = Ext._Internal.RawGetMetatable(a)
    local other = b
    if type(meta) ~= "table" or meta.__bg3leBitfieldName == nil then
      meta, other = Ext._Internal.RawGetMetatable(b), a
    end
    local name = meta.__bg3leBitfieldName
    return Ext._Internal.BitfieldValue(name,
      op(meta.__bg3leBitfieldValue, operand(name, other, mask)))
  end

  function Ext._Internal.BitfieldValue(name, value)
    local byValue = made[name]
    if byValue == nil then
      byValue = {}
      made[name] = byValue
    end
    local hit = byValue[value]
    if hit ~= nil then return hit end
    local info = info_of(name)

    local function labels()
      local out = {}
      for _, label in ipairs(info.order) do
        local v = info.byLabel[label]
        if v ~= 0 and (v & (v - 1)) == 0 and (value & v) == v then
          out[#out + 1] = label
        end
      end
      return out
    end

    hit = Ext._Internal.NewObjectProxy({
      __name = name,
      __bg3leBitfieldName = name,
      __bg3leBitfieldValue = value,
      __index = function(_, key)
        if type(key) == "string" then
          if key == "__Labels" then return labels() end
          if key == "__Value" then return value end
          if key == "__EnumName" then return name end
          local v = info.byLabel[key]
          if v == nil then
            error(string.format("Bitfield of type '%s' has no enum label or special property named '%s'",
                                name, key), 2)
          end
          return (value & v) == v
        elseif type(key) == "number" then
          local n = math.tointeger(key)
          if n == nil or n < 1 then return nil end
          local bit = bit_at(info, value, n)
          return bit and info.byBit[bit]
        end
        error("Bitfield __index parameter must be an integer, enum label, or special property name; got "
              .. type(key), 2)
      end,
      __len = function()
        local v, n = value & info.allowed, 0
        while v ~= 0 do v, n = v & (v - 1), n + 1 end
        return n
      end,
      __pairs = function(self)
        return function(_, k)
          k = (k or 0) + 1
          local bit = bit_at(info, value, k)
          if bit == nil then return nil end
          return k, info.byBit[bit]
        end, self, nil
      end,
      __tostring = function()
        local text = table.concat(labels(), ",")
        local unknown = value & ~info.allowed
        if unknown ~= 0 then
          text = (text ~= "" and text .. "," or "") .. "Unknowns:" .. tostring(unknown)
        end
        return name .. "(" .. text .. ")"
      end,
      __eq = function(a, b)
        local x, y = Ext._Internal.RawGetMetatable(a), Ext._Internal.RawGetMetatable(b)
        return type(x) == "table" and type(y) == "table"
               and x.__bg3leBitfieldName == y.__bg3leBitfieldName
               and x.__bg3leBitfieldValue == y.__bg3leBitfieldValue
      end,
      __band = function(a, b) return binary(a, b, function(x, y) return x & y end, true) end,
      __bor = function(a, b) return binary(a, b, function(x, y) return x | y end, false) end,
      __bxor = function(a, b) return binary(a, b, function(x, y) return x ~ y end, false) end,
      __bnot = function() return Ext._Internal.BitfieldValue(name, ~value & info.allowed) end,
    })
    byValue[value] = hit
    return hit
  end

  -- A BitfieldValue's bitfield and value, or nil for anything else.
  function Ext._Internal.BitfieldOf(v)
    if type(v) ~= "userdata" then return nil end
    local meta = Ext._Internal.RawGetMetatable(v)
    if type(meta) ~= "table" or meta.__bg3leBitfieldName == nil then return nil end
    return meta.__bg3leBitfieldName, meta.__bg3leBitfieldValue
  end
end

-- A class's field table is fixed when bg3le is built, so it is made once:
-- building it per object was most of a template's cost (33,806 of them in
-- GetAllRootTemplates). Callers only read it.
do
  local object_fields, component_fields = Ext._Internal.ObjectFields, Ext._Internal.ComponentFields
  local by_class, by_component, sizes = {}, {}, {}
  Ext._Internal.ObjectFields = function(class, path)
    local key = class .. "\0" .. (path or "")
    local hit = by_class[key]
    if hit ~= nil then return hit end
    local fields, err = object_fields(class, path)
    if fields ~= nil then by_class[key] = fields end
    return fields, err
  end
  Ext._Internal.ComponentFields = function(name, path)
    local key = name .. "\0" .. (path or "")
    local hit = by_component[key]
    if hit ~= nil then return hit, sizes[key] end
    local fields, size = component_fields(name, path)
    if fields ~= nil then by_component[key], sizes[key] = fields, size end
    return fields, size
  end
end

-- _D is Ext.Json.Stringify, which is why strings come back quoted:
-- _D(GetHostCharacter()) yields "<uuid>" while _P yields <uuid>.
Ext.Json = Ext.Json or {}

-- A JSON string literal.
--
-- string.format("%q") escapes for Lua, not for JSON: a control character
-- comes out as \1 rather than \u0001, which no JSON parser accepts -- ours
-- included, which is how this was found. A payload carrying one round-tripped
-- as a parse failure and the message was dropped.
local JSON_ESCAPES = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
  ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function json_string(v)
  local escaped = v:gsub('[%z\1-\31"\\]', function(c)
    local simple = JSON_ESCAPES[c]
    if simple ~= nil then return simple end
    return string.format('\\u%04x', c:byte())
  end)
  return '"' .. escaped .. '"'
end

-- A table, or one of the userdata views: anything iterable as key, value.
local function walkable(v)
  if type(v) == "table" then return true end
  if type(v) ~= "userdata" then return false end
  local meta = Ext._Internal.RawGetMetatable(v)
  return type(meta) == "table" and meta.__pairs ~= nil
end
Ext._Internal.Walkable = walkable

-- Ext.Json.Stringify, ported from upstream's Lua/Libs/Json.inl.
--
-- A plain table is walked raw: an array when its keys are exactly 1..n,
-- otherwise an object, its keys sorted when beautifying (strings, then
-- integers, then floats). One of bg3le's proxies -- anything carrying a
-- __pairs metamethod, which is what upstream's userdata objects are -- is
-- walked through __pairs only with IterateUserdata, as upstream's are; a
-- function, an entity or a proxy it may not walk becomes tostring() with
-- StringifyInternalTypes and an error without.

local function proxy_meta(v)
  local meta = Ext._Internal.RawGetMetatable(v)
  if type(meta) == "table" and (meta.__pairs ~= nil or meta.__name ~= nil) then
    return meta
  end
  return nil
end

-- What upstream's CheckForRecursion keys on: the object's pointer. A view
-- is made fresh on each read here, so it names the engine object instead.
local function identity(v, meta)
  local id = meta and meta.__bg3leIdentity
  if type(id) == "function" then return id(v) end
  if id ~= nil then return id end
  return v
end

local function json_number(v)
  -- Floats print to full round-trip precision and keep a decimal point
  -- even when whole, as the real extender's writer does: Weight is
  -- 1.350000023841858 and ValueScale 1.0.
  if math.type(v) == "integer" then return tostring(v) end
  -- Upstream's writer has kWriteNanAndInfNullFlag, so NaN and the
  -- infinities come out as null.
  if v ~= v or v == math.huge or v == -math.huge then return "null" end
  local text = string.format("%.17g", v)
  for _, fmt in ipairs({"%.15g", "%.16g"}) do
    local short = string.format(fmt, v)
    if tonumber(short) == v then text = short break end
  end
  if not text:find("[.eE]") then text = text .. ".0" end
  return text
end

local function is_linear_array(t)
  local n = 0
  for k in next, t do
    if math.type(k) ~= "integer" or k < 1 then return false end
    n = n + 1
  end
  for i = 1, n do
    if rawget(t, i) == nil then return false end
  end
  return true
end

local stringify

-- Emits a container: items is a list of { key text or nil, value }.
local function emit(items, array, indent, depth, ctx, out)
  local open, close = array and "[" or "{", array and "]" or "}"
  if #items == 0 then
    out[#out + 1] = open .. close
    return
  end
  local pad = ctx.Beautify and (indent .. "    ") or ""
  out[#out + 1] = open
  for i, item in ipairs(items) do
    if ctx.Beautify then out[#out + 1] = "\n" .. pad end
    if not array then
      out[#out + 1] = json_string(item[1]) .. (ctx.Beautify and ": " or ":")
    end
    stringify(item[2], pad, depth + 1, ctx, out)
    if i < #items then out[#out + 1] = "," end
  end
  if ctx.Beautify then out[#out + 1] = "\n" .. indent end
  out[#out + 1] = close
end

-- A key as upstream writes it; an object key -- an entity, say -- only
-- from a proxy, and only with StringifyInternalTypes.
local function key_text(k, ctx)
  if type(k) == "string" then return k end
  if type(k) == "number" then return tostring(k) end
  if ctx ~= nil and ctx.StringifyInternalTypes and type(k) == "userdata" then
    return tostring(k)
  end
  error("Can only stringify string or number table keys", 0)
end

local function stringify_table(t, indent, depth, ctx, out)
  if ctx.AvoidRecursion then
    if ctx.Seen[t] then out[#out + 1] = '"*RECURSION*"' return end
    ctx.Seen[t] = true
  end

  local items = {}
  if is_linear_array(t) then
    for i = 1, #t do items[i] = {nil, rawget(t, i)} end
    return emit(items, true, indent, depth, ctx, out)
  end

  for k, v in next, t do items[#items + 1] = {k, v} end
  if ctx.Beautify then
    local rank = function(k)
      if type(k) == "string" then return 0 end
      if math.type(k) == "integer" then return 1 end
      return 2
    end
    table.sort(items, function(a, b)
      local ra, rb = rank(a[1]), rank(b[1])
      if ra ~= rb then return ra < rb end
      if ra == 0 then return a[1] < b[1] end
      return a[1] < b[1]
    end)
  end
  for _, item in ipairs(items) do
    local k = item[1]
    if type(k) == "number" and math.type(k) == "float" and ctx.Beautify then
      item[1] = string.format("%f", k)
    else
      item[1] = key_text(k)
    end
  end
  emit(items, false, indent, depth, ctx, out)
end

local function internal_type(v, out, ctx)
  if not ctx.StringifyInternalTypes then
    error("Attempted to stringify unsupported type: "
          .. Ext.Types.GetValueType(v), 0)
  end
  out[#out + 1] = json_string(tostring(v))
end

local function stringify_proxy(v, meta, indent, depth, ctx, out)
  if ctx.IterateUserdata then
    if ctx.LimitDepth ~= -1 and depth > ctx.LimitDepth then
      out[#out + 1] = '"*DEPTH LIMIT EXCEEDED*"'
      return
    end
    -- Entities are values upstream, never marked: one seen twice prints twice.
    if ctx.AvoidRecursion and Ext._Internal.EntityProxyHandle(v) == nil then
      local id = identity(v, meta)
      if ctx.Seen[id] then out[#out + 1] = '"*RECURSION*"' return end
      ctx.Seen[id] = true
    end
    if meta.__pairs ~= nil then
      local container = meta.__bg3leContainer
      local array = container == "array"
      local items = {}
      for k, val in pairs(v) do
        if container ~= nil and ctx.LimitArrayElements ~= -1
           and #items > ctx.LimitArrayElements then
          break
        end
        items[#items + 1] = {not array and key_text(k, ctx) or nil, val}
      end
      -- An object's members come out in name order, as the reference
      -- captures show upstream's do; an array or map keeps its own.
      if container == nil and not meta.__bg3leOrdered then
        table.sort(items, function(a, b) return a[1] < b[1] end)
      end
      return emit(items, array, indent, depth, ctx, out)
    end
  end
  internal_type(v, out, ctx)
end

stringify = function(v, indent, depth, ctx, out)
  if depth > ctx.MaxDepth then
    error("Recursion depth exceeded while stringifying JSON", 0)
  end

  local t = type(v)
  if v == nil then
    out[#out + 1] = "null"
  elseif t == "boolean" then
    out[#out + 1] = tostring(v)
  elseif t == "number" then
    out[#out + 1] = json_number(v)
  elseif t == "string" then
    out[#out + 1] = json_string(v)
  elseif t == "userdata" and Ext._Internal.EnumLabelOf(v) ~= nil then
    -- Upstream's TryStringifyLightCppObject: an enum value is its label.
    out[#out + 1] = json_string(Ext._Internal.EnumLabelOf(v))
  elseif t == "userdata" and Ext._Internal.BitfieldOf(v) ~= nil then
    -- Upstream's StringifyBitfield: the set labels, as an array.
    local parts = {}
    for i, label in ipairs(v.__Labels) do parts[i] = json_string(label) end
    out[#out + 1] = "[" .. table.concat(parts, ",") .. "]"
  elseif t == "table" or t == "userdata" then
    local meta = proxy_meta(v)
    if t == "table" and (meta == nil or meta.__pairs == nil) then
      if ctx.LimitDepth ~= -1 and depth > ctx.LimitDepth then
        out[#out + 1] = '"*DEPTH LIMIT EXCEEDED*"'
      else
        stringify_table(v, indent, depth, ctx, out)
      end
    elseif meta ~= nil then
      stringify_proxy(v, meta, indent, depth, ctx, out)
    else
      internal_type(v, out, ctx)
    end
  else
    internal_type(v, out, ctx)
  end
end

-- Json.Stringify(value[, options]) or the older
-- Json.Stringify(value, beautify[, stringifyInternalTypes[, iterateUserdata]]).
function Ext.Json.Stringify(...)
  local nargs = select("#", ...)
  if nargs < 1 then error("Stringify expects at least one parameter.", 2) end
  if nargs > 4 then error("Stringify expects at most three parameters.", 2) end
  local v, opts, internal, iterate = ...

  local ctx = {
    Beautify = true, StringifyInternalTypes = false, IterateUserdata = false,
    AvoidRecursion = false, MaxDepth = 64, LimitDepth = -1,
    LimitArrayElements = -1, Seen = {},
  }
  if type(opts) == "table" then
    for _, k in ipairs({"Beautify", "StringifyInternalTypes", "IterateUserdata",
                        "AvoidRecursion"}) do
      if opts[k] ~= nil then ctx[k] = opts[k] == true end
    end
    for _, k in ipairs({"MaxDepth", "LimitDepth", "LimitArrayElements"}) do
      if type(opts[k]) == "number" then ctx[k] = math.floor(opts[k]) end
    end
    if ctx.MaxDepth > 64 then ctx.MaxDepth = 64 end
  elseif nargs >= 2 then
    ctx.Beautify = opts == true
    if nargs >= 3 then ctx.StringifyInternalTypes = internal == true end
    if nargs >= 4 then ctx.IterateUserdata = iterate == true end
  end

  -- Upstream's binary form, through its own writer. A value it cannot walk
  -- (one of bg3le's object proxies) goes through the text form first.
  if type(opts) == "table" and opts.Binary == true then
    local ok, blob = pcall(Ext._Internal.JsonBinaryEncode, v)
    if ok then return blob end
    ctx.Beautify = false
    local out = {}
    local done, err = pcall(stringify, v, "", 0, ctx, out)
    if not done then error(err, 2) end
    return Ext._Internal.JsonBinaryEncode(Ext.Json.Parse(table.concat(out)))
  end

  local out = {}
  local ok, err = pcall(stringify, v, "", 0, ctx, out)
  if not ok then error(err, 2) end
  return table.concat(out)
end

-- Upstream's BuiltinLibrary.lua.
function Ext.DumpExport(v)
  return Ext.Json.Stringify(v, {Beautify = true, StringifyInternalTypes = true,
                                IterateUserdata = true, AvoidRecursion = true})
end
function Ext.Dump(v) Ext.Log.Print(Ext.DumpExport(v)) end
function Ext.DumpShallow(v)
  Ext.Log.Print(Ext.Json.Stringify(v, {
    Beautify = true, StringifyInternalTypes = true, IterateUserdata = true,
    AvoidRecursion = true, LimitDepth = 1, LimitArrayElements = 3,
  }))
end

Ext.Table = {
  Find = function(tbl, value)
    for k, v in pairs(tbl) do
      if v == value then return v, k end
    end
    return nil
  end
}
table.find = Ext.Table.Find

-- Ext.Math is registered from C against the same glm the real extender
-- uses; see src/vendor/math.cpp. The Lua approximations that used to stand
-- in here were not merely incomplete, they disagreed: Round(-2.5) gave -2
-- where glm's gives -3.

-- ---- Ext.Json.Parse ----
--
-- Upstream's LuaParse, through rapidjson (src/vendor/json_binary.cpp):
-- objects become tables keyed by string, arrays tables keyed from one, and a
-- null leaves a hole, as upstream's does.
Ext.Json.Parse = Ext._Internal.JsonParse

-- ---- Ext.IO ----
--
-- LoadFile reads under the profile's Script Extender directory, as upstream
-- does, or the game's Data directory with a context of "data"; SaveFile
-- writes under Script Extender only. Both
-- refuse a path that climbs out of its root, as upstream's does.
Ext.IO = {}

function Ext.IO.LoadFile(path, context)
  return Ext._Internal.LoadFile(path, context)
end

function Ext.IO.SaveFile(path, contents)
  return Ext._Internal.SaveFile(path, tostring(contents), false)
end

function Ext.IO.AppendFile(path, contents)
  return Ext._Internal.SaveFile(path, tostring(contents), true)
end

-- bg3le's own, not upstream's: native plugins (include/bg3le_plugin.h) and
-- the settings they register. Set saves the plugin's settings file.
Ext.Plugins = {
  List = function() return Ext._Internal.PluginList() end,
  GetSettings = function(name) return Ext._Internal.PluginSettings(tostring(name)) end,
  Get = function(name, id) return Ext._Internal.PluginGet(tostring(name), tostring(id)) end,
  Set = function(name, id, value)
    return Ext._Internal.PluginSet(tostring(name), tostring(id), value)
  end,
}

-- Upstream's: consulted by the engine's FileReader (src/vendor/path_override.cpp),
-- keyed by absolute data path.
function Ext.IO.AddPathOverride(path, overridePath)
  if not Ext._Internal.PathOverrideAdd(tostring(path), tostring(overridePath)) then
    Ext.Log.PrintError("bg3le: path overrides are unavailable; the engine's FileReader is not hooked")
  end
end

function Ext.IO.GetPathOverride(path)
  return Ext._Internal.PathOverrideGet(tostring(path))
end

-- ---- Ext.Debug ----
--
-- Crash has no crash reporter to exercise, so it refuses rather than taking
-- the game down.
Ext.Debug = {}

-- "DeveloperMode" in ScriptExtenderSettings.json, off by default, as
-- upstream's release builds read it.
local developer_mode = Ext._Internal.SettingsFlag("DeveloperMode", false)
function Ext.Debug.IsDeveloperMode() return developer_mode end

function Ext.Debug.DebugBreak()
  local server = Ext._Internal.DebugBreak
  if server ~= nil then return server() end
  Ext.Log.PrintWarning("Ext.Debug.DebugBreak: no debugger attached")
end

function Ext.Debug.DumpStack()
  local level = 2
  while true do
    local info = debug.getinfo(level, "Sln")
    if info == nil then break end
    Ext.Log.Print(string.format("%d: %s %s:%d", level - 1,
      info.name or info.what or "?", info.short_src or "?",
      info.currentline or 0))
    level = level + 1
  end
end

function Ext.Debug.DebugDumpLifetimes()
  -- Upstream dumps its Lua lifetime pools. bg3le hands out plain values
  -- rather than lifetime-scoped proxies, so there is nothing to dump; an
  -- empty report is the honest one.
  Ext.Log.Print("bg3le does not use lifetime-scoped references")
end

-- Ext.Debug.GenerateIdeHelpers(builtinOnly)
--
-- One LuaLS annotation block and one stub per Osiris function, written where
-- upstream writes it: under the base module's Story/RawFiles/Lua in the
-- game's Data directory, which is the tree an editor has open.
--
-- Upstream refuses unless Osiris is available, because the function database
-- is what it reads; so does this.
function Ext.Debug.GenerateIdeHelpers(builtinOnly)
  local base = Ext.Mod.GetBaseMod()
  local dir = base and base.Info and base.Info.Directory
  if dir == nil or dir == "" then
    error("bg3le: Ext.Debug.GenerateIdeHelpers cannot name the base module's "
          .. "directory", 2)
  end

  local text = Ext._Internal.OsiIdeHelpers(builtinOnly == true)
  if text == nil or #text == 0 then
    error("bg3le: Ext.Debug.GenerateIdeHelpers() can only be called when "
          .. "Osiris is available", 2)
  end

  local path, err = Ext._Internal.WriteDataFile(
    "Mods/" .. dir .. "/Story/RawFiles/Lua/OsiIdeHelpers.lua", text)
  if path == nil then
    error("bg3le: Ext.Debug.GenerateIdeHelpers could not save: "
          .. tostring(err), 2)
  end

  Ext.Log.Print("bg3le: wrote " .. #text .. " bytes of Osiris IDE helpers to "
                .. path)
  return path
end

-- Upstream's crashes only in a debug build; a release build does nothing.
function Ext.Debug.Crash() end

-- Ext.Debug.Reset()
--
-- Tears both Lua contexts down, builds them again and reloads every mod,
-- which is what a mod author editing a script wants instead of restarting a
-- game that takes ninety seconds to load here.
--
-- Deferred to the next tick, and upstream's is asynchronous for the same
-- reason: this is called from the state being closed. Nothing after the call
-- in the caller's own script will run in the state that made it, so there is
-- nothing useful to return -- subscribe to Ext.Events.ResetCompleted in the
-- new one instead.
--
-- Only the Lua is reset. The story is still loaded, the Osiris node hooks are
-- still installed, and whatever a mod wrote into the engine before the reset
-- is still written -- the same bargain upstream offers.
function Ext.Debug.Reset()
  Ext.Log.Print("bg3le: reset requested; both contexts rebuild on the next "
                .. "tick")
  Ext._Internal.RequestReset()
end

-- Called in each fresh context once the reload is done.
--
-- Upstream's order, from ScriptExtender::ResetLuaState: ModuleResume, then
-- the session pair if a session is actually up, then ResetCompleted. The
-- session pair is what a mod rebuilds its own state in -- Mod Configuration
-- Menu's ResetCompleted handler reaches for a global that SessionLoaded
-- creates, and firing ResetCompleted alone left it indexing a nil.
--
-- StatsLoaded is deliberately not re-fired, as upstream does not: stats did
-- not reload, and a mod's stats pass is not idempotent.
-- ecl::ScriptExtender::OnGameStateChanged's event, on the client's thread.
function Ext._Internal.ClientStateChanged(from, to)
  Ext._Internal.FireEvent("GameStateChanged", { FromState = from, ToState = to })
end

-- esv::ScriptExtender::OnGameStateChanged's, from the server tick
-- (lua_tick); BoHReforged builds its state on Running.
Ext._Internal.ServerStateChanged = Ext._Internal.ClientStateChanged

function Ext._Internal.AfterReset()
  Ext._Internal.FireEvent("ModuleResume")

  local state = Ext.Utils.GetGameState()
  if state == "Paused" or state == "Running" then
    Ext._Internal.FireEvent("SessionLoading")
    Ext._Internal.FireEvent("SessionLoaded")
  end

  Ext._Internal.FireEvent("ResetCompleted")
end

function Ext.Debug.SetEntityRuntimeCheckLevel(level)
  if type(level) ~= "number" then
    error("Ext.Debug.SetEntityRuntimeCheckLevel expects a number", 2)
  end
  Ext._Internal.EntityCheckLevel = level
end

-- ---- Ext.Osiris ----
--
-- Osiris is already bound, so a listener is a subscription on the call
-- bg3le makes when the engine raises it.
Ext.Osiris = {}

local osiris_listeners = {}

local osiris_events = {
  before = true, after = true, beforeDelete = true, afterDelete = true
}

-- A user query runs through its QRY_X__DEF__ node, which is where upstream
-- attaches a QRY_X listener too.
local function osiris_listener_name(name)
  if name:find("^QRY_") and not name:find("__DEF__$")
     and Ext._Internal.StoryFunction(name .. "__DEF__") ~= nil then
    return name .. "__DEF__"
  end
  return name
end

function Ext.Osiris.RegisterListener(name, arity, event, handler)
  if type(name) ~= "string" or type(handler) ~= "function" then
    error("Ext.Osiris.RegisterListener(name, arity, event, handler)", 2)
  end
  if not osiris_events[event] then
    error("Ext.Osiris.RegisterListener: event must be one of before, after, " ..
          "beforeDelete, afterDelete; got " .. tostring(event), 2)
  end

  -- Watching costs two patched vtable slots and an index of Osiris'
  -- function database, so it does not start until someone subscribes.
  if not Ext._Internal.WatchOsiris() then
    error("Ext.Osiris.RegisterListener: bg3le could not start watching " ..
          "Osiris (no story loaded yet?)", 2)
  end

  -- An engine call has no node; it is seen at the DIV boundary instead,
  -- and like upstream it has no delete triggers.
  if Ext._Internal.WatchOsirisCall(name, arity)
     and (event == "beforeDelete" or event == "afterDelete") then
    Ext.Log.PrintError("Couldn't register Osiris subscriber for " .. name
      .. "/" .. tostring(arity) .. ": Delete triggers not supported on events.")
    return
  end

  name = osiris_listener_name(name)
  local key = name .. "/" .. tostring(arity) .. "/" .. tostring(event)
  osiris_listeners[key] = osiris_listeners[key] or {}
  table.insert(osiris_listeners[key], handler)
  return handler
end

function Ext.Osiris.UnregisterListener(name, arity, event, handler)
  name = osiris_listener_name(name)
  local key = name .. "/" .. tostring(arity) .. "/" .. tostring(event)
  local list = osiris_listeners[key]
  if list == nil then return false end
  for i, h in ipairs(list) do
    if h == handler then table.remove(list, i) return true end
  end
  return false
end

-- Called from the Osiris bridge when the engine raises a call.
function Ext._Internal.FireOsirisListener(name, arity, event, ...)
  local list = osiris_listeners[name .. "/" .. tostring(arity) .. "/"
                               .. tostring(event)]
  if list == nil then return end
  for _, handler in ipairs(list) do
    local ok, err = xpcall(handler, debug.traceback, ...)
    if not ok then
      Ext.Log.PrintError("Osiris event handler failed: " .. tostring(err))
    end
  end
end

-- ---- what still needs the engine ----
--
-- These names exist because a mod calls them by name and a missing one is
-- "attempt to call a nil value" with nothing to act on. Each says what is
-- missing rather than returning an empty result, because an empty result
-- is indistinguishable from a real answer and would send a mod author
-- looking in the wrong place.
local function needs(what)
  return function()
    error("bg3le: " .. what, 2)
  end
end

-- ---- Ext.Types ----
--
-- bg3se's type registry, over the same property maps bg3le already
-- compiles in. TypeInformation's keys are its own, from
-- GameDefinitions/Base/BaseTypeInformation.h, so a mod reading
-- info.Members or info.Kind finds what it expects.
Ext.Types = {}


local type_names_cache

-- ---- Ext.IMGUI ----
--
-- Upstream's seven module functions and the widget objects NewWindow hands
-- back. The names and shapes are upstream's exactly -- a mod written against
-- bg3se has to run here unmodified -- and the implementation is bg3se's own
-- widget tree, which is compiled into libbg3le.so; see
-- reference/IMGUI-ASSESSMENT.md.
--
-- A widget is a handle, not a pointer: it lives in a pool that can move it,
-- so the address and class are resolved on every access rather than kept.
-- Properties go through the same field machinery a component or a static data
-- resource does, so every property bg3se's maps describe is readable and
-- writable by name. The Add* methods are P_FUN entries, which the field
-- tables deliberately leave out, so they are bound here.
--
-- Client-side only, as upstream has it.
Ext.IMGUI = {}

-- Which Add* methods a widget that can hold children offers. One table
-- rather than thirty closures, and the C side takes the kind by name for the
-- same reason: from Lua every one of them is a label in and a widget out, and
-- anything else the widget needs is set through its properties afterwards.
local IMGUI_ADD = {
  -- Containers.
  AddGroup = "Group", AddCollapsingHeader = "CollapsingHeader",
  AddTabBar = "TabBar", AddTree = "Tree", AddTable = "Table",
  AddPopup = "Popup", AddChildWindow = "ChildWindow", AddMenu = "Menu",
  -- Text.
  AddText = "Text", AddTextLink = "TextLink", AddBulletText = "BulletText",
  AddSeparatorText = "SeparatorText",
  -- Layout.
  AddSpacing = "Spacing", AddNewLine = "NewLine", AddSeparator = "Separator",
  AddDummy = "Dummy",
  -- Buttons and selection.
  AddButton = "Button", AddSelectable = "Selectable",
  AddImageButton = "ImageButton", AddCheckbox = "Checkbox",
  AddRadioButton = "RadioButton", AddCombo = "Combo",
  -- Images.
  AddImage = "Image", AddIcon = "Icon",
  -- Numbers and text entry.
  AddInputText = "InputText", AddDrag = "Drag", AddDragInt = "DragInt",
  AddSlider = "Slider", AddSliderInt = "SliderInt",
  AddInputScalar = "InputScalar", AddInputInt = "InputInt",
  AddColorEdit = "ColorEdit", AddColorPicker = "ColorPicker",
  AddProgressBar = "ProgressBar",
}

-- The methods that are not Add*. Dispatched by name on the C side, which is
-- where the widget's own type decides whether it has them; a name that this
-- widget does not offer reports so rather than doing nothing.
local IMGUI_METHOD = {
  Destroy = true, GetStyle = true, SetStyle = true, GetColor = true,
  SetColor = true, Activate = true, Tooltip = true,
  RemoveChild = true, DetachChild = true, AttachChild = true,
  RemoveAllChildren = true, GetChildren = true,
  SetPos = true, SetSize = true, SetSizeConstraints = true,
  SetContentSize = true, SetCollapsed = true, SetFocus = true,
  SetScroll = true, SetBgAlpha = true,
  AddMainMenu = true, AddTabItem = true, SetOpen = true, AddRow = true,
  AddColumn = true, AddCell = true, Open = true, AddItem = true,
}

local imgui_widget = {}
local make_widget

-- Properties upstream declares as a getter/setter pair (P_GETTER_SETTER):
-- read and written through the widget's Get<Name>/Set<Name>.
local IMGUI_GETSET = {Text = true, DragDropType = true, ParentElement = true}

local function type_of_widget(handle)
  local _, short = Ext._Internal.ImguiObject(handle)
  return short
end

-- The address and class of a widget right now, or nil if it is gone.
local function widget_object(handle)
  local addr, short = Ext._Internal.ImguiObject(handle)
  if addr == nil then return nil end
  -- The field tables are keyed by the qualified class name.
  return addr, Ext._Internal.ClassName("extui::" .. short) or short
end

-- Callbacks.
--
-- The function stays here, filed under the id the C side hands back and
-- writes into the widget; see src/vendor/imgui_events.cpp for why it is not
-- held over there. A widget fires on the render thread, so nothing is called
-- at that moment: the arguments are queued and drained from this context's
-- tick, below.
local imgui_callbacks = {}
-- Upstream's UserData holds any Lua value; it lives here, by widget handle.
local imgui_userdata = {}

-- Which widget table to hand a callback, so the same widget arrives as the
-- same table each time and `self` behaves. Weak-valued, so a widget the mod
-- has dropped does not keep its table alive.
local imgui_widgets = setmetatable({}, {__mode = "v"})
-- Each widget's Handle and Callbacks. Widgets are userdata, as upstream's are
-- (mods tell one widget from a list of them by type()), so nothing lives on
-- the widget itself.
-- (On the metatable rather than a local: the prelude is at Lua's local limit.)
imgui_widget.__bg3leState = setmetatable({}, {__mode = "k"})

local imgui_methods = {}

function imgui_methods:Destroy()
  local handle = imgui_widget.__bg3leState[self].Handle
  for _, id in pairs(imgui_widget.__bg3leState[self].Callbacks or {}) do
    imgui_callbacks[id] = nil
  end
  imgui_widgets[handle] = nil
  imgui_userdata[handle] = nil
  return Ext._Internal.ImguiDestroy(handle)
end

imgui_widget.__index = function(self, key)
  local method = imgui_methods[key]
  if method ~= nil then return method end

  local handle = imgui_widget.__bg3leState[self].Handle
  if key == "Handle" then return handle end
  if key == "UserData" then return imgui_userdata[handle] end

  -- An Add* call: make the child and hand back a widget for it.
  local kind = IMGUI_ADD[key]
  if kind ~= nil then
    return function(_, ...)
      local child = Ext._Internal.ImguiAdd(handle, kind, ...)
      if child == nil then
        error("bg3le: " .. key .. " is not available on this widget", 2)
      end
      return make_widget(child)
    end
  end

  -- An event reads back as the function that was set, which is what
  -- upstream's delegate does.
  local registered = imgui_widget.__bg3leState[self].Callbacks
  if registered ~= nil and registered[key] ~= nil then
    return imgui_callbacks[registered[key]]
  end

  local addr, class = widget_object(handle)

  -- A property of this widget's own class, before a method of that name.
  --
  -- The two namespaces overlap: Popup has an Open() method and Window has an
  -- Open property, and a flat method table made `window.Open` a function.
  -- Whether this class declares the property is what decides, which is the
  -- same thing upstream's per-class property maps decide.
  -- Upstream's Children getter: the child widgets, not their raw handles.
  if key == "Children" then
    local children = Ext._Internal.ImguiChildren(handle) or {}
    for i, child in ipairs(children) do children[i] = make_widget(child) end
    return children
  end

  -- Upstream's getter/setter properties have no field to read.
  if IMGUI_GETSET[key] then
    local ok, value, isWidget = Ext._Internal.ImguiCall(handle, "Get" .. key)
    if ok then
      if isWidget then return make_widget(value) end
      return value
    end
  end

  if addr ~= nil then
    local value, err = Ext._Internal.ObjectGetField(addr, class, key)
    if err == nil then return value end
    -- A struct or container, such as Table.ColumnDefs: read as a component's
    -- is, with its elements writable through to the widget.
    local kind = Ext._Internal.ObjectFieldInfo(class, key)
    if kind == "struct" or kind == "array" or kind == "map" then
      return Ext._Internal.ReadObjectPath(addr, class, key, kind)
    end
  end

  if IMGUI_METHOD[key] then
    if key == "Destroy" then return imgui_methods.Destroy end
    if key == "GetChildren" then
      return function()
        local children = Ext._Internal.ImguiChildren(handle)
        for i, child in ipairs(children) do children[i] = make_widget(child) end
        return children
      end
    end

    return function(_, ...)
      local ok, value, isWidget = Ext._Internal.ImguiCall(handle, key, ...)
      if not ok then
        error(string.format("bg3le: %s is not a method on this widget (%s)",
                            tostring(key), tostring(type_of_widget(handle))), 2)
      end
      if isWidget then return make_widget(value) end
      return value
    end
  end

  return nil
end

imgui_widget.__newindex = function(self, key, value)
  local handle = imgui_widget.__bg3leState[self].Handle
  if key == "UserData" then
    imgui_userdata[handle] = value
    return
  end

  -- A function can only be an event handler: no other property of a widget
  -- takes one, and the C side refuses a name that is not a delegate.
  local registered = imgui_widget.__bg3leState[self].Callbacks
  if type(value) == "function" or registered ~= nil and registered[key] then
    if registered == nil then
      registered = {}
      imgui_widget.__bg3leState[self].Callbacks = registered
    end

    local was = registered[key]
    if was ~= nil then
      imgui_callbacks[was] = nil
      registered[key] = nil
      Ext._Internal.ImguiClearCallback(handle, key)
    end

    if value == nil then return end

    local id = Ext._Internal.ImguiSetCallback(handle, key)
    if id == nil then
      error(string.format("bg3le: %s is not an event on this widget",
                          tostring(key)), 2)
    end
    imgui_callbacks[id] = value
    registered[key] = id
    return
  end

  -- nil to an event with no handler clears it, as upstream's delegate does;
  -- the C side is what knows whether the name is an event.
  if value == nil and Ext._Internal.ImguiSetCallback(handle, key) ~= nil then
    Ext._Internal.ImguiClearCallback(handle, key)
    return
  end

  if IMGUI_GETSET[key] and value ~= nil then
    -- Upstream's limit: the type has to fit ImGuiPayload::DataType.
    if key == "DragDropType" and #tostring(value) >= 33 then
      error("Drag drop type must be at most 33 characters", 2)
    end
    if Ext._Internal.ImguiCall(handle, "Set" .. key, tostring(value)) then return end
  end

  local addr, class = widget_object(handle)
  if addr == nil then
    error("bg3le: this widget no longer exists", 2)
  end

  local called, ok, err = pcall(Ext._Internal.ObjectSetField, addr, class,
                                key, value)
  if not called then
    error(string.format("bg3le: %s.%s = %s: %s", tostring(class), tostring(key),
                        tostring(value), tostring(ok)), 2)
  end
  if not ok then error("bg3le: " .. tostring(err), 2) end
end

imgui_widget.__name = "ImguiHandle"

make_widget = function(handle)
  local existing = imgui_widgets[handle]
  if existing ~= nil then return existing end

  local widget = Ext._Internal.NewObjectProxy(imgui_widget)
  imgui_widget.__bg3leState[widget] = {Handle = handle}
  imgui_widgets[handle] = widget
  return widget
end

function Ext.IMGUI.NewWindow(name)
  -- A number is its text, as upstream's luaL_checkstring takes it.
  if type(name) == "number" then name = tostring(name) end
  if type(name) ~= "string" then
    error("Ext.IMGUI.NewWindow(name) takes a name", 2)
  end

  local handle = Ext._Internal.ImguiNewWindow(name)
  if handle == nil then
    local wanted, started, ready = Ext._Internal.ImguiStatus()
    if not wanted then
      error("bg3le: the ImGui overlay is off (BG3LE_IMGUI=0); unset it to turn it "
            .. "on", 2)
    end
    error(string.format(
      "bg3le: Ext.IMGUI.NewWindow could not create a window "
      .. "(started %s, render backend ready %s); see the imgui lines in the "
      .. "extender log", tostring(started), tostring(ready)), 2)
  end
  return make_widget(handle)
end

function Ext.IMGUI.EnableDemo(enabled)
  Ext._Internal.ImguiEnableDemo(enabled ~= false)
end

-- Delivering what the widgets queued.
--
-- Drained one at a time and re-checked, because a handler is free to build or
-- destroy widgets, and an error in one handler must not swallow the rest.
local function imgui_pump()
  while true do
    local id, handle, arg = Ext._Internal.ImguiTakeEvent()
    if id == nil then return end

    local fn = imgui_callbacks[id]
    if fn ~= nil then
      local ok, err = xpcall(fn, debug.traceback, make_widget(handle), arg)
      if not ok then
        Ext.Log.PrintError("Error while dispatching user function call: "
                           .. tostring(err))
      end
    end
  end
end

-- Called from the tick pump rather than subscribed here: Ext.Events does not
-- exist yet at this point in the prelude, and a widget callback should run
-- before the handlers that may be waiting on what it changed.
Ext._Internal.ImguiPump = imgui_pump

function Ext.IMGUI.GetViewportSize()
  local size = Ext._Internal.ImguiViewportSize()
  if size == nil then
    error("bg3le: the ImGui overlay has no viewport yet", 2)
  end
  return size
end

-- The manager's own font table, which is what upstream's LoadFont writes to.
-- A path relative to the game's data, or empty for the default the language
-- picks.
function Ext.IMGUI.LoadFont(name, path, size)
  -- A number is its text, as upstream's luaL_checkstring takes it.
  if type(name) == "number" then name = tostring(name) end
  if type(name) ~= "string" then
    error("Ext.IMGUI.LoadFont(name, path, size) takes a name", 2)
  end
  return Ext._Internal.ImguiLoadFont(name, path or "", size or 0.0)
end

function Ext.IMGUI.SetUIScaleMultiplier(scale)
  Ext._Internal.ImguiSetUIScale(scale)
end

function Ext.IMGUI.SetFontScaleMultiplier(scale)
  Ext._Internal.ImguiSetFontScale(scale)
end

-- Upstream deprecated this one and warns; it does not scale anything.
function Ext.IMGUI.SetScale()
  Ext.Log.Print("Ext.IMGUI.SetScale() is deprecated; UI scaling is managed "
                .. "by the extender")
end


-- ---- Ext.Enums ----
--
-- Every enum and bitfield bg3se describes, by its Lua name, each one a table
-- reached by label or by numeric value:
--
--   Ext.Enums.ClientGameState.Menu   -->  "Menu"
--   Ext.Enums.ClientGameState[3]     -->  "Menu"
--
-- Upstream's entries are EnumValue objects rather than strings, and the
-- difference is deliberate. bg3le reads an enum-typed field as its label --
-- that is what reference/ verifies against the real extender, attribute for
-- attribute -- and a comparison is what a mod does with these:
-- Mod Configuration Menu asks
-- `Ext.Utils.GetGameState() == Ext.Enums.ClientGameState["Menu"]`. Two
-- strings compare equal there; a proxy object against a string never would,
-- because Lua's __eq does not fire unless both sides are the same type. The
-- label is the form that makes the comparison mean what it says.
--
-- Built on first use and kept. There are hundreds of enums and a mod
-- generally wants one, so each is filled in when it is named.
local enum_names = nil
local enum_tables = {}

local function build_enum_names()
  if enum_names ~= nil then return enum_names end
  enum_names = {}
  for i = 0, Ext._Internal.EnumCount() - 1 do
    local name, isBitmask = Ext._Internal.EnumAt(i)
    if name ~= nil then enum_names[name] = isBitmask and "bitfield" or "enum" end
  end
  return enum_names
end

local function build_enum(name)
  local made = {}
  local bitfield = build_enum_names()[name] == "bitfield"
  local i = 0
  while true do
    local label, value = Ext._Internal.EnumValueAt(name, i)
    if label == nil then break end
    -- Both directions, as upstream does: the label names the entry and the
    -- numeric value reaches the same one. A bitfield's entries are
    -- BitfieldValues, as upstream's RegisterEnumeration makes them.
    local entry = bitfield and Ext._Internal.BitfieldValue(name, value) or label
    made[label] = entry
    made[value] = entry
    i = i + 1
  end
  return made
end

Ext.Enums = setmetatable({}, {
  __index = function(_, name)
    if type(name) ~= "string" then return nil end
    local found = enum_tables[name]
    if found ~= nil then return found end
    if not build_enum_names()[name] then return nil end

    found = build_enum(name)
    enum_tables[name] = found
    return found
  end,
  -- Iterating yields every enum, which is what a mod listing them expects;
  -- each is built as it is reached.
  __pairs = function(self)
    local key
    return function()
      key = next(build_enum_names(), key)
      if key == nil then return nil end
      return key, self[key]
    end, self, nil
  end,
})

function Ext.Types.GetAllTypes()
  if type_names_cache == nil then
    type_names_cache = Ext._Internal.TypeInfoNames()
    if #type_names_cache == 0 then type_names_cache = Ext._Internal.TypeNames() end
    table.sort(type_names_cache)
  end
  -- A copy: upstream returns a fresh array, and a caller sorting or
  -- clearing it must not corrupt the registry.
  local out = {}
  for i, name in ipairs(type_names_cache) do out[i] = name end
  return out
end

-- Upstream's TypeInformation, from its own registry (src/vendor/type_info.cpp),
-- as a read-only view whose refs are views in turn.
do
local TYPE_INFO_KEYS = {
  "TypeName", "Kind", "NativeName", "KeyType", "ElementType", "ParentType",
  "Members", "Methods", "HasWildcardProperties", "EnumValues",
  "ReturnValues", "Params", "VarargParams", "VarargsReturn", "IsBitfield",
  "IsBuiltin", "ModuleRole", "ComponentName", "SystemName",
}
local TYPE_INFO_KEYSET = {}
for _, k in ipairs(TYPE_INFO_KEYS) do TYPE_INFO_KEYSET[k] = true end
local type_info_views = {}

local function type_info_view(at)
  if at == nil then return nil end
  local view = type_info_views[at]
  if view ~= nil then return view end
  local loaded = {}
  local function get(k)
    if loaded[k] ~= nil then return loaded[k] end
    local value
    if k == "Members" or k == "Methods" then
      value = {}
      for name, a in pairs(Ext._Internal.TypeInfoEach(at, k == "Members" and 0 or 1)) do
        if a ~= 0 then value[name] = type_info_view(a) end
      end
    elseif k == "EnumValues" then
      value = Ext._Internal.TypeInfoEach(at, 2)
    elseif k == "Params" or k == "ReturnValues" then
      value = {}
      -- An unbound ref is nil, as upstream pushes it.
      for i, a in ipairs(Ext._Internal.TypeInfoEach(at, k == "Params" and 3 or 4)) do
        if a ~= 0 then value[i] = type_info_view(a) end
      end
    else
      value = (Ext._Internal.TypeInfoFields(at) or {})[k]
      if k == "KeyType" or k == "ElementType" or k == "ParentType" then
        value = type_info_view(value)
      end
    end
    loaded[k] = value
    return value
  end
  view = Ext._Internal.NewObjectProxy({
    __index = function(_, k)
      if TYPE_INFO_KEYSET[k] then return get(k) end
      return nil
    end,
    __newindex = function(_, k)
      error("Cannot set property " .. tostring(k) .. " of TypeInformation", 2)
    end,
    __pairs = function()
      local i = 0
      return function()
        while true do
          i = i + 1
          local k = TYPE_INFO_KEYS[i]
          if k == nil then return nil end
          local v = get(k)
          if v ~= nil then return k, v end
        end
      end
    end,
    __name = "TypeInformation",
    __bg3leIdentity = string.format("p:%x", at),
  })
  type_info_views[at] = view
  return view
end

function Ext.Types.GetTypeInfo(typeName)
  if type(typeName) ~= "string" then return nil end
  local view = type_info_view(Ext._Internal.TypeInfoAt(typeName))
  if view == nil or view.Kind == "Unknown" then return nil end
  return view
end
end

function Ext.Types.GetObjectType(object)
  local t = type(object)
  if t ~= "table" and t ~= "userdata" then return nil end
  -- Only the objects bg3le tagged carry a type: its views, and entities.
  local meta = Ext._Internal.RawGetMetatable(object)
  if meta ~= nil and meta.__name ~= nil then return meta.__name end
  return nil
end

function Ext.Types.TypeOf(object)
  local name = Ext.Types.GetObjectType(object)
  if name == nil then return nil end
  return Ext.Types.GetTypeInfo(name)
end

-- Upstream's: an unknown type name is an error; a type is also each of its
-- ancestors.
function Ext.Types.IsA(object, typeName)
  local expected = Ext.Types.GetTypeInfo(typeName)
  if expected == nil then error("No such type: " .. tostring(typeName), 2) end
  local ty = Ext.Types.TypeOf(object)
  while ty ~= nil do
    if ty == expected then return true end
    ty = ty.ParentType
  end
  return false
end

-- Upstream's GetDebugName: an entity is "Entity", an object its struct's
-- name (base type "CppObject"), and anything else Lua's own type name.
local function value_type(object, base)
  local t = type(object)
  if t == "userdata" and Ext._Internal.EntityProxyHandle(object) ~= nil then
    return "Entity"
  elseif t == "table" or t == "userdata" then
    local meta = Ext._Internal.RawGetMetatable(object)
    -- An enum value: its enum's name, and "Enum" as the base, as upstream's.
    if type(meta) == "table" and meta.__bg3leEnumName ~= nil then
      return base and "Enum" or meta.__bg3leEnumName
    end
    if type(meta) == "table" and meta.__bg3leBitfieldName ~= nil then
      return base and "Bitfield" or meta.__bg3leBitfieldName
    end
    if type(meta) == "table" and type(meta.__name) == "string"
       and meta.__name ~= "EntityProxy" then
      return base and "CppObject" or meta.__name
    end
  end
  return t
end

function Ext.Types.GetValueType(object)
  return value_type(object, false)
end

function Ext.Types.GetBaseValueType(object)
  return value_type(object, true)
end

function Ext.Types.Validate(object)
  -- Upstream walks an object's property map checking every member reads
  -- back. bg3le's objects are plain values that were already read, so
  -- there is nothing left that can fail; true is the honest answer for
  -- anything it produced, and a non-table is not one.
  return Ext._Internal.Walkable(object)
end

-- Upstream's Serialize turns an engine object proxy into a plain Lua
-- table, and Unserialize applies one back; neither touches JSON. An
-- earlier version here stringified, which would have handed a mod a string
-- where it expected a table.
local function deep_plain(value, seen)
  -- An enum value serializes as its label, as upstream's does.
  local label = Ext._Internal.EnumLabelOf(value)
  if label ~= nil then return label end
  -- A bitfield stays one, as upstream's push(L, *obj) leaves it.
  if Ext._Internal.BitfieldOf(value) ~= nil then return value end
  if not Ext._Internal.Walkable(value) then return value end
  if seen[value] then return seen[value] end

  local out = {}
  seen[value] = out
  for k, v in pairs(value) do
    if type(v) ~= "function" then out[k] = deep_plain(v, seen) end
  end
  return out
end

function Ext.Types.Serialize(object)
  return deep_plain(object, {})
end

-- Upstream's Unserialize writes nested objects in place: a struct field by
-- its fields, an array resized to the list, a map by its keys.
local function unserialize_into(object, values)
  local meta = Ext._Internal.RawGetMetatable(object)
  local assign = type(meta) == "table" and meta.__bg3leAssign
  if assign then
    assign(values)
    return
  end
  for k, v in pairs(values) do
    local cur
    if type(v) == "table" then
      local ok, value = pcall(function() return object[k] end)
      if ok then cur = value end
    end
    if type(v) == "table" and Ext._Internal.Walkable(cur) then
      local okLen, n = false, nil
      if next(v) == nil or v[1] ~= nil then
        okLen, n = pcall(function() return #cur end)
      end
      if okLen and n ~= #v then
        object[k] = v  -- resizes; the setter fills struct elements
      else
        unserialize_into(cur, v)
      end
    else
      local ok, err = pcall(function() object[k] = v end)
      -- A new map key: its value went in whole, or failed, and either way
      -- its structs are filled in place.
      local added = type(v) == "table" and object[k] or nil
      if Ext._Internal.Walkable(added) then
        unserialize_into(added, v)
      elseif not ok then
        error(err, 0)
      end
    end
  end
end

function Ext.Types.Unserialize(object, values)
  if type(values) ~= "table" then
    error("Ext.Types.Unserialize expects a table", 2)
  end

  -- Through the same setter an assignment uses, with one difference that
  -- upstream makes too: its Unserialize writes an OverrideableProperty's
  -- Value and leaves IsOverridden as it was, where assigning the property
  -- marks it overridden. So the setter is told which this is.
  local outer = Ext._Internal.Unserializing
  Ext._Internal.Unserializing = true
  local ok, err = pcall(unserialize_into, object, values)
  Ext._Internal.Unserializing = outer
  if not ok then error(err, 2) end
  return object
end

-- Fills the struct elements of an array just assigned from a list: the
-- whole-array write sizes it, but only writes plain elements.
function Ext._Internal.FillStructElements(view, list)
  if not Ext._Internal.Walkable(view) then return end
  for i, item in ipairs(list) do
    if type(item) == "table" then
      local element = view[i]
      if Ext._Internal.Walkable(element) then Ext.Types.Unserialize(element, item) end
    end
  end
end

-- Upstream's Construct checks the type and stops there -- its body is a
-- TODO -- so a constructible type returns nothing, and the rest raise its
-- messages.
function Ext.Types.Construct(typeName)
  typeName = tostring(typeName)
  local info = Ext.Types.GetTypeInfo(typeName)
  if info == nil then
    if Ext.Enums[typeName] ~= nil then
      error("Unable to construct non-object type '" .. typeName .. "'", 0)
    end
    error("Unknown type name '" .. typeName .. "'", 0)
  end
  if info.Kind ~= "Object" then
    error("Unable to construct non-object type '" .. typeName .. "'", 0)
  end
  if not Ext._Internal.ClassConstructible(typeName) then
    error("Type '" .. typeName .. "' is not constructible", 0)
  end
end

function Ext.Types.GetHashSetValueAt(object, index)
  if not Ext._Internal.Walkable(object) then return nil end
  local meta = Ext._Internal.RawGetMetatable(object)
  if type(meta) == "table" and meta.__bg3leSetAt then
    return meta.__bg3leSetAt(index + 1)
  end
  return object[index + 1]
end

function Ext.Types.GetFunctionLocation(fn)
  if type(fn) ~= "function" then return nil end
  local info = debug.getinfo(fn, "S")
  if info == nil then return nil end
  return info.short_src, info.linedefined
end

-- Custom methods and properties, by the type they were registered on.
--
-- Upstream grafts these onto the type's property map. bg3le's objects are
-- built per read, so there is nothing per-object to graft to -- but the type
-- is what the registration names, and a table keyed by type name outlives
-- every view of it. Each view consults this when a key is not one of the
-- engine's fields, which is exactly where upstream's property map would have
-- answered.
local custom_members = {}

-- Methods upstream's property maps declare with P_FUN, which bg3le's field
-- tables leave out because a method has no offset. Kept apart from what mods
-- register, and consulted first, as upstream's own map would be.
local builtin_members = {}

-- TranslatedString::Get and TranslatedFSString::Get: the text for the
-- handle, or nil if nothing is keyed by it -- upstream's std::optional. It
-- is how a mod turns a DisplayName into a name.
--
-- Upstream looks the handle and version up in the string repository's
-- primary pool, then its two fallback pools. bg3le's index is built from the
-- game's .loca files and keyed by the handle alone, and Ext.Loca's writes go
-- into it, so a string a mod has updated reads back through here too.
local function translated_get(self)
  local handle = self.Handle
  local key = handle ~= nil and handle.Handle or nil
  if type(key) ~= "string" then return nil end
  return Ext._Internal.Loca(key)
end
builtin_members["TranslatedString"] = {Get = {Fn = translated_get}}
builtin_members["TranslatedFSString"] = {Get = {Fn = translated_get}}

-- esv::Character::GetStatus: the character's status of that ID, from its
-- status machine, as upstream's loops over StatusManager->Statuses.
local function server_get_status(self, statusId)
  local machine = self.StatusManager
  local statuses = machine ~= nil and machine.Statuses or nil
  if statuses == nil then return nil end
  for _, status in pairs(statuses) do
    if status.StatusId == statusId then return status end
  end
  return nil
end

-- entity.ServerCharacter.Character and .ServerItem.Item: deprecated upstream
-- and the object itself.
local warned_self_alias = {}
local function self_alias(name)
  return function(self)
    if not warned_self_alias[name] then
      warned_self_alias[name] = true
      Ext.Log.PrintWarning("entity." .. name .. " is deprecated; use the component itself instead")
    end
    return self
  end
end
builtin_members["esv::Character"] = {
  GetStatus = {Fn = server_get_status},
  Character = {Get = self_alias("ServerCharacter.Character")},
}
builtin_members["esv::Item"] = {Item = {Get = self_alias("ServerItem.Item")}}

-- esv::Item::CreateCacheTemplate: the item's template cloned into the server's
-- cache and the item switched to it (src/vendor/cache_template.cpp).
do
  local function item_create_cache_template(self)
    local meta = Ext._Internal.RawGetMetatable(self)
    local id = type(meta) == "table" and meta.__bg3leIdentity or nil
    if type(id) == "function" then id = id(self) end
    -- A component view's identity is "c:<handle>:<component>:<prefix>".
    local handle, comp = nil, nil
    if type(id) == "string" then
      local h, c = id:match("^c:(%-?%d+):(.*):$")
      handle, comp = tonumber(h), c
    end
    if handle == nil or not Ext.IsServer() then
      error("bg3le: CreateCacheTemplate needs the server's ServerItem component", 2)
    end
    local address = Ext._Internal.ItemCreateCacheTemplate(handle, comp)
    if address == nil then return nil end
    return Ext._Internal.TemplateAt(address, "item")
  end
  builtin_members["esv::Item"].CreateCacheTemplate = {Fn = item_create_cache_template}
end

-- MoveableObject's SetWorldTranslate, SetWorldRotate and SetWorldScale, on it
-- and every class upstream's property maps derive from it.
do
  local function moveable_setter(what, name)
    return function(self, value)
      -- The object's address: "p:<hex>" on a pointed-to object, "o:<decimal>:" on a view.
      local meta = Ext._Internal.RawGetMetatable(self)
      local id = type(meta) == "table" and meta.__bg3leIdentity or nil
      if type(id) == "function" then id = id(self) end
      local addr = nil
      if type(id) == "string" then
        local hex = id:match("^p:(%x+)$")
        addr = hex and tonumber(hex, 16) or tonumber(id:match("^o:(%-?%d+):$"))
      end
      if addr == nil or not Ext._Internal.MoveableSet(addr, what, value) then
        error("bg3le: " .. name .. " could not reach this object's engine method", 2)
      end
    end
  end
  local methods = {
    SetWorldTranslate = {Fn = moveable_setter(0, "SetWorldTranslate")},
    SetWorldRotate = {Fn = moveable_setter(1, "SetWorldRotate")},
    SetWorldScale = {Fn = moveable_setter(2, "SetWorldScale")},
  }
  for _, class in ipairs({"MoveableObject", "Visual", "Effect", "RenderableObject", "AnimatableObject",
                          "Shape", "DecalObject", "CullableInstance", "InstancingRenderableObject",
                          "InstancingObject", "LightComponent"}) do
    builtin_members[class] = methods
  end
end

-- Published so the views can reach it; the prelude is compiled in more than
-- one chunk, so a local here is not in scope there.
-- The member names a type has beyond its fields, in a stable order, for
-- iteration: upstream's property map holds methods and custom members too,
-- so pairs over an object yields them.
function Ext._Internal.CustomMemberNames(typeName)
  local names = {}
  for _, set in ipairs({builtin_members[typeName] or {},
                        custom_members[typeName] or {}}) do
    for key in pairs(set) do names[#names + 1] = key end
  end
  table.sort(names)
  return names
end

-- A member's value as iteration yields it: the function for a method, the
-- property's value for a property.
function Ext._Internal.CustomMemberValue(self, typeName, key)
  local extra = Ext._Internal.CustomMember(typeName, key)
  if extra == nil then return nil end
  if extra.Fn ~= nil then return extra.Fn end
  local ok, value = pcall(extra.Get, self)
  return ok and value or nil
end

function Ext._Internal.CustomMember(typeName, key)
  if typeName == nil then return nil end
  local builtin = builtin_members[typeName]
  if builtin ~= nil and builtin[key] ~= nil then return builtin[key] end
  local members = custom_members[typeName]
  if members == nil then return nil end
  return members[key]
end

-- The name a view reports, as upstream's type names are spelled.
--
-- A top-level view names its class the way the property maps do --
-- "TranslatedString", "esv::Character". A nested one used to report the raw
-- C++ name, "bg3se::TranslatedString", which is not what
-- Ext.Types.GetObjectType returns upstream -- and custom members are filed
-- under the maps' spelling, so a function a mod added to TranslatedString
-- was never found on one nested inside anything.
-- classFirst for a view built from a class name rather than a component's.
function Ext._Internal.ViewTypeName(class, prefix, classFirst)
  if prefix == "" then return Ext._Internal.ClassName(class, classFirst) end
  local raw = Ext._Internal.TypeNameAt(class, prefix, classFirst)
  if raw == nil then return nil end
  local plain = raw:gsub("bg3se::", "")
  return Ext._Internal.ClassName(plain) or plain
end

local function register_custom(what, typeName, property, entry)
  if type(typeName) ~= "string" or type(property) ~= "string" then
    error("Ext.Types." .. what .. " takes a type name and a member name", 2)
  end

  -- Upstream's two refusals, in its own words: an unknown type, and a type
  -- that is not an object and so has no property map to extend.
  local class = Ext._Internal.ClassName(typeName)
  if class == nil then
    error("Type not found: " .. typeName, 2)
  end
  if Ext._Internal.ObjectFields(class, "") == nil then
    error("Cannot extend non-object type: " .. typeName, 2)
  end

  local members = custom_members[class]
  if members == nil then
    members = {}
    custom_members[class] = members
  end
  members[property] = entry
  return true
end

function Ext.Types.AddCustomFunction(typeName, property, func)
  if type(func) ~= "function" then
    error("Ext.Types.AddCustomFunction takes a function", 2)
  end
  return register_custom("AddCustomFunction", typeName, property,
                         {Fn = func})
end

function Ext.Types.AddCustomProperty(typeName, property, getter, setter)
  if type(getter) ~= "function" then
    error("Ext.Types.AddCustomProperty takes a getter", 2)
  end
  if setter ~= nil and type(setter) ~= "function" then
    error("Ext.Types.AddCustomProperty's setter must be a function", 2)
  end
  return register_custom("AddCustomProperty", typeName, property,
                         {Get = getter, Set = setter})
end


-- ---- Ext.Vars ----
--
-- Mod and user variables. Persistent ones are written into saves (see
-- CollectSaveExtras); ones flagged SyncToClient or SyncToServer cross to the
-- other Lua context as upstream's replicate: on write, on the tick, or on
-- Sync. User variables are keyed by entity GUID, the same on both sides.
Ext.Vars = {}

local mod_variable_defs = {}
local mod_variables = {}
local user_variable_defs = {}
local user_variables = {}
local dirty_mod = {}
local dirty_user = {}

do
  local CHANNEL = "__bg3le_vars__"

  -- Upstream's ParseUserVariableFlags defaults.
  local DEFAULTS = {Server = true, Client = false, WriteableOnServer = true,
                    WriteableOnClient = false, Persistent = true,
                    SyncToClient = false, SyncToServer = false,
                    SyncOnWrite = false, DontCache = false, SyncOnTick = true}

  local function option(defs, key, name)
    local d = defs and defs[key]
    local v = d and d[name]
    if v == nil then return DEFAULTS[name] end
    return v
  end
  Ext._Internal.VarOption = option

  local function syncs(defs, key, onTick)
    return option(defs, key, Ext.IsServer() and "SyncToClient" or "SyncToServer")
       and (not onTick or option(defs, key, "SyncOnTick"))
  end

  -- Takes what is due from one dirty table; values boxed so nil crosses.
  local function take(dirty, defsOf, valuesOf, onTick, out)
    local any = false
    for owner, keys in pairs(dirty) do
      local defs = defsOf(owner)
      for key in pairs(keys) do
        if syncs(defs, key, onTick) then
          if owner == "*" then
            for guid, vars in pairs(user_variables) do
              if vars[key] ~= nil then
                out[guid] = out[guid] or {}
                out[guid][key] = {V = vars[key]}
                any = true
              end
            end
          else
            local values = valuesOf(owner)
            out[owner] = out[owner] or {}
            out[owner][key] = {V = values and values[key]}
            any = true
          end
          keys[key] = nil
        elseif not onTick then
          keys[key] = nil
        end
      end
      if next(keys) == nil then dirty[owner] = nil end
    end
    return any
  end

  -- To peers on other machines too, as bg3se's MsgUserVars: a server's go to
  -- every client with the extender, a client's to a host on another machine.
  -- Tables travel in the binary serializer's form, as upstream sends them.
  local function sync_remote(payload)
    if Ext.IsClient() and Ext._Internal.NetInfo().Hosting then return end
    local vars = {}
    local function add(module, guid, entries)
      for key, box in pairs(entries) do
        local value = box.V
        if type(value) == "table" then
          value = {Binary = Ext.Json.Stringify(value, {Binary = true})}
        end
        vars[#vars + 1] = {Module = module, Guid = guid, Key = key, Value = value}
      end
    end
    for uuid, entries in pairs(payload.Mod) do add(true, uuid, entries) end
    for guid, entries in pairs(payload.User) do add(false, guid, entries) end
    if #vars > 0 then Ext._Internal.NetSyncVars(vars) end
  end

  local function flush(mod, user, onTick)
    local payload = {Mod = {}, User = {}}
    local any = false
    if mod then
      any = take(dirty_mod, function(uuid) return mod_variable_defs[uuid] end,
                 function(uuid) return mod_variables[uuid] end, onTick, payload.Mod) or any
    end
    if user then
      any = take(dirty_user, function() return user_variable_defs end,
                 function(guid) return user_variables[guid] end, onTick, payload.User) or any
    end
    if any then
      Ext._Internal.PostToOtherContext(CHANNEL, Ext.Json.Stringify(payload), 0)
      sync_remote(payload)
    end
  end

  function Ext._Internal.FlushVars() flush(true, true, true) end
  function Ext._Internal.FlushVarsOnWrite(defs, key)
    if option(defs, key, "SyncOnWrite") then flush(true, true, false) end
  end
  function Ext._Internal.IsVarChannel(channel) return channel == CHANNEL end

  function Ext._Internal.ApplyVarSync(payload)
    local ok, data = pcall(Ext.Json.Parse, payload)
    if not ok or type(data) ~= "table" then return end
    for uuid, entries in pairs(data.Mod or {}) do
      mod_variables[uuid] = mod_variables[uuid] or {}
      for key, box in pairs(entries) do mod_variables[uuid][key] = box.V end
    end
    for guid, entries in pairs(data.User or {}) do
      user_variables[guid] = user_variables[guid] or {}
      for key, box in pairs(entries) do user_variables[guid][key] = box.V end
    end
  end

  -- What a peer on another machine synced: upstream's NetworkSync, which
  -- takes only registered variables synced in this direction, and has the
  -- server pass a client's change on to the other clients.
  function Ext._Internal.ApplyNetVars(vars)
    local server = Ext.IsServer()
    for _, v in ipairs(vars) do
      local defs = v.Module and mod_variable_defs[v.Guid] or (not v.Module and user_variable_defs) or nil
      if defs == nil or defs[v.Key] == nil then
        Ext.Log.PrintError("Tried to sync variable '" .. tostring(v.Key) .. "' that has no prototype!")
        goto continue
      end
      if not option(defs, v.Key, server and "SyncToServer" or "SyncToClient") then
        Ext.Log.PrintError("Tried to sync variable '" .. tostring(v.Key) .. "' in illegal direction!")
        goto continue
      end
      local value = v.Value
      if v.Kind == "json" then
        local ok, parsed = pcall(Ext.Json.Parse, value)
        value = ok and parsed or nil
      elseif v.Kind == "binary" then
        local ok, parsed = pcall(Ext.Json.Parse, value, true)
        value = ok and parsed or nil
      elseif v.Kind == "null" then
        value = nil
      end
      local store = v.Module and mod_variables or user_variables
      store[v.Guid] = store[v.Guid] or {}
      store[v.Guid][v.Key] = value
      if server and option(defs, v.Key, "SyncToClient") then
        local dirty = v.Module and dirty_mod or dirty_user
        dirty[v.Guid] = dirty[v.Guid] or {}
        dirty[v.Guid][v.Key] = true
      end
      ::continue::
    end
  end

  function Ext.Vars.SyncModVariables() flush(true, false, false) end
  function Ext.Vars.SyncUserVariables() flush(false, true, false) end
end

function Ext.Vars.RegisterModVariable(moduleUuid, name, options)
  if type(moduleUuid) ~= "string" or type(name) ~= "string" then
    error("Ext.Vars.RegisterModVariable(moduleUuid, name[, options])", 2)
  end
  mod_variable_defs[moduleUuid] = mod_variable_defs[moduleUuid] or {}
  mod_variable_defs[moduleUuid][name] = options or {}
  mod_variables[moduleUuid] = mod_variables[moduleUuid] or {}
end

-- Returns the module's variable table. Writing to it is how a mod sets
-- one, so it is the live table rather than a copy, and a write marks the
-- key dirty through the proxy.
function Ext.Vars.GetModVariables(moduleUuid)
  local defs = mod_variable_defs[moduleUuid]
  -- An empty table, not nil, for a mod that has registered none: callers
  -- iterate the result without checking, and upstream lets them.
  if defs == nil then return {} end

  local store = mod_variables[moduleUuid]
  return Ext._Internal.NewObjectProxy({
    __index = function(_, key) return store[key] end,
    __newindex = function(_, key, value)
      if defs[key] == nil then
        error("no mod variable named " .. tostring(key)
              .. " is registered for " .. tostring(moduleUuid), 2)
      end
      local server = Ext.IsServer()
      if not Ext._Internal.VarOption(defs, key, server and "WriteableOnServer" or "WriteableOnClient") then
        Ext.Log.PrintError("Variable class '" .. key .. "' not writeable on "
                           .. (server and "server" or "client"))
        return
      end
      store[key] = value
      dirty_mod[moduleUuid] = dirty_mod[moduleUuid] or {}
      dirty_mod[moduleUuid][key] = true
      Ext._Internal.FlushVarsOnWrite(defs, key)
    end,
    __pairs = function()
      return next, store, nil
    end,
  })
end


function Ext.Vars.DirtyModVariables(moduleUuid, key)
  if moduleUuid == nil then
    for uuid, defs in pairs(mod_variable_defs) do
      dirty_mod[uuid] = dirty_mod[uuid] or {}
      for k in pairs(defs) do dirty_mod[uuid][k] = true end
    end
    return
  end
  dirty_mod[moduleUuid] = dirty_mod[moduleUuid] or {}
  if key == nil then
    for k in pairs(mod_variable_defs[moduleUuid] or {}) do
      dirty_mod[moduleUuid][k] = true
    end
  else
    dirty_mod[moduleUuid][key] = true
  end
end

function Ext.Vars.RegisterUserVariable(name, options)
  if type(name) ~= "string" then
    error("Ext.Vars.RegisterUserVariable(name[, options])", 2)
  end
  user_variable_defs[name] = options or {}
end


function Ext.Vars.DirtyUserVariables(entityGuid, key)
  local bucket = entityGuid or "*"
  dirty_user[bucket] = dirty_user[bucket] or {}
  if key == nil then
    for k in pairs(user_variable_defs) do dirty_user[bucket][k] = true end
  else
    dirty_user[bucket][key] = true
    Ext._Internal.FlushVarsOnWrite(user_variable_defs, key)
  end
end

function Ext.Vars.GetEntitiesWithVariable(variable)
  local out = {}
  for guid, vars in pairs(user_variables) do
    if vars[variable] ~= nil then out[#out + 1] = guid end
  end
  table.sort(out)
  return out
end

-- The store behind an entity's UserVars, used by Ext.Entity.
function Ext._Internal.UserVariableStore(guid)
  user_variables[guid] = user_variables[guid] or {}
  return user_variables[guid], user_variable_defs
end

-- ---- Ext.Net ----
--
-- Upstream's (Lua/Libs/ServerNet.inl, ClientNet.inl, Net.inl, and the
-- NetChannel and NetworkManager libraries). A message for this process's
-- other context is queued there directly, as upstream's local message
-- passing does; one for another machine goes over the game's connection in
-- bg3se's own format (src/net.cpp), so bg3le and Windows players running
-- bg3se talk to each other. Either way it is handled on the receiver's next
-- tick.
Ext.Net = {}

do

local net_listeners = {}
local warned_net_listener = {}

-- Messages that arrived, drained on the next tick.
local net_inbox = {}

-- Upstream's NetworkRequestSystem: the callbacks for requests this state
-- sent, by the ID their replies come back under.
local net_requests = {}
local net_next_request = 1

local function net_request(handler)
  if handler == nil then return 0 end
  local id = net_next_request
  net_next_request = net_next_request + 1
  net_requests[id] = handler
  return id
end

-- Upstream takes a string; a table is serialized rather than refused.
local function net_payload(payload)
  if type(payload) == "table" then return Ext.Json.Stringify(payload) end
  if payload ~= nil and type(payload) ~= "string" then return tostring(payload) end
  return payload
end

local function net_check(name, channel)
  if type(channel) ~= "string" then
    error("Ext.Net." .. name .. " takes a channel name", 3)
  end
end

local function peer_of(user) return user >> 16 end

-- Ext.IsServer is defined further down; the state already knows which it is.
if not Ext._Internal.IsClientState() then
  -- A character's user: its ServerCharacter's UserID, as upstream reads it.
  local function character_user(character)
    if character == nil then return nil end
    local ok, user = pcall(function()
      return Ext.Entity.Get(character).ServerCharacter.UserID
    end)
    if ok and type(user) == "number" then return user end
    if Osi ~= nil and Osi.GetReservedUserID ~= nil then
      ok, user = pcall(Osi.GetReservedUserID, character)
      if ok and type(user) == "number" then return user end
    end
    return nil
  end

  local function post_to_user(user, channel, payload, module, handler, replyId, binary)
    local info = Ext._Internal.NetInfo()
    local requestId = net_request(handler)
    payload = net_payload(payload)
    if peer_of(user) == info.LocalPeer then
      Ext._Internal.PostToOtherContext(channel, payload, user, module, requestId,
                                       replyId, binary)
      return
    end
    local ok, err = Ext._Internal.NetPost(user, channel, payload, module, requestId,
                                          replyId, binary)
    if not ok then Ext.Log.PrintError(err) end
  end

  function Ext.Net.BroadcastMessage(channel, payload, excludeCharacter, module,
                                    requestHandler, replyId, binary)
    net_check("BroadcastMessage", channel)
    local info = Ext._Internal.NetInfo()
    local exclude = character_user(excludeCharacter)
    local requestId = net_request(requestHandler)
    payload = net_payload(payload)
    if exclude == nil or peer_of(exclude) ~= info.LocalPeer then
      Ext._Internal.PostToOtherContext(channel, payload, info.ReservedUser, module,
                                       requestId, replyId, binary)
    end
    Ext._Internal.NetPost("broadcast", channel, payload, module, requestId, replyId,
                          binary, exclude)
  end

  function Ext.Net.PostMessageToClient(character, channel, payload, module,
                                       requestHandler, replyId, binary)
    net_check("PostMessageToClient", channel)
    local user = character_user(character)
    if user == nil then return end
    if user == Ext._Internal.NetInfo().ReservedUser then
      Ext.Log.PrintError("Attempted to send message to character " .. tostring(character)
                         .. " on channel '" .. channel .. "' that has no user assigned!")
      return
    end
    post_to_user(user, channel, payload, module, requestHandler, replyId, binary)
  end

  function Ext.Net.PostMessageToUser(userId, channel, payload, module,
                                     requestHandler, replyId, binary)
    net_check("PostMessageToUser", channel)
    if type(userId) ~= "number" then
      error("Ext.Net.PostMessageToUser takes a user ID", 2)
    end
    if userId == Ext._Internal.NetInfo().ReservedUser then
      Ext.Log.PrintError("Attempted to send message on channel '" .. channel
                         .. "' to reserved user ID!")
      return
    end
    post_to_user(math.tointeger(userId) or userId, channel, payload, module,
                 requestHandler, replyId, binary)
  end

  function Ext.Net.PlayerHasExtender(character)
    local user = character_user(character)
    local info = Ext._Internal.NetInfo()
    if user == nil or user == info.ReservedUser then return nil end
    local peer = peer_of(user)
    return peer == info.LocalPeer or info.Peers[peer] ~= nil
  end

  -- Kinda pointless on the server, as upstream says, but kept for symmetry.
  function Ext.Net.IsHost() return true end
  function Ext.Net.Version() return Ext._Internal.NetInfo().Version end
else
  function Ext.Net.PostMessageToServer(channel, payload, module, requestHandler,
                                       replyId, binary)
    net_check("PostMessageToServer", channel)
    local info = Ext._Internal.NetInfo()
    local requestId = net_request(requestHandler)
    payload = net_payload(payload)
    -- To this process's own server; or to the host, if it has the extender;
    -- or, with no host yet, queued for the server context here.
    if info.Hosting or (not info.HostHasExtender and not info.Connected) then
      Ext._Internal.PostToOtherContext(channel, payload, info.LocalUser, module,
                                       requestId, replyId, binary)
      return
    end
    local ok, err = Ext._Internal.NetPost(nil, channel, payload, module, requestId,
                                          replyId, binary)
    if not ok then Ext.Log.PrintError(err) end
  end

  function Ext.Net.IsHost() return Ext._Internal.NetInfo().Hosting end
  function Ext.Net.Version() return Ext._Internal.NetInfo().Version end
end

-- ---- net channels ----
--
-- Upstream's NetChannel and NetworkManager: the payload is serialized with
-- Ext.Json (binary from protocol version 2), sent with the mod's UUID as its
-- module, and a request's reply comes back under the request's ID.
local net_channels = {}

local NetChannel = {}
NetChannel.__index = NetChannel

function NetChannel:SetHandler(handler) self.MessageHandler = handler end
function NetChannel:SetRequestHandler(handler) self.RequestHandler = handler end

function NetChannel:IsBinary() return Ext.Net.Version() >= 2 end

function NetChannel:Stringify(message)
  return Ext.Json.Stringify(message, {Binary = self:IsBinary()})
end

function NetChannel:DoSendToServer(message, handler)
  Ext.Net.PostMessageToServer(self.Channel, self:Stringify(message), self.Module,
                              handler, nil, self:IsBinary())
end

function NetChannel:SendToServer(message) self:DoSendToServer(message, nil) end

function NetChannel:RequestToServer(message, handler)
  self:DoSendToServer(message, function(reply, binary)
    handler(Ext.Json.Parse(reply, binary))
  end)
end

function NetChannel:DoSendToClient(message, user, handler)
  local msg = self:Stringify(message)
  if type(user) == "number" then
    Ext.Net.PostMessageToUser(user, self.Channel, msg, self.Module, handler, nil,
                              self:IsBinary())
  else
    Ext.Net.PostMessageToClient(user, self.Channel, msg, self.Module, handler, nil,
                                self:IsBinary())
  end
end

function NetChannel:SendToClient(message, user) self:DoSendToClient(message, user, nil) end
-- Not upstream's; earlier bg3le had it, and a mod may use it.
NetChannel.SendToUser = NetChannel.SendToClient

function NetChannel:RequestToClient(message, user, handler)
  self:DoSendToClient(message, user, function(reply, binary)
    handler(Ext.Json.Parse(reply, binary))
  end)
end

function NetChannel:Broadcast(message, excludeCharacter)
  Ext.Net.BroadcastMessage(self.Channel, self:Stringify(message), excludeCharacter,
                           self.Module, nil, nil, self:IsBinary())
end

function NetChannel:OnMessage(e)
  local request = Ext.Json.Parse(e.Payload, e.Binary)
  if e.RequestId then
    if self.RequestHandler then
      local ok, ret = xpcall(self.RequestHandler, debug.traceback, request, e.UserID)
      if ok then
        local reply = self:Stringify(ret)
        if Ext.IsServer() then
          Ext.Net.PostMessageToUser(e.UserID, e.Channel, reply, e.Module, nil,
                                    e.RequestId, self:IsBinary())
        else
          Ext.Net.PostMessageToServer(e.Channel, reply, e.Module, nil, e.RequestId,
                                      self:IsBinary())
        end
      else
        Ext.Log.PrintError("Error during request dispatch for module " .. e.Module
                           .. ", channel " .. e.Channel .. ": " .. tostring(ret))
      end
    else
      Ext.Log.PrintWarning("Net request received for module " .. e.Module
                           .. ", channel " .. e.Channel
                           .. ", but no request handler was registered!")
    end
  else
    if self.MessageHandler then
      local ok, err = xpcall(self.MessageHandler, debug.traceback, request, e.UserID)
      if not ok then
        Ext.Log.PrintError("Error during message dispatch for module " .. e.Module
                           .. ", channel " .. e.Channel .. ": " .. tostring(err))
      end
    else
      Ext.Log.PrintWarning("Net message received for module " .. e.Module
                           .. ", channel " .. e.Channel
                           .. ", but no message handler was registered!")
    end
  end
end

function Ext.Net.CreateChannel(module, channel, messageHandler, requestHandler)
  if type(module) ~= "string" or type(channel) ~= "string" then
    error("Ext.Net.CreateChannel(module, channel[, messageHandler"
          .. "[, requestHandler]])", 2)
  end
  -- One object per (module, channel) in a context; creating it again hands
  -- back the same one rather than refusing as upstream does.
  local key = module .. "/" .. channel
  local made = net_channels[key]
  if made == nil then
    made = setmetatable({Module = module, Channel = channel}, NetChannel)
    net_channels[key] = made
  end
  -- As upstream's CreateChannel, which assigns both unconditionally.
  made.MessageHandler = messageHandler
  made.RequestHandler = requestHandler
  return made
end

-- NetworkManager:MessageReceived.
local function net_channel_message(e)
  local channel = net_channels[e.Module .. "/" .. e.Channel]
  if channel == nil then
    Ext.Log.PrintWarning("Net message received for module " .. e.Module .. ", channel "
                         .. e.Channel .. ", but no such channel was registered!")
    return
  end
  channel:OnMessage(e)
end

function Ext.RegisterNetListener(channel, handler)
  -- Upstream says the same thing: the NetChannel object replaced this.
  if not warned_net_listener[channel] then
    warned_net_listener[channel] = true
    Ext.Log.PrintWarning("Ext.RegisterNetListener(" .. tostring(channel)
                         .. ") is deprecated; consider using Ext.Net.CreateChannel() instead")
  end
  net_listeners[channel] = net_listeners[channel] or {}
  table.insert(net_listeners[channel], handler)
end

function Ext._Internal.FireNetMessage(channel, payload, userId)
  for _, handler in ipairs(net_listeners[channel] or {}) do
    local ok, err = xpcall(handler, debug.traceback, channel, payload, userId)
    if not ok then
      Ext.Log.PrintError("Error during NetMessage dispatch: " .. tostring(err))
    end
  end
end

-- Upstream's State::OnNetMessageReceived.
local function net_receive(m)
  if m.ReplyId ~= 0 then
    local callback = net_requests[m.ReplyId]
    net_requests[m.ReplyId] = nil
    if callback == nil then
      Ext.Log.PrintWarning("No handler found for net message request id " .. m.ReplyId)
      return
    end
    local ok, err = xpcall(callback, debug.traceback, m.Payload, m.Binary)
    if not ok then
      Ext.Log.PrintError("Error while dispatching user function call: " .. tostring(err))
    end
    return
  end

  local e = {
    Channel = m.Channel,
    Payload = m.Payload,
    Module = m.Module,
    RequestId = m.RequestId ~= 0 and m.RequestId or nil,
    UserID = m.User,
    Binary = m.Binary,
  }
  if e.RequestId == nil and e.Module == nil then
    -- Only legacy messages go through the old NetMessage event.
    Ext._Internal.FireNetMessage(e.Channel, e.Payload, e.UserID)
    Ext._Internal.FireEvent("NetMessage", e)
  else
    if e.Module ~= nil then net_channel_message(e) end
    Ext._Internal.FireEvent("NetModMessage", e)
  end
end

-- Drained by the tick, before timers, so a message posted on one tick is
-- handled on the next rather than whenever a handler happens to run.
function Ext._Internal.DrainNetMessages()
  for _, m in ipairs(Ext._Internal.TakeNetMessages()) do
    net_inbox[#net_inbox + 1] = m
  end
  if #net_inbox == 0 then return end

  -- Taken whole first: a handler may send, and that must land on the next
  -- drain rather than extend this one.
  local batch = net_inbox
  net_inbox = {}

  for _, m in ipairs(batch) do
    if m.Kind == "vars" then
      Ext._Internal.ApplyNetVars(m.Vars or {})
    elseif m.Kind == "reset" then
      Ext.Log.Print("bg3le: the host reset its Lua; resetting this client's")
      Ext._Internal.RequestReset()
    elseif Ext._Internal.IsVarChannel(m.Channel) then
      Ext._Internal.ApplyVarSync(m.Payload)
    else
      net_receive(m)
    end
  end
end
end

-- ---- top level ----

-- Upstream's: a one-shot Tick subscription, so fn gets the tick event.
-- Defined once the events exist; see below.

-- ---- Ext.Events and Ext.ModEvents ----
--
-- A port of upstream's Events library (LuaScripts/Libs/Events: Subscribable-
-- Event, MissingSubscribableEvent, EventManager, ModEventManager), by Norbyte
-- and the bg3se contributors. Handlers run by priority, highest first; a
-- subscription id carries its event's prefix in the high 32 bits; changes
-- made during a throw take effect after it; e:StopPropagation() ends it.
--
-- bg3le throws the events it can tell the truth about -- SessionLoading and
-- SessionLoaded, StatsLoaded, Tick -- and the rest exist and stay silent.


local events_by_id = {}

-- Upstream's Ext.Config (Lua/Libs/LuaSharedLibs.cpp RegisterConfig) with
-- ExtenderConfig's defaults; the thresholds are microseconds.
Ext.Config = {
  ProfilerEnabled = false,
  PerfMessagesEnabled = true,
  ProfilerLoadCallbackErrorThreshold = 50000,
  ProfilerCallbackErrorThreshold =
    Ext._Internal.IsClientState() and 2000 or 5000,
}

-- Upstream's Profiler:Report: a warning for a handler over the threshold,
-- the load threshold while the game is not running.
local function perf_report(took, desc)
  if not Ext.Config.PerfMessagesEnabled then return end
  local ok, state = pcall(Ext.Utils.GetGameState)
  if not ok then state = nil end
  local loading = state ~= "Running" and state ~= "Paused"
  local threshold = loading and Ext.Config.ProfilerLoadCallbackErrorThreshold
                    or Ext.Config.ProfilerCallbackErrorThreshold
  if took >= threshold then
    Ext.Log.PrintWarning(desc .. " took " .. (Ext.Math.Round(took) / 1000)
                         .. " ms")
  end
end
Ext._Internal.PerfReport = perf_report

local SubscribableEvent = {}
SubscribableEvent.__index = SubscribableEvent

local function new_event(name, idPrefix)
  return setmetatable({
    First = nil,
    NextIndex = 1,
    IdPrefix = idPrefix or Ext.Math.Random(1, 0xfffffff),
    Name = name,
    PendingDeletions = {},
    PendingAdds = {},
    EnterCount = 0,
  }, SubscribableEvent)
end

function SubscribableEvent:Subscribe(handler, opts)
  opts = opts or {}
  local index = self.NextIndex
  self.NextIndex = self.NextIndex + 1

  local sub = {
    Handler = handler,
    Index = index,
    Priority = opts.Priority or 100,
    Once = opts.Once or false,
    Options = opts,
  }

  if self.EnterCount == 0 then
    self:DoSubscribe(sub)
  else
    table.insert(self.PendingAdds, sub)
  end

  return index | (self.IdPrefix << 32)
end

function SubscribableEvent:DoSubscribeBefore(node, sub)
  sub.Prev = node.Prev
  sub.Next = node
  if node.Prev ~= nil then
    node.Prev.Next = sub
  else
    self.First = sub
  end
  node.Prev = sub
end

function SubscribableEvent:DoSubscribe(sub)
  if self.First == nil then
    self.First = sub
    return
  end

  local cur = self.First
  local last
  while cur ~= nil do
    last = cur
    if sub.Priority > cur.Priority then
      self:DoSubscribeBefore(cur, sub)
      return
    end
    cur = cur.Next
  end

  last.Next = sub
  sub.Prev = last
end

function SubscribableEvent:RemoveNode(node)
  if node.Prev ~= nil then node.Prev.Next = node.Next end
  if node.Next ~= nil then node.Next.Prev = node.Prev end
  if self.First == node then self.First = node.Next end
  node.Prev = nil
  node.Next = nil
end

function SubscribableEvent:Unsubscribe(id)
  if id >> 32 ~= self.IdPrefix then
    local evt = events_by_id[id >> 32]
    if evt == nil then
      Ext.Log.PrintWarning("Attempted to remove subscriber ID " .. id
        .. " for event '" .. self.Name
        .. "', but the subscription is for a different event!")
    else
      Ext.Log.PrintWarning("Attempted to remove subscriber ID " .. id
        .. " for event '" .. self.Name
        .. "', but the subscription is for event '" .. evt.Name .. "'!")
    end
    return
  end

  local handlerIndex = id & 0xffffffff
  if self.EnterCount == 0 then
    self:DoUnsubscribe(handlerIndex)
  else
    table.insert(self.PendingDeletions, handlerIndex)
  end
end

function SubscribableEvent:DoUnsubscribe(handlerIndex)
  local cur = self.First
  while cur ~= nil do
    if cur.Index == handlerIndex then
      self:RemoveNode(cur)
      return
    end
    cur = cur.Next
  end

  Ext.Log.PrintWarning("Attempted to remove subscriber index " .. handlerIndex
    .. " for event '" .. self.Name
    .. "', but no such subscriber exists (maybe it was removed already?)")
end

function SubscribableEvent:ProcessDeferredSubscriptions()
  if #self.PendingAdds > 0 then
    for _, sub in pairs(self.PendingAdds) do self:DoSubscribe(sub) end
    self.PendingAdds = {}
  end

  if #self.PendingDeletions > 0 then
    for _, handlerIndex in pairs(self.PendingDeletions) do
      self:DoUnsubscribe(handlerIndex)
    end
    self.PendingDeletions = {}
  end
end

-- BG3LE_PROFILE=1: time every call a slow handler makes, C functions
-- included, and log the most expensive by inclusive and by self time.
local profile_handlers = os.getenv("BG3LE_PROFILE") ~= nil

local function profiled_call(handler, event, label)
  local clock = Ext.Utils.MicrosecTime
  local incl, self_t, calls = {}, {}, {}
  local stack = {}
  local function key_of(at, named)
    if at.what == "C" then
      return (named and named.name or "?") .. " [C]"
    end
    return at.short_src .. ":" .. tostring(at.linedefined)
  end
  Ext._Internal.SetHook(function(ev)
    local now = clock()
    if ev == "call" or ev == "tail call" then
      local at = debug.getinfo(2, "S")
      local named = at and at.what == "C" and debug.getinfo(2, "n") or nil
      local k = at and key_of(at, named) or "?"
      if ev == "tail call" and #stack > 0 then
        -- replaces the caller's frame
        local top = stack[#stack]
        local spent = now - top[2]
        incl[top[1]] = (incl[top[1]] or 0) + spent
        self_t[top[1]] = (self_t[top[1]] or 0) + spent - top[3]
        stack[#stack] = {k, now, 0}
      else
        stack[#stack + 1] = {k, now, 0}
      end
      calls[k] = (calls[k] or 0) + 1
    else
      local top = table.remove(stack)
      if top ~= nil then
        local spent = now - top[2]
        incl[top[1]] = (incl[top[1]] or 0) + spent
        self_t[top[1]] = (self_t[top[1]] or 0) + spent - top[3]
        local parent = stack[#stack]
        if parent ~= nil then parent[3] = parent[3] + spent end
      end
    end
  end, "cr")
  local started = clock()
  local ok, result = xpcall(handler, debug.traceback, event)
  Ext._Internal.SetHook()
  local took = clock() - started
  if took > 100000 then
    local function top(t, title)
      local rows = {}
      for k, v in pairs(t) do rows[#rows + 1] = {k, v} end
      table.sort(rows, function(a, b) return a[2] > b[2] end)
      Ext.Log.Print(string.format("profile %s, %s (%.0f ms with hooks):", label, title, took / 1000))
      for i = 1, math.min(20, #rows) do
        Ext.Log.Print(string.format("  %9.1f ms %8d calls  %s", rows[i][2] / 1000,
                                    calls[rows[i][1]] or 0, rows[i][1]))
      end
    end
    top(self_t, "self time")
    top(incl, "inclusive")
  end
  return ok, result
end

-- Upstream's Dispatch, plus bg3le's report of a handler that holds the
-- thread up, named by the file and line it was defined at.
function SubscribableEvent:Dispatch(event, handler)
  local started = Ext.Utils.MicrosecTime()
  local ok, result
  if profile_handlers then
    ok, result = profiled_call(handler, event, self.Name)
  else
    ok, result = xpcall(handler, debug.traceback, event)
  end
  local took = Ext.Utils.MicrosecTime() - started
  if not ok then
    Ext.Log.PrintError("Error while dispatching event " .. self.Name .. ": ",
                       result)
  elseif self.Name ~= "DoConsoleCommand" and self.Name ~= "NetMessage" then
    local source, line = Ext.Types.GetFunctionLocation(handler)
    perf_report(took, "Dispatching event " .. self.Name .. " ("
                      .. tostring(source) .. ":" .. tostring(line) .. ")")
  end
end

function SubscribableEvent:Throw(event)
  self.EnterCount = self.EnterCount + 1

  local cur = self.First
  while cur ~= nil do
    if event.Stopped then break end

    self:Dispatch(event, cur.Handler)

    if cur.Once then
      local last = cur
      cur = last.Next
      self:RemoveNode(last)
    else
      cur = cur.Next
    end
  end

  self.EnterCount = self.EnterCount - 1
  if self.EnterCount == 0 then self:ProcessDeferredSubscriptions() end
end

local MissingSubscribableEvent = {}
MissingSubscribableEvent.__index = MissingSubscribableEvent

function MissingSubscribableEvent:Subscribe()
  Ext.Log.PrintError("Attempted to subscribe to nonexistent event: "
                     .. self.Name)
end

function MissingSubscribableEvent:Unsubscribe()
  Ext.Log.PrintError("Attempted to unsubscribe from nonexistent event: "
                     .. self.Name)
end

function MissingSubscribableEvent:Throw()
  Ext.Log.PrintError("Attempted to throw nonexistent event: " .. self.Name)
end

-- Upstream's _PublishedSharedEvents, then each context's _PublishedEvents.
local kSharedEvents = {
  "ModuleLoadStarted", "StatsLoaded", "ModuleResume", "SessionLoading",
  "SessionLoaded", "GameStateChanged", "ResetCompleted", "Shutdown",
  "DoConsoleCommand", "Tick", "StatsStructureLoaded", "FindPath",
  "NetMessage", "NetModMessage", "Log",
}
local kServerEvents = {
  "DealDamage", "DealtDamage", "BeforeDealDamage", "ExecuteFunctor",
  "AfterExecuteFunctor",
}
local kClientEvents = {
  "KeyInput", "MouseButtonInput", "MouseWheelInput", "ControllerAxisInput",
  "ControllerButtonInput", "ViewportResized",
}
Ext._Internal._PublishedSharedEvents = kSharedEvents
Ext._Internal._PublishedEvents = kServerEvents

local engine_events = {}

local function register_engine_event(name)
  local ev = new_event(name, #events_by_id + 1)
  engine_events[name] = ev
  table.insert(events_by_id, ev)
end

for _, name in ipairs(kSharedEvents) do register_engine_event(name) end
for _, name in ipairs(Ext._Internal.IsClientState() and kClientEvents
                      or kServerEvents) do
  register_engine_event(name)
end

do
  local oldSubscribe = engine_events.NetMessage.Subscribe
  engine_events.NetMessage.Subscribe = function(self, handler, opts)
    Ext.Log.PrintWarning("Ext.Events.NetMessage.Subscribe() is deprecated; "
                         .. "consider using Ext.Net.CreateChannel() instead")
    return oldSubscribe(self, handler, opts)
  end
end

Ext.Events = setmetatable({}, {
  __index = function(_, event)
    return engine_events[event]
           or setmetatable({ Name = event }, MissingSubscribableEvent)
  end,
  __newindex = function()
    error("Cannot write to Ext.Events directly!")
  end,
})

function Ext.OnNextTick(fn)
  Ext.Events.Tick:Subscribe(fn, {Once = true})
end

-- An engine event's object: its fields, plus upstream's EventBase members.
local EventBase = {}
EventBase.__index = EventBase

function EventBase:StopPropagation() self.Stopped = true end

function EventBase:PreventAction()
  if self.CanPreventAction then
    self.ActionPrevented = true
  else
    Ext.Log.PrintError("Can't prevent action")
  end
end

-- Throws one of them. Params are whatever the caller has; they become the
-- event object the handlers read.
function Ext._Internal.FireEvent(name, params)
  local event = engine_events[name]
  if event == nil then return end
  params = params or {}
  if Ext._Internal.RawGetMetatable(params) == nil then
    params.Name = params.Name or name
    if params.CanPreventAction == nil then params.CanPreventAction = false end
    if params.ActionPrevented == nil then params.ActionPrevented = false end
    if params.Stopped == nil then params.Stopped = false end
    setmetatable(params, EventBase)
  end
  -- Upstream throws StatsLoaded with ScopeModuleLoad set, the one window
  -- in which a stat edit needs no Sync.
  local moduleLoad = name == "StatsLoaded"
  if moduleLoad then Ext._Internal.StatsModuleLoad = true end
  event:Throw(params)
  if moduleLoad then Ext._Internal.StatsModuleLoad = false end
end

-- Upstream's ClientState::OnInputEvent: the SDL event as upstream's event
-- object, enums as their labels. Returns whether a handler prevented it.
local function input_label(enum, value)
  local labels = Ext.Enums[enum]
  return labels ~= nil and labels[value] or value
end

local function input_flags(enum, value)
  local out = {}
  local labels = Ext.Enums[enum]
  if labels == nil then return out end
  for k, label in pairs(labels) do
    if type(k) == "number" and k ~= 0 and value & k == k then
      out[#out + 1] = label
    end
  end
  table.sort(out)
  return out
end

function Ext._Internal.InputEvent(kind, a, b, c, d, e, x, y)
  local name, params
  if kind == 1 then
    name = "KeyInput"
    params = {Event = a ~= 0 and "KeyDown" or "KeyUp",
              Key = input_label("SDLScanCode", b),
              Modifiers = input_flags("SDLKeyModifier", c),
              Pressed = d ~= 0, Repeat = e ~= 0, CanPreventAction = true}
  elseif kind == 2 then
    name = "MouseButtonInput"
    params = {Button = a, Pressed = b ~= 0, Clicks = c, X = d, Y = e,
              CanPreventAction = true}
  elseif kind == 3 then
    name = "MouseWheelInput"
    params = {ScrollX = x, ScrollY = y, X = a, Y = b, CanPreventAction = true}
  elseif kind == 4 then
    name = "ControllerAxisInput"
    params = {DeviceId = a, Axis = input_label("SDLControllerAxis", b),
              Value = x}
  elseif kind == 5 then
    name = "ControllerButtonInput"
    params = {DeviceId = a, Event = b ~= 0 and "KeyDown" or "KeyUp",
              Button = input_label("SDLControllerButton", c),
              Pressed = d ~= 0, CanPreventAction = true}
  elseif kind == 6 then
    name = "ViewportResized"
    params = {Width = a, Height = b}
  else
    return false
  end
  Ext._Internal.FireEvent(name, params)
  return params.ActionPrevented == true
end

-- Ext.ModEvents[mod][event]: created on first index, as upstream's
-- ModEventManager does. Mod Configuration Menu subscribes to events it
-- never registers, including the one its own logger hangs off.
local mod_events = {}

local function create_mod_events(mod)
  local events = { Events = {}, PublicTable = {} }
  setmetatable(events.PublicTable, {
    __index = function(_, event)
      if events.Events[event] == nil then
        events.Events[event] = new_event(mod .. "." .. event)
      end
      return events.Events[event]
    end,
    __newindex = function()
      error("Cannot write to Ext.ModEvents directly!")
    end,
    __metatable = "ModEvents",
  })
  return events
end

Ext.ModEvents = setmetatable({}, {
  __index = function(_, mod)
    if mod_events[mod] == nil then mod_events[mod] = create_mod_events(mod) end
    return mod_events[mod].PublicTable
  end,
  __newindex = function()
    error("Cannot write to Ext.ModEvents directly!")
  end,
  __metatable = "ModEvents",
})

-- Upstream's ModEventManager:RegisterEvent: only while that mod bootstraps.
function Ext.RegisterModEvent(mod, event)
  local bootstrapping = Ext._Internal.BootstrappingMod()
  if bootstrapping == nil then
    Ext.Log.PrintWarning("Tried to register mod event while not bootstrapping a mod; this is not allowed.")
    return
  end
  if bootstrapping ~= mod then
    Ext.Log.PrintWarning("Tried to register mod event for mod '" .. tostring(mod)
                         .. "' while bootstrapping mod '" .. bootstrapping .. "'; this is not allowed.")
    return
  end
  if mod_events[mod] == nil then mod_events[mod] = create_mod_events(mod) end
  if mod_events[mod].Events[event] ~= nil then
    Ext.Log.PrintWarning("Tried to register mod event '" .. mod .. "."
                         .. event .. "' twice")
    return
  end
  mod_events[mod].Events[event] = new_event(mod .. "." .. event)
end

local console_commands = {}

function Ext.RegisterConsoleCommand(name, handler)
  if type(name) ~= "string" or type(handler) ~= "function" then
    error("Ext.RegisterConsoleCommand(name, handler)", 2)
  end
  console_commands[name] = handler
end

-- Called by the console when a line starts with a registered command.
function Ext._Internal.RunConsoleCommand(name, ...)
  local handler = console_commands[name]
  if handler == nil then return false end
  local ok, err = xpcall(handler, debug.traceback, name, ...)
  if not ok then
    Ext.Log.PrintError("Error during console command callback: ", err)
  end
  return true
end

-- Ext.Require(path) or Ext.Require(modGuid, path): loads a mod's Lua file
-- once, caching by the name it was asked for, as upstream does.
local required = {}

function Ext.Require(a, b)
  local path = b or a
  if required[path] ~= nil then return required[path] end

  local contents = Ext.IO.LoadFile(path, "data")
  if contents == nil then
    error("Ext.Require: cannot read " .. tostring(path), 2)
  end
  local chunk, err = Ext._Internal.RawLoad(contents, path, "t")
  if chunk == nil then error(err, 2) end

  local result = chunk()
  if result == nil then result = true end
  required[path] = result
  return result
end

-- Upstream's Startup.lua: one of the builtin:// libraries, loaded once.
do
  local core_libs = {}
  Ext.CoreLib = function(name)
    if core_libs[name] == nil then
      core_libs[name] = Ext.Utils.Include(nil, "builtin://Libs/" .. tostring(name) .. ".lua") or true
    end
    local lib = core_libs[name]
    if lib == true then return nil end
    return lib
  end
end

-- ---- Ext.Utils ----
Ext.Utils = {
  Print = Ext.Log.Print,
  PrintWarning = Ext.Log.PrintWarning,
  PrintError = Ext.Log.PrintError,
  Round = Ext.Math.Round,
  Random = Ext.Math.Random,
  MonotonicTime = Ext.Timer.MonotonicTime,
  MicrosecTime = Ext.Timer.MicrosecTime,
  GenerateGuid = Ext._Internal.GenerateGuid,
  GameVersion = Ext._Internal.GameVersion,
  GetCommandLineParams = Ext._Internal.GetCommandLineParams,
  GetMemoryUsage = Ext._Internal.GetMemoryUsage,
  ShowError = Ext._Internal.ShowError,
}

-- The extender version a mod tests against. bg3le reports the bg3se
-- version whose public API it implements, because that is the question the
-- mod is asking; reference/utils-shape.txt has the capture at 32.
function Ext.Utils.Version() return 32 end

-- Bound late: Ext.Timer is completed further down, after this table.
function Ext.Utils.GameTime() return Ext.Timer.GameTime() end

function Ext.Utils.ShowErrorAndExitGame(message)
  Ext._Internal.ShowErrorAndExit(tostring(message))
end

-- Upstream's is GetDebugName, as Ext.Types.GetValueType.
function Ext.Utils.GetValueType(value)
  return Ext.Types.GetValueType(value)
end

-- Upstream's: true for an entity whose handle is not the null handle.
function Ext.Utils.IsValidHandle(handle)
  local bits = Ext._Internal.EntityProxyHandle(handle)
  return bits ~= nil and bits ~= 0xFFC0000000000000
end

-- Upstream's take an entity (nil is the null handle) and give one back.
function Ext.Utils.HandleToInteger(handle)
  if handle == nil then return 0xFFC0000000000000 end
  if type(handle) == "number" then return math.tointeger(handle) end
  local bits = Ext._Internal.EntityProxyHandle(handle)
  if bits == nil then
    error("bad argument #1 to 'HandleToInteger' (entity expected, got "
          .. type(handle) .. ")", 2)
  end
  return bits
end

function Ext.Utils.IntegerToHandle(i)
  i = math.tointeger(i)
  if i == nil or i == 0xFFC0000000000000 then return nil end
  return Ext._Internal.EntityOf(i)
end

-- Upstream's: compiles text (never binary) and returns the chunk, or nil
-- and the error; globals, if given, is the chunk's environment.
function Ext.Utils.LoadString(text, globals)
  if globals ~= nil and type(globals) ~= "table" then
    error("bad argument #2 to 'LoadString' (table expected, got "
          .. type(globals) .. ")", 2)
  end
  if globals ~= nil then
    return Ext._Internal.RawLoad(text, "?", "t", globals)
  end
  return Ext._Internal.RawLoad(text, "?", "t")
end

-- Upstream's Include: a mod's script (by its UUID or name), a builtin://
-- script from bg3se's bundle, or a game file. Failures are logged and
-- return nothing, as upstream's are. Upstream swaps the registry's globals
-- while it runs, so a nested Include without its own globals sees the
-- outer one's; include_globals is that.
local include_globals = nil

function Ext.Utils.Include(modGuid, fileName, globals)
  if globals ~= nil and type(globals) ~= "table" then
    error("bad argument #3 to 'Include' (table expected, got "
          .. type(globals) .. ")", 2)
  end
  fileName = tostring(fileName)
  local text, name
  if modGuid ~= nil then
    local reader = Ext._Internal.ModReader(modGuid)
    if reader == nil then
      Ext.Log.PrintError("Mod does not exist or is not loaded: " .. tostring(modGuid))
      return
    end
    text, name = reader.Read("Lua/" .. fileName), reader.Name .. "/" .. fileName
    if text == nil then
      Ext.Log.PrintError("Script file could not be opened: Mods/" .. reader.Name
                         .. "/ScriptExtender/Lua/" .. fileName)
      return
    end
  elseif fileName:sub(1, 10) == "builtin://" then
    text, name = Ext._Internal.BuiltinFile(fileName:sub(11)), fileName
    if text == nil then
      Ext.Log.PrintError("Builtin Lua script file could not be opened: " .. fileName:sub(11))
      return
    end
  else
    text, name = Ext._Internal.LoadFile(fileName, "data"), fileName
    if text == nil then
      Ext.Log.PrintError("Script file could not be opened: " .. fileName)
      return
    end
  end

  local env = globals or include_globals
  local chunk, err
  if env ~= nil then
    chunk, err = Ext._Internal.RawLoad(text, name, "t", env)
  else
    chunk, err = Ext._Internal.RawLoad(text, name, "t")
  end
  if chunk == nil then
    Ext.Log.PrintError("Failed to parse script: " .. tostring(err))
    return
  end
  local outer = include_globals
  include_globals = env
  local results = table.pack(xpcall(chunk, debug.traceback))
  include_globals = outer
  if not results[1] then
    Ext.Log.PrintError("Failed to execute script: " .. tostring(results[2]))
    return
  end
  return table.unpack(results, 2, results.n)
end

-- Upstream's BuiltinLibrary.lua, verbatim in effect.
function Ext.Utils.LoadTestLibrary()
  local env = {}
  env._G = env
  setmetatable(env, {__index = _G})
  Ext.Test = env
  if Ext.IsServer() then
    Ext.Utils.Include(nil, "builtin://Tests/ServerTestRunner.lua", env)
  else
    Ext.Utils.Include(nil, "builtin://Tests/ClientTestRunner.lua", env)
  end
end

-- Profiling is Optick upstream, which is not built here. The calls keep
-- their shape so instrumented code runs, and the scopes are counted so the
-- data is not simply thrown away.
local profile_depth = 0

function Ext.Utils.ProfileBegin() profile_depth = profile_depth + 1 end
function Ext.Utils.ProfileEnd()
  if profile_depth > 0 then profile_depth = profile_depth - 1 end
end

function Ext.Utils.Profile(_, fn, ...) return fn(...) end
function Ext.Utils.ProfileNamed(_, fn, ...) return fn(...) end

function Ext.Utils.GetGameState()
  return Ext._Internal.GameState and Ext._Internal.GameState() or "Running"
end

-- The engine's ls::GlobalSwitches, live, as upstream's; nil if it is not
-- where this build keeps it. See src/vendor/global_switches.cpp.
function Ext.Utils.GetGlobalSwitches()
  local addr = Ext._Internal.GlobalSwitches()
  if addr == nil then return nil end
  return Ext._Internal.PointedObject(addr, "GlobalSwitches")
end

-- ---- Ext.Input ----
--
-- Upstream's client module.
if Ext._Internal.IsClientState() then
  Ext.Input = Ext.Input or {}
  function Ext.Input.GetInputManager()
    local addr = Ext._Internal.InputManager()
    if addr == nil then return nil end
    return Ext._Internal.ReadObject(addr, "input::InputManager", "", {})
  end

  -- Upstream's InjectKey*: an SDLScanCode label or number, and optionally an
  -- SDLKeyModifier label, list of labels or number.
  local function enum_number(enum, value, what)
    if type(value) == "number" then return math.tointeger(value) end
    local _, bits = Ext._Internal.BitfieldOf(value)
    if bits ~= nil then return bits end
    if type(value) == "table" then
      local bits = 0
      for _, label in ipairs(value) do bits = bits | enum_number(enum, label, what) end
      return bits
    end
    if type(value) == "string" then
      local i = 0
      while true do
        local label, n = Ext._Internal.EnumValueAt(enum, i)
        if label == nil then break end
        if label == value then return n end
        i = i + 1
      end
    end
    error(string.format("%s: not a %s: %s", what, enum, tostring(value)), 3)
  end

  local function inject(down, key, modifiers, what)
    local mods = modifiers ~= nil and enum_number("SDLKeyModifier", modifiers, what) or 0
    Ext._Internal.InjectKey(down, enum_number("SDLScanCode", key, what), mods)
  end

  function Ext.Input.InjectKeyPress(key, modifiers)
    inject(true, key, modifiers, "Ext.Input.InjectKeyPress")
    inject(false, key, modifiers, "Ext.Input.InjectKeyPress")
  end
  function Ext.Input.InjectKeyDown(key, modifiers)
    inject(true, key, modifiers, "Ext.Input.InjectKeyDown")
  end
  function Ext.Input.InjectKeyUp(key, modifiers)
    inject(false, key, modifiers, "Ext.Input.InjectKeyUp")
  end

  -- Upstream's client Audio module; src/vendor/audio.cpp.
  Ext.Audio = {}
  local INVALID = -1  -- InvalidSoundObjectId, 0xffffffffffffffff

  -- Upstream's GetSoundObjectId: nil or "Global" is no object, a built-in
  -- name takes an optional player digit, an entity is its sound's object.
  local function sound_object(v)
    if v == nil then return INVALID end
    if type(v) == "string" then
      local name, player = v, 0
      local last = v:sub(-1)
      if #v > 1 and last >= "1" and last <= "4" then
        player, name = tonumber(last) - 1, v:sub(1, -2)
      end
      if name == "Global" then return INVALID end
      local id = Ext._Internal.Audio("Builtin", name, player)
      if id == nil then error("Unknown built-in sound object name: " .. name, 3) end
      return id
    end
    if type(v) == "userdata" and Ext._Internal.EntityProxyHandle(v) ~= nil then
      local ok, id = pcall(function()
        local s = v.Sound
        return s ~= nil and s.ActiveData ~= nil and s.ActiveData.SoundObjectId or nil
      end)
      if ok and math.type(id) == "integer" then return id end
      return INVALID
    end
    error("Must specify nil, entity handle or built-in name as sound object", 3)
  end

  local A = function(...) return Ext._Internal.Audio(...) end
  function Ext.Audio.SetSwitch(obj, group, state) return A("SetSwitch", sound_object(obj), group, state) end
  function Ext.Audio.SetState(group, state) return A("SetState", group, state) end
  function Ext.Audio.SetRTPC(obj, name, value, bypass)
    return A("SetRTPC", sound_object(obj), name, value, bypass == true)
  end
  function Ext.Audio.GetRTPC(obj, name) return A("GetRTPC", sound_object(obj), name) end
  function Ext.Audio.ResetRTPC(obj, name) A("ResetRTPC", sound_object(obj), name) end
  function Ext.Audio.Stop(obj)
    if obj == nil then A("Stop") else A("Stop", sound_object(obj)) end
  end
  function Ext.Audio.PauseAllSounds() A("PauseAllSounds") end
  function Ext.Audio.ResumeAllSounds() A("ResumeAllSounds") end
  function Ext.Audio.PostEvent(obj, event, position)
    return A("PostEvent", sound_object(obj), event, position)
  end
  function Ext.Audio.LoadEvent(event) return A("LoadEvent", event) end
  function Ext.Audio.UnloadEvent(event) return A("UnloadEvent", event) end
  function Ext.Audio.PlayExternalSound(obj, event, path, codec, position)
    return A("PlayExternalSound", sound_object(obj), event, path,
             enum_number("AudioCodec", codec, "Ext.Audio.PlayExternalSound"), position)
  end
  function Ext.Audio.LoadBank(bank) return A("LoadBank", bank) end
  function Ext.Audio.UnloadBank(bank) return A("UnloadBank", bank) end
  function Ext.Audio.PrepareBank(bank) return A("PrepareBank", bank) end
  function Ext.Audio.UnprepareBank(bank) return A("UnprepareBank", bank) end
end

-- ---- Ext.UI ----
--
-- Upstream's client module over the game's Noesis. A Noesis object is a
-- proxy around a light userdata, with upstream's methods and getters and the
-- object's own properties behind them; src/vendor/bg3le_noesis_lua.inl is
-- the C side. Commands and routed events are queued there and delivered by
-- the tick pump, so a handler cannot set Handled on the event it is given.
if Ext._Internal.IsClientState() then
  local I = Ext._Internal
  Ext.UI = {}

  local UiObject = {}
  local ptr_of = setmetatable({}, {__mode = "k"})
  local proxies = setmetatable({}, {__mode = "v"})

  local function wrap(ptr)
    if ptr == nil then return nil end
    local proxy = proxies[ptr]
    if proxy == nil then
      proxy = setmetatable({}, UiObject)
      ptr_of[proxy] = ptr
      proxies[ptr] = proxy
    end
    return proxy
  end

  local function unwrap(value)
    if Ext._Internal.RawGetMetatable(value) == UiObject then return ptr_of[value] end
    return value
  end

  -- For checking event routing: raises a routed event on an element.
  function Ext._Internal.UiRaise(element, event)
    return I.UiRaiseEvent(unwrap(element), event)
  end

  local function out(value)
    if type(value) == "userdata" then return wrap(value) end
    return value
  end

  local function wrap_all(t)
    for k, v in pairs(t) do t[k] = out(v) end
    return t
  end

  -- Handler ids the C side queues against: commands and event subscriptions.
  local handlers = {}
  local next_handler = 0
  local command_handler = {}
  local subscription_handler = {}

  local function add_handler(fn)
    next_handler = next_handler + 1
    handlers[next_handler] = fn
    return next_handler
  end

  local function no_property(ptr, name)
    return string.format("Object %s has no property named '%s'",
                         I.UiTypeName(ptr), tostring(name))
  end

  local methods = {}

  function methods:GetProperty(name)
    local found, value = I.UiGet(ptr_of[self], name)
    if not found then
      Ext.Log.PrintError(no_property(ptr_of[self], name))
      return nil
    end
    return out(value)
  end

  function methods:SetProperty(name, value)
    if not I.UiSet(ptr_of[self], name, unwrap(value)) then
      Ext.Log.PrintError(no_property(ptr_of[self], name))
    end
  end

  function methods:GetAllProperties()
    return wrap_all(I.UiProperties(ptr_of[self], "all"))
  end
  function methods:DirectProperties()
    return wrap_all(I.UiProperties(ptr_of[self], "direct"))
  end
  function methods:DependencyProperties()
    return wrap_all(I.UiProperties(ptr_of[self], "dependency"))
  end
  function methods:ToString() return I.UiToString(ptr_of[self]) end
  function methods:VisualChild(i) return wrap(I.UiVisualChild(ptr_of[self], i)) end
  function methods:At(i) return wrap(I.UiCollectionItem(ptr_of[self], i)) end
  function methods:Child(i) return wrap(I.UiChild(ptr_of[self], i)) end
  function methods:Find(name) return wrap(I.UiFind(ptr_of[self], name)) end
  function methods:Resource(key, full)
    return wrap(I.UiResource(ptr_of[self], key, full == true))
  end
  function methods:CanExecute(arg) return I.UiCanExecute(ptr_of[self], unwrap(arg)) end
  function methods:Execute(arg) I.UiExecute(ptr_of[self], unwrap(arg)) end

  function methods:SetHandler(fn)
    local ptr = ptr_of[self]
    local old = command_handler[ptr]
    if old ~= nil then handlers[old] = nil end
    local id = fn ~= nil and add_handler(fn) or 0
    command_handler[ptr] = fn ~= nil and id or nil
    I.UiSetHandler(ptr, id)
  end

  function methods:Subscribe(event, fn)
    local id = add_handler(fn)
    local index = I.UiSubscribe(ptr_of[self], event, id)
    if index == nil then
      handlers[id] = nil
      return nil
    end
    subscription_handler[index] = id
    return index
  end

  function methods:Unsubscribe(index)
    local id = subscription_handler[index]
    if id ~= nil then handlers[id] = nil end
    subscription_handler[index] = nil
    return I.UiUnsubscribe(index)
  end

  local getters = {
    Type = function(ptr) return I.UiTypeName(ptr) end,
    VisualChildrenCount = function(ptr) return I.UiVisualCount(ptr) end,
    VisualParent = function(ptr) return wrap(I.UiVisualParent(ptr)) end,
    ChildrenCount = function(ptr) return I.UiChildCount(ptr) end,
    Parent = function(ptr) return wrap(I.UiParent(ptr)) end,
  }

  UiObject.__index = function(self, key)
    local method = methods[key]
    if method ~= nil then return method end
    local ptr = ptr_of[self]
    -- A collection indexes from 1, as upstream's array proxy does.
    if math.type(key) == "integer" and I.UiCollectionCount(ptr) ~= nil then
      return wrap(I.UiCollectionItem(ptr, key))
    end
    local getter = getters[key]
    if getter ~= nil then return getter(ptr) end
    local found, value = I.UiGet(ptr, key)
    if found then return out(value) end
    -- Raised as upstream's LightObjectProxy does; only :GetProperty logs and returns nil.
    error(string.format("Property does not exist: %s::%s - property does not exist",
                        I.UiProxyTypeName(ptr), tostring(key)), 0)
  end

  UiObject.__newindex = function(self, key, value)
    local ptr = ptr_of[self]
    if not I.UiSet(ptr, key, unwrap(value)) then
      error(string.format("Cannot set property %s::%s - property does not exist",
                          I.UiProxyTypeName(ptr), tostring(key)), 0)
    end
  end

  UiObject.__tostring = function(self)
    return string.format("%s (%s)", I.UiTypeName(ptr_of[self]), tostring(ptr_of[self]))
  end

  -- # and pairs over a collection's items; other objects keep reading as empty.
  UiObject.__len = function(self)
    return I.UiCollectionCount(ptr_of[self]) or 0
  end

  UiObject.__pairs = function(self)
    local ptr = ptr_of[self]
    local n = I.UiCollectionCount(ptr)
    if n == nil then return next, {}, nil end
    local i = 0
    return function()
      i = i + 1
      if i > n then return nil end
      return i, wrap(I.UiCollectionItem(ptr, i))
    end
  end

  function Ext.UI.GetRoot() return wrap(I.UiRoot()) end

  -- A property's WriteCallback crosses as a handler id, as a command's does.
  function Ext.UI.RegisterType(name, properties, wrappedContext)
    local props = {}
    for key, defn in pairs(properties or {}) do
      local copy = {}
      if type(defn) == "table" then
        for k, v in pairs(defn) do copy[k] = v end
        if type(defn.WriteCallback) == "function" then copy.WriteCallback = add_handler(defn.WriteCallback) end
      end
      props[key] = copy
    end
    return I.UiRegisterType(name, props, wrappedContext)
  end

  function Ext.UI.Instantiate(name, wrappedContext)
    return wrap(I.UiInstantiate(name, unwrap(wrappedContext)))
  end

  function Ext.UI.SetState()
    Ext.Log.PrintError("Ext.UI.SetState(): Deprecated")
  end

  function Ext.UI.EnableErrorReporting(enable) end

  -- Upstream's engine getters; nil where the object is not there.
  for name, what in pairs({GetStateMachine = "StateMachine", GetPickingHelper = "PickingHelper",
                           GetCursorControl = "CursorControl", GetDragDrop = "DragDrop"}) do
    Ext.UI[name] = function(player)
      local at, cls = I.UiObject(what, player)
      if at == nil then return nil end
      return Ext._Internal.PointedObject(at, cls)
    end
  end

  local function call(fn, ...)
    local ok, err = xpcall(fn, debug.traceback, ...)
    if not ok then
      Ext.Log.PrintError("Error while dispatching UI event: " .. tostring(err))
    end
  end

  -- Upstream calls an event handler inside Noesis's routing, so it can set
  -- Handled; so does bg3le, on the UI thread under the client state's lock.
  function Ext._Internal.UiEventNow(id, sender, event, source)
    local fn = handlers[id]
    if fn == nil then return false end
    local args = {RoutedEvent = event, Source = wrap(source), Handled = false}
    call(fn, wrap(sender), args)
    return args.Handled == true
  end

  function Ext._Internal.UiPump()
    while true do
      local id, command, parameter = I.UiTakeCommand()
      if id == nil then break end
      local fn = handlers[id]
      if fn ~= nil then call(fn, wrap(command), wrap(parameter)) end
    end
    -- Upstream's DeferredUIEvents::PostUpdate: handler(object, property).
    while true do
      local id, object, property = I.UiTakeWrite()
      if id == nil then break end
      local fn = handlers[id]
      if fn ~= nil then call(fn, wrap(object), property) end
    end
    while true do
      local id, sender, event, source = I.UiTakeEvent()
      if id == nil then break end
      local fn = handlers[id]
      if fn ~= nil then
        call(fn, wrap(sender), {RoutedEvent = event, Source = wrap(source),
                                Handled = false})
      end
    end
  end
end

-- Upstream's: the server's dlg::DialogManager, nil on the client.
function Ext.Utils.GetDialogManager()
  local addr = Ext._Internal.DialogManager()
  if addr == nil then return nil end
  return Ext._Internal.PointedObject(addr, "dlg::DialogManager")
end


-- One prelude builds both states, so these answer for the state they are
-- asked in rather than being written down.
function Ext.IsServer() return not Ext._Internal.IsClientState() end
function Ext.IsClient() return Ext._Internal.IsClientState() end

-- Only what is still unimplemented, and only if nothing has defined it
-- already: this used to assign unconditionally, which quietly replaced
-- Ext.IO and Ext.Debug with stubs because they are defined further up.
-- "Loca" rather than "Localization" -- the latter is not a module bg3se
-- has, so a mod asking for Ext.Loca got a nil index instead of the error
-- the stub exists to give.
-- Plain tables rather than stubs: every one of these is filled in at the
-- end of the prelude, and a stub's __index would shadow what is put there.
for _, name in ipairs({"Entity", "Stats", "Level", "StaticData", "Mod",
                       "Loca", "Resource", "Template"}) do
  if Ext[name] == nil then Ext[name] = {} end
end
Ext.Definition = Ext.StaticData
Mods = {}

-- Osi cannot be bound until a story is loaded (the engine generates its
-- function table on demand), so until then explain the situation rather
-- than letting every Osiris name look like a typo.
Osi = setmetatable({}, {__index = function(_, key)
  if Ext.IsClient() then
    -- Osiris runs server-side, and upstream's client state has no Osi
    -- either. Saying which context this is beats blaming the save.
    error(string.format(
      "bg3le: Osi.%s is not available in the client context; Osiris is "
      .. "server-side", key), 0)
  end
  error(string.format(
    "bg3le: Osiris is not bound yet (no story loaded), so Osi.%s is "
    .. "unavailable -- load a save first", key), 0)
end})

-- Server-side only. This exists so an Osiris name used before a save is
-- loaded says so instead of reading as a typo, and it is replaced by the
-- real resolver once Osiris binds. Nothing binds Osiris in the client
-- context, so installing it there makes it permanent -- and then the
-- ordinary `if SomeGlobal then` raises. Mod Configuration Menu's client
-- script does exactly that.
if Ext.IsServer() then
  setmetatable(_G, {__index = function(_, key)
    error(string.format(
      "bg3le: '%s' is not defined. Osiris functions become available as "
      .. "globals once a save is loaded.", key), 0)
  end})
end

_D = Ext.Dump
_DS = Ext.DumpShallow
Ext.Timer.ClockEpoch = Ext._Internal.ClockEpoch
Ext.Timer.ClockTime = Ext._Internal.ClockTime
Ext.Timer.MonotonicTime = Ext._Internal.MonotonicTime
Ext.Timer.MicrosecTime = Ext._Internal.MicrosecTime

_P = Ext.Log.Print
_PW = Ext.Log.PrintWarning

-- Upstream's BuiltinLibrary helpers: the host character and its weapon on
-- the server, the controlled character on the client.
if Ext._Internal.IsClientState() then
  function _C()
    for _, entity in pairs(Ext.Entity.GetAllEntitiesWithComponent("ClientControl")) do
      if entity.ClientCharacter and entity.ClientCharacter.ReservedUserID == 1 then
        return entity
      end
    end
    return nil
  end
else
  function _C() return Ext.Entity.Get(Osi.GetHostCharacter()) end
  function _W()
    return Ext.Entity.Get(Osi.GetEquippedWeapon(Osi.GetHostCharacter())
                          or "00000000-0000-0000-0000-000000000000")
  end
end
_PE = Ext.Log.PrintError
Print = Ext.Log.Print
print = Ext.Log.Print

-- ---- Ext.Timer ----
--
-- Server timers fire on the story thread, so callbacks may call Osiris.
-- Delays are in milliseconds, upstream's unit. As upstream's, both queues
-- run on the GameTime the engine hands that side's update: a delay counts
-- from the last update (0 before a state's first), and a timer fires on the
-- first update whose time has passed it. Unhooked, the monotonic clock stands in.
local timers, next_handle = {}, 1
-- The last update's GameTime, and its Time in milliseconds.
local clock = {ms = 0.0, time = nil}

-- Upstream marks realtime timers with a flag in the handle and reads it
-- back to pick a queue; the same bit is used here for the same reason.
local REALTIME_FLAG = 0x40000000

local function add_timer(ms, fn, repeat_ms, realtime)
  if type(fn) ~= "function" then
    error("Ext.Timer expects a callback function", 3)
  end
  local handle = next_handle
  next_handle = handle + 1
  if realtime then handle = handle | REALTIME_FLAG end

  timers[handle] = {
    due = clock.ms + (ms or 0),
    fn = fn,
    every = repeat_ms,
    paused = false,
  }
  return handle
end

function Ext.Timer.WaitFor(ms, fn, repeat_ms)
  return add_timer(ms, fn, repeat_ms, false)
end

function Ext.Timer.WaitForRealtime(ms, fn, repeat_ms)
  return add_timer(ms, fn, repeat_ms, true)
end

function Ext.Timer.Cancel(handle)
  if timers[handle] == nil then return false end
  timers[handle] = nil
  return true
end

function Ext.Timer.Pause(handle)
  local t = timers[handle]
  if t == nil then return false end
  if not t.paused then
    t.paused = true
    -- Held as remaining time, so resuming does not fire immediately for a
    -- timer that was paused past its due moment.
    t.remaining = math.max(0, t.due - clock.ms)
  end
  return true
end

function Ext.Timer.Resume(handle)
  local t = timers[handle]
  if t == nil then return false end
  if t.paused then
    t.paused = false
    t.due = clock.ms + (t.remaining or 0)
    t.remaining = nil
  end
  return true
end

function Ext.Timer.IsPaused(handle)
  local t = timers[handle]
  return t ~= nil and t.paused
end

-- Persistent timers, as upstream's: a handler name and JSON arguments, so
-- they can be written into a save and fired after a load. The handler is
-- looked up when the timer fires, not when it is started.
local persistent_handlers = {}

function Ext.Timer.RegisterPersistentHandler(name, fn)
  persistent_handlers[name] = fn
end

local function persistent_timer(name, args_json)
  return function(handle)
    local fn = persistent_handlers[name]
    if fn == nil then
      Ext.Log.PrintWarning("Tried to fire persistent timer '" .. tostring(name)
                           .. "' but it has no callback registered!")
      return
    end
    fn(Ext.Json.Parse(args_json), handle)
  end
end

function Ext.Timer.WaitForPersistent(ms, callbackName, args, repeat_ms)
  if Ext.IsClient() then
    error("Persistent timers are only supported on the server", 2)
  end
  local args_json = Ext.Json.Stringify(args, {Beautify = false})
  local handle = add_timer(ms, persistent_timer(callbackName, args_json),
                           repeat_ms, false)
  timers[handle].persistent = {handler = callbackName, args = args_json}
  return handle
end

-- ---- Ext.Vars and persistent timers in the savegame ----
--
-- Upstream's UserVariableManager, ModVariableManager and TimerManager
-- SavegameVisit; src/savegame.cpp does the visiting. Values carry
-- upstream's UserVariableType: 1 Int64, 2 Double, 3 String, 4 Composite
-- (JSON), 5 Boolean, 6 CompositeBinary (read only).
local function saved_value(value)
  local t = type(value)
  if t == "boolean" then return 5, value end
  if t == "number" then
    if math.type(value) == "integer" then return 1, value end
    return 2, value
  end
  if t == "string" then return 3, value end
  if t == "table" or t == "userdata" then
    return 4, Ext.Json.Stringify(value, {Beautify = false})
  end
  return nil
end

local function restored_value(kind, value)
  if kind == 4 then return Ext.Json.Parse(value) end
  if kind == 6 then return Ext.Json.Parse(value, true) end
  return value
end

local function persistent_var(defs, name)
  local def = defs ~= nil and defs[name] or nil
  -- Upstream's Persistent defaults to true, and an unregistered variable
  -- has no prototype, so it is not written.
  return def ~= nil and def.Persistent ~= false
end

function Ext._Internal.CollectSaveExtras()
  local out = {user = {}, mod = {}, timers = {}}
  for guid, vars in pairs(user_variables) do
    for name, value in pairs(vars) do
      if persistent_var(user_variable_defs, name) then
        local kind, v = saved_value(value)
        if kind then out.user[#out.user + 1] = {guid, name, kind, v} end
      end
    end
  end
  for uuid, vars in pairs(mod_variables) do
    for name, value in pairs(vars) do
      if persistent_var(mod_variable_defs[uuid], name) then
        local kind, v = saved_value(value)
        if kind then out.mod[#out.mod + 1] = {uuid, name, kind, v} end
      end
    end
  end
  local now = clock.ms
  for _, t in pairs(timers) do
    if t.persistent ~= nil then
      local remaining = t.paused and (t.remaining or 0) or math.max(0, t.due - now)
      out.timers[#out.timers + 1] = {remaining / 1000.0, (t.every or 0) / 1000.0,
                                     t.paused == true, t.persistent.handler,
                                     t.persistent.args}
    end
  end
  return out
end

-- On a save read: what upstream's managers clear, cleared, then restored.
function Ext._Internal.RestoreSaveExtras()
  if Ext.IsClient() then return end
  local saved = Ext._Internal.TakeSavedExtras()
  if saved == nil then return end

  for guid in pairs(user_variables) do user_variables[guid] = nil end
  for uuid in pairs(mod_variables) do mod_variables[uuid] = {} end
  for handle, t in pairs(timers) do
    if t.persistent ~= nil then timers[handle] = nil end
  end

  for _, e in ipairs(saved.user) do
    local ok, value = pcall(restored_value, e[3], e[4])
    if ok then
      user_variables[e[1]] = user_variables[e[1]] or {}
      user_variables[e[1]][e[2]] = value
    end
  end
  for _, e in ipairs(saved.mod) do
    local ok, value = pcall(restored_value, e[3], e[4])
    if ok then
      mod_variables[e[1]] = mod_variables[e[1]] or {}
      mod_variables[e[1]][e[2]] = value
    end
  end
  for _, e in ipairs(saved.timers) do
    local handle = add_timer(e[1] * 1000.0, persistent_timer(e[4], e[5]),
                             e[2] > 0 and e[2] * 1000.0 or nil, false)
    timers[handle].persistent = {handler = e[4], args = e[5]}
    if e[3] then
      timers[handle].paused = true
      timers[handle].remaining = e[1] * 1000.0
    end
  end
end

-- The Time of the last update this state saw, as upstream's.
function Ext.Timer.GameTime()
  return clock.time ~= nil and clock.time.Time or 0.0
end

function Ext._Internal.RunTimers()
  -- Anything the other context sent since the last tick, first: a message is
  -- what a timer or a handler this tick may be waiting on.
  Ext._Internal.DrainNetMessages()
  -- And the variables written since, which SyncOnTick sends.
  Ext._Internal.FlushVars()

  -- And what the overlay's widgets queued, for the same reason.
  Ext._Internal.ImguiPump()
  if Ext._Internal.UiPump then Ext._Internal.UiPump() end

  -- And the component events the engine raised.
  if Ext._Internal.DeliverComponentEvents then
    Ext._Internal.DeliverComponentEvents()
  end

  -- The rest is upstream's OnUpdate, which runs once per engine update of
  -- this side, with its GameTime: timers, pathfinding, then Tick.
  local time, delta, ticks, unknown = Ext._Internal.EngineTime(Ext.IsClient())
  if time == nil then
    local now = Ext.Timer.MonotonicTime() / 1000.0
    clock.first = clock.first or now
    time = now - clock.first
    delta = clock.time ~= nil and (time - clock.time.Time) or 0.0
    ticks, unknown = (clock.time ~= nil and clock.time.Ticks or 0) + 1, 0.0
  end
  local last = clock.time
  if time == false or (last ~= nil and last.Time == time and last.Ticks == ticks) then
    return
  end
  clock.time = {Time = time, DeltaTime = delta, Ticks = ticks, Unknown = unknown}
  clock.ms = time * 1000.0
  local now = clock.ms

  -- Collected first: a callback that starts a timer adds to the table, and
  -- adding a key mid-pairs is "invalid key to 'next'". Upstream fires its
  -- realtime queue, then its game queue, each soonest first.
  local due = {}
  for handle, t in pairs(timers) do
    if not t.paused and now >= t.due then due[#due + 1] = handle end
  end
  table.sort(due, function(a, b)
    local ra, rb = (a & REALTIME_FLAG) ~= 0, (b & REALTIME_FLAG) ~= 0
    if ra ~= rb then return ra end
    local x, y = timers[a].due, timers[b].due
    if x ~= y then return x < y end
    return a < b
  end)
  for _, handle in ipairs(due) do
    local t = timers[handle]
    if t ~= nil then
      if t.every then t.due = now + t.every else timers[handle] = nil end
      local ok, err = xpcall(t.fn, debug.traceback, handle)
      if not ok then
        Ext.Log.PrintError("Error while dispatching user function call: "
                           .. tostring(err))
      end
    end
  end

  -- The pathfinding requests the engine has finished.
  if Ext._Internal.PathfindingUpdate then Ext._Internal.PathfindingUpdate() end

  -- TODO: Ticks and Unknown are upstream's misreading; see EngineTime in src/game_state.cpp.
  Ext._Internal.FireEvent("Tick", {Time = {Time = time, DeltaTime = delta, Ticks = ticks,
                                           Unknown = unknown}})
end


-- ---- Ext.Entity ----
--
-- Components are reached through bg3se's own field metadata rather than
-- through an accessor written per component, so every component it describes
-- is available by name with nothing listed here. Either name works: bg3se's
-- short one (entity.Health) or the engine's (entity["eoc::HealthComponent"]).
--
-- Reads and writes go straight to the component in place, so a value is never
-- stale and a write is visible to the next read. The proxy holds only the
-- handle, the name and the field table.
--
-- Naming a field that does not exist raises rather than reading as nil, and so
-- does one whose kind bg3le cannot convert yet. A mod guarding on
-- "if not entity.Foo.Bar then return end" would otherwise skip work and look
-- as though it had succeeded. Ext._Internal.ComponentFields(name) reports what
-- a component offers and the kind of each field.

-- An array field is a view on the component, not a copy of it.
--
-- Returning a plain table would read correctly and then swallow writes:
-- "component.Field[i] = v" would mutate a temporary that is discarded, with
-- nothing to notice, which is the one outcome worse than raising. So elements
-- are read and written through to the component one at a time.
--
-- Elements are reached by extending the path -- "Events[0].Amount" -- rather
-- than by holding an address, so a dynamic array works the same way a fixed
-- one does even though its elements live behind the container's buffer
-- pointer. It also means an element that is itself a struct is just a longer
-- path, so nothing here has to know about nesting.
--
-- One-based, as Lua is. Where the engine indexes by an enum whose first value
-- is None = 0, element 1 is that None slot; that offset is the engine's, and
-- bg3se presents it the same way.
--
-- The length is re-read on every access rather than captured, because a
-- dynamic array can grow or shrink between one access and the next.
--
-- __len and __pairs are defined because Ext.Json.Stringify uses # and pairs,
-- and both honour metamethods -- so a view still dumps like an array.
local make_fields
local make_map
local make_array

-- Upstream's set proxy: set[x] is whether x is in the set, set[x] = true or
-- false inserts or removes it, # counts it and pairs yields its keys in order.
-- Each change rewrites the whole set, which is the one write bg3le can make
-- to a hash set without desynchronising its buckets.
-- owner and path name the set, so an enum key compares by label or number.
function Ext._Internal.HashSetView(elemKind, owner, path, read_keys, write_keys,
                                   identity)
  local element = path .. "[0]"
  local function key_of(v)
    if v == nil then return nil end
    if elemKind == "entity" then return Ext._Internal.EntityProxyHandle(v) end
    if elemKind == "guid" then return type(v) == "string" and v:lower() or nil end
    if elemKind == "string" then return tostring(v) end
    v = Ext._Internal.EnumKey(owner, element, v)
    return math.tointeger(v) or v
  end
  local function find(keys, v)
    local want = key_of(v)
    if want == nil then return nil end
    for i = 1, #keys do
      if key_of(keys[i]) == want then return i end
    end
    return nil
  end
  return Ext._Internal.NewObjectProxy({
    __bg3leContainer = "array",
    __bg3leIdentity = identity,
    __bg3leAssign = write_keys,
    __bg3leSetAt = function(i) return read_keys()[i] end,
    __index = function(_, v) return find(read_keys(), v) ~= nil end,
    __newindex = function(_, v, present)
      local keys = read_keys()
      local at = find(keys, v)
      if present then
        if at ~= nil then return end
        keys[#keys + 1] = v
      else
        if at == nil then return end
        table.remove(keys, at)
      end
      write_keys(keys)
    end,
    __len = function() return #read_keys() end,
    __pairs = function(self)
      local keys = read_keys()
      return function(_, k)
        local i = (k or 0) + 1
        if keys[i] == nil then return nil end
        return i, keys[i]
      end, self, nil
    end,
  })
end

-- Upstream's erase and push_back, as arr[i] = nil and arr[#arr + 1] = v: the
-- list one shorter or longer, for the array to be assigned whole.
function Ext._Internal.ResizedList(current, n, i, v)
  local list = {}
  for j = 1, n do
    if j ~= i then
      -- A struct element as a plain table, refilled after the resize.
      local element = current(j)
      if Ext._Internal.Walkable(element) then element = Ext.Types.Serialize(element) end
      list[#list + 1] = element
    end
  end
  if v ~= nil then list[#list + 1] = v end
  return list
end

-- Reads whatever is at a path, whichever kind it turns out to be.
--
-- One dispatch, because there were three: the component view, an array
-- element and a map value each had their own. They drifted, and the array one
-- asked the *container* for its element kind -- which reports "struct" for an
-- element that is itself a container, so reading a std::optional holding a
-- std::array raised instead of returning the array. Asking about the element's
-- own path instead is both correct and the same question in every case.
local read_path

read_path = function(handle, comp, path)
  local kind = Ext._Internal.FieldInfo(comp, path)
  if kind == nil then
    error("bg3le: " .. comp .. "." .. path .. " does not resolve", 0)
  end

  if kind == "array" and Ext._Internal.IsHashSet(comp, path) then
    local _, elemKind = Ext._Internal.FieldInfo(comp, path)
    local function read_keys()
      local n, err = Ext._Internal.ArrayInfo(handle, comp, path)
      if n == nil then
        error("bg3le: cannot size " .. comp .. "." .. path .. ": "
              .. tostring(err), 0)
      end
      local keys = {}
      for i = 0, n - 1 do
        keys[i + 1] = read_path(handle, comp, path .. "[" .. i .. "]")
      end
      return keys
    end
    local function write_keys(keys)
      local ok, err = Ext._Internal.SetSet(handle, comp, path, keys)
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end
    return Ext._Internal.HashSetView(elemKind, comp, path, read_keys, write_keys, function()
      return "c:" .. handle .. ":" .. comp .. ":" .. path
    end)
  end
  -- A glm vector is a plain table, as upstream pushes it.
  if kind == "array" and not Ext._Internal.IsVector(comp, path) then
    return make_array(handle, comp, path)
  end
  if kind == "map" then return make_map(handle, comp, path) end

  -- An optional holds nought or one. Empty reads as nil, which is the answer
  -- bg3se gives too, and stays distinct from unreadable, which raises. A full
  -- one reads as whatever it holds -- and what it holds may itself be a
  -- container, which is the case that was broken.
  if kind == "optional" then
    local held, err = Ext._Internal.ArrayInfo(handle, comp, path)
    if held == nil then
      error("bg3le: cannot size " .. comp .. "." .. path .. ": "
            .. tostring(err), 0)
    end
    if held == 0 then return nil end
    return read_path(handle, comp, path .. "[0]")
  end

  -- A variant reads as whatever it currently holds. Which alternative that is
  -- is a runtime fact, so it is asked for rather than derived -- and only the
  -- live one resolves, since the bytes are not any of the others.
  if kind == "variant" then
    local active, count = Ext._Internal.VariantIndex(handle, comp, path)
    if active == nil then
      error("bg3le: " .. comp .. "." .. path .. ": " .. tostring(count), 0)
    end
    if active >= count then return nil end  -- valueless
    return read_path(handle, comp, path .. "[" .. active .. "]")
  end

  if kind == "struct" then
    local inner, err = Ext._Internal.ComponentFields(comp, path)
    if inner == nil then error("bg3le: " .. tostring(err), 0) end
    return make_fields(handle, comp, path, inner)
  end

  -- A pointer reads as what it points at, or nil; the view is lazy, so a
  -- cycle of pointers is only followed as far as it is asked about.
  if kind == "pointer" then
    local target, err = Ext._Internal.GetField(handle, comp, path)
    if target == nil then
      if err ~= nil then error("bg3le: " .. err, 0) end
      return nil
    end
    local class = Ext._Internal.PointeeClass(comp, path, false)
    if class == nil then return read_path(handle, comp, path .. "[0]") end
    return Ext._Internal.PointedObject(target, class)
  end

  local value, err = Ext._Internal.GetField(handle, comp, path)
  if value == nil and err ~= nil then error("bg3le: " .. err, 0) end
  if kind == "entity" or (kind == "entityorvec3" and math.type(value) == "integer") then
    return Ext._Internal.EntityValue(value)
  end
  return value
end

-- An array index as upstream reads it, with lua_tointeger: "2" and 2.0 are 2.
function Ext._Internal.ArrayIndex(i)
  local n = tonumber(i)
  return n ~= nil and math.tointeger(n) or i
end

make_array = function(handle, comp, path)
  -- Raises rather than reporting zero, for the reason in make_map below: a
  -- failure to resolve must not read as an empty array.
  local function length()
    local count, err = Ext._Internal.ArrayInfo(handle, comp, path)
    if count == nil then
      error("bg3le: cannot size " .. comp .. "." .. path .. ": "
            .. tostring(err), 0)
    end
    return count
  end

  local function element_path(i)
    local count = length()
    if type(i) ~= "number" or i < 1 or i > count then
      error("bg3le: " .. comp .. "." .. path .. " index " .. tostring(i)
            .. " is out of range 1.." .. count, 0)
    end
    return path .. "[" .. (i - 1) .. "]"
  end

  local function element(i)
    return read_path(handle, comp, element_path(i))
  end

  return Ext._Internal.NewObjectProxy({
    __bg3leContainer = "array",
    __bg3leIdentity = function()
      return "c:" .. handle .. ":" .. comp .. ":" .. path
    end,
    -- Out of range is nil, as upstream's ArrayProxy answers, which is also
    -- what stops ipairs.
    __index = function(_, i)
      i = Ext._Internal.ArrayIndex(i)
      if type(i) ~= "number" or i < 1 or i > length() then return nil end
      return element(i)
    end,
    __newindex = function(_, i, v)
      i = Ext._Internal.ArrayIndex(i)
      local n = length()
      local ok, err
      if type(i) == "number" and ((v == nil and i >= 1 and i <= n) or i == n + 1) then
        local list = Ext._Internal.ResizedList(element, n, i, v)
        ok, err = Ext._Internal.SetField(handle, comp, path, list)
        if ok then
          for j, item in ipairs(list) do
            if type(item) == "table" then
              local e = element(j)
              if Ext._Internal.Walkable(e) then Ext.Types.Unserialize(e, item) end
            end
          end
        end
      elseif type(i) == "number" and (i < 1 or i > n) then
        return  -- upstream's SetElement refuses it, and its __newindex ignores that
      else
        ok, err = Ext._Internal.SetField(handle, comp, element_path(i), v)
      end
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end,
    __len = length,
    __pairs = function(self)
      return function(_, k)
        local i = (k or 0) + 1
        if i > length() then return nil end
        return i, element(i)
      end, self, nil
    end,
  })
end

-- A map field is a view too, keyed the way the engine keys it.
--
-- The engine keeps a hash map's keys and values in two parallel runs, so slot
-- i holds key i alongside value i. That is what makes this presentable without
-- hashing anything from Lua: iterating is a walk over both runs, and a lookup
-- is that walk plus a comparison. Lookup is therefore linear rather than
-- hashed, which is fine for the maps on a component -- they hold a handful of
-- entries -- and it avoids needing the engine's own hash for every key type.
--
-- Keys of a kind bg3le cannot convert -- a FixedString, which would need the
-- engine's global string table -- leave the key side unavailable while the
-- values stay reachable by slot, which is what Entries() is for.
make_map = function(handle, comp, path)
  -- Raises rather than reporting zero. "or 0" here turned any failure to
  -- resolve into an empty map, which is the worst possible answer: a mod sees
  -- a container that is present and empty, and there is nothing to notice.
  -- It hid a real discrepancy in SummonContainer.ByTag for exactly as long as
  -- it took to compare against bg3se on Windows.
  local function count()
    local n, err = Ext._Internal.ArrayInfo(handle, comp, path)
    if n == nil then
      error("bg3le: cannot size " .. comp .. "." .. path .. ": "
            .. tostring(err), 0)
    end
    return n
  end

  local function key_at(i)
    local k, kind = Ext._Internal.MapKey(handle, comp, path, i)
    if kind == "entity" then return Ext._Internal.EntityValue(k) end
    return k
  end

  local function value_at(i)
    return read_path(handle, comp, path .. "[" .. i .. "]")
  end

  -- Slot of a key, or nil. Linear, as above.
  local function slot_of(key)
    for i = 0, count() - 1 do
      if key_at(i) == key then return i end
    end
    return nil
  end

  return Ext._Internal.NewObjectProxy({
    __bg3leContainer = "map",
    __bg3leIdentity = function()
      return "c:" .. handle .. ":" .. comp .. ":" .. path
    end,
    __index = function(_, key)
      -- A method rather than a field, so a map whose keys cannot be converted
      -- is still walkable.
      if key == "Entries" then
        return function()
          local out = {}
          for i = 0, count() - 1 do
            out[i + 1] = {Key = key_at(i), Value = value_at(i)}
          end
          return out
        end
      end
      local i = slot_of(key)
      if i == nil then return nil end
      return value_at(i)
    end,
    -- As upstream's map proxy: a new key is added, and nil removes one. A
    -- table for a struct value is a default value filled from it, replacing
    -- any existing one, as upstream's get<TValue> and insert make it.
    __newindex = function(_, key, value)
      local i = slot_of(key)
      if value == nil then
        if i ~= nil then
          local ok, err = Ext._Internal.MapEdit(handle, comp, path, key, false)
          if not ok then error("bg3le: " .. tostring(err), 0) end
        end
        return
      end
      local struct = type(value) == "table"
        and Ext._Internal.FieldInfo(comp, path .. "[0]") == "struct"
      if struct and i ~= nil and Ext._Internal.MapEdit(handle, comp, path, key, false) then
        i = nil
      end
      if i == nil then
        local ok, err = Ext._Internal.MapEdit(handle, comp, path, key, true)
        if not ok then error("bg3le: " .. tostring(err), 0) end
        i = slot_of(key)
        if i == nil then error("bg3le: " .. comp .. "." .. path .. " did not take the key", 0) end
      end
      if struct then
        Ext.Types.Unserialize(value_at(i), value)
        return
      end
      local ok, err = Ext._Internal.SetField(
        handle, comp, path .. "[" .. i .. "]", value)
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end,
    __len = count,
    -- Iterating yields key, value.
    --
    -- A key that cannot be converted yields a placeholder rather than ending
    -- the iteration. Ending it made a map that holds entries render as {},
    -- which is indistinguishable from an empty one: SummonContainer.ByTag
    -- holds two tagged entries whose FixedString keys bg3le cannot read, and
    -- it dumped as empty. The values are perfectly reachable, so hiding them
    -- was the worst of the options -- a placeholder is visible, stopping was
    -- not.
    __pairs = function(self)
      local i = -1
      return function()
        i = i + 1
        if i >= count() then return nil end
        local k = key_at(i)
        -- Any placeholder carries the slot number, because two keys that
        -- cannot be read still have to be distinct: identical ones collide
        -- into a single entry and the map reads as shorter than it is. A
        -- placeholder always arrives bracketed, which is also what keeps it
        -- from being mistaken for a real key.
        if k == nil then
          k = "<unreadable key " .. i .. ">"
        elseif type(k) == "string" and k:sub(1, 1) == "<" and k:sub(-1) == ">" then
          k = k:sub(1, -2) .. " at slot " .. i .. ">"
        end
        return k, value_at(i)
      end, self, nil
    end,
  })
end

-- A view over a set of fields, used for a component, for a struct nested
-- inside one, and for a struct that is an array element; the only difference
-- is the path prefix.
-- The reflected type of a view, memoised.
--
-- A component answers to three names and only the class name keys the type
-- registry; a nested struct's type comes from the field that declares it.
-- Both are one call, and the set of (component, path) pairs is small, so the
-- answer is kept rather than asked for per view -- a component is read often
-- enough for that to matter.
local view_types = {}

local function type_of_view(comp, prefix)
  local key = comp .. "\0" .. prefix
  local found = view_types[key]
  if found ~= nil then
    if found == false then return nil end
    return found
  end

  local name = Ext._Internal.ViewTypeName(comp, prefix)
  view_types[key] = name or false
  return name
end

make_fields = function(handle, comp, prefix, fields, identity)
  local function path_to(key)
    if prefix == "" then return key end
    return prefix .. "." .. key
  end

  return Ext._Internal.NewObjectProxy({
    -- What Ext.Types.GetObjectType reports, and what a custom member is
    -- registered against.
    __name = type_of_view(comp, prefix),
    -- A pointed-at object is one object however it was reached, which is
    -- what lets AvoidRecursion stop a walk through a pointer graph.
    __bg3leIdentity = identity or function()
      return "c:" .. handle .. ":" .. comp .. ":" .. prefix
    end,
    __index = function(self, key)
      local kind = fields[key]
      if kind == nil then
        -- A member a mod grafted on with Ext.Types.AddCustomFunction or
        -- AddCustomProperty, which is where upstream's property map would
        -- have answered.
        local extra = Ext._Internal.CustomMember(type_of_view(comp, prefix),
                                                 key)
        if extra ~= nil then
          if extra.Fn ~= nil then return extra.Fn end
          return extra.Get(self)
        end

        -- P_BITMASK: one flag of a flags field, as a boolean.
        if type(key) == "string" then
          local flag = Ext._Internal.GetField(handle, comp, path_to(key))
          if type(flag) == "boolean" then return flag end
        end

        local where = prefix == "" and comp or (comp .. "." .. prefix)
        error("bg3le: " .. where .. " has no field " .. tostring(key), 0)
      end
      -- The field table says which kind it is, but read_path asks again
      -- about the field's own path. Both agree; going through the one
      -- dispatch is what keeps the three call sites from drifting.
      return read_path(handle, comp, path_to(key))
    end,
    __newindex = function(self, key, value)
      if fields[key] == nil then
        local extra = Ext._Internal.CustomMember(type_of_view(comp, prefix),
                                                 key)
        if extra ~= nil then
          if extra.Set == nil then
            error("bg3le: " .. tostring(key) .. " is read-only", 0)
          end
          extra.Set(self, value)
          return
        end
      end

      local path = path_to(key)
      local ok, err = Ext._Internal.SetField(handle, comp, path, value)
      -- A hash set is the one field a plain write refuses on purpose: its
      -- keys cannot be written in place. A table is the whole set.
      if not ok and type(value) == "table" and tostring(err):find("is read-only", 1, true) then
        ok, err = Ext._Internal.SetSet(handle, comp, path, value)
      elseif ok and type(value) == "table" and value[1] ~= nil then
        Ext._Internal.FillStructElements(self[key], value)
      end
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end,
    -- Iterating yields field names and their values, so dumping a component
    -- shows what it holds. A field of a kind bg3le cannot convert yields the
    -- marker string instead of raising the way direct access does -- a dump
    -- has to be able to walk the whole component, and Ext.Json.Stringify
    -- reads each key back through __index, so raising there would make any
    -- component with one unconvertible field undumpable. The marker is a
    -- string rather than nil for the usual reason: nil would read as absent.
    __pairs = function(self)
      local key
      local extras, extra = nil, 0
      return function()
        if extras == nil then
          local kind
          key, kind = next(fields, key)
          if key ~= nil then
            if kind == "unsupported" then return key, "<unsupported>" end
            local ok, value = pcall(function() return self[key] end)
            if not ok then return key, "<unreadable>" end
            -- A container view sizes itself lazily; one that cannot be sized
            -- would stop the walk later, so it is reported here instead.
            if (kind == "array" or kind == "map") and type(value) == "userdata"
               and not pcall(function() return #value end) then
              return key, "<unreadable>"
            end
            return key, value
          end
          extras = Ext._Internal.CustomMemberNames(type_of_view(comp, prefix))
        end
        extra = extra + 1
        local name = extras[extra]
        if name == nil then return nil end
        return name, Ext._Internal.CustomMemberValue(
          self, type_of_view(comp, prefix), name)
      end, self, nil
    end,
  })
end

local function make_component(handle, name, fields)
  return make_fields(handle, name, "", fields)
end

-- nil if bg3se has no metadata for the name, or if this entity does not carry
-- the component. Those are different answers, but both mean "not available
-- here", which is what a script branches on.
local function get_component(handle, name)
  local fields = Ext._Internal.ComponentFields(name)
  if fields == nil then return nil end
  if not Ext._Internal.HasComponent(handle, name) then return nil end
  return make_component(handle, name, fields)
end

-- An entity is a userdata holding its handle, as upstream's are.
local function handle_of(entity)
  return Ext._Internal.EntityProxyHandle(entity)
end

local entity_methods = {}

function entity_methods:GetComponent(name)
  return get_component(handle_of(self), name)
end

-- Every readable component the entity's storage holds, keyed by
-- upstream's component type name. Upstream adds the ones still pending in
-- the command buffer; see kCommandBuffer below for why those are not here.
function entity_methods:GetAllComponents()
  local handle = handle_of(self)
  local names, short = Ext._Internal.EntityComponentNames(handle, false)
  local out = {}
  for _, name in ipairs(names) do
    local key = short[name]
    if key then out[key] = get_component(handle, key) end
  end
  return out
end

-- The rest of upstream's EntityProxyMetatable methods.

function entity_methods:HasRawComponent(name)
  return Ext._Internal.HasComponent(handle_of(self), name)
end

function entity_methods:IsAlive()
  return Ext._Internal.EntityAlive(handle_of(self))
end

-- Engine names; requireMapped keeps only those with (true) or without
-- (false) a component type Lua can read.
function entity_methods:GetAllComponentNames(requireMapped)
  local names, short = Ext._Internal.EntityComponentNames(handle_of(self),
                                                         false)
  if requireMapped == nil then return names end
  local out = {}
  for _, name in ipairs(names) do
    if (short[name] ~= false) == requireMapped then out[#out + 1] = name end
  end
  return out
end

function entity_methods:GetChangedComponents()
  local handle = handle_of(self)
  local names, short = Ext._Internal.EntityComponentNames(handle, true)
  local out = {}
  for _, name in ipairs(names) do
    if short[name] then out[short[name]] = get_component(handle, short[name]) end
  end
  return out
end

function entity_methods:MarkChanged(name)
  return Ext._Internal.MarkChanged(handle_of(self), name) == true
end

function entity_methods:WasChanged(name)
  return Ext._Internal.EntityWasChanged(handle_of(self), name)
end

function entity_methods:GetReplicationFlags(name, qword)
  return Ext._Internal.EntityReplicationFlags(handle_of(self), name,
                                              qword)
end

-- Upstream's ReplicateComponent: OR the flags into the qword and mark the
-- replication dirty, or say why it cannot.
local function replicate(entity, name, flags, qword)
  local ok, err = Ext._Internal.Replicate(handle_of(entity), name,
                                          flags, qword)
  if not ok then Ext.Log.PrintError(tostring(err)) end
end

function entity_methods:SetReplicationFlags(name, flags, qword)
  replicate(self, name, flags, qword or 0)
end

function entity_methods:Replicate(name)
  replicate(self, name, nil, 0)
end

-- OnCreate(component, fn[, deferred[, once]]) and its fixed variants, as
-- Ext.Entity's with this entity.
local function entity_on(kind, deferredKind)
  return function(self, component, handler, deferred, once)
    return Ext._Internal.SubscribeEntity(deferred and deferredKind or kind,
                                         component, handler, self, once == true)
  end
end

local function entity_on_fixed(kind, once)
  return function(self, component, handler)
    return Ext._Internal.SubscribeEntity(kind, component, handler, self, once)
  end
end

entity_methods.OnCreate = entity_on("create", "create-deferred")
entity_methods.OnCreateDeferred = entity_on_fixed("create-deferred", false)
entity_methods.OnCreateDeferredOnce = entity_on_fixed("create-deferred", true)
entity_methods.OnCreateOnce = entity_on_fixed("create", true)
entity_methods.OnDestroy = entity_on("destroy", "destroy-deferred")
entity_methods.OnDestroyDeferred = entity_on_fixed("destroy-deferred", false)
entity_methods.OnDestroyDeferredOnce = entity_on_fixed("destroy-deferred", true)
entity_methods.OnDestroyOnce = entity_on_fixed("destroy", true)

function entity_methods:OnChanged(component, handler, flags)
  return Ext._Internal.SubscribeEntity("change", component, handler, self,
                                       false, flags)
end

-- Upstream's component edits and current-frame queries, through the
-- calling thread's entity command buffer or the immediate cache.
do
  local function create(self, name, immediate)
    local addr, class = Ext._Internal.EntityComponentAdd(handle_of(self), tostring(name), immediate)
    if addr == nil then
      Ext.Log.PrintError("Unable to construct components of this type: " .. tostring(name))
      return nil
    end
    return Ext._Internal.PointedObject(addr, class)
  end
  function entity_methods:CreateComponent(name) return create(self, name, false) end
  function entity_methods:CreateComponentImmediate(name) return create(self, name, true) end
  function entity_methods:RemoveComponent(name)
    return Ext._Internal.EntityComponentRemove(handle_of(self), tostring(name), false)
  end
  function entity_methods:RemoveComponentImmediate(name)
    return Ext._Internal.EntityComponentRemove(handle_of(self), tostring(name), true)
  end

  -- The first change of that type decides, as upstream's loop does.
  local function change_of(self, name)
    for _, c in ipairs(Ext._Internal.EntityEcbChanges(handle_of(self)) or {}) do
      if c[1] == name then return c[2] end
    end
    return nil
  end
  function entity_methods:WasAdded(name)
    local c = change_of(self, name)
    return c ~= nil and c ~= false
  end
  function entity_methods:WasRemoved(name) return change_of(self, name) == false end

  -- ecs::EntityChangeFlags: Create 1, Destroy 2.
  local function has_flag(self, bit)
    local _, flags = Ext._Internal.EntityEcbChanges(handle_of(self))
    return flags ~= nil and flags & bit == bit
  end
  function entity_methods:WasEntityAdded() return has_flag(self, 1) end
  function entity_methods:WasEntityRemoved() return has_flag(self, 2) end

  function entity_methods:GetAddedComponentsCurrentFrame()
    local out = {}
    for _, c in ipairs(Ext._Internal.EntityEcbChanges(handle_of(self)) or {}) do
      if math.type(c[2]) == "integer" then
        out[c[1]] = Ext._Internal.PointedObject(c[2], c[3] or c[1])
      end
    end
    return out
  end
  function entity_methods:GetRemovedComponentsCurrentFrame()
    local out = {}
    for _, c in ipairs(Ext._Internal.EntityEcbChanges(handle_of(self)) or {}) do
      if c[2] == false then out[#out + 1] = c[1] end
    end
    return out
  end
end
function entity_methods:GetNetId()
  return Ext._Internal.EntityNetId(handle_of(self))
end

-- entity.Vars: the entity's user variables, as upstream's holder.
local function user_var_option(defs, key, option, default)
  local value = defs[key] and defs[key][option]
  if value == nil then return default end
  return value
end

local function entity_vars(entity)
  local handle = handle_of(entity)
  local guid = Ext.Entity.HandleToUuid(handle) or tostring(handle)
  local store, defs = Ext._Internal.UserVariableStore(guid)
  local server = Ext.IsServer()
  local side = server and "server" or "client"

  return Ext._Internal.NewObjectProxy({
    __index = function(_, key)
      if defs[key] == nil then
        Ext.Log.PrintError("Variable class '" .. tostring(key)
                           .. "' not registered.")
        return nil
      end
      if not user_var_option(defs, key, server and "Server" or "Client",
                             server) then
        Ext.Log.PrintError("Variable class '" .. key .. "' not available on "
                           .. side)
        return nil
      end
      return store[key]
    end,
    __newindex = function(_, key, value)
      if defs[key] == nil then
        Ext.Log.PrintError("Variable class '" .. tostring(key)
                           .. "' not registered.")
        return
      end
      if not user_var_option(defs, key, server and "WriteableOnServer"
                             or "WriteableOnClient", server) then
        Ext.Log.PrintError("Variable class '" .. key .. "' not writeable on "
                           .. side)
        return
      end
      store[key] = value
      Ext.Vars.DirtyUserVariables(guid, key)
    end,
    __len = function()
      local n = 0
      for _ in pairs(store) do n = n + 1 end
      return n
    end,
    __pairs = function() return next, store, nil end,
  })
end

local entity_meta = {
  -- Upstream's Index: a method, a component (nil when absent), Vars, or an
  -- error for anything else.
  __index = function(entity, key)
    local method = entity_methods[key]
    if method ~= nil then return method end
    if key == "Handle" then return handle_of(entity) end
    if key == "Vars" then return entity_vars(entity) end
    if type(key) == "string" and Ext._Internal.ComponentFields(key) == nil then
      error(string.format("Not a valid EntityProxy method or component type: %s",
                          key), 2)
    end
    return get_component(handle_of(entity), key)
  end,

  -- The rest is upstream's EntityProxyMetatable. Two reads of one entity
  -- are two tables here, so without __eq `a.Owner == b` was false where
  -- upstream says true; ordering is by handle, as there.
  __eq = function(a, b)
    return handle_of(a) == handle_of(b)
  end,
  __lt = function(a, b)
    return math.ult(handle_of(a), handle_of(b))
  end,
  __le = function(a, b)
    local x, y = handle_of(a), handle_of(b)
    return x == y or math.ult(x, y)
  end,
  -- "Entity (%016llx)", which is what the captured entity-host reference
  -- prints; bg3le printed "table: 0x...".
  __tostring = function(entity)
    return string.format("Entity (%016x)", handle_of(entity))
  end,
  -- What Ext.Types.GetObjectType reports, as upstream's GetTypeName does.
  __name = "EntityProxy",
  -- And what getmetatable answers with (l_getmetatable): FocusCore's
  -- character test compares against it.
  __bg3leProxy = true,
}

-- An entity-valued field reads as the entity, or nil for the null handle --
-- upstream's push for both EntityHandle and EntityRef. The null is
-- TypedHandle::NullHandle, 0xFFC0000000000000, not all ones; any other value,
-- including zero, is a handle upstream would make a proxy for.
local NULL_ENTITY_HANDLE = 0xFFC0000000000000

local function entity_value(handle)
  if type(handle) ~= "number" or handle == NULL_ENTITY_HANDLE then
    return nil
  end
  return Ext._Internal.EntityOf(handle)
end
Ext._Internal.EntityValue = entity_value

Ext.Entity = {}

-- One object per handle while anything holds it, as upstream's proxies
-- compare equal by value even raw: an entity works as a table key.
local entity_objects = setmetatable({}, {__mode = "v"})

-- The entity for a raw handle, for bg3le's own callers.
function Ext._Internal.EntityOf(handle)
  local entity = entity_objects[handle]
  if entity == nil then
    entity = Ext._Internal.NewEntityProxy(handle, entity_meta)
    entity_objects[handle] = entity
  end
  return entity
end

-- Upstream's: a UUID string, a NetId, or an entity.
function Ext.Entity.Get(id)
  local t = type(id)
  if t == "nil" then return nil end
  local handle
  if t == "string" then
    handle = Ext._Internal.UuidToHandle(id)
  elseif t == "number" then
    handle = Ext._Internal.NetIdEntity(id)
  elseif t == "userdata" then
    handle = Ext._Internal.EntityProxyHandle(id)
  end
  if handle == nil then
    if t == "string" or t == "number" then return nil end
    error("Expected entity GUID, network ID or entity handle, got "
          .. Ext.Types.GetValueType(id), 2)
  end
  if handle == 0xFFC0000000000000 then return nil end
  return Ext._Internal.EntityOf(handle)
end

-- ---- Ext.Mod ----
--
-- Names from reference/ext-api-surface.txt; the shape of a mod from
-- reference/mod-shape.txt. GetLoadOrder returns an array of uuid strings, as
-- reference/mod-loadorder.txt shows.
Ext.Mod = {}

-- One object per Module for the session, as upstream hands out the same
-- engine object each time: Mod Configuration Menu asks for each mod's
-- dependencies hundreds of times as the menu comes up.
local mods_by_addr = {}

-- Upstream pushes a Version by value, so each read of ModVersion is a new
-- table; mods table.remove() the build number from theirs.
local function by_value_versions(t)
  local versions = {ModVersion = t.ModVersion, PublishVersion = t.PublishVersion}
  t.ModVersion, t.PublishVersion = nil, nil
  return setmetatable(t, {
    __index = function(_, key)
      local v = versions[key]
      if type(v) ~= "table" then return v end
      return {table.unpack(v)}
    end,
    __newindex = function(_, key, value)
      if key == "ModVersion" or key == "PublishVersion" then
        versions[key] = value
      else
        rawset(t, key, value)
      end
    end,
    __pairs = function(self)
      local raw = true
      return function(_, key)
        if raw then
          local k, v = next(t, key)
          if k ~= nil then return k, v end
          raw, key = false, nil
        end
        local k = next(versions, key)
        if k ~= nil then return k, self[k] end
      end, self, nil
    end,
  })
end

local function make_mod(addr)
  local known = mods_by_addr[addr]
  if known ~= nil then return known end
  local info = Ext._Internal.ModInfo(addr)
  if info == nil then return nil end
  local function descs(which)
    local list = Ext._Internal.ModList(addr, which)
    for _, desc in ipairs(list) do by_value_versions(desc) end
    return list
  end
  local mod = {
    Info = by_value_versions(info),
    Dependencies = descs(0),
    ModConflicts = descs(1),
    Addons = descs(2),
  }
  mods_by_addr[addr] = mod
  return mod
end

-- The mods the player has enabled, in order.
--
-- The engine's own list is first, and then anything modsettings.lsx
-- enables that the engine did not load -- which on this build is 14 of 57
-- modules, because a savegame's module list replaces the file's and the
-- engine mounts only what it needs where you are standing. See
-- reference/MOD-LOADING.md.
--
-- Upstream's load order holds everything the player enabled, so returning
-- the engine's partial list was not parity, and mods read this as "every
-- mod that is on". Mod Configuration Menu loads a blueprint for each mod
-- in it: the 14 it never saw had no settings registered, and every later
-- lookup for one of them warned that the mod "was not found by MCM" and
-- asked the player to contact its author.
-- The engine's list is missing some loaded mods (see IsModLoaded below), so
-- the player's modsettings.lsx supplies them -- and its order, which is the
-- one the engine follows for the mods it does list. Appending the missing
-- ones after the engine's put Mod Configuration Menu behind every mod that
-- depends on it, so its own load-order check complained, and bootstraps ran
-- in that order. Modules modsettings does not name (the base game's) keep
-- the engine's order, ahead of the player's mods.
-- Recomputed only when the engine's list or the file changes: IsModLoaded
-- asks for it on every call.
local load_order_key, load_order = nil, nil

-- The engine's list only grows while mods load, so its length and ends
-- are enough to notice a change.
local function current_load_order()
  local settings = Ext._Internal.ModSettingsOrder() or {}
  local n = Ext._Internal.ModCount()
  -- ModUuidAt returns nothing (not nil) past the end, as with an empty list.
  local key = table.concat({n, tostring((Ext._Internal.ModUuidAt(0))),
                            tostring((Ext._Internal.ModUuidAt(n - 1))),
                            table.concat(settings, ",")}, ";")
  if key ~= load_order_key then
    local engine = {}
    for i = 0, n - 1 do
      local uuid = Ext._Internal.ModUuidAt(i)
      if uuid ~= nil then engine[#engine + 1] = uuid end
    end
    load_order_key = key
    load_order = Ext._Internal.BuildLoadOrder(engine, settings)
  end
  return load_order
end

function Ext.Mod.GetLoadOrder()
  local order = current_load_order()
  return table.move(order, 1, #order, 1, {})
end

function Ext._Internal.BuildLoadOrder(engine, settings)
  local out = {}
  local seen = {}
  local listed = {}
  for _, uuid in ipairs(settings) do listed[uuid] = true end

  for _, uuid in ipairs(engine) do
    if not listed[uuid] and not seen[uuid] then
      seen[uuid] = true
      out[#out + 1] = uuid
    end
  end
  for _, uuid in ipairs(settings) do
    if not seen[uuid] then
      seen[uuid] = true
      out[#out + 1] = uuid
    end
  end
  return out
end

-- Whether a mod is loaded, over the same order Ext.Mod.GetLoadOrder reports.
--
-- Asking the engine's own load order is not enough: it holds 43 of the 70
-- enabled modules on this build, and Mod Configuration Menu is one of the
-- ones missing from it even though it is loaded, configured and answering
-- calls -- it mounts on content rather than through the load order, which
-- reference/MOD-LOADING.md establishes. 5eSpells reads every one of its own
-- settings behind `IsModLoaded(MCM)`, so a false there silently turned off
-- every feature the mod has: no spell lists edited, no stats changed, and not
-- one line of output to say so.
--
-- Recomputed when the order changes rather than cached outright, since a mod
-- may ask before the order is complete.
local mod_loaded = {}
local mod_loaded_for = -1

function Ext.Mod.IsModLoaded(uuid)
  if type(uuid) ~= "string" then return false end

  local order = current_load_order()
  if #order ~= mod_loaded_for then
    mod_loaded = {}
    mod_loaded_for = #order
    for _, id in ipairs(order) do mod_loaded[id] = true end
  end

  return mod_loaded[uuid] == true
end

-- Every mod the engine has a Module for, by uuid, whether or not it made
-- the load order.
--
-- ModFind only searches the load order, and on this build that holds 43
-- of the 57 installed modules -- so a mod asking about a neighbour got
-- nil, and MCM said "Mod 755a8a72-... was not found by MCM" for each one.
-- The available list is the engine's own and covers them.
--
-- Rebuilt whenever the engine's count changes: the list can still be
-- filling when the first client mod loads, and a table cached then would
-- keep later modules out for the whole session.
local available = {count = nil, by_uuid = {}}

local function available_mod(uuid)
  local n = Ext._Internal.ModAvailableCount() or 0
  if n ~= available.count then
    available.count = n
    available.by_uuid = {}
    for i = 0, n - 1 do
      local addr = Ext._Internal.ModAvailableAt(i)
      if addr ~= nil then
        local info = Ext._Internal.ModInfo(addr)
        local id = info and info.ModuleUUIDString
        if id ~= nil and id ~= "" and available.by_uuid[id] == nil then
          available.by_uuid[id] = addr
        end
      end
    end
  end

  local addr = available.by_uuid[uuid]
  return addr ~= nil and make_mod(addr) or nil
end

local installed_mods = nil

-- What the archives say about a mod the engine has not loaded, keyed by
-- UUID. Built once, from each mod's own meta.lsx -- the same file the
-- engine reads -- so a mod that is installed and enabled can still be
-- described.
local function installed_mod(uuid)
  -- Larian packs a version into one 64-bit number, and bg3se's Version
  -- spells out where each part sits: major above bit 55, minor in the eight
  -- bits below that, revision in sixteen more, build in the low
  -- thirty-one.
  --
  -- Worth doing rather than reporting zeros. Mod Configuration Menu prints
  -- its own version from this, and with {0,0,0,0} it announced itself as
  -- "version 0.0.0" on both sides.
  local function decode_version(packed)
    local v = math.tointeger(tonumber(packed or 0) or 0)
    if v == nil or v == 0 then return {0, 0, 0, 0} end
    return {(v >> 55) & 0x1ff, (v >> 47) & 0xff, (v >> 31) & 0xffff,
            v & 0x7fffffff}
  end

  if installed_mods == nil then
    installed_mods = {}
    for _, module in ipairs(Ext._Internal.PakModules()) do
      if module.Uuid ~= nil and module.Uuid ~= "" then
        installed_mods[module.Uuid] = {
          Info = by_value_versions({
            ModuleUUIDString = module.Uuid,
            Name = module.ModName,
            Directory = module.Name,
            Author = module.Author,
            Description = module.Description,
            Hash = "",
            StartLevelName = "",
            MenuLevelName = "",
            LobbyLevelName = "",
            CharacterCreationLevelName = "",
            PhotoBoothLevelName = "",
            ModVersion = decode_version(module.Version),
            PublishVersion = decode_version(module.Version),
            NumPlayers = 0,
            FileSize = 0,
            PublishHandle = 0,
          }),
          Dependencies = {},
          ModConflicts = {},
          Addons = {},
        }
      end
    end
  end
  return installed_mods[uuid]
end

function Ext.Mod.GetMod(uuid)
  if type(uuid) ~= "string" then return nil end
  local addr = Ext._Internal.ModFind(uuid)
  if addr ~= nil then return make_mod(addr) end

  -- Not in the load order, which on this build holds 43 of the 57
  -- installed modules -- see reference/MOD-LOADING.md. The engine still
  -- has a Module for the rest, so ask it before falling back to reading
  -- the archives, because what it has is the real thing.
  local known = available_mod(uuid)
  if known ~= nil then return known end

  -- And last, what the archives say. Returning nil here breaks any mod
  -- that looks its neighbours up, Mod Configuration Menu included.
  known = installed_mod(uuid)
  -- Said, so the next failure names the module and when it was asked; but
  -- only for a UUID. Mods also probe with names (Armory's item report does,
  -- for every mod), and upstream answers those with a quiet nil.
  if known == nil and type(uuid) == "string"
     and uuid:match("^%x+%-%x+%-%x+%-%x+%-%x+$") then
    print(("bg3le: GetMod found no module for %s (%d loaded, %d available)")
          :format(uuid, Ext._Internal.ModCount() or -1,
                  Ext._Internal.ModAvailableCount() or -1))
  end
  return known
end

-- ModManager::BaseModule, which is the campaign module rather than the first
-- mod in load order -- upstream returns that member, so this does too.
function Ext.Mod.GetBaseMod()
  local addr = Ext._Internal.ModBase()
  if addr == nil then return nil end
  return make_mod(addr)
end

-- Upstream returns the engine's ModManager. Settings is left out: it sits
-- past a hash map whose size on this build is not established, and an empty
-- table there would be a wrong answer rather than a missing one.
function Ext.Mod.GetModManager()
  local function collect(count, at)
    local out = {}
    for i = 0, count() - 1 do
      local addr = at(i)
      if addr ~= nil then out[#out + 1] = make_mod(addr) end
    end
    return out
  end

  return {
    BaseModule = Ext.Mod.GetBaseMod(),
    LoadOrderedModules = collect(Ext._Internal.ModCount,
                                 Ext._Internal.ModAt),
    AvailableMods = collect(Ext._Internal.ModAvailableCount,
                            Ext._Internal.ModAvailableAt),
    -- ModManager::Settings, which is one Array<ModuleShortDesc>. It sits at
    -- a derived offset past AvailableMods, a hash map and a spare word --
    -- see kSettingsModsAfterLoadOrder -- and it is the mod list the session
    -- has rather than the load order: for a loaded save, what the save
    -- recorded, without the base modules.
    Settings = { Mods = Ext._Internal.ModSettings() },
  }
end

-- ---- Ext.Stats ----
--
-- A stat's attributes are not fields at fixed offsets; each one is a name, a
-- type and a raw integer, and the type decides how to read the integer. That
-- decoding lives here rather than in C so the shape of a stat is described in
-- one readable place.
--
-- RPGEnumerationType, in the order bg3se declares it. The C side returns
-- these numbers.
local STAT_KIND = {
  [0] = "Int", [1] = "Int64", [2] = "Float", [3] = "FixedString",
  [4] = "Enumeration", [5] = "Flags", [6] = "GUID", [7] = "StatsFunctors",
  [8] = "Conditions", [9] = "RollConditions", [10] = "Requirements",
  [11] = "MemorizationRequirements", [12] = "TranslatedString",
  [13] = "Unknown",
}

Ext.Stats = {}

-- One attribute, decoded as far as its type allows.
-- A functor, with StatsExpressionRef filled in: a pointer to a pooled
-- expression whose Code, Params and RefCount upstream reports as a table.
local read_functor
do
  -- Per class, the fields that can read as "<unsupported>" (variants and
  -- unconvertible kinds). Only those are probed for an expression; walking
  -- every field with pairs read the whole functor, 118 us a SpellSuccess.
  local candidates = {}

  read_functor = function(address, class)
    local out = Ext._Internal.ReadObject(address, class, "", {})
    local fields = candidates[class]
    if fields == nil then
      fields = {}
      for field, kind in pairs(Ext._Internal.ObjectFields(class, "") or {}) do
        if kind == "variant" or kind == "unsupported" then fields[#fields + 1] = field end
      end
      candidates[class] = fields
    end
    for _, field in ipairs(fields) do
      if out[field] == "<unsupported>" then
        local pooled, code, refCount =
          Ext._Internal.ObjectExpression(address, class, field)
        if pooled ~= nil then
          local expression = Ext._Internal.ReadObject(
            pooled, "StatsExpressionPooled", "", {})
          Ext._Internal.AmendObject(expression, "Code", code)
          Ext._Internal.AmendObject(expression, "RefCount", refCount)
          Ext._Internal.AmendObject(out, field, expression)
        end
      end
    end
    return out
  end
end

local function read_attribute(addr, i)
  local name, typeName, kind, raw = Ext._Internal.StatsAttrAt(addr, i)
  if name == nil then return nil end

  local value
  -- AIFlags before anything else. It classifies as an Enumeration, since it
  -- carries labels and is not one of upstream's flag types, but
  -- Object::GetString special-cases it: the value is a FixedString on the
  -- object, not an index into a pool. Decoding it as an enumeration
  -- reported "CanNotUse" on a spell whose AIFlags is empty.
  if typeName == "AIFlags" then
    return name, Ext._Internal.StatsAIFlags(addr) or "",
           STAT_KIND[kind] or "Unknown", typeName, raw, kind
  end

  if kind == 0 or kind == 1 then
    value = raw
  elseif kind == 2 then
    -- nil, not a stand-in: the pools treat slot zero as unset, and upstream
    -- reports nothing for an attribute the stat does not carry. Reporting
    -- the pool index instead put {PoolIndex = 0} where the reference has
    -- null.
    value = Ext._Internal.StatsAttrFloat(raw)
  elseif kind == 6 then
    value = Ext._Internal.StatsAttrGuid(raw)
  elseif kind == 3 then
    -- Index 0 is the unset slot and does not resolve; an absent string is
    -- empty, not the number nought.
    value = Ext._Internal.StatsAttrString(raw) or ""
  elseif kind == 4 or kind == 5 then
    -- An enumeration matches one label exactly; a flag set indexes the
    -- int64 pool for a bitmask and may name several. Guessing at the latter
    -- produced a longsword proficient in clubs and light armour, so both
    -- paths now follow upstream's Object::GetFlags.
    if kind == 5 then
      -- A flag set is an array upstream, empty when nothing is set, so it is
      -- an array here. The public shape has to match or a mod that iterates
      -- it breaks.
      local joined = Ext._Internal.StatsAttrFlags(addr, i, raw)
      local list = {}
      if joined ~= nil and joined ~= "" then
        for part in joined:gmatch("[^;]+") do list[#list + 1] = part end
      end
      value = list
    else
      -- Upstream's GetString gives "" for a value no label matches.
      value = Ext._Internal.StatsAttrLabel(addr, i, raw) or ""
    end
  elseif kind == 10 then
    -- Object::Requirements, as upstream's serializer presents it.
    value = Ext._Internal.StatsRequirements(addr) or {}
  elseif kind == 7 then
    -- Each group is a text key and a list of functors. A functor is a
    -- polymorphic engine object, and its concrete class is in the same
    -- property maps the resource reader uses, so the fields come out the
    -- same way rather than being special-cased here.
    local groups = Ext._Internal.StatsFunctorGroups(addr, name)
    if groups == nil then
      value = nil
    else
      local out = {}
      for gi, group in ipairs(groups) do
        local functors = {}
        for fi, f in ipairs(group.Functors) do
          functors[fi] = read_functor(f.Address, f.Class)
        end
        out[gi] = {TextKey = group.TextKey, Functors = functors}
      end
      value = out
    end
  elseif kind == 8 then
    -- Conditions, TargetConditions and UseConditions index the condition
    -- pool. Upstream's Object::GetString falls back to "" when the lookup
    -- misses, so this does too.
    value = Ext._Internal.StatsAttrCondition(raw) or ""
  elseif kind == 9 then
    -- A table keyed by each roll condition's text key, holding its
    -- expression; nil when the attribute carries none.
    value = Ext._Internal.StatsRollConditions(addr, name)
  elseif kind == 11 then
    -- MemorizationRequirements is deprecated and upstream pushes nil for it
    -- unconditionally, whatever the stat holds.
    value = nil
  elseif kind == 12 then
    value = Ext._Internal.StatsAttrTranslated(raw)
  else
    value = raw
  end

  return name, value, STAT_KIND[kind] or "Unknown", typeName, raw, kind
end

-- A stat object, shaped the way upstream shapes one.
--
-- Upstream returns a userdata proxy carrying bound methods and mod
-- provenance, not a plain table, and a mod written against it may call
-- stat:Sync() or read stat.ModId. Returning a table would make those fail
-- with "attempt to call a nil value", so this is a proxy with a metatable:
-- the attributes read through __index, the methods exist, and __pairs
-- enumerates both so a dump looks like upstream's.
-- Writing an attribute.
--
-- An attribute is one int32 in the stat object's indexed properties, and
-- what that int32 means depends on the attribute's kind: an Int or an
-- Enumeration is the value, and the rest index one of RPGStats' pools. So
-- a write is a store, plus getting the value into a pool first when the
-- kind needs one.
--
-- The kinds that carry compiled data -- functors, roll conditions,
-- requirements -- are not written: the engine holds them parsed, and
-- storing an index to text it never compiles would look like it worked and
-- do nothing.
-- BG3LE_STAT_WRITES=0 turns attribute writing off, so "is bg3le's stat
-- write causing this?" can be answered from outside the process. It is
-- how the FixedString path below was shown to be the thing that hangs the
-- engine.
-- Integers and enumerations store a value in the stat object's own slot.
-- Conditions and strings have to put the value into one of RPGStats'
-- pools first, which appends into the array's spare capacity and raises
-- its size.
--
-- Those two spent a while switched off here, on the reading that a run
-- dying mid-load was one of those appends racing the engine. It was not:
-- the crash was bg3le's own nse_lua_report_handled_error, which the
-- interpreter calls only when an error handler is installed, and which
-- an xpcall added that afternoon turned from dead code into a segfault on
-- the first mod error. Three A/B runs were attributed to the wrong thing
-- before a core dump named it.
local STAT_WRITABLE_KINDS = {
  [0] = "int", [1] = "int", [2] = "float", [3] = "string", [4] = "enum",
  [5] = "flags", [6] = "guid", [8] = "condition", [9] = "roll",
  [10] = "requirements", [12] = "translated",
}
if Ext._Internal.Env("BG3LE_STAT_WRITES") == "0" then
  STAT_WRITABLE_KINDS = {}
end

local STAT_KIND_UNWRITABLE = {
  [11] = "deprecated upstream and reported as nil",
}

local sync_warning_shown = false

local function stat_write(self, key, value, raw)
  if not Ext._Internal.StatsModuleLoad and not sync_warning_shown then
    sync_warning_shown = true
    Ext.Log.PrintWarning("Stats edited after ModuleLoad must be synced "
      .. "manually; make sure that you call Sync() on it when you're finished!")
  end

  local addr = rawget(self, "__addr")
  if addr == nil then
    error("bg3le: this stat was not read from the engine, so there is "
          .. "nothing to write to", 3)
  end

  -- The engine loader appends a ComboCategory line to the set; "" adds nothing.
  if raw and key == "ComboCategory" then
    local set = Ext._Internal.StatsComboGet(addr, 1)
    for name in tostring(value or ""):gmatch("[^;]+") do
      local trimmed = name:match("^%s*(.-)%s*$")
      if trimmed ~= "" then set[#set + 1] = trimmed end
    end
    return Ext._Internal.StatsComboSet(addr, 1, set)
  end

  if key == "ComboProperties" or key == "ComboCategories" then
    if type(value) ~= "table" then
      error(string.format("%s is a set; assign a table of names", key), 3)
    end
    return Ext._Internal.StatsComboSet(addr, key == "ComboProperties" and 0 or 1, value)
  end

  local index, kind, typeName = Ext._Internal.StatsAttrFind(addr, key)
  if index == nil then
    error(string.format(
      "bg3le: %s is not an attribute of this stat, so there is nothing to "
      .. "write", tostring(key)), 3)
  end
  local slot = {index = index, kind = kind, typeName = typeName}

  -- Upstream's SetString assigns Object::AIFlags itself.
  if slot.typeName == "AIFlags" then
    if not Ext._Internal.StatsAIFlagsSet(addr, tostring(value or "")) then
      error(string.format("bg3le could not write %s", key), 3)
    end
    rawget(self, "__cache")[key] = nil
    return true
  end

  -- Functor lists: SetRawAttribute parses them as the engine's loader does;
  -- assigning one fails as upstream's TrySetValue does.
  if slot.kind == 7 then
    if raw then
      local ok, why = Ext._Internal.StatsSetFunctors(addr, key, tostring(value or ""))
      if not ok then error("bg3le: " .. tostring(why), 3) end
      rawget(self, "__cache")[key] = nil
      return true
    end
    local name = rawget(self, "__name") or "?"
    if type(value) == "table" then
      error(string.format("Cannot use table value for stat property %s of type StatsFunctors!", key), 3)
    elseif value == nil then
      Ext.Log.PrintError("Temporarily disabled until functors are mapped")
    else
      Ext.Log.PrintError(string.format("Couldn't set %s.%s to string value: Inappropriate type: StatsFunctors", name, key))
    end
    return false
  end

  local writable = STAT_WRITABLE_KINDS[slot.kind]
  if writable == nil then
    error(string.format("bg3le cannot write %s: it is %s", key,
                        STAT_KIND_UNWRITABLE[slot.kind]
                        or "of a kind bg3le does not write"), 3)
  end

  -- SetRawAttribute takes stats-file text: numbers as strings, flags joined
  -- by ';', and "" resetting the attribute, as the engine's loader does.
  if raw and value == "" and (writable == "int" or writable == "enum") then
    value = 0
  elseif raw and value == "" and (writable == "float" or writable == "guid") then
    value = nil
  elseif raw and type(value) == "string" then
    if writable == "int" or writable == "float" then
      value = tonumber(value) or value
    elseif writable == "roll" then
      value = Ext._Internal.StatsSplitGroups(value) or value
    elseif writable == "requirements" then
      -- "!Immobile;Level 3": each is [!]Name[ Param].
      local list = {}
      for part in value:gmatch("[^;]+") do
        local negate, name, param = part:match("^%s*(!?)%s*([%w_]+)%s*(%S*)%s*$")
        if name == nil then
          error(string.format("Couldn't set %s.%s: \"%s\" is not a requirement",
                              rawget(self, "__name"), key, part), 3)
        end
        list[#list + 1] = {Requirement = name, Not = negate == "!",
                           Param = param ~= "" and (tonumber(param) or param) or -1}
      end
      value = list
    elseif writable == "flags" then
      local labels = {}
      for label in value:gmatch("[^;]+") do
        local trimmed = label:match("^%s*(.-)%s*$")
        if trimmed ~= "" then labels[#labels + 1] = trimmed end
      end
      value = labels
    end
  end

  local raw
  if writable == "int" then
    if type(value) ~= "number" then
      error(string.format("%s is an integer attribute", key), 3)
    end
    raw = math.floor(value)
  elseif writable == "enum" then
    if type(value) == "number" then
      raw = math.floor(value)
    else
      raw = Ext._Internal.StatsEnumIndex(slot.typeName, tostring(value))
      if raw == nil then
        error(string.format("%q is not a value of enumeration %s", tostring(value),
                            tostring(slot.typeName)), 3)
      end
    end
  elseif writable == "float" then
    -- Upstream's SetFloat: a value goes into the float pool; nil clears it.
    if value == nil then
      raw = -1
    elseif type(value) ~= "number" then
      error(string.format("%s is a float attribute", key), 3)
    else
      raw = Ext._Internal.StatsFloatIntern(value)
      if raw == nil then
        error(string.format("bg3le could not add %s's value to the engine's "
                            .. "float pool; see the stats lines in the "
                            .. "extender log", key), 3)
      end
    end
  elseif writable == "guid" and value == nil then
    raw = -1
  elseif writable == "guid" then
    local why
    raw, why = Ext._Internal.StatsGuidIntern(tostring(value))
    if raw == nil and why == "not a GUID" then
      error(string.format("Couldn't set %s.%s: Value (\"%s\") is not a "
                          .. "valid GUID", rawget(self, "__name"), key,
                          tostring(value)), 3)
    elseif raw == nil then
      error(string.format("bg3le could not write %s: %s", key, why), 3)
    end
  elseif writable == "translated" then
    -- "handle" or "handle;version", as TranslatedString::FromString; nil
    -- clears it.
    if value == nil then
      raw = -1
    else
      raw = Ext._Internal.StatsTranslatedIntern(tostring(value))
      if raw == nil then
        error(string.format("bg3le could not add %s's handle to the engine's "
                            .. "translated string pool", key), 3)
      end
    end
  elseif writable == "requirements" then
    local ok, why = Ext._Internal.StatsRequirementsSet(addr, value or {})
    if not ok then
      error(string.format("bg3le could not write %s: %s", key, why), 3)
    end
    rawget(self, "__cache")[key] = nil
    return true
  elseif writable == "roll" then
    -- Not an indexed property: the stat's own RollConditions map.
    if type(value) ~= "table" then value = tostring(value or "") end
    local ok, why = Ext._Internal.StatsRollSet(addr, key, value)
    if not ok then
      error(string.format("bg3le could not write %s: %s", key, why), 3)
    end
    rawget(self, "__cache")[key] = nil
    return true
  elseif writable == "flags" then
    -- Upstream's Object::SetFlags: each label's index sets bit index - 1.
    -- A single label is upstream's SetString on a flag type.
    if type(value) == "string" then value = {value} end
    if type(value) ~= "table" then
      error(string.format("%s is a flag set; assign a table of labels", key), 3)
    end
    local mask = 0
    for _, label in ipairs(value) do
      local index = Ext._Internal.StatsEnumIndex(slot.typeName, tostring(label))
      if index == nil then
        error(string.format("Couldn't set %s.%s: Value (\"%s\") is not a "
                            .. "valid enum label", rawget(self, "__name"), key,
                            tostring(label)), 3)
      end
      if index > 0 then mask = mask | (1 << (index - 1)) end
    end
    raw = Ext._Internal.StatsInt64Intern(mask)
    if raw == nil then
      error(string.format("bg3le could not add %s's flag set to the engine's "
                          .. "int64 pool; see the stats lines in the extender "
                          .. "log", key), 3)
    end
  elseif writable == "string" then
    if type(value) ~= "string" then
      error(string.format("%s is a string attribute", key), 3)
    end
    raw = Ext._Internal.StatsStringIntern(value)
    if raw == nil then
      local shown = #value > 80 and (value:sub(1, 80) .. "...") or value
      error(string.format(
        "bg3le could not give %s's %d-character value %q a string-table entry "
        .. "and a pool slot; see the strings and stats lines in the extender log",
        key, #value, shown), 3)
    end
  else  -- condition
    if type(value) ~= "string" then
      error(string.format("%s is a condition expression", key), 3)
    end
    raw = Ext._Internal.StatsConditionIntern(value)
    if raw == nil then
      error(string.format(
        "bg3le could not add the condition %q to the engine's condition "
        .. "pool; see the stats lines in the extender log", value), 3)
    end
  end

  if not Ext._Internal.StatsAttrSet(addr, slot.index, raw) then
    error(string.format("bg3le could not write %s on this stat", key), 3)
  end

  -- Read back from the engine rather than cached as written, so it comes
  -- out as upstream's reads it: a float rounded to 32 bits, a GUID in its
  -- canonical form, a flag set in the enumeration's order, a translated
  -- string without its version.
  rawget(self, "__cache")[key] = nil
  return true
end

-- Upstream's WARN_ONCE.
local warned_once = {}
function Ext._Internal.WarnOnce(message)
  if warned_once[message] then return end
  warned_once[message] = true
  Ext.Log.PrintWarning(message)
end

-- Upstream's SyncWithPrototypeManager. The write already reached the stat,
-- so a prototype that cannot be rebuilt is reported once rather than raised.
function Ext._Internal.SyncStat(name, persist)
  local ok, err = Ext._Internal.StatSync(name)
  if not ok then
    Ext._Internal.WarnOnce("bg3le: Sync(" .. tostring(name) .. "): " .. tostring(err))
  end
  if persist ~= nil then
    Ext._Internal.WarnOnce("The 'persist' argument to Ext.Stats.Sync() is deprecated")
  end
end

local STAT_METHODS = {
  SetPersistence = function()
    Ext._Internal.WarnOnce("Ext.Stats.SetPersistence() is deprecated")
  end,

  -- Upstream's ObjectHelpers::CopyFrom and Object::CopyFrom: nothing to do
  -- from itself; otherwise refused across modifier lists, then AIFlags, every
  -- IndexedProperties entry, the Functors and RollConditions maps (sharing the
  -- compiled sets, as upstream does), Requirements and the combo sets.
  CopyFrom = function(self, from)
    if type(from) ~= "string" then
      error("stat:CopyFrom(name) takes a stat name", 2)
    end
    if from == rawget(self, "__name") then return true end

    local carried, total = Ext._Internal.StatsCopyFrom(
      rawget(self, "__addr"), from)
    if carried == nil then
      local source = Ext.Stats.Get(from)
      if source == nil then
        Ext.Log.PrintError("Cannot copy stats from nonexistent object: " .. from)
      elseif source.ModifierList ~= self.ModifierList then
        Ext.Log.PrintError(string.format("Cannot copy stats from object '%s' (a %s) to an object of type %s",
          from, tostring(source.ModifierList), tostring(self.ModifierList)))
      else
        Ext.Log.PrintError("bg3le: Cannot copy stats from " .. from .. ": " .. tostring(total))
      end
      return false
    end

    -- The proxy's cache holds what it read before the copy.
    for k in pairs(rawget(self, "__cache")) do
      rawget(self, "__cache")[k] = nil
    end
    return true
  end,

  -- Upstream's never throws: a stats file loads past a bad line.
  SetRawAttribute = function(self, name, value)
    local ok, result = pcall(stat_write, self, name, value, true)
    if not ok then
      Ext.Log.PrintError((tostring(result):gsub("^bg3le prelude:%d+: ", "")))
      return false
    end
    return result
  end,

  -- Rebuilds the spell, status or interrupt prototype from the stat.
  Sync = function(self, persist)
    Ext._Internal.SyncStat(self.Name, persist)
  end,
}

-- An attribute the engine has no value for. Upstream reports the key with a
-- nil value -- its FallbackNext walks the modifier list and pushes nil when
-- the get fails -- and a Lua table cannot hold nil, so the key is held with
-- this marker and read back as nil.
local STAT_NIL = setmetatable({}, {__tostring = function() return "nil" end})

-- The fields upstream puts alongside the attributes, computed when asked
-- for. Names and shapes follow reference/stats-weapon.txt.
local STAT_EXTRAS = {
  Name = function(self) return rawget(self, "__name") end,
  -- An empty string when no mod is known, as upstream's empty FixedString.
  ModId = function(self)
    local modId = Ext._Internal.StatOrigin(rawget(self, "__name"))
    return modId or ""
  end,
  OriginalModId = function(self)
    local _, original = Ext._Internal.StatOrigin(rawget(self, "__name"))
    return original or ""
  end,
  ModifierList = function(self)
    return Ext._Internal.StatsType(rawget(self, "__addr"))
  end,
  ModifierListIndex = function(self)
    return Ext._Internal.StatsListIndex(rawget(self, "__addr"))
  end,
  Using = function(self)
    return Ext._Internal.StatsUsing(rawget(self, "__addr")) or ""
  end,
  ComboProperties = function(self)
    return Ext._Internal.StatsComboGet(rawget(self, "__addr"), 0)
  end,
  ComboCategories = function(self)
    return Ext._Internal.StatsComboGet(rawget(self, "__addr"), 1)
  end,

  -- An empty attribute set means the discovery did not land, which is
  -- worth saying rather than handing back a name that looks complete.
  --
  -- bg3le's own, not upstream's, so it is listed in STAT_DIAGNOSTICS below
  -- and does not appear at all unless it has something to report. It used
  -- to come out of every dump as "AttributesUnavailable": null, which is a
  -- key a mod iterating a stat would see and upstream does not have --
  -- caught by tools/check-reference.sh against the real extender's capture.
  AttributesUnavailable = function(self)
    if Ext._Internal.StatsAttrCount(rawget(self, "__addr")) > 0 then
      return nil
    end
    return "no attributes readable; see the stats lines in the extender log"
  end,
}

-- The extras that are bg3le's rather than upstream's. A key here is omitted
-- when its value is nil, where one of upstream's own -- ModId, ModifierList
-- and the rest -- is reported either way, because upstream reports those.
local STAT_DIAGNOSTICS = {AttributesUnavailable = true}

local stat_proxy = {}

-- Read when asked for, not when the stat is fetched.
--
-- This used to snapshot every attribute into a table: a spell has a couple
-- of hundred, so Ext.Stats.Get built two hundred entries and decoded two
-- hundred values whether or not the caller wanted one of them. A mod that
-- walks the stats keeps what it fetches, so the heap grew, and the garbage
-- collector's share of each fetch grew with it -- 8ms per stat at two
-- thousand, 40ms at six, 120ms at ten. Quadratic, and it read as a hang.
--
-- Upstream's stat object reads on access too, so this is closer to it as
-- well as faster. What a caller reads twice is cached, and __pairs still
-- reads everything, because iterating asks for everything.
stat_proxy.__index = function(self, key)
  local m = STAT_METHODS[key]
  if m ~= nil then return m end

  local cache = rawget(self, "__cache")
  local hit = cache[key]
  if hit ~= nil then
    if hit == STAT_NIL then return nil end
    return hit
  end

  local extra = STAT_EXTRAS[key]
  if extra ~= nil then
    local value = extra(self)
    cache[key] = value == nil and STAT_NIL or value
    return value
  end

  local addr = rawget(self, "__addr")
  local index = Ext._Internal.StatsAttrFind(addr, key)
  if index == nil then return nil end

  local _, value = read_attribute(addr, index)
  cache[key] = value == nil and STAT_NIL or value
  return value
end

stat_proxy.__newindex = function(self, key, value)
  stat_write(self, key, value)
end

-- Enumerates methods alongside fields, which is what makes a dump match:
-- upstream prints Sync, SetPersistence, SetRawAttribute and CopyFrom as
-- function entries next to the attributes.
-- Iterating asks for everything, so this is where the whole read happens.
-- Methods appear alongside the attributes, which is what makes a dump match
-- upstream's: it prints Sync, SetPersistence, SetRawAttribute and CopyFrom
-- as function entries next to them.
stat_proxy.__pairs = function(self)
  local addr = rawget(self, "__addr")
  local cache = rawget(self, "__cache")

  -- Upstream's order: the object's own members and methods by name, then
  -- the attributes in the modifier list's order.
  local keys, listed = {}, {}
  for key in pairs(STAT_EXTRAS) do
    if cache[key] == nil then
      local value = STAT_EXTRAS[key](self)
      cache[key] = value == nil and STAT_NIL or value
    end
    if not (STAT_DIAGNOSTICS[key] and cache[key] == STAT_NIL) then
      keys[#keys + 1] = key
    end
    listed[key] = true
  end
  for k in pairs(STAT_METHODS) do
    keys[#keys + 1] = k
    listed[k] = true
  end
  table.sort(keys)

  local n = Ext._Internal.StatsAttrCount(addr)
  for i = 0, n - 1 do
    local attr, value = read_attribute(addr, i)
    if attr ~= nil and not listed[attr] then
      if cache[attr] == nil then
        cache[attr] = value == nil and STAT_NIL or value
      end
      keys[#keys + 1] = attr
      listed[attr] = true
    end
  end

  local i = 0
  return function()
    i = i + 1
    local k = keys[i]
    if k == nil then return nil end
    local v = STAT_METHODS[k] or cache[k]
    if v == STAT_NIL then v = nil end
    -- The key is what ends the loop, not the value, so an attribute with no
    -- value still appears.
    return k, v
  end
end

stat_proxy.__name = "stats::Object"
stat_proxy.__tostring = function(self)
  return string.format("stats::Object (%016X)", rawget(self, "__addr") or 0)
end
-- Its __pairs order is upstream's, so a dump keeps it.
stat_proxy.__bg3leOrdered = true

stat_proxy.__bg3leIdentity = function(self)
  return "s:" .. tostring(rawget(self, "__addr"))
end

local function make_stat(name, addr)
  return setmetatable({__name = name, __addr = addr, __cache = {}},
                      stat_proxy)
end

-- Ext.Stats.Get(name) -> stat object, or nil plus a reason
function Ext.Stats.Get(name)
  if type(name) ~= "string" then
    return nil, "Ext.Stats.Get takes a stat name"
  end

  local addr, err = Ext._Internal.StatsFind(name)
  if addr == nil then return nil, err end
  return make_stat(name, addr)
end

-- Ext.Stats.GetTypes(name) -> { attribute = type, ... }
--
-- Separate from Get because the values and their types are wanted for
-- different reasons, and putting both in one table would collide with the
-- attribute names.
function Ext.Stats.GetTypes(name)
  local addr, err = Ext._Internal.StatsFind(name)
  if addr == nil then return nil, err end
  local out = {}
  local n = Ext._Internal.StatsAttrCount(addr)
  for i = 0, n - 1 do
    local attr, _, kind, typeName = read_attribute(addr, i)
    if attr ~= nil then
      out[attr] = {Kind = kind, ValueList = typeName}
    end
  end
  return out
end

-- Ext.Stats.GetStats([modifierList]) -> { name, ... }
--
-- Upstream's name and signature. An earlier version called this GetAllStats
-- and added a GetStatsCount that upstream does not have; the public surface
-- has to match or a mod written against bg3se will not run here. Internal
-- entry points stay ours to shape -- it is Ext.* that is the contract.
--
-- The filter works now. It used to be refused because the obvious
-- implementation is quadratic -- a lookup by name per stat, each a linear
-- scan -- and both halves of that are indexed today: names by modifier
-- list here, and stats by name behind Ext.Stats.Get.
function Ext.Stats.GetStats(modifierList)
  return Ext._Internal.StatsNames(modifierList)
end

-- Not part of upstream's surface, so it lives under _Internal where our own
-- additions belong; #Ext.Stats.GetStats() is the public way to count.
function Ext._Internal.StatsTotal() return Ext._Internal.StatsCount() end

-- ---- Ext.StaticData ----
--
-- Static data is read as a snapshot rather than as a live view, which is the
-- one place this differs from a component. A component can change under you,
-- so its fields are read on access; a resource definition is loaded once and
-- does not, so a plain table is simpler and more useful -- it can be held,
-- compared and serialised without reading the game again.
--
-- Fields go through the same metadata and the same resolver a component's do;
-- only the base address comes from elsewhere. A kind bg3le cannot convert
-- arrives as a marker rather than being dropped, for the reason it does
-- everywhere else.
local read_object

-- One value at a path inside a reflected object.
--
-- Split out of read_object so that array elements go through the same
-- resolution as fields do. That is what makes a variant readable: an
-- element of StatsExpressionPooled.Params is a variant, and only its live
-- alternative has bytes, so which one that is has to be asked for at
-- runtime rather than derived.
-- A snapshot array or map, as a userdata over the values read, as
-- upstream's are userdata. source is where a set came from, for
-- Ext.Types.Unserialize to write it back.
-- A map's length is its entry count, as upstream's map proxy reports it;
-- its keys are not a sequence, so # on the table would say nought.
local function entry_count(items)
  local n = 0
  for _ in next, items do n = n + 1 end
  return n
end

-- write(k, v) takes an element write to the engine; assign(values), for
-- Ext.Types.Unserialize, the whole container.
local function snapshot_container(items, container, write, assign)
  return Ext._Internal.NewObjectProxy({
    __bg3leContainer = container,
    __index = container == "array"
              and function(_, k) return items[Ext._Internal.ArrayIndex(k)] end or items,
    __newindex = function(_, k, v)
      write(container == "array" and Ext._Internal.ArrayIndex(k) or k, v)
    end,
    __len = container == "map" and function() return entry_count(items) end
            or function() return #items end,
    __pairs = function() return next, items, nil end,
    __bg3leAssign = assign,
  })
end

local function read_object_path(addr, class, path, kind)
  if kind == "variant" or kind == "unsupported" then
    -- Only the alternative a variant currently holds has bytes, so which
    -- one that is gets asked for at runtime rather than derived. An
    -- out-of-range answer means the metadata and the object disagree, and
    -- nil is the safe reading -- better than decoding the bytes as an
    -- alternative they are not.
    local active, count = Ext._Internal.ObjectVariantIndex(addr, class, path)
    if active == nil then return "<unsupported>" end
    if active >= count then return nil end   -- a valueless variant
    return read_object_path(addr, class, path .. "[" .. active .. "]",
                            Ext._Internal.ObjectFieldInfo(class,
                              path .. "[" .. active .. "]"))
  end

  if kind == "struct" then
    return read_object(addr, class, path, {})
  end

  -- A pointer reads as what it points at, or nil. Read when first touched:
  -- a snapshot is eager, and pointers form cycles.
  if kind == "pointer" then
    local target, err = Ext._Internal.ObjectGetField(addr, class, path)
    if target == nil then return err ~= nil and "<unreadable>" or nil end
    local pointee = Ext._Internal.PointeeClass(class, path, true)
    if pointee == nil then
      local element = path .. "[0]"
      return read_object_path(addr, class, element,
                              Ext._Internal.ObjectFieldInfo(class, element))
    end
    return Ext._Internal.PointedObject(target, pointee)
  end

  if kind == "array" and not Ext._Internal.IsVector(class, path) then
    local function read_items(items)
      local count = Ext._Internal.ObjectArrayInfo(addr, class, path)
      for i = #items, 1, -1 do items[i] = nil end
      for i = 0, (count or 0) - 1 do
        local element = path .. "[" .. i .. "]"
        items[i + 1] = read_object_path(addr, class, element,
                                        Ext._Internal.ObjectFieldInfo(
                                          class, element))
      end
      return items
    end
    local function assign(values)
      local ok, err
      if Ext._Internal.IsHashSet(class, path) then
        ok, err = Ext._Internal.ObjectSetSet(addr, class, path, values)
      else
        ok, err = Ext._Internal.ObjectSetField(addr, class, path, values, true)
      end
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end

    -- A hash set -- a spell list, say -- behaves as upstream's set proxy,
    -- read live so a write is seen at once.
    if Ext._Internal.IsHashSet(class, path) then
      local _, elemKind = Ext._Internal.ObjectFieldInfo(class, path)
      return Ext._Internal.HashSetView(elemKind, class, path,
        function() return read_items({}) end, assign)
    end

    local items = read_items({})
    local function write(k, v)
      local n = #items
      -- Out of range is ignored, as upstream's array __newindex does.
      if type(k) == "number" and (k < 1 or k > n + 1) then return end
      if type(k) ~= "number" or (k == n + 1 and v == nil) then
        error("bg3le: " .. class .. "." .. path .. " index " .. tostring(k)
              .. " is out of range 1.." .. n, 0)
      end
      local ok, err
      if v == nil or k == n + 1 then
        ok, err = Ext._Internal.ObjectSetField(addr, class, path,
          Ext._Internal.ResizedList(function(j) return items[j] end, n, k, v))
      else
        ok, err = Ext._Internal.ObjectSetField(addr, class,
                                               path .. "[" .. (k - 1) .. "]", v)
      end
      if not ok then error("bg3le: " .. tostring(err), 0) end
      read_items(items)
    end
    return snapshot_container(items, "array", write, function(values)
      assign(values)
      read_items(items)
      for i, item in ipairs(values) do
        if type(item) == "table" and Ext._Internal.Walkable(items[i]) then
          Ext.Types.Unserialize(items[i], item)
        end
      end
    end)
  end

  -- Keyed by the map's own keys; an unreadable key gets make_map's
  -- placeholder, with its slot so two of them cannot collide.
  if kind == "map" then
    if Ext._Internal.ObjectArrayInfo(addr, class, path) == nil then return "<unreadable>" end
    local items, slots = {}, {}
    -- Refilled in place after a key is added or removed, so the snapshot
    -- container keeps the same table.
    local function scan()
      for k in pairs(items) do items[k] = nil end
      for k in pairs(slots) do slots[k] = nil end
      for i = 0, (Ext._Internal.ObjectArrayInfo(addr, class, path) or 0) - 1 do
        local k, keyKind = Ext._Internal.ObjectMapKey(addr, class, path, i)
        if keyKind == "entity" then k = Ext._Internal.EntityValue(k) end
        if k == nil then
          k = "<unreadable key " .. i .. ">"
        elseif type(k) == "string" and k:sub(1, 1) == "<" and k:sub(-1) == ">" then
          k = k:sub(1, -2) .. " at slot " .. i .. ">"
        end
        local element = path .. "[" .. i .. "]"
        items[k] = read_object_path(addr, class, element,
                                    Ext._Internal.ObjectFieldInfo(class, element))
        slots[k] = i
      end
    end
    scan()
    -- As a component's map, and upstream's map proxy: an existing key's value
    -- writes through, a new key is added, nil removes one, and a table for a
    -- struct value replaces it with a default value filled from the table.
    local function write(k, v)
      local i = slots[k]
      if v == nil then
        if i ~= nil then
          local ok, err = Ext._Internal.ObjectMapEdit(addr, class, path, k, false)
          if not ok then error("bg3le: " .. tostring(err), 0) end
          scan()
        end
        return
      end
      local struct = type(v) == "table"
        and Ext._Internal.ObjectFieldInfo(class, path .. "[0]") == "struct"
      if struct and i ~= nil and Ext._Internal.ObjectMapEdit(addr, class, path, k, false) then
        scan()
        i = nil
      end
      if i == nil then
        local ok, err = Ext._Internal.ObjectMapEdit(addr, class, path, k, true)
        if not ok then error("bg3le: " .. tostring(err), 0) end
        scan()
        i = slots[k]
        if i == nil then error("bg3le: " .. class .. "." .. path .. " did not take the key", 0) end
      end
      local element = path .. "[" .. i .. "]"
      if struct then
        Ext.Types.Unserialize(read_object_path(addr, class, element, "struct"), v)
      else
        local ok, err = Ext._Internal.ObjectSetField(addr, class, element, v)
        if not ok then error("bg3le: " .. tostring(err), 0) end
      end
      items[k] = read_object_path(addr, class, element,
                                  Ext._Internal.ObjectFieldInfo(class, element))
    end
    return snapshot_container(items, "map", write, nil)
  end

  local value, err = Ext._Internal.ObjectGetField(addr, class, path)
  if value == nil and err ~= nil then return "<unreadable>" end
  if kind == "entity" or (kind == "entityorvec3" and math.type(value) == "integer") then
    return Ext._Internal.EntityValue(value)
  end
  return value
end

-- Reading is a snapshot; writing goes to the engine.
--
-- The values are read once into a table behind the metatable rather than
-- into the object itself, which is what lets a write be noticed at all:
-- __newindex does not fire for a key the table already has, so filling the
-- object directly made every assignment a silent no-op. A mod editing a
-- spell list reported success and changed nothing.
--
-- Upstream hands back a live proxy. A snapshot still differs in that a
-- field changing under you is not seen until the object is fetched again,
-- which for a resource definition it does not -- but a write landing is not
-- optional, and that is what this is.
function read_object(addr, class, prefix, out)
  local fields, err = Ext._Internal.ObjectFields(class, prefix)
  if fields == nil then error("bg3le: " .. tostring(err), 0) end

  -- Read when first asked for, as upstream's proxy reads: an object with a
  -- few hundred nested entries used to be read whole to answer one field --
  -- Mod Configuration Menu asks for the input manager four times as the menu
  -- comes up. A value bg3le's own decoding amended is kept (amend_object).
  local values = {}
  local read = {}
  local function value_of(name)
    local held = values[name]
    if held ~= nil or read[name] then return held end
    local kind = fields[name]
    if kind == nil then return nil end
    local path = (prefix == "") and name or (prefix .. "." .. name)
    held = read_object_path(addr, class, path, kind)
    values[name] = held
    read[name] = true
    return held
  end

  local function where()
    if prefix == "" then return class end
    return class .. "." .. prefix
  end

  -- The type this view is of, for Ext.Types.GetObjectType and for the members
  -- a mod may have grafted on with Ext.Types.AddCustomFunction.
  local viewType = Ext._Internal.ViewTypeName(class, prefix, true)

  return Ext._Internal.NewObjectProxy({
    __bg3leIdentity = function() return "o:" .. addr .. ":" .. prefix end,
    __name = viewType,
    __index = function(self, key)
      local held = value_of(key)
      if held ~= nil then return held end

      local extra = Ext._Internal.CustomMember(viewType, key)
      if extra ~= nil then
        if extra.Fn ~= nil then return extra.Fn end
        return extra.Get(self)
      end
      return nil
    end,
    __newindex = function(self, key, value)
      local kind = fields[key]
      if kind == nil then
        local extra = Ext._Internal.CustomMember(viewType, key)
        if extra ~= nil then
          if extra.Set == nil then
            error("bg3le: " .. tostring(key) .. " is read-only", 0)
          end
          extra.Set(self, value)
          return
        end
        error("bg3le: " .. where() .. " has no field " .. tostring(key), 0)
      end

      local path = (prefix == "") and key or (prefix .. "." .. key)
      local ok, err2 = Ext._Internal.ObjectSetField(
        addr, class, path, value, Ext._Internal.Unserializing == true)
      -- As on a component: a hash set refuses a plain write, and a table is
      -- the whole set.
      if not ok and type(value) == "table" and tostring(err2):find("is read-only", 1, true) then
        ok, err2 = Ext._Internal.ObjectSetSet(addr, class, path, value)
      end
      if not ok then error("bg3le: " .. tostring(err2), 0) end

      -- Read back rather than storing what was asked for, so the view
      -- shows what the engine now holds.
      values[key] = read_object_path(addr, class, path, kind)
      read[key] = true
      if type(value) == "table" and value[1] ~= nil then
        Ext._Internal.FillStructElements(values[key], value)
      end
    end,
    __pairs = function(self)
      local key
      local amended = nil
      local extras, extra = nil, 0
      return function()
        if amended == nil then
          -- Every field, read as it is reached; one that reads as nil is
          -- skipped, as it was when the object was read up front.
          while true do
            key = next(fields, key)
            if key == nil then break end
            local value = value_of(key)
            if value ~= nil then return key, value end
          end
          amended = true
        end
        if amended == true then
          -- Then what bg3le's own decoding added that is not a field.
          while true do
            key = next(values, key)
            if key == nil then break end
            if fields[key] == nil then return key, values[key] end
          end
          amended = false
          extras = Ext._Internal.CustomMemberNames(viewType)
        end
        extra = extra + 1
        local name = extras[extra]
        if name == nil then return nil end
        return name, Ext._Internal.CustomMemberValue(self, viewType, name)
      end, self, nil
    end,
    -- Where it came from, for bg3le's own decoding.
    __bg3leObject = {addr = addr, class = class, path = prefix},
    -- The snapshot itself, so bg3le's own decoding can amend it. Assigning
    -- a field writes to the engine now, which is right for a mod and wrong
    -- for the code that fills in what a raw read could not decode: a
    -- StatsExpressionRef's Code and RefCount are not fields of anything.
    __bg3leValues = values,
  })
end

-- Upstream's Ext.System (SystemMap): the context's own ECS systems by bg3se's
-- label -- Ext.System.ClientVisual is ecl::VisualSystem -- as views of the
-- live system through its field table, so a write such as Armory's
-- ReloadAllVisuals = true lands on the engine's object.
Ext.System = setmetatable({}, {
  __index = function(_, name)
    if type(name) ~= "string" then return nil end
    local probe = Ext._Internal.SystemProbe(name)
    if probe == nil or not probe.Ok or probe.System == 0
       or Ext._Internal.ObjectFields(probe.Class, "") == nil then
      Ext.Log.PrintWarning("Trying to access system that is not mapped: " .. name)
      return nil
    end
    return read_object(probe.System, probe.Class, "", {})
  end,
  __newindex = function() error("Ext.System is read-only", 2) end,
  __tostring = function() return "SystemMap" end,
  __name = "SystemMap",
})

-- What a pointer field reads as: the object it points at, read as an object
-- of its own class -- a fresh root, so the paths below it stay short however
-- deep the chain -- and read when first touched, since pointers form cycles.
-- Its identity is its address, which is what AvoidRecursion keys on.
function Ext._Internal.PointedObject(target, class)
  if class == nil then return "<unsupported>" end
  local loaded
  -- A functor set changes under AddNew and Remove, so it is read afresh.
  local live = class == "stats::Functors"
  local function get()
    if loaded == nil or live then
      loaded = read_object(target, class, "", {})
      -- A pooled stats expression's Code and RefCount are getters upstream,
      -- not fields; read_functor supplies them the same way.
      if class:find("StatsExpressionPooled$") then
        local code, refCount = Ext._Internal.ExpressionAt(target)
        if code ~= nil then
          Ext._Internal.AmendObject(loaded, "Code", code)
          Ext._Internal.AmendObject(loaded, "RefCount", refCount)
        end
      end
      -- Upstream's FunctorList getter and its AddNew and Remove methods.
      if class == "stats::Functors" then
        local raw, list = Ext._Internal.FunctorsList(target), {}
        for i = 1, #raw, 2 do
          list[#list + 1] = Ext._Internal.PointedObject(raw[i], raw[i + 1])
        end
        Ext._Internal.AmendObject(loaded, "FunctorList", list)
        Ext._Internal.AmendObject(loaded, "AddNew", function(_, functorType)
          local value = functorType
          if type(functorType) == "string" then
            value = nil
            for k, label in pairs(Ext.Enums.StatsFunctorId) do
              if type(k) == "number" and label == functorType then value = k end
            end
          end
          value = math.tointeger(value)
          if value == nil then error("bg3le: " .. tostring(functorType) .. " is not a StatsFunctorId", 2) end
          local at, cls = Ext._Internal.FunctorsAdd(target, value)
          if at == nil then
            Ext.Log.PrintError("bg3le: Functors:AddNew(" .. tostring(functorType) .. "): " .. tostring(cls))
            return nil
          end
          return Ext._Internal.PointedObject(at, cls or "stats::Functor")
        end)
        Ext._Internal.AmendObject(loaded, "Remove", function(_, functor)
          local meta = Ext._Internal.RawGetMetatable(functor)
          local id = type(meta) == "table" and meta.__bg3leIdentity or nil
          local hex = type(id) == "string" and id:match("^p:(%x+)$") or nil
          if hex == nil then error("bg3le: Functors:Remove expects a functor", 2) end
          return Ext._Internal.FunctorsRemove(target, tonumber(hex, 16))
        end)
      end
    end
    return loaded
  end
  return Ext._Internal.NewObjectProxy({
    __index = function(_, k) return get()[k] end,
    __newindex = function(_, k, v) get()[k] = v end,
    __pairs = function() return pairs(get()) end,
    __len = function() return #get() end,
    __bg3leIdentity = string.format("p:%x", target),
    -- Its type, as Ext.Types.GetObjectType reports upstream's.
    __name = Ext._Internal.ViewTypeName(class, ""),
  })
end

-- Amends a snapshot without writing to the engine.
--
-- Assigning a field on a view goes to the engine, which is what a mod wants
-- and the opposite of what bg3le's own decoding wants: read_functor fills in
-- a condition and a pooled expression's Code and RefCount, none of which are
-- fields of the object they are filed under, and read_template adds the
-- engine's own name for a template. Those belong in the snapshot only.
local function amend_object(view, key, value)
  local meta = Ext._Internal.RawGetMetatable(view)
  local values = meta ~= nil and meta.__bg3leValues or nil
  if values == nil then
    view[key] = value
    return view
  end
  values[key] = value
  return view
end

-- Published so the stats code can reach it. The prelude is compiled in more
-- than one chunk, so a local here is not in scope there.
Ext._Internal.ReadObject = read_object
Ext._Internal.ReadObjectPath = read_object_path
Ext._Internal.AmendObject = amend_object

Ext.StaticData = {}

-- Ext.StaticData.Get(guid, type), where type is the resource class name such
-- as "ActionResource". Returns nil plus a reason, so a caller can tell "no
-- such GUID" from "no such resource type".
function Ext.StaticData.Get(guid, resourceType)
  if type(guid) ~= "string" or type(resourceType) ~= "string" then
    return nil, "Ext.StaticData.Get takes a GUID string and a type name"
  end

  local addr, err = Ext._Internal.ResourceGet(resourceType, guid)
  if addr == nil then return nil, err end
  return read_object(addr, resourceType, "", {})
end

-- Every GUID in a resource bank, so a type can be enumerated.
function Ext.StaticData.GetAll(resourceType)
  return Ext._Internal.ResourceGuids(resourceType)
end

-- Upstream's takes an entity as well as a handle.
function Ext.Entity.HandleToUuid(handle)
  if type(handle) ~= "number" then
    handle = Ext._Internal.EntityProxyHandle(handle)
    if handle == nil then return nil end
  end
  return Ext._Internal.GetField(handle, "Uuid", "EntityUuid")
end

function Ext.Entity.UuidToHandle(uuid) return Ext._Internal.UuidToHandle(uuid) end

-- ---- mod loading ----
--
-- Two kinds of mod: a loose directory containing Mods/<Name>/ScriptExtender/,
-- and a .pak with the same tree inside it, in the profile's Mods directory
-- or the install's Data/Mods. Installed mods are almost always packed, so
-- both have to work.
--
-- Packed mods load in the engine's load order, and then in the order of
-- anything else modsettings.lsx enables; see load_positions below for why
-- that second half exists. Loose ones load regardless: they are a
-- development convenience and never appear in modsettings.lsx at all.
local loaded = {}

-- Config.json of every mod seen, by module UUID: upstream's GetConfigs(),
-- which PersistentVars are keyed by.
local mod_configs = {}
local mods_loaded = false

local function read_file(path)
  local f = Ext._Internal.OpenFile(path, "rb")
  if not f then return nil end
  local text = f:read("a")
  f:close()
  return text
end

local function mod_roots()
  local roots = {}
  local env = Ext._Internal.Getenv("BG3LE_MOD_PATH")
  if env then
    for dir in string.gmatch(env, "[^:]+") do table.insert(roots, dir) end
  end
  local root = Ext._Internal.ExtenderRoot()
  if root then table.insert(roots, root .. "/mods") end
  return roots
end

-- Config.json is matched directly rather than parsed: the three fields
-- that matter are unambiguous in it.
local function mod_table_name(config)
  return string.match(config, '"ModTable"%s*:%s*"([^"]+)"')
end

local function mod_required_version(config)
  return tonumber(string.match(config, '"RequiredVersion"%s*:%s*(%d+)'))
end

local function mod_feature_flags(config)
  local flags = string.match(config, '"FeatureFlags"%s*:%s*%[(.-)%]')
  if flags == nil then return {} end

  local out = {}
  for flag in string.gmatch(flags, '"([^"]+)"') do out[#out + 1] = flag end
  return out
end

-- What each mod asks the extender for, and the union of it, the way
-- upstream reports both before it loads anything.
local function report_configs(configs)
  local version = 0
  local flags = {}
  local seen = {}

  for _, entry in ipairs(configs) do
    Ext.Log.Print(string.format("    '%s': SE v%s; flags: %s", entry.Name,
                                entry.Version or "?",
                                table.concat(entry.Flags, ", ")))
    if (entry.Version or 0) > version then version = entry.Version end
    for _, flag in ipairs(entry.Flags) do
      if not seen[flag] then
        seen[flag] = true
        flags[#flags + 1] = flag
      end
    end
  end

  if #configs > 0 then
    Ext.Log.Print(string.format("Merged config: SE v%d; flags: %s", version,
                                table.concat(flags, ", ")))
    local ours = Ext.Utils.Version()
    if version > ours then
      Ext.Log.PrintWarning(string.format(
        "bg3le: a mod asks for Script Extender v%d; this is v%d", version,
        ours))
    end
  end
end

-- The readers of loaded mods, and the one loading now.
local mod_readers = {}
local loading_mod = nil

-- Upstream's ModLoader.BootstrappingMod: the mod table being bootstrapped.
function Ext._Internal.BootstrappingMod()
  return loading_mod and loading_mod.ModTable
end

-- A loaded mod's reader, by its UUID or directory name, as upstream's
-- FindModByNameGuid takes either.
function Ext._Internal.ModReader(nameOrGuid)
  for _, reader in ipairs(mod_readers) do
    if reader.Uuid == nameOrGuid or reader.Name == nameOrGuid then return reader end
  end
  return nil
end


-- `read` takes a path under the mod's ScriptExtender directory and returns
-- its contents, so a loose mod and a packed one differ only in that.
-- A mod's view of Ext and of the log, ported from upstream's
-- LuaScripts/Libs/ModLoader.lua and Logger.lua.
local Logger = {
  EnabledLogLevels = {},
  EnabledMods = {},
  EnabledTopics = {},
  DispatchingLogEvent = false,
}
local LOG_LEVELS = {"Debug", "Info", "Warning", "Error"}

function Logger:Log(moduleUuid, level, topic, ...)
  if self.EnabledLogLevels[level] == false then return end
  if self.EnabledMods[moduleUuid] == false then return end
  if self.EnabledTopics[topic] == false then return end

  local prevented = false
  if not self.DispatchingLogEvent then
    self.DispatchingLogEvent = true
    local evt = {
      Stopped = false,
      ActionPrevented = false,
      Module = moduleUuid,
      Topic = topic,
      Level = level,
      Message = {...},
    }
    Ext.Events.Log:Throw(evt)
    prevented = evt.ActionPrevented
    self.DispatchingLogEvent = false
  end

  if not prevented then
    if level == "Error" then
      Ext.Log.PrintError(...)
    elseif level == "Warning" then
      Ext.Log.PrintWarning(...)
    else
      Ext.Log.Print(...)
    end
  end
end

function Logger:SetLogLevel(level)
  local enable = false
  for _, llevel in ipairs(LOG_LEVELS) do
    if level == llevel then enable = true end
    self.EnabledLogLevels[llevel] = enable
  end
  if not enable then error("Unknown log level: " .. tostring(level)) end
end

function Logger:CreateLogModule(moduleUuid)
  local log = setmetatable({}, {__index = Ext.Log})
  log.Log = function(level, topic, ...) self:Log(moduleUuid, level, topic, ...) end
  log.Debug = function(...) self:Log(moduleUuid, "Debug", "", ...) end
  log.Print = function(...) self:Log(moduleUuid, "Info", "", ...) end
  log.PrintWarning = function(...) self:Log(moduleUuid, "Warning", "", ...) end
  log.PrintError = function(...) self:Log(moduleUuid, "Error", "", ...) end
  log.MakePrinter = function(level, topic)
    return function(...) self:Log(moduleUuid, level, topic, ...) end
  end
  log.SetLogLevel = function(level) self:SetLogLevel(level) end
  log.EnableModLogging = function(modUuid, enable)
    self.EnabledMods[modUuid] = enable
  end
  log.EnableTopic = function(topic, enable) self.EnabledTopics[topic] = enable end
  return log
end

-- A mod's own Ext: the real one behind it, its own Log, Utils and Require,
-- and closed to writes once built, as upstream's is.
local function create_mod_ext(moduleUuid, require_fn)
  local ext = setmetatable({}, {__index = Ext})
  ext.Log = Logger:CreateLogModule(moduleUuid)
  ext.Utils = setmetatable({
    Print = function(...) ext.Log.Print(...) end,
    PrintWarning = function(...) ext.Log.PrintWarning(...) end,
    PrintError = function(...) ext.Log.PrintError(...) end,
  }, {__index = Ext.Utils})
  ext.Net = Ext.Net
  ext.Require = require_fn
  setmetatable(ext, {
    __index = Ext,
    __newindex = function(_, k)
      Ext.Log.PrintError("Couldn't set Ext." .. tostring(k)
        .. ": Please avoid extending the Ext table - it is dangerous and may "
        .. "break compatibility!")
    end,
  })
  return ext
end

local function load_mod_from(name, uuid, read, report, boot)
  -- boot is BootstrapModule.lua for the module-load state, else nil.
  local preinit = boot ~= nil
  local config = read("Config.json")
  if not config then
    if not preinit then
      Ext.Log.PrintWarning(string.format(
        "bg3le: %s has no readable ScriptExtender/Config.json; skipped", name))
    end
    return
  end

  if report ~= nil then
    report[#report + 1] = {
      Name = name,
      Version = mod_required_version(config),
      Flags = mod_feature_flags(config),
    }
  end

  local table_name = mod_table_name(config)
  if not table_name then
    if not preinit then
      Ext.Log.PrintWarning(string.format(
        "bg3le: %s has no ModTable in Config.json; skipping", name))
    end
    return
  end
  if uuid ~= nil then
    mod_configs[uuid] = {ModTable = table_name,
                         MinimumVersion = mod_required_version(config) or 0}
  end
  if loaded[table_name] then return end

  -- Each context runs its own bootstrap, as upstream does: the server
  -- state loads BootstrapServer.lua and the client state
  -- BootstrapClient.lua, and a mod that ships only one runs only there.
  boot = boot or (Ext.IsClient() and "BootstrapClient.lua" or "BootstrapServer.lua")
  local source = read("Lua/" .. boot)
  if not source then return end

  -- Upstream names the script it is about to run, which is what tells you
  -- the order mods actually loaded in.
  Ext.Log.Print(string.format("Loading %sbootstrap script: Mods/%s/ScriptExtender/Lua/%s",
                              preinit and "preinit " or "", name, boot))
  local started = Ext.Utils.MonotonicTime()

  -- A mod's globals live in its own table, as upstream's do: writing
  -- `function Foo() end` in a mod makes Mods.<ModTable>.Foo, and other mods
  -- reach it that way. Reads fall through to the real globals.
  -- ModuleUUID goes in before the table does. Mod Configuration Menu
  -- puts a __newindex on Mods to notice new mods and reads ModuleUUID off
  -- the value as it arrives, so assigning an empty table first and
  -- filling it afterwards makes every mod look anonymous.
  -- Ext.Require for this mod: resolved against it whenever it is called,
  -- and each file run once, its results kept, as upstream's FileLoader
  -- does -- a file two scripts require must not subscribe its handlers
  -- twice.
  local env
  local required = {}
  -- Chunk names as upstream's LuaLoadModScript gives them: "<Directory>/<file>",
  -- no "@", a directory over 37 characters losing its "_<guid>". MCM 1.41
  -- reads the calling mod from the first part of the source.
  local script_dir = #name > 37 and name:sub(1, #name - 37) or name
  local function mod_require(path, second)
    if second ~= nil then
      -- Ext.Require(mod, path): a file of that mod, as upstream's
      -- FileLoader includes it.
      if path ~= uuid then
        local key = tostring(path) .. "/" .. tostring(second)
        if required[key] == nil then
          required[key] = table.pack(Ext.Utils.Include(path, second, env))
        end
        return table.unpack(required[key], 1, required[key].n)
      end
      path = second
    end
    if path:sub(1, 10) == "builtin://" then
      if required[path] == nil then
        required[path] = table.pack(Ext.Utils.Include(nil, path, env))
      end
      return table.unpack(required[path], 1, required[path].n)
    end
    if required[path] ~= nil then return table.unpack(required[path]) end
    local text = read("Lua/" .. path)
    if not text then
      error("bg3le: Ext.Require could not read " .. path, 0)
    end
    local chunk, err = Ext._Internal.RawLoad(text, script_dir .. "/" .. path, "t", env)
    if not chunk then error(err, 0) end
    local results = table.pack(chunk())
    required[path] = results
    return table.unpack(results, 1, results.n)
  end

  -- Upstream's ModLoader:CreateModEnv.
  env = Mods[table_name]
  local ext = create_mod_ext(uuid, mod_require)
  local fields = {
    type = type, tostring = tostring, tonumber = tonumber, pairs = pairs,
    ipairs = ipairs, error = error, next = next,
    string = string, math = math, table = table,
    Ext = ext, Osi = Osi, Sandboxed = true,
    _P = ext.Log.Print, _PW = ext.Log.PrintWarning, _PE = ext.Log.PrintError,
    Print = ext.Log.Print, print = ext.Log.Print,
  }
  if env == nil then
    env = setmetatable({ ModuleUUID = uuid }, { __index = _G })
    for k, v in pairs(fields) do rawset(env, k, v) end
    env._G = env
    Mods[table_name] = env
  else
    if Ext._Internal.RawGetMetatable(env) == nil then setmetatable(env, { __index = _G }) end
    for k, v in pairs(fields) do
      if rawget(env, k) == nil then rawset(env, k, v) end
    end
    if rawget(env, "_G") == nil then rawset(env, "_G", env) end
  end

  -- ModuleUUID is the mod being loaded, set for the duration and cleared
  -- after, the way bg3se's LuaLoadGameBootstrap does it. Mods pass it
  -- straight to Ext.Vars and Ext.Mod, so without it they fail on line one.
  local previous = ModuleUUID
  ModuleUUID = uuid
  -- Also for the case where the table already existed.
  env.ModuleUUID = uuid

  local reader = { Name = name, Uuid = uuid, Read = read, Env = env,
                   ModTable = table_name }
  table.insert(mod_readers, reader)
  local outer = loading_mod
  loading_mod = reader

  -- A bootstrap that calls the global Ext.Require rather than its own
  -- still reaches this mod while it loads; the global is put back after.
  local global_require = Ext.Require
  Ext.Require = mod_require

  local chunk, err = Ext._Internal.RawLoad(source, script_dir .. "/" .. boot,
                                           "t", env)
  if not chunk then
    ModuleUUID = previous
    loading_mod = outer
    Ext.Require = global_require
    Ext.Log.PrintError(string.format("bg3le: %s failed to compile: %s", name, err))
    return
  end
  local ok, run_err = pcall(chunk)
  ModuleUUID = previous
  loading_mod = outer
  Ext.Require = global_require
  if not ok then
    Ext.Log.PrintError(string.format("bg3le: %s failed to load: %s", name, run_err))
    return
  end

  loaded[table_name] = true
  local took = Ext.Utils.MonotonicTime() - started
  if took >= 10 then
    Ext.Log.Print(string.format(
      "Loading %s for mod %s took %d ms", boot, name, took))
  end
  Ext.Log.Print(string.format("bg3le: loaded mod %s (Mods.%s)", name, table_name))
end

-- The game's load order, as UUID -> position. Packed mods load in it and
-- are skipped if absent, since that is what a disabled mod is. An empty
-- load order means the mod list has not been found yet, and then position
-- is unknown rather than absent -- dropping every mod would be worse.
local function positions_of(order)
  if not order or #order == 0 then return nil end
  local positions = {}
  for index, uuid in ipairs(order) do positions[uuid] = index end
  return positions
end

-- Which mods to load, and in what order.
--
-- The engine's load order decides, as upstream's does, and anything the
-- player enabled in modsettings.lsx that the engine did not load follows
-- it. That second half is a deliberate divergence: the engine drops a mod
-- from its list for reasons of its own -- a savegame's module list
-- overrides the file, for one -- and a script mod the player installed
-- and enabled should still run rather than vanish without a word.
local function load_positions(modules)
  local positions = positions_of(Ext.Mod.GetLoadOrder()) or {}

  -- Which uuids belong to a mod that actually carries scripts, so the
  -- message below can name them. Built from what was passed rather than
  -- read from a global: referencing one that was never assigned raised
  -- out of LoadMods, which loaded no packed mod at all.
  local scripted = {}
  for _, module in ipairs(modules or {}) do
    if module.Uuid ~= nil then scripted[module.Uuid] = module.Name end
  end

  local after = 0
  for _, at in pairs(positions) do
    if at > after then after = at end
  end

  local extra = 0
  local missing = {}
  for _, uuid in ipairs(Ext._Internal.ModSettingsOrder() or {}) do
    if positions[uuid] == nil then
      after = after + 1
      positions[uuid] = after
      -- Only a mod with scripts is worth mentioning. A data-only mod may
      -- be absent from the engine's module list for reasons of its own --
      -- one of these declares its Folder as "Game", so its content merges
      -- into the base module -- and none of that concerns bg3le.
      if scripted[uuid] ~= nil then
        extra = extra + 1
        missing[#missing + 1] = scripted[uuid]
      end
    end
  end

  if extra > 0 then
    -- Not a warning: on this build the engine's module list holds only
    -- what it needs where you are standing, and the rest of what the
    -- player enabled is still installed and still theirs to script
    -- against. reference/MOD-LOADING.md has the evidence.
    -- Named rather than counted while there are few of them: one missing
    -- mod is a question ("which?"), a long list is a summary.
    if extra <= 5 then
      Ext.Log.Print(string.format(
        "bg3le: the engine's load order has %d modules; it does not include "
        .. "%s, which modsettings.lsx enables, so %s scripts load after it",
        #Ext.Mod.GetLoadOrder(), table.concat(missing, ", "),
        extra == 1 and "its" or "their"))
    else
      Ext.Log.Print(string.format(
        "bg3le: the engine's load order has %d modules; %d mods enabled in "
        .. "modsettings.lsx are not among them, and their scripts load "
        .. "after it", #Ext.Mod.GetLoadOrder(), extra))
    end
  end
  return positions
end

-- ---- PersistentVars ----
--
-- Upstream's BuiltinLibraryServer.lua and ExtensionState's bookkeeping. The
-- savegame half is src/savegame.cpp.
function Ext._Internal._GetModPersistentVars(modTable)
  local tab = Mods[modTable]
  if tab ~= nil then
    local persistent = tab.PersistentVars
    if persistent ~= nil then
      return Ext.Json.Stringify(persistent)
    end
  end
end

function Ext._Internal._RestoreModPersistentVars(modTable, vars)
  local tab = Mods[modTable]
  if tab ~= nil then
    tab.PersistentVars = Ext.Json.Parse(vars)
  end
end

-- ExtensionState::RestoreModPersistentVars, for each mod in the save.
function Ext._Internal.RestorePersistentVars()
  if not mods_loaded or Ext.IsClient() then return end
  local saved = Ext._Internal.TakeSavedPersistentVars()
  if saved == nil then return end

  for mod, vars in pairs(saved) do
    local config = mod_configs[mod]
    if config ~= nil then
      Ext._Internal._RestoreModPersistentVars(config.ModTable, vars)
      if #vars > 3 then
        Ext.Log.PrintWarning(string.format("Mod %s: PersistentVars is "
          .. "deprecated; consider using ModVars/UserVars instead", mod))
      end
    else
      Ext.Log.PrintWarning(string.format("Savegame has persistent variables "
        .. "for mod %s, but it is not loaded or has no ModTable! Variables "
        .. "may be lost on next save!", mod))
    end
  end
end

-- GetPersistentVarMods and GetModPersistentVars: {{modId, json}, ...}.
function Ext._Internal.CollectPersistentVars()
  local cached = Ext._Internal.SavedPersistentVars()
  local mods = {}
  for mod in pairs(cached) do mods[mod] = true end
  for mod, config in pairs(mod_configs) do
    if config.MinimumVersion >= 4 then mods[mod] = true end
  end

  local out = {}
  for mod in pairs(mods) do
    local config = mod_configs[mod]
    local vars
    if config ~= nil then
      local ok, result = pcall(Ext._Internal._GetModPersistentVars,
                               config.ModTable)
      if ok then
        vars = result
      else
        Ext.Log.PrintError(tostring(result))
      end
    end
    if vars == nil and cached[mod] ~= nil then
      Ext.Log.PrintError(string.format("Persistent variables for mod %s could "
        .. "not be retrieved, saving cached values!", mod))
      vars = cached[mod]
    end
    if vars ~= nil then
      if #vars > 3 then
        Ext.Log.PrintWarning(string.format("Mod %s: PersistentVars is "
          .. "deprecated; consider using ModVars/UserVars instead", mod))
      end
      out[#out + 1] = {mod, vars}
    end
  end
  return out
end

-- The bootstraps, once per state. Upstream runs them when the game leaves
-- LoadModule, before the main menu exists; bg3le does that for the client
-- (src/game_state.cpp) and for the server once Osiris is bound.
local scripts_loaded = false

-- boot names another bootstrap to run instead (BootstrapModule.lua).
function Ext._Internal.LoadModScripts(boot)
  if boot == nil then
    if scripts_loaded then return end
    scripts_loaded = true
  end

  for _, root in ipairs(mod_roots()) do
    local names = Ext._Internal.ListDir(root .. "/Mods")
    if names then
      table.sort(names)
      for _, name in ipairs(names) do
        local dir = root .. "/Mods/" .. name .. "/ScriptExtender"
        local meta = read_file(root .. "/Mods/" .. name .. "/meta.lsx") or ""
        local uuid = string.match(meta,
          'id="UUID"%s+type="[%w]+"%s+value="([^"]+)"')
        load_mod_from(name, uuid,
          function(path) return read_file(dir .. "/" .. path) end, nil, boot)
      end
    end
  end

  local modules = Ext._Internal.PakModules()
  local positions = load_positions(modules)
  local configs = {}
  local packed = {}
  for _, module in ipairs(modules) do
    local at = positions[module.Uuid]
    if at ~= nil then
      module.At = at
      table.insert(packed, module)
    end
  end
  table.sort(packed, function(a, b)
    if a.At ~= b.At then return a.At < b.At end
    return a.Name < b.Name
  end)

  local readers = {}
  for _, module in ipairs(packed) do
    local prefix = "Mods/" .. module.Name .. "/ScriptExtender/"
    readers[module.Name] = function(path)
      return Ext._Internal.PakRead(module.Pak, prefix .. path)
    end
  end
  if boot ~= nil then
    for _, module in ipairs(packed) do
      load_mod_from(module.Name, module.Uuid, readers[module.Name], nil, boot)
    end
    return
  end

  Ext.Log.Print(string.format(
    "bg3le: %d of %d packed script modules will load",
    #packed, #modules))

  -- Upstream lists what every mod asks of the extender before it runs any
  -- of them, then the merged view. Collected on a first pass so the list
  -- comes out whole rather than interleaved with the loading.
  for _, module in ipairs(packed) do
    local config = readers[module.Name]("Config.json")
    if config ~= nil then
      configs[#configs + 1] = {
        -- The display name, as upstream lists it; the folder is what the
        -- bootstrap lines name.
        Name = module.ModName ~= "" and module.ModName or module.Name,
        Version = mod_required_version(config),
        Flags = mod_feature_flags(config),
      }
    end
  end
  report_configs(configs)

  -- What the client half would have run. bg3le has one Lua context, the
  -- server's, so a mod's client scripts do not run at all -- and for a UI
  -- mod that is most of it. Better said than silently missing.
  local clientOnly = {}
  for _, module in ipairs(packed) do
    local prefix = "Mods/" .. module.Name .. "/ScriptExtender/"
    if Ext._Internal.PakRead(module.Pak, prefix .. "Lua/BootstrapClient.lua")
        ~= nil then
      clientOnly[#clientOnly + 1] = module.Name
    end
  end
  if #clientOnly > 0 and Ext.IsServer() then
    Ext.Log.Print(string.format(
      "bg3le: %d mods also ship a BootstrapClient.lua, which the client "
      .. "context runs after this one (%s)", #clientOnly,
      table.concat(clientOnly, ", ")))
  end

  for _, module in ipairs(packed) do
    load_mod_from(module.Name, module.Uuid, readers[module.Name], nil)
  end
  mods_loaded = true
end

-- Upstream's LuaLoadPreinitBootstrap: each mod's BootstrapModule.lua, in the
-- state the client builds just before the stats load.
function Ext._Internal.LoadModuleBootstraps()
  Ext._Internal.LoadModScripts("BootstrapModule.lua")
end

-- A session coming up, in two halves so that both contexts finish the first
-- before either starts the second (lua_load_mods).
--
-- The first half: the bootstraps if they have not run. StatsLoaded fires here
-- only if RPGStats::Load could not be hooked (StatsTakeLoaded is false
-- otherwise); the hook fires it in the client, as upstream's does.
function Ext._Internal.LoadModsBegin()
  Ext._Internal.LoadModScripts()
  if Ext.Stats.Get ~= nil and Ext._Internal.StatsCount() > 0
     and Ext._Internal.StatsTakeLoaded() then
    Ext._Internal.FireEvent("StatsLoaded")
  end
end

-- The second: upstream's SessionLoading, the save's PersistentVars, then
-- SessionLoaded, once every mod has subscribed in its bootstrap.
function Ext._Internal.LoadModsSession()
  Ext._Internal.FireEvent("SessionLoading")
  Ext._Internal.RestorePersistentVars()
  Ext._Internal.RestoreSaveExtras()
  Ext._Internal.FireEvent("SessionLoaded")
end
-- Everything below runs last, once every module it touches exists.
-- An earlier version ran here from further up, which meant it bound
-- to tables that Ext.StaticData and others later replaced: the
-- context views kept pointing at the discarded table, and the
-- additions were simply lost.

-- ---- the rest of Ext.Stats ----

-- Upstream's submodules: SpellSet, EquipmentSet, TreasureTable,
-- TreasureCategory, ItemCombo, ItemComboPreview, ItemComboProperty,
-- ItemGroup and NameGroup, over the engine's managers
-- (src/vendor/stats_sets.cpp). Get and GetAll hand out live objects.
do
  local I = Ext._Internal
  local function object(addr, typeName)
    if addr == nil then return nil end
    return I.PointedObject(addr, typeName)
  end
  local function submodule(kind, withCreate, withGetAll)
    local m = {}
    function m.Get(name)
      return object(I.StatsSetGet(kind, tostring(name)))
    end
    if withGetAll then
      function m.GetAll()
        local addrs, typeName = I.StatsSetAll(kind)
        local out = {}
        for i, a in ipairs(addrs) do out[i] = I.PointedObject(a, typeName) end
        return out
      end
    end
    if withCreate then
      function m.Create(name)
        local addr, typeName = I.StatsSetCreate(kind, tostring(name))
        if addr == nil then error("bg3le: " .. tostring(typeName), 2) end
        return I.PointedObject(addr, typeName)
      end
    end
    return m
  end

  Ext.Stats.SpellSet = submodule(0, true, true)
  Ext.Stats.EquipmentSet = submodule(1, true, true)
  Ext.Stats.ItemCombo = submodule(4, true, true)
  Ext.Stats.ItemComboPreview = submodule(5, true, true)
  Ext.Stats.ItemComboProperty = submodule(6, true, true)
  Ext.Stats.ItemGroup = submodule(7, false, true)
  Ext.Stats.NameGroup = submodule(8, false, true)

  Ext.Stats.TreasureTable = submodule(2, false, false)
  function Ext.Stats.TreasureTable.GetLegacy(name)
    return I.StatsTreasureTableRead(tostring(name))
  end
  -- Errors point at the caller, as upstream's do, not at this file.
  local function forward(fn, ...)
    local ok, err = pcall(fn, ...)
    if not ok then error((tostring(err):gsub("^bg3le prelude:%d+: ", "")), 3) end
  end
  function Ext.Stats.TreasureTable.Update(tbl)
    if type(tbl) ~= "table" then error("Ext.Stats.TreasureTable.Update expects a table", 2) end
    forward(I.StatsTreasureTableUpdate, tbl)
  end

  Ext.Stats.TreasureCategory = {}
  function Ext.Stats.TreasureCategory.GetLegacy(name)
    return I.StatsTreasureCategoryRead(tostring(name))
  end
  function Ext.Stats.TreasureCategory.Update(name, tbl)
    if type(tbl) ~= "table" then error("Ext.Stats.TreasureCategory.Update expects a table", 2) end
    forward(I.StatsTreasureCategoryUpdate, tostring(name), tbl)
  end
end

-- The enumerations stats are written in terms of: "Damage Type",
-- "AbilityType" and so on, each a value list the engine parsed.
function Ext.Stats.EnumIndexToLabel(enumeration, index)
  return Ext._Internal.StatsEnumLabel(enumeration, index)
end

function Ext.Stats.EnumLabelToIndex(enumeration, label)
  return Ext._Internal.StatsEnumIndex(enumeration, label)
end

-- The attributes a modifier list declares, as name to type. This is what
-- says which keys a stat of that type can carry.
function Ext.Stats.GetModifierAttributes(modifierList)
  local attrs = Ext._Internal.StatsListAttrs(modifierList)
  if attrs == nil then return nil end
  local out = {}
  for _, a in ipairs(attrs) do out[a.Name] = a.Type end
  return out
end

-- Upstream's: the RPGStats object itself, with ExtraData as a live map.
do
  local extra
  local function extra_data()
    extra = extra or Ext._Internal.NewObjectProxy({
      __index = function(_, k) return Ext._Internal.StatsExtraGet(k) end,
      __newindex = function(_, k, v)
        if type(v) ~= "number" or not Ext._Internal.StatsExtraSet(tostring(k), v) then
          error("Cannot set ExtraData." .. tostring(k) .. " to " .. tostring(v), 2)
        end
      end,
      __pairs = function()
        return next, Ext._Internal.StatsExtraAll(), nil
      end,
      __name = "HashMap<FixedString, float>",
    })
    return extra
  end

  function Ext.Stats.GetStatsManager()
    local addr = Ext._Internal.StatsManagerAddress()
    if addr == nil then return nil end
    local view = Ext._Internal.PointedObject(addr, "stats::RPGStats")
    return Ext._Internal.NewObjectProxy({
      __index = function(_, k)
        if k == "ExtraData" then return extra_data() end
        return view[k]
      end,
      __newindex = function(_, k, v) view[k] = v end,
      __pairs = function()
        local f, st, c = pairs(view)
        return function(_, key)
          local k, v = f(st, key)
          if k == "ExtraData" then v = extra_data() end
          return k, v
        end, st, c
      end,
      __name = "stats::RPGStats",
      __bg3leIdentity = string.format("p:%x", addr),
    })
  end
end

-- Every stat that a mod loading before the named one could have seen.
-- Upstream walks its load-order bookkeeping; bg3le has the same
-- information from the archives, so this is a real answer rather than an
-- approximation.
function Ext.Stats.GetStatsLoadedBefore(modId)
  if type(modId) ~= "string" then
    error("Ext.Stats.GetStatsLoadedBefore expects a mod uuid", 2)
  end

  local order = Ext.Mod.GetLoadOrder()
  local rank, found = {}, false
  for i, uuid in ipairs(order) do
    rank[uuid] = i
    if uuid == modId then found = true end
  end
  if not found then
    Ext.Log.PrintError("Couldn't fetch stat entry list - mod " .. modId
                       .. " is not loaded.")
    return {}
  end

  local limit = rank[modId]
  local out = {}
  for _, name in ipairs(Ext.Stats.GetStats()) do
    local owner = select(1, Ext._Internal.StatOrigin(name))
    if owner ~= nil and rank[owner] ~= nil and rank[owner] <= limit then
      out[#out + 1] = name
    end
  end
  return out
end

-- Sync and SetPersistence exist at module level as well as on the stat
-- object, as upstream's do.
function Ext.Stats.Sync(statName, persist)
  if Ext._Internal.StatsFind(statName) == nil then
    Ext.Log.PrintError("Cannot sync nonexistent stat: " .. tostring(statName))
    return
  end
  Ext._Internal.SyncStat(statName, persist)
end

function Ext.Stats.SetPersistence()
  Ext._Internal.WarnOnce("Ext.Stats.SetPersistence() is deprecated")
end

-- Upstream's Create: a new stats entry in the modifier list, copied from a
-- template if one is named; nil, with upstream's message, if the name is
-- taken, the list unknown or the template missing. Sync gives it a prototype.
local create_warning_shown = false
function Ext.Stats.Create(statName, modifierList, copyFromTemplate, byRef)
  if not Ext._Internal.StatsModuleLoad and Ext.IsServer() and not create_warning_shown then
    create_warning_shown = true
    Ext.Log.PrintWarning("Stats entres created after ModuleLoad must be synced manually; "
      .. "make sure that you call SyncStat() on it when you're finished!")
  end
  local addr, err = Ext._Internal.StatsCreate(tostring(statName), tostring(modifierList))
  if addr == nil then
    Ext.Log.PrintError(err)
    return nil
  end
  local stat = Ext.Stats.Get(statName)
  if copyFromTemplate ~= nil then
    if Ext._Internal.StatsFind(copyFromTemplate) == nil then
      Ext.Log.PrintError("Cannot copy stats from nonexistent template: " .. tostring(copyFromTemplate))
      return nil
    end
    local ok, copyErr = pcall(stat.CopyFrom, stat, copyFromTemplate)
    if not ok then
      Ext.Log.PrintError(tostring(copyErr))
      return nil
    end
  end
  return stat
end
-- Upstream's structure edits. AddAttribute is only safe before any stats
-- object exists, and says so after; AddEnumerationValue appends a label.
function Ext.Stats.AddAttribute(modifierList, modifierName, typeName)
  local ok, err = Ext._Internal.StatsAttrAdd(tostring(modifierList), tostring(modifierName),
                                             tostring(typeName))
  if not ok then
    for line in tostring(err):gmatch("[^\n]+") do Ext.Log.PrintError(line) end
  end
  return ok
end

function Ext.Stats.AddEnumerationValue(typeName, enumLabel)
  local value, err = Ext._Internal.StatsEnumAdd(tostring(typeName), tostring(enumLabel))
  if value == nil then
    Ext.Log.PrintError(err)
    return nil
  end
  return value
end
-- Upstream's functor execution: a context from PrepareFunctorParams, run
-- through the engine's executor for its type; src/vendor/functor_exec.cpp.
do
  local CONTEXT_CLASS = {
    [1] = "stats::AttackTargetContextData", [2] = "stats::AttackPositionContextData",
    [3] = "stats::MoveContextData", [4] = "stats::TargetContextData",
    [5] = "stats::NearbyAttackedContextData", [6] = "stats::NearbyAttackingContextData",
    [7] = "stats::EquipContextData", [8] = "stats::SourceContextData",
    [9] = "stats::InterruptContextData",
  }

  local function address_of(v, what)
    local meta = Ext._Internal.RawGetMetatable(v)
    local id = type(meta) == "table" and meta.__bg3leIdentity or nil
    if type(id) == "function" then id = id(v) end
    local hex = type(id) == "string" and (id:match("^p:(%x+)$") or id:match("^o:(%d+):")) or nil
    if hex == nil then error("bg3le: expected " .. what, 3) end
    return id:sub(1, 2) == "p:" and tonumber(hex, 16) or tonumber(hex)
  end

  local function live_view(at, class)
    return Ext._Internal.NewObjectProxy({
      __index = function(_, k) return Ext._Internal.PointedObject(at, class)[k] end,
      __newindex = function(_, k, v) Ext._Internal.PointedObject(at, class)[k] = v end,
      __pairs = function() return pairs(Ext._Internal.PointedObject(at, class)) end,
      __bg3leIdentity = string.format("p:%x", at),
      __name = Ext._Internal.ViewTypeName(class, ""),
    })
  end

  function Ext.Stats.PrepareFunctorParams(contextType)
    local value = contextType
    if type(contextType) == "string" then
      value = nil
      for k, label in pairs(Ext.Enums.FunctorContextType) do
        if type(k) == "number" and label == contextType then value = k end
      end
    end
    value = math.tointeger(value)
    local at = value and CONTEXT_CLASS[value] and Ext._Internal.FunctorParams(value)
    if at == nil then error("Unsupported context type", 2) end
    return live_view(at, CONTEXT_CLASS[value])
  end

  local function execute(target, context, single, what)
    local ok, why = Ext._Internal.FunctorsExecute(address_of(target, what),
      address_of(context, "a functor context"), single)
    if not ok then Ext.Utils.PrintError(why) end
  end

  function Ext.Stats.ExecuteFunctors(functors, context)
    execute(functors, context, false, "a StatsFunctors")
  end

  function Ext.Stats.ExecuteFunctor(functor, context)
    execute(functor, context, true, "a StatsFunctor")
  end
end

-- A prototype is the engine's parsed form of a stat: the stats object says
-- what the .txt said, the prototype is what the engine runs. The spell and
-- status managers are found by validating each candidate against its own
-- contents; see src/vendor/prototypes.cpp.
local PROTOTYPE_KIND = {Spell = 0, Status = 1, Interrupt = 2, Passive = 3}

local function cached_prototype(kind, class)
  return function(name)
    if type(name) ~= "string" then return nil end
    -- An empty table means the search has not finished yet, which a
    -- caller cannot tell from "no such spell" if this returns nil.
    if Ext._Internal.PrototypeCount(PROTOTYPE_KIND[kind]) == 0 then
      error("bg3le: the " .. kind:lower() .. " prototype manager has not "
            .. "been found yet; it is still being searched for", 2)
    end
    local address = Ext._Internal.PrototypeFind(PROTOTYPE_KIND[kind], name)
    if address == nil then return nil end
    return Ext._Internal.ReadObject(address, class, "", {})
  end
end

Ext.Stats.GetCachedSpell = cached_prototype("Spell", "stats::SpellPrototype")
Ext.Stats.GetCachedStatus = cached_prototype("Status", "stats::StatusPrototype")

-- Passives and interrupts keep their prototypes in the map rather than
-- behind a pointer. The name still has to match the key, which is what
-- derives the stride between them.
Ext.Stats.GetCachedInterrupt =
  cached_prototype("Interrupt", "stats::InterruptPrototype")
Ext.Stats.GetCachedPassive =
  cached_prototype("Passive", "stats::PassivePrototype")
-- Keyed by GUID, as upstream's is: what a BoostInfo's Prototype holds.
function Ext.Stats.GetCachedBoost(guid)
  if type(guid) ~= "string" then return nil end
  local address = Ext._Internal.BoostPrototype(guid)
  if address == nil then return nil end
  return Ext._Internal.ReadObject(address, "stats::BoostPrototype", "", {})
end

-- ---- the rest of Ext.Entity ----

-- Every component bg3le can read by name, which is what a mod asks for
-- before deciding whether a component is worth looking at.
function Ext.Entity.GetRegisteredComponentTypes(oneFrame, mapped)
  local out = {}
  for _, t in ipairs(Ext._Internal.RegisteredComponentTypes()) do
    if (oneFrame == nil or t[2] == oneFrame)
       and (mapped == nil or (t[3] ~= false) == mapped) then
      out[#out + 1] = t[1]
    end
  end
  return out
end

-- Subscriptions. The registry and dispatch are real, so a mod's handlers
-- are held and fire when bg3le raises the event; what is missing is the
-- engine-side change detection that would raise them on its own. Nothing
-- is dropped silently -- Ext._Internal.FireEntityEvent is the seam, and it
-- is what the tick and the ECS hooks will call as they are written.
local entity_subs = {}
local next_sub = 1

local COMPONENT_EVENT_KINDS = {
  ["create"] = true, ["create-deferred"] = true,
  ["destroy"] = true, ["destroy-deferred"] = true,
}

local function subscribe(kind, component, handler, entity, once, flags)
  if type(handler) ~= "function" then
    error("Ext.Entity subscriptions expect a handler function", 3)
  end
  -- Under upstream's name, which is what events are delivered under.
  if type(component) == "string" and COMPONENT_EVENT_KINDS[kind] then
    component = Ext._Internal.ComponentShortName(component) or component
    Ext._Internal.WatchComponentEvents(component)
  elseif type(component) == "string" and kind == "change" then
    component = Ext._Internal.ComponentShortName(component) or component
    if not Ext._Internal.WatchReplication(component) then
      error("No replication events are available for components of type "
            .. component, 3)
    end
  end
  local id = next_sub
  next_sub = next_sub + 1
  entity_subs[id] = {
    Kind = kind, Component = component, Handler = handler,
    Entity = entity, Once = once or false,
    Flags = flags or -1,  -- all fields
  }
  return id
end

-- Construct and destroy events the engine raised since the last tick.
-- Upstream calls a non-deferred handler from inside the engine's own
-- callback; bg3le delivers both kinds here, the immediate ones first, since
-- its Lua states may only be entered from their own thread. A destroyed
-- component can no longer be read by then, so its handler gets nil for it.
function Ext._Internal.DeliverComponentEvents()
  local events = Ext._Internal.TakeComponentEvents()
  for _, e in ipairs(events) do
    local entity = Ext._Internal.EntityValue(e[1])
    if entity ~= nil then
      local component = e[3] == "create" and entity[e[2]] or nil
      Ext._Internal.FireEntityEvent(e[3], e[2], entity, component)
      Ext._Internal.FireEntityEvent(e[3] .. "-deferred", e[2], entity,
                                    e[3] == "create" and entity[e[2]] or nil)
    end
  end

  -- And the replication changes the last update made: upstream's OnChange
  -- handler gets the entity, the component and the changed field flags.
  -- Only the server world replicates, so the client leaves them queued.
  if Ext.IsClient() then return end
  for _, c in ipairs(Ext._Internal.TakeReplicationChanges()) do
    local entity = Ext._Internal.EntityValue(c[1])
    if entity ~= nil then
      Ext._Internal.FireEntityEvent("change", c[2], entity, c[3])
    end
  end
end

Ext._Internal.SubscribeEntity = subscribe

function Ext.Entity.Subscribe(component, handler, entity, flags)
  return subscribe("change", component, handler, entity, false, flags)
end

function Ext.Entity.OnCreate(component, handler, entity, deferred, once)
  return subscribe(deferred and "create-deferred" or "create", component,
                   handler, entity, once == true)
end

function Ext.Entity.OnCreateOnce(component, handler, entity)
  return subscribe("create", component, handler, entity, true)
end

function Ext.Entity.OnCreateDeferred(component, handler, entity)
  return subscribe("create-deferred", component, handler, entity, false)
end

function Ext.Entity.OnCreateDeferredOnce(component, handler, entity)
  return subscribe("create-deferred", component, handler, entity, true)
end

function Ext.Entity.OnDestroy(component, handler, entity, deferred, once)
  return subscribe(deferred and "destroy-deferred" or "destroy", component,
                   handler, entity, once == true)
end

function Ext.Entity.OnDestroyOnce(component, handler, entity)
  return subscribe("destroy", component, handler, entity, true)
end

function Ext.Entity.OnDestroyDeferred(component, handler, entity)
  return subscribe("destroy-deferred", component, handler, entity, false)
end

function Ext.Entity.OnDestroyDeferredOnce(component, handler, entity)
  return subscribe("destroy-deferred", component, handler, entity, true)
end

function Ext.Entity.OnChange(component, handler, entity, flags)
  return subscribe("change", component, handler, entity, false, flags)
end

-- Upstream's system hooks: the handler runs before or after the system's
-- own update, on the thread the engine updates it on.
local system_subs = { [false] = {}, [true] = {} }

local function subscribe_system(system, handler, post, once)
  if type(handler) ~= "function" then
    error("Ext.Entity subscriptions expect a handler function", 3)
  end
  local index, err = Ext._Internal.HookSystem(tostring(system))
  if index == nil then error(err, 3) end
  local id = next_sub
  next_sub = next_sub + 1
  local list = system_subs[post][index] or {}
  system_subs[post][index] = list
  list[#list + 1] = { Id = id, Handler = handler, Once = once == true }
  entity_subs[id] = { Kind = post and "system-post-update" or "system-update",
                      System = index, Post = post }
  return id
end

function Ext._Internal.FireSystemUpdate(index, post)
  local list = system_subs[post][index]
  if list == nil or #list == 0 then return end
  local i = 1
  while i <= #list do
    local sub = list[i]
    if entity_subs[sub.Id] == nil then
      table.remove(list, i)
    else
      local ok, err = xpcall(sub.Handler, debug.traceback)
      if not ok then
        Ext.Log.PrintError("System update event handler failed: " .. tostring(err))
      end
      if sub.Once then
        entity_subs[sub.Id] = nil
        table.remove(list, i)
      else
        i = i + 1
      end
    end
  end
end

function Ext.Entity.OnSystemUpdate(system, handler, once)
  return subscribe_system(system, handler, false, once)
end

function Ext.Entity.OnSystemPostUpdate(system, handler, once)
  return subscribe_system(system, handler, true, once)
end

function Ext.Entity.Unsubscribe(id)
  if entity_subs[id] == nil then return false end
  entity_subs[id] = nil
  return true
end

-- Raised by bg3le when it detects one of these; the ECS-side detection is
-- still to be written, so today it fires only for what bg3le itself does.
function Ext._Internal.FireEntityEvent(kind, component, entity, ...)
  local fields = kind == "change" and select(1, ...) or nil
  for id, sub in pairs(entity_subs) do
    if sub.Kind == kind and sub.Component == component
       and (sub.Entity == nil or sub.Entity == entity)
       and (fields == nil or sub.Flags & fields ~= 0) then
      if sub.Once then entity_subs[id] = nil end
      local ok, err = xpcall(sub.Handler, debug.traceback, entity, component,
                             ...)
      if not ok then
        Ext.Log.PrintError("Error while dispatching user function call: "
                           .. tostring(err))
      end
    end
  end
end

-- Entity enumeration, over the storage container bg3le captured. A
-- storage is an archetype, so the component filter is one lookup per
-- storage rather than per entity.
--
-- Handles come back as the integers Ext.Entity.Get accepts, which is what
-- upstream returns too.
-- Entities, not handles.
--
-- This returned the raw handles, and upstream returns entity objects: a
-- caller writes `for _, e in ipairs(...) do if e.DisplayName ...`, which on a
-- number raises "attempt to index a number value". Caught by
-- tools/check-reference.sh against the capture from the real extender, where
-- the same query works.
local function entities_from(handles)
  local out = {}
  for i, handle in ipairs(handles) do out[i] = Ext._Internal.EntityOf(handle) end
  return out
end

function Ext.Entity.GetAllEntities()
  local handles, err = Ext._Internal.AllEntities()
  if handles == nil then error("bg3le: " .. tostring(err), 2) end
  return entities_from(handles)
end

function Ext.Entity.GetAllEntitiesWithComponent(component)
  if type(component) ~= "string" then
    error("Ext.Entity.GetAllEntitiesWithComponent expects a component name", 2)
  end
  local handles, err = Ext._Internal.AllEntities(component)
  if handles == nil then error("bg3le: " .. tostring(err), 2) end
  return entities_from(handles)
end

-- The entities the engine gave a UUID, which is the set carrying
-- ls::uuid::Component.
function Ext.Entity.GetAllEntitiesWithUuid()
  local handles, err = Ext._Internal.AllEntities("Uuid")
  if handles == nil then error("bg3le: " .. tostring(err), 2) end

  -- Upstream keys this one by UUID rather than returning a plain list, and
  -- the values are entities like everywhere else.
  local out = {}
  for _, handle in ipairs(handles) do
    local uuid = Ext.Entity.HandleToUuid(handle)
    if uuid ~= nil then out[uuid] = Ext._Internal.EntityOf(handle) end
  end
  return out
end
-- Entities within a radius of a point.
--
-- Upstream asks the engine's spatial index; bg3le has not located it, so
-- this walks the entities that carry a transform and measures. The answer
-- is the same set -- the index is an acceleration structure, not a
-- different definition -- and at around five thousand transforms it is
-- fast enough to call, but it is linear where upstream's is not, so a
-- caller doing it every frame should know.
function Ext.Entity.GetEntitiesAroundPosition(position, radius)
  if type(position) ~= "table" or type(radius) ~= "number" then
    error("Ext.Entity.GetEntitiesAroundPosition(position, radius)", 2)
  end

  local out = {}
  local limit = radius * radius
  for _, entity in ipairs(Ext.Entity.GetAllEntitiesWithComponent("Transform")) do
    local transform = entity ~= nil and entity.Transform or nil
    local at = transform ~= nil and transform.Transform or nil
    local translate = at ~= nil and at.Translate or nil
    if translate ~= nil then
      local dx = translate[1] - position[1]
      local dy = translate[2] - position[2]
      local dz = translate[3] - position[3]
      if dx * dx + dy * dy + dz * dz <= limit then
        out[#out + 1] = entity
      end
    end
  end
  return out
end
-- Upstream's: a new entity through the calling thread's command buffer,
-- created immediately; Destroy queues the entity's removal.
function Ext.Entity.Create()
  local handle, err = Ext._Internal.EntityCreate()
  if handle == nil then error("bg3le: Ext.Entity.Create: " .. tostring(err), 2) end
  return Ext._Internal.EntityValue(handle)
end

function Ext.Entity.Destroy(entity)
  return Ext._Internal.EntityDestroy(entity)
end
-- Upstream's tracing: src/vendor/entity_trace.cpp logs from the engine's
-- command-buffer flush.
do
  local warned = false

  function Ext.Entity.SetupTracing(options)
    options = options or {}
    local function flag(name, default)
      if options[name] == nil then return default end
      return options[name] and true or false
    end
    local exclude = {}
    for _, name in ipairs(options.ExcludeModificationComponents or {}) do
      local index = Ext._Internal.ComponentIndex(name)
      -- One-frame types are not modified in place, as upstream skips them.
      if index ~= nil and index & 0x8000 == 0 then exclude[#exclude + 1] = index end
    end
    Ext._Internal.TraceSetup(flag("TrackECB", true), flag("TrackImmediateWorldCache", true),
      flag("TrackReplication", true), flag("TrackModifications", false), exclude)
  end

  function Ext.Entity.EnableTracing(enable)
    if not Ext.Debug.IsDeveloperMode() then
      Ext.Log.PrintError("Entity tracing is only available in developer mode")
      return
    end
    if enable and not warned then
      warned = true
      Ext.Log.PrintWarning("Entity tracing is a development tool designed for tracking entity changes; it should not be used in production!")
    end
    if not Ext._Internal.TraceEnable(enable and true or false) then
      Ext.Log.PrintError("bg3le: entity tracing needs the engine's command-buffer flush, which is not hooked on this build")
    end
  end

  function Ext.Entity.GetTrace()
    local at = Ext._Internal.TraceGet()
    if at == nil then return nil end
    return Ext._Internal.PointedObject(at, "ecs::ECSChangeLog")
  end

  function Ext.Entity.ClearTrace()
    Ext._Internal.TraceClear()
  end
end

-- ---- the rest of Ext.StaticData, and Ext.Definition ----

-- Upstream's bank writes; src/vendor/static_data_write.cpp.
function Ext.StaticData.Create(resourceType, guid)
  if guid == nil then guid = Ext.Utils.GenerateGuid() end
  local addr, err = Ext._Internal.StaticDataCreate(tostring(resourceType), tostring(guid))
  if addr == nil then
    Ext.Log.PrintError(err)
    return nil
  end
  return Ext._Internal.ReadObject(addr, tostring(resourceType), "", {})
end

function Ext.StaticData.ClearResourceBank(resourceType)
  local ok, err = Ext._Internal.StaticDataBankCall(tostring(resourceType), true)
  if not ok then Ext.Log.PrintError(err) end
end

function Ext.StaticData.SyncResourceBank(resourceType)
  local ok, err = Ext._Internal.StaticDataBankCall(tostring(resourceType), false)
  if not ok then Ext.Log.PrintError(err) end
end

-- Upstream's reads of ls::gTextureAtlasMap; nil where upstream's would be.
function Ext.StaticData.GetTextureAtlasManager()
  local at = Ext._Internal.TextureAtlasMap()
  return at and Ext._Internal.ReadObject(at, "TextureAtlasMap", "", {}) or nil
end

function Ext.StaticData.GetIconAtlas(icon)
  if type(icon) ~= "string" then return nil end
  local at = Ext._Internal.IconAtlas(icon)
  return at and Ext._Internal.ReadObject(at, "TextureAtlas", "", {}) or nil
end

function Ext.StaticData.GetIconUVs(icon)
  if type(icon) ~= "string" then return nil end
  local at = Ext._Internal.IconUVs(icon)
  return at and Ext._Internal.ReadObject(at, "UVValues", "", {}) or nil
end

-- Upstream's reads of a bank's ResourceGuidsByMod: each mod and the
-- resources it defines.
function Ext.StaticData.GetSources(resourceType)
  return Ext._Internal.ResourceSources(tostring(resourceType))
end

function Ext.StaticData.GetByModId(resourceType, modGuid)
  local sources = Ext._Internal.ResourceSources(tostring(resourceType))
  return sources and sources[string.lower(tostring(modGuid))] or nil
end

-- Upstream's Ext.Definition is Ext.StaticData under another name. It was
-- aliased further up, before Ext.StaticData existed, so it picked up the
-- stub instead; taken again here now that the real one is in place.
Ext.Definition = Ext.StaticData

-- ---- Ext.Resource ----
--
-- The other resource system: banks keyed by ResourceBankType, holding
-- visuals, animations and effects rather than GUID resources. Read from the
-- engine's current ResourceBank, as upstream's GetResource; src/resources.cpp.
local function resource_bank_type(bankType)
  local label = type(bankType) == "number" and Ext.Enums.ResourceBankType[bankType]
                or bankType
  if type(label) ~= "string" then return nil end
  for i = 0, 33 do
    if Ext.Enums.ResourceBankType[i] == label then return i, label end
  end
  return nil
end

function Ext.Resource.Get(id, bankType)
  local index, label = resource_bank_type(bankType)
  if index == nil then
    error("Ext.Resource.Get: unknown ResourceBankType " .. tostring(bankType), 2)
  end
  if type(id) ~= "string" then return nil end
  local addr = Ext._Internal.ResourceBankGet(index, id)
  if addr == nil then return nil end
  return Ext._Internal.ReadObject(addr, "resource::" .. label .. "Resource", "", {})
end

function Ext.Resource.GetAll(bankType)
  local index = resource_bank_type(bankType)
  if index == nil then
    error("Ext.Resource.GetAll: unknown ResourceBankType " .. tostring(bankType), 2)
  end
  local ids = Ext._Internal.ResourceBankKeys(index)
  if ids == nil then error("Resource manager not available", 2) end
  return ids
end

-- ---- Ext.Loca ----
--
-- Read from the engine's TranslatedStringRepository, as upstream does, and
-- from the .loca archives before it is populated: see
-- src/vendor/translated_strings.cpp and src/vendor/loca.cpp.

-- Upstream returns the fallback when a handle is unknown, and an empty
-- string when there is no fallback either.
function Ext.Loca.GetTranslatedString(handle, fallback)
  if type(handle) ~= "string" then return fallback or "" end
  local text = Ext._Internal.Loca(handle)
  if text ~= nil then return text end
  return fallback or ""
end

-- Upstream's: TranslatedStringKeyManager's Keys, each key's TranslatedString
-- read through the engine's own object.
function Ext.Loca.GetAllTranslatedStringKeys()
  local all = Ext._Internal.StringKeys()
  if all == nil then return nil end
  local views = {}
  local function view(key)
    local addr = all[key]
    if addr == nil then return nil end
    if views[key] == nil then
      views[key] = Ext._Internal.PointedObject(addr, "TranslatedString")
    end
    return views[key]
  end
  return Ext._Internal.NewObjectProxy({
    __bg3leContainer = "map",
    __index = function(_, key) return view(key) end,
    __len = function()
      local n = 0
      for _ in pairs(all) do n = n + 1 end
      return n
    end,
    __pairs = function(self)
      local key
      return function()
        key = next(all, key)
        if key == nil then return nil end
        return key, view(key)
      end, self, nil
    end,
  })
end

-- A key and a handle are the same string in this build -- a stat's
-- DisplayName is the handle, and it is what the .loca file is keyed by --
-- so the lookup is the same one, returning nil when nothing is keyed by it
-- rather than inventing a mapping.
-- Writes into the engine's repository, as upstream's does, so the game's
-- own interface shows it.
function Ext.Loca.UpdateTranslatedString(handle, value)
  if type(handle) ~= "string" or type(value) ~= "string" then
    error("Ext.Loca.UpdateTranslatedString(handle, value)", 2)
  end
  return Ext._Internal.LocaSet(handle, value)
end

-- Upstream returns the TranslatedString by value, which its serializer
-- pushes as the handle.
function Ext.Loca.GetTranslatedStringKey(key)
  local addr = Ext._Internal.StringKeyFind(tostring(key))
  if addr == nil then return nil end
  return Ext._Internal.PointedObject(addr, "TranslatedString").Handle.Handle
end

function Ext.Loca.UpdateTranslatedStringKey(key, handle)
  return Ext._Internal.StringKeySet(tostring(key), tostring(handle))
end

-- ---- Ext.Template ----
--
-- Upstream's ServerTemplate.inl and ClientTemplate.inl: the managers are
-- read in src/vendor/templates.cpp. A template's concrete type comes from its
-- own vtable, decoded rather than called, and decides which class the
-- reflective reader expands it as.

-- The engine's type name to the class bg3se describes. The types with no
-- entry -- terrain, fogVolume, Spline, lightProbe, TileConstruction --
-- have no property map upstream either, so they read as the base class,
-- which is every field bg3se would expose for them anyway.
local TEMPLATE_CLASS = {
  character = "CharacterTemplate",
  item = "ItemTemplate",
  trigger = "TriggerTemplate",
  LevelTemplate = "LevelTemplate",
  scenery = "SceneryTemplate",
}

local function template_at(address, engineType)
  if address == nil then return nil end
  local class = TEMPLATE_CLASS[engineType] or "GameObjectTemplate"
  local out = Ext._Internal.ReadObject(address, class, "", {})
  -- What the engine calls it, which is not a field on the object, so it goes
  -- into the snapshot rather than at the engine.
  Ext._Internal.AmendObject(out, "TemplateType", engineType)
  -- GetTemplateStorageType: the top three bits of the template's handle,
  -- which sits just after ParentTemplateId and has no field of its own.
  local at = Ext._Internal.ObjectFieldAddress(address, class, "ParentTemplateId")
  local handle = at ~= nil and Ext._Internal.Peek(at + 4, 4) or nil
  if handle ~= nil then
    local storage = {[0] = "RootTemplate", "GlobalTemplate", "LocalTemplate",
                     "CacheTemplate", "LevelTemplate", "LevelCacheTemplate"}
    Ext._Internal.AmendObject(out, "TemplateStorageType", storage[handle >> 29])
  end
  return out
end

-- For esv::Item::CreateCacheTemplate, which is defined before this.
Ext._Internal.TemplateAt = template_at

-- The managers besides the root one, as TemplateFindIn numbers them.
local LOCAL, CACHE, LOCAL_CACHE = 1, 2, 3

local function template_in(source, id)
  if type(id) ~= "string" then return nil end
  if source == nil then return template_at(Ext._Internal.TemplateFind(id)) end
  return template_at(Ext._Internal.TemplateFindIn(source, id))
end

local function all_in(source)
  if source == nil then
    local out = {}
    for _, id in ipairs(Ext._Internal.TemplateIds()) do
      out[id] = template_in(nil, id)
    end
    return out
  end
  local addresses, types = Ext._Internal.TemplatesIn(source)
  if addresses == nil then return nil end
  local out = {}
  for id, address in pairs(addresses) do
    out[id] = template_at(address, types[id])
  end
  return out
end

function Ext.Template.GetRootTemplate(id) return template_in(nil, id) end
function Ext.Template.GetAllRootTemplates() return all_in(nil) end

if Ext._Internal.IsClientState() then
  Ext.Template.GetTemplate = Ext.Template.GetRootTemplate
else
  function Ext.Template.GetLocalTemplate(id) return template_in(LOCAL, id) end
  function Ext.Template.GetCacheTemplate(id) return template_in(CACHE, id) end
  function Ext.Template.GetLocalCacheTemplate(id) return template_in(LOCAL_CACHE, id) end
  function Ext.Template.GetAllLocalTemplates() return all_in(LOCAL) end
  function Ext.Template.GetAllCacheTemplates() return all_in(CACHE) end
  function Ext.Template.GetAllLocalCacheTemplates() return all_in(LOCAL_CACHE) end

  function Ext.Template.GetTemplate(id)
    return Ext.Template.GetRootTemplate(id)
      or Ext.Template.GetLocalTemplate(id)
      or Ext.Template.GetCacheTemplate(id)
      or Ext.Template.GetLocalCacheTemplate(id)
  end
end

-- ---- Ext.Level ----
--
-- Raycasts, sweeps and pathfinding all go through the level's physics
-- scene and pathfinder.
-- In a block of its own: the prelude's main chunk is at Lua's local limit.
do
-- The physics scene's queries, as upstream's: through the current level's
-- scene, whose virtuals src/vendor/level.cpp calls as bg3se declares them.
-- A hit is upstream's thread-local result, overwritten by the next query.
local function query_vec(v, what)
  if type(v) ~= "table" then error("bg3le: " .. what .. " must be a vec3 table", 3) end
  return v[1] or 0.0, v[2] or 0.0, v[3] or 0.0
end

local function query_flags(enum, v)
  if v == nil then return 0 end
  if type(v) == "number" then return math.tointeger(v) or 0 end
  local labels = Ext.Enums[enum]
  local function value_of(label)
    if type(label) == "number" then return math.tointeger(label) or 0 end
    for k, l in pairs(labels) do
      if type(k) == "number" and l == label then return k end
    end
    error("bg3le: " .. tostring(label) .. " is not a " .. enum .. " label", 4)
  end
  if type(v) == "string" then return value_of(v) end
  local out = 0
  for _, label in ipairs(v) do out = out | value_of(label) end
  return out
end

-- op, and whether it answers a hit, all hits, or a boolean.
local function query(op, shape, src, dst, ext, radius, halfHeight, ptype, incl, excl, context)
  local sx, sy, sz = query_vec(src, "the source")
  local dx, dy, dz = 0.0, 0.0, 0.0
  if dst ~= nil then dx, dy, dz = query_vec(dst, "the destination") end
  local ex, ey, ez = 0.0, 0.0, 0.0
  if ext ~= nil then ex, ey, ez = query_vec(ext, "the extents") end
  local hit, at = Ext._Internal.PhysicsQuery(op, Ext._Internal.IsClientState(),
    sx, sy, sz, dx, dy, dz, ex, ey, ez, radius or 0.0, halfHeight or 0.0,
    query_flags("PhysicsType", ptype), query_flags("PhysicsGroupFlags", incl),
    query_flags("PhysicsGroupFlags", excl), math.tointeger(context) or 0)
  if hit == nil then error("No level loaded - physics scene unavailable", 3) end
  if shape == "any" then return hit end
  if not hit then return nil end
  return Ext._Internal.PointedObject(at, shape == "all" and "phx::PhysicsHitAll" or "phx::PhysicsHit")
end

local L = Ext.Level
function L.RaycastClosest(s, d, t, i, e, c) return query(0, "one", s, d, nil, 0, 0, t, i, e, c) end
function L.RaycastAll(s, d, t, i, e, c) return query(1, "all", s, d, nil, 0, 0, t, i, e, c) end
function L.RaycastAny(s, d, t, i, e, c) return query(2, "any", s, d, nil, 0, 0, t, i, e, c) end
function L.SweepSphereClosest(s, d, r, t, i, e, c) return query(3, "one", s, d, nil, r, 0, t, i, e, c) end
function L.SweepCapsuleClosest(s, d, r, h, t, i, e, c) return query(4, "one", s, d, nil, r, h, t, i, e, c) end
function L.SweepBoxClosest(s, d, x, t, i, e, c) return query(5, "one", s, d, x, 0, 0, t, i, e, c) end
function L.SweepSphereAll(s, d, r, t, i, e, c) return query(6, "all", s, d, nil, r, 0, t, i, e, c) end
function L.SweepCapsuleAll(s, d, r, h, t, i, e, c) return query(7, "all", s, d, nil, r, h, t, i, e, c) end
function L.SweepBoxAll(s, d, x, t, i, e, c) return query(8, "all", s, d, x, 0, 0, t, i, e, c) end
function L.TestBox(p, x, t, i, e) return query(9, "all", p, nil, x, 0, 0, t, i, e, 0) end
function L.TestSphere(p, r, t, i, e) return query(10, "all", p, nil, nil, r, 0, t, i, e, 0) end

-- The AI grid's tiles, as upstream's: read in src/vendor/level.cpp.
function Ext.Level.GetEntitiesOnTile(pos)
  local x, y, z = query_vec(pos, "the position")
  local out = {}
  for i, handle in ipairs(Ext._Internal.AiEntitiesOnTile(Ext._Internal.IsClientState(), x, y, z)) do
    out[i] = Ext._Internal.EntityValue(handle)
  end
  return out
end

function Ext.Level.GetTileDebugInfo(pos)
  local x, y, z = query_vec(pos, "the position")
  local at = Ext._Internal.AiTileInfo(Ext._Internal.IsClientState(), x, y, z)
  if at == nil then return nil end
  return Ext._Internal.PointedObject(at, "AiGridLuaTile")
end

function Ext.Level.GetHeightsAt(x, z)
  return Ext._Internal.AiHeightsAt(Ext._Internal.IsClientState(), x, z)
end

-- Paths, as upstream's PathfindingSystem and AiPath helpers. A request's
-- path goes onto the grid's Paths list, where the engine searches it; each
-- tick the finished ones are handed to their callbacks and released.
local path_requests = {}

-- A live view: every access reads the path again, as upstream's proxy does.
local function path_view(at)
  if at == nil then return nil end
  return Ext._Internal.NewObjectProxy({
    __index = function(_, k) return Ext._Internal.PointedObject(at, "AiPath")[k] end,
    __newindex = function(_, k, v) Ext._Internal.PointedObject(at, "AiPath")[k] = v end,
    __pairs = function() return pairs(Ext._Internal.PointedObject(at, "AiPath")) end,
    __bg3leIdentity = string.format("p:%x", at),
    __name = Ext._Internal.ViewTypeName("AiPath", ""),
  })
end

local function path_address(path)
  local meta = Ext._Internal.RawGetMetatable(path)
  local id = type(meta) == "table" and meta.__bg3leIdentity or nil
  if type(id) ~= "string" or id:sub(1, 2) ~= "p:" then
    error("bg3le: expected an AiPath", 3)
  end
  return tonumber(id:sub(3), 16)
end

local function has_label(list, label)
  for _, l in ipairs(list or {}) do
    if l == label then return true end
  end
  return false
end

-- AiPath::SetBounds, SetSourceTemplate and SetSourceEntity.
local function path_set_bounds(path, moving, standing)
  path.MovingBound = moving
  path.StandingBound = standing
  path.MovingBound2 = moving
  path.MovingBoundTiles = math.floor(2.0 * moving + 0.5 - 0.001)
  path.StandingBoundTiles = math.floor(2.0 * standing + 0.5 - 0.001)
  path.MovingBoundTiles2 = math.floor(2.0 * moving + 0.5 - 0.001)
end

local function path_set_source_template(path, entity, tmpl)
  path.StepHeight = tmpl.MovementStepUpHeight
  path.WorldClimbingHeight = 0.0
  if tmpl.IsWorldClimbingEnabled then
    local canMove = entity.CanMove
    if canMove and has_label(canMove.Flags, "CanWorldClimb") then
      path.WorldClimbingHeight = tmpl.WorldClimbingHeight
    end
  end
  local radius = tmpl.WorldClimbingRadius
  if radius < 0 then radius = path.MovingBound end
  path.WorldClimbingRadius = radius
  path.TurningNodeAngle = tmpl.TurningNodeAngle
  path.TurningNodeOffset = tmpl.TurningNodeOffset
  path.UseStandAtDestination = tmpl.UseStandAtDestination
  path.WorldClimbType = 1
  path.WorldDropType = 1
  path.CheckLockedDoors = true
  path.CloseEnoughMin = 0.5
  path.CloseEnoughMax = 3.5
end

local function path_set_source_entity(path, entity)
  if type(entity) == "string" then entity = Ext.Entity.Get(entity) end
  if entity == nil then error("bg3le: the pathfinding source is not an entity", 3) end
  path.Source = entity
  local transform = entity.Transform
  local bounds = entity.Bound
  local moving, standing
  if bounds then
    moving = bounds.Bound.AIBounds.Move
    standing = bounds.Bound.AIBounds.Stand
  end
  if transform then
    local t = transform.Transform.Translate
    path.SourceOriginal = t
    path.SourceAdjusted = t
  end
  if transform and standing then
    path.Height = transform.Transform.Scale[1] * standing.Height * 0.65
  end
  if moving and standing then
    path_set_bounds(path, moving.Radius, standing.Radius)
  else
    path_set_bounds(path, 0.5, 0.5)
  end
  local character = entity.ServerCharacter or entity.ClientCharacter
  if character then
    path_set_source_template(path, entity, character.Template)
    path.IsPlayer = has_label(character.Flags, "IsPlayer")
  end
end

local function path_request(callback, immediate, source, target)
  local client = Ext._Internal.IsClientState()
  local at, why = Ext._Internal.AiPathCreate(client)
  if at == nil then
    if why ~= "no level loaded" then Ext.Utils.PrintError(why) end
    return nil
  end
  path_requests[#path_requests + 1] = {at = at, callback = callback, immediate = immediate}
  local path = path_view(at)
  path_set_source_entity(path, source)
  local x, y, z = query_vec(target, "the target")
  path.TargetAdjusted = {x, y, z}
  path.TargetPosition = {x, y, z}
  return path
end

local function path_release(req)
  if Ext._Internal.PointedObject(req.at, "AiPath").InUse then
    Ext._Internal.AiPathFree(Ext._Internal.IsClientState(), req.at)
  else
    Ext.Utils.PrintError(string.format("Trying to release path %x that is no longer in use?", req.at))
  end
end

-- PathfindingSystem::Update, from the tick.
function Ext._Internal.PathfindingUpdate()
  if #path_requests == 0 then return end
  local pending, finished = {}, {}
  for _, req in ipairs(path_requests) do
    local complete = Ext._Internal.PointedObject(req.at, "AiPath").SearchComplete
    if req.immediate then
      if not complete then
        Ext.Utils.PrintWarning(string.format("BeginPathfindingImmediate() was called on path %x, but no pathfinding was performed", req.at))
      end
      path_release(req)
    elseif complete then
      finished[#finished + 1] = req
      path_release(req)
    else
      pending[#pending + 1] = req
    end
  end
  path_requests = pending
  for _, req in ipairs(finished) do
    local ok, err = pcall(req.callback, path_view(req.at))
    if not ok then Ext.Utils.PrintError("Pathfinding callback failed: " .. tostring(err)) end
  end
end

function Ext.Level.BeginPathfinding(source, target, callback)
  return path_request(callback, false, source, target)
end

function Ext.Level.BeginPathfindingImmediate(source, target)
  return path_request(nil, true, source, target)
end

-- A search that has not finished runs now, through the engine's own.
function Ext.Level.FindPath(path)
  local at = path_address(path)
  local p = Ext._Internal.PointedObject(at, "AiPath")
  if not p.InUse then
    Ext.Utils.PrintError(string.format("Trying to pathfind on released path %x?", at))
    return false
  end
  if not p.SearchComplete then
    local found, why = Ext._Internal.AiPathSearch(Ext._Internal.IsClientState(), at)
    if found == nil then error("bg3le: Ext.Level.FindPath: " .. why, 2) end
  end
  return Ext._Internal.PointedObject(at, "AiPath").GoalFound
end

function Ext.Level.ReleasePath(path)
  local at = path_address(path)
  if not Ext._Internal.PointedObject(at, "AiPath").InUse then return end
  for i = #path_requests, 1, -1 do
    if path_requests[i].at == at then
      path_release(path_requests[i])
      table.remove(path_requests, i)
    end
  end
end

function Ext.Level.GetPathById(id)
  return path_view(Ext._Internal.AiPathById(Ext._Internal.IsClientState(), math.tointeger(id) or 0))
end

function Ext.Level.GetActivePathfindingRequests()
  local out = {}
  for i, at in ipairs(Ext._Internal.AiPathsActive(Ext._Internal.IsClientState())) do
    out[i] = path_view(at)
  end
  return out
end
end

-- Upstream's server-only four, through the level manager.
if not Ext._Internal.IsClientState() then
  -- Each type's class, as upstream's MakePolymorphicRef.
  local SURFACE_ACTION_CLASS = {
    [1] = "esv::CreateSurfaceAction", [2] = "esv::CreatePuddleAction",
    [3] = "esv::RemoveSurfaceAction", [4] = "esv::ZoneAction",
    [5] = "esv::TransformSurfaceAction", [6] = "esv::ChangeSurfaceOnPathAction",
    [7] = "esv::RectangleSurfaceAction", [8] = "esv::PolygonSurfaceAction",
    [9] = "esv::ForceCreateSurfaceAction", [10] = "esv::CapsuleSurfaceAction",
  }

  -- A live view: every access reads the action again.
  local function action_view(at, class)
    return Ext._Internal.NewObjectProxy({
      __index = function(_, k) return Ext._Internal.PointedObject(at, class)[k] end,
      __newindex = function(_, k, v) Ext._Internal.PointedObject(at, class)[k] = v end,
      __pairs = function() return pairs(Ext._Internal.PointedObject(at, class)) end,
      __bg3leIdentity = string.format("p:%x", at),
      __name = Ext._Internal.ViewTypeName(class, ""),
    })
  end

  function Ext.Level.CreateSurfaceAction(actionType)
    local value = actionType
    if type(actionType) == "string" then
      value = nil
      for k, label in pairs(Ext.Enums.SurfaceActionType) do
        if type(k) == "number" and label == actionType then value = k end
      end
    end
    value = math.tointeger(value)
    if value == nil or SURFACE_ACTION_CLASS[value] == nil then
      error("bg3le: " .. tostring(actionType) .. " is not a SurfaceActionType", 2)
    end
    local at, why = Ext._Internal.SurfaceActionCreate(value)
    if at == nil then
      if why ~= nil then error("bg3le: Ext.Level.CreateSurfaceAction: " .. why, 2) end
      return nil
    end
    return action_view(at, SURFACE_ACTION_CLASS[value])
  end

  function Ext.Level.ExecuteSurfaceAction(action)
    local meta = Ext._Internal.RawGetMetatable(action)
    local id = type(meta) == "table" and meta.__bg3leIdentity or nil
    if type(id) ~= "string" or id:sub(1, 2) ~= "p:" then
      error("bg3le: Ext.Level.ExecuteSurfaceAction expects a surface action", 2)
    end
    local ok, why = Ext._Internal.SurfaceActionExecute(tonumber(id:sub(3), 16))
    if not ok then Ext.Utils.PrintError(why) end
  end

  function Ext.Level.GetLevelInfo(levelName)
    local at = Ext._Internal.LevelDataManager()
    if at == nil or type(levelName) ~= "string" then return nil end
    return Ext._Internal.PointedObject(at, "LevelDataManager").Levels[levelName]
  end

  function Ext.Level.AddActivePersistentLevelTemplate(parentLevel, subLevelName, instanceId)
    local count, why = Ext._Internal.LevelAddPersistentTemplate(
      tostring(parentLevel), tostring(subLevelName), tostring(instanceId))
    if count == nil then
      Ext.Utils.PrintError("Tried to add persistent level to parent level '"
        .. tostring(parentLevel) .. "': " .. why)
    end
    return count
  end
end

-- ---- Ext.Server* and Ext.Client* ----
--
-- bg3se exposes most modules three times: under a plain name, and under a
-- Server and a Client name. They are the same functions -- a mod picks the
-- name that says which side it means to run on -- except that a few are
-- only on the plain one.
--
-- The table below is generated from the captured surface by
-- tools/gen-context-aliases.py rather than written out, because getting a
-- single name wrong here is a mod that does not run and nothing that says
-- so. A view forwards to the base rather than copying it, so a function
-- added to the base later appears in both twins without being listed twice.
local CONTEXT_MODULES = {
  {"ClientDebug", "Debug"},
  {"ClientEntity", "Entity", {GetEntitiesOnTile = true}},
  {"ClientIO", "IO"},
  {"ClientJson", "Json"},
  {"ClientLoca", "Loca"},
  {"ClientLog", "Log"},
  {"ClientMath", "Math"},
  {"ClientMod", "Mod"},
  {"ClientResource", "Resource"},
  {"ClientStaticData", "StaticData"},
  {"ClientStats", "Stats", {LoadStatsFile = true}},
  {"ClientTable", "Table"},
  {"ClientTimer", "Timer"},
  {"ClientTypes", "Types", {GenerateIdeHelpers = true}},
  {"ClientUtils", "Utils", {GameTime = true, LoadTestLibrary = true, MicrosecTime = true, MonotonicTime = true, Print = true, PrintError = true, PrintWarning = true, Profile = true, ProfileNamed = true, Random = true, Round = true}},
  {"ClientVars", "Vars"},
  -- Modules upstream registers for one context only (DECLARE_MODULE(x, Client)
  -- or Server) get only that context's prefix, and only in that context. The
  -- capture the table came from was the server's, so these were missing.
  {"ClientAudio", "Audio", nil, "Client"},
  {"ClientIMGUI", "IMGUI", nil, "Client"},
  {"ClientInput", "Input", nil, "Client"},
  {"ClientLevel", "Level", nil, "Client"},
  {"ClientNet", "Net", nil, "Client"},
  {"ClientTemplate", "Template", nil, "Client"},
  {"ClientUI", "UI", nil, "Client"},
  {"ServerDebug", "Debug"},
  {"ServerEntity", "Entity", {GetEntitiesOnTile = true}},
  {"ServerIO", "IO"},
  {"ServerJson", "Json"},
  {"ServerLevel", "Level", nil, "Server"},
  {"ServerLoca", "Loca"},
  {"ServerLog", "Log"},
  {"ServerMath", "Math"},
  {"ServerMod", "Mod"},
  {"ServerNet", "Net", nil, "Server"},
  {"ServerResource", "Resource"},
  {"ServerStaticData", "StaticData"},
  {"ServerStats", "Stats", {LoadStatsFile = true}},
  {"ServerTable", "Table"},
  {"ServerTemplate", "Template", nil, "Server"},
  {"ServerTimer", "Timer"},
  {"ServerTypes", "Types", {GenerateIdeHelpers = true}},
  {"ServerUtils", "Utils", {GameTime = true, LoadTestLibrary = true, MicrosecTime = true, MonotonicTime = true, Print = true, PrintError = true, PrintWarning = true, Profile = true, ProfileNamed = true, Random = true, Round = true}},
  {"ServerVars", "Vars"},
}

local function context_view(base, omit)
  local function visible(key)
    if omit ~= nil and omit[key] then return false end
    return base[key] ~= nil
  end

  return setmetatable({}, {
    __index = function(_, key)
      if not visible(key) then return nil end
      return base[key]
    end,

    -- Iterating a module is how a mod discovers what is there, so the view
    -- has to enumerate as the real one does.
    __pairs = function()
      local keys = {}
      for key in pairs(base) do
        if visible(key) then keys[#keys + 1] = key end
      end
      table.sort(keys)

      local i = 0
      return function()
        i = i + 1
        local key = keys[i]
        if key == nil then return nil end
        return key, base[key]
      end
    end,

    __newindex = function(_, key)
      error("Ext context modules mirror their base; set Ext.<module>."
            .. tostring(key) .. " instead", 2)
    end,
  })
end

-- Upstream's backwards-compatibility aliases (BuiltinLibrary.lua).
Ext.Utils.MonotonicTime = Ext.Timer.MonotonicTime
Ext.Utils.MicrosecTime = Ext.Timer.MicrosecTime
Ext.Utils.GameTime = Ext.Timer.GameTime
Ext.Entity.GetTile = Ext.Level.GetTile
Ext.Entity.GetEntitiesOnTile = Ext.Level.GetEntitiesOnTile

do
  local context = Ext._Internal.IsClientState() and "Client" or "Server"
  for _, entry in ipairs(CONTEXT_MODULES) do
    local name, from, omit, only = entry[1], entry[2], entry[3], entry[4]
    if Ext[from] ~= nil and (only == nil or only == context) then
      Ext[name] = context_view(Ext[from], omit)
    end
  end
end

-- Upstream's own Lua for these, from the builtin bundle: LoadStatsFile reads
-- a stats .txt through Create, SetRawAttribute, CopyFrom and Sync.
Ext.Utils.Include(nil, "builtin://Libs/Stats.lua")
-- ServerStartup.lua's: Ext.Types.GenerateIdeHelpers over the type registry.
Ext.Utils.Include(nil, "builtin://Libs/IdeHelpersGenerator.lua")

-- Upstream's BuiltinLibraryServer/Client.lua: in developer mode the test
-- library and the development helpers load with the state.
if Ext.Debug.IsDeveloperMode() then
  Ext.Utils.LoadTestLibrary()
  Ext.Utils.Include(nil, "builtin://Libs/DevelopmentHelpers.lua")
end


)LUA";
    // Named, so tracebacks say "bg3le prelude:917" rather than quoting the
    // whole source as the chunk name.
    if ((luaL_loadbuffer(g_lua, kPrelude, std::strlen(kPrelude), "=bg3le prelude")
         || lua_pcall(g_lua, 0, LUA_MULTRET, 0)) != LUA_OK) {
        logf("lua: prelude failed: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
    run_sandbox();
    statusf("LUA VM initialised (%s)", LUA_RELEASE);
}

// Calls a niladic Ext._Internal function, if it is present. Errors are logged
// rather than propagated: this runs on the game's own threads.
void call_internal(const char* name) {
    if (g_lua == nullptr) return;
    lua_getglobal(g_lua, "Ext");
    if (!lua_istable(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    lua_getfield(g_lua, -1, "_Internal");
    lua_remove(g_lua, -2);
    if (!lua_istable(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    lua_getfield(g_lua, -1, name);
    lua_remove(g_lua, -2);
    if (!lua_isfunction(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
        logf("lua: Ext._Internal.%s failed: %s", name, lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
}

void lua_set_symbols(const SymbolTable* symbols) {
    g_symbols = symbols;
    noesis_set_symbols(symbols);
}


void lua_load_mods();
void lua_bind_osi(const std::vector<osi::Function>& functions);

// Both contexts torn down and built again, then every mod reloaded.
//
// What upstream's Ext.Debug.Reset does, and for the reason it does it: a mod
// author edits a script and wants it running without restarting a game that
// takes a minute and a half to load here.
//
// The engine-side state is deliberately untouched. The story is still loaded,
// the node hooks are still installed, and the Osiris bindings are rebuilt
// from the function list bg3le already has -- resetting the Lua is the whole
// of it. What a mod put in the engine before the reset stays there, which is
// the same bargain upstream offers.
void imgui_api_reset();

void lua_reset(bool load_mods) {
    // Both locks, so neither context is mid-call on another thread.
    std::scoped_lock both(g_server_lock, g_client_lock);
    imgui_api_reset();
    bg3le_ui_reset();
    lua_State* oldServer = g_server_lua;
    lua_State* oldClient = g_client_lua;

    // Cleared first, so anything reached during the teardown sees no state
    // rather than a closed one.
    g_server_lua = nullptr;
    g_client_lua = nullptr;
    t_lua = nullptr;

    if (oldClient != nullptr) lua_close(oldClient);
    if (oldServer != nullptr) lua_close(oldServer);

    build_state(false);
    build_state(true);
    t_lua = nullptr;
    if (g_server_lua == nullptr) {
        logf("lua: reset failed; the server context could not be rebuilt");
        return;
    }

    // Osi.* lives in the state, so it has to be bound again. The function
    // list is bg3le's own and survived, so this needs no walk.
    if (!g_functions.empty()) {
        const std::vector<osi::Function> functions = g_functions;
        lua_bind_osi(functions);
    }

    if (!load_mods) {
        logf("lua: both contexts rebuilt for a new session");
        return;
    }
    lua_load_mods();

    g_reset_events_pending = true;
    logf("lua: reset -- both contexts rebuilt and every mod reloaded");
}

// Once per client state: LoadModScripts guards itself, and a rebuilt state
// (a new session, or back at the menu) loads them again.
namespace {
// Held back until the module list is found: without the statics cache only the
// warming thread may search for it, which finishes well after the menu, and a
// mod that calls Ext.Mod.GetMod as it loads (MCM) would find nothing.
std::atomic<bool> g_client_scripts_waiting{false};
std::chrono::steady_clock::time_point g_client_scripts_since;
constexpr auto kClientScriptsMaxWait = std::chrono::seconds(60);
}  // namespace

void lua_load_client_scripts() {
    if (g_client_lua == nullptr) return;
    if (bg3le_mods_count() == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (!g_client_scripts_waiting.exchange(true)) {
            g_client_scripts_since = now;
            logf("lua: client mods wait for the module list");
        }
        if (now - g_client_scripts_since < kClientScriptsMaxWait) return;
        logf("lua: no module list after 60 s; loading client mods without it");
    }
    g_client_scripts_waiting.store(false);
    logf("lua: loading client mods");
    InContext client(Side::Client);
    if (!client) return;
    call_internal("LoadModScripts");
}

namespace {
void fire_client_event(char const* name) {
    if (g_client_lua == nullptr) return;
    InContext client(Side::Client);
    if (!client) return;
    lua_State* L = g_lua;
    const int top = lua_gettop(L);
    lua_getglobal(L, "Ext");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "_Internal");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "FireEvent");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, name);
        if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
            logf("lua: client %s failed: %s", name, lua_tostring(L, -1));
        }
    }
    lua_settop(L, top);
}
}  // namespace

// Upstream's client RequestLuaReset: a fresh client state, the server untouched.
void lua_reset_client() {
    std::lock_guard<std::recursive_mutex> lock(g_client_lock);
    imgui_api_reset();
    bg3le_ui_reset();
    lua_State* old = g_client_lua;
    lua_State* was = t_lua;
    g_client_lua = nullptr;
    t_lua = nullptr;
    if (old != nullptr) lua_close(old);
    build_state(true);
    t_lua = was == old ? nullptr : was;
    logf("lua: client context rebuilt");
}

// Upstream's LoadExtensionState(Load) before RPGStats::Load: a fresh client
// state runs each mod's BootstrapModule.lua, then ModuleLoadStarted.
void lua_module_load_started() {
    if (g_client_lua == nullptr) return;
    lua_reset_client();
    if (bg3le_mods_count() == 0) {
        logf("lua: no module list yet; BootstrapModule.lua not run");
        return;
    }
    {
        InContext client(Side::Client);
        if (!client) return;
        call_internal("LoadModuleBootstraps");
    }
    fire_client_event("ModuleLoadStarted");
}

// The module-load state is discarded here for the game one, as upstream's is.
void lua_stats_loaded() {
    if (g_client_lua == nullptr) return;
    lua_reset_client();
    lua_load_client_scripts();
    logf("lua: client StatsLoaded");
    fire_client_event("StatsLoaded");
}

void lua_restore_persistent_vars() {
    if (g_server_lua == nullptr) return;
    InContext server(Side::Server);
    if (!server) return;
    call_internal("RestorePersistentVars");
}

void lua_restore_save_extras() {
    if (g_server_lua == nullptr) return;
    InContext server(Side::Server);
    if (!server) return;
    call_internal("RestoreSaveExtras");
}

namespace {

void read_saved_variables(lua_State* L, int table, std::vector<SavedVariable>* out) {
    const lua_Integer n = luaL_len(L, table);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, table, i);
        SavedVariable v;
        lua_geti(L, -1, 1);
        lua_geti(L, -2, 2);
        lua_geti(L, -3, 3);
        lua_geti(L, -4, 4);
        if (lua_isstring(L, -4) && lua_isstring(L, -3)) {
            v.Owner = lua_tostring(L, -4);
            v.Name = lua_tostring(L, -3);
            v.Type = (std::uint8_t)lua_tointeger(L, -2);
            switch (v.Type) {
            case 5: v.Bool = lua_toboolean(L, -1) != 0; break;
            case 1: v.Int = (std::int64_t)lua_tointeger(L, -1); break;
            case 2: v.Num = lua_tonumber(L, -1); break;
            default: {
                std::size_t len = 0;
                char const* text = lua_tolstring(L, -1, &len);
                if (text != nullptr) v.Str.assign(text, len);
                break;
            }
            }
            out->push_back(std::move(v));
        }
        lua_pop(L, 5);
    }
}

}  // namespace

bool lua_extras_to_save(SaveExtras* out) {
    if (g_server_lua == nullptr) return false;
    InContext server(Side::Server);
    if (!server) return false;
    lua_State* L = g_lua;
    const int top = lua_gettop(L);
    lua_getglobal(L, "Ext");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "_Internal");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "CollectSaveExtras");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        logf("lua: Ext._Internal.CollectSaveExtras failed: %s",
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
        lua_settop(L, top);
        return false;
    }
    const int result = lua_gettop(L);
    lua_getfield(L, result, "user");
    if (lua_istable(L, -1)) read_saved_variables(L, lua_gettop(L), &out->User);
    lua_pop(L, 1);
    lua_getfield(L, result, "mod");
    if (lua_istable(L, -1)) read_saved_variables(L, lua_gettop(L), &out->Mod);
    lua_pop(L, 1);
    lua_getfield(L, result, "timers");
    if (lua_istable(L, -1)) {
        const int timers = lua_gettop(L);
        const lua_Integer n = luaL_len(L, timers);
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_geti(L, timers, i);
            SavedTimer t;
            lua_geti(L, -1, 1);
            t.Frozen = (float)lua_tonumber(L, -1);
            lua_geti(L, -2, 2);
            t.Repeat = (float)lua_tonumber(L, -1);
            lua_geti(L, -3, 3);
            t.Paused = lua_toboolean(L, -1) != 0;
            lua_geti(L, -4, 4);
            if (lua_isstring(L, -1)) t.Handler = lua_tostring(L, -1);
            lua_geti(L, -5, 5);
            if (lua_isstring(L, -1)) t.Args = lua_tostring(L, -1);
            lua_pop(L, 6);
            out->Timers.push_back(std::move(t));
        }
    }
    lua_settop(L, top);
    return true;
}

bool lua_persistent_vars_to_save(
    std::vector<std::pair<std::string, std::string>>* out) {
    if (g_server_lua == nullptr) return false;
    InContext server(Side::Server);
    if (!server) return false;
    lua_State* L = g_lua;
    const int top = lua_gettop(L);
    lua_getglobal(L, "Ext");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "_Internal");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "CollectPersistentVars");
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return false;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        logf("lua: Ext._Internal.CollectPersistentVars failed: %s",
             lua_isstring(L, -1) ? lua_tostring(L, -1) : "no table");
        lua_settop(L, top);
        return false;
    }
    const lua_Integer n = luaL_len(L, -1);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, -1, i);
        lua_geti(L, -1, 1);
        lua_geti(L, -2, 2);
        if (lua_isstring(L, -2) && lua_isstring(L, -1)) {
            out->emplace_back(lua_tostring(L, -2), lua_tostring(L, -1));
        }
        lua_pop(L, 3);
    }
    lua_settop(L, top);
    return true;
}

std::atomic<bool> g_client_ticks_itself{false};

// Runs fn in the client Lua state now, from any thread, under the state's
// lock, as upstream's LuaClientPin does; false before the client ticks.
extern "C" bool bg3le_with_client_lua(void (*fn)(lua_State*, void*), void* user) {
    if (g_client_lua == nullptr || !g_client_ticks_itself.load()) return false;
    InContext client(Side::Client);
    if (!client) return false;
    fn(g_lua, user);
    return true;
}

void lua_tick() {
    if (strtab_tripwire_on()) bg3le_strtab_check("server tick");
    if (g_reset_pending) {
        g_reset_pending = false;
        lua_reset(true);
        // Upstream's RequestResetClientLuaState: clients on other machines too.
        net_reset_remote_clients();
    }
    // The server's state as of this tick; a change is upstream's server-side
    // GameStateChanged. Tracked before the state exists too, so the first
    // change a freshly loaded mod sees is a real one.
    static char const* serverState = nullptr;
    if (char const* now = server_game_state(); now != nullptr && now != serverState) {
        char const* was = serverState;
        serverState = now;
        if (was != nullptr) {
            logf("gamestate: server %s -> %s", was, now);
            if (std::strcmp(was, "LoadModule") == 0) bg3le_stats_module_loaded();
            InContext server(Side::Server);
            if (server) {
                lua_getglobal(g_lua, "Ext");
                lua_getfield(g_lua, -1, "_Internal");
                lua_getfield(g_lua, -1, "ServerStateChanged");
                lua_pushstring(g_lua, was);
                lua_pushstring(g_lua, now);
                if (lua_pcall(g_lua, 2, 0, 0) != LUA_OK) {
                    logf("lua: server GameStateChanged failed: %s", lua_tostring(g_lua, -1));
                    lua_pop(g_lua, 1);
                }
                lua_pop(g_lua, 2);
            }
        }
    }
    if (g_server_lua == nullptr) return;

    if (g_reset_events_pending) {
        g_reset_events_pending = false;
        {
            InContext server(Side::Server);
            if (server) call_internal("AfterReset");
        }
        if (g_client_lua != nullptr) {
            InContext client(Side::Client);
            if (client) call_internal("AfterReset");
        }
    }
    {
        InContext server(Side::Server);
        if (server) call_internal("RunTimers");
    }
    // The client ticks on its own thread once src/game_state.cpp drives it.
    if (g_client_lua != nullptr && !g_client_ticks_itself.load()) {
        InContext client(Side::Client);
        if (client) call_internal("RunTimers");
    }
}

bool story_ready();  // src/preload.cpp

void lua_client_tick(char const* from, char const* to) {
    if (strtab_tripwire_on()) bg3le_strtab_check("client tick");
    if (g_client_scripts_waiting.load()) lua_load_client_scripts();
    // Ext.Debug.Reset at the menu, where there is no server tick to do it:
    // the client's mods reload now, the server's with the next story.
    if (g_reset_pending && !story_ready()) {
        g_reset_pending = false;
        lua_reset(false);
        lua_load_client_scripts();
        if (g_client_lua != nullptr) {
            InContext client(Side::Client);
            if (client) call_internal("AfterReset");
        }
    }
    if (g_client_lua == nullptr) return;
    g_client_ticks_itself.store(true);
    InContext client(Side::Client);
    if (!client) return;
    if (from != nullptr && to != nullptr) {
        lua_getglobal(g_lua, "Ext");
        lua_getfield(g_lua, -1, "_Internal");
        lua_getfield(g_lua, -1, "ClientStateChanged");
        if (lua_isfunction(g_lua, -1)) {
            lua_pushstring(g_lua, from);
            lua_pushstring(g_lua, to);
            if (lua_pcall(g_lua, 2, 0, 0) != LUA_OK) {
                logf("lua: GameStateChanged failed: %s", lua_tostring(g_lua, -1));
                lua_pop(g_lua, 1);
            }
        } else {
            lua_pop(g_lua, 1);
        }
        lua_pop(g_lua, 2);
    }
    call_internal("RunTimers");
    debug_server_pump_client();
}

bool lua_client_input(InputKind kind, long long a, long long b, long long c,
                      long long d, long long e, double x, double y) {
    if (g_client_lua == nullptr || !g_client_ticks_itself.load()) return false;
    InContext client(Side::Client);
    if (!client) return false;
    lua_getglobal(g_lua, "Ext");
    lua_getfield(g_lua, -1, "_Internal");
    lua_getfield(g_lua, -1, "InputEvent");
    if (!lua_isfunction(g_lua, -1)) {
        lua_pop(g_lua, 3);
        return false;
    }
    lua_pushinteger(g_lua, (lua_Integer)kind);
    for (long long v : {a, b, c, d, e}) lua_pushinteger(g_lua, (lua_Integer)v);
    lua_pushnumber(g_lua, x);
    lua_pushnumber(g_lua, y);
    bool prevented = false;
    if (lua_pcall(g_lua, 8, 1, 0) != LUA_OK) {
        logf("lua: input event failed: %s", lua_tostring(g_lua, -1));
    } else {
        prevented = lua_toboolean(g_lua, -1) != 0;
    }
    lua_pop(g_lua, 3);
    return prevented;
}

// Both contexts load mods, each running the bootstrap that belongs to it.
// The server goes first, as upstream's does: a client script that asks the
// server for something wants a listener already registered. Every context's
// StatsLoaded comes before any SessionLoaded, as upstream's do.
void lua_load_mods() {
    for (char const* phase : {"LoadModsBegin", "LoadModsSession"}) {
        {
            InContext server(Side::Server);
            if (server) call_internal(phase);
        }
        if (g_client_lua != nullptr) {
            InContext client(Side::Client);
            if (client) call_internal(phase);
        }
    }
}

bool lua_has_client() { return g_client_lua != nullptr; }

void lua_bind_osi(const std::vector<osi::Function>& functions) {
    if (g_lua == nullptr) return;

    g_functions = functions;  // one copy, then never resized again

    // Grouped by name first: several declarations of one name are one Lua
    // function that picks between them by argument count.
    g_overloads.clear();
    std::unordered_map<std::string, std::size_t> groupOf;
    int events = 0;
    for (const osi::Function& fn : g_functions) {
        if (fn.kind() == osi::kEvent) {
            ++events;  // raised by the game, not callable
            continue;
        }
        auto found = groupOf.find(fn.name);
        if (found == groupOf.end()) {
            groupOf.emplace(fn.name, g_overloads.size());
            g_overloads.push_back({&fn});
        } else {
            g_overloads[found->second].push_back(&fn);
        }
    }

    lua_createtable(g_lua, 0, static_cast<int>(g_overloads.size()));
    int bound = 0;
    int overloaded = 0;
    for (auto const& entry : groupOf) {
        std::vector<const osi::Function*>& group = g_overloads[entry.second];
        if (group.size() > 1) ++overloaded;
        lua_pushlightuserdata(g_lua, &group);
        lua_pushcclosure(g_lua, osi_dispatch, 1);
        lua_setfield(g_lua, -2, entry.first.c_str());
        ++bound;
    }
    lua_setglobal(g_lua, "Osi");

    // The Windows extender generates "Name = Osi.Name" for every symbol, so
    // mods call Osiris functions bare: _D(GetHostCharacter()) is idiomatic.
    // Matching that is the point of sharing the API surface. Verified against
    // the enumerated names that none collide with a Lua global.
    lua_getglobal(g_lua, "Osi");
    for (auto const& entry : groupOf) {
        lua_getfield(g_lua, -1, entry.first.c_str());
        lua_setglobal(g_lua, entry.first.c_str());
    }
    lua_pop(g_lua, 1);

    // Match bg3se's name resolver: a wrong-case lookup on Osi resolves with a
    // compatibility warning rather than failing, since mods rely on that
    // leniency. Globals stay exact-case, as they are there too.
    lua_run(R"LUA(
setmetatable(_G, nil)  -- Osiris is bound; typos are plain nils again

local lower = {}
for name in pairs(Osi) do lower[string.lower(name)] = name end
setmetatable(Osi, {
  __index = function(t, key)
    local real = lower[string.lower(key)]
    if real ~= nil then
      Ext.Log.PrintWarning(string.format(
        "COMPATIBILITY WARNING: Osiris symbol '%s' referenced using incorrect " ..
        "case; the correct name is '%s'", key, real))
      local fn = rawget(t, real)
      rawset(t, key, fn)  -- cache, so the warning fires once per name
      return fn
    end

    -- The story's own procedures, events and databases, which are not
    -- bound up front: the first mention of one resolves it.
    local story = Ext._Internal.StoryFunction(key)
    if story ~= nil then
      rawset(t, key, story)
      lower[string.lower(key)] = key
      _G[key] = story  -- bare calls work too, as they do upstream
      return story
    end
    return nil
  end
})

-- A bare PROC_Foo(...) is idiomatic, and the engine's own symbols are
-- already globals. Story names become globals as they resolve; until one
-- is asked for, this is what finds it.
setmetatable(_G, {
  __index = function(_, key)
    if type(key) ~= "string" then return nil end
    if key:find("^DB_") == nil and key:find("^PROC_") == nil
       and key:find("^QRY_") == nil then
      return nil
    end
    return Osi[key]
  end
})
)LUA");

    statusf("Bound %d Osiris functions as Osi.* and globals (%d of them "
            "declared with more than one arity, %d events skipped)",
            bound, overloaded, events);
}

// A hooked system is about to update, or has: called on the thread the
// scheduler ran it on, which enters the context under its lock, as
// upstream's ContextGuardAnyThread does.
void system_update_event(bool client, std::int32_t index, bool post) {
    lua_State* want = client ? g_client_lua : g_server_lua;
    if (want == nullptr) return;
    InContext context(client ? Side::Client : Side::Server);
    if (context.state() != want) return;  // reset meanwhile
    lua_State* L = want;
    const int top = lua_gettop(L);
    lua_getglobal(L, "Ext");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "_Internal");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "FireSystemUpdate");
            if (lua_isfunction(L, -1)) {
                lua_pushinteger(L, index);
                lua_pushboolean(L, post ? 1 : 0);
                if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
                    logf("lua: system update event: %s", lua_tostring(L, -1));
                }
            }
        }
    }
    lua_settop(L, top);
}

void lua_eval_in(bool client, const char* code, std::string* result,
                 std::string* error) {
    lua_State* want = client ? g_client_lua : g_server_lua;
    if (want == nullptr) {
        *error = client ? "there is no client Lua context"
                        : "there is no server Lua context";
        return;
    }

    InContext context(client ? Side::Client : Side::Server);
    if (!context) {
        *error = client ? "there is no client Lua context"
                        : "there is no server Lua context";
        return;
    }
    lua_eval(code, result, error);
}

void lua_eval(const char* code, std::string* result, std::string* error) {
    if (g_lua == nullptr) {
        *error = "Lua is not initialised";
        return;
    }

    // Prefer expression form so a bare expression yields its value, falling
    // back to statement form when that will not compile.
    const std::string as_expr = std::string("return ") + code;
    if (luaL_loadstring(g_lua, as_expr.c_str()) != LUA_OK) {
        lua_pop(g_lua, 1);
        if (luaL_loadstring(g_lua, code) != LUA_OK) {
            *error = lua_tostring(g_lua, -1);
            lua_pop(g_lua, 1);
            return;
        }
    }

    const int before = lua_gettop(g_lua) - 1;
    if (lua_pcall(g_lua, 0, LUA_MULTRET, 0) != LUA_OK) {
        *error = lua_tostring(g_lua, -1);
        lua_pop(g_lua, 1);
        return;
    }

    const int count = lua_gettop(g_lua) - before;
    for (int i = 0; i < count; ++i) {
        if (i > 0) *result += "\t";
        *result += luaL_tolstring(g_lua, before + 1 + i, nullptr);
        lua_pop(g_lua, 1);
    }
    lua_pop(g_lua, count);
}

void lua_run(const char* code) {
    if (g_lua == nullptr) return;
    if ((luaL_loadbuffer(g_lua, code, std::strlen(code), "=bg3le")
         || lua_pcall(g_lua, 0, LUA_MULTRET, 0)) != LUA_OK) {
        logf("lua error: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
        return;
    }
    if (lua_gettop(g_lua) > 0) {
        logf("lua: %s", luaL_tolstring(g_lua, -1, nullptr));
        lua_pop(g_lua, 2);
    }
}

}  // namespace bg3le


extern "C" void bg3le_system_update_event(bool client, std::int32_t index, bool post) {
    bg3le::system_update_event(client, index, post);
}
