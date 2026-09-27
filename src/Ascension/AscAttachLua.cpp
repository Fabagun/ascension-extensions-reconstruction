// Lua functions and one packet handler the attach init's hook installer (FUN_10a66d40) detours or
// replaces outright.
//
//   0x48FC30 / 0x48FCE0  GLOBAL_MOUSE_DOWN / GLOBAL_MOUSE_UP ("%u", button) before the client's mouse
//                        handlers -- events the original never registers, so both signals are dropped
//   0x4FBD20  LoggingCombat: turning it on opens "Logs\<date-time> WoWCombatLog.txt" (0xAC7A40 +4 is the
//             client's file name); off closes the open file (0x7754A0) first
//   0x510AC0  REPLACED: bandwidth in, bandwidth out, latency x 0.75 (0x6320D0 on the connection)
//   0x557BE0  one more value: the client lookup 0x5574C0 on (row +0x28 << 24 | id, index - 1)
//   0x567450  REPLACED: an entry of the 72-slot table 0xBEAE20 (or by name, 0x566330) -> its spell's
//             name, rank and id; nil x 3 when the spell is unknown, nothing when there is no entry
//   0x5846E0  REPLACED: GetMerchantItemCostInfo -- honor, arena, number of required items
//   0x584E10  the last value becomes 1 / nil: whether the merchant item has an extended cost
//   0x5854C0  REPLACED: BuyMerchantItem(index [, count]) -- CMSG_BUY_ITEM straight away
//   0x5DB2A0 / 0x5DB810  one more value each (0xC235F4's entry, 0xC235A4)
//   0x635710  the item-stat and quest-addon stores are emptied when its argument changes
//   0x801C90  a packet whose spell has AttributesEx3 0x2000 is dropped; otherwise read again from 2
#include <Ascension/AscBindings.hpp>
#include <Windows.h>
#include <cstring>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscItemStats.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscQuestAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <cstdio>
#include <ctime>
#include <string>

using namespace AscLua;

void AscCacheFiles_LoadItemStats();   // AscCacheFiles.cpp

namespace
{
    // The client's Lua API as the pointer table names it.
    bool IsNumber(lua_State* L, int i) { return reinterpret_cast<int(__cdecl*)(lua_State*, int)>(0x84DF20)(L, i) != 0; }
    bool IsString(lua_State* L, int i) { return reinterpret_cast<int(__cdecl*)(lua_State*, int)>(0x84DF60)(L, i) != 0; }
    double ToNumber(lua_State* L, int i) { return reinterpret_cast<double(__cdecl*)(lua_State*, int)>(0x84E030)(L, i); }
    int GetTop(lua_State* L) { return reinterpret_cast<int(__cdecl*)(lua_State*)>(0x84DBD0)(L); }
    void SetTop(lua_State* L, int i) { reinterpret_cast<void(__cdecl*)(lua_State*, int)>(0x84DBF0)(L, i); }
    void PushNumber(lua_State* L, double v) { reinterpret_cast<void(__cdecl*)(lua_State*, double)>(0x84E2A0)(L, v); }
    void PushInteger(lua_State* L, int32_t v) { reinterpret_cast<void(__cdecl*)(lua_State*, int32_t)>(0x84E2D0)(L, v); }
    void PushString(lua_State* L, const char* s) { reinterpret_cast<void(__cdecl*)(lua_State*, const char*)>(0x84E350)(L, s); }
    void PushNil(lua_State* L) { reinterpret_cast<void(__cdecl*)(lua_State*)>(0x84E280)(L); }
    void Usage(lua_State* L, const char* text) { reinterpret_cast<int(__cdecl*)(lua_State*, const char*, int)>(0x84F280)(L, text, 0); }
    int32_t Truncate(double d) { return static_cast<int32_t>(static_cast<int64_t>(d)); }   // FUN_10ae6370

    typedef int(__cdecl* Lua_t)(lua_State*);
    typedef int(__fastcall* Mouse_t)(void*, void*, const uint8_t*, uint32_t);
    Mouse_t g_48FC30 = nullptr, g_48FCE0 = nullptr;
    Lua_t g_4FBD20 = nullptr, g_557BE0 = nullptr, g_584E10 = nullptr, g_5DB2A0 = nullptr, g_5DB810 = nullptr;
    typedef int(__cdecl* Arg_t)(uint32_t);
    Arg_t g_635710 = nullptr;
    typedef int(__cdecl* Handler_t)(void*, uint32_t, uint32_t, CDataStore*);
    Handler_t g_801C90 = nullptr;

