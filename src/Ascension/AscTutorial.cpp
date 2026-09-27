// C_Tutorial -- tutorials, tutorial keywords and mentors. Transcribed from the original's manager at
// 0x10d3c5a8 (accessor FUN_10a18d70, ctor FUN_10a15d00, init 0x10a18c20, Lua table FUN_10a1dd80).
//
// Manager layout: +0x000 TutorialEntryContainer, +0x07C TutorialKeywordContainer, +0x0F8
// MentorWhoisDataContainer (all FilterableContainerBase, see AscFilterContainer.hpp); +0x174 rewarded
// tutorial ids; +0x180 active mentor specializations; +0x18C pending specialization; +0x190 mentor
// whois records (0x70 bytes); +0x19C mentor status; +0x19D last seen level; +0x1A0 tutorials not yet
// applicable; +0x1C0 applicable tutorials not yet complete.
//
// Tables (the DLL's own DBCs): Tutorial, TutorialCategories, TutorialKeywords, TutorialObjectives,
// TutorialObjectiveTypes, TutorialRewards, MentorSpecializations.
//
// Tutorial row: +0 id, +4 category, +8 completion quest, +0xC start quest, +0x10..+0x34 previous
// tutorials (any one complete), +0x3C race mask, +0x40 class mask (64-bit), +0x48 min level, +0x4C max
// level, +0x50 int, +0x54 sort order, +0x58..+0x78 required spells (any one known), +0x7C required
// game modes, +0x80 excluded game modes, +0x84 expansion (3 = any), +0x88 required for mentor status,
// +0x8C/+0x90/+0x9C strings, +0x94 name, +0x98 description, +0xA0..+0xB0 realm flags.
// Keyword row: +0 id, +4 bracketed, +8 colour, +0xC key, +0x10/+0x14 strings, +0x18 name, +0x1C info
// text, +0x20 index text, +0x24..+0x34 realm flags, +0x38 race mask, +0x3C class mask (64-bit).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscDbcPatch.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

using namespace AscScript;
using AscDbc::Table;

namespace
{
    Table& Tutorials()      { return AscDbc::Get("DBFilesClient\\Tutorial.dbc"); }
    Table& Categories()     { return AscDbc::Get("DBFilesClient\\TutorialCategories.dbc"); }
    Table& Keywords()       { return AscDbc::Get("DBFilesClient\\TutorialKeywords.dbc"); }
    Table& Objectives()     { return AscDbc::Get("DBFilesClient\\TutorialObjectives.dbc"); }
    Table& ObjectiveTypes() { return AscDbc::Get("DBFilesClient\\TutorialObjectiveTypes.dbc"); }
    Table& Rewards()        { return AscDbc::Get("DBFilesClient\\TutorialRewards.dbc"); }
    Table& MentorSpecs()    { return AscDbc::Get("DBFilesClient/MentorSpecializations.dbc"); }

    // ---- the player, as the applicability checks read it -----------------------------------------
    struct PlayerInfo { bool present; uint8_t race, cls; uint32_t level; uint8_t* obj; };
    PlayerInfo Player()
    {
        PlayerInfo p{false, 0, 0, 0, ActivePlayer()};
        if (!p.obj)
            return p;
        p.present = true;
        const uint8_t* desc = *reinterpret_cast<uint8_t**>(p.obj + 8);
        const int32_t type = *reinterpret_cast<const int32_t*>(p.obj + 0x14);
        if (type == 3 || type == 4)
        {
            p.race = desc[0x5C];
            p.cls = desc[0x5D];
        }
        p.level = *reinterpret_cast<const uint32_t*>(desc + 0xD8);
        return p;
    }

    bool ClassInMask(uint8_t cls, uint32_t lo, uint32_t hi)
    {
        const uint32_t c = static_cast<uint32_t>(cls) - 1;
        const uint32_t bitLo = c < 32 ? (1u << c) : 0;
        const uint32_t bitHi = (c >= 32 && c < 64) ? (1u << (c - 32)) : 0;
        return (bitLo & lo) || (bitHi & hi);
    }

    // The realm-flag gate both applicability checks share: the first set flag whose realm gate is
    // also set passes; none passing fails.
    bool RealmAllowed(const uint8_t* row, uint32_t firstFlagOff)
    {
        const RealmInfo& r = RealmInfoSvc::Get();
        for (int i = 0; i < 5; ++i)
            if (Table::U32(row, firstFlagOff + i * 4) && r.gates[i])
                return true;
        return false;
    }

    // FUN_103087e0: the client's completed-quest set (list at *(0xACFDB0 + 8)).
    bool QuestCompleted(uint32_t quest)
    {
        uint32_t node = *reinterpret_cast<const uint32_t*>(0xACFDB0 + 8);
        if (node & 1)
            node = 0;
        while (node)
        {
            if (*reinterpret_cast<const uint32_t*>(node + 8) == quest)
                return true;
            node = *reinterpret_cast<const uint32_t*>(node + 4);
            if (node & 1)
                node = 0;
        }
        return false;
    }

    // FUN_10a18e10
    bool TutorialComplete(uint32_t id)
    {
        const uint8_t* row = Tutorials().Row(id);
        if (!row)
            return false;
        const uint32_t quest = Table::U32(row, 8);
        return !quest || QuestCompleted(quest);
    }

    // ---- manager state -----------------------------------------------------------------------------
    struct MentorRecord
    {
        std::string name;
        uint8_t level = 0;
        std::string s1;
        std::vector<uint32_t> specs;
        std::string s2, s3;
    };

    std::vector<uint32_t> g_rewarded;          // +0x174
    std::vector<uint32_t> g_activeSpecs;       // +0x180
    uint32_t g_pendingSpec = 0;                // +0x18C
    std::vector<MentorRecord> g_mentors;       // +0x190
    uint8_t g_mentorStatus = 0;                // +0x19C
    uint8_t g_lastLevel = 0;                   // +0x19D
    std::set<uint32_t> g_notApplicable;        // +0x1A0
    std::set<uint32_t> g_incomplete;           // +0x1C0

    bool Rewarded(uint32_t id) { return std::find(g_rewarded.begin(), g_rewarded.end(), id) != g_rewarded.end(); }

