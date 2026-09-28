// C_Manastorm over ManastormMgr (FUN_102a4760, static 0x10BE31A8; module init 0x102A4540), transcribed
// from E:\oniwow-artifacts\ghidra-ext\out\all\102a*.c.
//
//   +0x00 vector<u32> levels completed this run (0x65E appends, ENTER_MANASTORM_OK clears)
//   +0x0C u32 active level      +0x14 u32 ActiveManastormType (index into kTypes)
//   +0x10 u32 active Manastorm.dbc id   +0x18 u32 / +0x1C float / +0x20 u32  cache info
//   +0x24 vector<vector<u32>> completed levels received before the player object existed; +0x84 set
//         -> moved into +0x30 for the active player by FUN_102a4ba0 (after 0x403340)
//   +0x30 unordered_map<u64 guid, vector<vector<u32>>> completed levels per ActiveManastormType,
//         each list sorted ascending (FUN_1011e1f0), so back() is the highest
//   +0x50 u32 chaotic link      +0x54 u32 spell of the loadout change in flight
//   +0x58 vector<u32> loadout (slot -> spell)
//   +0x64 unordered_map<u64 guid, unordered_map<u32 item, Reward>> reward visibility
//   0x10BE31A0 i64 ShowObjectiveIcon's time + 10 s (never compared with the clock: set = on)
//
//   SMSG 0x652 ENTER_RESULT (FUN_102a1a10)   C string -> ENTER_MANASTORM_RESULT("%s")
//   SMSG 0x666 LEAVE_RESULT (FUN_102a1d40)   C string -> LEAVE_MANASTORM_RESULT("%s")
//   SMSG 0x660 ACTIVE_DATA (FUN_102a1e90)    u32 level, u32 id, C string type, u32, float, u32; fires
//        ACTIVE_MANASTORM_UPDATED("%u%u", old, new) and MANASTORM_CACHE_INFO_UPDATED("%u%f%u"); entering
//        and leaving patch the client (below).
//   SMSG 0x65F DATA (FUN_102a2cf0)           the active player's completed levels
//   SMSG 0x65D UPDATE_MAX_COMPLETED_LEVEL (FUN_102a36f0)  u64 guid + the same lists
//   SMSG 0x65E COMPLETED_LEVEL (FUN_102a2990) u32 level -> MANASTORM_LEVEL_COMPLETED, LEVEL_UNLOCKED
//   SMSG 0x67B FAIL (FUN_102a2f40)           MANASTORM_FAILED
//   SMSG 0x67F CHAOTIC_LINK_UPDATE           u32 -> +0x50, MANASTORM_CHAOTIC_LINK_UPDATED("%u%u", old, new)
//   SMSG 0x688 / 0x68A / 0x68B               loadout: whole list / result of CMSG 0x689 / one slot
//   SMSG 0x6AD                               reward visibility for one guid
//   SMSG 0x67D / 0x67E                       PATCH_MANASTORM / _MESSAGES (0x68C / 0x68D, the two
//                                            modifier tables, are registered in AscPatchData.cpp)
//
// Entering (active level 0 -> non-zero) writes 0xEB over the client's jne at 0x80DB37 and clears the
// flags (+0x14) of SpellRange.dbc rows 114 and 445; leaving writes 0x75 back, sets those flags to 2 and
// restores the objective-icon scale (FUN_102a6c10(1.0)). The original goes through
// NtProtectVirtualMemory; VirtualProtect here.
//
// The (key, level) set CanEnter checks (map 0x10BE0658, FUN_10211020) is built inside CustomDBCMgr
// (FUN_101d3d20, which did not decompile; disassembly at 0x101D9F17): every ManastormModifiers.dbc row
// with +0x38 set adds +8 under +4. The original builds it once at load, so later 0x68D patches never
// reach it; here it is built at the first glue screen, before any server patch can arrive.
//
// Deviation: GetMaxCompletedLevels with a unit token that does not resolve reads an uninitialised GUID
// in the original; 0 here.
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscChallenge.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscDbcPatch.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    const char* const kTypes[] = {   // 0x10B45858
        "SOLO", "DUO", "TRIO", "GROUP", "SOLO_END_GAME", "DUO_END_GAME", "TRIO_END_GAME", "GROUP_END_GAME", "MAX",
    };
    const char* const kEnterResults[] = {   // 0x10B1D580
        "ENTER_MANASTORM_OK", "ENTER_MANASTORM_UNKNOWN", "ENTER_MANASTORM_ALREADY_ACTIVE",
        "ENTER_MANASTORM_TOO_LOW_COMPLETED_LEVEL", "ENTER_MANASTORM_BAD_LEVEL", "ENTER_MANASTORM_BAD_GROUP_LEVELS",
        "ENTER_MANASTORM_COOLDOWN", "ENTER_MANASTORM_GROUP_COOLDOWN", "ENTER_MANASTORM_HIGH_RISK",
        "ENTER_MANASTORM_GROUP_HIGH_RISK", "ENTER_MANASTORM_NO_MANASTORM_CHALLENGE",
        "ENTER_MANASTORM_GROUP_NO_MANASTORM_CHALLENGE", "ENTER_MANASTORM_ON_FLIGHT_PATH",
        "ENTER_MANASTORM_GROUP_ON_FLIGHT_PATH", "ENTER_MANASTORM_IN_PVP_COMBAT", "ENTER_MANASTORM_GROUP_IN_PVP_COMBAT",
        "ENTER_MANASTORM_IN_BATTLEGROUND_OR_ARENA", "ENTER_MANASTORM_GROUP_IN_BATTLEGROUND_OR_ARENA",
        "ENTER_MANASTORM_DUELING", "ENTER_MANASTORM_GROUP_DUELING", "ENTER_MANASTORM_TOO_LOW_PLAYER_LEVEL",
        "ENTER_MANASTORM_GROUP_TOO_LOW_PLAYER_LEVEL", "ENTER_MANASTORM_NOT_GROUP_LEADER",
        "ENTER_MANASTORM_END_GAME_NOT_ENABLED", "ENTER_MANASTORM_BAD_GROUP_SIZE",
    };
    const char* const kLeaveResults[] = {   // 0x10B1C490
        "LEAVE_MANASTORM_OK", "LEAVE_MANASTORM_UNKNOWN", "LEAVE_MANASTORM_NOT_ACTIVE", "LEAVE_MANASTORM_TRANSITION",
    };
    const char* const kLoadoutResults[] = {   // 0x10B205F8
        "SET_MANASTORM_LOADOUT_OK", "SET_MANASTORM_LOADOUT_UNKNOWN", "SET_MANASTORM_LOADOUT_OUT_OF_RANGE",
        "SET_MANASTORM_LOADOUT_BAD_SPELL", "SET_MANASTORM_LOADOUT_ACTIVE_MANASTORM", "SET_MANASTORM_LOADOUT_BAD_RANK",
        "SET_MANASTORM_LOADOUT_ALREADY_SET", "SET_MANASTORM_LOADOUT_NOT_KNOWN",
    };
    template <size_t N> int Index(const char* const (&table)[N], const std::string& s)
    {
        for (size_t i = 0; i < N; ++i)
            if (s == table[i])
                return static_cast<int>(i);
        return -1;
    }
    template <size_t N> std::string Name(const char* const (&table)[N], uint32_t v)
    {
        return v < N ? std::string(table[v]) : "UNEXPECTED_ENUM_VALUE_" + std::to_string(v);
    }

    typedef std::vector<std::vector<uint32_t>> Levels;   // per ActiveManastormType, ascending

    struct Reward        // node +0xC (FUN_102a4cc0's param_4)
    {
        uint32_t item = 0;
        uint8_t action = 0;                  // 0 NONE, 1 HIDE, 2 LOCKED_RATE_LIMIT_EXHAUSTED
        uint32_t lockEnd = 0, secondsRemaining = 0, earned = 0, max = 0, remaining = 0, windowStart = 0, windowEnd = 0;
    };

    struct Mgr
    {
        std::vector<uint32_t> completed;                                         // +0x00
        uint32_t level = 0, id = 0, type = 0;                                    // +0x0C
        uint32_t cacheA = 0;                                                     // +0x18
        float cacheB = 0.0f;                                                     // +0x1C
        uint32_t cacheC = 0;                                                     // +0x20
        Levels pending;                                                          // +0x24
        std::unordered_map<uint64_t, Levels> levels;                             // +0x30
        uint32_t chaoticLink = 0;                                                // +0x50
        uint32_t pendingLoadoutSpell = 0;                                        // +0x54
        std::vector<uint32_t> loadout;                                           // +0x58
        std::unordered_map<uint64_t, std::unordered_map<uint32_t, Reward>> rewards;   // +0x64
        bool pendingApply = false;                                               // +0x84
    };
    Mgr g_mgr;
    int64_t g_objectiveIconTime = 0;   // 0x10BE31A0

    AscDbc::Table& Manastorm()                    { return AscDbc::Get("DBFilesClient\\Manastorm.dbc"); }
    AscDbc::Table& ManastormModifiers()           { return AscDbc::Get("DBFilesClient\\ManastormModifiers.dbc"); }
    AscDbc::Table& ManastormPlayerGroupModifiers() { return AscDbc::Get("DBFilesClient\\ManastormPlayerGroupModifiers.dbc"); }
    AscDbc::Table& ManastormMessages()            { return AscDbc::Get("DBFilesClient\\ManastormMessages.dbc"); }
    uint32_t U32(const uint8_t* r, uint32_t off) { return AscDbc::Table::U32(r, off); }
    float F32(const uint8_t* r, uint32_t off)    { return AscDbc::Table::F32(r, off); }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadCString(CDataStore* p)
    {
        const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        std::string out(s);
        p->m_read += static_cast<uint32_t>(out.size()) + 1;
        return out;
    }

    // ---- client state ------------------------------------------------------------------------------
    uint32_t PlayerLevel(const uint8_t* player)   // descriptor +0xD8 (UNIT_FIELD_LEVEL)
    {
        return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(player + 8) + 0xD8);
    }

    // FUN_102a4920: the player is at or above the server's max level (never below 60).
    bool EndGame()
    {
        const uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        return std::max<uint32_t>(AscGameEvents::ServerMaxLevel(), 0x3C) <= PlayerLevel(player);
    }

    const uint64_t* PartyGuids() { return reinterpret_cast<const uint64_t*>(0xBD1948); }   // 4
    uint32_t RaidCount()         { return *reinterpret_cast<const uint32_t*>(0xBEB608); }
    const uint64_t* RaidGuids()  { return reinterpret_cast<const uint64_t*>(0xBEB568); }

    // FUN_102a0f00: the raid members with a GUID, else the party members, plus one.
    uint32_t GroupSize()
    {
        uint32_t n = 0;
        if (RaidCount() != 0)
        {
            for (uint32_t i = 0; i < RaidCount(); ++i)
                if (RaidGuids()[i] != 0)
                    ++n;
            if (n != 0)
                return n + 1;
        }
        for (uint32_t i = 0; i < 4; ++i)
            if (PartyGuids()[i] != 0)
                ++n;
        return n + 1;
    }
    uint32_t GroupTypeIndex()
    {
        switch (GroupSize())
        {
        case 1: return 0;
        case 2: return 1;
        case 3: return 2;
        default: return 3;
        }
    }

    typedef char(__thiscall* Knows_t)(void*, uint32_t);
    bool PlayerKnows(uint8_t* player, uint32_t spell) { return reinterpret_cast<Knows_t>(0x7260E0)(player, spell) != 0; }

    // FUN_10216470: the highest rank of the spell's chain the player knows; the first rank otherwise.
    uint32_t HighestKnownRank(uint32_t spell)
    {
        const uint32_t first = AscSpellRank::FirstRank(spell);
        uint8_t* player = ActivePlayer();
        if (!player)
            return first;
        const std::vector<uint32_t> chain = AscSpellRank::Chain(first);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if (PlayerKnows(player, *it))
                return *it;
        return first;
    }

    // ---- completed levels ------------------------------------------------------------------------
    uint32_t Highest(const Levels& v, uint32_t i) { return i < v.size() && !v[i].empty() ? v[i].back() : 0; }

    // FUN_102a0f60: the highest completed level -- over every type (all), of type `index` (specific),
    // or over the four normal (0..3) or end-game (4..7) types.
    uint32_t MaxCompleted(const Levels& v, uint32_t index, bool specific, bool endGame, bool all)
    {
        if (!all && specific)
            return Highest(v, index);
        uint32_t best = 0;
        const uint32_t from = all ? 0 : endGame ? 4 : 0;
        const uint32_t to = all ? 9 : from + 4;   // DAT_10B45900 = 0..8
        for (uint32_t i = from; i < to; ++i)
            best = std::max(best, Highest(v, i));
        return best;
    }
    uint32_t MaxOfPair(const Levels& v, uint32_t a, uint32_t b) { return std::max(Highest(v, a), Highest(v, b)); }   // FUN_102a08f0

    // FUN_102a1170: the lowest of the player's and every known group member's highest completed level.
    // A member the map does not know is skipped; the player missing counts as 0.
    uint32_t GroupMin(uint32_t index, bool specific, bool endGame, bool all)
    {
        auto own = g_mgr.levels.find(ActivePlayerGuid());
        uint32_t v = own == g_mgr.levels.end() ? 0 : MaxCompleted(own->second, index, specific, endGame, all);
        auto member = [&](uint64_t guid) {
            auto it = g_mgr.levels.find(guid);
            if (it != g_mgr.levels.end())
                v = std::min(v, MaxCompleted(it->second, index, specific, endGame, all));
        };
        for (uint32_t i = 0; i < 4; ++i)
            member(PartyGuids()[i]);
        for (uint32_t i = 0; i < RaidCount(); ++i)
            member(RaidGuids()[i]);
        return v;
    }

    // FUN_102a1440: the GUIDs to blame for TOO_LOW_COMPLETED_LEVEL. Each test is the GROUP minimum
    // (FUN_102a1170), not the member's own, so once the group falls short every known member is listed.
    std::vector<uint64_t> BelowRequirement(uint32_t level, bool specific, bool endGame, bool all)
    {
        std::vector<uint64_t> out;
        if (level == 0)
            return out;
        const uint32_t threshold = level - 1;
        if (GroupMin(GroupTypeIndex(), specific, endGame, all) < threshold)
            out.push_back(ActivePlayerGuid());
        auto member = [&](uint64_t guid) {
            if (g_mgr.levels.count(guid) && GroupMin(GroupTypeIndex(), specific, endGame, all) < threshold)
                out.push_back(guid);
        };
        for (uint32_t i = 0; i < 4; ++i)
            member(PartyGuids()[i]);
        for (uint32_t i = 0; i < RaidCount(); ++i)
            member(RaidGuids()[i]);
        return out;
    }

    // Map 0x10BE0658 (see the header): ManastormModifiers +4 -> {+8} for rows with +0x38 set.
    std::unordered_map<uint32_t, std::set<uint32_t>> g_allowedLevels;
    bool g_allowedBuilt = false;
    void BuildAllowedLevels()
    {
        if (g_allowedBuilt)
            return;
        g_allowedBuilt = true;
        AscDbc::Table& t = ManastormModifiers();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
                if (U32(r, 0x38) != 0)
                    g_allowedLevels[U32(r, 4)].insert(U32(r, 8));
    }
    bool LevelAllowed(uint32_t key, uint32_t level)   // FUN_10211020
    {
        BuildAllowedLevels();
        auto it = g_allowedLevels.find(key);
        return it != g_allowedLevels.end() && it->second.count(level) != 0;
    }

    uint32_t LoadoutsMax()   // FUN_102a1330
    {
        const int32_t* v = AscConfig::Int("CONFIG_MANASTORM_LOADOUTS_MAX");
        return v ? static_cast<uint32_t>(*v) : 4;
    }

    // ---- validators --------------------------------------------------------------------------------
    // FUN_102a09a0: why the player cannot enter `level` (ENTER_MANASTORM_* indexes).
    std::vector<uint32_t> EnterErrors(uint32_t level)
    {
        std::vector<uint32_t> e;
        if (g_mgr.level != 0)
            e.push_back(2);
        const bool endGame = EndGame();
        if (level != 0 && GroupMin(GroupTypeIndex(), false, endGame, false) < level - 1)
            e.push_back(3);
        uint8_t* player = ActivePlayer();
        if (!LevelAllowed(player ? (EndGame() ? 2 : 0) : 0, level))
            e.push_back(4);
        if (ActivePlayer() && UnitHasAuraSpell(ActivePlayer(), 0x16C7E))
            e.push_back(6);
        if (AscChallenge::ActiveHasRule(0x6C))
            e.push_back(10);
        if ((player = ActivePlayer()) != nullptr && PlayerLevel(player) < 10)
            e.push_back(0x14);
        return e;
    }

    // FUN_102a0b80: why `spell` cannot go into loadout slot `slot` (0-based; SET_MANASTORM_LOADOUT_*).
    // The range test is `max < slot`, so slot == max passes -- kept.
    std::vector<uint32_t> LoadoutErrors(uint32_t slot, uint32_t spell)
    {
        std::vector<uint32_t> e;
        if (LoadoutsMax() < slot)
            e.push_back(2);
        const uint8_t* attr = AscCA::SpellCustomAttrRow(spell);
        if (!attr || !(attr[0x14] & 0x80))
            e.push_back(3);
        if (g_mgr.level != 0)
            e.push_back(4);
        if (HighestKnownRank(spell) != spell)
            e.push_back(5);
        if (std::find(g_mgr.loadout.begin(), g_mgr.loadout.end(), spell) != g_mgr.loadout.end())
            e.push_back(6);
        uint8_t* player = ActivePlayer();
        if (player && !PlayerKnows(player, spell))
            e.push_back(7);
        return e;
    }

    // FUN_102a0200 / FUN_1029fe20 / FUN_102a0010: the codes as an array of names.
    template <size_t N> void PushErrors(lua_State* L, const std::vector<uint32_t>& e, const char* const (&table)[N])
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(e.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < e.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, Name(table, e[i]).c_str());
            AscLua::lua_settable(L, -3);
        }
    }
    void PushIntArray(lua_State* L, const std::vector<uint32_t>& v)   // FUN_1009a460
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushInt(L, static_cast<int32_t>(v[i]));
            AscLua::lua_settable(L, -3);
        }
    }

    // ---- client patches ------------------------------------------------------------------------------
    template <class T> void WriteProtected(uint32_t address, T value)
    {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), PAGE_EXECUTE_READWRITE, &old))
        {
            *reinterpret_cast<volatile T*>(address) = value;
            VirtualProtect(reinterpret_cast<void*>(address), sizeof(T), old, &old);
        }
    }

    // FUN_102a6c10: the objective-icon geometry constants in the client's .rdata.
    void SetObjectiveIconScale(float s)
    {
        WriteProtected<float>(0xAF46AC, s * -0.5f);
        WriteProtected<float>(0xAF46B0, s);
        WriteProtected<float>(0xAF46B8, s * 0.5f);
        WriteProtected<float>(0xAF46BC, s);
        WriteProtected<float>(0xAF46C4, s * 0.5f);
        WriteProtected<float>(0xAF46D0, s * -0.5f);
    }

    void SetSpellRangeFlags(uint32_t flags)   // FUN_100b2590 rows 0x72 / 0x1BD, +0x14
    {
        for (uint32_t id : {0x72u, 0x1BDu})
            if (const uint8_t* row = ClientDbcRow(0xAD4988, id))
                *reinterpret_cast<uint32_t*>(const_cast<uint8_t*>(row) + 0x14) = flags;
    }

    // ---- packets -------------------------------------------------------------------------------------
    void __cdecl OnEnterResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x652
    {
        const std::string result = ReadCString(p);
        const int i = Index(kEnterResults, result);
        if (i < 0)
            AscLog::Printf("Unexpected Value: %s", result.c_str());
        else if (i == 0)
            g_mgr.completed.clear();
        AscRuntime::Signal("ENTER_MANASTORM_RESULT", "%s", result.c_str());
    }

    void __cdecl OnLeaveResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x666
    {
        const std::string result = ReadCString(p);
        AscRuntime::Signal("LEAVE_MANASTORM_RESULT", "%s", result.c_str());
    }

    void __cdecl OnActiveData(void*, uint32_t, uint32_t, CDataStore* p)   // 0x660
    {
        const uint32_t oldLevel = g_mgr.level;
        const uint32_t oldA = g_mgr.cacheA, oldC = g_mgr.cacheC;
        const float oldB = g_mgr.cacheB;
        g_mgr.level = Read<uint32_t>(p);
        g_mgr.id = Read<uint32_t>(p);
        const std::string type = ReadCString(p);
        int t = Index(kTypes, type);
        if (t < 0)
        {
            AscLog::Printf("Unexpected Value: %s", type.c_str());
            t = 8;
        }
        g_mgr.type = static_cast<uint32_t>(t);
        if (g_mgr.type == 8)
            AscLog::Printf("ManastormMgr::HandleManastormActiveDataOpcode: Unknown ActiveManastormType: %s", type.c_str());
        g_mgr.cacheA = Read<uint32_t>(p);
        g_mgr.cacheB = Read<float>(p);
        g_mgr.cacheC = Read<uint32_t>(p);

        if (g_mgr.level != oldLevel)
            AscRuntime::Signal("ACTIVE_MANASTORM_UPDATED", "%u%u", oldLevel, g_mgr.level);
        if (g_mgr.cacheA != oldA || std::fabs(g_mgr.cacheB - oldB) > 0.001f || g_mgr.cacheC != oldC)
            AscRuntime::Signal("MANASTORM_CACHE_INFO_UPDATED", "%u%f%u", g_mgr.cacheA,
                               static_cast<double>(g_mgr.cacheB), g_mgr.cacheC);

        if (oldLevel == 0)
        {
            if (g_mgr.level != 0)
            {
                WriteProtected<uint8_t>(0x80DB37, 0xEB);
                SetSpellRangeFlags(0);
            }
        }
        else if (g_mgr.level == 0)
        {
            SetObjectiveIconScale(1.0f);
            WriteProtected<uint8_t>(0x80DB37, 0x75);
            SetSpellRangeFlags(2);
        }
    }

    // u32 n, then n lists of u32 n + n x u32; each list sorted.
    void ReadLevels(CDataStore* p, Levels& v)
    {
        const uint32_t n = Read<uint32_t>(p);
        v.assign(n, {});
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t m = Read<uint32_t>(p); m; --m)
                v[i].push_back(Read<uint32_t>(p));
        for (auto& l : v)
            std::sort(l.begin(), l.end());
    }

    void __cdecl OnData(void*, uint32_t, uint32_t, CDataStore* p)   // 0x65F
    {
        Levels* target;
        if (!ActivePlayer())
        {
            target = &g_mgr.pending;
            g_mgr.pendingApply = true;
        }
        else
            target = &g_mgr.levels[ActivePlayerGuid()];
        ReadLevels(p, *target);
    }

    void __cdecl OnUpdateMaxCompletedLevel(void*, uint32_t, uint32_t, CDataStore* p)   // 0x65D
    {
        const uint64_t guid = Read<uint64_t>(p);
        Levels& entry = g_mgr.levels[guid];
        const Levels old = entry;
        ReadLevels(p, entry);
        if (ActivePlayer() && ActivePlayerGuid() == guid && old != entry)
            AscRuntime::Signal("MANASTORM_COMPLETED_LEVELS_UPDATED");
    }

    void __cdecl OnCompletedLevel(void*, uint32_t, uint32_t, CDataStore* p)   // 0x65E
    {
        const uint32_t level = Read<uint32_t>(p);
        g_mgr.completed.push_back(level);
        g_mgr.chaoticLink = 0;
        AscRuntime::Signal("MANASTORM_LEVEL_COMPLETED", "%u", level);
        // Every message row for this difficulty (0xBD0894) and level whose game event (+0xC, u16) is
        // unset or active.
        AscDbc::Table& t = ManastormMessages();
        const uint32_t difficulty = *reinterpret_cast<const uint32_t*>(0xBD0894);
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* r = t.Row(id);
            if (!r || U32(r, 4) != difficulty || U32(r, 8) != level)
                continue;
            if (U32(r, 0xC) != 0 && !AscGameEvents::IsActive(static_cast<uint16_t>(U32(r, 0xC))))
                continue;
            AscRuntime::Signal("MANASTORM_LEVEL_UNLOCKED", "%s%s%s", t.Str(r, 0x10), t.Str(r, 0x14), t.Str(r, 0x18));
        }
    }

    void __cdecl OnFail(void*, uint32_t, uint32_t, CDataStore*)   // 0x67B
    {
        AscRuntime::Signal("MANASTORM_FAILED");
    }

    void __cdecl OnChaoticLink(void*, uint32_t, uint32_t, CDataStore* p)   // 0x67F
    {
        const uint32_t old = g_mgr.chaoticLink;
        g_mgr.chaoticLink = Read<uint32_t>(p);
        if (g_mgr.chaoticLink != old)
            AscRuntime::Signal("MANASTORM_CHAOTIC_LINK_UPDATED", "%u%u", old, g_mgr.chaoticLink);
    }

    void __cdecl OnLoadout(void*, uint32_t, uint32_t, CDataStore* p)   // 0x688
    {
        const uint32_t n = Read<uint32_t>(p);
        g_mgr.loadout.assign(n, 0);
        for (uint32_t& s : g_mgr.loadout)
            s = Read<uint32_t>(p);
    }

    void SetSlot(uint32_t slot, uint32_t spell)
    {
        if (g_mgr.loadout.size() <= slot)
            g_mgr.loadout.resize(slot + 1, 0);
        g_mgr.loadout[slot] = spell;
    }

    void __cdecl OnLoadoutResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x68A
    {
        const uint32_t slot = Read<uint32_t>(p);
        const std::string result = ReadCString(p);
        const int i = Index(kLoadoutResults, result);
        if (i < 0)
            AscLog::Printf("Unexpected Value: %s", result.c_str());
        else if (i == 0)
        {
            SetSlot(slot, g_mgr.pendingLoadoutSpell);
            g_mgr.pendingLoadoutSpell = 0;
        }
        AscRuntime::Signal("MANASTORM_LOADOUT_RESULT", "%u%s", slot + 1, result.c_str());
    }

    void __cdecl OnLoadoutUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x68B
    {
        const uint32_t slot = Read<uint32_t>(p);
        const uint32_t spell = Read<uint32_t>(p);
        SetSlot(slot, spell);
        AscRuntime::Signal("MANASTORM_LOADOUT_UPDATE", "%u%u", slot + 1, spell);
    }

    // 0x6AD: u64 guid, u32 n, n x 0x21 {u32 item, u8 action, u32 lockEnd, u32 secondsRemaining,
    // u32 earned, u32 max, u32 remaining, u32 windowStart, u32 windowEnd}. Every read is bounds-checked
    // and a short packet drops everything. Actions other than 0..2 are skipped; an empty result erases
    // the GUID's entry.
    void __cdecl OnRewardVisibility(void*, uint32_t, uint32_t, CDataStore* p)
    {
        auto left = [&]() { return static_cast<uint32_t>(p->m_size - p->m_read); };
        if (left() < 8)
            return;
        const uint64_t guid = Read<uint64_t>(p);
        if (left() < 4)
            return;
        const uint32_t n = Read<uint32_t>(p);
        if (n != 0 && left() / 0x21 < n)
            return;
        std::unordered_map<uint32_t, Reward> fresh;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (left() < 0x21)
                return;
            Reward r;
            r.item = Read<uint32_t>(p);
            r.action = Read<uint8_t>(p);
            r.lockEnd = Read<uint32_t>(p);
            r.secondsRemaining = Read<uint32_t>(p);
            r.earned = Read<uint32_t>(p);
            r.max = Read<uint32_t>(p);
            r.remaining = Read<uint32_t>(p);
            r.windowStart = Read<uint32_t>(p);
            r.windowEnd = Read<uint32_t>(p);
            if (r.action <= 2)
                fresh[r.item] = r;
        }
        if (fresh.empty())
            g_mgr.rewards.erase(guid);
        else
            g_mgr.rewards[guid].swap(fresh);
        AscRuntime::Signal("MANASTORM_REWARD_VISIBILITY_UPDATED", "%s%u", std::to_string(guid).c_str(), n);
    }

    const Reward* FindReward(uint64_t guid, uint32_t item)   // FUN_102a1960
    {
        auto g = g_mgr.rewards.find(guid);
        if (g == g_mgr.rewards.end())
            return nullptr;
        auto it = g->second.find(item);
        return it == g->second.end() ? nullptr : &it->second;
    }

    // ---- lifecycle ---------------------------------------------------------------------------------
    void OnGlueScreen()   // FUN_102a0d70
    {
        BuildAllowedLevels();
        g_mgr.completed.clear();
        g_mgr.level = g_mgr.id = g_mgr.type = g_mgr.cacheA = g_mgr.cacheC = 0;
        g_mgr.cacheB = 0.0f;
        g_mgr.pending.clear();
        g_mgr.levels.clear();
        g_mgr.chaoticLink = 0;
        g_mgr.pendingLoadoutSpell = 0;
        g_mgr.loadout.clear();
        g_mgr.rewards.clear();
        g_mgr.pendingApply = false;
        SetObjectiveIconScale(1.0f);
    }

    void ApplyPending()   // FUN_102a4ba0, after 0x403340
    {
        if (!g_mgr.pendingApply || !ActivePlayer())
            return;
        g_mgr.levels[ActivePlayerGuid()] = g_mgr.pending;
        g_mgr.pending.clear();
        g_mgr.pendingApply = false;
    }

    struct C3Vector { float x, y, z; };
    typedef void(__thiscall* GetPosition_t)(void*, C3Vector*);
    C3Vector Position(void* object)   // vtable +0x2C
    {
        C3Vector v{};
        reinterpret_cast<GetPosition_t>((*reinterpret_cast<void***>(object))[0x2C / 4])(object, &v);
        return v;
    }

    // FUN_102a4a10, after 0x743EC0: while in a Manastorm with the objective icon shown, a unit (type 3)
    // carrying a raid target icon (0x5728C0 < 8) rescales the icon to its distance / 20, in [1, 10].
    void OnObjectUpdate(void* object)
    {
        if (g_mgr.level == 0 || g_objectiveIconTime <= 0)
            return;
        const uint8_t* o = static_cast<const uint8_t*>(object);
        const uint32_t* guid = *reinterpret_cast<const uint32_t* const*>(o + 8);
        typedef int(__cdecl* RaidTarget_t)(uint32_t, uint32_t);
        if (reinterpret_cast<RaidTarget_t>(0x5728C0)(guid[0], guid[1]) >= 8)
            return;
        if (*reinterpret_cast<const uint32_t*>(o + 0x14) != 3)
            return;
        uint8_t* player = ActivePlayer();
        if (!player)   // the original calls through it unchecked
            return;
        const C3Vector a = Position(player), b = Position(object);
        // Each square through pow(double, 2.0) narrowed to float; the sum in float, sqrt in double.
        const float dz = static_cast<float>(std::pow(static_cast<double>(a.z - b.z), 2.0));
        const float dy = static_cast<float>(std::pow(static_cast<double>(a.y - b.y), 2.0));
        const float dx = static_cast<float>(std::pow(static_cast<double>(a.x - b.x), 2.0));
        const float d = static_cast<float>(std::sqrt(static_cast<double>(dx + dy + dz)));
        SetObjectiveIconScale(std::min(10.0f, std::max(1.0f, d / 20.0f)));
    }

    // ---- bindings ------------------------------------------------------------------------------------
    int Enter(lua_State* L)
    {
        uint32_t level;
        if (!ReadNumber(L, level))
            return 0;
        Packet(0x651).U32(level).Send();
        PushBool(L, true);
        return 1;
    }

    int Leave(lua_State* L)
    {
        Packet(0x665).Send();
        PushBool(L, true);
        return 1;
    }

    int CanEnter(lua_State* L)
    {
        uint32_t level;
        if (!ReadNumber(L, level))
            return 0;
        const std::vector<uint32_t> e = EnterErrors(level);
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushErrors(L, e, kEnterResults);
        if (std::find(e.begin(), e.end(), 3u) == e.end())
        {
            AscLua::lua_pushnil(L);
            return 3;
        }
        // The unit token of each member to blame: "player", else the first raid*, else the first party*.
        std::vector<std::string> tokens;
        for (uint64_t guid : BelowRequirement(level, false, EndGame(), false))
        {
            int count = 0;
            const char* const* list = reinterpret_cast<const char* const*(__cdecl*)(uint64_t*, int*)>(0x60BB70)(&guid, &count);
            const char* pick = nullptr;
            for (int i = 0; i < count && !pick; ++i)
                if (strcmp(list[i], "player") == 0)
                    pick = list[i];
            for (int i = 0; i < count && !pick; ++i)
                if (strncmp(list[i], "raid", 4) == 0)
                    pick = list[i];
            for (int i = 0; i < count && !pick; ++i)
                if (strncmp(list[i], "party", 5) == 0)
                    pick = list[i];
            if (pick)
                tokens.push_back(pick);
        }
        if (tokens.empty())
        {
            AscLua::lua_pushnil(L);
            return 3;
        }
        AscLua::lua_createtable(L, 0, static_cast<int>(tokens.size()));   // FUN_100beb90
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < tokens.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, tokens[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        return 3;
    }

    int CanLeave(lua_State* L)
    {
        std::vector<uint32_t> e;
        if (g_mgr.id == 0)
            e.push_back(2);
        if (uint8_t* player = ActivePlayer())
            if (UnitHasAuraSpell(player, 0x16C78))
                e.push_back(3);
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushErrors(L, e, kLeaveResults);
        return 2;
    }

    // 1, 6, 11, ... up to the group's highest completed level + 1 (at least 1).
    int GetEnterableLevels(lua_State* L)
    {
        uint32_t top = GroupMin(GroupTypeIndex(), false, EndGame(), false) + 1;
        if (top == 0)
            top = 1;
        std::vector<uint32_t> levels;
        uint32_t level = 1;
        do
        {
            levels.push_back(level);
            level += 5;
        } while (level <= top);
        std::sort(levels.begin(), levels.end());
        PushIntArray(L, levels);
        return 1;
    }

    int IsInManastorm(lua_State* L)
    {
        PushBool(L, g_mgr.level != 0);
        return 1;
    }

    int GetActiveLevel(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g_mgr.level));
        return 1;
    }

    int GetActiveManastormID(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g_mgr.id));
        return 1;
    }

    int GetActiveManastormType(lua_State* L)
    {
        PushStr(L, Name(kTypes, g_mgr.type).c_str());
        return 1;
    }

    // Boss (Manastorm +0xC), chaotic link, then spell 93459's +0x140 / +0x144 plus one, times the link.
    int GetBoss(lua_State* L)
    {
        const uint8_t* ms = Manastorm().Row(g_mgr.id);
        PushInt(L, ms ? static_cast<int32_t>(U32(ms, 0xC)) : 0);
        const int32_t link = static_cast<int32_t>(g_mgr.chaoticLink);
        PushInt(L, link);
        uint8_t rec[0x2A8];
        if (!FetchSpell(0x16D13, rec))
        {
            PushInt(L, 0);
            PushInt(L, 0);
            return 4;
        }
        PushInt(L, (*reinterpret_cast<const int32_t*>(rec + 0x140) + 1) * link);
        PushInt(L, (*reinterpret_cast<const int32_t*>(rec + 0x144) + 1) * link);
        return 4;
    }

    // GetMaxCompletedLevels(unit): all types, then solo/duo/trio/group each with its end-game twin.
    int GetMaxCompletedLevels(lua_State* L)
    {
        if (!ValidateInput(L, {STRING}))
            return 0;
        uint64_t guid = 0;
        UnitTokenGuid(CheckString(L, 1), guid);
        auto it = g_mgr.levels.find(guid);
        if (it == g_mgr.levels.end())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const Levels& v = it->second;
        PushInt(L, static_cast<int32_t>(MaxCompleted(v, 0, false, false, true)));
        PushInt(L, static_cast<int32_t>(MaxOfPair(v, 0, 4)));
        PushInt(L, static_cast<int32_t>(MaxOfPair(v, 1, 5)));
        PushInt(L, static_cast<int32_t>(MaxOfPair(v, 2, 6)));
        PushInt(L, static_cast<int32_t>(MaxOfPair(v, 3, 7)));
        return 5;
    }

    // The first ManastormModifiers row for (a, b): +0x2C and +0x34.
    int GetExperienceModifier(lua_State* L)
    {
        int32_t a, b;
        if (!ReadInt2(L, a, b))
            return 0;
        AscDbc::Table& t = ManastormModifiers();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* r = t.Row(id);
            if (r && AscDbc::Table::I32(r, 4) == a && AscDbc::Table::I32(r, 8) == b)
            {
                PushNum(L, static_cast<double>(F32(r, 0x2C)));
                PushNum(L, static_cast<double>(F32(r, 0x34)));
                return 2;
            }
        }
        return PushNils(L, 2);
    }

    // The active Manastorm's modifier row (+4 = Manastorm +8, +8 = active level): +0x2C * Manastorm
    // +0x18 and +0x34 * Manastorm +0x20.
    int GetStageBonusExperience(lua_State* L)
    {
        if (const uint8_t* ms = Manastorm().Row(g_mgr.id))
        {
            AscDbc::Table& t = ManastormModifiers();
            for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            {
                const uint8_t* r = t.Row(id);
                if (r && U32(r, 4) == U32(ms, 8) && U32(r, 8) == g_mgr.level)
                {
                    PushNum(L, static_cast<double>(F32(r, 0x2C) * F32(ms, 0x18)));
                    PushNum(L, static_cast<double>(F32(r, 0x34) * F32(ms, 0x20)));
                    return 2;
                }
            }
        }
        return PushNils(L, 2);
    }

    // GetRewardModifier(id): Manastorm +0x10 / +0x14 (1.0 without a row), the group modifier -- the
    // ManastormPlayerGroupModifiers row with the largest size (+4) not above the group and, among
    // those, the largest +8 below 2 -- and the two products.
    int GetRewardModifier(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        float a = 1.0f, b = 1.0f, c = 1.0f, d = 1.0f;
        if (const uint8_t* ms = Manastorm().Row(id))
        {
            a = F32(ms, 0x10);
            b = F32(ms, 0x14);
        }
        const uint32_t size = GroupSize();
        uint32_t bestSize = 0, bestKind = 0;
        AscDbc::Table& t = ManastormPlayerGroupModifiers();
        for (uint32_t i = t.MinId(); t.Loaded() && i <= t.MaxId(); ++i)
        {
            const uint8_t* r = t.Row(i);
            if (!r)
                continue;
            const uint32_t s = U32(r, 4), k = U32(r, 8);
            if (s <= size && bestSize <= s && k < 2 && bestKind <= k)
            {
                c = F32(r, 0xC);
                d = F32(r, 0x10);
                bestSize = s;
                bestKind = k;
            }
        }
        PushNum(L, static_cast<double>(a));
        PushNum(L, static_cast<double>(b));
        PushNum(L, static_cast<double>(c));
        PushNum(L, static_cast<double>(d));
        PushNum(L, static_cast<double>(c * a));
        PushNum(L, static_cast<double>(d * b));
        return 6;
    }

    int ShowObjectiveIcon(lua_State*)
    {
        g_objectiveIconTime = _time64(nullptr) + 10;
        return 0;
    }

    int GetManastormCacheInfo(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g_mgr.cacheA));
        PushNum(L, static_cast<double>(g_mgr.cacheB));
        PushInt(L, static_cast<int32_t>(g_mgr.cacheC));
        return 3;
    }

    int GetNumLoadoutSlots(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(LoadoutsMax()));
        return 1;
    }

    int GetLoadoutSpellAtIndex(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        if (index - 1 < g_mgr.loadout.size() && g_mgr.loadout[index - 1] != 0)
            PushInt(L, static_cast<int32_t>(g_mgr.loadout[index - 1]));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int SetLoadoutSpellAtIndex(lua_State* L)
    {
        int32_t index, spell;
        if (!ReadInt2(L, index, spell) || index == 0)
            return 0;
        Packet(0x689).U32(static_cast<uint32_t>(index - 1)).U32(static_cast<uint32_t>(spell)).Send();
        g_mgr.pendingLoadoutSpell = static_cast<uint32_t>(spell);
        PushBool(L, true);
        return 1;
    }

    int CanSetLoadoutSpellAtIndex(lua_State* L)
    {
        int32_t index, spell;
        if (!ReadInt2(L, index, spell) || index == 0)
            return 0;
        const std::vector<uint32_t> e = LoadoutErrors(static_cast<uint32_t>(index - 1), static_cast<uint32_t>(spell));
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushErrors(L, e, kLoadoutResults);
        return 2;
    }

    // Every SpellCustomAttr row flagged 0x80 (+0x14) whose spell is the highest rank the player knows:
    // first the ones some slot accepts, then the rest.
    int GetAvailableLoadoutSpells(lua_State* L)
    {
        std::vector<uint32_t> settable, other;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\SpellCustomAttr.dbc");
        for (uint32_t i = 0; i < t.Count(); ++i)
        {
            const uint8_t* r = t.RowAt(i);
            if (!r || !(r[0x14] & 0x80))
                continue;
            const uint32_t spell = U32(r, 4);
            if (HighestKnownRank(spell) != spell)
                continue;
            bool placed = false;
            for (uint32_t slot = 0; slot < LoadoutsMax() && !placed; ++slot)
                placed = LoadoutErrors(slot, spell).empty();
            (placed ? settable : other).push_back(spell);
        }
        PushIntArray(L, settable);
        PushIntArray(L, other);
        return 2;
    }

    // FUN_102a4cc0
    void PushReward(lua_State* L, uint64_t guid, const Reward& r)
    {
        AscLua::lua_createtable(L, 0, 0xE);
        AscLua::lua_checkstack(L, 2);
        auto optional = [&](const char* k, uint32_t v) {
            PushStr(L, k);
            if (v == 0)
                AscLua::lua_pushnil(L);
            else
                PushInt(L, static_cast<int32_t>(v));
            AscLua::lua_settable(L, -3);
        };
        auto flag = [&](const char* k, bool v) {
            PushStr(L, k);
            PushBool(L, v);
            AscLua::lua_settable(L, -3);
        };
        PushStr(L, "context");
        PushStr(L, std::to_string(guid).c_str());
        AscLua::lua_settable(L, -3);
        SetStrInt(L, "itemID", static_cast<int32_t>(r.item));
        PushStr(L, "action");
        PushStr(L, r.action == 1 ? "HIDE" : r.action == 2 ? "LOCKED_RATE_LIMIT_EXHAUSTED" : "NONE");
        AscLua::lua_settable(L, -3);
        flag("isHidden", r.action == 1);
        flag("isLocked", r.action == 2);
        optional("lockEndTimestamp", r.lockEnd);
        optional("secondsRemaining", r.secondsRemaining);
        SetStrInt(L, "earnedQuantity", static_cast<int32_t>(r.earned));
        SetStrInt(L, "maxQuantity", static_cast<int32_t>(r.max));
        SetStrInt(L, "remainingQuantity", static_cast<int32_t>(r.remaining));
        optional("windowStart", r.windowStart);
        optional("windowEnd", r.windowEnd);
    }

    // GetRewardVisibility(guid[, item]): every item's table keyed by item id, or the one item (nil).
    int GetRewardVisibility(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const uint64_t guid = static_cast<uint64_t>(CheckNumber(L, 1));
        if (AscLua::lua_type(L, 2) < 1)
        {
            auto g = g_mgr.rewards.find(guid);
            if (g == g_mgr.rewards.end())
            {
                AscLua::lua_createtable(L, 0, 0);
                return 1;
            }
            AscLua::lua_createtable(L, 0, static_cast<int>(g->second.size()));
            AscLua::lua_checkstack(L, 2);
            for (const auto& e : g->second)
            {
                PushInt(L, static_cast<int32_t>(e.first));
                PushReward(L, guid, e.second);
                AscLua::lua_settable(L, -3);
            }
            return 1;
        }
        const uint32_t item = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        if (const Reward* r = FindReward(guid, item))
            PushReward(L, guid, *r);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    bool ReadGuidItem(lua_State* L, uint64_t& guid, uint32_t& item)   // FUN_1029f060
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return false;
        guid = static_cast<uint64_t>(CheckNumber(L, 1));
        item = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return true;
    }

    int IsRewardLocked(lua_State* L)
    {
        uint64_t guid;
        uint32_t item;
        if (!ReadGuidItem(L, guid, item))
            return 0;
        const Reward* r = FindReward(guid, item);
        if (!r || r->action != 2)
        {
            PushBool(L, false);
            return 1 + PushNils(L, 2);
        }
        PushBool(L, true);
        if (r->lockEnd == 0)
            AscLua::lua_pushnil(L);
        else
            PushInt(L, static_cast<int32_t>(r->lockEnd));
        if (r->secondsRemaining == 0)
            AscLua::lua_pushnil(L);
        else
            PushInt(L, static_cast<int32_t>(r->secondsRemaining));
        return 3;
    }

    // Earned, max, remaining, seconds remaining (nil if 0), locked -- or nil without a limit (max 0).
    int GetRewardLimitProgress(lua_State* L)
    {
        uint64_t guid;
        uint32_t item;
        if (!ReadGuidItem(L, guid, item))
            return 0;
        const Reward* r = FindReward(guid, item);
        if (!r || r->max == 0)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushInt(L, static_cast<int32_t>(r->earned));
        PushInt(L, static_cast<int32_t>(r->max));
        PushInt(L, static_cast<int32_t>(r->remaining));
        if (r->secondsRemaining == 0)
            AscLua::lua_pushnil(L);
        else
            PushInt(L, static_cast<int32_t>(r->secondsRemaining));
        PushBool(L, r->action == 2);
        return 5;
    }

    int __cdecl AcceptAnyValue() { return 1; }   // 0x102A0990

    void Init()   // 0x102A4540
    {
        // FUN_10114540 (storage 0x10BE3190, which nothing in the plain code reads): no help, flags 1,
        // default "0", an always-accept callback (0x102A0990), category 4.
        AscClientOptions::QueueWorldCVar({"manastormObjectiveIconCulling", "0", 1, 4, reinterpret_cast<void*>(&AcceptAnyValue)});
        sDC.AddPacketHandler(0x652, CNetClientCustomPacket((void*)&OnEnterResult, nullptr));
        sDC.AddPacketHandler(0x666, CNetClientCustomPacket((void*)&OnLeaveResult, nullptr));
        sDC.AddPacketHandler(0x65D, CNetClientCustomPacket((void*)&OnUpdateMaxCompletedLevel, nullptr));
        sDC.AddPacketHandler(0x65E, CNetClientCustomPacket((void*)&OnCompletedLevel, nullptr));
        sDC.AddPacketHandler(0x65F, CNetClientCustomPacket((void*)&OnData, nullptr));
        sDC.AddPacketHandler(0x660, CNetClientCustomPacket((void*)&OnActiveData, nullptr));
        sDC.AddPacketHandler(0x67B, CNetClientCustomPacket((void*)&OnFail, nullptr));
        // FUN_102a3f40: a raw 0x24 row. FUN_102a3b50: 0x1C row image, then its three strings.
        AscDbcPatch::Register(0x67D, "DBFilesClient\\Manastorm.dbc", 0x24, 0, nullptr);
        AscDbcPatch::Register(0x67E, "DBFilesClient\\ManastormMessages.dbc", 0x1C, (1ull << 4) | (1ull << 5) | (1ull << 6), nullptr);
        sDC.AddPacketHandler(0x67F, CNetClientCustomPacket((void*)&OnChaoticLink, nullptr));
        sDC.AddPacketHandler(0x688, CNetClientCustomPacket((void*)&OnLoadout, nullptr));
        sDC.AddPacketHandler(0x68A, CNetClientCustomPacket((void*)&OnLoadoutResult, nullptr));
        sDC.AddPacketHandler(0x68B, CNetClientCustomPacket((void*)&OnLoadoutUpdate, nullptr));
        sDC.AddPacketHandler(0x6AD, CNetClientCustomPacket((void*)&OnRewardVisibility, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
        AscRuntime::OnAfter743EC0(OnObjectUpdate);
        AscRuntime::OnAfter403340(ApplyPending);
    }

    const AscBindings::Binding kBindings[] = {   // registrar 0x102A7FE0, in its order
        {"C_Manastorm", "Enter", Enter},
        {"C_Manastorm", "Leave", Leave},
        {"C_Manastorm", "CanEnter", CanEnter},
        {"C_Manastorm", "CanLeave", CanLeave},
        {"C_Manastorm", "GetEnterableLevels", GetEnterableLevels},
        {"C_Manastorm", "IsInManastorm", IsInManastorm},
        {"C_Manastorm", "GetActiveLevel", GetActiveLevel},
        {"C_Manastorm", "GetActiveManastormID", GetActiveManastormID},
        {"C_Manastorm", "GetBoss", GetBoss},
        {"C_Manastorm", "GetMaxCompletedLevels", GetMaxCompletedLevels},
        {"C_Manastorm", "GetActiveManastormType", GetActiveManastormType},
        {"C_Manastorm", "GetExperienceModifier", GetExperienceModifier},
        {"C_Manastorm", "GetStageBonusExperience", GetStageBonusExperience},
        {"C_Manastorm", "GetRewardModifier", GetRewardModifier},
        {"C_Manastorm", "ShowObjectiveIcon", ShowObjectiveIcon},
        {"C_Manastorm", "GetManastormCacheInfo", GetManastormCacheInfo},
        {"C_Manastorm", "GetNumLoadoutSlots", GetNumLoadoutSlots},
        {"C_Manastorm", "GetLoadoutSpellAtIndex", GetLoadoutSpellAtIndex},
        {"C_Manastorm", "SetLoadoutSpellAtIndex", SetLoadoutSpellAtIndex},
        {"C_Manastorm", "CanSetLoadoutSpellAtIndex", CanSetLoadoutSpellAtIndex},
        {"C_Manastorm", "GetAvailableLoadoutSpells", GetAvailableLoadoutSpells},
        {"C_Manastorm", "GetRewardVisibility", GetRewardVisibility},
        {"C_Manastorm", "IsRewardLocked", IsRewardLocked},
        {"C_Manastorm", "GetRewardLimitProgress", GetRewardLimitProgress},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

namespace AscManastorm
{
    // FUN_102a4a00 on the manager (FUN_102a4760): its +0x0C level is set.
    bool InManastorm() { return g_mgr.level != 0; }
}
