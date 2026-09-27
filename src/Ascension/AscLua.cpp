#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>

namespace AscLua
{
    void RegisterNamespace(lua_State* L, const char* name, const Reg* regs, size_t count)
    {
        if (!L) return;
        lua_checkstack(L, 4);
        lua_createtable(L, 0, (int)count);
        for (size_t i = 0; i < count; ++i)
        {
            lua_pushstring(L, regs[i].name);
            lua_pushcclosure(L, regs[i].fn, 0);
            lua_settable(L, -3);
        }
        lua_setfield(L, LUA_GLOBALSINDEX, name);
        AscLog::Printf("registered %s (%u natives)", name, (unsigned)count);
    }

    void RegisterGlobal(lua_State* L, const char* name, lua_CFunction fn)
    {
        if (!L) return;
        lua_checkstack(L, 2);
        lua_pushcclosure(L, fn, 0);
        lua_setfield(L, LUA_GLOBALSINDEX, name);
    }

    void RegisterGlobalIfMissing(lua_State* L, const char* name, lua_CFunction fn)
    {
        if (!L) return;
        lua_checkstack(L, 2);
        lua_getfield(L, LUA_GLOBALSINDEX, name);
        int existing = lua_type(L, -1);
        lua_settop(L, -2);   // pop the probed value; -2 relative pop (settop with negative count)
        if (existing == LUA_TFUNCTION) return;
        RegisterGlobal(L, name, fn);
    }
}