    // The merchant window's state: vendor GUID 0xBFA3E8, item count 0xBFA3F0, 0x20-byte items at 0xBF9128
    // (+0 slot, +4 item, +0x1C ItemExtendedCost id).
    uint64_t MerchantGuid() { return *reinterpret_cast<const uint64_t*>(0xBFA3E8); }
    uint32_t MerchantCount() { return *reinterpret_cast<const uint32_t*>(0xBFA3F0); }
    const uint8_t* MerchantItem(uint32_t i) { return reinterpret_cast<const uint8_t*>(0xBF9128 + i * 0x20); }
    uint32_t U32(const uint8_t* p, uint32_t off) { return *reinterpret_cast<const uint32_t*>(p + off); }

    int __fastcall Hook48FC30(void* self, void* edx, const uint8_t* event, uint32_t b)   // FUN_10a46660
    {
        AscRuntime::Signal("GLOBAL_MOUSE_DOWN", "%u", U32(event, 0x14));
        return g_48FC30(self, edx, event, b);
    }
    int __fastcall Hook48FCE0(void* self, void* edx, const uint8_t* event, uint32_t b)   // FUN_10a466f0
    {
        AscRuntime::Signal("GLOBAL_MOUSE_UP", "%u", U32(event, 0x14));
        return g_48FCE0(self, edx, event, b);
    }

    // FUN_10a6ff20. The name lives in a string the DLL keeps (0x10BCCCFC); the date part is also written
    // to stdout (puts), as the original does.
    std::string g_combatLogName;
    int __cdecl Hook4FBD20(lua_State* L)
    {
        if (GetTop(L) >= 1)
        {
            uint8_t* file = reinterpret_cast<uint8_t*>(0xB759D8);
            if (reinterpret_cast<int(__cdecl*)(lua_State*, int, int)>(0x815500)(L, 1, 1))
            {
                if (*reinterpret_cast<const uint32_t*>(file + 4) == 0)
                {
                    char stamp[0x50];
                    __time64_t now;
                    _time64(&now);
                    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H.%M.%S", _localtime64(&now));
                    puts(stamp);
                    g_combatLogName = std::string("Logs\\") + stamp + " WoWCombatLog.txt";
                    *reinterpret_cast<const char**>(0xAC7A40 + 4) = g_combatLogName.c_str();
                }
            }
            else
            {
                if (*reinterpret_cast<const uint32_t*>(file + 4) != 0)
                    reinterpret_cast<void(__cdecl*)(uint32_t)>(0x7754A0)(*reinterpret_cast<const uint32_t*>(file + 4));
                *reinterpret_cast<uint32_t*>(file + 4) = 0;
            }
        }
        return g_4FBD20(L);
    }

    int __cdecl Replace510AC0(lua_State* L)   // FUN_10a76ff0
    {
        void* connection = reinterpret_cast<void*(__cdecl*)()>(0x6B0970)();
        float in = 0.0f, out = 0.0f;
        uint32_t latency = 0;
        reinterpret_cast<void(__thiscall*)(void*, float*, float*, uint32_t*)>(0x6320D0)(connection, &in, &out, &latency);
        PushNumber(L, in);
        PushNumber(L, out);
        PushNumber(L, static_cast<double>(latency) * 0.75);
        return 3;
    }

    int __cdecl Hook557BE0(lua_State* L)   // FUN_10a76b70
    {
        const int r = g_557BE0(L);
        const int32_t id = Truncate(ToNumber(L, 1));
        const int32_t index = Truncate(ToNumber(L, 2));
        const uint8_t* row = AscClientDbc::Row(0xAD4040, static_cast<uint32_t>(id));
        if (!row)
            return r;
        const uint32_t key = U32(row, 0x28) << 24 | (static_cast<uint32_t>(id) & 0xFFFFFF);
        const uint8_t* found = reinterpret_cast<const uint8_t*(__cdecl*)(uint32_t, int32_t)>(0x5574C0)(key, index - 1);
        if (!found)
            return r;
        PushInteger(L, static_cast<int32_t>(U32(found, 8)));
        return r + 1;
    }

