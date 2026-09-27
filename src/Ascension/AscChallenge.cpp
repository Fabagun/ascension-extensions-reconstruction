// C_Challenge -- the original's ChallengeMgr (accessor FUN_1013b8b0, ctor FUN_10132340, module init
// FUN_1013b5b0: 18 events, 30 packet handlers, the tick 0x1013BE30 after client 0x403340, enter-world
// 0x1013BF00, glue reset FUN_10135010).
//
// Static data (Challenge*.dbc, hot-patched by SMSG_PATCH_CHALLENGE_*) is indexed by
// ChallengeMgr::UpdateChallengeDataStores (FUN_10143630) into
//   challenge id -> CachedChallengeInfo {+0 Challenge row, +4 ChallengeGroups rows, +0x10 level map,
//                   +0x30 min level, +0x34 max level}                              (manager +0x158)
//   group id     -> challenges in it                                              (manager +0x178)
//   level        -> CachedChallengeLevelInfo {+0 ChallengeLevels row, +4 conditions, +0x10
//                   requirements, +0x1C first-completion rewards, +0x28 repeat rewards, +0x34 group
//                   rewards, +0x40 rules, +0x4C spells, +0x58 computed rewards (0x12 each), +0x64
//                   modifier rows, +0x70 summed modifiers (0x10 each)}
// A row keyed (challenge, group 0, level) belongs to that challenge; (x, group, level) to every
// challenge of the group (lambdas 2..5 of UpdateChallengeDataStores).
//
// Server state: active challenges (+0x28, SMSG 0x596 / 0x5A6 / 0x593 / 0x595), completed (+0x48
// multimap, 0x597 / 0x598), criteria (+0x68, 0x599 / 0x59A), pending sync (+0x88, 0x59B), failures
// (+0x94, 0xEF each, 0x5A2 / 0x5A3), completions (+0xA0, 0x5C7 / 0x5C8), broken rules (+0xC0,
// 0x5BA / 0x5BB), the ChallengeEntryContainer (+0xE0, SetChallengeFilter).
//
// std::unordered_map / std::sort are used where the original uses them: MSVC's FNV hash and bucket
// policy and its introsort give the same iteration and tie orders.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscChallenge.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscDbcPatch.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscTrialCreator.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AscScript;

namespace
{
#include <Ascension/AscCAEnums.generated.inc>

    typedef const uint8_t* Row;
    uint32_t U32(Row r, uint32_t off) { return AscDbc::Table::U32(r, off); }
    float F32(Row r, uint32_t off) { return AscDbc::Table::F32(r, off); }

    AscDbc::Table& T(const char* name) { return AscDbc::Get(name); }
    AscDbc::Table& Challenges()       { return T("DBFilesClient\\Challenge.dbc"); }
    AscDbc::Table& Groups()           { return T("DBFilesClient\\ChallengeGroups.dbc"); }
    AscDbc::Table& Levels()           { return T("DBFilesClient\\ChallengeLevels.dbc"); }
    AscDbc::Table& Conditions()       { return T("DBFilesClient\\ChallengeConditions.dbc"); }
    AscDbc::Table& Requirements()     { return T("DBFilesClient\\ChallengeRequirements.dbc"); }
    AscDbc::Table& Rules()            { return T("DBFilesClient\\ChallengeRules.dbc"); }
    AscDbc::Table& Spells()           { return T("DBFilesClient\\ChallengeSpells.dbc"); }
    AscDbc::Table& Modifiers()        { return T("DBFilesClient\\ChallengeModifiers.dbc"); }
    AscDbc::Table& Rewards()          { return T("DBFilesClient\\ChallengeRewards.dbc"); }
    AscDbc::Table& GroupRewards()     { return T("DBFilesClient\\ChallengeGroupRewards.dbc"); }
    AscDbc::Table& Featured()         { return T("DBFilesClient\\ChallengeFeatured.dbc"); }
    AscDbc::Table& ConditionTypes()   { return T("DBFilesClient\\ChallengeConditionTypes.dbc"); }
    AscDbc::Table& RequirementTypes() { return T("DBFilesClient\\ChallengeRequirementTypes.dbc"); }
    AscDbc::Table& RuleTypes()        { return T("DBFilesClient\\ChallengeRuleTypes.dbc"); }
    AscDbc::Table& ModifierTypes()    { return T("DBFilesClient\\ChallengeModifierTypes.dbc"); }

    const char* S(AscDbc::Table& t, Row r, uint32_t off) { return t.Str(r, off); }

    template <class Fn> void ForEachRow(AscDbc::Table& t, Fn fn)   // ids min..max, the original's loops
    {
        if (!t.Loaded())
            return;
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
            if (Row r = t.Row(id))
                fn(r);
    }

    // FUN_10087260 over a {name, length} table; -1 when absent.
    template <size_t N> int Index(const char* const (&table)[N], const char* s)
    {
        for (size_t i = 0; i < N; ++i)
            if (strcmp(table[i], s ? s : "") == 0)
                return static_cast<int>(i);
        return -1;
    }
    // The enum parsers log "Unexpected Value: {}" and fall back to the caller's default.
    template <size_t N> int IndexOr(const char* const (&table)[N], const char* s, int fallback)
    {
        const int i = Index(table, s);
        if (i < 0)
        {
            AscLog::Printf("Unexpected Value: %s", s ? s : "");
            return fallback;
        }
        return i;
    }

    // ==== cached data stores ==========================================================================
    struct Reward     // 0x12 bytes packed in the original
    {
        uint32_t item = 0, amount = 0, achievement = 0;
        bool firstCompletion = false;
        uint32_t sort = 0;             // +0x0D
        bool special = false;          // +0x11
    };
    struct Modifier { uint32_t type = 0; float v[3] = {0, 0, 0}; };

    struct LevelInfo
    {
        Row row = nullptr;
        std::vector<Row> conditions, requirements, firstRewards, repeatRewards, groupRewards, rules, spells, modifierRows;
        std::vector<Reward> rewards;
        std::vector<Modifier> modifiers;
    };

    struct Info
    {
        Row row = nullptr;
        std::vector<Row> groups;
        std::unordered_map<uint32_t, LevelInfo> levels;
        uint32_t minLevel = 0, maxLevel = 0;
    };

    std::unordered_map<uint32_t, Info> g_challenges;             // +0x158
    std::unordered_map<uint32_t, std::vector<Info*>> g_groups;    // +0x178

    // ==== server state ===============================================================================
    struct Active        // 0x1A bytes packed
    {
        uint32_t guid = 0, id = 0, level = 0;
        bool enabled = false;
        uint64_t startTime = 0;
        bool eligible = false;
        uint32_t deaths = 0;
    };
    struct Completed { uint32_t guid = 0, id = 0, level = 0; uint64_t start = 0, end = 0; };
    struct Criteria
    {
        uint32_t guid = 0, challenge = 0, level = 0, requirement = 0;
        uint64_t start = 0, achieve = 0;
        bool active = false;
    };
    struct Failure       // 0xEF bytes packed
    {
        std::string name;
        uint32_t challenge = 0, level = 0;
        uint64_t start = 0, end = 0;
        uint32_t deaths = 0;
        uint8_t playerLevel = 0, ruleset = 0;
        uint32_t map = 0;
        uint8_t difficulty = 0;
        uint32_t zone = 0, area = 0;
        float pos[3] = {0, 0, 0};
        std::string failReason, race, gender, cls, killerType, killerName;
        uint32_t killerSpell = 0, killerDamage = 0, killerEntry = 0, killerLevel = 0;
    };
    struct Completion    // 0x78 bytes
    {
        uint32_t challenge = 0, level = 0;
        std::string player;
        uint64_t start = 0, complete = 0;
        std::string race, gender, cls;
    };

    int64_t g_patchedAt = 0;                                                    // +0x00
    std::unordered_set<uint32_t> g_patched;                                     // +0x04
    std::unordered_map<uint32_t, Active> g_active;                              // +0x28
    std::unordered_multimap<uint32_t, Completed> g_completed;                   // +0x48
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, Criteria>> g_criteria;   // +0x68
    std::vector<std::pair<uint32_t, uint32_t>> g_pending;                       // +0x88
    std::vector<Failure> g_failures;                                            // +0x94
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::vector<Completion>>> g_completions;   // +0xA0
    std::unordered_set<std::string> g_brokenRules;                              // +0xC0
    int64_t g_rebuildAt = 0;                                                    // +0x1A0
    bool g_enteredWorld = false;                                                // +0x1A8
    int32_t g_filterMap = -1, g_filterDifficulty = -1;                          // +0x1AC / +0x1B0

    Info* FindInfo(uint32_t id)   // lambda 2 / FUN_10145ff0
    {
        auto it = g_challenges.find(id);
        return it == g_challenges.end() ? nullptr : &it->second;
    }
    LevelInfo* FindLevel(uint32_t id, uint32_t level)   // lambda 4 / FUN_101460a0
    {
        Info* info = FindInfo(id);
        if (!info)
            return nullptr;
        auto it = info->levels.find(level);
        return it == info->levels.end() ? nullptr : &it->second;
    }
    std::vector<LevelInfo*> GroupLevels(uint32_t group, uint32_t level)   // lambda 5 / FUN_10133700
    {
        std::vector<LevelInfo*> out;
        auto g = g_groups.find(group);
        if (g == g_groups.end())
            return out;
        for (Info* info : g->second)
        {
            auto it = info->levels.find(level);
            if (it != info->levels.end())
                out.push_back(&it->second);
        }
        return out;
    }
    std::vector<LevelInfo*> Targets(Row r)   // (+4 challenge, +8 group, +0xC level)
    {
        if (U32(r, 8) == 0)
        {
            LevelInfo* li = FindLevel(U32(r, 4), U32(r, 0xC));
            return li ? std::vector<LevelInfo*>{li} : std::vector<LevelInfo*>();
        }
        return GroupLevels(U32(r, 8), U32(r, 0xC));
    }
    bool ChallengeOrGroupKnown(uint32_t challenge, uint32_t group)   // FUN_10133970
    {
        return group != 0 ? g_groups.count(group) != 0 : g_challenges.count(challenge) != 0;
    }

    // FUN_1013b9c0: a completed record for (challenge, level).
    bool IsCompleted(uint32_t id, uint32_t level)
    {
        auto range = g_completed.equal_range(id);
        for (auto it = range.first; it != range.second; ++it)
            if (it->second.level == level)
                return true;
        return false;
    }
    // FUN_1013ba00: at least `count` challenges of the group completed at `level`.
    bool GroupCompletedCount(uint32_t group, uint32_t level, uint32_t count);

    bool IsFeatured(uint32_t id)   // FUN_1013bab0: the row whose week is trunc(tm_yday / 7.0f), UTC
    {
        if (id == 0)
            return false;
        const time_t now = time(nullptr);
        tm t = {};
        gmtime_s(&t, &now);
        const uint32_t week = static_cast<uint32_t>(static_cast<int64_t>(static_cast<float>(t.tm_yday) / 7.0f));
        AscDbc::Table& f = Featured();
        if (!f.Loaded())
            return false;
        for (uint32_t i = f.MinId(); i < f.MaxId() + 1; ++i)
            if (Row r = f.Row(i))
                if (U32(r, 4) == week)
                    return U32(r, 8) == id || U32(r, 0xC) == id || U32(r, 0x10) == id;
        return false;
    }

