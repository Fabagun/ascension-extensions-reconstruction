#pragma once
#include <cstdint>

// The WildcardMgr's desired / undesired lists (AscWildcardSets.cpp).
namespace AscWildcardSets
{
    // FUN_10a34d10: {entry, kind} is on the undesired list (the suggestion manager's filter).
    bool IsUndesired(uint32_t id, uint8_t kind);
}
