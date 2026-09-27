// C_CustomStore (0x101A5DE0..0x101A6C00): the server-defined NPC / game-object stores.
//
// Manager FUN_101a5c30 -> 0x10BCA5D0: +0x00 store types (0x3C each: u32 id, u32 creature entry, u32
// game-object entry, str, str -- SMSG 0x6BD), +0x0C page size (0x12 by default), +0x10 items (0x40 each:
// id, type, item entry, money, 5 token items, 5 token counts, game event, achievement -- SMSG 0x6BA),
// then the CustomStoreDataEntryContainer (vtable 0x10B3A54C) over the items. Module init 0x101A5950:
// glue reset 0x101A3EB0; SMSG 0x6BA QUERY_CUSTOM_STORE_RESULT, 0x6BC PURCHASE_CUSTOM_STORE_ITEM_RESULT,
// 0x6BD the store types. CMSG 0x6B9 {u32 store} queries, 0x6BB {u32 item, u32 count} buys.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSkillCard.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace AscScript;

namespace AscItems { uint32_t BagItemCount(uint32_t item); }   // FUN_10308470
namespace AscCA { uint32_t UnitItemCount(const void* unit, uint32_t item); }   // FUN_10308570
namespace AscVanity { bool Owned(uint32_t id); }   // FUN_1032ea50

namespace
{
    const char* const kQuery[6] = {"QUERY_CUSTOM_STORE_OK", "QUERY_CUSTOM_STORE_UNKNOWN", "QUERY_CUSTOM_STORE_BAD_STORE",
        "QUERY_CUSTOM_STORE_NOT_FOUND", "QUERY_CUSTOM_STORE_NPC_NOT_IN_RANGE", "QUERY_CUSTOM_STORE_GO_NOT_IN_RANGE"};   // 0x10B39D10
    const char* const kPurchase[14] = {"PURCHASE_CUSTOM_STORE_ITEM_OK", "PURCHASE_CUSTOM_STORE_ITEM_UNKNOWN",
        "PURCHASE_CUSTOM_STORE_ITEM_NOT_FOUND", "PURCHASE_CUSTOM_STORE_ITEM_STORE_NOT_FOUND",
        "PURCHASE_CUSTOM_STORE_ITEM_NPC_NOT_IN_RANGE", "PURCHASE_CUSTOM_STORE_ITEM_GO_NOT_IN_RANGE",
        "PURCHASE_CUSTOM_STORE_ITEM_COUNT_TOO_LOW", "PURCHASE_CUSTOM_STORE_ITEM_COUNT_MONEY_OUT_OF_RANGE",
        "PURCHASE_CUSTOM_STORE_ITEM_NOT_ENOUGH_MONEY", "PURCHASE_CUSTOM_STORE_ITEM_COUNT_ITEMS_OUT_OF_RANGE",
        "PURCHASE_CUSTOM_STORE_ITEM_NOT_ENOUGH_ITEMS", "PURCHASE_CUSTOM_STORE_ITEM_CONDITIONS",
        "PURCHASE_CUSTOM_STORE_ITEM_GAME_EVENT", "PURCHASE_CUSTOM_STORE_ITEM_ACHIEVEMENT"};   // 0x10B3A4A8
    const char* const kFilters[31] = {"FILTER_NONE", "FILTER_CAN_AFFORD", "FILTER_CANNOT_AFFORD", "FILTER_VANITY_OWNED",
        "FILTER_VANITY_UNOWNED", "FILTER_QUALITY_POOR", "FILTER_QUALITY_NORMAL", "FILTER_QUALITY_UNCOMMON",
        "FILTER_QUALITY_RARE", "FILTER_QUALITY_EPIC", "FILTER_QUALITY_LEGENDARY", "FILTER_QUALITY_ARTIFACT",
        "FILTER_QUALITY_HEIRLOOM", "FILTER_SKILL_CARD_OWNED", "FILTER_SKILL_CARD_UNOWNED",
        "FILTER_SKILL_CARD_QUALITY_COMMON", "FILTER_SKILL_CARD_QUALITY_UNCOMMON", "FILTER_SKILL_CARD_QUALITY_RARE",
        "FILTER_SKILL_CARD_QUALITY_EPIC", "FILTER_SKILL_CARD_QUALITY_LEGENDARY", "FILTER_SKILL_CARD_SPELL_FAMILY_GENERIC",
        "FILTER_SKILL_CARD_SPELL_FAMILY_MAGE", "FILTER_SKILL_CARD_SPELL_FAMILY_WARRIOR",
        "FILTER_SKILL_CARD_SPELL_FAMILY_WARLOCK", "FILTER_SKILL_CARD_SPELL_FAMILY_PRIEST",
        "FILTER_SKILL_CARD_SPELL_FAMILY_DRUID", "FILTER_SKILL_CARD_SPELL_FAMILY_ROGUE",
        "FILTER_SKILL_CARD_SPELL_FAMILY_HUNTER", "FILTER_SKILL_CARD_SPELL_FAMILY_PALADIN",
        "FILTER_SKILL_CARD_SPELL_FAMILY_SHAMAN", "FILTER_SKILL_CARD_SPELL_FAMILY_DEATH_KNIGHT"};   // 0x10B39C18
    const uint32_t kFamilies[11] = {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15};   // filters 20..30 (SpellFamilyName)

