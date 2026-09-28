// Loose globals from the original's misc-bindings block (handlers at 0x10a4xxxx..0x10a7xxxx, state in
// the DLL globals around 0x10d3d748..0x10d3d820), with the packet handlers and client patches that
// feed them. Each function names its original.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Client/CVar.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <cstdlib>
#include <string>
#include <cstring>
#include <ctime>
#include <vector>

using namespace AscScript;

namespace
{
    // ---- DLL globals ---------------------------------------------------------------------------
    uint8_t  g_taxiEarlyLanding = 0;        // DAT_10d3d748
    uint8_t  g_friendStatusOffline = 0;     // DAT_10d3d74a
    uint32_t g_lootSpecialization = 0;      // DAT_10d3d7d8
    int32_t  g_serverTimeOffset = 0;        // DAT_10d3d80c
    std::vector<uint32_t> g_zoneInstances;  // DAT_10d3d814
    uint32_t g_zoneInstanceIndex = 0;       // DAT_10d3d820

    // Inline CDataStore reads, as the handlers do them (+0x04 buffer, +0x14 read position).
    uint32_t ReadU32(CDataStore* pkt)
    {
        uint32_t v = *reinterpret_cast<const uint32_t*>(pkt->m_buffer + pkt->m_read);
        pkt->m_read += 4;
        return v;
    }

    // ---- exe state, read directly -------------------------------------------------------------
    // handler_GetZoneId / GetAreaId / IsInside / GetActiveMapID / GetBindPosition
    int GetZoneId(lua_State* L)      { PushInt(L, *reinterpret_cast<const int32_t*>(0xBD080C)); return 1; }
    int GetAreaId(lua_State* L)      { PushInt(L, *reinterpret_cast<const int32_t*>(0xBD0810)); return 1; }
    int IsInside(lua_State* L)       { PushBool(L, *reinterpret_cast<const uint8_t*>(0xD39434) != 0); return 1; }
    int GetActiveMapID(lua_State* L) { PushInt(L, *reinterpret_cast<const int32_t*>(0xBD088C)); return 1; }

    int GetBindPosition(lua_State* L)
    {
        const float* pos = reinterpret_cast<const float*>(0xC9EB2C);
        PushNum(L, pos[0]);
        PushNum(L, pos[1]);
        PushNum(L, pos[2]);
        PushInt(L, *reinterpret_cast<const int32_t*>(0xC9D538));
        return 4;
    }

    // handler_GetViewRadius: 0x7F3B90 returns the current far-clip as a float.
    int GetViewRadius(lua_State* L)
    {
        PushNum(L, static_cast<float>(reinterpret_cast<float(__cdecl*)()>(0x7F3B90)()));
        return 1;
    }

    // ---- friend status ----------------------------------------------------------------------------
    // FUN_10a4cdb0: remember the choice and tell the server, CMSG 0x671 { u8 }.
    int FriendStatusSetOffline(lua_State* L)
    {
        g_friendStatusOffline = AscLua::lua_toboolean(L, 1) != 0;
        Packet(0x671).U8(g_friendStatusOffline).Send();
        return 0;
    }
    int FriendStatusIsOffline(lua_State* L) { PushBool(L, g_friendStatusOffline != 0); return 1; }
    // handler_0x0672 (0x10A4F4D0): the server's stored choice, one byte, straight into the same flag.
    void __cdecl OnFriendStatusOffline(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_friendStatusOffline = pkt->m_buffer[pkt->m_read++];
    }

    // ---- taxi early landing -------------------------------------------------------------------------
    // handler_TaxiRequestEarlyLanding / TaxiCancelEarlyLanding: empty CMSG 0x5F2 / 0x9C6 (FUN_100e08b0).
    int TaxiRequestEarlyLanding(lua_State*) { Packet(0x5F2).Send(); g_taxiEarlyLanding = 1; return 0; }
    int TaxiCancelEarlyLanding(lua_State*)  { Packet(0x9C6).Send(); g_taxiEarlyLanding = 0; return 0; }
    int TaxiIsEarlyLanding(lua_State* L)    { PushBool(L, g_taxiEarlyLanding != 0); return 1; }

