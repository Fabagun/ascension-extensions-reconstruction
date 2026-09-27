// C_Spell (handler_AppliesBuff .. handler_IsTrainerSpell). Spell records come from the client's
// Spell.dbc (FetchSpell, 0x2A8-byte row) and are read as its dword fields: [0] id, [3], [4]
// attributes, [0x47+i] effect, [0x4A+i] die sides, [0x50+i] base points, [0x56+i]/[0x59+i] implicit
// targets, [0x5F+i] aura, [0x6E+i] misc value, [0x74+i] trigger spell, [0x85], [0x90] family,
// [0x91] family flags.
// HasRuneUI lives with the CharacterAdvancement manager (AscCAMgr.cpp).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <cstring>
#include <unordered_map>

using namespace AscScript;

namespace
{
    struct SpellRec
    {
        uint32_t f[0x2A8 / 4] = {};
        bool Fetch(uint32_t id) { return FetchSpell(id, reinterpret_cast<uint8_t*>(f)); }
    };

    // FUN_102a7f10 over FUN_1020b080's map (one instance, AscCAMgr).
    const uint8_t* CustomAttr(uint32_t spellId) { return AscCA::SpellCustomAttrRow(spellId); }

    // FUN_10324e80: effect i applies an aura (effect 6/27/35/65/119/128/129/143/190 with an aura set).
    bool AppliesAura(const SpellRec& s, uint32_t i)
    {
        switch (s.f[0x47 + i])
        {
        case 0x06: case 0x1B: case 0x23: case 0x41: case 0x77: case 0x80: case 0x81: case 0x8F: case 0xBE:
            return s.f[0x5F + i] != 0;
        default:
            return false;
        }
    }

    // FUN_10325970: neither implicit target is an enemy-style target.
    bool FriendlyTargets(const SpellRec& s, uint32_t i)
    {
        for (uint32_t t : {s.f[0x56 + i], s.f[0x59 + i]})
            switch (t)
            {
            case 2: case 6: case 0xF: case 0x10: case 0x18: case 0x1C: case 0x35: case 0x68:
                return false;
            }
        return true;
    }

    bool IsPositiveEffect(const SpellRec& s, uint32_t i, bool nested);

    int32_t Amount(const SpellRec& s, uint32_t i) { return static_cast<int32_t>(s.f[0x50 + i] + s.f[0x4A + i]); }

