// The original's spell-modifier installer FUN_10324a90: server-driven spell modifiers, a few aura-type
// gates on casting, and the transient code patches around a spell cast. Everything it installs:
//
//   Lua        FUN_10328ff0 (C_Spell) / FUN_10328f00 (C_Aura) -- already registered by the bindings.
//   SMSG 0x64F handler_0x064F: u32 n, then n x 5 u32 -> 0x805B10(v0, v1, v2, v3, v4 & 0x7FFFFFFF,
//              v4 >> 31, 0).
//   0x7FDC60 -> 0x10325B70  REPLACED. SMSG_SET_FLAT / _PCT_SPELL_MODIFIER (0x266 = flat): u8 isFamily,
//              u8 bit. isFamily: u32 value, u32 spell -> FUN_10328ac0 (below). Otherwise: u8 op, u32 value,
//              u32 family -> table 0x10BE44A8 (flat) / 0x10C90228 (pct) at [bit * 0x1F + family * 0x11A0 +
//              op], unchecked.
//   0x7FD970 -> 0x103288A0  REPLACED. The modifier sum for (spell, op): both tables over the 96 family
//              bits of the spell (+0x244) for its family (+0x240, index < 0x2AF60), plus the per-spell maps
//              0x10D3BFB4 / 0x10D3BFD4 (FUN_10324870 / FUN_10324920). +0x1C bit 29 opts out.
//   0x8012F0 -> 0x10328CB0 / 0x7FF100 -> 0x10328DB0  unit addon float scaling (field power * 7 + 7 +
//              school) and, for 0x7FF100, SpellCustomAttr +0x18 & 0x20000 -> 0x71B770 * spell[0x2C] / 100.
//   0x7FF180 -> 0x103284E0  after the original: the player's type-0x15D SpellAddon effects scale the result
//              by (100 - amount) / 100 (amounts from SMSG 0x673 / 0x674, AscAuraAmounts).
//   0x802C30 -> 0x103287F0  after the original: *arg3 = 0 when flag 0x10BE44A0 is set or the spell passes
//              aura-type check 0x138.
//   0x71AF90 -> 0x10324390 / 0x71B820 -> FUN_10324300  gates for the pending spell 0x4B (Auto Shot).
//   0x80CCE0 -> FUN_103273F0  code patches around the cast: 0x73A0D0 (FUN_10323d50), 0x7370AF (aura
//              0x4E), 0x80D6EF..0x80D711 + 0x808323 (Sound_EnableErrorSpeech).
//   0x806200 -> FUN_10327240  Spell_C_CancelSpell's null-cast-link assert, and the 0x139 aura skip.
//   Lists     FUN_10278760(0x103243F0) clears everything at the glue screen; FUN_10278430(0x10325AA0) /
//              FUN_10278620(0x10325B50) set / clear flag 0x10BE44A0 (FUN_10326ff0) before 0x724820 /
//              0x71E930; FUN_102783D0(0x103267E0, 0x10326780) veto 0x80B5D0.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscCrashContext.hpp>
#include <Ascension/AscManastorm.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Client/CVar.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <intrin.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

const int32_t* AscAuraAmounts_Find(uint64_t unit, uint64_t caster, uint32_t spell);
const uint8_t* AscPatchData_SpellAddonForSpell(uint32_t spell);

namespace
{
    const uint32_t kTableSize = 0x2AF60;
    int32_t g_flat[kTableSize];   // 0x10BE44A8
    int32_t g_pct[kTableSize];    // 0x10C90228
    typedef std::unordered_map<uint32_t, std::unordered_map<uint8_t, int32_t>> FamilyMap;
    FamilyMap g_spellFlat;        // 0x10D3BFB4
    FamilyMap g_spellPct;         // 0x10D3BFD4
    bool g_auraFlag = false;      // 0x10BE44A0

