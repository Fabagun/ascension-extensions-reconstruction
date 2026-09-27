// The CharacterAdvancement learn rules (FUN_1015cbf0 installs them, FUN_10151480 runs them) and the
// learnable-order pass built on them (FUN_101557d0).
//
// Each rule is a static object with a vtable {?, applies(ctx, row), ok(ctx, row)}, stored in slot n of the
// build's unit-rule vector (+0x1A8, ctx = the owner's client object) or build-rule vector (+0x1B4, ctx =
// the build). Slot n is CA_LEARN_* value n; a slot fails when its rule applies and is not ok. Every
// installed rule's applies() is 0x1015B7C0 (`mov al,1; ret 8`), so only ok() is transcribed. Slot 29
// (a per-build object, FUN_1015b7d0) and slots 7, 32, 33, 34, 36 use 0x1015B7C0 for ok() too -- those
// checks are the server's.
//
// Which slots a build gets depends on FUN_10157450's flags: 1 normal, 2 draft, 4 wildcard (1 only when
// neither), | 8 when qualities are on. A named build then loses slots 8, 16 and 19 (FUN_10150a80).
//
// Dependencies not transcribed yet, and what the original does without their data:
//   slot 18 BUILD_CREATOR  FUN_10101370 / FUN_100fd750 (C_BuildCreator templates): no template -> fail.
//   slot 20 NO_TALENTS_CHALLENGE  FUN_1013b8b0 / FUN_1013b350 (C_Challenge rules): no challenge -> ok.
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <algorithm>
#include <cstring>
#include <map>

using namespace AscScript;

namespace AscCA
{
namespace
{
#include <Ascension/AscCAEnums.generated.inc>

    AscDbc::Table& Essence() { return AscDbc::Get("DBFilesClient\\CharacterAdvancementEssence.dbc"); }
    const char* RowStr(Row r, uint32_t off) { return AscDbc::Get("DBFilesClient\\CharacterAdvancement.dbc").Str(r, off); }

    bool IsTalent(Row r) { return RowType(r) == 2; }
    bool IsAbilityLike(Row r) { const int t = RowType(r); return t == 1 || t == 4; }   // FUN_101c6770

    // Client state the unit rules read.
    const uint32_t kMapDbc = 0xAD4160;   // Map.dbc container; +8 = instance type (1 party, 2 raid, 3 pvp, 4 arena)
    const uint8_t* CurrentMap() { return ClientDbcRow(kMapDbc, *reinterpret_cast<const uint32_t*>(0xBD088C)); }
    uint32_t MapType(const uint8_t* map) { return *reinterpret_cast<const uint32_t*>(map + 8); }
    const uint8_t* Descriptor(const uint8_t* unit) { return *reinterpret_cast<uint8_t* const*>(unit + 8); }
    bool InCombat(const uint8_t* unit) { return (*reinterpret_cast<const uint32_t*>(Descriptor(unit) + 0xEC) >> 19) & 1; }
    uint64_t UnitGuid(const uint8_t* unit) { return *reinterpret_cast<const uint64_t*>(Descriptor(unit)); }
    bool CoAClass(uint8_t c) { return c >= 12 && c <= 32; }

    // The mode's required-level column (FUN_10153af0).
    uint32_t RequiredLevel(const Build& b, Row r)
    {
        if (b.draft)
            return RowU32(r, 0x6C);
        return b.wildcard ? RowU32(r, 0x70) : RowU32(r, 0x68);
    }

    // ---- qualities (slots 21..24) -----------------------------------------------------------------
    int QualityOf(const Build& b, Row r)   // FUN_101539b0: the mode's quality string -> kQualities index
    {
        const char* s = RowStr(r, b.draft ? 0x50 : b.wildcard ? 0x60 : 0x40);
        for (int i = 0; i < 9; ++i)
            if (strcmp(s, kQualities[i]) == 0)
                return i;
        return 0;
    }

    uint32_t QualityCost(const Build& b, Row r)   // FUN_101539f0
    {
        if (b.wildcard && RowU32(r, 0x74) && b.HasGroup(RowU32(r, 0x74), RowU32(r, 0)))
            return 0;
        for (uint32_t i = 0; i < 3; ++i)
            if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
                if (b.Has(m))
                    return 0;
        return b.draft ? RowU32(r, 0x54) : b.wildcard ? RowU32(r, 0x64) : RowU32(r, 0x44);
    }

    uint32_t QualitySum(const Build& b, int quality)   // FUN_10153f60
    {
        uint32_t total = 0;
        for (const Entry& e : b.entries)
            if (QualityOf(b, e.row) == quality)
                total += QualityCost(b, e.row);
        return total;
    }

    bool QualityOk(const Build& b, Row r, int quality, const char* stem, int32_t def)
    {
        if (QualityOf(b, r) != quality)
            return true;
        const uint32_t ruleset = RealmInfoSvc::Get().ruleset;
        const char* exp = ruleset == 0 ? "CLASSIC" : ruleset == 1 ? "THE_BURNING_CRUSADE" : "WRATH_OF_THE_LICH_KING";
        const std::string name = std::string("CONFIG_CHARACTER_ADVANCEMENT_QUALITIES_MAX_") + stem + "_ABILITIES_" + exp;
        const int32_t* v = AscConfig::Int(name.c_str());   // FUN_10196ab0
        const uint32_t max = static_cast<uint32_t>(v ? *v : def) & 0xFF;
        return QualityCost(b, r) + QualitySum(b, quality) <= max;
    }

    // ---- wildcard level pools (slot 28) -----------------------------------------------------------
    // FUN_10152b30: per (class, draft, wildcard, specDraft, u2c) key, Essence.dbc's {level, AE, TE} rows in
    // level order.
    struct Pool { uint32_t level, ae, te; };
    uint64_t PoolKey(uint32_t cls, bool f3, bool f4, bool f5, bool f6)
    {
        return (static_cast<uint64_t>(cls) << 4) | (f3 << 3) | (f4 << 2) | (f5 << 1) | static_cast<uint64_t>(f6);
    }

    std::map<uint64_t, std::vector<Pool>>& Pools()
    {
        static std::map<uint64_t, std::vector<Pool>> pools;
        static uint32_t builtMin = 0, builtMax = 0;
        AscDbc::Table& t = Essence();
        if (pools.empty() || builtMin != t.MinId() || builtMax != t.MaxId())
        {
            pools.clear();
            builtMin = t.MinId();
            builtMax = t.MaxId();
            for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
                if (Row r = t.Row(id))
                    pools[PoolKey(RowU32(r, 8), RowU32(r, 0xC) != 0, RowU32(r, 0x10) != 0, RowU32(r, 0x14) != 0, RowU32(r, 0x18) != 0)]
                        .push_back({RowU32(r, 4), RowU32(r, 0x1C), RowU32(r, 0x20)});
            for (auto& kv : pools)
                std::sort(kv.second.begin(), kv.second.end(), [](const Pool& a, const Pool& b) { return a.level < b.level; });
        }
        return pools;
    }

    uint32_t ConfigInt(const char* name, int32_t def)
    {
        const int32_t* v = AscConfig::Int(name);
        return static_cast<uint32_t>(v ? *v : def);
    }

