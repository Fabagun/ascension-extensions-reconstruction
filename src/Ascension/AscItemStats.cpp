// Scaled item stats -- the itemstatcache.wdb store (static 0x10BDF360, FUN_1006dcf0) and the four globals over it
// and the item tooltip context.
//
// Store: an unordered_map keyed by the u64 {item, level}, each node's 0xA8-byte record at +0x10, pending
// callbacks at +0xC0 and a loaded flag at +0xC8.
//   FUN_10280330 Get(item, level): 0/0 -> nothing. Unknown -> an entry with the load callback queued and
//     CMSG 0x6FF {u64 key}; nothing. Loaded -> the record. Pending -> the callback is queued AGAIN (so the
//     events fire once per miss) and nothing.
//   SMSG 0x700 (LAB_101b0350 / 0x101AF5B0): the 0xA8-byte record, read field by field in order (u64 key,
//     u32, u32, 0x50 bytes, 0x18 bytes, 12 u32 -- i.e. the record verbatim); only for a requested key. The
//     entry becomes loaded and its callbacks run (item, level, arg), then the list is emptied.
//   The load callback (FUN_10280500) signals GET_ITEM_INFO_RECEIVED (%u%b: item, true) -- an event the
//     original never registers, so it is dropped as it is there -- then AUCTION_ITEM_LIST_UPDATE,
//     AUCTION_BIDDER_LIST_UPDATE and AUCTION_OWNER_LIST_UPDATE.
// Record: +0x00 item, +0x04 level, +0x08, +0x0C, +0x10 10 x {stat type, value}, +0x60 2 x {min f, max f,
//   +8}, +0x78 armor, +0x7C armor (reborn), +0x80..+0x94 holy/fire/nature/frost/shadow/arcane res, +0x98
//   block, +0x9C random property, +0xA0 required level, +0xA4 sell price.
//
// Tooltip context: SetInspectedSlot writes the original's tooltip state (0x10BE2F08 inspected slot), which
// the tooltip hooks (AscTooltipScaling.cpp, installer FUN_1027fb60) fill along with the item GUID 0x10BE2EB8
// and FUN_1027eee0 reads to pick the scaling level. SMSG_LOOT_ROLL_ADDON 0x73F's own state -- u32 per client
// loot-roll index (*0xCA0690) in the vector 0x10BE2F74 -- is kept here for them.
#include <Ascension/AscItemStats.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscJson.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace AscScript;

namespace
{
    struct Record { uint8_t bytes[0xA8]; };
    typedef void(__cdecl* Callback)(uint32_t item, uint32_t level, void* arg);
    struct Entry
    {
        Record record{};
        std::vector<std::pair<Callback, void*>> waiting;   // +0xC0
        bool loaded = false;                               // +0xC8
    };
    std::unordered_map<uint64_t, Entry> g_stats;

    using AscItemStats::g_inspectedSlot;
    using AscItemStats::g_lootRollLevels;

    uint32_t U32(const Record& r, uint32_t off) { uint32_t v; memcpy(&v, r.bytes + off, 4); return v; }
    int32_t I32(const Record& r, uint32_t off) { int32_t v; memcpy(&v, r.bytes + off, 4); return v; }
    float F32(const Record& r, uint32_t off) { float v; memcpy(&v, r.bytes + off, 4); return v; }

    void __cdecl OnLoaded(uint32_t item, uint32_t, void*)   // FUN_10280500
    {
        AscRuntime::Signal("GET_ITEM_INFO_RECEIVED", "%u%b", item, 1);
        AscRuntime::Signal("AUCTION_ITEM_LIST_UPDATE");
        AscRuntime::Signal("AUCTION_BIDDER_LIST_UPDATE");
        AscRuntime::Signal("AUCTION_OWNER_LIST_UPDATE");
    }