    int __cdecl Replace567450(lua_State* L)   // FUN_10a76c10
    {
        uint8_t* object = nullptr;
        if (IsNumber(L, 1))
        {
            const uint32_t index = static_cast<uint32_t>(Truncate(ToNumber(L, 1)) - 1);
            if (index >= 0x48)
                return 0;
            object = reinterpret_cast<uint8_t*(__thiscall*)(void*, uint32_t, int)>(0x6F6020)(
                reinterpret_cast<void*>(0xBEAF68), reinterpret_cast<const uint32_t*>(0xBEAE20)[index], 0);
        }
        else if (IsString(L, 1))
            object = reinterpret_cast<uint8_t*(__cdecl*)(const char*)>(0x566330)(AscLua::lua_tolstring(L, 1, nullptr));
        if (!object)
            return 0;
        uint8_t rec[0x2A8] = {};
        if (AscScript::FetchSpell(U32(object, 0x564), rec))
        {
            const char* name = *reinterpret_cast<const char* const*>(rec + 0x220);
            const char* rank = *reinterpret_cast<const char* const*>(rec + 0x224);
            PushString(L, name ? name : "");
            PushString(L, rank ? rank : "");
            PushInteger(L, static_cast<int32_t>(U32(object, 0x564)));
            return 3;
        }
        PushNil(L);
        PushNil(L);
        PushNil(L);
        return 3;
    }

    int __cdecl Replace5846E0(lua_State* L)   // FUN_10a76d80
    {
        if (!IsNumber(L, 1))
            Usage(L, "Usage: GetMerchantItemCostInfo(index)");
        const uint32_t index = static_cast<uint32_t>(Truncate(ToNumber(L, 1) - 1.0));
        if (static_cast<int32_t>(index) >= 0 && index < MerchantCount() && MerchantGuid() != 0)
        {
            const uint8_t* item = MerchantItem(index);
            if (U32(item, 4) != 0 && U32(item, 0x1C) != 0)
            {
                double count;
                if (const uint8_t* cost = AscClientDbc::Row(0xAD3E00, U32(item, 0x1C)))   // ItemExtendedCost
                {
                    PushNumber(L, static_cast<double>(U32(cost, 4)));
                    PushNumber(L, static_cast<double>(U32(cost, 8)));
                    uint8_t n = 0;
                    for (uint32_t off = 0x10; off <= 0x20; off += 4)
                        n += U32(cost, off) != 0;
                    count = static_cast<double>(n);
                }
                else
                {
                    PushNumber(L, -1.0);
                    PushNumber(L, -1.0);
                    count = -1.0;
                }
                PushNumber(L, count);
                return 3;
            }
        }
        PushNumber(L, 0.0);
        PushNumber(L, 0.0);
        PushNumber(L, 0.0);
        return 3;
    }

    int __cdecl Hook584E10(lua_State* L)   // FUN_10a76f40 (the index is not range-checked)
    {
        if (!IsNumber(L, 1))
            Usage(L, "Usage: GetMerchantItemCostInfo(index)");
        const int32_t index = static_cast<int32_t>(ToNumber(L, 1) - 1.0);
        const int r = g_584E10(L);
        SetTop(L, -2);
        if (U32(MerchantItem(static_cast<uint32_t>(index)), 0x1C) != 0)
            PushNumber(L, 1.0);
        else
            PushNil(L);
        return r;
    }

    int __cdecl Replace5854C0(lua_State* L)   // FUN_10a6ba50
    {
        if (!IsNumber(L, 1))
            Usage(L, "Usage: BuyMerchantItem(index)");
        uint8_t* player = AscScript::ActivePlayer();
        if (!player)
            return 0;
        reinterpret_cast<void(__cdecl*)(int, int)>(0x519280)(1, 1);   // FUN_10111cf0
        uint32_t count = 0;
        if (IsNumber(L, 2) && ToNumber(L, 2) >= 1.0)
            count = static_cast<uint32_t>(Truncate(ToNumber(L, 2)));
        const uint32_t index = static_cast<uint32_t>(Truncate(ToNumber(L, 1) - 1.0));
        if (index < MerchantCount() && MerchantGuid() != 0)
        {
            const uint8_t* item = MerchantItem(index);
            // FUN_1008d790: CMSG_BUY_ITEM {u64 vendor, u32 item, u32 slot, u32 count, u8 1}
            AscScript::Packet(0x1A2).U64(MerchantGuid()).U32(U32(item, 4)).U32(U32(item, 0)).U32(count).U8(1).Send();
        }
        return 0;
    }

    int __cdecl Hook5DB2A0(lua_State* L)   // FUN_10a770a0
    {
        const uint32_t n = static_cast<uint32_t>(Truncate(ToNumber(L, 1)));
        int r = g_5DB2A0(L);
        const uint8_t* table = reinterpret_cast<const uint8_t*>(0xC235F4);
        if (static_cast<int32_t>(n) > 0 && n <= U32(table, 4))
        {
            const uint32_t* const* entries = *reinterpret_cast<const uint32_t* const* const*>(table + 8);
            PushInteger(L, static_cast<int32_t>(*entries[n - 1]));
            ++r;
        }
        return r;
    }