    // FUN_10153510 (AE, ability roll) / FUN_10153750 (TE, talent roll): the lowest pool level whose budget
    // covers what is spent plus one roll, capped at the build's level.
    uint32_t PoolLevel(const Build& b, bool talent)
    {
        const uint32_t need = talent ? b.GlobalTE(0) + ConfigInt("CONFIG_WILDCARD_TALENT_ROLL_COST", 1)
                                     : b.GlobalAE(0) + ConfigInt("CONFIG_WILDCARD_ABILITY_ROLL_COST", 2);
        auto& pools = Pools();
        auto it = pools.find(PoolKey(b.classKey, b.draft, b.wildcard, b.specDraft, b.u2c != 0));
        if (it == pools.end())
            return b.level;
        const std::vector<Pool>& v = it->second;
        auto p = std::lower_bound(v.begin(), v.end(), need, [talent](const Pool& x, uint32_t n) { return (talent ? x.te : x.ae) < n; });
        if (p == v.end())
            return b.level;
        auto budget = [talent](const Pool& x) { return talent ? x.te : x.ae; };
        while (p + 1 != v.end() && budget(*(p + 1)) == budget(*p))
            ++p;
        uint32_t level = p->level;
        if (talent)
            level = std::max(level, ConfigInt("CONFIG_WILDCARD_TALENT_ROLL_POOL_START_LEVEL", 10));
        return std::min(level, b.level);
    }

    // ---- build rules (ctx = the build) ------------------------------------------------------------
    bool BuildRuleOk(uint32_t slot, Build& b, Row r)
    {
        const uint32_t flags = RowU32(r, 0x124);
        switch (slot)
        {
        case 3:  return r != nullptr;                                        // FUN_10160de0 BAD_ABILITY
        case 4:  return !(flags & 0x20000);                                  // FUN_10160f00 DISPLAY_ENTRY
        case 5:  return RequiredLevel(b, r) <= b.level;                      // FUN_10160fe0 LOW_LEVEL
        case 6:  return b.RankOf(RowU32(r, 0)) < RowMaxRank(r);             // FUN_10160db0 ALREADY_KNOWN
        case 9:                                                              // FUN_101644b0 WRONG_EXPANSION
        {
            const char* exp = RowStr(r, 0x120);
            uint32_t index = 0;
            for (uint32_t i = 0; i < 4; ++i)
                if (strcmp(exp, kExpansions[i]) == 0)
                {
                    index = i;
                    break;
                }
            return index <= RealmInfoSvc::Get().ruleset;
        }
        case 10: return !(flags & 8);                                        // FUN_10160ef0 DISABLED
        case 11: return ClassTypeAdmits(r, static_cast<uint8_t>(b.classKey)); // FUN_10164490 WRONG_CLASS
        case 12:                                                             // FUN_10160d20 ALREADY_KNOW_A_STARTING_NODE
            if (!r[0xE0])
                return true;
            for (const Entry& e : b.entries)
                if (Row er = FindRow(e.id))
                    if (er[0xE0])
                        return false;
            return true;
        case 13:                                                             // FUN_101616c0 MISSING_CONNECTED_ENTRIES
        {
            bool any = false;
            for (uint32_t i = 0; i < 15; ++i)
                any = any || RowU32(r, 0xE4 + i * 4) != 0;
            if (!any)
                return true;
            for (uint32_t i = 0; i < 15; ++i)
            {
                const uint32_t c = RowU32(r, 0xE4 + i * 4);
                if (!c)
                    continue;
                if (RowHasFlag(r, 0x200000))
                {
                    Row cr = FindRow(c);
                    if (cr && b.RankOf(c) == RowMaxRank(cr) && b.Has(c))   // FUN_10155090(c, max rank)
                        return true;
                }
                else if (b.Has(c))
                    return true;
            }
            return false;
        }
        case 14:                                                             // FUN_10161790 MISSING_REQUIRED_ID
            for (uint32_t i = 0; i < 3; ++i)
                if (const uint32_t q = RowU32(r, 8 + i * 4))
                    if (!b.Has(q))
                        return false;
            return true;
        case 15: return RowVisible(r);                                       // FUN_10164620 WRONG_REALM
        case 17: return !(flags & 1);                                        // FUN_10160ee0 DEPRECATED
        case 18:                                                             // FUN_10160df0 BUILD_CREATOR
            if ((b.specDraft || b.u64 || b.u65) && (b.u65 || RowU32(r, 0x74) != 1))
                return false;   // the template (C_BuildCreator) is not transcribed: none is found
            return true;
        case 21: return QualityOk(b, r, 2, "UNCOMMON", 8);                   // FUN_10163ed0
        case 22: return QualityOk(b, r, 3, "RARE", 8);                       // FUN_10163b90
        case 23: return QualityOk(b, r, 4, "EPIC", 4);                       // FUN_101634f0
        case 24: return QualityOk(b, r, 5, "LEGENDARY", 2);                  // FUN_10163830
        case 25: return !(RowU32(r, 0x74) == 2 && b.HasGroup(2, 0));         // FUN_10164460 WILDCARD_TAME_SPELLS
        case 26: return !(flags & 0x1000);                                   // FUN_10164320 WILDCARD_MASTERIES
        case 27: return !(flags & 0x10000);                                  // FUN_10164250 WILDCARD_DISABLED
        case 28:                                                             // FUN_10164270 WILDCARD_LOW_LEVEL
        {
            const uint32_t req = RequiredLevel(b, r);
            if (IsAbilityLike(r))
                return req <= b.level && req <= PoolLevel(b, false);
            if (IsTalent(r))
                return req <= b.level && req <= PoolLevel(b, true);
            return req <= b.level;
        }
        case 37:                                                             // FUN_10160f20 GROUP
        {
            const uint32_t g = RowU32(r, 0x74);
            return !(g != 0 && g != 1 && b.HasGroup(g, 0));
        }
        case 38:                                                             // FUN_101624e0 NOT_ENOUGH_INVESTED_AE
        {
            const uint32_t cls = RowU32(r, 0x80), tab = RowU32(r, 0x84);
            if (const uint32_t n = RowU32(r, 0x88))
                if (b.GlobalAE(n) < n)
                    return false;
            if (const uint32_t n = RowU32(r, 0x90))
                if (b.ClassAE(cls, n) < n)
                    return false;
            const uint32_t n = RowU32(r, 0x98);
            return !n || n <= b.TabAE(cls, tab, n);
        }
        case 39:                                                             // FUN_10163020 NOT_ENOUGH_INVESTED_TE
        {
            const uint32_t cls = RowU32(r, 0x80), tab = RowU32(r, 0x84);
            if (const uint32_t n = RowU32(r, 0x8C))
                if (b.GlobalTE(n) < n)
                    return false;
            if (const uint32_t n = RowU32(r, 0x94))
                if (b.ClassTE(cls, n) < n)
                    return false;
            const uint32_t n = RowU32(r, 0x9C);
            return !n || n <= b.TabTE(cls, tab, n);
        }
        case 40:                                                             // FUN_10162c70 NOT_ENOUGH_INVESTED_POINTS
        {
            const uint32_t n = RowU32(r, 0xA4);
            return !n || n <= b.ClassPoints(RowU32(r, 0x80), n);
        }
        case 41:                                                             // FUN_10161230 MISSING_AE
        {
            const uint32_t cost = b.AECost(r, 1);
            return cost == 0 || cost <= b.RemainingAE();
        }
        case 42:                                                             // FUN_101617d0 MISSING_TE
        {
            const uint32_t cost = b.TECost(r, 1);
            return cost == 0 || cost <= b.RemainingTE();
        }
        case 43:                                                             // FUN_10161960 MODE_RESTRICTED
            if (!(flags & 0x1800000))
                return true;
            if ((flags & 0x800000) && b.wildcard)
                return true;
            return (flags & 0x1000000) && b.draft;
        case 44: return RowU32(r, 0x74) == 1;                                // FUN_101634e0 NOT_WILDCARD
        default: return true;
        }
    }

