// C_Wildcard, part 4: rapid rolling (WildcardMgr +0x1D8..+0x274, FUN_10a349d0) -- the session record,
// the quick-roll configs (SMSG 0x6F0), the gates, and the unlearn (CMSG 0x61D) -> roll (CMSG 0x643) ->
// learn (SMSG 0x621) -> reveal loop driven by AscWildcardRolls.cpp's handlers. Also the plain roll,
// unlearn, roll-icon and starting-choice bindings.
//
// Session (+0x1E8): u64 id, phase (0 Idle .. 8 Cancelled), target entry, its rank at start, is-ability,
// roll count, reason, learned entry, new rank, pre-roll rank, stop code, is-desired, then the desired
// ids / tag ids sent with each roll; +0x268 the session counter, +0x270 the phase a cancel was
// requested in (0 none).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSkillCard.hpp>
#include <Ascension/AscToken.hpp>
#include <Ascension/AscWildcardRapid.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace AscScript;
using AscCA::Row;

namespace AscWildcardSets
{
    struct Next { uint32_t id; uint8_t kind; bool ability, talent; };
    bool NewestUnlearnable(Next& out, bool (*accept)(const Next&));   // FUN_10a344c0 (AscWildcardSets.cpp)
    bool HasDesiredWith(bool ability);                                // the desired list holds an ability / talent entry
    size_t DesiredCount();
    size_t UndesiredCount();
    void DesiredForRoll(std::vector<uint32_t>& ids, std::vector<uint32_t>& tags);
    void ClearForSpecChange();
    struct Selection { uint32_t id; uint8_t kind; };
    bool KindIndex(const std::string& name, uint8_t& kind);           // FUN_10a29980
    void ApplySelections(const std::vector<Selection>& desired, const std::vector<Selection>& undesired);
}
namespace AscWildcardRolls
{
    uint32_t SpecCounter(int which);   // ModeMgr per-spec arrays: 0 = +0x08, 1 = +0x58
    bool RevealsPending();
    bool CanAffordRoll(bool talent);   // FUN_10155120 / FUN_10155160 on the active build
    bool AtMaxLevel();                 // FUN_1008e380
    void ArmPoll();                    // FUN_10a31760
    uint32_t StartingCount();          // FUN_10154f50
    bool CanShowStartingChoice();      // FUN_10a2f920
}

namespace
{
    const char* const kPhases[9] = {"Idle", "WaitingForUnlearn", "WaitingForRoll", "WaitingForLearn", "Revealing",
        "AwaitingContinue", "Completed", "Failed", "Cancelled"};
    const char* const kCanUse[7] = {"CAN_USE_RAPID_ROLLING_OK", "CAN_USE_RAPID_ROLLING_UNKNOWN",
        "CAN_USE_RAPID_ROLLING_NOT_ENABLED", "CAN_USE_RAPID_ROLLING_NOT_WILDCARD", "CAN_USE_RAPID_ROLLING_NOT_MAX_LEVEL",
        "CAN_USE_RAPID_ROLLING_UNSPENT_ABILITY_ESSENCE", "CAN_USE_RAPID_ROLLING_UNSPENT_TALENT_ESSENCE"};   // 0x10B1FA94
    const char* const kCanStart[22] = {"CAN_START_RAPID_ROLLING_OK", "CAN_START_RAPID_ROLLING_UNKNOWN",
        "CAN_START_RAPID_ROLLING_NOT_ENABLED", "CAN_START_RAPID_ROLLING_NOT_WILDCARD", "CAN_START_RAPID_ROLLING_NOT_MAX_LEVEL",
        "CAN_START_RAPID_ROLLING_CANNOT_USE_RAPID_ROLLING", "CAN_START_RAPID_ROLLING_NO_ROLL",
        "CAN_START_RAPID_ROLLING_TOO_LOW_COUNT", "CAN_START_RAPID_ROLLING_TOO_HIGH_COUNT",
        "CAN_START_RAPID_ROLLING_NO_DESIRED_ENTRIES", "CAN_START_RAPID_ROLLING_NO_UNDESIRED_ENTRIES",
        "CAN_START_RAPID_ROLLING_NEXT_UNDESIRED_ENTRY_NOT_FOUND", "CAN_START_RAPID_ROLLING_NEXT_UNDESIRED_ENTRY_SPELL_TAG",
        "CAN_START_RAPID_ROLLING_NEXT_UNDESIRED_ENTRY_SUGGESTION", "CAN_START_RAPID_ROLLING_NEXT_UNDESIRED_ENTRY_NOT_VALID",
        "CAN_START_RAPID_ROLLING_MISSING_ABILITY_TOKENS", "CAN_START_RAPID_ROLLING_MISSING_TALENT_TOKENS",
        "CAN_START_RAPID_ROLLING_UNSPENT_ABILITY_ESSENCE", "CAN_START_RAPID_ROLLING_UNSPENT_TALENT_ESSENCE",
        "CAN_START_RAPID_ROLLING_NO_DESIRED_ABILITIES", "CAN_START_RAPID_ROLLING_NO_DESIRED_TALENTS",
        "CAN_START_RAPID_ROLLING_UNBLOCKED_SKILL_CARDS"};   // 0x10B20960

