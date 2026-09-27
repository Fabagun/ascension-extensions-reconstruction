// The Appearance subsystem: C_Appearance (24), C_AppearanceCollection (12), C_AppearanceOutfit (6) and
// C_ItemSet's GetAppearances / GetItemSetNumCollected, transcribed from the original's AppearanceMgr
// (FUN_100c5eb0 -> static 0x10BDB2A0, built by FUN_100c03a0).
//
// Module init 0x100C5AE0: the six libraries (C_Appearance 0x100CD9B0, C_AppearanceCollection
// 0x100CD850, C_AppearanceOutfit 0x100CDC20, C_ItemSet 0x100CDDF0; C_UICamera and C_Item live
// elsewhere), nine events, OnGlueScreen 0x100C2170 and the SMSG handlers below.
//
// Manager layout:
//   +0x000 vector<u32> pending    per-category appearance the player has picked (index = category id)
//   +0x00C AppearancesEntryContainer (collection browser)       +0x0C8 AppearanceOutfitEntryContainer
//   +0x144 u32 (reset at glue)    +0x148 vector<u32> current    (what the server says is applied)
//   +0x154 vector<{u32 id, u32, u64 time}> collected appearances
//   +0x160 unordered_map<string, vector<u32>> outfits           +0x180 string pending outfit name
//   +0x198 vector<u32> pending outfit appearances               +0x1A4 / +0x1A5 u8 visibility flags
//
// SMSG:
//   0x698 (FUN_100c4a50) str result: APPLY_APPEARANCES_* (29); OK -> current := pending;
//         APPLY_PENDING_APPEARANCE_RESULT(result, ok).
//   0x699 (FUN_100c4480) u32 n x {u32 appearance, u32 time}: the collected list.
//   0x69A (FUN_100c41e0) u32 n x u32: current; then pending := current.
//   0x69B (handler_0x069b) u32 appearance, u32 -- newly collected (time now); APPEARANCE_COLLECTED(id).
//   0x69C (handler_0x069c) u32 appearance, u32 -- uncollected; APPEARANCE_UNCOLLECTED(id).
//   0x69D (FUN_100c4530) u32 n x {str name, u32 m x u32}: the outfits.
//   0x69F (FUN_100c5320) str result: SAVE_APPEARANCE_OUTFIT_*; OK -> outfits[pending name] := pending
//         outfit; APPEARANCE_OUTFIT_SAVED_RESULT(result, ok).
//   0x6A1 (FUN_100c4f70) str result: DELETE_APPEARANCE_OUTFIT_*; OK -> erase outfits[pending name];
//         APPEARANCE_OUTFIT_DELETED_RESULT(result, ok).
//   0x6A2 (handler_0x06a2) u8, u8 -> +0x1A4, +0x1A5.
//   0x6EB (FUN_100c4da0) str -> COLLECT_ITEM_APPEARANCE_RESULT(str).
//
// DLL DBCs (rows as the original's parser lays them out, see AscDbcLayouts.generated.inc):
//   AppearanceCategories (0x10BE005C, patched by SMSG 0x691): +0 id, +4 type (APPEARANCE_TYPE_* name,
//     FUN_101b07c0), +8 name, +0xC tier, +0x10 inventory type, +0x1C/+0x20 class mask.
//   Appearances (0x10BDFCB4, SMSG 0x692): +0 id, +4, +8 name, +0xC, +0x14..+0x1C categories,
//     +0x20 source (item / item set / creature), +0x24, +0x30..+0x40 realm gates.
//   AppearanceDetails (0x10BDFA2C, iterated in file order): +4 appearance, +8 details, +0xC store
//     product, +0x10 Ethereal Bazaar flag.
//   ItemAppearances: by item (FUN_1020fae0) -> +8 appearance.  ItemSetAppearances: by item set
//     (FUN_10210030) -> +8 appearance.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <map>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AscScript;

namespace AscUICamera { const uint8_t* ItemCamera(uint32_t itemId); }   // AscBindings.cpp (FUN_1021a020)

namespace
{
    const char* const kApplyResults[29] = {"APPLY_APPEARANCES_OK", "APPLY_APPEARANCES_UNKNOWN",
        "APPLY_APPEARANCES_OUT_OF_RANGE", "APPLY_APPEARANCES_NOT_IN_WORLD", "APPLY_APPEARANCES_WRONG_SIZE",
        "APPLY_APPEARANCES_BAD_APPEARANCE", "APPLY_APPEARANCES_BAD_CATEGORY",
        "APPLY_APPEARANCES_CATEGORY_NOT_FOUND", "APPLY_APPEARANCES_NOT_AVAILABLE_FOR_CLASS",
        "APPLY_APPEARANCES_NOT_COLLECTED", "APPLY_APPEARANCES_CANNOT_APPLY_ITEM_SET",
        "APPLY_APPEARANCES_NOT_REALM_APPROPRIATE", "APPLY_APPEARANCES_BAD_ITEM_TEMPLATE",
        "APPLY_APPEARANCES_BAD_ITEM_CLASS", "APPLY_APPEARANCES_THROWN_APPEARANCE",
        "APPLY_APPEARANCES_COSMETIC_PET_IN_COMBAT", "APPLY_APPEARANCES_BAD_CREATURE_ENTRY",
        "APPLY_APPEARANCES_BAD_CREATURE_TEMPLATE", "APPLY_APPEARANCES_BAD_CREATURE_EXPANSION",
        "APPLY_APPEARANCES_CANNOT_TAME_EXOTIC", "APPLY_APPEARANCES_BAD_CREATURE_SCRIPT",
        "APPLY_APPEARANCES_PET_TYPE_NOT_FOUND", "APPLY_APPEARANCES_PET_SUMMON_SPELL_NOT_FOUND",
        "APPLY_APPEARANCES_PET_SUMMON_SPELL_NOT_LEARNED", "APPLY_APPEARANCES_PET_DISMISS_SPELL_NOT_FOUND",
        "APPLY_APPEARANCES_PET_DISMISS_SPELL_NOT_LEARNED", "APPLY_APPEARANCES_PET_DEAD_NON_SAFE_ZONE",
        "APPLY_APPEARANCES_UNSUPPORTED_CATEGORY", "APPLY_APPEARANCES_COSMETIC_PET_APPEARANCE_CONFIRMATION"};   // 0x10B2A020
    const char* const kSaveResults[2] = {"SAVE_APPEARANCE_OUTFIT_OK", "SAVE_APPEARANCE_OUTFIT_UNKNOWN"};           // 0x10B297C8
    const char* const kDeleteResults[2] = {"DELETE_APPEARANCE_OUTFIT_OK", "DELETE_APPEARANCE_OUTFIT_UNKNOWN"};     // 0x10B29F38
    const char* const kAppearanceTypes[12] = {"APPEARANCE_TYPE_NONE", "APPEARANCE_TYPE_ITEM",
        "APPEARANCE_TYPE_ILLUSION", "APPEARANCE_TYPE_INCARNATION", "APPEARANCE_TYPE_SPELL_VISUAL",
        "APPEARANCE_TYPE_COSMETIC_PET", "APPEARANCE_TYPE_COSMETIC", "APPEARANCE_TYPE_ITEM_SET",
        "APPEARANCE_TYPE_OUTFIT", "APPEARANCE_TYPE_MOUNT", "APPEARANCE_TYPE_COMPANION", "APPEARANCE_TYPE_TOY"};   // 0x10B2A198
    enum AppearanceType { TYPE_NONE, TYPE_ITEM, TYPE_ILLUSION, TYPE_INCARNATION, TYPE_SPELL_VISUAL,
        TYPE_COSMETIC_PET, TYPE_COSMETIC, TYPE_ITEM_SET, TYPE_OUTFIT, TYPE_MOUNT, TYPE_COMPANION, TYPE_TOY };

    struct Collected { uint32_t id; uint32_t unk; uint64_t time; };   // 16 bytes

    struct
    {
        std::vector<uint32_t> pending;                                   // +0x000
        uint32_t unk144 = 0;                                             // +0x144
        std::vector<uint32_t> current;                                   // +0x148
        std::vector<Collected> collected;                                // +0x154
        std::unordered_map<std::string, std::vector<uint32_t>> outfits;  // +0x160
        std::string pendingOutfitName;                                   // +0x180
        std::vector<uint32_t> pendingOutfit;                             // +0x198
        bool seeOwn = true, seeOthers = true;                            // +0x1A4 / +0x1A5
    } g;

    // ---- DBCs ------------------------------------------------------------------------------------
    AscDbc::Table& Categories() { return AscDbc::Get("DBFilesClient/AppearanceCategories.dbc"); }
    AscDbc::Table& Appearances() { return AscDbc::Get("DBFilesClient/Appearances.dbc"); }
    AscDbc::Table& Details() { return AscDbc::Get("DBFilesClient/AppearanceDetails.dbc"); }
    AscDbc::Table& ItemAppearances() { return AscDbc::Get("DBFilesClient/ItemAppearances.dbc"); }
    AscDbc::Table& ItemSetAppearances() { return AscDbc::Get("DBFilesClient/ItemSetAppearances.dbc"); }

    // FUN_101b0ac0 -> FUN_101b07c0: a category row's APPEARANCE_TYPE_* name as its index (0 = none).
    int CategoryType(const uint8_t* row)
    {
        const char* name = Categories().Str(row, 4);
        for (int i = 0; i < 12; ++i)
            if (strcmp(name, kAppearanceTypes[i]) == 0)
                return i;
        return TYPE_NONE;
    }
    // The inline pattern "category row exists && its type == t" (FUN_100c6030 and siblings).
    bool CategoryIs(uint32_t category, int type)
    {
        const uint8_t* row = Categories().Row(category);
        return row && CategoryType(row) == type;
    }

    // FUN_1020fc80 (ItemAppearances, file order) and FUN_102101d0 (ItemSetAppearances, id order) build
    // +4 -> row with the last row winning, and +8 -> rows; SMSG 0x693 / 0x6EC rebuild them.
    std::unordered_map<uint32_t, const uint8_t*> g_itemAppearanceByItem, g_itemSetAppearanceBySet;
    std::unordered_map<uint32_t, std::vector<const uint8_t*>> g_itemsForAppearance, g_setsByAppearance;
    bool g_itemIndexBuilt = false, g_setIndexBuilt = false;
    void BuildItemAppearanceIndex()   // FUN_1020fc80
    {
        g_itemIndexBuilt = true;
        g_itemAppearanceByItem.clear();
        g_itemsForAppearance.clear();
        AscDbc::Table& t = ItemAppearances();
        for (uint32_t i = 0; i < t.Count(); ++i)
            if (const uint8_t* r = t.RowAt(i))
            {
                g_itemAppearanceByItem[AscDbc::Table::U32(r, 4)] = r;
                g_itemsForAppearance[AscDbc::Table::U32(r, 8)].push_back(r);
            }
    }
    void BuildItemSetAppearanceIndex()   // FUN_102101d0
    {
        g_setIndexBuilt = true;
        g_itemSetAppearanceBySet.clear();
        g_setsByAppearance.clear();
        AscDbc::Table& t = ItemSetAppearances();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
            {
                g_itemSetAppearanceBySet[AscDbc::Table::U32(r, 4)] = r;
                g_setsByAppearance[AscDbc::Table::U32(r, 8)].push_back(r);
            }
    }

