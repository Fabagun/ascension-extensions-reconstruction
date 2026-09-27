// C_Wildcard, part 2: the WildcardMgr's desired and undesired roll lists (FUN_10a349d0, static
// 0x10D3D318: +0x00 desired, +0x0C undesired). Each entry is 8 bytes: u32 id, u8 kind (Ability 1,
// Talent 2, TalentAbility 4 -- CharacterAdvancement rows; Tag 5 -- SpellTagTypes rows; Suggestion 6 --
// id 1 "suggested abilities" / 2 "suggested talents"), u8 is-ability, u8 is-talent.
//
// The (id, kind name) argument pair is FUN_100e9180 + FUN_10a29980 (names 0x10B5D160).
// Undesired changes notify the suggestion cache (lists 0x10BE2CAC / 0x10BE2CCC -> 0x10333950).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAFilter.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <algorithm>
#include <map>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;
using AscCA::Row;

namespace
{
    const char* const kKinds[7] = {"None", "Ability", "Talent", "Trait", "TalentAbility", "Tag", "Suggestion"};
    const char* const kAddDesired[5] = {"ADD_WILDCARD_DESIRED_ENTRY_OK", "ADD_WILDCARD_DESIRED_ENTRY_UNKNOWN",
        "ADD_WILDCARD_DESIRED_ENTRY_NOT_FOUND", "ADD_WILDCARD_DESIRED_ENTRY_ALREADY_KNOWN",
        "ADD_WILDCARD_DESIRED_ENTRY_ALREADY_DESIRED"};   // 0x10B1D4DC
    const char* const kAddUndesired[8] = {"ADD_WILDCARD_UNDESIRED_ENTRY_OK", "ADD_WILDCARD_UNDESIRED_ENTRY_UNKNOWN",
        "ADD_WILDCARD_UNDESIRED_ENTRY_NOT_FOUND", "ADD_WILDCARD_UNDESIRED_ENTRY_NOT_KNOWN",
        "ADD_WILDCARD_UNDESIRED_ENTRY_LOCKED", "ADD_WILDCARD_UNDESIRED_ENTRY_ALREADY_UNDESIRED",
        "ADD_WILDCARD_UNDESIRED_ENTRY_SPELL_TAG", "ADD_WILDCARD_UNDESIRED_ENTRY_SUGGESTION"};   // 0x10B22C30

    struct Entry { uint32_t id; uint8_t kind, ability, talent; };
    std::vector<Entry> g_desired, g_undesired;

    bool Contains(const std::vector<Entry>& v, uint32_t id, uint8_t kind)   // FUN_10a34b20 / FUN_10a34d10
    {
        return std::any_of(v.begin(), v.end(), [&](const Entry& e) { return e.id == id && e.kind == kind; });
    }

    // FUN_100e9180 + FUN_10a29980: (number, kind name); false unless the name is a known kind.
    bool ReadIdKind(lua_State* L, uint32_t& id, uint8_t& kind)
    {
        if (!ValidateInput(L, {NUMBER, STRING}))
            return false;
        id = static_cast<uint32_t>(static_cast<int64_t>(CheckNumber(L, 1)));
        const std::string name = CheckString(L, 2);
        for (uint8_t i = 0; i < 7; ++i)
            if (name == kKinds[i])
            {
                kind = i;
                return true;
            }
        return false;
    }

    AscDbc::Table& TagTypes() { return AscDbc::Get("DBFilesClient/SpellTagTypes.dbc"); }   // 0x10BDF5F4

    // FUN_10a34d40: the row's own kind (type string at +4) equals the requested one.
    bool RowIsKind(Row r, uint8_t kind) { return r && AscCA::RowType(r) == kind; }

    bool IsAbilityRow(Row r) { const int t = AscCA::RowType(r); return t == 1 || t == 4; }   // FUN_101c6770
    bool AnyRollKind(Row r) { const int t = AscCA::RowType(r); return t == 1 || t == 4 || t == 2; }   // 6740 / 6eb0 / 6e80

