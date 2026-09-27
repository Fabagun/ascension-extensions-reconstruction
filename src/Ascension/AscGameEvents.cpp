// Game events: SMSG_GAME_EVENT_INFO 0x9BD (FUN_10256e70) and the globals GetGameEventInfo /
// GetGameEventInfos / IsGameEventActive (FUN_10257110 / FUN_102573a0 / FUN_102576b0).
//
// Record (0x34 bytes, copied from the packet verbatim): +0 u16 eventId, +2 u8 state (GAMEEVENT_*),
// +3 u64 start time, +0xB u64 end time, +0x13 u8 active, +0x14 i32 length. Keyed by the first u16.
// An update that turns a record active fires GAME_EVENT_STARTED("%u", id) unless it was already
// active; one that turns it inactive fires GAME_EVENT_ENDED if it was active before.
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <ctime>
#include <map>
#include <string>

using namespace AscScript;

namespace
{
    struct Record { uint8_t raw[0x34]; };
    std::map<uint16_t, Record> g_events;

    uint16_t Id(const Record& r)       { return *reinterpret_cast<const uint16_t*>(r.raw); }
    uint8_t State(const Record& r)     { return r.raw[2]; }
    uint64_t Start(const Record& r)    { uint64_t v; memcpy(&v, r.raw + 3, 8); return v; }
    uint64_t End(const Record& r)      { uint64_t v; memcpy(&v, r.raw + 0xB, 8); return v; }
    bool Active(const Record& r)       { return r.raw[0x13] != 0; }
    int32_t Length(const Record& r)    { int32_t v; memcpy(&v, r.raw + 0x14, 4); return v; }

    void __cdecl OnGameEventInfo(void*, uint32_t, uint32_t, CDataStore* p)
    {
        Record rec;
        memcpy(rec.raw, p->m_buffer + p->m_read, sizeof(rec.raw));
        p->m_read += sizeof(rec.raw);
        const uint16_t key = Id(rec);
        const auto before = g_events.find(key);
        const bool existed = before != g_events.end();
        const bool wasActive = existed && Active(before->second);
        g_events[key] = rec;
        if (Active(rec))
        {
            if (!wasActive)
                AscRuntime::Signal("GAME_EVENT_STARTED", "%u", static_cast<uint32_t>(key));
        }
        else if (wasActive)
            AscRuntime::Signal("GAME_EVENT_ENDED", "%u", static_cast<uint32_t>(key));
    }

    // FUN_10256ca0: 0..5 -> GAMEEVENT_*, else "UNEXPECTED_ENUM_VALUE_<n>".
    std::string StateName(uint8_t s)
    {
        static const char* const kNames[6] = {"GAMEEVENT_NORMAL", "GAMEEVENT_WORLD_INACTIVE",
            "GAMEEVENT_WORLD_CONDITIONS", "GAMEEVENT_WORLD_NEXTPHASE", "GAMEEVENT_WORLD_FINISHED",
            "GAMEEVENT_INTERNAL"};
        if (s < 6)
            return kNames[s];
        return "UNEXPECTED_ENUM_VALUE_" + std::to_string(s);
    }

    void SetInt(lua_State* L, const char* k, int32_t v)
    {
        AscLua::lua_pushstring(L, k);
        PushInt(L, v);
        AscLua::lua_settable(L, -3);
    }

    // Seconds until start/end (0 once passed or unset); timeSinceStart = length - untilEnd, or 0.
    // GetGameEventInfos pushes eventId as a signed short, GetGameEventInfo as unsigned.
    void PushInfo(lua_State* L, const Record& r, uint64_t now, bool signedId)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "eventId", signedId ? static_cast<int16_t>(Id(r)) : Id(r));
        AscLua::lua_pushstring(L, "active");
        PushBool(L, Active(r));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "eventState");
        PushStr(L, StateName(State(r)).c_str());
        AscLua::lua_settable(L, -3);
        const uint64_t start = Start(r), end = End(r);
        SetInt(L, "startTime", (start == 0 || start < now) ? 0 : static_cast<int32_t>(start - now));
        const uint64_t untilEnd = (end == 0 || end < now) ? 0 : end - now;
        SetInt(L, "endTime", static_cast<int32_t>(untilEnd));
        SetInt(L, "timeSinceStart", untilEnd == 0 ? 0 : Length(r) - static_cast<int32_t>(untilEnd));
    }

    int GetGameEventInfo(lua_State* L)
    {
        const uint16_t id = static_cast<uint16_t>(static_cast<int32_t>(CheckNumber(L, 1)));
        const auto it = g_events.find(id);
        if (it == g_events.end())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushInfo(L, it->second, static_cast<uint64_t>(_time64(nullptr)), false);
        return 1;
    }

    // { [eventId] = info, ... } in key order.
    int GetGameEventInfos(lua_State* L)
    {
        const uint64_t now = static_cast<uint64_t>(_time64(nullptr));
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const auto& e : g_events)
        {
            PushInt(L, e.first);
            PushInfo(L, e.second, now, true);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int IsGameEventActive(lua_State* L)
    {
        const uint16_t id = static_cast<uint16_t>(static_cast<int32_t>(CheckNumber(L, 1)));
        PushBool(L, AscGameEvents::IsActive(id));
        return 1;
    }

    int GetServerMaxLevel(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(AscGameEvents::ServerMaxLevel()));
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x9BD, CNetClientCustomPacket((void*)&OnGameEventInfo, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetServerMaxLevel", GetServerMaxLevel},
        {nullptr, "GetGameEventInfo", GetGameEventInfo},
        {nullptr, "GetGameEventInfos", GetGameEventInfos},
        {nullptr, "IsGameEventActive", IsGameEventActive},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

bool AscGameEvents::IsActive(uint16_t eventId)
{
    const auto it = g_events.find(eventId);
    return it != g_events.end() && Active(it->second);
}

// handler_GetServerMaxLevel -> FUN_102fc4d0: the ruleset cap (realm-object +8: 1 -> 70, 2 -> 80,
// else 60), lowered by whichever level-cap game event is active.
uint32_t AscGameEvents::ServerMaxLevel()
{
    const uint32_t ruleset = RealmInfoSvc::Get().ruleset;
    uint32_t cap = ruleset == 1 ? 70 : ruleset == 2 ? 80 : 60;
    if (IsActive(0x164))
        return cap;
    if (IsActive(0xAD))
        return cap < 70 ? cap : 70;
    if (!IsActive(0x1EA))
    {
        if (IsActive(0x1E9)) return 55;
        if (IsActive(0x1E8)) return 45;
        if (IsActive(0x1E7)) return 35;
        if (IsActive(0x1E6)) return 25;
        if (!IsActive(0x160)) return cap;
    }
    return cap < 60 ? cap : 60;
}
