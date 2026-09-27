#pragma once
// Skill cards: SkillCard.dbc rows (+ SMSG_PATCH_SKILL_CARD 0x647 patches) and the collection manager
// (FUN_1031bb20, 0x10BE3FE0). See AscSkillCardCollection.cpp.
#include <cstdint>
#include <string>
struct lua_State;

namespace AscSkillCard
{
    // A SkillCard.dbc row (0x30): +0 id, +4 card id, +8 rank, +0xC item, +0x10 spell, +0x14 type,
    // +0x18 expansion, +0x1C class, +0x20 quality (strings), +0x24 wildcard, +0x28 draft, +0x2C starter.
    struct Card
    {
        uint32_t id = 0, cardId = 0, rank = 0, item = 0, spell = 0;
        std::string type, expansion, cls, quality;
        uint32_t wildcard = 0, draft = 0, starter = 0;
    };

    const Card* ByItem(uint32_t item);                        // FUN_102142a0
    const Card* ByCardRank(uint32_t cardId, uint32_t rank);   // the (card, rank) map 0x10BE0678
    int QualityIndex(const std::string& q, int fallback);     // FUN_101a33d0 (SKILL_CARD_COMMON..MAX)
    bool CollectedAtLeast(uint32_t cardId, uint32_t rank);    // FUN_1031bbf0
    uint32_t CollectedRank(uint32_t cardId);                  // FUN_103195a0
    void PushCard(lua_State* L, const Card& c);               // FUN_1009bc60
    int TypeIndex(const std::string& s);                      // FUN_10214060 (-1 when unknown)
    const char* TypeName(uint32_t t);                         // FUN_102f3fd0 (SKILL_CARD_* incl. MAX)

    // C_SkillCard's slot manager (FUN_102f5140, AscSkillCardSlots.cpp).
    bool AnyUnblocked(uint32_t spec);                         // FUN_102f4f70
}
