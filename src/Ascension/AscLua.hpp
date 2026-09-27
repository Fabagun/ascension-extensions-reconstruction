#pragma once
// The client's own Lua 5.1 C API, at its fixed addresses in Wow.exe 12340 (Ascension.exe is
// byte-identical there, no ASLR). Addresses cross-checked against the real Extensions.dll's
// exe pointer table (E:\oniwow-artifacts\ghidra-ext\sym\exe-pointer-table.yaml).
#include <cstdint>
#include <Misc/Macros.hpp>

struct lua_State;
typedef int (__cdecl* lua_CFunction)(lua_State* L);

#define LUA_GLOBALSINDEX (-10002)
#define LUA_TNIL 0
#define LUA_TBOOLEAN 1
#define LUA_TNUMBER 3
#define LUA_TSTRING 4
#define LUA_TTABLE 5
#define LUA_TFUNCTION 6

namespace AscLua
{
    CLIENT_FUNCTION(lua_gettop,       0x84DBD0, __cdecl, int,          (lua_State*))
    CLIENT_FUNCTION(lua_settop,       0x84DBF0, __cdecl, void,         (lua_State*, int))
    CLIENT_FUNCTION(lua_checkstack,   0x84DAB0, __cdecl, int,          (lua_State*, int))
    CLIENT_FUNCTION(lua_type,         0x84DEB0, __cdecl, int,          (lua_State*, int))
    CLIENT_FUNCTION(lua_isnumber,     0x84DF20, __cdecl, int,          (lua_State*, int))
    CLIENT_FUNCTION(lua_isstring,     0x84DF60, __cdecl, int,          (lua_State*, int))
    CLIENT_FUNCTION(lua_tonumber,     0x84E030, __cdecl, double,       (lua_State*, int))
    CLIENT_FUNCTION(lua_toboolean,    0x84E0B0, __cdecl, int,          (lua_State*, int))
    CLIENT_FUNCTION(lua_tolstring,    0x84E0E0, __cdecl, const char*,  (lua_State*, int, size_t*))
    CLIENT_FUNCTION(lua_pushnil,      0x84E280, __cdecl, void,         (lua_State*))
    CLIENT_FUNCTION(lua_pushnumber,   0x84E2A0, __cdecl, void,         (lua_State*, double))
    CLIENT_FUNCTION(lua_pushinteger,  0x84E2D0, __cdecl, void,         (lua_State*, int))
    CLIENT_FUNCTION(lua_pushstring,   0x84E350, __cdecl, void,         (lua_State*, const char*))
    CLIENT_FUNCTION(lua_pushcclosure, 0x84E400, __cdecl, void,         (lua_State*, lua_CFunction, int))
    CLIENT_FUNCTION(lua_pushboolean,  0x84E4D0, __cdecl, void,         (lua_State*, int))
    CLIENT_FUNCTION(lua_getfield,     0x84E590, __cdecl, void,         (lua_State*, int, const char*))
    CLIENT_FUNCTION(lua_createtable,  0x84E6E0, __cdecl, void,         (lua_State*, int, int))
    CLIENT_FUNCTION(lua_settable,     0x84E8D0, __cdecl, void,         (lua_State*, int))
    CLIENT_FUNCTION(lua_setfield,     0x84E900, __cdecl, void,         (lua_State*, int, const char*))
    CLIENT_FUNCTION(lua_pushvalue,    0x84DE50, __cdecl, void,         (lua_State*, int))
    CLIENT_FUNCTION(lua_rawseti,      0x84EA00, __cdecl, void,         (lua_State*, int, int))
    CLIENT_FUNCTION(lua_next,         0x84EF50, __cdecl, int,          (lua_State*, int))
    CLIENT_FUNCTION(luaL_error,       0x84F280, __cdecl, int,          (lua_State*, const char*, ...))
    // FrameScript layer
    CLIENT_FUNCTION(GetState,         0x817DB0, __cdecl, lua_State*,   ())               // returns the live lua_State
    CLIENT_FUNCTION(Execute,          0x819210, __cdecl, void,         (const char* code, const char* chunkName, void* unused))

    inline const char* tostring(lua_State* L, int idx) { return lua_tolstring(L, idx, nullptr); }
    inline void pushstring(lua_State* L, const char* s) { if (s) lua_pushstring(L, s); else lua_pushnil(L); }

    struct Reg { const char* name; lua_CFunction fn; };

    // Builds a table of C functions and binds it to the global `name`, exactly the way the
    // real Extensions.dll does (createtable / pushstring / pushcclosure / settable / setfield).
    void RegisterNamespace(lua_State* L, const char* name, const Reg* regs, size_t count);
    // Binds a single global C function into the live state (no FrameScript table involvement).
    void RegisterGlobal(lua_State* L, const char* name, lua_CFunction fn);
    // Same, but only if `name` isn't already a function (so a real hand-written implementation, or one
    // registered earlier in the same pass, always wins over a generated stub).
    void RegisterGlobalIfMissing(lua_State* L, const char* name, lua_CFunction fn);
}