    // FUN_102182f0: 0 = applicable, otherwise the TUTORIAL_APPLICABILITY_* index. skipProgress stops
    // after the realm / race / class / expansion checks.
    uint32_t Applicability(const uint8_t* row, bool skipProgress)
    {
        const PlayerInfo p = Player();
        if (!p.present)
            return 1;
        if (!RealmAllowed(row, 0xA0))
            return 2;
        const uint32_t raceMask = Table::U32(row, 0x3C);
        if (raceMask && !(raceMask & (1u << ((p.race - 1u) & 31))))
            return 3;
        const uint32_t classLo = Table::U32(row, 0x40), classHi = Table::U32(row, 0x44);
        if ((classLo || classHi) && !ClassInMask(p.cls, classLo, classHi))
            return 4;
        const uint32_t expansion = Table::U32(row, 0x84);
        if (expansion != 3 && expansion != RealmInfoSvc::Get().ruleset)
            return 0xB;
        if (skipProgress)
            return 0;
        if (!TutorialComplete(Table::U32(row, 0)))
        {
            const uint32_t minLevel = Table::U32(row, 0x48), maxLevel = Table::U32(row, 0x4C);
            if (minLevel && p.level < minLevel)
                return 5;
            if (maxLevel && maxLevel < p.level)
                return 6;
        }
        int first = -1;
        for (int i = 0; i < 10; ++i)
            if (Table::U32(row, 0x10 + i * 4)) { first = i; break; }
        if (first >= 0)
        {
            bool any = false;
            for (int i = first; i < 10 && !any; ++i)
            {
                const uint32_t prev = Table::U32(row, 0x10 + i * 4);
                any = prev && TutorialComplete(prev);
            }
            if (!any)
                return 7;
        }
        first = -1;
        for (int i = 0; i < 9; ++i)
            if (Table::U32(row, 0x58 + i * 4)) { first = i; break; }
        if (first >= 0)
        {
            typedef char(__thiscall* Knows_t)(void*, uint32_t);
            bool any = false;
            for (int i = first; i < 9 && !any; ++i)
                any = reinterpret_cast<Knows_t>(0x7260E0)(p.obj, Table::U32(row, 0x58 + i * 4)) != 0;
            if (!any)
                return 8;
        }
        const uint32_t require = Table::U32(row, 0x7C);
        if (require && !(AscGameMode::ActiveMask() & require))
            return 9;
        const uint32_t exclude = Table::U32(row, 0x80);
        if (exclude && (AscGameMode::ActiveMask() & exclude))
            return 10;
        return 0;
    }

    const char* const kApplicability[] = {
        "TUTORIAL_APPLICABILITY_OK", "TUTORIAL_APPLICABILITY_NOT_IN_WORLD",
        "TUTORIAL_APPLICABILITY_NOT_ENABLED_ON_REALM", "TUTORIAL_APPLICABILITY_BAD_RACE",
        "TUTORIAL_APPLICABILITY_BAD_CLASS", "TUTORIAL_APPLICABILITY_LEVEL_TOO_LOW",
        "TUTORIAL_APPLICABILITY_LEVEL_TOO_HIGH", "TUTORIAL_APPLICABILITY_PREV_NOT_COMPLETED",
        "TUTORIAL_APPLICABILITY_REQUIRED_SPELLS", "TUTORIAL_APPLICABILITY_REQUIRED_GAME_MODES",
        "TUTORIAL_APPLICABILITY_EXCEPT_GAME_MODES", "TUTORIAL_APPLICABILITY_BAD_EXPANSION",
    };
    // FUN_10a15a90
    std::string ApplicabilityName(uint32_t v)
    {
        if (v < 12)
            return kApplicability[v];
        char buf[48];
        sprintf_s(buf, "UNEXPECTED_ENUM_VALUE_%u", v);
        return buf;
    }

    // FUN_10218dc0
    bool KeywordApplicable(const uint8_t* row)
    {
        const PlayerInfo p = Player();
        if (!p.present || !RealmAllowed(row, 0x24))
            return false;
        const uint32_t raceMask = Table::U32(row, 0x38);
        if (raceMask && !(raceMask & (1u << ((p.race - 1u) & 31))))
            return false;
        const uint32_t lo = Table::U32(row, 0x3C), hi = Table::U32(row, 0x40);
        return !(lo || hi) || ClassInMask(p.cls, lo, hi);
    }

