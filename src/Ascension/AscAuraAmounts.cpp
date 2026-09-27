// Server-sent aura effect amounts (absorb shields, armor penetration, ...) -- the original's map at
// 0x10d3c280 and everything that reads it.
//
// SMSG_AURA_UPDATE_ADDON (0x673) carries one record for a unit, SMSG_AURA_UPDATE_ALL_ADDON (0x674) a
// run of them to the end of the packet (FUN_10a121e0 / handler_0x0674 -> FUN_10a12470):
//     u64 unit, then per record: u32 slot, u64 caster, u32 spell, and unless slot == 0xFFFFFFFF
//     (removal) u32 amount[3] -- one per spell effect.
// Entries are keyed per unit by (caster, spell). Another unit's entry is only kept when the client's
// 0x53D580 accepts the spell for that unit. Every change fires UNIT_ABSORB_AMOUNT_CHANGED or
// UNIT_HEAL_ABSORB_AMOUNT_CHANGED ("%s", token) for each unit token naming the unit (FUN_10a13440).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    struct Entry
    {
        uint64_t caster;
        uint32_t spell;
        int32_t amount[3];
    };
    std::unordered_map<uint64_t, std::vector<Entry>> g_units;   // 0x10d3c280

    // Spell.dbc offsets used here: EffectApplyAuraName[3] +0x17C, EffectMiscValue[3] +0x1B8,
    // EffectBasePoints[3] +0x140, StackAmount +0xC4.
    int32_t Aura(const uint8_t* rec, int j) { return *reinterpret_cast<const int32_t*>(rec + 0x17C + j * 4); }
    int32_t Misc(const uint8_t* rec, int j) { return *reinterpret_cast<const int32_t*>(rec + 0x1B8 + j * 4); }

    bool HasEffect(const uint8_t* rec, int32_t aura, int32_t misc)   // FUN_10a117f0
    {
        for (int j = 0; j < 3; ++j)
            if (Aura(rec, j) == aura && Misc(rec, j) == misc)
                return true;
        return false;
    }

    void SignalForUnit(const char* event, uint64_t guid)
    {
        int count = 0;
        const char* const* tokens = reinterpret_cast<const char* const*(__cdecl*)(uint64_t*, int*)>(0x60BB70)(&guid, &count);
        for (int i = 0; i < count; ++i)
            AscRuntime::Signal(event, "%s", tokens[i]);
    }

    // FUN_10a13440
    void AmountsChanged(uint64_t guid, const uint8_t* rec)
    {
        if (HasEffect(rec, 0x45, 0x7F) || HasEffect(rec, 0x61, 0x7F))
            SignalForUnit("UNIT_ABSORB_AMOUNT_CHANGED", guid);
        else if (HasEffect(rec, 0x12D, 0x7F))
            SignalForUnit("UNIT_HEAL_ABSORB_AMOUNT_CHANGED", guid);
    }

    uint32_t ReadU32(CDataStore* pkt)
    {
        const uint32_t v = *reinterpret_cast<const uint32_t*>(pkt->m_buffer + pkt->m_read);
        pkt->m_read += 4;
        return v;
    }

    // FUN_10a12470: false stops a 0x674 run (unknown spell).
    bool ReadRecord(uint64_t unit, CDataStore* pkt)
    {
        const uint32_t slot = ReadU32(pkt);
        const uint64_t caster = ReadU32(pkt) | (static_cast<uint64_t>(ReadU32(pkt)) << 32);
        const uint32_t spell = ReadU32(pkt);
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return false;

        std::vector<Entry>& list = g_units[unit];
        if (slot == 0xFFFFFFFF)
        {
            list.erase(std::remove_if(list.begin(), list.end(),
                           [&](const Entry& e) { return e.caster == caster && e.spell == spell; }),
                       list.end());
        }
        else
        {
            int32_t amount[3];
            for (int j = 0; j < 3; ++j)
                amount[j] = static_cast<int32_t>(ReadU32(pkt));
            auto it = std::find_if(list.begin(), list.end(),
                                   [&](const Entry& e) { return e.caster == caster && e.spell == spell; });
            if (it != list.end())
            {
                std::copy(amount, amount + 3, it->amount);
            }
            else
            {
                uint64_t guid = unit;
                if (ActivePlayerGuid() != unit
                    && !(reinterpret_cast<uint32_t(__cdecl*)(const uint8_t*, uint64_t*, int)>(0x53D580)(rec, &guid, 1) & 0xFF))
                    return true;
                list.push_back({caster, spell, {amount[0], amount[1], amount[2]}});
            }
        }
        AmountsChanged(unit, rec);
        return true;
    }

    uint64_t ReadGuid(CDataStore* pkt)
    {
        const uint64_t lo = ReadU32(pkt);
        return lo | (static_cast<uint64_t>(ReadU32(pkt)) << 32);
    }

    void __cdecl OnAuraUpdateAddon(void*, uint32_t, uint32_t, CDataStore* pkt)        // 0x673
    {
        ReadRecord(ReadGuid(pkt), pkt);
    }

    void __cdecl OnAuraUpdateAllAddon(void*, uint32_t, uint32_t, CDataStore* pkt)     // 0x674
    {
        const uint64_t unit = ReadGuid(pkt);
        while (pkt->m_size != pkt->m_read)
            if (!ReadRecord(unit, pkt))
                return;
    }

    // FUN_10a12970 / FUN_10a12b60 / FUN_10a12d40: the unit's entries summed per effect, for the
    // effects `pick` selects. nil for a bad token; 0 when the token names no unit.
    template <class Pick>
    int SumAmounts(lua_State* L, Pick pick)
    {
        const std::string token = CheckString(L, 1);
        uint64_t guid;
        if (!UnitTokenGuid(token.c_str(), guid))
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        int32_t sum = 0;
        if (ObjectPtr(guid, 8))
        {
            for (const Entry& e : g_units[guid])
            {
                uint8_t rec[0x2A8];
                if (!FetchSpell(e.spell, rec))
                    continue;
                for (int j = 0; j < 3; ++j)
                    if (pick(rec, j))
                        sum += e.amount[j];
            }
        }
        PushInt(L, sum);
        return 1;
    }

    int UnitGetTotalAbsorbs(lua_State* L)
    {
        return SumAmounts(L, [](const uint8_t* r, int j) { return (Aura(r, j) == 0x45 || Aura(r, j) == 0x61) && Misc(r, j) == 0x7F; });
    }
    int UnitGetTotalHealAbsorbs(lua_State* L)
    {
        return SumAmounts(L, [](const uint8_t* r, int j) { return Aura(r, j) == 0x12D && Misc(r, j) == 0x7F; });
    }
    int UnitGetTotalSchoolAbsorbs(lua_State* L)
    {
        return SumAmounts(L, [](const uint8_t* r, int j) { return Aura(r, j) == 0x45; });
    }

    // handler_GetArmorPenetrationModifier: over the player's auras (count 0x4F8850, aura 0x556E10,
    // both __thiscall), each SPELL_AURA_MOD_ARMOR_PENETRATION_PCT (280) effect contributes the server's
    // amount when one is known for (caster, spell), else BasePoints+1 times the stack count for
    // stacking spells.
    int GetArmorPenetrationModifier(lua_State* L)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
        {
            PushInt(L, 0);
            return 1;
        }
        const uint64_t guid = **reinterpret_cast<uint64_t**>(player + 8);
        auto node = g_units.find(guid);
        int32_t sum = 0;
        typedef uint32_t(__thiscall* Count_t)(void*);
        typedef const uint8_t*(__thiscall* AuraAt_t)(void*, uint32_t);
        for (uint32_t i = 0; i < reinterpret_cast<Count_t>(0x4F8850)(player); ++i)
        {
            const uint8_t* aura = reinterpret_cast<AuraAt_t>(0x556E10)(player, i);
            uint8_t rec[0x2A8];
            if (!aura || !FetchSpell(*reinterpret_cast<const uint32_t*>(aura + 8), rec))
                continue;
            const Entry* known = nullptr;
            if (node != g_units.end())
                for (const Entry& e : node->second)
                    if (e.caster == *reinterpret_cast<const uint64_t*>(aura) && e.spell == *reinterpret_cast<const uint32_t*>(aura + 8))
                    {
                        known = &e;
                        break;
                    }
            for (int j = 0; j < 3; ++j)
            {
                if (Aura(rec, j) != 0x118)
                    continue;
                if (known)
                {
                    sum += known->amount[j];
                    continue;
                }
                int32_t amount = *reinterpret_cast<const int32_t*>(rec + 0x140 + j * 4) + 1;
                if (*reinterpret_cast<const int32_t*>(rec + 0xC4))
                {
                    uint32_t stacks = *reinterpret_cast<const uint16_t*>(aura + 0xE);
                    amount *= stacks ? stacks : 1;
                }
                sum += amount;
            }
        }
        PushInt(L, sum);
        return 1;
    }

    // FUN_10a10620 (every glue screen, `mov ecx, map; jmp FUN_10a13970`): the per-unit map emptied.
    void ClearUnits() { g_units.clear(); }
    // FUN_10a10630 (object free, list 0x10BE2BEC): the freed object's GUID erased from the map.
    void ForgetObject(void* object)
    {
        g_units.erase(*reinterpret_cast<const uint64_t*>(*reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(object) + 8)));
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&ClearUnits);
        AscRuntime::OnObjectFree(&ForgetObject);
        sDC.AddPacketHandler(0x673, CNetClientCustomPacket((void*)&OnAuraUpdateAddon, nullptr));
        sDC.AddPacketHandler(0x674, CNetClientCustomPacket((void*)&OnAuraUpdateAllAddon, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "UnitGetTotalAbsorbs", UnitGetTotalAbsorbs},
        {nullptr, "UnitGetTotalHealAbsorbs", UnitGetTotalHealAbsorbs},
        {nullptr, "UnitGetTotalSchoolAbsorbs", UnitGetTotalSchoolAbsorbs},
        {"C_Aura", "GetArmorPenetrationModifier", GetArmorPenetrationModifier},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

// The map 0x10d3c280 lookup FUN_103284e0 makes (0x10328ea0, then the first entry matching caster and
// spell): `unit`'s three effect amounts for that aura, or nullptr.
const int32_t* AscAuraAmounts_Find(uint64_t unit, uint64_t caster, uint32_t spell)
{
    auto it = g_units.find(unit);
    if (it == g_units.end())
        return nullptr;
    for (const Entry& e : it->second)
        if (e.caster == caster && e.spell == spell)
            return e.amount;
    return nullptr;
}

// The spell-description tokens 0xA2..0xA7 (FUN_10a10c30): every entry of `unit` for `spell`, whatever the
// caster; the last one wins. False when there is none.
bool AscAuraAmounts_LastForSpell(uint64_t unit, uint32_t spell, int32_t out[3])
{
    auto it = g_units.find(unit);
    if (it == g_units.end())
        return false;
    bool found = false;
    for (const Entry& e : it->second)
        if (e.spell == spell)
        {
            out[0] = e.amount[0];
            out[1] = e.amount[1];
            out[2] = e.amount[2];
            found = true;
        }
    return found;
}
