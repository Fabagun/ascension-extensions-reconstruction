// The original's spell-visual installer FUN_10322fb0: server-assigned spell visuals per object, the
// hideOtherPlayerHarmfulSpellVisuals option, and the report of a unit's spell event to the server.
//
//   Map        FUN_10323200 (0x10BE447C): guid kind (FUN_102cdc10) -> guid counter (FUN_100a03b0) -> spell
//              -> SpellVisual id. SMSG 0x662 (FUN_10322ee0): u64 guid, u32 spell, u32 visual.
//   Resolve    FUN_10322c70(kind, guid, spell): -1 when list 0x10BE2D98 hides it (FUN_10277870); for the
//              active player the applied appearance's visual (AscAppearance); else the map; else 0.
//   0x720F80 -> 0x10322B50  the object's spell visual record (a2, 0x80 bytes) after the original: zeroed
//              for -1, replaced by the resolved SpellVisual row. From 0x80ED4E, with the option on, another
//              player's harmful spell (FUN_10325940 false) returns 0 without calling the original.
//   0x702DB0 -> 0x10322AD0  the visual record argument (a3) is replaced before the original (null for -1).
//   0x705100 -> 0x10322760  with the option on, a dynamic object cast by another player of a harmful
//              spell is not created (the original is skipped).
//   0x7022D0 -> 0x10322810  after the original: CMSG 0x9C7 for our own events and those of units no other
//              player owns (+0x30 / +0x38 / +0x40).
//   Lists      0x10323300 (0x10BE2B6C, before 0x724820) patches the spell record's visual (+0x20C) and
//              keeps spell 0x31029 off the player and our own summons; 0x10323480 (0x10BE2D78) removes, and
//              0x10323410 (0x10BE2D98) hides, effects of spells flagged SpellCustomAttr +0x14 & 8 on players
//              and others' harmful effects (FUN_103233a0).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Client/CVar.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <intrin.h>
#include <unordered_map>

bool AscAppearance_PlayerSpellVisual(uint32_t spell, uint32_t& visual);
bool AscSpellApi_IsPositiveEffect(const uint8_t* rec, uint32_t i, bool nested);

namespace
{
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>>> g_visuals;   // 0x10BE447C