    struct Session
    {
        uint64_t id = 0;
        uint32_t phase = 0, target = 0;
        uint32_t startRank = 0;   // +0x1F8
        bool isAbility = false;
        uint32_t rollCount = 0;
        std::string reason;
        uint32_t learned = 0, newRank = 0, preRollRank = 0;
        std::string stopCode = "STOP_RAPID_ROLLING_NONE";
        bool isDesired = false;
        std::vector<uint32_t> ids, tags;   // +0x244 / +0x250
    };
    Session g_session;          // +0x1E8
    uint32_t g_cancelFrom = 0;  // +0x270
    uint64_t g_sessionCounter = 0;   // +0x268
    bool g_rerollPending = false;   // DAT_10d3d0b0: an unlearn (CMSG 0x61D) is outstanding

    struct QuickRoll { uint8_t kind; uint32_t threshold, max; };
    std::vector<QuickRoll> g_quick;   // +0x1D8 (SMSG 0x6F0)

    bool Active() { return g_session.phase >= 1 && g_session.phase <= 5; }                      // FUN_10a34ce0
    bool InFlight() { return g_cancelFrom != 0 || (g_session.phase >= 1 && g_session.phase <= 3); }   // FUN_10a34cb0
    bool Awaiting()   // FUN_10a34ad0
    {
        return g_cancelFrom == 0 && g_session.phase == 5 && g_session.stopCode == "STOP_RAPID_ROLLING_TALENT_UPGRADE_LEARNED";
    }
    bool QuickRollingEnabled()   // FUN_10a34ba0
    {
        const bool* v = AscConfig::Bool("CONFIG_WILDCARD_QUICK_ROLLING_ENABLED");
        return v && *v;
    }

    // FUN_10a346d0(isAbility, next): the configs of that kind in order against the spec's count (ability
    // ModeMgr +0x08, talent +0x58): without `next` the last one reached (threshold <= count), with it the
    // first not yet reached.
    const QuickRoll* Config(bool isAbility, bool next)
    {
        if (!ActivePlayer())
            return nullptr;
        const uint32_t count = AscWildcardRolls::SpecCounter(isAbility ? 0 : 1);
        const QuickRoll* last = nullptr;
        for (const QuickRoll& q : g_quick)
        {
            if ((q.kind != 0) != isAbility)
                continue;
            if (count < q.threshold && !next)
                return last;
            last = &q;
            if (next && count < q.threshold)
                return &q;
        }
        return last;
    }
    uint32_t MaxRolls(bool isAbility) { const QuickRoll* q = Config(isAbility, false); return q ? q->max : 0; }   // FUN_10a344a0

    // FUN_10a347e0: the spec's roll tokens plus its ability (or talent) tokens (FUN_10a2ffc0 /
    // FUN_10a2fd80 / FUN_10a30180; 0x3D past spec 19).
    uint64_t RollTokens(bool ability)
    {
        const int spec = AscGameMode::ActiveSpecIndex();
        const bool ok = spec >= 0 && spec < 20;
        const uint32_t roll = ok ? static_cast<uint32_t>(spec) : 0x3D;
        const uint32_t kind = ok ? (ability ? 0x14u : 0x28u) + static_cast<uint32_t>(spec) : 0x3D;
        return AscToken::Amount64(roll) + AscToken::Amount64(kind);
    }

    // FUN_10a344c0's acceptance test: out of tokens relative to this kind's quick-roll limit.
    bool Unlearnable(const AscWildcardSets::Next& n)
    {
        const QuickRoll* q = Config(n.ability, false);
        const uint32_t limit = q ? q->max : 0;
        return limit <= RollTokens(n.ability);
    }
    bool NextUnlearn(AscWildcardSets::Next& n) { return AscWildcardSets::NewestUnlearnable(n, Unlearnable); }

    bool NextIsAbility()   // FUN_10a34b50
    {
        AscWildcardSets::Next n;
        if (!NextUnlearn(n) || !(n.kind == 1 || n.kind == 2 || n.kind == 4))
            return false;
        Row r = AscCA::FindRow(n.id);
        const int t = r ? AscCA::RowType(r) : -1;
        return t == 1 || t == 4;
    }

    // FUN_10a336b0: CAN_USE_RAPID_ROLLING_*.
    uint32_t CanUse()
    {
        if (!QuickRollingEnabled())
            return 2;
        if (!((AscGameMode::Mode() >> 6) & 1))
            return 3;
        if (!ActivePlayer() || !AscWildcardRolls::AtMaxLevel())
            return 4;
        if (AscWildcardRolls::CanAffordRoll(false))
            return 5;
        return AscWildcardRolls::CanAffordRoll(true) ? 6 : 0;
    }