    // FUN_10a26d10: a row the player may still roll into -- visible, class-allowed, not held, none of the
    // excluding flags, its level within the build's and the wildcard pool's reach, and not a second
    // +0x74 == 2 entry.
    bool Desirable(Row r)
    {
        if (!r || !AnyRollKind(r))
            return false;
        AscCA::Build* b = AscCA::ActiveBuild();
        if (!b || !AscCA::RowVisible(r) || !AscCA::ClassAllowedForPlayer(r))
            return false;
        if (b->Has(AscCA::RowU32(r, 0)))
            return false;
        if (AscCA::RowHasFlag(r, 0x20000) || AscCA::RowHasFlag(r, 8) || AscCA::RowHasFlag(r, 0x10000)
            || AscCA::RowHasFlag(r, 0x1000) || AscCA::RowHasFlag(r, 0x80000))
            return false;
        const uint32_t f = AscCA::RowU32(r, 0x124);
        if ((f & 0x1800000) && !((f & 0x800000) && b->wildcard))
            if (!((f & 0x1000000) && b->draft))
                return false;
        const uint32_t req = AscCA::RequiredLevelOf(*b, r);
        if (b->level < req)
            return false;
        if (IsAbilityRow(r) && AscCA::PoolLevelOf(*b, false) < req)
            return false;
        if (AscCA::RowType(r) == 2 && AscCA::PoolLevelOf(*b, true) < req)
            return false;
        if (AscCA::RowU32(r, 0x74) == 2 && b->HasGroup(2, 0))
            return false;
        return true;
    }

    // FUN_10a26e80: a held, unlocked row the player may mark undesired.
    bool Undesirable(Row r)
    {
        if (!r || !AnyRollKind(r))
            return false;
        AscCA::Build* b = AscCA::ActiveBuild();
        if (!b || !AscCA::RowVisible(r) || !AscCA::ClassAllowedForPlayer(r))
            return false;
        const uint32_t id = AscCA::RowU32(r, 0);
        if (!b->Has(id) || b->LockedOf(id))
            return false;
        if (AscCA::RowHasFlag(r, 0x20000) || AscCA::RowHasFlag(r, 8) || AscCA::RowHasFlag(r, 0x10000)
            || AscCA::RowHasFlag(r, 0x1000) || AscCA::RowHasFlag(r, 4))
            return false;
        const uint32_t f = AscCA::RowU32(r, 0x124);
        if (!(f & 0x1800000))
            return true;
        return ((f & 0x800000) && b->wildcard) || ((f & 0x1000000) && b->draft);
    }

    // FUN_10a33170: 0 ok, else an ADD_WILDCARD_DESIRED_ENTRY_* index.
    uint8_t CanDesire(uint32_t id, uint8_t kind)
    {
        switch (kind)
        {
        case 1: case 2: case 4:
        {
            Row r = AscCA::FindRow(id);
            if (!r)
                return 2;
            AscCA::Build* b = AscCA::ActiveBuild();
            if (b)
            {
                if (b->Has(id))
                    return 3;
                if (RowIsKind(r, kind) && Desirable(r))
                    return Contains(g_desired, id, kind) ? 4 : 0;
            }
            return 1;
        }
        case 5:
            if (TagTypes().Row(id))
                return Contains(g_desired, id, kind) ? 4 : 0;
            return 2;
        case 6:
            if (id == 1 || id == 2)
                return Contains(g_desired, id, kind) ? 4 : 0;
            return 2;
        default:
            return 2;
        }
    }

    // FUN_10a33280: 0 ok, else an ADD_WILDCARD_UNDESIRED_ENTRY_* index.
    uint8_t CanUndesire(uint32_t id, uint8_t kind)
    {
        switch (kind)
        {
        case 1: case 2: case 4:
        {
            Row r = AscCA::FindRow(id);
            AscCA::Build* b = r ? AscCA::ActiveBuild() : nullptr;
            if (!b)
                return 1;
            if (!b->Has(id))
                return 3;
            if (b->LockedOf(id))
                return 4;
            if (RowIsKind(r, kind) && Undesirable(r))
                return Contains(g_undesired, id, kind) ? 5 : 0;
            return 1;
        }
        case 5:
            return TagTypes().Row(id) ? 6 : 1;
        case 6:
            return (id == 1 || id == 2) ? 7 : 1;
        default:
            return 1;
        }
    }

    Entry Make(uint32_t id, uint8_t kind)   // FUN_10a25fe0 / FUN_10a26020 / FUN_10a25fc0
    {
        if (kind == 5)
            return {id, 5, 1, 1};
        if (kind == 6)
            return {id, 6, static_cast<uint8_t>(id == 1), static_cast<uint8_t>(id == 2)};
        Row r = AscCA::FindRow(id);
        const uint8_t own = r ? static_cast<uint8_t>(AscCA::RowType(r)) : kind;
        return {id, own, static_cast<uint8_t>(r && IsAbilityRow(r)), static_cast<uint8_t>(r && AscCA::RowType(r) == 2)};
    }