    const Record* Get(uint32_t item, uint32_t level)   // FUN_10280330
    {
        if (!item && !level)
            return nullptr;
        const uint64_t key = static_cast<uint64_t>(level) << 32 | item;
        auto it = g_stats.find(key);
        if (it == g_stats.end())
        {
            g_stats[key].waiting.emplace_back(&OnLoaded, nullptr);
            AscScript::Packet(0x6FF).U64(key).Send();
            return nullptr;
        }
        if (it->second.loaded)
            return &it->second.record;
        it->second.waiting.emplace_back(&OnLoaded, nullptr);
        return nullptr;
    }

    void __cdecl OnItemStats(void*, uint32_t, uint32_t, CDataStore* p)   // SMSG 0x700
    {
        if (p->m_read + static_cast<int32_t>(sizeof(Record)) > p->m_size)
            return;
        Record r;
        memcpy(r.bytes, p->m_buffer + p->m_read, sizeof(Record));
        p->m_read += sizeof(Record);
        uint64_t key;
        memcpy(&key, r.bytes, 8);
        auto it = g_stats.find(key);
        if (it == g_stats.end())
            return;
        Entry& e = it->second;
        e.record = r;
        e.loaded = true;
        const auto waiting = std::move(e.waiting);
        e.waiting.clear();
        for (const auto& w : waiting)
            w.first(static_cast<uint32_t>(key), static_cast<uint32_t>(key >> 32), w.second);
    }

    void __cdecl OnLootRollAddon(void*, uint32_t, uint32_t, CDataStore* p)   // handler_0x073f
    {
        const uint32_t roll = *reinterpret_cast<const uint32_t*>(0xCA0690);   // *DAT_10bcb88c
        if (g_lootRollLevels.size() <= roll)
            g_lootRollLevels.resize(roll + 1, 0);
        int32_t level = 0;
        CDataStore::GetInt32(p, &level);
        g_lootRollLevels[roll] = static_cast<uint32_t>(level);
    }

    // ---- bindings -------------------------------------------------------------------------------------------
    // FUN_102816e0(item, level): nil until the record is cached, else FUN_102817e0's JSON as a table:
    // ItemStatsType0..9 / ItemStatsValue0..9, ItemDamageMin0..1 / ItemDamageMax0..1 (floats), ItemArmor,
    // ItemArmorReborn, ItemHolyRes, ItemFireRes, ItemNatureRes, ItemFrostRes, ItemShadowRes, ItemArcaneRes,
    // ItemBlock, RandomProperty, RequiredLevel, SellPrice.
    int GetScalingItemStats(lua_State* L)
    {
        const uint32_t item = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint32_t level = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const Record* r = Get(item, level);
        if (!r)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        nlohmann::json j = nlohmann::json::object();
        for (uint32_t i = 0; i < 10; ++i)
        {
            j["ItemStatsType" + std::to_string(i)] = static_cast<int64_t>(I32(*r, 0x10 + i * 8));
            j["ItemStatsValue" + std::to_string(i)] = static_cast<int64_t>(I32(*r, 0x14 + i * 8));
        }
        for (uint32_t i = 0; i < 2; ++i)
        {
            j["ItemDamageMin" + std::to_string(i)] = static_cast<double>(F32(*r, 0x60 + i * 0xC));
            j["ItemDamageMax" + std::to_string(i)] = static_cast<double>(F32(*r, 0x64 + i * 0xC));
        }
        static const char* const kInts[] = {"ItemArmor", "ItemArmorReborn", "ItemHolyRes", "ItemFireRes", "ItemNatureRes",
                                            "ItemFrostRes", "ItemShadowRes", "ItemArcaneRes"};
        for (uint32_t i = 0; i < 8; ++i)
            j[kInts[i]] = static_cast<int64_t>(I32(*r, 0x78 + i * 4));
        j["ItemBlock"] = static_cast<uint64_t>(U32(*r, 0x98));
        j["RandomProperty"] = static_cast<int64_t>(I32(*r, 0x9C));
        j["RequiredLevel"] = static_cast<uint64_t>(U32(*r, 0xA0));
        j["SellPrice"] = static_cast<uint64_t>(U32(*r, 0xA4));
        AscJson::Push(L, j);
        return 1;
    }

