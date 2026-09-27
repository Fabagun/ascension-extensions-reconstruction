// C_Callboard's view (CallboardViewMgr, static 0x10BDD1E8 via FUN_10119470: +4 categories, +0x10
// loaded). C_Callboard.RequestView (CMSG 0x55F) lives in AscBindings.cpp.
//
// Module init FUN_10119420: SMSG 0x589 / 0x58A, the library, CALLBOARD_VIEW_UPDATED and
// CALLBOARD_VIEW_REQUEST_RESULT.
//   SMSG 0x589 (FUN_101194e0): u32 n x Category -- replaces the view, marks it loaded,
//        CALLBOARD_VIEW_UPDATED.
//   SMSG 0x58A (FUN_101197c0): str -- CALLBOARD_VIEW_REQUEST_RESULT(str).
// Category (100 bytes, FUN_10118240): str ContentType, str Name, u32 RemainingCompletions,
//   u32 MoneyCost, u32 TokenCost, Reward, u32 n x SubCategory.
// SubCategory (0x5C, FUN_101187e0): str Type, str Name, u32 RemainingCompletions, u32 MoneyCost,
//   u32 TokenCost, u32 MinimumLevel, Reward.
// Reward (FUN_10116af0): u32 RewardMoney, RewardHonor, RewardArenaPoints, CallboardCachePoints,
//   u32 n x {u32 ItemID, u32 Amount} (FUN_10117120).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace AscScript;

namespace
{
    struct Reward
    {
        uint32_t money = 0, honor = 0, arena = 0, cachePoints = 0;
        std::vector<std::pair<uint32_t, uint32_t>> items;   // {ItemID, Amount}
    };
    struct SubCategory
    {
        std::string type, name;
        uint32_t remaining = 0, moneyCost = 0, tokenCost = 0, minimumLevel = 0;
        Reward reward;
    };
    struct Category
    {
        std::string contentType, name;
        uint32_t remaining = 0, moneyCost = 0, tokenCost = 0;
        Reward reward;
        std::vector<SubCategory> subCategories;
    };

    std::vector<Category> g_view;   // +4
    bool g_loaded = false;          // +0x10

    uint32_t ReadU32(CDataStore* p)
    {
        uint32_t v;
        memcpy(&v, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        return v;
    }
    std::string ReadStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }
    Reward ReadReward(CDataStore* p)
    {
        Reward r;
        r.money = ReadU32(p);
        r.honor = ReadU32(p);
        r.arena = ReadU32(p);
        r.cachePoints = ReadU32(p);
        for (uint32_t n = ReadU32(p); n; --n)
        {
            const uint32_t item = ReadU32(p);
            r.items.emplace_back(item, ReadU32(p));
        }
        return r;
    }

    void __cdecl OnView(void*, uint32_t, uint32_t, CDataStore* p)   // 0x589
    {
        std::vector<Category> view;
        for (uint32_t n = ReadU32(p); n; --n)
        {
            Category c;
            c.contentType = ReadStr(p);
            c.name = ReadStr(p);
            c.remaining = ReadU32(p);
            c.moneyCost = ReadU32(p);
            c.tokenCost = ReadU32(p);
            c.reward = ReadReward(p);
            for (uint32_t m = ReadU32(p); m; --m)
            {
                SubCategory s;
                s.type = ReadStr(p);
                s.name = ReadStr(p);
                s.remaining = ReadU32(p);
                s.moneyCost = ReadU32(p);
                s.tokenCost = ReadU32(p);
                s.minimumLevel = ReadU32(p);
                s.reward = ReadReward(p);
                c.subCategories.push_back(std::move(s));
            }
            view.push_back(std::move(c));
        }
        g_view = std::move(view);
        g_loaded = true;
        AscRuntime::Signal("CALLBOARD_VIEW_UPDATED");
    }

    void __cdecl OnRequestResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x58A
    {
        const std::string result = ReadStr(p);
        AscRuntime::Signal("CALLBOARD_VIEW_REQUEST_RESULT", "%s", result.c_str());
    }

