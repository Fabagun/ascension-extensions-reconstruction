// C_GroupFinder over GroupFinderMgr (FUN_10272490, static 0x10BE2770), transcribed from
// E:\oniwow-artifacts\ghidra-ext\out\all\1026d*..10274*.c. Tables: LFGActivities (+4 category,
// +8 min level, +0xC max level, +0x10 name, +0x14), LFGActivityCategories (+4 group type, +0xC),
// LFGActivityGroupType (+4 name, +8) -- patched by SMSG_PATCH_LFG_* 0x579..0x57B.
//
// Group record (manager +0x20 is the player's own listing, same layout; the listed groups are an
// unordered_map keyed by GroupID):
//   GroupID, ActivityIDs, MinItemLevel, NeedsTank/Healer/DPS, Title, Description, NewPlayerFriendly,
//   GroupLeader, LeaderName, MemberCount, GroupSize, LeaderClass, LeaderLevel, AutoAccept.
// CMSG 0x57C create / 0x57D update: u32 n x u32 activity, u32 min item level, u8 x3 roles,
//   u32-len title, u32-len description, u8, u8, u32 group size, u8 auto accept.
// CMSG 0x57E remove, 0x57F query (no body), 0x584 suggest invite {u32-len name},
// 0x59E request invite {u32-len group, u8, u32, u32-len, u32-len "CLASS_...", u8}.
// SMSG 0x580 create / 0x581 update / 0x582 remove: u32 result; 0 -> u32-len GroupID, else the
//   result's name. 0x583 query: the whole map (strings u32-len), GROUP_FINDER_LISTED_ACTIVITIES_UPDATED.
//   0x585 SUGGESTED_GROUP_INVITE("%s%s"), 0x587 REQUEST_GROUP_INVITE("%s").
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    struct Group
    {
        std::string id;
        std::vector<uint32_t> activities;
        uint32_t minItemLevel = 0;
        bool tank = false, healer = false, dps = false;
        std::string title, description;
        bool newPlayerFriendly = false, groupLeader = false;
        std::string leaderName;
        uint32_t memberCount = 0, groupSize = 0, leaderClass = 0, leaderLevel = 0;
        bool autoAccept = false;
    };

    Group g_own;                  // manager +0x20
    std::vector<Group> g_groups;  // the map, in insertion order

    AscDbc::Table& Activities()  { return AscDbc::Get("DBFilesClient\\LFGActivities.dbc"); }
    AscDbc::Table& Categories()  { return AscDbc::Get("DBFilesClient\\LFGActivityCategories.dbc"); }
    AscDbc::Table& GroupTypes()  { return AscDbc::Get("DBFilesClient\\LFGActivityGroupType.dbc"); }

    Group* FindGroup(const std::string& id)
    {
        for (Group& g : g_groups)
            if (g.id == id)
                return &g;
        return nullptr;
    }
    Group& UpsertGroup(const std::string& id)
    {
        if (Group* g = FindGroup(id))
            return *g;
        g_groups.emplace_back();
        g_groups.back().id = id;
        return g_groups.back();
    }

    uint32_t PlayerLevel()   // descriptor +0xD8 = UNIT_FIELD_LEVEL
    {
        uint8_t* p = ActivePlayer();
        return p ? *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0xD8) : 0;
    }
    const char* PlayerName()
    {
        uint8_t* p = ActivePlayer();
        if (!p)
            return "Unknown";
        typedef const char*(__thiscall* NameFn)(void*);
        return reinterpret_cast<NameFn>((*reinterpret_cast<void***>(p))[0xD8 / 4])(p);
    }

    // ---- wire -----------------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadSized(CDataStore* p)
    {
        const uint32_t n = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += static_cast<int32_t>(n);
        return s;
    }
    void PutSized(Packet& pk, const std::string& s)
    {
        pk.U32(static_cast<uint32_t>(s.size()));
        if (!s.empty())
            pk.Data(s.data(), static_cast<uint32_t>(s.size()));
    }

    // ---- Lua ------------------------------------------------------------------------------------
    void PushIdArray(lua_State* L, const std::vector<uint32_t>& v)   // FUN_1009a460
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
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        PushStr(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    void SetNum(lua_State* L, const char* k, uint32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushnumber(L, static_cast<double>(static_cast<int32_t>(v)));
        AscLua::lua_settable(L, -3);
    }
    void SetBool(lua_State* L, const char* k, bool v)
    {
        AscLua::lua_pushstring(L, k);
        PushBool(L, v);
        AscLua::lua_settable(L, -3);
    }

    void PushGroup(lua_State* L, const Group& g)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "GroupID", g.id);
        AscLua::lua_pushstring(L, "ActivityIDs");
        PushIdArray(L, g.activities);
        AscLua::lua_settable(L, -3);
        SetStr(L, "LeaderName", g.leaderName);
        SetStr(L, "Title", g.title);
        SetStr(L, "Description", g.description);
        SetNum(L, "MinItemLevel", g.minItemLevel);
        SetBool(L, "NeedsTank", g.tank);
        SetBool(L, "NeedsHealer", g.healer);
        SetBool(L, "NeedsDPS", g.dps);
        SetBool(L, "NewPlayerFriendly", g.newPlayerFriendly);
        SetNum(L, "MemberCount", g.memberCount);
        SetNum(L, "GroupSize", g.groupSize);
        SetBool(L, "GroupLeader", g.groupLeader);
        AscLua::lua_pushstring(L, "LeaderClass");   // ChrClasses +0x1C (FUN_100b13c0)
        const uint8_t* cls = ClientDbcRow(0xAD3404, g.leaderClass);
        const char* token = cls ? *reinterpret_cast<const char* const*>(cls + 0x1C) : "UNKNOWN";
        AscLua::lua_pushstring(L, token ? token : "");
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "LeaderLevel");
        AscLua::lua_pushinteger(L, static_cast<int>(g.leaderLevel));
        AscLua::lua_settable(L, -3);
        SetBool(L, "AutoAccept", g.autoAccept);
    }

    // FUN_1011f230 (lua_toarray): the numbers in a table, in lua_next order; any non-number value
    // logs and yields an empty list.
    std::vector<uint32_t> ToArray(lua_State* L, int idx)
    {
        std::vector<uint32_t> out;
        if (AscLua::lua_type(L, idx) != LUA_TTABLE)
        {
            AscLog::Printf("FrameScript::lua_toarray Value at index %d is not a table", idx);
            return out;
        }
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, idx))
        {
            if (!AscLua::lua_isnumber(L, -1))
            {
                AscLua::lua_settop(L, -3);
                AscLog::Printf("FrameScript::lua_toarray Invalid lua value type");
                return {};
            }
            out.push_back(static_cast<uint32_t>(ToInt(CheckNumber(L, -1))));
            AscLua::lua_settop(L, -2);
        }
        return out;
    }

    // ---- listings -------------------------------------------------------------------------------
    // FUN_1026d5e0: {table, number, bool x3, string x2, bool x2, number, bool}.
    int SendListing(lua_State* L, uint32_t opcode)
    {
        if (!ValidateInput(L, {TABLE, NUMBER, BOOLEAN, BOOLEAN, BOOLEAN, STRING, STRING, BOOLEAN, BOOLEAN, NUMBER, BOOLEAN}))
            return 0;
        const std::vector<uint32_t> activities = ToArray(L, 1);
        const uint32_t minItemLevel = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const bool tank = AscLua::lua_toboolean(L, 3) != 0, healer = AscLua::lua_toboolean(L, 4) != 0,
                   dps = AscLua::lua_toboolean(L, 5) != 0;
        const std::string title = CheckString(L, 6), description = CheckString(L, 7);
        const bool b8 = AscLua::lua_toboolean(L, 8) != 0, b9 = AscLua::lua_toboolean(L, 9) != 0;
        const uint32_t groupSize = static_cast<uint32_t>(ToInt(CheckNumber(L, 10)));
        const bool autoAccept = AscLua::lua_toboolean(L, 11) != 0;

        g_own.activities = activities;
        g_own.minItemLevel = minItemLevel;
        g_own.tank = tank;
        g_own.healer = healer;
        g_own.dps = dps;
        g_own.title = title;
        g_own.description = description;
        g_own.newPlayerFriendly = b8;
        g_own.groupLeader = b9;
        g_own.leaderName = PlayerName();
        const uint32_t a = *reinterpret_cast<const uint32_t*>(0xBD1998);
        const uint32_t b = *reinterpret_cast<const uint32_t*>(0xBD199C);
        g_own.memberCount = std::max(a, b ? b : 1u);
        g_own.leaderClass = 10;
        g_own.groupSize = groupSize;
        g_own.leaderLevel = PlayerLevel();
        g_own.autoAccept = autoAccept;

        Packet p(opcode);
        p.U32(static_cast<uint32_t>(activities.size()));
        for (uint32_t id : activities)
            p.U32(id);
        p.U32(minItemLevel).U8(tank).U8(healer).U8(dps);
        PutSized(p, title);
        PutSized(p, description);
        p.U8(b8).U8(b9).U32(groupSize).U8(autoAccept);
        p.Send();
        PushBool(L, true);
        return 1;
    }
    int CreateListing(lua_State* L) { return SendListing(L, 0x57C); }
    int UpdateListing(lua_State* L) { return SendListing(L, 0x57D); }

    int SendEmpty(lua_State* L, uint32_t opcode)
    {
        Packet(opcode).Send();
        PushBool(L, true);
        return 1;
    }
    int RemoveListing(lua_State* L) { return SendEmpty(L, 0x57E); }
    int QueryListedActivities(lua_State* L) { return SendEmpty(L, 0x57F); }

    // The two bare globals FUN_10274e60 registers after the C_GroupFinder table: one string, sent as
    // {u32 length, bytes}. SuggestInvite = FUN_10274610 (CMSG 0x584), the global RequestInvite =
    // FUN_10274490 (CMSG 0x586) -- not C_GroupFinder.RequestInvite below, which is a different function.
    int SendName(lua_State* L, uint32_t opcode)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        Packet p(opcode);
        PutSized(p, name);
        p.Send();
        PushBool(L, true);
        return 1;
    }
    int SuggestInvite(lua_State* L) { return SendName(L, 0x584); }
    int RequestInviteByName(lua_State* L) { return SendName(L, 0x586); }

    // FUN_1026d2a0: {string, number, number, string, string, number}; a class without "CLASS_" gets it.
    int RequestInvite(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, NUMBER, NUMBER, STRING, STRING, NUMBER}))
            return 0;
        const std::string group = CheckString(L, 1);
        const uint8_t b2 = static_cast<uint8_t>(ToInt(CheckNumber(L, 2)));
        const uint32_t n3 = static_cast<uint32_t>(ToInt(CheckNumber(L, 3)));
        const std::string s4 = CheckString(L, 4);
        std::string cls = CheckString(L, 5);
        const uint8_t b6 = static_cast<uint8_t>(ToInt(CheckNumber(L, 6)));
        if (!(cls.size() > 5 && cls.find("CLASS_") != std::string::npos))
            cls = "CLASS_" + cls;
        Packet p(0x59E);
        PutSized(p, group);
        p.U8(b2).U32(n3);
        PutSized(p, s4);
        PutSized(p, cls);
        p.U8(b6);
        p.Send();
        PushBool(L, true);
        return 1;
    }

    int GetListedGroupID(lua_State* L)
    {
        if (g_own.id.empty()) AscLua::lua_pushnil(L); else PushStr(L, g_own.id.c_str());
        return 1;
    }

    int GetGroupInfo(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        if (const Group* g = FindGroup(id)) PushGroup(L, *g); else AscLua::lua_pushnil(L);
        return 1;
    }

    // Groups for the given activities (all when the list is empty; always the player's own), whose
    // first activity's level range holds the player.
    int GetListedGroups(lua_State* L)
    {
        if (!ValidateInput(L, {TABLE}))
            return 0;
        const std::vector<uint32_t> filter = ToArray(L, 1);
        const uint32_t level = PlayerLevel();
        std::vector<const Group*> hits;
        for (const Group& g : g_groups)
        {
            bool include = filter.empty() || g.id == g_own.id;
            for (size_t i = 0; !include && i < g.activities.size(); ++i)
                include = std::find(filter.begin(), filter.end(), g.activities[i]) != filter.end();
            if (!include || g.activities.empty())
                continue;
            const uint8_t* row = Activities().Row(g.activities[0]);
            if (row && AscDbc::Table::U32(row, 8) <= level && level <= AscDbc::Table::U32(row, 0xC))
                hits.push_back(&g);
        }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < hits.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            PushGroup(L, *hits[i]);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // ---- DBC lists ------------------------------------------------------------------------------
    int GetActivityInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        if (const uint8_t* row = Activities().Row(id))
        {
            PushStr(L, Activities().Str(row, 0x10));
            AscLua::lua_pushnumber(L, AscDbc::Table::I32(row, 8));
            AscLua::lua_pushnumber(L, AscDbc::Table::I32(row, 0xC));
            AscLua::lua_pushnumber(L, AscDbc::Table::I32(row, 4));
            return 4;
        }
        PushStr(L, "");
        AscLua::lua_pushnumber(L, 0);
        AscLua::lua_pushnumber(L, 0);
        AscLua::lua_pushnumber(L, 0);
        return 4;
    }

    int GetGroupTypeInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = GroupTypes().Row(id);
        const char* name = row ? GroupTypes().Str(row, 4) : nullptr;
        PushStr(L, name ? name : "");
        return 1;
    }

    // The insertion-sort predicates FUN_1026df70 / FUN_1026e110 / FUN_1026e270, literally.
    bool ActivityBefore(uint32_t a, uint32_t b)
    {
        const uint8_t* A = Activities().Row(a);
        const uint8_t* B = Activities().Row(b);
        if (!A || !B)
            return false;
        auto f = [](const uint8_t* r, uint32_t i) { return AscDbc::Table::U32(r, i * 4); };
        const bool notBefore = f(A, 2) <= f(B, 2)
            && (f(A, 2) != f(B, 2)
                || ((f(B, 3) <= f(A, 3) && (f(A, 3) != f(B, 3) || f(B, 5) <= f(A, 5)))
                    && (f(A, 5) != f(B, 5) || f(B, 0) <= f(A, 0))));
        return !notBefore;
    }
    bool CategoryBefore(uint32_t a, uint32_t b)
    {
        const uint8_t* A = Categories().Row(a);
        const uint8_t* B = Categories().Row(b);
        if (!A || !B)
            return false;
        const uint32_t a3 = AscDbc::Table::U32(A, 0xC), b3 = AscDbc::Table::U32(B, 0xC);
        return !(b3 <= a3 && (a3 != b3 || b <= a));
    }
    bool GroupTypeBefore(uint32_t a, uint32_t b)
    {
        const uint8_t* A = GroupTypes().Row(a);
        const uint8_t* B = GroupTypes().Row(b);
        if (!A || !B)
            return false;
        const uint32_t a2 = AscDbc::Table::U32(A, 8), b2 = AscDbc::Table::U32(B, 8);
        return !(b2 <= a2 && (a2 != b2 || b <= a));
    }

    int GetAvailableActivities(lua_State* L)
    {
        uint32_t category;
        if (!ReadNumber(L, category))
            return 0;
        const uint32_t level = PlayerLevel();
        std::vector<uint32_t> ids;
        AscDbc::Table& t = Activities();
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (row && AscDbc::Table::U32(row, 4) == category && AscDbc::Table::U32(row, 8) <= level
                && level <= AscDbc::Table::U32(row, 0xC))
                ids.push_back(id);
        }
        std::stable_sort(ids.begin(), ids.end(), ActivityBefore);
        PushIdArray(L, ids);
        return 1;
    }

    int GetAvailableCategories(lua_State* L)
    {
        uint32_t type;
        if (!ReadNumber(L, type))
            return 0;
        std::vector<uint32_t> ids;
        AscDbc::Table& t = Categories();
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (row && AscDbc::Table::U32(row, 4) == type)
                ids.push_back(id);
        }
        std::stable_sort(ids.begin(), ids.end(), CategoryBefore);
        PushIdArray(L, ids);
        return 1;
    }

    int GetAvailableGroupTypes(lua_State* L)
    {
        std::vector<uint32_t> ids;
        AscDbc::Table& t = GroupTypes();
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
            if (t.Row(id))
                ids.push_back(id);
        std::stable_sort(ids.begin(), ids.end(), GroupTypeBefore);
        PushIdArray(L, ids);
        return 1;
    }

    // ---- packets --------------------------------------------------------------------------------
    template <uint32_t N>
    const char* ResultName(uint32_t r, const char* const (&names)[N], std::string& scratch)
    {
        if (r < N)
            return names[r];
        scratch = "UNEXPECTED_ENUM_VALUE_" + std::to_string(r);
        return scratch.c_str();
    }

    void __cdecl OnCreateResponse(void*, uint32_t, uint32_t, CDataStore* p)   // 0x580
    {
        static const char* const kNames[5] = {"CREATE_LISTING_OK", "CREATE_LISTING_ALREADY_LISTED",
            "CREATE_LISTING_NOT_LEADER", "CREATE_LISTING_IN_GROUP", "CREATE_LISTING_INVALID_LISTING"};
        const uint32_t r = Read<uint32_t>(p);
        if (r == 0)
        {
            g_own.id = ReadSized(p);
            UpsertGroup(g_own.id) = g_own;
            AscRuntime::Signal("GROUP_FINDER_ACTIVITY_CREATED", "%s", g_own.id.c_str());
            return;
        }
        std::string s;
        AscRuntime::Signal("GROUP_FINDER_ACTIVITY_CREATION_FAILED", "%s", ResultName(r, kNames, s));
    }

    void __cdecl OnUpdateResponse(void*, uint32_t, uint32_t, CDataStore* p)   // 0x581
    {
        static const char* const kNames[5] = {"UPDATE_LISTING_OK", "UPDATE_LISTING_NOT_LISTED",
            "UPDATE_LISTING_NOT_LEADER", "UPDATE_LISTING_IN_GROUP", "UPDATE_LISTING_INVALID_LISTING"};
        const uint32_t r = Read<uint32_t>(p);
        if (r == 0)
        {
            g_own.id = ReadSized(p);
            UpsertGroup(g_own.id) = g_own;
            AscRuntime::Signal("GROUP_FINDER_ACTIVITY_UPDATED", "%s", g_own.id.c_str());
            return;
        }
        std::string s;
        AscRuntime::Signal("GROUP_FINDER_ACTIVITY_UPDATE_FAILED", "%s", ResultName(r, kNames, s));
    }

    void __cdecl OnRemoveResponse(void*, uint32_t, uint32_t, CDataStore* p)   // 0x582
    {
        static const char* const kNames[4] = {"REMOVE_LISTING_OK", "REMOVE_LISTING_NOT_LISTED",
            "REMOVE_LISTING_NOT_LEADER", "REMOVE_LISTING_IN_GROUP"};
        const uint32_t r = Read<uint32_t>(p);
        if (r == 0)
        {
            const std::string id = ReadSized(p);
            g_own.id.clear();
            g_groups.erase(std::remove_if(g_groups.begin(), g_groups.end(),
                                          [&](const Group& g) { return g.id == id; }), g_groups.end());
            AscRuntime::Signal("GROUP_FINDER_ACTIVITY_DELISTED", "%s", id.c_str());
            return;
        }
        std::string s;
        AscRuntime::Signal("GROUP_FINDER_ACTIVITY_DELIST_FAILED", "%s", ResultName(r, kNames, s));
    }

    void __cdecl OnQueryResponse(void*, uint32_t, uint32_t, CDataStore* p)   // 0x583
    {
        g_groups.clear();   // FUN_10274e00
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            Group g;
            g.id = ReadSized(p);
            for (uint32_t k = Read<uint32_t>(p); k; --k)
                g.activities.push_back(Read<uint32_t>(p));
            g.minItemLevel = Read<uint32_t>(p);
            g.tank = Read<uint8_t>(p) != 0;
            g.healer = Read<uint8_t>(p) != 0;
            g.dps = Read<uint8_t>(p) != 0;
            g.title = ReadSized(p);
            g.description = ReadSized(p);
            g.newPlayerFriendly = Read<uint8_t>(p) != 0;
            g.groupLeader = Read<uint8_t>(p) != 0;
            g.leaderName = ReadSized(p);
            g.memberCount = Read<uint32_t>(p);
            g.groupSize = Read<uint32_t>(p);
            g.leaderClass = Read<uint32_t>(p);
            g.leaderLevel = Read<uint32_t>(p);
            g.autoAccept = Read<uint8_t>(p) != 0;
            UpsertGroup(g.id) = g;
        }
        AscRuntime::Signal("GROUP_FINDER_LISTED_ACTIVITIES_UPDATED");
    }

    void __cdecl OnSuggestedInvite(void*, uint32_t, uint32_t, CDataStore* p)   // 0x585
    {
        const std::string first = ReadSized(p);
        const std::string second = ReadSized(p);
        AscRuntime::Signal("SUGGESTED_GROUP_INVITE", "%s%s", second.c_str(), first.c_str());
    }

    void __cdecl OnRequestInvite(void*, uint32_t, uint32_t, CDataStore* p)   // 0x587
    {
        const std::string name = ReadSized(p);
        AscRuntime::Signal("REQUEST_GROUP_INVITE", "%s", name.c_str());
    }

    // SMSG 0x59D (FUN_102719f0): sized name, u8, u32, sized string, sized class, u8. A class naming
    // "CLASS_" anywhere loses its first six characters. -> REQUEST_GROUP_FINDER_INVITE("%s%u%u%s%s%u").
    void __cdecl OnGroupFinderInvite(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string name = ReadSized(p);
        const uint32_t a = static_cast<uint8_t>(p->m_buffer[p->m_read++]);
        uint32_t b;
        memcpy(&b, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        const std::string c = ReadSized(p);
        std::string cls = ReadSized(p);
        const uint32_t d = static_cast<uint8_t>(p->m_buffer[p->m_read++]);
        if (cls.size() >= 6 && cls.find("CLASS_") != std::string::npos)
            cls = cls.substr(6);
        AscRuntime::Signal("REQUEST_GROUP_FINDER_INVITE", "%s%u%u%s%s%u", name.c_str(), a, b, c.c_str(), cls.c_str(), d);
    }

    // FUN_1026d0e0 (every glue screen): the listed groups cleared (FUN_10274e00) and the player's own
    // listing (+0x20) replaced by a default record (FUN_102704b0).
    void Reset()
    {
        g_groups.clear();
        g_own = Group{};
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&Reset);
        sDC.AddPacketHandler(0x59D, CNetClientCustomPacket((void*)&OnGroupFinderInvite, nullptr));
        sDC.AddPacketHandler(0x580, CNetClientCustomPacket((void*)&OnCreateResponse, nullptr));
        sDC.AddPacketHandler(0x581, CNetClientCustomPacket((void*)&OnUpdateResponse, nullptr));
        sDC.AddPacketHandler(0x582, CNetClientCustomPacket((void*)&OnRemoveResponse, nullptr));
        sDC.AddPacketHandler(0x583, CNetClientCustomPacket((void*)&OnQueryResponse, nullptr));
        sDC.AddPacketHandler(0x585, CNetClientCustomPacket((void*)&OnSuggestedInvite, nullptr));
        sDC.AddPacketHandler(0x587, CNetClientCustomPacket((void*)&OnRequestInvite, nullptr));
    }

    // C_GroupFinder.GetCategoryInfo(id) (0x10273100): LFGActivityCategories.dbc +8 name, +4 int; "" and 0
    // for an unknown id.
    int GetCategoryInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = Categories();
        if (const uint8_t* row = t.Row(id))
        {
            PushStr(L, t.Str(row, 8));
            PushInt(L, t.I32(row, 4));
            return 2;
        }
        PushStr(L, "");
        PushInt(L, 0);
        return 2;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_GroupFinder", "GetCategoryInfo", GetCategoryInfo},
        {"C_GroupFinder", "CreateListing", CreateListing},
        {"C_GroupFinder", "GetActivityInfo", GetActivityInfo},
        {"C_GroupFinder", "GetAvailableActivities", GetAvailableActivities},
        {"C_GroupFinder", "GetAvailableCategories", GetAvailableCategories},
        {"C_GroupFinder", "GetAvailableGroupTypes", GetAvailableGroupTypes},
        {"C_GroupFinder", "GetGroupInfo", GetGroupInfo},
        {"C_GroupFinder", "GetGroupTypeInfo", GetGroupTypeInfo},
        {"C_GroupFinder", "GetListedGroupID", GetListedGroupID},
        {"C_GroupFinder", "GetListedGroups", GetListedGroups},
        {"C_GroupFinder", "QueryListedActivities", QueryListedActivities},
        {"C_GroupFinder", "RemoveListing", RemoveListing},
        {"C_GroupFinder", "RequestInvite", RequestInvite},
        {"C_GroupFinder", "UpdateListing", UpdateListing},
        {nullptr, "SuggestInvite", SuggestInvite},
        {nullptr, "RequestInvite", RequestInviteByName},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