    // ---- unit rules (ctx = the owner's client object) ---------------------------------------------
    bool UnitRuleOk(uint32_t slot, const uint8_t* unit, Row r)
    {
        const uint32_t flags = RowU32(r, 0x124);
        switch (slot)
        {
        case 8:                                                              // FUN_10163300 NOT_IN_BATTLEGROUNDS
        {
            if (IsTalent(r))
                return true;
            if (CoAClass(UnitClassOf(unit)) && !(flags & 0x40000))
                return true;
            const uint8_t* map = CurrentMap();
            if (!map)
                return true;
            const uint32_t type = MapType(map);
            if (type != 4)
            {
                if (type != 3)
                    return true;
                Build* b = BuildOf(UnitGuid(unit));
                if (RowU32(r, 0x74) != 1 || (b && !b->HasGroup(1, 0)))
                    return reinterpret_cast<int(__cdecl*)(uint32_t)>(0x548D10)(0x1097) != 1;
            }
            return false;
        }
        case 16:                                                             // FUN_10163410 NOT_IN_COMBAT
        {
            if (!InCombat(unit) || CoAClass(UnitClassOf(unit)))
                return true;
            const uint8_t* map = CurrentMap();
            if (!IsTalent(r) || !map)
                return false;
            return MapType(map) == 1 || MapType(map) == 2;
        }
        case 19:                                                             // FUN_10160f50 INVULNERABLE
        {
            void* u = const_cast<uint8_t*>(unit);
            const uint32_t count = reinterpret_cast<uint32_t(__thiscall*)(void*)>(0x4F8850)(u);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t* aura = reinterpret_cast<const uint8_t*(__thiscall*)(void*, uint32_t)>(0x556E10)(u, i);
                uint8_t rec[0x2A8];
                if (aura && FetchSpell(*reinterpret_cast<const uint32_t*>(aura + 8), rec))
                    for (uint32_t k = 0; k < 3; ++k)
                        if (*reinterpret_cast<const uint32_t*>(rec + 0x17C + k * 4) == 0x27)   // SCHOOL_IMMUNITY
                            return false;
            }
            return true;
        }
        case 20:                                                             // FUN_101619b0 NO_TALENTS_CHALLENGE
            return true;   // needs an active challenge with rule 14; C_Challenge holds none until transcribed
        case 30:                                                             // FUN_101634c0 NOT_WHILE_DEAD
            return !reinterpret_cast<bool(__thiscall*)(void*)>(0x6DAC10)(const_cast<uint8_t*>(unit));
        case 31:                                                             // FUN_10164430 WILDCARD_NOT_IN_COMBAT
            return !InCombat(unit) || RowU32(r, 0x74) != 1;
        case 35:                                                             // FUN_10164340 WILDCARD_NOT_IN_BGS
        {
            Build* b = BuildOf(UnitGuid(unit));
            if (!b)
                return false;
            if (CoAClass(UnitClassOf(unit)) && !(flags & 0x40000))
                return true;
            const uint8_t* map = CurrentMap();
            if (map && (MapType(map) == 4 || MapType(map) == 3) && RowU32(r, 0x74) == 1)
                return !b->HasGroup(1, 0);
            return true;
        }
        default: return true;
        }
    }

    // FUN_1015cbf0's slots per mode (see the header comment).
    const uint32_t kAnyBuild[] = {3, 4, 6, 9, 10, 17, 43};
    const uint32_t kAnyUnit[] = {7, 30};
    const uint32_t kNormalDraftBuild[] = {41, 42, 5, 11, 12, 13, 38, 39, 40, 14, 15, 18, 37};
    const uint32_t kNormalDraftUnit[] = {8, 16, 19, 20};
    const uint32_t kWildcardBuild[] = {44, 25, 26, 27, 28};
    const uint32_t kWildcardUnit[] = {29, 31, 32, 33, 34, 35, 36};
    const uint32_t kQualityBuild[] = {21, 22, 23, 24};

    template <size_t N> void Set(uint64_t& mask, const uint32_t (&slots)[N])
    {
        for (uint32_t s : slots)
            mask |= 1ull << s;
    }

    bool Contains(const std::vector<uint32_t>& v, uint32_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }
}

uint32_t PoolLevelOf(const Build& b, bool talent) { return PoolLevel(b, talent); }   // FUN_10153510 / FUN_10153750

void Build::Prepare()
{
    uint32_t mode = (draft ? 2 : 0) | (wildcard ? 4 : 0);
    if (!mode)
        mode = 1;
    if (qualities)
        mode |= 8;
    learnUnitRules = learnBuildRules = 0;
    if (mode & 7)
    {
        Set(learnBuildRules, kAnyBuild);
        Set(learnUnitRules, kAnyUnit);
    }
    if (mode & 3)
    {
        Set(learnBuildRules, kNormalDraftBuild);
        Set(learnUnitRules, kNormalDraftUnit);
    }
    if (mode & 4)
    {
        Set(learnBuildRules, kWildcardBuild);
        Set(learnUnitRules, kWildcardUnit);
    }
    if (mode & 8)
        Set(learnBuildRules, kQualityBuild);
    // The installer marks these slots in the +0x1C0 bitset as it installs them (the helpers mark none).
    learnMask = 0;
    for (uint32_t s = 3; s <= 30; ++s)
        if ((s <= 20 || (s >= 25 && s <= 27) || s == 30) && (((learnUnitRules | learnBuildRules) >> s) & 1))
            learnMask |= 1ull << s;
    unlearnUnitRules = unlearnBuildRules = 0;
    if (mode & 7)
        for (uint32_t s : {3u, 7u, 13u})
            unlearnBuildRules |= 1u << s;
    if (mode & 3)
    {
        for (uint32_t s : {4u, 8u, 6u})
            unlearnUnitRules |= 1u << s;
        for (uint32_t s : {9u, 17u, 18u, 19u, 11u, 5u, 10u, 16u})
            unlearnBuildRules |= 1u << s;
    }
    if (mode & 4)
    {
        for (uint32_t s : {12u, 20u})
            unlearnUnitRules |= 1u << s;
        for (uint32_t s : {21u, 14u, 15u})
            unlearnBuildRules |= 1u << s;
    }
    // Purge slots: 3 NOT_IN_BATTLEGROUNDS, 4 NOT_IN_COMBAT, 5 NO_PURGE_ITEM on the unit; 6 NO_KNOWN_*,
    // 7 HERO_CLASS on the build. Same layout for abilities and talents.
    for (int t = 0; t < 2; ++t)
    {
        purgeUnitRules[t] = purgeBuildRules[t] = 0;
        if (mode & 3)
        {
            purgeBuildRules[t] |= 1u << 6;
            purgeUnitRules[t] |= (1u << 5) | (1u << 3) | (1u << 4);
        }
        if (mode & 7)
            purgeBuildRules[t] |= 1u << 7;
    }
    unlearnCache.clear();
    if (!name.empty())
        for (uint32_t s : {8u, 16u, 19u})
        {
            learnUnitRules &= ~(1ull << s);
            learnBuildRules &= ~(1ull << s);
        }
    learnCache.clear();
    learnCacheOn = true;
    ready = 1;
}

