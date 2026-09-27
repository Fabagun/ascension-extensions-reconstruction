// The attach-time client code patches of FUN_10a3d840 (its 27 unconditional sub-writers 0x10A60240 ..
// 0x10A5E020 and its own), byte for byte and in the original's order, from AscAttachPatches.generated.inc
// (tools/gen_attach_patches.py). Found missing by tools/live_patch_check.py, which compares the running
// client with the original's writes.
//
// Among them: mov cl, 3 for the player class byte at 0x57E9A8 / 0x57EB27..; the camera cave at 0x86A62C ->
// 0x9296A2 with the rewritten 0x869DB0 (a 50-pixel clamp); 0x80 -> 0x7D0 at 0x4910DB / 0x4910E2;
// 0x100 -> 0x200 at 0x5663D4; the AFK logout jmp at 0x52B27A (SetAFKLogoutTimer changes it later) and the
// rune-UI class 0 at 0x728AA3 (AscCAMgr changes it later).
//
// Not in the generated table, because their bytes hold an Extensions.dll address: the race tables
// (FUN_10a5e020, RaceTables below), the fade flag 0x744A55 (AscAttachFixes), the vtable slot
// and 0x9F3A88. FUN_10a5b790's two 8-entry level tables and uiScale's default string are written here.
#include <Ascension/AscBindings.hpp>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <windows.h>

namespace
{
    struct Patch
    {
        uint32_t address;
        uint32_t size;
        uint8_t bytes[80];
    };
    const Patch kPatches[] = {
#include <Ascension/AscAttachPatches.generated.inc>
    };

    // The uiScale CVar's default ("1.0", pushed at 0x5238AD) becomes the original's own "0.85"
    // (0x10B610DC); written in table order, between 0x4D17F8 and 0x5238D1 (useUiScale "0" -> "1").
    const char kUiScaleDefault[] = "0.85";

    // FUN_10a5b790: the client's 6-entry level tables at 0xA41E04 (ints 14..4) and 0xA41E1C (floats
    // 150..25) become the original's 8-entry ones (0x10BCD230 / 0x10BCD250); the loop count 5 -> 7 at
    // 0x7F3AFD / 0x7F3B03 comes right after in the table.
    int32_t g_levels[8] = {18, 16, 14, 12, 10, 9, 7, 4};
    float g_distances[8] = {210.0f, 180.0f, 150.0f, 120.0f, 90.0f, 60.0f, 40.0f, 25.0f};

    // FUN_10a5f0e0: the class colour table read at 0x98EE88 (`mov ecx, [class*4 + table]`), 0x84 bytes on
    // the heap -- the client's 12 colours (0xB2DA20), class 10 (Hero) recoloured, then the 21 CoA
    // classes 12..32. 0x98EE8B points at it, written in table order just before 0x707BE2.
    uint32_t* g_classColours = nullptr;
    const uint32_t kCoAColours[21] = {
        0xFF8A3303, 0xFF6EFF00, 0xFFA330C9, 0xFFABD473, 0xFF0070DE, 0xFFD64747, 0xFFC79C6E,
        0xFF00FF99, 0xFFCC9900, 0xFFFFF569, 0xFFF2E699, 0xFF8787ED, 0xFFFF300F, 0xFFE0C7FF,
        0xFFB5FFFF, 0xFFF58CBA, 0xFFA3A3A3, 0xFFFF7D0A, 0xFFC41F3B, 0xFF0D2ED6, 0xFF40C7EB,
    };

    // FUN_10a5e020, the installer's last sub-writer: the character-creation race tables. Eight sites
    // (0x4E157D ..) point at a 64-entry table (0x10D3D830, zeroed); 0x4CDA43 at a 32-entry one (0x10D3D930)
    // holding the client's first 22 entries of 0xB24180 (0x58 bytes, movups) and ten pointers to the DLL's
    // empty string (0x10B1B240). Upstream's CHARCREATIONRACE_FIX, which used to cover these sites, copied
    // only 0x30 bytes and zero-filled the rest.
    uint32_t g_raceTable[64] = {};
    const char* g_raceMemoryTable[32] = {};
    const char kEmpty[] = "";

    void RaceTables();

    void Write(uint32_t address, const void* bytes, uint32_t size)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(address), bytes, size);
        VirtualProtect(reinterpret_cast<void*>(address), size, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    }

    void Init()
    {
        for (const Patch& p : kPatches)
        {
            if (p.address == 0x7F3AFD)
            {
                const uint32_t distances = reinterpret_cast<uint32_t>(g_distances);
                const uint32_t levels = reinterpret_cast<uint32_t>(g_levels);
                for (uint32_t at : {0x7F3BA5u, 0x7F4272u, 0x7F5D8Eu, 0x7F62A7u})   // HOOKED 0x7F3BA5
                    Write(at, &distances, 4);
                for (uint32_t at : {0x7F3BB6u, 0x7F3BBDu, 0x7F4281u, 0x7F4288u, 0x7F5A9Du})
                    Write(at, &levels, 4);
            }
            if (p.address == 0x707BE2)
            {
                g_classColours = static_cast<uint32_t*>(operator new(0x84));
                memcpy(g_classColours, reinterpret_cast<const void*>(0xB2DA20), 12 * 4);
                g_classColours[10] = 0xFFFFD624;
                memcpy(g_classColours + 12, kCoAColours, sizeof(kCoAColours));
                const uint32_t table = reinterpret_cast<uint32_t>(g_classColours);
                Write(0x98EE8B, &table, 4);   // HOOKED 0x98EE8B
            }
            if (p.address == 0x5238D1)
            {
                const uint32_t text = reinterpret_cast<uint32_t>(kUiScaleDefault);
                Write(0x5238AE, &text, 4);   // HOOKED 0x5238AE
            }
            Write(p.address, p.bytes, p.size);
        }
        RaceTables();
    }

    void RaceTables()
    {
        const uint32_t race = reinterpret_cast<uint32_t>(g_raceTable);
        for (uint32_t at : {0x4E157Du, 0x4E16A3u, 0x4E15B5u, 0x4E20EEu, 0x4E222Au, 0x4E2127u, 0x4E1E94u, 0x4E1C3Au})
            Write(at, &race, 4);   // HOOKED 0x4E157D
        memcpy(g_raceMemoryTable, reinterpret_cast<const void*>(0xB24180), 0x58);
        for (int i = 22; i < 32; ++i)
            g_raceMemoryTable[i] = kEmpty;
        const uint32_t memory = reinterpret_cast<uint32_t>(g_raceMemoryTable);
        Write(0x4CDA43, &memory, 4);   // HOOKED 0x4CDA43
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
