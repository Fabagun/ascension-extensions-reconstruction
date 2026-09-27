// C_WildcardRewards: Scroll of Fortune rewards (manager FUN_10a38f20 -> static 0x10D3D608), the
// wildcard twin of C_DraftRewards (AscDraftMode.cpp).
//
// SMSG_SCROLL_OF_FORTUNE_REWARDS_LIST 0x6AB (FUN_10a38a30): list and both maps cleared, then a
//   CompressedDataStore: u32 n x 0x31-byte records {u32 tier, u8 flag, u32 level (+5), u32 item[4]
//   (+9), u32 count[4] (+0x19), u32 cost (+0x29), u32 release (+0x2D)}, indexed by tier in the map
//   for their flag (+0x0C when set, +0x2C when clear). Always fires SCROLL_OF_FORTUNE_REWARDS_LIST_UPDATED.
// SMSG_COLLECTED_SCROLL_OF_FORTUNE_REWARDS_LIST 0x6B8 (FUN_10a388a0): claimed list (+0x4C) replaced
//   by u32 n x 12 bytes {u32, u32 claimed (flag), u32 claimed (no flag)}; limits (+0x58) replaced by
//   u32 n x 28 bytes {u32 tier, u32 x6}; fires SCROLL_OF_FORTUNE_LIMITS_UPDATED.
// SMSG_COLLECT_SCROLL_OF_FORTUNE_REWARDS_RESULT 0x6B6 (FUN_10a38630): cstring; on _OK the last claim
//   (+0x64 tier, +0x68 count, +0x6C flag) is added to the claimed list; then
//   COLLECT_SCROLL_OF_FORTUNE_REWARDS_RESULT("%s").
// ClaimScrollOfFortuneRewards sends CMSG 0x6B5 {u32 tier, u32 count, u8 flag}.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
#pragma pack(push, 1)
    struct Reward
    {
        uint32_t tier;
        uint8_t flag;
        uint32_t level;
        uint32_t items[4];
        uint32_t counts[4];
        uint32_t cost;
        uint32_t release;
    };
