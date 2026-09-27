// More GLOBAL bindings, each from its decompile (ghidra-ext/out/bindings/<Name>.c) together with the
// client hook or patch that consumes the state it sets: the combat-log filter, the script error
// handler's stack depth, nameplate cast bars and game-object fading.
//
//   CombatLogDisableAll / CombatLogDisable(event)  byte 0x10D3D749 / vector 0x10D3D74C, read by the
//        detour FUN_10a3d090 over 0x74F910 (the COMBAT_LOG_EVENT dispatch; hook object 0x10BCCBE4,
//        installed from FUN_10a60cd0). Nothing ever clears either.
//   GetErrorCallstackHeight   0x10BE3088, counted by FUN_10296df0, which REPLACES the client's script
//        error function 0x819730 (hook object 0x10BCB934, never calls the original).
//   SetNamePlateCastBarMode   FUN_102c1000: NOPs / restores three client code sites.
//   SetFadeOutGameObjects     byte 0x10BCC8D9 (initially 1), returned by LAB_10a437b0, which
//        FUN_10a3d840 writes into the game-object vtable slot 0x9F3A88 (stock: 0x8C8DE0, return 1).
//   GetCharacterSelectionMailData / GameModeData   SMSG 0x770 / 0x771 (replacing the log-only
//        handlers CharSelectOpcodes.cpp had).
//   GM_GetTransactionIdList / Content   SMSG 0x9B7 created / 0x9B8 closed / 0x9B9 whole list.
//   GetWeaponTempEnchantInfo, GetRangedWeaponEnchantInfo, GetEquipmentSetItemGUIDs, and the three
//   Custom_HandleTerrainClick* (the client's terrain-click 0x80C340).
//   SetAttackCursor / SetAttackTexture / SetRangedAttackTexture / SetAutoRangedCombatSpell /
//   SetAttackAnimation / SetAttackDistance, with the two detours (0x7385C0, 0x4F5F40) that read them.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscAddons.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscLootLockout.hpp>
#include <Ascension/AscMythicPlus.hpp>
#include <Ascension/AscQuestAddon.hpp>
#include <Ascension/AscMysticEnchant.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellText.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Windows.h>
#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <cstring>
#include <share.h>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

extern void* g_ascCursorTable[0x35 * 5];   // AscCursor.cpp, 0x10D3C7A8