    // ---- loot specialization -------------------------------------------------------------------------
    // handler_0x067a SMSG_SET_LOOT_SPECIALIZATION { u32 } -> PLAYER_LOOT_SPEC_UPDATED
    void __cdecl OnSetLootSpecialization(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_lootSpecialization = ReadU32(pkt);
        AscRuntime::Signal("PLAYER_LOOT_SPEC_UPDATED");
    }
    int GetLootSpecialization(lua_State* L) { PushInt(L, static_cast<int32_t>(g_lootSpecialization)); return 1; }

    // ---- server time --------------------------------------------------------------------------------
    // FUN_10a4f500 REPLACES the client's SMSG_SERVERTIME (0x49) handler at 0x7E2A50 outright (the hook
    // never calls the trampoline). It skips the first u32, takes the second as server epoch time and
    // stores its distance from local time, corrected for the local timezone.
    int __cdecl OnServerTime(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        pkt->m_read += 4;
        const int32_t server = static_cast<int32_t>(ReadU32(pkt));
        __time64_t now = _time64(nullptr);
        tm gm = *_gmtime64(&now);
        tm local = *_localtime64(&now);
        const __time64_t tLocal = _mktime64(&local);
        const __time64_t tGm = _mktime64(&gm);
        g_serverTimeOffset = (server - static_cast<int32_t>(now)) - (static_cast<int32_t>(tLocal) - static_cast<int32_t>(tGm));
        return 1;
    }
    int GetServerTimeOffset(lua_State* L) { PushInt(L, g_serverTimeOffset); return 1; }
    int GetServerTime(lua_State* L)       { PushInt(L, static_cast<int32_t>(_time64(nullptr)) + g_serverTimeOffset); return 1; }

