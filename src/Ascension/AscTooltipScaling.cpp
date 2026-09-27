// Item-scaling tooltips (module init FUN_1027fb60, 22 FUN_100010f0 hooks + SMSG 0x716 / 0x73F).
//
// Items can carry a level of their own (the item addon field, FUN_102cdbb0) or take one from context --
// the inspected player, a trade slot, a loot roll, an auction entry, a guild bank slot, a quest's
// scaling row. FUN_1027eee0 picks that level and asks the scaled item-stat store (AscItemStats) for the
// record; the hooks below feed it the context and let the tooltip, money line, feral attack power and
// repair cost read the scaled record instead of the item cache.
//
//   0x6277F0 -> sub_1027c870  GameTooltip's item builder: records the item / GUIDs for the duration of
//                             the call, may force three client branches (the patches below), then clears
//   0x61FEC0 -> FUN_1027b110  AddLine: rewrites Classes / damage / stat / armor / item level / required
//                             level / DPS / "Equip:" lines from the scaled record, else runs OnTextChanged
//   0x62D930 -> FUN_1027cb00  item-comparison stat deltas from scaled records
//   0x61A0D0 -> sub_10287600  money line: the scaled sell price
//   0x61A8F0 -> sub_10287640  feral attack power from the scaled damage
//   0x708540 -> sub_1027ae80  repair cost from the item's own level (replaced)
//   0x754400 -> sub_1027ade0  remembers the item object it returns
//   0x5E9250 -> sub_1027ae00  "can use" also passes 5 levels under a scaled required level
//   0x61B4A0 -> sub_1027c7f0  GameTooltip methods + GetItemGUID / ClearLine
//   0x49EAB0 / 0x49EB70 / 0x49EC80 -> FUN_1027e0a0 / FUN_1027dfb0 / FUN_1027e1a0
//                             Has/Get/SetScript: GameTooltip "OnTextChanged" kept in the DLL (map 0x10BE2F80)
//   GameTooltip:Set*Item context: SetQuestItem 0x62E670 / SetQuestLogItem 0x62E790 (sub_10283fe0 /
//     sub_10284040), SetTradeSkillItem 0x62EAE0 (sub_102840a0), SetTradePlayerItem 0x62EFF0 (FUN_1027b070),
//     SetTradeTargetItem 0x62F1E0 (FUN_1027b0c0), SetInboxItem 0x62F9E0 (sub_1027afc0), SetAuctionItem
//     0x62FCF0 (FUN_10283e00), SetLootRollItem 0x6300A0 (FUN_10283f90), SetGuildBankItem 0x6304A0
//     (FUN_10283f20), SetHyperlinkCompareItem 0x631B60 (sub_1027af70)
//   SMSG 0x716 (handler_0x0716): the 19 inspected-slot item levels
//
// Code patches (FUN_1027da50 / FUN_1027d4f0 / FUN_1027cf90, written through NtProtectVirtualMemory in the
// original): 0 forces the jump, 1 restores the client's conditional.
//   0x62B98F  0F 84 C0 01 00 00 (je)  <->  E9 C1 01 00 00 00
//   0x627980  0F 85 97 00 00 00 (jne) <->  E9 98 00 00 00 90
//   0x631760  0F 84 DE 00 00 00 (je)  <->  E9 DF 00 00 00 90
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscItemStats.hpp>
#include <Ascension/AscManastorm.hpp>
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

using namespace AscScript;
using AscItemStats::g_tooltipGuid;
using AscItemStats::g_inspectedSlot;
using AscItemStats::g_lootRollLevels;

namespace
{
    // ---- client functions -----------------------------------------------------------------------------
    const auto lua_rawgeti = reinterpret_cast<void(__cdecl*)(lua_State*, int, int)>(0x84E670);
    const auto lua_touserdata = reinterpret_cast<void*(__cdecl*)(lua_State*, int)>(0x84E1C0);
    const auto luaL_ref = reinterpret_cast<int(__cdecl*)(lua_State*, int)>(0x84F6C0);
    constexpr int LUA_REGISTRYINDEX = -10000;

