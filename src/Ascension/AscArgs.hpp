#pragma once
#include <Ascension/AscLua.hpp>
#include <string>

// Argument marshalling for the reconstructed Lua bindings.
//
// The original Extensions.dll routes almost every binding's argument reads through one small family
// of helpers. Decompiled (E:\oniwow-artifacts\ghidra-ext\out\all):
//
//   FUN_100b8970  93 bindings  type tag 4 (LUA_TSTRING)  -> luaL_checklstring @ 0x84F9F0
//   FUN_100b88c0  42 bindings  type tag 3 (LUA_TNUMBER)  -> luaL_checknumber  @ 0x84FAB0
//   FUN_100b8aa0  20 bindings  type tag 1 (LUA_TBOOLEAN)
//
// Each builds a small type descriptor (FUN_1008bf80), validates the argument through the shared
// validator FUN_10309e50, and only then reads it -- returning a BOOL rather than raising. That
// distinction matters: a Lua error longjmps past our __except, so the original's non-throwing
// contract is the one to reproduce, not luaL_check*'s.
//
// Validation here uses lua_isstring/lua_isnumber because those implement precisely the coercion
// rules luaL_check* applies (numbers count as strings, numeric strings count as numbers), so the
// accepted set matches the original without transcribing the validator's 1,204 lines.
//
// The originals are all hard-wired to argument 1; the index parameter here is a superset.
namespace AscArgs
{
    bool GetString(lua_State* L, int idx, std::string& out);
    bool GetNumber(lua_State* L, int idx, double& out);
    bool GetInt(lua_State* L, int idx, int& out);
    bool GetBool(lua_State* L, int idx, bool& out);

    // Optional forms: an absent argument yields the default and still reports success, a present
    // argument of the wrong type does not.
    bool OptString(lua_State* L, int idx, std::string& out, const char* def = "");
    bool OptNumber(lua_State* L, int idx, double& out, double def = 0.0);
    bool OptInt(lua_State* L, int idx, int& out, int def = 0);
    bool OptBool(lua_State* L, int idx, bool& out, bool def = false);

    bool Present(lua_State* L, int idx);

}
