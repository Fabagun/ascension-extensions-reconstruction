// Seasonal collection (SeasonalCollectionMgr, FUN_10310320 -> static 0x10BE3EB8) and custom points
// (AscensionCustomPointsMgr, FUN_100cf7b0 -> static 0x10BDB57C).
//
// SMSG_SEASONAL_COLLECTION_INFO 0x9BE (FUN_10310430): u32 n x 8-byte achievement {i32 id, u8 node,
//   u8 chapter, u8 sortOrder, u8 isReward} APPENDED to the list (never cleared), then twice
//   {str icon, str name, str description} (u32 length strings) into slots 0 and 1.
// SMSG_SEASONAL_COLLECTION_UNLOCKED 0x9BF (handler_0x09bf): sets the unlocked flag (+0xA0).
// RequestDeliverSeasonalCollectionItem(id) (FUN_100cf8d0): CMSG 0x523 {u8 1, u32 id}.
//
// Custom points 0x905 (FUN_100cfb40): u8 initial, u8 type, i32 value. Initial packets only insert
//   (an existing value is kept) and fire nothing; others store the value and fire the type's
//   ASCENSION_CUSTOM_POINTS_*_VALUE_CHANGED("%u%u", old, new) -- old is 0 for a new type.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadLenString(CDataStore* p)
    {
        const uint32_t len = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), len);
        p->m_read += static_cast<int32_t>(len);
        return s;
    }

    // ---- seasonal collection ----
    struct Achievement { int32_t id; uint8_t node, chapter, sortOrder, isReward; };
    static_assert(sizeof(Achievement) == 8, "seasonal achievement");
    struct
    {
        std::vector<Achievement> achievements;
        std::string icons[2], names[2], descriptions[2];
        bool unlocked = false;
    } g_seasonal;

    void __cdecl OnSeasonalInfo(void*, uint32_t, uint32_t, CDataStore* p)
    {
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_seasonal.achievements.push_back(Read<Achievement>(p));
        for (int i = 0; i < 2; ++i)
        {
            g_seasonal.icons[i] = ReadLenString(p);
            g_seasonal.names[i] = ReadLenString(p);
            g_seasonal.descriptions[i] = ReadLenString(p);
        }
    }

    void __cdecl OnSeasonalUnlocked(void*, uint32_t, uint32_t, CDataStore*)
    {
        g_seasonal.unlocked = true;
    }

    void SetNum(lua_State* L, const char* k, double v)
    {
        AscLua::lua_pushstring(L, k);
        PushNum(L, v);
        AscLua::lua_settable(L, -3);
    }

    void SetPair(lua_State* L, const char* k, const std::string (&v)[2])
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (int i = 0; i < 2; ++i)
        {
            PushInt(L, i + 1);
            PushStr(L, v[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    // FUN_10310e60
    int GetSeasonalCollectionInfo(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "achievements");
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        int i = 0;
        for (const Achievement& a : g_seasonal.achievements)
        {
            PushNum(L, ++i);
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetNum(L, "id", a.id);
            SetNum(L, "node", a.node);
            SetNum(L, "chapter", a.chapter);
            SetNum(L, "sortOrder", a.sortOrder);
            AscLua::lua_pushstring(L, "isReward");
            PushBool(L, a.isReward != 0);
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        SetPair(L, "icons", g_seasonal.icons);
        SetPair(L, "names", g_seasonal.names);
        SetPair(L, "descriptions", g_seasonal.descriptions);
        return 1;
    }

    int IsSeasonalCollectionUnlocked(lua_State* L)
    {
        PushBool(L, g_seasonal.unlocked);
        return 1;
    }

    int RequestDeliverSeasonalCollectionItem(lua_State* L)
    {
        Packet p(0x523);
        p.U8(1);
        p.U32(static_cast<uint32_t>(ToInt(CheckNumber(L, 1))));
        p.Send();
        return 0;
    }

    // ---- custom points ----
    std::unordered_map<uint8_t, int32_t> g_points;

    const char* PointsEvent(uint8_t type)   // FUN_100cf550
    {
        switch (type)
        {
        case 0: return "ASCENSION_CUSTOM_POINTS_UNKNOWN_VALUE_CHANGED";
        case 1: return "ASCENSION_CUSTOM_POINTS_SEASONAL_POINTS_VALUE_CHANGED";
        case 2: return "ASCENSION_CUSTOM_POINTS_DONATION_POINTS_VALUE_CHANGED";
        case 3: return "ASCENSION_CUSTOM_POINTS_BAZAAR_TOKENS_VALUE_CHANGED";
        case 4: return "ASCENSION_CUSTOM_POINTS_REDEMPTION_POINTS_VALUE_CHANGED";
        default: return "ASCENSION_CUSTOM_POINTS_UNHANDLED_VALUE_CHANGED";
        }
    }

    void __cdecl OnCustomPoints(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const bool initial = Read<uint8_t>(p) != 0;
        const uint8_t type = Read<uint8_t>(p);
        const int32_t value = Read<int32_t>(p);
        if (initial)
        {
            g_points.emplace(type, value);
            return;
        }
        int32_t old = 0;
        const auto it = g_points.find(type);
        if (it == g_points.end())
            g_points.emplace(type, value);
        else
        {
            old = it->second;
            it->second = value;
        }
        AscRuntime::Signal(PointsEvent(type), "%u%u", old, value);
    }

    int32_t Points(uint8_t type)   // FUN_100cfad0
    {
        const auto it = g_points.find(type);
        return it != g_points.end() ? it->second : 0;
    }

    int GetAscensionSeasonalPoints(lua_State* L) { PushInt(L, Points(1)); return 1; }
    int GetAscensionDonationPoints(lua_State* L) { PushInt(L, Points(2)); return 1; }

    // FUN_100cf600: the type is the low byte of the truncated number.
    int GetCustomAscensionPointValue(lua_State* L)
    {
        PushInt(L, Points(static_cast<uint8_t>(ToInt(CheckNumber(L, 1)))));
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x9BE, CNetClientCustomPacket((void*)&OnSeasonalInfo, nullptr));
        sDC.AddPacketHandler(0x9BF, CNetClientCustomPacket((void*)&OnSeasonalUnlocked, nullptr));
        sDC.AddPacketHandler(0x905, CNetClientCustomPacket((void*)&OnCustomPoints, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetSeasonalCollectionInfo", GetSeasonalCollectionInfo},
        {nullptr, "IsSeasonalCollectionUnlocked", IsSeasonalCollectionUnlocked},
        {nullptr, "RequestDeliverSeasonalCollectionItem", RequestDeliverSeasonalCollectionItem},
        {nullptr, "GetAscensionSeasonalPoints", GetAscensionSeasonalPoints},
        {nullptr, "GetAscensionDonationPoints", GetAscensionDonationPoints},
        {nullptr, "GetCustomAscensionPointValue", GetCustomAscensionPointValue},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