    uint8_t* Player() { return ActivePlayer(); }
    uint32_t PlayerClass()   // unit bytes0 byte 1 for a unit / player object
    {
        uint8_t* p = Player();
        if (!p)
            return 0;
        const uint32_t type = *reinterpret_cast<uint32_t*>(p + 0x14);
        if (type != 3 && type != 4)
            return 0;
        return (*reinterpret_cast<uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0x5C) >> 8) & 0xFF;
    }
    uint32_t PlayerLevel()
    {
        uint8_t* p = Player();
        return p ? *reinterpret_cast<uint32_t*>(*reinterpret_cast<uint8_t**>(p + 8) + 0xD8) : 0;
    }
    uint32_t Prestige()   // FUN_102d9f00: the per-unit record's +0x18
    {
        std::vector<AscObjectAddon::Field>& f = AscObjectAddon::Unit(ActivePlayerGuid());
        return f.size() > 2 ? f[2].value : 0;
    }
    uint32_t PartyMembers() { return reinterpret_cast<uint32_t(__cdecl*)()>(0x52BD10)(); }
    uint64_t PartyLeader() { return *reinterpret_cast<const uint64_t*>(0xBD1968); }

    // FUN_103087e0: the client's completed-achievement list.
    bool AchievementDone(uint32_t id)
    {
        uint32_t node = *reinterpret_cast<const uint32_t*>(0xACFDB0 + 8);
        if (node & 1)
            node = 0;
        while (node)
        {
            if (*reinterpret_cast<const uint32_t*>(node + 8) == id)
                return true;
            const uint32_t next = *reinterpret_cast<const uint32_t*>(node + 4);
            node = (next & 1) ? 0 : next;
        }
        return false;
    }
    // The reward achievement gate: a flagged (0x302) or unknown achievement, or one already earned.
    bool AchievementBlocks(uint32_t id)
    {
        if (id == 0)
            return false;
        Row a = ClientDbcRow(0xAD305C, id);   // FUN_100b1150
        return !a || (U32(a, 0x24) & 0x302) != 0 || AchievementDone(id);
    }

    // ==== UpdateChallengeDataStores (FUN_10143630) ====================================================
    Reward FromRewardRow(Row r)   // FUN_101309b0
    {
        Reward w;
        w.item = U32(r, 0x1C);
        w.amount = U32(r, 0x20);
        if (w.amount > 1 && IsFeatured(U32(r, 4)))
            if (const float* mult = AscConfig::Float("CONFIG_CHALLENGE_FEATURED_REWARD_MULTIPLIER"))
                w.amount = static_cast<uint32_t>(static_cast<int64_t>(*mult * static_cast<float>(w.amount)));
        w.achievement = U32(r, 0x24);
        w.firstCompletion = U32(r, 0x18) != 0;
        w.sort = U32(r, 0x28);
        w.special = false;
        return w;
    }
    Reward FromGroupRewardRow(Row r)
    {
        Reward w;
        w.item = U32(r, 0x18);
        w.amount = U32(r, 0x1C);
        w.achievement = U32(r, 0x20);
        w.firstCompletion = false;
        w.sort = U32(r, 0x24);
        w.special = true;
        return w;
    }

    void LogMissing(const char* what, Row r)
    {
        AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Challenge %u, group %u not found for %s %u",
                       U32(r, 4), U32(r, 8), what, U32(r, 0));
    }

    // Attach a row to every level it targets (conditions / requirements / rules / spells / modifiers).
    template <class Member> void Attach(AscDbc::Table& t, const char* what, Member member)
    {
        ForEachRow(t, [&](Row r) {
            std::vector<LevelInfo*> targets = Targets(r);
            if (targets.empty())
            {
                if (!ChallengeOrGroupKnown(U32(r, 4), U32(r, 8)))
                    LogMissing(what, r);
                return;
            }
            for (LevelInfo* li : targets)
                (li->*member).push_back(r);
        });
    }

    void Rebuild()
    {
        g_rebuildAt = 0;
        g_challenges.clear();
        g_groups.clear();

        ForEachRow(Challenges(), [](Row r) {
            if (!g_challenges.emplace(U32(r, 0), Info()).second)
                AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Challenge %u already exists", U32(r, 0));
            else
                g_challenges[U32(r, 0)].row = r;
        });
        ForEachRow(Groups(), [](Row r) {
            Info* info = FindInfo(U32(r, 4));
            if (!info)
            {
                AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Challenge %u not found for group %u", U32(r, 4), U32(r, 8));
                return;
            }
            std::vector<Info*>& members = g_groups[U32(r, 8)];
            if (std::find(members.begin(), members.end(), info) != members.end())
                AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Challenge %u already exists for group %u", U32(r, 4), U32(r, 8));
            else
                members.push_back(info);
            info->groups.push_back(r);
        });
        ForEachRow(Levels(), [](Row r) {
            Info* owner = FindInfo(U32(r, 4));
            if (!owner)
            {
                AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Challenge %u not found for level %u", U32(r, 4), U32(r, 0xC));
                return;
            }
            std::vector<Info*> infos;
            if (U32(r, 8) == 0)
                infos.push_back(owner);
            else if (g_groups.count(U32(r, 8)))
                infos = g_groups[U32(r, 8)];
            if (infos.empty())
                AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Challenge %u, group %u not found for level %u", U32(r, 4), U32(r, 8), U32(r, 0xC));
            for (Info* info : infos)
                info->levels[U32(r, 0xC)].row = r;
        });
        Attach(Conditions(), "condition", &LevelInfo::conditions);
        Attach(Requirements(), "requirement", &LevelInfo::requirements);
        Attach(Rules(), "rule", &LevelInfo::rules);
        Attach(Spells(), "spell", &LevelInfo::spells);
        Attach(Modifiers(), "modifier", &LevelInfo::modifierRows);

        // Rewards for this realm's expansion (RealmInfo ruleset) and the player's class; an achievement
        // reward that is flagged or already earned is dropped.
        const uint32_t expansion = RealmInfoSvc::Get().ruleset;
        const uint32_t cls = PlayerClass();
        ForEachRow(Rewards(), [&](Row r) {
            const int exp = IndexOr(kExpansions, S(Rewards(), r, 0x10), 3);
            if (exp != 3 && static_cast<uint32_t>(exp) != expansion)
                return;
            const int c = IndexOr(kChallengeClasses, S(Rewards(), r, 0x14), 0);
            if (c != 0 && static_cast<uint32_t>(c) != cls)
                return;
            if (AchievementBlocks(U32(r, 0x24)))
                return;
            std::vector<LevelInfo*> targets = Targets(r);
            if (targets.empty())
            {
                if (!ChallengeOrGroupKnown(U32(r, 4), U32(r, 8)))
                    LogMissing("reward", r);
                return;
            }
            for (LevelInfo* li : targets)
                (U32(r, 0x18) == 0 ? li->repeatRewards : li->firstRewards).push_back(r);
        });
        // Group rewards: shown on the challenges of the group once all but one of the required count
        // are completed at that level.
        ForEachRow(GroupRewards(), [&](Row r) {
            const int exp = IndexOr(kExpansions, S(GroupRewards(), r, 0x10), 3);
            if (exp != 3 && static_cast<uint32_t>(exp) != expansion)
                return;
            const int c = IndexOr(kChallengeClasses, S(GroupRewards(), r, 0x14), 0);
            if (c != 0 && static_cast<uint32_t>(c) != cls)
                return;
            if (AchievementBlocks(U32(r, 0x20)))
                return;
            std::set<Info*> done;
            AscDbc::Table& ct = Challenges();
            for (uint32_t id = ct.MinId(); ct.Loaded() && id <= ct.MaxId() + 1; ++id)
            {
                Info* info = FindInfo(id);
                if (!info)
                    continue;
                bool inGroup = false;
                for (Row g : info->groups)
                    inGroup = inGroup || U32(g, 8) == U32(r, 4);
                if (inGroup && IsCompleted(U32(info->row, 0), U32(r, 8)))
                    done.insert(info);
            }
            if (done.size() + 1 != U32(r, 0xC))
                return;
            std::vector<LevelInfo*> targets = GroupLevels(U32(r, 4), U32(r, 8));
            if (targets.empty())
            {
                if (!g_groups.count(U32(r, 4)))
                    AscLog::Printf("ChallengeMgr::UpdateChallengeDataStores: Group %u not found for group reward %u", U32(r, 4), U32(r, 0));
                return;
            }
            for (LevelInfo* li : targets)
                li->groupRewards.push_back(r);
        });

        for (auto& c : g_challenges)
            for (auto& l : c.second.levels)
                for (Row m : l.second.modifierRows)
                {
                    int type = Index(kChallengeModifiers, S(Modifiers(), m, 0x10));
                    if (type < 0)
                    {
                        AscLog::Printf("Unexpected Value: %s", S(Modifiers(), m, 0x10));
                        type = 0;
                    }
                    const float v[3] = {F32(m, 0x14), F32(m, 0x18), F32(m, 0x1C)};
                    auto it = std::find_if(l.second.modifiers.begin(), l.second.modifiers.end(),
                                           [&](const Modifier& x) { return x.type == static_cast<uint32_t>(type); });
                    if (it == l.second.modifiers.end())
                        l.second.modifiers.push_back({static_cast<uint32_t>(type), {v[0], v[1], v[2]}});
                    else
                        for (int i = 0; i < 3; ++i)
                            it->v[i] += v[i];
                }
        for (auto& c : g_challenges)
            for (auto& l : c.second.levels)
            {
                LevelInfo& li = l.second;
                const std::vector<Row>& own = IsCompleted(c.first, l.first) ? li.repeatRewards : li.firstRewards;
                for (Row r : own)
                    li.rewards.push_back(FromRewardRow(r));
                for (Row r : li.groupRewards)
                    li.rewards.push_back(FromGroupRewardRow(r));
            }
        for (auto& c : g_challenges)
        {
            uint32_t lo = 0xFFFFFFFF, hi = 0;
            for (auto& l : c.second.levels)
            {
                lo = std::min(lo, l.first);
                hi = std::max(hi, l.first);
            }
            c.second.minLevel = lo;
            c.second.maxLevel = hi;
        }
        for (auto& c : g_challenges)
            for (auto& l : c.second.levels)
            {
                LevelInfo& li = l.second;
                std::sort(li.requirements.begin(), li.requirements.end(), [](Row a, Row b) { return U32(a, 0x20) < U32(b, 0x20); });   // FUN_1012eb60
                std::sort(li.firstRewards.begin(), li.firstRewards.end(), [](Row a, Row b) { return U32(a, 0x28) < U32(b, 0x28); });   // FUN_1012edb0
                std::sort(li.repeatRewards.begin(), li.repeatRewards.end(), [](Row a, Row b) { return U32(a, 0x28) < U32(b, 0x28); });
                std::sort(li.rewards.begin(), li.rewards.end(), [](const Reward& a, const Reward& b) { return a.sort < b.sort; });   // FUN_1012f2b0
            }
    }

    void ScheduleRebuild() { g_rebuildAt = static_cast<int64_t>(time(nullptr)) + 1; }   // FUN_10143630(false)

    bool GroupCompletedCount(uint32_t group, uint32_t level, uint32_t count)
    {
        uint32_t n = 0;
        std::vector<Row> rows;   // FUN_10136100: normal, then trials, then dungeon trials
        for (int pass = 0; pass < 3; ++pass)
            ForEachRow(Challenges(), [&](Row r) {
                const bool trial = U32(r, 0x30) != 0, dungeon = U32(r, 0x34) != 0;
                if ((pass == 0 && !trial && !dungeon) || (pass == 1 && trial) || (pass == 2 && dungeon))
                    if (Info* info = FindInfo(U32(r, 0)))
                    {
                        bool in = group == 0;
                        for (Row g : info->groups)
                            in = in || U32(g, 8) == group;
                        if (in)
                            rows.push_back(r);
                    }
            });
        for (Row r : rows)
            if (IsCompleted(U32(r, 0), level) && ++n >= count)
                return true;
        return false;
    }

    // ==== conditions / validators =====================================================================
    // FUN_101b7900 (condition row: +0x14 type, +0x18..+0x20 values, +0x24 negative).
    bool ConditionMet(Row c)
    {
        int type = Index(kChallengeConditions, S(Conditions(), c, 0x14));
        if (type < 0)
        {
            AscLog::Printf("Unexpected Value: %s", S(Conditions(), c, 0x14));
            type = 0;
        }
        const bool neg = U32(c, 0x24) != 0;
        const float v1 = F32(c, 0x18), v2 = F32(c, 0x1C), v3 = F32(c, 0x20);
        const uint32_t mode = AscGameMode::Mode();
        auto clear = [&](int bit) { return (((mode >> bit) & 1) == 0) != neg; };
        switch (type)
        {
            case 1: return clear(0xF);
            case 2:
                if ((mode & 0x10000) || (Player() && PlayerLevel() != 1))
                    return neg;
                break;
            case 3: return clear(0x11);
            case 4: return clear(0x12);
            case 5: return clear(0x13);
            case 6: return clear(0x14);
            case 7: return clear(0x15);
            case 8: return IsCompleted(static_cast<uint32_t>(static_cast<int64_t>(v1)), static_cast<uint32_t>(static_cast<int64_t>(v2))) != neg;
            case 9: return GroupCompletedCount(static_cast<uint32_t>(static_cast<int64_t>(v1)), static_cast<uint32_t>(static_cast<int64_t>(v2)),
                                               static_cast<uint32_t>(static_cast<int64_t>(v3))) != neg;
            case 10: return (static_cast<uint32_t>(static_cast<int64_t>(v1)) <= Prestige()) != neg;
            case 0xB:
            {
                const uint8_t size = static_cast<uint8_t>(PartyMembers() + 1);
                const uint8_t want = static_cast<uint8_t>(static_cast<int32_t>(v1));
                if (size < 2)
                    return (want < 2) != neg;
                return (size == want) != neg;
            }
            case 0xC: return ((mode & 0x3F8000) == 0) != neg;
            case 0xD: return clear(0x16);
            case 0x10: return clear(0x17);
            default: break;
        }
        return !neg;
    }

    // FUN_1012e460 comparator: unmet conditions first, then by id.
    void SortConditions(std::vector<Row>& v)
    {
        std::sort(v.begin(), v.end(), [](Row a, Row b) {
            const bool ma = ConditionMet(a), mb = ConditionMet(b);
            if (ma != mb)
                return !ma && mb;
            return U32(a, 0) < U32(b, 0);
        });
    }
    Info* InfoSorted(uint32_t id, bool sort)   // FUN_10136810
    {
        Info* info = FindInfo(id);
        if (info && sort)
            for (auto& l : info->levels)
                SortConditions(l.second.conditions);
        return info;
    }
    LevelInfo* LevelSorted(uint32_t id, uint32_t level, bool sort)   // FUN_101368f0
    {
        LevelInfo* li = FindLevel(id, level);
        if (li && sort)
            SortConditions(li->conditions);
        return li;
    }

    // FUN_10130f40: requirement type index, or -1.
    int RequirementType(const char* s) { return Index(kChallengeRequirements, s); }

    const uint8_t* ChallengeRow(uint32_t id) { return Challenges().Row(id); }

    // FUN_10133dc0: every reason `id` at `level` cannot be started (CHALLENGE_START_*), minus `ignore`.
    bool CanActivate(uint32_t id, uint32_t level, std::vector<uint32_t>& errors, const std::set<uint32_t>& ignore)
    {
        auto add = [&](uint32_t code) { if (!ignore.count(code)) errors.push_back(code); };
        Row row = ChallengeRow(id);
        if (!row)
        {
            add(1);
        }
        else
        {
            if (!ignore.count(3) && g_active.count(id))
                errors.push_back(3);
            if (!ignore.count(4) && U32(row, 0x18) != 0)
                for (auto& a : g_active)
                    if (Row other = ChallengeRow(a.second.id))
                        if (U32(other, 0x18) == U32(row, 0x18))
                        {
                            errors.push_back(4);
                            break;
                        }
            if (!ignore.count(5) && U32(row, 0x24) != 0 && !AscGameEvents::IsActive(static_cast<uint16_t>(U32(row, 0x24))))
                errors.push_back(5);
            if (!ignore.count(6) && (U32(row, 0x14) & 9) != 0)
                errors.push_back(6);
            if (!ignore.count(8) && level > 1 && (U32(row, 0x14) & 0x12) != 0 && !IsCompleted(id, level - 1))
                errors.push_back(8);
            if (!ignore.count(9) && PartyLeader() != 0 && ActivePlayerGuid() != PartyLeader())
                errors.push_back(9);
            if (!ignore.count(0xA) && U32(row, 0x38) == 0 && Prestige() != 0)
                errors.push_back(0xA);
            if (!ignore.count(0xB) && U32(row, 0x38) == 0 && (AscGameMode::ActiveMask() & 0x3F8000) != 0)
                errors.push_back(0xB);
            if (!ignore.count(0xC))
                if (LevelInfo* li = FindLevel(id, level))
                    for (Row r : li->rules)
                        if (g_brokenRules.count(S(Rules(), r, 0x10)))
                        {
                            add(0xC);
                            break;
                        }
            if (!ignore.count(2) && U32(row, 0xC) != 0)
            {
                // RequiredGameMode (+0x10) names one of the realm game modes; its bit must be active.
                // Unknown names and "None" require that no restricted mode (0x19EF) is active.
                static const uint32_t kMasks[7] = {0x0, 0x1, 0x2, 0x4, 0x8, 0x20, 0x40};   // 0x10B31900
                const int m = IndexOr(kChallengeGameModes, S(Challenges(), row, 0x10), -1);
                const uint32_t mask = m >= 0 ? kMasks[m] : 0;
                const uint32_t mode = AscGameMode::Mode();
                if (mask != 0 ? (mode & mask) == 0 : (mode & 0x19EF) != 0)
                    errors.push_back(2);
            }
            if (!ignore.count(7))
                if (LevelInfo* li = FindLevel(id, level))
                    for (Row c : li->conditions)
                        if (!ConditionMet(c))
                        {
                            add(7);
                            break;
                        }
            const bool trial = U32(row, 0x30) != 0, dungeon = U32(row, 0x34) != 0;
            if (!ignore.count(0xD) && !trial && !dungeon)
                for (auto& p : g_pending)
                    if (Row pr = ChallengeRow(p.first))
                        if (U32(pr, 0x30) != 0)
                        {
                            add(0xD);
                            break;
                        }
            if (!ignore.count(0xE) && trial)
                for (auto& p : g_pending)
                    if (Row pr = ChallengeRow(p.first))
                        if (U32(pr, 0x30) == 0 && U32(pr, 0x34) == 0)
                        {
                            add(0xE);
                            break;
                        }
            if (!ignore.count(0x11) && !trial && !dungeon)
                add(0x11);
            if (U32(row, 0x40) != 0)
            {
                if (!ignore.count(0x13) && U32(row, 0x44) != *reinterpret_cast<const uint32_t*>(0xBD088C))
                    add(0x13);
                if (!ignore.count(0x14) && U32(row, 0x48) != *reinterpret_cast<const uint32_t*>(0xBD0894))
                    add(0x14);
            }
            if (!ignore.count(0x17))
            {
                LevelInfo* li = FindLevel(id, level);
                if (!li || !li->row)
                    add(0x17);
                else if (U32(li->row, 0x10) != 0 && !AscGameEvents::IsActive(static_cast<uint16_t>(U32(li->row, 0x10))))
                    add(0x17);
            }
        }
        if (!ignore.count(0xF) && AscTrialCreator::HasActiveTrial())
            add(0xF);
        if (!ignore.count(0x10))
        {
            const int32_t* forced = AscConfig::Int("CONFIG_CHALLENGE_FORCED");
            const int32_t* forcedLevel = AscConfig::Int("CONFIG_CHALLENGE_FORCED_LEVEL");
            if (forced && forcedLevel && *forced != 0 && !(static_cast<uint32_t>(*forced) == id && static_cast<uint32_t>(*forcedLevel) == level))
                add(0x10);
        }
        return errors.empty();
    }

    // FUN_1008e380: the player is at the level cap (or, on a dev realm, at 60 / 70 / 80).
    bool AtMaxLevel()
    {
        const uint32_t level = PlayerLevel();
        if (level >= AscGameEvents::ServerMaxLevel())
            return true;
        return RealmInfoSvc::Get().gates[4] != 0 && (level == 60 || level == 70 || level == 80);
    }

    // FUN_10134a80: every reason `id` cannot be stopped (CHALLENGE_STOP_*).
    bool CanDeactivate(uint32_t id, std::vector<uint32_t>& errors, const std::set<uint32_t>& ignore = {})
    {
        auto add = [&](uint32_t code) { if (!ignore.count(code)) errors.push_back(code); };
        auto active = g_active.find(id);
        if (active == g_active.end())
            add(1);
        bool permadeath = false;
        if (Player() && !AtMaxLevel())
            if (const bool* on = AscConfig::Bool("CONFIG_CHALLENGE_PERMADEATH_ENABLED"))
                permadeath = *on;
        if (permadeath && active != g_active.end() && active->second.deaths != 0)
            if (LevelInfo* li = FindLevel(id, active->second.level))
                for (Row r : li->requirements)
                    if (RequirementType(S(Requirements(), r, 0x10)) == 3)
                    {
                        add(2);
                        break;
                    }
        if (PartyLeader() != 0 && ActivePlayerGuid() != PartyLeader())
            add(3);
        if (AscTrialCreator::HasActiveTrial())
            add(4);
        const int32_t* forced = AscConfig::Int("CONFIG_CHALLENGE_FORCED");
        const int32_t* forcedLevel = AscConfig::Int("CONFIG_CHALLENGE_FORCED_LEVEL");
        if (forced && forcedLevel && *forced != 0 && static_cast<uint32_t>(*forced) == id)
        {
            const uint32_t level = active != g_active.end() ? active->second.level : 0;
            if (static_cast<uint32_t>(*forcedLevel) == level)
                add(5);
        }
        return errors.empty();
    }

    std::string StartResult(uint32_t code)   // FUN_10099eb0
    {
        return code < 0x18 ? kChallengeStartResults[code] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(code);
    }
    std::string StopResult(uint32_t code)    // FUN_1009a050
    {
        return code < 6 ? kChallengeStopResults[code] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(code);
    }

    // FUN_1012ae20 / FUN_101b86c0: active first, then dungeon trials, then featured, then SortOrder.
    bool ChallengeBefore(Row a, Row b)
    {
        const bool aa = g_active.count(U32(a, 0)) != 0, ab = g_active.count(U32(b, 0)) != 0;
        if (aa != ab)
            return aa;
        if (U32(a, 0x34) != 0)
            return true;
        if (U32(b, 0x34) != 0)
            return false;
        if (IsFeatured(U32(a, 0)))
            return true;
        if (IsFeatured(U32(b, 0)))
            return false;
        return U32(a, 0x4C) < U32(b, 0x4C);
    }

    // The list filter GetAllChallenges / GetAllTrials / the container share: hidden (flags bit 0) never;
    // "check activation" (bit 2) only when startable at level 1; map-bound ones only on their map or
    // while active.
    bool Listed(Row r)
    {
        if (U32(r, 0x14) & 1)
            return false;
        std::set<uint32_t> none;
        if (U32(r, 0x14) & 4)
        {
            std::vector<uint32_t> e;
            if (!CanActivate(U32(r, 0), 1, e, none))
                return false;
        }
        if (U32(r, 0x40) != 0 && !g_active.count(U32(r, 0)))
        {
            std::vector<uint32_t> e;
            CanActivate(U32(r, 0), 1, e, none);
            if (std::find(e.begin(), e.end(), 0x13u) != e.end() || std::find(e.begin(), e.end(), 0x14u) != e.end())
                return false;
        }
        return true;
    }

    // ==== ChallengeEntryContainer (vtable 0x10B32484) ================================================
    class Container : public AscFilter::Container<Row>
    {
    protected:
        std::vector<Row> DoPopulate() override   // FUN_101b8ce0
        {
            std::vector<Row> out;
            ForEachRow(Challenges(), [&](Row r) {
                if (Listed(r))
                    out.push_back(r);
            });
            std::sort(out.begin(), out.end(), ChallengeBefore);
            return out;
        }
        bool ArgMatch(uint32_t filter, const Row& r) override   // FUN_101b9270
        {
            if (g_active.count(U32(r, 0)) || U32(r, 0x34) != 0)
                return true;
            auto hasSoloRule = [&]() {
                if (LevelInfo* li = FindLevel(U32(r, 0), 1))
                    for (Row rule : li->rules)
                        if (IndexOr(kChallengeRules, S(Rules(), rule, 0x10), 2) == 2)   // FUN_1011f040(.., 2)
                            return true;
                return false;
            };
            switch (filter)
            {
                case 1: return U32(r, 0x30) != 0;
                case 2: return U32(r, 0x30) == 0;
                case 4: return U32(r, 0x38) != 0;
                case 5: return U32(r, 0x38) == 0;
                case 6:
                {
                    std::vector<uint32_t> e;
                    return CanActivate(U32(r, 0), 1, e, std::set<uint32_t>());
                }
                case 7: return U32(r, 4) != 0;
                case 8: return U32(r, 8) != 0;
                case 9: return hasSoloRule();
                case 10: return !hasSoloRule();
                case 11: case 12: case 13: case 14: case 15:
                    return IndexOr(kChallengeDifficulties, S(Challenges(), r, 0x2C), 0) == static_cast<int>(filter - 11);
                default: return false;
            }
        }
        uint32_t ArgGroup(uint32_t filter) override   // FUN_101b95f0
        {
            switch (filter)
            {
                case 1: case 2: case 3: return 1;
                case 4: case 5: return 2;
                case 7: case 8: return 3;
                case 9: case 10: return 4;
                case 11: case 12: case 13: case 14: case 15: return 5;
                default: return 0;
            }
        }
        bool TextMatch(const std::string& text, const Row& r) override   // FUN_101b9100
        {
            if (text.empty())
                return true;
            if (AscFilter::Lower(S(Challenges(), r, 0x1C)).find(text) != std::string::npos)
                return true;
            return AscFilter::Lower(S(Challenges(), r, 0x20)).find(text) != std::string::npos;
        }
    };
    Container& Entries() { static Container c; return c; }

    // ==== Lua ========================================================================================
    void Key(lua_State* L, const char* k) { AscLua::lua_pushstring(L, k); }
    void Set(lua_State* L) { AscLua::lua_settable(L, -3); }
    void SetNum(lua_State* L, const char* k, double v) { Key(L, k); PushNum(L, v); Set(L); }
    void SetInt(lua_State* L, const char* k, int32_t v) { Key(L, k); PushInt(L, v); Set(L); }
    void SetBool(lua_State* L, const char* k, bool v) { Key(L, k); PushBool(L, v); Set(L); }
    void SetStr(lua_State* L, const char* k, const char* v) { Key(L, k); PushStr(L, v); Set(L); }
    void NewTable(lua_State* L, int arr = 0) { AscLua::lua_createtable(L, arr, 0); AscLua::lua_checkstack(L, 2); }
    double U64d(uint64_t v) { return static_cast<double>(v); }   // FUN_10ae6470

    // RequiredGameMode as the serializer reads it: a fixed mode name -> its game-mode bit.
    uint32_t RequiredGameModeBit(const char* s)
    {
        static const struct { const char* n; uint32_t bit; } kModes[] = {
            {"Random", 1}, {"Ironman", 2}, {"Survivalist", 4}, {"Draft", 8}, {"Resolute", 0x20}, {"WildCard", 0x40},
            {"Felforged", 0x80}, {"Nightmare", 0x100}, {"BuildDraft", 0x800}, {"Crusader", 0x1000},
        };
        uint32_t bit = 0;
        for (const auto& m : kModes)
            if (strcmp(s, m.n) == 0)
                bit = m.bit;
        return bit;
    }

    // The per-level fields (the body of FUN_1013c330, inlined in FUN_1013cd30's Levels loop).
    void PushLevelFields(lua_State* L, uint32_t id, uint32_t level, LevelInfo* li)
    {
        SetBool(L, "IsCompleted", IsCompleted(id, level));
        Key(L, "Rewards");
        NewTable(L);
        int i = 0;
        for (const Reward& w : li->rewards)
        {
            PushNum(L, ++i);
            NewTable(L);
            SetBool(L, "IsFirstCompletion", w.firstCompletion);
            SetNum(L, "ItemID", static_cast<int32_t>(w.item));
            SetNum(L, "ItemAmount", static_cast<int32_t>(w.amount));
            SetNum(L, "Achievement", static_cast<int32_t>(w.achievement));
            SetBool(L, "IsSpecialReward", w.special);
            Set(L);
        }
        Set(L);
        Key(L, "Spells");
        NewTable(L);
        i = 0;
        for (Row s : li->spells)
        {
            // A spell line is listed unless every spell it names is hidden (Spell attributes bit 7).
            uint8_t rec[0x2A8];
            bool pveHidden = true, pvpHidden = true;
            if (FetchSpell(U32(s, 0x10), rec))
                pveHidden = (U32(rec, 0x10) >> 7) & 1;
            if (FetchSpell(U32(s, 0x14), rec))
                pvpHidden = (U32(rec, 0x10) >> 7) & 1;
            if (pveHidden && pvpHidden)
                continue;
            PushNum(L, ++i);
            NewTable(L);
            SetNum(L, "PvESpellID", static_cast<int32_t>(U32(s, 0x10)));
            SetNum(L, "PvPSpellID", static_cast<int32_t>(U32(s, 0x14)));
            SetBool(L, "IsDungeonPlayerSpell", U32(s, 0x1C) != 0);
            SetBool(L, "IsDungeonBossSpell", U32(s, 0x20) != 0);
            SetBool(L, "IsDungeonMinionSpell", U32(s, 0x24) != 0);
            Set(L);
        }
        Set(L);
        Key(L, "Conditions");
        NewTable(L);
        i = 0;
        for (Row c : li->conditions)
        {
            PushNum(L, ++i);
            NewTable(L);
            SetNum(L, "ElseGroup", static_cast<int32_t>(U32(c, 0x10)));
            SetStr(L, "ConditionType", S(Conditions(), c, 0x14));
            SetNum(L, "ConditionValue1", F32(c, 0x18));
            SetNum(L, "ConditionValue2", F32(c, 0x1C));
            SetNum(L, "ConditionValue3", F32(c, 0x20));
            SetBool(L, "NegativeCondition", U32(c, 0x24) != 0);
            SetBool(L, "ConditionMet", ConditionMet(c));
            Set(L);
        }
        Set(L);
        Key(L, "Rules");
        NewTable(L, static_cast<int>(li->rules.size()));
        i = 0;
        for (Row r : li->rules)
        {
            PushNum(L, ++i);
            PushStr(L, S(Rules(), r, 0x10));   // FUN_1009b2d0
            Set(L);
        }
        Set(L);
        Key(L, "Requirements");
        NewTable(L);
        i = 0;
        for (Row r : li->requirements)
        {
            PushNum(L, ++i);
            NewTable(L);
            SetStr(L, "RequirementType", S(Requirements(), r, 0x10));
            SetNum(L, "MiscValue1", F32(r, 0x14));
            SetNum(L, "MiscValue2", F32(r, 0x18));
            SetNum(L, "MiscValue3", F32(r, 0x1C));
            Set(L);
        }
        Set(L);
        Key(L, "Modifiers");
        NewTable(L);
        for (const Modifier& m : li->modifiers)
        {
            const std::string name = m.type < 7 ? kChallengeModifiers[m.type] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(m.type);
            PushStr(L, name.c_str());
            NewTable(L, 3);
            for (int k = 0; k < 3; ++k)
            {
                PushNum(L, k + 1);
                PushNum(L, m.v[k]);   // FUN_1009b2b0
                Set(L);
            }
            Set(L);
        }
        Set(L);
    }

    // FUN_1013cd30: the challenge table (or nil).
    void PushChallenge(lua_State* L, uint32_t id, bool sort)
    {
        Info* info = InfoSorted(id, sort);
        if (!info || !info->row)
        {
            AscLua::lua_pushnil(L);
            return;
        }
        Row r = info->row;
        AscDbc::Table& t = Challenges();
        NewTable(L);
        SetNum(L, "ID", static_cast<int32_t>(U32(r, 0)));
        SetBool(L, "PvE", U32(r, 4) != 0);
        SetBool(L, "PvP", U32(r, 8) != 0);
        SetBool(L, "RequiresGameMode", U32(r, 0xC) != 0);
        Key(L, "RequiredGameMode");
        PushInt(L, static_cast<int32_t>(RequiredGameModeBit(S(t, r, 0x10))));
        Set(L);
        SetNum(L, "Flags", static_cast<int32_t>(U32(r, 0x14)));
        SetNum(L, "ExclusiveGroup", static_cast<int32_t>(U32(r, 0x18)));
        SetStr(L, "Name", S(t, r, 0x1C));
        SetStr(L, "Description", S(t, r, 0x20));
        SetNum(L, "RequiredGameEvent", static_cast<int32_t>(U32(r, 0x24)));
        SetStr(L, "Icon", S(t, r, 0x28));
        SetStr(L, "DifficultyRating", S(t, r, 0x2C));
        Key(L, "GroupIds");
        NewTable(L, static_cast<int>(info->groups.size()));
        for (size_t i = 0; i < info->groups.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushInt(L, static_cast<int32_t>(U32(info->groups[i], 8)));   // FUN_1009b2a0
            Set(L);
        }
        Set(L);
        SetBool(L, "IsTrial", U32(r, 0x30) != 0);
        SetBool(L, "IsDungeonTrial", U32(r, 0x34) != 0);
        SetBool(L, "IsPrestige", U32(r, 0x38) != 0);
        SetBool(L, "ShouldCheckMap", U32(r, 0x40) != 0);
        SetInt(L, "MapID", static_cast<int32_t>(U32(r, 0x44)));
        SetInt(L, "MapDifficulty", static_cast<int32_t>(U32(r, 0x48)));
        SetNum(L, "SortOrder", static_cast<int32_t>(U32(r, 0x4C)));
        SetStr(L, "Artwork", S(t, r, 0x50));
        SetBool(L, "IsFeatured", IsFeatured(id));
        Key(L, "Levels");
        NewTable(L);
        for (uint32_t level = 1; level <= info->maxLevel && info->maxLevel != 0xFFFFFFFF; ++level)
        {
            PushNum(L, level);
            if (LevelInfo* li = LevelSorted(id, level, sort))
            {
                NewTable(L);
                PushLevelFields(L, id, level, li);
            }
            else
                AscLua::lua_pushnil(L);
            Set(L);
        }
        Set(L);
    }

    void PushCriteria(lua_State* L, const Criteria& c)   // FUN_1013c0f0 (into the table on top)
    {
        SetNum(L, "Guid", static_cast<int32_t>(c.guid));
        SetNum(L, "Challenge", static_cast<int32_t>(c.challenge));
        SetNum(L, "Level", static_cast<int32_t>(c.level));
        Row req = Requirements().Row(c.requirement);
        SetStr(L, "RequirementType", req ? S(Requirements(), req, 0x10) : "CHALLENGE_REQUIREMENT_TYPE_NONE");
        SetNum(L, "StartTime", U64d(c.start));
        SetNum(L, "AchieveTime", U64d(c.achieve));
        SetBool(L, "Active", c.active);
        SetNum(L, "MiscValue1", req ? F32(req, 0x14) : 0.0);
        SetNum(L, "MiscValue2", req ? F32(req, 0x18) : 0.0);
        SetNum(L, "MiscValue3", req ? F32(req, 0x1C) : 0.0);
    }

    void PushCompletion(lua_State* L, const Completion& c)   // FUN_1013bf30
    {
        NewTable(L);
        SetNum(L, "Challenge", static_cast<int32_t>(c.challenge));
        SetNum(L, "Level", static_cast<int32_t>(c.level));
        SetStr(L, "PlayerName", c.player.c_str());
        SetNum(L, "StartTime", static_cast<int32_t>(c.start));
        SetNum(L, "CompleteTime", static_cast<int32_t>(c.complete));
        SetStr(L, "PlayerRace", c.race.c_str());
        SetStr(L, "PlayerGender", c.gender.c_str());
        SetStr(L, "PlayerClass", c.cls.c_str());
    }

    void PushFailure(lua_State* L, const Failure& f)   // FUN_1013ea00
    {
        NewTable(L);
        SetStr(L, "Name", f.name.c_str());
        SetNum(L, "Challenge", static_cast<int32_t>(f.challenge));
        SetNum(L, "Level", static_cast<int32_t>(f.level));
        SetNum(L, "StartTime", static_cast<int32_t>(f.start));
        SetNum(L, "EndTime", static_cast<int32_t>(f.end));
        SetNum(L, "DeathCount", static_cast<int32_t>(f.deaths));
        SetNum(L, "Ruleset", f.ruleset);
        SetNum(L, "MapId", static_cast<int32_t>(f.map));
        SetNum(L, "Difficulty", f.difficulty);
        SetNum(L, "ZoneId", static_cast<int32_t>(f.zone));
        SetNum(L, "AreaId", static_cast<int32_t>(f.area));
        float pos[3] = {f.pos[0], f.pos[1], f.pos[2]}, x = 0, y = 0;
        reinterpret_cast<void(__cdecl*)(float*, uint32_t, float*, float*, int, int, int)>(0x544140)(pos, f.map, &x, &y, 0, 0, 0);   // FUN_10256a70
        SetNum(L, "X", x);
        SetNum(L, "Y", y);
        SetStr(L, "FailReason", f.failReason.c_str());
        SetStr(L, "PlayerRace", f.race.c_str());
        SetStr(L, "PlayerGender", f.gender.c_str());
        SetStr(L, "PlayerClass", f.cls.c_str());
        SetNum(L, "PlayerLevel", f.playerLevel);
        SetStr(L, "KillerType", f.killerType.c_str());
        SetStr(L, "KillerName", f.killerName.c_str());
        SetNum(L, "KillerSpell", static_cast<int32_t>(f.killerSpell));
        SetNum(L, "KillerDamage", static_cast<int32_t>(f.killerDamage));
        SetNum(L, "KillerEntry", static_cast<int32_t>(f.killerEntry));
        SetNum(L, "KillerLevel", static_cast<int32_t>(f.killerLevel));
    }

    int PushResults(lua_State* L, bool ok, const std::vector<uint32_t>& errors, std::string (*name)(uint32_t))
    {
        PushBool(L, ok);
        if (errors.empty())
        {
            AscLua::lua_pushnil(L);
            return 2;
        }
        NewTable(L, static_cast<int>(errors.size()));
        for (size_t i = 0; i < errors.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, name(errors[i]).c_str());
            Set(L);
        }
        return 2;
    }

    // ---- bindings ------------------------------------------------------------------------------------
    int CanActivateChallenge(lua_State* L)   // FUN_1013f700 (id[, level = 1])
    {
        int32_t id = 0, level = 0;
        if (!ReadInt2(L, id, level))
        {
            uint32_t only;
            if (!ReadNumber(L, only))
                return 0;
            id = static_cast<int32_t>(only);
            level = 1;
        }
        std::vector<uint32_t> errors;
        const bool ok = CanActivate(static_cast<uint32_t>(id), static_cast<uint32_t>(level), errors, std::set<uint32_t>());
        return PushResults(L, ok, errors, StartResult);
    }

    int CanDeactivateChallenge(lua_State* L)   // FUN_1013f9a0
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        std::vector<uint32_t> errors;
        const bool ok = CanDeactivate(id, errors);
        return PushResults(L, ok, errors, StopResult);
    }

    int CanResurrect(lua_State* L)   // FUN_10133bb0
    {
        bool ok = true;
        const bool* permadeath = AscConfig::Bool("CONFIG_CHALLENGE_PERMADEATH_ENABLED");
        if (permadeath && *permadeath)
            for (auto& a : g_active)
                if (LevelInfo* li = FindLevel(a.second.id, a.second.level))
                    for (Row r : li->requirements)
                        if (RequirementType(S(Requirements(), r, 0x10)) == 3 && F32(r, 0x14) <= static_cast<float>(static_cast<int32_t>(a.second.deaths)))
                            ok = false;
        PushBool(L, ok);
        return 1;
    }

    int GetActiveChallenges(lua_State* L)   // FUN_1013fcc0
    {
        NewTable(L);
        int i = 0;
        for (auto& a : g_active)
        {
            PushNum(L, ++i);
            NewTable(L);
            SetNum(L, "Id", static_cast<int32_t>(a.second.id));
            SetNum(L, "Level", static_cast<int32_t>(a.second.level));
            SetBool(L, "Enabled", a.second.enabled);
            SetNum(L, "StartTime", U64d(a.second.startTime));
            SetBool(L, "EligibleForCompletion", a.second.eligible);
            SetNum(L, "DeathCount", static_cast<int32_t>(a.second.deaths));
            Set(L);
        }
        return 1;
    }

    int GetCompletedChallenges(lua_State* L)   // FUN_10141da0
    {
        NewTable(L);
        int i = 0;
        for (auto& c : g_completed)
        {
            PushNum(L, ++i);
            NewTable(L);
            SetNum(L, "Id", static_cast<int32_t>(c.second.id));
            SetNum(L, "Level", static_cast<int32_t>(c.second.level));
            SetNum(L, "StartTime", U64d(c.second.start));
            SetNum(L, "EndTime", U64d(c.second.end));
            Set(L);
        }
        return 1;
    }

    int GetChallengeCriterias(lua_State* L)   // FUN_10141140
    {
        NewTable(L);
        int i = 0;
        for (auto& c : g_criteria)
            for (auto& r : c.second)
            {
                PushNum(L, ++i);
                NewTable(L);
                PushCriteria(L, r.second);
                Set(L);
            }
        return 1;
    }

    int GetChallengeCriteriaInfo(lua_State* L)   // FUN_10140dd0 (challenge, requirement type)
    {
        if (!ValidateInput(L, {NUMBER, STRING}))
            return 0;
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const std::string type = CheckString(L, 2);
        NewTable(L);
        auto it = g_criteria.find(id);
        if (it == g_criteria.end())
            return 1;
        int i = 0;
        for (auto& r : it->second)
            if (Row req = Requirements().Row(r.second.requirement))
                if (type == S(Requirements(), req, 0x10))
                {
                    PushNum(L, ++i);
                    NewTable(L);
                    PushCriteria(L, r.second);
                    Set(L);
                }
        return 1;
    }

    // GetAllChallenges (FUN_1013fea0) / GetAllTrials (FUN_101402f0): normal challenges or trials,
    // listed ones only, in challenge order.
    int PushList(lua_State* L, bool trials)
    {
        std::vector<Row> rows;
        ForEachRow(Challenges(), [&](Row r) {
            const bool t = U32(r, 0x30) != 0 || U32(r, 0x34) != 0;
            if (t == trials && FindInfo(U32(r, 0)))
                rows.push_back(r);
        });
        std::sort(rows.begin(), rows.end(), ChallengeBefore);   // FUN_1012e6a0
        NewTable(L);
        int i = 0;
        for (Row r : rows)
            if (Listed(r))
            {
                PushNum(L, ++i);
                PushChallenge(L, U32(r, 0), true);
                Set(L);
            }
        return 1;
    }
    int GetAllChallenges(lua_State* L) { return PushList(L, false); }
    int GetAllTrials(lua_State* L) { return PushList(L, true); }

    int GetBrokenChallengeRules(lua_State* L)   // FUN_10140740: one type string per broken rule row
    {
        std::vector<std::string> out;
        ForEachRow(Rules(), [&](Row r) {
            if (g_brokenRules.count(S(Rules(), r, 0x10)))
                out.push_back(S(Rules(), r, 0x10));
        });
        NewTable(L, static_cast<int>(out.size()));
        for (size_t i = 0; i < out.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, out[i].c_str());
            Set(L);
        }
        return 1;
    }

    int HasBrokenChallengeRule(lua_State* L)   // FUN_101429b0
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        PushBool(L, g_brokenRules.count(s) != 0);
        return 1;
    }

    int HasChallengeRule(lua_State* L)   // FUN_10142ae0: any active challenge's level has the rule
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        bool found = false;
        for (auto& a : g_active)
            if (LevelInfo* li = FindLevel(a.second.id, a.second.level))
                for (Row r : li->rules)
                    found = found || s == S(Rules(), r, 0x10);
        PushBool(L, found);
        return 1;
    }

    int GetChallengeAtIndex(lua_State* L)   // FUN_101408b0 (index, sortConditions)
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return 0;
        const int32_t index = ToInt(CheckNumber(L, 1));
        const bool sort = AscLua::lua_toboolean(L, 2) != 0;
        if (index == 0)
            return 0;
        const std::vector<Row>& f = Entries().Filtered();
        if (static_cast<uint32_t>(index - 1) < f.size() && f[index - 1])
            PushChallenge(L, U32(f[index - 1], 0), sort);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetChallengeIDByIndex(lua_State* L)   // handler_GetChallengeIDByIndex
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        const std::vector<Row>& f = Entries().Filtered();
        if (index - 1 < f.size() && f[index - 1])
            PushNum(L, static_cast<int32_t>(U32(f[index - 1], 0)));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetNumChallenges(lua_State* L) { PushInt(L, static_cast<int32_t>(Entries().Size())); return 1; }

    int GetChallengeInfo(lua_State* L)   // handler_GetChallengeInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        PushChallenge(L, id, true);
        return 1;
    }

    int GetChallengeInfoByLevel(lua_State* L)   // FUN_10141760 -> FUN_1013c330
    {
        int32_t id, level;
        if (!ReadInt2(L, id, level))
            return 0;
        if (!InfoSorted(static_cast<uint32_t>(id), true))
            return 1;
        LevelInfo* li = LevelSorted(static_cast<uint32_t>(id), static_cast<uint32_t>(level), true);
        if (!li)
            return 1;
        PushChallenge(L, static_cast<uint32_t>(id), true);
        PushLevelFields(L, static_cast<uint32_t>(id), static_cast<uint32_t>(level), li);
        return 1;
    }

    // FUN_101417b0 (id): (available level, highest completed level, max level).
    int GetChallengeLevels(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        uint32_t lo = 0, hi = 0;
        if (Info* info = FindInfo(id))
        {
            hi = info->maxLevel;
            lo = info->minLevel;
        }
        const uint32_t maxLevel = hi;
        uint32_t available = 0, completed = 0;
        if (hi >= lo)
        {
            for (uint32_t l = hi;; --l)
            {
                if (IsCompleted(id, l))
                {
                    completed = l;
                    break;
                }
                if (l == lo || l == 0)
                    break;
            }
            const std::set<uint32_t> ignore = {3};
            for (uint32_t l = hi;; --l)
            {
                std::vector<uint32_t> e;
                if (CanActivate(id, l, e, ignore))
                {
                    available = l;
                    break;
                }
                if (l == lo || l == 0)
                    break;
            }
        }
        PushNum(L, std::max<uint32_t>(1, available));
        PushNum(L, completed);
        PushNum(L, maxLevel);
        return 3;
    }

    int GetPendingChallenges(lua_State* L)   // FUN_10142400
    {
        NewTable(L);
        int i = 0;
        for (auto& p : g_pending)
        {
            PushNum(L, ++i);
            AscLua::lua_createtable(L, 0, 2);
            SetNum(L, "ChallengeId", static_cast<int32_t>(p.first));
            SetNum(L, "Level", static_cast<int32_t>(p.second));
            Set(L);
        }
        return 1;
    }

    int GetChallengesWithGroupID(lua_State* L)   // FUN_10141af0
    {
        uint32_t group;
        if (!ReadNumber(L, group))
            return 0;
        std::vector<uint32_t> ids;
        for (int pass = 0; pass < 3; ++pass)
            ForEachRow(Challenges(), [&](Row r) {
                const bool trial = U32(r, 0x30) != 0, dungeon = U32(r, 0x34) != 0;
                if (!((pass == 0 && !trial && !dungeon) || (pass == 1 && trial) || (pass == 2 && dungeon)))
                    return;
                Info* info = FindInfo(U32(r, 0));
                if (!info)
                    return;
                bool in = group == 0;
                for (Row g : info->groups)
                    in = in || U32(g, 8) == group;
                if (in)
                    ids.push_back(U32(r, 0));
            });
        NewTable(L, static_cast<int>(ids.size()));
        for (size_t i = 0; i < ids.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushInt(L, static_cast<int32_t>(ids[i]));
            Set(L);
        }
        return 1;
    }

    int IsChallengeActive(lua_State* L)   // handler_IsChallengeActive: (active, level)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        auto it = g_active.find(id);
        PushBool(L, it != g_active.end());
        PushNum(L, it != g_active.end() ? static_cast<int32_t>(it->second.level) : 0);
        return 2;
    }

    int GetChallengeFailure(lua_State* L)   // handler_GetChallengeFailure (0-based index)
    {
        uint32_t index;
        if (!ReadNumber(L, index))
            return 0;
        if (index >= g_failures.size())
            AscLua::lua_pushnil(L);
        else
            PushFailure(L, g_failures[index]);
        return 1;
    }

    // FUN_101412b0 (zone, count): this zone's failures of the last 7 days, latest first.
    int GetChallengeFailures(lua_State* L)
    {
        int32_t zone, count;
        if (!ReadInt2(L, zone, count))
            return 0;
        NewTable(L);
        const uint64_t since = static_cast<uint64_t>(static_cast<int64_t>(time(nullptr)) - 0x93A80);
        std::vector<const Failure*> list;
        for (const Failure& f : g_failures)
            if (f.zone == static_cast<uint32_t>(zone) && f.end >= since)
                list.push_back(&f);
        std::sort(list.begin(), list.end(), [](const Failure* a, const Failure* b) { return a->end > b->end; });   // FUN_1012e870
        if (list.size() > static_cast<uint32_t>(count))
            list.resize(static_cast<uint32_t>(count));
        int i = 0;
        for (const Failure* f : list)
        {
            PushNum(L, ++i);
            PushFailure(L, *f);
            Set(L);
        }
        return 1;
    }

    int QueryChallengeFailures(lua_State* L)   // FUN_10142ec0
    {
        Packet(0x5A1).Send();
        PushBool(L, true);
        return 1;
    }

    int QueryChallengeCompletions(lua_State* L)   // FUN_10142da0 (challenge, level)
    {
        int32_t id, level;
        if (!ReadInt2(L, id, level))
            return 0;
        Packet(0x5C6).U32(static_cast<uint32_t>(id)).U32(static_cast<uint32_t>(level)).Send();
        PushBool(L, true);
        return 1;
    }

    const std::vector<Completion>* CompletionsOf(uint32_t id, uint32_t level)
    {
        auto c = g_completions.find(id);
        if (c == g_completions.end())
            return nullptr;
        auto l = c->second.find(level);
        return l == c->second.end() ? nullptr : &l->second;
    }

    bool Read3(lua_State* L, uint32_t& a, uint32_t& b, uint32_t& c)   // FUN_100d0410
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, NUMBER}))
            return false;
        a = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        b = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        c = static_cast<uint32_t>(ToInt(CheckNumber(L, 3)));
        return true;
    }

    int GetChallengeCompletion(lua_State* L)   // handler_GetChallengeCompletion (challenge, level, index)
    {
        uint32_t id, level, index;
        if (!Read3(L, id, level, index))
            return 0;
        const std::vector<Completion>* v = CompletionsOf(id, level);
        if (!v || index >= v->size())
            AscLua::lua_pushnil(L);
        else
            PushCompletion(L, (*v)[index]);
        return 1;
    }

    // FUN_10140ab0 (challenge, level, ranks): completions grouped by equal completion time, fastest first.
    int GetChallengeCompletions(lua_State* L)
    {
        uint32_t id, level, ranks;
        if (!Read3(L, id, level, ranks) || ranks == 0)
            return 0;
        NewTable(L);
        std::vector<std::vector<const Completion*>> groups;
        if (const std::vector<Completion>* v = CompletionsOf(id, level))
            for (const Completion& c : *v)
            {
                const uint64_t d = c.complete - c.start;
                if (groups.empty() || groups.back().back()->complete - groups.back().back()->start != d)
                    groups.emplace_back();
                groups.back().push_back(&c);
            }
        for (uint32_t g = 0; g < ranks && g < groups.size(); ++g)
        {
            PushNum(L, g + 1);
            NewTable(L);
            for (size_t j = 0; j < groups[g].size(); ++j)
            {
                PushNum(L, static_cast<double>(j + 1));
                PushCompletion(L, *groups[g][j]);
                Set(L);
            }
            Set(L);
        }
        return 1;
    }

    // Localization lookups: the type row whose name (+4) equals the argument, else nil.
    int Localize(lua_State* L, AscDbc::Table& t, std::initializer_list<uint32_t> offs)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        if (t.Loaded())
            for (uint32_t id = t.MinId(); id < t.MaxId() + 1; ++id)
                if (Row r = t.Row(id))
                    if (s == S(t, r, 4))
                    {
                        for (uint32_t off : offs)
                            PushStr(L, S(t, r, off));
                        return static_cast<int>(offs.size());
                    }
        AscLua::lua_pushnil(L);
        return 1;
    }
    int GetConditionLocalization(lua_State* L)   { return Localize(L, ConditionTypes(), {8, 0xC, 0x10, 0x14, 0x18, 0x1C, 0x20}); }   // FUN_10141f20
    int GetRequirementLocalization(lua_State* L) { return Localize(L, RequirementTypes(), {8, 0xC, 0x10, 0x14, 0x18, 0x1C, 0x20}); }   // FUN_10142520
    int GetModifierLocalization(lua_State* L)    { return Localize(L, ModifierTypes(), {8, 0xC, 0x10}); }   // FUN_101421a0
    int GetRuleLocalization(lua_State* L)        { return Localize(L, RuleTypes(), {8, 0xC}); }   // FUN_101427a0

    int SendChallengeSyncResponse(lua_State* L)   // FUN_10142f90 (accept)
    {
        if (!ValidateInput(L, {BOOLEAN}))
            return 0;
        const uint8_t accept = AscLua::lua_toboolean(L, 1) != 0;
        Packet(0x59C).U8(accept).Send();
        PushBool(L, true);
        return 1;
    }

    int StartChallenge(lua_State* L)   // FUN_10143300 (id, level)
    {
        int32_t id, level;
        if (!ReadInt2(L, id, level))
            return 0;
        Packet(0x592).U32(static_cast<uint32_t>(id)).U32(static_cast<uint32_t>(level)).Send();
        PushBool(L, true);
        return 1;
    }

    int StopChallenge(lua_State* L)   // FUN_10143420 (id)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Packet(0x594).U32(id).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_101430f0 (text, {FILTER_* = bool}): a map / difficulty change repopulates first.
    int SetChallengeFilter(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, TABLE}))
            return 0;
        const std::string text = CheckString(L, 1);
        std::vector<uint32_t> filters;
        std::map<std::string, bool> names;   // FUN_10131480
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 2))
        {
            if (AscLua::lua_isstring(L, -2))
                names[AscLua::lua_tolstring(L, -2, nullptr)] = AscLua::lua_toboolean(L, -1) != 0;
            AscLua::lua_settop(L, -2);
        }
        for (auto& n : names)
            if (n.second)
            {
                const int i = Index(kChallengeFilters, n.first.c_str());
                if (i >= 0)
                    filters.push_back(static_cast<uint32_t>(i));
            }
        const int32_t map = *reinterpret_cast<const int32_t*>(0xBD088C);
        const int32_t difficulty = *reinterpret_cast<const int32_t*>(0xBD0894);
        if (g_filterMap != map || g_filterDifficulty != difficulty)
        {
            Entries().Reset();
            g_filterMap = map;
            g_filterDifficulty = difficulty;
        }
        Entries().ApplyFilter(text, filters, {});
        return 0;
    }

    // ==== packets ====================================================================================
    template <class T> T Read(CDataStore* p) { T v; memcpy(&v, p->m_buffer + p->m_read, sizeof(T)); p->m_read += sizeof(T); return v; }
    std::string ReadSized(CDataStore* p)   // FUN_100d3680: u32 length + bytes
    {
        const uint32_t n = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += static_cast<int32_t>(n);
        return s;
    }

    Active ReadActive(CDataStore* p)   // 0x1A bytes
    {
        Active a;
        a.guid = Read<uint32_t>(p);
        a.id = Read<uint32_t>(p);
        a.level = Read<uint32_t>(p);
        a.enabled = Read<uint8_t>(p) != 0;
        a.startTime = Read<uint64_t>(p);
        a.eligible = Read<uint8_t>(p) != 0;
        a.deaths = Read<uint32_t>(p);
        return a;
    }

    void __cdecl OnActiveList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x596
    {
        g_active.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const Active a = ReadActive(p);
            g_active[a.id] = a;
        }
        AscRuntime::Signal("CHALLENGE_ACTIVE_LIST_CHANGED");
    }

    void __cdecl OnActiveUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5A6
    {
        const Active a = ReadActive(p);
        auto old = g_active.find(a.id);
        const bool existed = old != g_active.end();
        const uint32_t oldDeaths = existed ? old->second.deaths : 0;
        g_active[a.id] = a;
        if (!existed || oldDeaths == a.deaths)
            return;
        // A death under a no-death requirement: remaining deaths.
        if (LevelInfo* li = FindLevel(a.id, a.level))
            for (Row r : li->requirements)
                if (RequirementType(S(Requirements(), r, 0x10)) == 3)
                {
                    const uint32_t allowed = static_cast<uint32_t>(static_cast<int64_t>(F32(r, 0x14)));
                    const uint32_t left = allowed > a.deaths ? allowed - a.deaths : 0;
                    AscRuntime::Signal("CHALLENGE_DEATH_UPDATE", "%u%u%u", a.id, a.level, left);
                    break;
                }
    }

    void __cdecl OnCompletedList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x597
    {
        g_completed.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            Completed c;
            c.guid = Read<uint32_t>(p);
            c.id = Read<uint32_t>(p);
            c.level = Read<uint32_t>(p);
            c.start = Read<uint64_t>(p);
            c.end = Read<uint64_t>(p);
            g_completed.emplace(c.id, c);
        }
        ScheduleRebuild();
        AscRuntime::Signal("CHALLENGE_COMPLETED_LIST_CHANGED");
    }

    void __cdecl OnCompleted(void*, uint32_t, uint32_t, CDataStore* p)   // 0x598
    {
        Completed c;
        c.guid = Read<uint32_t>(p);
        c.id = Read<uint32_t>(p);
        c.level = Read<uint32_t>(p);
        c.start = Read<uint64_t>(p);
        c.end = Read<uint64_t>(p);
        g_completed.emplace(c.id, c);   // FUN_10130b60
        ScheduleRebuild();
        AscRuntime::Signal("CHALLENGE_COMPLETED", "%u%u", c.id, c.level);
    }

    Criteria ReadCriteria(CDataStore* p)   // 0x21 bytes
    {
        Criteria c;
        c.guid = Read<uint32_t>(p);
        c.challenge = Read<uint32_t>(p);
        c.level = Read<uint32_t>(p);
        c.requirement = Read<uint32_t>(p);
        c.start = Read<uint64_t>(p);
        c.achieve = Read<uint64_t>(p);
        c.active = Read<uint8_t>(p) != 0;
        return c;
    }

    void __cdecl OnCriteriaList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x599
    {
        g_criteria.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const Criteria c = ReadCriteria(p);
            g_criteria[c.challenge][c.requirement] = c;
        }
        AscRuntime::Signal("CHALLENGE_CRITERIA_LIST_CHANGED");
    }

    void __cdecl OnCriteriaUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x59A
    {
        const Criteria c = ReadCriteria(p);
        bool completed = false, failed = false;
        auto outer = g_criteria.find(c.challenge);
        if (outer != g_criteria.end())
        {
            auto old = outer->second.find(c.requirement);
            if (old != outer->second.end())
            {
                if (old->second.achieve == 0)
                    completed = c.achieve != 0;
                else if (old->second.active)
                    failed = !(c.active || c.achieve != 0);
            }
        }
        g_criteria[c.challenge][c.requirement] = c;
        const char* type = "CHALLENGE_REQUIREMENT_TYPE_NONE";
        double v[3] = {0, 0, 0};
        if (Row req = Requirements().Row(c.requirement))
        {
            type = S(Requirements(), req, 0x10);
            v[0] = F32(req, 0x14);
            v[1] = F32(req, 0x18);
            v[2] = F32(req, 0x1C);
        }
        AscRuntime::Signal("CHALLENGE_CRITERIA_UPDATED", "%u%u%s%f%f%f", c.challenge, c.level, type, v[0], v[1], v[2]);
        if (completed)
            AscRuntime::Signal("CHALLENGE_CRITERIA_COMPLETED", "%u%u%s%f%f%f", c.challenge, c.level, type, v[0], v[1], v[2]);
        else if (failed)
            AscRuntime::Signal("CHALLENGE_CRITERIA_FAILED", "%u%u%s%f%f%f", c.challenge, c.level, type, v[0], v[1], v[2]);
    }

    void __cdecl OnSyncRequest(void*, uint32_t, uint32_t, CDataStore* p)   // 0x59B
    {
        g_pending.clear();
        const uint32_t arg = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const uint32_t id = Read<uint32_t>(p);
            const uint32_t level = Read<uint32_t>(p);
            g_pending.emplace_back(id, level);
        }
        AscRuntime::Signal("CHALLENGE_SYNC_REQUEST", "%u", arg);
    }

    Failure ReadFailure(CDataStore* p)   // FUN_1013f230
    {
        Failure f;
        f.name = ReadSized(p);
        f.challenge = Read<uint32_t>(p);
        f.level = Read<uint32_t>(p);
        f.start = Read<uint64_t>(p);
        f.end = Read<uint64_t>(p);
        f.deaths = Read<uint32_t>(p);
        f.playerLevel = Read<uint8_t>(p);
        f.ruleset = Read<uint8_t>(p);
        f.map = Read<uint32_t>(p);
        f.difficulty = Read<uint8_t>(p);
        f.zone = Read<uint32_t>(p);
        f.area = Read<uint32_t>(p);
        for (float& v : f.pos)
            v = Read<float>(p);
        f.failReason = ReadSized(p);
        f.race = ReadSized(p);
        f.gender = ReadSized(p);
        f.cls = ReadSized(p);
        f.killerType = ReadSized(p);
        f.killerName = ReadSized(p);
        f.killerSpell = Read<uint32_t>(p);
        f.killerDamage = Read<uint32_t>(p);
        f.killerEntry = Read<uint32_t>(p);
        f.killerLevel = Read<uint32_t>(p);
        return f;
    }

    void __cdecl OnFailureList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5A2
    {
        g_failures.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_failures.push_back(ReadFailure(p));
        AscRuntime::Signal("CHALLENGE_FAILURE_LIST_CHANGED");
    }

    void __cdecl OnFailure(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5A3
    {
        g_failures.push_back(ReadFailure(p));
        const Failure& f = g_failures.back();
        AscRuntime::Signal("CHALLENGE_FAILURE_ADDED", "%u%u%u", f.challenge, f.level, static_cast<uint32_t>(g_failures.size() - 1));
    }

    Completion ReadCompletion(CDataStore* p)   // FUN_1013efd0
    {
        Completion c;
        c.challenge = Read<uint32_t>(p);
        c.level = Read<uint32_t>(p);
        c.player = ReadSized(p);
        c.start = Read<uint64_t>(p);
        c.complete = Read<uint64_t>(p);
        c.race = ReadSized(p);
        c.gender = ReadSized(p);
        c.cls = ReadSized(p);
        return c;
    }

    void SortCompletions(uint32_t id, uint32_t level)   // FUN_10143520: fastest first
    {
        auto c = g_completions.find(id);
        if (c == g_completions.end())
            return;
        auto l = c->second.find(level);
        if (l == c->second.end())
            return;
        std::sort(l->second.begin(), l->second.end(),
                  [](const Completion& a, const Completion& b) { return a.complete - a.start < b.complete - b.start; });
    }

    void __cdecl OnCompletionList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5C7
    {
        const uint32_t id = Read<uint32_t>(p);
        const uint32_t level = Read<uint32_t>(p);
        auto c = g_completions.find(id);
        if (c != g_completions.end())
        {
            c->second.erase(level);
            ScheduleRebuild();
        }
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            Completion r = ReadCompletion(p);
            g_completions[r.challenge][r.level].push_back(std::move(r));
        }
        SortCompletions(id, level);
        AscRuntime::Signal("CHALLENGE_COMPLETION_LIST_CHANGED", "%u%u", id, level);
    }

    void __cdecl OnCompletion(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5C8
    {
        Completion r = ReadCompletion(p);
        const uint32_t id = r.challenge, level = r.level;
        g_completions[id][level].push_back(std::move(r));
        SortCompletions(id, level);
        AscRuntime::Signal("CHALLENGE_COMPLETION_ADDED", "%u%u%u", id, level,
                           static_cast<uint32_t>(g_completions[id][level].size() - 1));
    }

    void __cdecl OnBrokenRuleList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5BA
    {
        g_brokenRules.clear();
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_brokenRules.insert(ReadSized(p));
        ScheduleRebuild();
    }

    void __cdecl OnBrokenRuleUpdate(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5BB
    {
        const std::string rule = ReadSized(p);
        const uint8_t broken = Read<uint8_t>(p);
        if (broken)
            g_brokenRules.insert(rule);
        else
            g_brokenRules.erase(rule);
        ScheduleRebuild();
        AscRuntime::Signal("CHALLENGE_RULE_BROKEN_UPDATE", "%s%b", rule.c_str(), static_cast<int>(broken));
    }

    void __cdecl OnStartResponse(void*, uint32_t, uint32_t, CDataStore* p)   // 0x593
    {
        const uint32_t id = Read<uint32_t>(p);
        const uint32_t level = Read<uint32_t>(p);
        const uint32_t result = Read<uint32_t>(p);
        const uint64_t start = Read<uint64_t>(p);
        const uint8_t eligible = Read<uint8_t>(p);
        if (result == 0)
        {
            uint64_t guid = 0;
            if (!UnitTokenGuid("player", guid))
                return;
            // The player's GUID low part, as the packed-GUID helpers trim it.
            const uint32_t high = static_cast<uint32_t>(guid >> 32);
            uint32_t low = static_cast<uint32_t>(guid);
            if (high >= 0x10000000)
            {
                const uint32_t kind = high >> 16;
                if (!(kind == 0xF101 || kind == 0x1FC0 || kind == 0 || kind == 8000 || kind == 0x1F50 || kind == 0x4000 || kind == 0xF100))
                    low &= 0xFFFFFF;
            }
            Active a;
            a.guid = low;
            a.id = id;
            a.level = level;
            a.enabled = true;
            a.startTime = start;
            a.eligible = eligible != 0;
            a.deaths = 0;
            g_active[id] = a;
        }
        AscRuntime::Signal("CHALLENGE_START_RESPONSE", "%u%u%s", id, level, StartResult(result).c_str());
    }

    void __cdecl OnStopResponse(void*, uint32_t, uint32_t, CDataStore* p)   // 0x595
    {
        const uint32_t id = Read<uint32_t>(p);
        const uint32_t level = Read<uint32_t>(p);
        const uint32_t result = Read<uint32_t>(p);
        if (result == 0)
            g_active.erase(id);
        AscRuntime::Signal("CHALLENGE_STOP_RESPONSE", "%u%u%s", id, level, StopResult(result).c_str());
    }

    // ---- SMSG_PATCH_CHALLENGE_*: mark the affected challenges (CHALLENGE_PATCHED two seconds later)
    // and re-index a second later.
    void MarkPatched(uint32_t id)   // FUN_10135130
    {
        g_patched.insert(id);
        g_patchedAt = static_cast<int64_t>(time(nullptr)) + 2;
    }
    void MarkGroup(uint32_t group)   // FUN_10135180
    {
        auto g = g_groups.find(group);
        if (g == g_groups.end())
            return;
        std::vector<Info*> members = g->second;
        for (Info* info : members)
            if (info->row)
                MarkPatched(U32(info->row, 0));
    }
    void AfterChallenge(const uint8_t* r)      { Entries().Reset(); ScheduleRebuild(); MarkPatched(U32(r, 0)); }
    void AfterKeyed(const uint8_t* r)          // (+4 challenge, +8 group)
    {
        ScheduleRebuild();
        if (U32(r, 4))
            MarkPatched(U32(r, 4));
        if (U32(r, 8))
            MarkGroup(U32(r, 8));
    }
    void AfterGroup(const uint8_t* r)          { ScheduleRebuild(); MarkPatched(U32(r, 4)); }
    void AfterFeatured(const uint8_t* r)
    {
        for (uint32_t off : {8u, 0xCu, 0x10u})
            if (U32(r, off))
                MarkPatched(U32(r, off));
    }
    void AfterGroupReward(const uint8_t* r)    { ScheduleRebuild(); if (U32(r, 4)) MarkGroup(U32(r, 4)); }

    // The tick after client 0x403340 (0x1013BE30).
    void Tick()
    {
        const int64_t now = static_cast<int64_t>(time(nullptr));
        if (!g_patched.empty() && now > g_patchedAt)
        {
            AscRuntime::Signal("CHALLENGE_PATCHED");
            g_patched.clear();
        }
        if (g_rebuildAt != 0 && now > g_rebuildAt)
            Rebuild();
    }

    void OnEnteredWorld()   // 0x1013BF00
    {
        if (!g_enteredWorld)
        {
            ScheduleRebuild();
            g_enteredWorld = true;
        }
    }

    void OnGlueScreen()   // FUN_10135010
    {
        g_patched.clear();
        g_active.clear();
        g_completed.clear();
        g_criteria.clear();
        g_pending.clear();
        g_failures.clear();
        g_completions.clear();
        g_brokenRules.clear();
        Entries().Reset();
        g_rebuildAt = 0;
        g_enteredWorld = false;
        g_filterMap = -1;
        g_filterDifficulty = -1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x596, CNetClientCustomPacket((void*)&OnActiveList, nullptr));
        sDC.AddPacketHandler(0x5A6, CNetClientCustomPacket((void*)&OnActiveUpdate, nullptr));
        sDC.AddPacketHandler(0x597, CNetClientCustomPacket((void*)&OnCompletedList, nullptr));
        sDC.AddPacketHandler(0x598, CNetClientCustomPacket((void*)&OnCompleted, nullptr));
        sDC.AddPacketHandler(0x599, CNetClientCustomPacket((void*)&OnCriteriaList, nullptr));
        sDC.AddPacketHandler(0x59A, CNetClientCustomPacket((void*)&OnCriteriaUpdate, nullptr));
        sDC.AddPacketHandler(0x59B, CNetClientCustomPacket((void*)&OnSyncRequest, nullptr));
        sDC.AddPacketHandler(0x5A2, CNetClientCustomPacket((void*)&OnFailureList, nullptr));
        sDC.AddPacketHandler(0x5A3, CNetClientCustomPacket((void*)&OnFailure, nullptr));
        sDC.AddPacketHandler(0x5C7, CNetClientCustomPacket((void*)&OnCompletionList, nullptr));
        sDC.AddPacketHandler(0x5C8, CNetClientCustomPacket((void*)&OnCompletion, nullptr));
        sDC.AddPacketHandler(0x5BA, CNetClientCustomPacket((void*)&OnBrokenRuleList, nullptr));
        sDC.AddPacketHandler(0x5BB, CNetClientCustomPacket((void*)&OnBrokenRuleUpdate, nullptr));
        sDC.AddPacketHandler(0x593, CNetClientCustomPacket((void*)&OnStartResponse, nullptr));
        sDC.AddPacketHandler(0x595, CNetClientCustomPacket((void*)&OnStopResponse, nullptr));

        // Struct images with u32-length strings (FUN_100d3680), except the modifier tables' C strings.
        AscDbcPatch::RegisterSized(0x591, "DBFilesClient\\Challenge.dbc", 0x54, (1ull << 4) | (1ull << 7) | (1ull << 8) | (1ull << 10) | (1ull << 11) | (1ull << 20), AfterChallenge);
        AscDbcPatch::RegisterSized(0x5B2, "DBFilesClient\\ChallengeConditions.dbc", 0x28, 1ull << 5, AfterKeyed);
        AscDbcPatch::RegisterSized(0x5B3, "DBFilesClient\\ChallengeFeatured.dbc", 0x14, 0, AfterFeatured);
        AscDbcPatch::RegisterSized(0x5B4, "DBFilesClient\\ChallengeGroupRewards.dbc", 0x28, (1ull << 4) | (1ull << 5), AfterGroupReward);
        AscDbcPatch::RegisterSized(0x5B5, "DBFilesClient\\ChallengeGroups.dbc", 0xC, 0, AfterGroup);
        AscDbcPatch::RegisterSized(0x5B6, "DBFilesClient\\ChallengeRequirements.dbc", 0x24, 1ull << 4, AfterKeyed);
        AscDbcPatch::RegisterSized(0x5B7, "DBFilesClient\\ChallengeRewards.dbc", 0x2C, (1ull << 4) | (1ull << 5), AfterKeyed);
        AscDbcPatch::RegisterSized(0x5B8, "DBFilesClient\\ChallengeRules.dbc", 0x14, 1ull << 4, AfterKeyed);
        AscDbcPatch::RegisterSized(0x5B9, "DBFilesClient\\ChallengeSpells.dbc", 0x28, 0, AfterKeyed);
        AscDbcPatch::RegisterSized(0x5BC, "DBFilesClient\\ChallengeRequirementTypes.dbc", 0x24, 0x1FEull, nullptr);
        AscDbcPatch::RegisterSized(0x5BE, "DBFilesClient\\ChallengeConditionTypes.dbc", 0x24, 0x1FEull, nullptr);
        AscDbcPatch::RegisterSized(0x5BD, "DBFilesClient\\ChallengeRuleTypes.dbc", 0x10, 0xEull, nullptr);
        AscDbcPatch::RegisterSized(0x677, "DBFilesClient\\ChallengeLevels.dbc", 0x14, 0, AfterKeyed);
        AscDbcPatch::Register(0x663, "DBFilesClient\\ChallengeModifiers.dbc", 0x20, 1ull << 4, AfterKeyed);
        AscDbcPatch::Register(0x664, "DBFilesClient\\ChallengeModifierTypes.dbc", 0x14, 0x1Eull, nullptr);

        AscRuntime::OnGlueScreen(OnGlueScreen);
        AscRuntime::OnAfterEnterWorld(OnEnteredWorld);
        AscRuntime::OnAfter403340(Tick);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Challenge", "StartChallenge", StartChallenge},
        {"C_Challenge", "GetActiveChallenges", GetActiveChallenges},
        {"C_Challenge", "GetCompletedChallenges", GetCompletedChallenges},
        {"C_Challenge", "GetChallengeCriterias", GetChallengeCriterias},
        {"C_Challenge", "GetChallengeCriteriaInfo", GetChallengeCriteriaInfo},
        {"C_Challenge", "StopChallenge", StopChallenge},
        {"C_Challenge", "GetAllChallenges", GetAllChallenges},
        {"C_Challenge", "GetAllTrials", GetAllTrials},
        {"C_Challenge", "GetChallengeInfo", GetChallengeInfo},
        {"C_Challenge", "GetChallengeInfoByLevel", GetChallengeInfoByLevel},
        {"C_Challenge", "IsChallengeActive", IsChallengeActive},
        {"C_Challenge", "CanActivateChallenge", CanActivateChallenge},
        {"C_Challenge", "GetChallengeLevels", GetChallengeLevels},
        {"C_Challenge", "SendChallengeSyncResponse", SendChallengeSyncResponse},
        {"C_Challenge", "GetPendingChallenges", GetPendingChallenges},
        {"C_Challenge", "CanResurrect", CanResurrect},
        {"C_Challenge", "CanDeactivateChallenge", CanDeactivateChallenge},
        {"C_Challenge", "GetChallengesWithGroupID", GetChallengesWithGroupID},
        {"C_Challenge", "QueryChallengeFailures", QueryChallengeFailures},
        {"C_Challenge", "GetChallengeFailures", GetChallengeFailures},
        {"C_Challenge", "GetChallengeFailure", GetChallengeFailure},
        {"C_Challenge", "HasBrokenChallengeRule", HasBrokenChallengeRule},
        {"C_Challenge", "GetBrokenChallengeRules", GetBrokenChallengeRules},
        {"C_Challenge", "GetConditionLocalization", GetConditionLocalization},
        {"C_Challenge", "GetRuleLocalization", GetRuleLocalization},
        {"C_Challenge", "GetRequirementLocalization", GetRequirementLocalization},
        {"C_Challenge", "GetModifierLocalization", GetModifierLocalization},
        {"C_Challenge", "HasChallengeRule", HasChallengeRule},
        {"C_Challenge", "QueryChallengeCompletions", QueryChallengeCompletions},
        {"C_Challenge", "GetChallengeCompletions", GetChallengeCompletions},
        {"C_Challenge", "GetChallengeCompletion", GetChallengeCompletion},
        {"C_Challenge", "GetNumChallenges", GetNumChallenges},
        {"C_Challenge", "GetChallengeAtIndex", GetChallengeAtIndex},
        {"C_Challenge", "SetChallengeFilter", SetChallengeFilter},
        {"C_Challenge", "GetChallengeIDByIndex", GetChallengeIDByIndex},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscChallenge
{
bool CanActivate(uint32_t id, uint32_t level, const std::set<uint32_t>& ignore)
{
    std::vector<uint32_t> errors;
    return ::CanActivate(id, level, errors, ignore);
}

bool CanDeactivate(uint32_t id, const std::set<uint32_t>& ignore)
{
    std::vector<uint32_t> errors;
    return ::CanDeactivate(id, errors, ignore);
}

std::vector<uint32_t> ActiveIds()
{
    std::vector<uint32_t> out;
    for (auto& a : g_active)
        out.push_back(a.second.id);
    return out;
}

const uint8_t* ChallengeRowOf(uint32_t id) { return ChallengeRow(id); }

int Difficulty(const uint8_t* row) { return IndexOr(kChallengeDifficulties, S(Challenges(), row, 0x2C), 0); }

bool HasSoloRule(uint32_t id, uint32_t level)
{
    if (LevelInfo* li = FindLevel(id, level))
        for (Row rule : li->rules)
            if (IndexOr(kChallengeRules, S(Rules(), rule, 0x10), 2) == 2)
                return true;
    return false;
}

bool ActiveHasRule(uint32_t type)
{
    for (auto& a : g_active)
        if (LevelInfo* li = FindLevel(a.second.id, a.second.level))
            for (Row rule : li->rules)
                if (static_cast<uint32_t>(IndexOr(kChallengeRules, S(Rules(), rule, 0x10), 0)) == type)
                    return true;
    return false;
}

void PushMergedLevels(lua_State* L, const std::vector<std::pair<uint32_t, uint32_t>>& levels, bool sortConditions)
{
    std::vector<Row> spells, conditions, requirements, rules;
    std::vector<Reward> rewards;
    for (const auto& lv : levels)
        if (LevelInfo* li = LevelSorted(lv.first, lv.second, sortConditions))
        {
            spells.insert(spells.end(), li->spells.begin(), li->spells.end());
            conditions.insert(conditions.end(), li->conditions.begin(), li->conditions.end());
            requirements.insert(requirements.end(), li->requirements.begin(), li->requirements.end());
            rules.insert(rules.end(), li->rules.begin(), li->rules.end());
            rewards.insert(rewards.end(), li->rewards.begin(), li->rewards.end());
        }
    for (std::vector<Row>* v : {&spells, &conditions, &requirements, &rules})   // FUN_1011e1f0 + std::unique
    {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    SetBool(L, "IsCompleted", false);
    Key(L, "Rewards");
    NewTable(L);
    int i = 0;
    for (const Reward& w : rewards)
    {
        PushNum(L, ++i);
        NewTable(L);
        SetBool(L, "IsFirstCompletion", w.firstCompletion);
        SetNum(L, "ItemID", static_cast<int32_t>(w.item));
        SetNum(L, "ItemAmount", static_cast<int32_t>(w.amount));
        SetNum(L, "Achievement", static_cast<int32_t>(w.achievement));
        SetBool(L, "IsSpecialReward", w.special);
        Set(L);
    }
    Set(L);
    Key(L, "Spells");
    NewTable(L);
    i = 0;
    for (Row s : spells)
    {
        PushNum(L, ++i);
        NewTable(L);
        SetNum(L, "PvESpellID", static_cast<int32_t>(U32(s, 0x10)));
        SetNum(L, "PvPSpellID", static_cast<int32_t>(U32(s, 0x14)));
        SetBool(L, "IsDungeonPlayerSpell", U32(s, 0x1C) != 0);
        SetBool(L, "IsDungeonBossSpell", U32(s, 0x20) != 0);
        SetBool(L, "IsDungeonMinionSpell", U32(s, 0x24) != 0);
        Set(L);
    }
    Set(L);
    Key(L, "Conditions");
    NewTable(L);
    i = 0;
    for (Row c : conditions)
    {
        PushNum(L, ++i);
        NewTable(L);
        SetNum(L, "ElseGroup", static_cast<int32_t>(U32(c, 0x10)));
        SetStr(L, "ConditionType", S(Conditions(), c, 0x14));
        SetNum(L, "ConditionValue1", F32(c, 0x18));
        SetNum(L, "ConditionValue2", F32(c, 0x1C));
        SetNum(L, "ConditionValue3", F32(c, 0x20));
        SetBool(L, "NegativeCondition", U32(c, 0x24) != 0);
        SetBool(L, "ConditionMet", ConditionMet(c));
        Set(L);
    }
    Set(L);
    Key(L, "Rules");
    NewTable(L, static_cast<int>(rules.size()));
    i = 0;
    for (Row r : rules)
    {
        PushNum(L, ++i);
        PushStr(L, S(Rules(), r, 0x10));
        Set(L);
    }
    Set(L);
    Key(L, "Requirements");
    NewTable(L);
    i = 0;
    for (Row r : requirements)
    {
        PushNum(L, ++i);
        NewTable(L);
        SetStr(L, "RequirementType", S(Requirements(), r, 0x10));
        SetNum(L, "MiscValue1", F32(r, 0x14));
        SetNum(L, "MiscValue2", F32(r, 0x18));
        SetNum(L, "MiscValue3", F32(r, 0x1C));
        Set(L);
    }
    Set(L);
}
}

// FUN_10143630(false) for other modules (the attach hooks after 0x5B3020 / 0x5B32F0 / 0x5B3610).
void AscChallenge_ScheduleRebuild() { ScheduleRebuild(); }
