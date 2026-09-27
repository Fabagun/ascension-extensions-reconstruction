// C_VanityCollection, part 1: the known-items set, purchases and the buff test.
//
// Manager FUN_1032e9b0 -> 0x10D3C030 (ctor FUN_1032b180): +0x00 the known vanity item ids, +0x10 an
// unordered_set of the same ids (FUN_1032ea50 tests both). Module init 0x1032E6C0: SMSG 0x6F7 list /
// 0x6F8 add / 0x6F9 remove (each re-runs the item index FUN_10332980 once built), 0x9D0 clears the
// character-customization unlocks (+0x2C / +0x3C). CMSG 0x523 CUSTOM_ASCENSION_POINT_SPEND_REQUEST
// {u8 kind, u32 item} buys (kind 2 = web shop / delivery).
//
// The item browser (FUN_1032c140 builder, FUN_1032fcd0 query) reads the MemoryBridge VanityCollection
// records (AscBridgeStore::VanityRecord, 45 dwords).
// Record: 0 ID, 1 itemid, 2 category, 3 artwork (str), 4 dp, 5 sp, 6 bt, 7..11 Live / Seasonal / League
// / PTR / Dev, 0xC flags, 0xD..0xE class mask (u64), 0xF race mask, 0x10 creature preview, 0x11 spell
// preview, 0x12..0x29 contents, 0x2A description (str), 0x2B additional text (str), 0x2C learned spell.
// Item names, descriptions, quality and flags come from the ItemAddon records (FUN_1020ef10).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AscScript;

