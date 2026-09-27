// C_LootLockout over LootLockoutMgr (FUN_100d51b0 -> static 0x10BDB6B8): an unordered_map keyed by
// unit GUID of {u32 encounter index, u32 expiry} lists, plus the instance-bind list (+0x24, 12-byte
// {u32, u32 map, u32 difficulty}). Encounters are the client's DungeonEncounter (container 0xAD36B0:
// +0 id, +4 map, +8 difficulty, +0xC order, +0x14 name, +0x18 SpellIcon id).
//   0x9B4: u64 guid, u32 n x {encounter, expiry} -> all appended.
//   SMSG_ADD_LOOT_LOCKOUT 0x9B5: u64 guid, u32 encounter, u32 expiry -> appended.
//   SMSG_REMOVE_LOOT_LOCKOUT 0x9B6: u64 guid, u32 encounter -> first matching entry removed.
//   SMSG_QUERY_INSTANCE_BINDS_RESULT 0x6FE: str result; the bind list is cleared, and on
//     QUERY_INSTANCE_BINDS_OK refilled from u32 n x 12 bytes; QUERY_INSTANCE_BINDS_RESULT("%b%s", ok, result).
//   QueryInstanceBinds: CMSG 0x6FD. ResetInstanceDifficulty(map, difficulty): CMSG 0x58C {u32 map, u8 difficulty-1}.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLootLockout.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    const uint32_t kDungeonEncounter = 0xAD36B0;
    const uint32_t kMap = 0xAD4160;
    const uint32_t kSpellIcon = 0xAD488C;

    struct Lock { uint32_t encounter, expiry; };
    struct Bind { uint32_t unk, map, difficulty; };
    std::unordered_map<uint64_t, std::vector<Lock>> g_locks;
    std::vector<Bind> g_binds;

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    void __cdecl OnAddLockout(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint64_t guid = Read<uint64_t>(p);
        Lock l;
        l.encounter = Read<uint32_t>(p);
        l.expiry = Read<uint32_t>(p);
        g_locks[guid].push_back(l);
    }

    // SMSG 0x9B4 (FUN_10295020): u64 guid, u32 n, then n x {u32 encounter, u32 expiry}, each appended
    // (the list is not cleared first).
    void __cdecl OnAddLockouts(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint64_t guid = Read<uint64_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            Lock l;
            l.encounter = Read<uint32_t>(p);
            l.expiry = Read<uint32_t>(p);
            g_locks[guid].push_back(l);
        }
    }

    void __cdecl OnRemoveLockout(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint64_t guid = Read<uint64_t>(p);
        const uint32_t encounter = Read<uint32_t>(p);
        const auto it = g_locks.find(guid);
        if (it == g_locks.end())
            return;
        for (auto e = it->second.begin(); e != it->second.end(); ++e)
            if (e->encounter == encounter)
            {
                it->second.erase(e);
                break;
            }
    }

    void __cdecl OnInstanceBinds(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_binds.clear();
        const std::string result(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(result.size() + 1);
        const bool ok = result == "QUERY_INSTANCE_BINDS_OK";
        if (ok)
            for (uint32_t n = Read<uint32_t>(p); n; --n)
                g_binds.push_back(Read<Bind>(p));
        AscRuntime::Signal("QUERY_INSTANCE_BINDS_RESULT", "%b%s", static_cast<int>(ok), result.c_str());
    }

    uint64_t TokenGuid(const char* token)   // FUN_103086c0 -> 0x60C1C0
    {
        return reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x60C1C0)(token);
    }

    // FUN_102948d0: seconds left on the unit's lock for the encounter; 0 if none or expired.
    uint32_t TimeRemaining(uint64_t guid, uint32_t encounter)
    {
        const auto it = g_locks.find(guid);
        if (it == g_locks.end())
            return 0;
        for (const Lock& l : it->second)
            if (l.encounter == encounter)
            {
                const __time64_t now = _time64(nullptr);
                if ((now >> 32) != 0 || l.expiry <= static_cast<uint32_t>(now))
                    return 0;
                return l.expiry - static_cast<uint32_t>(now);
            }
        return 0;
    }

    // FUN_10294820: the SpellIcon path, or the question mark.
    std::string IconPath(uint32_t iconId)
    {
        if (const uint8_t* row = ClientDbcRow(kSpellIcon, iconId))
            return *reinterpret_cast<const char* const*>(row + 4);
        return "Interface\\Icons\\inv_misc_questionmark";
    }

    const char* EncounterName(const uint8_t* row)
    {
        return *reinterpret_cast<const char* const*>(row + 0x14);
    }
    uint32_t U32(const uint8_t* row, uint32_t off) { return *reinterpret_cast<const uint32_t*>(row + off); }

    bool Args(lua_State* L, int count)   // exact arity, arg 1 a string, the rest numbers
    {
        if (AscLua::lua_gettop(L) != count || !AscLua::lua_isstring(L, 1))
            return false;
        for (int i = 2; i <= count; ++i)
            if (!AscLua::lua_isnumber(L, i))
                return false;
        return true;
    }

    // FUN_10295220: name, map, difficulty+1, icon, order, time remaining.
    int GetEncounterData(lua_State* L)
    {
        if (!Args(L, 2))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string token = CheckString(L, 1);
        const uint32_t encounter = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const uint64_t guid = TokenGuid(token.c_str());
        const uint8_t* row = ClientDbcRow(kDungeonEncounter, encounter);
        if (!row)
            return PushNils(L, 6);   // the original returns 6 having pushed nothing
        PushStr(L, EncounterName(row));
        PushInt(L, static_cast<int32_t>(U32(row, 4)));
        PushInt(L, static_cast<int32_t>(U32(row, 8) + 1));
        PushStr(L, IconPath(U32(row, 0x18)).c_str());
        PushInt(L, static_cast<int32_t>(U32(row, 0xC)));
        PushInt(L, static_cast<int32_t>(TimeRemaining(guid, encounter)));
        return 6;
    }

    void SetStr(lua_State* L, const char* k, const char* v) { AscLua::lua_pushstring(L, k); PushStr(L, v); AscLua::lua_settable(L, -3); }
    void SetInt(lua_State* L, const char* k, int32_t v) { AscLua::lua_pushstring(L, k); PushInt(L, v); AscLua::lua_settable(L, -3); }

    // FUN_102953c0: { [encounter id] = {EncounterName, MapID, DifficultyID, SpellIconID, OrderIndex,
    // TimeRemaining} } for encounter indices 0 .. max-1 on the map and difficulty.
    int GetEncounterDatasForMapAndDifficulty(lua_State* L)
    {
        if (!Args(L, 3))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string token = CheckString(L, 1);
        const int32_t map = ToInt(CheckNumber(L, 2));
        const int32_t difficulty = ToInt(CheckNumber(L, 3));
        const uint32_t max = *reinterpret_cast<const uint32_t*>(kDungeonEncounter + 0x0C);
        const uint64_t guid = TokenGuid(token.c_str());
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < max; ++i)
        {
            const uint8_t* row = ClientDbcRow(kDungeonEncounter, i);
            if (!row || static_cast<int32_t>(U32(row, 4)) != map || static_cast<int32_t>(U32(row, 8)) != difficulty - 1)
                continue;
            PushInt(L, static_cast<int32_t>(U32(row, 0)));
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetStr(L, "EncounterName", EncounterName(row));
            SetInt(L, "MapID", static_cast<int32_t>(U32(row, 4)));
            SetInt(L, "DifficultyID", static_cast<int32_t>(U32(row, 8) + 1));
            SetStr(L, "SpellIconID", IconPath(U32(row, 0x18)).c_str());
            SetInt(L, "OrderIndex", static_cast<int32_t>(U32(row, 0xC)));
            SetInt(L, "TimeRemaining", static_cast<int32_t>(TimeRemaining(guid, i)));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_10295ee0: any encounter (min..max) on the map/difficulty still locked.
    int HasEncounterDatasForMapAndDifficulty(lua_State* L)
    {
        if (!Args(L, 3))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string token = CheckString(L, 1);
        const int32_t map = ToInt(CheckNumber(L, 2));
        const int32_t difficulty = ToInt(CheckNumber(L, 3));
        const uint64_t guid = TokenGuid(token.c_str());
        bool any = false;
        const uint32_t min = *reinterpret_cast<const uint32_t*>(kDungeonEncounter + 0x10);
        const uint32_t max = *reinterpret_cast<const uint32_t*>(kDungeonEncounter + 0x0C);
        for (uint32_t i = min; i < max + 1 && !any; ++i)
            if (const uint8_t* row = ClientDbcRow(kDungeonEncounter, i))
                if (static_cast<int32_t>(U32(row, 4)) == map && static_cast<int32_t>(U32(row, 8)) == difficulty - 1)
                    any = static_cast<int32_t>(TimeRemaining(guid, i)) > 0;
        PushBool(L, any);
        return 1;
    }

    // FUN_102956c0 (also GetUnitLootLockForEncounter).
    int GetLootLockoutTimeRemaining(lua_State* L)
    {
        if (!Args(L, 2))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string token = CheckString(L, 1);
        const uint32_t encounter = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        PushInt(L, static_cast<int32_t>(TimeRemaining(TokenGuid(token.c_str()), encounter)));
        return 1;
    }

    // FUN_102957d0: { [map] = { [difficulty] = { [encounter] = seconds left } } }, ordered maps; expired
    // locks still create their map/difficulty tables.
    int GetLootLockouts(lua_State* L)
    {
        if (!Args(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string token = CheckString(L, 1);
        std::map<uint32_t, std::map<uint32_t, std::set<std::pair<uint32_t, uint32_t>>>> tree;
        const auto it = g_locks.find(TokenGuid(token.c_str()));
        if (it != g_locks.end())
            for (const Lock& l : it->second)
                if (const uint8_t* row = ClientDbcRow(kDungeonEncounter, l.encounter))
                    tree[U32(row, 4)][U32(row, 8)].insert({l.encounter, l.expiry});
        const __time64_t now = _time64(nullptr);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const auto& m : tree)
        {
            PushInt(L, static_cast<int32_t>(m.first));
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            for (const auto& d : m.second)
            {
                PushInt(L, static_cast<int32_t>(d.first));
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                for (const auto& e : d.second)
                    if ((now >> 32) == 0 && static_cast<uint32_t>(now) < e.second)
                    {
                        PushInt(L, static_cast<int32_t>(e.first));
                        PushInt(L, static_cast<int32_t>(e.second - static_cast<uint32_t>(now)));
                        AscLua::lua_settable(L, -3);
                    }
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_10295d70: the DungeonEncounterExtra row whose +4 is the unit's entry.
    int GetUnitEncounterID(lua_State* L)
    {
        if (!Args(L, 1))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::string token = CheckString(L, 1);
        if (const uint8_t* unit = static_cast<const uint8_t*>(ObjectPtr(TokenGuid(token.c_str()), 8)))
        {
            const uint32_t entry = *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0xC);
            AscDbc::Table& t = AscDbc::Get("DBFilesClient\\DungeonEncounterExtra.dbc");
            for (uint32_t id = t.MinId(); t.Loaded() && id < t.MaxId() + 1; ++id)
                if (const uint8_t* row = t.Row(id))
                    if (AscDbc::Table::U32(row, 4) == entry)
                    {
                        PushInt(L, static_cast<int32_t>(AscDbc::Table::U32(row, 0)));
                        return 1;
                    }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_10296060: map ids of the binds whose map is (raid ? instance type 2 : not 2) at difficulty-1.
    int ListInstanceBinds(lua_State* L)
    {
        if (!ValidateInput(L, {BOOLEAN, NUMBER}))
            return 0;
        const bool raid = AscLua::lua_toboolean(L, 1) != 0;
        const int32_t difficulty = ToInt(CheckNumber(L, 2));
        if (difficulty == 0)
            return 0;
        std::vector<uint32_t> maps;
        for (const Bind& b : g_binds)
            if (const uint8_t* row = ClientDbcRow(kMap, b.map))
                if ((U32(row, 8) == 2) == raid && static_cast<int32_t>(b.difficulty) == difficulty - 1)
                    maps.push_back(b.map);
        AscLua::lua_createtable(L, 0, static_cast<int>(maps.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < maps.size(); ++i)
        {
            PushNum(L, i + 1);
            PushInt(L, static_cast<int32_t>(maps[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int QueryInstanceBinds(lua_State* L)
    {
        Packet(0x6FD).Send();
        PushBool(L, true);
        return 1;
    }

    int ResetInstanceDifficulty(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return 0;
        const uint32_t map = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint8_t difficulty = static_cast<uint8_t>(ToInt(CheckNumber(L, 2)));
        if (difficulty == 0)
            return 0;
        Packet(0x58C).U32(map).U8(static_cast<uint8_t>(difficulty - 1)).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_10294740 (every glue screen): the per-GUID lockouts (+4, FUN_102965d0) and the instance binds
    // (+0x24) emptied.
    void Reset()
    {
        g_locks.clear();
        g_binds.clear();
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&Reset);
        sDC.AddPacketHandler(0x9B4, CNetClientCustomPacket((void*)&OnAddLockouts, nullptr));
        sDC.AddPacketHandler(0x9B5, CNetClientCustomPacket((void*)&OnAddLockout, nullptr));
        sDC.AddPacketHandler(0x9B6, CNetClientCustomPacket((void*)&OnRemoveLockout, nullptr));
        sDC.AddPacketHandler(0x6FE, CNetClientCustomPacket((void*)&OnInstanceBinds, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_LootLockout", "GetEncounterData", GetEncounterData},
        {"C_LootLockout", "GetEncounterDatasForMapAndDifficulty", GetEncounterDatasForMapAndDifficulty},
        {"C_LootLockout", "HasEncounterDatasForMapAndDifficulty", HasEncounterDatasForMapAndDifficulty},
        {"C_LootLockout", "GetLootLockoutTimeRemaining", GetLootLockoutTimeRemaining},
        {"C_LootLockout", "GetUnitLootLockForEncounter", GetLootLockoutTimeRemaining},
        {"C_LootLockout", "GetLootLockouts", GetLootLockouts},
        {"C_LootLockout", "GetUnitEncounterID", GetUnitEncounterID},
        {"C_LootLockout", "ListInstanceBinds", ListInstanceBinds},
        {"C_LootLockout", "QueryInstanceBinds", QueryInstanceBinds},
        {"C_LootLockout", "ResetInstanceDifficulty", ResetInstanceDifficulty},
        // FUN_10296640 then registers the same ten functions again as bare globals, in this order.
        {nullptr, "GetLootLockouts", GetLootLockouts},
        {nullptr, "GetLootLockoutTimeRemaining", GetLootLockoutTimeRemaining},
        {nullptr, "GetEncounterData", GetEncounterData},
        {nullptr, "GetEncounterDatasForMapAndDifficulty", GetEncounterDatasForMapAndDifficulty},
        {nullptr, "GetUnitLootLockForEncounter", GetLootLockoutTimeRemaining},
        {nullptr, "GetUnitEncounterID", GetUnitEncounterID},
        {nullptr, "HasEncounterDatasForMapAndDifficulty", HasEncounterDatasForMapAndDifficulty},
        {nullptr, "ResetInstanceDifficulty", ResetInstanceDifficulty},
        {nullptr, "QueryInstanceBinds", QueryInstanceBinds},
        {nullptr, "ListInstanceBinds", ListInstanceBinds},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

uint32_t AscLootLockout::SecondsRemaining(uint64_t guid, uint32_t encounter) { return TimeRemaining(guid, encounter); }
