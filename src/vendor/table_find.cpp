// Ext.Table.Find over a plain table, as upstream's FindValueTable
// (Lua/Shared/LuaTable.inl): raw equality, the array part alone when the table
// has one, its hash part otherwise. The search reads the table's own layout,
// which no public Lua call exposes, so it lives here beside the fork's headers.

#include "lua.h"
#include "lauxlib.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
#include "lapi.h"

// bg3le_table_is_linear_array(table) -> boolean, as upstream's
// lua_is_linear_array (Lua/Shared/LuaGet.inl), which Ext.Json.Stringify asks:
// no hash part at all, and no hole in the array part before its trailing nils.
extern "C" int bg3le_table_is_linear_array(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto const* t = static_cast<Table const*>(lua_topointer(L, 1));
    bool linear = t->lastfree == nullptr;
    if (linear) {
        unsigned size = t->sizearray;
        while (size > 0 && ttisnil(&t->array[size - 1])) --size;
        for (unsigned i = 0; i < size && linear; ++i) {
            if (ttisnil(&t->array[i])) linear = false;
        }
    }
    lua_pushboolean(L, linear ? 1 : 0);
    return 1;
}

// bg3le_table_find_raw(table, value) -> key, or nil
extern "C" int bg3le_table_find_raw(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    auto const* t = static_cast<Table const*>(lua_topointer(L, 1));

    if (t->sizearray > 0) {
        for (unsigned i = 0; i < t->sizearray; ++i) {
            lua_rawgeti(L, 1, (lua_Integer)i + 1);
            const bool eq = lua_rawequal(L, -1, 2) != 0;
            lua_pop(L, 1);
            if (eq) {
                lua_pushinteger(L, (lua_Integer)i + 1);
                return 1;
            }
        }
    } else if (t->lastfree != nullptr) {
        const int nodes = sizenode(t);
        for (int i = 0; i < nodes; ++i) {
            Node* node = gnode(t, i);
            setobj2s(L, L->top, gval(node));
            api_incr_top(L);
            const bool eq = lua_rawequal(L, -1, 2) != 0;
            lua_pop(L, 1);
            if (eq) {
                setobj2s(L, L->top, gkey(node));
                api_incr_top(L);
                return 1;
            }
        }
    }

    lua_pushnil(L);
    return 1;
}
