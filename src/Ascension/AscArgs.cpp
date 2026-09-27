#include <Ascension/AscArgs.hpp>
#include <Ascension/AscLog.hpp>
#include <windows.h>

namespace
{
    int TypeAt(lua_State* L, int idx) { return AscLua::lua_type(L, idx); }
}

namespace AscArgs
{
    bool Present(lua_State* L, int idx)
    {
        if (!L) return false;
        if (idx > AscLua::lua_gettop(L)) return false;
        return TypeAt(L, idx) > LUA_TNIL;
    }

    bool GetString(lua_State* L, int idx, std::string& out)
    {
        if (!Present(L, idx) || !AscLua::lua_isstring(L, idx)) return false;
        const char* s = AscLua::lua_tolstring(L, idx, nullptr);
        if (!s) return false;
        out.assign(s);
        return true;
    }

    bool GetNumber(lua_State* L, int idx, double& out)
    {
        if (!Present(L, idx) || !AscLua::lua_isnumber(L, idx)) return false;
        out = AscLua::lua_tonumber(L, idx);
        return true;
    }

    bool GetInt(lua_State* L, int idx, int& out)
    {
        double d = 0.0;
        if (!GetNumber(L, idx, d)) return false;
        out = static_cast<int>(d);
        return true;
    }

    // The original's tag is LUA_TBOOLEAN, but this UI passes 1/0 for flags as often as true/false,
    // and lua_toboolean is what the client itself applies to them.
    bool GetBool(lua_State* L, int idx, bool& out)
    {
        if (!Present(L, idx)) return false;
        int t = TypeAt(L, idx);
        if (t != LUA_TBOOLEAN && t != LUA_TNUMBER && t != LUA_TSTRING) return false;
        out = AscLua::lua_toboolean(L, idx) != 0;
        return true;
    }

    bool OptString(lua_State* L, int idx, std::string& out, const char* def)
    {
        if (!Present(L, idx)) { out.assign(def ? def : ""); return true; }
        return GetString(L, idx, out);
    }

    bool OptNumber(lua_State* L, int idx, double& out, double def)
    {
        if (!Present(L, idx)) { out = def; return true; }
        return GetNumber(L, idx, out);
    }

    bool OptInt(lua_State* L, int idx, int& out, int def)
    {
        if (!Present(L, idx)) { out = def; return true; }
        return GetInt(L, idx, out);
    }

    bool OptBool(lua_State* L, int idx, bool& out, bool def)
    {
        if (!Present(L, idx)) { out = def; return true; }
        return GetBool(L, idx, out);
    }
}
