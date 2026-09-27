#pragma once
// The client's WowClientDB containers (Ascension.exe 12340): +0xC max id, +0x10 min id, +0x20 the row pointer
// index. The original DLL reads and extends them with ~100 identical per-table functions -- the getters
// FUN_100b2500-style and the inserters FUN_100ae510-style (tools/client_dbc_map.py names the containers).
#include <cstdint>

namespace AscClientDbc
{
    // The getters: the row with this id, or nullptr (index missing, id out of [min, max], empty slot).
    uint8_t* Row(uint32_t container, uint32_t id);

    // The inserters: store `row` (its first dword is the id). Inside [min, max] it takes the slot; outside,
    // a new index spanning the old range and the id is allocated (SMemAlloc 0x76E540, "WowClientDB.h" line
    // 0x178), the old rows are copied -- ids min .. max-1 only, as in the original, so the row that held
    // the old maximum is lost (IMPROVEMENTS.md) -- and the old index freed (SMemFree 0x76E5A0).
    void Insert(uint32_t container, uint8_t* row);
}
