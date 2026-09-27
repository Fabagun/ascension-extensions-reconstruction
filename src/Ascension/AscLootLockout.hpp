#pragma once
#include <cstdint>

// LootLockoutMgr (AscLootLockout.cpp) for other subsystems.
namespace AscLootLockout
{
    uint32_t SecondsRemaining(uint64_t guid, uint32_t encounter);   // FUN_102948d0
}
