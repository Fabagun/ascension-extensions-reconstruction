#pragma once
// The scaled item-stat store (AscItemStats.cpp, itemstatcache.wdb) and the tooltip state it shares with the
// tooltip hooks (AscTooltipScaling.cpp).
#include <cstdint>
#include <vector>

namespace AscItemStats
{
    // FUN_10280330: the 0xA8-byte record for {item, level}, or nullptr (then CMSG 0x6FF asks for it).
    // +0x00 item, +0x04 level, +0x0C item level, +0x10 10 x {stat type, value}, +0x60 / +0x6C damage
    // {min f, max f, +8}, +0x78 armor, +0x7C armor (reborn), +0x80..+0x94 resistances, +0x98 block,
    // +0x9C random property, +0xA0 required level, +0xA4 sell price.
    const uint8_t* Get(uint32_t item, uint32_t level);
    // FUN_101b0070 (the attach hook on 0x635710): the store is emptied when its key changes.
    void ClearStore();

    // itemstatcache.wdb (AscCacheFiles.cpp): the store's key / file version (0x10BDF384), its size, every
    // entry's 0xA8-byte record in map order, a loaded record from the file, and the pending entries
    // dropped (FUN_101afbb0).
    uint32_t& Version();
    size_t Count();
    void ForEachRecord(void (*cb)(const uint8_t* record, void* ctx), void* ctx);
    void InsertLoaded(const uint8_t* record);
    void PruneUnloaded();

    extern uint64_t g_tooltipGuid;                 // 0x10BE2EB8
    extern uint32_t g_inspectedSlot;               // 0x10BE2F08
    extern std::vector<uint32_t> g_lootRollLevels; // 0x10BE2F74
}
