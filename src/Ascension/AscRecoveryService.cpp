// C_RecoveryService, transcribed from the original's RecoveryServiceMgr (FUN_103056d0, static
// 0x10BE3B38, init FUN_102fea30) -- E:\oniwow-artifacts\ghidra-ext\out\all\102f*..1030*.c.
//
// Six FilterableContainerBase<T, NullFilterType, NullSortType> instances, one per category:
//   1 DISENCHANTED_ITEM   query 0x5D2 -> 0x5D3, recover 0x5D4 -> 0x5D5
//   2 DELETED_CHARACTER   query 0x5DA -> 0x5DB, recover 0x5DC -> 0x5DD
//   3 VENDORED_ITEM       query 0x5DE -> 0x5DF, recover 0x5E0 -> 0x5E1
//   4 DELETED_ITEM        query 0x5E2 -> 0x5E3, recover 0x5E4 -> 0x5E5
//   5 MYTHIC_RECYCLING    query 0x5E6 -> 0x5E7, recover 0x5E8 -> 0x5E9
//   6 WORLDFORGED_ITEM    query 0x682 -> 0x683, recover 0x684 -> 0x685
// A query response clears the category (FUN_10306370: base AND view) and appends u32 n records,
// then fires RECOVERY_QUERY_RESULT("%s", category). The VIEW is filled only by UpdateFilter
// (FUN_10306f80 -> the generic FUN_102499c0 with no filters or sorts) and re-applied after a
// successful recovery: until the UI calls UpdateFilter, GetNumItemsInCategory is 0. Recovered
// items (+0x20) never pass the filter.
//
// Records (common base RecoveryServiceType): str Id +8, u8 recovered +0x20, u64 date +0x28,
// u64 money cost +0x30, item costs {u32 item, u32 count} +0x38, token costs {str token, u32, u32}
// +0x58. Wire order per type:
//   item  (FUN_10301a70): Id, u32 ItemEntry, u32 ItemCount, str, u32, u8, u64 date, u64 cost, maps
//   char  (FUN_10300ff0): Id, Name, Race, Sex, Class, u32 Level, u8, u64 date, u64 cost, maps
//   wforg (FUN_10302200): Id, u32 RequiredItemId, u32 RefundedItemId, u32 ItemCount, str, u32, u8,
//                         u64 date, u32 n -> item costs {375250 = n}
// Recover: CMSG {u32 position in the category's base list, str}; the (category, position) pair is
// held until any recover result arrives. A result string equal to <TYPE>_RECOVERY_OK marks that
// record recovered and re-applies the filter; every result fires RECOVERY_RESULT("%s%s",
// category, result).
//
// The item search (FUN_103004e0) needs the item addon record (FUN_101a2690, filled by
// SMSG_PATCH_ITEM_ADDON 0x569 -- AscItemAddon): an item the server never described never matches.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscToken.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace AscScript;

namespace
{
    const char* const kCategories[8] = {"RECOVERY_SERVICE_CATEGORY_NONE",
        "RECOVERY_SERVICE_CATEGORY_DISENCHANTED_ITEM", "RECOVERY_SERVICE_CATEGORY_DELETED_CHARACTER",
        "RECOVERY_SERVICE_CATEGORY_VENDORED_ITEM", "RECOVERY_SERVICE_CATEGORY_DELETED_ITEM",
        "RECOVERY_SERVICE_CATEGORY_MYTHIC_RECYCLING", "RECOVERY_SERVICE_CATEGORY_WORLDFORGED_ITEM",
        "RECOVERY_SERVICE_CATEGORY_MAX"};                                                    // 0x10B5A990
    const uint32_t kQueryOpcode[7] = {0, 0x5D2, 0x5DA, 0x5DE, 0x5E2, 0x5E6, 0x682};
    const uint32_t kRecoverOpcode[7] = {0, 0x5D4, 0x5DC, 0x5E0, 0x5E4, 0x5E8, 0x684};
    const char* const kRecoveryOk[7] = {nullptr, "DISENCHANTED_ITEM_RECOVERY_OK",
        "DELETED_CHARACTER_RECOVERY_OK", "VENDORED_ITEM_RECOVERY_OK", "DELETED_ITEM_RECOVERY_OK",
        "MYTHIC_RECYCLING_RECOVERY_OK", "WORLDFORGED_ITEM_RECOVERY_OK"};

    enum class Kind { Character, Item, Mythic, Worldforged };