    typedef uint8_t*(__cdecl* ObjectByGuid_t)(uint32_t lo, uint32_t hi, uint32_t typeMask);
    const ObjectByGuid_t ObjectByGuid = reinterpret_cast<ObjectByGuid_t>(0x4D4DB0);
    uint8_t* ActivePlayer() { return reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)(); }
    uint32_t UnitLevel(const uint8_t* unit) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0xD8); }

    typedef const uint8_t*(__thiscall* CacheGet_t)(void*, uint32_t, int, int, int, int);
    const uint8_t* ItemCache(uint32_t item)
    {
        return reinterpret_cast<CacheGet_t>(0x67CA30)(reinterpret_cast<void*>(0xC5D828), item, 0, 0, 0, 0);
    }

    uint32_t ItemLevelOf(uint64_t guid) { return AscObjectAddon::Item(guid)[0].value; }
    uint64_t Guid(const void* p) { return *static_cast<const uint64_t*>(p); }

    // (double)(uint32) the way MSVC converts: signed conversion plus 2^32 when the top bit is set (0x10B1B220).
    double UintToDouble(uint32_t v) { return static_cast<double>(static_cast<int32_t>(v)) + (static_cast<int32_t>(v) < 0 ? 4294967296.0 : 0.0); }

    // ---- the tooltip context (0x10BE2EB0..) ---------------------------------------------------------------
    uint8_t g_scaling = 1;               // DAT_10BCB6A0
    uint8_t g_rewriteFirst = 1;          // DAT_10BCB6A1 (read by AddLine)
    uint32_t g_item = 0;                 // 0x10BE2EB0 (also 0x10D3C278)
    uint32_t g_tooltipField = 0;         // 0x10BE2EB4: tooltip +0x127
    uint64_t g_linkGuid = 0;             // 0x10BE2EC0
    int32_t g_damage[4] = {};            // 0x10BE2EC8 / ECC / ED0 / ED4 (AddLine)
    const uint8_t* g_itemCache = nullptr;// 0x10BE2EDC
    uint8_t g_trade = 0;                 // 0x10BE2EE0
    uint8_t g_tradeSkill = 0;            // 0x10BE2EE1
    uint8_t g_lootRoll = 0;              // 0x10BE2EE2
    uint8_t g_compare = 0;               // 0x10BE2EE3
    uint32_t g_lootRollId = 0;           // 0x10BE2EE4
    uint32_t g_tradeSlot = 0;            // 0x10BE2EE8
    uint32_t g_questLevel = 0;           // 0x10BE2EEC
    uint64_t g_compareA = 0;             // 0x10BE2EF0
    uint64_t g_compareB = 0;             // 0x10BE2EF8
    uint32_t g_guildBankLevel = 0;       // 0x10BE2F00
    uint32_t g_auctionLevel = 0;         // 0x10BE2F04
    uint32_t g_inspectLevels[20] = {};   // 0x10BE2F10, 19 from SMSG 0x716; slot 19 (SetInspectedSlot allows
                                         // it) reads past them in the original -- here a zero
    uint8_t* g_lastItemObject = nullptr; // 0x10BE24D0
    std::unordered_map<void*, int> g_onTextChanged;   // 0x10BE2F80: GameTooltip object -> registry ref

    // ---- code patches ---------------------------------------------------------------------------------
    void WriteCode(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    }
    void Patch62B98F(bool restore)   // FUN_1027da50
    {
        static const uint8_t kJmp[6] = {0xE9, 0xC1, 0x01, 0x00, 0x00, 0x00}, kJe[6] = {0x0F, 0x84, 0xC0, 0x01, 0x00, 0x00};
        WriteCode(0x62B98F, restore ? kJe : kJmp, 6);
    }
    void Patch627980(bool restore)   // FUN_1027d4f0
    {
        static const uint8_t kJmp[6] = {0xE9, 0x98, 0x00, 0x00, 0x00, 0x90}, kJne[6] = {0x0F, 0x85, 0x97, 0x00, 0x00, 0x00};
        WriteCode(0x627980, restore ? kJne : kJmp, 6);
    }
    void Patch631760(bool restore)   // FUN_1027cf90
    {
        static const uint8_t kJmp[6] = {0xE9, 0xDF, 0x00, 0x00, 0x00, 0x90}, kJe[6] = {0x0F, 0x84, 0xDE, 0x00, 0x00, 0x00};
        WriteCode(0x631760, restore ? kJe : kJmp, 6);
    }

    // FUN_1027adc0: the player's unit addon field 87 is set.
    bool PlayerScales() { return AscObjectAddon::Unit(AscScript::ActivePlayerGuid())[87].value != 0; }

    // FUN_10213cd0 on a QuestTemplateScaling row: the level a quest reward scales to for `level`.
    uint32_t QuestScaledLevel(const uint8_t* row, uint32_t level)
    {
        const uint32_t lo = AscDbc::Table::U32(row, 4), hi = AscDbc::Table::U32(row, 8);
        if (level < lo)
            return lo;
        for (uint32_t i = 0; i < 5; ++i)
        {
            if (AscDbc::Table::U32(row, 0xC + i * 4) <= level)
            {
                level += AscDbc::Table::U32(row, 0x24 + i * 4);
                break;
            }
        }
        if (hi < level)
            level = hi;
        if (level < lo)
            level = lo;
        return level;
    }
    AscDbc::Table& QuestTemplateScaling() { return AscDbc::Get("DBFilesClient\\QuestTemplateScaling.dbc"); }

    // ---- FUN_1027eee0: the scaled record for the current tooltip item ---------------------------------------
    const uint8_t* ScaledRecord()
    {
        if (!g_scaling)
            return nullptr;
        if (g_tooltipGuid != 0)
        {
            uint32_t level = g_guildBankLevel;
            if (!level)
                level = ItemLevelOf(g_tooltipGuid);
            if (level)
                return AscItemStats::Get(g_item, level);
        }
        if (g_trade)
            return AscItemStats::Get(g_item, *reinterpret_cast<const uint32_t*>(0xCA0DE0 + g_tradeSlot * 0x50));
        const AscItemAddon::Record* addon = AscItemAddon::Find(g_item);
        if (!addon)
            return nullptr;
        if (g_auctionLevel)
            return AscItemStats::Get(g_item, g_auctionLevel);

        uint32_t level = 0;
        const uint32_t flags = addon->fields[1];   // +0x14
        const uint8_t* unit = nullptr;
        bool fromUnit = false;
        if (!(flags >> 5 & 1))
        {
            if ((flags >> 9 & 1) && AscManastorm::InManastorm())
            {
                unit = ActivePlayer();
                fromUnit = unit != nullptr;
            }
        }
        else if (g_lootRoll && g_lootRollId < g_lootRollLevels.size())
            level = g_lootRollLevels[g_lootRollId];
        else
        {
            const uint64_t a = Guid(reinterpret_cast<void*>(0xBFA8D8));
            unit = ObjectByGuid(static_cast<uint32_t>(a), static_cast<uint32_t>(a >> 32), 8);
            if (unit)
                fromUnit = true;
            else
            {
                const uint64_t b = Guid(reinterpret_cast<void*>(0xC24220));
                if (ObjectByGuid(static_cast<uint32_t>(b), static_cast<uint32_t>(b >> 32), 0x10))
                    level = g_inspectLevels[g_inspectedSlot];
                else
                {
                    unit = ActivePlayer();
                    fromUnit = unit != nullptr;
                }
            }
        }
        if (fromUnit)
            level = UnitLevel(unit);

        if (g_questLevel != 0 && PlayerScales())
            return AscItemStats::Get(g_item, g_questLevel);
        if (!PlayerScales())
            return nullptr;
        if (level <= 0x14)
            return AscItemStats::Get(g_item, level + 2);
        if (level < 0x1F)
            return AscItemStats::Get(g_item, level + 3);
        if (level < 0x33)
            return AscItemStats::Get(g_item, level + 4);
        switch (level)
        {
            case 0x3D: return AscItemStats::Get(g_item, level + 0x19);
            case 0x3E: return AscItemStats::Get(g_item, level + 0x1A);
            case 0x3F: return AscItemStats::Get(g_item, level + 0x1C);
            case 0x40: return AscItemStats::Get(g_item, level + 0x1E);
            case 0x41: return AscItemStats::Get(g_item, level + 0x1F);
            case 0x42:
            case 0x43: return AscItemStats::Get(g_item, level + 0x20);
            case 0x44: return AscItemStats::Get(g_item, level + 0x21);
            case 0x45: return AscItemStats::Get(g_item, level + 0x22);
            default:
                if (level >= 0x46 && level <= 0x50)
                    return AscItemStats::Get(g_item, level + 0x23);
                return AscItemStats::Get(g_item, level + 5);
        }
    }

    float AverageDamage(const uint8_t* rec)   // FUN_102102c0
    {
        const float max = *reinterpret_cast<const float*>(rec + 0x64);
        if (max != 0.0f)
            return (max + *reinterpret_cast<const float*>(rec + 0x60)) * 0.5f;
        return 0.0f;
    }

    // ---- 0x6277F0: the item builder (__thiscall, 16 arguments, ret 0x40) ----------------------------------
    typedef int(__fastcall* Fn6277F0_t)(void*, void*, uint32_t, const uint64_t*, const uint64_t*, uint32_t, uint32_t, uint32_t,
                                        uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn6277F0_t g_6277F0 = nullptr;
    int __fastcall Detour6277F0(void* tip, void* edx, uint32_t item, const uint64_t* linkGuid, const uint64_t* objectGuid,
                                uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10,
                                uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15, uint32_t a16)
    {
        if (g_guildBankLevel != 0)
            a15 = 0;
        g_item = item;
        g_tooltipField = *reinterpret_cast<const uint32_t*>(static_cast<uint8_t*>(tip) + 0x127);
        if (linkGuid)
            g_linkGuid = *linkGuid;
        if (!g_compare)
        {
            g_compareA = 0;
            g_compareB = 0;
        }
        if (objectGuid && *objectGuid != 0)
        {
            g_tooltipGuid = *objectGuid;
            if (!g_compare)
                g_compareA = *objectGuid;
            else if (g_compareB == 0)
                g_compareB = *objectGuid;
        }
        g_itemCache = ItemCache(item);
        const uint8_t* scaled = ScaledRecord();
        if (scaled && g_itemCache && *reinterpret_cast<const int32_t*>(scaled + 0xC) < *reinterpret_cast<const int32_t*>(g_itemCache + 0x34))
            Patch62B98F(false);
        if (g_compare)
            Patch631760(false);
        if (!RealmInfoSvc::Get().Dev() && g_itemCache && *reinterpret_cast<const uint32_t*>(g_itemCache + 0x14) == 4)
        {
            const uint32_t cap = AscGameEvents::ServerMaxLevel();
            const uint32_t required = *reinterpret_cast<const uint32_t*>(g_itemCache + 0x38);
            if ((cap == 0x50 && required == 0x46) || (cap == 0x46 && required == 0x3C))
                Patch62B98F(false);
        }
        const int result = g_6277F0(tip, edx, item, linkGuid, objectGuid, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
        g_item = 0;
        g_linkGuid = 0;
        g_tooltipGuid = 0;
        memset(g_damage, 0, sizeof(g_damage));
        g_itemCache = nullptr;
        Patch62B98F(true);
        Patch627980(true);
        Patch631760(true);
        g_scaling = 1;
        g_rewriteFirst = 1;
        g_questLevel = 0;
        g_guildBankLevel = 0;
        g_auctionLevel = 0;
        g_lootRollId = 0;
        return result;
    }

    // ---- 0x62D930: comparison stat deltas (__thiscall(out, arg), ret 8) ----------------------------------
    // FUN_10210430: a scaled-record image built from the item cache (and the addon's reborn armor) for an
    // item without a scaled level. Its damage block is filled from +0xC0 / +0xC8 / +0xC4 / +0xCC, not the
    // cache's {min, max} pairs -- the original's order, kept (IMPROVEMENTS.md).
    void CacheRecord(uint32_t out[0x2A], uint32_t item)
    {
        memset(out, 0, 0x2A * 4);
        const uint8_t* c = ItemCache(item);
        if (!c)
            return;
        auto at = [c](uint32_t off) { return *reinterpret_cast<const uint32_t*>(c + off); };
        out[2] = item;
        out[3] = at(0x34);
        for (uint32_t i = 0; i < 10; ++i)
        {
            out[4 + i * 2] = at(0x68 + i * 4);
            out[5 + i * 2] = at(0x90 + i * 4);
        }
        out[0x18] = at(0xC0);
        out[0x19] = at(0xC8);
        out[0x1B] = at(0xC4);
        out[0x1C] = at(0xCC);
        out[0x1E] = at(0xD8);
        const AscItemAddon::Record* addon = AscItemAddon::Find(item);
        out[0x1F] = addon ? addon->fields[7] : at(0xD8);   // +0x2C
        for (uint32_t i = 0; i < 6; ++i)
            out[0x20 + i] = at(0xDC + i * 4);
        out[0x26] = at(0x1A4);
        out[0x27] = at(0x19C);
        out[0x28] = at(0x38);
    }

    // The record FUN_1027cb00 compares for one item object: its scaled record when the item has a level
    // and the store has it, else the cache image.
    void CompareRecord(uint32_t out[0x2A], const uint8_t* object)
    {
        const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(object + 8);
        const uint32_t entry = *reinterpret_cast<const uint32_t*>(desc + 0xC);
        const uint32_t level = ItemLevelOf(Guid(desc));
        if (level)
        {
            if (AscItemStats::Get(entry, level))
            {
                const uint8_t* r = AscItemStats::Get(entry, ItemLevelOf(Guid(desc)));
                memcpy(out, r, 0x2A * 4);
                return;
            }
        }
        CacheRecord(out, entry);
    }

    float Dps(const uint32_t rec[0x2A], float delay)
    {
        const float max = reinterpret_cast<const float*>(rec)[0x19];
        if (max == 0.0f)
            return 0.0f;
        return (max + reinterpret_cast<const float*>(rec)[0x18]) * 0.5f / (delay * 0.001f);
    }
    float Delay(const uint32_t rec[0x2A])
    {
        const uint8_t* c = ItemCache(rec[2]);
        return c ? static_cast<float>(UintToDouble(*reinterpret_cast<const uint32_t*>(c + 0xF4))) : 2.0f;
    }

    // FUN_10210780: stat `type` in a minus its value in b (0 where absent).
    int32_t StatDelta(const uint32_t a[0x2A], const uint32_t b[0x2A], int32_t type)
    {
        int32_t va = 0;
        for (uint32_t i = 0; i < 10; ++i)
            if (static_cast<int32_t>(a[4 + i * 2]) == type)
            {
                va = static_cast<int32_t>(a[5 + i * 2]);
                break;
            }
        for (uint32_t i = 0; i < 10; ++i)
            if (static_cast<int32_t>(b[4 + i * 2]) == type)
                return va - static_cast<int32_t>(b[5 + i * 2]);
        return va;
    }

    // The player's class (UNIT_FIELD_BYTES_0 byte 1), 0 unless the object is a unit / player (+0x14 3 / 4).
    uint32_t PlayerClass(const uint8_t* player)
    {
        const uint32_t type = *reinterpret_cast<const uint32_t*>(player + 0x14);
        if (type != 3 && type != 4)
            return 0;
        return (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0x5C) >> 8) & 0xFF;
    }
    bool StockClass(const uint8_t* player)   // FUN_100c6380
    {
        const uint32_t c = PlayerClass(player);
        return (c >= 1 && c <= 9) || c == 0xB;
    }
    bool CoAClass(const uint8_t* player)     // FUN_100c5f50
    {
        const uint32_t c = PlayerClass(player);
        return c >= 0xC && c <= 0x20;
    }

    typedef int(__fastcall* Fn62D930_t)(void*, void*, void*, uint32_t);
    Fn62D930_t g_62D930 = nullptr;
    int __fastcall Detour62D930(void* self, void* edx, void* outPtr, uint32_t arg)
    {
        const uint8_t* objA = ObjectByGuid(static_cast<uint32_t>(g_compareA), static_cast<uint32_t>(g_compareA >> 32), 2);
        const uint8_t* objB = ObjectByGuid(static_cast<uint32_t>(g_compareB), static_cast<uint32_t>(g_compareB >> 32), 2);
        if (objA && objB)
        {
            uint32_t a[0x2A], b[0x2A];
            CompareRecord(a, objA);
            CompareRecord(b, objB);
            int32_t* out = static_cast<int32_t*>(outPtr);
            const float dps = Dps(a, Delay(a)) - Dps(b, Delay(b));   // FUN_10210300
            memcpy(&out[0], &dps, 4);
            auto I = [](const uint32_t* r, uint32_t i) { return static_cast<int32_t>(r[i]); };
            out[1] = I(a, 0x1E) - I(b, 0x1E);   // armor
            if (const uint8_t* player = ActivePlayer())
                if (StockClass(player) || CoAClass(player))
                    out[1] = I(a, 0x1F) - I(b, 0x1F);   // reborn armor
            for (uint32_t i = 0; i < 6; ++i)
                out[2 + i] = I(a, 0x20 + i) - I(b, 0x20 + i);   // resistances
            static const struct { uint32_t index; int32_t stat; } kStats[] = {
                {0x08, 0x26}, {0x0F, 3}, {0x10, 4}, {0x11, 5}, {0x12, 6}, {0x13, 7}, {0x18, 0xC}, {0x19, 0xD},
                {0x1B, 0xF}, {0x20, 0x14}, {0x2B, 0x1F}, {0x2C, 0x20}, {0x3A, 0x2E},
            };
            for (const auto& s : kStats)
                out[s.index] = StatDelta(a, b, s.stat);
            out[0x3D] = StatDelta(a, b, 0x30) + (I(a, 0x26) - I(b, 0x26));   // block value + block
            static const struct { uint32_t index; int32_t stat; } kStats2[] = {
                {0x2F, 0x23}, {0x30, 0x24}, {0x38, 0x2C}, {0x39, 0x2D}, {0x3E, 0x2B},
            };
            for (const auto& s : kStats2)
                out[s.index] = StatDelta(a, b, s.stat);
        }
        return g_62D930(self, edx, outPtr, arg);
    }

    // ---- 0x61A0D0: money line (__thiscall(money, a2), ret 8) -------------------------------------------------
    typedef int(__fastcall* Fn61A0D0_t)(void*, void*, uint32_t, uint32_t);
    Fn61A0D0_t g_61A0D0 = nullptr;
    int __fastcall Detour61A0D0(void* self, void* edx, uint32_t money, uint32_t a2)
    {
        if (g_scaling)
            if (const uint8_t* r = ScaledRecord())
                money = *reinterpret_cast<const uint32_t*>(r + 0xA4);
        return g_61A0D0(self, edx, money, a2);
    }

    // ---- 0x61A8F0: feral attack power (__cdecl(item cache record, damage)) -------------------------------------
    typedef int(__cdecl* Fn61A8F0_t)(const uint8_t*, float);
    Fn61A8F0_t g_61A8F0 = nullptr;
    int __cdecl Detour61A8F0(const uint8_t* item, float damage)
    {
        if (const uint8_t* r = ScaledRecord())
            damage = AverageDamage(r);
        const uint32_t subclass = *reinterpret_cast<const uint32_t*>(item + 0x28);
        if (*reinterpret_cast<const uint32_t*>(item + 4) == 2 &&
            (subclass == 0xD || subclass == 0x11 || subclass == 0x15 || subclass == 0x16))
        {
            const float delay = static_cast<float>(UintToDouble(*reinterpret_cast<const uint32_t*>(item + 0xF4))) / 1000.0f;
            const float dps = damage / delay;
            const float low = (dps - 30.0f) * 8.0f;
            const float high = (dps - 54.81f) * 14.0f + 200.0f;
            float v = low > 0.0f ? low : 0.0f;
            v = v < 200.0f ? v : 200.0f;
            const float ap = high > v ? high : v;
            const int32_t n = static_cast<int32_t>(ap);
            return n > 0 ? n : 0;
        }
        return g_61A8F0(item, damage);
    }

    // ---- 0x708540: repair cost (__thiscall on the item object; replaced) ----------------------------------------
    uint32_t __fastcall RepairCost(uint8_t* item, void*)
    {
        const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(item + 8);
        const uint8_t* c = ItemCache(*reinterpret_cast<const uint32_t*>(desc + 0xC));
        if (!c)
            return 0;
        const uint32_t max = *reinterpret_cast<const uint32_t*>(desc + 0xF4);
        if (!max)
            return 0;
        const uint32_t cur = *reinterpret_cast<const uint32_t*>(desc + 0xF0);
        if (cur > max || max == cur)
            return 0;
        const uint32_t lost = max - cur;
        uint32_t level = ItemLevelOf(Guid(desc));
        if (!level)
            level = *reinterpret_cast<const uint32_t*>(c + 0x34);
        const uint32_t costsTable = *reinterpret_cast<const uint32_t*>(0xAD373C);   // DurabilityCosts, FUN_100b2c50
        const uint8_t* costs = costsTable ? *reinterpret_cast<uint8_t* const*>(costsTable + (level - 1) * 4) : nullptr;
        if (!costs)
            return 0;
        const uint32_t qualityTable = *reinterpret_cast<const uint32_t*>(0xAD3760);  // DurabilityQuality, FUN_100b2c70
        const uint8_t* quality = qualityTable
            ? *reinterpret_cast<uint8_t* const*>(qualityTable + (*reinterpret_cast<const uint32_t*>(c + 0x14) * 2 + 1) * 4)
            : nullptr;
        if (!quality)
            return 0;
        const uint32_t cls = *reinterpret_cast<const uint32_t*>(c + 4);
        uint32_t column = *reinterpret_cast<const uint32_t*>(c + 8);
        if (cls == 2)
            ;
        else if (cls == 4)
            column += 0x15;
        else
            column = 0;
        column &= 0xFF;
        const uint32_t base = *reinterpret_cast<const uint32_t*>(costs + column * 4 + 4) * lost;
        const int32_t cost = static_cast<int32_t>(static_cast<int64_t>(UintToDouble(base) * static_cast<double>(*reinterpret_cast<const float*>(quality + 4))));
        return cost ? static_cast<uint32_t>(cost) : 1;
    }

    // ---- 0x754400 (__thiscall(a), ret 4): the item object it returns --------------------------------------------
    typedef uint8_t*(__fastcall* Fn754400_t)(void*, void*, uint32_t);
    Fn754400_t g_754400 = nullptr;
    uint8_t* __fastcall Detour754400(void* self, void* edx, uint32_t a)
    {
        g_lastItemObject = g_754400(self, edx, a);
        return g_lastItemObject;
    }

    // ---- 0x5E9250 (__cdecl(item cache record, a2)): usable under a scaled required level ---------------------------
    typedef int(__cdecl* Fn5E9250_t)(const uint8_t*, uint32_t);
    Fn5E9250_t g_5E9250 = nullptr;
    int __cdecl Detour5E9250(const uint8_t* item, uint32_t a2)
    {
        int result = g_5E9250(item, a2);
        if (result == 0)
        {
            const uint8_t* player = ActivePlayer();
            const uint32_t required = *reinterpret_cast<const uint32_t*>(item + 0x38);
            if (player && required > 1 && UnitLevel(player) < required && g_lastItemObject)
            {
                const uint32_t level = ItemLevelOf(Guid(*reinterpret_cast<uint8_t* const*>(g_lastItemObject + 8)));
                if (level != 0 && level - 5 <= UnitLevel(player))
                    result = 1;
            }
        }
        return result;
    }

    // ---- GameTooltip methods (0x61B4A0 registers the method table 0xAD2AE0) --------------------------------------
    // FUN_10281690: the tooltip item's GUID through the client's formatter 0x74D0D0(lo, hi, buffer).
    int GetItemGUID(lua_State* L)
    {
        char buf[32] = {};
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, char*)>(0x74D0D0)(
            static_cast<uint32_t>(g_tooltipGuid), static_cast<uint32_t>(g_tooltipGuid >> 32), buf);
        PushStr(L, buf);
        return 1;
    }

    // FUN_10281460 ClearLine(tooltip, line): empties both font strings of that line (SetText 0x483910).
    int ClearLine(lua_State* L)
    {
        lua_rawgeti(L, 1, 0);
        uint8_t* tip = static_cast<uint8_t*>(lua_touserdata(L, -1));
        AscLua::lua_settop(L, -2);
        const int32_t line = ToInt(CheckNumber(L, 2));
        if (line > 0)
        {
            typedef void(__thiscall* SetTextFn)(void*, const char*, int);
            void* left = *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(tip + 0x2B4) - 4 + line * 4);
            if (left)
                reinterpret_cast<SetTextFn>(0x483910)(left, "", 1);
            void* right = *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(tip + 0x2C0) - 4 + line * 4);
            if (right)
                reinterpret_cast<SetTextFn>(0x483910)(right, "", 1);
        }
        return 0;
    }

    typedef int(__cdecl* LuaFn_t)(lua_State*);
    LuaFn_t g_61B4A0 = nullptr;
    int __cdecl Detour61B4A0(lua_State* L)
    {
        const int result = g_61B4A0(L);
        static const struct { const char* name; lua_CFunction fn; } kMethods[] = {{"GetItemGUID", GetItemGUID}, {"ClearLine", ClearLine}};
        for (const auto& m : kMethods)
        {
            AscLua::lua_pushstring(L, m.name);
            AscLua::lua_pushcclosure(L, m.fn, 0);
            AscLua::lua_settable(L, -3);
        }
        return result;
    }

    // ---- Has/Get/SetScript: GameTooltip "OnTextChanged" -------------------------------------------------------
    // The frame's object when it is a GameTooltip and argument 2 is "OnTextChanged", else nullptr.
    void* TooltipOnTextChanged(lua_State* L)
    {
        lua_rawgeti(L, 1, 0);
        void* obj = lua_touserdata(L, -1);
        AscLua::lua_settop(L, -2);
        typedef bool(__thiscall* IsA_t)(void*, uint32_t);
        const uint32_t type = reinterpret_cast<uint32_t(__cdecl*)()>(0x514410)();
        if (!(*reinterpret_cast<IsA_t*>(*static_cast<uint8_t**>(obj) + 0x10))(obj, type))
            return nullptr;
        if (!AscLua::lua_isstring(L, 2))
            return nullptr;
        const char* what = AscLua::lua_tolstring(L, 2, nullptr);
        return strcmp(what, "OnTextChanged") == 0 ? obj : nullptr;
    }

    LuaFn_t g_HasScript = nullptr, g_GetScript = nullptr, g_SetScript = nullptr;
    int __cdecl HasScript(lua_State* L)   // FUN_1027e0a0
    {
        if (void* obj = TooltipOnTextChanged(L))
            if (g_onTextChanged.find(obj) != g_onTextChanged.end())
            {
                AscLua::lua_pushnumber(L, 1.0);
                return 1;
            }
        return g_HasScript(L);
    }
    int __cdecl GetScript(lua_State* L)   // FUN_1027dfb0
    {
        if (void* obj = TooltipOnTextChanged(L))
        {
            auto it = g_onTextChanged.find(obj);
            if (it != g_onTextChanged.end())
            {
                lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
                return 1;
            }
        }
        return g_GetScript(L);
    }
    int __cdecl SetScript(lua_State* L)   // FUN_1027e1a0
    {
        if (void* obj = TooltipOnTextChanged(L))
        {
            const int ref = luaL_ref(L, LUA_REGISTRYINDEX);
            g_onTextChanged[obj] = ref;
            return 0;
        }
        return g_SetScript(L);
    }

    // ---- GameTooltip:Set*Item context hooks (all __cdecl(L)) ------------------------------------------------
    LuaFn_t g_SetQuestItem = nullptr, g_SetQuestLogItem = nullptr, g_SetTradeSkillItem = nullptr, g_SetTradePlayerItem = nullptr,
            g_SetTradeTargetItem = nullptr, g_SetInboxItem = nullptr, g_SetAuctionItem = nullptr, g_SetLootRollItem = nullptr,
            g_SetGuildBankItem = nullptr, g_SetHyperlinkCompareItem = nullptr;

    // sub_10283fe0 / sub_10284040: the quest (*0xC0D65C / *0xC23AD8) has a QuestTemplateScaling row.
    void QuestContext(uint32_t questAddress)
    {
        const uint8_t* player = ActivePlayer();
        if (!player)
            return;
        AscDbc::Table& t = QuestTemplateScaling();
        const uint32_t quest = *reinterpret_cast<const uint32_t*>(questAddress);
        if (!t.Loaded() || quest < t.MinId() || quest > t.MaxId())
            return;
        if (const uint8_t* row = t.Row(quest))
            g_questLevel = QuestScaledLevel(row, UnitLevel(player));
    }
    int __cdecl SetQuestItem(lua_State* L)
    {
        QuestContext(0xC0D65C);
        return g_SetQuestItem(L);
    }
    int __cdecl SetQuestLogItem(lua_State* L)
    {
        QuestContext(0xC23AD8);
        return g_SetQuestLogItem(L);
    }
    int __cdecl SetTradeSkillItem(lua_State* L)
    {
        g_tradeSkill = 1;
        const int r = g_SetTradeSkillItem(L);
        g_tradeSkill = 0;
        return r;
    }
    // FUN_1027b070 leaves the trade flag set; FUN_1027b0c0 clears it (IMPROVEMENTS.md).
    int __cdecl SetTradePlayerItem(lua_State* L)
    {
        g_trade = 1;
        g_tradeSlot = ToInt(CheckNumber(L, 2)) - 1;
        const int r = g_SetTradePlayerItem(L);
        g_tradeSlot = 0;
        return r;
    }
    int __cdecl SetTradeTargetItem(lua_State* L)
    {
        g_trade = 1;
        g_tradeSlot = ToInt(CheckNumber(L, 2)) - 1;
        const int r = g_SetTradeTargetItem(L);
        g_tradeSlot = 0;
        g_trade = 0;
        return r;
    }
    int __cdecl SetInboxItem(lua_State* L)   // sub_1027afc0
    {
        if (AscLua::lua_isnumber(L, 3))
        {
            const int32_t mail = ToInt(CheckNumber(L, 2));
            const int32_t attachment = ToInt(CheckNumber(L, 3));
            uint8_t* m = (*reinterpret_cast<uint8_t***>(0xBEB204))[mail - 1];
            if (m)
            {
                const uint32_t lo = *reinterpret_cast<uint32_t*(__cdecl*)(uint8_t*, int32_t)>(0x56D920)(m, attachment - 1);
                g_tooltipGuid = lo ? (static_cast<uint64_t>(0x4000u << 16) << 32 | lo) : 0;   // FUN_100a55d0
            }
        }
        return g_SetInboxItem(L);
    }
    int __cdecl SetAuctionItem(lua_State* L)   // FUN_10283e00
    {
        const char* type = AscLua::lua_tolstring(L, 2, nullptr);
        const int32_t index = ToInt(CheckNumber(L, 3));
        uint32_t list = 0;
        if (strcmp(type, "bidder") == 0)
            list = 0xC0F460;
        else if (strcmp(type, "owner") == 0)
            list = 0xC0F450;
        else if (strcmp(type, "list") == 0)
            list = 0xC0F440;
        if (list)
        {
            const uint8_t* entries = *reinterpret_cast<const uint8_t* const*>(list + 8);
            const uint32_t count = *reinterpret_cast<const uint32_t*>(list + 4);
            if (entries && count && static_cast<uint32_t>(index - 1) < count + 1)
                g_auctionLevel = *reinterpret_cast<const uint32_t*>(entries + (index - 1) * 0xA0 + 0x70);
        }
        return g_SetAuctionItem(L);
    }
    int __cdecl SetLootRollItem(lua_State* L)   // FUN_10283f90
    {
        g_lootRoll = 1;
        g_lootRollId = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const int r = g_SetLootRollItem(L);
        g_lootRoll = 0;
        return r;
    }
    int __cdecl SetGuildBankItem(lua_State* L)   // FUN_10283f20
    {
        const int32_t tab = ToInt(CheckNumber(L, 2));
        const int32_t slot = ToInt(CheckNumber(L, 3));
        if (const uint8_t* item = reinterpret_cast<const uint8_t*(__cdecl*)(int32_t, int32_t)>(0x5A4C10)(tab - 1, slot - 1))
            g_guildBankLevel = *reinterpret_cast<const uint32_t*>(item + 0x28);
        return g_SetGuildBankItem(L);
    }
    int __cdecl SetHyperlinkCompareItem(lua_State* L)   // sub_1027af70
    {
        if (*reinterpret_cast<const uint64_t*>(0xC1DC10) != 0)
            return 0;
        g_compare = 1;
        g_compareB = 0;
        const int r = g_SetHyperlinkCompareItem(L);
        g_compare = 0;
        g_compareB = 0;
        return r;
    }

    // ---- 0x61FEC0: GameTooltip AddLine (__thiscall(left, right, color*, a4, a5), ret 0x14) -----------------
    // FUN_1027b110 and its helpers. Each line is tested against the patterns below in order; the first that
    // applies rewrites it from the scaled record and passes the rewrite to the client. A line nothing
    // rewrites goes through the tooltip's OnTextChanged ref (Has/Get/SetScript above) and then the client.
    const auto GetGlobalString = reinterpret_cast<const char*(__cdecl*)(const char*, int, int)>(0x819D40);
    const auto lua_pcall = reinterpret_cast<int(__cdecl*)(lua_State*, int, int, int)>(0x84EC50);

    std::string GlobalString(const char* key) { return GetGlobalString(key, -1, 1); }

    int Format(char* out, size_t size, const char* fmt, ...)   // FUN_100b82f0
    {
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(out, size, fmt, args);
        va_end(args);
        return n < 0 ? -1 : n;
    }

    // FUN_10280760: an ITEM_MOD stat type for the name an "Equip:" / "+N" line uses, -1 for none.
    int32_t StatTypeByName(const std::string& name)
    {
        static const struct { const char* name; int32_t type; } kStats[] = {
            {"Mana", 0}, {"Health", 1}, {"Strength", 4}, {"Agility", 3}, {"Intellect", 5}, {"Spirit", 6},
            {"Stamina", 7}, {"defense rating", 0xC}, {"your dodge rating", 0xD}, {"your parry rating", 0xE},
            {"your shield block rating", 0xF}, {"melee hit rating", 0x10}, {"ranged hit rating", 0x11},
            {"spell hit rating", 0x12}, {"hit rating", 0x1F}, {"critical strike rating", 0x20},
            {"haste rating", 0x24}, {"your expertise rating", 0x25}, {"attack power", 0x26},
            {"mana per 5 sec", 0x2B}, {"your armor penetration rating", 0x2C}, {"spell power", 0x2D},
            {"health per 5 sec", 0x2E}, {"your spell penetration", 0x2F}, {"the block value of your shield", 0x30},
        };
        for (const auto& s : kStats)
            if (name == s.name)
                return s.type;
        return -1;
    }

    // FUN_1027e550(name, value): the scaled value of a stat or resistance line. A school name ("Fire")
    // becomes its resistance name ("Fire Resistance") in `name`, which the caller then prints.
    bool ScaledStat(char* name, int32_t* value)
    {
        if (!g_scaling)
            return false;
        const uint8_t* rec = ScaledRecord();
        if (!rec)
            return false;
        for (uint32_t off = 0x10; off != 0x60; off += 8)
        {
            const int32_t type = *reinterpret_cast<const int32_t*>(rec + off);
            const int32_t v = *reinterpret_cast<const int32_t*>(rec + off + 4);
            if (type != -1 && v != 0 && type == StatTypeByName(std::string(name)))
            {
                *value = v;
                return true;
            }
        }
        static const struct { const char* school; const char* resistance; uint32_t off; } kSchools[] = {
            {"DAMAGE_SCHOOL3", "RESISTANCE2_NAME", 0x84}, {"DAMAGE_SCHOOL7", "RESISTANCE6_NAME", 0x94},
            {"DAMAGE_SCHOOL4", "RESISTANCE3_NAME", 0x88}, {"DAMAGE_SCHOOL5", "RESISTANCE4_NAME", 0x8C},
            {"DAMAGE_SCHOOL6", "RESISTANCE5_NAME", 0x90},
        };
        std::string schools[5], resistances[5];   // all ten looked up first, in the original's order
        for (uint32_t i = 0; i < 5; ++i)
        {
            schools[i] = GlobalString(kSchools[i].school);
            resistances[i] = GlobalString(kSchools[i].resistance);
        }
        for (uint32_t i = 0; i < 5; ++i)
            if (strcmp(name, schools[i].c_str()) == 0)
            {
                *value = *reinterpret_cast<const int32_t*>(rec + kSchools[i].off);
                strcpy(name, resistances[i].c_str());
                return true;
            }
        return false;
    }

    // FUN_1027e280(name, value): armor (reborn armor for stock and CoA classes) and block.
    bool ScaledArmorOrBlock(const char* name, int32_t* value)
    {
        if (!g_scaling)
            return false;
        const std::string armor = GlobalString("ARMOR");
        const std::string block = GlobalString("BLOCK");
        const uint8_t* rec = ScaledRecord();
        if (!rec)
        {
            if (armor == name)
            {
                const AscItemAddon::Record* addon = AscItemAddon::Find(g_item);
                const uint8_t* player = addon ? ActivePlayer() : nullptr;
                if (player && (StockClass(player) || CoAClass(player)))
                {
                    *value = static_cast<int32_t>(addon->fields[7]);   // +0x2C
                    return true;
                }
            }
            return false;
        }
        if (armor == name)
        {
            *value = *reinterpret_cast<const int32_t*>(rec + 0x78);
            const uint8_t* player = ActivePlayer();
            if (player && (StockClass(player) || CoAClass(player)))
                *value = *reinterpret_cast<const int32_t*>(rec + 0x7C);
            return true;
        }
        if (block == name)
        {
            *value = *reinterpret_cast<const int32_t*>(rec + 0x98);
            return true;
        }
        return false;
    }

    // FUN_1027ee10(min, max, i): damage pair i of the scaled record, truncated.
    bool ScaledDamage(int32_t* min, int32_t* max, uint32_t i)
    {
        if (!g_scaling)
            return false;
        const uint8_t* rec = ScaledRecord();
        if (!rec)
            return false;
        *min = static_cast<int32_t>(*reinterpret_cast<const float*>(rec + 0x60 + i * 0xC));
        *max = static_cast<int32_t>(*reinterpret_cast<const float*>(rec + 0x64 + i * 0xC));
        return true;
    }

    // FUN_1008e380 on the player: under the level cap, only a Dev realm at 60 / 70 / 80 passes.
    bool PowerLabelShown(const uint8_t* player)
    {
        const uint32_t level = UnitLevel(player);
        if (level < AscGameEvents::ServerMaxLevel())
            if (!RealmInfoSvc::Get().Dev() || (level != 0x3C && level != 0x46 && level != 0x50))
                return false;
        return true;
    }

    // FUN_1027f210: the "Classes:" line reduced to the classes of the player's own class model (CoA 12..32,
    // stock 1..9 / 11, else Hero 10), in ChrClasses order. Empty when every one of them is already listed
    // -- the caller then drops the line -- or when there is no player.
    std::string ClassesLine(const char* line)
    {
        const uint8_t* player = ActivePlayer();
        const bool coa = player && CoAClass(ActivePlayer());
        const bool stock = player && StockClass(ActivePlayer());
        const bool hero = ActivePlayer() && !coa && !stock;
        std::vector<const char*> names;
        const uint32_t minId = *reinterpret_cast<const uint32_t*>(0xAD3414), maxId = *reinterpret_cast<const uint32_t*>(0xAD3410);
        for (uint32_t id = minId; id <= maxId; ++id)
        {
            const uint8_t* rows = *reinterpret_cast<uint8_t* const*>(0xAD3424);   // FUN_100b13c0
            const uint8_t* rec = rows ? *reinterpret_cast<uint8_t* const*>(rows + (id - minId) * 4) : nullptr;
            if (!rec)
                continue;
            const uint8_t cls = rec[0];
            bool include = false;
            if (coa)
                include = cls >= 0xC && cls <= 0x20;
            else if (stock)
                include = (cls >= 1 && cls <= 9) || cls == 0xB;
            else if (hero)
                include = cls == 0xA;
            else
                return std::string();
            if (include)
                names.push_back(*reinterpret_cast<const char* const*>(rec + 0x10));
        }
        for (const char* n : names)
        {
            if (strstr(line, n))
                continue;
            std::string prefix = GlobalString("ITEM_CLASSES_ALLOWED");
            prefix.resize(prefix.size() - 3);   // "Classes: %s" -> "Classes:"
            std::string result = prefix + " ";
            std::string listed(line);
            if (listed.compare(0, prefix.size(), prefix) == 0)
                listed = listed.substr(9);
            std::unordered_set<std::string> present;   // FUN_102871a0: split on ',' and trim
            std::istringstream in(listed);
            std::string token;
            while (std::getline(in, token, ','))
            {
                size_t b = 0, e = token.size();
                while (b < e && isspace(static_cast<unsigned char>(token[b])))
                    ++b;
                while (e > b && isspace(static_cast<unsigned char>(token[e - 1])))
                    --e;
                present.insert(token.substr(b, e - b));
            }
            bool first = true;
            for (const char* m : names)
                if (present.count(m))
                {
                    if (!first)
                        result.append(", ", 2);
                    first = false;
                    result.append(m);
                }
            return result;
        }
        return std::string();
    }

    uint32_t g_tooltipSpell = 0;   // DAT_10D3C27C: set by the SetSpell detour 0x10A11130 (installer 10a6271a, not yet transcribed)

    typedef void(__fastcall* Fn61FEC0_t)(void*, void*, const char*, const char*, uint32_t*, uint32_t*, uint32_t);
    Fn61FEC0_t g_61FEC0 = nullptr;

    // FUN_10a10af0: under a spell tooltip built from 0x625231, the spell's required form (SpellCustomAttr
    // +0x18 bit 0x10000 opts out), coloured by whether the player is in it.
    void RequiredFormLine(void* tip)
    {
        const uint8_t* attr = AscCA::SpellCustomAttrRow(g_tooltipSpell);
        if (attr && (*reinterpret_cast<const uint32_t*>(attr + 0x18) & 0x10000))
            return;
        uint8_t spell[0x2A8];
        if (!AscScript::FetchSpell(g_tooltipSpell, spell))
            return;
        if (*reinterpret_cast<const uint32_t*>(spell + 0x28) >> 2 & 1)
            return;
        const uint32_t form = *reinterpret_cast<const uint32_t*>(spell + 0x60);
        if (!form)
            return;
        uint8_t* player = ActivePlayer();
        if (!player)
            return;
        uint8_t formSpell[0x2A8];
        if (!AscScript::FetchSpell(form, formSpell))
            return;
        const char* fmt = GetGlobalString("SPELL_REQUIRED_FORM", -1, 0);
        if (!fmt)
            return;
        const char* formName = *reinterpret_cast<const char* const*>(formSpell + 0x220);
        char text[3000];
        Format(text, sizeof(text), fmt, formName ? formName : "");
        uint32_t color = *reinterpret_cast<const uint32_t*>(0xAD2D30);
        if (!AscScript::UnitHasAuraSpell(player, form))
            color = *reinterpret_cast<const uint32_t*>(0xAD2D34);
        reinterpret_cast<void(__thiscall*)(void*, const char*, const char*, uint32_t*, uint32_t*, uint32_t)>(0x61FEC0)(
            tip, text, nullptr, &color, nullptr, 0);
    }

    void __fastcall Detour61FEC0(void* tip, void* edx, const char* left, const char* right, uint32_t* color, uint32_t* a4, uint32_t a5)
    {
        auto Original = [&](const char* l, const char* r, uint32_t* c, uint32_t* d) { g_61FEC0(tip, edx, l, r, c, d, a5); };

        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == 0x625231)
            RequiredFormLine(tip);
        if (left && *left && strcmp(left, "Requires Two-Handed Axes, Two-Handed Maces, Polearms, Two-Handed Swords, Staves") == 0)
            left = "Requires Two-Handed Melee Weapon";

        char scan[0x400];
        const std::string classes = GlobalString("ITEM_CLASSES_ALLOWED");
        if (!classes.empty() && left && *left && sscanf(left, classes.c_str(), scan) != 0)
        {
            const std::string line = ClassesLine(left);
            if (!line.empty())
                Original(line.c_str(), right, color, a4);
            return;
        }

        char out[0x400];
        char school[0x400] = {};
        int32_t min = 0, max = 0;
        {
            const std::string withSchool = GlobalString("DAMAGE_TEMPLATE_WITH_SCHOOL");
            const std::string damage = GlobalString("DAMAGE");
            if (left && *left && sscanf(left, withSchool.c_str(), &min, &max, school) == 3 && strcmp(school, damage.c_str()) != 0 &&
                ScaledDamage(&min, &max, 1))
            {
                if (min == 0 || max == 0)
                    ScaledDamage(&min, &max, 0);
                if (min != 0 && max != 0)
                {
                    g_damage[1] = min;   // 0x10BE2ECC
                    g_damage[3] = max;   // 0x10BE2ED4
                    Format(out, sizeof(out), GetGlobalString("DAMAGE_TEMPLATE_WITH_SCHOOL", -1, 1), min, max, school);
                    Original(out, right, color, a4);
                    return;
                }
            }
        }
        {
            const std::string plain = GlobalString("DAMAGE_TEMPLATE");
            if (left && *left && sscanf(left, plain.c_str(), &min, &max) == 2 && g_scaling)
                if (const uint8_t* rec = ScaledRecord())
                {
                    min = static_cast<int32_t>(*reinterpret_cast<const float*>(rec + 0x60));
                    max = static_cast<int32_t>(*reinterpret_cast<const float*>(rec + 0x64));
                    if (min != 0 && max != 0)
                    {
                        g_damage[0] = min;   // 0x10BE2EC8
                        g_damage[2] = max;   // 0x10BE2ED0
                        Format(out, sizeof(out), plain.c_str(), min, max);
                        Original(out, right, color, a4);
                        return;
                    }
                }
        }

        char name[0x400] = {};
        int32_t value = 0;
        if (left && sscanf(left, "%d %s", &value, name) == 2)
        {
            const std::string armor = GlobalString("ARMOR");
            const bool green = *color == 0xFF1EFF00 || *color == 0xFF00FF00;
            const bool rewrite = !green || (armor == name && g_rewriteFirst);
            if (g_itemCache && *reinterpret_cast<const uint32_t*>(g_itemCache + 0x14) != 7 && rewrite)
            {
                if (ScaledStat(name, &value))
                {
                    if (value != 0)
                    {
                        Format(out, sizeof(out), value > 0 ? "+%d %s" : "%d %s", value, name);
                        Original(out, right, color, a4);
                        if (armor == name)
                            g_rewriteFirst = 0;
                        return;
                    }
                }
                else if (ScaledArmorOrBlock(name, &value) && value != 0)
                {
                    Format(out, sizeof(out), "%d %s", value, name);
                    Original(out, right, color, a4);
                    g_rewriteFirst = 0;
                    return;
                }
            }
        }

        {
            const std::string itemLevel = GlobalString("ITEM_LEVEL");
            if (left && sscanf(left, itemLevel.c_str(), &value) == 1)
                if (const uint8_t* rec = ScaledRecord())
                    if (const int32_t level = *reinterpret_cast<const int32_t*>(rec + 0xC))
                    {
                        Format(out, sizeof(out), itemLevel.c_str(), level);
                        Original(out, right, color, a4);
                        return;
                    }
        }

        const std::string minLevel = GlobalString("ITEM_MIN_LEVEL");
        if (left)
        {
            const uint8_t* scaled = sscanf(left, minLevel.c_str(), &value) == 1 ? ScaledRecord() : nullptr;
            if (scaled)
            {
                int32_t required = *reinterpret_cast<const int32_t*>(scaled + 0xC) - 5;
                if (const uint8_t* rec = ScaledRecord())
                    required = *reinterpret_cast<const int32_t*>(rec + 0xA0);
                if (required != 0)
                {
                    Format(out, sizeof(out), minLevel.c_str(), required);
                    const uint8_t* player = ActivePlayer();
                    if (!player)
                        Original(out, right, color, a4);
                    else
                    {
                        uint32_t* c = reinterpret_cast<uint32_t*>(static_cast<uint32_t>(required) > UnitLevel(player) ? 0xAD2D34 : 0xAD2D30);
                        Original(out, right, c, c);
                    }
                    return;
                }
            }

            float dps = 0.0f;
            if (sscanf(left, "(%f damage per second)", &dps) == 1)
            {
                const int32_t min1 = g_damage[0], max1 = g_damage[2], min2 = g_damage[1], max2 = g_damage[3];
                if ((g_itemCache && min1 != 0 && max1 != 0) || (min2 != 0 && max2 != 0))
                {
                    const float average = static_cast<float>(UintToDouble(static_cast<uint32_t>(max1 + min1 + max2 + min2))) * 0.5f;
                    const float delay = static_cast<float>(UintToDouble(*reinterpret_cast<const uint32_t*>(g_itemCache + 0xF4))) / 1000.0f;
                    Format(out, sizeof(out), "(%.1f damage per second)", static_cast<double>(average / delay));
                    Original(out, right, color, a4);
                    return;
                }
            }

            if (strncmp(left, "Equip:", 6) == 0)
            {
                std::cmatch m;
                const std::regex increases("Equip: Increases (.*) by (\\d+).", std::regex::ECMAScript);
                if (std::regex_match(left, left + strlen(left), m, increases) && m[1].str().size() <= 0x40)
                {
                    strcpy_s(name, sizeof(name), m[1].str().c_str());
                    value = atoi(m[2].str().c_str());
                    const std::string pve = GetGlobalString("PVE_POWER_LABEL", -1, 1);
                    const std::string pvp = GetGlobalString("PVP_POWER_LABEL", -1, 1);
                    const bool power = std::string(name) == pve || std::string(name) == pvp;
                    const uint8_t* player = power ? ActivePlayer() : nullptr;
                    if (player && !PowerLabelShown(player))
                        return;   // the line is dropped
                    if (ScaledStat(name, &value) && value != 0)
                    {
                        Format(out, sizeof(out), "Equip: Increases %s by %d.", name, value);
                        Original(out, right, color, a4);
                        return;
                    }
                }
                {
                    const std::regex improves("Equip: Improves (.*) by (\\d+).", std::regex::ECMAScript);
                    if (std::regex_match(left, left + strlen(left), m, improves))
                    {
                        strcpy_s(name, sizeof(name), m[1].str().c_str());
                        value = atoi(m[2].str().c_str());
                        if (ScaledStat(name, &value) && value != 0)
                        {
                            Format(out, sizeof(out), "Equip: Improves %s by %d.", name, value);
                            Original(out, right, color, a4);
                            return;
                        }
                    }
                    const std::regex restores("Equip: Restores (\\d+) (.*).", std::regex::ECMAScript);
                    if (std::regex_match(left, left + strlen(left), m, restores))
                    {
                        value = atoi(m[1].str().c_str());
                        strcpy_s(name, sizeof(name), m[2].str().c_str());
                        if (ScaledStat(name, &value) && value != 0)
                        {
                            Format(out, sizeof(out), "Equip: Restores %d %s", value, name);
                            Original(out, right, color, a4);
                            return;
                        }
                    }
                }
            }
        }

        // No rewrite: the tooltip's OnTextChanged(self, left, right) may replace either text.
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        auto it = g_onTextChanged.find(tip);
        if (it != g_onTextChanged.end())
        {
            lua_rawgeti(L, LUA_REGISTRYINDEX, it->second);
            lua_rawgeti(L, LUA_REGISTRYINDEX, *reinterpret_cast<const int*>(static_cast<uint8_t*>(tip) + 8));
            AscLua::lua_pushstring(L, left ? left : "");
            AscLua::lua_pushstring(L, right ? right : "");
            if (lua_pcall(L, 3, 2, 0) == 0)
            {
                if (AscLua::lua_type(L, -2) != 0)
                    left = AscLua::lua_tolstring(L, -2, nullptr);
                if (AscLua::lua_type(L, -1) != 0)
                    right = AscLua::lua_tolstring(L, -1, nullptr);
            }
        }
        AscLua::lua_settop(L, top);
        Original(left, right, color, a4);
    }

    // ---- SMSG 0x716 (handler_0x0716): the inspected player's 19 slot item levels ------------------------------
    void __cdecl OnInspectLevels(void*, uint32_t, uint32_t, CDataStore* p)
    {
        for (uint32_t i = 0; i < 0x13; ++i)
        {
            g_inspectLevels[i] = *reinterpret_cast<const uint32_t*>(p->m_buffer + p->m_read);
            p->m_read += 4;
        }
    }

    void Init()
    {
        g_61FEC0 = reinterpret_cast<decltype(g_61FEC0)>(AscRuntime::Detour(0x61FEC0, 9, reinterpret_cast<void*>(&Detour61FEC0)));
        g_6277F0 = reinterpret_cast<decltype(g_6277F0)>(AscRuntime::Detour(0x6277F0, 8, reinterpret_cast<void*>(&Detour6277F0)));
        g_62D930 = reinterpret_cast<decltype(g_62D930)>(AscRuntime::Detour(0x62D930, 9, reinterpret_cast<void*>(&Detour62D930)));
        g_61A0D0 = reinterpret_cast<decltype(g_61A0D0)>(AscRuntime::Detour(0x61A0D0, 6, reinterpret_cast<void*>(&Detour61A0D0)));
        g_61A8F0 = reinterpret_cast<decltype(g_61A8F0)>(AscRuntime::Detour(0x61A8F0, 6, reinterpret_cast<void*>(&Detour61A8F0)));
        AscRuntime::ReplaceFunction(0x708540, reinterpret_cast<void*>(&RepairCost));
        g_754400 = reinterpret_cast<decltype(g_754400)>(AscRuntime::Detour(0x754400, 7, reinterpret_cast<void*>(&Detour754400)));
        g_5E9250 = reinterpret_cast<decltype(g_5E9250)>(AscRuntime::Detour(0x5E9250, 6, reinterpret_cast<void*>(&Detour5E9250)));
        g_61B4A0 = reinterpret_cast<decltype(g_61B4A0)>(AscRuntime::Detour(0x61B4A0, 7, reinterpret_cast<void*>(&Detour61B4A0)));
        g_HasScript = reinterpret_cast<decltype(g_HasScript)>(AscRuntime::Detour(0x49EAB0, 9, reinterpret_cast<void*>(&HasScript)));
        g_GetScript = reinterpret_cast<decltype(g_GetScript)>(AscRuntime::Detour(0x49EB70, 9, reinterpret_cast<void*>(&GetScript)));
        g_SetScript = reinterpret_cast<decltype(g_SetScript)>(AscRuntime::Detour(0x49EC80, 9, reinterpret_cast<void*>(&SetScript)));
        g_SetQuestItem = reinterpret_cast<decltype(g_SetQuestItem)>(AscRuntime::Detour(0x62E670, 8, reinterpret_cast<void*>(&SetQuestItem)));
        g_SetQuestLogItem = reinterpret_cast<decltype(g_SetQuestLogItem)>(AscRuntime::Detour(0x62E790, 8, reinterpret_cast<void*>(&SetQuestLogItem)));
        g_SetTradeSkillItem = reinterpret_cast<decltype(g_SetTradeSkillItem)>(AscRuntime::Detour(0x62EAE0, 8, reinterpret_cast<void*>(&SetTradeSkillItem)));
        g_SetTradePlayerItem = reinterpret_cast<decltype(g_SetTradePlayerItem)>(AscRuntime::Detour(0x62EFF0, 8, reinterpret_cast<void*>(&SetTradePlayerItem)));
        g_SetTradeTargetItem = reinterpret_cast<decltype(g_SetTradeTargetItem)>(AscRuntime::Detour(0x62F1E0, 8, reinterpret_cast<void*>(&SetTradeTargetItem)));
        g_SetInboxItem = reinterpret_cast<decltype(g_SetInboxItem)>(AscRuntime::Detour(0x62F9E0, 8, reinterpret_cast<void*>(&SetInboxItem)));
        g_SetAuctionItem = reinterpret_cast<decltype(g_SetAuctionItem)>(AscRuntime::Detour(0x62FCF0, 8, reinterpret_cast<void*>(&SetAuctionItem)));
        g_SetLootRollItem = reinterpret_cast<decltype(g_SetLootRollItem)>(AscRuntime::Detour(0x6300A0, 8, reinterpret_cast<void*>(&SetLootRollItem)));
        g_SetGuildBankItem = reinterpret_cast<decltype(g_SetGuildBankItem)>(AscRuntime::Detour(0x6304A0, 8, reinterpret_cast<void*>(&SetGuildBankItem)));
        g_SetHyperlinkCompareItem = reinterpret_cast<decltype(g_SetHyperlinkCompareItem)>(AscRuntime::Detour(0x631B60, 8, reinterpret_cast<void*>(&SetHyperlinkCompareItem)));
        sDC.AddPacketHandler(0x716, CNetClientCustomPacket((void*)&OnInspectLevels, nullptr));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
