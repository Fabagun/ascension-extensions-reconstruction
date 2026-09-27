#pragma once
// The original's MemoryBridge-backed DBC stores. Its DBC manager (FUN_101d3d20, run by the hook on the
// client's DBC initialisation 0x634E00) moves six large DBCs out of the 32-bit client into Ascension's
// MMgr64.exe (AscMemoryBridge): Creature, Quest, ItemAddon, VanityCollection, MythicKeystones,
// SpellAffect. Each gets a loader that uploads its packed records + string block to a bridge table, and
// lookups that query the table and keep the answers in a size-bounded LRU cache (the framework around
// FUN_100c3b00 / FUN_100c3910 / FUN_100c5c40 / FUN_100c19c0).
//
// Done: all six (Creature, Quest, ItemAddon, VanityCollection, MythicKeystones, SpellAffect).
// Returned pointers stay valid until the cache evicts them (0x400 records / 2 MB), as in the original.
#include <array>
#include <cstdint>
#include <vector>

namespace AscBridgeStore
{
    // FUN_101fd240's packed Creature.dbc record (0x1C bytes). `name` is a malloc'd copy owned by the cache.
    struct CreatureRecord
    {
        uint32_t id;          // file column 0
        uint32_t entry;       // column 1
        const char* name;     // the locale slot of the 17-column locstring (columns 2..18)
        uint32_t displayId;   // column 19
        uint32_t f10, f14, f18;   // columns 20..22
    };
    static_assert(sizeof(CreatureRecord) == 0x1C, "CreatureRecord");

    // FUN_100b8c00 -> FUN_100b8cc0: the record whose entry is `entry`, or nullptr.
    const CreatureRecord* CreatureByEntry(uint32_t entry);

    // Quest.dbc rows are stored verbatim (FUN_102137b0: 29 u32, 0x74 bytes): +0 id, +8..+0x10 groups,
    // +0x14 min level, +0x18 reward items x4, +0x28 reward amounts x4, +0x38 required items x6,
    // +0x50 required counts x6, +0x68..+0x70 sort ids.
    const uint8_t* QuestById(uint32_t id);                       // FUN_101dc700 -> FUN_101d0bd0
    std::vector<const uint8_t*> QuestsInGroup(uint32_t group);   // FUN_10212ad0 -> FUN_102123e0
    bool PatchQuest(const uint8_t* row);                         // SMSG 0x56F: FUN_101e2a30 -> FUN_10209860

    // VanityCollection.dbc as FUN_1021a230 packs it (0xB4 bytes, 45 dwords): dword i is the item browser's
    // record slot i -- 0 ID, 1 item, 2 category, 3 artwork (string), 4..6 costs, 7..11 realm kinds, 0xC
    // flags, 0xD..0xE class mask, 0xF race mask, 0x10/0x11 previews, 0x12..0x29 contents, 0x2A
    // description (string), 0x2B additional text (string), 0x2C learned spell. Strings are owned by the
    // cache (a copy keeps pointing at them).
    struct VanityRecord
    {
        uint32_t f[0x2D];
        const char* Str(uint32_t i) const { return reinterpret_cast<const char*>(static_cast<uintptr_t>(f[i])); }
    };
    static_assert(sizeof(VanityRecord) == 0xB4, "VanityRecord");
    struct VanityEntry { uint32_t index; bool found; VanityRecord record; };

    // MythicKeystones.dbc record {+0 key (the keystone item id), +4 dungeon id, +8 level} by key
    // (FUN_101d0500). False when the store has none.
    bool KeystoneById(uint32_t id, uint32_t out[3]);
    void PatchKeystone(const uint32_t rec[3]);   // SMSG 0x56D

    uint32_t VanityCount();
    // FUN_1032ec60's store part (SMSG 0x573): string fields are C strings on input.
    bool PatchVanity(const VanityRecord& r);
    bool VanityRecords(const uint32_t* indexes, uint32_t n, std::vector<VanityEntry>& out);   // FUN_1032dc10
    const VanityRecord* VanityAt(uint32_t index, uint32_t itemId);                          // FUN_1032e090

    // SpellAffect.dbc records {id, modifier spell, affected spell or -(first rank of a chain)} whose
    // modifier spell (+4) is `spell` (FUN_103234d0, over the bridge).
    std::vector<std::array<uint32_t, 3>> SpellAffectsFor(uint32_t spell);

}