void Build::ForgetResult(uint32_t id)
{
    if (!learnCacheOn)
        return;
    for (size_t i = 0; i < learnCache.size(); ++i)
        if (learnCache[i].first == id)
        {
            learnCache.erase(learnCache.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

namespace
{
    void Remember(Build& b, uint32_t id, uint32_t result)   // FUN_10158a60(id, 1, result)
    {
        if (!b.learnCacheOn)
            return;
        if (!result)
        {
            b.ForgetResult(id);
            return;
        }
        for (auto& kv : b.learnCache)
            if (kv.first == id)
            {
                kv.second = {1, result};
                return;
            }
        b.learnCache.push_back({id, {1, result}});
    }
}

uint32_t Build::ValidateLearn(void* unitIn, uint32_t id, const std::vector<uint32_t>& include,
                              const std::vector<uint32_t>& exclude)
{
    if (!ready || (!learnUnitRules && !learnBuildRules))
        return 0;
    Row r = FindRow(id);
    const uint8_t* unit = static_cast<const uint8_t*>(unitIn);
    auto ownerUnit = [&]() {
        if (!unit && guid)
            unit = static_cast<const uint8_t*>(ObjectPtr(guid, 0x10));
        return unit;
    };
    auto fails = [&](uint32_t slot) {
        if ((learnBuildRules >> slot) & 1 && !BuildRuleOk(slot, *this, r))
            return true;
        return ownerUnit() && ((learnUnitRules >> slot) & 1) && !UnitRuleOk(slot, unit, r);
    };

    // A remembered failure is returned while it still fails.
    if (r && learnCacheOn)
        for (auto& kv : learnCache)
            if (kv.first == id && kv.second.first == 1)
            {
                const uint32_t cached = kv.second.second;
                if (cached && cached != 0x2C && (include.empty() || Contains(include, cached))
                    && (exclude.empty() || !Contains(exclude, cached)) && fails(cached))
                    return cached;
                break;
            }

    for (uint32_t slot = 0; slot < 45; ++slot)
    {
        if (!include.empty() && !Contains(include, slot))
            continue;
        if (!exclude.empty() && Contains(exclude, slot))
            continue;
        if (!r && slot != 3)
            continue;   // every rule but BAD_ABILITY reads the row
        if (fails(slot))
        {
            Remember(*this, id, slot);
            return slot;
        }
    }
    Remember(*this, id, 0);
    return 0;
}

void Build::AddEntry(uint32_t id, uint32_t rank)   // FUN_10151190 (+ FUN_1014ff10)
{
    if (Has(id))
        return;
    Entry e;
    e.id = id;
    e.rank = rank;
    e.row = FindRow(id);
    // lambda_1 (FUN_10183190): a choice-group entry evicts the build's other entries of its group.
    if (pendingHooks && e.row)
        if (const uint32_t group = RowU32(e.row, 0x74))
        {
            std::vector<uint32_t> members;   // FUN_10152960(group, 0)
            for (const Entry& x : entries)
                if (RowU32(x.row, 0x74) == group && x.id != 0)
                    members.push_back(x.id);
            for (uint32_t m : members)
                Remove(m);
        }
    entries.push_back(e);
    if (pendingHooks)
        RequestAutoLearn();   // lambda_2
    ForgetResult(id);
}

void Build::SetRank(uint32_t id, uint32_t rank)   // FUN_10158940
{
    for (Entry& e : entries)
        if (e.id == id)
        {
            if (e.rank != rank)
            {
                e.rank = rank;
                if (pendingHooks)
                    RequestAutoLearn();   // lambda_3
                ForgetResult(id);
            }
            return;
        }
}

void Build::Remove(uint32_t id)   // FUN_10157c30
{
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].id == id)
        {
            const Entry removed = entries[i];
            entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(i));
            // lambda_4: removing anything but a 0x100000 (auto-learned) row re-runs the auto-learn pass.
            if (pendingHooks && !(removed.row && RowHasFlag(removed.row, 0x100000)))
                RequestAutoLearn();
            ForgetResult(id);
            return;
        }
}

void Build::AddRank(uint32_t id)   // FUN_10151390
{
    if (Has(id))
        SetRank(id, RankOf(id) + 1);
    else
        AddEntry(id, 1);
}

namespace
{
    bool HasRequirements(Row r)
    {
        for (uint32_t i = 0; i < 3; ++i)
            if (RowU32(r, 8 + i * 4))
                return true;
        return false;
    }

    // FUN_1014d470's order: masteries first, then the mode's required level, then id.
    bool NormalOrder(const Build& b, const Entry& x, const Entry& y)
    {
        const bool mx = RowHasFlag(x.row, 0x1000), my = RowHasFlag(y.row, 0x1000);
        if (mx != my)
            return mx;
        const uint32_t lx = RequiredLevel(b, x.row), ly = RequiredLevel(b, y.row);
        if (lx != ly)
            return lx < ly;
        return x.id < y.id;
    }

    // FUN_10150950 (CoA builds): no required ids first, then free, then AE-costing, then PositionY
    // (larger first), PositionX, id.
    bool CoAOrder(const Build& b, const Entry& x, const Entry& y)
    {
        const bool rx = HasRequirements(x.row), ry = HasRequirements(y.row);
        if (rx != ry)
            return !rx;
        const bool fx = b.AECost(x.row, 1) == 0 && b.TECost(x.row, 1) == 0;
        const bool fy = b.AECost(y.row, 1) == 0 && b.TECost(y.row, 1) == 0;
        if (fx != fy)
            return fx;
        const bool ax = b.AECost(x.row, 1) != 0, ay = b.AECost(y.row, 1) != 0;
        if (ax != ay)
            return ax;
        const float yx = *reinterpret_cast<const float*>(x.row + 0xD4), yy = *reinterpret_cast<const float*>(y.row + 0xD4);
        if (yx != yy)
            return yy < yx;
        const float xx = *reinterpret_cast<const float*>(x.row + 0xD0), xy = *reinterpret_cast<const float*>(y.row + 0xD0);
        if (xx != xy)
            return xx < xy;
        return x.id < y.id;
    }
}

bool Build::Optimize(bool restoreOnFail, uint32_t& outResult, uint32_t& outId, uint32_t& outRank)
{
    outResult = outId = outRank = 0;
    std::vector<Entry> steps;
    for (const Entry& e : entries)
        for (uint32_t k = 0; k < e.rank; ++k)
        {
            Entry s = e;
            s.rank = 1;
            steps.push_back(s);
        }
    if (coa)
        std::sort(steps.begin(), steps.end(), [this](const Entry& x, const Entry& y) { return CoAOrder(*this, x, y); });
    else
        std::sort(steps.begin(), steps.end(), [this](const Entry& x, const Entry& y) { return NormalOrder(*this, x, y); });

    Build temp(*this, true);
    temp.entries.clear();
    const std::vector<uint32_t> none, notWildcard = {0x2C};
    std::vector<Entry> placed;
    while (!steps.empty())
    {
        bool progress = false;
        for (size_t i = 0; i < steps.size();)
        {
            if (temp.ValidateLearn(nullptr, steps[i].id, none, notWildcard) == 0)
            {
                placed.push_back(steps[i]);
                temp.AddRank(steps[i].id);
                steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(i));
                progress = true;
            }
            else
                ++i;
        }
        if (!progress)
        {
            // FUN_101560d0 reports the first step left; the entries are unchanged either way.
            outId = steps.front().id;
            outRank = steps.front().rank;
            outResult = temp.ValidateLearn(nullptr, outId, none, notWildcard);
            (void)restoreOnFail;
            return false;
        }
    }
    std::vector<Entry> merged;
    for (const Entry& p : placed)
    {
        auto it = std::find_if(merged.begin(), merged.end(), [&](const Entry& m) { return m.id == p.id; });
        if (it == merged.end())
            merged.push_back(p);
        else
            ++it->rank;
    }
    entries = merged;
    return true;
}