namespace
{
    std::vector<uint32_t> g_known;              // +0x00
    std::unordered_set<uint32_t> g_knownSet;    // +0x10

    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }

    void __cdecl OnList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6F7: u32 n, n x u32
    {
        g_known.clear();
        g_knownSet.clear();
        const uint32_t n = U32(p);
        g_known.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t id = U32(p);
            g_known.push_back(id);
            g_knownSet.insert(id);
        }
    }

    void __cdecl OnAdd(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6F8: u32
    {
        const uint32_t id = U32(p);
        g_known.push_back(id);
        g_knownSet.insert(id);
    }

    void __cdecl OnRemove(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6F9: u32
    {
        const uint32_t id = U32(p);
        g_known.erase(std::remove(g_known.begin(), g_known.end(), id), g_known.end());
        g_knownSet.erase(id);
    }

    void __cdecl OnCustomizationUnlocks(void*, uint32_t, uint32_t, CDataStore*) {}   // 0x9D0: clears unused lists

    // IsCollectionItemOwned / IsOwned (handler_IsCollectionItemOwned): exactly one argument.
    int IsCollectionItemOwned(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        PushBool(L, g_knownSet.count(id) != 0 || std::find(g_known.begin(), g_known.end(), id) != g_known.end());
        return 1;
    }

    int PurchaseCollectionItem(lua_State* L)   // FUN_103311d0: (item, kind)
    {
        if (AscLua::lua_gettop(L) != 2)
            return 0;
        const int32_t item = ToInt(CheckNumber(L, 1));
        const uint8_t kind = static_cast<uint8_t>(static_cast<int>(CheckNumber(L, 2)));
        Packet(0x523).U8(kind).U32(static_cast<uint32_t>(item)).Send();
        return 0;
    }

    int PurchaseWebShopItem(lua_State* L)   // FUN_103312f0: (item) -> true
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        Packet(0x523).U8(2).U32(static_cast<uint32_t>(ToInt(CheckNumber(L, 1)))).Send();
        PushBool(L, true);
        return 1;
    }

    int RequestDelivery(lua_State* L)   // FUN_103317c0: (item)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        Packet(0x523).U8(2).U32(static_cast<uint32_t>(ToInt(CheckNumber(L, 1)))).Send();
        return 0;
    }

    // FUN_10330f60: SpellCustomAttr +0x18 bit 5, and the spell's mechanic (+0xC) is not 21 (mount).
    int IsConsolidatedVanityBuff(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        bool yes = false;
        if (AscCA::SpellCustomAttr18(spell, 0x20))
        {
            uint8_t rec[0x2A8];
            yes = FetchSpell(spell, rec) && *reinterpret_cast<const uint32_t*>(rec + 0xC) != 0x15;
        }
        PushBool(L, yes);
        return 1;
    }

    bool AscVanityOwned(uint32_t id)   // FUN_1032ea50
    {
        return g_knownSet.count(id) != 0 || std::find(g_known.begin(), g_known.end(), id) != g_known.end();
    }

    // ---- the item browser --------------------------------------------------------------------------------
    using VanityRecord = AscBridgeStore::VanityRecord;
    uint32_t Rec(const VanityRecord* row, uint32_t i) { return row->f[i]; }
    std::string RecStr(const VanityRecord* row, uint32_t i)
    {
        const char* s = row->Str(i);
        return s ? s : "";
    }

    // The 0xE8-byte VanityCollectionItem (FUN_1032d3e0, names / quality / flags by FUN_1032bee0).
    struct Item
    {
        uint32_t row = 0;                    // +0x00 row index
        uint32_t id = 0, itemId = 0;         // +0x04 / +0x08
        uint32_t category = 0, mask = 0;     // +0x0C / +0x10
        uint32_t dp = 0, sp = 0, bt = 0;     // +0x14 / +0x18 / +0x1C
        uint32_t flags = 0;                  // +0x20
        uint64_t classMask = 0;              // +0x28
        uint32_t raceMask = 0;               // +0x30
        uint32_t contents[24] = {};          // +0x34
        uint32_t learnedSpell = 0;           // +0x94
        std::string search;                  // +0x98 (lower-case)
        std::string name, itemDescription;   // +0xB0 / +0xC8
        uint32_t quality = 0, itemFlags = 0; // +0xE0 / +0xE4
    };
    std::vector<Item> g_items;       // +0x5C
    std::vector<Item*> g_itemList;   // +0x104
    bool g_built = false;            // +0x58

    std::string Lower(std::string s)   // FUN_10331970
    {
        for (char& c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // FUN_1021a1a0: not hidden (flags bit 3), and offered on one of this realm's kinds.
    bool Offered(const VanityRecord* row)
    {
        if (Rec(row, 0xC) & 8)
            return false;
        const RealmInfo& r = RealmInfoSvc::Get();
        return (Rec(row, 7) && r.Live()) || (Rec(row, 8) && r.Seasonal()) || (Rec(row, 9) && r.League())
            || (Rec(row, 10) && r.PTR()) || (Rec(row, 11) && r.Dev());
    }
    uint32_t CategoryMask(const VanityRecord* row)   // rec[2] | 0x20000000 (sp cost) | 0x40000000 (flags 0x20)
    {
        uint32_t m = Rec(row, 2);
        if (Rec(row, 5))
            m |= 0x20000000;
        if (Rec(row, 0xC) & 0x20)
            m |= 0x40000000;
        return m;
    }

    // FUN_1032c140: every record index 0..count-1, fetched 1,000 at a time (FUN_1032dc10). A failed batch
    // abandons the build with an empty list; records not found or not offered are skipped.
    void Build()   // FUN_1032c140
    {
        g_items.clear();
        g_itemList.clear();
        const uint32_t n = AscBridgeStore::VanityCount();
        std::vector<uint32_t> indexes(n);
        for (uint32_t i = 0; i < n; ++i)
            indexes[i] = i;
        std::vector<AscBridgeStore::VanityEntry> batch;
        for (uint32_t done = 0; done < n; done += 1000)
        {
            const uint32_t take = n - done < 1000 ? n - done : 1000;
            if (!AscBridgeStore::VanityRecords(indexes.data() + done, take, batch))
            {
                g_items.clear();
                g_built = true;
                return;
            }
            for (const AscBridgeStore::VanityEntry& v : batch)
            {
                if (!v.found || !Offered(&v.record))
                    continue;
                const VanityRecord* row = &v.record;
                const uint32_t i = v.index;
                Item e;
                e.row = i;
                e.id = Rec(row, 0);
                e.itemId = Rec(row, 1);
                e.category = Rec(row, 2);
                e.mask = CategoryMask(row);
                e.dp = Rec(row, 4);
                e.sp = Rec(row, 5);
                e.bt = Rec(row, 6);
                e.flags = Rec(row, 0xC);
                e.classMask = static_cast<uint64_t>(Rec(row, 0xD)) | (static_cast<uint64_t>(Rec(row, 0xE)) << 32);
                e.raceMask = Rec(row, 0xF);
                for (uint32_t c = 0; c < 24; ++c)
                    e.contents[c] = Rec(row, 0x12 + c);
                e.learnedSpell = Rec(row, 0x2C);
                e.search = RecStr(row, 0x2A) + " " + RecStr(row, 0x2B) + " " + std::to_string(e.itemId);   // FUN_1032d3e0
                g_items.push_back(std::move(e));
            }
        }
        // FUN_1020ef10 over the unique item ids (its result is not checked), then FUN_1032bee0 per item:
        // the item text when found, and the search text rebuilt either way.
        std::vector<uint32_t> ids;
        std::unordered_set<uint32_t> seen;
        for (const Item& e : g_items)
            if (seen.insert(e.itemId).second)
                ids.push_back(e.itemId);
        std::unordered_map<uint32_t, AscItemAddon::Info> info;
        AscItemAddon::FetchInfo(ids.data(), static_cast<uint32_t>(ids.size()), info);
        for (Item& e : g_items)
        {
            auto it = info.find(e.itemId);
            if (it != info.end())
            {
                e.name = it->second.name;
                e.itemDescription = it->second.description;
                e.quality = it->second.quality;
                e.itemFlags = it->second.flags;
            }
            e.search = Lower(e.name + " " + e.itemDescription + " " + e.search);
        }
        for (Item& e : g_items)
            g_itemList.push_back(&e);
        g_built = true;
    }
    // SMSG 0x573 (FUN_101e55d0): the record as FUN_101d3a00 reads it -- the record's fields in order with
    // its three strings as C strings in place -- then three more C strings (read and dropped). FUN_1032ec60
    // patches the store (FUN_1032eae0); only on success does it rebuild the browser (FUN_1032c140) and
    // signal HOT_PATCH_VANITY_COLLECTION("%u", item). With the shipped MMgr64.exe the patch is refused.
    std::string CStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }
    void __cdecl OnPatchVanity(void*, uint32_t, uint32_t, CDataStore* p)
    {
        AscBridgeStore::VanityRecord r = {};
        std::string strings[3];
        for (uint32_t i = 0; i < 3; ++i)
            r.f[i] = U32(p);
        strings[0] = CStr(p);
        for (uint32_t i = 4; i <= 0xC; ++i)
            r.f[i] = U32(p);
        r.f[0xD] = U32(p);
        r.f[0xE] = U32(p);
        for (uint32_t i = 0xF; i <= 0x11; ++i)
            r.f[i] = U32(p);
        for (uint32_t i = 0x12; i <= 0x29; ++i)
            r.f[i] = U32(p);
        strings[1] = CStr(p);
        strings[2] = CStr(p);
        r.f[0x2C] = U32(p);
        for (uint32_t i = 0; i < 3; ++i)
            CStr(p);
        r.f[3] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(strings[0].c_str()));
        r.f[0x2A] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(strings[1].c_str()));
        r.f[0x2B] = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(strings[2].c_str()));
        if (!AscBridgeStore::PatchVanity(r))
            return;
        Build();
        AscRuntime::Signal("HOT_PATCH_VANITY_COLLECTION", "%u", r.f[1]);
    }

    std::vector<Item*>& Items()
    {
        if (!g_built)
            Build();
        return g_itemList;
    }
    Item* ByItemId(uint32_t item)   // FUN_1032e220
    {
        for (Item* e : Items())
            if (e->itemId == item)
                return e;
        return nullptr;
    }

    // FUN_1032f6e0: the Lua table, re-read from the record (empty when it is gone).
    void PushItem(lua_State* L, const Item* e)
    {
        const VanityRecord* row = AscBridgeStore::VanityAt(e->row, e->itemId);   // FUN_1032e090
        if (!row)
        {
            AscLua::lua_createtable(L, 0, 0);
            return;
        }
        auto num = [L](const char* k, uint32_t v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushinteger(L, static_cast<int32_t>(v)); AscLua::lua_settable(L, -3); };
        auto str = [L](const char* k, const std::string& v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushstring(L, v.c_str()); AscLua::lua_settable(L, -3); };
        const uint32_t item = Rec(row, 1);
        const AscItemAddon::Info info = AscItemAddon::GetInfo(item);   // FUN_10330740 -> FUN_1020e9c0
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        num("id", Rec(row, 0));
        num("itemid", item);
        num("itemID", item);
        str("name", info.name);
        num("quality", info.quality);
        str("description", RecStr(row, 0x2A));
        const uint32_t mask = CategoryMask(row);
        num("group", mask);
        num("categoryMask", mask);
        str("additionalText", RecStr(row, 0x2B));
        const char* icon = "";   // Item.dbc display id -> ItemDisplayInfo inventory icon
        uint8_t display[100] = {};
        if (const uint8_t* it = ClientDbcRow(0xAD3D4C, item))
            if (reinterpret_cast<int(__thiscall*)(void*, uint32_t, void*)>(0x4CFD90)(reinterpret_cast<void*>(0xAD3DDC), AscDbc::Table::U32(it, 0x14), display))
                icon = *reinterpret_cast<const char* const*>(display + 0x14);
        str("icon", icon ? icon : "");
        str("artwork", RecStr(row, 3));
        num("dpCost", Rec(row, 4));
        num("spCost", Rec(row, 5));
        num("btCost", Rec(row, 6));
        num("creaturePreview", Rec(row, 0x10));
        num("spellPreview", Rec(row, 0x11));
        AscLua::lua_pushstring(L, "contentsPreview");
        AscLua::lua_createtable(L, 0, 0x18);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < 24; ++i)
            if (Rec(row, 0x12 + i) != 0)
            {
                AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
                AscLua::lua_pushinteger(L, static_cast<int32_t>(Rec(row, 0x12 + i)));
                AscLua::lua_settable(L, -3);
            }
        AscLua::lua_settable(L, -3);
        num("flags", Rec(row, 0xC));
        num("learnedSpell", Rec(row, 0x2C));
        num("allowableClass", Rec(row, 0xD));
        num("allowableRace", Rec(row, 0xF));
        AscLua::lua_pushstring(L, "owned");
        PushBool(L, AscVanityOwned(item));
        AscLua::lua_settable(L, -3);
    }

    // ---- BuildVanityCollectionIndex filters (lambda_1..8) and sorts (lambda_9..13) -----------------------
    const uint32_t kCategoryMasks[] = {   // DAT_10b5cc20
        0x2000000, 0x4000000, 0x8000000, 0x10000000, 0x20000000, 0x40000000, 0x80000000,
        0x100001, 0x100002, 0x100004, 0x100008, 0x100010, 0x100020, 0x100040, 0x100080, 0x100100,
        0x100200, 0x100400, 0x100800, 0x101000, 0x102000, 0x104000, 0x108000, 0x110000, 0x120000,
        0x200001, 0x200002, 0x200004, 0x200008, 0x200010, 0x200020, 0x200040, 0x200080, 0x200100,
        0x200200, 0x200400, 0x200800, 0x201000, 0x202000, 0x400001, 0x400002, 0x400004, 0x800001,
        0x800002, 0x800004, 0x800008, 0x1000001, 0x1000002, 0x1000004, 0x1000008, 0x1000010,
    };
    bool CategoryFilter(const Item& e, uint32_t arg)   // 0x10331CC0
    {
        if (arg == 0 || arg == 0xFFFFFFFF)
            return true;
        for (uint32_t m : kCategoryMasks)
            if ((e.mask & m) == m && (m & arg) == m)
                return true;
        return false;
    }
    bool CurrentCharacterFilter(const Item& e)   // 0x10331D40 (always given true)
    {
        if (e.classMask != 0)
        {
            const uint32_t c = (reinterpret_cast<uint32_t(__cdecl*)()>(0x6B1080)() & 0xFF) - 1;
            if (c > 0x3F || !(e.classMask & (1ull << c)))
                return false;
        }
        if (e.raceMask != 0)
        {
            const uint32_t r = (reinterpret_cast<uint32_t(__cdecl*)()>(0x6B1070)() & 0xFF) - 1;
            if (r > 0x1F || !(e.raceMask & (1u << r)))
                return false;
        }
        return true;
    }
    bool OwnershipFilter(const Item& e, uint32_t arg)   // 0x10331F30: bit 0 owned, bit 1 not owned
    {
        if ((arg & 1) == ((arg >> 1) & 1))
            return true;
        return AscVanityOwned(e.itemId) == ((arg & 1) != 0);
    }

    enum SortKind { SORT_NAME, SORT_QUALITY, SORT_COST, SORT_OWNED_FIRST, SORT_UNOWNED_FIRST };
    uint32_t CostKey(const Item& e)   // 0x10332090: the lowest non-zero price
    {
        uint32_t k = 0xFFFFFFFF;
        for (uint32_t v : {e.dp, e.sp, e.bt})
            if (v != 0 && v < k)
                k = v;
        return k == 0xFFFFFFFF ? 0 : k;
    }

    std::vector<Item*> g_results;   // +0xF8 (0x20-byte entries)
    std::vector<Item*> g_page;      // +0x110

    // FUN_1032fcd0
    uint32_t Query(const std::string& text, uint32_t category, uint32_t filters, uint32_t sort, uint32_t offset, uint32_t count)
    {
        const std::string needle = Lower(text);
        g_results.clear();
        for (Item* e : Items())
        {
            if (!needle.empty() && e->search.find(needle) == std::string::npos)   // vanity_raw_search_filter
                continue;
            if (!CurrentCharacterFilter(*e))
                continue;
            if (!CategoryFilter(*e, category))
                continue;
            if ((filters & 3) && !OwnershipFilter(*e, filters))
                continue;
            if ((filters & 4) && e->dp == 0 && e->sp == 0 && e->bt == 0)   // purchasable
                continue;
            if ((filters & 8) && (e->flags & 4))   // deliverable
                continue;
            if ((filters & 0x10) && e->quality != 7)   // heirloom
                continue;
            if ((filters & 0x20) && !(e->itemFlags & 0x200) && Lower(e->itemDescription).find("manastorm") == std::string::npos)
                continue;
            g_results.push_back(e);
        }
        bool desc = false;
        int kind = -1;
        switch (sort)
        {
        case 1: kind = SORT_NAME; break;
        case 2: kind = SORT_NAME; desc = true; break;
        case 3: kind = SORT_QUALITY; desc = true; break;
        case 4: kind = SORT_QUALITY; break;
        case 5: kind = SORT_COST; break;
        case 6: kind = SORT_COST; desc = true; break;
        case 7: kind = SORT_OWNED_FIRST; break;
        case 8: kind = SORT_UNOWNED_FIRST; break;
        }
        if (kind >= 0)
            std::stable_sort(g_results.begin(), g_results.end(), [kind, desc](const Item* a, const Item* b) {
                if (kind == SORT_NAME)
                {
                    const std::string x = Lower(a->name), y = Lower(b->name);
                    return desc ? y < x : x < y;
                }
                uint32_t x = 0, y = 0;
                if (kind == SORT_QUALITY)      { x = a->quality; y = b->quality; }
                else if (kind == SORT_COST)    { x = CostKey(*a); y = CostKey(*b); }
                else if (kind == SORT_OWNED_FIRST)   { x = !AscVanityOwned(a->itemId); y = !AscVanityOwned(b->itemId); }
                else                           { x = AscVanityOwned(a->itemId); y = AscVanityOwned(b->itemId); }
                return desc ? y < x : x < y;
            });
        g_page.clear();
        const uint32_t total = static_cast<uint32_t>(g_results.size());
        if (offset < total && count != 0)
        {
            const uint32_t n = std::min(count, total - offset);
            g_page.assign(g_results.begin() + offset, g_results.begin() + offset + n);
        }
        return total;
    }

    // FUN_10331420: ([text [, category [, filters [, sort [, offset [, count = 9]]]]]]) ->
    // {total, items}; at most six arguments, nil ones default.
    int QueryItems(lua_State* L)
    {
        const int top = AscLua::lua_gettop(L);
        if (top >= 7)
            return 0;
        auto given = [L, top](int i) { return top >= i && AscLua::lua_type(L, i) > 0; };
        std::string text;
        if (given(1))
            text = CheckString(L, 1);
        const uint32_t category = given(2) ? static_cast<uint32_t>(ToInt(CheckNumber(L, 2))) : 0;
        const uint32_t filters = given(3) ? static_cast<uint32_t>(ToInt(CheckNumber(L, 3))) : 0;
        uint32_t sort = given(4) ? static_cast<uint32_t>(ToInt(CheckNumber(L, 4))) : 0;
        const uint32_t offset = given(5) ? static_cast<uint32_t>(ToInt(CheckNumber(L, 5))) : 0;
        const uint32_t count = given(6) ? static_cast<uint32_t>(ToInt(CheckNumber(L, 6))) : 9;
        if (sort > 8)
            sort = 0;
        const uint32_t total = Query(text, category, filters, sort, offset, count);
        AscLua::lua_createtable(L, 0, 2);
        AscLua::lua_checkstack(L, 3);
        AscLua::lua_pushstring(L, "total");
        AscLua::lua_pushinteger(L, static_cast<int32_t>(total));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "items");
        AscLua::lua_createtable(L, 0, static_cast<int>(g_page.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < g_page.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
            PushItem(L, g_page[i]);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        return 1;
    }

    int PushList(lua_State* L, const std::vector<Item*>& list)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(list.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < list.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int32_t>(i + 1));
            PushItem(L, list[i]);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GetAllItems(lua_State* L)   // handler_GetAllItems: no arguments
    {
        if (AscLua::lua_gettop(L) != 0)
            return 0;
        return PushList(L, Items());
    }

    int GetItem(lua_State* L)   // handler_GetItem: (itemid)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const Item* e = ByItemId(static_cast<uint32_t>(ToInt(CheckNumber(L, 1))));
        if (!e)
            return 0;
        PushItem(L, e);
        return 1;
    }

    int GetItemByLearnedSpell(lua_State* L)   // handler_GetItemByLearnedSpell: (spell)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        for (const Item* e : Items())
            if (e->learnedSpell == spell)
            {
                PushItem(L, e);
                return 1;
            }
        return 0;
    }

    int GetOwnerByContentItem(lua_State* L)   // handler_GetOwnerByContentItem: (item in contents)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const uint32_t item = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        for (const Item* e : Items())
            for (uint32_t c : e->contents)
                if (c == item)
                {
                    PushItem(L, e);
                    return 1;
                }
        return 0;
    }

    int GetRandomItem(lua_State* L)   // handler_GetRandomItem
    {
        if (AscLua::lua_gettop(L) != 0)
            return 0;
        std::vector<Item*>& items = Items();
        if (items.empty())
            return 0;
        PushItem(L, items[static_cast<uint32_t>(rand()) % items.size()]);
        return 1;
    }

    int GetSeasonalShowcaseItems(lua_State* L)   // FUN_10330d70 / FUN_1032de20: flags bit 4
    {
        if (AscLua::lua_gettop(L) != 0)
            return 0;
        std::vector<Item*> list;
        for (Item* e : Items())
            if (e->flags & 0x10)
                list.push_back(e);
        return PushList(L, list);
    }

    int GetDPPrice(lua_State* L)   // handler_GetDPPrice: (itemid) -> +0x14, 0 when unknown
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const Item* e = ByItemId(static_cast<uint32_t>(ToInt(CheckNumber(L, 1))));
        AscLua::lua_pushinteger(L, e ? static_cast<int32_t>(e->dp) : 0);
        return 1;
    }

    int GetVPPrice(lua_State* L)   // handler_GetVPPrice: (itemid) -> +0x18, 0 when unknown
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const Item* e = ByItemId(static_cast<uint32_t>(ToInt(CheckNumber(L, 1))));
        AscLua::lua_pushinteger(L, e ? static_cast<int32_t>(e->sp) : 0);
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x6F7, CNetClientCustomPacket((void*)&OnList, nullptr));
        sDC.AddPacketHandler(0x6F8, CNetClientCustomPacket((void*)&OnAdd, nullptr));
        sDC.AddPacketHandler(0x6F9, CNetClientCustomPacket((void*)&OnRemove, nullptr));
        sDC.AddPacketHandler(0x9D0, CNetClientCustomPacket((void*)&OnCustomizationUnlocks, nullptr));
        sDC.AddPacketHandler(0x573, CNetClientCustomPacket((void*)&OnPatchVanity, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_VanityCollection", "IsCollectionItemOwned", IsCollectionItemOwned},
        {nullptr, "IsCollectionItemOwned", IsCollectionItemOwned},   // FUN_10332760 also sets it as a global
        {"C_VanityCollection", "IsOwned", IsCollectionItemOwned},
        {"C_VanityCollection", "PurchaseCollectionItem", PurchaseCollectionItem},
        {"C_VanityCollection", "PurchaseWebShopItem", PurchaseWebShopItem},
        {"C_VanityCollection", "RequestDelivery", RequestDelivery},
        {"C_VanityCollection", "IsConsolidatedVanityBuff", IsConsolidatedVanityBuff},
        {"C_VanityCollection", "QueryItems", QueryItems},
        {"C_VanityCollection", "GetAllItems", GetAllItems},
        {"C_VanityCollection", "GetItem", GetItem},
        {"C_VanityCollection", "GetItemByLearnedSpell", GetItemByLearnedSpell},
        {"C_VanityCollection", "GetOwnerByContentItem", GetOwnerByContentItem},
        {"C_VanityCollection", "GetRandomItem", GetRandomItem},
        {"C_VanityCollection", "GetSeasonalShowcaseItems", GetSeasonalShowcaseItems},
        {"C_VanityCollection", "GetDPPrice", GetDPPrice},
        {"C_VanityCollection", "GetVPPrice", GetVPPrice},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscVanity
{
    bool Owned(uint32_t id)   // FUN_1032ea50
    {
        return g_knownSet.count(id) != 0 || std::find(g_known.begin(), g_known.end(), id) != g_known.end();
    }
}
