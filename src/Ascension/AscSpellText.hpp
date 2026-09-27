#pragma once
// Spell text the original builds for searching and tooltips. See AscSpellText.cpp.
#include <cstdint>
#include <string>
#include <vector>

namespace AscSpellText
{
    // FUN_101ac910 / FUN_101ac3d0: the spell's description as the client formats it (0x57ABC0), then
    // through the DLL's @-markup pass (FUN_101a8e30). "" when the spell or its description is missing.
    std::string Description(uint32_t spellId, bool a, bool cached, int c);

    // FUN_101a8e30: rewrite the @-markup in `text` in place. `a` = show everything (as if SHIFT were held),
    // `stripQuotes` drops one pair of enclosing quotes, `spell` = the spell being described (@wflocation,
    // @flyingmountspeed), `collect` moves @s / @re / @wflocation results into `collected` instead of
    // inlining them, `noRequirements` drops @req / @unlockby. True when anything changed.
    bool Markup(std::string& text, bool a, bool cached, int stripQuotes, uint32_t spell,
                std::vector<std::string>* collected, bool collect, bool noRequirements);

    // The per-spell caches FUN_101c1ce0 searches: DAT_10bdf458 (the name, Spell +0x220) and
    // FUN_101c6300 / DAT_10bdf498 (FUN_101c1b60: the shapeshift forms the spell requires).
    const std::string& Name(uint32_t spellId);
    const std::string& FormText(uint32_t spellId);
}
