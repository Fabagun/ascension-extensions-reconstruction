// C_CollectorCache over the DLL's four CollectorCache DBCs (FUN_101942*..101948*), patched at runtime by
// SMSG_PATCH_COLLECTOR_CACHE_* 0x6D9..0x6DC (AscDbcPatch). GetCollectorCacheRarityRatesInfo is in
// AscBindings.cpp (it reads its argument and returns nothing).
//   CollectorCacheItems      +4 type, +8, +0xC, +0x10 float, +0x14..+0x60 20 values
//   CollectorCacheTypes      +4, +8, +0xC/+0x10/+0x14 strings
//   CollectorCacheRarityTypes +4 flag, +8 string
//   CollectorCacheRarityRates +8 (1 = the row GetCollectorCacheItemInfo reports), +0xC float
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscScript.hpp>
#include <vector>

using namespace AscScript;

namespace
{
    AscDbc::Table& Items()       { return AscDbc::Get("DBFilesClient\\CollectorCacheItems.dbc"); }
    AscDbc::Table& Types()       { return AscDbc::Get("DBFilesClient\\CollectorCacheTypes.dbc"); }
    AscDbc::Table& RarityTypes() { return AscDbc::Get("DBFilesClient\\CollectorCacheRarityTypes.dbc"); }
    AscDbc::Table& RarityRates() { return AscDbc::Get("DBFilesClient\\CollectorCacheRarityRates.dbc"); }

    void PushDbcStr(lua_State* L, AscDbc::Table& t, const uint8_t* row, uint32_t off)
    {
        const char* s = t.Str(row, off);
        PushStr(L, s);
    }

    // Four values, then the first RarityRates row with +8 == 1 -- and 5 returned either way, so with
    // no such row Lua's fifth value is the slot below (the argument).
    int GetCollectorCacheItemInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = Items().Row(id);
        if (!row)
            return 0;
        AscLua::lua_pushinteger(L, AscDbc::Table::I32(row, 8));
        AscLua::lua_pushinteger(L, AscDbc::Table::I32(row, 0xC));
        AscLua::lua_pushnumber(L, static_cast<double>(AscDbc::Table::F32(row, 0x10)));
        AscLua::lua_createtable(L, 0, 0x14);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < 0x14; ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, AscDbc::Table::I32(row, 0x14 + i * 4));
            AscLua::lua_settable(L, -3);
        }
        AscDbc::Table& rates = RarityRates();
        for (uint32_t r = rates.MinId(); r <= rates.MaxId(); ++r)
        {
            const uint8_t* rate = rates.Row(r);
            if (rate && AscDbc::Table::U32(rate, 8) == 1)
            {
                AscLua::lua_pushnumber(L, static_cast<double>(AscDbc::Table::F32(rate, 0xC)));
                break;
            }
        }
        return 5;
    }

    std::vector<uint32_t> ItemsOfType(uint32_t type)
    {
        std::vector<uint32_t> ids;
        AscDbc::Table& t = Items();
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (row && AscDbc::Table::U32(row, 4) == type)
                ids.push_back(id);
        }
        return ids;
    }

    int GetCollectorCacheItems(lua_State* L)
    {
        uint32_t type;
        if (!ReadNumber(L, type))
            return 0;
        const std::vector<uint32_t> ids = ItemsOfType(type);
        AscLua::lua_createtable(L, 0, static_cast<int>(ids.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < ids.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(ids[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GetNumCollectorCacheItems(lua_State* L)
    {
        uint32_t type;
        if (!ReadNumber(L, type))
            return 0;
        AscLua::lua_pushinteger(L, static_cast<int>(ItemsOfType(type).size()));
        return 1;
    }

    int GetCollectorCacheRarityTypeInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = RarityTypes().Row(id);
        if (!row)
            return 0;
        PushBool(L, AscDbc::Table::U32(row, 4) != 0);
        PushDbcStr(L, RarityTypes(), row, 8);
        return 2;
    }

    int GetCollectorCacheTypeInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = Types().Row(id);
        if (!row)
            return 0;
        AscLua::lua_pushinteger(L, AscDbc::Table::I32(row, 4));
        AscLua::lua_pushinteger(L, AscDbc::Table::I32(row, 8));
        PushDbcStr(L, Types(), row, 0xC);
        PushDbcStr(L, Types(), row, 0x10);
        PushDbcStr(L, Types(), row, 0x14);
        return 5;
    }

    int OpenCollectorCache(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Packet p(0x6DF);
        p.U32(id);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_CollectorCache", "GetCollectorCacheItemInfo", GetCollectorCacheItemInfo},
        {"C_CollectorCache", "GetCollectorCacheItems", GetCollectorCacheItems},
        {"C_CollectorCache", "GetCollectorCacheRarityTypeInfo", GetCollectorCacheRarityTypeInfo},
        {"C_CollectorCache", "GetCollectorCacheTypeInfo", GetCollectorCacheTypeInfo},
        {"C_CollectorCache", "GetNumCollectorCacheItems", GetNumCollectorCacheItems},
        {"C_CollectorCache", "OpenCollectorCache", OpenCollectorCache},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
