// More GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c). Several write
// straight into client code or read-only data; the original unprotects through ntdll's
// NtProtectVirtualMemory (resolved by name hash 0xF0ADCB54 into DAT_10bdb104) -- VirtualProtect here.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Windows.h>
#include <cmath>
#include <cstring>
#include <string>

using namespace AscScript;

namespace
{
    void Patch(uint32_t addr, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(addr), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
    }
    template <typename T> void PatchValue(uint32_t addr, T v) { Patch(addr, &v, sizeof(T)); }

    int32_t ArgInt(lua_State* L, int idx) { return ToInt(AscLua::lua_tonumber(L, idx)); }

    // FUN_10a50910 + the idle check at 0x52B259: seconds (0 = never). The branch at 0x52B25F becomes an
    // unconditional jmp when disabled, the original jl otherwise; the compare immediate is -seconds.
    int SetAFKTimer(lua_State* L)
    {
        const int32_t v = ArgInt(L, 1);
        if (v == 0)
        {
            static const uint8_t kJmp[] = {0xE9, 0xEE, 0xFD, 0xFF, 0xFF};
            Patch(0x52B25F, kJmp, sizeof(kJmp));
        }
        else
        {
            static const uint8_t kJl[] = {0x0F, 0x8C, 0xED, 0xFD, 0xFF, 0xFF};
            Patch(0x52B25F, kJl, sizeof(kJl));
        }
        PatchValue<int32_t>(0x52B259, -v);
        return 0;
    }

    // FUN_10a503c0 + 0x52B266: the same for the AFK logout (jne at 0x52B27A; 4 jmp bytes when disabled).
    int SetAFKLogoutTimer(lua_State* L)
    {
        const int32_t v = ArgInt(L, 1);
        if (v == 0)
        {
            static const uint8_t kJmp[] = {0xE9, 0xD3, 0xFD, 0xFF};
            Patch(0x52B27A, kJmp, sizeof(kJmp));
        }
        else
        {
            static const uint8_t kJne[] = {0x0F, 0x85, 0xD2, 0xFD, 0xFF, 0xFF};
            Patch(0x52B27A, kJne, sizeof(kJne));
        }
        PatchValue<int32_t>(0x52B266, -v);
        return 0;
    }

    // Name-plate distance clamped to 5..60 yards, stored squared at 0xADAA7C.
    int SetNamePlateRange(lua_State* L)
    {
        const float x = static_cast<float>(AscLua::lua_tonumber(L, 1));
        const float lo = 5.0f <= x ? x : 5.0f;
        const float r = x <= 60.0f ? lo : 60.0f;
        PatchValue<float>(0xADAA7C, r * r);
        return 0;
    }

    float Clamp01(float x)
    {
        const float lo = 0.0f <= x ? x : 0.0f;
        return x <= 1.0f ? lo : 1.0f;
    }
    int SetNamePlateScaleX(lua_State* L)
    {
        PatchValue<float>(0xB2DA74, Clamp01(static_cast<float>(AscLua::lua_tonumber(L, 1))));
        return 0;
    }
    int SetNamePlateScaleY(lua_State* L)
    {
        PatchValue<float>(0xB2DA70, Clamp01(static_cast<float>(AscLua::lua_tonumber(L, 1))));
        return 0;
    }