    uint32_t U32(const void* p, uint32_t off) { return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + off); }
    int32_t I32(const void* p, uint32_t off) { return *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(p) + off); }
    uint8_t* ActivePlayer() { return reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)(); }
    const uint8_t* Descriptor(const void* object) { return *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(object) + 8); }
    uint64_t Guid(const void* object) { const uint8_t* d = Descriptor(object); return U32(d, 0) | static_cast<uint64_t>(U32(d, 4)) << 32; }
    uint32_t PendingSpell() { return *reinterpret_cast<const uint32_t*>(0xD397D0); }   // *DAT_10BCC4B0

    bool HasAuraType(void* unit, uint32_t type, const uint8_t* rec)   // 0x7283A0
    {
        return reinterpret_cast<char(__thiscall*)(void*, uint32_t, const uint8_t*)>(0x7283A0)(unit, type, rec) != 0;
    }
    int HasAura(void* unit, uint32_t spell)   // 0x7282A0
    {
        return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x7282A0)(unit, spell);
    }
    bool IsPlayer(void* unit) { return reinterpret_cast<char(__thiscall*)(void*)>(0x4CEE50)(unit) != 0; }
    uint32_t UnitField(void* unit, uint32_t n) { return U32(Descriptor(unit), (n + 0xA2) * 8); }   // FUN_1008d8a0
    uint32_t AuraCount(void* unit) { return reinterpret_cast<uint32_t(__thiscall*)(void*)>(0x4F8850)(unit); }
    const uint8_t* AuraAt(void* unit, uint32_t i) { return reinterpret_cast<const uint8_t*(__thiscall*)(void*, uint32_t)>(0x556E10)(unit, i); }
    float UnitAddonFloat(const void* unit, uint32_t index)   // FUN_100d0a30
    {
        return AscObjectAddon::AsFloat(AscObjectAddon::Unit(Guid(unit))[index & 0xFFFF]);
    }

    void Patch(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
    }
    void PatchByte(uint32_t at, uint8_t b) { Patch(at, &b, 1); }

    // FUN_10323d50: NOP 0x73A0D0..0x73A0EF while on, the stock bytes when off.
    void SetCastPatch(bool on)
    {
        static const uint8_t kStock[0x20] = {0x8B, 0x45, 0x08, 0x6A, 0x00, 0x6A, 0xFF, 0x6A, 0xFF, 0x6A, 0x33, 0x56, 0x50, 0xE8, 0x1E, 0xE1,
                                             0x0C, 0x00, 0x83, 0xC4, 0x18, 0x5B, 0x5F, 0x32, 0xC0, 0x5E, 0x8B, 0xE5, 0x5D, 0xC2, 0x0C, 0x00};
        uint8_t nop[0x20];
        memset(nop, 0x90, sizeof nop);
        Patch(0x73A0D0, on ? nop : kStock, 0x20);
    }

    // FUN_10326ff0: the jne at 0x80DB37 becomes a jmp while on, and SpellRange.dbc rows 0x72 / 0x1BD (client
    // container 0xAD4988) get their flags (+0x14) = 0 (on) / 2 (off).
    void SetAuraFlag(bool on)
    {
        PatchByte(0x80DB37, on ? 0xEB : 0x75);
        for (uint32_t id : {0x72u, 0x1BDu})   // FUN_100b2590
        {
            const uint32_t rows = *reinterpret_cast<const uint32_t*>(0xAD49A8);
            const uint32_t minId = *reinterpret_cast<const uint32_t*>(0xAD4998), maxId = *reinterpret_cast<const uint32_t*>(0xAD4994);
            if (rows && id >= minId && id <= maxId)
                if (uint8_t* row = *reinterpret_cast<uint8_t**>(rows + (id - minId) * 4))
                    *reinterpret_cast<uint32_t*>(row + 0x14) = on ? 0 : 2;
        }
        g_auraFlag = on;
    }

    // FUN_10325020: an effect applies aura 0x138, and some effect has +0x1E8 / +0x1F4 / +0x200 all zero and
    // +0x1B8 == 1.
    bool IsAuraFlagSpell(const uint8_t* rec)
    {
        if (!rec)
            return false;
        bool aura = false;
        for (uint32_t i = 0; i < 3 && !aura; ++i)
            aura = U32(rec, 0x17C + i * 4) == 0x138;
        if (!aura)
            return false;
        for (uint32_t i = 0; i < 3; ++i)
            if (U32(rec, 0x1E8 + i * 4) == 0 && U32(rec, 0x1F4 + i * 4) == 0 && U32(rec, 0x200 + i * 4) == 0 && U32(rec, 0x1B8 + i * 4) == 1)
                return true;
        return false;
    }

    bool __cdecl AuraFlagOn(void*, uint32_t, uint32_t rec)   // 0x10325AA0, before 0x724820
    {
        if (IsAuraFlagSpell(reinterpret_cast<const uint8_t*>(rec)))
            SetAuraFlag(true);
        return true;
    }
    bool __cdecl AuraFlagOff(void*, uint32_t, uint32_t rec)   // 0x10325B50, before 0x71E930
    {
        if (IsAuraFlagSpell(reinterpret_cast<const uint8_t*>(rec)))
            SetAuraFlag(false);
        return true;
    }

    // 0x103267E0 (after 0x80B5D0): a spell flagged SpellCustomAttr +0x18 & 0x2000 with a required aura
    // spell (+0x60) needs the unit's aura of that spell (0x7289C0 slot, 0x556E10) at +0xE stacks >= the
    // required spell's StackAmount (+0xC4). An unknown required spell or missing aura vetoes.
    bool __cdecl StackRequirement(uint32_t unitArg, uint32_t recArg, uint32_t, uint32_t, uint32_t)
    {
        void* unit = reinterpret_cast<void*>(unitArg);
        const uint8_t* rec = reinterpret_cast<const uint8_t*>(recArg);
        if (!rec)
            return true;
        const uint32_t required = U32(rec, 0x60);
        if (required == 0 && U32(rec, 0x64) == 0)
            return true;
        const uint8_t* attr = AscCA::SpellCustomAttrRow(U32(rec, 0));
        if (!attr || !(U32(attr, 0x18) & 0x2000) || required == 0 || !unit)
            return true;
        uint8_t req[0x2B0];
        if (!AscScript::FetchSpell(required, req))
            return false;
        const uint32_t slot = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(0x7289C0)(unit, U32(rec, 0x60));
        const uint8_t* aura = AuraAt(unit, slot);
        if (!aura)
            return false;
        return *reinterpret_cast<const uint16_t*>(aura + 0xE) >= U32(req, 0xC4);
    }

    // 0x10326780 (after 0x80B5D0): SpellCustomAttr +0x18 & 0x4000 needs aura 0xC3849 on the unit.
    bool __cdecl AuraRequirement(uint32_t unitArg, uint32_t recArg, uint32_t, uint32_t, uint32_t)
    {
        const uint8_t* rec = reinterpret_cast<const uint8_t*>(recArg);
        if (!rec)
            return true;
        const uint8_t* attr = AscCA::SpellCustomAttrRow(U32(rec, 0));
        if (!attr || !(U32(attr, 0x18) & 0x4000) || !unitArg)
            return true;
        return HasAura(reinterpret_cast<void*>(unitArg), 0xC3849) != 0;
    }

    // 0x103243F0, at the glue screen.
    void ClearModifiers()
    {
        memset(g_flat, 0, sizeof g_flat);
        memset(g_pct, 0, sizeof g_pct);
        g_spellFlat.clear();   // FUN_101cf290
        g_spellPct.clear();
    }

    // FUN_10324870 / FUN_10324920
    int32_t SpellModifier(const FamilyMap& m, const uint8_t* rec, uint32_t op)
    {
        auto it = m.find(U32(rec, 0));
        if (it == m.end())
            return 0;
        auto jt = it->second.find(static_cast<uint8_t>(op));
        return jt != it->second.end() ? jt->second : 0;
    }

    // FUN_10328ac0: a family (per-spell) modifier. Every spell SpellAffect names for `spell` (FUN_10215950:
    // +8 itself, or when negative every rank of the chain whose first rank is -value) gets value added
    // to its current entry for `op`.
    void AddSpellModifier(uint32_t op, int32_t value, uint32_t spell, bool flat)
    {
        uint8_t rec[0x2B0];
        if (!AscScript::FetchSpell(spell, rec))
            return;
        FamilyMap& m = flat ? g_spellFlat : g_spellPct;
        for (const auto& affect : AscBridgeStore::SpellAffectsFor(spell))
        {
            const int32_t target = static_cast<int32_t>(affect[2]);
            std::vector<uint32_t> spells;
            if (target >= 0)
                spells.push_back(static_cast<uint32_t>(target));
            else
                spells = AscSpellRank::Chain(static_cast<uint32_t>(target < 0 ? -target : target));
            for (uint32_t s : spells)
            {
                uint8_t affected[0x2B0];
                if (!AscScript::FetchSpell(s, affected))
                    continue;
                const int32_t total = value + SpellModifier(m, affected, op);
                m[s][static_cast<uint8_t>(op)] = total;
            }
        }
    }

    uint8_t ReadU8(CDataStore* p) { const uint8_t v = static_cast<uint8_t>(p->m_buffer[p->m_read]); p->m_read += 1; return v; }
    uint32_t ReadU32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }

    // 0x10325B70, replacing 0x7FDC60.
    int __cdecl SetSpellModifier(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        const uint8_t isFamily = ReadU8(p);
        const uint8_t bit = ReadU8(p);
        if (isFamily)
        {
            const uint32_t value = ReadU32(p);
            const uint32_t spell = ReadU32(p);
            AddSpellModifier(bit, static_cast<int32_t>(value), spell, opcode == 0x266);
            return 1;
        }
        const uint8_t op = ReadU8(p);
        const uint32_t value = ReadU32(p);
        const uint32_t family = ReadU32(p);
        const uint32_t index = bit * 0x1F + family * 0x11A0 + op;   // unchecked, as the original
        (opcode == 0x266 ? g_flat : g_pct)[index] = static_cast<int32_t>(value);
        return 1;
    }

    // 0x103288A0, replacing 0x7FD970.
    uint32_t __cdecl SpellModifierSum(const uint8_t* rec, uint32_t op, int32_t* flat, int32_t* pct)
    {
        // The false returns keep whatever the register held: the pct pointer, the flat pointer, or 0.
        if (!rec || U32(rec, 0x240) == 0)
        {
            *flat = 0;
            *pct = 100;
            return reinterpret_cast<uintptr_t>(pct) & ~0xFFu;
        }
        if ((U32(rec, 0x1C) >> 29) & 1)
        {
            *flat = 0;
            *pct = 100;
            return reinterpret_cast<uintptr_t>(flat) & ~0xFFu;
        }
        *pct = 0;
        *flat = 0;
        for (uint32_t bit = 0; bit < 96; ++bit)
        {
            if (!(U32(rec, 0x244 + (bit >> 5) * 4) & (1u << (bit & 0x1F))))
                continue;
            const uint32_t index = U32(rec, 0x240) * 0x11A0 + bit * 0x1F + (op & 0xFF);
            *flat += index < kTableSize ? g_flat[index] : 0;
            *pct += index < kTableSize ? g_pct[index] : 0;
        }
        *flat += SpellModifier(g_spellFlat, rec, op);
        *pct += SpellModifier(g_spellPct, rec, op);
        if (*flat == 0 && *pct == 0)
            return 0;
        const int32_t v = *pct + 100;
        *pct = v < 0 ? 0 : v;
        return ((static_cast<uint32_t>(*pct) >> 8) & 7) * 10 | 1;
    }

    typedef uint32_t(__cdecl* C2_t)(const void*, void*);
    typedef uint32_t(__cdecl* C4_t)(const void*, uint32_t, uint32_t, uint32_t);
    typedef uint32_t(__cdecl* C5_t)(void*, uint32_t, float*, uint32_t, uint32_t);
    typedef uint32_t(__cdecl* C7_t)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    typedef uint32_t(__fastcall* This0_t)(void*, void*);
    typedef uint32_t(__fastcall* This1_t)(void*, void*, uint32_t);
    C2_t g_7FF100 = nullptr, g_8012F0 = nullptr;
    C4_t g_7FF180 = nullptr, g_806200 = nullptr;
    C5_t g_802C30 = nullptr;
    C7_t g_80CCE0 = nullptr;
    This0_t g_71AF90 = nullptr;
    This1_t g_71B820 = nullptr;

    // Unit addon float at power type (+0x254) * 7 + 7 + school (+0xA4), plus 1, times the value.
    uint32_t ScaleBySchool(const uint32_t* rec, const void* unit, uint32_t r)
    {
        const float f = UnitAddonFloat(unit, rec[0x95] * 7 + 7 + rec[0x29]);
        const float product = static_cast<float>((static_cast<double>(f) + 1.0) * static_cast<float>(static_cast<int32_t>(r)));
        return static_cast<uint32_t>(static_cast<int32_t>(product));
    }

    uint32_t __cdecl Detour7FF100(const uint32_t* rec, void* unit)   // 0x10328DB0
    {
        if (unit)
        {
            const uint8_t* attr = AscCA::SpellCustomAttrRow(rec[0]);
            if (attr && (U32(attr, 0x18) & 0x20000))
            {
                const uint32_t v = reinterpret_cast<uint32_t(__thiscall*)(void*, uint32_t)>(0x71B770)(unit, rec[0x29]);
                return (rec[0x2C] * v) / 100;
            }
        }
        uint32_t r = g_7FF100(rec, unit);
        if (r && rec && unit && rec[0x95] < 4 && rec[0x29] < 7)
            r = ScaleBySchool(rec, unit, r);
        return r;
    }

    uint32_t __cdecl Detour8012F0(const uint32_t* rec, void* unit)   // 0x10328CB0
    {
        uint32_t r = g_8012F0(rec, unit);
        if (r && rec && unit)
        {
            if (rec[0x95] <= 3 && rec[0x29] < 7)
            {
                r = ScaleBySchool(rec, unit, r);
                if (r == 0)
                    return 0;
            }
        }
        else
        {
            if (r == 0)
                return 0;
            if (!unit)
                return r;
        }
        // 0x7FF180 is called through its hooked entry, as the original does.
        if (reinterpret_cast<uint32_t(__cdecl*)(const void*, uint32_t, uint32_t, uint32_t)>(0x7FF180)(rec, 0, 0, 0) == 0 && rec[0x29] == 0)
        {
            const float f = UnitAddonFloat(unit, 6);   // FUN_102cdcf0 +0x48
            return static_cast<uint32_t>(static_cast<int32_t>((1.0f - f) * static_cast<float>(static_cast<int32_t>(r))));
        }
        return r;
    }

    uint32_t __cdecl Detour7FF180(const uint8_t* rec, uint32_t b, uint32_t c, uint32_t d)   // 0x103284E0
    {
        uint32_t r = g_7FF180(rec, b, c, d);
        uint8_t* player = ActivePlayer();
        if (!player)
            return r;
        for (uint32_t i = 0; i < AuraCount(player); ++i)
        {
            const uint8_t* aura = AuraAt(player, i);
            if (!aura)
                continue;
            uint8_t auraRec[0x2B0];
            if (!AscScript::FetchSpell(U32(aura, 8), auraRec))
                continue;
            const uint8_t* addon = AscPatchData_SpellAddonForSpell(U32(aura, 8));   // map 0x10BE0708
            if (!addon)
                continue;
            const uint64_t caster = U32(aura, 0) | static_cast<uint64_t>(U32(aura, 4)) << 32;
            const int32_t* amount = AscAuraAmounts_Find(Guid(player), caster, U32(aura, 8));
            if (!amount)
                continue;
            for (uint32_t j = 0; j < 3; ++j)
                if (U32(addon, 0x50 + j * 4) == 0x15D && (U32(rec, 0x284) & U32(auraRec, 0x1B8 + j * 4)))
                {
                    const float scale = 100.0f - static_cast<float>(amount[j]);
                    const float value = static_cast<float>(static_cast<double>(r)) * scale / 100.0f;
                    r = static_cast<uint32_t>(static_cast<int64_t>(value));   // FUN_10ae6220
                }
        }
        return r;
    }

    uint32_t __cdecl Detour802C30(void* unit, uint32_t spell, float* value, uint32_t d, uint32_t e)   // 0x103287F0
    {
        const uint32_t r = g_802C30(unit, spell, value, d, e);
        if (!unit || !value || *value <= 0.0f)
            return r;
        if (!g_auraFlag)
        {
            uint8_t rec[0x2B0];
            if (!AscScript::FetchSpell(spell, rec) || !HasAuraType(unit, 0x138, rec))
                return r;
        }
        *value = 0.0f;
        return r;
    }

    // FUN_10324270: flag 0x10BE44A0, or the pending spell passes aura-type check 0x138 on the unit.
    bool PendingSpellFlagged(void* unit)
    {
        if (g_auraFlag)
            return true;
        uint8_t rec[0x2B0];
        return AscScript::FetchSpell(PendingSpell(), rec) && HasAuraType(unit, 0x138, rec);
    }

    uint32_t __fastcall Detour71AF90(void* unit, void* edx)   // 0x10324390
    {
        if (PendingSpell() == 0x4B)
        {
            if (AscManastorm::InManastorm() || PendingSpellFlagged(unit))
                return 1;
            if (IsPlayer(unit) && UnitField(unit, 0xF) == 0)
                return 1;
        }
        return g_71AF90(unit, edx);
    }

    uint32_t __fastcall Detour71B820(void* unit, void* edx, uint32_t a)   // FUN_10324300
    {
        if (IsPlayer(unit) && UnitField(unit, 0xF) == 0 && UnitField(unit, 0x11) != 0)
            return 0;
        if (PendingSpell() == 0x4B)
        {
            if (AscManastorm::InManastorm() || PendingSpellFlagged(unit))
                return 0;
            if (IsPlayer(unit) && UnitField(unit, 0xF) == 0)
                return 0;
        }
        return g_71B820(unit, edx, a);
    }

    // FUN_10327240 over Spell_C_CancelSpell. A null cast link is never passed on; from 0x809AB9 it is the
    // original's fatal assert. From 0x8066DA, a spell the player passes aura-type check 0x139 for is kept.
    uint32_t __cdecl Detour806200(const uint8_t* link, uint32_t b, uint32_t c, uint32_t d)
    {
        const uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (!link)
        {
            if (from == 0x809AB9)
                if (uint8_t* player = ActivePlayer())
                {
                    const std::string message = "Spell_C_CancelSpell: spellCastLink = nullptr but GetUICastingSpellID = " +
                                                std::to_string(I32(player, 0xA60)) + ".";
                    AscCrashContext::Assert(message.c_str(), nullptr, 0);
                }
            return 0;
        }
        if (from == 0x8066DA)
            if (uint8_t* player = ActivePlayer())
            {
                uint8_t rec[0x2B0];
                if (AscScript::FetchSpell(U32(link, 0x20), rec) && HasAuraType(player, 0x139, rec))
                    return 0;
            }
        return g_806200(link, b, c, d);
    }

    // FUN_103273F0 over 0x80CCE0 (unit, spell, ...): the cast's code patches, the original, then the cast
    // patch and 0x7370AF back to stock. 0x80D6EF / 0x808323 are left as set.
    uint32_t __cdecl Detour80CCE0(void* unit, uint32_t spell, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g)
    {
        uint8_t rec[0x2B0];
        if (AscScript::FetchSpell(spell, rec))
        {
            if (HasAuraType(unit, 0x139, rec))
                SetCastPatch(true);
            for (uint32_t i = 0; i < 3; ++i)
                if (U32(rec, 0x17C + i * 4) == 0x4E)
                    PatchByte(0x7370AF, 0xEB);
            const uint32_t family = U32(rec, 0x240);
            if (family == 5)
            {
                if ((rec[0x244] & 0x64) || (rec[0x248] & 0xC0) || (U32(rec, 0x24C) & 0x200))
                {
                    if (HasAura(unit, 0x7B9B))
                        SetCastPatch(true);
                }
                else
                    goto errorSpeech;
            }
            if (family == 3 && (U32(rec, 0x244) & 0x400015) && HasAura(unit, 0x7B9B))
                SetCastPatch(true);
        }
    errorSpeech:
        if (U32(unit, 0xA6C) != 0)
        {
            CVar* cvar = CVar::Lookup("Sound_EnableErrorSpeech");
            if (!cvar || *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(cvar) + 0x30) != 0)
            {
                static const uint8_t kStock[0x23] = {0xF6, 0x85, 0x58, 0xFD, 0xFF, 0xFF, 0x10, 0x6A, 0x00, 0x6A, 0x00, 0x6A,
                                                     0x00, 0x74, 0x07, 0x68, 0xA4, 0x66, 0xA1, 0x00, 0xEB, 0x05, 0x68, 0xE4,
                                                     0x3A, 0xA4, 0x00, 0xE8, 0x91, 0x9D, 0xCB, 0xFF, 0x83, 0xC4, 0x10};
                Patch(0x80D6EF, kStock, sizeof kStock);
                PatchByte(0x808323, 0x74);
            }
            else
            {
                uint8_t nop[0x23];
                memset(nop, 0x90, sizeof nop);
                Patch(0x80D6EF, nop, sizeof nop);
                PatchByte(0x808323, 0xEB);
            }
        }
        const uint32_t r = g_80CCE0(unit, spell, c, d, e, f, g);
        SetCastPatch(false);
        PatchByte(0x7370AF, 0x74);
        return r;
    }

    // handler_0x064F
    void __cdecl OnSpellVisual(void*, uint32_t, uint32_t, CDataStore* p)
    {
        for (uint32_t n = ReadU32(p); n != 0; --n)
        {
            const uint32_t v0 = ReadU32(p), v1 = ReadU32(p), v2 = ReadU32(p), v3 = ReadU32(p), v4 = ReadU32(p);
            reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(0x805B10)(
                v0, v1, v2, v3, v4 & 0x7FFFFFFF, v4 >> 31, 0);
        }
    }

    void Init()
    {
        sDC.AddPacketHandler(0x64F, CNetClientCustomPacket((void*)&OnSpellVisual, nullptr));
        AscRuntime::ReplaceFunction(0x7FDC60, reinterpret_cast<void*>(&SetSpellModifier));
        AscRuntime::ReplaceFunction(0x7FD970, reinterpret_cast<void*>(&SpellModifierSum));
        g_8012F0 = reinterpret_cast<C2_t>(AscRuntime::Detour(0x8012F0, 9, reinterpret_cast<void*>(&Detour8012F0)));
        g_7FF100 = reinterpret_cast<C2_t>(AscRuntime::Detour(0x7FF100, 7, reinterpret_cast<void*>(&Detour7FF100)));
        AscRuntime::OnGlueScreen(&ClearModifiers);
        g_7FF180 = reinterpret_cast<C4_t>(AscRuntime::Detour(0x7FF180, 6, reinterpret_cast<void*>(&Detour7FF180)));
        g_802C30 = reinterpret_cast<C5_t>(AscRuntime::Detour(0x802C30, 9, reinterpret_cast<void*>(&Detour802C30)));
        g_71B820 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x71B820, 6, reinterpret_cast<void*>(&Detour71B820)));
        g_71AF90 = reinterpret_cast<This0_t>(AscRuntime::Detour(0x71AF90, 6, reinterpret_cast<void*>(&Detour71AF90)));
        AscRuntime::OnBefore724820(&AuraFlagOn, 0x10324D9A);
        AscRuntime::OnBefore71E930(&AuraFlagOff, 0x10324DA4);
        AscRuntime::OnAfter80B5D0(&StackRequirement, 0x10324DAE);
        AscRuntime::OnAfter80B5D0(&AuraRequirement, 0x10324DB8);
        g_80CCE0 = reinterpret_cast<C7_t>(AscRuntime::Detour(0x80CCE0, 9, reinterpret_cast<void*>(&Detour80CCE0)));
        g_806200 = reinterpret_cast<C4_t>(AscRuntime::Detour(0x806200, 9, reinterpret_cast<void*>(&Detour806200)));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