    void DesiredChanged() { AscRuntime::Signal("WILDCARD_DESIRED_ENTRIES_CHANGED"); }
    void UndesiredChanged() { AscRuntime::Signal("WILDCARD_UNDESIRED_ENTRIES_CHANGED"); }

    // FUN_10a32c10 (quiet = bulk add, no event).
    void AddDesired(uint32_t id, uint8_t kind, bool quiet)
    {
        if (Contains(g_desired, id, kind) || CanDesire(id, kind) != 0)
            return;
        g_desired.push_back(Make(id, kind));
        if (!quiet)
            DesiredChanged();
    }

    // FUN_10a32ee0: only the CA kinds pass the checks in the original's order; tags and suggestions are
    // refused by CanUndesire, so they are never added by this path.
    void AddUndesired(uint32_t id, uint8_t kind, bool quiet)
    {
        if (Contains(g_undesired, id, kind) || CanUndesire(id, kind) != 0)
            return;
        g_undesired.push_back(Make(id, kind));
        AscCA::MarkSuggestionsDirty();
        if (!quiet)
            UndesiredChanged();
    }

    int AddDesiredID(lua_State* L)   // FUN_10a2af20
    {
        uint32_t id;
        uint8_t kind;
        if (ReadIdKind(L, id, kind) && CanDesire(id, kind) == 0)
            AddDesired(id, kind, false);
        return 0;
    }
    int AddUndesiredID(lua_State* L)   // FUN_10a2b020
    {
        uint32_t id;
        uint8_t kind;
        if (ReadIdKind(L, id, kind) && CanUndesire(id, kind) == 0)
            AddUndesired(id, kind, false);
        return 0;
    }