    struct Record
    {
        Kind kind = Kind::Item;
        std::string id;
        bool recovered = false;
        uint64_t date = 0, cost = 0;
        std::vector<std::pair<uint32_t, uint32_t>> itemCosts;    // unordered_map, insertion order
        std::vector<std::pair<uint32_t, uint32_t>> tokenCosts;   // token index -> first value
        // character
        std::string name, race, sex, cls;
        uint32_t level = 0;
        // item / worldforged
        uint32_t itemEntry = 0, itemCount = 0, refundedItem = 0;
        std::string extraStr;
        uint32_t extraU32 = 0;
    };

    struct Container
    {
        std::vector<std::shared_ptr<Record>> base;
        std::vector<std::shared_ptr<Record>> view;
        std::string search;
    };

    Container g_containers[7];
    uint32_t g_pendingCategory = 0;
    uint32_t g_pendingPosition = 0xFFFFFFFF;

    // FUN_102fe640: the category's index, 0 for NONE or an unknown name.
    uint32_t CategoryIndex(const std::string& s)
    {
        for (uint32_t i = 0; i < 8; ++i)
            if (s == kCategories[i])
                return i;
        AscLog::Printf("RecoveryService: Unexpected Value: %s", s.c_str());
        return 0;
    }

    // ---- wire -----------------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }
    void MapSet(std::vector<std::pair<uint32_t, uint32_t>>& m, uint32_t key, uint32_t value)
    {
        for (auto& kv : m)
            if (kv.first == key)
            {
                kv.second = value;
                return;
            }
        m.emplace_back(key, value);
    }