    // FUN_10a333d0(count, isAbility): CAN_START_RAPID_ROLLING_*.
    uint32_t CanStartCode(uint32_t count, bool isAbility)
    {
        if (!QuickRollingEnabled())
            return 2;
        if (!((AscGameMode::Mode() >> 6) & 1))
            return 3;
        if (!ActivePlayer() || !AscWildcardRolls::AtMaxLevel())
            return 4;
        if (AscWildcardRolls::CanAffordRoll(false))
            return 0x11;
        if (AscWildcardRolls::CanAffordRoll(true))
            return 0x12;
        const QuickRoll* q = Config(isAbility, false);
        if (q && count < q->max)
            return 7;
        if (MaxRolls(isAbility) < count)
            return 8;
        if (AscWildcardSets::DesiredCount() == 0)
            return 9;
        if (AscWildcardSets::UndesiredCount() == 0)
            return 10;
        AscWildcardSets::Next n;
        if (!NextUnlearn(n))
            return 0xB;
        if (n.kind == 5)
            return 0xC;
        if (n.kind == 6)
            return 0xD;
        Row r = AscCA::FindRow(n.id);
        if (!r)
            return 0xB;
        const bool abilityRow = AscCA::RowType(r) == 1 || AscCA::RowType(r) == 4;
        const uint64_t tokens = RollTokens(abilityRow);
        if (tokens < count)
            return abilityRow ? 0xF : 0x10;
        if (n.ability && !AscWildcardSets::HasDesiredWith(true))
            return 0x13;
        if (n.talent && !AscWildcardSets::HasDesiredWith(false))
            return 0x14;
        // FUN_102f4f70: a slotted, unblocked skill card in the active spec.
        const int spec = AscGameMode::ActiveSpecIndex();
        return AscSkillCard::AnyUnblocked(spec < 0 ? 0xFFFFFFFF : static_cast<uint32_t>(spec)) ? 0x15 : 0;
    }

    // FUN_10a2a410: {ok, reason}.
    std::pair<bool, std::string> CanStart()
    {
        if (Active())
            return {false, "RAPID_ROLLING_SESSION_ACTIVE"};
        if (InFlight())
            return {false, "RAPID_ROLLING_REQUEST_IN_FLIGHT"};
        if (AscWildcardRolls::RevealsPending())
            return {false, "WILDCARD_REVEAL_PENDING"};
        if (g_rerollPending)
            return {false, "WILDCARD_REROLL_PENDING"};
        const bool isAbility = NextIsAbility();
        const uint32_t code = CanStartCode(MaxRolls(isAbility), isAbility);
        if (code == 0)
            return {true, std::string()};
        return {false, code < 22 ? kCanStart[code] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(code)};
    }

    // ---- the session's transitions -----------------------------------------------------------------
    // FUN_10a36740: a new phase (or reason) is announced; the same phase with the same reason is not.
    void SetPhase(uint32_t phase, const std::string& reason)
    {
        if (g_session.phase == phase && g_session.reason == reason)
            return;
        g_session.phase = phase;
        g_session.reason = reason;
        AscRuntime::Signal("WILDCARD_RAPID_ROLL_STATE_CHANGED");
    }

    // FUN_10a36280: a fresh record under the same session id, in `phase` with `reason`.
    void Reset(uint32_t phase, const std::string& reason)
    {
        const uint64_t id = g_session.id;
        g_session = Session{};
        g_session.id = id;
        g_session.phase = phase;
        g_session.reason = reason;
        AscRuntime::Signal("WILDCARD_RAPID_ROLL_STATE_CHANGED");
    }

    // FUN_10a34180: the answer a cancel was waiting for arrived; a cancelled session records why.
    void Drain(const std::string& reason)
    {
        g_cancelFrom = 0;
        if (g_session.phase == 8)
            SetPhase(8, reason);
    }

    // FUN_10a33840 (and the first half of FUN_10a35270): mid-request, the session is marked cancelled
    // and the request's answer drained later; otherwise it simply ends.
    void CancelSession()
    {
        const uint32_t phase = g_session.phase;
        if (phase == 0 || g_cancelFrom != 0)
            return;
        if (phase >= 1 && phase <= 3)
        {
            Reset(8, "CANCELLED");
            g_cancelFrom = phase;
        }
        else
            Reset(0, std::string());
    }

    // CMSG 0x643: u32 roll count, the desired ids and tag ids (u32 count + u32 each), u8 is-ability.
    void SendRoll(uint32_t count, const std::vector<uint32_t>& ids, const std::vector<uint32_t>& tags, bool isAbility)
    {
        Packet p(0x643);
        p.U32(count).U32(static_cast<uint32_t>(ids.size()));
        for (uint32_t id : ids)
            p.U32(id);
        p.U32(static_cast<uint32_t>(tags.size()));
        for (uint32_t t : tags)
            p.U32(t);
        p.U8(isAbility ? 1 : 0).Send();
    }

