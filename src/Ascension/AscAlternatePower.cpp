// FUN_10321ac0 (world entry) clears an unordered_map at 0x10BE4430 that only its static initialiser
// (FUN_1007e240) otherwise touches -- always empty, so not reproduced.
// Alternate power and the jump-cast spell -- two small module inits and one callback of the attach init.
//
// FUN_10321bf0 (alternate power, once): the UnitAlternativePower* globals (AscUnitAddon.cpp), the event
// UNIT_POWER_ALTERNATIVE_UPDATE, a world-entry clear of the map 0x10BE4430 (nothing ever inserts into
// it, so not kept), and after every 0x6E7F50 (list 0x10BE2DB8) callbacks on the object's addon fields
// 0 and 1 that fire the event with field 0 (the power type).
//
// FUN_10322230 (the jump cast): after a spell is added to the spellbook (list 0x10BE2AEC) a spell whose
// SpellCustomAttr.dbc +0x14 has 0x1000 is remembered at 0x10BE4454 (nothing reads it) and one with
// 0x2000 at 0x10BE4458. On key event 0x20 (list 0x10BE2B0C before 0x4943C0) the player casts that
// spell on themself while falling (movement flag 0x1000), or cancels its aura when they have it.
//
// FUN_10a63e24's list callback 0x10A3AB50 (after every 0x6E7F50): the type callback for field 4 of
// kind-3 objects (0x10A4BAF0) -- 1 there zeroes the unit's health descriptor and runs 0x729220.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <cstdint>

namespace
{
    uint64_t GuidOf(void* object) { return **reinterpret_cast<const uint64_t* const*>(static_cast<uint8_t*>(object) + 8); }

    // LAB_10321bb0
    void __cdecl AlternatePowerChanged(uint32_t lo, uint32_t hi, uint32_t, void*, AscObjectAddon::Field*)
    {
        const uint32_t type = AscObjectAddon::Unit(lo | static_cast<uint64_t>(hi) << 32)[0].value;
        AscRuntime::Signal("UNIT_POWER_ALTERNATIVE_UPDATE", "%u", type);
    }

    void WatchAlternatePower(void* object)   // FUN_10321b60: FUN_102ce900(guid, kind 4, field 0 / 1, arg 4, ...)
    {
        std::vector<AscObjectAddon::Field>& fields = AscObjectAddon::Unit(GuidOf(object));
        for (uint32_t index = 0; index < 2; ++index)
        {
            fields[index].callback = &AlternatePowerChanged;
            fields[index].arg = reinterpret_cast<void*>(4);
        }
    }

    // 0x10A4BAF0
    void __cdecl ForcedDeath(uint32_t lo, uint32_t hi, uint32_t, void*, AscObjectAddon::Field*)
    {
        if (AscObjectAddon::Unit(lo | static_cast<uint64_t>(hi) << 32)[4].value != 1)
            return;
        uint8_t* unit = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, 8);
        if (!unit)
            return;
        uint32_t* health = reinterpret_cast<uint32_t*>(*reinterpret_cast<uint8_t**>(unit + 8) + 0x60);
        if (*health == 0)
            return;
        *health = 0;
        reinterpret_cast<void(__thiscall*)(uint8_t*)>(0x729220)(unit);
    }

    void RegisterForcedDeath(void*)   // 0x10A3AB50: FUN_102ce930(kind 3, field 4, arg 4, 0x10A4BAF0)
    {
        AscObjectAddon::RegisterTypeCallback(3, 4, &ForcedDeath, reinterpret_cast<void*>(4));
    }

    uint32_t g_flagSpell = 0;   // 0x10BE4454 (written only)
    uint32_t g_jumpSpell = 0;   // 0x10BE4458

    bool CustomAttr14(uint32_t spell, uint32_t mask)   // FUN_10324a10
    {
        const uint8_t* row = AscCA::SpellCustomAttrRow(spell);
        return row && (*reinterpret_cast<const uint32_t*>(row + 0x14) & mask) != 0;
    }

    void WatchAddedSpell(uint32_t spell)   // FUN_10322270
    {
        if (CustomAttr14(spell, 0x1000))
            g_flagSpell = spell;
        if (CustomAttr14(spell, 0x2000))
            g_jumpSpell = spell;
    }

    void JumpCast(const uint32_t* event)   // FUN_103222b0
    {
        if (*event != 0x20 || !g_jumpSpell)
            return;
        uint8_t* player = AscScript::ActivePlayer();
        if (!player)
            return;
        uint8_t rec[0x2A8];
        if (reinterpret_cast<int(__thiscall*)(uint8_t*, uint32_t)>(0x7282A0)(player, g_jumpSpell))
        {
            if (AscScript::FetchSpell(g_jumpSpell, rec))
                reinterpret_cast<void(__cdecl*)(uint8_t*, uint8_t*, int)>(0x802F80)(rec, player, 0);
            return;
        }
        if (!AscScript::FetchSpell(g_jumpSpell, rec))
            return;
        const uint8_t* movement = *reinterpret_cast<uint8_t* const*>(player + 0xD8);
        if (!movement || !(*reinterpret_cast<const uint32_t*>(movement + 0x44) & 0x1000))
            return;
        const uint32_t* guid = *reinterpret_cast<uint32_t* const*>(player + 8);
        reinterpret_cast<void(__cdecl*)(uint32_t, int, uint32_t, uint32_t, int)>(0x80DA40)(g_jumpSpell, 0, guid[0], guid[1], 0);
    }

    void Init()
    {
        AscRuntime::OnAfter6E7F50(&WatchAlternatePower);   // FUN_10321bf0
        AscRuntime::OnAfterAddSpell(&WatchAddedSpell);     // FUN_10322230
        AscRuntime::OnBefore4943C0(&JumpCast);
        AscRuntime::OnAfter6E7F50(&RegisterForcedDeath);   // FUN_10a63e24
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
