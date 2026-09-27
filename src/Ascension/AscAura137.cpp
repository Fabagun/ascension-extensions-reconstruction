// FUN_10a73ea0: the code patch for aura type 0x137. On: the two jne at 0x988E48 / 0x989053 are NOPed, the
// jne at 0x989B97 too, and the byte at 0x989B9A becomes 1; off: the stock bytes. Driven by
//   - list 0x10BE2B6C (before 0x724820) / 0x10BE2B8C (before 0x71E930): 0x10A4E2E0 / 0x10A4E330 turn it
//     on / off for a player when the spell has an aura-0x137 effect;
//   - SMSG 0x6E9 / 0x6E8 (0x10A4F4B0 / 0x10A4F490, registered by FUN_10a66100): u64 guid, then on / off.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <windows.h>

namespace
{
    void Patch(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    }

    void SetAura137Patch(bool on)   // FUN_10a73ea0
    {
        static const uint8_t kJne[6] = {0x0F, 0x85, 0xAA, 0x01, 0x00, 0x00};
        static const uint8_t kNop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        static const uint8_t kJne2[2] = {0x75, 0x12}, kNop2[2] = {0x90, 0x90};
        Patch(0x988E48, on ? kNop6 : kJne, 6);
        Patch(0x989053, on ? kNop6 : kJne, 6);
        Patch(0x989B97, on ? kNop2 : kJne2, 2);
        const uint8_t flag = on ? 1 : 0;
        Patch(0x989B9A, &flag, 1);
    }

    bool HasAura137(const uint8_t* rec)
    {
        for (uint32_t i = 0; i < 3; ++i)
            if (*reinterpret_cast<const uint32_t*>(rec + 0x17C + i * 4) == 0x137)
                return true;
        return false;
    }

    bool __cdecl BeforeApply(void* unit, uint32_t, uint32_t rec)   // 0x10A4E2E0
    {
        if (reinterpret_cast<char(__thiscall*)(void*)>(0x4CEE50)(unit) && HasAura137(reinterpret_cast<const uint8_t*>(rec)))
            SetAura137Patch(true);
        return true;
    }
    bool __cdecl BeforeRemove(void* unit, uint32_t, uint32_t rec)   // 0x10A4E330
    {
        if (reinterpret_cast<char(__thiscall*)(void*)>(0x4CEE50)(unit) && HasAura137(reinterpret_cast<const uint8_t*>(rec)))
            SetAura137Patch(false);
        return true;
    }

    void __cdecl OnOff(void*, uint32_t, uint32_t, CDataStore* p) { p->m_read += 8; SetAura137Patch(false); }   // 0x6E8
    void __cdecl OnOn(void*, uint32_t, uint32_t, CDataStore* p) { p->m_read += 8; SetAura137Patch(true); }     // 0x6E9

    void Init()
    {
        AscRuntime::OnBefore724820(&BeforeApply, 0x10A683D0);
        AscRuntime::OnBefore71E930(&BeforeRemove, 0x10A683DA);
        sDC.AddPacketHandler(0x6E8, CNetClientCustomPacket((void*)&OnOff, nullptr));
        sDC.AddPacketHandler(0x6E9, CNetClientCustomPacket((void*)&OnOn, nullptr));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
