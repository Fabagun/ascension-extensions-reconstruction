// C_CallboardCache: GetCallboardCacheInfo / GetCurrentPoints / GetItemLevelInfo (FUN_10116270 /
// FUN_101162f0 / FUN_10116370), all over FUN_10115df0.
//
// SMSG 0x730 (handler_0x0730 / FUN_10115400) replaces the level table (static 0x10BDD1C4): u32 n,
// then n raw 0x18-byte entries -- +8 gating game event (0 = always), +0xC level threshold, +0x10
// info value, +0x14 points.
//
// The current cache level is the client's own progress counter for criteria 0x13901 (client DBC
// container 0xAD3080; 0x5B2350 walks the progress list at 0xACFDA0 by id; value at node +0x10).
// FUN_10115df0 walks the eligible entries (event 0 or active): the first whose threshold <= level
// is "current", the entry after it "next"; while none is current, each entry above the level is
// "upcoming". Upcoming/next are dropped unless they share the current entry's event.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscQuestAddon.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscToken.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <vector>

using namespace AscScript;

namespace
{
    struct Entry { uint32_t a, b, event, threshold, info, points; };
    static_assert(sizeof(Entry) == 0x18, "0x730 entry");
    std::vector<Entry> g_levels;

    void __cdecl OnCacheLevels(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t n;
        memcpy(&n, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        std::vector<Entry> levels;
        for (; n; --n)
        {
            Entry e;
            memcpy(&e, p->m_buffer + p->m_read, sizeof(e));
            p->m_read += sizeof(e);
            levels.push_back(e);
        }
        g_levels.swap(levels);
    }

    uint32_t CacheLevel()
    {
        const uint8_t* row = ClientDbcRow(0xAD3080, 0x13901);
        if (!row)
            return 0;
        const uint8_t* node = static_cast<const uint8_t*>(
            reinterpret_cast<void*(__fastcall*)(const void*, uint32_t)>(0x5B2350)(row, 0x13901));
        return node ? *reinterpret_cast<const uint32_t*>(node + 0x10) : 0;
    }

    struct Levels { uint32_t level; const Entry* next; const Entry* current; const Entry* upcoming; };

    Levels Resolve()
    {
        Levels r{CacheLevel(), nullptr, nullptr, nullptr};
        for (const Entry& e : g_levels)
        {
            if (e.event != 0 && !AscGameEvents::IsActive(static_cast<uint16_t>(e.event)))
                continue;
            if (r.current)
            {
                r.next = &e;
                break;
            }
            if (r.level < e.threshold)
                r.upcoming = &e;
            else
                r.current = &e;
        }
        if (r.upcoming && (!r.current || r.upcoming->event != r.current->event))
            r.upcoming = nullptr;
        if (r.next && r.next->event != r.current->event)
            r.next = nullptr;
        return r;
    }

    void PushOr(lua_State* L, const Entry* e, uint32_t Entry::*field)
    {
        if (e)
            PushInt(L, static_cast<int32_t>(e->*field));
        else
            AscLua::lua_pushnil(L);
    }

    int GetCallboardCacheInfo(lua_State* L)
    {
        const Levels r = Resolve();
        PushOr(L, r.current, &Entry::info);
        PushOr(L, r.upcoming, &Entry::info);
        return 2;
    }

    // Token TOKEN_TYPE_CALLBOARD_CACHE_POINTS (index 0x3C), then the current entry's points.
    int GetCurrentPoints(lua_State* L)
    {
        const int32_t points = AscToken::Amount(0x3C);
        const Levels r = Resolve();
        PushInt(L, points);
        PushOr(L, r.current, &Entry::points);
        return 2;
    }

    int GetItemLevelInfo(lua_State* L)
    {
        const Levels r = Resolve();
        PushInt(L, static_cast<int32_t>(r.level));
        PushOr(L, r.current, &Entry::threshold);
        PushOr(L, r.upcoming, &Entry::threshold);
        return 3;
    }

    // FUN_10116400: (questID) -> the quest addon record's points, or nil while it is being fetched.
    int GetPointsForQuest(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const uint32_t quest = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const AscQuestAddon::Record* r = AscQuestAddon::Get(quest);
        if (!r)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_pushinteger(L, static_cast<int>(r->points));
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x730, CNetClientCustomPacket((void*)&OnCacheLevels, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_CallboardCache", "GetCallboardCacheInfo", GetCallboardCacheInfo},
        {"C_CallboardCache", "GetCurrentPoints", GetCurrentPoints},
        {"C_CallboardCache", "GetItemLevelInfo", GetItemLevelInfo},
        {"C_CallboardCache", "GetPointsForQuest", GetPointsForQuest},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
