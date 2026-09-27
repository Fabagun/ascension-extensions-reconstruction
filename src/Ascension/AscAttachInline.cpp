// The client byte patches the original writes unconditionally at attach outside FUN_10a3d840's generated table and
// FUN_10a60d20's (AscAttachLate): the attach init's own inline blocks, FUN_1021c2b0, a block of FUN_10a3d840 and the
// bankEquipmentManager installer FUN_10227970. Found missing by the live exactness audit (step 4, 2026-09-27); the bytes
// are the original's, see AscAttachInline.generated.inc.
#include <Ascension/AscBindings.hpp>
#include <Windows.h>
#include <cstdint>
#include <cstring>

namespace
{
    struct BytePatch
    {
        uint32_t address;
        uint32_t size;
        uint8_t bytes[64];
    };
    const BytePatch kPatches[] = {
#include <Ascension/AscAttachInline.generated.inc>
    };

    void Init()
    {
        for (const BytePatch& p : kPatches)
        {
            DWORD old;
            if (!VirtualProtect(reinterpret_cast<void*>(p.address), p.size, PAGE_EXECUTE_READWRITE, &old))
                continue;
            memcpy(reinterpret_cast<void*>(p.address), p.bytes, p.size);
            VirtualProtect(reinterpret_cast<void*>(p.address), p.size, old, &old);
            FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(p.address), p.size);
        }
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
