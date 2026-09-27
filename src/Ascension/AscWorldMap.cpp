// C_WorldMap (handler_GetMapFileByAreaID .. handler_GetZoneIDByAreaID), over the client's own
// WorldMapArea (container 0xAD4EBC: +0 id, +4 map, +8 area, +0xC file name, +0x10/+0x14/+0x18/+0x1C
// bounds) and AreaTable (container 0xAD3134: +4 map). "AreaID" arguments are WorldMapArea ids + 1.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <algorithm>

using namespace AscScript;

namespace
{
    const uint32_t kWorldMapArea = 0xAD4EBC;
    const uint32_t kAreaTable = 0xAD3134;

    uint32_t U32(const uint8_t* row, uint32_t off) { return *reinterpret_cast<const uint32_t*>(row + off); }
    float F32(const uint8_t* row, uint32_t off) { return *reinterpret_cast<const float*>(row + off); }

    const uint8_t* MapArea(uint32_t areaId)   // id - 1, and the row's own id must match
    {
        const uint8_t* row = ClientDbcRow(kWorldMapArea, areaId - 1);
        return (row && U32(row, 0) == areaId - 1) ? row : nullptr;
    }

    int GetMapFileByAreaID(lua_State* L)
    {
        uint32_t area;
        if (!ReadNumber(L, area) || area == 0)
            return 0;
        if (const uint8_t* row = MapArea(area))
            PushStr(L, *reinterpret_cast<const char* const*>(row + 0xC));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // First WorldMapArea (in id order) whose area column is the zone.
    int GetMapFileByZoneID(lua_State* L)
    {
        uint32_t zone;
        if (!ReadNumber(L, zone))
            return 0;
        const uint32_t min = *reinterpret_cast<const uint32_t*>(kWorldMapArea + 0x10);
        for (uint32_t id = min; id <= *reinterpret_cast<const uint32_t*>(kWorldMapArea + 0x0C); ++id)
            if (const uint8_t* row = ClientDbcRow(kWorldMapArea, id))
                if (U32(row, 8) == zone)
                {
                    PushStr(L, *reinterpret_cast<const char* const*>(row + 0xC));
                    return 1;
                }
        AscLua::lua_pushnil(L);
        return 1;
    }

    int GetMapIDByAreaID(lua_State* L)
    {
        uint32_t area;
        if (!ReadNumber(L, area) || area == 0)
            return 0;
        const uint8_t* row = ClientDbcRow(kWorldMapArea, area - 1);
        if (!row)
            return 0;
        PushInt(L, static_cast<int32_t>(U32(row, 4)));
        return 1;
    }

    int GetMapIDByZoneID(lua_State* L)
    {
        uint32_t zone;
        if (!ReadNumber(L, zone))
            return 0;
        const uint8_t* row = ClientDbcRow(kAreaTable, zone);
        if (!row)
            return 0;
        PushInt(L, static_cast<int32_t>(U32(row, 4)));
        return 1;
    }

    int GetZoneIDByAreaID(lua_State* L)
    {
        uint32_t area;
        if (!ReadNumber(L, area) || area == 0)
            return 0;
        if (const uint8_t* row = MapArea(area))
            PushInt(L, static_cast<int32_t>(U32(row, 8)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_10a3a490: the client's world -> map projection 0x544140(pos, area, &x, &y, 0, 0, false).
    int GetMapPosition(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, NUMBER, NUMBER}))
            return 0;
        const int32_t area = ToInt(CheckNumber(L, 1));
        const float pos[3] = {static_cast<float>(CheckNumber(L, 2)), static_cast<float>(CheckNumber(L, 3)),
                              static_cast<float>(CheckNumber(L, 4))};
        float x = 0.0f, y = 0.0f;
        reinterpret_cast<void(__cdecl*)(const float*, int32_t, float*, float*, int, int, int)>(0x544140)(
            pos, area, &x, &y, 0, 0, 0);
        PushNum(L, x);
        PushNum(L, y);
        return 2;
    }

    // FUN_10a3a5f0: map (x, y) clamped to [0, 1] -> world, returned as (y-axis, x-axis).
    int GetWorldPosition(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, NUMBER}))
            return 0;
        const int32_t area = ToInt(CheckNumber(L, 1));
        const float mx = std::min(std::max(static_cast<float>(CheckNumber(L, 2)), 0.0f), 1.0f);
        const float my = std::min(std::max(static_cast<float>(CheckNumber(L, 3)), 0.0f), 1.0f);
        const uint8_t* row = ClientDbcRow(kWorldMapArea, static_cast<uint32_t>(area - 1));
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const float wx = (F32(row, 0x14) - F32(row, 0x10)) * mx + F32(row, 0x10);
        PushNum(L, my * (F32(row, 0x1C) - F32(row, 0x18)) + F32(row, 0x18));
        PushNum(L, wx);
        return 2;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_WorldMap", "GetMapFileByAreaID", GetMapFileByAreaID},
        {"C_WorldMap", "GetMapFileByZoneID", GetMapFileByZoneID},
        {"C_WorldMap", "GetMapIDByAreaID", GetMapIDByAreaID},
        {"C_WorldMap", "GetMapIDByZoneID", GetMapIDByZoneID},
        {"C_WorldMap", "GetMapPosition", GetMapPosition},
        {"C_WorldMap", "GetWorldPosition", GetWorldPosition},
        {"C_WorldMap", "GetZoneIDByAreaID", GetZoneIDByAreaID},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), nullptr);
}