    // FUN_10325130
    bool IsPositiveEffect(const SpellRec& s, uint32_t i, bool nested)
    {
        if (s.f[4] & 0x4000000)
            return false;
        const uint32_t id = s.f[0];
        if (const uint8_t* attr = CustomAttr(id))
        {
            const uint32_t flags = AscDbc::Table::U32(attr, 8);
            const uint32_t bit = i == 0 ? 0x1000 : i == 1 ? 0x2000 : i == 2 ? 0x4000 : 0;
            if (bit && (flags & bit))
                return false;
        }
        switch (s.f[0x90])
        {
        case 0:
            switch (id)
            {
            case 0xD634: case 0x878C: case 0x721E: case 0xF223: case 0xF224: return false;
            case 0x789D: case 0x119F: case 0x7E38: case 0x920C: case 0x95AE: case 0xC4A8:
            case 0xF114: case 0xF126: case 0xF17B: case 0xF18A: case 0xF388: case 0x10DA7F: return true;
            }
            break;
        case 3:
            if (s.f[0x91] == 0x2000 || s.f[0x85] == 0x2D)
                return true;
            if (s.f[0x91] == 0x800)
                return false;
            break;
        case 6:
            switch (id)
            {
            case 0x1182C1: case 0xFD4C: case 0xFD88: case 0xB9E1: case 0x11C62C: case 0x11C668: return true;
            case 0xE40D: case 0x11ACED: return false;
            }
            if (s.f[0x91] & 0x800000)
                return i == 0;
            break;
        case 8:
            switch (id)
            {
            case 0x16A74: case 0x7FAC: case 0x1433: case 0x1A76: case 0x7F85: case 0xE288: case 0xE289:
            case 0x16A73: case 0x114865: case 0xEED7E: case 0x10DD13: case 0x10E356: case 0x11488C:
            case 0x11AB68: case 0x11AB69:
                return true;
            }
            break;
        case 9:
            if (id == 0x851A || id == 0x114DFA)
                return true;
            break;
        case 0xB:
            if (id == 0x77F4)
                return false;
            break;
        }
        if (s.f[3] == 0x1D)
            return true;
        for (uint32_t k = 0; k < 3; ++k)
            if (AppliesAura(s, k))
            {
                if (s.f[0x5F + k] == 0x10)
                    return true;
                if (s.f[0x5F + k] == 0x56)
                    return false;
            }

        switch (s.f[0x47 + i])
        {
        case 3:
            if (id == 0x6F19)
                return false;
            break;
        case 6:
        case 0x80:
        {
            const uint32_t aura = s.f[0x5F + i];
            switch (aura)
            {
            case 0x122: case 0x39:
                if (Amount(s, i) > 0)
                    return true;
                break;
            case 3:
                if (s.f[0x56 + i] == 1)
                    return false;
                break;
            case 0xC:
                if (i == 0 && s.f[0x48] == 0 && s.f[0x49] == 0)
                    return false;
                break;
            case 0xD: case 0x1D: case 0x1E: case 0x31: case 0x4F: case 0x76: case 0x87: case 0x146:
                if (Amount(s, i) < 0)
                    return false;
                break;
            case 0xE:
                if (Amount(s, i) > 0)
                    return false;
                break;
            case 0x17: case 0xE3:
                if (!nested)
                {
                    SpellRec t;
                    if (t.Fetch(s.f[0x74 + i]))
                        for (uint32_t k = 0; k < 3; ++k)
                            if (t.f[0x47 + k] != 0 && FriendlyTargets(t, k) && !IsPositiveEffect(t, k, true))
                                return false;
                }
                break;
            case 0x1A: case 0x1B: case 0x35: case 0x44: case 0x59: case 0x5F:
                return false;
            case 0x21:
                if (s.f[0x56 + i] != 1)
                    return false;
                break;
            case 0x3C:
                return id == 0x60A4;
            case 0x4D:
                switch (s.f[0x6E + i])
                {
                case 0x10: case 0x13: case 0x15: case 0x19: return false;
                }
                break;
            case 0x6B: case 0x6C:
                if (s.f[0x6E + i] == 0xE && Amount(s, i) > 0 && !nested)
                {
                    uint32_t k = 0;
                    for (; k < 3; ++k)
                        if (k != i && IsPositiveEffect(s, k, true))
                            break;
                    if (k == 3)
                        return false;
                }
                break;
            case 0x6D:
                return true;
            case 0x13A:
                return false;
            }
            break;
        }
        case 0xA: case 0x24: case 0x2C: case 0x88: case 0x89:
            return true;
        case 0x81:
            return false;
        default:
            break;
        }
        if (!FriendlyTargets(s, i))
            return false;
        if (!nested && s.f[0x5F + i] == 0 && s.f[0x74 + i] != 0)
        {
            SpellRec t;
            if (t.Fetch(s.f[0x74 + i]))
                for (uint32_t k = 0; k < 3; ++k)
                    if (!IsPositiveEffect(t, k, true))
                        return false;
        }
        return true;
    }

    bool AllPositive(const SpellRec& s)   // FUN_10325940
    {
        for (uint32_t i = 0; i < 3; ++i)
            if (!IsPositiveEffect(s, i, true))
                return false;
        return true;
    }