    // The tail every non-worldforged record shares: u8 recovered, u64 date, u64 cost, then the two
    // cost maps (entries with a zero on the wrong side are dropped).
    void ReadCommonTail(CDataStore* p, Record& r)
    {
        r.recovered = Read<uint8_t>(p) != 0;
        r.date = Read<uint64_t>(p);
        r.cost = Read<uint64_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const uint32_t item = Read<uint32_t>(p), count = Read<uint32_t>(p);
            if (item != 0 && count != 0)
                MapSet(r.itemCosts, item, count);
        }
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const std::string token = ReadStr(p);
            const uint32_t a = Read<uint32_t>(p), b = Read<uint32_t>(p);
            uint32_t index;
            if (AscToken::TypeIndex(token.c_str(), index) && (a != 0 || b != 0))
                MapSet(r.tokenCosts, index, a);
        }
    }

    std::shared_ptr<Record> ReadItem(CDataStore* p, Kind kind)   // FUN_10301a70
    {
        auto r = std::make_shared<Record>();
        r->kind = kind;
        r->id = ReadStr(p);
        r->itemEntry = Read<uint32_t>(p);
        r->itemCount = Read<uint32_t>(p);
        r->extraStr = ReadStr(p);
        r->extraU32 = Read<uint32_t>(p);
        ReadCommonTail(p, *r);
        return r;
    }

    std::shared_ptr<Record> ReadCharacter(CDataStore* p)   // FUN_10300ff0
    {
        auto r = std::make_shared<Record>();
        r->kind = Kind::Character;
        r->id = ReadStr(p);
        r->name = ReadStr(p);
        r->race = ReadStr(p);
        r->sex = ReadStr(p);
        r->cls = ReadStr(p);
        r->level = Read<uint32_t>(p);
        ReadCommonTail(p, *r);
        return r;
    }

    std::shared_ptr<Record> ReadWorldforged(CDataStore* p)   // FUN_10302200
    {
        auto r = std::make_shared<Record>();
        r->kind = Kind::Worldforged;
        r->id = ReadStr(p);
        r->itemEntry = Read<uint32_t>(p);     // RequiredItemId
        r->refundedItem = Read<uint32_t>(p);  // RefundedItemId
        r->itemCount = Read<uint32_t>(p);
        r->extraStr = ReadStr(p);
        r->extraU32 = Read<uint32_t>(p);
        r->recovered = Read<uint8_t>(p) != 0;
        r->date = Read<uint64_t>(p);
        if (const uint32_t n = Read<uint32_t>(p))
            MapSet(r->itemCosts, 0x5B9D2, n);
        return r;
    }

    // ---- Lua ------------------------------------------------------------------------------------
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        PushStr(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    void SetInt(lua_State* L, const char* k, uint32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushinteger(L, static_cast<int>(v));
        AscLua::lua_settable(L, -3);
    }
    void SetNum(lua_State* L, const char* k, int32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushnumber(L, static_cast<double>(v));
        AscLua::lua_settable(L, -3);
    }

    void PushItemCosts(lua_State* L, const Record& r)   // FUN_102222e0
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(r.itemCosts.size()));
        AscLua::lua_checkstack(L, 2);
        for (const auto& kv : r.itemCosts)
        {
            AscLua::lua_pushinteger(L, static_cast<int>(kv.first));
            AscLua::lua_pushinteger(L, static_cast<int>(kv.second));
            AscLua::lua_settable(L, -3);
        }
    }

    void PushTokenCosts(lua_State* L, const Record& r)   // FUN_102fe830
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(r.tokenCosts.size()));
        AscLua::lua_checkstack(L, 2);
        for (const auto& kv : r.tokenCosts)
        {
            const char* name = AscToken::TypeName(kv.first);
            PushStr(L, name ? name : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(kv.first)).c_str());
            AscLua::lua_pushinteger(L, static_cast<int>(kv.second));
            AscLua::lua_settable(L, -3);
        }
    }

    // Money when positive, else the item costs, else the token costs, else nil.
    void SetRecoveryCost(lua_State* L, const char* k, const Record& r)
    {
        AscLua::lua_pushstring(L, k);
        if (static_cast<int64_t>(r.cost) > 0)
            AscLua::lua_pushinteger(L, static_cast<int>(static_cast<uint32_t>(r.cost)));
        else if (!r.itemCosts.empty())
            PushItemCosts(L, r);
        else if (!r.tokenCosts.empty())
            PushTokenCosts(L, r);
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
    }

    void PushRecord(lua_State* L, const Record& r)   // item vtable +4
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "Id", r.id);
        switch (r.kind)
        {
        case Kind::Character:   // FUN_10305b20
            SetStr(L, "Name", r.name);
            SetStr(L, "Race", r.race);
            SetStr(L, "Sex", r.sex);
            SetStr(L, "Class", r.cls);
            SetNum(L, "Level", static_cast<int32_t>(r.level));
            SetRecoveryCost(L, "RecoveryCost", r);
            break;
        case Kind::Item:        // FUN_10305d00
        case Kind::Mythic:      // FUN_10305e30
            SetInt(L, "ItemEntry", r.itemEntry);
            SetInt(L, "ItemCount", r.itemCount);
            SetRecoveryCost(L, r.kind == Kind::Mythic ? "RecoveryCosts" : "RecoveryCost", r);
            break;
        case Kind::Worldforged: // FUN_10305f60
            SetInt(L, "RequiredItemId", r.itemEntry);
            SetInt(L, "RefundedItemId", r.refundedItem);
            SetInt(L, "ItemCount", r.itemCount);
            AscLua::lua_pushstring(L, "RecoveryCosts");
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            AscLua::lua_pushinteger(L, static_cast<int>(r.itemEntry));
            AscLua::lua_pushinteger(L, static_cast<int>(r.itemCount));
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "AdditionalRefund");
            PushItemCosts(L, r);
            AscLua::lua_settable(L, -3);
            break;
        }
        SetNum(L, "Date", static_cast<int32_t>(static_cast<uint32_t>(r.date)));
    }

    // ---- search ---------------------------------------------------------------------------------
    std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }
    // FUN_102ffa20: drop a leading prefix, '_' -> ' ', lowercase.
    std::string Normalize(std::string s, const char* prefix)
    {
        const size_t n = strlen(prefix);
        if (n && s.size() >= n && s.compare(0, n, prefix) == 0)
            s.erase(0, n);
        std::replace(s.begin(), s.end(), '_', ' ');
        return Lower(s);
    }
    bool Has(const std::string& field, const std::string& search) { return field.find(search) != std::string::npos; }

    // FUN_101a2690: the item addon record (SMSG_PATCH_ITEM_ADDON 0x569).
    struct ItemQuery { const char* name; const char* description; uint32_t quality; };
    const ItemQuery* ItemQueryRecord(uint32_t entry)
    {
        static ItemQuery q;
        const AscItemAddon::Record* r = AscItemAddon::Find(entry);
        if (!r)
            return nullptr;
        q = {r->name, r->description, r->Quality()};
        return &q;
    }

    const char* SubclassToken(uint32_t cls, uint32_t sub)
    {
        static const char* const k0[9] = {"ITEM_SUBCLASS_CONSUMABLE", "ITEM_SUBCLASS_POTION", "ITEM_SUBCLASS_ELIXIR",
            "ITEM_SUBCLASS_FLASK", "ITEM_SUBCLASS_SCROLL", "ITEM_SUBCLASS_FOOD", "ITEM_SUBCLASS_ITEM_ENHANCEMENT",
            "ITEM_SUBCLASS_BANDAGE", "ITEM_SUBCLASS_CONSUMABLE_OTHER"};
        static const char* const k1[9] = {"ITEM_SUBCLASS_CONTAINER", "ITEM_SUBCLASS_SOUL_CONTAINER",
            "ITEM_SUBCLASS_HERB_CONTAINER", "ITEM_SUBCLASS_ENCHANTING_CONTAINER", "ITEM_SUBCLASS_ENGINEERING_CONTAINER",
            "ITEM_SUBCLASS_GEM_CONTAINER", "ITEM_SUBCLASS_MINING_CONTAINER", "ITEM_SUBCLASS_LEATHERWORKING_CONTAINER",
            "ITEM_SUBCLASS_INSCRIPTION_CONTAINER"};
        static const char* const k2[22] = {"ITEM_SUBCLASS_WEAPON_AXE", "ITEM_SUBCLASS_WEAPON_AXE2",
            "ITEM_SUBCLASS_WEAPON_BOW", "ITEM_SUBCLASS_WEAPON_GUN", "ITEM_SUBCLASS_WEAPON_MACE",
            "ITEM_SUBCLASS_WEAPON_MACE2", "ITEM_SUBCLASS_WEAPON_POLEARM", "ITEM_SUBCLASS_WEAPON_SWORD",
            "ITEM_SUBCLASS_WEAPON_SWORD2", "ITEM_SUBCLASS_WEAPON_obsolete", "ITEM_SUBCLASS_WEAPON_STAFF",
            "ITEM_SUBCLASS_WEAPON_EXOTIC", "ITEM_SUBCLASS_WEAPON_EXOTIC2", "ITEM_SUBCLASS_WEAPON_FIST",
            "ITEM_SUBCLASS_WEAPON_MISC", "ITEM_SUBCLASS_WEAPON_DAGGER", "ITEM_SUBCLASS_WEAPON_THROWN",
            "ITEM_SUBCLASS_WEAPON_SPEAR", "ITEM_SUBCLASS_WEAPON_CROSSBOW", "ITEM_SUBCLASS_WEAPON_WAND",
            "ITEM_SUBCLASS_WEAPON_FISHING_POLE", "ITEM_SUBCLASS_WEAPON_MAX"};
        static const char* const k3[9] = {"ITEM_SUBCLASS_GEM_RED", "ITEM_SUBCLASS_GEM_BLUE", "ITEM_SUBCLASS_GEM_YELLOW",
            "ITEM_SUBCLASS_GEM_PURPLE", "ITEM_SUBCLASS_GEM_GREEN", "ITEM_SUBCLASS_GEM_ORANGE", "ITEM_SUBCLASS_GEM_META",
            "ITEM_SUBCLASS_GEM_SIMPLE", "ITEM_SUBCLASS_GEM_PRISMATIC"};
        static const char* const k4[12] = {"ITEM_SUBCLASS_ARMOR_MISC", "ITEM_SUBCLASS_ARMOR_CLOTH",
            "ITEM_SUBCLASS_ARMOR_LEATHER", "ITEM_SUBCLASS_ARMOR_MAIL", "ITEM_SUBCLASS_ARMOR_PLATE",
            "ITEM_SUBCLASS_ARMOR_BUCKLER", "ITEM_SUBCLASS_ARMOR_SHIELD", "ITEM_SUBCLASS_ARMOR_LIBRAM",
            "ITEM_SUBCLASS_ARMOR_IDOL", "ITEM_SUBCLASS_ARMOR_TOTEM", "ITEM_SUBCLASS_ARMOR_SIGIL",
            "ITEM_SUBCLASS_ARMOR_MAX"};
        static const char* const k6[5] = {"ITEM_SUBCLASS_WAND", "ITEM_SUBCLASS_BOLT", "ITEM_SUBCLASS_ARROW",
            "ITEM_SUBCLASS_BULLET", "ITEM_SUBCLASS_THROWN"};
        static const char* const k7[16] = {"ITEM_SUBCLASS_TRADE_GOODS", "ITEM_SUBCLASS_PARTS",
            "ITEM_SUBCLASS_EXPLOSIVES", "ITEM_SUBCLASS_DEVICES", "ITEM_SUBCLASS_JEWELCRAFTING", "ITEM_SUBCLASS_CLOTH",
            "ITEM_SUBCLASS_LEATHER", "ITEM_SUBCLASS_METAL_STONE", "ITEM_SUBCLASS_MEAT", "ITEM_SUBCLASS_HERB",
            "ITEM_SUBCLASS_ELEMENTAL", "ITEM_SUBCLASS_TRADE_GOODS_OTHER", "ITEM_SUBCLASS_ENCHANTING",
            "ITEM_SUBCLASS_MATERIAL", "ITEM_SUBCLASS_ARMOR_ENCHANTMENT", "ITEM_SUBCLASS_WEAPON_ENCHANTMENT"};
        static const char* const k9[12] = {"ITEM_SUBCLASS_BOOK", "ITEM_SUBCLASS_LEATHERWORKING_PATTERN",
            "ITEM_SUBCLASS_TAILORING_PATTERN", "ITEM_SUBCLASS_ENGINEERING_SCHEMATIC", "ITEM_SUBCLASS_BLACKSMITHING",
            "ITEM_SUBCLASS_COOKING_RECIPE", "ITEM_SUBCLASS_ALCHEMY_RECIPE", "ITEM_SUBCLASS_FIRST_AID_MANUAL",
            "ITEM_SUBCLASS_ENCHANTING_FORMULA", "ITEM_SUBCLASS_FISHING_MANUAL", "ITEM_SUBCLASS_JEWELCRAFTING_RECIPE",
            "ITEM_SUBCLASS_WOODCUTTING_RECEIPE"};
        static const char* const k11[4] = {"ITEM_SUBCLASS_QUIVER0", "ITEM_SUBCLASS_QUIVER1", "ITEM_SUBCLASS_QUIVER",
            "ITEM_SUBCLASS_AMMO_POUCH"};
        static const char* const k13[2] = {"ITEM_SUBCLASS_KEY", "ITEM_SUBCLASS_LOCKPICK"};
        static const char* const k15[6] = {"ITEM_SUBCLASS_JUNK", "ITEM_SUBCLASS_JUNK_REAGENT", "ITEM_SUBCLASS_JUNK_PET",
            "ITEM_SUBCLASS_JUNK_HOLIDAY", "ITEM_SUBCLASS_JUNK_OTHER", "ITEM_SUBCLASS_JUNK_MOUNT"};
        static const uint32_t kGlyphIds[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 11};
        static const char* const kGlyph[10] = {"ITEM_SUBCLASS_GLYPH_WARRIOR", "ITEM_SUBCLASS_GLYPH_PALADIN",
            "ITEM_SUBCLASS_GLYPH_HUNTER", "ITEM_SUBCLASS_GLYPH_ROGUE", "ITEM_SUBCLASS_GLYPH_PRIEST",
            "ITEM_SUBCLASS_GLYPH_DEATH_KNIGHT", "ITEM_SUBCLASS_GLYPH_SHAMAN", "ITEM_SUBCLASS_GLYPH_MAGE",
            "ITEM_SUBCLASS_GLYPH_WARLOCK", "ITEM_SUBCLASS_GLYPH_DRUID"};
        switch (cls)
        {
        case 0: return sub < 9 ? k0[sub] : "";
        case 1: return sub < 9 ? k1[sub] : "";
        case 2: return sub < 22 ? k2[sub] : "";
        case 3: return sub < 9 ? k3[sub] : "";
        case 4: return sub < 12 ? k4[sub] : "";
        case 5: return sub == 0 ? "ITEM_SUBCLASS_REAGENT" : "";
        case 6: return sub < 5 ? k6[sub] : "";
        case 7: return sub < 16 ? k7[sub] : "";
        case 8: return sub == 0 ? "ITEM_SUBCLASS_GENERIC" : "";
        case 9: return sub < 12 ? k9[sub] : "";
        case 10: return sub == 0 ? "ITEM_SUBCLASS_MONEY" : "";
        case 11: return sub < 4 ? k11[sub] : "";
        case 12: return sub == 0 ? "ITEM_SUBCLASS_QUEST" : "";
        case 13: return sub < 2 ? k13[sub] : "";
        case 14: return sub == 0 ? "ITEM_SUBCLASS_PERMANENT" : "";
        case 15: return sub < 6 ? k15[sub] : "";
        case 16:
            for (uint32_t i = 0; i < 10; ++i)
                if (kGlyphIds[i] == sub)
                    return kGlyph[i];
            return "";
        default: return "ITEM_SUBCLASS_NONE";
        }
    }

    // FUN_103004e0: name, description, quality, class, subclass or inventory type contains the
    // (lowercased) search. No Item.dbc row or no cached record: never a match.
    bool ItemMatches(const std::string& search, uint32_t entry)
    {
        static const char* const kQuality[9] = {"ITEM_QUALITY_POOR", "ITEM_QUALITY_NORMAL", "ITEM_QUALITY_UNCOMMON",
            "ITEM_QUALITY_RARE", "ITEM_QUALITY_EPIC", "ITEM_QUALITY_LEGENDARY", "ITEM_QUALITY_ARTIFACT",
            "ITEM_QUALITY_HEIRLOOM", "MAX_ITEM_QUALITY"};
        static const char* const kClass[17] = {"ITEM_CLASS_CONSUMABLE", "ITEM_CLASS_CONTAINER", "ITEM_CLASS_WEAPON",
            "ITEM_CLASS_GEM", "ITEM_CLASS_ARMOR", "ITEM_CLASS_REAGENT", "ITEM_CLASS_PROJECTILE",
            "ITEM_CLASS_TRADE_GOODS", "ITEM_CLASS_GENERIC", "ITEM_CLASS_RECIPE", "ITEM_CLASS_MONEY",
            "ITEM_CLASS_QUIVER", "ITEM_CLASS_QUEST", "ITEM_CLASS_KEY", "ITEM_CLASS_PERMANENT", "ITEM_CLASS_MISC",
            "ITEM_CLASS_GLYPH"};
        static const char* const kInvType[29] = {"INVTYPE_NON_EQUIP", "INVTYPE_HEAD", "INVTYPE_NECK",
            "INVTYPE_SHOULDERS", "INVTYPE_BODY", "INVTYPE_CHEST", "INVTYPE_WAIST", "INVTYPE_LEGS", "INVTYPE_FEET",
            "INVTYPE_WRISTS", "INVTYPE_HANDS", "INVTYPE_FINGER", "INVTYPE_TRINKET", "INVTYPE_WEAPON",
            "INVTYPE_SHIELD", "INVTYPE_RANGED", "INVTYPE_CLOAK", "INVTYPE_2HWEAPON", "INVTYPE_BAG", "INVTYPE_TABARD",
            "INVTYPE_ROBE", "INVTYPE_WEAPONMAINHAND", "INVTYPE_WEAPONOFFHAND", "INVTYPE_HOLDABLE", "INVTYPE_AMMO",
            "INVTYPE_THROWN", "INVTYPE_RANGEDRIGHT", "INVTYPE_QUIVER", "INVTYPE_RELIC"};
        if (entry == 0)
            return false;
        const uint8_t* row = ClientDbcRow(kItemDbc, entry);   // FUN_100b1870
        if (!row)
            return false;
        const ItemQuery* q = ItemQueryRecord(entry);
        if (!q)
            return false;
        if (search.empty())
            return true;
        const std::string s = Normalize(search, "");
        if (Has(Normalize(q->name ? q->name : "", ""), s) || Has(Normalize(q->description ? q->description : "", ""), s))
            return true;
        if (q->quality < 9 && Has(Normalize(kQuality[q->quality], "ITEM_QUALITY_"), s))
            return true;
        const uint32_t cls = *reinterpret_cast<const uint32_t*>(row + 4);
        const uint32_t sub = *reinterpret_cast<const uint32_t*>(row + 8);
        const uint32_t inv = *reinterpret_cast<const uint32_t*>(row + 0x18);
        if (cls < 17 && Has(Normalize(kClass[cls], "ITEM_CLASS_"), s))
            return true;
        if (Has(Normalize(SubclassToken(cls, sub), "ITEM_SUBCLASS_"), s))
            return true;
        return inv < 29 && Has(Normalize(kInvType[inv], "INVTYPE_"), s);
    }

    // FUN_102ffbf0: name, race name, sex, class name or level contains the (lowercased) search.
    bool CharacterMatches(const std::string& search, const Record& r)
    {
        static const char* const kRaces[11] = {"RACE_NONE", "RACE_HUMAN", "RACE_ORC", "RACE_DWARF", "RACE_NIGHTELF",
            "RACE_UNDEAD_PLAYER", "RACE_TAUREN", "RACE_GNOME", "RACE_TROLL", "RACE_BLOODELF", "RACE_DRAENEI"};
        static const uint32_t kRaceIds[11] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11};
        static const char* const kClasses[33] = {"CLASS_NONE", "CLASS_WARRIOR", "CLASS_PALADIN", "CLASS_HUNTER",
            "CLASS_ROGUE", "CLASS_PRIEST", "CLASS_DEATH_KNIGHT", "CLASS_SHAMAN", "CLASS_MAGE", "CLASS_WARLOCK",
            "CLASS_HERO", "CLASS_DRUID", "CLASS_BARBARIAN", "CLASS_WITCH_DOCTOR", "CLASS_DEMON_HUNTER",
            "CLASS_WITCH_HUNTER", "CLASS_STORMBRINGER", "CLASS_KNIGHT_OF_XOROTH", "CLASS_GUARDIAN", "CLASS_MONK",
            "CLASS_SON_OF_ARUGAL", "CLASS_RANGER", "CLASS_CHRONOMANCER", "CLASS_NECROMANCER", "CLASS_PYROMANCER",
            "CLASS_CULTIST", "CLASS_STARCALLER", "CLASS_SUN_CLERIC", "CLASS_TINKER", "CLASS_VENOMANCER",
            "CLASS_REAPER", "CLASS_PRIMALIST", "CLASS_RUNEMASTER"};
        if (search.empty())
            return true;
        const std::string name = Lower(r.name);

        std::string race = "none";
        uint32_t raceId = 0;
        for (uint32_t i = 0; i < 11; ++i)
            if (r.race == kRaces[i])
                raceId = kRaceIds[i];
        if (const uint8_t* row = ClientDbcRow(0xAD3428, raceId))   // ChrRaces, FUN_100b13f0
            if (const char* n = *reinterpret_cast<const char* const*>(row + 0x38))
                race = Lower(n);

        std::string sex = r.sex;
        const size_t g = sex.find("GENDER_");
        if (g != std::string::npos)
            sex.erase(g, 7);
        std::replace(sex.begin(), sex.end(), '_', ' ');
        sex = Lower(sex);

        std::string cls = "none";
        uint32_t classId = 0;
        for (uint32_t i = 0; i < 33; ++i)
            if (r.cls == kClasses[i])
                classId = i;
        if (const uint8_t* row = ClientDbcRow(0xAD3404, classId))  // ChrClasses, FUN_100b13c0
            if (const char* n = *reinterpret_cast<const char* const*>(row + 0x10))
                cls = Lower(n);

        const std::string level = std::to_string(r.level);
        const std::string s = Lower(search);
        return Has(name, s) || Has(race, s) || Has(sex, s) || Has(cls, s) || Has(level, s);
    }

    // FUN_10307270: a recovered record never shows; otherwise the type's own search.
    bool Matches(const std::string& search, const Record& r)
    {
        if (r.recovered)
            return false;
        return r.kind == Kind::Character ? CharacterMatches(search, r) : ItemMatches(search, r.itemEntry);
    }

    // FUN_100c2840 via the container's pair test ([1] FUN_100cc230, `xor al, al`): any "$<c><digits>"
    // token in the search rejects everything.
    bool HasSearchToken(const std::string& s)
    {
        size_t pos = 0;
        while ((pos = s.find('$', pos)) != std::string::npos)
        {
            size_t end = s.find(' ', pos);
            if (end == std::string::npos)
                end = s.size();
            if (pos + 2 < end)
            {
                bool digits = true;
                for (size_t i = pos + 2; i < end; ++i)
                    if (!isdigit(static_cast<unsigned char>(s[i])))
                        digits = false;
                if (digits)
                    return true;
            }
            pos = end;
        }
        return false;
    }

    // FUN_10306f80 -> FUN_102499c0(search, {}, {}, 1). An empty base leaves the view alone.
    void ApplyFilter(uint32_t category, const std::string& search)
    {
        Container& c = g_containers[category];
        if (c.base.empty())
            return;
        const bool token = HasSearchToken(search);
        const std::string lowered = Lower(search);
        c.view.clear();
        for (const auto& r : c.base)
            if (!token && Matches(lowered, *r))
                c.view.push_back(r);
        c.search = search;
    }

    void Reset(uint32_t category)   // FUN_10306370 / FUN_103062d0
    {
        g_containers[category].base.clear();
        g_containers[category].view.clear();
        g_containers[category].search.clear();
    }

    // ---- bindings -------------------------------------------------------------------------------
    int QueryCategory(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        const uint32_t c = CategoryIndex(name);
        if (c == 0)
            return 0;
        if (c > 6)
        {
            PushBool(L, false);
            return 1;
        }
        Packet(kQueryOpcode[c]).Send();
        PushBool(L, true);
        return 1;
    }

    int GetNumItemsInCategory(lua_State* L)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        const uint32_t c = CategoryIndex(name);
        if (c == 0)
            return 0;
        AscLua::lua_pushinteger(L, c <= 6 ? static_cast<int>(g_containers[c].view.size()) : 0);
        return 1;
    }

    int GetCategoryItemAtIndex(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string name = CheckString(L, 1);
        const int32_t index = ToInt(CheckNumber(L, 2));
        if (index == 0)
            return 0;
        const uint32_t c = CategoryIndex(name);
        if (c == 0)
            return 0;
        const uint32_t i = static_cast<uint32_t>(index - 1);
        if (c <= 6 && i < g_containers[c].view.size())
            PushRecord(L, *g_containers[c].view[i]);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int UpdateFilter(lua_State* L)   // FUN_10306e50
    {
        if (!ValidateInput(L, {STRING, STRING}))
            return 0;
        const std::string name = CheckString(L, 1);
        const std::string search = CheckString(L, 2);
        const uint32_t c = CategoryIndex(name);
        if (c != 0 && c <= 6)
            ApplyFilter(c, search);
        return 0;
    }

    // RecoverCategoryItemAtIndex(category, index, text) -- FUN_102fe420 reads {string, number, string}.
    int RecoverCategoryItemAtIndex(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, NUMBER, STRING}))
            return 0;
        const std::string name = CheckString(L, 1);
        const int32_t index = ToInt(CheckNumber(L, 2));
        const std::string text = CheckString(L, 3);
        if (index == 0)
            return 0;
        const uint32_t c = CategoryIndex(name);
        if (c == 0)
            return 0;
        int32_t position = -1;
        if (c <= 6)
        {
            Container& ct = g_containers[c];
            const uint32_t i = static_cast<uint32_t>(index - 1);
            if (i < ct.view.size())
            {
                auto it = std::find(ct.base.begin(), ct.base.end(), ct.view[i]);
                if (it != ct.base.end())
                    position = static_cast<int32_t>(it - ct.base.begin());
            }
        }
        if (position < 0)
        {
            PushBool(L, false);
            return 1;
        }
        Packet p(kRecoverOpcode[c]);
        p.U32(static_cast<uint32_t>(position)).Str(text.c_str());
        p.Send();
        g_pendingCategory = c;
        g_pendingPosition = static_cast<uint32_t>(position);
        PushBool(L, true);
        return 1;
    }

    // ---- packets --------------------------------------------------------------------------------
    template <uint32_t Category>
    void __cdecl OnQueryResponse(void*, uint32_t, uint32_t, CDataStore* p)
    {
        Reset(Category);
        Container& c = g_containers[Category];
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            switch (Category)
            {
            case 2: c.base.push_back(ReadCharacter(p)); break;
            case 5: c.base.push_back(ReadItem(p, Kind::Mythic)); break;
            case 6: c.base.push_back(ReadWorldforged(p)); break;
            default: c.base.push_back(ReadItem(p, Kind::Item)); break;
            }
        }
        AscRuntime::Signal("RECOVERY_QUERY_RESULT", "%s", kCategories[Category]);
    }

    // FUN_103060b0: success marks the PENDING record recovered and re-applies that category's search.
    template <uint32_t Category>
    void __cdecl OnRecoverResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadStr(p);
        const bool ok = result == kRecoveryOk[Category];
        if (ok && g_pendingCategory >= 1 && g_pendingCategory <= 6)
        {
            Container& c = g_containers[g_pendingCategory];
            if (g_pendingPosition < c.base.size() && c.base[g_pendingPosition])
                c.base[g_pendingPosition]->recovered = true;
            const std::string search = c.search;
            ApplyFilter(g_pendingCategory, search);
        }
        g_pendingCategory = 0;
        g_pendingPosition = 0xFFFFFFFF;
        AscRuntime::Signal("RECOVERY_RESULT", "%s%s", kCategories[Category], result.c_str());
    }

    void Init()
    {
        sDC.AddPacketHandler(0x5D3, CNetClientCustomPacket((void*)&OnQueryResponse<1>, nullptr));
        sDC.AddPacketHandler(0x5DB, CNetClientCustomPacket((void*)&OnQueryResponse<2>, nullptr));
        sDC.AddPacketHandler(0x5DF, CNetClientCustomPacket((void*)&OnQueryResponse<3>, nullptr));
        sDC.AddPacketHandler(0x5E3, CNetClientCustomPacket((void*)&OnQueryResponse<4>, nullptr));
        sDC.AddPacketHandler(0x5E7, CNetClientCustomPacket((void*)&OnQueryResponse<5>, nullptr));
        sDC.AddPacketHandler(0x683, CNetClientCustomPacket((void*)&OnQueryResponse<6>, nullptr));
        sDC.AddPacketHandler(0x5D5, CNetClientCustomPacket((void*)&OnRecoverResult<1>, nullptr));
        sDC.AddPacketHandler(0x5DD, CNetClientCustomPacket((void*)&OnRecoverResult<2>, nullptr));
        sDC.AddPacketHandler(0x5E1, CNetClientCustomPacket((void*)&OnRecoverResult<3>, nullptr));
        sDC.AddPacketHandler(0x5E5, CNetClientCustomPacket((void*)&OnRecoverResult<4>, nullptr));
        sDC.AddPacketHandler(0x5E9, CNetClientCustomPacket((void*)&OnRecoverResult<5>, nullptr));
        sDC.AddPacketHandler(0x685, CNetClientCustomPacket((void*)&OnRecoverResult<6>, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_RecoveryService", "GetCategoryItemAtIndex", GetCategoryItemAtIndex},
        {"C_RecoveryService", "GetNumItemsInCategory", GetNumItemsInCategory},
        {"C_RecoveryService", "QueryCategory", QueryCategory},
        {"C_RecoveryService", "RecoverCategoryItemAtIndex", RecoverCategoryItemAtIndex},
        {"C_RecoveryService", "UpdateFilter", UpdateFilter},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
