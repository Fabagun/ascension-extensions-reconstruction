#pragma once
// Item addon records -- the original's ItemAddon store (FUN_101a2690, table singleton FUN_101a4fd0 =
// 0x10BDEDE0): DBFilesClient\ItemAddon.dbc in the MemoryBridge store (AscBridgeStore), keyed by item
// entry. SMSG_PATCH_ITEM_ADDON 0x569 (FUN_101e1800) patches single records.
#include <cstdint>
#include <string>
#include <unordered_map>

namespace AscItemAddon
{
    // FUN_1020e270's packed row (0x40 bytes). The strings are malloc'd copies owned by the cache.
    struct Record
    {
        uint32_t rowId;              // file column 0
        uint32_t id;                 // column 1, the item entry (the key)
        const char* name;            // the locale slot of the name locstring (columns 2..18)
        const char* description;     // the locale slot of the description locstring (19..35)
        uint32_t fields[12];         // columns 36..47, record +0x10..+0x3C
        uint32_t Quality() const { return fields[0]; }
    };
    static_assert(sizeof(Record) == 0x40, "Record");

    // FUN_101a2690: nullptr when the store has no such item. Valid until the cache evicts it.
    const Record* Find(uint32_t itemId);

    // The item text + quality the UI shows (the 0x48-byte result FUN_1020e9c0 returns).
    struct Info
    {
        std::string name, description;
        uint32_t quality = 0, flags = 0;   // record +0x10 / +0x14
    };
    // FUN_1020ef10: batch read that bypasses the record cache -- one 0x6D over the unique items (key
    // field 4, no string fields), then one 0x6E over the unique name / description offsets. `out` is
    // cleared first and holds only the items found; false (and `out` cleared) when a query fails.
    bool FetchInfo(const uint32_t* items, uint32_t n, std::unordered_map<uint32_t, Info>& out);
    // FUN_1020e9c0: FetchInfo for one item, else the cached record (Find), else empty strings and zeros.
    Info GetInfo(uint32_t itemId);
}