    // FUN_10281500(list, index): "bidder" 0xC0F460, "owner" 0xC0F450, "list" 0xC0F440 (+4 count, +8 entries of
    // 0xA0: +8 item, +0x70 level). The scaled record's required level, else the item cache's (+0x38), else
    // nil. The bound check lets index == count + 1 through, as the original's does.
    int GetAuctionItemRequiredLevel(lua_State* L)
    {
        const std::string type = CheckString(L, 1);
        const int32_t index = ToInt(CheckNumber(L, 2));
        uint32_t list = 0;
        if (type == "bidder")
            list = 0xC0F460;
        else if (type == "owner")
            list = 0xC0F450;
        else if (type == "list")
            list = 0xC0F440;
        if (list)
        {
            const uint32_t count = *reinterpret_cast<const uint32_t*>(list + 4);
            const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(list + 8);
            if (entries && count && static_cast<uint32_t>(index - 1) < count + 1)
            {
                const uint8_t* e = entries + (index - 1) * 0xA0;
                const uint32_t item = *reinterpret_cast<const uint32_t*>(e + 8);
                if (const Record* r = Get(item, *reinterpret_cast<const uint32_t*>(e + 0x70)))
                {
                    PushInt(L, I32(*r, 0xA0));
                    return 1;
                }
                typedef const uint8_t*(__thiscall * CacheGet_t)(void*, uint32_t, int, int, int, int);
                if (const uint8_t* rec = reinterpret_cast<CacheGet_t>(0x67CA30)(reinterpret_cast<void*>(0xC5D828), item, 0, 0, 0, 0))
                {
                    PushInt(L, *reinterpret_cast<const int32_t*>(rec + 0x38));
                    return 1;
                }
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_102817a0(slot): 1..20 -> 0x10BE2F08 = slot - 1.
    int SetInspectedSlot(lua_State* L)
    {
        const uint32_t slot = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (slot - 1 < 0x14)
            g_inspectedSlot = slot - 1;
        return 0;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x700, CNetClientCustomPacket((void*)&OnItemStats, nullptr));
        sDC.AddPacketHandler(0x73F, CNetClientCustomPacket((void*)&OnLootRollAddon, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetScalingItemStats", GetScalingItemStats},
        {nullptr, "GetAuctionItemRequiredLevel", GetAuctionItemRequiredLevel},
        {nullptr, "SetInspectedSlot", SetInspectedSlot},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

namespace AscItemStats
{
    uint64_t g_tooltipGuid = 0;
    uint32_t g_inspectedSlot = 0;
    std::vector<uint32_t> g_lootRollLevels;

    const uint8_t* Get(uint32_t item, uint32_t level)
    {
        const Record* r = ::Get(item, level);
        return r ? r->bytes : nullptr;
    }
}

void AscItemStats::ClearStore() { g_stats.clear(); }

namespace
{
    uint32_t g_version = 0;   // 0x10BDF384
}
uint32_t& AscItemStats::Version() { return g_version; }
size_t AscItemStats::Count() { return g_stats.size(); }
void AscItemStats::ForEachRecord(void (*cb)(const uint8_t*, void*), void* ctx)
{
    for (auto& kv : g_stats)
        cb(kv.second.record.bytes, ctx);
}
void AscItemStats::InsertLoaded(const uint8_t* record)   // FUN_101aee00 + FUN_101af410
{
    uint64_t key;
    memcpy(&key, record, 8);
    Entry& e = g_stats[key];
    memcpy(e.record.bytes, record, sizeof(e.record.bytes));
    e.loaded = true;
}
void AscItemStats::PruneUnloaded()
{
    for (auto it = g_stats.begin(); it != g_stats.end();)
        it = it->second.loaded ? std::next(it) : g_stats.erase(it);
}