bool Build::TraverseOrFix(const std::vector<uint32_t>& keep)
{
    const std::vector<uint32_t> none, notWildcard = {0x2C};
    auto learnable = [&](const Build& b) {
        uint32_t id, rank, result;
        if (!b.ValidateAll(true, none, notWildcard, false, id, rank, result))
            return false;
        for (uint32_t k : keep)
            if (!b.Has(k))
                return false;
        return true;
    };
    if (learnable(*this))
        return true;
    const std::vector<Entry> saved = entries;   // FUN_1014e460
    if (Reorder(false))
    {
        if (learnable(*this))
            return true;
        entries = saved;   // FUN_1014b040
    }
    // One unit per rank, split into kept and removable.
    std::vector<uint32_t> kept, removable;
    for (const Entry& e : saved)
        for (uint32_t k = 0; k < e.rank; ++k)
            (std::find(keep.begin(), keep.end(), e.id) != keep.end() ? kept : removable).push_back(e.id);
    for (uint32_t k : keep)
        if (!Has(k))
            return false;
    const size_t n = removable.size();
    if (!n)
        return false;

    // FUN_10150af0: rebuild from the kept units plus the removable ones the mask leaves in, reorder and
    // validate; on success the build takes the result.
    auto attempt = [&](const std::vector<uint8_t>& mask) {
        std::vector<std::pair<uint32_t, uint32_t>> counts;   // id -> units, in first-seen order
        auto count = [&](uint32_t id) {
            for (auto& c : counts)
                if (c.first == id)
                {
                    ++c.second;
                    return;
                }
            counts.push_back({id, 1});
        };
        for (uint32_t id : kept)
            count(id);
        for (size_t i = 0; i < n; ++i)
            if (mask[i])
                count(removable[i]);
        Build trial(*this, true);
        trial.entries.clear();   // FUN_10157d30(1)
        for (const auto& c : counts)
        {
            Entry e;
            e.id = c.first;
            e.rank = c.second;
            trial.entries.push_back(e);
        }
        trial.UpdatePointers(false);
        trial.Reorder(false);
        if (!learnable(trial))
            return false;
        entries = trial.entries;
        return true;
    };

    std::vector<uint8_t> mask(n, 1);
    if (attempt(mask))
        return true;
    for (size_t size = 1; size <= n; ++size)
    {
        std::vector<size_t> pick(size);
        for (size_t i = 0; i < size; ++i)
            pick[i] = i;
        for (;;)
        {
            std::fill(mask.begin(), mask.end(), 1);
            for (size_t p : pick)
                mask[p] = 0;
            if (attempt(mask))
                return true;
            size_t i = size;
            while (i > 0 && pick[i - 1] == i - 1 + n - size)
                --i;
            if (i == 0)
                break;
            ++pick[i - 1];
            for (size_t j = i; j < size; ++j)
                pick[j] = pick[j - 1] + 1;
        }
    }
    entries = saved;
    return false;
}

bool Build::Reorder(bool restoreOnFail)   // FUN_101557d0: the same pass without the report
{
    uint32_t r, i, k;
    return Optimize(restoreOnFail, r, i, k);
}

// ---- ValidateAll (FUN_10158050) ------------------------------------------------------------------------
bool Build::ValidateAll(bool inOrder, const std::vector<uint32_t>& include, std::vector<uint32_t> exclude,
                        bool maskMode, uint32_t& outId, uint32_t& outRank, uint32_t& outResult) const
{
    outId = outRank = outResult = 0;
    if (entries.empty())
        return true;
    Build temp(*this, true);
    temp.entries.clear();
    if (maskMode)
    {
        for (uint32_t s = 0; s < 45; ++s)
            if (!((temp.learnMask >> s) & 1) && (((temp.learnUnitRules | temp.learnBuildRules) >> s) & 1))
                exclude.push_back(s);
        if (temp.draft)
            exclude.push_back(5);
    }
    if (inOrder)
    {
        for (const Entry& e : entries)
            for (uint32_t k = 0; k < e.rank; ++k)
            {
                const uint32_t result = temp.ValidateLearn(nullptr, e.id, include, exclude);
                if (result)
                {
                    outId = e.id;
                    outRank = e.rank;
                    outResult = result;
                    return false;
                }
                temp.AddRank(e.id);
            }
        return true;
    }
    // The Reorder pass with these sets; the first step left over is reported.
    std::vector<Entry> steps;
    for (const Entry& e : entries)
        for (uint32_t k = 0; k < e.rank; ++k)
        {
            Entry s = e;
            s.rank = 1;
            steps.push_back(s);
        }
    if (coa)
        std::sort(steps.begin(), steps.end(), [this](const Entry& x, const Entry& y) { return CoAOrder(*this, x, y); });
    else
        std::sort(steps.begin(), steps.end(), [this](const Entry& x, const Entry& y) { return NormalOrder(*this, x, y); });
    while (!steps.empty())
    {
        bool progress = false;
        for (size_t i = 0; i < steps.size();)
            if (temp.ValidateLearn(nullptr, steps[i].id, include, exclude) == 0)
            {
                temp.AddRank(steps[i].id);
                steps.erase(steps.begin() + static_cast<std::ptrdiff_t>(i));
                progress = true;
            }
            else
                ++i;
        if (!progress)
        {
            outId = steps.front().id;
            outRank = steps.front().rank;
            outResult = temp.ValidateLearn(nullptr, outId, include, exclude);
            return false;
        }
    }
    return true;
}