    // ---- Lua writers (arrays: createtable(0, n), keys lua_pushnumber(i)) ---------------------------
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        PushStr(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    void SetU32(lua_State* L, const char* k, uint32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushinteger(L, static_cast<int>(v));
        AscLua::lua_settable(L, -3);
    }

    void PushReward(lua_State* L, const Reward& r)   // FUN_10118030 (table form) -> FUN_101186d0
    {
        AscLua::lua_createtable(L, 0, 5);
        AscLua::lua_checkstack(L, 2);
        SetU32(L, "RewardMoney", r.money);
        SetU32(L, "RewardHonor", r.honor);
        SetU32(L, "RewardArenaPoints", r.arena);
        SetU32(L, "CallboardCachePoints", r.cachePoints);
        AscLua::lua_pushstring(L, "Items");   // FUN_10118110
        AscLua::lua_createtable(L, 0, static_cast<int>(r.items.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < r.items.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_createtable(L, 0, 2);
            AscLua::lua_checkstack(L, 2);
            SetU32(L, "ItemID", r.items[i].first);
            SetU32(L, "Amount", r.items[i].second);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    void PushCategory(lua_State* L, const Category& c)   // FUN_101184d0
    {
        SetStr(L, "ContentType", c.contentType);
        SetStr(L, "Name", c.name);
        SetU32(L, "RemainingCompletions", c.remaining);
        SetU32(L, "MoneyCost", c.moneyCost);
        SetU32(L, "TokenCost", c.tokenCost);
        AscLua::lua_pushstring(L, "Rewards");
        PushReward(L, c.reward);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "SubCategories");   // FUN_10117e70
        AscLua::lua_createtable(L, 0, static_cast<int>(c.subCategories.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < c.subCategories.size(); ++i)
        {
            const SubCategory& s = c.subCategories[i];
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_createtable(L, 0, 7);   // FUN_10118a40
            AscLua::lua_checkstack(L, 2);
            SetStr(L, "Type", s.type);
            SetStr(L, "Name", s.name);
            SetU32(L, "RemainingCompletions", s.remaining);
            SetU32(L, "MoneyCost", s.moneyCost);
            SetU32(L, "TokenCost", s.tokenCost);
            SetU32(L, "MinimumLevel", s.minimumLevel);
            AscLua::lua_pushstring(L, "Rewards");
            PushReward(L, s.reward);
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
    }

    // FUN_10119980: nil until the first view arrives, else {Categories = [...]}.
    int GetView(lua_State* L)
    {
        if (!g_loaded)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_createtable(L, 0, 1);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "Categories");   // FUN_10117cb0
        AscLua::lua_createtable(L, 0, static_cast<int>(g_view.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < g_view.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_createtable(L, 0, 7);
            AscLua::lua_checkstack(L, 2);
            PushCategory(L, g_view[i]);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        return 1;
    }

    void Init()   // FUN_10119420
    {
        sDC.AddPacketHandler(0x589, CNetClientCustomPacket((void*)&OnView, nullptr));
        sDC.AddPacketHandler(0x58A, CNetClientCustomPacket((void*)&OnRequestResult, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Callboard", "GetView", GetView},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscCallboard
{
    // FUN_10337270 (C_TemporalContracts.ActivateAll*): nothing until the view is loaded; otherwise
    // every category (or, given a content type, those whose ContentType or Name equals it) hands each
    // sub-category with RemainingCompletions to fn once per completion. True when fn ran at all.
    bool ForEachCompletion(const std::string* contentType, void (*fn)(const std::string& type, void* ctx), void* ctx)
    {
        bool any = false;
        if (!g_loaded)
            return any;
        for (const Category& c : g_view)
        {
            if (contentType && c.contentType != *contentType && c.name != *contentType)
                continue;
            for (const SubCategory& s : c.subCategories)
                for (uint32_t i = 0; i < s.remaining; ++i)
                {
                    fn(s.type, ctx);
                    any = true;
                }
        }
        return any;
    }
}
