// C_TrialCreator -- player-made "trials" (bundles of challenge levels), the original's
// ChallengeCreatorMgr (accessor FUN_10124310 -> 0x10BDD210, ctor FUN_10120690, module init
// FUN_10124200: 7 events, 8 packet handlers, glue reset FUN_10119df0).
//
//   +0x00 entries (0xA4 each): +0 id, +0x18 owned, +0x1C author, +0x34 name, +0x4C description,
//         +0x64 icon, +0x7C challenges {u32 challenge, u32 level, str description} (0x20 each),
//         +0x88 upvoted, +0x89 downvoted, +0x8C upvote times, +0x98 downvote times (u64 each)
//   +0x0C the active trial's id (string)
//   +0x24 trial id -> completions (0x88 each: trial, player, u64 start, u64 complete, race, gender,
//         class), fastest first
//   +0x44 ChallengeCreatorEntryContainer (vtable 0x10B312D0)
// Strings on the wire are u32 length + bytes (FUN_100d3680).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscChallenge.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscTrialCreator.hpp>
#include <Client/CDataStore.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
#include <Ascension/AscCAEnums.generated.inc>

    struct TrialChallenge { uint32_t challenge = 0, level = 0; std::string description; };
    struct Trial
    {
        std::string id;
        bool owned = false;
        std::string author, name, description, icon;
        std::vector<TrialChallenge> challenges;
        bool upvoted = false, downvoted = false;
        std::vector<uint64_t> upvotes, downvotes;
        int32_t Score() const { return static_cast<int32_t>(upvotes.size()) - static_cast<int32_t>(downvotes.size()); }
    };
    struct Completion
    {
        std::string trial, player;
        uint64_t start = 0, complete = 0;
        std::string race, gender, cls;
    };

    std::vector<Trial> g_trials;                                           // +0x00
    std::string g_active;                                                  // +0x0C
    std::unordered_map<std::string, std::vector<Completion>> g_completions; // +0x24

    const Trial* Find(const std::string& id)   // FUN_10122420
    {
        for (const Trial& t : g_trials)
            if (t.id == id)
                return &t;
        return nullptr;
    }
    Trial* FindMutable(const std::string& id) { return const_cast<Trial*>(Find(id)); }

    std::vector<std::pair<uint32_t, uint32_t>> Levels(const Trial& t)
    {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (const TrialChallenge& c : t.challenges)
            v.emplace_back(c.challenge, c.level);
        return v;
    }

    // FUN_10121d00: why the trial cannot be activated (ACTIVATE_TRIAL_*).
    bool CanActivate(const std::string& id, std::vector<std::string>& errors)
    {
        if (!g_active.empty())
            errors.push_back("ACTIVATE_TRIAL_ALREADY_ACTIVE");
        const Trial* t = Find(id);
        if (!t)
            errors.push_back("ACTIVATE_TRIAL_NOT_FOUND");
        else
        {
            // Each level must be startable (ignoring "not a trial", 0x11) and every running challenge
            // stoppable.
            bool failed = false;
            for (const TrialChallenge& c : t->challenges)
                if (!AscChallenge::CanActivate(c.challenge, c.level, {0x11}))
                {
                    errors.push_back("ACTIVATE_TRIAL_CANNOT_ACTIVATE_CHALLENGE");
                    failed = true;
                    break;
                }
            if (!failed)
                for (uint32_t active : AscChallenge::ActiveIds())
                    if (!AscChallenge::CanDeactivate(active, {0x11}))
                    {
                        errors.push_back("ACTIVATE_TRIAL_CANNOT_ACTIVATE_CHALLENGE");
                        break;
                    }
        }
        return errors.empty();
    }

    // FUN_10125e50's checks: why the active trial cannot be deactivated (DEACTIVATE_TRIAL_*).
    bool CanDeactivate(std::vector<std::string>& errors)
    {
        if (g_active.empty())
            errors.push_back("DEACTIVATE_TRIAL_NO_ACTIVE_CHALLENGE");
        const Trial* t = Find(g_active);
        if (!t)
            errors.push_back("DEACTIVATE_TRIAL_NOT_FOUND");
        else
            for (const TrialChallenge& c : t->challenges)
                if (!AscChallenge::CanDeactivate(c.challenge, {1, 4}))
                {
                    errors.push_back("DEACTIVATE_TRIAL_CANNOT_DEACTIVATE_CHALLENGE");
                    break;
                }
        return errors.empty();
    }

    // FUN_1011f170 (via FUN_1011cb30): the active trial first, then higher score.
    bool Before(const Trial& a, const Trial& b)
    {
        const bool aa = a.id == g_active, ab = b.id == g_active;
        if (aa != ab)
            return aa;
        return b.Score() < a.Score();
    }

    // ---- ChallengeCreatorEntryContainer (vtable 0x10B312D0) ----------------------------------------
    class Container : public AscFilter::Container<const Trial*>
    {
    protected:
        std::vector<const Trial*> DoPopulate() override   // FUN_10128950
        {
            std::vector<const Trial*> v;
            for (const Trial& t : g_trials)
                v.push_back(&t);
            return v;
        }
        bool ArgMatch(uint32_t filter, const Trial* const& t) override   // FUN_10128cf0
        {
            if (t->id == g_active)
                return true;
            auto any = [&](auto pred) {
                for (const TrialChallenge& c : t->challenges)
                    if (const uint8_t* r = AscChallenge::ChallengeRowOf(c.challenge))
                        if (pred(r, c))
                            return true;
                return false;
            };
            auto u32 = [](const uint8_t* r, uint32_t off) { uint32_t v; memcpy(&v, r + off, 4); return v; };
            switch (filter)
            {
                case 1: return any([&](const uint8_t* r, const TrialChallenge&) { return u32(r, 0x38) != 0; });
                case 2: return any([&](const uint8_t* r, const TrialChallenge&) { return u32(r, 0x38) == 0; });
                case 3:
                {
                    std::vector<std::string> e;
                    return CanActivate(t->id, e);
                }
                case 4: return any([&](const uint8_t* r, const TrialChallenge&) { return u32(r, 4) != 0; });
                case 5: return any([&](const uint8_t* r, const TrialChallenge&) { return u32(r, 8) != 0; });
                case 6:
                    for (const TrialChallenge& c : t->challenges)
                        if (AscChallenge::HasSoloRule(c.challenge, c.level))
                            return true;
                    return false;
                case 7:
                    for (const TrialChallenge& c : t->challenges)
                        if (AscChallenge::HasSoloRule(c.challenge, c.level))
                            return false;
                    return true;
                case 8: case 9: case 10: case 11: case 12:
                    return any([&](const uint8_t* r, const TrialChallenge&) {
                        return AscChallenge::Difficulty(r) == static_cast<int>(filter - 8);
                    });
                default: return false;
            }
        }
        uint32_t ArgGroup(uint32_t filter) override   // FUN_10129370
        {
            switch (filter)
            {
                case 1: case 2: return 1;
                case 4: case 5: return 2;
                case 6: case 7: return 3;
                case 8: case 9: case 10: case 11: case 12: return 4;
                default: return 0;
            }
        }
        bool TextMatch(const std::string& text, const Trial* const& t) override   // FUN_10128b80
        {
            if (text.empty())
                return true;
            if (AscFilter::Lower(t->name).find(text) != std::string::npos)
                return true;
            return AscFilter::Lower(t->author).find(text) != std::string::npos;
        }
    };
    Container& Entries() { static Container c; return c; }

    // ---- Lua -----------------------------------------------------------------------------------------
    void SetStr(lua_State* L, const char* k, const std::string& v) { AscLua::lua_pushstring(L, k); PushStr(L, v.c_str()); AscLua::lua_settable(L, -3); }
    void SetBool(lua_State* L, const char* k, bool v) { AscLua::lua_pushstring(L, k); PushBool(L, v); AscLua::lua_settable(L, -3); }
    void SetInt(lua_State* L, const char* k, int32_t v) { AscLua::lua_pushstring(L, k); PushInt(L, v); AscLua::lua_settable(L, -3); }
    void SetNum(lua_State* L, const char* k, double v) { AscLua::lua_pushstring(L, k); PushNum(L, v); AscLua::lua_settable(L, -3); }
    void NewTable(lua_State* L) { AscLua::lua_createtable(L, 0, 0); AscLua::lua_checkstack(L, 2); }

    // FUN_101243b0: the trial's fields into the table on top.
    void PushTrial(lua_State* L, const Trial& t, bool sortConditions)
    {
        SetStr(L, "ID", t.id);
        SetBool(L, "Owned", t.owned);
        SetStr(L, "Author", t.author);
        SetStr(L, "Name", t.name);
        SetStr(L, "Description", "Created By: " + t.author + "\n" + t.description);
        SetStr(L, "Icon", t.icon);
        AscLua::lua_pushstring(L, "Challenges");
        NewTable(L);
        for (size_t i = 0; i < t.challenges.size(); ++i)
        {
            PushInt(L, static_cast<int32_t>(i + 1));
            NewTable(L);
            SetInt(L, "ChallengeId", static_cast<int32_t>(t.challenges[i].challenge));
            SetInt(L, "Level", static_cast<int32_t>(t.challenges[i].level));
            SetStr(L, "Description", t.challenges[i].description);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        AscChallenge::PushMergedLevels(L, Levels(t), sortConditions);
        SetBool(L, "Upvoted", t.upvoted);
        SetBool(L, "Downvoted", t.downvoted);
        SetInt(L, "Score", t.Score());
    }

    void PushCompletion(lua_State* L, const Completion& c)   // FUN_10125630
    {
        NewTable(L);
        SetStr(L, "Challenge", c.trial);
        SetStr(L, "PlayerName", c.player);
        SetNum(L, "StartTime", static_cast<int32_t>(c.start));
        SetNum(L, "CompleteTime", static_cast<int32_t>(c.complete));
        SetStr(L, "PlayerRace", c.race);
        SetStr(L, "PlayerGender", c.gender);
        SetStr(L, "PlayerClass", c.cls);
    }

    void PushStrings(lua_State* L, const std::vector<std::string>& v)   // FUN_100beb90
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, v[i].c_str());
            AscLua::lua_settable(L, -3);
        }
    }

    void SendId(uint32_t opcode, const std::string& id)
    {
        Packet p(opcode);
        p.U32(static_cast<uint32_t>(id.size()));
        if (!id.empty())
            p.Data(id.data(), static_cast<uint32_t>(id.size()));
        p.Send();
    }
    void Query() { Packet(0x5AB).Send(); }   // FUN_101257a0

    // FrameScript::lua_toarray over the table at `idx` (numbers / strings; anything else empties it).
    bool NumberArray(lua_State* L, int idx, std::vector<uint32_t>& out)   // FUN_1011f230
    {
        out.clear();
        if (AscLua::lua_type(L, idx) != TABLE)
        {
            AscLog::Printf("FrameScript::lua_toarray Value at index %d is not a table", idx);
            return false;
        }
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, idx))
        {
            if (!AscLua::lua_isnumber(L, -1))
            {
                AscLua::lua_settop(L, -3);
                AscLog::Printf("FrameScript::lua_toarray Invalid lua value type");
                out.clear();
                return false;
            }
            out.push_back(static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, -1))));
            AscLua::lua_settop(L, -2);
        }
        return true;
    }
    bool StringArray(lua_State* L, int idx, std::vector<std::string>& out)   // FUN_1011f710
    {
        out.clear();
        if (AscLua::lua_type(L, idx) != TABLE)
        {
            AscLog::Printf("FrameScript::lua_toarray Value at index %d is not a table", idx);
            return false;
        }
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, idx))
        {
            if (!AscLua::lua_isstring(L, -1))
            {
                AscLua::lua_settop(L, -3);
                AscLog::Printf("FrameScript::lua_toarray Invalid lua value type");
                out.clear();
                return false;
            }
            out.push_back(AscLua::lua_tolstring(L, -1, nullptr));
            AscLua::lua_settop(L, -2);
        }
        return true;
    }

    struct SaveArgs
    {
        std::string id, name, description, icon;
        std::vector<uint32_t> challenges, levels;
        std::vector<std::string> descriptions;
    };
    bool ReadSave(lua_State* L, SaveArgs& a)   // FUN_10119e50 (string x4, table x3)
    {
        if (!ValidateInput(L, {STRING, STRING, STRING, STRING, TABLE, TABLE, TABLE}))
            return false;
        a.id = CheckString(L, 1);
        a.name = CheckString(L, 2);
        a.description = CheckString(L, 3);
        a.icon = CheckString(L, 4);
        NumberArray(L, 5, a.challenges);
        NumberArray(L, 6, a.levels);
        StringArray(L, 7, a.descriptions);
        return true;
    }

    bool IconExists(const std::string& icon)   // SpellIcon (client container 0xAD488C, +4 path)
    {
        const uint32_t minId = *reinterpret_cast<const uint32_t*>(0xAD489C);
        const uint32_t maxId = *reinterpret_cast<const uint32_t*>(0xAD4898);
        for (uint32_t id = minId; id < maxId + 1; ++id)
            if (const uint8_t* r = ClientDbcRow(0xAD488C, id))
            {
                const char* path = *reinterpret_cast<const char* const*>(r + 4);
                if (path && icon == path)
                    return true;
            }
        return false;
    }

    std::string TrialName(std::string s)   // "CHALLENGE" -> "TRIAL", once (FUN_100ee5d0)
    {
        if (s.size() > 8)
        {
            const size_t at = s.find("CHALLENGE");
            if (at != std::string::npos)
                s.replace(at, 9, "TRIAL");
        }
        return s;
    }

    // ---- bindings ------------------------------------------------------------------------------------
    int SaveTrial(lua_State* L)   // FUN_101280e0
    {
        SaveArgs a;
        if (!ReadSave(L, a))
            return 0;
        if (a.challenges.size() != a.levels.size())
        {
            PushBool(L, false);
            return 1;
        }
        Packet p(0x5A7);
        for (const std::string* s : {&a.id, &a.name, &a.description, &a.icon})
        {
            p.U32(static_cast<uint32_t>(s->size()));
            if (!s->empty())
                p.Data(s->data(), static_cast<uint32_t>(s->size()));
        }
        p.U32(static_cast<uint32_t>(a.challenges.size()));
        for (size_t i = 0; i < a.challenges.size(); ++i)
        {
            p.U32(a.challenges[i]);
            p.U32(a.levels[i]);
            const std::string d = i < a.descriptions.size() ? a.descriptions[i] : std::string();
            p.U32(static_cast<uint32_t>(d.size()));
            if (!d.empty())
                p.Data(d.data(), static_cast<uint32_t>(d.size()));
        }
        p.Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_101263f0: (true, {SAVE_TRIAL_*} or nil); false, nil when the two id tables differ in length.
    int CanSaveTrial(lua_State* L)
    {
        SaveArgs a;
        if (!ReadSave(L, a))
            return 0;
        if (a.challenges.size() != a.levels.size())
        {
            PushBool(L, false);
            AscLua::lua_pushnil(L);
            return 2;
        }
        std::vector<uint32_t> codes;
        if (!a.id.empty())
        {
            const Trial* t = Find(a.id);
            if (!t)
                codes.push_back(1);
            else
            {
                if (!t->owned)
                    codes.push_back(2);
                std::vector<uint32_t> ids = a.challenges, levels = a.levels, oldIds, oldLevels;
                for (const TrialChallenge& c : t->challenges)
                {
                    oldIds.push_back(c.challenge);
                    oldLevels.push_back(c.level);
                }
                std::sort(ids.begin(), ids.end());
                std::sort(levels.begin(), levels.end());
                std::sort(oldIds.begin(), oldIds.end());
                std::sort(oldLevels.begin(), oldLevels.end());
                if (ids != oldIds || levels != oldLevels)
                    codes.push_back(0xB);
            }
        }
        if (a.name.empty())
            codes.push_back(3);
        if (a.challenges.empty())
            codes.push_back(4);
        if (!IconExists(a.icon))
            codes.push_back(5);
        if (a.id.size() > 0x24)
            codes.push_back(6);
        if (a.name.size() > 0x80)
            codes.push_back(7);
        if (a.description.size() > 0x2000)
            codes.push_back(8);
        if (a.icon.size() > 0x80)
            codes.push_back(9);
        for (const std::string& d : a.descriptions)
            if (d.size() > 0x400)
            {
                codes.push_back(10);
                break;
            }
        std::vector<std::string> names;
        for (uint32_t c : codes)
            names.push_back(TrialName(c < 12 ? kTrialSaveResults[c] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(c)));
        PushBool(L, true);
        if (names.empty())
            AscLua::lua_pushnil(L);
        else
            PushStrings(L, names);
        return 2;
    }

    int DeleteTrial(lua_State* L)   // FUN_10126f20
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        SendId(0x5A9, id);
        PushBool(L, true);
        return 1;
    }

    int ActivateTrial(lua_State* L)   // FUN_10125b10
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        SendId(0x5AD, id);
        PushBool(L, true);
        return 1;
    }

    int DeactivateTrial(lua_State* L)   // FUN_10126e30
    {
        Packet(0x5AF).Send();
        PushBool(L, true);
        return 1;
    }

    int QueryTrials(lua_State* L) { Query(); PushBool(L, true); return 1; }   // handler_QueryTrials

    int QueryTrialCompletions(lua_State* L)   // FUN_10127b90
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        SendId(0x5C9, id);
        PushBool(L, true);
        return 1;
    }

    int CanEditTrial(lua_State* L)   // FUN_101262e0
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        const Trial* t = Find(id);
        PushBool(L, t && t->owned);
        return 1;
    }

    int CanActivateTrial(lua_State* L)   // FUN_10125c90
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        std::vector<std::string> errors;
        PushBool(L, CanActivate(id, errors));
        PushStrings(L, errors);
        return 2;
    }

    int CanDeactivateTrial(lua_State* L)   // FUN_10125e50
    {
        std::vector<std::string> errors;
        PushBool(L, CanDeactivate(errors));
        PushStrings(L, errors);
        return 2;
    }

    // FUN_10127d30 (id, upvote, downvote): a changed, non-contradictory vote is recorded and sent (0x5BF).
    int RateTrial(lua_State* L)
    {
        if (!ValidateInput(L, {STRING, BOOLEAN, BOOLEAN}))
            return 0;
        const std::string id = CheckString(L, 1);
        const bool up = AscLua::lua_toboolean(L, 2) != 0, down = AscLua::lua_toboolean(L, 3) != 0;
        Trial* t = FindMutable(id);
        if (!t || (t->upvoted == up && t->downvoted == down) || (up && down))
        {
            PushBool(L, false);
            return 1;
        }
        if (t->upvoted && !t->upvotes.empty())
            t->upvotes.pop_back();
        if (t->downvoted && !t->downvotes.empty())
            t->downvotes.pop_back();
        t->upvoted = up;
        t->downvoted = down;
        if (up)
            t->upvotes.push_back(static_cast<uint64_t>(time(nullptr)));
        if (down)
            t->downvotes.push_back(static_cast<uint64_t>(time(nullptr)));
        Packet p(0x5BF);
        p.U32(static_cast<uint32_t>(id.size()));
        if (!id.empty())
            p.Data(id.data(), static_cast<uint32_t>(id.size()));
        p.U8(up ? 1 : 0).U8(down ? 1 : 0).Send();
        PushBool(L, true);
        return 1;
    }

    std::vector<const Trial*> Sorted()   // FUN_101204f0 copy + FUN_1011e400 sort
    {
        std::vector<const Trial*> v;
        for (const Trial& t : g_trials)
            v.push_back(&t);
        std::sort(v.begin(), v.end(), [](const Trial* a, const Trial* b) { return Before(*a, *b); });
        return v;
    }

    // FUN_101270d0: every trial except the archive's (unless it is the active one).
    int GetAllTrials(lua_State* L)
    {
        NewTable(L);
        int i = 0;
        for (const Trial* t : Sorted())
            if (t->author != "Project Ascension Archive" || t->id == g_active)
            {
                PushInt(L, ++i);
                NewTable(L);
                PushTrial(L, *t, true);
                AscLua::lua_settable(L, -3);
            }
        return 1;
    }

    int GetOwnedTrials(lua_State* L)   // FUN_10127830
    {
        NewTable(L);
        int i = 0;
        for (const Trial* t : Sorted())
            if (t->owned)
            {
                PushInt(L, ++i);
                NewTable(L);
                PushTrial(L, *t, true);
                AscLua::lua_settable(L, -3);
            }
        return 1;
    }

    int GetTrialInfo(lua_State* L)   // FUN_101276d0
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        if (const Trial* t = Find(id))
        {
            NewTable(L);
            PushTrial(L, *t, true);
        }
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetActiveTrial(lua_State* L) { PushStr(L, g_active.c_str()); return 1; }   // handler_GetActiveTrial
    int GetNumTrials(lua_State* L) { PushInt(L, static_cast<int32_t>(Entries().Size())); return 1; }

    int GetTrialAtIndex(lua_State* L)   // FUN_10127930 (index, sortConditions)
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return 0;
        const int32_t index = ToInt(CheckNumber(L, 1));
        const bool sort = AscLua::lua_toboolean(L, 2) != 0;
        if (index == 0)
            return 0;
        const std::vector<const Trial*>& f = Entries().Filtered();
        if (static_cast<uint32_t>(index - 1) < f.size() && f[index - 1])
        {
            NewTable(L);
            PushTrial(L, *f[index - 1], sort);
        }
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetTrialIDByIndex(lua_State* L)   // FUN_10127a70
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const int32_t index = ToInt(CheckNumber(L, 1));
        if (index == 0)
            return 0;
        const std::vector<const Trial*>& f = Entries().Filtered();
        if (static_cast<uint32_t>(index - 1) < f.size() && f[index - 1])
            PushStr(L, f[index - 1]->id.c_str());
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    bool ReadIdNumber(lua_State* L, std::string& id, uint32_t& n)   // FUN_100e96d0
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return false;
        id = CheckString(L, 1);
        n = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return true;
    }

    int GetTrialCompletion(lua_State* L)   // FUN_10127220 (id, 1-based index)
    {
        std::string id;
        uint32_t index;
        if (!ReadIdNumber(L, id, index) || index == 0)
            return 0;
        const std::vector<Completion>& v = g_completions[id];
        if (index - 1 < v.size())
            PushCompletion(L, v[index - 1]);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // FUN_10127350 (id, ranks): completions grouped by equal time taken, fastest first.
    int GetTrialCompletions(lua_State* L)
    {
        std::string id;
        uint32_t ranks;
        if (!ReadIdNumber(L, id, ranks))
            return 0;
        NewTable(L);
        std::vector<std::vector<const Completion*>> groups;
        for (const Completion& c : g_completions[id])
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
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int SetTrialFilter(lua_State* L)   // FUN_101286e0 (text, {FILTER_* = bool})
    {
        if (!ValidateInput(L, {STRING, TABLE}))
            return 0;
        const std::string text = CheckString(L, 1);
        std::map<std::string, bool> names;
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 2))
        {
            if (AscLua::lua_isstring(L, -2))
                names[AscLua::lua_tolstring(L, -2, nullptr)] = AscLua::lua_toboolean(L, -1) != 0;
            AscLua::lua_settop(L, -2);
        }
        std::vector<uint32_t> filters;
        for (auto& n : names)
            if (n.second)
                for (uint32_t i = 0; i < 13; ++i)
                    if (n.first == kTrialFilters[i])
                    {
                        filters.push_back(i);
                        break;
                    }
        Entries().ApplyFilter(text, filters, {});
        return 0;
    }

    // ---- packets -------------------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p) { T v; memcpy(&v, p->m_buffer + p->m_read, sizeof(T)); p->m_read += sizeof(T); return v; }
    std::string Str(CDataStore* p)
    {
        const uint32_t n = Read<uint32_t>(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += static_cast<int32_t>(n);
        return s;
    }
    Completion ReadCompletion(CDataStore* p)   // FUN_10125860
    {
        Completion c;
        c.trial = Str(p);
        c.player = Str(p);
        c.start = Read<uint64_t>(p);
        c.complete = Read<uint64_t>(p);
        c.race = Str(p);
        c.gender = Str(p);
        c.cls = Str(p);
        return c;
    }
    void SortCompletions(const std::string& id)   // FUN_101288b0
    {
        auto it = g_completions.find(id);
        if (it != g_completions.end())
            std::sort(it->second.begin(), it->second.end(),
                      [](const Completion& a, const Completion& b) { return a.complete - a.start < b.complete - b.start; });
    }

    void __cdecl OnSaveResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5A8
    {
        const std::string a = Str(p), b = Str(p);
        AscRuntime::Signal("TRIAL_SAVE_RESULT", "%s%s", a.c_str(), b.c_str());
    }
    void __cdecl OnDeleteResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5AA
    {
        const std::string a = Str(p), b = Str(p);
        AscRuntime::Signal("TRIAL_DELETE_RESULT", "%s%s", a.c_str(), b.c_str());
    }

    void __cdecl OnQueryResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5AC
    {
        if (Read<uint8_t>(p))
        {
            g_trials.clear();
            Entries().Reset();
        }
        const bool notify = Read<uint8_t>(p) != 0;
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            Trial t;
            t.id = Str(p);
            t.owned = Read<uint8_t>(p) != 0;
            t.author = Str(p);
            t.name = Str(p);
            t.description = Str(p);
            t.icon = Str(p);
            for (uint32_t m = Read<uint32_t>(p); m; --m)
            {
                TrialChallenge c;
                c.challenge = Read<uint32_t>(p);
                c.level = Read<uint32_t>(p);
                c.description = Str(p);
                t.challenges.push_back(std::move(c));
            }
            t.upvoted = Read<uint8_t>(p) != 0;
            t.downvoted = Read<uint8_t>(p) != 0;
            for (uint32_t m = Read<uint32_t>(p); m; --m)
                t.upvotes.push_back(Read<uint64_t>(p));
            for (uint32_t m = Read<uint32_t>(p); m; --m)
                t.downvotes.push_back(Read<uint64_t>(p));
            g_trials.push_back(std::move(t));
        }
        Entries().Reset();   // entry pointers moved with the vector
        if (notify)
            AscRuntime::Signal("TRIAL_QUERY_RESULT");
    }

    void __cdecl OnActivateResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5AE
    {
        const std::string id = Str(p), result = Str(p);
        if (result == "ACTIVATE_CHALLENGE_OK")
        {
            g_active = id;
            Query();
        }
        AscRuntime::Signal("TRIAL_ACTIVATE_RESULT", "%s%s", id.c_str(), TrialName(result).c_str());
    }

    void __cdecl OnDeactivateResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5B0
    {
        const std::string id = Str(p), result = Str(p);
        if (result == "DEACTIVATE_CHALLENGE_OK")
        {
            g_active.clear();
            Query();
        }
        AscRuntime::Signal("TRIAL_DEACTIVATE_RESULT", "%s%s", id.c_str(), TrialName(result).c_str());
    }

    void __cdecl OnActiveUpdate(void*, uint32_t, uint32_t, CDataStore* p) { g_active = Str(p); }   // 0x5B1

    void __cdecl OnCompletionList(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5CA
    {
        const std::string id = Str(p);
        g_completions.erase(id);
        for (uint32_t n = Read<uint32_t>(p); n; --n)
            g_completions[id].push_back(ReadCompletion(p));
        SortCompletions(id);
        AscRuntime::Signal("TRIAL_COMPLETION_LIST_CHANGED", "%s", id.c_str());
    }

    void __cdecl OnCompletion(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5CB
    {
        Completion c = ReadCompletion(p);
        const std::string id = c.trial;
        g_completions[id].push_back(std::move(c));
        SortCompletions(id);
        AscRuntime::Signal("TRIAL_COMPLETION_ADDED", "%s%u", id.c_str(), static_cast<uint32_t>(g_completions[id].size() - 1));
    }

    void OnGlueScreen()   // FUN_10119df0
    {
        g_trials.clear();
        g_active.clear();
        g_completions.clear();
        Entries().Reset();
    }

    void Init()
    {
        sDC.AddPacketHandler(0x5A8, CNetClientCustomPacket((void*)&OnSaveResult, nullptr));
        sDC.AddPacketHandler(0x5AA, CNetClientCustomPacket((void*)&OnDeleteResult, nullptr));
        sDC.AddPacketHandler(0x5AC, CNetClientCustomPacket((void*)&OnQueryResult, nullptr));
        sDC.AddPacketHandler(0x5AE, CNetClientCustomPacket((void*)&OnActivateResult, nullptr));
        sDC.AddPacketHandler(0x5B0, CNetClientCustomPacket((void*)&OnDeactivateResult, nullptr));
        sDC.AddPacketHandler(0x5B1, CNetClientCustomPacket((void*)&OnActiveUpdate, nullptr));
        sDC.AddPacketHandler(0x5CA, CNetClientCustomPacket((void*)&OnCompletionList, nullptr));
        sDC.AddPacketHandler(0x5CB, CNetClientCustomPacket((void*)&OnCompletion, nullptr));
        AscRuntime::OnGlueScreen(OnGlueScreen);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_TrialCreator", "SaveTrial", SaveTrial},
        {"C_TrialCreator", "CanSaveTrial", CanSaveTrial},
        {"C_TrialCreator", "DeleteTrial", DeleteTrial},
        {"C_TrialCreator", "GetAllTrials", GetAllTrials},
        {"C_TrialCreator", "GetOwnedTrials", GetOwnedTrials},
        {"C_TrialCreator", "QueryTrials", QueryTrials},
        {"C_TrialCreator", "CanEditTrial", CanEditTrial},
        {"C_TrialCreator", "GetTrialInfo", GetTrialInfo},
        {"C_TrialCreator", "GetActiveTrial", GetActiveTrial},
        {"C_TrialCreator", "ActivateTrial", ActivateTrial},
        {"C_TrialCreator", "DeactivateTrial", DeactivateTrial},
        {"C_TrialCreator", "CanActivateTrial", CanActivateTrial},
        {"C_TrialCreator", "CanDeactivateTrial", CanDeactivateTrial},
        {"C_TrialCreator", "RateTrial", RateTrial},
        {"C_TrialCreator", "QueryTrialCompletions", QueryTrialCompletions},
        {"C_TrialCreator", "GetTrialCompletions", GetTrialCompletions},
        {"C_TrialCreator", "GetTrialCompletion", GetTrialCompletion},
        {"C_TrialCreator", "GetNumTrials", GetNumTrials},
        {"C_TrialCreator", "GetTrialAtIndex", GetTrialAtIndex},
        {"C_TrialCreator", "SetTrialFilter", SetTrialFilter},
        {"C_TrialCreator", "GetTrialIDByIndex", GetTrialIDByIndex},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscTrialCreator
{
bool HasActiveTrial() { return !g_active.empty(); }
}