    template <size_t N>
    int PushCan(lua_State* L, uint8_t code, const char* const (&names)[N])
    {
        PushBool(L, code == 0);
        if (code == 0)
            AscLua::lua_pushnil(L);
        else
            PushStr(L, code < N ? names[code] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(code)).c_str());
        return 2;
    }
    int CanAddDesiredID(lua_State* L)   // FUN_10a2b120
    {
        uint32_t id;
        uint8_t kind;
        if (!ReadIdKind(L, id, kind))
            return 0;
        return PushCan(L, CanDesire(id, kind), kAddDesired);
    }
    int CanAddUndesiredID(lua_State* L)   // FUN_10a2b340
    {
        uint32_t id;
        uint8_t kind;
        if (!ReadIdKind(L, id, kind))
            return 0;
        return PushCan(L, CanUndesire(id, kind), kAddUndesired);
    }

    int IsDesiredID(lua_State* L)   // FUN_10a2d1c0
    {
        uint32_t id;
        uint8_t kind;
        if (!ReadIdKind(L, id, kind))
            return 0;
        PushBool(L, Contains(g_desired, id, kind));
        return 1;
    }
    int IsUndesiredID(lua_State* L)   // FUN_10a2d360
    {
        uint32_t id;
        uint8_t kind;
        if (!ReadIdKind(L, id, kind))
            return 0;
        PushBool(L, Contains(g_undesired, id, kind));
        return 1;
    }

    // FUN_10a36010 / FUN_10a36150: erase every matching entry, then notify and signal.
    int RemoveDesiredID(lua_State* L)
    {
        uint32_t id;
        uint8_t kind;
        if (!ReadIdKind(L, id, kind) || !Contains(g_desired, id, kind))
            return 0;
        g_desired.erase(std::remove_if(g_desired.begin(), g_desired.end(),
            [&](const Entry& e) { return e.id == id && e.kind == kind; }), g_desired.end());
        DesiredChanged();
        return 0;
    }
    int RemoveUndesiredID(lua_State* L)
    {
        uint32_t id;
        uint8_t kind;
        if (!ReadIdKind(L, id, kind) || !Contains(g_undesired, id, kind))
            return 0;
        g_undesired.erase(std::remove_if(g_undesired.begin(), g_undesired.end(),
            [&](const Entry& e) { return e.id == id && e.kind == kind; }), g_undesired.end());
        AscCA::MarkSuggestionsDirty();
        UndesiredChanged();
        return 0;
    }

    int ClearDesiredSpells(lua_State*)   // FUN_10a339b0: only when non-empty
    {
        if (g_desired.empty())
            return 0;
        g_desired.clear();
        DesiredChanged();
        return 0;
    }
    int ClearUndesiredSpells(lua_State*)   // FUN_10a33e60
    {
        if (g_undesired.empty())
            return 0;
        g_undesired.clear();
        AscCA::MarkSuggestionsDirty();
        UndesiredChanged();
        return 0;
    }

    // FUN_10a33ca0 / FUN_10a33fc0: drop the ability (+5) / talent (+6) entries; notify for each removed
    // one and signal when anything went.
    void RemoveAllOf(bool talents)
    {
        const size_t before = g_undesired.size();
        g_undesired.erase(std::remove_if(g_undesired.begin(), g_undesired.end(),
            [&](const Entry& e) { return talents ? e.talent != 0 : e.ability != 0; }), g_undesired.end());
        if (g_undesired.size() != before)
        {
            AscCA::MarkSuggestionsDirty();
            UndesiredChanged();
        }
    }
    int RemoveAllUndesiredAbilities(lua_State*) { RemoveAllOf(false); return 0; }
    int RemoveAllUndesiredTalents(lua_State*) { RemoveAllOf(true); return 0; }

    // ---- the entry browsers (WildcardDesired/UndesiredEntryContainer: mgr +0x18 / +0xF8) -----------
    // WildcardEntryContainerBase: +8 type (0 desired, 1 undesired); entries are 0x6C-byte views
    // (FUN_10a25390 CA row / FUN_10a254c0 tag / FUN_10a25cf0 suggestion); the filtered list is +0xD0.
    const char* const kEntryFilters[19] = {"FILTER_NONE", "FILTER_QUALITY_NORMAL", "FILTER_QUALITY_UNCOMMON",
        "FILTER_QUALITY_RARE", "FILTER_QUALITY_EPIC", "FILTER_QUALITY_LEGENDARY", "FILTER_CLASS_HUNTER",
        "FILTER_CLASS_WARRIOR", "FILTER_CLASS_ROGUE", "FILTER_CLASS_MAGE", "FILTER_CLASS_PRIEST",
        "FILTER_CLASS_WARLOCK", "FILTER_CLASS_DRUID", "FILTER_CLASS_SHAMAN", "FILTER_CLASS_PALADIN",
        "FILTER_CLASS_DEATH_KNIGHT", "FILTER_CLASS_GENERAL", "FILTER_TYPE_ABILITY", "FILTER_TYPE_TALENT"};   // 0x10B5FD90

    struct View
    {
        uint32_t id = 0;
        uint8_t kind = 0;
        std::string name, icon;
        uint32_t requiredLevel = 0, classType = 0, tab = 0;
        uint32_t spells[9] = {};
        bool ability = false, talent = false;
    };

    View ViewOfRow(Row r)   // FUN_10a25390
    {
        View v;
        v.id = AscCA::RowU32(r, 0);
        v.kind = static_cast<uint8_t>(AscCA::RowType(r) < 0 ? 0 : AscCA::RowType(r));
        v.name = AscCA::RowText(r, 0xBC);
        v.icon = AscCA::RowText(r, 0xC0);
        if (AscCA::Build* b = AscCA::ActiveBuild())
            v.requiredLevel = AscCA::RequiredLevelOf(*b, r);
        v.classType = AscCA::RowU32(r, 0x80);
        v.tab = AscCA::RowU32(r, 0x84);
        for (uint32_t k = 0; k < 9; ++k)
            v.spells[k] = AscCA::RowU32(r, 0x14 + k * 4);
        v.ability = IsAbilityRow(r);
        v.talent = AscCA::RowType(r) == 2;
        return v;
    }

    // FUN_10a26560 (desired): every desirable row, every SpellTagTypes row with +0xC and +0x10 zero, then
    // the two suggestion entries. FUN_10a267e0 (undesired): every undesirable row.
    std::vector<View> Populate(bool undesired)
    {
        std::vector<View> out;
        AscDbc::Table& ca = AscDbc::Get("DBFilesClient\\CharacterAdvancement.dbc");
        for (uint32_t id = ca.MinId(); ca.Count() && id <= ca.MaxId(); ++id)
            if (Row r = ca.Row(id))
                if (undesired ? Undesirable(r) : Desirable(r))
                    out.push_back(ViewOfRow(r));
        if (undesired)
            return out;
        AscDbc::Table& tags = TagTypes();
        for (uint32_t i = 0; i < tags.Count(); ++i)
            if (const uint8_t* t = tags.RowAt(i))
                if (AscDbc::Table::U32(t, 0xC) == 0 && AscDbc::Table::U32(t, 0x10) == 0)
                {
                    View v;
                    v.id = AscDbc::Table::U32(t, 0);
                    v.kind = 5;
                    v.name = tags.Str(t, 0x70);
                    v.icon = tags.Str(t, 0x68);
                    v.ability = v.talent = true;
                    out.push_back(v);
                }
        View a;
        a.id = 1;
        a.kind = 6;
        a.name = "Suggested Abilities";
        a.icon = "Interface\\Icons\\Ability_ThunderBolt";
        a.ability = true;
        out.push_back(a);
        View t;
        t.id = 2;
        t.kind = 6;
        t.name = "Suggested Talents";
        t.icon = "Interface\\Icons\\Warrior_talent_icon_singlemindedfury";
        t.talent = true;
        out.push_back(t);
        return out;
    }

    // FUN_10a26f90: FILTER_* against a CharacterAdvancement view (tags and suggestions never match).
    bool EnumMatch(const View& v, uint32_t f)
    {
        if (v.kind < 1 || v.kind > 4)
            return false;
        Row r = AscCA::FindRow(v.id);
        if (!r)
            return false;
        if (f >= 1 && f <= 5)
            return AscCAFilter::RowQualityIndex(r) == static_cast<int>(f);
        if (f >= 6 && f <= 16)
            return AscCA::RowU32(r, 0x80) == f - 4;
        if (f == 17)
            return IsAbilityRow(r);
        if (f == 18)
            return AscCA::RowType(r) == 2;
        return false;
    }
    uint32_t FilterGroup(uint32_t f)   // FUN_10a27320's grouping
    {
        if (f >= 1 && f <= 5) return 1;
        if (f >= 6 && f <= 16) return 2;
        if (f == 17 || f == 18) return 3;
        return 0;
    }

    // FUN_10a269e0: empty query, or the name contains it, or (a CA view) the row's text search does.
    bool QueryMatch(const View& v, const std::string& lower)
    {
        if (lower.empty())
            return true;
        if (AscFilter::Lower(v.name).find(lower) != std::string::npos)
            return true;
        if (v.kind >= 1 && v.kind <= 4)
            if (Row r = AscCA::FindRow(v.id))
                return AscCAFilter::RowMatchesQuery(r, lower);
        return false;
    }

    // FUN_10152fc0: how many of the build's entries were learned after this one (+0x18 as a u64), -1 when
    // the build does not hold it -- so the most recent sorts first.
    int64_t LearnedOrder(uint32_t id)
    {
        AscCA::Build* b = AscCA::ActiveBuild();
        if (!b)
            return -1;
        const AscCA::Entry* mine = nullptr;
        for (const AscCA::Entry& e : b->entries)
            if (e.id == id)
            {
                mine = &e;
                break;
            }
        if (!mine)
            return -1;
        const uint64_t key = (static_cast<uint64_t>(mine->u1c) << 32) | mine->u18;
        int64_t later = 0;
        for (const AscCA::Entry& e : b->entries)
            if (((static_cast<uint64_t>(e.u1c) << 32) | e.u18) > key)
                ++later;
        return later;
    }

    struct Browser
    {
        bool undesired;
        std::vector<View> filtered;   // +0xD0
        bool built = false;           // +0xC
        std::string lastText;         // +0x4
        std::vector<uint32_t> lastFilters;   // +0x28

        // FUN_10a27c80: re-run the last query once one has been made.
        void Refresh()
        {
            if (built)
                Apply(std::string(lastText), std::vector<uint32_t>(lastFilters));
        }

        // FUN_10a27320: groups AND, filters in a group OR; then the selected sort (entries already on
        // the list first), suggestions first (desired) or learned order (undesired), then id.
        void Apply(const std::string& text, const std::vector<uint32_t>& filters)
        {
            built = true;
            lastText = text;
            lastFilters = filters;
            const std::string lower = AscFilter::Lower(text);
            std::vector<std::pair<uint32_t, std::vector<uint32_t>>> groups;
            for (uint32_t f : filters)
            {
                const uint32_t g = FilterGroup(f);
                auto it = std::find_if(groups.begin(), groups.end(), [g](const auto& p) { return p.first == g; });
                if (it == groups.end())
                    groups.emplace_back(g, std::vector<uint32_t>{f});
                else
                    it->second.push_back(f);
            }
            filtered.clear();
            for (const View& v : Populate(undesired))
            {
                bool keep = QueryMatch(v, lower);
                for (const auto& g : groups)
                    keep = keep && std::any_of(g.second.begin(), g.second.end(), [&](uint32_t f) { return EnumMatch(v, f); });
                if (keep)
                    filtered.push_back(v);
            }
            const std::vector<Entry>& list = undesired ? g_undesired : g_desired;
            auto selected = [&](const View& v) { return Contains(list, v.id, v.kind) ? 0 : 1; };
            std::stable_sort(filtered.begin(), filtered.end(), [&](const View& a, const View& b) {
                const int sa = selected(a), sb = selected(b);
                if (sa != sb)
                    return sa < sb;
                if (!undesired)
                {
                    const int ga = a.kind != 6, gb = b.kind != 6;
                    if (ga != gb)
                        return ga < gb;
                }
                else
                {
                    const uint64_t la = static_cast<uint64_t>(LearnedOrder(a.id)), lb = static_cast<uint64_t>(LearnedOrder(b.id));
                    if (la != lb)
                        return la < lb;
                }
                return a.id < b.id;
            });
        }
    };
    Browser g_desiredView{false}, g_undesiredView{true};

    // FUN_102dd650: (text, {FILTER_* = true}).
    bool ReadFilterArgs(lua_State* L, std::string& text, std::vector<uint32_t>& filters)
    {
        if (!ValidateInput(L, {STRING, TABLE}))
            return false;
        text = CheckString(L, 1);
        std::map<std::string, bool> names;
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, 2))
        {
            if (AscLua::lua_isstring(L, -2))
                names[AscLua::lua_tolstring(L, -2, nullptr)] = AscLua::lua_toboolean(L, -1) != 0;
            AscLua::lua_settop(L, -2);
        }
        for (const auto& n : names)
            if (n.second)
                for (uint32_t i = 0; i < 19; ++i)
                    if (n.first == kEntryFilters[i])
                        filters.push_back(i);
        return true;
    }

    int SetFilteredDesiredEntries(lua_State* L)   // FUN_10a2dd60
    {
        std::string text;
        std::vector<uint32_t> filters;
        if (!ReadFilterArgs(L, text, filters))
            return 0;
        g_desiredView.Apply(text, filters);
        return 0;
    }
    int SetFilteredUndesiredEntries(lua_State* L)   // FUN_10a2e190
    {
        std::string text;
        std::vector<uint32_t> filters;
        if (!ReadFilterArgs(L, text, filters))
            return 0;
        g_undesiredView.Apply(text, filters);
        return 0;
    }
    int GetNumFilteredDesiredEntries(lua_State* L) { AscLua::lua_pushinteger(L, static_cast<int>(g_desiredView.filtered.size())); return 1; }
    int GetNumFilteredUndesiredEntries(lua_State* L) { AscLua::lua_pushinteger(L, static_cast<int>(g_undesiredView.filtered.size())); return 1; }

    // func_0x10a27190 -> FUN_10a257b0 / FUN_10a255c0.
    int PushView(lua_State* L, const Browser& br)
    {
        uint32_t index;
        if (!ReadNumber(L, index) || index == 0)
            return 0;
        if (index - 1 >= br.filtered.size())
            return 0;
        const View& v = br.filtered[index - 1];
        AscLua::lua_createtable(L, 0, 8);
        AscLua::lua_checkstack(L, 2);
        auto setStr = [L](const char* k, const std::string& s) { AscLua::lua_pushstring(L, k); PushStr(L, s.c_str()); AscLua::lua_settable(L, -3); };
        AscLua::lua_pushstring(L, "ID");
        AscLua::lua_pushinteger(L, static_cast<int>(v.id));
        AscLua::lua_settable(L, -3);
        setStr("Type", v.kind < 7 ? kKinds[v.kind] : "");
        AscLua::lua_pushstring(L, "Spells");   // the non-zero spells (FUN_10169a80)
        std::vector<uint32_t> spells;
        for (uint32_t s : v.spells)
            if (s)
                spells.push_back(s);
        AscLua::lua_createtable(L, 0, static_cast<int>(spells.size()));
        for (uint32_t i = 0; i < spells.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int>(spells[i]));
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);
        const uint8_t* ct = AscDbc::Get("DBFilesClient\\CharacterAdvancementClassTypes.dbc").Row(v.classType);
        setStr("Class", ct ? AscDbc::Get("DBFilesClient\\CharacterAdvancementClassTypes.dbc").Str(ct, 4)
                           : "INVALID_CLASS_TYPE_" + std::to_string(v.classType));
        const uint8_t* tb = AscDbc::Get("DBFilesClient\\CharacterAdvancementTabTypes.dbc").Row(v.tab);
        setStr("Tab", tb ? AscDbc::Get("DBFilesClient\\CharacterAdvancementTabTypes.dbc").Str(tb, 4)
                         : "INVALID_TAB_TYPE_" + std::to_string(v.tab));
        setStr("Name", v.name);
        AscLua::lua_pushstring(L, "RequiredLevel");
        AscLua::lua_pushinteger(L, static_cast<int>(v.requiredLevel));
        AscLua::lua_settable(L, -3);
        setStr("Icon", v.icon);
        return 1;
    }
    int GetFilteredDesiredEntryAtIndex(lua_State* L) { return PushView(L, g_desiredView); }
    int GetFilteredUndesiredEntryAtIndex(lua_State* L) { return PushView(L, g_undesiredView); }

    // FUN_10a329c0 / FUN_10a32af0: add every filtered entry quietly; one event if the list changed.
    int AddAllDesiredFilter(lua_State*)
    {
        const std::vector<Entry> before = g_desired;
        for (const View& v : g_desiredView.filtered)
            AddDesired(v.id, v.kind, true);
        if (g_desired.size() != before.size())
            DesiredChanged();
        return 0;
    }
    int AddAllUndesiredFilter(lua_State*)
    {
        const std::vector<Entry> before = g_undesired;
        for (const View& v : g_undesiredView.filtered)
            AddUndesired(v.id, v.kind, true);
        if (g_undesired.size() != before.size())
            UndesiredChanged();
        return 0;
    }

    void OnGlueScreen()   // 0x10A33940
    {
        g_desired.clear();
        g_undesired.clear();
        g_desiredView.filtered.clear();
        g_undesiredView.filtered.clear();
        g_desiredView.built = g_undesiredView.built = false;
    }

    // The row's kind (FUN_103331b0 on its type string), 0 without a row.
    uint8_t KindOf(uint32_t id)
    {
        Row r = AscCA::FindRow(id);
        const int t = r ? AscCA::RowType(r) : -1;
        return static_cast<uint8_t>(t < 0 ? 0 : t);
    }

    // FUN_10a34e80 (CA list 0x10be2c0c, an entry learned): the browsers re-run, and a learned entry
    // leaves the desired list.
    void OnEntryLearned(uint32_t id, uint32_t)
    {
        g_desiredView.Refresh();
        g_undesiredView.Refresh();
        const uint8_t kind = KindOf(id);
        if (!Contains(g_desired, id, kind))
            return;
        g_desired.erase(std::remove_if(g_desired.begin(), g_desired.end(),
            [&](const Entry& e) { return e.id == id && e.kind == kind; }), g_desired.end());
        DesiredChanged();
    }

    // FUN_10a35080 (CA list 0x10be2c4c, an entry unlearned): an unlearned entry leaves the undesired list.
    void OnEntryUnlearned(uint32_t id, uint32_t)
    {
        g_desiredView.Refresh();
        g_undesiredView.Refresh();
        const uint8_t kind = KindOf(id);
        if (!Contains(g_undesired, id, kind))
            return;
        g_undesired.erase(std::remove_if(g_undesired.begin(), g_undesired.end(),
            [&](const Entry& e) { return e.id == id && e.kind == kind; }), g_undesired.end());
        AscCA::MarkSuggestionsDirty();
        UndesiredChanged();
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(OnGlueScreen);
        AscCA::OnEntryLearned(OnEntryLearned);
        AscCA::OnEntryUnlearned(OnEntryUnlearned);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Wildcard", "AddDesiredID", AddDesiredID},
        {"C_Wildcard", "AddUndesiredID", AddUndesiredID},
        {"C_Wildcard", "CanAddDesiredID", CanAddDesiredID},
        {"C_Wildcard", "CanAddUndesiredID", CanAddUndesiredID},
        {"C_Wildcard", "IsDesiredID", IsDesiredID},
        {"C_Wildcard", "IsUndesiredID", IsUndesiredID},
        {"C_Wildcard", "RemoveDesiredID", RemoveDesiredID},
        {"C_Wildcard", "RemoveUndesiredID", RemoveUndesiredID},
        {"C_Wildcard", "ClearDesiredSpells", ClearDesiredSpells},
        {"C_Wildcard", "ClearUndesiredSpells", ClearUndesiredSpells},
        {"C_Wildcard", "RemoveAllUndesiredAbilities", RemoveAllUndesiredAbilities},
        {"C_Wildcard", "RemoveAllUndesiredTalents", RemoveAllUndesiredTalents},
        {"C_Wildcard", "SetFilteredDesiredEntries", SetFilteredDesiredEntries},
        {"C_Wildcard", "SetFilteredUndesiredEntries", SetFilteredUndesiredEntries},
        {"C_Wildcard", "GetNumFilteredDesiredEntries", GetNumFilteredDesiredEntries},
        {"C_Wildcard", "GetNumFilteredUndesiredEntries", GetNumFilteredUndesiredEntries},
        {"C_Wildcard", "GetFilteredDesiredEntryAtIndex", GetFilteredDesiredEntryAtIndex},
        {"C_Wildcard", "GetFilteredUndesiredEntryAtIndex", GetFilteredUndesiredEntryAtIndex},
        {"C_Wildcard", "AddAllDesiredFilter", AddAllDesiredFilter},
        {"C_Wildcard", "AddAllUndesiredFilter", AddAllUndesiredFilter},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), Init);
}

