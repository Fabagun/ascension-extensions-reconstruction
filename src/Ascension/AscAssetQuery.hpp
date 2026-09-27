#pragma once
// The asset-query manager (FUN_100e15d0). See AscAssetQuery.cpp.
#include <cstdint>

namespace AscAssetQuery
{
    // FUN_100e2d60 with the prefetch flag clear: queue an item-cache request unless it is cached.
    bool TryCacheItem(uint32_t id);
}