    uint32_t U32(const void* p, uint32_t off) { return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + off); }
    const uint8_t* Descriptor(const void* object) { return *reinterpret_cast<uint8_t* const*>(static_cast<const uint8_t*>(object) + 8); }
    uint64_t Guid64(uint32_t lo, uint32_t hi) { return lo | static_cast<uint64_t>(hi) << 32; }
    uint64_t DescGuid(const uint8_t* desc, uint32_t off) { return Guid64(U32(desc, off), U32(desc, off + 4)); }
    uint64_t ActiveGuid() { return AscScript::ActivePlayerGuid(); }
    void* ObjectByGuid(uint64_t guid, uint32_t mask)
    {
        return reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), mask);
    }
    bool IsPlayerGuid(uint64_t guid) { return guid != 0 && static_cast<uint32_t>(guid >> 32) < 0x10000000; }   // FUN_102ea1c0
    const uint8_t* SpellVisualRow(uint32_t id)   // FUN_100b2620: SpellVisual.dbc (0xAD4AA8)
    {
        const uint32_t rows = *reinterpret_cast<const uint32_t*>(0xAD4AC8);
        const uint32_t minId = *reinterpret_cast<const uint32_t*>(0xAD4AB8), maxId = *reinterpret_cast<const uint32_t*>(0xAD4AB4);
        if (!rows || id < minId || id > maxId)
            return nullptr;
        return *reinterpret_cast<const uint8_t* const*>(rows + (id - minId) * 4);
    }
    bool OptionOn()   // DAT_10BE4478 +0x30
    {
        static CVar* cvar = nullptr;
        if (!cvar)
            cvar = CVar::Lookup("hideOtherPlayerHarmfulSpellVisuals");
        return cvar && *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(cvar) + 0x30) != 0;
    }

    // FUN_102cdc10
    uint32_t GuidKind(uint32_t lo, uint32_t hi)
    {
        (void)lo;
        if (hi < 0x10000000)
            return 4;
        switch (hi >> 16)
        {
        case 0xF100: return 6;
        case 0x1FC0: return 5;
        case 0x4000: return 1;
        case 0xF101: return 7;
        case 0xF110: return 5;
        case 0xF130: case 0xF140: case 0xF150: return 3;
        default: return 0;
        }
    }

    // FUN_100a03b0: the GUID's counter -- the low 24 bits for creature-like kinds.
    uint32_t GuidCounter(uint32_t lo, uint32_t hi)
    {
        if (hi < 0x10000000)
            return lo;
        switch (hi >> 16)
        {
        case 0xF101: case 0x1FC0: case 0x1F40: case 0x1F50: case 0x4000: case 0xF100:
            return lo;
        default:
            return lo & 0xFFFFFF;
        }
    }

    // FUN_10322c70
    uint32_t ResolveVisual(uint32_t kind, uint32_t lo, uint32_t hi, uint32_t spell)
    {
        if (AscRuntime::VisualHidden(kind, lo, hi, spell))
            return 0xFFFFFFFF;
        uint32_t visual;
        if (kind == 4 && Guid64(GuidCounter(lo, hi), 0) == ActiveGuid()   // FUN_100a55d0(out, 0, counter)
            && AscAppearance_PlayerSpellVisual(spell, visual))
            return visual;
        auto k = g_visuals.find(kind);
        if (k == g_visuals.end())
            return 0;
        auto c = k->second.find(GuidCounter(lo, hi));
        if (c == k->second.end())
            return 0;
        auto s = c->second.find(spell);
        return s != c->second.end() ? s->second : 0;
    }

    // FUN_10325940 -> FUN_10325130(rec, i, 1) for every effect: the spell is harmless.
    bool IsHelpful(const uint8_t* rec)
    {
        for (uint32_t i = 0; i < 3; ++i)
            if (!AscSpellApi_IsPositiveEffect(rec, i, true))
                return false;
        return true;
    }

    // FUN_103233a0: a spell flagged SpellCustomAttr +0x18 & 0x40 on an object that is not us, nor (for a
    // unit) charmed or summoned by us.
    bool OthersFlaggedEffect(const void* object, uint32_t spell)
    {
        const uint8_t* attr = AscCA::SpellCustomAttrRow(spell);   // FUN_10324a50
        if (!attr || !(U32(attr, 0x18) & 0x40))
            return false;
        const uint8_t* desc = Descriptor(object);
        if (DescGuid(desc, 0) == ActiveGuid())
            return false;
        if (U32(object, 0x14) == 3 && (DescGuid(desc, 0x30) == ActiveGuid() || DescGuid(desc, 0x38) == ActiveGuid()))
            return false;
        return true;
    }

    // The shared body of 0x10323410 / 0x10323480: flag 8 hides the spell on a player (type 4 / 3 without
    // +0x60, or type 7) outright; otherwise FUN_103233a0 decides.
    bool HideFor(const void* object, uint32_t spell)
    {
        const uint8_t* attr = AscCA::SpellCustomAttrRow(spell);   // FUN_10324a10(spell, 8)
        if (attr && (U32(attr, 0x14) & 8))
        {
            const uint32_t type = U32(object, 0x14);
            if (type == 4 || type == 3)
            {
                if (U32(Descriptor(object), 0x60) == 0)
                    return true;
            }
            else if (type == 7)
                return true;
        }
        return OthersFlaggedEffect(object, spell);
    }

    bool __cdecl HideVisual(uint32_t, uint32_t lo, uint32_t hi, uint32_t spell)   // 0x10323410
    {
        void* object = ObjectByGuid(Guid64(lo, hi), 0x10);
        return object && HideFor(object, spell);
    }

    bool __cdecl RemoveEffect(void* unit, void* effect)   // 0x10323480
    {
        return HideFor(unit, U32(effect, 0x18));
    }

    // 0x10323300 (list 0x10BE2B6C, before 0x724820): the spell record's first visual (+0x20C) follows the
    // resolved visual (0 for -1, unchanged for 0 or an unknown row). Spell 0x31029 is refused for the player
    // and for units summoned by the player.
    bool __cdecl BeforeAuraApply(void* unit, uint32_t, uint32_t recArg)
    {
        uint8_t* rec = reinterpret_cast<uint8_t*>(recArg);
        const uint8_t* desc = Descriptor(unit);
        const uint32_t visual = ResolveVisual(GuidKind(U32(desc, 0), U32(desc, 4)), U32(desc, 0), U32(desc, 4), U32(rec, 0));
        if (visual == 0xFFFFFFFF)
            *reinterpret_cast<uint32_t*>(rec + 0x20C) = 0;
        else if (visual && SpellVisualRow(visual))
            *reinterpret_cast<uint32_t*>(rec + 0x20C) = visual;
        if (U32(rec, 0) != 0x31029)
            return true;
        if (unit == reinterpret_cast<void*(__cdecl*)()>(0x4038F0)())
            return false;
        return DescGuid(Descriptor(unit), 0x38) != ActiveGuid();
    }

    // SMSG 0x662
    void __cdecl OnSpellVisualOverride(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t v[4];
        memcpy(v, p->m_buffer + p->m_read, sizeof v);
        p->m_read += sizeof v;
        g_visuals[GuidKind(v[0], v[1])][GuidCounter(v[0], v[1])][v[2]] = v[3];
    }

    typedef uint32_t(__fastcall* Fn720F80_t)(void*, void*, const uint32_t*, uint8_t*, uint32_t, uint32_t);
    typedef uint32_t(__fastcall* Fn702DB0_t)(const uint32_t*, void*, uint32_t, const uint32_t*, const uint8_t*);
    typedef uint32_t(__fastcall* This1_t)(void*, void*, uint32_t);
    Fn720F80_t g_720F80 = nullptr;
    Fn702DB0_t g_702DB0 = nullptr;
    This1_t g_705100 = nullptr, g_7022D0 = nullptr;

    uint32_t __fastcall Detour720F80(void* object, void* edx, const uint32_t* spellRec, uint8_t* out, uint32_t c, uint32_t d)   // 0x10322B50
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x80ED4E && object != reinterpret_cast<void*(__cdecl*)()>(0x4038F0)()
            && U32(object, 0x14) == 4 && OptionOn() && !IsHelpful(reinterpret_cast<const uint8_t*>(spellRec)))
            return 0;
        const uint32_t r = g_720F80(object, edx, spellRec, out, c, d);
        const uint8_t* desc = Descriptor(object);
        const uint32_t visual = ResolveVisual(U32(object, 0x14), U32(desc, 0), U32(desc, 4), spellRec[0]);
        if (visual == 0)
            return r;
        uint8_t zero[0x80];
        const uint8_t* src;
        if (visual == 0xFFFFFFFF)
        {
            memset(zero, 0, sizeof zero);
            src = zero;
        }
        else if (!(src = SpellVisualRow(visual)))
            return r;
        memcpy(out, src, 0x80);
        return r;
    }

    uint32_t __fastcall Detour702DB0(const uint32_t* guid, void* edx, uint32_t a, const uint32_t* spell, const uint8_t* visualRec)   // 0x10322AD0
    {
        const uint32_t visual = ResolveVisual(GuidKind(guid[0], guid[1]), guid[0], guid[1], *spell);
        if (visual == 0xFFFFFFFF)
            visualRec = nullptr;
        else if (visual)
            if (const uint8_t* row = SpellVisualRow(visual))
                visualRec = row;
        return g_702DB0(guid, edx, a, spell, visualRec);
    }

    uint32_t __fastcall Detour705100(void* object, void* edx, uint32_t a)   // 0x10322760
    {
        if (OptionOn())
        {
            const uint8_t* desc = Descriptor(object);
            const uint64_t caster = DescGuid(desc, 0x18);
            if (caster != 0 && caster != ActiveGuid() && ObjectByGuid(caster, 0x10))
            {
                uint8_t rec[0x2B0];
                AscScript::FetchSpell(U32(desc, 0x24), rec);
                if (!IsHelpful(rec))
                    return 0;
            }
        }
        return g_705100(object, edx, a);
    }

    uint32_t __fastcall Detour7022D0(void* self, void* edx, uint32_t a)   // 0x10322810
    {
        const uint32_t r = g_7022D0(self, edx, a);
        const uint8_t* s = static_cast<const uint8_t*>(self);
        const uint64_t active = ActiveGuid();
        const uint64_t guid = Guid64(U32(s, 0), U32(s, 4));
        if (guid != active)
        {
            if (guid != 0 && static_cast<uint32_t>(guid >> 32) < 0x10000000)
                return r;
            void* unit = ObjectByGuid(guid, 8);
            if (!unit)
                return r;
            const uint8_t* desc = Descriptor(unit);
            const uint64_t summoner = DescGuid(desc, 0x38);
            if (summoner != 0 && static_cast<uint32_t>(summoner >> 32) < 0x10000000 && summoner != active)
                return r;
            if (IsPlayerGuid(DescGuid(desc, 0x30)) && DescGuid(desc, 0x30) != active)
                return r;
            if (IsPlayerGuid(DescGuid(desc, 0x40)) && DescGuid(desc, 0x40) != active)
                return r;
        }
        AscScript::Packet(0x9C7)
            .U64(Guid64(U32(s, 0), U32(s, 4)))
            .U64(Guid64(U32(s, 8), U32(s, 0xC)))
            .U32(U32(s, 0x1C))
            .U8(s[0x20])
            .U32(U32(s, 0x38))
            .U32(U32(s, 0x3C))
            .U32(U32(s, 0x40))
            .U32(U32(s, 0x44))
            .U32(U32(s, 0x48))
            .U32(U32(s, 0x4C))
            .Send();
        return r;
    }

    void Init()
    {
        g_720F80 = reinterpret_cast<Fn720F80_t>(AscRuntime::Detour(0x720F80, 6, reinterpret_cast<void*>(&Detour720F80)));
        g_7022D0 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x7022D0, 9, reinterpret_cast<void*>(&Detour7022D0)));
        g_702DB0 = reinterpret_cast<Fn702DB0_t>(AscRuntime::Detour(0x702DB0, 6, reinterpret_cast<void*>(&Detour702DB0)));
        sDC.AddPacketHandler(0x662, CNetClientCustomPacket((void*)&OnSpellVisualOverride, nullptr));
        AscRuntime::OnEffectFilter(&RemoveEffect, 0x1032313D);
        AscRuntime::OnVisualHide(&HideVisual, 0x10323147);
        AscRuntime::OnBefore724820(&BeforeAuraApply, 0x10323151);
        g_705100 = reinterpret_cast<This1_t>(AscRuntime::Detour(0x705100, 9, reinterpret_cast<void*>(&Detour705100)));
        AscClientOptions::QueueWorldCVar({"hideOtherPlayerHarmfulSpellVisuals", "0", 1, 1, nullptr});
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
