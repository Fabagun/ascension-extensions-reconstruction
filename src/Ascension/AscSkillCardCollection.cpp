// C_SkillCardCollection (0x1031BDD0..0x1031DC30) and the skill-card data it stands on.
//
// Cards: SkillCard.dbc (container 0x10BDF4F8), patched per row by SMSG_PATCH_SKILL_CARD 0x647
// (FUN_101e2f10: 12 u32 then the four strings), indexed by (card id, rank) in the map 0x10BE0678.
//
// Collection manager (FUN_1031bb20 -> 0x10BE3FE0, ctor FUN_10316ac0):
//   +0x00 bonus sealed-pack progress    +0x04 collected {card id, progress, rank} (0xC each)
//   +0x10 pending cards {name, a, b}    +0x20 type -> SkillCardEntryContainer (types 0 1 4 5 6 7)
//   +0x3C 24 sealed-card purchase counts  +0x9C / +0xA0 the purchase in flight (type 0x18 = none, amount)
// Module init 0x1031B9E0: SMSG 0x648 list / 0x649 collected / 0x653 / 0x654 / 0x655 pending /
// 0x657 claim result / 0x66B sealed result / 0x6DE booster result / 0x66C SealedCardCosts row patch;
// glue reset 0x10317DF0. SMSG_GAME_MODE_CHANGED resets the containers (FUN_1031bd50).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSkillCard.hpp>
#include <Ascension/AscSpellText.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace AscScript;
using AscSkillCard::Card;

namespace AscItems { uint32_t BagItemCount(uint32_t item); }   // FUN_10308470 (AscMysticEnchantPreset.cpp)

namespace
{
    const char* const kTypes[9] = {"SKILL_CARD_DEFAULT_NORMAL", "SKILL_CARD_DEFAULT_GOLDEN", "SKILL_CARD_STARTER_NORMAL",
        "SKILL_CARD_STARTER_GOLDEN", "SKILL_CARD_LUCKY_NORMAL", "SKILL_CARD_LUCKY_GOLDEN", "SKILL_CARD_TALENT_NORMAL",
        "SKILL_CARD_TALENT_GOLDEN", "SKILL_CARD_MAX"};   // 0x10B3CA48 (FUN_10214060 parses all nine)
    const char* const kQualities[6] = {"SKILL_CARD_COMMON", "SKILL_CARD_UNCOMMON", "SKILL_CARD_RARE", "SKILL_CARD_EPIC",
        "SKILL_CARD_LEGENDARY", "SKILL_CARD_MAX"};   // 0x10B3A348
    const char* const kExpansions[4] = {"EXPANSION_CLASSIC", "EXPANSION_THE_BURNING_CRUSADE",
        "EXPANSION_WRATH_OF_THE_LICH_KING", "MAX_EXPANSIONS"};   // 0x10B3C9C0
    const char* const kClasses[0x21] = {"CLASS_NONE", "CLASS_WARRIOR", "CLASS_PALADIN", "CLASS_HUNTER", "CLASS_ROGUE",
        "CLASS_PRIEST", "CLASS_DEATH_KNIGHT", "CLASS_SHAMAN", "CLASS_MAGE", "CLASS_WARLOCK", "CLASS_HERO", "CLASS_DRUID",
        "CLASS_BARBARIAN", "CLASS_WITCH_DOCTOR", "CLASS_DEMON_HUNTER", "CLASS_WITCH_HUNTER", "CLASS_STORMBRINGER",
        "CLASS_KNIGHT_OF_XOROTH", "CLASS_GUARDIAN", "CLASS_MONK", "CLASS_SON_OF_ARUGAL", "CLASS_RANGER",
        "CLASS_CHRONOMANCER", "CLASS_NECROMANCER", "CLASS_PYROMANCER", "CLASS_CULTIST", "CLASS_STARCALLER",
        "CLASS_SUN_CLERIC", "CLASS_TINKER", "CLASS_VENOMANCER", "CLASS_REAPER", "CLASS_PRIMALIST",
        "CLASS_RUNEMASTER"};   // 0x10B31920
    const char* const kSealedTypes[25] = {"PURCHASE_SEALED_CARD_TYPE_NORMAL_LEGENDARY_ABILITIES",
        "PURCHASE_SEALED_CARD_TYPE_NORMAL_EPIC_ABILITIES", "PURCHASE_SEALED_CARD_TYPE_NORMAL_RARE_ABILITIES",
        "PURCHASE_SEALED_CARD_TYPE_NORMAL_UNCOMMON_ABILITIES", "PURCHASE_SEALED_CARD_TYPE_NORMAL_COMMON_ABILITIES",
        "PURCHASE_SEALED_CARD_TYPE_NORMAL_LEGENDARY_TALENTS", "PURCHASE_SEALED_CARD_TYPE_NORMAL_EPIC_TALENTS",
        "PURCHASE_SEALED_CARD_TYPE_NORMAL_RARE_TALENTS", "PURCHASE_SEALED_CARD_TYPE_NORMAL_UNCOMMON_TALENTS",
        "PURCHASE_SEALED_CARD_TYPE_NORMAL_COMMON_TALENTS", "PURCHASE_SEALED_CARD_TYPE_GOLDEN_LEGENDARY_ABILITIES",
        "PURCHASE_SEALED_CARD_TYPE_GOLDEN_EPIC_ABILITIES", "PURCHASE_SEALED_CARD_TYPE_GOLDEN_RARE_ABILITIES",
        "PURCHASE_SEALED_CARD_TYPE_GOLDEN_UNCOMMON_ABILITIES", "PURCHASE_SEALED_CARD_TYPE_GOLDEN_COMMON_ABILITIES",
        "PURCHASE_SEALED_CARD_TYPE_GOLDEN_LEGENDARY_TALENTS", "PURCHASE_SEALED_CARD_TYPE_GOLDEN_EPIC_TALENTS",
        "PURCHASE_SEALED_CARD_TYPE_GOLDEN_RARE_TALENTS", "PURCHASE_SEALED_CARD_TYPE_GOLDEN_UNCOMMON_TALENTS",
        "PURCHASE_SEALED_CARD_TYPE_GOLDEN_COMMON_TALENTS", "PURCHASE_SEALED_CARD_TYPE_DEFAULT_PACK",
        "PURCHASE_SEALED_CARD_TYPE_DEFAULT_GOLDEN_PACK", "PURCHASE_SEALED_CARD_TYPE_DEFAULT_TALENT_PACK",
        "PURCHASE_SEALED_CARD_TYPE_DEFAULT_GOLDEN_TALENT_PACK", "PURCHASE_SEALED_CARD_TYPE_MAX"};   // 0x10B5BB70
    const char* const kPurchase[10] = {"PURCHASE_SEALED_CARD_OK", "PURCHASE_SEALED_CARD_UNKNOWN",
        "PURCHASE_SEALED_CARD_RANDOM_MODE_NOT_ENABLED", "PURCHASE_SEALED_CARD_BAD_TYPE",
        "PURCHASE_SEALED_CARD_NO_COST_ENTRY", "PURCHASE_SEALED_CARD_BAD_COST", "PURCHASE_SEALED_CARD_NO_TOKEN",
        "PURCHASE_SEALED_CARD_NO_UNCOLLECTED_CARDS_OF_QUALITY", "PURCHASE_SEALED_CARD_BAD_AMOUNT",
        "PURCHASE_SEALED_CARD_UNCLAIMED_CARDS"};   // 0x10B1EAC8
    const char* const kBooster[6] = {"PURCHASE_SEALED_CARD_BOOSTER_PACK_OK", "PURCHASE_SEALED_CARD_BOOSTER_PACK_UNKNOWN",
        "PURCHASE_SEALED_CARD_BOOSTER_PACK_RANDOM_MODE_NOT_ENABLED", "PURCHASE_SEALED_CARD_BOOSTER_PACK_TOO_FEW",
        "PURCHASE_SEALED_CARD_BOOSTER_PACK_TOO_MANY", "PURCHASE_SEALED_CARD_BOOSTER_PACK_NO_MONEY"};   // 0x10B1C81C
    const char* const kFilters[16] = {"FILTER_NONE", "FILTER_DEFAULT_NORMAL", "FILTER_DEFAULT_GOLDEN",
        "FILTER_LUCKY_NORMAL", "FILTER_LUCKY_GOLDEN", "FILTER_TALENT_NORMAL", "FILTER_TALENT_GOLDEN",
        "FILTER_ACTIVE_CLASS", "FILTER_COMMON", "FILTER_UNCOMMON", "FILTER_RARE", "FILTER_EPIC", "FILTER_LEGENDARY",
        "FILTER_COLLECTED", "FILTER_NOT_COLLECTED", "FILTER_STARTER"};   // 0x10B21EB0

