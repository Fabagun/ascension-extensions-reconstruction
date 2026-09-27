// Exact transcriptions of the original Extensions.dll's Lua bindings.
//
// Each function here is read off its decompile in E:\oniwow-artifacts\ghidra-ext\out\bindings\<Name>.c
// (dump with tools/dump_binding.py, which inlines the DLL helpers it reaches). The comment above each
// names the original handler and helpers, so a reader can check the transcription against the source.
//
// Registered LAST (after every hand-written module), so an exact transcription always replaces an
// earlier inference of the same name. The generated nil-returning stubs (AscStubs.generated.cpp) and
// the old C_CharacterAdvancement module were retired to client-ext/retired/ on 2026-09-27.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscDbc.hpp>
#include <Windows.h>
#include <cstring>
#include <algorithm>
#include <vector>

using namespace AscScript;

namespace AscBindings
{
namespace
{
    // FUN_100b2710: the shared "return 0;" the original registers for bindings it never implemented
    // (IsPurchaseInProgress, SwapFactionChangePotion, SwapRaceChangePotion, ClearRecentlyLearnedEntries).
    int ReturnNothing(lua_State*) { return 0; }

    // ---- gossip ----------------------------------------------------------------------------------
    // 0x58A660 / 0x58A750: the client's available / active gossip-quest slots (0..0xFD), each a
    // record whose first dword is the quest id; NULL past the end. 0xC016F0 is the gossip NPC guid.
    typedef const uint32_t*(__cdecl* GossipQuestAt_t)(int);
    const uint64_t& GossipGuid() { return *reinterpret_cast<const uint64_t*>(0xC016F0); }

    std::vector<uint32_t> GossipQuests(uint32_t fn, bool completableOnly)
    {
        std::vector<uint32_t> ids;
        for (int i = 0; i < 0xFE; ++i)
        {
            const uint32_t* q = reinterpret_cast<GossipQuestAt_t>(fn)(i);
            if (!q)
                break;
            if (completableOnly)
            {
                // FUN_10194190: quest-log index for the id (0x5DEEB0, 1-based), then 0x5E0EA0(index-1, 1)
                int logIdx = reinterpret_cast<int(__cdecl*)(uint32_t)>(0x5DEEB0)(*q);
                if (logIdx - 1 < 0)
                    continue;
                if (!(reinterpret_cast<uint32_t(__cdecl*)(int, int)>(0x5E0EA0)(logIdx - 1, 1) & 0xFF))
                    continue;
            }
            ids.push_back(*q);
        }
        return ids;
    }

    // handler_AcceptAllAvailableGossipQuests -> FUN_10111f10 / FUN_10112410:
    // CMSG_QUESTGIVER_ACCEPT_QUEST (0x189) { u64 npc, u32 quest, u32 0 } per available quest.
    int AcceptAllAvailableGossipQuests(lua_State*)
    {
        for (uint32_t id : GossipQuests(0x58A660, false))
            Packet(0x189).U64(GossipGuid()).U32(id).U32(0).Send();
        return 0;
    }

    // handler_CompleteAllActiveGossipQuests -> FUN_10112250 / FUN_101124b0:
    // CMSG_QUESTGIVER_COMPLETE_QUEST (0x18E) { u64 npc, u32 quest, u32 0 } per completable active quest.
    int CompleteAllActiveGossipQuests(lua_State*)
    {
        for (uint32_t id : GossipQuests(0x58A750, true))
            Packet(0x18E).U64(GossipGuid()).U32(id).U32(0).Send();
        return 0;
    }

