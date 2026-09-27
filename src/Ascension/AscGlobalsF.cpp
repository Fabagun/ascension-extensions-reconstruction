// More GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c): request senders
// and small unit/cursor queries.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Windows.h>
#include <string>

using namespace AscScript;

namespace
{
    bool Gate() { return reinterpret_cast<int(__cdecl*)(uint32_t)>(0x5191C0)(0x11) != 0; }   // FUN_10111ce0
    int32_t ArgInt(lua_State* L, int idx) { return ToInt(AscLua::lua_tonumber(L, idx)); }

    // (x, y) in 0..1 of the client rect (0x86A1A0), y measured from the bottom; returns 1 without pushing.
    int SetCursorPosition(lua_State* L)
    {
        const float x = static_cast<float>(AscLua::lua_tonumber(L, 1));
        const float y = static_cast<float>(AscLua::lua_tonumber(L, 2));
        int rect[4];   // left, top, right, bottom
        if (reinterpret_cast<int(__cdecl*)(int*)>(0x86A1A0)(rect))
        {
            POINT pt;
            pt.x = static_cast<LONG>(static_cast<float>(rect[2] - rect[0]) * x) + rect[0];
            pt.y = static_cast<LONG>(static_cast<float>(rect[3] - rect[1]) * (1.0f - y)) + rect[1];
            HWND wnd = reinterpret_cast<HWND(__cdecl*)(int)>(0x86C6A0)(0);
            ClientToScreen(wnd, &pt);
            SetCursorPos(pt.x, pt.y);
        }
        return 1;
    }

    int RequestRespawnAtCheckpoint(lua_State*)
    {
        Packet(0x774).U8(1).Send();
        return 0;
    }

    int ToggleDraftMode(lua_State* L)   // CMSG 0x749 on, 0x74A off
    {
        bool enable;
        if (!ReadBool(L, enable))
            return 0;
        Packet(enable ? 0x749 : 0x74A).Send();
        AscLua::lua_pushboolean(L, 1);
        return 1;
    }

    int SetLevelScaling(lua_State* L)
    {
        if (Gate())
            Packet(0x667).U32(AscLua::lua_toboolean(L, 1) != 0).Send();
        return 0;
    }

    int SetLootSpecialization(lua_State* L)
    {
        Packet(0x679).U32(static_cast<uint32_t>(static_cast<int>(AscLua::lua_tonumber(L, 1)))).Send();
        return 0;
    }

    int RequestChangeRuleset(lua_State* L)
    {
        Packet(0x53F).U8(static_cast<uint8_t>(ArgInt(L, 1))).Send();
        return 0;
    }

    int RequestSetDungeonDifficulty(lua_State* L)
    {
        Packet(0x773).U8(static_cast<uint8_t>(ArgInt(L, 1))).Send();
        return 0;
    }

    int JoinArenaWithRole(lua_State* L)   // (bracket, role)
    {
        if (AscLua::lua_gettop(L) == 2)
        {
            const uint32_t a = static_cast<uint32_t>(ArgInt(L, 1));
            const uint32_t b = static_cast<uint32_t>(ArgInt(L, 2));
            Packet(0x5C1).U32(a).U32(b).Send();
        }
        return 0;
    }

    // Fires a registered interface event with the remaining arguments (0x81AA00), in world only.
    int SignalEvent(lua_State* L)
    {
        if (!*reinterpret_cast<const uint8_t*>(0xBD0792))
        {
            AscLua::luaL_error(L, "Script_SignalEvent can only be called once the player is in the world.");
            return 0;
        }
        const char* name = CheckString(L, 1);
        const int id = AscRuntime::EventId(name);
        if (id == -1)
        {
            const std::string msg = std::string("Event ") + name + " not found. Use RegisterInterfaceEvent(\"" + name
                                  + "\") to register it.";
            AscLua::luaL_error(L, msg.c_str());
            return 0;
        }
        reinterpret_cast<void(__cdecl*)(int, lua_State*, int)>(0x81AA00)(id, L, AscLua::lua_gettop(L));
        return 0;
    }

    // Unit by token (0x60C1F0): the spell being cast (+0xA6C), else the one channelled (+0xA80).
    int UnitCastingSpellID(lua_State* L)
    {
        std::string token;
        if (!ReadString(L, token))
            return 0;
        const uint8_t* unit = reinterpret_cast<const uint8_t*(__cdecl*)(const char*)>(0x60C1F0)(token.c_str());
        if (!unit)
            return 0;
        int32_t id = *reinterpret_cast<const int32_t*>(unit + 0xA6C);
        if (id == 0)
            id = *reinterpret_cast<const int32_t*>(unit + 0xA80);
        AscLua::lua_pushinteger(L, id);
        return 1;
    }

    // Player by token (FUN_103086d0): descriptor +0x13C bit 8; nil for a non-player.
    int UnitIsLevelScaling(lua_State* L)
    {
        const std::string token = CheckString(L, 1);
        const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x60C1C0)(token.c_str());
        const uint8_t* obj = guid ? static_cast<const uint8_t*>(ObjectPtr(guid, 8)) : nullptr;
        if (!obj)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_pushboolean(L, (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(obj + 8) + 0x13C) >> 8) & 1);
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "JoinArenaWithRole", JoinArenaWithRole},
        {nullptr, "RequestChangeRuleset", RequestChangeRuleset},
        {nullptr, "RequestRespawnAtCheckpoint", RequestRespawnAtCheckpoint},
        {nullptr, "RequestSetDungeonDifficulty", RequestSetDungeonDifficulty},
        {nullptr, "SetCursorPosition", SetCursorPosition},
        {nullptr, "SetLevelScaling", SetLevelScaling},
        {nullptr, "SetLootSpecialization", SetLootSpecialization},
        {nullptr, "SignalEvent", SignalEvent},
        {nullptr, "ToggleDraftMode", ToggleDraftMode},
        {nullptr, "UnitCastingSpellID", UnitCastingSpellID},
        {nullptr, "UnitIsLevelScaling", UnitIsLevelScaling},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
