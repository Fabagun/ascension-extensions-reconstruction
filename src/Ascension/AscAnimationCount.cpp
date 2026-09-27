// The client's animation count (FUN_10a51180, reached through 0x10A3AB70 in the 0x634E00 set -- see
// AscBridgeStore.cpp). The client hard-codes stock AnimationData.dbc's 506 rows (0x1FA) as an immediate in
// 44 places (bounds checks and array sizes); after the DBCs load, each becomes the loaded row count
// (AnimationData's container 0xAD30C8, +8 = rows), so Ascension's extra animations are accepted.
// The original writes each dword through NtProtectVirtualMemory, one site at a time, in this order.
#include <cstdint>
#include <windows.h>

namespace AscAnimationCount
{
    void Apply()
    {
        static const uint32_t kSites[] = {
            0x738E7F, 0x7176DD, 0x7177F9, 0x71E603, 0x71E63A, 0x71E653, 0x73ABC3, 0x73AC63,
            0x73BC36, 0x73E42E, 0x73E72C, 0x73E749, 0x7486F6, 0x748763, 0x825F4F, 0x825FB1,
            0x825FD5, 0x82605A, 0x826404, 0x960843, 0x96085D, 0x71D307, 0x71D3A7, 0x71D477,
            0x71D5B7, 0x71D827, 0x71D967, 0x71E25C, 0x71E360, 0x723E65, 0x724255, 0x724518,
            0x737CC8, 0x737CEC, 0x737D12, 0x73B548, 0x73B58C, 0x73B5DB, 0x73BA62, 0x7484F1,
            0x7485F7, 0x747BA8, 0x74858C, 0x748F6F,
        };
        const uint32_t rows = *reinterpret_cast<const uint32_t*>(0xAD30D0);
        for (uint32_t at : kSites)
        {
            DWORD old;
            if (!VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &old))
                continue;
            *reinterpret_cast<uint32_t*>(at) = rows;
            VirtualProtect(reinterpret_cast<void*>(at), 4, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), 4);
        }
    }
}
