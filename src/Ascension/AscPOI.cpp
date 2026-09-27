// POIMgr (FUN_102cf0d0 -> static 0x10BE36E8): an unordered_map<string, POI> keyed by the POI's first
// string. MSVC's unordered_map with the default hash (FNV-1a) is what the original uses, so iteration
// order -- which GetPOIInfo(index) exposes -- matches.
//
// Strings on the wire are u32 length + bytes (FUN_100d3680).
//   SMSG_POI_ADD 0x90F (FUN_102cf1f0): str key, str text, u32 a, u32 b, u32 c, f32 d, f32 e -- fires
//     ASCENSION_POI_ADD("%s%s%u%u%u%f%f"), then inserts or overwrites.
//   SMSG_POI_REMOVE 0x910 (FUN_102cf7e0): str key -- ASCENSION_POI_REMOVE("%s"), then erases.
//   SMSG_POI_CLEAR 0x911 (FUN_102cf6a0): ASCENSION_POI_CLEAR, then clears.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <cstring>
#include <string>
#include <unordered_map>

using namespace AscScript;

namespace
{
    struct POI
    {
        std::string key, text;
        uint32_t a = 0, b = 0, c = 0;
        float d = 0, e = 0;
    };
    std::unordered_map<std::string, POI> g_pois;

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

    void __cdecl OnPoiAdd(void*, uint32_t, uint32_t, CDataStore* p)
    {
        POI poi;
        poi.key = ReadLenString(p);
        poi.text = ReadLenString(p);
        poi.a = Read<uint32_t>(p);
        poi.b = Read<uint32_t>(p);
        poi.c = Read<uint32_t>(p);
        poi.d = Read<float>(p);
        poi.e = Read<float>(p);
        AscRuntime::Signal("ASCENSION_POI_ADD", "%s%s%u%u%u%f%f", poi.key.c_str(), poi.text.c_str(),
            poi.a, poi.b, poi.c, static_cast<double>(poi.d), static_cast<double>(poi.e));
        g_pois[poi.key] = poi;
    }

    void __cdecl OnPoiRemove(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string key = ReadLenString(p);
        AscRuntime::Signal("ASCENSION_POI_REMOVE", "%s", key.c_str());
        g_pois.erase(key);
    }

    void __cdecl OnPoiClear(void*, uint32_t, uint32_t, CDataStore*)
    {
        AscRuntime::Signal("ASCENSION_POI_CLEAR");
        g_pois.clear();
    }

    int GetNumActivePOI(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g_pois.size()));
        return 1;
    }

    // FUN_102cfa50: the index-th element in map order (0-based), 7 values; nothing when out of range.
    int GetPOIInfo(lua_State* L)
    {
        const uint32_t index = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        if (index >= g_pois.size())
            return 0;
        auto it = g_pois.begin();
        std::advance(it, index);
        const POI& poi = it->second;
        PushStr(L, poi.key.c_str());
        PushStr(L, poi.text.c_str());
        PushInt(L, static_cast<int32_t>(poi.a));
        PushInt(L, static_cast<int32_t>(poi.b));
        PushInt(L, static_cast<int32_t>(poi.c));
        PushNum(L, poi.d);
        PushNum(L, poi.e);
        return 7;
    }

    // FUN_10a6bbc0: luaL_checknumber(L, 2) written as a float over the client constant at 0xA41E00
    // (unprotected with NtProtectVirtualMemory first). No other argument is examined.
    int SetPOIMaxRange(lua_State* L)
    {
        const float range = static_cast<float>(CheckNumber(L, 2));
        DWORD old;
        VirtualProtect(reinterpret_cast<void*>(0xA41E00), 4, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<float*>(0xA41E00) = range;
        VirtualProtect(reinterpret_cast<void*>(0xA41E00), 4, old, &old);
        return 0;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x90F, CNetClientCustomPacket((void*)&OnPoiAdd, nullptr));
        sDC.AddPacketHandler(0x910, CNetClientCustomPacket((void*)&OnPoiRemove, nullptr));
        sDC.AddPacketHandler(0x911, CNetClientCustomPacket((void*)&OnPoiClear, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetNumActivePOI", GetNumActivePOI},
        {nullptr, "GetPOIInfo", GetPOIInfo},
        {"#Minimap", "SetPOIMaxRange", SetPOIMaxRange},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
