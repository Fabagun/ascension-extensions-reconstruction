// More small GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c).
//   SMSG 0x720 (FUN_102d9f20 -> FUN_102d9dd0): 11 x u32 wildcard prestige record (words 8..10 are
//   what GetWildcardPrestigeInfo reports).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>

using namespace AscScript;

namespace
{
    uint32_t g_prestige[11] = {};   // DAT_10be37fc..
    bool g_prestigeSet = false;     // DAT_10be3828

    int32_t ArgInt(lua_State* L, int idx) { return ToInt(CheckNumber(L, idx)); }
    uint8_t* PlayerDescriptors()
    {
        uint8_t* p = ActivePlayer();
        return p ? *reinterpret_cast<uint8_t**>(p + 8) : nullptr;
    }

    int GetScreenLocationInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\ScreenLocations.dbc");
        if (const uint8_t* row = t.Row(id))
            PushStr(L, t.Str(row, 4));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetWildcardPrestigeInfo(lua_State* L)
    {
        if (!g_prestigeSet)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 3;
        }
        for (uint32_t i = 8; i < 11; ++i)
            AscLua::lua_pushnumber(L, static_cast<double>(static_cast<int32_t>(g_prestige[i])));
        return 3;
    }

    // x, y, z (object vtable +0x2C) and the client's map id at 0xADFBC4.
    int GetCurrentPlayerPosition(lua_State* L)
    {
        uint8_t* p = ActivePlayer();
        const int32_t map = *reinterpret_cast<const int32_t*>(0xADFBC4);
        if (!p)
            return 0;
        float pos[3] = {};
        typedef void(__thiscall* PosFn)(void*, float*);
        reinterpret_cast<PosFn>((*reinterpret_cast<void***>(p))[0x2C / 4])(p, pos);
        AscLua::lua_pushnumber(L, pos[0]);
        AscLua::lua_pushnumber(L, pos[1]);
        AscLua::lua_pushnumber(L, pos[2]);
        AscLua::lua_pushinteger(L, map);
        return 4;
    }

    int SendEmpty(uint32_t opcode)
    {
        Packet(opcode).Send();
        return 0;
    }
    int ResetRaids(lua_State*) { return SendEmpty(0x620); }
    int ResetDungeons(lua_State*) { return SendEmpty(0x61F); }
    int PortGraveyard(lua_State*) { return SendEmpty(0x544); }
    int ResetWorldTime(lua_State*) { return SendEmpty(0x543); }

    // Only while the party has at most 4 members (0x52BD10).
    int ConvertToParty(lua_State*)
    {
        if (reinterpret_cast<uint8_t(__cdecl*)()>(0x52BD10)() <= 4)
            Packet(0x525).Send();
        return 0;
    }

    // ChrRaces: name (+0x38), client prefix file string (+0x2C), id.
    int GetRaceInfoByID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = ClientDbcRow(0xAD3428, id);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            AscLua::lua_pushnil(L);
            return 3;
        }
        PushStr(L, *reinterpret_cast<const char* const*>(row + 0x38));
        PushStr(L, *reinterpret_cast<const char* const*>(row + 0x2C));
        AscLua::lua_pushinteger(L, *reinterpret_cast<const int32_t*>(row));
        return 3;
    }

    // The item addon record's +0x3C (FUN_101a2690).
    int GetItemLevelInstant(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        if (const AscItemAddon::Record* r = AscItemAddon::Find(id))
            AscLua::lua_pushinteger(L, static_cast<int>(r->fields[11]));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // StackAmount (+0xC4) through the player's spell modifiers (0x7FD970, SPELLMOD 0x1F).
    int GetSpellMaxStack(lua_State* L)
    {
        uint8_t rec[0x2A8];
        if (!FetchSpell(static_cast<uint32_t>(ArgInt(L, 1)), rec))
            return 0;
        int32_t add = 0, pct = 100;
        reinterpret_cast<void(__cdecl*)(void*, int, int32_t*, int32_t*)>(0x7FD970)(rec, 0x1F, &add, &pct);
        int32_t stack = *reinterpret_cast<int32_t*>(rec + 0xC4);
        if (add != 0 || pct != 0)
            stack = ((add + stack) * pct) / 100;
        AscLua::lua_pushinteger(L, stack);
        return 1;
    }

    // Main-hand weapon delay (item cache +0xF4, 2000 without one) over UNIT_FIELD_BASEATTACKTIME.
    int GetMeleeHaste(lua_State* L)
    {
        uint8_t* d = PlayerDescriptors();
        if (!d)
        {
            AscLua::lua_pushnumber(L, 0);
            return 1;
        }
        const int32_t attackTime = *reinterpret_cast<int32_t*>(d + 0xF8);
        const uint64_t guid = *reinterpret_cast<uint64_t*>(d + 0x588);
        int32_t delay = 2000;
        if (uint8_t* item = static_cast<uint8_t*>(ObjectPtr(guid, 2)))
        {
            const uint32_t entry = *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint8_t**>(item + 8) + 0xC);
            const uint8_t* rec = static_cast<const uint8_t*>(
                reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67CA30)(
                    reinterpret_cast<void*>(0xC5D828), entry, nullptr, nullptr, nullptr, 0));
            if (rec)
                delay = *reinterpret_cast<const int32_t*>(rec + 0xF4);
        }
        AscLua::lua_pushnumber(L, static_cast<double>(static_cast<float>(delay) / static_cast<float>(attackTime)));
        return 1;
    }

    void __cdecl OnPrestige(void*, uint32_t, uint32_t, CDataStore* p)   // 0x720
    {
        memcpy(g_prestige, p->m_buffer + p->m_read, sizeof(g_prestige));
        p->m_read += sizeof(g_prestige);
        g_prestigeSet = true;
    }

    void ResetPrestige() { g_prestigeSet = false; }   // FUN_102d9ef0 (every glue screen)

    void Init()
    {
        AscRuntime::OnGlueScreen(&ResetPrestige);
        sDC.AddPacketHandler(0x720, CNetClientCustomPacket((void*)&OnPrestige, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "ConvertToParty", ConvertToParty},
        {nullptr, "GetCurrentPlayerPosition", GetCurrentPlayerPosition},
        {nullptr, "GetItemLevelInstant", GetItemLevelInstant},
        {nullptr, "GetMeleeHaste", GetMeleeHaste},
        {nullptr, "GetRaceInfoByID", GetRaceInfoByID},
        {nullptr, "GetScreenLocationInfo", GetScreenLocationInfo},
        {nullptr, "GetSpellMaxStack", GetSpellMaxStack},
        {nullptr, "GetWildcardPrestigeInfo", GetWildcardPrestigeInfo},
        {nullptr, "PortGraveyard", PortGraveyard},
        {nullptr, "ResetDungeons", ResetDungeons},
        {nullptr, "ResetRaids", ResetRaids},
        {nullptr, "ResetWorldTime", ResetWorldTime},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
