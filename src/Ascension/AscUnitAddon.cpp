// Globals that read a unit's addon fields (AscObjectAddon::Unit, FUN_102cdcf0). The unit token is
// resolved with the client's 0x60ABF0; the original reads an uninitialised GUID when that fails
// (GetGloryPoints / UnitAverageItemLevel), GUID 0 here.
//   field 0  alternative power type (SpellAlternativePowerType.dbc id)   field 1  alternative power
//   field 2  prestige level          field 3  prestige (second value)    field 5  average item level (float)
//   field 35 glory points
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscScript.hpp>
#include <string>

using namespace AscScript;

namespace
{
    std::vector<AscObjectAddon::Field>& UnitFields(const char* token)
    {
        uint64_t guid = 0;
        UnitTokenGuid(token, guid);
        return AscObjectAddon::Unit(guid);
    }

    // FUN_100d9a30
    int GetGloryPoints(lua_State* L)
    {
        std::string token;
        double v = 0.0;
        if (ReadString(L, token))
            v = static_cast<int32_t>(UnitFields(token.c_str())[35].value);
        PushNum(L, v);
        return 1;
    }

    // FUN_100dde70
    int UnitAverageItemLevel(lua_State* L)
    {
        std::string token;
        double v = 0.0;
        if (ReadString(L, token))
            v = AscObjectAddon::AsFloat(UnitFields(token.c_str())[5]);
        PushNum(L, v);
        return 1;
    }

    // FUN_10321c30 / d30 / e60: lua_isstring gate, then nothing at all for an unknown token.
    bool AltPowerUnit(lua_State* L, uint64_t& guid)
    {
        if (!AscLua::lua_isstring(L, 1))
            return false;
        const std::string token = CheckString(L, 1);
        return UnitTokenGuid(token.c_str(), guid);
    }

    int UnitAlternativePower(lua_State* L)
    {
        uint64_t guid;
        if (!AltPowerUnit(L, guid))
            return 0;
        PushInt(L, static_cast<int32_t>(AscObjectAddon::Unit(guid)[1].value));
        return 1;
    }

    int UnitAlternativePowerMax(lua_State* L)
    {
        uint64_t guid;
        if (!AltPowerUnit(L, guid))
            return 0;
        const uint32_t type = AscObjectAddon::Unit(guid)[0].value;
        int32_t max = 0;
        if (type != 0)
            if (const uint8_t* row = AscDbc::Get("DBFilesClient\\SpellAlternativePowerType.dbc").Row(type))
                max = AscDbc::Table::I32(row, 8);
        PushInt(L, max);
        return 1;
    }

    int UnitAlternativePowerType(lua_State* L)
    {
        uint64_t guid;
        if (!AltPowerUnit(L, guid))
            return 0;
        const int32_t type = static_cast<int32_t>(AscObjectAddon::Unit(guid)[0].value);
        if (type == 0)
            AscLua::lua_pushnil(L);
        else
            PushInt(L, type);
        return 1;
    }

    // handler_GetPrestigeLevel: the active player's fields 2 and 3.
    int GetPrestigeLevel(lua_State* L)
    {
        std::vector<AscObjectAddon::Field>& f = AscObjectAddon::Unit(ActivePlayerGuid());
        PushNum(L, static_cast<int32_t>(f[2].value));
        PushNum(L, static_cast<int32_t>(AscObjectAddon::Unit(ActivePlayerGuid())[3].value));
        return 2;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetGloryPoints", GetGloryPoints},
        {nullptr, "UnitAverageItemLevel", UnitAverageItemLevel},
        {nullptr, "UnitAlternativePower", UnitAlternativePower},
        {nullptr, "UnitAlternativePowerMax", UnitAlternativePowerMax},
        {nullptr, "UnitAlternativePowerType", UnitAlternativePowerType},
        {nullptr, "GetPrestigeLevel", GetPrestigeLevel},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), nullptr);
}
