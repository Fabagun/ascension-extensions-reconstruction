// More GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <cstdio>
#include <string>

using namespace AscScript;

namespace
{
    constexpr uint32_t kChrRacesDbc = 0xAD3428;
    constexpr uint32_t kChrClassesDbc = 0xAD3404;
    constexpr uint32_t kSkillLineDbc = 0xAD45E0;
    constexpr uint32_t kWorldMapAreaDbc = 0xAD4EBC;

    uint32_t DbcMinId(uint32_t c) { return *reinterpret_cast<const uint32_t*>(c + 0x10); }
    uint32_t DbcMaxId(uint32_t c) { return *reinterpret_cast<const uint32_t*>(c + 0x0C); }
    const char* RowStr(const uint8_t* row, uint32_t off) { return *reinterpret_cast<const char* const*>(row + off); }

    // First row (min..max) whose string at +off equals name exactly.
    const uint8_t* FindByName(uint32_t dbc, uint32_t off, const std::string& name)
    {
        for (uint32_t i = DbcMinId(dbc); i <= DbcMaxId(dbc); ++i)
            if (const uint8_t* row = ClientDbcRow(dbc, i))
                if (name == RowStr(row, off))
                    return row;
        return nullptr;
    }

    int PushNils(lua_State* L, int n)
    {
        for (int i = 0; i < n; ++i)
            AscLua::lua_pushnil(L);
        return n;
    }

    // ChrRaces by client prefix (+0x2C): name (+0x38), prefix, id.
    int GetRaceInfo(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        const uint8_t* row = FindByName(kChrRacesDbc, 0x2C, name);
        if (!row)
            return PushNils(L, 3);
        PushStr(L, RowStr(row, 0x38));
        PushStr(L, RowStr(row, 0x2C));
        AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row));
        return 3;
    }

    // ChrClasses by name (+0x10): name, file name (+0x1C), id.
    int GetClassInfoByName(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        const uint8_t* row = FindByName(kChrClassesDbc, 0x10, name);
        if (!row)
            return PushNils(L, 3);
        PushStr(L, RowStr(row, 0x10));
        PushStr(L, RowStr(row, 0x1C));
        AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row));
        return 3;
    }

    // SkillLine name (+0x0C), then the player's rank / temp bonus / max via the player's skill slot
    // (0x6DC1C0 -> 0x51A250, 0x5CD920, 0x594710), profession flag (category +4 == 11), description (+0x10).
    int GetSkillInfo(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1)));
        const uint8_t* row = ClientDbcRow(kSkillLineDbc, id);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const bool profession = *reinterpret_cast<const int32_t*>(row + 4) == 0xB;
        if (uint8_t* p = ActivePlayer())
        {
            typedef int(__thiscall* SkillFn)(void*, int);
            const int slot = reinterpret_cast<SkillFn>(0x6DC1C0)(p, static_cast<int>(id));
            if (slot != -1)
            {
                const int rank = reinterpret_cast<SkillFn>(0x51A250)(p, slot);
                const int b = reinterpret_cast<SkillFn>(0x594710)(p, slot);
                const int c = reinterpret_cast<SkillFn>(0x5CD920)(p, slot);
                PushStr(L, RowStr(row, 0x0C));
                AscLua::lua_pushinteger(L, rank);
                AscLua::lua_pushinteger(L, c);
                AscLua::lua_pushinteger(L, b);
                AscLua::lua_pushboolean(L, profession);
                PushStr(L, RowStr(row, 0x10));
                return 6;
            }
        }
        PushStr(L, RowStr(row, 0x0C));
        PushNils(L, 3);
        AscLua::lua_pushboolean(L, profession);
        PushStr(L, RowStr(row, 0x10));
        return 6;
    }

    // WorldMapArea by AreaName (+0x0C); one string argument; an empty string finds nothing.
    int GetWorldMapAreaID(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isstring(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string name = CheckString(L, 1);
        if (!name.empty())
            if (const uint8_t* row = FindByName(kWorldMapAreaDbc, 0x0C, name))
            {
                AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row));
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // The first primary-stat marker aura the unit has (0x7282A0 on the unit) -> the stat number the original
    // pairs with it; nil when none.
    int GetUnitPrimaryStat(lua_State* L)
    {
        uint64_t guid = 0;
        if (UnitTokenGuid(CheckString(L, 1), guid))
            if (uint8_t* unit = static_cast<uint8_t*>(ObjectPtr(guid, 0x10)))
            {
                typedef int(__thiscall* HasAura_t)(void*, uint32_t);
                const HasAura_t hasAura = reinterpret_cast<HasAura_t>(0x7282A0);
                static const struct { uint32_t spell; int stat; } kMarkers[] = {
                    {0xE913F, 1}, {0xE9140, 2}, {0xE9141, 3}, {0xE9142, 4}, {0xE914B, 6}};
                for (const auto& m : kMarkers)
                    if (hasAura(unit, m.spell))
                    {
                        AscLua::lua_pushinteger(L, m.stat);
                        return 1;
                    }
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // Inbox item n (count 0xBEB1D8, items *0xBEB204): the invoice string at +0x114 parsed as
    // "%llX:%u:%u:%u:%u:%u:%u:%u" or "%llX:%u:%u:%u:%u:%u"; the quantity is its last field.
    int GetInboxInvoiceQuantity(lua_State* L)
    {
        const uint32_t n = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1)));
        if (n == 0 || *reinterpret_cast<const uint32_t*>(0xBEB1D8) < n)
            return 0;
        const char* s = reinterpret_cast<const char*>(
            (*reinterpret_cast<uint8_t* const* const*>(0xBEB204))[n - 1] + 0x114);
        unsigned long long guid;
        unsigned a, b, c, d, e, f, qty;
        if (sscanf(s, "%llX:%u:%u:%u:%u:%u:%u:%u", &guid, &a, &b, &c, &d, &e, &f, &qty) != 8
            && sscanf(s, "%llX:%u:%u:%u:%u:%u", &guid, &a, &b, &c, &d, &qty) != 6)
            return 0;
        AscLua::lua_pushinteger(L, static_cast<int>(qty));
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetClassInfoByName", GetClassInfoByName},
        {nullptr, "GetInboxInvoiceQuantity", GetInboxInvoiceQuantity},
        {nullptr, "GetRaceInfo", GetRaceInfo},
        {nullptr, "GetSkillInfo", GetSkillInfo},
        {nullptr, "GetUnitPrimaryStat", GetUnitPrimaryStat},
        {nullptr, "GetWorldMapAreaID", GetWorldMapAreaID},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