namespace AscWildcardSets
{
    // The suggestion manager's filter: an entry of this kind is on the undesired list.
    bool IsUndesired(uint32_t id, uint8_t kind) { return Contains(g_undesired, id, kind); }

    size_t DesiredCount() { return g_desired.size(); }
    size_t UndesiredCount() { return g_undesired.size(); }

    // FUN_10a333d0's walks over the desired list: an entry flagged ability (+5) / talent (+6).
    bool HasDesiredWith(bool ability)
    {
        return std::any_of(g_desired.begin(), g_desired.end(), [&](const Entry& e) { return ability ? e.ability : e.talent; });
    }

    // FUN_10a35270's second half (a spec change): both lists empty, one event each when anything went.
    void ClearForSpecChange()
    {
        if (!g_desired.empty())
        {
            g_desired.clear();
            DesiredChanged();
        }
        if (!g_undesired.empty())
        {
            g_undesired.clear();
            AscCA::MarkSuggestionsDirty();
            UndesiredChanged();
        }
    }

    // The session snapshot of the desired list (FUN_10a36d00's walk): Ability / Talent / TalentAbility
    // ids, tag ids, and the desirable suggestions behind the two suggestion entries.
    void DesiredForRoll(std::vector<uint32_t>& ids, std::vector<uint32_t>& tags)
    {
        for (const Entry& e : g_desired)
            switch (e.kind)
            {
            case 1: case 2: case 4:
                ids.push_back(e.id);
                break;
            case 5:
                tags.push_back(e.id);
                break;
            case 6:
                if (e.id == 1 || e.id == 2)
                    for (uint32_t s : AscCA::SuggestedList(e.id == 1, 0xFA, true))   // FUN_10a32940
                        if (Desirable(AscCA::FindRow(s)))
                            ids.push_back(s);
                break;
            }
    }