    struct Type { uint32_t id = 0, creature = 0, gameObject = 0; std::string first, second; };
    struct Item
    {
        uint32_t id = 0, type = 0, item = 0, money = 0;
        uint32_t tokens[5] = {}, counts[5] = {};
        uint32_t gameEvent = 0, achievement = 0;
    };

    std::vector<Type> g_types;   // +0x00
    uint32_t g_pageSize = 0x12;  // +0x0C
    std::vector<Item> g_items;   // +0x10

    const Type* FindType(uint32_t id)
    {
        for (const Type& t : g_types)
            if (t.id == id)
                return &t;
        return nullptr;
    }
    const Item* FindItem(uint32_t id)
    {
        for (const Item& i : g_items)
            if (i.id == id)
                return &i;
        return nullptr;
    }

    uint32_t Money()
    {
        uint8_t* p = ActivePlayer();
        return p ? *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0x1248) : 0;
    }
    uint32_t Held(uint32_t item) { return AscItems::BagItemCount(item) + AscCA::UnitItemCount(ActivePlayer(), item); }

    // FUN_101a3d50 / FUN_101a3bf0: a visible unit (mask 8) / game object (0x20) of `entry` within 5.5 yd.
    std::vector<uint8_t*> g_nearby;
    int __cdecl CollectUnit(uint64_t guid, void*)
    {
        if (uint8_t* o = static_cast<uint8_t*>(ObjectPtr(guid, 8)))
            g_nearby.push_back(o);
        return 1;
    }
    int __cdecl CollectGameObject(uint64_t guid, void*)
    {
        if (uint8_t* o = static_cast<uint8_t*>(ObjectPtr(guid, 0x20)))
            g_nearby.push_back(o);
        return 1;
    }
    void Position(uint8_t* object, float* xyz)
    {
        typedef void(__thiscall* GetPosition_t)(void*, float*);
        (*reinterpret_cast<GetPosition_t**>(object))[0x2C / 4](object, xyz);
    }
    bool Nearby(uint32_t entry, bool gameObject)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        float p[3];
        Position(player, p);
        g_nearby.clear();
        typedef int(__cdecl* Visit_t)(uint64_t, void*);
        reinterpret_cast<int(__cdecl*)(Visit_t, void*)>(0x4D4B30)(gameObject ? &CollectGameObject : &CollectUnit, nullptr);
        for (uint8_t* o : g_nearby)
        {
            float q[3];
            Position(o, q);
            const double d = std::sqrt(std::pow(q[0] - p[0], 2.0) + std::pow(q[1] - p[1], 2.0) + std::pow(q[2] - p[2], 2.0));
            if (static_cast<float>(d) <= 5.5f && *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(o + 8) + 0xC) == entry)
                return true;
        }
        return false;
    }

    // FUN_103087e0: the client's completed-achievement / quest record list at *(0xACFDB0 + 8).
    bool Completed(uint32_t id)
    {
        uint32_t node = *reinterpret_cast<const uint32_t*>(0xACFDB0 + 8);
        if (node & 1)
            node = 0;
        while (node)
        {
            if (*reinterpret_cast<const uint32_t*>(node + 8) == id)
                return true;
            const uint32_t next = *reinterpret_cast<const uint32_t*>(node + 4);
            node = (next & 1) ? 0 : next;
        }
        return false;
    }

    // FUN_101a3a20: PURCHASE_CUSTOM_STORE_ITEM_* for `count` of item `id`.
    uint32_t CanPurchase(uint32_t id, uint32_t count)
    {
        const Item* it = FindItem(id);
        if (!it)
            return 2;
        const Type* t = FindType(it->type);
        if (!t)
            return 3;
        if (t->creature && !Nearby(t->creature, false))
            return 4;
        if (t->gameObject && !Nearby(t->gameObject, true))
            return 5;
        if (count == 0)
            return 6;
        const uint32_t limit = static_cast<uint32_t>(0x7FFFFFFFull / count);
        if (limit < it->money)
            return 7;
        if (ActivePlayer() && it->money * count < Money())   // (sic: the original compares this way round)
            return 8;
        for (uint32_t k = 0; k < 5; ++k)
            if (it->tokens[k] && it->counts[k])
            {
                if (limit < it->counts[k])
                    return 9;
                if (Held(it->tokens[k]) < it->counts[k] * count)
                    return 10;
            }
        if (it->gameEvent && !AscGameEvents::IsActive(static_cast<uint16_t>(it->gameEvent)))
            return 12;
        if (it->achievement && !Completed(it->achievement))
            return 13;
        return 0;
    }

    // FUN_101a6f50 cases 0/1: money and every token covered.
    bool Affordable(const Item& it)
    {
        if (ActivePlayer() && Money() < it.money)
            return false;
        for (uint32_t k = 0; k < 5; ++k)
            if (it.tokens[k] && it.counts[k] && Held(it.tokens[k]) < it.counts[k])
                return false;
        return true;
    }

    // FUN_101a5d50: the item addon record's quality (+0x10) is `q`.
    bool ItemQuality(uint32_t item, uint32_t q)
    {
        const AscItemAddon::Record* r = AscItemAddon::Find(item);
        return r && r->Quality() == q;
    }

    class Container : public AscFilter::Container<const Item*>
    {
    protected:
        std::vector<const Item*> DoPopulate() override   // FUN_101a6d00
        {
            std::vector<const Item*> out;
            for (const Item& i : g_items)
                out.push_back(&i);
            return out;
        }

        bool ArgMatch(uint32_t f, const Item* const& it) override   // FUN_101a6f50
        {
            switch (f)
            {
            case 1: return Affordable(*it);
            case 2: return !Affordable(*it);
            case 3: return AscVanity::Owned(it->item);
            case 4: return !AscVanity::Owned(it->item);
            default: break;
            }
            if (f >= 5 && f <= 12)
                return ItemQuality(it->item, f - 5);
            const AscSkillCard::Card* c = AscSkillCard::ByItem(it->item);   // FUN_102142a0
            if (f == 13 || f == 14)
                return c && (AscSkillCard::CollectedAtLeast(c->cardId, c->rank) == (f == 13));
            if (f >= 15 && f <= 19)
                return c && AscSkillCard::QualityIndex(c->quality, 5) == static_cast<int>(f - 15);
            if (f >= 20 && f <= 30)
            {
                uint8_t rec[0x2A8];
                return c && FetchSpell(c->spell, rec) && *reinterpret_cast<const uint32_t*>(rec + 0x240) == kFamilies[f - 20];
            }
            return false;
        }

        uint32_t ArgGroup(uint32_t f) override   // FUN_101a75c0
        {
            if (f == 1 || f == 2) return 1;
            if (f == 3 || f == 4) return 2;
            if (f >= 5 && f <= 12) return 3;
            if (f == 13 || f == 14) return 4;
            if (f >= 15 && f <= 19) return 5;
            if (f >= 20 && f <= 30) return 6;
            return 0;
        }

        // FUN_101a6da0: the item's name (item addon record) contains the text; no text matches all.
        bool TextMatch(const std::string& lower, const Item* const& it) override
        {
            if (lower.empty())
                return true;
            const AscItemAddon::Record* r = AscItemAddon::Find(it->item);
            if (!r)
                return false;
            return AscFilter::Lower(r->name).find(lower) != std::string::npos;
        }
    };
    Container& Items() { static Container c; return c; }

    // ---- bindings ---------------------------------------------------------------------------------
    // FUN_101a5de0: (text, {FILTER_* = true}, {sorts, unused}, page size -- 0 means 18).
    int ApplyCustomStoreFilter(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, TABLE, TABLE, NUMBER}))
            return 0;
        const std::string text = CheckString(L, 1);
        std::vector<uint32_t> filters;
        std::map<std::string, bool> names;   // FUN_100bed80
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 2))
        {
            if (AscLua::lua_isstring(L, -2))
                names[AscLua::lua_tolstring(L, -2, nullptr)] = AscLua::lua_toboolean(L, -1) != 0;
            AscLua::lua_settop(L, -2);
        }
        for (const auto& n : names)
            if (n.second)
                for (uint32_t i = 0; i < 31; ++i)
                    if (n.first == kFilters[i])
                    {
                        filters.push_back(i);
                        break;
                    }
        const int32_t size = ToInt(CheckNumber(L, 4));
        Items().ApplyFilter(text, filters, {});   // FUN_101a4170
        g_pageSize = size == 0 ? 0x12 : static_cast<uint32_t>(size);
        return 0;
    }

    int GetCustomStoreData(lua_State* L)   // FUN_101a6560: (page) -> {item ids}
    {
        uint32_t page;
        if (!ReadNumber(L, page) || page == 0)
            return 0;
        const std::vector<const Item*>& f = Items().Filtered();
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        int n = 0;
        for (uint32_t i = g_pageSize * (page - 1); i < g_pageSize * page && i < f.size() && f[i]; ++i)
        {
            AscLua::lua_pushnumber(L, ++n);
            AscLua::lua_pushinteger(L, static_cast<int>(f[i]->id));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GetCustomStoreMaxPages(lua_State* L)   // ceil(filtered / page size)
    {
        const float n = static_cast<float>(Items().Size());
        AscLua::lua_pushnumber(L, static_cast<float>(std::ceil(n / static_cast<float>(g_pageSize))));
        return 1;
    }

    void PushIntArray(lua_State* L, const std::vector<uint32_t>& v)   // FUN_1009a460
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(v[i]));
            AscLua::lua_settable(L, -3);
        }
    }

    // FUN_101a66a0: item entry, money, {token items}, {token counts}, game event | nil, achievement | nil.
    int GetCustomStoreItemInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const Item* it = FindItem(id);
        if (!it)
        {
            for (int i = 0; i < 6; ++i)
                AscLua::lua_pushnil(L);
            return 6;
        }
        AscLua::lua_pushinteger(L, static_cast<int>(it->item));
        AscLua::lua_pushinteger(L, static_cast<int>(it->money));
        std::vector<uint32_t> tokens, counts;
        for (uint32_t k = 0; k < 5; ++k)
            if (it->tokens[k] && it->counts[k])
            {
                tokens.push_back(it->tokens[k]);
                counts.push_back(it->counts[k]);
            }
        PushIntArray(L, tokens);
        PushIntArray(L, counts);
        if (it->gameEvent)
            AscLua::lua_pushinteger(L, static_cast<int>(it->gameEvent));
        else
            AscLua::lua_pushnil(L);
        if (it->achievement)
            AscLua::lua_pushinteger(L, static_cast<int>(it->achievement));
        else
            AscLua::lua_pushnil(L);
        return 6;
    }

    int GetCustomStoreTypeInfo(lua_State* L)   // (the second string, the first | nil)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const Type* t = FindType(id);
        if (!t)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 2;
        }
        PushStr(L, t->second.c_str());
        if (t->first.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, t->first.c_str());
        return 2;
    }

    template <size_t N>
    int PushCode(lua_State* L, uint32_t code, const char* const (&names)[N])
    {
        PushBool(L, code == 0);
        if (code == 0)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, code < N ? names[code] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(code)).c_str());
        return 2;
    }

    // FUN_101a63a0: some loaded item must carry the id (else BAD_STORE), then the store itself.
    int CanQueryCustomStore(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        bool any = false;
        for (const Item& i : g_items)
            any = any || i.id == id;
        uint32_t code = 2;
        if (any)
        {
            const Type* t = FindType(id);
            if (!t)
                code = 3;
            else if (t->creature && !Nearby(t->creature, false))
                code = 4;
            else if (t->gameObject && !Nearby(t->gameObject, true))
                code = 5;
            else
                code = 0;
        }
        return PushCode(L, code, kQuery);
    }

    int CanPurchaseCustomStoreItem(lua_State* L)   // FUN_101a6220: (item, count)
    {
        int32_t id, count;
        if (!ReadInt2(L, id, count))
            return 0;
        return PushCode(L, CanPurchase(static_cast<uint32_t>(id), static_cast<uint32_t>(count)), kPurchase);
    }

    int QueryCustomStore(lua_State* L)   // FUN_101a6c00
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Packet(0x6B9).U32(id).Send();
        PushBool(L, true);
        return 1;
    }

    int PurchaseCustomStoreItem(lua_State* L)   // FUN_101a6af0
    {
        int32_t id, count;
        if (!ReadInt2(L, id, count))
            return 0;
        Packet(0x6BB).U32(static_cast<uint32_t>(id)).U32(static_cast<uint32_t>(count)).Send();
        PushBool(L, true);
        return 1;
    }

    int IsItemLockedDueToAchievement(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const Item* it = FindItem(id);
        PushBool(L, it && it->achievement && !Completed(it->achievement));
        return 1;
    }

    int IsItemLockedDueToGameEvent(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const Item* it = FindItem(id);
        PushBool(L, it && it->gameEvent && !AscGameEvents::IsActive(static_cast<uint16_t>(it->gameEvent)));
        return 1;
    }

    // ---- SMSG -------------------------------------------------------------------------------------
    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    std::string Str(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    // 0x6BA (FUN_101a5590): str result; on QUERY_CUSTOM_STORE_OK, u32 n x 0x40-byte items.
    void __cdecl OnQueryResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string r = Str(p);
        g_items.clear();
        Items().Reset();
        int code = -1;
        for (int i = 0; i < 6; ++i)
            if (r == kQuery[i])
                code = i;
        if (code < 0)
            AscLog::Printf("Unexpected Value: %s", r.c_str());
        if (code == 0)
        {
            const uint32_t n = U32(p);
            g_items.reserve(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                uint32_t raw[16];
                memcpy(raw, p->m_buffer + p->m_read, 0x40);
                p->m_read += 0x40;
                Item it;
                it.id = raw[0];
                it.type = raw[1];
                it.item = raw[2];
                it.money = raw[3];
                memcpy(it.tokens, raw + 4, 20);
                memcpy(it.counts, raw + 9, 20);
                it.gameEvent = raw[14];
                it.achievement = raw[15];
                g_items.push_back(it);
            }
        }
        AscRuntime::Signal("QUERY_CUSTOM_STORE_RESULT", "%s", r.c_str());
    }

    void __cdecl OnPurchaseResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6BC
    {
        const std::string r = Str(p);
        AscRuntime::Signal("PURCHASE_CUSTOM_STORE_ITEM_RESULT", "%s", r.c_str());
    }

    // 0x6BD (FUN_101a51a0): u32 n x {u32 id, u32 creature, u32 game object, str, str}.
    void __cdecl OnTypes(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_types.clear();
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            Type t;
            t.id = U32(p);
            t.creature = U32(p);
            t.gameObject = U32(p);
            t.first = Str(p);
            t.second = Str(p);
            g_types.push_back(t);
        }
    }

    void OnGlueScreen()   // 0x101A3EB0
    {
        g_types.clear();
        g_pageSize = 0x12;
        g_items.clear();
        Items().Reset();
    }

    void Init()
    {
        sDC.AddPacketHandler(0x6BA, CNetClientCustomPacket((void*)&OnQueryResult, nullptr));
        sDC.AddPacketHandler(0x6BC, CNetClientCustomPacket((void*)&OnPurchaseResult, nullptr));
        sDC.AddPacketHandler(0x6BD, CNetClientCustomPacket((void*)&OnTypes, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_CustomStore", "QueryCustomStore", QueryCustomStore},
        {"C_CustomStore", "PurchaseCustomStoreItem", PurchaseCustomStoreItem},
        {"C_CustomStore", "ApplyCustomStoreFilter", ApplyCustomStoreFilter},
        {"C_CustomStore", "GetCustomStoreData", GetCustomStoreData},
        {"C_CustomStore", "GetCustomStoreMaxPages", GetCustomStoreMaxPages},
        {"C_CustomStore", "GetCustomStoreItemInfo", GetCustomStoreItemInfo},
        {"C_CustomStore", "GetCustomStoreTypeInfo", GetCustomStoreTypeInfo},
        {"C_CustomStore", "CanQueryCustomStore", CanQueryCustomStore},
        {"C_CustomStore", "CanPurchaseCustomStoreItem", CanPurchaseCustomStoreItem},
        {"C_CustomStore", "IsItemLockedDueToGameEvent", IsItemLockedDueToGameEvent},
        {"C_CustomStore", "IsItemLockedDueToAchievement", IsItemLockedDueToAchievement},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}