    // handler_AcceptAvailableGossipQuest / handler_CompleteActiveGossipQuest (FUN_10112030 / FUN_10112140):
    // one numeric argument; sent only when that quest is in the matching list.
    bool QuestArg(lua_State* L, uint32_t& id)
    {
        if (AscLua::lua_gettop(L) != 1 || !AscLua::lua_isnumber(L, 1))
            return false;
        id = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 1)));
        return true;
    }
    bool Contains(const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); }

    int AcceptAvailableGossipQuest(lua_State* L)
    {
        uint32_t id;
        if (QuestArg(L, id) && Contains(GossipQuests(0x58A660, false), id))
            Packet(0x189).U64(GossipGuid()).U32(id).U32(0).Send();
        return 0;
    }

    int CompleteActiveGossipQuest(lua_State* L)
    {
        uint32_t id;
        if (QuestArg(L, id) && Contains(GossipQuests(0x58A750, true), id))
            Packet(0x18E).U64(GossipGuid()).U32(id).U32(0).Send();
        return 0;
    }

    // FUN_100d8740 / FUN_100d8ae0 / FUN_100d9290: the ids as a 1-based table; nothing when empty.
    int PushIdList(lua_State* L, const std::vector<uint32_t>& ids)
    {
        if (ids.empty())
            return 0;
        AscLua::lua_createtable(L, 0, 0);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(ids[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }
    int GetActiveGossipQuestIds(lua_State* L)    { return PushIdList(L, GossipQuests(0x58A750, false)); }
    int GetAvailableGossipQuestIds(lua_State* L) { return PushIdList(L, GossipQuests(0x58A660, false)); }
    int GetCompleteGossipQuestIds(lua_State* L)  { return PushIdList(L, GossipQuests(0x58A750, true)); }

    // ---- client-side auras -----------------------------------------------------------------------
    // DAT_10bdb230/4: a DLL-global guid that is never written -- the empty guid.
    const uint64_t kNoCaster = 0;

    // FUN_100de1a0: apply. Builds a complete SMSG_AURA_UPDATE (0x496) and asks the server to echo it
    // back through CMSG_ECHO (0x532). Flags follow the spell record: 0x88 base (no caster | negative),
    // +0x20 (duration sent) unless DurationIndex is 21 (infinite) or bit 10 of the dword at +0x24 is
    // set, and one bit per non-zero Effect[0..2].
    void ApplyClientsideAura(uint32_t spellId, uint8_t slot, uint64_t target, uint64_t caster)
    {
        uint8_t rec[0x2A8];
        if (!FetchSpell(spellId, rec))
            return;
        auto dw = [&](size_t off) { return *reinterpret_cast<const uint32_t*>(rec + off); };

        uint8_t flags = 0x88;
        if (dw(0xA0) != 0x15)
            flags = static_cast<uint8_t>(((dw(0x24) >> 10 & 1) ^ 1) * 0x20 + 0x88);
        if (dw(0x11C)) flags |= 1;
        if (dw(0x120)) flags |= 2;
        if (dw(0x124)) flags |= 4;

        Packet p(0x532);
        p.U16(0x496).PackedGuid(target).U8(slot).U32(spellId).U8(flags).U8(rec[0x9C]).U8(1);
        if (!(flags & 8))
            p.PackedGuid(caster);
        if (flags & 0x20)
            p.U32(5000).U32(5000);
        p.Send();
    }

    // FUN_100d5400(L, target, caster): arg1 = apply?, arg2 = slot (stored as 0xFE - n), arg3 = spell.
    // Removal is the same SMSG_AURA_UPDATE with spell 0.
    void ClientsideAuraOn(lua_State* L, uint64_t target, uint64_t caster)
    {
        const int apply = AscLua::lua_toboolean(L, 1);
        const uint8_t slot = static_cast<uint8_t>(-2 - static_cast<int8_t>(ToInt(CheckNumber(L, 2))));
        if (apply)
        {
            ApplyClientsideAura(static_cast<uint32_t>(ToInt(CheckNumber(L, 3))), slot, target, caster);
            return;
        }
        Packet(0x532).U16(0x496).PackedGuid(target).U8(slot).U32(0).Send();
    }

    // handler_ClientsideAura: on the active player, no caster.
    int ClientsideAura(lua_State* L)
    {
        ClientsideAuraOn(L, ActivePlayerGuid(), kNoCaster);
        return 0;
    }

    // handler_ClientsideAuraTarget: on the player's current target (descriptor UNIT_FIELD_TARGET, +0x48).
    int ClientsideAuraTarget(lua_State* L)
    {
        uint8_t* player = reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)();
        if (!player)
            return 0;
        const uint8_t* desc = *reinterpret_cast<uint8_t**>(player + 8);
        ClientsideAuraOn(L, *reinterpret_cast<const uint64_t*>(desc + 0x48), kNoCaster);
        return 0;
    }

    // FUN_100d0160: 0x4D4B30 enumerates every visible object; for each UNIT whose guid carries the
    // creature entry given in arg 4, apply/remove with the player as caster. Guids whose high word is
    // a player/item/pet-less type carry no entry (0).
    int __cdecl AuraCreatureVisit(uint64_t guid, lua_State* L)
    {
        if (!ObjectPtr(guid, 8))
            return 1;
        const uint32_t want = static_cast<uint32_t>(ToInt(CheckNumber(L, 4)));
        const uint32_t lo = static_cast<uint32_t>(guid), hi = static_cast<uint32_t>(guid >> 32);
        uint32_t entry = 0;
        if (hi > 0x0FFFFFFF)
        {
            const uint32_t t = hi >> 16;
            const bool noEntry = t == 0xF101 || t == 0x1FC0 || t == 0 || t == 8000 || t == 0x1F50
                              || t == 0x4000 || t == 0xF100;
            if (!noEntry)
                entry = (lo >> 24) | ((hi & 0xFFFF) << 8);
        }
        if (entry == want)
            ClientsideAuraOn(L, guid, ActivePlayerGuid());
        return 1;
    }

    int ClientsideAuraCreature(lua_State* L)
    {
        typedef int(__cdecl* Visit_t)(uint64_t, lua_State*);
        reinterpret_cast<void(__cdecl*)(Visit_t, lua_State*)>(0x4D4B30)(AuraCreatureVisit, L);
        return 0;
    }

    // ---- resurrection choices ------------------------------------------------------------------
    // FUN_100de3b0: CMSG_CUSTOM_ASCENSION_RESURRECT_REQUEST (0x524) { u8 where }.
    void Resurrect(uint8_t where) { Packet(0x524).U8(where).Send(); }
    int RessurectInClosestTown(lua_State*)    { Resurrect(1); return 0; }
    int RessurectInClosestCapital(lua_State*) { Resurrect(2); return 0; }
    int RessurectAtCheckpoint(lua_State*)     { Resurrect(3); return 0; }

    // ---- misc client passthroughs ----------------------------------------------------------------
    // handler_GuildQueryPermissions: the client's own CMSG_GUILD_PERMISSIONS (0x3FD) sender.
    int GuildQueryPermissions(lua_State*)
    {
        reinterpret_cast<void(__cdecl*)()>(0x5CAA40)();
        return 0;
    }

    // handler_SendWindowsFlash: the game window (0x86C6A0(0)), FlashWindow(hwnd, FALSE).
    int SendWindowsFlash(lua_State*)
    {
        HWND hwnd = reinterpret_cast<HWND(__cdecl*)(int)>(0x86C6A0)(0);
        if (hwnd)
            FlashWindow(hwnd, FALSE);
        return 0;
    }

    // handler_SetCameraFoV: active camera (0x4F5960 via FUN_101941c0), field of view float at +0x40.
    int SetCameraFoV(lua_State* L)
    {
        uint8_t* camera = reinterpret_cast<uint8_t*(__cdecl*)()>(0x4F5960)();
        if (camera)
            *reinterpret_cast<float*>(camera + 0x40) = static_cast<float>(CheckNumber(L, 1));
        return 0;
    }

    // ---- C_VanityCollection.Purchase -------------------------------------------------------------
    // FUN_103311d0: exactly two args -> CMSG_CUSTOM_ASCENSION_POINT_SPEND_REQUEST (0x523)
    // { u8 (int)arg2, u32 (int)arg1 }.
    int Purchase(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 2)
        {
            const int32_t item = ToInt(CheckNumber(L, 1));
            const uint8_t kind = static_cast<uint8_t>(static_cast<int>(CheckNumber(L, 2)));
            Packet(0x523).U8(kind).U32(static_cast<uint32_t>(item)).Send();
        }
        return 0;
    }

    // ---- C_CollectorCache.GetCollectorCacheRarityRatesInfo ---------------------------------------
    // FUN_1008ba00: validates {number}, reads it into a local, and returns nothing. Unfinished in
    // the original; transcribed as such.
    int GetCollectorCacheRarityRatesInfo(lua_State* L)
    {
        if (ValidateInput(L, {NUMBER}))
            (void)ToInt(CheckNumber(L, 1));
        return 0;
    }

    AscDbc::Table& UICameras()               { return AscDbc::Get("DBFilesClient/UICameras.dbc"); }
    AscDbc::Table& UICameraWeapons()         { return AscDbc::Get("DBFilesClient/UICameraAppearanceWeapons.dbc"); }
    AscDbc::Table& UICameraChrRaces()        { return AscDbc::Get("DBFilesClient/UICameraAppearanceChrRaces.dbc"); }

    // ---- C_ItemSet.GetSetName ----------------------------------------------------------------------
    // handler_GetSetName: the client's ItemSet.dbc (FUN_100b19c0), name pointer at +4.
    int GetSetName(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = ClientDbcRow(kItemSetDbc, id);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushStr(L, *reinterpret_cast<const char* const*>(row + 4));
        return 1;
    }

    // ---- C_TradeSkill.GetCraftedItem / GetReagentItems -------------------------------------------
    // Spell record: Effect[3] at +0x11C, EffectItemType[3] at +0x1AC, Reagent[8] at +0x1D0,
    // ReagentCount[8] at +0xF0.
    //
    // handler_GetCraftedItem: for each CREATE_ITEM (24), CREATE_ITEM_2 (157) or CREATE_RANDOM_ITEM (59)
    // effect, 0x7FF770 gives the float min/max count. NOTE the original never settable()s the per-effect
    // table into the outer one (three settable calls for three fields; confirmed in the disassembly),
    // so what it returns is the TOP of the stack: the last effect's {ItemID, MinCount, MaxCount}.
    // Reproduced as-is.
    int GetCraftedItem(lua_State* L)
    {
        const uint32_t spellId = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        uint8_t rec[0x2A8];
        int found = 0;
        if (FetchSpell(spellId, rec))
        {
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            for (uint32_t i = 0; i < 3; ++i)
            {
                const int32_t effect = *reinterpret_cast<const int32_t*>(rec + 0x11C + i * 4);
                if (effect != 0x18 && effect != 0x9D && effect != 0x3B)
                    continue;
                const int32_t item = *reinterpret_cast<const int32_t*>(rec + 0x1AC + i * 4);
                float mn = 0, mx = 0;
                reinterpret_cast<void(__cdecl*)(void*, uint32_t, float*, float*, int, int, int, int)>(0x7FF770)(
                    rec, i, &mn, &mx, 0, 0, 0, 0);
                const uint32_t lo = static_cast<uint32_t>(mn), hi = static_cast<uint32_t>(mx);
                PushInt(L, static_cast<int32_t>(i + 1));
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                SetStrInt(L, "ItemID", item);
                SetStrInt(L, "MinCount", static_cast<int32_t>(lo > 1 ? lo : 1));
                SetStrInt(L, "MaxCount", static_cast<int32_t>(hi > 1 ? hi : 1));
                ++found;
            }
            if (found)
                return 1;
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // handler_GetReagentItems: {[i] = {ItemID, Count}} for each non-zero Reagent[i], or nil.
    int GetReagentItems(lua_State* L)
    {
        const uint32_t spellId = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        uint8_t rec[0x2A8];
        int found = 0;
        if (FetchSpell(spellId, rec))
        {
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            for (uint32_t i = 0; i < 8; ++i)
            {
                const int32_t reagent = *reinterpret_cast<const int32_t*>(rec + 0xD0 + i * 4);
                if (!reagent)
                    continue;
                PushInt(L, static_cast<int32_t>(i + 1));
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                SetStrInt(L, "ItemID", reagent);
                SetStrInt(L, "Count", *reinterpret_cast<const int32_t*>(rec + 0xF0 + i * 4));
                AscLua::lua_settable(L, -3);
                ++found;
            }
            if (found)
                return 1;
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- C_Aura.UnitHasAura -------------------------------------------------------------------------
    // FUN_10326dc0: {string, number}; resolves the unit token (0x60ABF0), requires a PLAYER (0x10) or
    // UNIT (8) object, then __thiscall 0x7282A0(unit, spellId). Returns nothing on a bad unit.
    int UnitHasAura(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string token = CheckString(L, 1);
        const int32_t spellId = ToInt(CheckNumber(L, 2));
        uint64_t guid;
        if (!UnitTokenGuid(token.c_str(), guid))
            return 0;
        void* unit = ObjectPtr(guid, 0x10);
        if (!unit)
            unit = ObjectPtr(guid, 8);
        if (!unit)
            return 0;
        PushBool(L, UnitHasAuraSpell(unit, static_cast<uint32_t>(spellId)));
        return 1;
    }

    // ---- UnitNameBadge / UnitNameTag ------------------------------------------------------------------
    // World-state globals the static census could not see (registered by unscanned code; found by the live
    // capture 2026-09-27). Both take a unit token (luaL_checklstring), resolve it (0x60ABF0) to a PLAYER
    // (0x10) and read PLAYER_FLAGS (descriptors +0x258); one value, nil when the unit or flag is absent.
    const uint8_t* PlayerFlagsOf(lua_State* L, uint32_t& flags)
    {
        const std::string token = CheckString(L, 1);
        uint64_t guid;
        if (!UnitTokenGuid(token.c_str(), guid))
            return nullptr;
        const uint8_t* player = static_cast<const uint8_t*>(ObjectPtr(guid, 0x10));
        if (!player)
            return nullptr;
        flags = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(player + 8) + 600);
        return player;
    }

    // FUN_10a73690: AFK (flag 2) -> the CHAT_FLAG_AFK text, else DND (4) -> CHAT_FLAG_DND.
    int UnitNameBadge(lua_State* L)
    {
        uint32_t flags = 0;
        if (PlayerFlagsOf(L, flags) && (flags & 6))
        {
            const char* key = (flags & 2) ? "CHAT_FLAG_AFK" : "CHAT_FLAG_DND";
            const char* text = reinterpret_cast<const char*(__cdecl*)(const char*, int, int)>(0x819D40)(key, -1, 1);
            AscLua::lua_pushstring(L, text ? text : "");
        }
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_10a738b0: GM (flag 8) -> "<GM>", else developer (0x8000) -> "<Dev>".
    int UnitNameTag(lua_State* L)
    {
        uint32_t flags = 0;
        if (PlayerFlagsOf(L, flags) && (flags & 8))
            AscLua::lua_pushstring(L, "<GM>");
        else if (flags & 0x8000)
            AscLua::lua_pushstring(L, "<Dev>");
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- C_UICamera.GetItemCameraID / GetCameraInfo --------------------------------------------------
    // FUN_1021a020: weapons (Item class 2) match UICameraAppearanceWeapons on (2, subclass, inventory
    // type); armor (class 4) matches UICameraAppearanceChrRaces on (race 0x6B1070, sex 0x6B1090,
    // inventory type). The first matching weapon row decides even if its camera is missing.
    const uint8_t* ItemCamera(uint32_t itemId)
    {
        const uint8_t* item = ClientDbcRow(kItemDbc, itemId);
        if (!item)
            return nullptr;
        const int32_t cls = *reinterpret_cast<const int32_t*>(item + 4);
        const int32_t sub = *reinterpret_cast<const int32_t*>(item + 8);
        const int32_t inv = *reinterpret_cast<const int32_t*>(item + 0x18);
        if (cls == 2)
        {
            AscDbc::Table& w = UICameraWeapons();
            for (uint32_t id = w.MinId(); w.Loaded() && id <= w.MaxId(); ++id)
            {
                const uint8_t* r = w.Row(id);
                if (r && w.I32(r, 4) == 2 && w.I32(r, 8) == sub && w.I32(r, 0xC) == inv)
                    return UICameras().Row(w.U32(r, 0x10));
            }
        }
        else if (cls == 4 && ActivePlayer())
        {
            AscDbc::Table& c = UICameraChrRaces();
            for (uint32_t id = c.MinId(); c.Loaded() && id <= c.MaxId(); ++id)
            {
                const uint8_t* r = c.Row(id);
                if (!r)
                    continue;
                const uint32_t race = reinterpret_cast<uint32_t(__cdecl*)()>(0x6B1070)() & 0xFF;
                if (c.U32(r, 4) != race)
                    continue;
                const uint32_t sex = reinterpret_cast<uint32_t(__cdecl*)()>(0x6B1090)() & 0xFF;
                if (c.U32(r, 8) != sex || c.I32(r, 0xC) != inv)
                    continue;
                return UICameras().Row(c.U32(r, 0x10));
            }
        }
        return nullptr;
    }

    int GetItemCameraID(lua_State* L)
    {
        uint32_t itemId;
        if (!ReadNumber(L, itemId))
            return 0;
        const uint8_t* cam = ItemCamera(itemId);
        if (cam)
            PushInt(L, AscDbc::Table::I32(cam, 0));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // The HD camera set applies when Data\Patch-Q.mpq exists and the command line does not say
    // "-hd 0" (FUN_100cd740 lookup, then a compare against "0").
    bool HdCameras()
    {
        const DWORD attr = GetFileAttributesA("Data\\Patch-Q.mpq");
        const bool patchQ = attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        const char* hd = CommandLineArg("hd");
        return patchQ && !(hd && strcmp(hd, "0") == 0);
    }

    // FUN_100c9000: (fov, x, y, z, flags) -- the HD set at +0x14.., the SD set at +0x4..
    int GetCameraInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = UICameras();
        const uint8_t* row = t.Row(id);
        if (!row)
            return PushNils(L, 5);
        const bool hd = HdCameras();
        PushNum(L, t.F32(row, 4 + (hd ? 0x10 : 0)));
        PushNum(L, t.F32(row, hd ? 0x18 : 0x8));
        PushNum(L, t.F32(row, hd ? 0x1C : 0xC));
        PushNum(L, t.F32(row, hd ? 0x20 : 0x10));
        PushInt(L, t.I32(row, 0x24));
        return 5;
    }

    // ---- C_Callboard.RequestView ------------------------------------------------------------------
    // FUN_10119a50: CMSG 0x55F with no body, returns true.
    int RequestView(lua_State* L)
    {
        Packet(0x55F).Send();
        AscLua::lua_pushboolean(L, 1);
        return 1;
    }

    const Binding kBindings[] = {
        {"C_ItemSet", "GetSetName", GetSetName},
        {"C_TradeSkill", "GetCraftedItem", GetCraftedItem},
        {"C_TradeSkill", "GetReagentItems", GetReagentItems},
        {"C_Aura", "UnitHasAura", UnitHasAura},
        {"C_UICamera", "GetItemCameraID", GetItemCameraID},
        {"C_UICamera", "GetCameraInfo", GetCameraInfo},
        {"C_Callboard", "RequestView", RequestView},
        {nullptr, "AcceptAllAvailableGossipQuests", AcceptAllAvailableGossipQuests},
        {nullptr, "CompleteAllActiveGossipQuests", CompleteAllActiveGossipQuests},
        {nullptr, "AcceptAvailableGossipQuest", AcceptAvailableGossipQuest},
        {nullptr, "CompleteActiveGossipQuest", CompleteActiveGossipQuest},
        {nullptr, "GetActiveGossipQuestIds", GetActiveGossipQuestIds},
        {nullptr, "GetAvailableGossipQuestIds", GetAvailableGossipQuestIds},
        {nullptr, "GetCompleteGossipQuestIds", GetCompleteGossipQuestIds},
        {nullptr, "ClientsideAura", ClientsideAura},
        {nullptr, "ClientsideAuraTarget", ClientsideAuraTarget},
        {nullptr, "ClientsideAuraCreature", ClientsideAuraCreature},
        {nullptr, "RessurectInClosestTown", RessurectInClosestTown},
        {nullptr, "RessurectInClosestCapital", RessurectInClosestCapital},
        {nullptr, "RessurectAtCheckpoint", RessurectAtCheckpoint},
        {nullptr, "GuildQueryPermissions", GuildQueryPermissions},
        {nullptr, "SendWindowsFlash", SendWindowsFlash},
        {nullptr, "SetCameraFoV", SetCameraFoV},
        // The original's names end in a space (strings 0x10B60B04 / 0x10B60AF4); IMPROVEMENTS.md.
        {nullptr, "UnitNameBadge ", UnitNameBadge},
        {nullptr, "UnitNameTag ", UnitNameTag},
        // handler_SetPortraitToTexture tail-calls the client's own native; register that directly.
        {nullptr, "SetPortraitToTexture", reinterpret_cast<lua_CFunction>(0x516970)},
        // 0x101912E0 is `mov eax, 0x60A510; jmp eax`: the glue state gets the client's own world native
        // (glue-only, live capture 2026-09-27; the world state has the exe's registration already).
        {nullptr, "FillLocalizedClassList", reinterpret_cast<lua_CFunction>(0x60A510)},
        {"C_VanityCollection", "Purchase", Purchase},
        {"C_VanityCollection", "IsPurchaseInProgress", ReturnNothing},
        {"C_RecoveryService", "SwapFactionChangePotion", ReturnNothing},
        {"C_RecoveryService", "SwapRaceChangePotion", ReturnNothing},
        {"C_CharacterAdvancement", "ClearRecentlyLearnedEntries", ReturnNothing},
        {"C_CollectorCache", "GetCollectorCacheRarityRatesInfo", GetCollectorCacheRarityRatesInfo},
    };

    constexpr int kRegistryIndex = -10000;   // LUA_REGISTRYINDEX

    // FUN_1030efe0 / the CDataStore registrar: a method table kept in the registry under `mt` and as the
    // global of the same name, __index pointing at itself; created on first use.
    void SetMethod(lua_State* L, const char* mt, const char* name, lua_CFunction fn)
    {
        const int top = AscLua::lua_gettop(L);
        AscLua::lua_getfield(L, kRegistryIndex, mt);
        if (AscLua::lua_type(L, -1) != LUA_TTABLE)
        {
            AscLua::lua_settop(L, top);
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_pushvalue(L, -1);
            AscLua::lua_setfield(L, kRegistryIndex, mt);
            AscLua::lua_pushvalue(L, -1);
            AscLua::lua_setfield(L, LUA_GLOBALSINDEX, mt);
            AscLua::lua_pushvalue(L, -1);
            AscLua::lua_setfield(L, -2, "__index");
        }
        AscLua::lua_pushcclosure(L, fn, 0);
        AscLua::lua_setfield(L, -2, name);
        AscLua::lua_settop(L, top);
    }

    void SetField(lua_State* L, const char* ns, const char* name, lua_CFunction fn)
    {
        const int top = AscLua::lua_gettop(L);
        AscLua::lua_getfield(L, LUA_GLOBALSINDEX, ns);
        if (AscLua::lua_type(L, -1) != LUA_TTABLE)
        {
            AscLua::lua_settop(L, top);
            AscLua::lua_createtable(L, 0, 0);
        }
        AscLua::lua_pushcclosure(L, fn, 0);
        AscLua::lua_setfield(L, -2, name);
        AscLua::lua_setfield(L, LUA_GLOBALSINDEX, ns);
        AscLua::lua_settop(L, top);
    }
}

namespace
{
    struct ModuleEntry { const Binding* bindings; size_t count; void (*init)(); };
    std::vector<ModuleEntry>& Modules() { static std::vector<ModuleEntry> m; return m; }
}

Module::Module(const Binding* bindings, size_t count, void (*init)())
{
    Modules().push_back({bindings, count, init});
}

void InitModules()
{
    for (const ModuleEntry& m : Modules())
        if (m.init)
            m.init();
}

struct StateName
{
    const char* ns;
    const char* name;
};
#include "AscBindingStates.generated.inc"

static bool Listed(const StateName* list, size_t count, const Binding& b)
{
    const char* ns = b.ns ? b.ns : "";
    for (size_t i = 0; i < count; ++i)
        if (strcmp(list[i].ns, ns) == 0 && strcmp(list[i].name, b.name) == 0)
            return true;
    return false;
}

// '@' registry metatables and '#' widget methods are not in the capture (it walks _G functions and C_*
// tables), so they keep going into both states.
static bool InState(const Binding& b, bool glue)
{
    if (b.ns && (b.ns[0] == '@' || b.ns[0] == '#'))
        return true;
    if (glue)
        return Listed(kGlueBindings, sizeof(kGlueBindings) / sizeof(kGlueBindings[0]), b);
    return !Listed(kGlueOnlyBindings, sizeof(kGlueOnlyBindings) / sizeof(kGlueOnlyBindings[0]), b);
}

static void RegisterOne(lua_State* L, const Binding& b)
{
    if (b.ns && b.ns[0] == '#')
        return;   // widget methods go into the client's method tables (AscWidget)
    if (b.ns && b.ns[0] == '@')
        SetMethod(L, b.ns + 1, b.name, b.fn);
    else if (b.ns)
        SetField(L, b.ns, b.name, b.fn);
    else
        AscLua::RegisterGlobal(L, b.name, b.fn);
}

void ForEachWidgetMethod(const char* widget, void (*cb)(const char* name, lua_CFunction fn, void* ctx), void* ctx)
{
    auto visit = [&](const Binding& b) {
        if (b.ns && b.ns[0] == '#' && strcmp(b.ns + 1, widget) == 0)
            cb(b.name, b.fn, ctx);
    };
    for (const Binding& b : kBindings)
        visit(b);
    for (const ModuleEntry& m : Modules())
        for (size_t i = 0; i < m.count; ++i)
            visit(m.bindings[i]);
}

std::vector<void (*)()>& WorldRegisteredList()
{
    static std::vector<void (*)()> v;
    return v;
}
void OnWorldRegistered(void (*cb)()) { WorldRegisteredList().push_back(cb); }
void RunWorldRegistered()
{
    for (auto cb : WorldRegisteredList())
        cb();
}

std::vector<void (*)()>& GlueRegisteredList()
{
    static std::vector<void (*)()> v;
    return v;
}
void OnGlueRegistered(void (*cb)()) { GlueRegisteredList().push_back(cb); }
void RunGlueRegistered()
{
    for (auto cb : GlueRegisteredList())
        cb();
}

void Register(lua_State* L, bool glue)
{
    if (!L)
        return;
    size_t n = 0;
    for (const Binding& b : kBindings)
        if (InState(b, glue))
            RegisterOne(L, b), ++n;
    for (const ModuleEntry& m : Modules())
        for (size_t i = 0; i < m.count; ++i)
            if (InState(m.bindings[i], glue))
                RegisterOne(L, m.bindings[i]), ++n;
    AscLog::Printf("AscBindings: %u exact transcriptions registered (%s)", static_cast<unsigned>(n),
                   glue ? "glue" : "world");
}
}

namespace AscUICamera
{
    const uint8_t* ItemCamera(uint32_t itemId) { return AscBindings::ItemCamera(itemId); }
}
