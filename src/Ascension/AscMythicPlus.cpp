// C_MythicPlus over MythicPlusMgr (FUN_100d52f0, static 0x10BDB68C), transcribed from
// E:\oniwow-artifacts\ghidra-ext\out\all\102bb*.c / 102bc*.c.
//
//   +0x04 set<u32> encounters killed      +0x14 u32 run instance id      +0x20 u32 timer (ms)
//   +0x0C u32 trash                        +0x18 u32 keystone key         +0x24 u32 current instance
//   +0x10 u32 champions dead               +0x1C u8  state                      (SMSG_INSTANCE_INFO)
//
//   SMSG 0x9C1 SMSG_INSTANCE_INFO (handler_0x09c1): u32 -> +0x24.
//   SMSG 0x908 DUNGEON_STATE_CHANGED (FUN_102bb630): u64 skipped, u8 state, u32 key, u32 instance.
//        A countdown (state 1) or a new instance resets the run (FUN_102bb390) and takes key and
//        instance. Then 1 -> MYTHIC_PLUS_COUNTDOWN_STARTED("%u", 10), 3 -> MYTHIC_PLUS_STARTED,
//        4/5 -> MYTHIC_PLUS_COMPLETE("%b", state != 5).
//   SMSG 0x909 PROGRESS_UPDATE (FUN_102bb850): u32 n x u32 encounter (newly seen ids fire
//        MYTHIC_PLUS_ENCOUNTER_UPDATE("%u"), except 0xBA5..0xBAA), u32 trash, u32 champions; a
//        change fires MYTHIC_PLUS_TRASH_UPDATE / _CHAMPIONS_UPDATE("%u%u", count, required).
//   SMSG 0x90A TIMER_UPDATE (FUN_102bbdb0): u32 ms; outside the countdown fires
//        MYTHIC_PLUS_TIMER_UPDATE("%f", seconds).
//
// "required" comes from TimedDungeons.dbc via the keystone record FUN_101d0500 looks up by key in the
// MemoryBridge MythicKeystones store (AscBridgeStore): {u32 key (the keystone item id), u32 dungeon id,
// u32 level}, loaded from MythicKeystones.dbc and patched by SMSG_PATCH_MYTHIC_KEYSTONE 0x56D
// (FUN_101e24d0, see AscBridgeStore::PatchKeystone).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscMythicPlus.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Ascension/RealmInfo.hpp>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <vector>

using namespace AscScript;

namespace
{
    struct Mgr
    {
        std::set<uint32_t> encounters;
        uint32_t trash = 0, champions = 0, instance = 0, key = 0;
        uint8_t state = 0;
        uint32_t timerMs = 0;
        uint32_t currentInstance = 0;

        void Reset()   // FUN_102bb390 (the current instance is not part of the run)
        {
            encounters.clear();
            trash = champions = instance = key = 0;
            state = 0;
            timerMs = 0;
        }
    };
    Mgr g_mgr;

    AscDbc::Table& TimedDungeons()         { return AscDbc::Get("DBFilesClient\\TimedDungeons.dbc"); }
    AscDbc::Table& DungeonEncounterExtra() { return AscDbc::Get("DBFilesClient\\DungeonEncounterExtra.dbc"); }
    AscDbc::Table& MythicAffixes()         { return AscDbc::Get("DBFilesClient\\MythicAffixes.dbc"); }

    // FUN_101d0500 on the keystone table (0x10BE03F0): {+0 key, +4 dungeon id, +8 level}.
    struct Keystone { uint32_t key, dungeonId, level; };
    const Keystone* KeystoneRecord(uint32_t key)
    {
        static Keystone k;
        uint32_t rec[3];
        if (!AscBridgeStore::KeystoneById(key, rec))
            return nullptr;
        k = {rec[0], rec[1], rec[2]};
        return &k;
    }
    const uint8_t* TimedDungeonFor(const Keystone* k) { return k ? TimedDungeons().Row(k->dungeonId) : nullptr; }

    // TimedDungeons row +off for the active keystone's dungeon, 0 when either lookup fails.
    uint32_t Required(uint32_t off)
    {
        const Keystone* k = KeystoneRecord(g_mgr.key);
        if (!k)
            return 0;
        const uint8_t* row = TimedDungeons().Row(k->dungeonId);
        return row ? AscDbc::Table::U32(row, off) : 0;
    }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    void __cdecl OnPatchKeystone(void*, uint32_t, uint32_t, CDataStore* p)   // 0x56D
    {
        uint32_t rec[3];
        rec[0] = Read<uint32_t>(p);
        rec[1] = Read<uint32_t>(p);
        rec[2] = Read<uint32_t>(p);
        AscBridgeStore::PatchKeystone(rec);
    }