    // FUN_10a36d00: start (or, from AwaitingContinue, continue) a session on the newest undesired entry.
    std::pair<bool, std::string> Start(bool continuing)
    {
        const uint32_t phase = g_session.phase;
        if (phase >= 1 && phase <= 5 && !(continuing && phase == 5))
            return {false, "RAPID_ROLLING_SESSION_ACTIVE"};
        if (g_cancelFrom != 0 || (phase >= 1 && phase <= 3))
            return {false, "RAPID_ROLLING_REQUEST_IN_FLIGHT"};
        if (AscWildcardRolls::RevealsPending())
            return {false, "WILDCARD_REVEAL_PENDING"};
        if (g_rerollPending)
            return {false, "WILDCARD_REROLL_PENDING"};
        uint32_t code = 0xB;
        AscWildcardSets::Next n;
        if (NextUnlearn(n))
            if (Row r = AscCA::FindRow(n.id))
            {
                const int t = AscCA::RowType(r);
                const bool isAbility = t == 1 || t == 4;   // FUN_101c6770
                const uint32_t count = MaxRolls(isAbility);
                code = CanStartCode(count, isAbility);
                if (code == 0)
                {
                    const AscCA::Build* b = AscCA::ActiveBuild();
                    const uint32_t rank = b ? b->RankOf(n.id) : 0;   // FUN_10152920
                    Session s;
                    s.id = ++g_sessionCounter;   // +0x268
                    s.phase = 1;
                    s.target = n.id;
                    s.startRank = rank;
                    s.isAbility = isAbility;
                    s.rollCount = count;
                    s.reason = "WAITING_FOR_UNLEARN";
                    s.preRollRank = rank;
                    AscWildcardSets::DesiredForRoll(s.ids, s.tags);
                    g_session = s;   // FUN_10a327e0
                    Packet(0x61D).U32(g_session.target).Send();
                    AscWildcardRolls::ArmPoll();   // FUN_10a31760
                    AscRuntime::Signal("WILDCARD_RAPID_ROLL_STATE_CHANGED");
                    return {true, std::string()};
                }
            }
        return {false, code < 22 ? kCanStart[code] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(code)};   // FUN_10a343d0
    }

    // FUN_10a364f0: a talent-upgrade learn earns one more talent roll, sent at once.
    bool TalentUpgradeRoll()
    {
        if (g_session.phase != 4 || g_session.stopCode != "STOP_RAPID_ROLLING_TALENT_UPGRADE_LEARNED")
            return false;
        g_session.rollCount = 0;
        g_session.isAbility = false;
        g_session.learned = g_session.newRank = g_session.preRollRank = 0;
        g_session.stopCode = "STOP_RAPID_ROLLING_TALENT_UPGRADE_BONUS_PENDING";
        g_session.isDesired = false;
        g_session.ids.clear();
        g_session.tags.clear();
        SendRoll(0, {}, {}, false);
        SetPhase(2, "WAITING_FOR_TALENT_UPGRADE_ROLL");
        return true;
    }

    const char* const kUnlearnResults[23] = {"CA_UNLEARN_OK", "CA_UNLEARN_UNKNOWN", "CA_UNLEARN_NOT_IN_WORLD",
        "CA_UNLEARN_BAD_ABILITY", "CA_UNLEARN_NOT_IN_BATTLEGROUNDS", "CA_UNLEARN_NOT_THIS_ABILITY",
        "CA_UNLEARN_NOT_IN_COMBAT", "CA_UNLEARN_NOT_KNOWN", "CA_UNLEARN_NO_UNLEARN_ITEM",
        "CA_UNLEARN_MISSING_CONNECTED_ENTRIES", "CA_UNLEARN_MASTERY_ID", "CA_UNLEARN_MISSING_REQUIRED_ID",
        "CA_UNLEARN_NO_SCROLL_OF_FORTUNE", "CA_UNLEARN_LOCKED", "CA_UNLEARN_WILDCARD_UNSPENT_AE",
        "CA_UNLEARN_WILDCARD_UNSPENT_TE", "CA_UNLEARN_BUILD_CREATOR", "CA_UNLEARN_NOT_ENOUGH_INVESTED_AE",
        "CA_UNLEARN_NOT_ENOUGH_INVESTED_TE", "CA_UNLEARN_NOT_ENOUGH_INVESTED_POINTS",
        "CA_UNLEARN_SCROLL_OF_FORTUNE_LIMIT", "CA_UNLEARN_NOT_WILDCARD", "CA_UNLEARN_MAX"};   // 0x10B204C8

    template <size_t N>
    int IndexOf(const std::string& s, const char* const (&names)[N])
    {
        for (size_t i = 0; i < N; ++i)
            if (s == names[i])
                return static_cast<int>(i);
        return -1;
    }

    const char* const kRollResults[3] = {"ROLL_ABILITIES_OK", "ROLL_ABILITIES_NO_TOKENS", "ROLL_ABILITIES_NO_ROLL"};   // 0x10B60C60
    const char* const kStopCodes[7] = {"STOP_RAPID_ROLLING_NONE", "STOP_RAPID_ROLLING_UNKNOWN",
        "STOP_RAPID_ROLLING_UNHANDLED", "STOP_RAPID_ROLLING_DESIRED_ENTRY_LEARNED", "STOP_RAPID_ROLLING_COUNT_DEPLETED",
        "STOP_RAPID_ROLLING_TALENT_UPGRADE_LEARNED", "STOP_RAPID_ROLLING_TALENT_UPGRADE_BONUS_LEARNED"};   // 0x10B607D4

