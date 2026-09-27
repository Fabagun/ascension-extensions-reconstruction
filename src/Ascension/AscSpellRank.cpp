#include <Ascension/AscSpellRank.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscScript.hpp>
#include <unordered_map>

namespace AscSpellRank
{
namespace
{
    const uint32_t kTalentDbc = 0xAD4B5C;   // FUN_100b1120 / FUN_100b1060 / FUN_100b2720

    // Every Talent.dbc rank chain (ranks at +0x10..), in id order: fn(spell, rank index 0..8, row).
    template <class Fn> void ForEachTalentRank(Fn fn)
    {
        const uint32_t minId = *reinterpret_cast<const uint32_t*>(kTalentDbc + 0x10);
        const uint32_t maxId = *reinterpret_cast<const uint32_t*>(kTalentDbc + 0x0C);
        for (uint32_t id = minId; id < maxId + 1; ++id)
        {
            const uint8_t* talent = AscScript::ClientDbcRow(kTalentDbc, id);
            if (!talent)
                continue;
            for (uint32_t r = 0; r < 9; ++r)
                if (const uint32_t spell = *reinterpret_cast<const uint32_t*>(talent + 0x10 + r * 4))
                    fn(spell, r, talent);
        }
    }

    std::unordered_map<uint32_t, uint32_t>& Map()
    {
        static std::unordered_map<uint32_t, uint32_t> m;
        static bool built = false;
        if (!built)
        {
            built = true;
            AscDbc::Table& ranks = AscDbc::Get("DBFilesClient\\SpellRank.dbc");
            for (uint32_t id = ranks.MinId(); ranks.Loaded() && id <= ranks.MaxId(); ++id)
                if (const uint8_t* row = ranks.Row(id))
                    m[AscDbc::Table::U32(row, 8)] = AscDbc::Table::U32(row, 4);
            ForEachTalentRank([](uint32_t spell, uint32_t, const uint8_t* talent) {
                m[spell] = *reinterpret_cast<const uint32_t*>(talent + 0x10);
            });
        }
        return m;
    }

    // FUN_10216740 fills the map at 0x10be0558: SpellRank.dbc row +8 spell -> +0xC rank number, then
    // each Talent.dbc rank -> its 1-based position in the chain.
    std::unordered_map<uint32_t, uint32_t>& RankMap()
    {
        static std::unordered_map<uint32_t, uint32_t> m;
        static bool built = false;
        if (!built)
        {
            built = true;
            AscDbc::Table& ranks = AscDbc::Get("DBFilesClient\\SpellRank.dbc");
            for (uint32_t id = ranks.MinId(); ranks.Loaded() && id <= ranks.MaxId(); ++id)
                if (const uint8_t* row = ranks.Row(id))
                    m[AscDbc::Table::U32(row, 8)] = AscDbc::Table::U32(row, 0xC);
            ForEachTalentRank([](uint32_t spell, uint32_t r, const uint8_t*) { m[spell] = r + 1; });
        }
        return m;
    }
}

const std::vector<uint32_t>& Chain(uint32_t firstRank)
{
    static std::unordered_map<uint32_t, std::vector<uint32_t>> chains;
    static bool built = false;
    static const std::vector<uint32_t> none;
    if (!built)
    {
        built = true;
        AscDbc::Table& ranks = AscDbc::Get("DBFilesClient\\SpellRank.dbc");
        for (uint32_t id = ranks.MinId(); ranks.Loaded() && id <= ranks.MaxId(); ++id)
            if (const uint8_t* row = ranks.Row(id))
                chains[AscDbc::Table::U32(row, 4)].push_back(AscDbc::Table::U32(row, 8));
        ForEachTalentRank([](uint32_t spell, uint32_t, const uint8_t* talent) {
            chains[*reinterpret_cast<const uint32_t*>(talent + 0x10)].push_back(spell);
        });
    }
    auto it = chains.find(firstRank);
    return it == chains.end() ? none : it->second;
}

uint32_t FirstRank(uint32_t spellId)
{
    auto& m = Map();
    auto it = m.find(spellId);
    return it != m.end() ? it->second : spellId;
}

uint32_t RankNumber(uint32_t spellId)
{
    auto& m = RankMap();
    auto it = m.find(spellId);
    return it != m.end() ? it->second : 1;
}
}
