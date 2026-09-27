// Four single-hook installers.
//
//   FUN_10111c60  0x5C2030 (__cdecl(a, b)): runs with the flag 0x10D3D69C set -- a flag nothing reads
//   FUN_102cb370  0x6E09E0 (__thiscall(unit)): after it, when the unit's descriptor +0x598 names an item
//                 for which 0x707330 is true, 0x72B7F0(unit, sheath state != 2, 0) -- the state is
//                 descriptor +0x1E8 for units and players (kinds 3 / 4), 0 otherwise   (FUN_102cac10)
//   FUN_103223a0  the SpellQueueWindow CVar (flags 1, "400", category 3) and 0x805F60 REPLACED: false
//                 without a player or a cast end time (+0xA7C); true once that time has passed; otherwise
//                 whether it is more than SpellQueueWindow ms away                       (FUN_10322440)
//   FUN_101952e0  0x6B2F60 (__stdcall(state), the login-state step; FUN_10194af0): while the realmList
//                 CVar has no string (+0x28 null) the jne at 0x6B2FD4 is NOPed for the call; afterwards it
//                 is written back as 75 59 unconditionally. Once per process, a non-empty "-realm <name>"
//                 command-line value and the realmList string go to 0x4D8BD0 ({6 unused bytes, name[256],
//                 realmList[66]}, both copied unbounded in the original).
//
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscRuntime.hpp>
#include <string>
#include <Ascension/AscRealmData.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CVar.hpp>
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace
{
    bool g_in5C2030 = false;   // 0x10D3D69C (written only)

    typedef int(__cdecl* Fn2_t)(uint32_t, uint32_t);
    Fn2_t g_5C2030 = nullptr;
    int __cdecl Hook5C2030(uint32_t a, uint32_t b)   // FUN_10111c30
    {
        g_in5C2030 = true;
        const int r = g_5C2030(a, b);
        g_in5C2030 = false;
        return r;
    }

    // 0x6E09E0 begins push esi / mov esi,ecx / call 0x7202C0: the call is re-made, then 0x6E09E8.
    __declspec(naked) int __fastcall Original6E09E0(uint8_t*, void*)
    {
        __asm
        {
            push esi
            mov esi, ecx
            mov eax, 0x7202C0
            call eax
            push 0x6E09E8
            ret
        }
    }
    int __fastcall Hook6E09E0(uint8_t* unit, void* edx)   // FUN_102cac10
    {
        const int r = Original6E09E0(unit, edx);
        const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
        const uint32_t lo = *reinterpret_cast<const uint32_t*>(d + 0x598), hi = *reinterpret_cast<const uint32_t*>(d + 0x59C);
        if (!lo && !hi)
            return r;
        void* item = reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, 2);
        if (!item || !reinterpret_cast<int(__thiscall*)(void*)>(0x707330)(item))
            return r;
        const uint32_t kind = *reinterpret_cast<const uint32_t*>(unit + 0x14);
        const uint8_t state = (kind == 3 || kind == 4) ? d[0x1E8] : 0;
        reinterpret_cast<void(__thiscall*)(uint8_t*, int, int)>(0x72B7F0)(unit, state != 2, 0);
        return r;
    }

    // FUN_10228610 (installer FUN_10227970) on 0x5ADA20 (__cdecl(a)): with bankEquipmentManager on and no bank
    // GUID (0xBEB9A0), the GUID is 1 while the client function runs, then 0 again (both through
    // NtProtect in the original).
    void WriteGuid(uint64_t v)
    {
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(0xBEB9A0), 8, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint64_t*>(0xBEB9A0) = v;
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(0xBEB9A0), 8, old, &old);
    }
    bool BankManagerOn()
    {
        const CVar* c = CVar::Lookup("bankEquipmentManager");   // 0x10BE24E0
        return c && *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(c) + 0x30) != 0;
    }
    typedef char(__cdecl* Fn5ADA20_t)(uint32_t);
    Fn5ADA20_t g_5ADA20 = nullptr;
    char __cdecl Hook5ADA20(uint32_t a)
    {
        bool wrote = false;
        if (BankManagerOn() && *reinterpret_cast<const uint64_t*>(0xBEB9A0) == 0)
        {
            WriteGuid(1);
            wrote = true;
        }
        const char r = g_5ADA20(a);
        if (BankManagerOn() && wrote)
            WriteGuid(0);
        return r;
    }

    // CMSG 0x55D: one byte, the bank equipment manager's state.
    void SendBankManager(uint8_t on)
    {
        uint8_t store[0x18];
        reinterpret_cast<void(__thiscall*)(void*)>(0x401050)(store);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x47B0A0)(store, 0x55D);
        reinterpret_cast<void(__thiscall*)(void*, const void*, uint32_t)>(0x47B1C0)(store, &on, 1);
        reinterpret_cast<void(__thiscall*)(void*)>(0x401130)(store);
        void* connection = reinterpret_cast<void*(__cdecl*)()>(0x6B0970)();
        reinterpret_cast<void(__thiscall*)(void*, void*)>(0x632B50)(connection, store);
        reinterpret_cast<void(__thiscall*)(void*)>(0x403880)(store);
    }
    // bankEquipmentManager's callback (FUN_102277e0): in world, 1 for "1", else 0.
    int __cdecl BankManagerChanged(void*, const char*, const char* value, void*)
    {
        if (AscScript::ActivePlayerGuid() != 0)
            SendBankManager(strcmp(value, "1") == 0 ? 1 : 0);
        return 1;
    }
    void BankManagerEnterWorld()   // FUN_10227700
    {
        if (BankManagerOn())
            SendBankManager(1);
    }

    char __cdecl Replace805F60()   // FUN_10322440
    {
        const uint8_t* player = AscScript::ActivePlayer();
        if (!player)
            return 0;
        const uint32_t castEnd = *reinterpret_cast<const uint32_t*>(player + 0xA7C);
        if (!castEnd)
            return 0;
        const CVar* window = CVar::Lookup("SpellQueueWindow");   // 0x10BE4468
        const uint32_t ms = window ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(window) + 0x30) : 0;
        const uint32_t now = reinterpret_cast<uint32_t(__cdecl*)()>(0x86AE20)();
        if (castEnd <= now)
            return 1;
        return castEnd - now > ms;
    }

    void WriteCode16(uint32_t at, uint16_t value)
    {
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(at), 2, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint16_t*>(at) = value;
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(at), 2, old, &old);
    }

    bool g_realmArgDone = false;   // 0x10BDEC0A

    typedef void(__fastcall* Fn6B2F60_t)(void*, void*, uint32_t);
    Fn6B2F60_t g_6B2F60 = nullptr;
    void __fastcall Hook6B2F60(void* ecx, void* edx, uint32_t state)   // FUN_10194af0
    {
        const CVar* realmList = CVar::Lookup("realmList");
        if (*reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(realmList) + 0x28) == nullptr)
            WriteCode16(0x6B2FD4, 0x9090);
        g_6B2F60(ecx, edx, state);
        WriteCode16(0x6B2FD4, 0x5975);
        if (g_realmArgDone)
            return;
        g_realmArgDone = true;
        const char* realm = AscScript::CommandLineArg("realm");
        if (!realm || !*realm)
            return;
        struct { uint8_t unused[6]; char name[256]; char realmList[66]; } select = {};
        strcpy(select.name, realm);
        strcpy(select.realmList, *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(CVar::Lookup("realmList")) + 0x28));
        reinterpret_cast<void(__cdecl*)(void*)>(0x4D8BD0)(&select);
    }

    // 0x454070 (__cdecl(a, path), 5 bytes: push ebp / mov ebp,esp / push -1): the original's detour FUN_102faa20
    // (installer FUN_102f8930) runs the original, then, while a realm data path is set (0x10BCC0B8, that
    // string's length), builds the path with "/" + that data path inserted before its last '/' -- and discards
    // it. Transcribed as is (IMPROVEMENTS-worthy dead work, but exact).
    typedef int(__cdecl* Fn454070_t)(uint32_t, const char*);
    Fn454070_t g_454070 = nullptr;
    int __cdecl Hook454070(uint32_t a, const char* path)
    {
        const int r = g_454070(a, path);
        const std::string& data = AscRealmData::CurrentPath();
        if (!data.empty() && path)
        {
            std::string copy(path);
            const size_t slash = copy.rfind('/');
            if (slash != std::string::npos)
                copy.insert(slash, "/" + data);
            (void)copy;
        }
        return r;
    }

    void Init()
    {
        g_454070 = reinterpret_cast<Fn454070_t>(AscRuntime::Detour(0x454070, 5, reinterpret_cast<void*>(&Hook454070)));
        g_5C2030 = reinterpret_cast<Fn2_t>(AscRuntime::Detour(0x5C2030, 9, reinterpret_cast<void*>(&Hook5C2030)));
        AscRuntime::ReplaceFunction(0x6E09E0, reinterpret_cast<void*>(&Hook6E09E0));
        AscClientOptions::CVarSpec queue{"SpellQueueWindow", "400", 1, 3, nullptr};
        queue.help = "Sets how early you can pre-activate/queue a spell/ability. (In Milliseconds)";
        AscClientOptions::QueueWorldCVar(queue);
        AscRuntime::ReplaceFunction(0x805F60, reinterpret_cast<void*>(&Replace805F60));
        g_5ADA20 = reinterpret_cast<Fn5ADA20_t>(AscRuntime::Detour(0x5ADA20, 9, reinterpret_cast<void*>(&Hook5ADA20)));
        AscClientOptions::QueueWorldCVar({"bankEquipmentManager", "0", 1, 4, reinterpret_cast<void*>(&BankManagerChanged)});
        AscRuntime::OnEnterWorld(&BankManagerEnterWorld);
        g_6B2F60 = reinterpret_cast<Fn6B2F60_t>(AscRuntime::Detour(0x6B2F60, 9, reinterpret_cast<void*>(&Hook6B2F60)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