    void __cdecl OnInstanceInfo(void*, uint32_t, uint32_t, CDataStore* p)   // 0x9C1
    {
        g_mgr.currentInstance = Read<uint32_t>(p);
    }

    void __cdecl OnStateChanged(void*, uint32_t, uint32_t, CDataStore* p)   // 0x908
    {
        p->m_read += 8;
        const uint8_t state = Read<uint8_t>(p);
        const uint32_t key = Read<uint32_t>(p);
        const uint32_t instance = Read<uint32_t>(p);
        if (state == 1 || g_mgr.instance != instance)
        {
            g_mgr.Reset();
            g_mgr.instance = instance;
            g_mgr.key = key;
        }
        g_mgr.state = state;
        switch (state)
        {
        case 1: AscRuntime::Signal("MYTHIC_PLUS_COUNTDOWN_STARTED", "%u", 10u); break;
        case 3: AscRuntime::Signal("MYTHIC_PLUS_STARTED"); break;
        case 4:
        case 5: AscRuntime::Signal("MYTHIC_PLUS_COMPLETE", "%b", state != 5 ? 1 : 0); break;
        default: break;
        }
    }

    void __cdecl OnProgressUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x909
    {
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const uint32_t id = Read<uint32_t>(p);
            if (!g_mgr.encounters.insert(id).second)
                continue;
            if (id >= 0xBA5 && id <= 0xBAA)
                continue;
            AscRuntime::Signal("MYTHIC_PLUS_ENCOUNTER_UPDATE", "%u", id);
        }
        const uint32_t trash = Read<uint32_t>(p);
        if (g_mgr.trash != trash)
        {
            g_mgr.trash = trash;
            AscRuntime::Signal("MYTHIC_PLUS_TRASH_UPDATE", "%u%u", g_mgr.trash, Required(4));
        }
        const uint32_t champions = Read<uint32_t>(p);
        if (g_mgr.champions != champions)
        {
            g_mgr.champions = champions;
            AscRuntime::Signal("MYTHIC_PLUS_CHAMPIONS_UPDATE", "%u%u", g_mgr.champions, Required(0xC));
        }
    }

    void __cdecl OnTimerUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x90A
    {
        g_mgr.timerMs = Read<uint32_t>(p);
        if (g_mgr.state != 1)
            AscRuntime::Signal("MYTHIC_PLUS_TIMER_UPDATE", "%f",
                               static_cast<double>(static_cast<float>(static_cast<int32_t>(g_mgr.timerMs)) / 1000.0f));
    }

    // FUN_102bb0c0(L, level): this week's MythicAffixes row for `level` (week = yday / 7, UTC).
    // With no match the scan leaves the LAST row in hand, and the original uses it.
    void PushAffixes(lua_State* L, uint32_t level)
    {
        const __time64_t now = _time64(nullptr);
        tm t{};
        _gmtime64_s(&t, &now);
        const int32_t week = static_cast<int32_t>(static_cast<float>(t.tm_yday) / 7.0f);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscDbc::Table& tbl = MythicAffixes();
        const uint8_t* row = nullptr;
        for (uint32_t id = tbl.MinId(); id < tbl.MaxId() + 1; ++id)
        {
            row = tbl.Row(id);
            if (row && AscDbc::Table::U32(row, 8) == level && AscDbc::Table::I32(row, 4) == week)
                break;
        }
        if (!row)
            return;
        std::vector<uint32_t> ids;
        auto add = [&](uint32_t off) { if (const uint32_t v = AscDbc::Table::U32(row, off)) ids.push_back(v); };
        add(0x1C);
        add(0x2C);
        add(0x0C);
        add(0x30);
        add(0x34);
        for (uint32_t i = 1; i < 4; ++i) add(0x1C + i * 4);
        add(0x38);
        for (uint32_t i = 1; i < 4; ++i) add(0x0C + i * 4);
        for (uint32_t i = 0; i < ids.size(); ++i)
        {
            AscLua::lua_pushinteger(L, static_cast<int>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(ids[i]));
            AscLua::lua_settable(L, -3);
        }
    }

    int ActivateKeystone(lua_State*)
    {
        Packet(0x527).Send();
        return 0;
    }

    int GetCurrentAffixes(lua_State* L)
    {
        PushAffixes(L, 0xFE);
        return 1;
    }

    // Encounters whose DungeonEncounterExtra row names this map in +0xC or +0x8.
    int GetMapEncounters(lua_State* L)
    {
        const int32_t map = ToInt(CheckNumber(L, 1));
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscDbc::Table& tbl = DungeonEncounterExtra();
        int32_t n = 1;
        for (uint32_t id = tbl.MinId(); id < tbl.MaxId() + 1; ++id)
        {
            if (id >= 0xBA5 && id <= 0xBAA)
                continue;
            const uint8_t* row = tbl.Row(id);
            if (!row)
                continue;
            const int32_t a = AscDbc::Table::I32(row, 0xC), b = AscDbc::Table::I32(row, 8);
            if ((a != 0 && a == map) || (b != 0 && b == map))
            {
                AscLua::lua_pushinteger(L, n++);
                AscLua::lua_pushinteger(L, static_cast<int>(AscDbc::Table::U32(row, 0)));
                AscLua::lua_settable(L, -3);
            }
        }
        return 1;
    }

    int GetMapFinalEncounter(lua_State* L)
    {
        const int32_t map = ToInt(CheckNumber(L, 1));
        AscDbc::Table& tbl = DungeonEncounterExtra();
        uint32_t found = 0;
        for (uint32_t id = tbl.MinId(); id < tbl.MaxId() + 1; ++id)
        {
            const uint8_t* row = tbl.Row(id);
            if (row && AscDbc::Table::I32(row, 0xC) != 0 && AscDbc::Table::I32(row, 0xC) == map)
            {
                found = AscDbc::Table::U32(row, 0);
                break;
            }
        }
        AscLua::lua_pushinteger(L, static_cast<int>(found));
        return 1;
    }

    // Active: the run is in this instance and counting down, running or complete.
    int IsKeystoneActive(lua_State* L)
    {
        const uint8_t s = g_mgr.state;
        PushBool(L, g_mgr.instance == g_mgr.currentInstance && (s == 1 || s == 3 || s == 4 || s == 5));
        return 1;
    }

    const Keystone* ActiveKeystone() { return KeystoneRecord(g_mgr.key); }
    uint32_t RowU32(const uint8_t* row, uint32_t off) { return row ? AscDbc::Table::U32(row, off) : 0; }
    float RowSeconds(const uint8_t* row, uint32_t off)
    {
        return row ? static_cast<float>(AscDbc::Table::I32(row, off)) / 1000.0f : 0.0f;
    }
    float RewardMultiplier(const uint8_t* timed) { return timed ? AscDbc::Table::F32(timed, 0x14) : 1.0f; }
    void SetInt(lua_State* L, const char* k, uint32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushinteger(L, static_cast<int>(v));
        AscLua::lua_settable(L, -3);
    }
    void SetNum(lua_State* L, const char* k, float v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushnumber(L, static_cast<double>(v));
        AscLua::lua_settable(L, -3);
    }

    int GetActiveKeystoneChampions(lua_State* L)
    {
        const uint8_t* timed = TimedDungeonFor(ActiveKeystone());
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "championsDead", g_mgr.champions);
        SetInt(L, "championsRequired", RowU32(timed, 0xC));
        return 1;
    }

    int GetActiveKeystoneTrash(lua_State* L)
    {
        const uint8_t* timed = TimedDungeonFor(ActiveKeystone());
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "trashDead", g_mgr.trash);
        SetInt(L, "trashRequired", RowU32(timed, 4));
        return 1;
    }

    // Completed = encounters seen, not counting the dungeon's final one (the first
    // DungeonEncounterExtra row naming the dungeon in +0xC); required = TimedDungeons +8, minus one.
    int GetActiveKeystoneEncounters(lua_State* L)
    {
        const Keystone* k = ActiveKeystone();
        const uint8_t* timed = TimedDungeonFor(k);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        const uint32_t dungeon = k ? k->dungeonId : 0;
        uint32_t last = 0;
        AscDbc::Table& tbl = DungeonEncounterExtra();
        for (uint32_t id = tbl.MinId(); id < tbl.MaxId() + 1; ++id)
        {
            const uint8_t* row = tbl.Row(id);
            if (row && AscDbc::Table::U32(row, 0xC) != 0 && AscDbc::Table::U32(row, 0xC) == dungeon)
            {
                last = AscDbc::Table::U32(row, 0);
                break;
            }
        }
        const uint32_t seen = static_cast<uint32_t>(g_mgr.encounters.size());
        SetInt(L, "encountersCompleted", g_mgr.encounters.count(last) ? seen - 1 : seen);
        const uint32_t req = RowU32(timed, 8);
        SetInt(L, "encountersRequired", req ? req - 1 : 0);
        return 1;
    }

    int GetActiveKeystoneInfo(lua_State* L)
    {
        const Keystone* k = ActiveKeystone();
        const uint8_t* timed = TimedDungeonFor(k);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "keystoneLevel", k ? k->level : 0);
        SetInt(L, "dungeonID", k ? k->dungeonId : 0);
        AscLua::lua_pushstring(L, "activeAffixes");
        PushAffixes(L, k ? k->level : 0);
        AscLua::lua_settable(L, -3);
        SetNum(L, "rewardMultiplier", RewardMultiplier(timed));
        return 1;
    }

    // Elapsed seconds, then the dungeon's time limit.
    int GetActiveKeystoneTime(lua_State* L)
    {
        const uint8_t* timed = TimedDungeonFor(ActiveKeystone());
        AscLua::lua_pushnumber(L, static_cast<double>(static_cast<float>(static_cast<int32_t>(g_mgr.timerMs)) / 1000.0f));
        AscLua::lua_pushnumber(L, static_cast<double>(RowSeconds(timed, 0x10)));
        return 2;
    }

    // GetKeystoneInfo(key): scaling is the first MythicPlusScaling row for the keystone's level
    // whose +8 is this realm's ruleset (realm-info object +8).
    int GetKeystoneInfo(lua_State* L)
    {
        const uint32_t key = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const Keystone* k = KeystoneRecord(key);
        const uint8_t* timed = TimedDungeonFor(k);
        const uint8_t* scaling = nullptr;
        if (k)
        {
            AscDbc::Table& tbl = AscDbc::Get("DBFilesClient\\MythicPlusScaling.dbc");
            const uint32_t ruleset = RealmInfoSvc::Get().ruleset;
            for (uint32_t id = tbl.MinId(); id < tbl.MaxId() + 1 && !scaling; ++id)
            {
                const uint8_t* row = tbl.Row(id);
                if (row && AscDbc::Table::U32(row, 4) == k->level && AscDbc::Table::U32(row, 8) == ruleset)
                    scaling = row;
            }
        }
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "keystoneLevel", k ? k->level : 0);
        SetInt(L, "dungeonID", k ? k->dungeonId : 0);
        SetInt(L, "healthPercentage", RowU32(scaling, 0x18));
        SetInt(L, "magicDamagePercentage", RowU32(scaling, 0xC));
        SetInt(L, "physicalDamagePercentage", RowU32(scaling, 0x14));
        SetNum(L, "timeLimit", RowSeconds(timed, 0x10));
        AscLua::lua_pushstring(L, "affixIDs");
        PushAffixes(L, k ? k->level : 0);
        AscLua::lua_settable(L, -3);
        SetNum(L, "rewardMultiplier", RewardMultiplier(timed));
        return 1;
    }

    // A keystone: an item with display 67762 (Item.dbc +0x14) that the keystone table knows.
    int IsItemKeystone(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return AscLua::luaL_error(L, "Usage: IsItemKeystone(itemId)");
        const uint32_t item = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint8_t* row = ClientDbcRow(kItemDbc, item);
        PushBool(L, row && *reinterpret_cast<const uint32_t*>(row + 0x14) == 0x108B2 && KeystoneRecord(item));
        return 1;
    }

    void ResetRun() { g_mgr.Reset(); }   // FUN_102bb0b0 (every glue screen) -> FUN_102bb390

    void Init()
    {
        AscRuntime::OnGlueScreen(&ResetRun);
        sDC.AddPacketHandler(0x56D, CNetClientCustomPacket((void*)&OnPatchKeystone, nullptr));
        sDC.AddPacketHandler(0x908, CNetClientCustomPacket((void*)&OnStateChanged, nullptr));
        sDC.AddPacketHandler(0x909, CNetClientCustomPacket((void*)&OnProgressUpdate, nullptr));
        sDC.AddPacketHandler(0x90A, CNetClientCustomPacket((void*)&OnTimerUpdate, nullptr));
        sDC.AddPacketHandler(0x9C1, CNetClientCustomPacket((void*)&OnInstanceInfo, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_MythicPlus", "ActivateKeystone", ActivateKeystone},
        {"C_MythicPlus", "GetActiveKeystoneChampions", GetActiveKeystoneChampions},
        {"C_MythicPlus", "GetActiveKeystoneEncounters", GetActiveKeystoneEncounters},
        {"C_MythicPlus", "GetActiveKeystoneInfo", GetActiveKeystoneInfo},
        {"C_MythicPlus", "GetActiveKeystoneTime", GetActiveKeystoneTime},
        {"C_MythicPlus", "GetActiveKeystoneTrash", GetActiveKeystoneTrash},
        {"C_MythicPlus", "GetCurrentAffixes", GetCurrentAffixes},
        {"C_MythicPlus", "GetKeystoneInfo", GetKeystoneInfo},
        {"C_MythicPlus", "IsItemKeystone", IsItemKeystone},
        {"C_MythicPlus", "GetMapEncounters", GetMapEncounters},
        {"C_MythicPlus", "GetMapFinalEncounter", GetMapFinalEncounter},
        {"C_MythicPlus", "IsKeystoneActive", IsKeystoneActive},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

bool AscMythicPlus::EncounterKilled(uint32_t encounter) { return g_mgr.encounters.count(encounter) != 0; }