    // handler_AppliesBuff / AppliesDebuff: an aura-applying effect, positive or not.
    int Applies(lua_State* L, bool buff)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        SpellRec s;
        const bool found = s.Fetch(id);
        const bool aura = AppliesAura(s, 0) || AppliesAura(s, 1) || AppliesAura(s, 2);
        const bool positive = found && AllPositive(s);
        PushBool(L, buff ? (aura && positive) : (aura && !positive));
        return 1;
    }
    int AppliesBuff(lua_State* L) { return Applies(L, true); }
    int AppliesDebuff(lua_State* L) { return Applies(L, false); }

    int IsHarmfulSpell(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        SpellRec s;
        PushBool(L, s.Fetch(id) && !AllPositive(s));
        return 1;
    }

    int IsHelpfulSpell(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        SpellRec s;
        PushBool(L, s.Fetch(id) && AllPositive(s));
        return 1;
    }

    // FUN_10325090: a fixed list.
    int IsImmunitySpell(lua_State* L)
    {
        switch (static_cast<uint32_t>(ToInt(CheckNumber(L, 1))))
        {
        case 0xB17E: case 0x3FE: case 0x282: case 0x2C6: case 0x48D7: case 0x83FA:
        case 0xC4D0A: case 0xC392B: case 0xC4BEB: case 0xC4DA6: case 0xC4DF2:
            PushBool(L, true);
            break;
        default:
            PushBool(L, false);
        }
        return 1;
    }

    // handler_IsTrainerSpell: any NPCTrainer.dbc row with +4 == spell.
    int IsTrainerSpell(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\NPCTrainer.dbc");
        bool found = false;
        for (uint32_t r = t.MinId(); t.Loaded() && r <= t.MaxId() && !found; ++r)
            if (const uint8_t* row = t.Row(r))
                found = AscDbc::Table::U32(row, 4) == id;
        PushBool(L, found);
        return 1;
    }

    int GetFirstRank(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        PushInt(L, static_cast<int32_t>(AscSpellRank::FirstRank(id)));
        return 1;
    }

    // FUN_10328a30(0) into the map at 0x10D3C000: spell (SkillLineAbility +8) -> its SkillLineAbility row,
    // over every row in id order (a later row replaces an earlier one).
    const uint32_t kSkillLineAbilityDbc = 0xAD4598;
    std::unordered_map<uint32_t, const uint8_t*> g_slaBySpell;   // 0x10D3C000
    bool g_slaBuilt = false;
    void BuildSkillLineAbility()
    {
        g_slaBuilt = true;
        g_slaBySpell.clear();
        const uint32_t minId = *reinterpret_cast<const uint32_t*>(kSkillLineAbilityDbc + 0x10);
        const uint32_t maxId = *reinterpret_cast<const uint32_t*>(kSkillLineAbilityDbc + 0x0C);
        for (uint32_t id = minId; id < maxId + 1; ++id)
            if (const uint8_t* row = ClientDbcRow(kSkillLineAbilityDbc, id))
                g_slaBySpell[AscDbc::Table::U32(row, 8)] = row;
    }
    const uint8_t* SkillLineAbilityOf(uint32_t spellId)
    {
        if (!g_slaBuilt)
            BuildSkillLineAbility();
        auto it = g_slaBySpell.find(spellId);
        return it == g_slaBySpell.end() ? nullptr : it->second;
    }

    // FUN_10324550: (spellId, level) -> the highest rank of the spell's chain whose spellLevel (Spell.dbc
    // +0x9C) the level reaches. A rank other than the spell itself needs the spell to be a class skill
    // (SkillLine category 7); nil otherwise, and without a player.
    int GetMaxLearnableRank(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return 0;
        const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint32_t level = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        if (!ActivePlayer())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        const std::vector<uint32_t> chain = AscSpellRank::Chain(AscSpellRank::FirstRank(spell));
        uint32_t best = 0;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            SpellRec rec;
            if (rec.Fetch(*it) && level >= rec.f[0x9C / 4])
            {
                best = *it;
                break;
            }
        }
        if (best && best != spell)
        {
            const uint8_t* sla = SkillLineAbilityOf(spell);
            const uint8_t* line = sla ? ClientDbcRow(0xAD45E0, AscDbc::Table::U32(sla, 4)) : nullptr;
            if (!line || AscDbc::Table::U32(line, 4) != 7)
                best = 0;
        }
        if (best)
            PushInt(L, static_cast<int32_t>(best));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Spell", "GetMaxLearnableRank", GetMaxLearnableRank},
        {"C_Spell", "AppliesBuff", AppliesBuff},
        {"C_Spell", "AppliesDebuff", AppliesDebuff},
        {"C_Spell", "IsHarmfulSpell", IsHarmfulSpell},
        {"C_Spell", "IsHelpfulSpell", IsHelpfulSpell},
        {"C_Spell", "IsImmunitySpell", IsImmunitySpell},
        {"C_Spell", "IsTrainerSpell", IsTrainerSpell},
        {"C_Spell", "GetFirstRank", GetFirstRank},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), nullptr);
}

// FUN_10325130 for callers in other files (e.g. FUN_10325940): `rec` is a whole Spell.dbc record.
bool AscSpellApi_IsPositiveEffect(const uint8_t* rec, uint32_t i, bool nested)
{
    SpellRec s;
    memcpy(s.f, rec, sizeof s.f);
    return IsPositiveEffect(s, i, nested);
}

// FUN_10328a30(spell) for SMSG 0x934: 0 rebuilds the whole map; otherwise the FIRST row (by id) naming the
// spell is stored for it.
void AscSpellApi_SkillLineAbilityUpdate(uint32_t spell)
{
    if (spell == 0)
    {
        BuildSkillLineAbility();
        return;
    }
    if (!g_slaBuilt)
        BuildSkillLineAbility();
    const uint32_t minId = *reinterpret_cast<const uint32_t*>(kSkillLineAbilityDbc + 0x10);
    const uint32_t maxId = *reinterpret_cast<const uint32_t*>(kSkillLineAbilityDbc + 0x0C);
    for (uint32_t id = minId; id < maxId + 1; ++id)
        if (const uint8_t* row = ClientDbcRow(kSkillLineAbilityDbc, id))
            if (AscDbc::Table::U32(row, 8) == spell)
            {
                g_slaBySpell[spell] = row;
                return;
            }
}
// FUN_103266c0: the spell's entry is erased.
void AscSpellApi_SkillLineAbilityErase(uint32_t spell)
{
    if (!g_slaBuilt)
        BuildSkillLineAbility();
    g_slaBySpell.erase(spell);
}
