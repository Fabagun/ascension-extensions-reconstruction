// DraftModeMgr (FUN_102228f0 -> static 0x10BE23B0): draft picks, Hand of Fate picks/sacrifices and
// the skill-card flags, from SMSG_DRAFT_STATE 0x753 (FUN_10225b40). Reset at the glue screen
// (FUN_10224e40, registered through FUN_10278760).
//
// Manager bytes +0x30..+0x69 are kept as the original lays them out (Byte/Dword below take the
// original offset). Packet:
//   u32 -> +8, u8 -> +0x30, u32 -> +0x34 (selected HoF spell), u32 -> +0x38, u8 -> +0x3C, u32 -> +0x40,
//   u8 x6 -> +0x44..+0x49; then +0x58 := +0x3C, +0x5F := +0x44, +0x66 := +0x45, +0x67 := +0x46,
//   +0x68 := +0x47, +0x69 := +0x48;
//   u8 n x {u32 spell, u8 a, u8 b}  draft picks      (i < 3: +0x59+i/+0x4A+i := a, +0x5C+i/+0x4D+i := b)
//   u8 n x {u32 spell, u8 a, u8 b}  HoF picks        (i < 3: +0x60+i/+0x50+i := a, +0x63+i/+0x53+i := b)
//   u8 n x {u32 spell}              HoF sacrifices
//   u32 n, then n x 8 bytes skipped.
// Afterwards +4 := +0x34, and if +0x68 or +0x69 is set and the selected spell's index in the HoF
// picks is below min(count, 3), +0x68/+0x69 take that pick's a/b bytes. Events (no arguments) fire
// for each part that changed against the pre-packet state, then ASCENSION_DRAFT_STATE_UPDATED unless
// nothing changed and +0x49 is also unchanged.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    struct Draft
    {
        uint32_t selected = 0;      // +0x04
        uint32_t field8 = 0;        // +0x08
        std::vector<uint32_t> picks, hof, sacrifice;   // +0x0C, +0x18, +0x24
        uint8_t bytes[0x3A] = {};   // +0x30..+0x69

        uint8_t& Byte(uint32_t off) { return bytes[off - 0x30]; }
        uint8_t Byte(uint32_t off) const { return bytes[off - 0x30]; }
        uint32_t Dword(uint32_t off) const { uint32_t v; memcpy(&v, &bytes[off - 0x30], 4); return v; }
        void SetDword(uint32_t off, uint32_t v) { memcpy(&bytes[off - 0x30], &v, 4); }
        bool Same(const Draft& o, uint32_t from, uint32_t to) const
        {
            return memcmp(&bytes[from - 0x30], &o.bytes[from - 0x30], to - from + 1) == 0;
        }
    } g_draft;

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    // FUN_102226a0 + FUN_10224ad0 and the metadata clear shared by the reset and the packet.
    void ResetState()
    {
        g_draft.selected = 0;
        g_draft.field8 = 0;
        g_draft.picks.clear();
        g_draft.hof.clear();
        g_draft.sacrifice.clear();
        memset(g_draft.bytes, 0, sizeof(g_draft.bytes));
    }

    void __cdecl OnDraftState(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const Draft old = g_draft;
        ResetState();
        Draft& d = g_draft;
        d.field8 = Read<uint32_t>(p);
        d.Byte(0x30) = Read<uint8_t>(p);
        d.SetDword(0x34, Read<uint32_t>(p));
        d.SetDword(0x38, Read<uint32_t>(p));
        d.Byte(0x3C) = Read<uint8_t>(p);
        d.SetDword(0x40, Read<uint32_t>(p));
        for (uint32_t off = 0x44; off <= 0x49; ++off)
            d.Byte(off) = Read<uint8_t>(p);
        d.Byte(0x58) = d.Byte(0x3C);
        d.Byte(0x5F) = d.Byte(0x44);
        d.Byte(0x66) = d.Byte(0x45);
        d.Byte(0x67) = d.Byte(0x46);
        d.Byte(0x68) = d.Byte(0x47);
        d.Byte(0x69) = d.Byte(0x48);

        for (uint8_t i = 0, n = Read<uint8_t>(p); i < n; ++i)
        {
            d.picks.push_back(Read<uint32_t>(p));
            const uint8_t a = Read<uint8_t>(p), b = Read<uint8_t>(p);
            if (i < 3)
            {
                d.Byte(0x59 + i) = a;
                d.Byte(0x5C + i) = b;
                d.Byte(0x4A + i) = a;
                d.Byte(0x4D + i) = b;
            }
        }
        for (uint8_t i = 0, n = Read<uint8_t>(p); i < n; ++i)
        {
            d.hof.push_back(Read<uint32_t>(p));
            const uint8_t a = Read<uint8_t>(p), b = Read<uint8_t>(p);
            if (i < 3)
            {
                d.Byte(0x60 + i) = a;
                d.Byte(0x63 + i) = b;
                d.Byte(0x50 + i) = a;
                d.Byte(0x53 + i) = b;
            }
        }
        for (uint8_t i = 0, n = Read<uint8_t>(p); i < n; ++i)
            d.sacrifice.push_back(Read<uint32_t>(p));
        p->m_read += static_cast<int32_t>(Read<uint32_t>(p) * 8);

        const uint32_t sel = d.Dword(0x34);
        uint32_t index = static_cast<uint32_t>(d.hof.size());
        if (sel != 0)
        {
            const auto it = std::find(d.hof.begin(), d.hof.end(), sel);
            index = static_cast<uint32_t>(it - d.hof.begin());
        }
        d.selected = sel;
        if ((d.Byte(0x68) || d.Byte(0x69)) && index < d.hof.size() && index < 3)
        {
            d.Byte(0x68) = d.Byte(0x60 + index);
            d.Byte(0x69) = d.Byte(0x63 + index);
        }

        const bool picksChanged = old.picks != d.picks;
        const bool hofChanged = old.hof != d.hof || old.Byte(0x30) != d.Byte(0x30)
            || old.Dword(0x38) != d.Dword(0x38) || old.Dword(0x34) != d.Dword(0x34);
        const bool sacrificeChanged = old.sacrifice != d.sacrifice;
        const bool selectedChanged = old.selected != d.selected;
        const bool pickMetaChanged = !old.Same(d, 0x58, 0x5E);
        const bool hofMetaChanged = !old.Same(d, 0x5F, 0x65);
        const bool sacrificeMetaChanged = old.Byte(0x66) != d.Byte(0x66);
        const bool selectedMetaChanged = !old.Same(d, 0x67, 0x69);

        if (picksChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_DRAFT_PICK_CHANGED");
        if (hofChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_HAND_OF_FATE_CHANGED");
        if (sacrificeChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_HAND_OF_FATE_SACRIFICE_CHANGED");
        if (selectedChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_HAND_OF_FATE_SELECTED_CHANGED");
        if (pickMetaChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_DRAFT_PICK_METADATA_CHANGED");
        if (hofMetaChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_HAND_OF_FATE_METADATA_CHANGED");
        if (sacrificeMetaChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_HAND_OF_FATE_SACRIFICE_METADATA_CHANGED");
        if (selectedMetaChanged) AscRuntime::Signal("ASCENSION_DRAFTMODE_HAND_OF_FATE_SELECTED_METADATA_CHANGED");
        const bool any = picksChanged || hofChanged || sacrificeChanged || selectedChanged || pickMetaChanged
            || hofMetaChanged || sacrificeMetaChanged || selectedMetaChanged;
        if (any || old.Byte(0x49) != d.Byte(0x49))
            AscRuntime::Signal("ASCENSION_DRAFT_STATE_UPDATED");
    }

    bool DraftModeOn() { return ((AscGameMode::Mode() >> 3) & 1) != 0; }   // FUN_1031ef10 bit 3

    int PushFlag(lua_State* L, uint32_t off) { PushBool(L, g_draft.Byte(off) != 0); return 1; }

    int GetDraftModePickCount(lua_State* L) { PushInt(L, static_cast<int32_t>(g_draft.picks.size())); return 1; }
    int GetHandOfFatePickCount(lua_State* L)
    {
        PushInt(L, g_draft.Byte(0x30) == 1 ? static_cast<int32_t>(g_draft.hof.size()) : 0);
        return 1;
    }
    int GetHandOfFateSacrificePickCount(lua_State* L)
    {
        PushInt(L, g_draft.Byte(0x30) == 2 ? static_cast<int32_t>(g_draft.sacrifice.size()) : 0);
        return 1;
    }

    bool AnyNonZero(const std::vector<uint32_t>& v)
    {
        return std::any_of(v.begin(), v.end(), [](uint32_t x) { return x != 0; });
    }
    int HasDraftModePick(lua_State* L) { PushBool(L, DraftModeOn() && AnyNonZero(g_draft.picks)); return 1; }
    int HasHandOfFatePick(lua_State* L)
    {
        PushBool(L, DraftModeOn() && g_draft.Byte(0x30) == 1 && AnyNonZero(g_draft.hof));
        return 1;
    }
    int HasHandOfFateSacrificePick(lua_State* L)
    {
        PushBool(L, DraftModeOn() && g_draft.Byte(0x30) == 2 && AnyNonZero(g_draft.sacrifice));
        return 1;
    }

    uint32_t At(const std::vector<uint32_t>& v, lua_State* L)
    {
        const uint32_t i = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        return i < v.size() ? v[i] : 0;
    }
    int GetDraftModePickSpellAtIndex(lua_State* L) { PushInt(L, static_cast<int32_t>(At(g_draft.picks, L))); return 1; }
    int GetHandOfFatePickSpellAtIndex(lua_State* L)
    {
        const uint32_t v = At(g_draft.hof, L);
        PushInt(L, g_draft.Byte(0x30) == 1 ? static_cast<int32_t>(v) : 0);
        return 1;
    }
    int GetHandOfFateSacrificePickSpellAtIndex(lua_State* L)
    {
        const uint32_t v = At(g_draft.sacrifice, L);
        PushInt(L, g_draft.Byte(0x30) == 2 ? static_cast<int32_t>(v) : 0);
        return 1;
    }

    int GetSelectedHandOfFateSpell(lua_State* L) { PushInt(L, static_cast<int32_t>(g_draft.selected)); return 1; }
    int IsMasteryDraft(lua_State* L) { return PushFlag(L, 0x58); }
    int IsCardSwapPick(lua_State* L) { return PushFlag(L, 0x5F); }
    int IsCardSwapSacrifice(lua_State* L) { return PushFlag(L, 0x66); }
    int IsCardSwapPicked(lua_State* L) { return PushFlag(L, 0x67); }
    int IsSkillCardPicked(lua_State* L) { return PushFlag(L, 0x68); }
    int IsLuckySkillCardPicked(lua_State* L) { return PushFlag(L, 0x69); }
    int IsCardSwap(lua_State* L)
    {
        PushBool(L, g_draft.Byte(0x5F) || g_draft.Byte(0x66) || g_draft.Byte(0x67));
        return 1;
    }

    // FUN_10223cd0 / bb0 / d30 / c10: (byte)index < 3, else false.
    template <uint32_t Base>
    int IndexedFlag(lua_State* L)
    {
        const uint8_t i = static_cast<uint8_t>(static_cast<int32_t>(CheckNumber(L, 1)));
        PushBool(L, i < 3 && g_draft.Byte(Base + i) != 0);
        return 1;
    }

    // ---- Hand of Fate rewards ----
    // SMSG_HAND_OF_FATE_REWARDS_LIST 0x6AA (FUN_10226ff0) is a CompressedDataStore (FUN_10195a60):
    // u32 inflated size, then zlib data inflated with the client's 0x778180. Inflated: u32 n x 0x32-byte
    // records, each also indexed by (flagA, flagB) and tier: maps at +0x78 (A=1,B=0), +0x98 (1,1),
    // +0xB8 (0,0), +0xD8 (0,1). The list and maps are cleared first, even if inflating fails.
#pragma pack(push, 1)
    struct Reward
    {
        uint32_t tier;
        uint8_t flagA, flagB;
        uint32_t level;
        uint32_t items[4];
        uint32_t counts[4];
        uint32_t cost;
        uint32_t release;
    };
#pragma pack(pop)
    static_assert(sizeof(Reward) == 0x32, "hand of fate reward");

    struct Claimed { uint32_t v[5]; };   // 0x14 bytes: v[0] tier, v[1] claimed (flag), v[2] claimed (no flag)

    struct
    {
        std::vector<Reward> rewards;                                      // +0x6C
        std::unordered_map<uint32_t, std::vector<uint32_t>> maps[2][2];   // [flagA][flagB] -> tier -> reward indices
        std::vector<Claimed> claimed;                                     // +0xF8
        uint32_t lastTier = 0, lastCount = 0;                             // +0x104, +0x108
        bool lastFlag = false;                                            // +0x10C
    } g_hof;

    void __cdecl OnRewardsList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_hof.rewards.clear();
        for (auto& row : g_hof.maps)
            for (auto& m : row)
                m.clear();
        std::vector<uint8_t> data;
        if (!InflatePacket(p, data) || data.size() < 4)
            return;
        uint32_t n;
        memcpy(&n, data.data(), 4);
        size_t at = 4;
        g_hof.rewards.reserve(n);
        for (uint32_t i = 0; i < n && at + sizeof(Reward) <= data.size(); ++i, at += sizeof(Reward))
        {
            Reward r;
            memcpy(&r, data.data() + at, sizeof(r));
            g_hof.rewards.push_back(r);
            g_hof.maps[r.flagA != 0][r.flagB != 0][r.tier].push_back(static_cast<uint32_t>(g_hof.rewards.size() - 1));
        }
    }

    // SMSG_COLLECTED_HAND_OF_FATE_REWARDS_LIST 0x6B7 (FUN_10225780): u32 n x 0x14 raw.
    void __cdecl OnClaimedList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_hof.claimed.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_hof.claimed.push_back(Read<Claimed>(p));
    }

    const char* const kResults[8] = {
        "COLLECT_HAND_OF_FATE_REWARDS_OK", "COLLECT_HAND_OF_FATE_REWARDS_UNKNOWN",
        "COLLECT_HAND_OF_FATE_REWARDS_SPECIALIZATION_OUT_OF_RANGE", "COLLECT_HAND_OF_FATE_REWARDS_COUNT_TOO_HIGH",
        "COLLECT_HAND_OF_FATE_REWARDS_INVALID_COUNT", "COLLECT_HAND_OF_FATE_REWARDS_NOT_ENOUGH_MARKS_OF_ASCENSION",
        "COLLECT_HAND_OF_FATE_REWARDS_TOO_LOW_LEVEL", "COLLECT_HAND_OF_FATE_REWARDS_NOT_RELEASED",
    };

    // SMSG_COLLECT_HAND_OF_FATE_REWARDS_RESULT 0x6B4 (FUN_102253c0): cstring result. On _OK the last
    // claim (ClaimHandOfFateRewards) is applied to the claimed list; then the event fires with it.
    void __cdecl OnClaimResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(result.size() + 1);
        if (result == kResults[0])
        {
            if (g_hof.claimed.size() <= g_hof.lastTier)
                g_hof.claimed.resize(g_hof.lastTier + 1, Claimed{});
            Claimed& c = g_hof.claimed[g_hof.lastTier];
            c.v[0] = g_hof.lastTier;
            c.v[g_hof.lastFlag ? 1 : 2] += g_hof.lastCount;
        }
        AscRuntime::Signal("COLLECT_HAND_OF_FATE_REWARDS_RESULT", "%s", result.c_str());
    }

    // FUN_10225370
    uint32_t ClaimedCount(uint32_t tier, bool flag)
    {
        return tier < g_hof.claimed.size() ? g_hof.claimed[tier].v[flag ? 1 : 2] : 0;
    }

    // FUN_102252c0: the 1-based index-th reward of (flag, 0) for the tier, or null.
    const Reward* RewardAt(uint32_t tier, uint32_t index, bool flag)
    {
        const auto& m = g_hof.maps[flag][0];
        const auto it = m.find(tier);
        if (it == m.end() || index == 0 || index > it->second.size())
            return nullptr;
        return &g_hof.rewards[it->second[index - 1]];
    }

    // FUN_10225200: consecutive rewards from `start`, stopping at the first gap.
    std::vector<const Reward*> NextRewards(uint32_t tier, uint32_t start, uint32_t count, bool flag)
    {
        std::vector<const Reward*> out;
        for (uint32_t i = start; i < start + count; ++i)
        {
            const Reward* r = RewardAt(tier, i, flag);
            if (!r)
                break;
            out.push_back(r);
        }
        return out;
    }

    // FUN_10225080: items summed over the next `count` unclaimed rewards, and their total cost.
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

    // FUN_10308570: stack counts of `entry` in the active player's slots 0x76..0x95.
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
        {
            const uint64_t guid = *reinterpret_cast<const uint64_t*>(desc + 0x510 + i * 8);
            if (const uint8_t* item = static_cast<const uint8_t*>(ObjectPtr(guid, 2)))
            {
                const uint8_t* idesc = *reinterpret_cast<uint8_t* const*>(item + 8);
                if (*reinterpret_cast<const uint32_t*>(idesc + 0xC) == entry)
                    total += *reinterpret_cast<const uint32_t*>(idesc + 0x38);
            }
        }
        return total;
    }

    // FUN_10224c10: 0 = claimable, else the COLLECT_HAND_OF_FATE_REWARDS_* code.
    uint32_t CheckClaim(uint32_t tier, uint32_t count, bool flag)
    {
        if (tier >= g_hof.claimed.size())
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

    // FUN_10221e50: {number, number, boolean} -> tier (1-based), count, flag.
    bool ReadClaimArgs(lua_State* L, uint32_t& tier, uint32_t& count, bool& flag)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, BOOLEAN}))
            return false;
        tier = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        count = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 2)));
        flag = AscLua::lua_toboolean(L, 3) != 0;
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

    int CanClaimHandOfFateRewards(lua_State* L)
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

    // CMSG 0x6B3 {u32 tier, u32 count, u8 flag}; the claim is remembered for the 0x6B4 result.
    int ClaimHandOfFateRewards(lua_State* L)
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
        Packet(0x6B3).U32(tier - 1).U32(count).U8(flag ? 1 : 0).Send();
        g_hof.lastTier = tier - 1;
        g_hof.lastCount = count;
        g_hof.lastFlag = flag;
        PushBool(L, true);
        return 1;
    }

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

    // FUN_100e92d0: {number, boolean}.
    int GetNumClaimedHandOfFateRewards(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return 0;
        const uint32_t tier = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const bool flag = AscLua::lua_toboolean(L, 2) != 0;
        if (tier == 0)
            return 0;
        PushInt(L, static_cast<int32_t>(ClaimedCount(tier - 1, flag)));
        return 1;
    }

    // FUN_102240f0 / FUN_10224010 / FUN_10222aa0: CMSG 0x750 / 0x751 {u32 spell}, CMSG 0x752 {}.
    int SendHandOfFateSpellSelection(lua_State* L)
    {
        Packet(0x750).U32(static_cast<uint32_t>(ToInt(CheckNumber(L, 1)))).Send();
        return 0;
    }
    int SendHandOfFateSacrificeSpellSelection(lua_State* L)
    {
        Packet(0x751).U32(static_cast<uint32_t>(ToInt(CheckNumber(L, 1)))).Send();
        return 0;
    }
    int CancelHandOfFateSelection(lua_State*)
    {
        Packet(0x752).Send();
        return 0;
    }

    // FUN_102237c0: the largest count (from 1, up to 0x4000) whose claim check passes.
    int GetNumClaimableHandOfFateRewards(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return 0;
        const uint32_t tier = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const bool flag = AscLua::lua_toboolean(L, 2) != 0;
        if (tier == 0)
            return 0;
        uint32_t n = 0;
        while (n < 0x4000 && CheckClaim(tier - 1, n + 1, flag) == 0)
            ++n;
        PushInt(L, static_cast<int32_t>(n));
        return 1;
    }

    const std::vector<uint32_t>* TierRewards(bool flagA, uint32_t tier)   // FUN_10224f20 / FUN_10224fd0
    {
        const auto& m = g_hof.maps[flagA][0];
        const auto it = m.find(tier);
        return it != m.end() ? &it->second : nullptr;
    }

    // FUN_10223590: (released, total, seconds until the next release) for the tier's (0,0) rewards.
    int GetHandOfFateRewardsReleaseInfo(lua_State* L)
    {
        uint32_t tier;
        if (!ReadNumber(L, tier) || tier == 0)
            return 0;
        const std::vector<uint32_t>* recs = TierRewards(false, tier - 1);
        if (!recs || recs->empty())
            return PushNils(L, 3);
        const uint32_t now = static_cast<uint32_t>(_time64(nullptr));
        uint32_t released = 0, next = 0;
        for (uint32_t idx : *recs)
        {
            next = g_hof.rewards[idx].release;
            if (next > now)
                break;
            ++released;
        }
        PushInt(L, static_cast<int32_t>(released));
        PushInt(L, static_cast<int32_t>(recs->size()));
        if (released < recs->size())
            PushInt(L, static_cast<int32_t>(next - now));
        else
            AscLua::lua_pushnil(L);
        return 3;
    }

    // FUN_10222d50: the tier's (1,0) rewards up to the server's max level: {Level, Rewards, IsClaimed}.
    int GetLevelingInfo(lua_State* L)
    {
        uint32_t tier;
        if (!ReadNumber(L, tier) || tier == 0)
            return 0;
        struct Entry { uint32_t level; std::unordered_map<uint32_t, uint32_t> items; bool claimed; };
        std::vector<Entry> entries;
        if (const std::vector<uint32_t>* recs = TierRewards(true, tier - 1))
        {
            const uint32_t maxLevel = AscGameEvents::ServerMaxLevel();
            for (uint32_t i = 0; i < recs->size(); ++i)
            {
                const Reward& r = g_hof.rewards[(*recs)[i]];
                if (r.level > maxLevel)
                    continue;
                Entry e{r.level, {}, false};
                for (int k = 0; k < 4; ++k)
                    if (r.items[k] && r.counts[k])
                        e.items[r.items[k]] = r.counts[k];
                e.claimed = i + 1 <= ClaimedCount(tier - 1, true);
                entries.push_back(std::move(e));
            }
        }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < entries.size(); ++i)
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

    void Init()
    {
        sDC.AddPacketHandler(0x753, CNetClientCustomPacket((void*)&OnDraftState, nullptr));
        sDC.AddPacketHandler(0x6AA, CNetClientCustomPacket((void*)&OnRewardsList, nullptr));
        sDC.AddPacketHandler(0x6B4, CNetClientCustomPacket((void*)&OnClaimResult, nullptr));
        sDC.AddPacketHandler(0x6B7, CNetClientCustomPacket((void*)&OnClaimedList, nullptr));
        AscRuntime::OnGlueScreen(&ResetState);
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetDraftModePickCount", GetDraftModePickCount},
        {nullptr, "GetHandOfFatePickCount", GetHandOfFatePickCount},
        {nullptr, "GetHandOfFateSacrificePickCount", GetHandOfFateSacrificePickCount},
        {nullptr, "HasDraftModePick", HasDraftModePick},
        {nullptr, "HasHandOfFatePick", HasHandOfFatePick},
        {nullptr, "HasHandOfFateSacrificePick", HasHandOfFateSacrificePick},
        {nullptr, "GetDraftModePickSpellAtIndex", GetDraftModePickSpellAtIndex},
        {nullptr, "GetHandOfFatePickSpellAtIndex", GetHandOfFatePickSpellAtIndex},
        {nullptr, "GetHandOfFateSacrificePickSpellAtIndex", GetHandOfFateSacrificePickSpellAtIndex},
        {nullptr, "GetSelectedHandOfFateSpell", GetSelectedHandOfFateSpell},
        {nullptr, "IsMasteryDraft", IsMasteryDraft},
        {nullptr, "IsCardSwap", IsCardSwap},
        {nullptr, "IsCardSwapPick", IsCardSwapPick},
        {nullptr, "IsCardSwapSacrifice", IsCardSwapSacrifice},
        {nullptr, "IsCardSwapPicked", IsCardSwapPicked},
        {nullptr, "IsSkillCardPicked", IsSkillCardPicked},
        {nullptr, "IsLuckySkillCardPicked", IsLuckySkillCardPicked},
        {nullptr, "IsSkillCardDraft", IndexedFlag<0x59>},
        {nullptr, "IsLuckySkillCardDraft", IndexedFlag<0x5C>},
        {nullptr, "IsSkillCardPick", IndexedFlag<0x60>},
        {nullptr, "IsLuckySkillCardPick", IndexedFlag<0x63>},
        {nullptr, "SendHandOfFateSpellSelection", SendHandOfFateSpellSelection},
        {nullptr, "SendHandOfFateSacrificeSpellSelection", SendHandOfFateSacrificeSpellSelection},
        {nullptr, "CancelHandOfFateSelection", CancelHandOfFateSelection},
        {"C_DraftRewards", "CanClaimHandOfFateRewards", CanClaimHandOfFateRewards},
        {"C_DraftRewards", "ClaimHandOfFateRewards", ClaimHandOfFateRewards},
        {"C_DraftRewards", "GetRewards", GetRewards},
        {"C_DraftRewards", "GetMarkOfAscensionCost", GetMarkOfAscensionCost},
        {"C_DraftRewards", "GetNumClaimedHandOfFateRewards", GetNumClaimedHandOfFateRewards},
        {"C_DraftRewards", "GetHandOfFateRewardsReleaseInfo", GetHandOfFateRewardsReleaseInfo},
        {"C_DraftRewards", "GetLevelingInfo", GetLevelingInfo},
        {"C_DraftRewards", "GetNumClaimableHandOfFateRewards", GetNumClaimableHandOfFateRewards},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