// ---- unlearn rules (FUN_10151a50) ---------------------------------------------------------------------
namespace
{
    // Items and money on the active player (FUN_10308700 / FUN_10308570 / FUN_103087a0): stacks of
    // `item` among the descriptor GUID slots +0x510 + i*8, i in [0x76, 0x96); coinage at +0x1248.
    uint32_t ItemCount(const uint8_t* unit, uint32_t item)
    {
        if (!unit || UnitGuid(unit) != ActivePlayerGuid())
            return 0;
        const uint8_t* desc = Descriptor(unit);
        uint32_t total = 0;
        for (uint32_t i = 0x76; i < 0x96; ++i)
            if (const uint8_t* obj = static_cast<const uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(desc + 0x510 + i * 8), 2)))
            {
                const uint8_t* itemDesc = *reinterpret_cast<uint8_t* const*>(obj + 8);
                if (*reinterpret_cast<const uint32_t*>(itemDesc + 0xC) == item)
                    total += *reinterpret_cast<const uint32_t*>(itemDesc + 0x38);
            }
        return total;
    }

    bool HasItems(const uint8_t* unit, uint32_t item, uint32_t count)   // FUN_10308700
    {
        if (!unit || UnitGuid(unit) != ActivePlayerGuid())
            return count == 0;
        return count <= ItemCount(unit, item);
    }

    bool HasMoney(const uint8_t* unit, uint32_t money)   // FUN_103087a0
    {
        return unit && UnitGuid(unit) == ActivePlayerGuid()
            && money <= *reinterpret_cast<const uint32_t*>(Descriptor(unit) + 0x1248);
    }

    const uint32_t kMarkOfAscension = 0x5B9D2;

    // FUN_10154650: what unlearning `r` costs this build.
    TokenCost UnlearnCost(const Build& b, Row r)
    {
        TokenCost c{0, 0, 0, 0};
        if (b.level <= 10 || RowHasFlag(r, 0x2000))
            return c;
        const double level = static_cast<double>(b.level);
        const uint32_t tier = level <= 19.0 ? 32 : (level >= 20.0 && level <= 29.0) ? 71 : (level >= 30.0 && level <= 49.0) ? 521
                            : (level >= 50.0 && level <= 59.0) ? 1107 : level >= 60.0 ? 2221 : 0;
        double base = static_cast<double>(tier) * level, extra = 0.0;
        if (b.coa)
        {
            base *= 0.25;
            extra = static_cast<double>(static_cast<int32_t>(b.credit[1] + b.credit[2])) * 4.16666666666667 * level * 0.25;
        }
        c.money = static_cast<uint32_t>(ToInt(std::min(std::max(2147483647.0 - base, 0.0), extra) + base));
        c.marks = b.coa ? b.level * 4 : b.stock ? b.level * 5 : 250;
        return c;
    }

    bool CanPay(const uint8_t* unit, const TokenCost& c)   // FUN_10151400
    {
        if (!c.count && !c.marks && !c.money)
            return true;
        if (c.item && HasItems(unit, c.item, c.count))
            return true;
        if (c.marks && HasItems(unit, kMarkOfAscension, c.marks))
            return true;
        return c.money && HasMoney(unit, c.money);
    }

    bool UnlearnBuildRuleOk(uint32_t slot, Build& b, Row r)
    {
        const uint32_t id = RowU32(r, 0), flags = RowU32(r, 0x124);
        switch (slot)
        {
        case 3:                                                              // FUN_10164940 BAD_ABILITY
            for (uint32_t i = 0; i < 9; ++i)
                if (const uint32_t spell = RowU32(r, 0x14 + i * 4))
                {
                    uint8_t rec[0x2A8];
                    if (!FetchSpell(spell, rec))
                        return false;
                }
            return true;
        case 5:  return !(flags & 4);                                        // FUN_10165450 NOT_THIS_ABILITY
        case 7:  return b.Has(id);                                           // FUN_10165430 NOT_KNOWN
        case 9:                                                              // FUN_10164b10 MISSING_CONNECTED_ENTRIES
        {
            Build copy(b, true);
            copy.Remove(id);
            uint32_t x, y, z;
            return copy.ValidateAll(true, {0xD}, {}, false, x, y, z);
        }
        case 10:                                                             // FUN_10164ac0 MASTERY_ID
            for (const Entry& e : b.entries)
                for (uint32_t i = 0; i < 3; ++i)
                    if (RowU32(e.row, 0x1B0 + i * 4) == id)
                        return false;
            return true;
        case 11:                                                             // FUN_10164c90 MISSING_REQUIRED_ID
            for (const Entry& e : b.entries)
                if (e.id != id)
                    for (uint32_t i = 0; i < 3; ++i)
                        if (RowU32(e.row, 8 + i * 4) == id)
                            return false;
            return true;
        case 13: return !b.LockedOf(id);                                     // FUN_10164aa0 LOCKED
        case 14:                                                             // FUN_10165670 WILDCARD_UNSPENT_AE
        case 15:                                                             // FUN_101657d0 WILDCARD_UNSPENT_TE
        {
            if ((b.u44 & 0xFF) && b.GlobalTE(0) == 0)
                return true;
            const bool ae = slot == 14;
            if (!(ae ? IsAbilityLike(r) : IsTalent(r)))
                return true;
            // FUN_10155120 / FUN_10155160: one more roll still fits the budget.
            const uint32_t spent = ae ? b.GlobalAE(0) + ConfigInt("CONFIG_WILDCARD_ABILITY_ROLL_COST", 2)
                                      : b.GlobalTE(0) + ConfigInt("CONFIG_WILDCARD_TALENT_ROLL_COST", 1);
            return !(spent <= (ae ? b.AEBudget(b.level) : b.TEBudget(b.level)));
        }
        case 16:                                                             // FUN_101649b0 BUILD_CREATOR
            if ((b.specDraft || b.u64 || b.u65) && (b.u65 || RowU32(r, 0x74) != 1))
                return false;   // no C_BuildCreator template exists yet (see the header)
            return true;
        case 17: case 18: case 19:
            // FUN_10164e80 / 10165180 / 10165000 revalidate a FRESH default build (FUN_1014f820), which is
            // empty, so FUN_10158050 succeeds at once -- the original's slots never fail.
            return true;
        case 21: return false;                                               // FUN_100cc240 NOT_WILDCARD
        default: return true;
        }
    }

    // FUN_101656c0: the unspent-essence slots only apply without this config.
    bool UnlearnApplies(uint32_t slot)
    {
        if (slot != 14 && slot != 15)
            return true;
        const bool* v = AscConfig::Bool("CONFIG_WILDCARD_SCROLL_OF_FORTUNE_ALLOW_UNLEARN_WITH_UNSPENT_ESSENCE");
        return !(v && *v);
    }

    bool UnlearnUnitRuleOk(uint32_t slot, const uint8_t* unit, Row r)
    {
        const uint32_t flags = RowU32(r, 0x124);
        switch (slot)
        {
        case 4:                                                              // FUN_10165300 NOT_IN_BATTLEGROUNDS
        {
            Build* b = BuildOf(UnitGuid(unit));
            if (!b)
                return false;
            if (IsTalent(r) || (CoAClass(UnitClassOf(unit)) && !(flags & 0x40000)))
                return true;
            const uint8_t* map = CurrentMap();
            if (!map)
                return true;
            if (MapType(map) == 4)
                return false;
            if (MapType(map) == 3)
            {
                if (RowU32(r, 0x74) == 1 && b->HasGroup(1, 0))
                    return false;
                return reinterpret_cast<int(__cdecl*)(uint32_t)>(0x548D10)(0x1097) != 1;
            }
            return true;
        }
        case 6: return !InCombat(unit);                                      // FUN_10165410 NOT_IN_COMBAT
        case 8:                                                              // FUN_10164de0 NO_UNLEARN_ITEM
        {
            if (flags & 0x2000 || *reinterpret_cast<const uint32_t*>(Descriptor(unit) + 0xD8) <= 9)
                return true;
            Build* b = BuildOf(UnitGuid(unit));
            return b && CanPay(unit, UnlearnCost(*b, r));
        }
        default:
            // 12 NO_SCROLL_OF_FORTUNE / 20 SCROLL_OF_FORTUNE_LIMIT (wildcard only) count Scroll of Fortune
            // items through a manager not transcribed yet; slot 21 fails every wildcard unlearn regardless.
            return true;
        }
    }

    void RememberUnlearn(Build& b, uint32_t id, uint32_t result)   // FUN_10158f50(id, 1, result)
    {
        if (!b.learnCacheOn)
            return;
        for (size_t i = 0; i < b.unlearnCache.size(); ++i)
            if (b.unlearnCache[i].first == id)
            {
                if (result)
                    b.unlearnCache[i].second = result;
                else
                    b.unlearnCache.erase(b.unlearnCache.begin() + static_cast<std::ptrdiff_t>(i));
                return;
            }
        if (result)
            b.unlearnCache.push_back({id, result});
    }
}