    // FUN_1020fae0 / FUN_10210030: the ItemAppearances / ItemSetAppearances row keyed by +4.
    const uint8_t* ByKey(AscDbc::Table& t, uint32_t key)
    {
        const bool items = &t == &ItemAppearances();
        if (items ? !g_itemIndexBuilt : !g_setIndexBuilt)
            items ? BuildItemAppearanceIndex() : BuildItemSetAppearanceIndex();
        const auto& index = items ? g_itemAppearanceByItem : g_itemSetAppearanceBySet;
        auto it = index.find(key);
        return it == index.end() ? nullptr : it->second;
    }

    // DAT_10bdfa34 / DAT_10bdfa48: the first AppearanceDetails row (file order) for an appearance.
    const uint8_t* DetailsFor(uint32_t appearance)
    {
        AscDbc::Table& t = Details();
        for (uint32_t i = 0; i < t.Count(); ++i)
            if (const uint8_t* r = t.RowAt(i))
                if (AscDbc::Table::U32(r, 4) == appearance)
                    return r;
        return nullptr;
    }

    uint32_t At(const std::vector<uint32_t>& v, uint32_t i) { return i < v.size() ? v[i] : 0; }

    void PendingChanged() { AscRuntime::Signal("PENDING_APPEARANCE_CHANGED"); }

    // ---- collected (FUN_100c57a0) ----------------------------------------------------------------
    bool IsCollected(uint32_t appearance);

    // FUN_100c3ba0: an item set's appearances per category -- for each set item (client ItemSet.dbc
    // +8..+0x48) whose item record's inventory type (+0x18; 0x14 robe counts as 5 chest) matches an
    // ITEM category's +0x10, and that inventory type is one of the tracked slots (DAT_10bdb264), the
    // item's ItemAppearances appearance goes in that category's index.
    void ItemSetAppearanceList(const uint8_t* itemSet, std::vector<uint32_t>& out);

    bool IsCollected(uint32_t appearance)
    {
        if (const uint8_t* a = Appearances().Row(appearance))
            if (const uint8_t* set = ClientDbcRow(kItemSetDbc, AscDbc::Table::U32(a, 0x20)))
            {
                std::vector<uint32_t> parts;
                ItemSetAppearanceList(set, parts);
                for (uint32_t p : parts)
                    if (p && !IsCollected(p))
                        return false;
                return true;
            }
        for (const Collected& c : g.collected)
            if (c.id == appearance)
                return true;
        return false;
    }

    // DAT_10bdb264 (FUN_100059a0): the inventory types an ITEM category may take from an item set.
    const uint32_t kSetSlots[12] = {1, 3, 4, 5, 6, 7, 8, 9, 10, 16, 19, 20};