namespace
{
    void WriteCode(uint32_t address, const void* bytes, size_t n)
    {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(address), n, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(reinterpret_cast<void*>(address), bytes, n);
            VirtualProtect(reinterpret_cast<void*>(address), n, old, &old);
        }
    }
    void FillCode(uint32_t address, uint8_t value, size_t n)
    {
        std::vector<uint8_t> v(n, value);
        WriteCode(address, v.data(), n);
    }

    // ---- combat log filter ---------------------------------------------------------------------------
    bool g_combatLogDisableAll = false;          // 0x10D3D749
    std::vector<uint32_t> g_combatLogDisabled;   // 0x10D3D74C

    int CombatLogDisableAll(lua_State*)
    {
        g_combatLogDisableAll = true;
        return 0;
    }

    int CombatLogDisable(lua_State* L)
    {
        g_combatLogDisabled.push_back(static_cast<uint32_t>(ToInt(CheckNumber(L, 1))));
        return 0;
    }

    // FUN_10a3d090: the entry being dispatched is [0xCA1394]; +0x0C its event index, +0x44 its spell.
    // Periodic events (0x0A / 0x23 and 0x09 / 0x22) of a spell flagged 4 in SpellCustomAttr +0x14 are
    // dropped when +0x58 == +0x5C (resp. +0x5C == +0x60); then any disabled event is dropped.
    bool __cdecl CombatLogAllowed()
    {
        if (g_combatLogDisableAll)
            return false;
        const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(0xCA1394);
        const uint32_t type = *reinterpret_cast<const uint32_t*>(e + 0xC);
        auto u32 = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(e + off); };
        auto quiet = [&]() {
            const uint8_t* attr = AscCA::SpellCustomAttrRow(u32(0x44));
            return attr && (*reinterpret_cast<const uint32_t*>(attr + 0x14) & 4) != 0;
        };
        if ((type == 0xA || type == 0x23) && quiet() && u32(0x58) == u32(0x5C))
            return false;
        if ((type == 9 || type == 0x22) && quiet() && u32(0x5C) == u32(0x60))
            return false;
        return std::find(g_combatLogDisabled.begin(), g_combatLogDisabled.end(), type) == g_combatLogDisabled.end();
    }

    void* g_74F910Trampoline = nullptr;
    __declspec(naked) void Detour74F910()
    {
        __asm {
            call CombatLogAllowed
            test al, al
            jz drop
            jmp dword ptr [g_74F910Trampoline]
        drop:
            ret
        }
    }

    // ---- script error handler ------------------------------------------------------------------------
    int32_t g_errorCallstackHeight = 0;   // 0x10BE3088

    int GetErrorCallstackHeight(lua_State* L)
    {
        PushInt(L, g_errorCallstackHeight);
        return 1;
    }

    // FUN_10296df0: the client's 0x819730, plus the lua_getstack count while the handler runs.
    int __cdecl ScriptErrorHandler(lua_State* L)
    {
        typedef int(__cdecl * IsString_t)(lua_State*, int);
        typedef void(__cdecl * PushString_t)(lua_State*, const char*);
        typedef void(__cdecl * Idx_t)(lua_State*, int);
        typedef const char*(__cdecl * ToString_t)(lua_State*, int, size_t*);
        typedef const char*(__cdecl * ScriptFile_t)(lua_State*, int);
        typedef void(__cdecl * PushLString_t)(lua_State*, const char*, size_t);
        typedef int(__cdecl * GetStack_t)(lua_State*, int, void*);
        typedef void(__cdecl * RawGetI_t)(lua_State*, int, int);
        typedef void(__cdecl * Call_t)(lua_State*, int, int);
        const PushString_t pushstring = reinterpret_cast<PushString_t>(0x84E350);

        if (!reinterpret_cast<IsString_t>(0x84DF60)(L, -1))
        {
            pushstring(L, "UNKNOWN ERROR");
            reinterpret_cast<Idx_t>(0x84DCC0)(L, -1);
        }
        const char* msg = reinterpret_cast<ToString_t>(0x84E0E0)(L, -1, nullptr);
        const char* mark = strstr(msg, "*:");
        const char* file = reinterpret_cast<ScriptFile_t>(0x817DE0)(L, 1);
        if (mark && file)
        {
            reinterpret_cast<PushLString_t>(0x84E300)(L, msg, static_cast<size_t>(mark - msg));
            pushstring(L, file);
            pushstring(L, mark + 1);
            reinterpret_cast<Idx_t>(0x84EF90)(L, 3);    // lua_concat
            reinterpret_cast<Idx_t>(0x84DD70)(L, -2);
        }
        const int32_t ref = *reinterpret_cast<const int32_t*>(0xAF5770);
        if (ref != -1)
        {
            uint8_t ar[100];
            const GetStack_t getstack = reinterpret_cast<GetStack_t>(0x84FE40);
            while (getstack(L, g_errorCallstackHeight, ar))
                ++g_errorCallstackHeight;
            *reinterpret_cast<int32_t*>(0xD3F79C) = 1;
            reinterpret_cast<RawGetI_t>(0x84E670)(L, -10000, ref);   // LUA_REGISTRYINDEX
            reinterpret_cast<Idx_t>(0x84DCC0)(L, -2);
            reinterpret_cast<Call_t>(0x84EBF0)(L, 1, 1);
            g_errorCallstackHeight = 0;
            *reinterpret_cast<int32_t*>(0xD3F79C) = 0;
        }
        return 1;
    }

    // ---- nameplate cast bars -------------------------------------------------------------------------
    // FUN_102c1000(mode != 0): each site's original bytes; on = NOPs, off = these written back.
    const uint8_t kSite720E68[] = {   // the unit-is-the-player GUID test before a cast bar
        0x8B, 0x46, 0x08, 0x8B, 0x08, 0x3B, 0x0D, 0xB0, 0x07, 0xBD, 0x00, 0x0F, 0x85, 0xFE, 0x00, 0x00,
        0x00, 0x8B, 0x50, 0x04, 0x3B, 0x15, 0xB4, 0x07, 0xBD, 0x00, 0x0F, 0x85, 0xEF, 0x00, 0x00, 0x00,
    };
    const uint8_t kSite524DC1[] = {0xE8, 0x5A, 0x60, 0x34, 0x00, 0x50, 0x8B, 0xCE, 0xE8, 0x82, 0xC0, 0x1F, 0x00};
    const uint8_t kSite524276[] = {0x8B, 0xCE, 0xE8, 0xA3, 0x18, 0x1F, 0x00};

    int SetNamePlateCastBarMode(lua_State* L)
    {
        const bool on = ToInt(CheckNumber(L, 1)) != 0;
        auto apply = [&](uint32_t address, const uint8_t* bytes, size_t n) {
            if (on)
                FillCode(address, 0x90, n);
            else
                WriteCode(address, bytes, n);
        };
        apply(0x720E68, kSite720E68, sizeof(kSite720E68));
        apply(0x524DC1, kSite524DC1, sizeof(kSite524DC1));
        apply(0x524276, kSite524276, sizeof(kSite524276));
        return 0;
    }

    // ---- game-object fading --------------------------------------------------------------------------
    uint8_t g_fadeOutGameObjects = 1;   // 0x10BCC8D9

    int __cdecl FadeOutGameObjects() { return g_fadeOutGameObjects; }   // LAB_10a437b0

    // Tests the argument's TYPE, not its value: nil, a boolean (either) or a number turn fading on.
    int SetFadeOutGameObjects(lua_State* L)
    {
        const int type = AscLua::lua_type(L, 1);
        g_fadeOutGameObjects = (type == 0 || type == 1 || type == 3) ? 1 : 0;
        return 0;
    }

    // ---- packet readers ------------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }
    std::string ReadSized(CDataStore* p)   // FUN_100d3680: u32 length, then the bytes
    {
        const uint32_t n = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += n;
        return s;
    }

    // ---- character select ----------------------------------------------------------------------------
    // Maps 0x10BDB5FC / 0x10BDB61C, keyed by the character's list index; never cleared.
    struct SelectionMail { bool mail = false, storeMail = false; };
    struct SelectionGameMode { uint32_t a = 0, b = 0, c = 0; bool d = false; uint8_t e = 0; };
    std::unordered_map<uint32_t, SelectionMail> g_selectionMail;
    std::unordered_map<uint32_t, SelectionGameMode> g_selectionGameMode;

    void __cdecl OnSelectionMail(void*, uint32_t, uint32_t, CDataStore* p)   // handler_0x0770
    {
        const uint32_t index = Read<uint32_t>(p);
        SelectionMail& m = g_selectionMail[index];
        m.mail = Read<uint8_t>(p) != 0;
        m.storeMail = Read<uint8_t>(p) != 0;
    }

    void __cdecl OnSelectionGameMode(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_100d3910
    {
        const uint32_t index = Read<uint32_t>(p);
        SelectionGameMode& g = g_selectionGameMode[index];
        g.a = Read<uint32_t>(p);
        g.b = Read<uint32_t>(p);
        g.c = Read<uint32_t>(p);
        g.d = Read<uint8_t>(p) != 0;
        g.e = Read<uint8_t>(p);
    }

    int GetCharacterSelectionMailData(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index))
            return 0;
        auto it = g_selectionMail.find(index);
        if (it == g_selectionMail.end())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushBool(L, it->second.mail);
        PushBool(L, it->second.storeMail);
        return 2;
    }

    int GetCharacterSelectionGameModeData(lua_State* L)
    {
        uint32_t index;
        if (!ReadNumber(L, index))
            return 0;
        auto it = g_selectionGameMode.find(index);
        if (it == g_selectionGameMode.end())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const SelectionGameMode& g = it->second;
        PushInt(L, static_cast<int32_t>(g.a));
        PushInt(L, static_cast<int32_t>(g.b));
        PushInt(L, static_cast<int32_t>(g.c));
        PushBool(L, g.d);
        PushInt(L, g.e);
        return 5;
    }

    // ---- GM transactions (TransactionSystemMgr, FUN_10a13e80 -> 0x10D3C570) --------------------------
    // Record (FUN_10a13fa0): u32 id (+0xC, the key), u32 (+0x10), sized string (+0x14), u32 (+0x2C),
    // sized string (+0x30), u32 (+0x48), u32 kept as a byte (+0x4C), four u32 (+0x50..+0x5C).
    struct Transaction
    {
        uint32_t id = 0, b = 0;
        std::string s1;
        uint32_t c = 0;
        std::string s2;
        uint32_t d = 0;
        uint8_t e = 0;
        uint32_t f[4] = {0, 0, 0, 0};
    };
    std::unordered_map<uint32_t, Transaction> g_transactions;
    std::vector<uint32_t> g_transactionOrder;   // the map's iteration order (insertion)

    uint32_t ParseTransaction(CDataStore* p)
    {
        Transaction t;
        t.id = Read<uint32_t>(p);
        t.b = Read<uint32_t>(p);
        t.s1 = ReadSized(p);
        t.c = Read<uint32_t>(p);
        t.s2 = ReadSized(p);
        t.d = Read<uint32_t>(p);
        t.e = static_cast<uint8_t>(Read<uint32_t>(p));
        for (uint32_t& v : t.f)
            v = Read<uint32_t>(p);
        if (!g_transactions.count(t.id))
            g_transactionOrder.push_back(t.id);
        g_transactions[t.id] = t;
        return t.id;
    }

    void __cdecl OnTransactionCreated(void*, uint32_t, uint32_t, CDataStore* p)   // handler_0x09b7
    {
        const uint32_t id = ParseTransaction(p);
        AscRuntime::Signal("ASCENSION_CUSTOM_GM_TRANSACTION_CREATED", "%u", id);
        AscRuntime::Signal("ASCENSION_CUSTOM_GM_TRANSACTION_LIST_UPDATED", "%b", 0);
    }

    // The original fires "GM_TRANSACTION_REMOVED", which no table registers, so nothing is signalled;
    // the registered ASCENSION_CUSTOM_GM_TRANSACTION_REMOVED is never fired. Kept.
    void __cdecl OnTransactionClosed(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10a142e0
    {
        const uint32_t id = static_cast<uint32_t>(Read<uint64_t>(p));
        if (!g_transactions.count(id))
            return;
        g_transactions.erase(id);
        g_transactionOrder.erase(std::remove(g_transactionOrder.begin(), g_transactionOrder.end(), id), g_transactionOrder.end());
        AscRuntime::Signal("GM_TRANSACTION_REMOVED", "%u", id);
    }

    void __cdecl OnTransactionList(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10a14590
    {
        g_transactions.clear();
        g_transactionOrder.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            ParseTransaction(p);
        AscRuntime::Signal("ASCENSION_CUSTOM_GM_TRANSACTION_LIST_UPDATED", "%b", 1);
    }

    int GM_GetTransactionIdList(lua_State* L)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        int32_t i = 0;
        for (uint32_t id : g_transactionOrder)
        {
            PushNum(L, static_cast<double>(++i));
            PushInt(L, static_cast<int32_t>(id));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GM_GetTransactionContent(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        auto it = g_transactions.find(id);
        if (it == g_transactions.end())
            return 0;
        const Transaction& t = it->second;
        PushInt(L, static_cast<int32_t>(t.id));
        PushInt(L, static_cast<int32_t>(t.b));
        PushStr(L, t.s1.c_str());
        PushInt(L, static_cast<int32_t>(t.c));
        PushStr(L, t.s2.c_str());
        PushInt(L, static_cast<int32_t>(t.d));
        PushInt(L, t.e);
        for (uint32_t v : t.f)
            PushInt(L, static_cast<int32_t>(v));
        return 0xB;
    }

    // ---- weapon enchants -----------------------------------------------------------------------------
    // SpellItemEnchantment.dbc (client container 0xAD48B0, FUN_100b24a0); +0x38 its name.
    const uint8_t* EnchantRow(uint32_t id) { return ClientDbcRow(0xAD48B0, id); }
    const uint8_t* ItemDescriptor(uint64_t guid)
    {
        void* item = ObjectPtr(guid, 2);
        return item ? *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(item) + 8) : nullptr;
    }

    // GetWeaponTempEnchantInfo(slot): the item in inventory slot `slot` (player descriptor +0x508 +
    // slot*8); its temporary enchantment (item descriptor +0x64) -> name, id.
    int GetWeaponTempEnchantInfo(lua_State* L)
    {
        const int32_t slot = ToInt(CheckNumber(L, 1));
        if (slot == 0)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        if (uint8_t* player = ActivePlayer())
        {
            const uint8_t* desc = *reinterpret_cast<const uint8_t* const*>(player + 8);
            if (const uint8_t* item = ItemDescriptor(*reinterpret_cast<const uint64_t*>(desc + 0x508 + slot * 8)))
            {
                const uint32_t enchant = *reinterpret_cast<const uint32_t*>(item + 100);
                if (const uint8_t* row = EnchantRow(enchant))
                {
                    PushStr(L, *reinterpret_cast<const char* const*>(row + 0x38));
                    PushInt(L, static_cast<int32_t>(enchant));
                    return 2;
                }
            }
        }
        return PushNils(L, 2);
    }

    // The ranged slot (player descriptor +0x598): has-enchant (1), then the enchantment's duration and
    // charges (item descriptor +0x68 / +0x6C, nil when 0).
    int GetRangedWeaponEnchantInfo(lua_State* L)
    {
        if (uint8_t* player = ActivePlayer())
        {
            const uint8_t* desc = *reinterpret_cast<const uint8_t* const*>(player + 8);
            if (const uint8_t* item = ItemDescriptor(*reinterpret_cast<const uint64_t*>(desc + 0x598)))
            {
                const uint32_t enchant = *reinterpret_cast<const uint32_t*>(item + 100);
                if (EnchantRow(enchant))
                {
                    const int32_t duration = *reinterpret_cast<const int32_t*>(item + 0x68);
                    const int32_t charges = *reinterpret_cast<const int32_t*>(item + 0x6C);
                    PushInt(L, enchant != 0 ? 1 : 0);
                    if (duration == 0)
                        AscLua::lua_pushnil(L);
                    else
                        PushInt(L, duration);
                    if (charges == 0)
                        AscLua::lua_pushnil(L);
                    else
                        PushInt(L, charges);
                    return 3;
                }
            }
        }
        return PushNils(L, 3);
    }

    // GetEquipmentSetItemGUIDs(name): the set's 19 item GUIDs (set +0x198) as the client formats them
    // (0x74D0D0), keyed 1..19. Returns nothing for an unknown set or an empty name.
    int GetEquipmentSetItemGUIDs(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        if (name.empty())
            return 0;
        typedef const uint8_t*(__cdecl * FindSet_t)(const char*);
        const uint8_t* set = reinterpret_cast<FindSet_t>(0x5AE600)(name.c_str());
        if (!set)
            return 0;
        AscLua::lua_createtable(L, 0, 0);
        typedef void(__cdecl * GuidString_t)(uint32_t, uint32_t, char*);
        for (int32_t i = 0; i < 0x13; ++i)
        {
            const uint32_t lo = *reinterpret_cast<const uint32_t*>(set + 0x198 + i * 8);
            const uint32_t hi = *reinterpret_cast<const uint32_t*>(set + 0x19C + i * 8);
            AscLua::lua_checkstack(L, 2);
            PushNum(L, static_cast<double>(i + 1));
            char buf[24];
            reinterpret_cast<GuidString_t>(0x74D0D0)(lo, hi, buf);
            PushStr(L, buf);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // ---- terrain click (0x80C340 {u64 guid, float x, y, z, u32 flags}) -------------------------------
    struct TerrainClick { uint64_t guid; float pos[3]; uint32_t flags; };
    void ClickTerrain(const float* pos)
    {
        TerrainClick c{0, {pos[0], pos[1], pos[2]}, 1};
        reinterpret_cast<char(__cdecl*)(TerrainClick*)>(0x80C340)(&c);
    }

    // FUN_10307950 / FUN_10307d20: the client's 0x5FC6C0 / 0x5FC730 (camera-or-select start / stop)
    // with the push-immediate at +8 / +0xB patched to 200 around the call.
    void CallWithPatchedPush(uint32_t fn, uint32_t immediate, lua_State* L)
    {
        uint8_t v = 200;
        WriteCode(immediate, &v, 1);
        reinterpret_cast<int(__cdecl*)(lua_State*)>(fn)(L);
        v = 0;
        WriteCode(immediate, &v, 1);
    }

    // At the world frame's cursor terrain point (CGWorldFrame [0xB7436C] +0x2E8).
    int Custom_HandleTerrainClick(lua_State* L)
    {
        CallWithPatchedPush(0x5FC6C0, 0x5FC6C8, L);
        const uint8_t* worldFrame = *reinterpret_cast<const uint8_t* const*>(0xB7436C);
        ClickTerrain(reinterpret_cast<const float*>(worldFrame + 0x2E8));
        CallWithPatchedPush(0x5FC730, 0x5FC73B, L);
        return 0;
    }

    typedef void(__thiscall * Position_t)(void*, float*);
    bool ObjectPosition(void* object, float* pos)
    {
        if (!object)
            return false;
        reinterpret_cast<Position_t>((*reinterpret_cast<void***>(object))[0x2C / 4])(object, pos);
        return true;
    }

    int Custom_HandleTerrainClick_PlayerPos(lua_State*)
    {
        float pos[3];
        if (ObjectPosition(ActivePlayer(), pos))
            ClickTerrain(pos);
        return 0;
    }

    int Custom_HandleTerrainClick_TargetPos(lua_State*)
    {
        if (!ActivePlayer())
            return 0;
        float pos[3];
        if (ObjectPosition(ObjectPtr(*reinterpret_cast<const uint64_t*>(0xBD07B0), 0xFFFF), pos))
            ClickTerrain(pos);
        return 0;
    }

    // ---- attack cursor, textures, animation, distance ---------------------------------------------------
    // SetAttackCursor(name) (FUN_10a1edf0): "Interface\Cursor\<name>" and "...\Unable<name>" into slot 0,
    // "<name><n>" / "Unable<name><n>" for n = 2 and 4, through the client's texture load 0x4B9760
    // (flags 0x201, status 0xAF5718). The slots are cursor indices 4 (Attack, 0x10D3C7F8) and 30
    // (UnableAttack, 0x10D3CA00) of the cursor module's table (AscCursor.cpp), read by its software cursor.
    void** const g_attackCursor = &g_ascCursorTable[4 * 5];         // 0x10D3C7F8
    void** const g_attackCursorUnable = &g_ascCursorTable[30 * 5];  // 0x10D3CA00

    int SetAttackCursor(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        typedef void*(__cdecl * LoadTexture_t)(const char*, uint32_t, void*, int);
        const LoadTexture_t load = reinterpret_cast<LoadTexture_t>(0x4B9760);
        void* const status = reinterpret_cast<void*>(0xAF5718);
        char path[260], unable[260];
        sprintf_s(path, "Interface\\Cursor\\%s", name.c_str());
        sprintf_s(unable, "Interface\\Cursor\\Unable%s", name.c_str());
        g_attackCursor[0] = load(path, 0x201, status, 1);
        g_attackCursorUnable[0] = load(unable, 0x201, status, 1);
        for (uint32_t n : {2u, 4u})
        {
            sprintf_s(path, "Interface\\Cursor\\%s%u", name.c_str(), n);
            sprintf_s(unable, "Interface\\Cursor\\Unable%s%u", name.c_str(), n);
            g_attackCursor[n] = load(path, 0x201, status, 1);
            g_attackCursorUnable[n] = load(unable, 0x201, status, 1);
        }
        return 0;
    }

    // SetAttackTexture(on) / SetRangedAttackTexture(on): three conditional jumps in the client's
    // attack-cursor logic. On restores the stock opcode; off makes each an unconditional jmp (0xEB).
    void PatchJumps(const uint32_t (&sites)[3], uint8_t stock, bool on)
    {
        const uint8_t op = on ? stock : 0xEB;
        for (uint32_t site : sites)
            WriteCode(site, &op, 1);
    }
    int SetAttackTexture(lua_State* L)
    {
        static const uint32_t kSites[3] = {0x53DC8A, 0x540B66, 0x5209A8};   // jne
        PatchJumps(kSites, 0x75, AscLua::lua_toboolean(L, 1) != 0);
        return 0;
    }
    int SetRangedAttackTexture(lua_State* L)
    {
        static const uint32_t kSites[3] = {0x53DCAC, 0x540B76, 0x5209B8};   // je
        PatchJumps(kSites, 0x74, AscLua::lua_toboolean(L, 1) != 0);
        return 0;
    }

    // SetAutoRangedCombatSpell(spell): the client's auto-shot spell id at 0xBE5D84.
    int SetAutoRangedCombatSpell(lua_State* L)
    {
        const int32_t spell = ToInt(CheckNumber(L, 1));
        WriteCode(0xBE5D84, &spell, 4);
        return 0;
    }

    // SetAttackAnimation(anim) (0x10D3D810): the detour 0x10a45c10 over 0x7385C0 (__fastcall(unit, edx,
    // anim, b), ret 8; hook object 0x10BCD0EC) substitutes it for the anim argument when the unit is
    // the active player and the call comes from 0x755206 (return address 0x75520B), unless it is 0.
    int32_t g_attackAnimation = 0;
    void* g_7385C0Trampoline = nullptr;
    __declspec(naked) void Detour7385C0()
    {
        __asm {
            push esi
            push edi
            mov edi, edx
            mov esi, ecx
            mov eax, 0x4038F0
            call eax
            mov ecx, dword ptr [esp + 0xC]
            cmp esi, eax
            jne pass
            cmp dword ptr [esp + 8], 0x75520B
            jne pass
            mov eax, dword ptr [g_attackAnimation]
            test eax, eax
            cmovne ecx, eax
        pass:
            push dword ptr [esp + 0x10]
            push ecx
            mov ecx, esi
            mov edx, edi
            call dword ptr [g_7385C0Trampoline]
            pop edi
            pop esi
            ret 8
        }
    }

    int SetAttackAnimation(lua_State* L)
    {
        g_attackAnimation = ToInt(CheckNumber(L, 1));
        return 0;
    }

    // SetAttackDistance(yards) (0x10D3D7EC): the detour 0x10a79040 over 0x4F5F40 (a float result,
    // ret 4; hook object 0x10BCCADC) returns it instead when called from 0x4F7FB1 (return address
    // 0x4F7FB6) and it is not 0.
    float g_attackDistance = 0.0f;
    const float kZero = 0.0f;
    void* g_4F5F40Trampoline = nullptr;
    __declspec(naked) void Detour4F5F40()
    {
        __asm {
            cmp dword ptr [esp], 0x4F7FB6
            jne original
            movss xmm0, dword ptr [g_attackDistance]
            ucomiss xmm0, dword ptr [kZero]
            lahf
            test ah, 0x44
            jnp original
            fld dword ptr [g_attackDistance]
            ret 4
        original:
            jmp dword ptr [g_4F5F40Trampoline]
        }
    }

    int SetAttackDistance(lua_State* L)
    {
        g_attackDistance = static_cast<float>(CheckNumber(L, 1));
        return 0;
    }

    // ---- GetSpellData / CanPerformAction ---------------------------------------------------------------
    // GetSpellData(spell [, a, b]) (FUN_10a4da30): accepts {number, boolean, boolean}, then
    // {number, boolean} (the boolean is read and ignored), then {number}. Pushes the Spell.dbc name
    // (+0x220), the description (FUN_101ac910(spell, a, 0, b) = AscSpellText::Description) and the
    // SpellIcon path (+0x214 -> SpellIcon.dbc row +4, default the question mark). Nothing for an
    // unknown spell.
    int GetSpellData(lua_State* L)
    {
        uint32_t spell = 0;
        bool a = false, b = false;
        if (ValidateInput(L, {AscScript::NUMBER, AscScript::BOOLEAN, AscScript::BOOLEAN}))
        {
            spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            a = AscLua::lua_toboolean(L, 2) != 0;
            b = AscLua::lua_toboolean(L, 3) != 0;
        }
        else if (ValidateInput(L, {AscScript::NUMBER, AscScript::BOOLEAN}))
        {
            spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            AscLua::lua_toboolean(L, 2);
        }
        else if (!ReadNumber(L, spell))
            return 0;
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return 0;
        std::string icon = "Interface\\Icons\\INV_Misc_QuestionMark";
        if (const uint8_t* row = ClientDbcRow(0xAD488C, *reinterpret_cast<const uint32_t*>(rec + 0x214)))
            icon = *reinterpret_cast<const char* const*>(row + 4);
        PushStr(L, *reinterpret_cast<const char* const*>(rec + 0x220));
        PushStr(L, AscSpellText::Description(spell, a, false, b ? 1 : 0).c_str());
        PushStr(L, icon.c_str());
        return 3;
    }

    // CanPerformAction(action) (FUN_10312e90): action 0x19 is "inside the client's Lua function
    // 0x510B30", a flag (0x10BE3F60) the detour 0x10313920 sets around the original call; every other
    // action is the client's own gate 0x5191C0.
    int32_t g_insideProtectedCall = 0;
    typedef int(__cdecl* Fn510B30_t)(lua_State*);
    Fn510B30_t g_510B30 = nullptr;
    int __cdecl Detour510B30(lua_State* L)
    {
        g_insideProtectedCall = 1;
        const int r = g_510B30(L);
        g_insideProtectedCall = 0;
        return r;
    }

    int CanPerformAction(lua_State* L)
    {
        const int32_t action = ToInt(CheckNumber(L, 1));
        if (action == 0x19)
        {
            PushBool(L, g_insideProtectedCall != 0);
            return 1;
        }
        PushBool(L, reinterpret_cast<int(__cdecl*)(int32_t)>(0x5191C0)(action) != 0);
        return 1;
    }

    // ---- GetCreatureInfoInstant / GetItemTemplate --------------------------------------------------------
    // GetCreatureInfoInstant(entry) (FUN_10209b30): exactly one numeric argument; the bridged creature
    // record (FUN_100b8cc0) as {creatureID = +4 entry, name = +8}, else nil.
    int GetCreatureInfoInstant(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isnumber(L, 1))
        {
            const uint32_t entry = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            if (const AscBridgeStore::CreatureRecord* r = AscBridgeStore::CreatureByEntry(entry))
            {
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                SetStrInt(L, "creatureID", static_cast<int32_t>(r->entry));
                PushStr(L, "name");
                PushStr(L, r->name);
                AscLua::lua_settable(L, -3);
                return 1;
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // GetItemTemplate(item) (handler_GetItemTemplate): the item cache record (0x67CA30, 0x1F8 bytes, +0
    // replaced by the id asked for) as the JSON object FUN_1021c4b0 builds, converted by FUN_100a4580.
    // In Lua: integer fields pushinteger, float fields pushnumber((float)), strings pushstring; arrays
    // become tables keyed 1..n (the converter turns digit-only keys into index + 1). Nothing when the
    // item is not cached.
    enum class Field { U32, I32, F32, Str };
    struct TemplateField { const char* key; uint32_t offset; Field type; uint32_t count; };   // count > 0: array
    const TemplateField kTemplate[] = {
        {"ID", 0x000, Field::U32, 0}, {"Name", 0x1F4, Field::Str, 0},
        {"Class", 0x004, Field::U32, 0}, {"SubClass", 0x008, Field::U32, 0},
        {"SoundOverrideSubclass", 0x00C, Field::I32, 0}, {"DisplayInfoID", 0x010, Field::U32, 0},
        {"Quality", 0x014, Field::U32, 0}, {"Flags1", 0x018, Field::U32, 0}, {"Flags2", 0x01C, Field::U32, 0},
        {"BuyPrice", 0x020, Field::I32, 0}, {"SellPrice", 0x024, Field::U32, 0}, {"InvType", 0x028, Field::U32, 0},
        {"AllowableClass", 0x02C, Field::I32, 0}, {"AllowableRace", 0x030, Field::I32, 0},
        {"ItemLevel", 0x034, Field::U32, 0}, {"RequiredLevel", 0x038, Field::U32, 0},
        {"RequiredSkill", 0x03C, Field::U32, 0}, {"RequiredSkillRank", 0x040, Field::U32, 0},
        {"RequiredSpell", 0x044, Field::U32, 0}, {"RequiredHonorRank", 0x048, Field::U32, 0},
        {"RequiredCityRank", 0x04C, Field::U32, 0}, {"RequiredReputationFaction", 0x050, Field::U32, 0},
        {"RequiredReputationRank", 0x054, Field::U32, 0}, {"MaxCount", 0x058, Field::I32, 0},
        {"Stackable", 0x05C, Field::I32, 0}, {"ContainerSlots", 0x060, Field::U32, 0},
        {"StatsCount", 0x064, Field::U32, 0}, {"ItemStatType", 0x068, Field::U32, 10},
        {"ItemStatValue", 0x090, Field::I32, 10}, {"ScalingStatDistribution", 0x0B8, Field::U32, 0},
        {"ScalingStatValue", 0x0BC, Field::U32, 0}, {"DamageMin", 0x0C0, Field::F32, 2},
        {"DamageMax", 0x0C8, Field::F32, 2}, {"DamageType", 0x0D0, Field::U32, 2},
        {"Armor", 0x0D8, Field::U32, 0}, {"HolyRes", 0x0DC, Field::U32, 0}, {"FireRes", 0x0E0, Field::U32, 0},
        {"NatureRes", 0x0E4, Field::U32, 0}, {"FrostRes", 0x0E8, Field::U32, 0},
        {"ShadowRes", 0x0EC, Field::U32, 0}, {"ArcaneRes", 0x0F0, Field::U32, 0},
        {"Delay", 0x0F4, Field::U32, 0}, {"AmmoType", 0x0F8, Field::U32, 0},
        {"RangedModRange", 0x0FC, Field::F32, 0}, {"SpellId", 0x100, Field::I32, 5},
        {"SpellTrigger", 0x114, Field::U32, 5}, {"SpellCharges", 0x128, Field::I32, 5},
        {"SpellCooldown", 0x13C, Field::I32, 5}, {"SpellCategory", 0x150, Field::U32, 5},
        {"SpellCategoryCooldown", 0x164, Field::I32, 5}, {"Bonding", 0x178, Field::U32, 0},
        {"Description", 0x17C, Field::Str, 0}, {"PageTextID", 0x180, Field::U32, 0},
        {"LanguageID", 0x184, Field::U32, 0}, {"PageMaterial", 0x188, Field::U32, 0},
        {"StartQuest", 0x18C, Field::U32, 0}, {"LockID", 0x190, Field::U32, 0},
        {"Material", 0x194, Field::I32, 0}, {"Sheath", 0x198, Field::U32, 0},
        {"RandomProperty", 0x19C, Field::I32, 0}, {"RandomSuffix", 0x1A0, Field::I32, 0},
        {"Block", 0x1A4, Field::U32, 0}, {"ItemSetID", 0x1A8, Field::U32, 0},
        {"MaxDurability", 0x1AC, Field::U32, 0}, {"AreaID", 0x1B0, Field::U32, 0},
        {"MapID", 0x1B4, Field::U32, 0}, {"BagFamily", 0x1B8, Field::U32, 0},
        {"TotemCategory", 0x1BC, Field::U32, 0}, {"SocketColor", 0x1C0, Field::U32, 3},
        {"SocketContent", 0x1CC, Field::U32, 3}, {"SocketBonus", 0x1D8, Field::U32, 0},
        {"GemProperties", 0x1DC, Field::U32, 0}, {"RequiredDisenchantSkill", 0x1E0, Field::U32, 0},
        {"ArmorDamageModifier", 0x1E4, Field::F32, 0}, {"Duration", 0x1E8, Field::U32, 0},
        {"ItemLimitCategory", 0x1EC, Field::U32, 0}, {"HolidayID", 0x1F0, Field::U32, 0},
    };

    void PushTemplateValue(lua_State* L, const uint8_t* rec, uint32_t offset, Field type)
    {
        switch (type)
        {
        case Field::U32:
        case Field::I32: PushInt(L, *reinterpret_cast<const int32_t*>(rec + offset)); break;
        case Field::F32: PushNum(L, static_cast<double>(*reinterpret_cast<const float*>(rec + offset))); break;
        case Field::Str:
        {
            const char* s = *reinterpret_cast<const char* const*>(rec + offset);
            PushStr(L, s ? s : "");
            break;
        }
        }
    }

    int GetItemTemplate(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        typedef const uint8_t*(__thiscall * ItemRecord_t)(void*, uint32_t, void*, void*, void*, int);
        const uint8_t* cached = reinterpret_cast<ItemRecord_t>(0x67CA30)(reinterpret_cast<void*>(0xC5D828), id, nullptr, nullptr, nullptr, 0);
        if (!cached)
            return 0;
        uint8_t rec[0x1F8];
        memcpy(rec, cached, sizeof(rec));
        memcpy(rec, &id, 4);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const TemplateField& f : kTemplate)
        {
            PushStr(L, f.key);
            if (f.count == 0)
                PushTemplateValue(L, rec, f.offset, f.type);
            else
            {
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                for (uint32_t i = 0; i < f.count; ++i)
                {
                    PushInt(L, static_cast<int32_t>(i + 1));
                    PushTemplateValue(L, rec, f.offset + i * 4, f.type);
                    AscLua::lua_settable(L, -3);
                }
            }
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // ---- known / secure addons (SecureAddonMgr, FUN_10291ed0 -> 0x10BE3044) ----------------------------
    // SMSG 0x94E (FUN_10312b90) replaces the list: u32 n x {cstring name, u8 secure}.
    struct KnownAddon { std::string name; bool secure; };
    std::vector<KnownAddon> g_knownAddons;

    void __cdecl OnKnownAddonList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_knownAddons.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            KnownAddon a;
            a.name = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
            p->m_read += static_cast<int32_t>(a.name.size() + 1);
            a.secure = Read<uint8_t>(p) != 0;
            g_knownAddons.push_back(a);
        }
    }

    // GetKnownAddons / GetSecureAddons: every (secure) name that does not contain "Blizzard", pushed as
    // separate return values.
    int PushAddonNames(lua_State* L, bool secureOnly)
    {
        int n = 0;
        for (const KnownAddon& a : g_knownAddons)
        {
            if (secureOnly && !a.secure)
                continue;
            if (a.name.size() >= 8 && a.name.find("Blizzard") != std::string::npos)
                continue;
            PushStr(L, a.name.c_str());
            ++n;
        }
        return n;
    }
    int GetKnownAddons(lua_State* L) { return PushAddonNames(L, false); }
    int GetSecureAddons(lua_State* L) { return PushAddonNames(L, true); }

    int IsKnownAddon(lua_State* L)   // an exact name match, Blizzard ones included
    {
        const std::string name = CheckString(L, 1);
        bool found = false;
        for (const KnownAddon& a : g_knownAddons)
            if (a.name == name)
            {
                found = true;
                break;
            }
        PushBool(L, found);
        return 1;
    }

    // ---- custom WTF files and saved CVars ----------------------------------------------------------------
    // ReadCustomWTF(name) / WriteCustomWTF(name, text) (FUN_10259d10 / FUN_1025a1d0): "WTF\Custom\<name>.json"
    // relative to the working directory, text mode (std::ifstream / std::ofstream mode out|trunc). Both need
    // the client's action gate 0x11 (0x5191C0); without it they return 1 value having pushed nothing
    // (kept). Read: "" when the directory is missing or the file cannot be read. Write: false for text
    // over 0x6400000 bytes or when the directory cannot be created, else whether the stream stayed good.
    bool CustomWTFAllowed() { return reinterpret_cast<int(__cdecl*)(int32_t)>(0x5191C0)(0x11) != 0; }
    std::string CustomWTFPath(const std::string& name) { return "WTF\\Custom\\" + name + ".json"; }

    int ReadCustomWTF(lua_State* L)
    {
        if (!CustomWTFAllowed())
            return 1;
        const std::string name = CheckString(L, 1);
        std::string text;
        if (GetFileAttributesA("WTF\\Custom") != INVALID_FILE_ATTRIBUTES)
            if (FILE* f = _fsopen(CustomWTFPath(name).c_str(), "r", _SH_DENYNO))
            {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    text.append(buf, n);
                fclose(f);
            }
        PushStr(L, text.c_str());
        return 1;
    }

    int WriteCustomWTF(lua_State* L)
    {
        if (!CustomWTFAllowed())
            return 1;
        const std::string name = CheckString(L, 1);
        const std::string text = CheckString(L, 2);
        if (text.size() > 0x6400000)
        {
            PushBool(L, false);
            return 1;
        }
        if (GetFileAttributesA("WTF\\Custom") == INVALID_FILE_ATTRIBUTES && !CreateDirectoryA("WTF\\Custom", nullptr))
        {
            PushBool(L, false);
            return 1;
        }
        bool good = false;
        if (FILE* f = _fsopen(CustomWTFPath(name).c_str(), "w", _SH_DENYNO))
        {
            good = fwrite(text.data(), 1, text.size(), f) == text.size();
            good = fclose(f) == 0 && good;
        }
        PushBool(L, good);
        return 1;
    }

    // RegisterSavedCVar(name, default [, perCharacter]) (FUN_10312910): refused (false) while an addon
    // runs (0xD4139C) that is not a known secure one; otherwise the client's CVar register (0x767FC0) with
    // no help text, flags 0x80000001 (0x80000021 when a third argument is true), category 5 -> true.
    int RegisterSavedCVar(lua_State* L)
    {
        const char* running = *reinterpret_cast<const char* const*>(0xD4139C);
        bool blocked = false;
        if (running)
        {
            blocked = true;
            for (const KnownAddon& a : g_knownAddons)
                if (a.name == running)
                {
                    blocked = !a.secure;
                    break;
                }
        }
        if (blocked)
        {
            PushBool(L, false);
            return 1;
        }
        const std::string name = CheckString(L, 1);
        const std::string value = CheckString(L, 2);
        uint32_t flags = 0x80000001;
        if (AscLua::lua_gettop(L) == 3)
            flags = AscLua::lua_toboolean(L, 3) ? 0x80000021 : 0x80000001;
        typedef int(__cdecl * Register_t)(const char*, const char*, uint32_t, const char*, void*, uint32_t, int, int, int);
        // The original passes category 5 to FUN_10114510, but that wrapper zeroes its four trailing arguments
        // (category, a7, a8, a9) before jumping to 0x767FC0, so the client registers category 0.
        reinterpret_cast<Register_t>(0x767FC0)(name.c_str(), "", flags, value.c_str(), nullptr, 0, 0, 0, 0);
        PushBool(L, true);
        return 1;
    }

    // ---- GetEncounterInfo --------------------------------------------------------------------------------
    // GetEncounterInfo(encounter [, unit]) (FUN_100d95e0; arguments FUN_100d0500: {number} or
    // {number, string}): the client's DungeonEncounter.dbc row (container 0xAD36B0) -> name (+0x14),
    // map (+4), difficulty + 1 (+8), SpellIcon path of +0x18 (default the question mark), +0xC, the
    // unit's loot-lockout seconds for it (0x60C1C0 on the token, "" without one) and whether the current
    // Mythic+ run has killed it. nil for an unknown encounter.
    int GetEncounterInfo(lua_State* L)
    {
        const bool hasUnit = AscLua::lua_type(L, 2) > 0;
        if (hasUnit ? !ValidateInput(L, {AscScript::NUMBER, AscScript::STRING}) : !ValidateInput(L, {AscScript::NUMBER}))
            return 0;
        const uint32_t encounter = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const std::string unit = hasUnit ? CheckString(L, 2) : std::string();
        uint64_t guid = 0;
        UnitTokenGuid(unit.c_str(), guid);
        const uint8_t* row = ClientDbcRow(0xAD36B0, encounter);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const uint8_t* icon = ClientDbcRow(0xAD488C, *reinterpret_cast<const uint32_t*>(row + 0x18));
        const std::string iconPath = icon ? *reinterpret_cast<const char* const*>(icon + 4) : "Interface\\Icons\\inv_misc_questionmark";
        PushStr(L, *reinterpret_cast<const char* const*>(row + 0x14));
        PushInt(L, *reinterpret_cast<const int32_t*>(row + 4));
        PushInt(L, *reinterpret_cast<const int32_t*>(row + 8) + 1);
        PushStr(L, iconPath.c_str());
        PushInt(L, *reinterpret_cast<const int32_t*>(row + 0xC));
        PushInt(L, static_cast<int32_t>(AscLootLockout::SecondsRemaining(guid, encounter)));
        PushBool(L, AscMythicPlus::EncounterKilled(encounter));
        return 7;
    }

    // ---- GetReducedChanceToBeCrit -------------------------------------------------------------------------
    // handler_GetReducedChanceToBeCrit: the attacker-crit reductions the player carries, summed as
    // BasePoints + 1 of each SPELL_AURA_MOD_ATTACKER_{SPELL (179), MELEE (187), RANGED (188)}_CRIT_CHANCE
    // effect, SPELL_AURA_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE (197) counting towards all three. Over
    // the player's auras (every aura, repeats included), then the spellbook (0xBE6D88, count 0xBE8D98)
    // and the CA known spells, each of those only once and not when an aura already counted it.
    // (spell, melee, ranged); 0, 0, 0 without a player.
    int GetReducedChanceToBeCrit(lua_State* L)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
        {
            PushNum(L, 0);
            PushNum(L, 0);
            PushNum(L, 0);
            return 3;
        }
        int32_t spell = 0, melee = 0, ranged = 0;
        std::unordered_set<uint32_t> seen;
        uint8_t rec[0x2A8];
        auto add = [&](const uint8_t* r) {
            for (int j = 0; j < 3; ++j)
            {
                const int32_t amount = *reinterpret_cast<const int32_t*>(r + 0x140 + j * 4) + 1;
                switch (*reinterpret_cast<const uint32_t*>(r + 0x17C + j * 4))
                {
                    case 0xB3: spell += amount; break;
                    case 0xBB: melee += amount; break;
                    case 0xBC: ranged += amount; break;
                    case 0xC5: spell += amount; melee += amount; ranged += amount; break;
                }
            }
        };
        typedef uint32_t(__thiscall * Count_t)(void*);
        typedef const uint8_t*(__thiscall * AuraAt_t)(void*, uint32_t);
        const uint32_t auras = reinterpret_cast<Count_t>(0x4F8850)(player);
        for (uint32_t i = 0; i < auras; ++i)
        {
            const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(player, i);
            if (aura && FetchSpell(*reinterpret_cast<const uint32_t*>(aura + 8), rec))
            {
                seen.insert(*reinterpret_cast<const uint32_t*>(rec));
                add(rec);
            }
        }
        auto fromKnown = [&](uint32_t id) {
            if (!seen.count(id) && FetchSpell(id, rec))
            {
                seen.insert(*reinterpret_cast<const uint32_t*>(rec));
                add(rec);
            }
        };
        const uint32_t* book = reinterpret_cast<const uint32_t*>(0xBE6D88);
        for (uint32_t i = 0; i < *reinterpret_cast<const uint32_t*>(0xBE8D98); ++i)
            fromKnown(book[i]);
        for (uint32_t id : AscCA::KnownSpells())
            fromKnown(id);
        PushNum(L, spell);
        PushNum(L, melee);
        PushNum(L, ranged);
        return 3;
    }

    // ---- IsSpellHoldToCast --------------------------------------------------------------------------------
    // handler_IsSpellHoldToCast(spell): nothing unless the game window has focus (0x86C6A0(0) == (2)) and
    // the spell has a record. Shaman spells (family 11) hold with family flag 0x2 when the mystic enchant
    // 81091 is slotted, or with family flag[2] 0x20000000 when CA spell 955831 is known. Otherwise a spell
    // holds when it has no SpellCharges row, lacks AttributesEx2 0x20, has a family, applies no shapeshift
    // (aura 36) and targets neither 23 nor 26 with ImplicitTargetA/B.
    int IsSpellHoldToCast(lua_State* L)
    {
        const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (!spell)
            return 0;
        typedef HWND(__cdecl * Window_t)(int);
        if (reinterpret_cast<Window_t>(0x86C6A0)(0) != reinterpret_cast<Window_t>(0x86C6A0)(2))
            return 0;
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return 0;
        auto u32 = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(rec + off); };
        const uint32_t family = u32(0x240);
        bool hold = false;
        if (family == 0xB && (((u32(0x244) & 2) && AscMysticEnchant::Slotted(0x13CC3)) ||
                              ((u32(0x24C) & 0x20000000) && AscCA::KnownSpells().count(0xE91B7))))
            hold = true;
        else if (!AscDbc::Get("DBFilesClient\\SpellCharges.dbc").Row(u32(0)) && !(u32(0x18) & 0x20) && family != 0)
        {
            bool shapeshift = false;
            for (int j = 0; j < 3; ++j)
                if (u32(0x17C + j * 4) == 0x24)
                    shapeshift = true;
            auto targets = [&](uint32_t t) {
                for (int j = 0; j < 3; ++j)
                    if (u32(0x158 + j * 4) == t || u32(0x164 + j * 4) == t)
                        return true;
                return false;
            };
            hold = !shapeshift && !targets(0x17) && !targets(0x1A);
        }
        PushBool(L, hold);
        return 1;
    }

    // ---- PlayerAuraHidden ---------------------------------------------------------------------------------
    // PlayerAuraHidden(spell) (FUN_100dc840): UnitAura's eleven values for the player's first aura of the
    // spell, found whether or not the client hides it -- name, rank, icon (FUN_100b2470), count (+0x0E),
    // dispel type name (SpellDispelType 0xAD4800 +0x10 when +0x0C is set), duration and expiration in
    // seconds (0, 0 for a permanent aura), caster token (0x60B420), stealable (0x53D680) and
    // AttributesEx7 0x10000000 as 1 or nil, and the spell id. Nothing when it is not on the player.
    int PlayerAuraHidden(lua_State* L)
    {
        uint32_t spell = 0;
        uint8_t* player = nullptr;
        if (!ReadNumber(L, spell) || !(player = ActivePlayer()))
            return 0;
        typedef uint32_t(__thiscall * Count_t)(void*);
        typedef const uint8_t*(__thiscall * AuraAt_t)(void*, uint32_t);
        uint8_t rec[0x2A8];
        for (uint32_t i = 0; i < reinterpret_cast<Count_t>(0x4F8850)(player); ++i)
        {
            const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(player, i);
            if (!aura || *reinterpret_cast<const uint32_t*>(aura + 8) != spell || !FetchSpell(spell, rec))
                continue;
            auto u32 = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(rec + off); };
            PushStr(L, *reinterpret_cast<const char* const*>(rec + 0x220));
            PushStr(L, *reinterpret_cast<const char* const*>(rec + 0x224));
            const uint8_t* icon = ClientDbcRow(0xAD488C, u32(0x214));
            PushStr(L, icon ? *reinterpret_cast<const char* const*>(icon + 4) : nullptr);
            PushNum(L, *reinterpret_cast<const uint16_t*>(aura + 0xE));
            const char* dispel = nullptr;
            if (u32(8))
                if (const uint8_t* d = ClientDbcRow(0xAD4800, u32(8)))
                    if (*reinterpret_cast<const uint32_t*>(d + 0xC))
                        dispel = *reinterpret_cast<const char* const*>(d + 0x10);
            PushStr(L, dispel);
            const int32_t duration = *reinterpret_cast<const int32_t*>(aura + 0x10);
            if (duration < 1)
            {
                PushNum(L, 0);
                PushNum(L, 0);
            }
            else
            {
                PushNum(L, duration * 0.001);
                PushNum(L, *reinterpret_cast<const uint32_t*>(aura + 0x14) * 0.001);
            }
            PushStr(L, reinterpret_cast<const char*(__cdecl*)(const uint8_t*)>(0x60B420)(aura));
            if (reinterpret_cast<char(__cdecl*)(uint8_t*, const uint8_t*, uint8_t*)>(0x53D680)(player, aura, rec))
                PushNum(L, 1.0);
            else
                AscLua::lua_pushnil(L);
            if (u32(0x2C) & 0x10000000)
                PushNum(L, 1.0);
            else
                AscLua::lua_pushnil(L);
            PushNum(L, static_cast<double>(u32(0)));
            return 11;
        }
        return 0;
    }

    // ---- UnitAuraAllowHidden ------------------------------------------------------------------------------
    // UnitAuraAllowHidden(bool) (FUN_100dd950): true NOPs the client's hidden-aura skip in UnitAura (the
    // 6-byte `je` at 0x614AF8), false restores it. Returns its argument, as the original does by returning
    // 1 with nothing pushed.
    int UnitAuraAllowHidden(lua_State* L)
    {
        bool allow = false;
        if (!ReadBool(L, allow))
            return 0;
        static const uint8_t kJe[6] = {0x0F, 0x84, 0x8D, 0x00, 0x00, 0x00};
        if (allow)
            FillCode(0x614AF8, 0x90, 6);
        else
            WriteCode(0x614AF8, kJe, 6);
        return 1;
    }

    // ---- Asset queries ------------------------------------------------------------------------------------
    // RequestSingleAssetQuery(type, id) (FUN_100dcee0): CMSG 0x775 {u32 type, u32 id}.
    int RequestSingleAssetQuery(lua_State* L)
    {
        int32_t type = 0, id = 0;
        if (ReadInt2(L, type, id))
            AscScript::Packet(0x775).U32(static_cast<uint32_t>(type)).U32(static_cast<uint32_t>(id)).Send();
        return 0;
    }

    // RequestMultipleAssetQuery(type, id, ...) (FUN_100dcbd0): CMSG 0x776 {u32 type, u32 count,
    // u32 id * count}, for two or more arguments.
    int RequestMultipleAssetQuery(lua_State* L)
    {
        const int top = AscLua::lua_gettop(L);
        if (top < 2)
            return 0;
        AscScript::Packet pkt(0x776);
        pkt.U32(static_cast<uint32_t>(ToInt(CheckNumber(L, 1)))).U32(static_cast<uint32_t>(top - 1));
        for (int i = 2; i <= top; ++i)
            pkt.U32(static_cast<uint32_t>(ToInt(CheckNumber(L, i))));
        pkt.Send();
        return 0;
    }

    // ---- AscensionPvPMgr (0x10BDBAD4): arena reward info --------------------------------------------------
    // SMSG 0x925 (FUN_100e04a0) fills seven u32s in wire order +4, +8, +0xC, +0x1C, +0x14, +0x18, +0x10 and
    // signals ASCENSION_CUSTOM_PVP_REWARD_INFO_AVAILABLE; CMSG 0x53E asks for it.
    struct PvPRewardInfo { uint32_t f04, f08, bracket, f10, f14, f18, f1C; };
    PvPRewardInfo g_pvpReward{};

    void __cdecl OnPvPRewardInfo(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_pvpReward.f04 = Read<uint32_t>(p);
        g_pvpReward.f08 = Read<uint32_t>(p);
        g_pvpReward.bracket = Read<uint32_t>(p);
        g_pvpReward.f1C = Read<uint32_t>(p);
        g_pvpReward.f14 = Read<uint32_t>(p);
        g_pvpReward.f18 = Read<uint32_t>(p);
        g_pvpReward.f10 = Read<uint32_t>(p);
        AscRuntime::Signal("ASCENSION_CUSTOM_PVP_REWARD_INFO_AVAILABLE");
    }

    int Ascension_QueryArenaRewardInfo(lua_State*)   // FUN_100e07f0
    {
        AscScript::Packet(0x53E).Send();
        return 0;
    }

    // FUN_100e0660: +8, +4, the bracket's name (BRACKET_NONE .. BRACKET_GLADIATOR, else
    // UNEXPECTED_ENUM_VALUE_<n>), +0x1C, +0x14, +0x18, +0x10.
    int Ascension_GetArenaRewardInfo(lua_State* L)
    {
        static const char* const kBrackets[] = {"BRACKET_NONE", "BRACKET_COMBATANT", "BRACKET_CHALLENGER",
                                                "BRACKET_RIVAL", "BRACKET_DUELIST", "BRACKET_GLADIATOR"};
        const PvPRewardInfo& r = g_pvpReward;
        PushInt(L, static_cast<int32_t>(r.f08));
        PushInt(L, static_cast<int32_t>(r.f04));
        const std::string bracket = r.bracket < 6 ? kBrackets[r.bracket] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r.bracket);
        PushStr(L, bracket.c_str());
        PushInt(L, static_cast<int32_t>(r.f1C));
        PushInt(L, static_cast<int32_t>(r.f14));
        PushInt(L, static_cast<int32_t>(r.f18));
        PushInt(L, static_cast<int32_t>(r.f10));
        return 7;
    }

    // ---- GetAdditionalRoleRequirementsByClassID -----------------------------------------------------------
    // FUN_10a6e990(class): for each of the ChrClassesRoles.dbc row's three slots whose role mask & 0xE is
    // set, {Role, ConditionType, ConditionTypeName, ConditionValue, Met} (FUN_10a4e970: type 0 always, 1 the
    // spell is known (FUN_1008e5b0 with the build), 2 the player has the aura; never without a player).
    // An empty table for an unknown class.
    int GetAdditionalRoleRequirementsByClassID(lua_State* L)
    {
        const uint32_t cls = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        AscLua::lua_createtable(L, 0, 0);
        const uint8_t* row = AscDbc::Get("DBFilesClient\\ChrClassesRoles.dbc").Row(cls);
        if (!row)
            return 1;
        uint8_t* player = ActivePlayer();
        int32_t index = 1;
        for (int i = 0; i < 3; ++i)
        {
            const uint32_t role = *reinterpret_cast<const uint32_t*>(row + 8 + i * 4) & 0xE;
            if (!role)
                continue;
            const int32_t type = *reinterpret_cast<const int32_t*>(row + 0x14 + i * 4);
            const uint32_t value = *reinterpret_cast<const uint32_t*>(row + 0x20 + i * 4);
            bool met = false;
            if (player)
            {
                if (type == 0)
                    met = true;
                else if (type == 1)
                    met = value && AscCA::SpellKnown(value, true);
                else if (type == 2 && value)
                {
                    typedef uint32_t(__thiscall * Count_t)(void*);
                    typedef const uint8_t*(__thiscall * AuraAt_t)(void*, uint32_t);
                    for (uint32_t a = 0; a < reinterpret_cast<Count_t>(0x4F8850)(player) && !met; ++a)
                    {
                        const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(player, a);
                        met = aura && *reinterpret_cast<const uint32_t*>(aura + 8) == value;
                    }
                }
            }
            PushInt(L, index++);
            AscLua::lua_createtable(L, 0, 0);
            SetStrInt(L, "Role", static_cast<int32_t>(role));
            SetStrInt(L, "ConditionType", type);
            AscLua::lua_pushstring(L, "ConditionTypeName");
            AscLua::lua_pushstring(L, type == 0 ? "None" : type == 1 ? "SpellKnown" : type == 2 ? "AuraActive" : "Unknown");
            AscLua::lua_settable(L, -3);
            SetStrInt(L, "ConditionValue", static_cast<int32_t>(value));
            AscLua::lua_pushstring(L, "Met");
            PushBool(L, met);
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // ---- GetBattlefieldList -------------------------------------------------------------------------------
    // FUN_100d8c00(bgType [, fromWhere [, unk]]): CMSG_BATTLEFIELD_LIST (0x23C) {u32, u8, u8}.
    int GetBattlefieldList(lua_State* L)
    {
        const uint32_t bg = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint8_t from = AscLua::lua_type(L, 2) > 0 ? static_cast<uint8_t>(ToInt(CheckNumber(L, 2))) : 0;
        const uint8_t unk = AscLua::lua_type(L, 3) > 0 ? static_cast<uint8_t>(ToInt(CheckNumber(L, 3))) : 0;
        AscScript::Packet(0x23C).U32(bg).U8(from).U8(unk).Send();
        return 0;
    }

    // ---- SetCurrencyShow -----------------------------------------------------------------------------------
    // FUN_10a71430(n): non-zero NOPs the 6-byte `je` at 0x5B01C8, zero restores it (0F 84 F9 00 00 00).
    int SetCurrencyShow(lua_State* L)
    {
        static const uint8_t kJe[6] = {0x0F, 0x84, 0xF9, 0x00, 0x00, 0x00};
        if (ToInt(CheckNumber(L, 1)) != 0)
            FillCode(0x5B01C8, 0x90, 6);
        else
            WriteCode(0x5B01C8, kJe, 6);
        return 0;
    }

    // ---- SetItemQualityColor -------------------------------------------------------------------------------
    // handler_SetItemQualityColor(quality 0..7, r, g, b): nothing when a component is above 1. Each is
    // scaled by 255 and written as two lower-case hex digits (FUN_10a77ec0; "" above 0xFF, which skips
    // the whole update); "|cffRRGGBB" overwrites the client's colour string for the quality (the buffer
    // 0xAD2A84[q] points at, its length without the NUL) and 0xAD2D84[q] becomes {B, G, R, 0xFF}.
    int SetItemQualityColor(lua_State* L)
    {
        const uint32_t q = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (q > 7)
            return 0;
        const float r = static_cast<float>(CheckNumber(L, 2));
        const float g = static_cast<float>(CheckNumber(L, 3));
        const float b = static_cast<float>(CheckNumber(L, 4));
        if (r > 1.0f || g > 1.0f || b > 1.0f)
            return 0;
        auto hex = [](float f) {
            const uint32_t v = static_cast<uint32_t>(static_cast<int64_t>(f));
            char buf[8] = {};
            if (v <= 0xFF)
                snprintf(buf, sizeof(buf), "%02x", v);
            return std::string(buf);
        };
        const float r255 = r * 255.0f, g255 = g * 255.0f, b255 = b * 255.0f;
        const std::string rr = hex(r255), gg = hex(g255), bb = hex(b255);
        if (rr.empty() || gg.empty() || bb.empty())
            return 0;
        const std::string color = "|cff" + rr + gg + bb;
        WriteCode(*reinterpret_cast<const uint32_t*>(0xAD2A84 + q * 4), color.data(), color.size());
        const uint8_t argb[4] = {static_cast<uint8_t>(static_cast<int32_t>(b255)), static_cast<uint8_t>(static_cast<int32_t>(g255)),
                                 static_cast<uint8_t>(static_cast<int32_t>(r255)), 0xFF};
        WriteCode(0xAD2D84 + q * 4, argb, 4);
        return 0;
    }

    // ---- SetCharacterSelectionSortOrder -----------------------------------------------------------------------
    // FUN_100dd280(order): CMSG 0x772 {string order, NUL included (FUN_100b97a0)}.
    int SetCharacterSelectionSortOrder(lua_State* L)
    {
        std::string order;
        if (ReadString(L, order))
            AscScript::Packet(0x772).Data(order.c_str(), static_cast<uint32_t>(order.size() + 1)).Send();
        return 0;
    }

    // ---- SetWorldTimeSpeed ---------------------------------------------------------------------------------
    // FUN_10a75cf0(speed): the client's game-time rate (float 0xD37FC8) becomes `speed`, or the stock
    // 1/60 (0x3C888889) for a negative one.
    int SetWorldTimeSpeed(lua_State* L)
    {
        const float speed = static_cast<float>(CheckNumber(L, 1));
        const uint32_t bits = speed >= 0.0f ? *reinterpret_cast<const uint32_t*>(&speed) : 0x3C888889u;
        WriteCode(0xD37FC8, &bits, 4);
        return 0;
    }

    // ---- IsQuestSequenced ---------------------------------------------------------------------------------
    // FUN_1021eb20(quest): the quest addon cache's record (FUN_10115fc0 on 0x10BDF3A8, which requests an
    // unknown quest) -> its +8 & 1, or nil while it is not loaded.
    int IsQuestSequenced(lua_State* L)
    {
        if (!ValidateInput(L, {AscScript::NUMBER}))
            return 0;
        const uint32_t quest = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (const AscQuestAddon::Record* r = AscQuestAddon::Get(quest))
            PushBool(L, (r->unk & 1) != 0);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- SetWorldTime -------------------------------------------------------------------------------------
    // FUN_10a75170(hour 0..23, minute 0..59): the client's clock bytes (hour 0xD37F9C, minute 0xD37F98) are
    // set, the game-time rate is overwritten with the DOUBLE 100.0 (8 bytes over 0xD37FC8 -- the original's
    // own width, which leaves 0x40590000 in 0xD37FCC), the thread sleeps until now + 50 ms (FUN_100b53c0),
    // the clock bytes are written again and the rate dword goes back to the stock 1/60.
    int SetWorldTime(lua_State* L)
    {
        const double hour = CheckNumber(L, 1);
        const double minute = CheckNumber(L, 2);
        if (static_cast<uint32_t>(static_cast<int32_t>(hour)) > 0x17 || static_cast<uint32_t>(static_cast<int32_t>(minute)) > 0x3B)
            return 0;
        const uint8_t h = static_cast<uint8_t>(static_cast<int32_t>(hour));
        const uint8_t m = static_cast<uint8_t>(static_cast<int32_t>(minute));
        WriteCode(0xD37F9C, &h, 1);
        WriteCode(0xD37F98, &m, 1);
        const double fast = 100.0;
        WriteCode(0xD37FC8, &fast, 8);
        Sleep(50);
        WriteCode(0xD37F9C, &h, 1);
        WriteCode(0xD37F98, &m, 1);
        const uint32_t stock = 0x3C888889;
        WriteCode(0xD37FC8, &stock, 4);
        return 0;
    }

    // ---- SubmitGameFeedback -------------------------------------------------------------------------------
    // FUN_100dd3c0(n [, text]) (arguments FUN_100d0290: {number} or {number, string}; an empty text counts
    // as none): CMSG 0x735 {u32 n, u8 hasText, [text + NUL]}, then true.
    int SubmitGameFeedback(lua_State* L)
    {
        const bool hasText = AscLua::lua_type(L, 2) > 0;
        if (hasText ? !ValidateInput(L, {AscScript::NUMBER, AscScript::STRING}) : !ValidateInput(L, {AscScript::NUMBER}))
            return 0;
        const uint32_t n = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const std::string text = hasText ? CheckString(L, 2) : std::string();
        AscScript::Packet pkt(0x735);
        pkt.U32(n).U8(text.empty() ? 0 : 1);
        if (!text.empty())
            pkt.Data(text.c_str(), static_cast<uint32_t>(text.size() + 1));
        pkt.Send();
        PushBool(L, true);
        return 1;
    }

    // ---- GetItemInfoInstant -------------------------------------------------------------------------------
    // FUN_10209ee0(item): one numeric argument, the client Item.dbc row (FUN_100b1870) and the DLL's item
    // addon record (FUN_101a2610), else nil. {itemID, icon ("Interface\\Icons\\" + the ItemDisplayInfo
    // inventory icon, or INV_Misc_QuestionMark), classID, subclassID, name, description, quality (+0x10),
    // pvePower (+0x18), pvpPower (+0x1C), miscValue {+0x20, +0x24, +0x28}, inventoryType, itemLevel (+0x3C)}.
    int GetItemInfoInstant(lua_State* L)
    {
        if (AscLua::lua_gettop(L) == 1 && AscLua::lua_isnumber(L, 1))
        {
            const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            const uint8_t* item = ClientDbcRow(kItemDbc, id);
            const AscItemAddon::Record* addon = item ? AscItemAddon::Find(*reinterpret_cast<const uint32_t*>(item)) : nullptr;
            if (item && addon)
            {
                auto u32 = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(item + off); };
                AscLua::lua_createtable(L, 0, 0);
                SetStrInt(L, "itemID", static_cast<int32_t>(u32(0)));
                uint8_t display[124] = {};
                std::string icon;
                typedef int(__thiscall * GetRec_t)(void*, uint32_t, void*);
                if (reinterpret_cast<GetRec_t>(0x4CFD90)(reinterpret_cast<void*>(0xAD3DDC), u32(0x14), display))
                {
                    const char* name = *reinterpret_cast<const char* const*>(display + 0x14);
                    icon = std::string("Interface\\Icons\\") + (name ? name : "");
                }
                else
                    icon = "Interface\\Icons\\INV_Misc_QuestionMark";
                AscLua::lua_pushstring(L, "icon");
                PushStr(L, icon.c_str());
                AscLua::lua_settable(L, -3);
                SetStrInt(L, "classID", static_cast<int32_t>(u32(4)));
                SetStrInt(L, "subclassID", static_cast<int32_t>(u32(8)));
                AscLua::lua_pushstring(L, "name");
                PushStr(L, addon->name);
                AscLua::lua_settable(L, -3);
                AscLua::lua_pushstring(L, "description");
                PushStr(L, addon->description);
                AscLua::lua_settable(L, -3);
                SetStrInt(L, "quality", static_cast<int32_t>(addon->fields[0]));
                SetStrInt(L, "pvePower", static_cast<int32_t>(addon->fields[2]));
                SetStrInt(L, "pvpPower", static_cast<int32_t>(addon->fields[3]));
                AscLua::lua_pushstring(L, "miscValue");
                AscLua::lua_createtable(L, 0, 3);
                for (uint32_t i = 0; i < 3; ++i)
                {
                    PushNum(L, i + 1);
                    PushInt(L, static_cast<int32_t>(addon->fields[4 + i]));
                    AscLua::lua_settable(L, -3);
                }
                AscLua::lua_settable(L, -3);
                SetStrInt(L, "inventoryType", static_cast<int32_t>(u32(0x18)));
                SetStrInt(L, "itemLevel", static_cast<int32_t>(addon->fields[11]));
                return 1;
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- FindScriptEvents ---------------------------------------------------------------------------------
    // FUN_100d8410(text): every event name (FUN_10278d20: the custom list 0x10BE2E58, then the client's 600
    // from FUN_10073fe0's table) whose lower-case form contains the lower-case text -- all of them for "".
    const char* const kClientEventNames[] = {
#include <Ascension/AscScriptEventNames.generated.inc>
    };

    int FindScriptEvents(lua_State* L)
    {
        std::string text;
        if (!ReadString(L, text))
            return 0;
        auto lower = [](std::string v) {
            for (char& c : v)
                c = static_cast<char>(tolower(c));
            return v;
        };
        text = lower(text);
        std::vector<const char*> all;
        uint32_t custom = 0;
        const char* const* names = AscRuntime::CustomEventNames(custom);
        all.assign(names, names + custom);
        all.insert(all.end(), std::begin(kClientEventNames), std::end(kClientEventNames));
        std::vector<const char*> found;
        for (const char* name : all)
            if (text.empty() || lower(name).find(text) != std::string::npos)
                found.push_back(name);
        AscLua::lua_createtable(L, 0, static_cast<int>(found.size()));
        for (uint32_t i = 0; i < found.size(); ++i)
        {
            PushNum(L, i + 1);
            PushStr(L, found[i]);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // ---- SetMaskTexture / ClearMaskTexture -----------------------------------------------------------------
    // SetMaskTexture(size, path) (FUN_10a71fc0) re-points the client's portrait alpha mask: the size
    // (0x10D3D808) into 0x6194ED, 0x617721, 0x61779F (dword) and 0x619546, 0x619548 (byte); the path (kept
    // in 0x10BCCAF4) into 0x61773A, 0x6177A6, 0x6177B2; 0x61935B becomes an unconditional jmp (0xEB) and the
    // 6-byte jne at 0x61770A is NOPed. ClearMaskTexture (FUN_10a6d670) restores the stock bytes: 0x40 / 0x80
    // / 0x80 / 0x40 / 0x40, TempPortraitAlphaMask.tga / .blp / Small (0xA23F38 / 0xA23FA4 / 0xA23FD8), 0x74,
    // 0F 85 3F 01 00 00. Both return 0.
    std::string g_maskTexture;   // 0x10BCCAF4

    int SetMaskTexture(lua_State* L)
    {
        const int32_t size = ToInt(CheckNumber(L, 1));
        g_maskTexture = CheckString(L, 2);
        const char* path = g_maskTexture.c_str();
        const uint8_t size8 = static_cast<uint8_t>(size);
        WriteCode(0x6194ED, &size, 4);
        WriteCode(0x617721, &size, 4);
        WriteCode(0x61779F, &size, 4);
        WriteCode(0x619546, &size8, 1);
        WriteCode(0x619548, &size8, 1);
        WriteCode(0x61773A, &path, 4);
        WriteCode(0x6177A6, &path, 4);
        WriteCode(0x6177B2, &path, 4);
        static const uint8_t kJmp = 0xEB;
        WriteCode(0x61935B, &kJmp, 1);
        FillCode(0x61770A, 0x90, 6);
        return 0;
    }

    int ClearMaskTexture(lua_State*)
    {
        const uint32_t k40 = 0x40, k80 = 0x80, tga = 0xA23F38, blp = 0xA23FA4, small = 0xA23FD8;
        const uint8_t b40 = 0x40, je = 0x74;
        static const uint8_t kJne[6] = {0x0F, 0x85, 0x3F, 0x01, 0x00, 0x00};
        WriteCode(0x6194ED, &k40, 4);
        WriteCode(0x617721, &k80, 4);
        WriteCode(0x61779F, &k80, 4);
        WriteCode(0x619546, &b40, 1);
        WriteCode(0x619548, &b40, 1);
        WriteCode(0x61773A, &tga, 4);
        WriteCode(0x6177A6, &blp, 4);
        WriteCode(0x6177B2, &small, 4);
        WriteCode(0x61935B, &je, 1);
        WriteCode(0x61770A, kJne, 6);
        return 0;
    }

    // ---- ApplyComponents ----------------------------------------------------------------------------------
    // FUN_102cbb80(slot, texture, model): "Item\\ObjectComponents\\" + each name is copied into the client's
    // component path buffers (0xB6B600, 0xB6B4F8); with ItemDisplayInfo row 17 (0x4CFD90 on 0xAD3DDC) they
    // are applied to the player's character component (0x4EAA70(*(player +0xB4C) +0x38, slot, 0xB6B600,
    // 0xB6B4F8, 0, row)). Returns 0. (The original dereferences the player unchecked.)
    int ApplyComponents(lua_State* L)
    {
        const int32_t slot = ToInt(CheckNumber(L, 1));
        const std::string first = std::string("Item\\ObjectComponents\\") + CheckString(L, 2);
        const std::string second = std::string("Item\\ObjectComponents\\") + CheckString(L, 3);
        strcpy(reinterpret_cast<char*>(0xB6B600), first.c_str());
        strcpy(reinterpret_cast<char*>(0xB6B4F8), second.c_str());
        uint8_t* player = ActivePlayer();
        uint8_t display[124] = {};
        typedef int(__thiscall * GetRec_t)(void*, uint32_t, void*);
        if (player && reinterpret_cast<GetRec_t>(0x4CFD90)(reinterpret_cast<void*>(0xAD3DDC), 0x11, display))
            reinterpret_cast<void(__cdecl*)(uint32_t, int32_t, char*, char*, int, void*)>(0x4EAA70)(
                *reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(player + 0xB4C) + 0x38), slot,
                reinterpret_cast<char*>(0xB6B600), reinterpret_cast<char*>(0xB6B4F8), 0, display);
        return 0;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x94E, CNetClientCustomPacket((void*)&OnKnownAddonList, nullptr));
        g_510B30 = reinterpret_cast<Fn510B30_t>(AscRuntime::Detour(0x510B30, 7, reinterpret_cast<void*>(&Detour510B30)));
        g_7385C0Trampoline = AscRuntime::Detour(0x7385C0, 9, reinterpret_cast<void*>(&Detour7385C0));
        g_4F5F40Trampoline = AscRuntime::Detour(0x4F5F40, 6, reinterpret_cast<void*>(&Detour4F5F40));
        sDC.AddPacketHandler(0x770, CNetClientCustomPacket((void*)&OnSelectionMail, nullptr));
        sDC.AddPacketHandler(0x771, CNetClientCustomPacket((void*)&OnSelectionGameMode, nullptr));
        sDC.AddPacketHandler(0x9B7, CNetClientCustomPacket((void*)&OnTransactionCreated, nullptr));
        sDC.AddPacketHandler(0x9B8, CNetClientCustomPacket((void*)&OnTransactionClosed, nullptr));
        sDC.AddPacketHandler(0x9B9, CNetClientCustomPacket((void*)&OnTransactionList, nullptr));
        sDC.AddPacketHandler(0x925, CNetClientCustomPacket((void*)&OnPvPRewardInfo, nullptr));
        g_74F910Trampoline = AscRuntime::Detour(0x74F910, 8, reinterpret_cast<void*>(&Detour74F910));
        AscRuntime::ReplaceFunction(0x819730, reinterpret_cast<void*>(&ScriptErrorHandler));
        const uint32_t getter = reinterpret_cast<uint32_t>(&FadeOutGameObjects);
        WriteCode(0x9F3A88, &getter, 4);
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "CombatLogDisableAll", CombatLogDisableAll},
        {nullptr, "CombatLogDisable", CombatLogDisable},
        {nullptr, "GetErrorCallstackHeight", GetErrorCallstackHeight},
        {nullptr, "SetNamePlateCastBarMode", SetNamePlateCastBarMode},
        {nullptr, "SetFadeOutGameObjects", SetFadeOutGameObjects},
        {nullptr, "GetCharacterSelectionMailData", GetCharacterSelectionMailData},
        {nullptr, "GetCharacterSelectionGameModeData", GetCharacterSelectionGameModeData},
        {nullptr, "GM_GetTransactionIdList", GM_GetTransactionIdList},
        {nullptr, "GM_GetTransactionContent", GM_GetTransactionContent},
        {nullptr, "GetWeaponTempEnchantInfo", GetWeaponTempEnchantInfo},
        {nullptr, "GetRangedWeaponEnchantInfo", GetRangedWeaponEnchantInfo},
        {nullptr, "GetEquipmentSetItemGUIDs", GetEquipmentSetItemGUIDs},
        {nullptr, "Custom_HandleTerrainClick", Custom_HandleTerrainClick},
        {nullptr, "Custom_HandleTerrainClick_PlayerPos", Custom_HandleTerrainClick_PlayerPos},
        {nullptr, "Custom_HandleTerrainClick_TargetPos", Custom_HandleTerrainClick_TargetPos},
        {nullptr, "SetAttackCursor", SetAttackCursor},
        {nullptr, "SetAttackTexture", SetAttackTexture},
        {nullptr, "SetRangedAttackTexture", SetRangedAttackTexture},
        {nullptr, "SetAutoRangedCombatSpell", SetAutoRangedCombatSpell},
        {nullptr, "SetAttackAnimation", SetAttackAnimation},
        {nullptr, "SetAttackDistance", SetAttackDistance},
        {nullptr, "GetSpellData", GetSpellData},
        {nullptr, "CanPerformAction", CanPerformAction},
        {nullptr, "GetCreatureInfoInstant", GetCreatureInfoInstant},
        {nullptr, "GetItemTemplate", GetItemTemplate},
        {nullptr, "GetKnownAddons", GetKnownAddons},
        {nullptr, "GetSecureAddons", GetSecureAddons},
        {nullptr, "IsKnownAddon", IsKnownAddon},
        {nullptr, "ReadCustomWTF", ReadCustomWTF},
        {nullptr, "WriteCustomWTF", WriteCustomWTF},
        {nullptr, "RegisterSavedCVar", RegisterSavedCVar},
        {nullptr, "GetEncounterInfo", GetEncounterInfo},
        {nullptr, "GetReducedChanceToBeCrit", GetReducedChanceToBeCrit},
        {nullptr, "IsSpellHoldToCast", IsSpellHoldToCast},
        {nullptr, "PlayerAuraHidden", PlayerAuraHidden},
        {nullptr, "UnitAuraAllowHidden", UnitAuraAllowHidden},
        {nullptr, "RequestSingleAssetQuery", RequestSingleAssetQuery},
        {nullptr, "RequestMultipleAssetQuery", RequestMultipleAssetQuery},
        {nullptr, "Ascension_QueryArenaRewardInfo", Ascension_QueryArenaRewardInfo},
        {nullptr, "Ascension_GetArenaRewardInfo", Ascension_GetArenaRewardInfo},
        {nullptr, "GetAdditionalRoleRequirementsByClassID", GetAdditionalRoleRequirementsByClassID},
        {nullptr, "GetBattlefieldList", GetBattlefieldList},
        {nullptr, "SetCurrencyShow", SetCurrencyShow},
        {nullptr, "SetItemQualityColor", SetItemQualityColor},
        {nullptr, "SetCharacterSelectionSortOrder", SetCharacterSelectionSortOrder},
        {nullptr, "SetWorldTime", SetWorldTime},
        {nullptr, "SetWorldTimeSpeed", SetWorldTimeSpeed},
        {nullptr, "IsQuestSequenced", IsQuestSequenced},
        {nullptr, "SubmitGameFeedback", SubmitGameFeedback},
        {nullptr, "GetItemInfoInstant", GetItemInfoInstant},
        {nullptr, "FindScriptEvents", FindScriptEvents},
        {nullptr, "SetMaskTexture", SetMaskTexture},
        {nullptr, "ClearMaskTexture", ClearMaskTexture},
        {nullptr, "ApplyComponents", ApplyComponents},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

bool AscAddons::IsKnown(const char* name)
{
    for (const KnownAddon& a : g_knownAddons)
        if (a.name == name)
            return true;
    return false;
}

bool AscAddons::InsideProtectedCall() { return g_insideProtectedCall != 0; }

bool AscAddons::IsSecure(const char* name)
{
    for (const KnownAddon& a : g_knownAddons)
        if (a.name == name)
            return a.secure;
    return false;
}

namespace AscGlobalsJ
{
    bool CombatLogDisableAll() { return g_combatLogDisableAll; }   // byte 0x10D3D749
}