    // ---- FUN_10219110: link every applicable keyword name inside `text` ----------------------------
    // Case-insensitive, whole-word (the delimiter set below, or any byte >= 0x80), never inside an
    // existing |H...|h...|h link, longest match wins, `exclude` (a keyword's own id) is never linked.
    // plain = true replaces with "name info index" instead of a hyperlink (used for text search).
    // The left-boundary test at position 0 reads the keyword's own first character, as the original's
    // does, so a keyword at the very start of a text is not linked.
    std::string FormatKeywords(std::string text, bool plain, uint32_t exclude)
    {
        if (text.empty())
            return text;
        static const std::string kDelims(" \n\r\t.,!?;:-()<>'\"");
        auto isDelim = [](char c) { return static_cast<signed char>(c) < 0 || kDelims.find(c) != std::string::npos; };
        const std::string lower = AscFilter::Lower(text);

        std::vector<std::pair<size_t, size_t>> links;
        for (size_t pos = 0;;)
        {
            const size_t h = lower.size() >= 2 ? lower.find("|H", pos) : std::string::npos;
            if (h == std::string::npos) break;
            const size_t m = lower.find("|h", h + 2);
            if (m == std::string::npos) break;
            const size_t e = lower.find("|h", m + 2);
            if (e == std::string::npos) break;
            links.emplace_back(h, e + 2);
            pos = e + 2;
        }

        struct Match { size_t start, end; uint32_t id; };
        std::vector<Match> matches;
        Table& kw = Keywords();
        for (uint32_t id = kw.MinId(); kw.Loaded() && id <= kw.MaxId(); ++id)
        {
            const uint8_t* row = kw.Row(id);
            if (id == exclude || !row || !KeywordApplicable(row))
                continue;
            const std::string name = AscFilter::Lower(kw.Str(row, 0x18));
            if (name.empty())
                continue;
            for (size_t from = 0; from <= lower.size();)
            {
                const size_t p = lower.find(name, from);
                if (p == std::string::npos)
                    break;
                const size_t end = p + name.size();
                from = end;
                const size_t left = p ? p - 1 : 0;
                if (!isDelim(lower[left]))
                    continue;
                if (end < lower.size() && !isDelim(lower[end]))
                    continue;
                matches.push_back({p, end, id});
            }
        }
        matches.erase(std::remove_if(matches.begin(), matches.end(), [&](const Match& m) {
                          for (auto& l : links)
                              if (l.first <= m.start && m.end <= l.second)
                                  return true;
                          return false;
                      }),
                      matches.end());
        std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
            return std::tie(a.start, a.end, a.id) < std::tie(b.start, b.end, b.id);
        });
        std::vector<Match> chosen;
        for (const Match& m : matches)
        {
            if (chosen.empty() || chosen.back().end <= m.start)
                chosen.push_back(m);
            else if (chosen.back().end - chosen.back().start < m.end - m.start)
                chosen.back() = m;
        }
        for (auto it = chosen.rbegin(); it != chosen.rend(); ++it)
        {
            const uint8_t* row = kw.Row(it->id);
            if (!row || !KeywordApplicable(row))
                continue;
            std::string rep;
            if (!plain)
            {
                const bool bracket = Table::U32(row, 4) != 0;
                char idBuf[16];
                sprintf_s(idBuf, "%u", it->id);
                rep = std::string("|c") + kw.Str(row, 8) + "|Hkeyword:" + idBuf + "|h" + (bracket ? "[" : "")
                    + kw.Str(row, 0x18) + (bracket ? "]" : "") + "|h|r";
            }
            else
            {
                rep = std::string(kw.Str(row, 0x18)) + " " + kw.Str(row, 0x1C) + " " + kw.Str(row, 0x20);
            }
            text.replace(it->start, it->end - it->start, rep);
        }
        return text;
    }

    // ---- the three containers ---------------------------------------------------------------------
    class TutorialContainer : public AscFilter::Container<const uint8_t*>
    {
    protected:
        std::vector<const uint8_t*> DoPopulate() override   // FUN_10a1cc10
        {
            std::vector<const uint8_t*> v;
            Table& t = Tutorials();
            for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
                if (const uint8_t* row = t.Row(id))
                    if (Applicability(row, true) == 0)
                        v.push_back(row);
            return v;
        }
        bool TokenMatch(char type, int32_t value, const uint8_t* const& e) override   // FUN_10a1cf40
        {
            return type != 'c' || Table::I32(e, 4) == value;
        }
        bool TextMatch(const std::string& lower, const uint8_t* const& e) override   // FUN_10a1cf70
        {
            Table& t = Tutorials();
            if (AscFilter::Lower(t.Str(e, 0x94)).find(lower) != std::string::npos)
                return true;
            if (AscFilter::Lower(FormatKeywords(t.Str(e, 0x98), true, 0)).find(lower) != std::string::npos)
                return true;
            if (const uint8_t* cat = Categories().Row(Table::U32(e, 4)))
                if (AscFilter::Lower(Categories().Str(cat, 4)).find(lower) != std::string::npos)
                    return true;
            Table& o = Objectives();
            for (uint32_t id = o.MinId(); o.Loaded() && id <= o.MaxId(); ++id)
            {
                const uint8_t* obj = o.Row(id);
                if (!obj || Table::U32(obj, 4) != Table::U32(e, 0))
                    continue;
                if (AscFilter::Lower(o.Str(obj, 0x1C)).find(lower) != std::string::npos)
                    return true;
                if (const uint8_t* type = ObjectiveTypes().Row(Table::U32(obj, 8)))
                    if (AscFilter::Lower(ObjectiveTypes().Str(type, 8)).find(lower) != std::string::npos)
                        return true;
            }
            return false;
        }
        std::string FilterKey(const uint8_t* const& e) override   // FUN_10a1dbb0
        {
            std::string k;
            AscFilter::AppendByte(k, Applicability(e, false) != 0 ? 1 : 0);
            AscFilter::AppendU32(k, Table::U32(e, 0x54));
            AscFilter::AppendU32(k, Table::U32(e, 0));
            return k;
        }
    };

    class KeywordContainer : public AscFilter::Container<const uint8_t*>
    {
    protected:
        std::vector<const uint8_t*> DoPopulate() override   // FUN_10a1ccf0
        {
            std::vector<const uint8_t*> v;
            Table& t = Keywords();
            for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
                if (const uint8_t* row = t.Row(id))
                    if (KeywordApplicable(row))
                        v.push_back(row);
            return v;
        }
        bool TextMatch(const std::string& lower, const uint8_t* const& e) override   // FUN_10a1d5f0
        {
            if (lower.empty())
                return true;
            Table& t = Keywords();
            return AscFilter::Lower(t.Str(e, 0x18)).find(lower) != std::string::npos
                || AscFilter::Lower(t.Str(e, 0x1C)).find(lower) != std::string::npos
                || AscFilter::Lower(t.Str(e, 0x20)).find(lower) != std::string::npos;
        }
        std::string PopulateKey(const uint8_t* const& e) override   // FUN_10a1d980
        {
            std::string k(32, '\0');
            const std::string name = AscFilter::Lower(Keywords().Str(e, 0x18));
            for (size_t i = 0; i < name.size() && i < 32; ++i)
                k[i] = name[i];
            AscFilter::AppendU32(k, Table::U32(e, 0));
            return k;
        }
    };

    class MentorContainer : public AscFilter::Container<const MentorRecord*>
    {
    protected:
        std::vector<const MentorRecord*> DoPopulate() override   // FUN_10a1c960
        {
            std::vector<const MentorRecord*> v;
            for (const MentorRecord& r : g_mentors)
                v.push_back(&r);
            return v;
        }
        bool TokenMatch(char type, int32_t value, const MentorRecord* const& e) override   // FUN_10a1cdd0
        {
            if (type != 's' || value == 0)
                return true;
            return std::find(e->specs.begin(), e->specs.end(), static_cast<uint32_t>(value)) != e->specs.end();
        }
        bool TextMatch(const std::string& lower, const MentorRecord* const& e) override   // FUN_10a1ce10
        {
            return AscFilter::Lower(e->name).find(lower) != std::string::npos;
        }
    };

    TutorialContainer g_tutorials;
    KeywordContainer g_keywords;
    MentorContainer g_mentorContainer;

    // ---- events ------------------------------------------------------------------------------------
    // FUN_10a1bfd0: tutorials that have become applicable since the last check -> TUTORIAL_ENTRY_ADDED.
    void RefreshAdded()
    {
        std::vector<uint32_t> nowApplicable;
        for (uint32_t id : g_notApplicable)
            if (const uint8_t* row = Tutorials().Row(id))
                if (Applicability(row, false) == 0)
                {
                    AscRuntime::Signal("TUTORIAL_ENTRY_ADDED", "%u%u", Table::U32(row, 0), Table::U32(row, 4));
                    nowApplicable.push_back(Table::U32(row, 0));
                }
        for (uint32_t id : nowApplicable)
            g_notApplicable.erase(id);
    }

    // handler_0x0258 (stock event 0x258): tutorials whose quest is now done -> TUTORIAL_ENTRY_COMPLETE,
    // and the tutorial list is refiltered with its current filter if any completed.
    void RefreshCompleted(lua_State*)
    {
        std::vector<uint32_t> done;
        for (uint32_t id : g_incomplete)
            if (const uint8_t* row = Tutorials().Row(id))
                if (TutorialComplete(Table::U32(row, 0)))
                    done.push_back(Table::U32(row, 0));
        if (done.empty())
            return;
        g_tutorials.Reapply();
        for (uint32_t id : done)
        {
            const uint8_t* row = Tutorials().Row(id);
            if (row && Table::U32(row, 0x88))
                AscRuntime::Signal("TUTORIAL_ENTRY_COMPLETE", "%u%u", Table::U32(row, 0), Table::U32(row, 4));
            g_incomplete.erase(id);
        }
    }

    void RefreshAddedOnEvent(lua_State*) { RefreshAdded(); }   // handler_0x01c0 (stock event 0x1C0)

    // 0x10a18e80: after 0x403340, a level gain re-runs the applicability watch.
    void LevelWatch()
    {
        const PlayerInfo p = Player();
        if (!p.present)
            return;
        if (g_lastLevel < p.level)
        {
            RefreshAdded();
            g_lastLevel = static_cast<uint8_t>(p.level);
        }
    }

    // FUN_10a18ed0 (after world entry): seed both watch sets.
    void SeedWatches()
    {
        g_notApplicable.clear();
        Table& t = Tutorials();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* row = t.Row(id))
                if (Applicability(row, false) != 0)
                    g_notApplicable.insert(id);
        g_incomplete.clear();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (!row || Applicability(row, false) != 0)
                continue;
            if (!TutorialComplete(Table::U32(row, 0)))
                g_incomplete.insert(id);
        }
    }

    // 0x10a16920 (glue screen)
    void ResetAll()
    {
        g_tutorials.Reset();
        g_rewarded.clear();
        g_lastLevel = 0;
        g_notApplicable.clear();
        g_incomplete.clear();
        g_activeSpecs.clear();
        g_pendingSpec = 0;
        g_mentors.clear();
        g_mentorContainer.Reset();
        g_mentorStatus = 0;
    }

    // ---- packet handlers --------------------------------------------------------------------------
    uint32_t ReadU32(CDataStore* p) { uint32_t v = *reinterpret_cast<const uint32_t*>(p->m_buffer + p->m_read); p->m_read += 4; return v; }
    uint8_t ReadU8(CDataStore* p)   { return static_cast<uint8_t>(p->m_buffer[p->m_read++]); }
    std::string ReadStr(CDataStore* p)
    {
        const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        std::string out(s);
        p->m_read += static_cast<int32_t>(out.size() + 1);
        return out;
    }

    void __cdecl OnClaimRewardsResult(void*, uint32_t, uint32_t, CDataStore* pkt)   // 0x6AF FUN_10a17910
    {
        AscRuntime::Signal("CLAIM_TUTORIAL_REWARDS_RESULT", "%s", ReadStr(pkt).c_str());
    }

    void __cdecl OnRewardedList(void*, uint32_t, uint32_t, CDataStore* pkt)   // 0x6B0 handler_0x06b0
    {
        g_rewarded.clear();
        for (uint32_t n = ReadU32(pkt); n; --n)
            g_rewarded.push_back(ReadU32(pkt));
    }

    void __cdecl OnRewarded(void*, uint32_t, uint32_t, CDataStore* pkt)   // 0x6B1 FUN_10a18aa0
    {
        const uint32_t id = ReadU32(pkt);
        g_rewarded.push_back(id);
        AscRuntime::Signal("TUTORIAL_ENTRY_UPDATE", "%u", id);
        RefreshAdded();
    }

    void __cdecl OnToggleMentorResult(void*, uint32_t, uint32_t, CDataStore* pkt)   // 0x6C4 FUN_10a18800
    {
        const std::string r = ReadStr(pkt);
        if (r == "ACTIVATE_MENTOR_STATUS_OK")
            g_mentorStatus = 1;
        else if (r == "DEACTIVATE_MENTOR_STATUS_OK")
            g_mentorStatus = 0;
        AscRuntime::Signal("TOGGLE_MENTOR_STATUS_RESULT", "%s", r.c_str());
    }

    void __cdecl OnAddSpecResult(void*, uint32_t, uint32_t, CDataStore* pkt)   // 0x6C7 FUN_10a17640
    {
        const std::string r = ReadStr(pkt);
        if (r == "ADD_MENTOR_SPECIALIZATION_OK")
            g_activeSpecs.push_back(g_pendingSpec);
        else if (r != "ADD_MENTOR_SPECIALIZATION_UNKNOWN")
            AscLog::Printf("Unexpected Value: %s", r.c_str());
        AscRuntime::Signal("ADD_MENTOR_SPECIALIZATION_RESULT", "%s", r.c_str());
    }

    void __cdecl OnRemoveSpecResult(void*, uint32_t, uint32_t, CDataStore* pkt)   // 0x6C9 FUN_10a184e0
    {
        const std::string r = ReadStr(pkt);
        if (r == "REMOVE_MENTOR_SPECIALIZATION_OK")
        {
            auto it = std::find(g_activeSpecs.begin(), g_activeSpecs.end(), g_pendingSpec);
            if (it != g_activeSpecs.end())
                g_activeSpecs.erase(it);
        }
        else if (r != "REMOVE_MENTOR_SPECIALIZATION_UNKNOWN")
            AscLog::Printf("Unexpected Value: %s", r.c_str());
        AscRuntime::Signal("REMOVE_MENTOR_SPECIALIZATION_RESULT", "%s", r.c_str());
    }

    // 0x6CA FUN_10a17a70 { u8 status, u32 n, u32 spec[n] } -- appends, as the original does.
    void __cdecl OnSpecList(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_mentorStatus = ReadU8(pkt);
        for (uint32_t n = ReadU32(pkt); n; --n)
            g_activeSpecs.push_back(ReadU32(pkt));
    }

    // 0x6CC FUN_10a17c00 { str result, [u32 n, n x { str name, u8, str, u32 k, u32 spec[k], str, str }] }
    void __cdecl OnWhoisResult(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        g_mentors.clear();
        const std::string r = ReadStr(pkt);
        if (r == "QUERY_MENTOR_WHOIS_OK")
        {
            for (uint32_t n = ReadU32(pkt); n; --n)
            {
                MentorRecord m;
                m.name = ReadStr(pkt);
                m.level = ReadU8(pkt);
                m.s1 = ReadStr(pkt);
                for (uint32_t k = ReadU32(pkt); k; --k)
                    m.specs.push_back(ReadU32(pkt));
                m.s2 = ReadStr(pkt);
                m.s3 = ReadStr(pkt);
                g_mentors.push_back(std::move(m));
            }
        }
        g_mentorContainer.Reset();
        AscRuntime::Signal("MENTOR_WHOIS_DATA_CONTAINER_RESET");
        AscRuntime::Signal("QUERY_MENTOR_WHOIS_RESULT", "%s", r.c_str());
    }

    // 0x6B2 handler_0x06b2 { u32 id, u32 a, u32 b, u32 c } -> TutorialRewards row upsert, then
    // TUTORIAL_ENTRY_UPDATE for field a (FUN_10a1c850).
    void __cdecl OnPatchRewards(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        std::vector<uint8_t> row(16);
        uint32_t f[4];
        for (uint32_t& v : f)
            v = ReadU32(pkt);
        memcpy(row.data(), f, 16);
        Rewards().Upsert(f[0], row);
        AscRuntime::Signal("TUTORIAL_ENTRY_UPDATE", "%u", f[1]);
    }

    // ---- bindings -----------------------------------------------------------------------------------
    // FUN_10a19040
    int PushTutorial(lua_State* L, const uint8_t* e)
    {
        if (!e)
            return PushNils(L, 8);
        Table& t = Tutorials();
        auto strOrNil = [&](uint32_t off) {
            const char* s = t.Str(e, off);
            if (*s) PushStr(L, s); else AscLua::lua_pushnil(L);
        };
        PushInt(L, Table::I32(e, 0));
        PushStr(L, t.Str(e, 0x94));
        if (Table::U32(e, 0xC)) PushInt(L, Table::I32(e, 0xC)); else AscLua::lua_pushnil(L);
        PushInt(L, Table::I32(e, 0x50));
        strOrNil(0x8C);
        if (Table::U32(e, 8)) PushInt(L, Table::I32(e, 8)); else AscLua::lua_pushnil(L);
        strOrNil(0x90);
        strOrNil(0x9C);
        return 8;
    }

    // FUN_10a19310
    int PushKeyword(lua_State* L, const uint8_t* e, bool forInfo)
    {
        if (!e)
            return PushNils(L, 4);
        Table& t = Keywords();
        const std::string text = FormatKeywords(t.Str(e, forInfo ? 0x1C : 0x20), false, Table::U32(e, 0));
        PushStr(L, t.Str(e, 0x18));
        PushStr(L, text.c_str());
        PushStr(L, t.Str(e, 0x10));
        PushStr(L, t.Str(e, 0x14));
        return 4;
    }

    int PushIntTable(lua_State* L, const std::vector<uint32_t>& v)   // FUN_1009a460
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushInt(L, static_cast<int32_t>(v[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // FUN_10a17510
    void CategoryProgress(uint32_t cat, int& unclaimed, int& claimed, int& total, std::vector<uint32_t>& ids)
    {
        unclaimed = claimed = total = 0;
        ids.clear();
        Table& t = Tutorials();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (!row || Table::U32(row, 4) != cat || Applicability(row, false) != 0)
                continue;
            if (Rewarded(Table::U32(row, 0)))
                ++claimed;
            else if (TutorialComplete(Table::U32(row, 0)))
            {
                ++unclaimed;
                ids.push_back(Table::U32(row, 0));
            }
            ++total;
        }
    }

    int GetNumTutorials(lua_State* L) { PushInt(L, static_cast<int32_t>(g_tutorials.Size())); return 1; }
    int GetNumKeywords(lua_State* L)  { PushInt(L, static_cast<int32_t>(g_keywords.Size())); return 1; }
    int GetNumAvailableMentors(lua_State* L) { PushInt(L, static_cast<int32_t>(g_mentorContainer.Size())); return 1; }
    int IsMentorStatusActive(lua_State* L) { PushBool(L, g_mentorStatus != 0); return 1; }

    int GetTutorialAtIndex(lua_State* L)
    {
        uint32_t i;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        return PushTutorial(L, i - 1 < g_tutorials.Size() ? g_tutorials.At(i - 1) : nullptr);
    }

    int GetTutorialByID(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        return PushTutorial(L, Tutorials().Row(id));
    }

    int GetTutorialDisplay(lua_State* L)   // FUN_10a1af90
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* e = Tutorials().Row(id);
        if (!e)
            return PushNils(L, 3);
        PushStr(L, Tutorials().Str(e, 0x94));
        PushStr(L, FormatKeywords(Tutorials().Str(e, 0x98), false, 0).c_str());
        if (Table::U32(e, 0xC)) PushInt(L, Table::I32(e, 0xC)); else AscLua::lua_pushnil(L);
        return 3;
    }

    int IsTutorialComplete(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        PushBool(L, TutorialComplete(id));
        return 1;
    }

    int IsTutorialAvailable(lua_State* L)   // FUN_10a1b420
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* e = Tutorials().Row(id);
        if (!e)
        {
            PushBool(L, false);
            return 1;
        }
        const uint32_t r = Applicability(e, false);
        PushBool(L, r == 0);
        if (r)
            PushStr(L, ApplicabilityName(r).c_str());
        else
            AscLua::lua_pushnil(L);
        return 2;
    }

    int DebugTutorialAppliccability(lua_State* L)   // FUN_10a19c50
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* e = Tutorials().Row(id);
        if (!e)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, ApplicabilityName(Applicability(e, true)).c_str());
        return 1;
    }

    int CanCollectReward(lua_State* L)   // handler_CanCollectReward
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        PushBool(L, Tutorials().Row(id) && TutorialComplete(id) && !Rewarded(id));
        return 1;
    }

    int IsRewardCollected(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        PushBool(L, Rewarded(id));
        return 1;
    }

    int CollectReward(lua_State* L)   // FUN_10a19b50: CMSG 0x6A8 { u32 }
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Packet(0x6A8).U32(id).Send();
        PushBool(L, true);
        return 1;
    }

    int GetCategoryProgress(lua_State* L)   // FUN_10a1a070
    {
        uint32_t cat;
        if (!ReadNumber(L, cat))
            return 0;
        int unclaimed, claimed, total;
        std::vector<uint32_t> ids;
        CategoryProgress(cat, unclaimed, claimed, total, ids);
        PushInt(L, unclaimed + claimed);
        PushInt(L, claimed);
        PushInt(L, total);
        return 3;
    }

    int IsCategoryEnabled(lua_State* L)   // FUN_10a1b270
    {
        uint32_t cat;
        if (!ReadNumber(L, cat))
            return 0;
        const PlayerInfo p = Player();
        if (!p.present || p.level < 8)
        {
            PushBool(L, false);
            return 1;
        }
        int unclaimed, claimed, total;
        std::vector<uint32_t> ids;
        CategoryProgress(cat, unclaimed, claimed, total, ids);
        PushBool(L, total != 0);
        return 1;
    }

    int AnyUnclaimedRewards(lua_State* L)   // FUN_10a19680
    {
        const uint8_t* cvar = reinterpret_cast<const uint8_t*(__cdecl*)(const char*)>(0x767440)("showTutorials");
        if (!cvar || *reinterpret_cast<const int32_t*>(cvar + 0x30) != 0)
        {
            Table& c = Categories();
            for (uint32_t cat = c.MinId(); c.Loaded() && cat <= c.MaxId(); ++cat)
            {
                if (!c.Row(cat))
                    continue;
                int unclaimed, claimed, total;
                std::vector<uint32_t> ids;
                CategoryProgress(cat, unclaimed, claimed, total, ids);
                if (unclaimed)
                {
                    PushBool(L, true);
                    PushInt(L, static_cast<int32_t>(cat));
                    return 1 + 1 + PushIntTable(L, ids);
                }
            }
        }
        PushBool(L, false);
        return 1 + PushNils(L, 2);
    }

    int GetTutorialsRequiredForMentorStatus(lua_State* L)   // FUN_10a1b150
    {
        std::vector<uint32_t> ids;
        Table& t = Tutorials();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* row = t.Row(id))
                if (Table::U32(row, 0x88) && !Rewarded(Table::U32(row, 0)))
                    ids.push_back(Table::U32(row, 0));
        return PushIntTable(L, ids);
    }

    int StartQuest(lua_State* L)   // FUN_10a1bd00: CMSG_QUESTGIVER_ACCEPT_QUEST with no giver
    {
        uint32_t quest;
        if (!ReadNumber(L, quest))
            return 0;
        Table& t = Tutorials();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (row && Table::U32(row, 0xC) == quest)
            {
                Packet(0x189).U64(0).U32(quest).U32(0).Send();
                PushBool(L, true);
                return 1;
            }
        }
        PushBool(L, false);
        return 1;
    }

    int GetObjectives(lua_State* L)   // FUN_10a1aac0
    {
        uint32_t tut;
        if (!ReadNumber(L, tut))
            return 0;
        std::vector<uint32_t> ids;
        Table& o = Objectives();
        for (uint32_t id = o.MinId(); o.Loaded() && id <= o.MaxId(); ++id)
        {
            const uint8_t* obj = o.Row(id);
            if (!obj || Table::U32(obj, 4) != tut)
                continue;
            const uint8_t* row = Tutorials().Row(Table::U32(obj, 4));   // FUN_10219ea0
            if (row && Applicability(row, false) != 0)
                continue;
            ids.push_back(id);
        }
        std::stable_sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
            return Table::U32(o.Row(a), 0x18) < Table::U32(o.Row(b), 0x18);
        });
        return PushIntTable(L, ids);
    }

    int GetObjectiveInfo(lua_State* L)   // FUN_10a1a970
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* obj = Objectives().Row(id);
        if (!obj)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, FormatKeywords(Objectives().Str(obj, 0x1C), false, 0).c_str());
        return 1;
    }

    int SetTutorialFilter(lua_State* L)   // FUN_10a1bb50 -> "$c<category> <text>"
    {
        if (!ValidateInput(L, {NUMBER, STRING}))
            return 0;
        char buf[16];
        sprintf_s(buf, "$c%d ", ToInt(CheckNumber(L, 1)));
        g_tutorials.ApplyFilter(buf + std::string(CheckString(L, 2)));
        return 0;
    }

    int SetKeywordFilter(lua_State* L)   // FUN_10a1b9c0
    {
        std::string text;
        if (ReadString(L, text))
            g_keywords.ApplyFilter(text);
        return 0;
    }

    int GetKeywordAtIndex(lua_State* L)   // handler_GetKeywordAtIndex
    {
        uint32_t i;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        return PushKeyword(L, i - 1 < g_keywords.Size() ? g_keywords.At(i - 1) : nullptr, false);
    }

    int GetKeywordInfo(lua_State* L)   // handler_GetKeywordInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        return PushKeyword(L, Keywords().Row(id), true);
    }

    int GetIndexByKeywordID(lua_State* L)   // handler_GetIndexByKeywordID
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        for (size_t i = 0; i < g_keywords.Size(); ++i)
            if (Table::U32(g_keywords.At(i), 0) == id)
            {
                PushInt(L, static_cast<int32_t>(i + 1));
                return 1;
            }
        AscLua::lua_pushnil(L);
        return 1;
    }

    int GetKeywordID(lua_State* L)   // FUN_10a1a290: exact match on the key column
    {
        std::string key;
        if (!ReadString(L, key))
            return 0;
        Table& t = Keywords();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (row && KeywordApplicable(row) && key == t.Str(row, 0xC))
            {
                PushInt(L, Table::I32(row, 0));
                return 1;
            }
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- mentors --------------------------------------------------------------------------------------
    // FUN_10a16870: 0 ok, 2 not in world, 3 already active, 4 required tutorials not rewarded.
    uint32_t CanActivateMentor()
    {
        if (!ActivePlayer())
            return 2;
        if (g_mentorStatus)
            return 3;
        Table& t = Tutorials();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (const uint8_t* row = t.Row(id))
                if (Table::U32(row, 0x88) && !Rewarded(Table::U32(row, 0)))
                    return 4;
        return 0;
    }

    int CanToggleMentorStatus(lua_State* L)   // FUN_10a19910
    {
        bool activate;
        if (!ReadBool(L, activate))
            return 0;
        static const char* const kActivate[] = {
            "ACTIVATE_MENTOR_STATUS_OK", "ACTIVATE_MENTOR_STATUS_UNKNOWN", "ACTIVATE_MENTOR_STATUS_NOT_IN_WORLD",
            "ACTIVATE_MENTOR_STATUS_ALREADY_ACTIVE", "ACTIVATE_MENTOR_STATUS_TUTORIALS_NOT_COMPLETED"};
        static const char* const kDeactivate[] = {
            "DEACTIVATE_MENTOR_STATUS_OK", "DEACTIVATE_MENTOR_STATUS_UNKNOWN",
            "DEACTIVATE_MENTOR_STATUS_NOT_IN_WORLD", "DEACTIVATE_MENTOR_STATUS_NOT_ACTIVE"};
        std::string reason;
        if (activate)
        {
            const uint32_t r = CanActivateMentor();
            if (r)
                reason = r < 5 ? kActivate[r] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(r);
        }
        else
        {
            const uint32_t r = !ActivePlayer() ? 2 : (g_mentorStatus ? 0 : 3);
            if (r)
                reason = kDeactivate[r];
        }
        PushBool(L, reason.empty());
        if (reason.empty())
            AscLua::lua_pushnil(L);
        else
            PushStr(L, reason.c_str());
        return 2;
    }

    int ToggleMentorStatus(lua_State* L)   // FUN_10a1be90: CMSG 0x6C3 { u8 activate }
    {
        bool activate;
        if (!ReadBool(L, activate))
            return 0;
        const bool ok = activate ? CanActivateMentor() == 0 : (ActivePlayer() && g_mentorStatus);
        if (ok)
            Packet(0x6C3).U8(activate ? 1 : 0).Send();
        PushBool(L, ok);
        return 1;
    }

    int AddMentorSpecialization(lua_State* L)   // FUN_10a19590: CMSG 0x6C6 { u32 }
    {
        uint32_t id;
        if (ReadNumber(L, id))
        {
            Packet(0x6C6).U32(id).Send();
            g_pendingSpec = id;
        }
        return 0;
    }

    int RemoveMentorSpecialization(lua_State* L)   // FUN_10a1b710: CMSG 0x6C8 { u32 }
    {
        uint32_t id;
        if (ReadNumber(L, id))
        {
            Packet(0x6C8).U32(id).Send();
            g_pendingSpec = id;
        }
        return 0;
    }

    int QueryActiveMentors(lua_State* L)   // FUN_10a1b650: empty CMSG 0x6CB
    {
        Packet(0x6CB).Send();
        PushBool(L, true);
        return 1;
    }

    int GetMentorSpecializationActive(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        PushBool(L, std::find(g_activeSpecs.begin(), g_activeSpecs.end(), id) != g_activeSpecs.end());
        return 1;
    }

    int GetMentorSpecializations(lua_State* L)   // FUN_10a1a6c0
    {
        std::vector<uint32_t> ids;
        Table& t = MentorSpecs();
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (t.Row(id))
                ids.push_back(id);
        return PushIntTable(L, ids);
    }

    int GetMentorSpecializationInfo(lua_State* L)   // handler_GetMentorSpecializationInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = MentorSpecs().Row(id);
        if (!row)
            return PushNils(L, 2);
        PushStr(L, MentorSpecs().Str(row, 8));
        const char* name = MentorSpecs().Str(row, 4);
        if (*name) PushStr(L, name); else AscLua::lua_pushnil(L);
        return 2;
    }

    int GetAvailableMentorAtIndex(lua_State* L)   // handler_GetAvailableMentorAtIndex
    {
        uint32_t i;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        const MentorRecord* m = i - 1 < g_mentorContainer.Size() ? g_mentorContainer.At(i - 1) : nullptr;
        if (!m)
            return PushNils(L, 6);
        PushStr(L, m->name.c_str());
        PushInt(L, m->level);
        PushStr(L, m->s1.c_str());
        PushIntTable(L, m->specs);
        PushStr(L, m->s2.c_str());
        PushStr(L, m->s3.c_str());
        return 6;
    }

    int SetAvailableMentorFilter(lua_State* L)   // FUN_10a1b800 -> "$s<spec> <text>"
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string text = CheckString(L, 1);
        char buf[16];
        sprintf_s(buf, "$s%d ", ToInt(CheckNumber(L, 2)));
        g_mentorContainer.ApplyFilter(buf + text);
        return 0;
    }

    // SMSG_PATCH_TUTORIAL (0x6A5, 0x101e4ea0): 0xB4 struct, strings in fields 35..39; then the
    // tutorial container is reset (FUN_10a19510) and TUTORIAL_ENTRY_UPDATE fires for the id.
    void AfterTutorialPatch(const uint8_t* row)
    {
        g_tutorials.Reset();
        AscRuntime::Signal("TUTORIAL_CONTAINER_RESET");
        AscRuntime::Signal("TUTORIAL_ENTRY_UPDATE", "%u", row ? Table::U32(row, 0) : 0);
    }
    // SMSG_PATCH_TUTORIAL_OBJECTIVES (0x6A7, 0x101e4cf0): TUTORIAL_ENTRY_UPDATE for the tutorial (+4).
    void AfterObjectivePatch(const uint8_t* row)
    {
        AscRuntime::Signal("TUTORIAL_ENTRY_UPDATE", "%u", row ? Table::U32(row, 4) : 0);
    }
    // SMSG_PATCH_TUTORIAL_KEYWORDS (0x6AC, 0x101e44c0): keyword container reset (FUN_10a19480), then
    // TUTORIAL_ENTRY_UPDATE with 0.
    void AfterKeywordPatch(const uint8_t*)
    {
        g_keywords.Reset();
        AscRuntime::Signal("TUTORIAL_KEYWORD_CONTAINER_RESET");
        AscRuntime::Signal("TUTORIAL_ENTRY_UPDATE", "%u", 0u);
    }

    void Init()
    {
        AscDbcPatch::Register(0x6A5, "DBFilesClient\\Tutorial.dbc", 0xB4, 0x1Full << 35, AfterTutorialPatch);
        AscDbcPatch::Register(0x6A7, "DBFilesClient\\TutorialObjectives.dbc", 0x20, 1ull << 7, AfterObjectivePatch);
        AscDbcPatch::Register(0x6AC, "DBFilesClient\\TutorialKeywords.dbc", 0x44, 0x1FCull, AfterKeywordPatch);
        AscRuntime::OnGlueScreen(ResetAll);
        AscRuntime::OnAfterEnterWorld(SeedWatches);
        AscRuntime::OnAfter403340(LevelWatch);
        AscRuntime::OnStockEvent(0x258, RefreshCompleted);
        AscRuntime::OnStockEvent(0x1C0, RefreshAddedOnEvent);
        sDC.AddPacketHandler(0x6AF, CNetClientCustomPacket((void*)&OnClaimRewardsResult, nullptr));
        sDC.AddPacketHandler(0x6B0, CNetClientCustomPacket((void*)&OnRewardedList, nullptr));
        sDC.AddPacketHandler(0x6B1, CNetClientCustomPacket((void*)&OnRewarded, nullptr));
        sDC.AddPacketHandler(0x6B2, CNetClientCustomPacket((void*)&OnPatchRewards, nullptr));
        sDC.AddPacketHandler(0x6C4, CNetClientCustomPacket((void*)&OnToggleMentorResult, nullptr));
        sDC.AddPacketHandler(0x6C7, CNetClientCustomPacket((void*)&OnAddSpecResult, nullptr));
        sDC.AddPacketHandler(0x6C9, CNetClientCustomPacket((void*)&OnRemoveSpecResult, nullptr));
        sDC.AddPacketHandler(0x6CA, CNetClientCustomPacket((void*)&OnSpecList, nullptr));
        sDC.AddPacketHandler(0x6CC, CNetClientCustomPacket((void*)&OnWhoisResult, nullptr));
    }

    // GetCategories() (0x10a19e50): every TutorialCategories id, in id order.
    int GetCategories(lua_State* L)
    {
        Table& t = Categories();
        std::vector<uint32_t> ids;
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
            if (t.Row(id))
                ids.push_back(id);
        AscLua::lua_createtable(L, 0, static_cast<int>(ids.size()));   // FUN_1009a460
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < ids.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushInt(L, static_cast<int32_t>(ids[i]));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // GetCategoryInfo(id) (0x10a17490): the category's +4 name, else nil.
    int GetCategoryInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Table& t = Categories();
        if (const uint8_t* row = t.Row(id))
            PushStr(L, t.Str(row, 4));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // GetRewards(id) (0x10a1ac20): {item = count} summed over the tutorial's TutorialRewards rows (+4
    // tutorial, +8 item, +0xC count; zero item or count skipped); nil for an unknown tutorial or none.
    int GetRewards(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* row = Tutorials().Row(id);
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        std::unordered_map<uint32_t, uint32_t> items;
        Table& r = Rewards();
        for (uint32_t rid = r.MinId(); r.Loaded() && rid <= r.MaxId(); ++rid)
        {
            const uint8_t* rr = r.Row(rid);
            if (!rr || r.U32(rr, 4) != Table::U32(row, 0) || !r.U32(rr, 8) || !r.U32(rr, 0xC))
                continue;
            items[r.U32(rr, 8)] += r.U32(rr, 0xC);
        }
        if (items.empty())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscLua::lua_createtable(L, 0, static_cast<int>(items.size()));
        AscLua::lua_checkstack(L, 2);
        for (const auto& e : items)
        {
            PushInt(L, static_cast<int32_t>(e.first));
            PushInt(L, static_cast<int32_t>(e.second));
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    // IsTutorialRequiredForMentorStatus(id) (0x10a1b5d0): Tutorial +0x88 != 0, else nil.
    int IsTutorialRequiredForMentorStatus(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        Table& t = Tutorials();
        if (const uint8_t* row = t.Row(id))
            PushBool(L, t.U32(row, 0x88) != 0);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Tutorial", "GetCategories", GetCategories},
        {"C_Tutorial", "GetCategoryInfo", GetCategoryInfo},
        {"C_Tutorial", "GetRewards", GetRewards},
        {"C_Tutorial", "IsTutorialRequiredForMentorStatus", IsTutorialRequiredForMentorStatus},
        {"C_Tutorial", "AddMentorSpecialization", AddMentorSpecialization},
        {"C_Tutorial", "AnyUnclaimedRewards", AnyUnclaimedRewards},
        {"C_Tutorial", "CanCollectReward", CanCollectReward},
        {"C_Tutorial", "CanToggleMentorStatus", CanToggleMentorStatus},
        {"C_Tutorial", "CollectReward", CollectReward},
        {"C_Tutorial", "DebugTutorialAppliccability", DebugTutorialAppliccability},
        {"C_Tutorial", "GetAvailableMentorAtIndex", GetAvailableMentorAtIndex},
        {"C_Tutorial", "GetCategoryProgress", GetCategoryProgress},
        {"C_Tutorial", "GetIndexByKeywordID", GetIndexByKeywordID},
        {"C_Tutorial", "GetKeywordAtIndex", GetKeywordAtIndex},
        {"C_Tutorial", "GetKeywordID", GetKeywordID},
        {"C_Tutorial", "GetKeywordInfo", GetKeywordInfo},
        {"C_Tutorial", "GetMentorSpecializationActive", GetMentorSpecializationActive},
        {"C_Tutorial", "GetMentorSpecializationInfo", GetMentorSpecializationInfo},
        {"C_Tutorial", "GetMentorSpecializations", GetMentorSpecializations},
        {"C_Tutorial", "GetNumAvailableMentors", GetNumAvailableMentors},
        {"C_Tutorial", "GetNumKeywords", GetNumKeywords},
        {"C_Tutorial", "GetNumTutorials", GetNumTutorials},
        {"C_Tutorial", "GetObjectiveInfo", GetObjectiveInfo},
        {"C_Tutorial", "GetObjectives", GetObjectives},
        {"C_Tutorial", "GetTutorialAtIndex", GetTutorialAtIndex},
        {"C_Tutorial", "GetTutorialByID", GetTutorialByID},
        {"C_Tutorial", "GetTutorialDisplay", GetTutorialDisplay},
        {"C_Tutorial", "GetTutorialsRequiredForMentorStatus", GetTutorialsRequiredForMentorStatus},
        {"C_Tutorial", "IsCategoryEnabled", IsCategoryEnabled},
        {"C_Tutorial", "IsMentorStatusActive", IsMentorStatusActive},
        {"C_Tutorial", "IsRewardCollected", IsRewardCollected},
        {"C_Tutorial", "IsTutorialAvailable", IsTutorialAvailable},
        {"C_Tutorial", "IsTutorialComplete", IsTutorialComplete},
        {"C_Tutorial", "QueryActiveMentors", QueryActiveMentors},
        {"C_Tutorial", "RemoveMentorSpecialization", RemoveMentorSpecialization},
        {"C_Tutorial", "SetAvailableMentorFilter", SetAvailableMentorFilter},
        {"C_Tutorial", "SetKeywordFilter", SetKeywordFilter},
        {"C_Tutorial", "SetTutorialFilter", SetTutorialFilter},
        {"C_Tutorial", "StartQuest", StartQuest},
        {"C_Tutorial", "ToggleMentorStatus", ToggleMentorStatus},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