    // ---- zone instances ---------------------------------------------------------------------------------
    // handler_0x0743 SMSG_INSTANCED_AREAS_LIST { u32 count, u32 current, u32 id[count] } -> ZONE_INSTANCE_LIST
    void __cdecl OnInstancedAreasList(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_zoneInstances.clear();
        const uint32_t count = ReadU32(pkt);
        const uint32_t current = ReadU32(pkt);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t id = ReadU32(pkt);
            if (id == current)
                g_zoneInstanceIndex = i;
            g_zoneInstances.push_back(id);
        }
        AscRuntime::Signal("ZONE_INSTANCE_LIST");
    }
    int GetZoneInstanceID(lua_State* L)   { PushInt(L, static_cast<int32_t>(g_zoneInstanceIndex + 1)); return 1; }
    int GetZoneInstanceList(lua_State* L) { PushInt(L, static_cast<int32_t>(g_zoneInstances.size())); return 1; }

    // FUN_10a73310: 1-based index into the list -> CMSG 0x744 { u32 id }.
    int SetZoneInstanceID(lua_State* L)
    {
        const uint32_t idx = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)) - 1);
        if (static_cast<int32_t>(idx) >= 0 && idx < g_zoneInstances.size())
            Packet(0x744).U32(g_zoneInstances[idx]).Send();
        return 0;
    }

    // ---- command line ----------------------------------------------------------------------------------
    // FUN_10195180 (on the first glue screen only, flag 0x10BDEC0B; installer FUN_101952e0):
    //   -controller v  -> gxWindow "0" when v is "1", else "1"
    //   -realm v       -> realmName v (CVar set with its last flag 1)
    //   -login l -password p -> 0xB6B474 = 1, 0xB499A4 = time(), then the client's 0x4D7F60(l) and
    //                     0x4D8A30(l, p): the launcher's automatic account login
    //   -autologin n / -renderdebug n -> atoi(n) != 0 into 0x10BDEC08 / 0x10BDEC09
    bool g_commandLineDone = false;   // 0x10BDEC0B
    bool g_autoLogin = false;         // 0x10BDEC08
    bool g_renderDebug = false;       // 0x10BDEC09

    void SetCVarValue(void* cvar, const char* value, uint32_t last)   // FUN_10114c40 -> 0x7668C0
    {
        const char* key = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(cvar) + 0x14);
        if (strlen(value) + strlen(key) > 0xF9)
            return;
        reinterpret_cast<void(__thiscall*)(void*, const char*, int, int, int, int)>(0x7668C0)(cvar, value, 1, 0, 0, last);
    }

    void ApplyCommandLine()
    {
        if (g_commandLineDone)
            return;
        g_commandLineDone = true;
        const uint32_t now = static_cast<uint32_t>(_time64(nullptr));
        if (const char* controller = CommandLineArg("controller"))
            if (CVar* gx = CVar::Lookup("gxWindow"))
                SetCVarValue(gx, strcmp(controller, "1") == 0 ? "0" : "1", 0);
        const char* realm = CommandLineArg("realm");
        if (realm && *realm)
            if (CVar* name = CVar::Lookup("realmName"))
                SetCVarValue(name, realm, 1);
        const char* login = CommandLineArg("login");
        const char* password = CommandLineArg("password");
        if (login && password)
        {
            *reinterpret_cast<uint8_t*>(0xB6B474) = 1;
            *reinterpret_cast<uint32_t*>(0xB499A4) = now;
            reinterpret_cast<void(__cdecl*)(const char*)>(0x4D7F60)(login);
            reinterpret_cast<void(__cdecl*)(const char*, const char*)>(0x4D8A30)(login, password);
        }
        if (const char* v = CommandLineArg("autologin"))
            g_autoLogin = atoi(v) != 0;
        if (const char* v = CommandLineArg("renderdebug"))
            g_renderDebug = atoi(v) != 0;
    }

    int IsAutoLogin(lua_State* L)   // handler_IsAutoLogin: DAT_10BDEC08
    {
        PushBool(L, g_autoLogin);
        return 1;
    }

    int IsRenderDebug(lua_State* L)   // handler_IsRenderDebug: DAT_10BDEC09
    {
        PushBool(L, g_renderDebug);
        return 1;
    }

    // handler_GetFileStreamingStatus: always 0.
    int GetFileStreamingStatus(lua_State* L) { PushInt(L, 0); return 1; }

    // SMSG_UPDATE_GOSSIP_ICON 0x669 (FUN_10a4fd70): clears the 32-entry table 0x10d3d758, then
    // u32 n x u32 icon. GetGossipIconOverride(i) (FUN_10a4d3f0) indexes it unchecked; out-of-range
    // indices read 0 here.
    int32_t g_gossipIcons[32] = {};
    void __cdecl OnUpdateGossipIcon(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        memset(g_gossipIcons, 0, sizeof(g_gossipIcons));
        uint32_t n;
        memcpy(&n, pkt->m_buffer + pkt->m_read, 4);
        pkt->m_read += 4;
        for (uint32_t i = 0; i < n; ++i)
        {
            int32_t v;
            memcpy(&v, pkt->m_buffer + pkt->m_read, 4);
            pkt->m_read += 4;
            if (i < 32)
                g_gossipIcons[i] = v;
        }
    }
    int GetGossipIconOverride(lua_State* L)
    {
        const int32_t i = ToInt(CheckNumber(L, 1));
        PushInt(L, (i >= 0 && i < 32) ? g_gossipIcons[i] : 0);
        return 1;
    }

    // ---- RealmInfo (FUN_1008e2d0, the object at 0x10bdb138) -----------------------------------------
    int GetRealmId(lua_State* L)                  { PushInt(L, static_cast<int32_t>(RealmInfoSvc::Get().field0)); return 1; }   // +0x04
    int GetRealmExpansion(lua_State* L)           { PushInt(L, static_cast<int32_t>(RealmInfoSvc::Get().ruleset)); return 1; } // +0x08
    int GetRealmAuctionCutRate(lua_State* L)      { PushNum(L, RealmInfoSvc::Get().rate3); return 1; }                          // +0x1C
    int GetAuctionHouseDepositRateVanity(lua_State* L) { PushNum(L, RealmInfoSvc::Get().rateAuctionDepositVanity); return 1; } // +0x20
    int HasVisitedArea52ThisSession(lua_State* L) { PushBool(L, RealmInfoSvc::Get().visitedArea52 != 0); return 1; }          // +0x49

    // ---- more exe passthroughs ------------------------------------------------------------------------
    // FUN_100d5770: rated brackets 1..3 may always be joined solo.
    int CanJoinRatedArenaWithoutGroup(lua_State* L)
    {
        const int32_t bracket = ToInt(CheckNumber(L, 1));
        PushBool(L, bracket == 1 || bracket == 2 || bracket == 3);
        return 1;
    }

    // FUN_10a4df90: stable slot n (1-based) of the client's stable list (count at 0xC0F6CC, 0x60-byte
    // entries at 0xC0F4E0, pet number first); nil past the count.
    int GetStablePetID(lua_State* L)
    {
        const int idx = static_cast<int>(CheckNumber(L, 1));
        if (*reinterpret_cast<const int32_t*>(0xC0F6CC) < idx)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushInt(L, *reinterpret_cast<const int32_t*>(0xC0F4E0 + idx * 0x60 - 0x60));
        return 1;
    }

    // handler_SetInteractDistance: writes straight into the client's interact-range table at 0xAC8A64.
    int SetInteractDistance(lua_State* L)
    {
        const int32_t idx = ToInt(CheckNumber(L, 1));
        const float dist = static_cast<float>(CheckNumber(L, 2));
        reinterpret_cast<float*>(0xAC8A64)[idx] = dist;
        return 0;
    }

    // FUN_10a4e280: the client's world-state value (0x548D10).
    int GetWorldState(lua_State* L)
    {
        const int32_t id = ToInt(CheckNumber(L, 1));
        PushInt(L, reinterpret_cast<int32_t(__cdecl*)(int32_t)>(0x548D10)(id));
        return 1;
    }

    // handler_IsKeyDown -> FUN_10308820 -> 0x47D230(key)
    int IsKeyDown(lua_State* L)
    {
        const uint32_t key = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        PushBool(L, (reinterpret_cast<uint32_t(__cdecl*)(uint32_t)>(0x47D230)(key) & 0xFF) != 0);
        return 1;
    }

    // handler_ReloadMap: the current map's Map.dbc row (client container 0xAD4160, FUN_100b1c60), then
    // 0x7BFCE0(directory, 1). Returns 1 without pushing anything, as the original does.
    int ReloadMap(lua_State*)
    {
        if (const uint8_t* map = ClientDbcRow(0xAD4160, *reinterpret_cast<const uint32_t*>(0xBD088C)))
            reinterpret_cast<void(__cdecl*)(const char*, int)>(0x7BFCE0)(*reinterpret_cast<const char* const*>(map + 4), 1);
        return 1;
    }

    // handler_FadeScreen: 0x5ED480(seconds, callback, 0); the callback (0x10a4c0a0) calls
    // 0x5ED080(seconds, 0, 0) with the remembered duration. Returns 1 without pushing.
    float g_fadeSeconds = 0;   // DAT_10d3d7e8
    void __cdecl FadeScreenDone()
    {
        reinterpret_cast<void(__cdecl*)(float, int, int)>(0x5ED080)(g_fadeSeconds, 0, 0);
    }
    int FadeScreen(lua_State* L)
    {
        const float seconds = static_cast<float>(CheckNumber(L, 1));
        reinterpret_cast<void(__cdecl*)(float, void(__cdecl*)(), int)>(0x5ED480)(seconds, FadeScreenDone, 0);
        g_fadeSeconds = seconds;
        return 1;
    }

    // ---- server flags ------------------------------------------------------------------------------------
    // FUN_100d3aa0 SMSG_CHECKPOINTS_ENABLED (0x767) { u8 }
    uint8_t g_checkpointsKnown = 0, g_checkpointsEnabled = 0;   // DAT_10bdb5ec / 5ed
    void __cdecl OnCheckpointsEnabled(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_checkpointsKnown = 1;
        g_checkpointsEnabled = pkt->m_buffer[pkt->m_read++] != 0;
    }
    int GetCheckpointsEnabled(lua_State* L)
    {
        if (!g_checkpointsKnown)
            AscLua::lua_pushnil(L);
        else
            PushBool(L, g_checkpointsEnabled != 0);
        return 1;
    }

    // FUN_100d3750 SMSG_BANK_PERMISSIONS (0x769) { u8, u8 } -> BANK_PERMISSIONS_PAYLOAD
    uint8_t g_bankKnown = 0, g_bankA = 0, g_bankB = 0;          // DAT_10bdb5ef / 5fc / 5fd
    void __cdecl OnBankPermissions(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_bankKnown = 1;
        g_bankA = pkt->m_buffer[pkt->m_read] != 0;
        g_bankB = pkt->m_buffer[pkt->m_read + 1] != 0;
        pkt->m_read += 2;
        AscRuntime::Signal("BANK_PERMISSIONS_PAYLOAD");
    }
    int GetBankPermissions(lua_State* L)
    {
        if (!g_bankKnown)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushBool(L, g_bankA != 0);
        PushBool(L, g_bankB != 0);
        return 2;
    }

    // RealmInfo +0x24 (as an unsigned number) and +0x48 (FUN_102fc620).
    int GetAuctionHouseDepositMaxVanity(lua_State* L) { PushNum(L, static_cast<double>(RealmInfoSvc::Get().maxAuctionDepositVanity)); return 1; }
    int IsAddOnsAllowed(lua_State* L)                 { PushBool(L, RealmInfoSvc::Get().trailingFlag != 0); return 1; }

    // ---- LoadADT (FUN_10112a00) and its tile-load hook (FUN_10112920 -> LAB_101129b0 over 0x7D9A20) -------
    // LoadADT(x, y, file) arms a one-shot override {x 0x10BC98A4, y 0x10BC98A8, file 0x10BC98AC} and
    // reloads the current map like ReloadMap; returns 1 with nothing pushed. When the client next loads
    // the tile whose +0x48 / +0x4C are (x, y), the hook loads `file` through 0x7D7150 (__thiscall on the tile) in place
    // of the original and disarms.
    bool g_adtArmed = false;   // DAT_10bc98a0
    int32_t g_adtX = 0, g_adtY = 0;
    std::string g_adtFile;
    typedef int(__cdecl* LoadTile_t)(void* tile);
    LoadTile_t g_loadTile = nullptr;

    int __cdecl LoadTileDetour(void* tile)
    {
        const uint8_t* t = static_cast<const uint8_t*>(tile);
        if (g_adtArmed && g_adtX == *reinterpret_cast<const int32_t*>(t + 0x48) && g_adtY == *reinterpret_cast<const int32_t*>(t + 0x4C))
        {
            const int r = reinterpret_cast<int(__thiscall*)(void*, const char*)>(0x7D7150)(tile, g_adtFile.c_str());   // LAB_101129b0: ecx = the tile
            g_adtArmed = false;
            return r;
        }
        return g_loadTile(tile);
    }

    int LoadADT(lua_State* L)
    {
        const int32_t x = ToInt(CheckNumber(L, 1));
        const int32_t y = ToInt(CheckNumber(L, 2));
        const std::string file = CheckString(L, 3);
        g_adtArmed = true;
        g_adtX = x;
        g_adtY = y;
        g_adtFile = file;
        if (const uint8_t* map = ClientDbcRow(0xAD4160, *reinterpret_cast<const uint32_t*>(0xBD088C)))
            reinterpret_cast<void(__cdecl*)(const char*, int)>(0x7BFCE0)(*reinterpret_cast<const char* const*>(map + 4), 1);
        return 1;
    }

    // ---- CreateWorldText (FUN_10a4ac90) and its position hook (LAB_10a77da0 over 0x7E6DC0) ----------------
    // CreateWorldText(text, x, y, z, scale, r, g, b [, a = 0.018] [, b2 = 0.018] [, c = 1.0]) stores the
    // position (0x10D3D7F0..F8), writes the rest into the client's combat-text tables (the pointers the
    // DLL keeps at 0x10BCC920 = 0xAF4748: +0x8C float, +0x90/+0x94/+0x98 u32, +0x9C/+0xA0 float; 0x10BCC924
    // = 0xAF487C: +0x14 float) and raises combat text type 5 on the player (0x7E6030(player +0xB0, 5,
    // text, 0, 0)). The hook substitutes the stored position for arg 2 when all three are non-zero, then
    // clears it. Returns 0. (The original dereferences the player unchecked; we skip the call without one.)
    float g_worldTextPos[3] = {};   // DAT_10d3d7f0
    typedef int(__cdecl* CombatTextAt_t)(int, float*, int, int, int);
    CombatTextAt_t g_combatTextAt = nullptr;

    int __cdecl CombatTextAtDetour(int a1, float* pos, int a3, int a4, int a5)
    {
        if (g_worldTextPos[0] != 0.0f && g_worldTextPos[1] != 0.0f && g_worldTextPos[2] != 0.0f)
        {
            pos[0] = g_worldTextPos[0];
            pos[1] = g_worldTextPos[1];
            pos[2] = g_worldTextPos[2];
        }
        const int r = g_combatTextAt(a1, pos, a3, a4, a5);
        g_worldTextPos[0] = g_worldTextPos[1] = g_worldTextPos[2] = 0.0f;
        return r;
    }

    int CreateWorldText(lua_State* L)
    {
        const std::string text = CheckString(L, 1);   // FUN_1009e280: luaL_checklstring
        uint8_t* style = reinterpret_cast<uint8_t*>(0xAF4748);
        g_worldTextPos[0] = static_cast<float>(CheckNumber(L, 2));
        g_worldTextPos[1] = static_cast<float>(CheckNumber(L, 3));
        g_worldTextPos[2] = static_cast<float>(CheckNumber(L, 4));
        *reinterpret_cast<float*>(style + 0x8C) = static_cast<float>(CheckNumber(L, 5));
        *reinterpret_cast<uint32_t*>(style + 0x90) = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 6)));
        *reinterpret_cast<uint32_t*>(style + 0x94) = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 7)));
        *reinterpret_cast<uint32_t*>(style + 0x98) = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 8)));
        *reinterpret_cast<float*>(style + 0x9C) = AscLua::lua_type(L, 9) > 0 ? static_cast<float>(CheckNumber(L, 9)) : 0.018f;
        *reinterpret_cast<float*>(style + 0xA0) = AscLua::lua_type(L, 10) > 0 ? static_cast<float>(CheckNumber(L, 10)) : 0.018f;
        *reinterpret_cast<float*>(0xAF487C + 0x14) = AscLua::lua_type(L, 11) > 0 ? static_cast<float>(CheckNumber(L, 11)) : 1.0f;
        if (uint8_t* player = ActivePlayer())
            reinterpret_cast<void(__cdecl*)(uint32_t, int, const char*, int, int)>(0x7E6030)(
                *reinterpret_cast<const uint32_t*>(player + 0xB0), 5, text.c_str(), 0, 0);
        return 0;
    }

    // FUN_101952e0 (the installer), after registering FUN_10195180: "-config v" replaces the file name the
    // client loads its settings from -- the "Config.wtf" pushed at 0x406850 (push imm32, operand 0x406851,
    // before the load 0x768340) is repointed at the DLL's string (0x10BCA060, initially "Config.wtf").
    // The pointer is written before the protection result is checked; on success the old protection is
    // put back.
    std::string g_configFile = "Config.wtf";   // 0x10BCA060

    void ApplyConfigFile()
    {
        const char* config = CommandLineArg("config");
        if (!config)
            return;
        g_configFile = config;
        void* at = reinterpret_cast<void*>(0x406851);
        DWORD old;
        const BOOL ok = VirtualProtect(at, 4, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<const char**>(at) = g_configFile.c_str();
        if (!ok)
            return;
        VirtualProtect(at, 4, old, &old);
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&ApplyCommandLine);
        ApplyConfigFile();
        g_combatTextAt = reinterpret_cast<CombatTextAt_t>(AscRuntime::Detour(0x7E6DC0, 7, reinterpret_cast<void*>(&CombatTextAtDetour)));
        g_loadTile = reinterpret_cast<LoadTile_t>(AscRuntime::Detour(0x7D9A20, 9, reinterpret_cast<void*>(&LoadTileDetour)));
        sDC.AddPacketHandler(0x672, CNetClientCustomPacket((void*)&OnFriendStatusOffline, nullptr));
        sDC.AddPacketHandler(0x767, CNetClientCustomPacket((void*)&OnCheckpointsEnabled, nullptr));
        sDC.AddPacketHandler(0x769, CNetClientCustomPacket((void*)&OnBankPermissions, nullptr));
        sDC.AddPacketHandler(0x669, CNetClientCustomPacket((void*)&OnUpdateGossipIcon, nullptr));
        sDC.AddPacketHandler(0x67A, CNetClientCustomPacket((void*)&OnSetLootSpecialization, nullptr));
        sDC.AddPacketHandler(0x743, CNetClientCustomPacket((void*)&OnInstancedAreasList, nullptr));
        AscRuntime::ReplaceFunction(0x7E2A50, reinterpret_cast<void*>(&OnServerTime));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetZoneId", GetZoneId},
        {nullptr, "GetAreaId", GetAreaId},
        {"#Minimap", "IsInside", IsInside},
        {nullptr, "GetActiveMapID", GetActiveMapID},
        {nullptr, "GetBindPosition", GetBindPosition},
        {"#Minimap", "GetViewRadius", GetViewRadius},
        {nullptr, "FriendStatusSetOffline", FriendStatusSetOffline},
        {nullptr, "FriendStatusIsOffline", FriendStatusIsOffline},
        {nullptr, "TaxiRequestEarlyLanding", TaxiRequestEarlyLanding},
        {nullptr, "TaxiCancelEarlyLanding", TaxiCancelEarlyLanding},
        {nullptr, "TaxiIsEarlyLanding", TaxiIsEarlyLanding},
        {nullptr, "GetLootSpecialization", GetLootSpecialization},
        {nullptr, "GetServerTimeOffset", GetServerTimeOffset},
        {nullptr, "GetServerTime", GetServerTime},
        {nullptr, "GetZoneInstanceID", GetZoneInstanceID},
        {nullptr, "GetZoneInstanceList", GetZoneInstanceList},
        {nullptr, "SetZoneInstanceID", SetZoneInstanceID},
        {nullptr, "IsAutoLogin", IsAutoLogin},
        {nullptr, "IsRenderDebug", IsRenderDebug},
        {nullptr, "GetFileStreamingStatus", GetFileStreamingStatus},
        {nullptr, "GetGossipIconOverride", GetGossipIconOverride},
        {nullptr, "GetRealmId", GetRealmId},
        {nullptr, "GetRealmExpansion", GetRealmExpansion},
        {nullptr, "GetRealmAuctionCutRate", GetRealmAuctionCutRate},
        {nullptr, "GetAuctionHouseDepositRateVanity", GetAuctionHouseDepositRateVanity},
        {nullptr, "HasVisitedArea52ThisSession", HasVisitedArea52ThisSession},
        {nullptr, "CanJoinRatedArenaWithoutGroup", CanJoinRatedArenaWithoutGroup},
        {nullptr, "GetStablePetID", GetStablePetID},
        {nullptr, "SetInteractDistance", SetInteractDistance},
        {nullptr, "GetWorldState", GetWorldState},
        {nullptr, "IsKeyDown", IsKeyDown},
        {nullptr, "ReloadMap", ReloadMap},
        {nullptr, "LoadADT", LoadADT},
        {nullptr, "CreateWorldText", CreateWorldText},
        {nullptr, "FadeScreen", FadeScreen},
        {nullptr, "GetCheckpointsEnabled", GetCheckpointsEnabled},
        {nullptr, "GetBankPermissions", GetBankPermissions},
        {nullptr, "GetAuctionHouseDepositMaxVanity", GetAuctionHouseDepositMaxVanity},
        {nullptr, "IsAddOnsAllowed", IsAddOnsAllowed},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