    // The unit's chosen title (descriptor +0x504 -> 0x719900) as its CharTitles name: the female form
    // (+0x0C) for a player whose gender byte is set, else +0x08.
    int UnitTitle(lua_State* L)
    {
        uint64_t guid = 0;
        const uint8_t* unit = nullptr;
        const uint8_t* row = nullptr;
        if (UnitTokenGuid(CheckString(L, 1), guid) && (unit = static_cast<const uint8_t*>(ObjectPtr(guid, 0x10))) != nullptr)
        {
            const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
            const uint32_t title = reinterpret_cast<uint32_t(__stdcall*)(uint32_t)>(0x719900)(*reinterpret_cast<const uint32_t*>(d + 0x504));
            if (title)
                row = ClientDbcRow(0xAD3374, title);
        }
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
        const int32_t type = *reinterpret_cast<const int32_t*>(unit + 0x14);
        const bool female = (type == 3 || type == 4) && static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(d + 0x5C) >> 16) != 0;
        PushStr(L, *reinterpret_cast<const char* const*>(row + (female ? 0x0C : 0x08)));
        return 1;
    }

    // A non-empty string to the clipboard (CF_TEXT), unless the player carries descriptor +0xEC bit 19.
    // The original frees the handle after SetClipboardData; that is kept.
    int Internal_CopyToClipboard(lua_State* L)
    {
        std::string text;
        if (!ReadString(L, text) || text.empty())
            return 0;
        if (uint8_t* p = ActivePlayer())
            if ((*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(p + 8) + 0xEC) >> 19) & 1)
                return 0;
        if (!OpenClipboard(nullptr))
            return 0;
        if (EmptyClipboard())
        {
            const SIZE_T bytes = text.size() + 1;
            if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes))
            {
                memcpy(GlobalLock(mem), text.c_str(), bytes);
                SetClipboardData(CF_TEXT, mem);
                CloseClipboard();
                GlobalFree(mem);
                return 0;
            }
        }
        CloseClipboard();
        return 0;
    }

    // The unit tokens that currently resolve to a GUID (0x74D120 parses it, 0x60BB70 lists tokens).
    int GetTokenIDsFromGUID(lua_State* L)
    {
        if (!AscLua::lua_isstring(L, 1))
            AscLua::luaL_error(L, "Usage: GetTokenIDsFromGUID(\"playerGUID\")");
        const std::string s = CheckString(L, 1);
        uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x74D120)(s.c_str());
        if (guid == 0)
            AscLua::luaL_error(L, "Usage: GetTokenIDsFromGUID(\"playerGUID\")");
        int count = 0;
        const char* const* tokens = reinterpret_cast<const char* const*(__cdecl*)(uint64_t*, int*)>(0x60BB70)(&guid, &count);
        for (int i = 0; i < count; ++i)
            PushStr(L, tokens[i]);
        return count;
    }

    // Squared distance from the player to a player unit (0x718CA0 on the player checks the target);
    // (0, false) when it cannot be measured, nil for an unknown token.
    int UnitDistanceSquared(lua_State* L)
    {
        uint64_t guid = 0;
        if (!UnitTokenGuid(CheckString(L, 1), guid))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        uint8_t* player = ActivePlayer();
        uint8_t* target = static_cast<uint8_t*>(ObjectPtr(guid, 8));
        float d2 = 0.0f;
        bool ok = false;
        if (target && reinterpret_cast<bool(__thiscall*)(void*, void*)>(0x718CA0)(player, target))
        {
            typedef void(__thiscall* PosFn)(void*, float*);
            float a[3], b[3];
            reinterpret_cast<PosFn>((*reinterpret_cast<void***>(player))[0x2C / 4])(player, a);
            reinterpret_cast<PosFn>((*reinterpret_cast<void***>(target))[0x2C / 4])(target, b);
            const float dz = static_cast<float>(std::pow(static_cast<double>(a[2] - b[2]), 2.0));
            const float dx = static_cast<float>(std::pow(static_cast<double>(a[0] - b[0]), 2.0));
            const float dy = static_cast<float>(std::pow(static_cast<double>(a[1] - b[1]), 2.0));
            const float d = static_cast<float>(std::sqrt(static_cast<double>(dy + dx + dz)));
            d2 = d * d;
            ok = true;
        }
        AscLua::lua_pushnumber(L, d2);
        AscLua::lua_pushboolean(L, ok);
        return 2;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetTokenIDsFromGUID", GetTokenIDsFromGUID},
        {nullptr, "Internal_CopyToClipboard", Internal_CopyToClipboard},
        {nullptr, "SetAFKLogoutTimer", SetAFKLogoutTimer},
        {nullptr, "SetAFKTimer", SetAFKTimer},
        {nullptr, "SetNamePlateRange", SetNamePlateRange},
        {nullptr, "SetNamePlateScaleX", SetNamePlateScaleX},
        {nullptr, "SetNamePlateScaleY", SetNamePlateScaleY},
        {nullptr, "UnitDistanceSquared", UnitDistanceSquared},
        {nullptr, "UnitTitle", UnitTitle},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