TokenCost UnlearnCostOf(const Build& b, Row r) { return UnlearnCost(b, r); }
int QualityIndexOf(const Build& b, Row r) { return QualityOf(b, r); }
uint32_t QualitySumOf(const Build& b, int quality) { return QualitySum(b, quality); }
uint32_t QualityCostOf(const Build& b, Row r) { return QualityCost(b, r); }
uint32_t RequiredLevelOf(const Build& b, Row r) { return RequiredLevel(b, r); }
uint32_t UnitItemCount(const void* unit, uint32_t item) { return ItemCount(static_cast<const uint8_t*>(unit), item); }

// ---- purge (unlearn all) ------------------------------------------------------------------------------
// FUN_10152650 (abilities, E = +0x34) / FUN_101543b0 (talents, E = +0x38). Free below level 11 except for
// the token entry; the gold cost is a cubic in the level, capped at 0xFFFFFFFF and truncated through
// __dtol3 (so a negative value wraps, as in the original).
TokenCost PurgeCost(const Build& b, bool talents)
{
    TokenCost c{0, 0, 0, 0};
    const uint32_t e = talents ? b.credit[1] : b.credit[0];
    const bool paid = b.level > 10;
    if (paid)
    {
        const double lvl = static_cast<double>(b.level);
        double x;
        if (b.coa)
        {
            x = lvl * lvl * 0x1.29867c3eca801p+0 + static_cast<double>(e) * 0x1.555555546ac55p-5 * 10000.0;
            x = (x + lvl * 0x1.2b42e6bdc5bb1p+4 - 359.025) * lvl * 0.25;
        }
        else
            x = lvl * lvl * 139.465 + static_cast<double>(e * 50000u) + lvl * 2244.46 - 43083.0;
        c.money = static_cast<uint32_t>(static_cast<int64_t>(4294967295.0 < x ? 4294967295.0 : x));
    }
    if (b.coa)
    {
        c.marks = paid ? b.level * (talents ? 0x7D : 0xA7) : 0;
        c.item = talents ? 0xE06FB : 0xE06FA;
        c.count = paid ? 1 : 0;
        return c;
    }
    if (paid)
        c.marks = b.stock ? b.level * 25 : (talents ? 7500 : 10000);
    const bool* cfg = AscConfig::Bool("CONFIG_CHARACTER_ADVANCEMENT_HERO_ABILITY_TALENT_PURGE_ENABLED");
    if (!talents)
    {
        c.item = 0x5D86A;   // the abilities variant reads the config and ignores it
        c.count = paid ? 1 : 0;
    }
    else if ((cfg && *cfg) || b.stock)
    {
        c.item = 0x5D86B;
        c.count = paid ? 1 : 0;
    }
    return c;
}

bool Build::SpentAtLeast(bool talents, uint32_t amount, uint32_t below) const
{
    if (!amount)
        return true;
    Build walk(*this, false);   // FUN_10157d30(1) on a hookless copy: emptied, then re-added entry by entry
    walk.entries.clear();
    uint32_t total = 0;
    for (const Entry& e : entries)
    {
        if (below && RowU32(e.row, talents ? 0x8C : 0x88) >= below)
            continue;
        total += talents ? walk.TECost(e.row, e.rank) : walk.AECost(e.row, e.rank);
        if (!walk.Has(e.id))   // FUN_101510d0
            walk.entries.push_back(e);
        if (total >= amount)
            return true;
    }
    return false;
}

namespace
{
    bool PurgeUnitRuleOk(uint32_t slot, bool talents, const uint8_t* unit)
    {
        switch (slot)
        {
        case 3:                                                          // FUN_101647f0 / FUN_101648f0
        {
            const uint8_t* map = CurrentMap();
            if (!map)
                return true;
            if (MapType(map) == 4)
                return false;
            if (MapType(map) == 3)
                return reinterpret_cast<int(__cdecl*)(uint32_t)>(0x548D10)(0x1097) != 1;
            return true;
        }
        case 4: return !InCombat(unit);                                  // FUN_10164840
        case 5:                                                          // FUN_10164780 / FUN_10164880
        {
            Build* b = BuildOf(UnitGuid(unit));
            return b && CanPay(unit, PurgeCost(*b, talents));
        }
        default: return true;
        }
    }

    bool PurgeBuildRuleOk(uint32_t slot, bool talents, const Build& b)
    {
        switch (slot)
        {
        case 6: return b.SpentAtLeast(talents, 1, 0);                   // FUN_10164770 / FUN_10164870
        case 7:                                                          // FUN_10164630
        {
            const bool* cfg = AscConfig::Bool("CONFIG_CHARACTER_ADVANCEMENT_HERO_ABILITY_TALENT_PURGE_ENABLED");
            return (cfg && *cfg) || b.classKey != 10;
        }
        default: return true;
        }
    }
}

uint32_t Build::ValidatePurge(bool talents, void* unitIn) const
{
    const int t = talents ? 1 : 0;
    if (!ready || !purgeUnitRules[t] || !purgeBuildRules[t])
        return 0;
    const uint8_t* unit = static_cast<const uint8_t*>(unitIn);
    if (!unit && guid)
        unit = static_cast<const uint8_t*>(ObjectPtr(guid, 0x10));
    for (uint32_t slot = 0; slot < 8; ++slot)   // each rule's gate (FUN_1015a580) always admits
    {
        if (((purgeBuildRules[t] >> slot) & 1) && !PurgeBuildRuleOk(slot, talents, *this))
            return slot;
        if (unit && ((purgeUnitRules[t] >> slot) & 1) && !PurgeUnitRuleOk(slot, talents, unit))
            return slot;
    }
    return 0;
}

uint32_t Build::ValidateUnlearn(void* unitIn, uint32_t id, const std::vector<uint32_t>& include,
                                const std::vector<uint32_t>& exclude)
{
    if (!ready || (!unlearnUnitRules && !unlearnBuildRules))
        return 0;
    Row r = FindRow(id);
    const uint8_t* unit = static_cast<const uint8_t*>(unitIn);
    auto ownerUnit = [&]() {
        if (!unit && guid)
            unit = static_cast<const uint8_t*>(ObjectPtr(guid, 0x10));
        return unit;
    };
    auto fails = [&](uint32_t slot) {
        if (!UnlearnApplies(slot))
            return false;
        if (((unlearnBuildRules >> slot) & 1) && !UnlearnBuildRuleOk(slot, *this, r))
            return true;
        return ownerUnit() && ((unlearnUnitRules >> slot) & 1) && !UnlearnUnitRuleOk(slot, unit, r);
    };
    if (r && learnCacheOn)
        for (auto& kv : unlearnCache)
            if (kv.first == id)
            {
                const uint32_t cached = kv.second;
                if ((include.empty() || Contains(include, cached)) && (exclude.empty() || !Contains(exclude, cached))
                    && fails(cached))
                    return cached;
                break;
            }
    for (uint32_t slot = 0; slot < 22; ++slot)
    {
        if (!include.empty() && !Contains(include, slot))
            continue;
        if (!exclude.empty() && Contains(exclude, slot))
            continue;
        if (!r && slot != 3)
            continue;
        if (fails(slot))
        {
            RememberUnlearn(*this, id, slot);
            return slot;
        }
    }
    RememberUnlearn(*this, id, 0);
    return 0;
}

