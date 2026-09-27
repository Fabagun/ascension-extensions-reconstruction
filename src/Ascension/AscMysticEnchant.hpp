#pragma once
// Mystic enchanting (C_MysticEnchant / C_MysticEnchantPreset). See AscMysticEnchant.cpp.
#include <cstdint>

namespace AscMysticEnchant
{
    // FUN_102e3fc0: the enchant spell in slot `slot` (0-based) of the manager's slot vector
    // (0x10BE3928 +0x0C), 0 for an empty or invalid slot.
    uint32_t SlotEnchant(uint32_t slot);

    // FUN_102e9c20 +0x0C: the slot vector itself, for the preset save (SMSG 0x602).
    const uint32_t* Slots(uint32_t& count);

    bool Slotted(uint32_t spell);       // FUN_102E9680: in one of the slots (+0x0C)
    bool Collected(uint32_t spell);     // FUN_102E96B0: in the known collection (+0x00)
    bool Worldforged(uint32_t spell);   // the by-spell row's +0x1C (false without a row)
}