    // ---- bindings: start / continue / cancel / selections -------------------------------------------
    int PushResult(lua_State* L, const std::pair<bool, std::string>& r)   // FUN_10a2a810
    {
        PushBool(L, r.first);
        if (r.second.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, r.second.c_str());
        return 2;
    }

    int StartRapidRolling(lua_State* L) { return PushResult(L, Start(false)); }   // FUN_10a2e710

    int ContinueRapidRolling(lua_State* L)   // FUN_10a2bc40 -> FUN_10a34250
    {
        if (g_session.phase == 5)
            return PushResult(L, Start(true));
        return PushResult(L, {false, g_session.reason.empty() ? "RAPID_ROLLING_NOT_AWAITING_CONTINUE" : g_session.reason});
    }

    int CancelRapidRolling(lua_State* L)   // FUN_10a2bb30 -> FUN_10a33840
    {
        CancelSession();
        return PushResult(L, {true, std::string()});
    }

    // FUN_10a2aba0: the table's values (lua_next order, FUN_10a29b80), each {ID = number, Type = kind
    // name}; false when a Type is not a kind. A value that is not a table empties the list (logged by
    // the original) without failing it.
    bool ReadSelections(lua_State* L, int index, std::vector<AscWildcardSets::Selection>& out)
    {
        if (AscLua::lua_type(L, index) != LUA_TTABLE)
            return false;
        out.clear();
        bool ok = true;
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, index))
        {
            if (AscLua::lua_type(L, -1) != LUA_TTABLE)
            {
                AscLua::lua_settop(L, -3);
                out.clear();
                return true;
            }
            AscLua::lua_getfield(L, -1, "ID");
            const uint32_t id = AscLua::lua_isnumber(L, -1) ? static_cast<uint32_t>(AscLua::lua_tonumber(L, -1)) : 0;
            AscLua::lua_getfield(L, -2, "Type");
            const char* type = AscLua::lua_isstring(L, -1) ? AscLua::lua_tolstring(L, -1, nullptr) : nullptr;
            uint8_t kind = 0;
            if (!AscWildcardSets::KindIndex(type ? type : "", kind))
                ok = false;
            else if (ok)
                out.push_back({id, kind});
            AscLua::lua_settop(L, -4);   // ID, Type, the value; the key stays for lua_next
        }
        if (!ok)
            out.clear();
        return ok;
    }

    int SetRapidRollingSelections(lua_State* L)   // FUN_10a2e4b0 -> FUN_10a368b0
    {
        std::vector<AscWildcardSets::Selection> desired, undesired;
        if (AscLua::lua_gettop(L) < 2 || !ReadSelections(L, 1, desired) || !ReadSelections(L, 2, undesired))
            return PushResult(L, {false, "INVALID_SELECTIONS"});
        const uint32_t phase = g_session.phase;
        if (phase >= 1 && phase <= 5)
            return PushResult(L, {false, "RAPID_ROLLING_SESSION_ACTIVE"});
        if (g_cancelFrom != 0)
            return PushResult(L, {false, "RAPID_ROLLING_REQUEST_IN_FLIGHT"});
        AscWildcardSets::ApplySelections(desired, undesired);
        return PushResult(L, {true, std::string()});
    }

    int UnlearnAbility(lua_State* L)   // FUN_10a2e800: CMSG 0x61D, u32 entry
    {
        if (Active() || InFlight())
        {
            PushBool(L, false);
            return 1;
        }
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Packet(0x61D).U32(id).Send();
        PushBool(L, true);
        return 1;
    }

    int RollAbilities(lua_State* L)   // FUN_10a2dbe0: CMSG 0x643 with nothing desired
    {
        if (!InFlight() && (!Active() || Awaiting()) && !AscWildcardRolls::RevealsPending())
        {
            SendRoll(0, {}, {}, false);
            PushBool(L, true);
            return 1;
        }
        PushBool(L, false);
        return 1;
    }

    // FUN_10a2c5a0: `count` random entry ids with distinct icons -- every indexed, realm-visible,
    // class-allowed row, five times over, shuffled (std::mt19937 from random_device).
    int GetRollIcons(lua_State* L)
    {
        uint32_t count;
        if (!ReadNumber(L, count))
            return 0;
        std::vector<std::pair<uint32_t, const char*>> list;
        for (Row r : AscCA::IndexedRows())
        {
            const char* icon = AscCA::RowIconPtr(r);
            if (!icon || !*icon)
                continue;
            if (std::any_of(list.begin(), list.end(), [&](const auto& e) { return e.second == icon; }))
                continue;
            if (AscCA::RowVisible(r) && AscCA::ClassAllowedForPlayer(r))
                list.emplace_back(AscCA::RowU32(r, 0), icon);
        }
        const std::vector<std::pair<uint32_t, const char*>> once = list;
        for (int i = 0; i < 4; ++i)   // FUN_10a29590 x4
            list.insert(list.end(), once.begin(), once.end());
        std::random_device rd;
        std::mt19937 rng(rd());
        for (size_t i = 1; i < list.size(); ++i)
        {
            std::uniform_int_distribution<size_t> pick(0, i);
            const size_t j = pick(rng);
            if (j != i)
                std::swap(list[i], list[j]);
        }
        if (count < list.size())
            list.resize(count);
        AscLua::lua_createtable(L, 0, static_cast<int>(list.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < list.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushnumber(L, list[i].first);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_10a2ccd0: the build's ability entries in learn order (+0x18 key ascending, unset last),
    // at most the starting count, each as FUN_10a2a870's table.
    int GetStartingChoiceEntries(lua_State* L)
    {
        const AscCA::Build* b = AscCA::ActiveBuild();
        if (!b || !AscWildcardRolls::CanShowStartingChoice())
        {
            AscLua::lua_createtable(L, 0, 0);
            return 1;
        }
        std::vector<AscCA::Entry> picks;
        for (const AscCA::Entry& e : b->entries)
        {
            Row r = e.row ? e.row : AscCA::FindRow(e.id);
            if (!r)
                continue;
            const int t = AscCA::RowType(r);
            if (t == 1 || t == 4)
                picks.push_back(e);
        }
        auto key = [](const AscCA::Entry& e) { return (static_cast<uint64_t>(e.u1c) << 32) | e.u18; };
        std::stable_sort(picks.begin(), picks.end(), [&](const AscCA::Entry& x, const AscCA::Entry& y) {
            const uint64_t kx = key(x), ky = key(y);
            return kx != 0 && (ky == 0 || kx < ky);
        });
        const uint32_t limit = AscWildcardRolls::StartingCount();
        if (limit < picks.size())
            picks.resize(limit);
        AscLua::lua_createtable(L, 0, static_cast<int>(picks.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < picks.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int>(i + 1));
            const AscCA::Entry& e = picks[i];
            Row r = e.row ? e.row : AscCA::FindRow(e.id);
            uint32_t spell = AscCA::EntryCurrentSpell(*b, e.id);   // FUN_101528d0
            if (!spell && r)
                spell = AscCA::SpellAtRankOf(r, e.rank);          // FUN_101c62e0
            AscLua::lua_createtable(L, 0, 9);
            auto num = [L](const char* k, uint32_t v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushinteger(L, static_cast<int>(v)); AscLua::lua_settable(L, -3); };
            auto boo = [L](const char* k, bool v) { AscLua::lua_pushstring(L, k); PushBool(L, v); AscLua::lua_settable(L, -3); };
            num("EntryID", e.id);
            num("EntryId", e.id);
            num("internalID", e.id);
            num("SpellID", spell);
            num("Rank", e.rank);
            num("LearnedRank", e.u0c);
            AscLua::lua_pushstring(L, "Quality");
            if (r)
                PushStr(L, AscCA::QualityName(AscCA::QualityIndexOf(*b, r)).c_str());
            else
                PushStr(L, "");
            AscLua::lua_settable(L, -3);
            boo("IsLocked", e.locked != 0);
            boo("Locked", e.locked != 0);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // ---- bindings --------------------------------------------------------------------------------
    int IsRapidRollingEnabled(lua_State* L) { PushBool(L, QuickRollingEnabled()); return 1; }
    int IsAwaitingRapidRollingTalentUpgradeRoll(lua_State* L) { PushBool(L, Awaiting()); return 1; }

    int IsRollRequestBlocked(lua_State* L)   // handler_IsRollRequestBlocked
    {
        bool blocked = true;
        if (!InFlight() && (!Active() || Awaiting()))
            blocked = AscWildcardRolls::RevealsPending();
        PushBool(L, blocked);
        return 1;
    }

    int CanUseRapidRolling(lua_State* L)   // FUN_10a2b9a0
    {
        const uint32_t code = CanUse();
        PushBool(L, code == 0);
        if (code == 0)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, code < 7 ? kCanUse[code] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(code)).c_str());
        return 2;
    }

    int CanStartRapidRolling(lua_State* L)   // FUN_10a2b8c0
    {
        const auto r = CanStart();
        PushBool(L, r.first);
        if (r.second.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, r.second.c_str());
        return 2;
    }

    // FUN_10a34740: count, this config's threshold, the next config's threshold and max; nils once the
    // next threshold is reached.
    int Breakpoint(lua_State* L, bool isAbility)
    {
        const uint32_t count = AscWildcardRolls::SpecCounter(isAbility ? 0 : 1);
        const QuickRoll* next = Config(isAbility, true);
        const uint32_t nextThreshold = next ? next->threshold : 0;
        if (nextThreshold <= count)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 3;
        }
        AscLua::lua_pushinteger(L, static_cast<int>(count));
        AscLua::lua_pushinteger(L, static_cast<int>(nextThreshold));
        AscLua::lua_pushinteger(L, static_cast<int>(next ? next->max : 0));
        return 3;
    }
    int GetRapidRollAbilityBreakpointInfo(lua_State* L) { return Breakpoint(L, true); }
    int GetRapidRollTalentBreakpointInfo(lua_State* L) { return Breakpoint(L, false); }

    int GetMaximumRapidRolls(lua_State* L)   // handler_GetMaximumRapidRolls
    {
        AscLua::lua_pushinteger(L, static_cast<int>(MaxRolls(NextIsAbility())));
        return 1;
    }

    int GetNextUnlearnedID(lua_State* L)   // handler_GetNextUnlearnedID
    {
        AscWildcardSets::Next n;
        if (NextUnlearn(n))
            AscLua::lua_pushinteger(L, static_cast<int>(n.id));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_10a2c190: the session as a table (13 keys).
    int GetRapidRollingState(lua_State* L)
    {
        const auto can = CanStart();
        AscLua::lua_createtable(L, 0, 0xD);
        AscLua::lua_checkstack(L, 2);
        auto num = [L](const char* k, double v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushnumber(L, v); AscLua::lua_settable(L, -3); };
        auto str = [L](const char* k, const std::string& v) { AscLua::lua_pushstring(L, k); PushStr(L, v.c_str()); AscLua::lua_settable(L, -3); };
        auto boo = [L](const char* k, bool v) { AscLua::lua_pushstring(L, k); PushBool(L, v); AscLua::lua_settable(L, -3); };
        num("SessionID", static_cast<double>(g_session.id));
        str("Phase", g_session.phase < 9 ? kPhases[g_session.phase] : "Unknown");
        boo("IsActive", Active());
        num("TargetEntryID", g_session.target);
        boo("IsAbility", g_session.isAbility);
        num("RollCount", g_session.rollCount);
        str("Reason", g_session.reason);
        num("LearnedEntryID", g_session.learned);
        num("NewRank", g_session.newRank);
        num("PreRollRank", g_session.preRollRank);
        str("StopCode", g_session.stopCode);
        boo("IsDesired", g_session.isDesired);
        boo("CanStart", can.first);
        str("CanStartReason", can.second);
        return 1;
    }

    // SMSG 0x6F0 (FUN_10a34860): u32 n x {u8 kind, u32 threshold, u32 max}.
    void __cdecl OnQuickRollConfigs(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t n;
        memcpy(&n, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        g_quick.resize(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint8_t* b = reinterpret_cast<const uint8_t*>(p->m_buffer) + p->m_read;
            g_quick[i].kind = b[0];
            memcpy(&g_quick[i].threshold, b + 1, 4);
            memcpy(&g_quick[i].max, b + 5, 4);
            p->m_read += 9;
        }
    }

    void OnGlueScreen()   // 0x10A33940 (the session part)
    {
        const uint64_t id = g_session.id;
        g_session = Session{};
        g_session.id = id;
        g_cancelFrom = 0;
        g_rerollPending = false;   // FUN_10a2f950
    }

    // FUN_10a35270 (CA list 0x10be2e18, a spec change): the session is cancelled and both lists empty.
    void OnSpecChanged(uint32_t, uint32_t)
    {
        CancelSession();
        AscWildcardSets::ClearForSpecChange();
    }

    void ResetOnEnterWorld() { AscWildcardRapid::ResetSession(); }   // FUN_10a33c90 -> FUN_10a33b10

    void Init()
    {
        AscRuntime::OnEnterWorld(&ResetOnEnterWorld);
        AscCA::OnSpecChanged(OnSpecChanged);
        sDC.AddPacketHandler(0x6F0, CNetClientCustomPacket((void*)&OnQuickRollConfigs, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Wildcard", "IsRapidRollingEnabled", IsRapidRollingEnabled},
        {"C_Wildcard", "IsAwaitingRapidRollingTalentUpgradeRoll", IsAwaitingRapidRollingTalentUpgradeRoll},
        {"C_Wildcard", "IsRollRequestBlocked", IsRollRequestBlocked},
        {"C_Wildcard", "CanUseRapidRolling", CanUseRapidRolling},
        {"C_Wildcard", "CanStartRapidRolling", CanStartRapidRolling},
        {"C_Wildcard", "GetRapidRollAbilityBreakpointInfo", GetRapidRollAbilityBreakpointInfo},
        {"C_Wildcard", "GetRapidRollTalentBreakpointInfo", GetRapidRollTalentBreakpointInfo},
        {"C_Wildcard", "GetMaximumRapidRolls", GetMaximumRapidRolls},
        {"C_Wildcard", "GetNextUnlearnedID", GetNextUnlearnedID},
        {"C_Wildcard", "GetRapidRollingState", GetRapidRollingState},
        {"C_Wildcard", "StartRapidRolling", StartRapidRolling},
        {"C_Wildcard", "ContinueRapidRolling", ContinueRapidRolling},
        {"C_Wildcard", "CancelRapidRolling", CancelRapidRolling},
        {"C_Wildcard", "SetRapidRollingSelections", SetRapidRollingSelections},
        {"C_Wildcard", "UnlearnAbility", UnlearnAbility},
        {"C_Wildcard", "RollAbilities", RollAbilities},
        {"C_Wildcard", "GetRollIcons", GetRollIcons},
        {"C_Wildcard", "GetStartingChoiceEntries", GetStartingChoiceEntries},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscWildcardRapid
{
    bool Active() { return ::Active(); }
    bool InFlight() { return ::InFlight(); }
    void MarkRerollPending() { g_rerollPending = true; }   // FUN_10a2f910

    void ResetSession()
    {
        g_session = Session{};   // FUN_10a327e0 with a default record
        g_cancelFrom = 0;
    }

    bool TakeRerollPending()   // SMSG 0x61E reads and clears DAT_10d3d0b0
    {
        const bool was = g_rerollPending;
        g_rerollPending = false;
        return was;
    }

    // FUN_10a35c20: SMSG 0x61E inside a session. A drained cancel or a WaitingForUnlearn session
    // consumes it; success sends the session's roll (CMSG 0x643) and waits for it.
    bool OnUnlearnResult(const std::string& result)
    {
        if (g_cancelFrom == 1)
        {
            Drain("CANCELLED_UNLEARN_RESULT_DRAINED");
            return true;
        }
        if (g_session.phase != 1)
            return false;
        const int code = IndexOf(result, kUnlearnResults);
        if (code != 0)
        {
            const uint32_t c = code < 0 ? 1 : static_cast<uint32_t>(code);   // FUN_100999d0
            SetPhase(7, c < 23 ? kUnlearnResults[c] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(c));
            return true;
        }
        AscRuntime::Signal("WILDCARD_RAPID_ROLL_UNLEARNED", "%u%u", g_session.target, g_session.startRank);
        if (g_session.target == 0)
        {
            SetPhase(7, "RAPID_ROLLING_SEND_FAILED");
            return true;
        }
        SendRoll(g_session.rollCount, g_session.ids, g_session.tags, g_session.isAbility);
        SetPhase(2, kUnlearnResults[0]);
        return true;
    }

    // FUN_10a359c0: SMSG 0x644 inside a session. Success is ROLL_ABILITIES_OK or CAN_START_RAPID_ROLLING_OK.
    bool OnRollResult(const std::string& result)
    {
        const bool ok = IndexOf(result, kRollResults) == 0 || IndexOf(result, kCanStart) == 0;
        if (g_cancelFrom == 2)
        {
            if (!ok)
                Drain("CANCELLED_ROLL_RESULT_DRAINED");
            else
                g_cancelFrom = 3;
            return true;
        }
        if (g_session.phase != 2)
            return false;
        if (!ok)
            SetPhase(7, result.empty() ? "ROLL_ABILITIES_NO_ROLL" : result);
        else
            SetPhase(3, result);
        return true;
    }

    // FUN_10a355d0: SMSG 0x621 for the session -- a stop code (or the bonus roll's learn) moves it
    // to Revealing.
    void OnEntryLearned(uint32_t entry, uint32_t newRank, uint32_t preRoll, const std::string& stop)
    {
        int code = IndexOf(stop, kStopCodes);   // FUN_10a2f5e0
        const bool bonus = (g_session.phase == 2 || g_session.phase == 3)
            && g_session.stopCode == "STOP_RAPID_ROLLING_TALENT_UPGRADE_BONUS_PENDING" && (code == 0 || code == 5);
        if (code == 0 && !bonus)
            return;
        const std::string reached = bonus ? "STOP_RAPID_ROLLING_TALENT_UPGRADE_BONUS_LEARNED" : stop;
        if (bonus)
            code = 6;
        if (g_cancelFrom == 2 || g_cancelFrom == 3)
            Drain("CANCELLED_ENTRY_LEARNED_DRAINED");
        else if (g_session.phase == 3 || g_session.phase == 2)
        {
            g_session.learned = entry;
            g_session.newRank = newRank;
            g_session.preRollRank = preRoll;
            g_session.stopCode = reached;
            g_session.isDesired = code == 3;
            SetPhase(4, reached);
        }
    }

    // FUN_10a35870: the reveal of the learned entry ended.
    void OnRevealDone(uint32_t entry)
    {
        if (g_session.phase != 4 || g_session.learned != entry)
            return;
        switch (IndexOf(g_session.stopCode, kStopCodes))
        {
        case 3:
            SetPhase(6, g_session.stopCode);
            break;
        case 4:
        case 6:
            SetPhase(5, g_session.stopCode);
            break;
        case 5:
            if (!TalentUpgradeRoll())
                SetPhase(7, "RAPID_ROLLING_TALENT_UPGRADE_ROLL_SEND_FAILED");
            break;
        default:
            SetPhase(7, g_session.stopCode);
        }
    }
}
