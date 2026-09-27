#include <Ascension/AscRegistry.hpp>
#include <Client/CVar.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscBuildCreator.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/RealmInfo.hpp>

#include <Windows.h>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdlib>


using namespace AscLua;

// The natives go into each Lua state from the original's detour on 0x855060 (FUN_10278260), the client's Lua
// library setup, called once per state by FrameScript's initialisation (0x819C7A) -- i.e. BEFORE the client
// registers its own glue / world natives. It runs the original, then, by the exe's flags:
//   *0xB6AA2C == 0 and *0xB2F9A0 == 0  ->  the glue lists 0x10BE2A0C (natives) and 0x10BE284C (after glue)
//   *0xB6AA2C != 0                     ->  the world lists 0x10BE2A2C (natives) and 0x10BE2A4C (after world)
// (Until 2026-09-27 the scaffold's call-site hooks on the client's own registrations, 0x4DA71D and 0x52AB17, ran
// ours AFTER the stock natives instead.) The reconstruction runs NO Lua of its own, as the original runs none.
// Diagnostics and the unattended test driver are the separate harness DLL (AscensionRebirth/harness).
namespace
{
    // No stand-ins for UI-defined globals: IsExtensionsLoaded (SharedConstants.lua), C_ClassInfo.GetSpecInfoByID
    // / GetClassBySpecName (ClassInfoUtil.lua, `if not ... then`) and GetInboxInvoiceQuantity at glue
    // (CustomFunctionChecks.lua, `if not ... then`) are the UI's own Lua in the original. A native registered
    // here would pre-empt the `if not` definitions. GlobalStrings.dbc is applied by the after-lists.
    typedef int(__cdecl* LuaSetup_t)(lua_State*);
    LuaSetup_t g_luaSetup = nullptr;

    int __cdecl LuaSetupDetour(lua_State* L)   // FUN_10278260
    {
        const int r = g_luaSetup(L);
        if (*reinterpret_cast<const uint32_t*>(0xB6AA2C) == 0)
        {
            if (*reinterpret_cast<const uint32_t*>(0xB2F9A0) == 0)
            {
                AscLog::Printf("Lua state %p: glue natives", static_cast<void*>(L));
                AscBindings::Register(L, true);
                AscBindings::RunGlueRegistered();
            }
        }
        else
        {
            AscLog::Printf("Lua state %p: world natives", static_cast<void*>(L));
            AscBindings::Register(L, false);
            AscBindings::RunWorldRegistered();
        }
        return r;
    }
}

namespace AscRegistry
{
    void OnAttach()
    {
        // SMSG_REALM_INFO (FUN_102fc6c0). Every other handler is registered by its module's Init.
        RealmInfoSvc::RegisterPacketHandlers();
        // 0x855060 starts push ebp / mov ebp,esp / push esi / mov esi,[ebp+8]: seven bytes, no relative operand.
        g_luaSetup = reinterpret_cast<LuaSetup_t>(AscRuntime::Detour(0x855060, 7, reinterpret_cast<void*>(&LuaSetupDetour)));
    }
}