    template <size_t N>
    int IndexIn(const std::string& s, const char* const (&names)[N], int count = static_cast<int>(N))
    {
        for (int i = 0; i < count; ++i)
            if (s == names[i])
                return i;
        return -1;
    }

    // FUN_10196ab0: a config integer with its default.
    int32_t ConfigInt(const char* name, int32_t fallback)
    {
        const int32_t* v = AscConfig::Int(name);
        return v ? *v : fallback;
    }

    uint32_t Money()   // player +0x1248
    {
        uint8_t* p = ActivePlayer();
        return p ? *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0x1248) : 0;
    }

    uint8_t PlayerClass()   // FUN_100a0390: the unit's class byte (+0x5D) for a player / unit
    {
        uint8_t* p = ActivePlayer();
        if (!p)
            return 0;
        const uint32_t t = *reinterpret_cast<const uint32_t*>(p + 0x14);
        if (t != 3 && t != 4)
            return 0;
        return *(*reinterpret_cast<uint8_t**>(p + 8) + 0x5D);
    }

    // ---- the cards --------------------------------------------------------------------------------
    struct Store
    {
        bool loaded = false;
        std::map<uint32_t, std::unique_ptr<Card>> byId;
        std::map<std::pair<uint32_t, uint32_t>, const Card*> byCardRank;   // 0x10BE0678
    };
    Store& Cards()
    {
        static Store s;
        if (s.loaded)
            return s;
        s.loaded = true;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\SkillCard.dbc");
        for (uint32_t i = 0; i < t.Count(); ++i)
        {
            const uint8_t* r = t.RowAt(i);
            if (!r)
                continue;
            auto c = std::make_unique<Card>();
            c->id = AscDbc::Table::U32(r, 0x00);
            c->cardId = AscDbc::Table::U32(r, 0x04);
            c->rank = AscDbc::Table::U32(r, 0x08);
            c->item = AscDbc::Table::U32(r, 0x0C);
            c->spell = AscDbc::Table::U32(r, 0x10);
            c->type = t.Str(r, 0x14);
            c->expansion = t.Str(r, 0x18);
            c->cls = t.Str(r, 0x1C);
            c->quality = t.Str(r, 0x20);
            c->wildcard = AscDbc::Table::U32(r, 0x24);
            c->draft = AscDbc::Table::U32(r, 0x28);
            c->starter = AscDbc::Table::U32(r, 0x2C);
            s.byCardRank[{c->cardId, c->rank}] = c.get();
            s.byId[c->id] = std::move(c);
        }
        return s;
    }

    // Card rows in id order (the containers' populate walks the DBC min..max).
    std::vector<const Card*> AllCards()
    {
        std::vector<const Card*> out;
        for (auto& e : Cards().byId)
            out.push_back(e.second.get());
        return out;
    }

    int CardTypeIndex(const std::string& s) { return IndexIn(s, kTypes); }   // FUN_10214060

    uint32_t MaxRank(uint32_t cardId)   // the consecutive ranks from 1 in the (card, rank) map
    {
        uint32_t r = 0;
        while (AscSkillCard::ByCardRank(cardId, r + 1))
            ++r;
        return r;
    }

    // FUN_10214310 / FUN_102143a0 / FUN_10214350: the card belongs to this mode, expansion and class.
    bool ForMode(const Card& c)
    {
        const uint32_t m = AscGameMode::Mode();
        return (c.draft && ((m >> 3) & 1)) || (c.wildcard && ((m >> 6) & 1));
    }
    bool ForExpansion(const Card& c)
    {
        int e = IndexIn(c.expansion, kExpansions);
        if (e < 0)
        {
            AscLog::Printf("Unexpected Value: %s", c.expansion.c_str());
            e = 3;
        }
        return static_cast<uint32_t>(e) <= RealmInfoSvc::Get().ruleset;   // DAT_10bdb140: realm-object +8 (wire +4)
    }
    int ClassIndex(const std::string& s) { const int i = IndexIn(s, kClasses); return i < 0 ? 0 : i; }   // FUN_10130fa0(s, 0)
    bool ForClass(const Card& c) { return ClassIndex(c.cls) == PlayerClass(); }

    // FUN_10319c40: progress to the next rank, by quality (0xFFFFFFFF for legendary / unknown).
    uint32_t Required(const Card& c)
    {
        switch (IndexIn(c.quality, kQualities))
        {
        case 0: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_UNCOMMON_PROGRESS_REQUIRED", 5));
        case 1: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_RARE_PROGRESS_REQUIRED", 10));
        case 2: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_EPIC_PROGRESS_REQUIRED", 15));
        case 3: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_LEGENDARY_PROGRESS_REQUIRED", 25));
        default: return 0xFFFFFFFF;
        }
    }

    // FUN_10319860: progress a duplicate grants, by quality.
    uint32_t Granted(const Card& c)
    {
        switch (IndexIn(c.quality, kQualities))
        {
        case 0: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_COMMON_PROGRESS_GRANTED", 1));
        case 1: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_UNCOMMON_PROGRESS_GRANTED", 2));
        case 2: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_RARE_PROGRESS_GRANTED", 4));
        case 3: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_EPIC_PROGRESS_GRANTED", 8));
        case 4: return static_cast<uint32_t>(ConfigInt("CONFIG_SKILL_CARD_LEGENDARY_PROGRESS_GRANTED", 10));
        default: return 0;
        }
    }

    // ---- SealedCardCosts (0x10BE01C4) with SMSG 0x66C row patches ------------------------------------
    std::map<uint32_t, std::vector<uint32_t>> g_costPatches;
    // FUN_10213d50: the row's 25 u32, empty when absent.
    std::vector<uint32_t> CostRow(uint32_t id)
    {
        auto it = g_costPatches.find(id);
        if (it != g_costPatches.end())
            return it->second;
        const uint8_t* r = AscDbc::Get("DBFilesClient\\SealedCardCosts.dbc").Row(id);
        if (!r)
            return {};
        std::vector<uint32_t> v(25);
        memcpy(v.data(), r, 100);
        return v;
    }

    // ---- the collection ---------------------------------------------------------------------------
    struct Collected { uint32_t cardId, progress, rank; };
    struct Pending { std::string name; uint32_t a, b; };

    class CardContainer : public AscFilter::Container<const Card*>
    {
    protected:
        std::vector<const Card*> DoPopulate() override { return AllCards(); }   // FUN_102148b0

        // FUN_10214bf0
        bool ArgMatch(uint32_t f, const Card* const& c) override
        {
            static const int kTypeOf[7] = {-1, 0, 1, 4, 5, 6, 7};
            if (f >= 1 && f <= 6)
                return CardTypeIndex(c->type) == kTypeOf[f];
            if (f == 7)
                return ActivePlayer() && static_cast<uint32_t>(ClassIndex(c->cls)) == PlayerClass();
            if (f >= 8 && f <= 12)
                return AscSkillCard::QualityIndex(c->quality, 0) == static_cast<int>(f - 8);
            if (f == 13)
                return AscSkillCard::CollectedRank(c->cardId) == c->rank;
            if (f == 14)
                return AscSkillCard::CollectedRank(c->cardId) < c->rank;
            if (f == 15)
                return c->starter != 0;
            return false;
        }

        uint32_t ArgGroup(uint32_t f) override   // FUN_10214e00
        {
            if (f >= 1 && f <= 6)
                return 1;
            if (f >= 8 && f <= 12)
                return 2;
            return 0;
        }

        // FUN_102149a0: the mode's cards only; with text, the spell's name or description contains it.
        bool TextMatch(const std::string& lower, const Card* const& c) override
        {
            if (!ForMode(*c))
                return false;
            if (lower.empty())
                return true;
            uint8_t rec[0x2A8];
            if (!FetchSpell(c->spell, rec))
                return false;
            const std::string name = AscFilter::Lower(*reinterpret_cast<const char* const*>(rec + 0x220)
                ? *reinterpret_cast<const char* const*>(rec + 0x220) : "");
            if (name.find(lower) != std::string::npos)
                return true;
            return AscFilter::Lower(AscSpellText::Description(c->spell, true, true, 0)).find(lower) != std::string::npos;
        }

        // FUN_10214e50: the lowercase spell name as a big integer (length first, then bytes).
        std::string PopulateKey(const Card* const& c) override
        {
            std::string k;
            uint8_t rec[0x2A8];
            if (!FetchSpell(c->spell, rec))
                return k;
            const char* n = *reinterpret_cast<const char* const*>(rec + 0x220);
            const std::string name = AscFilter::Lower(n ? n : "");
            AscFilter::AppendU32(k, static_cast<uint32_t>(name.size()));
            k += name;
            return k;
        }
    };

    struct Manager
    {
        uint32_t bonusProgress = 0;             // +0x00
        std::vector<Collected> collected;       // +0x04
        std::vector<Pending> pending;           // +0x10
        std::map<int, CardContainer> containers;   // +0x20: types 0 1 4 5 6 7 (FUN_102f42e0)
        uint32_t purchased[24] = {};            // +0x3C
        uint32_t pendingType = 0x18, pendingAmount = 0;   // +0x9C / +0xA0

        Manager()
        {
            for (int t : {0, 1, 4, 5, 6, 7})
                containers[t];
        }
    };
    Manager& Mgr() { static Manager m; return m; }

    // FUN_10319f80: the type's container, or a shared empty one.
    CardContainer& ContainerOf(int type)
    {
        auto it = Mgr().containers.find(type);
        if (it != Mgr().containers.end())
            return it->second;
        static CardContainer empty;
        return empty;
    }

    void ResetContainers()   // FUN_1031bd50
    {
        for (auto& c : Mgr().containers)
            c.second.Reset();
    }

    Collected* Find(uint32_t cardId)
    {
        for (Collected& c : Mgr().collected)
            if (c.cardId == cardId)
                return &c;
        return nullptr;
    }

    // FUN_10319390: 1 at max rank, else progress / required rounded to hundredths; 0 without ranks.
    double Progress(uint32_t cardId)
    {
        const uint32_t max = MaxRank(cardId);
        if (max == 0)
            return 0;
        const Collected* e = Find(cardId);
        const uint32_t rank = e ? e->rank : 0;
        if (max <= rank)
            return 1;
        const Card* c = AscSkillCard::ByCardRank(cardId, rank);
        if (!c)
            return 0;
        const uint32_t progress = e ? e->progress : 0;
        const uint32_t req = Required(*c);
        return std::round(static_cast<double>(progress) * 100.0 / static_cast<double>(static_cast<int32_t>(req))) / 100.0;
    }

    // FUN_10317ef0: a card was collected -- new, upgraded, or a duplicate's progress.
    void OnCollected(const Card& card)
    {
        Collected* e = Find(card.cardId);
        if (!e)
        {
            Mgr().collected.push_back({card.cardId, 0, card.rank});   // FUN_10314430
            AscRuntime::Signal("SKILL_CARD_ADDED", "%u%u", card.cardId, card.rank);
            return;
        }
        if (e->rank < card.rank)
        {
            e->rank = card.rank;
            e->progress = 0;
            AscRuntime::Signal("SKILL_CARD_UPGRADED", "%u%u", card.cardId, card.rank);
            return;
        }
        const uint32_t granted = Granted(card);
        if (granted == 0)
            return;
        const Card* current = AscSkillCard::ByCardRank(e->cardId, e->rank);
        if (current && MaxRank(current->cardId) == current->rank)   // FUN_102145f0: at max rank
        {
            const uint32_t old = Mgr().bonusProgress;
            Mgr().bonusProgress += granted;
            while (static_cast<int32_t>(Mgr().bonusProgress) >= ConfigInt("CONFIG_BONUS_SEALED_CARD_PACKS_PROGRESS_REQUIRED", 100))
            {
                Mgr().bonusProgress -= ConfigInt("CONFIG_BONUS_SEALED_CARD_PACKS_PROGRESS_REQUIRED", 100);
                AscRuntime::Signal("BONUS_SEALED_CARD_PACK_REWARDED");
            }
            AscRuntime::Signal("BONUS_SEALED_CARD_PACK_PROGRESS_UPDATE", "%u%u%u%u", card.cardId, card.rank, old,
                Mgr().bonusProgress);
            return;
        }
        e->progress += granted;
        const Card* at = AscSkillCard::ByCardRank(e->cardId, e->rank);
        if (!at)
            return;
        uint32_t req = Required(*at);
        bool upgraded = false;
        while (req != 0 && req <= e->progress)
        {
            if (!AscSkillCard::ByCardRank(e->cardId, e->rank + 1))
                break;
            ++e->rank;
            e->progress -= req;
            at = AscSkillCard::ByCardRank(e->cardId, e->rank);
            req = at ? Required(*at) : 0;
            upgraded = true;
            AscRuntime::Signal("SKILL_CARD_UPGRADED", "%u%u", e->cardId, e->rank);
            if (req == 0)
                return;
        }
        if (upgraded)
            return;
        AscRuntime::Signal("SKILL_CARD_PROGRESS_UPDATED", "%u%u", at->cardId, at->rank);
    }

    // FUN_103195d0: the SealedCardCosts sum for `amount` purchases of `type` after those already made.
    uint32_t SealedCost(uint32_t type, uint32_t amount)
    {
        if (type == 0x18)
            return 0;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < amount; ++i)
        {
            const std::vector<uint32_t> row = CostRow(Mgr().purchased[type] + 1 + i);
            if (!row.empty())
                sum += row[1 + type];
        }
        return sum;
    }

    uint32_t SealedToken(uint32_t type)   // FUN_10319630
    {
        if (type < 10) return 0x3C1AE;
        if (type < 20) return 0x17C77;
        switch (type)
        {
        case 20: return 0x17E5D;
        case 21: return 0xBE2F6;
        case 22: return 0x17E5E;
        case 23: return 0x17E4E;
        default: return 0;
        }
    }

    bool RandomModeOn() { const uint32_t m = AscGameMode::Mode(); return ((m >> 3) & 1) || ((m >> 6) & 1); }

    // FUN_103177c0: PURCHASE_SEALED_CARD_* reasons (empty = allowed).
    std::vector<uint32_t> CanPurchase(uint32_t type, uint32_t amount)
    {
        std::vector<uint32_t> out;
        if (!RandomModeOn())
            out.push_back(2);
        if (type == 0x18)
            out.push_back(3);
        else
        {
            if (CostRow(Mgr().purchased[type] + 1).empty())
                out.push_back(4);
            else
            {
                const uint32_t cost = SealedCost(type, amount);
                if (cost == 0)
                    out.push_back(5);
                else
                {
                    const uint32_t token = SealedToken(type);
                    if (AscItems::BagItemCount(token) + AscCA::UnitItemCount(ActivePlayer(), token) < cost)
                        out.push_back(6);
                }
            }
            if (type < 20)
            {
                static const int kCategory[4] = {0, 6, 1, 7};   // normal / talent normal / golden / talent golden
                const int category = kCategory[type / 5];
                const int quality = 4 - static_cast<int>(type % 5);
                bool found = false;
                for (const Card* c : AllCards())
                {
                    const int t = CardTypeIndex(c->type);
                    if (t < 0)
                    {
                        AscLog::Printf("Unexpected Value: %s", c->type.c_str());
                        continue;
                    }
                    if (t != category)
                        continue;
                    const int q = AscSkillCard::QualityIndex(c->quality, -1);
                    if (q < 0)
                    {
                        AscLog::Printf("Unexpected Value: %s", c->quality.c_str());
                        continue;
                    }
                    if (q != quality)
                        continue;
                    if (!ForExpansion(*c) || !ForClass(*c) || !ForMode(*c))
                        continue;
                    if (!AscSkillCard::CollectedAtLeast(c->cardId, c->rank))
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                    out.push_back(7);
            }
        }
        const uint32_t limit = (type >= 20 && type <= 23) ? 200 : 1;
        if (amount == 0 || limit < amount)
            out.push_back(8);
        if (!Mgr().pending.empty())
            out.push_back(9);
        return out;
    }

    // FUN_103196c0 body: PURCHASE_SEALED_CARD_BOOSTER_PACK_* reasons.
    std::vector<uint32_t> CanPurchaseBooster(uint32_t count)
    {
        std::vector<uint32_t> out;
        if (!RandomModeOn())
            out.push_back(2);
        if (count == 0)
            out.push_back(3);
        else if (count >= 0x10C7)
            out.push_back(4);
        if (ActivePlayer() && count != 0)
        {
            const uint32_t price = count < 0x10C7 ? count * 500000 : 0xFFFFFFFF;
            if (Money() < price)
                out.push_back(5);
        }
        return out;
    }

    template <size_t N>
    void PushNames(lua_State* L, const std::vector<uint32_t>& codes, const char* const (&names)[N])   // FUN_103160e0 / FUN_10315ef0
    {
        AscLua::lua_createtable(L, static_cast<int>(codes.size()), 0);
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < codes.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            if (codes[i] < N)
                PushStr(L, names[codes[i]]);
            else
                PushStr(L, ("UNEXPECTED_ENUM_VALUE_" + std::to_string(codes[i])).c_str());
            AscLua::lua_settable(L, -3);
        }
    }

    // FUN_10315d50: the PURCHASE_SEALED_CARD_TYPE_* index (0x18 when unknown, logged).
    uint32_t SealedType(const std::string& s)
    {
        const int i = IndexIn(s, kSealedTypes);
        if (i < 0)
        {
            AscLog::Printf("Unexpected Value: %s", s.c_str());
            return 0x18;
        }
        return static_cast<uint32_t>(i);
    }

    // FUN_1009bc60: a card as a table.
    void PushCardTable(lua_State* L, const Card& c)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        auto num = [L](const char* k, uint32_t v) { AscLua::lua_pushstring(L, k); AscLua::lua_pushinteger(L, static_cast<int>(v)); AscLua::lua_settable(L, -3); };
        auto str = [L](const char* k, const std::string& v) { AscLua::lua_pushstring(L, k); PushStr(L, v.c_str()); AscLua::lua_settable(L, -3); };
        auto boo = [L](const char* k, bool v) { AscLua::lua_pushstring(L, k); PushBool(L, v); AscLua::lua_settable(L, -3); };
        num("CardID", c.cardId);
        num("Rank", c.rank);
        num("ItemID", c.item);
        num("SpellID", c.spell);
        str("Type", c.type);
        str("Expansion", c.expansion);
        str("Class", c.cls);
        str("Quality", c.quality);
        boo("IsWildcard", c.wildcard != 0);
        boo("IsDraftMode", c.draft != 0);
        boo("IsStarterCard", c.starter != 0);
        boo("IsCollected", AscSkillCard::CollectedRank(c.cardId) != 0);   // FUN_1031bbc0
        AscLua::lua_pushstring(L, "CollectedProgress");
        AscLua::lua_pushnumber(L, Progress(c.cardId));
        AscLua::lua_settable(L, -3);
        num("CollectedRank", AscSkillCard::CollectedRank(c.cardId));
        AscLua::lua_pushstring(L, "QualityCost");
        AscCA::Row r = AscCA::RowBySpell(c.spell);   // FUN_101498d0
        if (r && AscCA::ActiveBuild())
            AscLua::lua_pushinteger(L, static_cast<int>(AscCA::QualityCostOf(*AscCA::ActiveBuild(), r)));   // FUN_101539f0
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
        num("MaxRank", MaxRank(c.cardId));
    }

    // ---- bindings ---------------------------------------------------------------------------------
    int GetProgress(lua_State* L)   // FUN_1031d330
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const Collected* e = Find(id);
        AscLua::lua_pushinteger(L, static_cast<int>(e ? e->rank : 0));
        AscLua::lua_pushnumber(L, Progress(id));
        return 2;
    }

    int IsCollected(lua_State* L)   // FUN_1031d4d0: (id [, rank]), rank 1 by default
    {
        uint32_t id, rank = 1;
        if (ValidateInput(L, {NUMBER, NUMBER}))
        {
            id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            rank = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        }
        else if (!ReadNumber(L, id))
            return 0;
        PushBool(L, AscSkillCard::CollectedAtLeast(id, rank));
        return 1;
    }

    // FUN_1031c930: progress / required, fixed to four decimals (an ostringstream round trip).
    int GetBonusSealedCardPacksProgress(lua_State* L)
    {
        const double required = ConfigInt("CONFIG_BONUS_SEALED_CARD_PACKS_PROGRESS_REQUIRED", 100);
        char buf[64];
        _snprintf(buf, sizeof(buf), "%.4f", static_cast<double>(static_cast<int32_t>(Mgr().bonusProgress)) / required);
        AscLua::lua_pushnumber(L, strtod(buf, nullptr));
        return 1;
    }

    int GetNumSkillCards(lua_State* L)   // FUN_1031ced0
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        const int t = CardTypeIndex(s);
        if (t < 0)
            return 0;
        AscLua::lua_pushinteger(L, static_cast<int>(ContainerOf(t).Size()));
        return 1;
    }

    int GetSkillCardAtIndex(lua_State* L)   // FUN_1031d200: (type, 1-based index)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string s = CheckString(L, 1);
        const uint32_t i = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const int t = CardTypeIndex(s);
        if (t < 0 || i == 0)
            return 0;
        CardContainer& c = ContainerOf(t);
        if (i - 1 < c.Size() && c.At(i - 1))
            PushCardTable(L, *c.At(i - 1));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_10314150: (type, text, {filter names}) -- the array holds FILTER_* names (FUN_103166c0).
    bool ReadFilterArgs(lua_State* L, std::string& type, std::string& text, std::vector<uint32_t>& filters)
    {
        if (!ValidateInput(L, {STRING, STRING, TABLE}))
            return false;
        type = CheckString(L, 1);
        text = CheckString(L, 2);
        filters.clear();
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 3))
        {
            if (AscLua::lua_isstring(L, -1))
            {
                const int i = IndexIn(std::string(AscLua::lua_tolstring(L, -1, nullptr)), kFilters);
                filters.push_back(i < 0 ? 0 : static_cast<uint32_t>(i));
            }
            else
                filters.push_back(0);
            AscLua::lua_settop(L, -2);
        }
        return true;
    }

    void AddFilter(std::vector<uint32_t>& v, uint32_t f)   // FUN_100b9d70 behind a find
    {
        if (std::find(v.begin(), v.end(), f) == v.end())
            v.push_back(f);
    }

    // FUN_1031dc30: quality words in the text become filters and leave the text; the type's own
    // filter and FILTER_ACTIVE_CLASS are always applied.
    int SetSkillCardFilter(lua_State* L)
    {
        std::string typeName, text;
        std::vector<uint32_t> filters;
        if (!ReadFilterArgs(L, typeName, text, filters))
            return 0;
        const int t = CardTypeIndex(typeName);
        if (t < 0)
            return 0;
        std::string lower = AscFilter::Lower(text);
        auto take = [&](const char* word, uint32_t f) {
            const size_t len = strlen(word);
            if (lower.size() < len)
                return;
            const size_t at = lower.find(word);
            if (at == std::string::npos)
                return;
            AddFilter(filters, f);
            lower.erase(at, len);
        };
        take("uncommon", 9);
        take("common", 8);
        take("rare", 10);
        take("epic", 11);
        take("legendary", 12);
        for (size_t at; lower.size() >= 2 && (at = lower.find("  ")) != std::string::npos;)
            lower.erase(at, 1);
        if (!lower.empty() && lower[0] == ' ')
            lower.erase(0, 1);
        if (!lower.empty() && lower.back() == ' ')
            lower.pop_back();
        static const int kOwn[8] = {1, 2, 0, 0, 3, 4, 5, 6};
        if (kOwn[t])
            AddFilter(filters, static_cast<uint32_t>(kOwn[t]));
        AddFilter(filters, 7);
        ContainerOf(t).ApplyFilter(lower, filters, {});   // FUN_103185e0
        return 0;
    }

    int HasAnySkillCardsCollected(lua_State* L)   // FUN_1031d3c0: ([type])
    {
        std::string s;
        bool any = false;
        if (!ReadString(L, s))
            any = !Mgr().collected.empty();
        else
        {
            const int t = CardTypeIndex(s);
            if (t >= 0)
                for (const Collected& e : Mgr().collected)   // FUN_1031b530
                {
                    const Card* c = AscSkillCard::ByCardRank(e.cardId, e.rank);
                    if (c && CardTypeIndex(c->type) == t)
                    {
                        any = true;
                        break;
                    }
                }
        }
        PushBool(L, any);
        return 1;
    }

    int GetMaxRank(lua_State* L)   // handler_GetMaxRank
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscLua::lua_pushinteger(L, static_cast<int>(MaxRank(id)));
        return 1;
    }

    int GetNumPendingSkillCards(lua_State* L)
    {
        AscLua::lua_pushinteger(L, static_cast<int>(Mgr().pending.size()));
        return 1;
    }

    int GetPendingSkillCardAtIndex(lua_State* L)   // (name, a, b) or nil, nil
    {
        uint32_t i;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        if (i - 1 < Mgr().pending.size())
        {
            const Pending& p = Mgr().pending[i - 1];
            PushStr(L, p.name.c_str());
            AscLua::lua_pushinteger(L, static_cast<int>(p.a));
            AscLua::lua_pushinteger(L, static_cast<int>(p.b));
            return 3;
        }
        AscLua::lua_pushnil(L);
        AscLua::lua_pushnil(L);
        return 2;
    }

    // CMSG 0x656: u32 n, n x {str name, u32 a, u32 b}.
    void SendClaim(const std::vector<const Pending*>& list)
    {
        Packet p(0x656);
        p.U32(static_cast<uint32_t>(list.size()));
        for (const Pending* e : list)
            p.Str(e->name.c_str()).U32(e->a).U32(e->b);
        p.Send();
    }

    int ClaimPendingSkillCard(lua_State* L)   // FUN_1031c4f0: ({names})
    {
        if (!ValidateInput(L, {TABLE}))
            return 0;
        std::vector<std::string> names;   // FUN_1011f710
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 1))
        {
            if (AscLua::lua_isstring(L, -1))
                names.emplace_back(AscLua::lua_tolstring(L, -1, nullptr));
            AscLua::lua_settop(L, -2);
        }
        std::vector<const Pending*> list;
        for (const std::string& n : names)
            for (const Pending& p : Mgr().pending)
                if (p.name == n)
                {
                    list.push_back(&p);
                    break;
                }
        SendClaim(list);
        PushBool(L, true);
        return 1;
    }

    int CanClaimPendingSkillCardAtIndex(lua_State* L)   // true, nil
    {
        uint32_t i;
        if (!ReadNumber(L, i))
            return 0;
        PushBool(L, true);
        AscLua::lua_pushnil(L);
        return 2;
    }

    int PurchaseSealedCard(lua_State* L)   // FUN_1031d930: (type, amount) -> CMSG 0x66A {str, u32}
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string s = CheckString(L, 1);
        const uint32_t amount = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const int t = IndexIn(s, kSealedTypes);   // FUN_10315d00
        if (t < 0)
            return 0;
        Packet(0x66A).Str(s.c_str()).U32(amount).Send();
        Mgr().pendingType = static_cast<uint32_t>(t);
        Mgr().pendingAmount = amount;
        PushBool(L, true);
        return 1;
    }

    int CanPurchaseSealedCard(lua_State* L)   // FUN_1031be10
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const uint32_t type = SealedType(CheckString(L, 1));
        const std::vector<uint32_t> codes = CanPurchase(type, static_cast<uint32_t>(ToInt(CheckNumber(L, 2))));
        PushBool(L, codes.empty());
        if (codes.empty())
            AscLua::lua_pushnil(L);
        else
            PushNames(L, codes, kPurchase);
        return 2;
    }

    int PurchaseSealedCardBoosterPack(lua_State* L)   // FUN_1031db30: CMSG 0x6DD {u32}
    {
        uint32_t count;
        if (!ReadNumber(L, count))
            return 0;
        Packet(0x6DD).U32(count).Send();
        PushBool(L, true);
        return 1;
    }

    int CanPurchaseSealedCardBoosterPack(lua_State* L)   // FUN_1031bfa0
    {
        uint32_t count;
        if (!ReadNumber(L, count))
            return 0;
        const std::vector<uint32_t> codes = CanPurchaseBooster(count);
        PushBool(L, codes.empty());
        if (codes.empty())
            AscLua::lua_pushnil(L);
        else
            PushNames(L, codes, kBooster);
        return 2;
    }

    int GetSealedCardCost(lua_State* L)   // FUN_1031d0f0: token item, cost
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const uint32_t type = SealedType(CheckString(L, 1));
        AscLua::lua_pushinteger(L, static_cast<int>(SealedToken(type)));
        AscLua::lua_pushinteger(L, static_cast<int>(SealedCost(type, static_cast<uint32_t>(ToInt(CheckNumber(L, 2))))));
        return 2;
    }

    // FUN_1031d610 (development realms only): as many of `type` as can be bought in one request.
    int PurchaseAllSealedCards(lua_State* L)
    {
        if (!RealmInfoSvc::Get().Dev())
            return 0;
        std::string s;
        if (!ReadString(L, s))
            return 0;
        const uint32_t type = SealedType(s);
        if (!CanPurchase(type, 1).empty())
            return 0;
        uint32_t n = 1;
        while (CanPurchase(type, n + 1).empty())
            ++n;
        Packet(0x66A).Str(s.c_str()).U32(n).Send();
        Mgr().pendingType = type;
        Mgr().pendingAmount = n;
        return 0;
    }

    // FUN_1031c1a0 (development realms only): every pending card, 150 per CMSG 0x656.
    int ClaimAllPendingSkillCards(lua_State*)
    {
        if (!RealmInfoSvc::Get().Dev())
            return 0;
        const std::vector<Pending>& all = Mgr().pending;
        for (size_t i = 0; i < all.size(); i += 0x96)
        {
            std::vector<const Pending*> chunk;
            for (size_t j = i; j < all.size() && j < i + 0x96; ++j)
                chunk.push_back(&all[j]);
            SendClaim(chunk);
        }
        return 0;
    }

    int GetSealedCardBoosterPackCost(lua_State* L)   // handler_GetSealedCardBoosterPackCost
    {
        uint32_t count;
        if (!ReadNumber(L, count))
            return 0;
        if (count == 0)
            AscLua::lua_pushinteger(L, 0);
        else if (count > 0x10C6)
            AscLua::lua_pushnumber(L, 4294967295.0);
        else
            AscLua::lua_pushnumber(L, static_cast<double>(count * 500000u));
        return 1;
    }

    int GetMaxNumPurchasableSealedCardBoosterPacks(lua_State* L)   // FUN_103196c0
    {
        uint32_t n = 0x10C6;
        for (uint32_t k = 1; k <= 0x10C6; ++k)
            if (!CanPurchaseBooster(k).empty())
            {
                n = k - 1;
                break;
            }
        AscLua::lua_pushinteger(L, static_cast<int>(n));
        return 1;
    }

    // ---- SMSG -------------------------------------------------------------------------------------
    uint32_t U32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    uint8_t U8(CDataStore* p) { const uint8_t v = static_cast<uint8_t>(p->m_buffer[p->m_read]); p->m_read += 1; return v; }
    std::string Str(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    // 0x648: u32 bonus progress, 24 u32 purchase counts, u32 n x {u32 card, u32 progress, u32 rank}.
    void __cdecl OnList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        Manager& m = Mgr();
        m.collected.clear();
        m.bonusProgress = U32(p);
        for (uint32_t& c : m.purchased)
            c = U32(p);
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            Collected e;
            e.cardId = U32(p);
            e.progress = U32(p);
            e.rank = U32(p);
            m.collected.push_back(e);
        }
    }

    // 0x649: u32 n x {u32 card, u32 rank, u8 auto-revealed}.
    void __cdecl OnCollectedPacket(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t cardId = U32(p);
            const uint32_t rank = U32(p);
            const uint8_t reveal = U8(p);
            const Card* c = AscSkillCard::ByCardRank(cardId, rank);
            if (!c)
                continue;
            if (reveal)
                AscRuntime::Signal("SKILL_CARD_AUTO_REVEALED", "%u%u", c->cardId, c->rank);
            OnCollected(*c);
            AscRuntime::Signal("SKILL_CARD_COLLECTED", "%u%u", c->cardId, c->rank);
        }
    }

    // 0x653: the pending list, u32 n x {str, u32, u32}.
    void __cdecl OnPendingList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        Mgr().pending.clear();
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            Pending e;
            e.name = Str(p);
            e.a = U32(p);
            e.b = U32(p);
            Mgr().pending.push_back(e);
        }
    }

    // 0x654: added, each with PENDING_SKILL_CARD_ADDED (count, a, b).
    void __cdecl OnPendingAdded(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            Pending e;
            e.name = Str(p);
            e.a = U32(p);
            e.b = U32(p);
            Mgr().pending.push_back(e);
            AscRuntime::Signal("PENDING_SKILL_CARD_ADDED", "%u%u%u", static_cast<uint32_t>(Mgr().pending.size()), e.a, e.b);
        }
    }

    // 0x655: u32 n x {str}; stops at the first name it doesn't hold. PENDING_SKILL_CARD_REMOVED (index, a, b).
    void __cdecl OnPendingRemoved(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t n = U32(p);
        for (uint32_t i = 0; i < n; ++i)
        {
            const std::string name = Str(p);
            std::vector<Pending>& v = Mgr().pending;
            auto it = std::find_if(v.begin(), v.end(), [&](const Pending& e) { return e.name == name; });
            if (it == v.end())
                return;
            AscRuntime::Signal("PENDING_SKILL_CARD_REMOVED", "%u%u%u", static_cast<uint32_t>(it - v.begin() + 1), it->a, it->b);
            v.erase(it);
        }
    }

    void __cdecl OnClaimResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x657
    {
        const std::string r = Str(p);
        AscRuntime::Signal("CLAIM_SKILL_CARD_RESULT", "%s", r.c_str());
    }

    // 0x66B: a successful purchase adds the request in flight to its type's count.
    void __cdecl OnPurchaseResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string r = Str(p);
        const int code = IndexIn(r, kPurchase);
        if (code < 0)
            AscLog::Printf("Unexpected Value: %s", r.c_str());
        else if (code == 0 && Mgr().pendingType != 0x18)
            Mgr().purchased[Mgr().pendingType] += Mgr().pendingAmount;
        AscRuntime::Signal("PURCHASE_SEALED_CARD_RESULT", "%s", r.c_str());
    }

    void __cdecl OnBoosterResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x6DE
    {
        const std::string r = Str(p);
        AscRuntime::Signal("PURCHASE_SEALED_CARD_BOOSTER_PACK_RESULT", "%s", r.c_str());
    }

    void __cdecl OnCostPatch(void*, uint32_t, uint32_t, CDataStore* p)   // 0x66C: a whole SealedCardCosts row
    {
        std::vector<uint32_t> row(25);
        for (uint32_t& v : row)
            v = U32(p);
        g_costPatches[row[0]] = row;
    }

    // 0x647 (FUN_101e2f10): 12 u32 of the row, then type / expansion / class / quality.
    void __cdecl OnPatchCard(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t raw[12];
        for (uint32_t& v : raw)
            v = U32(p);
        auto c = std::make_unique<Card>();
        c->id = raw[0];
        c->cardId = raw[1];
        c->rank = raw[2];
        c->item = raw[3];
        c->spell = raw[4];
        c->type = Str(p);
        c->expansion = Str(p);
        c->cls = Str(p);
        c->quality = Str(p);
        c->wildcard = raw[9];
        c->draft = raw[10];
        c->starter = raw[11];
        Store& s = Cards();
        auto old = s.byId.find(c->id);
        if (old != s.byId.end())
        {
            *old->second = *c;
            s.byCardRank[{old->second->cardId, old->second->rank}] = old->second.get();
        }
        else
        {
            s.byCardRank[{c->cardId, c->rank}] = c.get();
            s.byId[c->id] = std::move(c);
        }
        ResetContainers();
    }

    void OnGlueScreen()   // 0x10317DF0
    {
        Manager& m = Mgr();
        m.bonusProgress = 0;
        memset(m.purchased, 0, sizeof(m.purchased));
        m.pendingType = 0x18;
        m.pendingAmount = 0;
        m.collected.clear();
        m.pending.clear();
        ResetContainers();
    }

    void Init()
    {
        sDC.AddPacketHandler(0x647, CNetClientCustomPacket((void*)&OnPatchCard, nullptr));
        sDC.AddPacketHandler(0x648, CNetClientCustomPacket((void*)&OnList, nullptr));
        sDC.AddPacketHandler(0x649, CNetClientCustomPacket((void*)&OnCollectedPacket, nullptr));
        sDC.AddPacketHandler(0x653, CNetClientCustomPacket((void*)&OnPendingList, nullptr));
        sDC.AddPacketHandler(0x654, CNetClientCustomPacket((void*)&OnPendingAdded, nullptr));
        sDC.AddPacketHandler(0x655, CNetClientCustomPacket((void*)&OnPendingRemoved, nullptr));
        sDC.AddPacketHandler(0x657, CNetClientCustomPacket((void*)&OnClaimResult, nullptr));
        sDC.AddPacketHandler(0x66B, CNetClientCustomPacket((void*)&OnPurchaseResult, nullptr));
        sDC.AddPacketHandler(0x6DE, CNetClientCustomPacket((void*)&OnBoosterResult, nullptr));
        sDC.AddPacketHandler(0x66C, CNetClientCustomPacket((void*)&OnCostPatch, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
        AscGameMode::OnModeChanged(ResetContainers);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_SkillCardCollection", "GetProgress", GetProgress},
        {"C_SkillCardCollection", "IsCollected", IsCollected},
        {"C_SkillCardCollection", "GetBonusSealedCardPacksProgress", GetBonusSealedCardPacksProgress},
        {"C_SkillCardCollection", "GetNumSkillCards", GetNumSkillCards},
        {"C_SkillCardCollection", "GetSkillCardAtIndex", GetSkillCardAtIndex},
        {"C_SkillCardCollection", "SetSkillCardFilter", SetSkillCardFilter},
        {"C_SkillCardCollection", "HasAnySkillCardsCollected", HasAnySkillCardsCollected},
        {"C_SkillCardCollection", "GetMaxRank", GetMaxRank},
        {"C_SkillCardCollection", "GetNumPendingSkillCards", GetNumPendingSkillCards},
        {"C_SkillCardCollection", "GetPendingSkillCardAtIndex", GetPendingSkillCardAtIndex},
        {"C_SkillCardCollection", "ClaimPendingSkillCard", ClaimPendingSkillCard},
        {"C_SkillCardCollection", "CanClaimPendingSkillCardAtIndex", CanClaimPendingSkillCardAtIndex},
        {"C_SkillCardCollection", "PurchaseSealedCard", PurchaseSealedCard},
        {"C_SkillCardCollection", "CanPurchaseSealedCard", CanPurchaseSealedCard},
        {"C_SkillCardCollection", "PurchaseSealedCardBoosterPack", PurchaseSealedCardBoosterPack},
        {"C_SkillCardCollection", "CanPurchaseSealedCardBoosterPack", CanPurchaseSealedCardBoosterPack},
        {"C_SkillCardCollection", "GetSealedCardCost", GetSealedCardCost},
        {"C_SkillCardCollection", "PurchaseAllSealedCards", PurchaseAllSealedCards},
        {"C_SkillCardCollection", "ClaimAllPendingSkillCards", ClaimAllPendingSkillCards},
        {"C_SkillCardCollection", "GetSealedCardBoosterPackCost", GetSealedCardBoosterPackCost},
        {"C_SkillCardCollection", "GetMaxNumPurchasableSealedCardBoosterPacks", GetMaxNumPurchasableSealedCardBoosterPacks},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscSkillCard
{
    const Card* ByItem(uint32_t item)
    {
        for (auto& e : Cards().byId)
            if (e.second->item == item)
                return e.second.get();
        return nullptr;
    }

    const Card* ByCardRank(uint32_t cardId, uint32_t rank)
    {
        auto& m = Cards().byCardRank;
        auto it = m.find({cardId, rank});
        return it == m.end() ? nullptr : it->second;
    }

    int QualityIndex(const std::string& q, int fallback)
    {
        const int i = IndexIn(q, kQualities);
        if (i < 0)
        {
            AscLog::Printf("Unexpected Value: %s", q.c_str());
            return fallback;
        }
        return i;
    }

    bool CollectedAtLeast(uint32_t cardId, uint32_t rank)
    {
        for (const Collected& e : Mgr().collected)
            if (e.cardId == cardId && rank <= e.rank)
                return true;
        return false;
    }

    void PushCard(lua_State* L, const Card& c) { ::PushCardTable(L, c); }
    int TypeIndex(const std::string& s) { return ::CardTypeIndex(s); }
    const char* TypeName(uint32_t t)
    {
        return t < 9 ? kTypes[t] : nullptr;
    }

    uint32_t CollectedRank(uint32_t cardId)
    {
        const Collected* e = Find(cardId);
        return e ? e->rank : 0;
    }
}