#pragma pack(pop)
    static_assert(sizeof(Reward) == 0x31, "scroll of fortune reward");

    struct Claimed { uint32_t v[3]; };
    struct Limit { uint32_t tier, v[6]; };

    struct
    {
        std::vector<Reward> rewards;
        std::unordered_map<uint32_t, std::vector<uint32_t>> maps[2];   // [flag] -> tier -> reward indices
        std::vector<Claimed> claimed;
        std::vector<Limit> limits;
        uint32_t lastTier = 0, lastCount = 0;
        bool lastFlag = false;
    } g_sof;

    // FUN_10a38070 (every glue screen): rewards (+0), both tier maps (+0xC / +0x2C), claimed (+0x4C) and
    // limits (+0x58) emptied; the last-claim fields (+0x64 / +0x68 / +0x6C) zeroed.
    void Reset()
    {
        g_sof.rewards.clear();
        g_sof.maps[0].clear();
        g_sof.maps[1].clear();
        g_sof.claimed.clear();
        g_sof.limits.clear();
        g_sof.lastTier = 0;
        g_sof.lastCount = 0;
        g_sof.lastFlag = false;
    }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    void __cdecl OnRewardsList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_sof.rewards.clear();
        g_sof.maps[0].clear();
        g_sof.maps[1].clear();
        std::vector<uint8_t> data;
        if (InflatePacket(p, data) && data.size() >= 4)
        {
            uint32_t n;
            memcpy(&n, data.data(), 4);
            g_sof.rewards.reserve(n);
            size_t at = 4;
            for (uint32_t i = 0; i < n && at + sizeof(Reward) <= data.size(); ++i, at += sizeof(Reward))
            {
                Reward r;
                memcpy(&r, data.data() + at, sizeof(r));
                g_sof.rewards.push_back(r);
                g_sof.maps[r.flag != 0][r.tier].push_back(static_cast<uint32_t>(g_sof.rewards.size() - 1));
            }
        }
        AscRuntime::Signal("SCROLL_OF_FORTUNE_REWARDS_LIST_UPDATED");
    }

    void __cdecl OnClaimedList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_sof.claimed.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_sof.claimed.push_back(Read<Claimed>(p));
        g_sof.limits.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_sof.limits.push_back(Read<Limit>(p));
        AscRuntime::Signal("SCROLL_OF_FORTUNE_LIMITS_UPDATED");
    }

    const char* const kResults[8] = {
        "COLLECT_SCROLL_OF_FORTUNE_REWARDS_OK", "COLLECT_SCROLL_OF_FORTUNE_REWARDS_UNKNOWN",
        "COLLECT_SCROLL_OF_FORTUNE_REWARDS_SPECIALIZATION_OUT_OF_RANGE", "COLLECT_SCROLL_OF_FORTUNE_REWARDS_COUNT_TOO_HIGH",
        "COLLECT_SCROLL_OF_FORTUNE_REWARDS_INVALID_COUNT", "COLLECT_SCROLL_OF_FORTUNE_REWARDS_NOT_ENOUGH_MARKS_OF_ASCENSION",
        "COLLECT_SCROLL_OF_FORTUNE_REWARDS_TOO_LOW_LEVEL", "COLLECT_SCROLL_OF_FORTUNE_REWARDS_NOT_RELEASED",
    };

    void __cdecl OnClaimResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(result.size() + 1);
        if (result == kResults[0])
        {
            if (g_sof.claimed.size() <= g_sof.lastTier)
                g_sof.claimed.resize(g_sof.lastTier + 1, Claimed{});
            g_sof.claimed[g_sof.lastTier].v[g_sof.lastFlag ? 1 : 2] += g_sof.lastCount;
        }
        AscRuntime::Signal("COLLECT_SCROLL_OF_FORTUNE_REWARDS_RESULT", "%s", result.c_str());
    }

    uint32_t ClaimedCount(uint32_t tier, bool flag)
    {
        return tier < g_sof.claimed.size() ? g_sof.claimed[tier].v[flag ? 1 : 2] : 0;
    }

    // FUN_10a38400: consecutive rewards of the tier from the 1-based `start`, stopping at a gap.
    std::vector<const Reward*> NextRewards(uint32_t tier, uint32_t start, uint32_t count, bool flag)
    {
        std::vector<const Reward*> out;
        const auto& m = g_sof.maps[flag];
        const auto it = m.find(tier);
        for (uint32_t i = start; i < start + count; ++i)
        {
            if (i == 0 || it == m.end() || i - 1 >= it->second.size())
                break;
            out.push_back(&g_sof.rewards[it->second[i - 1]]);
        }
        return out;
    }

    // FUN_10a380e0
    std::vector<const Reward*> Totals(uint32_t tier, uint32_t count, bool flag,
                                      std::unordered_map<uint32_t, uint32_t>& items, uint32_t& cost)
    {
        cost = 0;
        const std::vector<const Reward*> recs = NextRewards(tier, ClaimedCount(tier, flag) + 1, count, flag);
        for (const Reward* r : recs)
        {
            cost += r->cost;
            for (int k = 0; k < 4; ++k)
                if (r->items[k] && r->counts[k])
                    items[r->items[k]] += r->counts[k];
        }
        return recs;
    }

    // FUN_10308570, as in AscDraftMode.
    uint32_t CountItem(uint32_t entry)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return 0;
        const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(player + 8);
        if (*reinterpret_cast<const uint64_t*>(desc) != ActivePlayerGuid())
            return 0;
        uint32_t total = 0;
        for (uint32_t i = 0x76; i < 0x96; ++i)
            if (const uint8_t* item = static_cast<const uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(desc + 0x510 + i * 8), 2)))
            {
                const uint8_t* idesc = *reinterpret_cast<uint8_t* const*>(item + 8);
                if (*reinterpret_cast<const uint32_t*>(idesc + 0xC) == entry)
                    total += *reinterpret_cast<const uint32_t*>(idesc + 0x38);
            }
        return total;
    }

    // FUN_10a37eb0
    uint32_t CheckClaim(uint32_t tier, uint32_t count, bool flag)
    {
        if (tier >= g_sof.claimed.size())
            return 2;
        if (count > 100)
            return 3;
        const std::vector<const Reward*> recs = NextRewards(tier, ClaimedCount(tier, flag) + 1, count, flag);
        if (recs.size() != count)
            return 4;
        std::unordered_map<uint32_t, uint32_t> items;
        uint32_t cost;
        Totals(tier, count, flag, items, cost);
        if (CountItem(0x5B9D2) < cost)
            return 5;
        const __time64_t now = _time64(nullptr);
        for (const Reward* r : recs)
        {
            if (uint8_t* player = ActivePlayer())
                if (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0xD8) < r->level)
                    return 6;
            if (now < static_cast<__time64_t>(r->release))
                return 7;
        }
        return 0;
    }

    bool ReadClaimArgs(lua_State* L, uint32_t& tier, uint32_t& count, bool& flag)   // FUN_10221e50
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, BOOLEAN}))
            return false;
        tier = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        count = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 2)));
        flag = AscLua::lua_toboolean(L, 3) != 0;
        return true;
    }

    bool ReadTierFlag(lua_State* L, uint32_t& tier, bool& flag)   // FUN_100e92d0
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return false;
        tier = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        flag = AscLua::lua_toboolean(L, 2) != 0;
        return true;
    }

    void PushItems(lua_State* L, const std::unordered_map<uint32_t, uint32_t>& items)   // FUN_102222e0
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(items.size()));
        AscLua::lua_checkstack(L, 2);
        for (const auto& e : items)
        {
            PushInt(L, static_cast<int32_t>(e.first));
            PushInt(L, static_cast<int32_t>(e.second));
            AscLua::lua_settable(L, -3);
        }
    }

    // GetMarkOfAscensionCost(tier, count, flag) (0x10a39880): the Marks the next `count` claims cost.
    int GetMarkOfAscensionCost(lua_State* L)
    {
        uint32_t tier, count;
        bool flag;
        if (!ReadClaimArgs(L, tier, count, flag) || tier == 0)
            return 0;
        std::unordered_map<uint32_t, uint32_t> items;
        uint32_t cost;
        Totals(tier - 1, count, flag, items, cost);
        PushInt(L, static_cast<int32_t>(cost));
        return 1;
    }

    // GetRewards(tier, count, flag) (0x10a39980): {item = count} the next `count` claims give.
    int GetRewards(lua_State* L)
    {
        uint32_t tier, count;
        bool flag;
        if (!ReadClaimArgs(L, tier, count, flag) || tier == 0)
            return 0;
        std::unordered_map<uint32_t, uint32_t> items;
        uint32_t cost;
        Totals(tier - 1, count, flag, items, cost);
        PushItems(L, items);
        return 1;
    }

    // GetLevelingInfo(tier) (0x10a39330): the tier's flagged rewards (map +0x0C) up to the server's max
    // level, as {Level, Rewards = {item = count} (later slots overwrite a repeated item), IsClaimed}.
    int GetLevelingInfo(lua_State* L)
    {
        uint32_t tier;
        if (!ReadNumber(L, tier) || tier == 0)
            return 0;
        const uint32_t t = tier - 1;
        struct Entry { uint32_t level; std::unordered_map<uint32_t, uint32_t> items; bool claimed; };
        std::vector<Entry> entries;
        const auto& m = g_sof.maps[1];
        const auto it = m.find(t);
        if (it != m.end())
            for (size_t i = 0; i < it->second.size(); ++i)
            {
                const Reward& r = g_sof.rewards[it->second[i]];
                if (r.level > AscGameEvents::ServerMaxLevel())
                    continue;
                Entry e{r.level, {}, false};
                for (int k = 0; k < 4; ++k)
                    if (r.items[k] && r.counts[k])
                        e.items[r.items[k]] = r.counts[k];
                const uint32_t claimed = t < g_sof.claimed.size() ? g_sof.claimed[t].v[1] : 0;
                e.claimed = i + 1 <= claimed;
                entries.push_back(std::move(e));
            }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < entries.size(); ++i)
        {
            PushInt(L, static_cast<int32_t>(i + 1));
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            AscLua::lua_pushstring(L, "Level");
            PushInt(L, static_cast<int32_t>(entries[i].level));
            AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "Rewards");
            PushItems(L, entries[i].items);
            AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "IsClaimed");
            PushBool(L, entries[i].claimed);
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int CanClaimScrollOfFortuneRewards(lua_State* L)
    {
        uint32_t tier, count;
        bool flag;
        if (!ReadClaimArgs(L, tier, count, flag) || tier == 0)
            return 0;
        const uint32_t code = CheckClaim(tier - 1, count, flag);
        PushBool(L, code == 0);
        if (code != 0)
            PushStr(L, code < 8 ? kResults[code] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(code)).c_str());
        else
            AscLua::lua_pushnil(L);
        return 2;
    }

    int ClaimScrollOfFortuneRewards(lua_State* L)
    {
        uint32_t tier, count;
        bool flag;
        if (!ReadClaimArgs(L, tier, count, flag) || tier == 0)
            return 0;
        if (CheckClaim(tier - 1, count, flag) != 0)
        {
            PushBool(L, false);
            return 1;
        }
        Packet(0x6B5).U32(tier - 1).U32(count).U8(flag ? 1 : 0).Send();
        g_sof.lastTier = tier - 1;
        g_sof.lastCount = count;
        g_sof.lastFlag = flag;
        PushBool(L, true);
        return 1;
    }

    int GetNumClaimableScrollOfFortuneRewards(lua_State* L)
    {
        uint32_t tier;
        bool flag;
        if (!ReadTierFlag(L, tier, flag) || tier == 0)
            return 0;
        uint32_t n = 0;
        while (n < 0x4000 && CheckClaim(tier - 1, n + 1, flag) == 0)
            ++n;
        PushInt(L, static_cast<int32_t>(n));
        return 1;
    }

    int GetNumClaimedScrollOfFortuneRewards(lua_State* L)
    {
        uint32_t tier;
        bool flag;
        if (!ReadTierFlag(L, tier, flag) || tier == 0)
            return 0;
        PushInt(L, static_cast<int32_t>(ClaimedCount(tier - 1, flag)));
        return 1;
    }

    // handler_GetScrollOfFortuneLimits: {ability = {used, cap}, talent = ..., generic = ...} for the
    // limits entry of the tier; nothing when there is none.
    void SetLimit(lua_State* L, const char* k, uint32_t used, uint32_t cap)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_createtable(L, 0, 2);
        AscLua::lua_pushstring(L, "used");
        PushInt(L, static_cast<int32_t>(used));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "cap");
        PushInt(L, static_cast<int32_t>(cap));
        AscLua::lua_settable(L, -3);
        AscLua::lua_settable(L, -3);
    }

    int GetScrollOfFortuneLimits(lua_State* L)
    {
        uint32_t tier;
        if (!ReadNumber(L, tier) || tier == 0)
            return 0;
        for (const Limit& l : g_sof.limits)
            if (l.tier == tier - 1)
            {
                AscLua::lua_createtable(L, 0, 3);
                AscLua::lua_checkstack(L, 3);
                SetLimit(L, "ability", l.v[3], l.v[0]);
                SetLimit(L, "talent", l.v[4], l.v[1]);
                SetLimit(L, "generic", l.v[5], l.v[2]);
                return 1;
            }
        return 0;
    }

    // FUN_10a39a80: (released, total, seconds until the next release) for the tier's no-flag rewards.
    int GetScrollOfFortuneRewardsReleaseInfo(lua_State* L)
    {
        uint32_t tier;
        if (!ReadNumber(L, tier) || tier == 0)
            return 0;
        const auto it = g_sof.maps[0].find(tier - 1);
        if (it == g_sof.maps[0].end() || it->second.empty())
            return PushNils(L, 3);
        const uint32_t now = static_cast<uint32_t>(_time64(nullptr));
        uint32_t released = 0, next = 0;
        for (uint32_t idx : it->second)
        {
            next = g_sof.rewards[idx].release;
            if (next > now)
                break;
            ++released;
        }
        PushInt(L, static_cast<int32_t>(released));
        PushInt(L, static_cast<int32_t>(it->second.size()));
        if (released < it->second.size())
            PushInt(L, static_cast<int32_t>(next - now));
        else
            AscLua::lua_pushnil(L);
        return 3;
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&Reset);
        sDC.AddPacketHandler(0x6AB, CNetClientCustomPacket((void*)&OnRewardsList, nullptr));
        sDC.AddPacketHandler(0x6B6, CNetClientCustomPacket((void*)&OnClaimResult, nullptr));
        sDC.AddPacketHandler(0x6B8, CNetClientCustomPacket((void*)&OnClaimedList, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_WildcardRewards", "CanClaimScrollOfFortuneRewards", CanClaimScrollOfFortuneRewards},
        {"C_WildcardRewards", "ClaimScrollOfFortuneRewards", ClaimScrollOfFortuneRewards},
        {"C_WildcardRewards", "GetNumClaimableScrollOfFortuneRewards", GetNumClaimableScrollOfFortuneRewards},
        {"C_WildcardRewards", "GetNumClaimedScrollOfFortuneRewards", GetNumClaimedScrollOfFortuneRewards},
        {"C_WildcardRewards", "GetScrollOfFortuneLimits", GetScrollOfFortuneLimits},
        {"C_WildcardRewards", "GetScrollOfFortuneRewardsReleaseInfo", GetScrollOfFortuneRewardsReleaseInfo},
        {"C_WildcardRewards", "GetMarkOfAscensionCost", GetMarkOfAscensionCost},
        {"C_WildcardRewards", "GetRewards", GetRewards},
        {"C_WildcardRewards", "GetLevelingInfo", GetLevelingInfo},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
