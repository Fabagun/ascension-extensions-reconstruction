#pragma once
// Registers the natives into every Lua state from the original's detour on the client's Lua library setup
// (0x855060, FUN_10278260). OnAttach (DllMain, main thread) installs it and SMSG_REALM_INFO.
struct lua_State;
namespace AscRegistry
{
    void OnAttach();
}