    int __cdecl Hook5DB810(lua_State* L)   // FUN_10a77100
    {
        const int r = g_5DB810(L);
        PushInteger(L, *reinterpret_cast<const int32_t*>(0xC235A4));
        return r + 1;
    }

    // FUN_10a76b10: the original, then once (0x10D3DC6C) the two stores' packet handlers (FUN_101b0040,
    // registered by their own modules here) and the itemstatcache.wdb load (FUN_101b0010 -> FUN_101b00c0,
    // AscCacheFiles.cpp); then FUN_101b0070: a store whose key (its file version once loaded) differs from
    // the argument is emptied and takes the argument as its key.
    bool g_cachesLoaded = false;   // 0x10D3DC6C
    int __cdecl Hook635710(uint32_t key)
    {
        const int r = g_635710(key);
        if (!g_cachesLoaded)
        {
            AscCacheFiles_LoadItemStats();
            g_cachesLoaded = true;
        }
        if (AscItemStats::Version() != key)
        {
            AscItemStats::ClearStore();
            AscItemStats::Version() = key;
        }
        if (AscQuestAddon::Version() != key)
        {
            AscQuestAddon::ClearStore();
            AscQuestAddon::Version() = key;
        }
        return r;
    }

    int __cdecl Hook801C90(void* param, uint32_t opcode, uint32_t time, CDataStore* p)   // FUN_10a4ef40
    {
        uint64_t guid = 0;
        reinterpret_cast<void(__cdecl*)(CDataStore*, uint64_t*)>(0x76DC20)(p, &guid);
        uint32_t spell;
        memcpy(&spell, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        uint8_t rec[0x2A8];
        if (AscScript::FetchSpell(spell, rec) && (U32(rec, 0x1C) & 0x2000))
            return 1;
        p->m_read = 2;
        return g_801C90(param, opcode, time, p);
    }

    template <class T> T Hook(uint32_t target, uint32_t prologue, void* fn)
    {
        return reinterpret_cast<T>(AscRuntime::Detour(target, prologue, fn));
    }

    void Init()
    {
        g_48FC30 = Hook<Mouse_t>(0x48FC30, 7, reinterpret_cast<void*>(&Hook48FC30));   // hooked 0x48FC30
        g_48FCE0 = Hook<Mouse_t>(0x48FCE0, 7, reinterpret_cast<void*>(&Hook48FCE0));   // hooked 0x48FCE0
        g_4FBD20 = Hook<Lua_t>(0x4FBD20, 7, reinterpret_cast<void*>(&Hook4FBD20));     // hooked 0x4FBD20
        AscRuntime::ReplaceFunction(0x510AC0, reinterpret_cast<void*>(&Replace510AC0));
        g_557BE0 = Hook<Lua_t>(0x557BE0, 6, reinterpret_cast<void*>(&Hook557BE0));     // hooked 0x557BE0
        AscRuntime::ReplaceFunction(0x567450, reinterpret_cast<void*>(&Replace567450));
        AscRuntime::ReplaceFunction(0x5846E0, reinterpret_cast<void*>(&Replace5846E0));
        g_584E10 = Hook<Lua_t>(0x584E10, 9, reinterpret_cast<void*>(&Hook584E10));     // hooked 0x584E10
        AscRuntime::ReplaceFunction(0x5854C0, reinterpret_cast<void*>(&Replace5854C0));
        g_5DB2A0 = Hook<Lua_t>(0x5DB2A0, 9, reinterpret_cast<void*>(&Hook5DB2A0));     // hooked 0x5DB2A0
        g_5DB810 = Hook<Lua_t>(0x5DB810, 9, reinterpret_cast<void*>(&Hook5DB810));     // hooked 0x5DB810
        g_635710 = Hook<Arg_t>(0x635710, 7, reinterpret_cast<void*>(&Hook635710));     // hooked 0x635710
        g_801C90 = Hook<Handler_t>(0x801C90, 9, reinterpret_cast<void*>(&Hook801C90)); // hooked 0x801C90
        // Between the 0x718A00 and 0x424B50 hooks the installer NOPs 14 bytes at 0x7BD4AF for good: the
        // "CMap::SafeOpen() failed %s" report (push esi / push 0xA40218 / call 0x772AA0 / add esp,8)
        // after CMap's ten open retries (only the protection is restored afterwards).
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(0x7BD4AF), 14, PAGE_EXECUTE_READWRITE, &old);
        memset(reinterpret_cast<void*>(0x7BD4AF), 0x90, 14);
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(0x7BD4AF), 14, old, &old);
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
