// C_CharacterAdvancement -- the CoA advancement tree, reconstructed over state we already receive.
//
// Manager DAT_10bde440 in the original (accessor FUN_101722e0), 68 bindings. Unlike the build
// browser this subsystem has a real data source on both sides already:
//
//   * SMSG_CA_ACTIVE_SPEC   (0x725) and SMSG_CA_KNOWN_ENTRIES (0x726) are parsed by AscensionCA.cpp
//     into AscensionCASvc::Get(); that side table was built for exactly this and was previously
//     never read by anything.
//   * CharacterAdvancement.dbc supplies each entry's AE/TE cost, level, class and tab, so the
//     essence accounting below is computed from real rows rather than invented.
//   * CMSG_CHARACTER_ADVANCEMENT_KNOWN_ENTRIES (0x727) is parsed server-side by
//     ParseKnownEntriesUpload as `u32 count` followed by 21-byte records whose first two dwords are
//     entry and rank -- so ApplyPendingBuild is a genuine round trip, not a local no-op.
//
// The pending build is client-side staging: ranks the player has selected but not yet committed.
// Reads overlay pending on top of known, which is what the tree UI expects while editing.
#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscArgs.hpp>
#include <Ascension/AscensionCA.hpp>
#include <Ascension/AscCharacterAdvancement.hpp>
#include <Ascension/AscClassInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Client/ClientServices.hpp>
#include <map>
#include <vector>
#include <string>
#include <algorithm>

#define CMSG_CA_KNOWN_ENTRIES 0x0727

namespace AscCharAdvancementApi
{
namespace
{
    constexpr int GLOBALS = -10002;

    struct Staging
    {
        std::map<uint32_t, uint32_t> pending;      // entryId -> staged rank
        std::vector<uint32_t> suggestionOverrides;
        std::map<int, std::vector<uint32_t>> filteredByCategory;
        uint32_t inspectedFrom = 0;
    };

    Staging& S()
    {
        static Staging s;
        return s;
    }

    // The essence budget comes from CharacterAdvancementEssence.dbc keyed by (classType, level) --
    // our server never sends SMSG_CA_ESSENCE_BUDGET (0x722), measured: zero such packets in a
    // session. Without it the panel believed there was nothing to spend.
    //
    // The class and level arrive from Lua via Asc_SetPlayerContext, because this DLL has no bound
    // lua_pcall to call UnitClass/UnitLevel itself.
    uint32_t g_playerClassType = 0;
    uint32_t g_playerLevel = 0;

    bool Budget(uint32_t& ae, uint32_t& te)
    {
        ae = te = 0;
        if (!g_playerClassType || !g_playerLevel) return false;
        return AscCA_EssenceForLevel(g_playerClassType, g_playerLevel, ae, te);
    }

    uint32_t KnownRank(uint32_t id)
    {
        for (const auto& k : AscensionCASvc::Get().known)
            if (k.entry == id) return k.rank;
        return 0;
    }

    bool IsStaged(uint32_t id) { return S().pending.find(id) != S().pending.end(); }

    // What the tree should draw: the staged rank if the player has touched this node, else what the
    // server says they know.
    uint32_t EffectiveRank(uint32_t id)
    {
        auto it = S().pending.find(id);
        return it != S().pending.end() ? it->second : KnownRank(id);
    }