// ---- mastery removal (FUN_101731e0) ------------------------------------------------------------------
bool Build::MasteryInvolved(uint32_t id) const   // FUN_10173c80: known, and a mastery or carrying a known one
{
    Row r = FindRow(id);
    if (!Has(id) || !r)
        return false;
    if (RowHasFlag(r, 0x1000))
        return true;
    for (uint32_t i = 0; i < 3; ++i)
        if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
            if (Has(m))
                return true;
    return false;
}

uint32_t Build::MasteryTarget(uint32_t id) const   // FUN_1016f4f0
{
    Row r = FindRow(id);
    if (!r)
        return 0;
    if (RowHasFlag(r, 0x1000) && Has(id))
        return id;
    for (uint32_t i = 0; i < 3; ++i)
        if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
            if (Has(m))
                return m;
    return 0;
}

bool Build::ClassFusion() const   // FUN_10172490: class fusion on a Hero build in a plain mode
{
    const bool* fusion = AscConfig::Bool("CONFIG_CLASS_FUSION_ENABLED");
    return fusion && *fusion && hero && !coa && !stock && (AscGameMode::Mode() & 0x1DEF) == 0;
}

std::vector<Entry> Build::TakeMasteryBearers()   // FUN_101738f0
{
    std::vector<Entry> saved;
    for (const Entry& e : entries)
        for (uint32_t i = 0; i < 3; ++i)
            if (RowU32(e.row, 0x1B0 + i * 4))
            {
                saved.push_back(e);
                break;
            }
    for (const Entry& e : saved)
        Remove(e.id);
    return saved;
}

void Build::ReaddWhileValid(const std::vector<Entry>& saved)   // FUN_10173e20
{
    for (bool progress = true; progress;)
    {
        progress = false;
        for (const Entry& e : saved)
            if (RankOf(e.id) < e.rank && ValidateLearn(nullptr, e.id, {}, {}) == 0)
            {
                AddRank(e.id);
                progress = true;
            }
    }
}

void Build::RemoveWithMastery(uint32_t id)   // FUN_101731e0
{
    if (!MasteryInvolved(id))
    {
        Remove(id);
        return;
    }
    if (ClassFusion())
    {
        const uint32_t mastery = MasteryTarget(id);
        const std::vector<Entry> saved = TakeMasteryBearers();
        Remove(mastery);
        ReaddWhileValid(saved);
        return;
    }
    // FUN_101488f0 with rank 0: the mastery and every entry that lists it.
    Row r = FindRow(id);
    uint32_t mastery = RowHasFlag(r, 0x1000) ? id : 0;
    for (uint32_t i = 0; i < 3 && !mastery; ++i)
        if (const uint32_t m = RowU32(r, 0x1B0 + i * 4))
            if (Has(m))
                mastery = m;
    if (!mastery)
    {
        Remove(id);
        return;
    }
    std::vector<uint32_t> doomed = {mastery};
    for (const Entry& e : entries)
        for (uint32_t i = 0; i < 3; ++i)
            if (RowU32(e.row, 0x1B0 + i * 4) == mastery)
            {
                doomed.push_back(e.id);
                break;
            }
    for (uint32_t d : doomed)
        Remove(d);
}

// ---- apply validation ---------------------------------------------------------------------------------
namespace
{
    struct DiffItem { uint32_t id; int32_t delta; bool added, removed, modified; };

    std::vector<DiffItem> Diff(const Build& now, const Build& base)   // FUN_101523d0 (this = now)
    {
        std::vector<DiffItem> out;
        for (const Entry& e : now.entries)
            if (!base.Has(e.id))
                out.push_back({e.id, static_cast<int32_t>(e.rank), true, false, false});
        for (const Entry& e : base.entries)
            if (!now.Has(e.id))
                out.push_back({e.id, -static_cast<int32_t>(e.rank), false, true, false});
        for (const Entry& e : now.entries)
        {
            bool listed = false;
            for (const DiffItem& d : out)
                listed = listed || d.id == e.id;
            if (!listed && e.rank != base.RankOf(e.id))
                out.push_back({e.id, static_cast<int32_t>(e.rank) - static_cast<int32_t>(base.RankOf(e.id)), false, false, true});
        }
        return out;
    }

    // FUN_1014bb50: the same {id, rank} multiset.
    bool SameEntries(const std::vector<Entry>& a, const std::vector<Entry>& b)
    {
        if (a.size() != b.size())
            return false;
        std::vector<std::pair<uint32_t, uint32_t>> x, y;
        for (const Entry& e : a)
            x.push_back({e.id, e.rank});
        for (const Entry& e : b)
            y.push_back({e.id, e.rank});
        std::sort(x.begin(), x.end());
        std::sort(y.begin(), y.end());
        return x == y;
    }

    // FUN_101547e0: every removal costs its unlearn price, in Marks of Ascension while they last, else money.
    bool PayForRemovals(const uint8_t* unit, const Build& base, const std::vector<DiffItem>& diff, ApplyCosts& costs)
    {
        costs = ApplyCosts();
        Build temp(base, false);
        std::vector<TokenCost> prices;
        for (const DiffItem& d : diff)
            if (d.removed)
                if (Row r = FindRow(d.id))
                {
                    prices.push_back(UnlearnCost(temp, r));
                    temp.Remove(d.id);
                }
        uint32_t marksLeft = ItemCount(unit, kMarkOfAscension);
        for (const TokenCost& p : prices)
        {
            if (!(p.item && p.count) && !p.marks && !p.money)
                continue;
            if (!p.marks || marksLeft < p.marks)
            {
                if (!p.money || !HasMoney(unit, costs.money + p.money))
                    return false;
                costs.money += p.money;
            }
            else
            {
                marksLeft -= p.marks;
                costs.marks += p.marks;
            }
        }
        return true;
    }
}

uint32_t ValidateApply(void* unitIn, const Build& base, const std::vector<Entry>& entries, uint32_t& learnResult,
                       uint32_t& id, uint32_t& rank, ApplyCosts& costs)
{
    learnResult = id = rank = 0;
    costs = ApplyCosts();
    for (const Entry& e : entries)   // FUN_10151df0
        if (!FindRow(e.id))
        {
            id = e.id;
            rank = e.rank;
            return 3;
        }
    Build now(base, false);
    now.entries = entries;
    now.UpdatePointers(false);

    const uint8_t* unit = static_cast<const uint8_t*>(unitIn);
    const std::vector<DiffItem> diff = Diff(now, base);   // FUN_10151fd0
    if (diff.empty())
        return 4;
    for (const DiffItem& d : diff)
    {
        Row r = FindRow(d.id);
        if (r && RowU32(r, 0x74) != 1 && (now.wildcard || (now.draft && !IsTalent(r))))
            return 8;
    }
    std::vector<Entry> kept;
    for (const Entry& e : base.entries)
        if (RowU32(e.row, 0x74) == 1 || RowHasFlag(e.row, 2))
            kept.push_back(e);
    bool allRemovals = true;
    for (const DiffItem& d : diff)
        allRemovals = allRemovals && d.removed;
    if (!(allRemovals && SameEntries(now.entries, kept)))
        if (!now.ValidateAll(true, {}, {0x2C}, true, id, rank, learnResult))
            return 5;
    if (!PayForRemovals(unit, base, diff, costs))
        return 6;
    if (costs.marks && !HasItems(unit, kMarkOfAscension, costs.marks))
        return 7;
    if (costs.money && !HasMoney(unit, costs.money))
        return 7;
    for (const TokenCost& t : costs.tokens)
        if (!HasItems(unit, t.item, t.count))
            return 7;
    return 0;
}
}
