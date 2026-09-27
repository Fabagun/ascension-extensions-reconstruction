#pragma once
// The original's quest addon cache (static 0x10BDF3A8, built by FUN_1006de50 as
// FUN_101af030("questcacheaddon.wdb", 0x731, 0x732, 1)): an unordered_map keyed by quest id, each
// entry holding its record, the callbacks waiting for it, and a loaded flag.
//
// FUN_10115fc0 Get(quest, cb, arg): an unknown quest gets an entry (with cb queued when given) and
// CMSG 0x731 {u32 quest}; a known, loaded one returns its record; a known, pending one queues cb.
// SMSG 0x732 (FUN_101af9f0): u32 quest, u32 points, u32 unk -- only for a requested quest; the
// record becomes {quest, points, unk, quest}, the entry is loaded, and every queued callback runs
// with (quest, arg), then the queue is emptied.
// Users: C_CallboardCache.GetPointsForQuest (FUN_10116400) and C_TemporalContracts.
#include <cstdint>

namespace AscQuestAddon
{
    struct Record { uint32_t quest, points, unk, quest2; };   // entry +0xC

    typedef void(__cdecl* Callback)(uint32_t quest, void* arg);
    const Record* Get(uint32_t quest, Callback cb = nullptr, void* arg = nullptr);
    // FUN_101b0070 (the attach hook on 0x635710): the store is emptied when its key changes.
    void ClearStore();

    // questcacheaddon.wdb (AscCacheFiles.cpp; written at shutdown, never read back): key / file version
    // (0x10BDF3CC), size, every entry's record, and the pending entries dropped (FUN_101afbb0).
    uint32_t& Version();
    size_t Count();
    void ForEachRecord(void (*cb)(const Record& record, void* ctx), void* ctx);
    void PruneUnloaded();
}