    uint32_t MaxRank(const AscCAEntry& e)
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < 5; ++i)
            if (e.spells[i]) ++n;
        return n ? n : 1;
    }

    int PushBool(lua_State* L, bool v) { AscLua::lua_pushboolean(L, v ? 1 : 0); return 1; }
    int PushNum(lua_State* L, double v) { AscLua::lua_pushnumber(L, v); return 1; }
    int PushNil(lua_State* L) { AscLua::lua_pushnil(L); return 1; }

    void Field(lua_State* L, const char* k, double v)
    {
        AscLua::lua_pushnumber(L, v);
        AscLua::lua_setfield(L, -2, k);
    }
    void FieldB(lua_State* L, const char* k, bool v)
    {
        AscLua::lua_pushboolean(L, v ? 1 : 0);
        AscLua::lua_setfield(L, -2, k);
    }

    // ---- essence accounting ------------------------------------------------------------------
    struct Spend { uint32_t ae = 0; uint32_t te = 0; uint32_t points = 0; };

    // Charge a node once per rank held, from the DBC costs. An entry carrying a TE cost is charged
    // against talent essence, otherwise ability essence -- the same split the server applies.
    Spend SpendOver(bool stagedOnly, int tabFilter, uint32_t classFilter)
    {
        Spend out;
        auto charge = [&](uint32_t id, uint32_t rank)
        {
            if (!rank) return;
            AscCAEntry e;
            if (!AscCA_FindEntry(id, e)) return;
            if (tabFilter >= 0 && static_cast<int>(e.tab) != tabFilter) return;
            if (classFilter && e.classType != classFilter) return;
            out.points += rank;
            if (e.teCost) out.te += e.teCost * rank;
            else out.ae += e.aeCost * rank;
        };

        if (stagedOnly)
        {
            for (const auto& kv : S().pending) charge(kv.first, kv.second);
        }
        else
        {
            for (const auto& k : AscensionCASvc::Get().known)
                if (!IsStaged(k.entry)) charge(k.entry, k.rank);
            for (const auto& kv : S().pending) charge(kv.first, kv.second);
        }
        return out;
    }

    // ---- known-entry queries -----------------------------------------------------------------
    int IsKnownID(lua_State* L)
    {
        int id = 0;
        return PushBool(L, AscArgs::GetInt(L, 1, id) && KnownRank(static_cast<uint32_t>(id)) > 0);
    }

    int IsPendingEntryID(lua_State* L)
    {
        int id = 0;
        return PushBool(L, AscArgs::GetInt(L, 1, id) && IsStaged(static_cast<uint32_t>(id)));
    }

    int IsPending(lua_State* L) { return PushBool(L, !S().pending.empty()); }

    // Returns rank AND maxRank: CATalentBaseMixin does
    //   self.rank, self.maxRank = GetPendingRankByEntryID(id)
    // and then TOOLTIP_TALENT_RANK:format(self.rank, self.maxRank), so a single return leaves
    // maxRank nil and every node tooltip throws.
    int GetPendingRankByEntryID(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id))
        {
            AscLua::lua_pushnumber(L, 0);
            AscLua::lua_pushnumber(L, 0);
            return 2;
        }
        const uint32_t key = static_cast<uint32_t>(id);
        AscCAEntry e;
        const int maxRank = AscCA_FindEntry(key, e) ? static_cast<int>(MaxRank(e)) : 1;
        AscLua::lua_pushnumber(L, EffectiveRank(key));
        AscLua::lua_pushnumber(L, maxRank);
        return 2;
    }

    // UnitKnownID/UnitTalentRankByID answer for other units in the original; we only hold the
    // player's own set, so a non-player unit reports nothing rather than the player's data.
    bool UnitIsPlayer(lua_State* L, int idx)
    {
        std::string unit;
        if (!AscArgs::GetString(L, idx, unit)) return false;
        return unit == "player" || unit == "Player";
    }

    int UnitKnownID(lua_State* L)
    {
        int id = 0;
        if (!UnitIsPlayer(L, 1) || !AscArgs::GetInt(L, 2, id)) return PushBool(L, false);
        return PushBool(L, KnownRank(static_cast<uint32_t>(id)) > 0);
    }

    int UnitTalentRankByID(lua_State* L)
    {
        int id = 0;
        if (!UnitIsPlayer(L, 1) || !AscArgs::GetInt(L, 2, id)) return PushNum(L, 0);
        return PushNum(L, KnownRank(static_cast<uint32_t>(id)));
    }

    // A node's spells are known when the node is; IsSpellIDKnown maps the spell back to its entry.
    bool SpellKnown(uint32_t spellId)
    {
        for (const auto& k : AscensionCASvc::Get().known)
        {
            if (!k.rank) continue;
            AscCAEntry e;
            if (!AscCA_FindEntry(k.entry, e)) continue;
            for (uint32_t i = 0; i < 5 && i < k.rank; ++i)
                if (e.spells[i] == spellId) return true;
        }
        return false;
    }

    int IsSpellIDKnown(lua_State* L)
    {
        int id = 0;
        return PushBool(L, AscArgs::GetInt(L, 1, id) && SpellKnown(static_cast<uint32_t>(id)));
    }
    int IsKnownSpellID(lua_State* L) { return IsSpellIDKnown(L); }

    void PushEntryList(lua_State* L, bool talentsOnly, uint32_t classFilter)
    {
        AscLua::lua_createtable(L, 0, 0);
        int n = 1;
        for (const auto& k : AscensionCASvc::Get().known)
        {
            if (!k.rank) continue;
            AscCAEntry e;
            if (!AscCA_FindEntry(k.entry, e)) continue;
            if (classFilter && e.classType != classFilter) continue;
            if (talentsOnly && !e.teCost) continue;
            if (!talentsOnly && e.teCost) continue;
            AscLua::lua_pushinteger(L, n++);
            AscLua::lua_createtable(L, 0, 7);
            Field(L, "ID", e.id);
            Field(L, "EntryID", e.id);
            Field(L, "Rank", k.rank);
            Field(L, "Level", e.level);
            Field(L, "Tab", e.tab);
            Field(L, "ClassType", e.classType);
            Field(L, "SpellID", e.spells[0]);
            AscLua::lua_settable(L, -3);
        }
    }

    int GetKnownTalentEntries(lua_State* L) { PushEntryList(L, true, 0); return 1; }
    int GetKnownSpellEntries(lua_State* L)  { PushEntryList(L, false, 0); return 1; }

    int GetKnownTalentEntriesForClass(lua_State* L)
    {
        int cls = 0;
        AscArgs::OptInt(L, 1, cls, 0);
        PushEntryList(L, true, static_cast<uint32_t>(cls));
        return 1;
    }
    int GetKnownSpellEntriesForClass(lua_State* L)
    {
        int cls = 0;
        AscArgs::OptInt(L, 1, cls, 0);
        PushEntryList(L, false, static_cast<uint32_t>(cls));
        return 1;
    }

    // ---- specs -------------------------------------------------------------------------------
    // DERIVED from what the character has learned, not from the packet.
    //
    // The original does the same: handler_GetActiveChrSpec reads mgr+0x24 and maps it through
    // FUN_1016f480 rather than reading the SMSG_CA_ACTIVE_SPEC field. That matters here because our
    // server sends that field hardcoded (`packet << uint32(0) << uint32(1)` in AscensionCompat.cpp)
    // -- and the JS client still remembers the choice against that same server, which is only
    // possible if the spec is derived client-side.
    //
    // A learned node belongs to a tab; every tab except "Class" IS a specialization, and the tab
    // name matches the ChrSpecs spec name. So: known entries -> their tab -> tab name -> spec id.
    // A local switch still wins while the panel is open, so clicking a spec takes effect at once.
    uint32_t DerivedActiveSpec()
    {
        for (const auto& k : AscensionCASvc::Get().known)
        {
            if (!k.rank) continue;
            AscCAEntry e;
            if (!AscCA_FindEntry(k.entry, e) || !e.tab) continue;
            const char* tabName = AscCA_TabNameById(e.tab);
            if (!tabName || !*tabName || _stricmp(tabName, "Class") == 0) continue;
            const unsigned int specId = AscClassInfo::SpecIdByName(tabName);
            if (specId) return specId;
        }
        return 0;
    }

    uint32_t ActiveSpec()
    {
        const uint32_t local = AscensionCASvc::Get().activeSpecIndex;
        if (local) return local;
        return DerivedActiveSpec();
    }

    int GetActiveChrSpec(lua_State* L) { return PushNum(L, ActiveSpec()); }
    int GetActiveSpecID(lua_State* L)  { return PushNum(L, ActiveSpec()); }

    // Takes the SPEC ID. CoASpecChoiceMixin:UpdateVisualState greys a spec out entirely when this
    // is false, so the old test -- specSlotCount > 1 -- disabled every spec, because the server
    // sends specSlotCount = 1 in SMSG_CA_ACTIVE_SPEC. A spec is switchable when it exists, is not
    // already the active one, and nothing is staged.
    int CanSwitchActiveChrSpec(lua_State* L)
    {
        if (!S().pending.empty()) return PushBool(L, false);
        int specId = 0;
        if (!AscArgs::GetInt(L, 1, specId)) return PushBool(L, true);
        return PushBool(L, specId > 0 && static_cast<uint32_t>(specId) != ActiveSpec());
    }

    // CoATalentFrameMixin:ChangeSpecID sets expectingNewSpec, calls this, and then does NOTHING
    // until the event CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED arrives -- which is why returning
    // false here made clicking a specialization appear to do nothing at all.
    //
    // The original fires that event BY NAME (FUN_10182780 builds the string and calls FUN_100b94b0,
    // which checks a name registry via FUN_10278ab0, resolves the name to a real client event id
    // via FUN_10278c90, and signals it through DAT_10bc93b8). Reproducing that registry is its own
    // job and would serve every custom event, not just this one. Until then, notify the frame
    // directly: the Ascension mixins dispatch events to a method of the same name, so calling it is
    // what the event would have done.
    int SwitchActiveChrSpec(lua_State* L)
    {
        int specId = 0;
        if (!AscArgs::GetInt(L, 1, specId) || specId <= 0)
            return PushBool(L, false);

        AscensionCASvc::Get().activeSpecIndex = static_cast<uint32_t>(specId);
        AscLog::Printf("SwitchActiveChrSpec: active spec -> %d", specId);

        AscLua::Execute(
            "if CoATalentFrame then "
            "  if CoATalentFrame.CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED then "
            "    CoATalentFrame:CHARACTER_ADVANCEMENT_PENDING_BUILD_UPDATED() end "
            "  if CoATalentFrame.UpdateActiveSpec then CoATalentFrame:UpdateActiveSpec() end "
            "end",
            "Extensions:ca-spec-switch", nullptr);
        return PushBool(L, true);
    }

    // ---- staging -----------------------------------------------------------------------------
    bool CanAdd(uint32_t id, AscCAEntry& e)
    {
        if (!AscCA_FindEntry(id, e)) return false;
        return EffectiveRank(id) < MaxRank(e);
    }

    // Returns (canLearn, cantLearnReason) -- CATalentBaseMixin assigns both and shows the reason
    // in the node tooltip.
    int CanAddByEntryID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
        {
            AscLua::lua_pushboolean(L, 0);
            AscLua::lua_pushstring(L, "");
            return 2;
        }
        const uint32_t key = static_cast<uint32_t>(id);
        const char* reason = "";
        bool ok = true;
        if (EffectiveRank(key) >= MaxRank(e))
        {
            ok = false;
            reason = "Already at maximum rank.";
        }
        else
        {
            for (uint32_t i = 0; i < 5 && ok; ++i)
                if (e.connected[i] && EffectiveRank(e.connected[i]) == 0)
                {
                    ok = false;
                    reason = "Requires a connected node first.";
                }
        }
        AscLua::lua_pushboolean(L, ok ? 1 : 0);
        AscLua::lua_pushstring(L, reason);
        return 2;
    }

    int AddByEntryID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !CanAdd(static_cast<uint32_t>(id), e))
            return PushBool(L, false);
        const uint32_t key = static_cast<uint32_t>(id);
        S().pending[key] = EffectiveRank(key) + 1;
        return PushBool(L, true);
    }

    int CanRemoveByEntryID(lua_State* L)
    {
        int id = 0;
        return PushBool(L, AscArgs::GetInt(L, 1, id) && EffectiveRank(static_cast<uint32_t>(id)) > 0);
    }

    int RemoveByEntryID(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return PushBool(L, false);
        const uint32_t key = static_cast<uint32_t>(id);
        const uint32_t cur = EffectiveRank(key);
        if (!cur) return PushBool(L, false);
        S().pending[key] = cur - 1;
        return PushBool(L, true);
    }

    int CanSwapEntriesByID(lua_State* L)
    {
        int a = 0, b = 0;
        if (!AscArgs::GetInt(L, 1, a) || !AscArgs::GetInt(L, 2, b)) return PushBool(L, false);
        AscCAEntry ea, eb;
        return PushBool(L, AscCA_FindEntry(static_cast<uint32_t>(a), ea)
                        && AscCA_FindEntry(static_cast<uint32_t>(b), eb)
                        && ea.tab == eb.tab);
    }

    int SwapEntriesByID(lua_State* L)
    {
        int a = 0, b = 0;
        if (!AscArgs::GetInt(L, 1, a) || !AscArgs::GetInt(L, 2, b)) return PushBool(L, false);
        const uint32_t ka = static_cast<uint32_t>(a), kb = static_cast<uint32_t>(b);
        const uint32_t ra = EffectiveRank(ka), rb = EffectiveRank(kb);
        S().pending[ka] = rb;
        S().pending[kb] = ra;
        return PushBool(L, true);
    }

    int CanClearPendingBuild(lua_State* L) { return PushBool(L, !S().pending.empty()); }

    int ClearPendingBuild(lua_State* L)
    {
        (void)L;
        S().pending.clear();
        return 0;
    }

    int ClearPendingBuildByTab(lua_State* L)
    {
        int tab = 0;
        if (!AscArgs::GetInt(L, 1, tab)) return 0;
        for (auto it = S().pending.begin(); it != S().pending.end();)
        {
            AscCAEntry e;
            if (AscCA_FindEntry(it->first, e) && static_cast<int>(e.tab) == tab)
                it = S().pending.erase(it);
            else
                ++it;
        }
        return 0;
    }

    int CancelPendingBuild(lua_State* L)
    {
        (void)L;
        S().pending.clear();
        return 0;
    }

    int IsPendingBuildAvailable(lua_State* L) { return PushBool(L, !S().pending.empty()); }
    int IsActiveBuildAvailable(lua_State* L)  { return PushBool(L, AscensionCASvc::Get().haveKnown); }

    // Returns SEVEN values. CharacterAdvancementUtil.ConfirmApplyPendingBuild does
    //   local canApply, reason, traversalError, entryID, entryRank, marksCost, goldCost = ...
    // then `if marksCost > 0`, so a single return throws "attempt to compare number with nil" the
    // moment the apply dialog opens. Costs are server-side tuning we are not sent; 0 means free,
    // which is at least a number the dialog can reason about.
    int CanApplyPendingBuild(lua_State* L)
    {
        const bool ok = !S().pending.empty() && AscensionCASvc::Get().haveKnown;
        const char* reason = "";
        if (S().pending.empty())
            reason = "Nothing to apply.";
        else if (!AscensionCASvc::Get().haveKnown)
            reason = "Waiting for your known abilities from the server.";

        AscLua::lua_pushboolean(L, ok ? 1 : 0);   // canApply
        AscLua::lua_pushstring(L, reason);        // reason
        AscLua::lua_pushstring(L, "");            // traversalError
        AscLua::lua_pushnumber(L, 0);             // entryID
        AscLua::lua_pushnumber(L, 0);             // entryRank
        AscLua::lua_pushnumber(L, 0);             // marksCost
        AscLua::lua_pushnumber(L, 0);             // goldCost
        return 7;
    }

    // The real round trip: the merged known+staged set, in the exact layout
    // AscensionCoATalentState::ParseKnownEntriesUpload expects (u32 count, then 21-byte records
    // beginning entry, rank).
    int ApplyPendingBuild(lua_State* L)
    {
        if (S().pending.empty()) return PushBool(L, false);

        // Refuse until SMSG_CA_KNOWN_ENTRIES has actually landed. This upload is AUTHORITATIVE --
        // the server replaces the character's known set with whatever we send -- so applying before
        // we know what they already have would upload only the staged nodes and silently wipe the
        // rest. Measured: the probe could apply while CanApplyPendingBuild() was already false.
        if (!AscensionCASvc::Get().haveKnown)
        {
            AscLog::Printf("ApplyPendingBuild: refused, known-entry set not received yet");
            return PushBool(L, false);
        }

        std::map<uint32_t, uint32_t> merged;
        for (const auto& k : AscensionCASvc::Get().known)
            if (k.rank) merged[k.entry] = k.rank;
        for (const auto& kv : S().pending)
        {
            if (kv.second) merged[kv.first] = kv.second;
            else merged.erase(kv.first);
        }

        CDataStore pkt{};
        CDataStore::GenPacket(&pkt);
        CDataStore::PutInt32(&pkt, CMSG_CA_KNOWN_ENTRIES);
        CDataStore::PutInt32(&pkt, static_cast<int32_t>(merged.size()));
        for (const auto& kv : merged)
        {
            CDataStore::PutInt32(&pkt, static_cast<int32_t>(kv.first));
            CDataStore::PutInt32(&pkt, static_cast<int32_t>(kv.second));
            CDataStore::PutInt32(&pkt, 0);
            CDataStore::PutInt8(&pkt, 0);
            CDataStore::PutInt32(&pkt, 0);
            CDataStore::PutInt32(&pkt, 0);
        }
        pkt.m_read = 0;
        ClientServices::SendPacket(&pkt);
        CDataStore::Release(&pkt);

        AscLog::Printf("ApplyPendingBuild -> CMSG 0x727 with %u entries (%u staged)",
                       static_cast<unsigned>(merged.size()), static_cast<unsigned>(S().pending.size()));
        S().pending.clear();
        return PushBool(L, true);
    }

    int LearnID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !CanAdd(static_cast<uint32_t>(id), e))
            return PushBool(L, false);
        const uint32_t key = static_cast<uint32_t>(id);
        S().pending[key] = EffectiveRank(key) + 1;
        return ApplyPendingBuild(L);
    }

    // ---- investment reporting -----------------------------------------------------------------
    int GetPendingGlobalAEInvestment(lua_State* L) { return PushNum(L, SpendOver(false, -1, 0).ae); }
    int GetPendingGlobalTEInvestment(lua_State* L) { return PushNum(L, SpendOver(false, -1, 0).te); }
    int GetPendingClassAEInvestment(lua_State* L)  { return PushNum(L, SpendOver(false, -1, 0).ae); }
    int GetPendingClassTEInvestment(lua_State* L)  { return PushNum(L, SpendOver(false, -1, 0).te); }
    int GetPendingClassPointInvestment(lua_State* L) { return PushNum(L, SpendOver(false, -1, 0).points); }
    int GetPendingExpectedAE(lua_State* L) { return PushNum(L, SpendOver(true, -1, 0).ae); }
    int GetPendingExpectedTE(lua_State* L) { return PushNum(L, SpendOver(true, -1, 0).te); }

    int GetPendingTabAEInvestment(lua_State* L)
    {
        int tab = -1;
        AscArgs::OptInt(L, 1, tab, -1);
        return PushNum(L, SpendOver(false, tab, 0).ae);
    }

    // Pending remainder: the same budget, minus what the staged build would cost.
    int GetPendingRemainingAE(lua_State* L)
    {
        uint32_t ae = 0, te = 0;
        if (!Budget(ae, te)) return PushNum(L, 0);
        const uint32_t spent = SpendOver(false, -1, 0).ae;
        return PushNum(L, ae > spent ? ae - spent : 0);
    }
    int GetPendingRemainingTE(lua_State* L)
    {
        uint32_t ae = 0, te = 0;
        if (!Budget(ae, te)) return PushNum(L, 0);
        const uint32_t spent = SpendOver(false, -1, 0).te;
        return PushNum(L, te > spent ? te - spent : 0);
    }

    int GetPendingSummary(lua_State* L)
    {
        const Spend all = SpendOver(false, -1, 0);
        const Spend staged = SpendOver(true, -1, 0);
        AscLua::lua_createtable(L, 0, 6);
        Field(L, "AE", all.ae);
        Field(L, "TE", all.te);
        Field(L, "Points", all.points);
        Field(L, "PendingAE", staged.ae);
        Field(L, "PendingTE", staged.te);
        Field(L, "NumPending", static_cast<double>(S().pending.size()));
        return 1;
    }

    int GetLowestInvestmentRequired(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushNum(L, 0);
        return PushNum(L, e.teCost ? e.teCost : e.aeCost);
    }

    int MeetsInvestmentForAddByEntryID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushBool(L, false);
        return PushBool(L, SpendOver(false, static_cast<int>(e.tab), 0).points >= e.level);
    }

    // Prerequisites: the node links measured at bytes 415..431, all of which must be held.
    int KnowsConnectedNodesFor(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushBool(L, false);
        for (uint32_t i = 0; i < 5; ++i)
            if (e.connected[i] && EffectiveRank(e.connected[i]) == 0)
                return PushBool(L, false);
        return PushBool(L, true);
    }

    int IsConnectionAllowed(lua_State* L) { return KnowsConnectedNodesFor(L); }

    // ---- filters ------------------------------------------------------------------------------
    int SetFilteredEntriesByCategory(lua_State* L)
    {
        int cat = 0;
        if (!AscArgs::GetInt(L, 1, cat)) return 0;
        auto& v = S().filteredByCategory[cat];
        v.clear();
        for (const auto& k : AscensionCASvc::Get().known)
        {
            AscCAEntry e;
            if (AscCA_FindEntry(k.entry, e) && static_cast<int>(e.tab) == cat)
                v.push_back(k.entry);
        }
        return 0;
    }

    int GetNumFilteredEntriesByCategory(lua_State* L)
    {
        int cat = 0;
        if (!AscArgs::GetInt(L, 1, cat)) return PushNum(L, 0);
        auto it = S().filteredByCategory.find(cat);
        return PushNum(L, it == S().filteredByCategory.end() ? 0 : static_cast<double>(it->second.size()));
    }

    int GetFilteredEntryAtIndexByCategory(lua_State* L)
    {
        int cat = 0, idx = 0;
        if (!AscArgs::GetInt(L, 1, cat) || !AscArgs::GetInt(L, 2, idx)) return PushNil(L);
        auto it = S().filteredByCategory.find(cat);
        if (it == S().filteredByCategory.end() || idx < 1
            || static_cast<size_t>(idx) > it->second.size())
            return PushNil(L);
        return PushNum(L, it->second[idx - 1]);
    }

    int IsFiltered(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return PushBool(L, false);
        for (const auto& kv : S().filteredByCategory)
            if (std::find(kv.second.begin(), kv.second.end(), static_cast<uint32_t>(id)) != kv.second.end())
                return PushBool(L, true);
        return PushBool(L, false);
    }

    // ---- suggestion context overrides ----------------------------------------------------------
    int AddSuggestionContextOverride(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return 0;
        auto& v = S().suggestionOverrides;
        if (std::find(v.begin(), v.end(), static_cast<uint32_t>(id)) == v.end())
            v.push_back(static_cast<uint32_t>(id));
        return 0;
    }

    int RemoveSuggestionContextOverride(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return 0;
        auto& v = S().suggestionOverrides;
        v.erase(std::remove(v.begin(), v.end(), static_cast<uint32_t>(id)), v.end());
        return 0;
    }

    int ClearSuggestionContextOverrides(lua_State* L)
    {
        (void)L;
        S().suggestionOverrides.clear();
        return 0;
    }

    int HasAnySuggestionContextOverrides(lua_State* L)
    {
        return PushBool(L, !S().suggestionOverrides.empty());
    }

    int IsSuggestionContextOverride(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return PushBool(L, false);
        const auto& v = S().suggestionOverrides;
        return PushBool(L, std::find(v.begin(), v.end(), static_cast<uint32_t>(id)) != v.end());
    }

    // ---- confirmation gates --------------------------------------------------------------------
    // The original prompts before anything that destroys progress. Learning only prompts when the
    // node costs essence.
    int ShouldConfirmLearnID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushBool(L, false);
        return PushBool(L, e.aeCost > 0 || e.teCost > 0);
    }

    int ShouldConfirmUnlearnID(lua_State* L)
    {
        int id = 0;
        return PushBool(L, AscArgs::GetInt(L, 1, id) && EffectiveRank(static_cast<uint32_t>(id)) > 0);
    }

    int ShouldConfirmUnlearnAllSpells(lua_State* L)
    {
        return PushBool(L, SpendOver(false, -1, 0).points > 0);
    }
    int ShouldConfirmUnlearnAllTalents(lua_State* L)
    {
        return PushBool(L, SpendOver(false, -1, 0).te > 0);
    }

    // ---- inspect and the remaining reads --------------------------------------------------------
    // Inspecting another player needs their advancement state from the server; no opcode delivers it
    // yet, so these report nothing rather than showing the player their own build as someone else's.
    int GetInspectedBuild(lua_State* L) { return PushNil(L); }
    int GetInspectInfo(lua_State* L)    { return PushNil(L); }

    int GetEntriesAvailableForTrade(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, 0);
        return 1;
    }

    int GetQualityCount(lua_State* L) { return PushNum(L, 0); }

    // Crit reduction (resilience-style) is a server-computed stat we are not sent.
    int GetReducedChanceToBeCrit(lua_State* L) { return PushNum(L, 0); }

    int HasRuneUI(lua_State* L) { return PushBool(L, false); }

    int IsSpellHoldToCast(lua_State* L) { return PushBool(L, false); }

    int ImportPendingBuild(lua_State* L)
    {
        (void)L;
        S().pending.clear();
        return 0;
    }

    int ImportPendingBuildID(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return PushBool(L, false);
        S().inspectedFrom = static_cast<uint32_t>(id);
        return PushBool(L, true);
    }


    // ---- the second CA container (DAT_10bde8a8) --------------------------------------------------
    // Same subsystem, separate manager in the original: the learned/unlearn half and the essence
    // costs. It reads the same known set and DBC rows, so it belongs on the same state here.
    int GetAbilityEssenceCost(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushNum(L, 0);
        return PushNum(L, e.aeCost);
    }

    int GetTalentEssenceCost(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushNum(L, 0);
        return PushNum(L, e.teCost);
    }

    // "Learned" is what the server has confirmed, i.e. the known set with staging ignored.
    Spend LearnedSpend(int tabFilter)
    {
        Spend out;
        for (const auto& k : AscensionCASvc::Get().known)
        {
            if (!k.rank) continue;
            AscCAEntry e;
            if (!AscCA_FindEntry(k.entry, e)) continue;
            if (tabFilter >= 0 && static_cast<int>(e.tab) != tabFilter) continue;
            out.points += k.rank;
            if (e.teCost) out.te += e.teCost * k.rank;
            else out.ae += e.aeCost * k.rank;
        }
        return out;
    }

    int GetLearnedAE(lua_State* L) { return PushNum(L, LearnedSpend(-1).ae); }
    int GetLearnedTE(lua_State* L) { return PushNum(L, LearnedSpend(-1).te); }
    int GetGlobalAEInvestment(lua_State* L) { return PushNum(L, LearnedSpend(-1).ae); }
    int GetGlobalTEInvestment(lua_State* L) { return PushNum(L, LearnedSpend(-1).te); }
    int GetClassAEInvestment(lua_State* L) { return PushNum(L, LearnedSpend(-1).ae); }
    int GetClassTEInvestment(lua_State* L) { return PushNum(L, LearnedSpend(-1).te); }
    int GetClassPointInvestment(lua_State* L) { return PushNum(L, LearnedSpend(-1).points); }
    int GetExpectedAE(lua_State* L) { return PushNum(L, SpendOver(false, -1, 0).ae); }
    int GetExpectedTE(lua_State* L) { return PushNum(L, SpendOver(false, -1, 0).te); }

    int GetTabAEInvestment(lua_State* L)
    {
        int tab = -1;
        AscArgs::OptInt(L, 1, tab, -1);
        return PushNum(L, LearnedSpend(tab).ae);
    }
    int GetTabTEInvestment(lua_State* L)
    {
        int tab = -1;
        AscArgs::OptInt(L, 1, tab, -1);
        return PushNum(L, LearnedSpend(tab).te);
    }

    int GetRemainingAE(lua_State* L)
    {
        uint32_t ae = 0, te = 0;
        if (!Budget(ae, te)) return PushNum(L, 0);
        const uint32_t spent = LearnedSpend(-1).ae;
        return PushNum(L, ae > spent ? ae - spent : 0);
    }
    int GetRemainingTE(lua_State* L)
    {
        uint32_t ae = 0, te = 0;
        if (!Budget(ae, te)) return PushNum(L, 0);
        const uint32_t spent = LearnedSpend(-1).te;
        return PushNum(L, te > spent ? te - spent : 0);
    }

    int CanLearnID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushBool(L, false);
        const uint32_t key = static_cast<uint32_t>(id);
        if (EffectiveRank(key) >= MaxRank(e)) return PushBool(L, false);
        for (uint32_t i = 0; i < 5; ++i)
            if (e.connected[i] && EffectiveRank(e.connected[i]) == 0)
                return PushBool(L, false);
        return PushBool(L, true);
    }

    int CanUnlearnID(lua_State* L)
    {
        int id = 0;
        return PushBool(L, AscArgs::GetInt(L, 1, id) && EffectiveRank(static_cast<uint32_t>(id)) > 0);
    }

    int UnlearnID(lua_State* L)
    {
        int id = 0;
        if (!AscArgs::GetInt(L, 1, id)) return PushBool(L, false);
        const uint32_t key = static_cast<uint32_t>(id);
        if (EffectiveRank(key) == 0) return PushBool(L, false);
        S().pending[key] = 0;
        return ApplyPendingBuild(L);
    }

    int CanUnlearnAllSpells(lua_State* L) { return PushBool(L, LearnedSpend(-1).points > 0); }
    int CanUnlearnAllTalents(lua_State* L) { return PushBool(L, LearnedSpend(-1).te > 0); }

    // A node is locked when its prerequisites are not held.
    int IsLockedID(lua_State* L)
    {
        int id = 0;
        AscCAEntry e;
        if (!AscArgs::GetInt(L, 1, id) || !AscCA_FindEntry(static_cast<uint32_t>(id), e))
            return PushBool(L, true);
        for (uint32_t i = 0; i < 5; ++i)
            if (e.connected[i] && EffectiveRank(e.connected[i]) == 0)
                return PushBool(L, true);
        return PushBool(L, false);
    }

    // The spell ids behind every node the character actually knows.
    int GetKnownSpells(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, 0);
        int n = 1;
        for (const auto& k : AscensionCASvc::Get().known)
        {
            if (!k.rank) continue;
            AscCAEntry e;
            if (!AscCA_FindEntry(k.entry, e)) continue;
            for (uint32_t i = 0; i < 5 && i < k.rank; ++i)
            {
                if (!e.spells[i]) continue;
                AscLua::lua_pushinteger(L, n++);
                AscLua::lua_pushinteger(L, static_cast<int>(e.spells[i]));
                AscLua::lua_settable(L, -3);
            }
        }
        return 1;
    }

    // Stat suggestions are authored server//data side; an empty table is iterable, a wrong one is not
    // recoverable by the UI.
    int GetSuggestedStats(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, 0);
        return 1;
    }

    // Dragging a node's spell to the action bar. The client native does the pickup; without it the
    // honest answer is that nothing was picked up.
    int PickupSpell(lua_State* L) { return PushBool(L, false); }


    // Bound by CoATalentFrame.xml as the tree's gate currency and entry source:
    //   gateCurrencyCount = GetPendingTabAEInvestment / GetPendingTabTEInvestment
    //   getEntries        = GetEntriesByClass
    // Missing GetPendingTabTEInvestment tripped an assert in TalentTreeBaseMixin:UpdateGates, and a
    // nil GetEntriesByClass failed ipairs in TalentTreeBase.lua:331 -- both only reachable once the
    // rest of C_CharacterAdvancement worked, which is why they surfaced late.
    int GetPendingTabTEInvestment(lua_State* L)
    {
        int tab = -1;
        AscArgs::OptInt(L, 1, tab, -1);
        return PushNum(L, SpendOver(false, tab, 0).te);
    }

    // GetEntriesByClass(class, tab, includeUnavailable) -- the rows the tree lays out, straight from
    // CharacterAdvancement.dbc, with the character's current rank attached.
    int GetEntriesByClass(lua_State* L)
    {
        // The UI passes NAMES here ("Necromancer", "Class"), not ids -- measured by driving
        // CoATreeViewMixin:OnLoad's own path. Reading them as integers is why the tree was always
        // empty. Accept either form.
        int cls = 0, tab = -1;
        std::string clsName, tabName;
        if (AscArgs::GetString(L, 1, clsName) && !clsName.empty()
            && !AscLua::lua_isnumber(L, 1))
            cls = static_cast<int>(AscCA_ClassIdByName(clsName.c_str()));
        else
            AscArgs::OptInt(L, 1, cls, 0);

        if (AscArgs::GetString(L, 2, tabName) && !tabName.empty()
            && !AscLua::lua_isnumber(L, 2))
            tab = static_cast<int>(AscCA_TabIdByName(tabName.c_str()));
        else
            AscArgs::OptInt(L, 2, tab, -1);

        if (!clsName.empty() && cls <= 0)
            AscLog::Printf("GetEntriesByClass: unknown class name '%s'", clsName.c_str());
        if (!tabName.empty() && tab <= 0)
            AscLog::Printf("GetEntriesByClass: unknown tab name '%s'", tabName.c_str());

        // No class set means the tree has nothing to draw -- TalentTreeBaseMixin says as much
        // ("No Class Set. Use SetClassTab"). Treating a missing class as "any class" returned all
        // 10,255 rows as 20-field tables and the UI then tried to build a frame per entry, which is
        // what froze the client when the talent panel was opened. Measured 2026-09-23.
        if (cls <= 0)
        {
            AscLog::Printf("GetEntriesByClass: no class set, returning empty");
            AscLua::lua_createtable(L, 0, 0);
            return 1;
        }

        std::vector<AscCAEntry> rows;
        AscCA_CollectByClassTab(static_cast<uint32_t>(cls), tab, rows);

        // Defence in depth: a single tree is tens of nodes. Anything near a thousand means the
        // filter did not narrow and the UI must not be handed it.
        if (rows.size() > 400)
        {
            AscLog::Printf("GetEntriesByClass: class %d tab %d yielded %u rows, refusing (filter too broad)",
                           cls, tab, static_cast<unsigned>(rows.size()));
            AscLua::lua_createtable(L, 0, 0);
            return 1;
        }

        AscLua::lua_createtable(L, static_cast<int>(rows.size()), 0);
        int n = 1;
        for (const auto& e : rows)
        {
            AscLua::lua_pushinteger(L, n++);
            AscLua::lua_createtable(L, 0, 12);
            Field(L, "ID", e.id);
            Field(L, "EntryID", e.id);
            Field(L, "Rank", EffectiveRank(e.id));
            Field(L, "MaxRank", MaxRank(e));
            Field(L, "Level", e.level);
            Field(L, "Tab", e.tab);
            Field(L, "Group", e.group);
            Field(L, "ClassType", e.classType);
            Field(L, "AECost", e.aeCost);
            Field(L, "TECost", e.teCost);
            Field(L, "SpellID", e.spells[0]);
            Field(L, "Required", static_cast<int>(e.connected[0]));
            // CharacterAdvancementUtil.GetEntryPosition reads Column/Row (or PositionX/PositionY for
            // some classes) and GridLayoutUtil does arithmetic on them, so nil is a hard error. The
            // DBC columns holding the grid position are not identified yet -- 0 stacks every node at
            // one spot, which is visibly wrong but lets the tree build instead of aborting. Finding
            // those two columns is what makes the layout correct.
            // The full field set the CoA tree UI reads, extracted in one pass from
            // Ascension_CoATalents / Ascension_TalentUI / FrameXML\CharacterAdvancement rather than
            // one error per rebuild. Real values where CharacterAdvancement.dbc has them; correctly
            // TYPED defaults elsewhere, because the failures here are type errors (arithmetic on
            // nil, format expecting a number, ipairs wanting a table), not missing-value errors.
            // The full 31-field contract the ORIGINAL serialiser writes (FUN_1016c910), adopted
            // wholesale rather than inferred from UI field reads. Sourced from the DBC where the
            // column is known; the rest are correctly TYPED defaults, because the failures this
            // prevents are type errors (arithmetic on nil, format wanting a number, ipairs wanting
            // a table) and only the VALUES remain to be sourced.
            Field(L, "Class", e.classType);
            Field(L, "RequiredLevel", e.level);
            Field(L, "AECost", e.aeCost);
            Field(L, "TECost", e.teCost);
            Field(L, "ParentNode", static_cast<int>(e.connected[0]));
            Field(L, "Column", static_cast<int>(e.posX));
            Field(L, "Row", static_cast<int>(e.posY));
            AscLua::lua_pushnumber(L, e.posX);  AscLua::lua_setfield(L, -2, "PositionX");
            AscLua::lua_pushnumber(L, e.posY);  AscLua::lua_setfield(L, -2, "PositionY");
            AscLua::lua_pushnumber(L, e.sizeX); AscLua::lua_setfield(L, -2, "SizeX");
            AscLua::lua_pushnumber(L, e.sizeY); AscLua::lua_setfield(L, -2, "SizeY");

            // Named by the original serialiser but not yet located in the record.
            Field(L, "Quality", 0);
            Field(L, "QualityCost", 0);
            Field(L, "RequiredAEInvestment", 0);
            Field(L, "RequiredTEInvestment", 0);
            Field(L, "RequiredClassAEInvestment", 0);
            Field(L, "RequiredClassTEInvestment", 0);
            Field(L, "RequiredTabAEInvestment", 0);
            Field(L, "RequiredTabTEInvestment", 0);
            Field(L, "Points", 0);
            Field(L, "RequiredClassPoints", 0);
            Field(L, "Flags", 0);
            Field(L, "Type", 0);
            Field(L, "NodeType", 0);
            Field(L, "Distance", 0);
            Field(L, "Anchor", 0);
            Field(L, "Color", 0);
            Field(L, "SpellCastReq", 0);
            FieldB(L, "Reset", false);
            AscLua::lua_pushstring(L, "");
            AscLua::lua_setfield(L, -2, "Name");
            AscLua::lua_pushstring(L, "");
            AscLua::lua_setfield(L, -2, "Icon");

            // The rank chain.
            AscLua::lua_createtable(L, 5, 0);
            int sp = 1;
            for (uint32_t k = 0; k < 5; ++k)
            {
                if (!e.spells[k]) continue;
                AscLua::lua_pushinteger(L, sp++);
                AscLua::lua_pushinteger(L, static_cast<int>(e.spells[k]));
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_setfield(L, -2, "Spells");

            // ConnectedNodes and RequiredIDs are the same link set in the original; emit both.
            for (int pass = 0; pass < 2; ++pass)
            {
                AscLua::lua_createtable(L, 5, 0);
                int cn = 1;
                for (uint32_t k = 0; k < 5; ++k)
                {
                    if (!e.connected[k]) continue;
                    AscLua::lua_pushinteger(L, cn++);
                    AscLua::lua_pushinteger(L, static_cast<int>(e.connected[k]));
                    AscLua::lua_settable(L, -3);
                }
                AscLua::lua_setfield(L, -2, pass == 0 ? "ConnectedNodes" : "RequiredIDs");
            }

            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_setfield(L, -2, "Masteries");
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // Asc_SetPlayerContext(classFile, level) -- called from the world bootstrap.
    int SetPlayerContext(lua_State* L)
    {
        std::string cls;
        int level = 0;
        AscArgs::OptString(L, 1, cls, "");
        AscArgs::OptInt(L, 2, level, 0);
        if (!cls.empty()) g_playerClassType = AscCA_ClassIdByName(cls.c_str());
        if (level > 0) g_playerLevel = static_cast<uint32_t>(level);
        uint32_t ae = 0, te = 0;
        Budget(ae, te);
        AscLog::Printf("player context: class '%s' -> %u, level %u, budget AE %u TE %u",
                       cls.c_str(), g_playerClassType, g_playerLevel, ae, te);
        return 0;
    }

    const AscLua::Reg REGS[] = {
        {"GetPendingTabTEInvestment", GetPendingTabTEInvestment},
        {"GetEntriesByClass", GetEntriesByClass},

        {"GetAbilityEssenceCost", GetAbilityEssenceCost},
        {"GetTalentEssenceCost", GetTalentEssenceCost}, {"GetLearnedAE", GetLearnedAE},
        {"GetLearnedTE", GetLearnedTE}, {"GetGlobalAEInvestment", GetGlobalAEInvestment},
        {"GetGlobalTEInvestment", GetGlobalTEInvestment},
        {"GetClassAEInvestment", GetClassAEInvestment},
        {"GetClassTEInvestment", GetClassTEInvestment},
        {"GetClassPointInvestment", GetClassPointInvestment},
        {"GetExpectedAE", GetExpectedAE}, {"GetExpectedTE", GetExpectedTE},
        {"GetTabAEInvestment", GetTabAEInvestment}, {"GetTabTEInvestment", GetTabTEInvestment},
        {"GetRemainingAE", GetRemainingAE}, {"GetRemainingTE", GetRemainingTE},
        {"CanLearnID", CanLearnID}, {"CanUnlearnID", CanUnlearnID}, {"UnlearnID", UnlearnID},
        {"CanUnlearnAllSpells", CanUnlearnAllSpells},
        {"CanUnlearnAllTalents", CanUnlearnAllTalents}, {"IsLockedID", IsLockedID},
        {"GetKnownSpells", GetKnownSpells}, {"GetSuggestedStats", GetSuggestedStats},
        {"PickupSpell", PickupSpell},

        {"IsKnownID", IsKnownID}, {"IsPendingEntryID", IsPendingEntryID}, {"IsPending", IsPending},
        {"GetPendingRankByEntryID", GetPendingRankByEntryID}, {"UnitKnownID", UnitKnownID},
        {"UnitTalentRankByID", UnitTalentRankByID}, {"IsSpellIDKnown", IsSpellIDKnown},
        {"IsKnownSpellID", IsKnownSpellID}, {"GetKnownTalentEntries", GetKnownTalentEntries},
        {"GetKnownSpellEntries", GetKnownSpellEntries},
        {"GetKnownTalentEntriesForClass", GetKnownTalentEntriesForClass},
        {"GetKnownSpellEntriesForClass", GetKnownSpellEntriesForClass},
        {"GetActiveChrSpec", GetActiveChrSpec}, {"GetActiveSpecID", GetActiveSpecID},
        {"CanSwitchActiveChrSpec", CanSwitchActiveChrSpec}, {"SwitchActiveChrSpec", SwitchActiveChrSpec},
        {"CanAddByEntryID", CanAddByEntryID}, {"AddByEntryID", AddByEntryID},
        {"CanRemoveByEntryID", CanRemoveByEntryID}, {"RemoveByEntryID", RemoveByEntryID},
        {"CanSwapEntriesByID", CanSwapEntriesByID}, {"SwapEntriesByID", SwapEntriesByID},
        {"CanClearPendingBuild", CanClearPendingBuild}, {"ClearPendingBuild", ClearPendingBuild},
        {"ClearPendingBuildByTab", ClearPendingBuildByTab}, {"CancelPendingBuild", CancelPendingBuild},
        {"IsPendingBuildAvailable", IsPendingBuildAvailable},
        {"IsActiveBuildAvailable", IsActiveBuildAvailable},
        {"CanApplyPendingBuild", CanApplyPendingBuild}, {"ApplyPendingBuild", ApplyPendingBuild},
        {"LearnID", LearnID},
        {"GetPendingGlobalAEInvestment", GetPendingGlobalAEInvestment},
        {"GetPendingGlobalTEInvestment", GetPendingGlobalTEInvestment},
        {"GetPendingClassAEInvestment", GetPendingClassAEInvestment},
        {"GetPendingClassTEInvestment", GetPendingClassTEInvestment},
        {"GetPendingClassPointInvestment", GetPendingClassPointInvestment},
        {"GetPendingExpectedAE", GetPendingExpectedAE}, {"GetPendingExpectedTE", GetPendingExpectedTE},
        {"GetPendingTabAEInvestment", GetPendingTabAEInvestment},
        {"GetPendingRemainingAE", GetPendingRemainingAE},
        {"GetPendingRemainingTE", GetPendingRemainingTE}, {"GetPendingSummary", GetPendingSummary},
        {"GetLowestInvestmentRequired", GetLowestInvestmentRequired},
        {"MeetsInvestmentForAddByEntryID", MeetsInvestmentForAddByEntryID},
        {"KnowsConnectedNodesFor", KnowsConnectedNodesFor},
        {"IsConnectionAllowed", IsConnectionAllowed},
        {"SetFilteredEntriesByCategory", SetFilteredEntriesByCategory},
        {"GetNumFilteredEntriesByCategory", GetNumFilteredEntriesByCategory},
        {"GetFilteredEntryAtIndexByCategory", GetFilteredEntryAtIndexByCategory},
        {"IsFiltered", IsFiltered},
        {"AddSuggestionContextOverride", AddSuggestionContextOverride},
        {"RemoveSuggestionContextOverride", RemoveSuggestionContextOverride},
        {"ClearSuggestionContextOverrides", ClearSuggestionContextOverrides},
        {"HasAnySuggestionContextOverrides", HasAnySuggestionContextOverrides},
        {"IsSuggestionContextOverride", IsSuggestionContextOverride},
        {"ShouldConfirmLearnID", ShouldConfirmLearnID},
        {"ShouldConfirmUnlearnID", ShouldConfirmUnlearnID},
        {"ShouldConfirmUnlearnAllSpells", ShouldConfirmUnlearnAllSpells},
        {"ShouldConfirmUnlearnAllTalents", ShouldConfirmUnlearnAllTalents},
        {"GetInspectedBuild", GetInspectedBuild}, {"GetInspectInfo", GetInspectInfo},
        {"GetEntriesAvailableForTrade", GetEntriesAvailableForTrade},
        {"GetQualityCount", GetQualityCount},
        {"GetReducedChanceToBeCrit", GetReducedChanceToBeCrit}, {"HasRuneUI", HasRuneUI},
        {"IsSpellHoldToCast", IsSpellHoldToCast}, {"ImportPendingBuild", ImportPendingBuild},
        {"ImportPendingBuildID", ImportPendingBuildID},
    };
}

void Register(lua_State* L)
{
    if (!L) return;
    AscLua::lua_pushcclosure(L, SetPlayerContext, 0);
    AscLua::lua_setfield(L, -10002 /* LUA_GLOBALSINDEX */, "Asc_SetPlayerContext");
    int top = AscLua::lua_gettop(L);
    AscLua::lua_getfield(L, GLOBALS, "C_CharacterAdvancement");
    if (AscLua::lua_type(L, -1) != LUA_TTABLE)
    {
        AscLua::lua_settop(L, top);
        AscLua::lua_createtable(L, 0, 80);
    }
    for (const auto& r : REGS)
    {
        AscLua::lua_pushcclosure(L, r.fn, 0);
        AscLua::lua_setfield(L, -2, r.name);
    }
    AscLua::lua_setfield(L, GLOBALS, "C_CharacterAdvancement");
    AscLua::lua_settop(L, top);
    AscLog::Printf("C_CharacterAdvancement: %u real natives registered",
                   static_cast<unsigned>(sizeof(REGS) / sizeof(REGS[0])));
}
}