    void ItemSetAppearanceList(const uint8_t* itemSet, std::vector<uint32_t>& out)
    {
        AscDbc::Table& cats = Categories();
        out.assign(cats.MaxId(), 0);   // FUN_100ce6b0(max) + memset
        for (uint32_t k = 0; k < 17; ++k)
        {
            const uint32_t item = AscDbc::Table::U32(itemSet, 8 + k * 4);
            const uint8_t* itemRow = ClientDbcRow(0xAD3D4C, item);   // Item.dbc (FUN_100b1870)
            if (!itemRow)
                continue;
            uint32_t inv = AscDbc::Table::U32(itemRow, 0x18);
            if (inv == 0x14)
                inv = 5;   // robe -> chest
            for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id)
            {
                const uint8_t* c = cats.Row(id);
                if (!c || AscDbc::Table::U32(c, 0x10) != inv || CategoryType(c) != TYPE_ITEM)
                    continue;
                if (std::find(std::begin(kSetSlots), std::end(kSetSlots), inv) != std::end(kSetSlots)
                    && AscDbc::Table::U32(c, 0) < out.size())
                {
                    const uint8_t* ia = ByKey(ItemAppearances(), item);
                    out[AscDbc::Table::U32(c, 0)] = ia ? AscDbc::Table::U32(ia, 8) : 0;
                }
                break;
            }
        }
    }

    bool g_browserDirty = true;   // AppearancesEntryContainer (+0xC) reset flag, read by the browser

    // FUN_100c6750: reset the browser (+0xC, FUN_100c6670) and the outfit view (+0xC8, FUN_100c65d0);
    // in world (0xBD0792), VIEWABLE_APPEARANCES_RESET.
    void RebuildOutfits()
    {
        g_browserDirty = true;
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
            AscRuntime::Signal("VIEWABLE_APPEARANCES_RESET");
    }

    // ---- SMSG ------------------------------------------------------------------------------------
    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    uint8_t U8(CDataStore* p) { const uint8_t v = static_cast<uint8_t>(p->m_buffer[p->m_read]); p->m_read += 1; return v; }
    std::string Str(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }
    int ResultIndex(const std::string& s, const char* const* names, int n)
    {
        for (int i = 0; i < n; ++i)
            if (s == names[i])
                return i;
        return -1;
    }

    void __cdecl OnApplyResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x698
    {
        const std::string result = Str(p);
        const int i = ResultIndex(result, kApplyResults, 29);
        if (i == 0)
            g.current = g.pending;
        AscRuntime::Signal("APPLY_PENDING_APPEARANCE_RESULT", "%b%s", i == 0, result.c_str());
    }

    void __cdecl OnCollectedList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x699
    {
        g.collected.clear();
        for (uint32_t n = U32(p); n; --n)
        {
            const uint32_t id = U32(p);
            const uint32_t time = U32(p);
            g.collected.push_back({id, 0, time});
        }
    }

    void __cdecl OnCurrent(void*, uint32_t, uint32_t, CDataStore* p)   // 0x69A
    {
        g.current.clear();
        for (uint32_t n = U32(p); n; --n)
            g.current.push_back(U32(p));
        g.pending = g.current;
    }

    void __cdecl OnCollected(void*, uint32_t, uint32_t, CDataStore* p)   // 0x69B
    {
        const uint32_t id = U32(p);
        const uint32_t second = U32(p);
        if (!IsCollected(id))
        {
            g.collected.push_back({id, 0, static_cast<uint64_t>(_time64(nullptr))});
            AscRuntime::Signal("APPEARANCE_COLLECTED", "%u%u", id, second);
        }
    }

    void __cdecl OnUncollected(void*, uint32_t, uint32_t, CDataStore* p)   // 0x69C
    {
        const uint32_t id = U32(p);
        const uint32_t second = U32(p);
        if (IsCollected(id))
        {
            g.collected.erase(std::remove_if(g.collected.begin(), g.collected.end(),
                [id](const Collected& c) { return c.id == id; }), g.collected.end());
            AscRuntime::Signal("APPEARANCE_UNCOLLECTED", "%u%u", id, second);
        }
    }

    void RebuildOutfits();   // FUN_100c6750

    void __cdecl OnOutfits(void*, uint32_t, uint32_t, CDataStore* p)   // 0x69D
    {
        g.outfits.clear();   // FUN_100cd0f0
        for (uint32_t n = U32(p); n; --n)
        {
            std::vector<uint32_t>& list = g.outfits[Str(p)];
            for (uint32_t m = U32(p); m; --m)
                list.push_back(U32(p));
        }
        RebuildOutfits();
    }

    void __cdecl OnOutfitSaved(void*, uint32_t, uint32_t, CDataStore* p)   // 0x69F
    {
        const std::string result = Str(p);
        const int i = ResultIndex(result, kSaveResults, 2);
        if (i == 0)
        {
            g.outfits[g.pendingOutfitName] = g.pendingOutfit;
            g.pendingOutfitName.clear();
            g.pendingOutfit.clear();
            RebuildOutfits();
        }
        AscRuntime::Signal("APPEARANCE_OUTFIT_SAVED_RESULT", "%b%s", i == 0, result.c_str());
    }

    void __cdecl OnOutfitDeleted(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6A1
    {
        const std::string result = Str(p);
        const int i = ResultIndex(result, kDeleteResults, 2);
        if (i == 0)
        {
            g.outfits.erase(g.pendingOutfitName);
            g.pendingOutfitName.clear();
            RebuildOutfits();
        }
        AscRuntime::Signal("APPEARANCE_OUTFIT_DELETED_RESULT", "%b%s", i == 0, result.c_str());
    }

    void __cdecl OnVisibility(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6A2
    {
        g.seeOwn = U8(p) != 0;
        g.seeOthers = U8(p) != 0;
    }

    void __cdecl OnCollectItemResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6EB
    {
        const std::string result = Str(p);
        AscRuntime::Signal("COLLECT_ITEM_APPEARANCE_RESULT", "%s", result.c_str());
    }

    // ---- C_Appearance ----------------------------------------------------------------------------
    // handler_GetAppearanceForCategory: current[category], nil when 0 / out of range.
    int GetAppearanceForCategory(lua_State* L)
    {
        uint32_t category;
        if (!ReadNumber(L, category))
            return 0;
        const uint32_t a = At(g.current, category);
        if (a)
            AscLua::lua_pushinteger(L, static_cast<int>(a));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetItemAppearanceID(lua_State* L)   // handler_GetItemAppearanceID
    {
        uint32_t item;
        if (!ReadNumber(L, item))
            return 0;
        if (const uint8_t* r = ByKey(ItemAppearances(), item))
            AscLua::lua_pushinteger(L, static_cast<int>(AscDbc::Table::U32(r, 8)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetItemSetAppearanceID(lua_State* L)   // handler_GetItemSetAppearanceID
    {
        uint32_t set;
        if (!ReadNumber(L, set))
            return 0;
        if (const uint8_t* r = ByKey(ItemSetAppearances(), set))
            AscLua::lua_pushinteger(L, static_cast<int>(AscDbc::Table::U32(r, 8)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // handler_HasPendingAppearances: any index where pending and current differ.
    int HasPendingAppearances(lua_State* L)
    {
        const size_t n = std::max(g.pending.size(), g.current.size());
        bool any = false;
        for (uint32_t i = 0; i < n && !any; ++i)
            any = At(g.pending, i) != At(g.current, i);
        PushBool(L, any);
        return 1;
    }

    // handler_GetPendingAppearance: pending[category] when it differs from current, else nil.
    int GetPendingAppearance(lua_State* L)
    {
        uint32_t category;
        if (!ReadNumber(L, category))
            return 0;
        const uint32_t p = At(g.pending, category);
        if (p == At(g.current, category))
            AscLua::lua_pushnil(L);
        else
            AscLua::lua_pushinteger(L, static_cast<int>(p));
        return 1;
    }

    // FUN_100c21f0: put one category back to current (growing pending to current's size first).
    void ClearPending(uint32_t category)
    {
        if (At(g.pending, category) == At(g.current, category))
            return;
        if (g.pending.size() < g.current.size())
        {
            const size_t from = g.pending.size();
            g.pending.resize(g.current.size());
            for (size_t i = from; i < g.current.size(); ++i)
                g.pending[i] = g.current[i];
            PendingChanged();
        }
        if (category < g.pending.size())
        {
            // The original indexes current unchecked (the server always sends 0x69A at login); a server
            // that never sends it leaves current empty, so read it bounded.
            g.pending[category] = At(g.current, category);
            PendingChanged();
        }
    }

    int ClearPendingAppearance(lua_State* L)   // handler_ClearPendingAppearance
    {
        uint32_t category;
        if (ReadNumber(L, category))
            ClearPending(category);
        return 0;
    }

    int ClearPendingAppearances(lua_State*)   // handler_ClearPendingAppearances
    {
        if (g.pending != g.current)
        {
            g.pending = g.current;
            PendingChanged();
        }
        return 0;
    }

    int CanSeeAppearances(lua_State* L)   // handler_CanSeeAppearances
    {
        PushBool(L, g.seeOwn);
        PushBool(L, g.seeOthers);
        return 2;
    }

    // FUN_100cb2e0: (own, others) -> CMSG 0x6A3 {u8, u8}, flags updated at once, true.
    int SetCanSeeAppearances(lua_State* L)
    {
        if (!ValidateInput(L, {BOOLEAN, BOOLEAN}))
            return 0;
        const bool own = AscLua::lua_toboolean(L, 1) != 0;
        const bool others = AscLua::lua_toboolean(L, 2) != 0;
        Packet(0x6A3).U8(own ? 1 : 0).U8(others ? 1 : 0).Send();
        g.seeOwn = own;
        g.seeOthers = others;
        PushBool(L, true);
        return 1;
    }

    // handler_IsTransmogable: every category except MOUNT / COMPANION / TOY.
    int IsTransmogable(lua_State* L)
    {
        uint32_t category;
        if (!ReadNumber(L, category))
            return 0;
        PushBool(L, !CategoryIs(category, TYPE_MOUNT) && !CategoryIs(category, TYPE_COMPANION)
            && !CategoryIs(category, TYPE_TOY));
        return 1;
    }

    int GetActiveDiscount(lua_State* L)   // handler_GetActiveDiscount: (id) -> false, nil, nil
    {
        uint32_t unused;
        if (!ReadNumber(L, unused))
            return 0;
        PushBool(L, false);
        AscLua::lua_pushnil(L);
        AscLua::lua_pushnil(L);
        return 3;
    }

    int IsEtherealBazaarAppearance(lua_State* L)   // handler_IsEtherealBazaarAppearance
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        const uint8_t* d = DetailsFor(appearance);
        PushBool(L, d && AscDbc::Table::U32(d, 0x10) != 0);
        return 1;
    }

    // FUN_100c7f60: the details text, nil when absent or empty.
    int GetAppearanceDetails(lua_State* L)
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        const uint8_t* d = DetailsFor(appearance);
        const char* text = d ? Details().Str(d, 8) : "";
        if (d && *text)
            PushStr(L, text);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_100c4120 / FUN_100c3ff0: "store/{realm +0x4C}/{product}/game" and ".../checkout"; empty when
    // the appearance has no details row or no product.
    std::string StoreUrl(uint32_t appearance, const char* tail)
    {
        const uint8_t* d = DetailsFor(appearance);
        const uint32_t product = d ? AscDbc::Table::U32(d, 0xC) : 0;
        if (!product)
            return std::string();
        return std::string("store/") + RealmInfoSvc::Get().dataPath + "/" + std::to_string(product) + "/" + tail;
    }

    // FUN_100c8950: (appearance) -> game URL (or nil), and the checkout URL when there is one.
    int GetAppearanceWebURL(lua_State* L)
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        const std::string game = StoreUrl(appearance, "game");
        if (game.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, game.c_str());
        const std::string checkout = StoreUrl(appearance, "checkout");
        if (!checkout.empty())
        {
            PushStr(L, checkout.c_str());
            return 2;
        }
        AscLua::lua_pushnil(L);
        return 2;
    }

    // FUN_100ca120: the store pages of product 0x93, both always pushed.
    int GetEtherealBazaarWebURL(lua_State* L)
    {
        const std::string base = std::string("store/") + RealmInfoSvc::Get().dataPath + "/" + std::to_string(0x93) + "/";
        PushStr(L, (base + "game").c_str());
        PushStr(L, (base + "checkout").c_str());
        return 2;
    }

    // ---- the validator (FUN_100c1b40) -------------------------------------------------------------
    // The player's class (UNIT_FIELD_BYTES_0 byte 1), 0 unless the object is a unit/player (+0x14 3 / 4).
    uint32_t PlayerClass(uint8_t* player)
    {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(player + 0x14);
        if (type != 3 && type != 4)
            return 0;
        return (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(player + 8) + 0x5C) >> 8) & 0xFF;
    }
    bool StockClass(uint8_t* player)   // FUN_100c6380
    {
        const uint32_t c = PlayerClass(player);
        return (c >= 1 && c <= 9) || c == 0xB;
    }
    bool CoAClass(uint8_t* player)     // FUN_100c5f50
    {
        const uint32_t c = PlayerClass(player);
        return c >= 0xC && c <= 0x20;
    }

    // FUN_101b0b00: the category's 64-bit class mask (+0x1C / +0x20) has the player's class bit.
    bool CategoryAllowsClass(const uint8_t* category)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        const uint32_t bit = PlayerClass(player) - 1;
        uint32_t lo = 1u << (bit & 0x1F), hi = 0;
        if (bit > 0x1F)
            hi = lo;
        lo ^= hi;
        if (bit > 0x3F)
            hi = lo;
        return (lo & AscDbc::Table::U32(category, 0x1C)) != 0 || (hi & AscDbc::Table::U32(category, 0x20)) != 0;
    }

    // FUN_101b49f0: an Appearances row's realm gates (+0x30 live, +0x34 seasonal, +0x38 league, +0x3C PTR,
    // +0x40 development) against the realm object +0x40.. flags.
    bool RealmAppropriate(const uint8_t* a)
    {
        const RealmInfo& r = RealmInfoSvc::Get();
        return (AscDbc::Table::U32(a, 0x30) && r.Live()) || (AscDbc::Table::U32(a, 0x34) && r.Seasonal())
            || (AscDbc::Table::U32(a, 0x38) && r.League()) || (AscDbc::Table::U32(a, 0x3C) && r.PTR())
            || (AscDbc::Table::U32(a, 0x40) && r.Dev());
    }

    // FUN_100b8cc0: the DLL's creature-template cache -- DBFilesClient\Creature.dbc, which the DBC manager
    // (FUN_101d3d20 -> FUN_101fd240) moves into the MemoryBridge store; keyed by creature entry.
    bool CreatureTemplateKnown(uint32_t entry) { return AscBridgeStore::CreatureByEntry(entry) != nullptr; }

    // The summon (first) / dismiss (second) spell a cosmetic pet of tier 1..5 needs.
    uint32_t PetSpell(uint8_t* player, uint32_t tier, bool dismiss)
    {
        if (!player)
            return 0;
        const bool stock = StockClass(player);
        switch (tier)
        {
        case 1: return stock ? (dismiss ? 0x10D331 : 0x10CC53) : CoAClass(player) ? (dismiss ? 0x8C35F : 0x8C35E) : (dismiss ? 0xA51 : 0x373);
        case 2: return stock ? (dismiss ? 0x10CC5C : 0x10CC54) : (dismiss ? 0x37C : 0x374);
        case 3: return dismiss ? 0x37D : 0x375;
        case 4: return stock ? (dismiss ? 0x15FF73 : 0x15FF42) : (dismiss ? 0x16603 : 0x165D2);
        case 5: return stock ? (dismiss ? 0x15FF74 : 0x15FF5F) : (dismiss ? 0x16604 : 0x165EF);
        default: return 0;
        }
    }

    // FUN_100c1b40(list, category, confirmed): 0 or an APPLY_APPEARANCES_* index for list[category].
    uint32_t Validate(const std::vector<uint32_t>& list, uint32_t category, bool confirmed)
    {
        if (category >= list.size())
            return 2;                                   // OUT_OF_RANGE
        uint8_t* player = ActivePlayer();
        if (!player)
            return 3;                                   // NOT_IN_WORLD
        const uint32_t appearance = list[category];
        if (appearance == At(g.current, category) || appearance == 0)
            return 0;
        const uint8_t* a = Appearances().Row(appearance);
        if (!a)
            return 5;                                   // BAD_APPEARANCE
        if (!RealmAppropriate(a))
            return 0xB;                                 // NOT_REALM_APPROPRIATE
        bool inCategory = false;
        for (uint32_t k = 0; k < 3; ++k)
            inCategory = inCategory || AscDbc::Table::U32(a, 0x14 + k * 4) == category;
        if (!inCategory)
            return 6;                                   // BAD_CATEGORY
        const uint8_t* cat = Categories().Row(category);
        if (!cat)
            return 7;                                   // CATEGORY_NOT_FOUND
        if (!CategoryAllowsClass(cat))
            return 8;                                   // NOT_AVAILABLE_FOR_CLASS
        if (!IsCollected(AscDbc::Table::U32(a, 0)))
            return 9;                                   // NOT_COLLECTED

        const int type = CategoryType(cat);
        if (type == TYPE_ITEM)
        {
            const uint8_t* item = ClientDbcRow(0xAD3D4C, AscDbc::Table::U32(a, 0x20));   // Item.dbc
            if (!item)
                return 0xC;                             // BAD_ITEM_TEMPLATE
            const uint32_t cls = AscDbc::Table::U32(item, 4);
            if (cls != 4 && cls != 2)
                return 0xD;                             // BAD_ITEM_CLASS
            const bool* thrown = AscConfig::Bool("CONFIG_THROWN_WEAPON_APPEARANCES_ENABLED");
            if (!(thrown && *thrown) && cls == 2 && AscDbc::Table::U32(item, 8) == 0x10)
                return 0xE;                             // THROWN_APPEARANCE
        }
        else if (type != TYPE_ILLUSION && type != TYPE_INCARNATION && type != TYPE_SPELL_VISUAL)
        {
            if (type == TYPE_COSMETIC_PET)
            {
                if (!CreatureTemplateKnown(AscDbc::Table::U32(a, 0x20)))
                    return 0x11;                        // BAD_CREATURE_TEMPLATE
                const uint32_t tier = AscDbc::Table::U32(cat, 0xC);
                if (tier > 9)
                    return 0x15;                        // PET_TYPE_NOT_FOUND
                uint8_t rec[0x2A8];
                const uint32_t summon = PetSpell(ActivePlayer(), tier, false);
                if (!FetchSpell(summon, rec))
                    return 0x16;                        // PET_SUMMON_SPELL_NOT_FOUND
                typedef char(__thiscall * Knows_t)(void*, uint32_t);
                if (!reinterpret_cast<Knows_t>(0x7260E0)(player, summon))
                    return 0x17;                        // PET_SUMMON_SPELL_NOT_LEARNED
                const uint32_t dismiss = PetSpell(ActivePlayer(), tier, true);
                if (!FetchSpell(dismiss, rec))
                    return 0x18;                        // PET_DISMISS_SPELL_NOT_FOUND
                if (!reinterpret_cast<Knows_t>(0x7260E0)(player, dismiss))
                    return 0x19;                        // PET_DISMISS_SPELL_NOT_LEARNED
            }
            else if (type != TYPE_COSMETIC)
                return type == TYPE_ITEM_SET ? 10 : 0x1B;   // CANNOT_APPLY_ITEM_SET / UNSUPPORTED_CATEGORY
        }
        if (!confirmed && type == TYPE_COSMETIC_PET)
            return 0x1C;                                // COSMETIC_PET_APPEARANCE_CONFIRMATION
        return 0;
    }

    // FUN_100be920: the result's name, or UNEXPECTED_ENUM_VALUE_<n>.
    std::string ResultName(uint32_t code)
    {
        return code < 29 ? kApplyResults[code] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(code);
    }

    // FUN_100cb740: grow pending to category + 1 (zeros), set it, PENDING_APPEARANCE_CHANGED on change.
    void SetPending(uint32_t category, uint32_t appearance)
    {
        if (category >= g.pending.size())
            g.pending.resize(category + 1, 0);
        if (g.pending[category] != appearance)
        {
            g.pending[category] = appearance;
            PendingChanged();
        }
    }

    // FUN_100cb8b0: an item set's parts into the pending ITEM categories whose inventory type is a set
    // slot; PENDING_APPEARANCE_CHANGED when anything changed.
    void SetPendingItemSet(const uint8_t* itemSet)
    {
        const std::vector<uint32_t> before = g.pending;
        std::vector<uint32_t> parts;
        ItemSetAppearanceList(itemSet, parts);
        AscDbc::Table& cats = Categories();
        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id)
        {
            const uint8_t* c = cats.Row(id);
            if (!c || CategoryType(c) != TYPE_ITEM)
                continue;
            const uint32_t inv = AscDbc::Table::U32(c, 0x10);
            if (std::find(std::begin(kSetSlots), std::end(kSetSlots), inv) == std::end(kSetSlots))
                continue;
            const uint32_t idx = AscDbc::Table::U32(c, 0);
            if (idx < parts.size() && idx < g.pending.size() && parts[idx] != g.pending[idx])
                g.pending[idx] = parts[idx];
        }
        if (g.pending != before)
            PendingChanged();
    }

    // FUN_100cb4a0: (category, appearance) -- an ITEM_SET category takes the whole set (false when the
    // appearance or its set (+4) is unknown); anything else sets the one category. Pushes success.
    int SetPendingAppearance(lua_State* L)
    {
        int32_t cat32, app32;
        if (!ReadInt2(L, cat32, app32))
            return 0;
        const uint32_t category = static_cast<uint32_t>(cat32), appearance = static_cast<uint32_t>(app32);
        bool ok = true;
        if (CategoryIs(category, TYPE_ITEM_SET))
        {
            const uint8_t* a = Appearances().Row(appearance);
            const uint8_t* set = a ? ClientDbcRow(kItemSetDbc, AscDbc::Table::U32(a, 4)) : nullptr;
            if (set)
                SetPendingItemSet(set);
            else
                ok = false;
        }
        else
            SetPending(category, appearance);
        PushBool(L, ok);
        return 1;
    }

    // FUN_100c7350: (category, appearance) -> ok, error name, and for NOT_COLLECTED the store game /
    // checkout URLs (else nil).
    int CanSetAppearance(lua_State* L)
    {
        int32_t cat32, app32;
        if (!ReadInt2(L, cat32, app32))
            return 0;
        const uint32_t category = static_cast<uint32_t>(cat32), appearance = static_cast<uint32_t>(app32);
        std::vector<uint32_t> list = g.pending;
        std::vector<uint32_t> check;
        const uint8_t* a = Appearances().Row(appearance);
        const uint8_t* set = (CategoryIs(category, TYPE_ITEM_SET) && a)
            ? ClientDbcRow(kItemSetDbc, AscDbc::Table::U32(a, 4)) : nullptr;
        if (!CategoryIs(category, TYPE_ITEM_SET))
        {
            if (category < list.size())
                list[category] = appearance;
            check.push_back(category);
        }
        else if (set)
        {
            std::vector<uint32_t> parts;
            ItemSetAppearanceList(set, parts);
            for (uint32_t i = 0; i < parts.size(); ++i)
            {
                if (i < list.size())
                    list[i] = parts[i];
                check.push_back(i);
            }
        }
        uint32_t err = 0;
        for (uint32_t c : check)
            if ((err = Validate(list, c, true)) != 0)
                break;
        PushBool(L, err == 0);
        if (err == 0)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, ResultName(err).c_str());
        const std::string game = err == 9 ? StoreUrl(appearance, "game") : std::string();
        if (game.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, game.c_str());
        const std::string checkout = err == 9 ? StoreUrl(appearance, "checkout") : std::string();
        if (checkout.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, checkout.c_str());
        return 4;
    }

    // FUN_100c6de0: (confirmed) -- every changed category must validate, else
    // APPLY_PENDING_APPEARANCE_RESULT(false, error) and false; then CMSG 0x697 {u32 n, u32 x n} and true.
    int ApplyPendingAppearances(lua_State* L)
    {
        if (!ValidateInput(L, {BOOLEAN}))
            return 0;
        const bool confirmed = AscLua::lua_toboolean(L, 1) != 0;
        for (uint32_t i = 0; i < g.pending.size(); ++i)
            if (At(g.pending, i) != At(g.current, i))
                if (const uint32_t err = Validate(g.pending, i, confirmed))
                {
                    AscRuntime::Signal("APPLY_PENDING_APPEARANCE_RESULT", "%b%s", false, ResultName(err).c_str());
                    PushBool(L, false);
                    return 1;
                }
        Packet p(0x697);
        p.U32(static_cast<uint32_t>(g.pending.size()));
        for (uint32_t v : g.pending)
            p.U32(v);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // handler_CanApplyPendingAppearances: the first failing category's error, else true, nil.
    int CanApplyPendingAppearances(lua_State* L)
    {
        AscDbc::Table& cats = Categories();
        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id)
            if (cats.Row(id))
                if (const uint32_t err = Validate(g.pending, id, true))
                {
                    PushBool(L, false);
                    PushStr(L, err <= 0x1C ? kApplyResults[err] : "");
                    return 2;
                }
        PushBool(L, true);
        AscLua::lua_pushnil(L);
        return 2;
    }

    // FUN_100c77c0: every category whose pending appearance fails validation goes back to current.
    int ClearInvalidPendingAppearances(lua_State*)
    {
        AscDbc::Table& cats = Categories();
        for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id)
            if (cats.Row(id) && Validate(g.pending, id, true) != 0)
                ClearPending(id);
        return 0;
    }

    // ---- display info / item sets / alternates ---------------------------------------------------
    const char* const kDisplayTypes[8] = {"APPEARANCE_DISPLAY_TYPE_NONE", "APPEARANCE_DISPLAY_TYPE_ITEM",
        "APPEARANCE_DISPLAY_TYPE_CREATURE", "APPEARANCE_DISPLAY_TYPE_SPELL", "APPEARANCE_DISPLAY_TYPE_ITEM_ENCHANT",
        "APPEARANCE_DISPLAY_TYPE_ITEM_SET", "APPEARANCE_DISPLAY_TYPE_STATIC_SPELL_VISUAL",
        "APPEARANCE_DISPLAY_TYPE_ANIMATED_SPELL_VISUAL"};   // 0x10B3AEF0 (FUN_101b28a0)

    // Client DBC rows carry resolved string pointers.
    const char* ClientStr(const uint8_t* row, uint32_t off)
    {
        const char* s = *reinterpret_cast<const char* const*>(row + off);
        return s ? s : "";
    }

    bool HasCategoryOfType(const uint8_t* a, int type)   // the +0x14..+0x1C category loop
    {
        for (uint32_t k = 0; k < 3; ++k)
        {
            const uint32_t c = AscDbc::Table::U32(a, 0x14 + k * 4);
            if (c && CategoryIs(c, type))
                return true;
        }
        return false;
    }

    // FUN_101b46e0: a UICameras row -- +0x10 directly, else for an ITEM display the item's camera
    // (FUN_1021a020, shared with C_UICamera.GetItemCameraID).
    const uint8_t* DisplayCamera(const uint8_t* a)
    {
        const uint32_t cam = AscDbc::Table::U32(a, 0x10);
        if (cam)
            return AscDbc::Get("DBFilesClient/UICameras.dbc").Row(cam);
        if (strcmp(Appearances().Str(a, 8), kDisplayTypes[1]) == 0)
            return AscUICamera::ItemCamera(AscDbc::Table::U32(a, 0xC));
        return nullptr;
    }

    // FUN_101b41e0: an item set appearance's set name (ItemSet.dbc +4, "Unknown Item Set"), else the
    // item's name from FUN_1020e9c0 (AscItemAddon::GetInfo), "Unknown Item" when it is empty.
    std::string AppearanceName(const uint8_t* a)
    {
        const uint32_t source = AscDbc::Table::U32(a, 4);
        if (HasCategoryOfType(a, TYPE_ITEM_SET))
        {
            const uint8_t* set = ClientDbcRow(kItemSetDbc, source);
            return set ? ClientStr(set, 4) : "Unknown Item Set";
        }
        const AscItemAddon::Info info = AscItemAddon::GetInfo(source);
        return info.name.empty() ? "Unknown Item" : info.name;
    }

    // FUN_100c8090: display type, +0xC, camera id, name, source, and for a SPELL_VISUAL category the
    // spell (+0x20); five nils for an unknown appearance.
    int GetAppearanceDisplayInfo(lua_State* L)
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        const uint8_t* a = Appearances().Row(appearance);
        if (!a)
        {
            for (int i = 0; i < 5; ++i)
                AscLua::lua_pushnil(L);
            return 5;
        }
        PushStr(L, Appearances().Str(a, 8));
        AscLua::lua_pushinteger(L, AscDbc::Table::I32(a, 0xC));
        if (const uint8_t* cam = DisplayCamera(a))
            AscLua::lua_pushinteger(L, AscDbc::Table::I32(cam, 0));
        else
            AscLua::lua_pushnil(L);
        PushStr(L, AppearanceName(a).c_str());
        AscLua::lua_pushinteger(L, AscDbc::Table::I32(a, 4));
        if (HasCategoryOfType(a, TYPE_SPELL_VISUAL))
            AscLua::lua_pushinteger(L, AscDbc::Table::I32(a, 0x20));
        else
            AscLua::lua_pushnil(L);
        return 6;
    }

    // FUN_101b5660: item appearance -> the item-set appearances whose set contains that item (ItemSet.dbc
    // in id order, set items +8..+0x48).
    std::unordered_map<uint32_t, std::vector<uint32_t>> g_setsForItemAppearance;
    bool g_setsForItemAppearanceBuilt = false;
    const std::vector<uint32_t>* SetsForItemAppearance(uint32_t appearance)
    {
        auto& s_map = g_setsForItemAppearance;
        if (!g_setsForItemAppearanceBuilt)
        {
            g_setsForItemAppearanceBuilt = true;
            s_map.clear();
            const uint32_t lo = *reinterpret_cast<const uint32_t*>(kItemSetDbc + 0x10);
            const uint32_t hi = *reinterpret_cast<const uint32_t*>(kItemSetDbc + 0x0C);
            for (uint32_t id = lo; id && id <= hi; ++id)
            {
                const uint8_t* set = ClientDbcRow(kItemSetDbc, id);
                const uint8_t* sa = set ? ByKey(ItemSetAppearances(), id) : nullptr;
                if (!sa)
                    continue;
                for (uint32_t k = 0; k < 17; ++k)
                {
                    const uint32_t item = AscDbc::Table::U32(set, 8 + k * 4);
                    const uint8_t* ia = item ? ByKey(ItemAppearances(), item) : nullptr;
                    if (ia)
                        s_map[AscDbc::Table::U32(ia, 8)].push_back(AscDbc::Table::U32(sa, 8));
                }
            }
        }
        auto it = s_map.find(appearance);
        return it == s_map.end() ? nullptr : &it->second;
    }

    // FUN_100c82e0: the first item set (with an ItemSet.dbc row) that uses this appearance: its name
    // (nil if empty), set id and set appearance; else three nils.
    int GetAppearanceItemSet(lua_State* L)
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        if (Appearances().Row(appearance))
            if (const std::vector<uint32_t>* sets = SetsForItemAppearance(appearance))
                for (uint32_t sid : *sets)
                {
                    const uint8_t* sa = Appearances().Row(sid);
                    const uint8_t* set = sa ? ClientDbcRow(kItemSetDbc, AscDbc::Table::U32(sa, 4)) : nullptr;
                    if (!set)
                        continue;
                    const char* name = ClientStr(set, 4);
                    if (*name)
                        PushStr(L, name);
                    else
                        AscLua::lua_pushnil(L);
                    AscLua::lua_pushinteger(L, AscDbc::Table::I32(set, 0));
                    AscLua::lua_pushinteger(L, static_cast<int>(sid));
                    return 3;
                }
        AscLua::lua_pushnil(L);
        AscLua::lua_pushnil(L);
        AscLua::lua_pushnil(L);
        return 3;
    }

    // FUN_1020fc80's second map: appearance -> ItemAppearances rows (file order).
    const std::vector<const uint8_t*>* ItemsForAppearance(uint32_t appearance)
    {
        if (!g_itemIndexBuilt)
            BuildItemAppearanceIndex();
        auto it = g_itemsForAppearance.find(appearance);
        return it == g_itemsForAppearance.end() ? nullptr : &it->second;
    }

    // FUN_100c7c70: (appearance, item) -> the other items with that appearance.
    int GetAlternativeIDs(lua_State* L)
    {
        int32_t app32, item32;
        if (!ReadInt2(L, app32, item32))
            return 0;
        std::vector<uint32_t> out;
        if (const std::vector<const uint8_t*>* rows = ItemsForAppearance(static_cast<uint32_t>(app32)))
            for (const uint8_t* r : *rows)
                if (AscDbc::Table::I32(r, 4) != item32)
                    out.push_back(AscDbc::Table::U32(r, 4));
        AscLua::lua_createtable(L, 0, static_cast<int>(out.size()));   // FUN_1009a460
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < out.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(out[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_100c9de0: an appearance whose every category is an INCARNATION with tier 2 and with +0x24 set
    // -> {+0x24}; else nil.
    int GetCreatureDisplayItems(lua_State* L)
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        const uint8_t* a = Appearances().Row(appearance);
        bool ok = a != nullptr;
        for (uint32_t k = 0; ok && k < 3; ++k)
        {
            const uint32_t c = AscDbc::Table::U32(a, 0x14 + k * 4);
            if (!c)
                continue;
            const uint8_t* cat = Categories().Row(c);
            ok = cat && CategoryType(cat) == TYPE_INCARNATION && AscDbc::Table::U32(cat, 0xC) == 2;
        }
        if (ok && AscDbc::Table::U32(a, 0x24))
        {
            AscLua::lua_createtable(L, 0, 1);
            AscLua::lua_checkstack(L, 2);
            AscLua::lua_pushnumber(L, 1.0);
            AscLua::lua_pushinteger(L, AscDbc::Table::I32(a, 0x24));
            AscLua::lua_settable(L, -3);
            return 1;
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- the collection browser ------------------------------------------------------------------
    const char* const kFilters[38] = {"FILTER_NONE", "FILTER_COLLECTED", "FILTER_UNCOLLECTED",
        "FILTER_WEAPON_AXE", "FILTER_WEAPON_AXE2", "FILTER_WEAPON_BOW", "FILTER_WEAPON_GUN",
        "FILTER_WEAPON_MACE", "FILTER_WEAPON_MACE2", "FILTER_WEAPON_POLEARM", "FILTER_WEAPON_SWORD",
        "FILTER_WEAPON_SWORD2", "FILTER_WEAPON_STAFF", "FILTER_WEAPON_FIST", "FILTER_WEAPON_DAGGER",
        "FILTER_WEAPON_SPEAR", "FILTER_WEAPON_CROSSBOW", "FILTER_WEAPON_WAND", "FILTER_WEAPON_FISHING_POLE",
        "FILTER_WEAPON_THROWN", "FILTER_ARMOR_CLOTH", "FILTER_ARMOR_LEATHER", "FILTER_ARMOR_MAIL",
        "FILTER_ARMOR_PLATE", "FILTER_ARMOR_SHIELD", "FILTER_INVTYPE_MAINHAND", "FILTER_INVTYPE_OFFHAND",
        "FILTER_INVTYPE_HOLDABLE", "FILTER_AVAILABLE_ON_THE_WEB_STORE", "FILTER_AVAILABLE_ON_THE_ETHEREAL_BAZAAR",
        "FILTER_ITEM_QUALITY_POOR", "FILTER_ITEM_QUALITY_NORMAL", "FILTER_ITEM_QUALITY_UNCOMMON",
        "FILTER_ITEM_QUALITY_RARE", "FILTER_ITEM_QUALITY_EPIC", "FILTER_ITEM_QUALITY_LEGENDARY",
        "FILTER_ITEM_QUALITY_ARTIFACT", "FILTER_ITEM_QUALITY_HEIRLOOM"};   // 0x10B29C70
    const char* const kSorts[7] = {"SORT_NONE", "SORT_RECENTLY_COLLECTED_ASC", "SORT_RECENTLY_COLLECTED_DESC",
        "SORT_ALPHABETICAL_ASC", "SORT_ALPHABETICAL_DESC", "SORT_ID_ASC", "SORT_ID_DESC"};   // 0x10B29A74
    const uint32_t kPageSize = 18;

    bool IsItemDisplay(const uint8_t* a) { return strcmp(Appearances().Str(a, 8), kDisplayTypes[1]) == 0; }   // FUN_101b28a0 == 1

    // FUN_100c3ab0: the collected entry's time, 0 when not collected.
    uint64_t CollectedTime(uint32_t appearance)
    {
        for (const Collected& c : g.collected)
            if (c.id == appearance)
                return c.time;
        return 0;
    }

    // The browser's item text caches (manager +0x7C item -> lower-case name, +0x9C item -> quality),
    // filled by the prefetch FUN_101b4a70 before each query. Only ever cleared by the item addon refresh
    // FUN_101b5420, which the shipped MMgr64.exe never lets run (IMPROVEMENTS.md).
    std::unordered_map<uint32_t, std::string> g_itemNames;
    std::unordered_map<uint32_t, uint32_t> g_itemQualities;
    std::string ItemNameLower(uint32_t item)
    {
        auto it = g_itemNames.find(item);
        return it == g_itemNames.end() ? std::string() : it->second;
    }
    int ItemQuality(uint32_t item)
    {
        auto it = g_itemQualities.find(item);
        return it == g_itemQualities.end() ? -1 : static_cast<int>(it->second);
    }

    // FUN_101b3450: queue `item` unless its name is cached or it is already queued.
    void QueueItem(std::vector<uint32_t>& queue, std::unordered_set<uint32_t>& queued, uint32_t item)
    {
        if (g_itemNames.count(item) == 0 && queued.insert(item).second)
            queue.push_back(item);
    }

    // FUN_102100b0: the item sets whose ItemSetAppearances row points at this appearance.
    std::vector<uint32_t> SetsWithAppearance(uint32_t appearance)
    {
        static std::unordered_map<uint32_t, std::vector<uint32_t>> s_map;
        if (s_map.empty())
        {
            AscDbc::Table& t = ItemSetAppearances();
            for (uint32_t i = 0; i < t.Count(); ++i)
                if (const uint8_t* r = t.RowAt(i))
                    s_map[AscDbc::Table::U32(r, 8)].push_back(AscDbc::Table::U32(r, 4));
        }
        auto it = s_map.find(appearance);
        return it == s_map.end() ? std::vector<uint32_t>() : it->second;
    }

    // AppearancesEntryContainer (vtable 0x10B2A398), entries are Appearances rows.
    class AppearanceBrowser : public AscFilter::Container<const uint8_t*>
    {
    protected:
        // FUN_101b41b0 -> FUN_101b4a70: before the query, fetch the text the query will need. Runs when there
        // is filter text or a name sort (3 / 4), a quality filter (0x1E..0x25), or an item filter
        // (3..0x1B). For every item-display appearance that is not an item set and, when a category is
        // selected (manager +0x144 > 0), lies in it: with text / name sort its own item (+4), and with
        // text, name sort or quality filter every ItemAppearances row's item. One FetchInfo batch, then
        // the names (lower-cased) and qualities go into the caches.
        void BeforeFilter(const std::string& text, const std::vector<uint32_t>& args, const std::vector<uint32_t>& sorts) override
        {
            bool byText = !text.empty();
            for (uint32_t s : sorts)
                byText = byText || s == 3 || s == 4;
            bool byQuality = false, byItem = false;
            for (uint32_t f : args)
                byQuality = byQuality || (f >= 0x1E && f <= 0x25);
            for (uint32_t f : args)
                byItem = byItem || (f >= 3 && f <= 0x1B);
            if (!byText && !byQuality && !byItem)
                return;
            const int32_t category = static_cast<int32_t>(g.unk144);
            std::vector<uint32_t> queue;
            std::unordered_set<uint32_t> queued;
            for (const uint8_t* e : All())
            {
                if (!e || HasCategoryOfType(e, TYPE_ITEM_SET) || !IsItemDisplay(e))
                    continue;
                if (category > 0)
                {
                    bool in = false;
                    for (uint32_t k = 0; k < 3; ++k)
                        in = in || AscDbc::Table::U32(e, 0x14 + k * 4) == static_cast<uint32_t>(category);
                    if (!in)
                        continue;
                }
                if (byText)
                    QueueItem(queue, queued, AscDbc::Table::U32(e, 4));
                else if (!byQuality)
                    continue;
                if (const std::vector<const uint8_t*>* rows = ItemsForAppearance(AscDbc::Table::U32(e, 0)))
                    for (const uint8_t* r : *rows)
                        if (r)
                            QueueItem(queue, queued, AscDbc::Table::U32(r, 4));
            }
            std::unordered_map<uint32_t, AscItemAddon::Info> info;
            if (!AscItemAddon::FetchInfo(queue.data(), static_cast<uint32_t>(queue.size()), info))
                return;
            for (const auto& i : info)
            {
                g_itemNames[i.first] = AscFilter::Lower(i.second.name);
                g_itemQualities[i.first] = i.second.quality;
            }
        }

        // [0] 0x101B5930: every realm-appropriate appearance, less thrown weapons unless enabled.
        std::vector<const uint8_t*> DoPopulate() override
        {
            const bool* thrownCfg = AscConfig::Bool("CONFIG_THROWN_WEAPON_APPEARANCES_ENABLED");
            const bool thrown = thrownCfg && *thrownCfg;
            std::vector<const uint8_t*> out;
            AscDbc::Table& t = Appearances();
            for (uint32_t id = t.MinId(); t.Count() && id <= t.MaxId(); ++id)
            {
                const uint8_t* a = t.Row(id);
                if (!a || !RealmAppropriate(a))
                    continue;
                if (!thrown)
                {
                    bool skip = false;
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const uint32_t c = AscDbc::Table::U32(a, 0x14 + k * 4);
                        if (c && CategoryIs(c, TYPE_ITEM))
                        {
                            const uint8_t* item = ClientDbcRow(0xAD3D4C, AscDbc::Table::U32(a, 0xC));
                            skip = item && AscDbc::Table::U32(item, 4) == 2 && AscDbc::Table::U32(item, 8) == 0x10;
                            break;
                        }
                    }
                    if (skip)
                        continue;
                }
                out.push_back(a);
            }
            return out;
        }

        // [2] 0x101B6100
        bool ArgMatch(uint32_t f, const uint8_t* const& a) override
        {
            static const uint32_t kWeaponSub[17] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 13, 15, 17, 18, 19, 20, 16};
            static const uint32_t kArmorSub[5] = {1, 2, 3, 4, 6};
            auto itemIs = [&](uint32_t cls, uint32_t sub) {   // FUN_101b4730
                if (!IsItemDisplay(a))
                    return false;
                const uint8_t* item = ClientDbcRow(0xAD3D4C, AscDbc::Table::U32(a, 4));
                return item && AscDbc::Table::U32(item, 4) == cls && AscDbc::Table::U32(item, 8) == sub;
            };
            auto invIs = [&](uint32_t inv) {                  // FUN_101b4770
                if (!IsItemDisplay(a))
                    return false;
                const uint8_t* item = ClientDbcRow(0xAD3D4C, AscDbc::Table::U32(a, 4));
                return item && AscDbc::Table::U32(item, 0x18) == inv;
            };
            const uint32_t id = AscDbc::Table::U32(a, 0);
            if (f == 1) return IsCollected(id);
            if (f == 2) return !IsCollected(id);
            if (f >= 3 && f <= 0x13) return itemIs(2, kWeaponSub[f - 3]);
            if (f >= 0x14 && f <= 0x18) return itemIs(4, kArmorSub[f - 0x14]);
            if (f >= 0x19 && f <= 0x1B) return invIs(0x15 + (f - 0x19));
            if (f == 0x1C || f == 0x1D)
            {
                const uint8_t* d = DetailsFor(id);
                return d && AscDbc::Table::U32(d, f == 0x1C ? 0xC : 0x10) != 0;
            }
            if (f >= 0x1E && f <= 0x25 && IsItemDisplay(a))
                if (const std::vector<const uint8_t*>* rows = ItemsForAppearance(id))
                    for (const uint8_t* r : *rows)
                        if (ItemQuality(AscDbc::Table::U32(r, 4)) == static_cast<int>(f - 0x1E))
                            return true;
            return false;
        }

        // [4] 0x101B6630
        uint32_t ArgGroup(uint32_t f) override
        {
            if (f == 1 || f == 2) return 1;
            if (f >= 3 && f <= 0x18) return 2;
            if (f == 0x19 || f == 0x1A) return 3;
            if (f == 0x1C || f == 0x1D) return 4;
            if (f >= 0x1E && f <= 0x25) return 5;
            return 0;
        }

        // [3] 0x101B5D10: in the selected category; then the item set's / an alternate item's name.
        bool TextMatch(const std::string& lower, const uint8_t* const& a) override
        {
            const int32_t category = static_cast<int32_t>(g.unk144);
            if (category < 1)
                return false;
            bool inCategory = false;
            for (uint32_t k = 0; k < 3; ++k)
                inCategory = inCategory || AscDbc::Table::U32(a, 0x14 + k * 4) == static_cast<uint32_t>(category);
            if (!inCategory)
                return false;
            if (lower.empty())
                return true;
            const uint32_t id = AscDbc::Table::U32(a, 0);
            if (HasCategoryOfType(a, TYPE_ITEM_SET))
            {
                for (uint32_t set : SetsWithAppearance(id))
                    if (const uint8_t* s = ClientDbcRow(kItemSetDbc, set))
                        if (AscFilter::Lower(ClientStr(s, 4)).find(lower) != std::string::npos)
                            return true;
                return false;
            }
            if (const std::vector<const uint8_t*>* rows = ItemsForAppearance(id))
                for (const uint8_t* r : *rows)
                {
                    const std::string name = ItemNameLower(AscDbc::Table::U32(r, 4));
                    if (!name.empty() && name.find(lower) != std::string::npos)
                        return true;
                }
            return false;
        }

        // [8] 0x101B66F0: [!item set:1][!collected:1][set incompletion:16][+0x2C:32].
        std::string FilterKey(const uint8_t* const& a) override
        {
            const uint32_t id = AscDbc::Table::U32(a, 0);
            const bool isSet = HasCategoryOfType(a, TYPE_ITEM_SET);
            uint16_t setKey = 0;
            if (isSet)
                if (const uint8_t* set = ClientDbcRow(kItemSetDbc, AscDbc::Table::U32(a, 0x20)))
                {
                    std::vector<uint32_t> parts;
                    ItemSetAppearanceList(set, parts);
                    uint32_t total = 0, have = 0;
                    for (uint32_t p : parts)
                    {
                        if (p)
                            ++total;
                        if (IsCollected(p))
                            ++have;
                    }
                    if (total)
                        setKey = static_cast<uint16_t>(~((have * 0xFFFFu) / total));
                }
            std::string k;
            AscFilter::AppendByte(k, isSet ? 0 : 1);
            AscFilter::AppendByte(k, IsCollected(id) ? 0 : 1);
            AscFilter::AppendByte(k, static_cast<uint8_t>(setKey >> 8));
            AscFilter::AppendByte(k, static_cast<uint8_t>(setKey));
            AscFilter::AppendU32(k, AscDbc::Table::U32(a, 0x2C));
            return k;
        }

        // [9] 0x101B6B10. Names become big integers byte by byte, so a longer name is larger: the key
        // leads with the length to keep that order.
        std::string SortKey(uint32_t s, const uint8_t* const& a) override
        {
            const uint32_t id = AscDbc::Table::U32(a, 0);
            std::string k;
            switch (s)
            {
            case 1: AscFilter::AppendU64(k, CollectedTime(id)); break;
            case 2: AscFilter::AppendU64(k, ~CollectedTime(id)); break;
            case 3:
            case 4:
            {
                const std::string name = AscFilter::Lower(AppearanceName(a));
                AscFilter::AppendU32(k, static_cast<uint32_t>(name.size()));
                for (char c : name)
                    AscFilter::AppendByte(k, s == 3 ? static_cast<uint8_t>(c) : static_cast<uint8_t>(0xFF - static_cast<uint8_t>(c)));
                break;
            }
            case 5: AscFilter::AppendU32(k, id); break;
            case 6: AscFilter::AppendU32(k, ~id); break;
            default: AscFilter::AppendU32(k, 0); break;
            }
            return k;
        }
    };

    struct OutfitEntry { std::string name; std::vector<uint32_t> list; };

    std::string NewOutfitText()   // the GlobalStrings NEW_OUTFIT text (DAT_10bdb270)
    {
        AscDbc::Table& gs = AscDbc::Get("DBFilesClient\\GlobalStrings.dbc");
        for (uint32_t id = gs.MinId(); gs.Count() && id <= gs.MaxId(); ++id)
            if (const uint8_t* r = gs.Row(id))
                if (strcmp(gs.Str(r, 8), "NEW_OUTFIT") == 0)
                    return gs.Str(r, 0xC);
        return std::string();
    }

    // AppearanceOutfitEntryContainer (vtable 0x10B2A3F8): the NEW_OUTFIT entry, then every outfit.
    class OutfitBrowser : public AscFilter::Container<OutfitEntry>
    {
    protected:
        std::vector<OutfitEntry> DoPopulate() override   // 0x100CBBC0
        {
            std::vector<OutfitEntry> out;
            out.push_back({NewOutfitText(), {}});
            for (const auto& o : g.outfits)
                out.push_back({o.first, o.second});
            return out;
        }
        bool TextMatch(const std::string& lower, const OutfitEntry& e) override   // 0x100CC250
        {
            if (e.name == NewOutfitText())
                return true;
            return AscFilter::Lower(e.name).find(lower) != std::string::npos;
        }
    };

    AppearanceBrowser g_browser;   // manager +0x0C
    OutfitBrowser g_outfitBrowser; // manager +0xC8

    std::vector<std::string> ReadFlagNames(lua_State* L, int idx)   // FUN_100bed80: {name = true}
    {
        std::map<std::string, bool> map;
        if (AscLua::lua_type(L, idx) == LUA_TTABLE)
        {
            AscLua::lua_pushnil(L);
            while (AscLua::lua_next(L, idx))
            {
                if (AscLua::lua_isstring(L, -2))
                    map[AscLua::lua_tolstring(L, -2, nullptr)] = AscLua::lua_toboolean(L, -1) != 0;
                AscLua::lua_settop(L, -2);
            }
        }
        std::vector<std::string> out;
        for (const auto& p : map)
            if (p.second)
                out.push_back(p.first);
        return out;
    }

    // FUN_100c67f0: (category, text, {FILTER_* = true}, {SORT_* = true}). A category above 0 filters the
    // appearances; -2 filters the outfits.
    int ApplyCategoryFilter(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, STRING, TABLE, TABLE}))
            return 0;
        const int32_t category = ToInt(CheckNumber(L, 1));
        const char* t = AscLua::tostring(L, 2);
        const std::string text = t ? t : "";
        const std::vector<std::string> filterNames = ReadFlagNames(L, 3);
        const std::vector<std::string> sortNames = ReadFlagNames(L, 4);
        if (category >= 1)
        {
            std::vector<uint32_t> filters, sorts;
            for (const std::string& n : filterNames)
                for (uint32_t i = 0; i < 38; ++i)
                    if (n == kFilters[i])
                        filters.push_back(i);
            for (const std::string& n : sortNames)
                for (uint32_t i = 0; i < 7; ++i)
                    if (n == kSorts[i])
                        sorts.push_back(i);
            g.unk144 = static_cast<uint32_t>(category);
            if (g_browserDirty)
            {
                g_browser.Reset();
                g_browserDirty = false;
            }
            g_browser.ApplyFilter(text, filters, sorts);
        }
        else if (category == -2)
        {
            g.unk144 = static_cast<uint32_t>(-2);
            g_outfitBrowser.Reset();
            g_outfitBrowser.ApplyFilter(text);
        }
        return 0;
    }

    uint32_t MaxPages()   // FUN_100c3ce0
    {
        const int32_t c = static_cast<int32_t>(g.unk144);
        if (c > 0)
            return static_cast<uint32_t>((g_browser.Size() + kPageSize - 1) / kPageSize);
        if (c == -2)
            return static_cast<uint32_t>((g_outfitBrowser.Size() + kPageSize - 1) / kPageSize);
        return 0;
    }

    int GetCategoryMaxPages(lua_State* L)   // handler_GetCategoryMaxPages
    {
        AscLua::lua_pushinteger(L, static_cast<int>(MaxPages()));
        return 1;
    }

    // FUN_100c98d0: (page, 1-based) -> 18 appearance ids, or 18 outfit names for -2; nil otherwise.
    int GetCategoryAppearances(lua_State* L)
    {
        uint32_t page;
        if (!ReadNumber(L, page) || page == 0)
            return 0;
        const uint32_t first = (page - 1) * kPageSize;
        const int32_t c = static_cast<int32_t>(g.unk144);
        if (c >= 1)
        {
            std::vector<uint32_t> ids;
            if (page - 1 < MaxPages())
                for (uint32_t i = first; i < first + kPageSize && i < g_browser.Size(); ++i)
                    if (const uint8_t* a = g_browser.At(i))
                        ids.push_back(AscDbc::Table::U32(a, 0));
            AscLua::lua_createtable(L, 0, static_cast<int>(ids.size()));
            AscLua::lua_checkstack(L, 2);
            for (uint32_t i = 0; i < ids.size(); ++i)
            {
                AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
                AscLua::lua_pushinteger(L, static_cast<int>(ids[i]));
                AscLua::lua_settable(L, -3);
            }
            return 1;
        }
        if (c == -2)
        {
            std::vector<std::string> names;
            if (page - 1 < MaxPages())
                for (uint32_t i = first; i < first + kPageSize && i < g_outfitBrowser.Size(); ++i)
                    names.push_back(g_outfitBrowser.At(i).name);
            AscLua::lua_createtable(L, 0, static_cast<int>(names.size()));
            AscLua::lua_checkstack(L, 2);
            for (uint32_t i = 0; i < names.size(); ++i)
            {
                AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
                PushStr(L, names[i].c_str());
                AscLua::lua_settable(L, -3);
            }
            return 1;
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_100c9c00: (category) -> collected appearances in it, all appearances in it.
    int GetCollectedCount(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const int32_t category = ToInt(CheckNumber(L, 1));
        auto inCategory = [category](const uint8_t* a) {
            for (uint32_t k = 0; k < 3; ++k)
                if (AscDbc::Table::I32(a, 0x14 + k * 4) == category)
                    return true;
            return false;
        };
        int have = 0, total = 0;
        for (const Collected& c : g.collected)
            if (const uint8_t* a = Appearances().Row(c.id))
                if (inCategory(a))
                    ++have;
        if (category > 0)
        {
            AscDbc::Table& t = Appearances();
            for (uint32_t id = t.MinId(); t.Count() && id <= t.MaxId(); ++id)
                if (const uint8_t* a = t.Row(id))
                    if (inCategory(a))
                        ++total;
        }
        AscLua::lua_pushinteger(L, have);
        AscLua::lua_pushinteger(L, total);
        return 2;
    }

    // FUN_100c85e0: the APPEARANCE_TYPE_* names that have a category the player's class may use, then
    // APPEARANCE_TYPE_OUTFIT.
    int GetAppearanceTypes(lua_State* L)
    {
        std::vector<std::string> names;
        AscDbc::Table& cats = Categories();
        for (int type = 0; type < 12; ++type)
            for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id)
            {
                const uint8_t* c = cats.Row(id);
                if (c && CategoryType(c) == type && CategoryAllowsClass(c))
                {
                    names.push_back(kAppearanceTypes[type]);
                    break;
                }
            }
        names.push_back("APPEARANCE_TYPE_OUTFIT");
        AscLua::lua_createtable(L, 0, static_cast<int>(names.size()));   // FUN_100beb90
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < names.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            PushStr(L, names[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // handler_GetCategoryInfo: name (+8), icon (+0x18), type (+4); three nils if unknown.
    int GetCategoryInfo(lua_State* L)
    {
        uint32_t category;
        if (!ReadNumber(L, category))
            return 0;
        const uint8_t* c = Categories().Row(category);
        if (!c)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 3;
        }
        PushStr(L, Categories().Str(c, 8));
        PushStr(L, Categories().Str(c, 0x18));
        PushStr(L, Categories().Str(c, 4));
        return 3;
    }

    // FUN_100c92f0: (type name) -> the ids of its categories the class may use, ascending; {-2} for
    // APPEARANCE_TYPE_OUTFIT.
    int GetCategoriesForType(lua_State* L)
    {
        std::string type;
        if (!ReadString(L, type))
            return 0;
        std::vector<int32_t> ids;
        if (type == "APPEARANCE_TYPE_OUTFIT")
            ids.push_back(-2);
        else
        {
            AscDbc::Table& cats = Categories();
            for (uint32_t id = cats.MinId(); cats.Count() && id <= cats.MaxId(); ++id)
                if (const uint8_t* c = cats.Row(id))
                    if (type == cats.Str(c, 4) && CategoryAllowsClass(c))
                        ids.push_back(AscDbc::Table::I32(c, 0));
        }
        std::sort(ids.begin(), ids.end());
        AscLua::lua_createtable(L, 0, static_cast<int>(ids.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < ids.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, ids[i]);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_100c7910: (item guid string) -> false for an unknown item, else CMSG 0x6EA {u64 guid}, true.
    int CollectItemAppearance(lua_State* L)
    {
        std::string guidText;
        if (!ReadString(L, guidText))
            return 0;
        const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x74D120)(guidText.c_str());
        const uint8_t* item = static_cast<const uint8_t*>(ObjectPtr(guid, 2));
        if (!item)
        {
            PushBool(L, false);
            return 1;
        }
        const uint64_t itemGuid = *reinterpret_cast<const uint64_t*>(*reinterpret_cast<uint8_t* const*>(item + 8));
        Packet(0x6EA).Data(&itemGuid, 8).Send();
        PushBool(L, true);
        return 1;
    }

    // ---- seasonal (SeasonalAppearances.dbc: +4 appearance, +8 season, +0xC chapter, +0x10 order,
    // +0x14..+0x24 realm gates) ------------------------------------------------------------------------
    AscDbc::Table& Seasonal() { return AscDbc::Get("DBFilesClient/SeasonalAppearances.dbc"); }
    bool SeasonalAvailable(const uint8_t* r)
    {
        const RealmInfo& ri = RealmInfoSvc::Get();
        return (AscDbc::Table::U32(r, 0x14) && ri.Live()) || (AscDbc::Table::U32(r, 0x18) && ri.Seasonal())
            || (AscDbc::Table::U32(r, 0x1C) && ri.League()) || (AscDbc::Table::U32(r, 0x20) && ri.PTR())
            || (AscDbc::Table::U32(r, 0x24) && ri.Dev());
    }
    void PushIntList(lua_State* L, const std::vector<uint32_t>& v)   // FUN_1009a460
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(v[i]));
            AscLua::lua_settable(L, -3);
        }
    }

    int GetAvailableSeasons(lua_State* L)   // FUN_100c8dc0: distinct +8, ascending
    {
        std::vector<uint32_t> out;
        AscDbc::Table& t = Seasonal();
        for (uint32_t id = t.MinId(); t.Count() && id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
                if (SeasonalAvailable(r) && std::find(out.begin(), out.end(), AscDbc::Table::U32(r, 8)) == out.end())
                    out.push_back(AscDbc::Table::U32(r, 8));
        std::sort(out.begin(), out.end());
        PushIntList(L, out);
        return 1;
    }

    int GetAvailableChapters(lua_State* L)   // FUN_100c8c10: (season, 0 = any) -> distinct +0xC, ascending
    {
        uint32_t season;
        if (!ReadNumber(L, season))
            return 0;
        std::vector<uint32_t> out;
        AscDbc::Table& t = Seasonal();
        for (uint32_t id = t.MinId(); t.Count() && id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
                if (SeasonalAvailable(r) && (season == 0 || AscDbc::Table::U32(r, 8) == season)
                    && std::find(out.begin(), out.end(), AscDbc::Table::U32(r, 0xC)) == out.end())
                    out.push_back(AscDbc::Table::U32(r, 0xC));
        std::sort(out.begin(), out.end());
        PushIntList(L, out);
        return 1;
    }

    // FUN_100ca810: (season, chapter; 0 = any) -> distinct appearances ordered by +0x10.
    int GetSeasonalItems(lua_State* L)
    {
        int32_t season, chapter;
        if (!ReadInt2(L, season, chapter))
            return 0;
        std::vector<const uint8_t*> rows;
        AscDbc::Table& t = Seasonal();
        for (uint32_t id = t.MinId(); t.Count() && id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
                if (SeasonalAvailable(r) && (season == 0 || AscDbc::Table::I32(r, 8) == season)
                    && (chapter == 0 || AscDbc::Table::I32(r, 0xC) == chapter))
                    rows.push_back(r);
        std::stable_sort(rows.begin(), rows.end(),
            [](const uint8_t* a, const uint8_t* b) { return AscDbc::Table::U32(a, 0x10) < AscDbc::Table::U32(b, 0x10); });
        std::vector<uint32_t> out;
        for (const uint8_t* r : rows)
            if (std::find(out.begin(), out.end(), AscDbc::Table::U32(r, 4)) == out.end())
                out.push_back(AscDbc::Table::U32(r, 4));
        PushIntList(L, out);
        return 1;
    }

    // ---- C_AppearanceOutfit ----------------------------------------------------------------------
    // FUN_100c84d0 -> FUN_100c3a20: the outfit names in the map's iteration order.
    int GetAppearanceOutfits(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(g.outfits.size()));   // FUN_100beb90
        AscLua::lua_checkstack(L, 2);
        uint32_t i = 0;
        for (const auto& o : g.outfits)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(++i));
            PushStr(L, o.first.c_str());
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_100ca4b0: (name) -> the outfit's appearances without index 0, as a list (empty if unknown).
    int GetOutfitInfo(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        std::vector<uint32_t> list;
        auto it = g.outfits.find(name);
        if (it != g.outfits.end())
            list = it->second;
        if (!list.empty())
            list.erase(list.begin());
        AscLua::lua_createtable(L, 0, static_cast<int>(list.size()));   // FUN_1009a460
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < list.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(list[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_100cb580: (name) -- a known outfit becomes the pending set.
    int SetPendingOutfit(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        auto it = g.outfits.find(name);
        if (it != g.outfits.end() && it->second != g.pending)
        {
            g.pending = it->second;
            PendingChanged();
        }
        return 0;
    }

    // FUN_100caef0: (name) -- false for the localized "NEW_OUTFIT" name (GlobalStrings +8 tag, +0xC
    // text), else remember the name and the pending set, CMSG 0x69E {str name, u32 n, u32 x n}, true.
    int SaveOutfit(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        AscDbc::Table& gs = AscDbc::Get("DBFilesClient\\GlobalStrings.dbc");
        for (uint32_t id = gs.MinId(); gs.Count() && id <= gs.MaxId(); ++id)
        {
            const uint8_t* r = gs.Row(id);
            if (r && strcmp(gs.Str(r, 8), "NEW_OUTFIT") == 0 && name == gs.Str(r, 0xC))
            {
                PushBool(L, false);
                return 1;
            }
        }
        g.pendingOutfitName = name;
        g.pendingOutfit = g.pending;
        Packet p(0x69E);
        p.Str(name.c_str()).U32(static_cast<uint32_t>(g.pendingOutfit.size()));
        for (uint32_t a : g.pendingOutfit)
            p.U32(a);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_100c7ab0: (name) -> CMSG 0x6A0 {str}, true; the answer is SMSG 0x6A1.
    int DeleteOutfit(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        g.pendingOutfitName = name;
        Packet(0x6A0).Str(name.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_100c9f70: the first outfit (iteration order) equal to the pending set, else nil.
    int GetCurrentOutfitName(lua_State* L)
    {
        for (const auto& o : g.outfits)
            if (o.second == g.pending)
            {
                if (o.first.empty())
                    break;
                PushStr(L, o.first.c_str());
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- C_ItemSet ------------------------------------------------------------------------------
    // FUN_100c8af0: (item set) -> its per-category appearances without index 0, or nil.
    int GetAppearances(lua_State* L)
    {
        uint32_t set;
        if (!ReadNumber(L, set))
            return 0;
        const uint8_t* row = ClientDbcRow(kItemSetDbc, set);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        std::vector<uint32_t> list;
        ItemSetAppearanceList(row, list);
        if (!list.empty())
            list.erase(list.begin());
        AscLua::lua_createtable(L, 0, static_cast<int>(list.size()));   // FUN_1009a460
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < list.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(list[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_100ca370: (item set) -> collected, total over its non-zero appearances; nil, nil if unknown.
    int GetItemSetNumCollected(lua_State* L)
    {
        uint32_t set;
        if (!ReadNumber(L, set))
            return 0;
        const uint8_t* row = ClientDbcRow(kItemSetDbc, set);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 2;
        }
        std::vector<uint32_t> list;
        ItemSetAppearanceList(row, list);
        int total = 0, collected = 0;
        for (uint32_t a : list)
            if (a)
            {
                ++total;
                if (IsCollected(a))
                    ++collected;
            }
        AscLua::lua_pushinteger(L, collected);
        AscLua::lua_pushinteger(L, total);
        return 2;
    }

    // ---- C_AppearanceCollection ------------------------------------------------------------------
    int IsAppearanceCollected(lua_State* L)   // handler_IsAppearanceCollected
    {
        uint32_t appearance;
        if (!ReadNumber(L, appearance))
            return 0;
        PushBool(L, IsCollected(appearance));
        return 1;
    }

    void OnGlueScreen()   // 0x100C2170
    {
        g.pending.clear();
        g_browserDirty = true;   // +0xC container reset (FUN_100c6670)
        g.unk144 = 0;
        g.current.clear();
        g.pendingOutfitName.clear();
        g.pendingOutfit.clear();
    }

    // 0x100C6410 (list 0x10BE2B6C, before 0x724820): with other players' appearances hidden (+0x1A5), a
    // spell flagged SpellCustomAttr +0x14 & 0x2000000 is not applied to anyone but the player (FUN_10324a10).
    bool __cdecl BeforeAuraApply(void* unit, uint32_t, uint32_t rec)
    {
        if (reinterpret_cast<char(__thiscall*)(void*)>(0x4CEE50)(unit) || g.seeOthers)
            return true;
        const uint8_t* attr = AscCA::SpellCustomAttrRow(*reinterpret_cast<const uint32_t*>(rec));
        return !(attr && (*reinterpret_cast<const uint32_t*>(attr + 0x14) & 0x2000000));
    }

    void Init()   // 0x100C5AE0
    {
        AscRuntime::OnBefore724820(&BeforeAuraApply, 0x100C5B88);
        sDC.AddPacketHandler(0x698, CNetClientCustomPacket((void*)&OnApplyResult, nullptr));
        sDC.AddPacketHandler(0x699, CNetClientCustomPacket((void*)&OnCollectedList, nullptr));
        sDC.AddPacketHandler(0x69A, CNetClientCustomPacket((void*)&OnCurrent, nullptr));
        sDC.AddPacketHandler(0x69B, CNetClientCustomPacket((void*)&OnCollected, nullptr));
        sDC.AddPacketHandler(0x69C, CNetClientCustomPacket((void*)&OnUncollected, nullptr));
        sDC.AddPacketHandler(0x69D, CNetClientCustomPacket((void*)&OnOutfits, nullptr));
        sDC.AddPacketHandler(0x69F, CNetClientCustomPacket((void*)&OnOutfitSaved, nullptr));
        sDC.AddPacketHandler(0x6A1, CNetClientCustomPacket((void*)&OnOutfitDeleted, nullptr));
        sDC.AddPacketHandler(0x6A2, CNetClientCustomPacket((void*)&OnVisibility, nullptr));
        sDC.AddPacketHandler(0x6EB, CNetClientCustomPacket((void*)&OnCollectItemResult, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Appearance", "GetAppearanceForCategory", GetAppearanceForCategory},
        {"C_Appearance", "GetItemAppearanceID", GetItemAppearanceID},
        {"C_Appearance", "GetItemSetAppearanceID", GetItemSetAppearanceID},
        {"C_Appearance", "HasPendingAppearances", HasPendingAppearances},
        {"C_Appearance", "GetPendingAppearance", GetPendingAppearance},
        {"C_Appearance", "ClearPendingAppearance", ClearPendingAppearance},
        {"C_Appearance", "ClearPendingAppearances", ClearPendingAppearances},
        {"C_Appearance", "CanSeeAppearances", CanSeeAppearances},
        {"C_Appearance", "SetCanSeeAppearances", SetCanSeeAppearances},
        {"C_Appearance", "IsTransmogable", IsTransmogable},
        {"C_Appearance", "GetActiveDiscount", GetActiveDiscount},
        {"C_Appearance", "IsEtherealBazaarAppearance", IsEtherealBazaarAppearance},
        {"C_Appearance", "GetAppearanceDetails", GetAppearanceDetails},
        {"C_Appearance", "GetAppearanceWebURL", GetAppearanceWebURL},
        {"C_Appearance", "GetEtherealBazaarWebURL", GetEtherealBazaarWebURL},
        {"C_Appearance", "SetPendingAppearance", SetPendingAppearance},
        {"C_Appearance", "CanSetAppearance", CanSetAppearance},
        {"C_Appearance", "ApplyPendingAppearances", ApplyPendingAppearances},
        {"C_Appearance", "CanApplyPendingAppearances", CanApplyPendingAppearances},
        {"C_Appearance", "ClearInvalidPendingAppearances", ClearInvalidPendingAppearances},
        {"C_Appearance", "GetAppearanceDisplayInfo", GetAppearanceDisplayInfo},
        {"C_Appearance", "GetAppearanceItemSet", GetAppearanceItemSet},
        {"C_Appearance", "GetAlternativeIDs", GetAlternativeIDs},
        {"C_Appearance", "GetCreatureDisplayItems", GetCreatureDisplayItems},
        {"C_AppearanceCollection", "IsAppearanceCollected", IsAppearanceCollected},
        {"C_AppearanceCollection", "GetAppearanceTypes", GetAppearanceTypes},
        {"C_AppearanceCollection", "GetCategoryInfo", GetCategoryInfo},
        {"C_AppearanceCollection", "GetCategoriesForType", GetCategoriesForType},
        {"C_AppearanceCollection", "ApplyCategoryFilter", ApplyCategoryFilter},
        {"C_AppearanceCollection", "GetCategoryAppearances", GetCategoryAppearances},
        {"C_AppearanceCollection", "GetCategoryMaxPages", GetCategoryMaxPages},
        {"C_AppearanceCollection", "GetCollectedCount", GetCollectedCount},
        {"C_AppearanceCollection", "CollectItemAppearance", CollectItemAppearance},
        {"C_AppearanceCollection", "GetSeasonalItems", GetSeasonalItems},
        {"C_AppearanceCollection", "GetAvailableSeasons", GetAvailableSeasons},
        {"C_AppearanceCollection", "GetAvailableChapters", GetAvailableChapters},
        {"C_AppearanceOutfit", "GetAppearanceOutfits", GetAppearanceOutfits},
        {"C_AppearanceOutfit", "GetOutfitInfo", GetOutfitInfo},
        {"C_AppearanceOutfit", "SetPendingOutfit", SetPendingOutfit},
        {"C_AppearanceOutfit", "SaveOutfit", SaveOutfit},
        {"C_AppearanceOutfit", "DeleteOutfit", DeleteOutfit},
        {"C_AppearanceOutfit", "GetCurrentOutfitName", GetCurrentOutfitName},
        {"C_ItemSet", "GetAppearances", GetAppearances},
        {"C_ItemSet", "GetItemSetNumCollected", GetItemSetNumCollected},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

// FUN_10322c70's branch for the active player: the first AppearanceCategories.dbc row (by id) of
// APPEARANCE_TYPE 4 whose +0xC or +0x10 is `spell` gives the category (0 when none); the appearance applied
// there (FUN_100c39f0 over +0x148) names an Appearances.dbc row whose +0x20 is the visual. False (the caller
// falls back to its map) when there is no applied appearance or no such row.
bool AscAppearance_PlayerSpellVisual(uint32_t spell, uint32_t& visual)
{
    uint32_t category = 0;
    AscDbc::Table& cats = Categories();
    for (uint32_t id = cats.MinId(); cats.Loaded() && id <= cats.MaxId(); ++id)
    {
        const uint8_t* row = cats.Row(id);
        if (row && CategoryType(row) == 4 && (AscDbc::Table::U32(row, 0xC) == spell || AscDbc::Table::U32(row, 0x10) == spell))
        {
            category = AscDbc::Table::U32(row, 0);
            break;
        }
    }
    const uint32_t appearance = At(g.current, category);
    if (appearance == 0)
        return false;
    const uint8_t* row = Appearances().Row(appearance);
    if (!row)
        return false;
    visual = AscDbc::Table::U32(row, 0x20);
    return true;
}

// The refreshes the SMSG_PATCH_* handlers run: 0x693 (FUN_1020fc80 then FUN_101b5660) and 0x6EC
// (FUN_102101d0 then FUN_101b5660).
void AscAppearance_ItemAppearancesPatched()
{
    BuildItemAppearanceIndex();
    g_setsForItemAppearanceBuilt = false;
}
void AscAppearance_ItemSetAppearancesPatched()
{
    BuildItemSetAppearanceIndex();
    g_setsForItemAppearanceBuilt = false;
}

// SMSG 0x692 (FUN_101dced0): after inserting a new Appearances.dbc row, the manager's FUN_100c6750.
void AscAppearance_RebuildOutfits() { RebuildOutfits(); }
