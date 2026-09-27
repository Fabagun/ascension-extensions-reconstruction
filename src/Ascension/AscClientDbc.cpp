// See AscClientDbc.hpp. Transcribed from FUN_100ae510 (every inserter is this code with another container).
#include <Ascension/AscClientDbc.hpp>
#include <cstring>

namespace
{
    uint32_t& Max(uint32_t c) { return *reinterpret_cast<uint32_t*>(c + 0xC); }
    uint32_t& Min(uint32_t c) { return *reinterpret_cast<uint32_t*>(c + 0x10); }
    uint8_t**& Index(uint32_t c) { return *reinterpret_cast<uint8_t***>(c + 0x20); }
}

uint8_t* AscClientDbc::Row(uint32_t container, uint32_t id)
{
    uint8_t** index = Index(container);
    if (!index || id < Min(container) || id > Max(container))
        return nullptr;
    return index[id - Min(container)];
}

void AscClientDbc::Insert(uint32_t container, uint8_t* row)
{
    const uint32_t id = *reinterpret_cast<const uint32_t*>(row);
    const uint32_t oldMax = Max(container), oldMin = Min(container);
    if (id <= oldMax && id >= oldMin)
    {
        Index(container)[id - oldMin] = row;
        return;
    }
    const uint32_t lo = id < oldMin ? id : oldMin;
    const uint32_t hi = oldMax < id ? id : oldMax;
    const uint32_t size = (hi - lo) * 4 + 4;
    uint8_t** index = static_cast<uint8_t**>(
        reinterpret_cast<void*(__stdcall*)(uint32_t, const char*, uint32_t, uint32_t)>(0x76E540)(size, "WowClientDB.h", 0x178, 0));
    memset(index, 0, size);
    for (uint32_t i = oldMin; i < oldMax; ++i)   // the old maximum is not copied (original bug)
        if (i >= lo && i <= hi)
            if (uint8_t* r = Index(container)[i - oldMin])
                index[i - lo] = r;
    index[id - lo] = row;
    reinterpret_cast<void(__stdcall*)(void*, const char*, uint32_t, uint32_t)>(0x76E5A0)(Index(container), nullptr, 0, 0);
    Index(container) = index;
    Min(container) = lo;
    Max(container) = hi;
}