    struct Selection { uint32_t id; uint8_t kind; };

    bool KindIndex(const std::string& name, uint8_t& kind)   // FUN_10a29980
    {
        for (uint8_t i = 0; i < 7; ++i)
            if (name == kKinds[i])
            {
                kind = i;
                return true;
            }
        return false;
    }

    // FUN_10a368b0's list swap: each list emptied (suggestions marked dirty per undesired entry) and
    // refilled through the quiet adds; an event only when the list really changed (FUN_10a31ea0).
    void ApplySelections(const std::vector<Selection>& desired, const std::vector<Selection>& undesired)
    {
        auto same = [](const std::vector<Entry>& a, const std::vector<Entry>& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Entry& x, const Entry& y) {
                return x.id == y.id && x.kind == y.kind && x.ability == y.ability && x.talent == y.talent; });
        };
        const std::vector<Entry> oldDesired = g_desired;
        g_desired.clear();
        for (const Selection& e : desired)
            AddDesired(e.id, e.kind, true);
        if (!same(oldDesired, g_desired))
            DesiredChanged();
        const std::vector<Entry> oldUndesired = g_undesired;
        if (!g_undesired.empty())
        {
            g_undesired.clear();
            AscCA::MarkSuggestionsDirty();
        }
        for (const Selection& e : undesired)
            AddUndesired(e.id, e.kind, true);
        if (!same(oldUndesired, g_undesired))
            UndesiredChanged();
    }

    struct Next { uint32_t id; uint8_t kind; bool ability, talent; };

    // FUN_10a344c0: newest undesired Ability / Talent / TalentAbility entry with a CharacterAdvancement
    // row whose view `enough` accepts (enough roll tokens for that kind); failing that, the newest one
    // with a row at all.
    bool NewestUnlearnable(Next& out, bool (*enough)(const Next&))
    {
        bool have = false;
        for (auto it = g_undesired.rbegin(); it != g_undesired.rend(); ++it)
        {
            if (it->kind != 1 && it->kind != 2 && it->kind != 4)
                continue;
            Row r = AscCA::FindRow(it->id);
            if (!r)
                continue;
            const View v = ViewOfRow(r);
            const Next n{v.id, v.kind, v.ability, v.talent};
            if (!have)
            {
                out = n;
                have = true;
            }
            if (enough(n))
            {
                out = n;
                return true;
            }
        }
        return have;
    }
}
