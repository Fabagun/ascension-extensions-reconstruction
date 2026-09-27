#pragma once
#include <cstdint>
#include <vector>

// FUN_102163f0 / FUN_10216660: the original's spell -> first-rank map (the unordered_map at
// 0x10be0578, FNV-1a keyed). Built from the DLL's own SpellRank.dbc (row +8 spell -> +4 first rank),
// then every Talent.dbc rank chain (ranks at +0x10.., each -> rank 1). An unmapped spell is its own
// first rank. ~30 bindings and subsystems go through it.
namespace AscSpellRank
{
    uint32_t FirstRank(uint32_t spellId);

    // FUN_10216520: the spell's rank number within its chain (map 0x10be0558, FUN_10216740), 1 if
    // unmapped.
    uint32_t RankNumber(uint32_t spellId);

    // FUN_102165a0: every spell of the chain whose first rank this is (map 0x10be0598, FUN_10216820:
    // SpellRank.dbc +4 first rank -> +8 spell, then each Talent.dbc chain); empty if none.
    const std::vector<uint32_t>& Chain(uint32_t firstRank);
}
