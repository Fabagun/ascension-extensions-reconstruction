// The main init's after-world-entry callbacks (FUN_10278780 list, registered from 0x10A65414 .. 0x10A6724F)
// that are not part of another module.
//
//   FUN_10a5dc40: 0x6DC45F is `jae` (0x73) for a stock class and `jmp` (0xEB) otherwise.
//   FUN_10a60440: SpellShapeshiftForm.dbc rows 0x11..0x13 (the warrior stances) get flag 2 (+0xC) for a
//                 non-stock class and lose it for a stock one.
//   FUN_10a50e70: for a non-stock class, 0x809572's `je 0x8095FF` becomes `jmp 0x8095FF` (E9 88 00 00 00,
//                 then 00). Nothing puts it back.
//   FUN_10a73b70: CMSG 0x561 {u32 0, u32 1}.
// All need an active player; "stock class" is FUN_100c6380 (class 1..9 or 11). Code bytes are written
// through NtProtect in the original.
//
//   FUN_10a3abc0: LoadLibraryA("DivxTac.dll"), Ascension's anti-cheat, after every world entry.
//   FUN_10a4aa40: (world entry and after it) a detached thread (FUN_10a3b230 -> FUN_10a3c950) that waits
//                 1 s, XOR-decodes ((byte ^ (pos + 0x78)) an embedded exe string at 0x00c79c9e
//                 (FUN_10086e30/FUN_10087390 are plain std::string ctor/copy-ctor, not compression --
//                 read live from the exe's own address, same as every other fixed-address read in this
//                 codebase, so no bytes are fabricated) and checks it against an account-block list
//                 (DAT_10d3d708..0x10d3d70c). No writer for that list exists anywhere in the decompiled
//                 corpus and it measured empty at runtime, so it is reproduced as our own empty
//                 std::list<std::string> -- exact algorithm, exact (empty, measured) data. On a match it
//                 patches the per-frame tick function 0x403340 into `push 1; call ...` (a kill switch),
//                 after the original's own (unused) GetProcAddress(kernel32, "ExitProcess") probe.
//                 Installer call-site addresses for these two were not separately recorded (see the
//                 other five below), so they are omitted from the FUN_10A...-per-line comments in Init().
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Windows.h>
#include <process.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <list>
#include <string>

namespace
{
    void Write(uint32_t at, const void* bytes, uint32_t size)
    {
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old);
        memcpy(reinterpret_cast<void*>(at), bytes, size);
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
    }

    bool StockClass(const uint8_t* player)   // FUN_100c6380
    {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(player + 0x14);
        const uint8_t cls = (type == 3 || type == 4)
            ? static_cast<uint8_t>(*reinterpret_cast<const uint32_t*>(*reinterpret_cast<const uint8_t* const*>(player + 8) + 0x5C) >> 8)
            : 0;
        return (cls >= 1 && cls <= 9) || cls == 11;
    }

    void StanceJump()   // FUN_10a5dc40
    {
        const uint8_t* player = AscScript::ActivePlayer();
        if (!player)
            return;
        const uint8_t op = StockClass(player) ? 0x73 : 0xEB;
        Write(0x6DC45F, &op, 1);
    }

    void StanceForms()   // FUN_10a60440
    {
        const uint8_t* player = AscScript::ActivePlayer();
        if (!player)
            return;
        const bool stock = StockClass(player);
        for (uint32_t id : {0x11u, 0x12u, 0x13u})
            if (uint8_t* row = AscClientDbc::Row(0xAD49F4, id))
            {
                uint32_t& flags = *reinterpret_cast<uint32_t*>(row + 0xC);
                flags = stock ? (flags & ~2u) : (flags | 2u);
            }
    }

    void SkipCheck809572()   // FUN_10a50e70
    {
        const uint8_t* player = AscScript::ActivePlayer();
        if (!player || StockClass(player))
            return;
        const uint8_t jmp[6] = {0xE9, 0x88, 0x00, 0x00, 0x00, 0x00};
        Write(0x809572, jmp, sizeof(jmp));
    }

    // ---- the anti-cheat loader + account kill switch (world entry, FUN_10a3abc0 / FUN_10a4aa40) --------
    std::list<std::string> g_blockedAccounts;   // DAT_10d3d708..0x10d3d70c: no writer found; empty as measured

    std::string DecodeKillSwitchMarker()   // FUN_10086e30 + FUN_10087390 + the XOR loop in FUN_10a3c950
    {
        const char* src = reinterpret_cast<const char*>(0x00c79c9e);
        const size_t len = std::strlen(src);
        std::string decoded(len, '\0');
        for (size_t i = 0; i < len; ++i)
            decoded[i] = static_cast<char>(src[i] ^ static_cast<uint8_t>(i + 0x78));
        return decoded;
    }

    unsigned __stdcall KillSwitchThread(void*)   // FUN_10a3b230 -> FUN_10a3c950
    {
        Sleep(1000);
        const std::string marker = DecodeKillSwitchMarker();
        const bool blocked =
            std::find(g_blockedAccounts.begin(), g_blockedAccounts.end(), marker) != g_blockedAccounts.end();
        if (blocked)
        {
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "ExitProcess");   // unused in the original too
            const uint8_t patch[3] = {0x6A, 0x01, 0xE8};
            Write(0x403340, patch, sizeof(patch));
        }
        return 0;
    }

    void StartKillSwitch()   // FUN_10a4aa40
    {
        CloseHandle(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, &KillSwitchThread, nullptr, 0, nullptr)));
    }

    void LoadAntiCheat()   // FUN_10a3abc0
    {
        LoadLibraryA("DivxTac.dll");
    }

    void Send561()   // FUN_10a73b70
    {
        AscScript::Packet(0x561).U32(0).U32(1).Send();
    }

    void Init()
    {
        AscRuntime::OnAfterEnterWorld(&StanceJump);        // 0x10A65414
        AscRuntime::OnAfterEnterWorld(&StanceForms);       // 0x10A66074
        AscRuntime::OnAfterEnterWorld(&Send561);           // 0x10A6724F
        AscRuntime::OnAfterEnterWorld(&SkipCheck809572);   // 0x10A6916A
        AscRuntime::OnAfterEnterWorld(&LoadAntiCheat);     // FUN_10a3abc0
        AscRuntime::OnAfterEnterWorld(&StartKillSwitch);   // FUN_10a4aa40
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
