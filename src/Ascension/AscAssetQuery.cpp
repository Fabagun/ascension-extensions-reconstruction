// C_AssetQueryService (FUN_100e3770) and the manager behind it (FUN_100e15d0, static at 0x10BDBB30).
//
// Per kind (item / quest / creature) the manager keeps a request queue (+0x00 / +0x2C / +0x58), a set
// of every id ever queued (+0x0C / +0x38 / +0x64), a per-second batch counter (+0x84 / +0x88 / +0x8C)
// and an in-flight list of {id, time64, succeeded, failed} (+0x98 / +0xA4 / +0xB0).
//
// TryCache*(id) (FUN_100e2d60 / FUN_100e2910 / FUN_100e2a60 with the prefetch flag clear): false if
// the client cache already has the record; true and queued if the id was never seen; otherwise
// false, moving the id to the front of the queue if it is still waiting there.
//
// The tick (FUN_100e1700, run after 0x403340) resets the counters once a second, then for each kind
// with a non-empty queue -- at most 3 batches a second unless C_Realm.IsDevelopment -- moves up to 50
// ids to the in-flight list and sends them as CMSG 0x61B (items) / 0x61C (quests) / 0x61A
// (creatures): u32 count, u32 ids. In-flight ids younger than 31 s that reached the cache fire
// <KIND>_CACHE_REQUEST_SUCCESS("%u"); older ones leave the seen-set and fire _FAILURE.
// Back at the glue screen (FUN_100e1480) everything is cleared.
#include <Ascension/AscAssetQuery.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <algorithm>
#include <ctime>
#include <unordered_set>
#include <vector>

using namespace AscScript;

namespace
{
    typedef void* (__thiscall* CacheGet_t)(void* cache, uint32_t id, void* guid, void* cb, void* arg, int flag);

    struct InFlight { uint32_t id; __time64_t sent; bool succeeded, failed; };

    struct Kind
    {
        uint32_t cache;            // client DBCache: 0xC5D828 item, 0xC5DA48 quest, 0xC5D690 creature
        uint32_t cacheGet;         // 0x67CA30 / 0x67DE90 / 0x67B6A0
        uint32_t opcode;
        const char* success;
        const char* failure;
        std::vector<uint32_t> queue;
        std::unordered_set<uint32_t> seen;
        uint32_t batches = 0;
        std::vector<InFlight> inFlight;

        bool Cached(uint32_t id) const
        {
            return reinterpret_cast<CacheGet_t>(cacheGet)(reinterpret_cast<void*>(cache), id, nullptr, nullptr, nullptr, 0) != nullptr;
        }
    };

    Kind g_item{0xC5D828, 0x67CA30, 0x61B, "ITEM_CACHE_REQUEST_SUCCESS", "ITEM_CACHE_REQUEST_FAILURE"};
    Kind g_quest{0xC5DA48, 0x67DE90, 0x61C, "QUEST_CACHE_REQUEST_SUCCESS", "QUEST_CACHE_REQUEST_FAILURE"};
    Kind g_creature{0xC5D690, 0x67B6A0, 0x61A, "CREATURE_CACHE_REQUEST_SUCCESS", "CREATURE_CACHE_REQUEST_FAILURE"};
    __time64_t g_second = 0;

    bool TryCache(Kind& k, uint32_t id)
    {
        if (k.Cached(id))
            return false;
        if (k.seen.insert(id).second)
        {
            k.queue.push_back(id);
            return true;
        }
        auto it = std::find(k.queue.begin(), k.queue.end(), id);
        if (it != k.queue.end())
        {
            k.queue.erase(it);
            k.queue.insert(k.queue.begin(), id);
        }
        return false;
    }

    void SendBatch(Kind& k)
    {
        if (k.queue.empty() || (!RealmInfoSvc::Get().Dev() && k.batches >= 3))
            return;
        const size_t n = std::min<size_t>(k.queue.size(), 50);
        Packet p(k.opcode);
        p.U32(static_cast<uint32_t>(n));
        for (size_t i = 0; i < n; ++i)
        {
            p.U32(k.queue[i]);
            k.inFlight.push_back({k.queue[i], _time64(nullptr), false, false});
        }
        k.queue.erase(k.queue.begin(), k.queue.begin() + n);
        p.Send();
        ++k.batches;
    }

    void CheckInFlight(Kind& k)
    {
        if (k.inFlight.empty())
            return;
        const __time64_t now = _time64(nullptr);
        for (InFlight& f : k.inFlight)
        {
            if (now - f.sent < 31)
            {
                if (k.Cached(f.id))
                {
                    f.succeeded = true;
                    AscRuntime::Signal(k.success, "%u", f.id);
                }
            }
            else
            {
                k.seen.erase(f.id);
                f.failed = true;
                AscRuntime::Signal(k.failure, "%u", f.id);
            }
        }
        k.inFlight.erase(std::remove_if(k.inFlight.begin(), k.inFlight.end(),
            [](const InFlight& f) { return f.succeeded || f.failed; }), k.inFlight.end());
    }

    void Tick()
    {
        const __time64_t now = _time64(nullptr);
        if (g_second < now)
        {
            g_second = now;
            g_item.batches = g_quest.batches = g_creature.batches = 0;
        }
        SendBatch(g_item);
        SendBatch(g_quest);
        SendBatch(g_creature);
        CheckInFlight(g_item);
        CheckInFlight(g_quest);
        CheckInFlight(g_creature);
    }

    void Reset()
    {
        for (Kind* k : {&g_item, &g_quest, &g_creature})
        {
            k->queue.clear();
            k->seen.clear();
            k->batches = 0;
            k->inFlight.clear();
        }
        g_second = 0;
    }

    int TryCacheLua(lua_State* L, Kind& k, const char* usage)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
        {
            AscLua::luaL_error(L, usage);
            return 0;
        }
        PushBool(L, TryCache(k, id));
        return 1;
    }
    int TryCacheItem(lua_State* L)     { return TryCacheLua(L, g_item, "Usage: TryCacheItem(itemId)"); }
    int TryCacheQuest(lua_State* L)    { return TryCacheLua(L, g_quest, "Usage: TryCacheQuest(questId)"); }
    int TryCacheCreature(lua_State* L) { return TryCacheLua(L, g_creature, "Usage: TryCacheCreature(creatureId)"); }

    void Init()
    {
        AscRuntime::OnAfter403340(&Tick);
        AscRuntime::OnGlueScreen(&Reset);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_AssetQueryService", "TryCacheItem", TryCacheItem},
        {"C_AssetQueryService", "TryCacheQuest", TryCacheQuest},
        {"C_AssetQueryService", "TryCacheCreature", TryCacheCreature},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

namespace AscAssetQuery
{
    bool TryCacheItem(uint32_t id) { return TryCache(g_item, id); }
}
