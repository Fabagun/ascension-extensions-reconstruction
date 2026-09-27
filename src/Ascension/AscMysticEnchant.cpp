// C_MysticEnchant -- transcribed from the original's subsystem at 0x102DD000..0x102F1200.
//
// Manager DAT_10BE3928 (accessor FUN_102E9C20, ctor FUN_102E0260):
//   +0x00 vector<u32> known enchant spells       SMSG 0x5F9 (replace) / 0x5FA (add) / 0x5FB (remove)
//   +0x0C vector<u32> slot enchants (0-based)    SMSG 0x5FD (replace) / 0x5FE (one slot)
//   +0x18 vector<u32> zeroed alongside +0x0C     (never read by a binding)
//   +0x28 u64 progress, +0x30 level              SMSG 0x5FC / 0x619 / 0x604
//   +0x34 vector<{u32 slot (1-based), u32 spell}> pending collection reforges
//   +0x40 vector<{u32 slot (1-based), u32 item guid low}> pending applies
//   +0x4C u32 inspect target (guid low), +0x50 unordered_map<u32, InspectData> inspect results (0x614)
//   +0x70 u8 "altar open" (set by the altar-use detour, cleared by the 1 s proximity check)
//   +0x74 MysticEnchantContainer: FilterableContainerBase<MysticEnchantEntry, REFilterType, NullSortType>
//   +0xF0 vector<ScrollEntry (0x30)> the filtered mystic scrolls (SetMysticScrollFilter)
//
// Enchant rows are MysticEnchant.dbc (0x7C bytes, DAT_10BDFFF0) indexed twice by FUN_101E72E0 at DBC load:
// 0x10BE04C8 by spell (+0x04), 0x10BE0510 by item (+0x18); a later row with the same key wins.
//   +0x04 spell  +0x0C / +0x10 quality NAME (the +0x10 one for the stock "reborn" classes)  +0x14 level
//   +0x18 item   +0x1C worldforged  +0x20..+0x30 realm flags (live / seasonal / league / ptr / dev)
//   +0x34 / +0x38 class mask  +0x3C..+0x48 spec tag NAMES  +0x4C / +0x58 / +0x64 / +0x70 class, tab,
//   required AE, required TE (three each)
#include <Ascension/AscMysticEnchant.hpp>
#include <Ascension/AscAssetQuery.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscConfig.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellText.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
#include <Ascension/AscCAEnums.generated.inc>

    const uint32_t kMysticScroll = 0xF25D0;        // "Untarnished Mystic Scroll"
    const uint32_t kMarkOfAscension = 0x5B9D2;
    const uint32_t kMysticExtract = 0x1809F;
    const uint32_t kPresetUnlockToken = 0x1B9271;

    // ---- string tables (name, length) at the addresses given ------------------------------------
    // kReQualities (0x10B1B448) comes from AscCAEnums.generated.inc.
    const char* const kFilters[55] = {  // 0x10B55DA0
        "RE_FILTER_NONE", "RE_FILTER_ALL", "RE_FILTER_RELEVANT_POPULAR", "RE_FILTER_RELEVANT_ALL", "RE_FILTER_KNOWN",
        "RE_FILTER_UNKNOWN", "RE_FILTER_UNCOMMON", "RE_FILTER_RARE", "RE_FILTER_EPIC", "RE_FILTER_LEGENDARY",
        "RE_FILTER_ARTIFACT", "RE_FILTER_WORLDFORGED", "RE_FILTER_NOT_WORLDFORGED", "RE_FILTER_CLASS_WARRIOR",
        "RE_FILTER_CLASS_PALADIN", "RE_FILTER_CLASS_HUNTER", "RE_FILTER_CLASS_ROGUE", "RE_FILTER_CLASS_PRIEST",
        "RE_FILTER_CLASS_DEATH_KNIGHT", "RE_FILTER_CLASS_SHAMAN", "RE_FILTER_CLASS_MAGE", "RE_FILTER_CLASS_WARLOCK",
        "RE_FILTER_CLASS_HERO", "RE_FILTER_CLASS_DRUID", "RE_FILTER_CLASS_WARRIOR_SPEC_PROTECTION",
        "RE_FILTER_CLASS_WARRIOR_SPEC_ARMS", "RE_FILTER_CLASS_WARRIOR_SPEC_FURY",
        "RE_FILTER_CLASS_PALADIN_SPEC_PROTECTION", "RE_FILTER_CLASS_PALADIN_SPEC_HOLY",
        "RE_FILTER_CLASS_PALADIN_SPEC_RETRIBUTION", "RE_FILTER_CLASS_HUNTER_SPEC_SURVIVAL",
        "RE_FILTER_CLASS_HUNTER_SPEC_BEASTMASTERY", "RE_FILTER_CLASS_HUNTER_SPEC_MARKSMANSHIP",
        "RE_FILTER_CLASS_ROGUE_SPEC_ASSASSINATION", "RE_FILTER_CLASS_ROGUE_SPEC_COMBAT",
        "RE_FILTER_CLASS_ROGUE_SPEC_SUBTLETY", "RE_FILTER_CLASS_PRIEST_SPEC_DISCIPLINE",
        "RE_FILTER_CLASS_PRIEST_SPEC_HOLY", "RE_FILTER_CLASS_PRIEST_SPEC_SHADOW",
        "RE_FILTER_CLASS_DEATH_KNIGHT_SPEC_BLOOD", "RE_FILTER_CLASS_DEATH_KNIGHT_SPEC_FROST",
        "RE_FILTER_CLASS_DEATH_KNIGHT_SPEC_UNHOLY", "RE_FILTER_CLASS_SHAMAN_SPEC_ELEMENTAL",
        "RE_FILTER_CLASS_SHAMAN_SPEC_ENHANCEMENT", "RE_FILTER_CLASS_SHAMAN_SPEC_RESTORATION",
        "RE_FILTER_CLASS_MAGE_SPEC_ARCANE", "RE_FILTER_CLASS_MAGE_SPEC_FROST", "RE_FILTER_CLASS_MAGE_SPEC_FIRE",
        "RE_FILTER_CLASS_WARLOCK_SPEC_DEMONOLOGY", "RE_FILTER_CLASS_WARLOCK_SPEC_DESTRUCTION",
        "RE_FILTER_CLASS_WARLOCK_SPEC_AFFLICTION", "RE_FILTER_CLASS_HERO_SPEC_HERO",
        "RE_FILTER_CLASS_DRUID_SPEC_RESTORATION", "RE_FILTER_CLASS_DRUID_SPEC_BALANCE",
        "RE_FILTER_CLASS_DRUID_SPEC_FERAL"};
    const char* const kPurchase[8] = {  // 0x10B20748
        "RE_PURCHASE_OK", "RE_PURCHASE_UNKNOWN", "RE_PURCHASE_ITEM_NOT_FOUND", "RE_PURCHASE_NOT_ENOUGH_MONEY",
        "RE_PURCHASE_NOT_ENOUGH_SPACE", "RE_PURCHASE_NO_MYSTIC_ALTAR", "RE_PURCHASE_NOT_WHILE_CASTING",
        "RE_PURCHASE_BAD_CLASS"};
    const char* const kInspect[5] = {  // 0x10B1C600
        "RE_INSPECT_OK", "RE_INSPECT_UNKNOWN", "RE_INSPECT_PLAYER_NOT_FOUND", "RE_INSPECT_PLAYER_NOT_IN_MAP",
        "RE_INSPECT_BAD_CLASS"};
    const char* const kDestroy[7] = {  // 0x10B21D74
        "RE_DESTROY_OK", "RE_DESTROY_UNKNOWN", "RE_DESTROY_BAD_SLOT", "RE_DESTROY_NO_ENCHANT_APPLIED",
        "RE_DESTROY_NOT_WHILE_CASTING", "RE_DESTROY_BAD_CLASS", "RE_DESTROY_BUILD_DRAFT"};
    const char* const kApply[26] = {  // 0x10B21DE0
        "RE_APPLY_OK", "RE_APPLY_UNKNOWN", "RE_APPLY_BAD_ITEM", "RE_APPLY_NOT_MYSTIC_SCROLL", "RE_APPLY_BAD_SLOT",
        "RE_APPLY_DUPLICATE_BAG_SLOT_PAIR", "RE_APPLY_STACK_LIMIT", "RE_APPLY_UNCOMMON_LIMIT", "RE_APPLY_RARE_LIMIT",
        "RE_APPLY_EPIC_LIMIT", "RE_APPLY_LEGENDARY_LIMIT", "RE_APPLY_ARTIFACT_LIMIT", "RE_APPLY_UNHANDLED_LIMIT",
        "RE_APPLY_BAD_ENCHANTMENT", "RE_APPLY_BUILD_DRAFT", "RE_APPLY_NOT_WHILE_CASTING", "RE_APPLY_NO_MONEY",
        "RE_APPLY_BAD_CLASS", "RE_APPLY_BAD_REALM", "RE_APPLY_DISABLED_IN_WILDCARD",
        "RE_APPLY_RARE_WORLDFORGED_LIMIT", "RE_APPLY_REQUIRED_AE_INVESTMENT", "RE_APPLY_REQUIRED_TE_INVESTMENT",
        "RE_APPLY_TOO_LOW_LEVEL", "RE_APPLY_DUPLICATE_SLOT", "RE_APPLY_NO_MYSTIC_ALTAR"};
    const char* const kCollReforge[30] = {  // 0x10B1CAA0
        "RE_COLLECTION_REFORGE_OK", "RE_COLLECTION_REFORGE_UNKNOWN", "RE_COLLECTION_REFORGE_BAD_ITEM",
        "RE_COLLECTION_REFORGE_NOT_MYSTIC_SCROLL", "RE_COLLECTION_REFORGE_WORLDFORGED_SCROLL",
        "RE_COLLECTION_REFORGE_WORLDFORGED_ENCHANT", "RE_COLLECTION_REFORGE_BAD_SLOT",
        "RE_COLLECTION_REFORGE_BUILD_DRAFT", "RE_COLLECTION_REFORGE_BAD_CLASS",
        "RE_COLLECTION_REFORGE_ALREADY_APPLIED", "RE_COLLECTION_REFORGE_BAD_ENCHANTMENT",
        "RE_COLLECTION_REFORGE_NOT_KNOWN_ENCHANTMENT", "RE_COLLECTION_REFORGE_NO_MONEY",
        "RE_COLLECTION_REFORGE_NOT_IN_BATTLEGROUNDS", "RE_COLLECTION_REFORGE_DISABLED",
        "RE_COLLECTION_REFORGE_STACK_LIMIT", "RE_COLLECTION_REFORGE_UNCOMMON_LIMIT",
        "RE_COLLECTION_REFORGE_RARE_LIMIT", "RE_COLLECTION_REFORGE_EPIC_LIMIT",
        "RE_COLLECTION_REFORGE_LEGENDARY_LIMIT", "RE_COLLECTION_REFORGE_ARTIFACT_LIMIT",
        "RE_COLLECTION_REFORGE_UNHANDLED_LIMIT", "RE_COLLECTION_REFORGE_NO_MYSTIC_ALTAR",
        "RE_COLLECTION_REFORGE_NOT_WHILE_CASTING", "RE_COLLECTION_REFORGE_BAD_REALM",
        "RE_COLLECTION_REFORGE_DISABLED_IN_WILDCARD", "RE_COLLECTION_REFORGE_RARE_WORLDFORGED_LIMIT",
        "RE_COLLECTION_REFORGE_REQUIRED_AE_INVESTMENT", "RE_COLLECTION_REFORGE_REQUIRED_TE_INVESTMENT",
        "RE_COLLECTION_REFORGE_TOO_LOW_LEVEL"};
    const char* const kSpecTags[91] = {  // 0x10B1DAC8
        "NONE", "BRUTALITY", "TACTICS", "ANCESTRY", "VOODOO", "BREWING", "SHADOWHUNTING", "SLAYING", "FELBLOOD",
        "DEMONOLOGY", "BOLTSLINGER", "DARKNESS", "INQUISITION", "LIGHTNING", "WIND", "GIFTS", "WAR", "HELLFIRE",
        "DEFIANCE", "PROTECTION", "INSPIRATION", "GLADIATOR", "FIGHTING", "DISCIPLINE", "RUNES", "BLOOD", "FEROCITY",
        "PACKLEADER", "ARCHERY", "DUELING", "SURVIVAL", "DUALITY", "TIME", "DISPLACEMENT", "DEATH", "RIME",
        "ANIMATION", "INCINERATION", "DESTRUCTION", "DRACONIC", "GODBLADE", "CORRUPTION", "INFLUENCE",
        "ASTRALWARFARE", "TIDES", "MOONBOW", "PIETY", "BLESSINGS", "SERAPHIM", "FIREARMS", "INVENTION", "MECHANICS",
        "VENOM", "STALKING", "FORTITUDE", "REAPING", "SOUL", "DOMINATION", "PRIMAL", "GEOMANCY", "LIFE", "ARCANE",
        "RUNIC", "RIFTBLADE", "ARMS", "FURY", "HOLY", "RETRIBUTION", "BEASTMASTERY", "MARKSMANSHIP", "ASSASSINATION",
        "COMBAT", "SUBTLETY", "SHADOW", "FROST", "UNHOLY", "ELEMENTAL", "ENHANCEMENT", "RESTORATION", "FIRE",
        "AFFLICTION", "BALANCE", "FERAL", "HERO", "BULWARK", "HYDROMANCY", "VALKYR", "MOUNTAINKING", "VIZIER",
        "FLESHWEAVER", "WITCHKNIGHT"};
    const char* const kReforge[13] = {  // 0x10B1E5D8
        "RE_REFORGE_OK", "RE_REFORGE_UNKNOWN", "RE_REFORGE_BAD_ITEM", "RE_REFORGE_NOT_MYSTIC_SCROLL",
        "RE_REFORGE_WORLDFORGED_SCROLL", "RE_REFORGE_BAD_SLOT", "RE_REFORGE_BUILD_DRAFT", "RE_REFORGE_NO_MONEY",
        "RE_REFORGE_NOT_IN_BATTLEGROUNDS", "RE_REFORGE_NO_MYSTIC_ALTAR", "RE_REFORGE_NOT_WHILE_CASTING",
        "RE_REFORGE_BAD_CLASS", "RE_REFORGE_DISABLED_IN_WILDCARD"};
    const char* const kDisenchant[16] = {  // 0x10B1BCB8
        "RE_DISENCHANT_OK", "RE_DISENCHANT_UNKNOWN", "RE_DISENCHANT_BAD_ITEM", "RE_DISENCHANT_NOT_MYSTIC_SCROLL",
        "RE_DISENCHANT_BAD_SLOT", "RE_DISENCHANT_BUILD_DRAFT", "RE_DISENCHANT_NO_ENCHANTMENT",
        "RE_DISENCHANT_BAD_ENCHANTMENT", "RE_DISENCHANT_ALREADY_KNOWN_ENCHANTMENT", "RE_DISENCHANT_NO_MONEY",
        "RE_DISENCHANT_NOT_IN_BATTLEGROUNDS", "RE_DISENCHANT_DISABLED", "RE_DISENCHANT_NO_MYSTIC_ALTAR",
        "RE_DISENCHANT_NOT_WHILE_CASTING", "RE_DISENCHANT_BAD_REALM", "RE_DISENCHANT_DISABLED_IN_WILDCARD"};
    const char* const kEquip[19] = {  // 0x10B1E188
        "RE_EQUIP_OK", "RE_EQUIP_UNKNOWN", "RE_EQUIP_BAD_ENCHANTMENT", "RE_EQUIP_STACK_LIMIT",
        "RE_EQUIP_UNCOMMON_LIMIT", "RE_EQUIP_RARE_LIMIT", "RE_EQUIP_EPIC_LIMIT", "RE_EQUIP_LEGENDARY_LIMIT",
        "RE_EQUIP_ARTIFACT_LIMIT", "RE_EQUIP_UNHANDLED_LIMIT", "RE_EQUIP_BAD_CLASS", "RE_EQUIP_DISABLED",
        "RE_EQUIP_BAD_REALM", "RE_EQUIP_DISABLED_IN_WILDCARD", "RE_EQUIP_RARE_WORLDFORGED_LIMIT",
        "RE_EQUIP_SPELL_GROUP_STACK_RULES", "RE_EQUIP_REQUIRED_AE_INVESTMENT", "RE_EQUIP_REQUIRED_TE_INVESTMENT",
        "RE_EQUIP_TOO_LOW_LEVEL"};
    const char* const kExtract[4] = {  // 0x10B55D7C
        "RE_PURCHASE_MYSTIC_EXTRACT_OK", "RE_PURCHASE_MYSTIC_EXTRACT_UNKNOWN",
        "RE_PURCHASE_MYSTIC_EXTRACT_NO_TOKENS", "RE_PURCHASE_MYSTIC_EXTRACT_ALREADY_OBTAINED"};

    // The enum-to-string helpers (FUN_102DFEC0 & co): the name, or "UNEXPECTED_ENUM_VALUE_<n>".
    std::string EnumName(const char* const* table, uint32_t count, uint32_t v)
    {
        if (v < count)
            return table[v];
        return "UNEXPECTED_ENUM_VALUE_" + std::to_string(v);
    }
    // ... pushed through FUN_1009AFD0 (lua_pushstring of the std::string).
    void PushEnum(lua_State* L, const char* const* table, uint32_t count, uint32_t v)
    {
        PushStr(L, EnumName(table, count, v).c_str());
    }
    // FUN_10087260 over a table: the index whose name equals `s`, or -1.
    int EnumIndex(const char* const* table, uint32_t count, const std::string& s)
    {
        for (uint32_t i = 0; i < count; ++i)
            if (s == table[i])
                return static_cast<int>(i);
        return -1;
    }
    // FUN_10099DE0 / FUN_10099D10 pushed as the table FUN_1009ADC0 / FUN_1009AC40 builds: {[i] = name}.
    void PushEnumList(lua_State* L, const char* const* table, uint32_t count, const std::vector<uint32_t>& v)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushEnum(L, table, count, v[i]);
            AscLua::lua_settable(L, -3);
        }
    }

    // ---- the enchant table and its two indexes -----------------------------------------------
    AscDbc::Table& Dbc() { return AscDbc::Get("DBFilesClient\\MysticEnchant.dbc"); }
    typedef const uint8_t* Row;

    struct Index
    {
        bool built = false;
        std::unordered_map<uint32_t, Row> bySpell, byItem;   // 0x10BE04C8 / 0x10BE0510
    };
    Index& Idx()
    {
        static Index idx;
        if (!idx.built)
        {
            idx.built = true;
            AscDbc::Table& t = Dbc();
            for (uint32_t i = 0; i < t.Count(); ++i)
                if (Row r = t.RowAt(i))
                {
                    idx.bySpell[AscDbc::Table::U32(r, 4)] = r;
                    idx.byItem[AscDbc::Table::U32(r, 0x18)] = r;
                }
        }
        return idx;
    }
    // FUN_100A0420 on 0x10BE04C8 / 0x10BE0510 (no fallback provider is ever installed).
    Row BySpell(uint32_t spell)
    {
        auto& m = Idx().bySpell;
        auto it = m.find(spell);
        return it == m.end() ? nullptr : it->second;
    }
    Row ByItem(uint32_t item)
    {
        auto& m = Idx().byItem;
        auto it = m.find(item);
        return it == m.end() ? nullptr : it->second;
    }
    uint32_t U32(Row r, uint32_t off) { return AscDbc::Table::U32(r, off); }
    const char* Str(Row r, uint32_t off) { return Dbc().Str(r, off); }

    // ---- the player --------------------------------------------------------------------------
    uint8_t ClassOf(const uint8_t* unit) { return unit ? AscCA::UnitClassOf(unit) : 0; }
    // FUN_100C6380: the stock ("reborn") classes.
    bool IsStockClass(uint8_t c) { return (c >= 1 && c <= 9) || c == 11; }
    // FUN_100C5F50: the Conquest of Azeroth classes.
    bool IsCoAClass(uint8_t c) { return c >= 12 && c <= 32; }
    uint32_t UnitLevel(const uint8_t* unit) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0xD8); }
    uint32_t Coinage(const uint8_t* player) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0x1248); }
    bool Casting(const uint8_t* player) { return *reinterpret_cast<const uint32_t*>(player + 0xA60) != 0; }
    uint32_t EntryOf(const uint8_t* object) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(object + 8) + 0xC); }
    uint64_t GuidOf(const uint8_t* object) { return *reinterpret_cast<const uint64_t*>(*reinterpret_cast<uint8_t* const*>(object + 8)); }

    // FUN_100A55D0: the item guid for a low guid (high 0x4000 << 16), 0 for 0.
    uint64_t ItemGuid(uint32_t low) { return low ? (static_cast<uint64_t>(0x4000u << 16) << 32) | low : 0; }
    uint8_t* ItemObject(uint32_t low) { return static_cast<uint8_t*>(ObjectPtr(ItemGuid(low), 2)); }

    // FUN_100A03B0: the counter part of a guid.
    uint32_t GuidLow(uint64_t guid)
    {
        const uint32_t lo = static_cast<uint32_t>(guid), hi = static_cast<uint32_t>(guid >> 32);
        if (hi > 0x0FFFFFFF)
        {
            const uint32_t h = hi >> 16;
            if (h > 0xF101)
                return lo & 0xFFFFFF;
            if (h != 0xF101 && h != 0x1FC0 && h != 0 && h != 0x1F40 && h != 0x1F50 && h != 0x4000 && h != 0xF100)
                return lo & 0xFFFFFF;
        }
        return lo;
    }

    const uint64_t* BagGuids() { return reinterpret_cast<const uint64_t*>(0xC23540); }   // DAT_10BC91E0
    uint8_t* ContainerItem(uint8_t* container, uint32_t i)
    {
        return reinterpret_cast<uint8_t*(__thiscall*)(void*, uint32_t)>(0x754390)(container, i);
    }
    void* ContainerInventory(uint8_t* container)
    {
        typedef void*(__thiscall* Inventory_t)(void*);
        return (*reinterpret_cast<Inventory_t**>(container))[0x24 / 4](container);
    }
    uint32_t ContainerSlots(uint8_t* container) { return *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(container + 8) + 0x100); }
    uint64_t InvSlotGuid(const uint8_t* player, uint32_t slot) { return *reinterpret_cast<const uint64_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0x510 + slot * 8); }

    // FUN_10111D30 / FUN_10111E20: bag (0 = backpack, 1..4) and 1-based slot of an item the player holds.
    void BagAndSlot(const uint8_t* item, uint8_t& bag, uint8_t& slot)
    {
        bag = 0;
        slot = 0;
        const uint8_t* player = ActivePlayer();
        if (!player)
            return;
        const uint64_t guid = GuidOf(item);
        for (uint32_t s = 0x17; s < 0x27; ++s)
            if (InvSlotGuid(player, s) == guid)
            {
                slot = static_cast<uint8_t>(s - 0x16);
                return;
            }
        for (uint32_t b = 0; b < 4; ++b)
        {
            uint8_t* container = static_cast<uint8_t*>(ObjectPtr(BagGuids()[b], 4));
            if (!container || !ContainerInventory(container))
                continue;
            for (uint32_t i = 0; i < ContainerSlots(container); ++i)
            {
                uint8_t* it = ContainerItem(container, i);
                if (it && GuidOf(it) == guid)
                {
                    bag = static_cast<uint8_t>(b + 1);
                    slot = static_cast<uint8_t>(i + 1);
                    return;
                }
            }
        }
    }
    // The wire form every item-addressed CMSG uses: bag 0 -> 0xFF / slot + 0x16, bag n -> n + 0x12 / slot - 1.
    void WireBagSlot(const uint8_t* item, uint8_t& bagByte, uint8_t& slotByte)
    {
        uint8_t bag, slot;
        BagAndSlot(item, bag, slot);
        bagByte = bag ? static_cast<uint8_t>(bag + 0x12) : 0xFF;
        slotByte = bag ? static_cast<uint8_t>(slot - 1) : static_cast<uint8_t>(slot + 0x16);
    }

    // FUN_10308270: every item object the player holds (equipment, bag slots, backpack, bag contents).
    std::vector<uint8_t*> PlayerItems()
    {
        std::vector<uint8_t*> items;
        uint8_t* player = ActivePlayer();
        if (!player)
            return items;
        for (uint32_t s = 0; s < 0x27; ++s)
            if (uint8_t* item = static_cast<uint8_t*>(ObjectPtr(InvSlotGuid(player, s), 2)))
                items.push_back(item);
        for (uint32_t b = 0; b < 4; ++b)
        {
            uint8_t* container = static_cast<uint8_t*>(ObjectPtr(BagGuids()[b], 4));
            if (!container || !ContainerInventory(container))
                continue;
            for (uint32_t i = 0; i < ContainerSlots(container); ++i)
                if (uint8_t* item = ContainerItem(container, i))
                    items.push_back(item);
        }
        return items;
    }
    // FUN_10308700(unit, item, count): at least `count` of `item` in the currency-token slots 0x76..0x95.
    bool HasTokenCount(const uint8_t* unit, uint32_t item, uint32_t count)
    {
        if (!unit || GuidOf(unit) != ActivePlayerGuid())
            return count == 0;
        uint32_t n = 0;
        for (uint32_t s = 0x76; s < 0x96; ++s)
            if (uint8_t* it = static_cast<uint8_t*>(ObjectPtr(InvSlotGuid(unit, s), 2)))
                if (EntryOf(it) == item)
                    n += *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(it + 8) + 0x38);
        return count <= n;
    }
    // FUN_102EA1F0: {item, count} affordable -- the token slots first, else everything the player holds.
    bool HasItems(const uint8_t* player, uint32_t item, uint32_t count)
    {
        if (HasTokenCount(player, item, count))
            return true;
        uint32_t n = 0;
        for (uint8_t* it : PlayerItems())
            if (EntryOf(it) == item)
                n += *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(it + 8) + 0x38);
        return count <= n;
    }
    // FUN_103085F0: free backpack slots plus free slots in equipped bags.
    uint32_t FreeBagSlots()
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return 0;
        uint32_t n = 0;
        for (uint32_t s = 0x17; s < 0x27; ++s)
            if (InvSlotGuid(player, s) == 0)
                ++n;
        for (uint32_t b = 0; b < 4; ++b)
        {
            uint8_t* container = static_cast<uint8_t*>(ObjectPtr(BagGuids()[b], 4));
            if (!container || !ContainerInventory(container))
                continue;
            for (uint32_t i = 0; i < ContainerSlots(container); ++i)
                if (!ContainerItem(container, i))
                    ++n;
        }
        return n;
    }
    // 0x67CA30 on the item cache 0xC5D828: the client's item record, nullptr when not cached.
    const uint8_t* ItemCacheRecord(uint32_t id)
    {
        return static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67CA30)(
            reinterpret_cast<void*>(0xC5D828), id, nullptr, nullptr, nullptr, 0));
    }
    // CGItem_C::GetName (0x707C20, __thiscall(item, buffer, size)).
    std::string ClientItemName(uint8_t* item)
    {
        char buf[1024] = {};
        reinterpret_cast<void(__thiscall*)(void*, char*, uint32_t)>(0x707C20)(item, buf, 0x100);
        return buf;
    }

    // ---- row predicates -----------------------------------------------------------------------
    // FUN_102DF780: an RE_QUALITY_* name to its index; an unknown name is logged.
    uint32_t QualityIndex(const char* name)
    {
        const int i = EnumIndex(kReQualities, 9, name ? name : "");
        if (i >= 0)
            return static_cast<uint32_t>(i);
        AscLog::Printf("Unexpected Value: %s", name ? name : "");
        return 0;
    }
    // FUN_102E65D0: the row's quality -- the "reborn" (+0x10) column for a stock-class player.
    uint32_t QualityOf(Row r)
    {
        const uint8_t* player = ActivePlayer();
        if (player && IsStockClass(ClassOf(player)))
            return QualityIndex(Str(r, 0x10));
        return QualityIndex(Str(r, 0xC));
    }
    // The inlined class test: bit (class - 1) of the 64-bit mask at +0x34 / +0x38.
    bool ClassAllowed(Row r, uint8_t cls)
    {
        const uint32_t u = static_cast<uint32_t>(cls) - 1;
        uint32_t lo = 1u << (u & 31), hi = 0;
        if (u > 31)
            hi = lo;
        lo ^= hi;
        if (u > 63)
            hi = lo;
        return (lo & U32(r, 0x34)) != 0 || (hi & U32(r, 0x38)) != 0;
    }
    // FUN_102115C0 over the player.
    bool PlayerClassAllowed(Row r) { return ClassAllowed(r, ClassOf(ActivePlayer())); }
    // The realm gates (FUN_102FC660 / 6B0 / 650 / 670 / 640 on RealmInfo +0x40..+0x44).
    bool RealmAllows(Row r)
    {
        const RealmInfo& ri = RealmInfoSvc::Get();
        return (U32(r, 0x20) && ri.gates[0]) || (U32(r, 0x24) && ri.gates[1]) || (U32(r, 0x28) && ri.gates[2]) ||
               (U32(r, 0x2C) && ri.gates[3]) || (U32(r, 0x30) && ri.gates[4]);
    }
    // FUN_1031EF10 +0x00 bit 6 (the wildcard mode disables mystic enchanting).
    bool WildcardMode() { return (AscGameMode::Mode() >> 6) & 1; }
    // FUN_1031EFD0(FUN_1016F580()): the active spec is a build draft.
    bool DraftSpec() { return AscGameMode::SpecBuildDraft(AscGameMode::ActiveSpecIndex()); }
    // FUN_102115F0: a rare worldforged enchant.
    bool RareWorldforged(Row r) { return QualityOf(r) == 3 && U32(r, 0x1C) != 0; }
    // FUN_10211570(cls, strict) / FUN_10211500(cls, tab, strict) over the three class / tab slots.
    bool HasClassReq(Row r, uint32_t cls, bool strict)
    {
        if (!strict)
        {
            bool allNone = true;
            for (uint32_t i = 0; i < 3; ++i)
                allNone = allNone && U32(r, 0x4C + i * 4) == 1;
            if (allNone)
                return true;
        }
        for (uint32_t i = 0; i < 3; ++i)
            if (U32(r, 0x4C + i * 4) == cls)
                return true;
        return false;
    }
    bool HasTabReq(Row r, uint32_t cls, uint32_t tab, bool strict)
    {
        if (!strict)
        {
            bool allNone = true;
            for (uint32_t i = 0; i < 3; ++i)
                allNone = allNone && U32(r, 0x4C + i * 4) == 1;
            if (allNone)
                return true;
        }
        bool cl = false;
        for (uint32_t i = 0; i < 3; ++i)
            cl = cl || U32(r, 0x4C + i * 4) == cls;
        if (!cl)
            return false;
        for (uint32_t i = 0; i < 3; ++i)
            if (U32(r, 0x58 + i * 4) == tab)
                return true;
        return false;
    }
    // FUN_102113B0 / FUN_10211440: the AE (+0x64) / TE (+0x70) investment one of the three requirements
    // asks for is met by the active build (FUN_1016F390); no requirement at all passes.
    bool InvestmentMet(Row r, uint32_t base, bool te)
    {
        bool any = false;
        for (uint32_t i = 0; i < 3; ++i)
            any = any || U32(r, base + i * 4) != 0;
        if (!any)
            return true;
        const AscCA::Build* b = AscCA::ActiveBuild();
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t need = U32(r, base + i * 4);
            if (!need)
                continue;
            const uint32_t cls = U32(r, 0x4C + i * 4), tab = U32(r, 0x58 + i * 4);
            uint32_t have = 0;
            if (b)
            {
                if (cls == 1)
                    have = te ? b->GlobalTE(0) : b->GlobalAE(0);
                else if (tab == 1)
                    have = te ? b->ClassTE(cls, 0) : b->ClassAE(cls, 0);
                else
                    have = te ? b->TabTE(cls, tab, 0) : b->TabAE(cls, tab, 0);
            }
            if (need <= have)
                return true;
        }
        return false;
    }
    bool AEMet(Row r) { return InvestmentMet(r, 0x64, false); }
    bool TEMet(Row r) { return InvestmentMet(r, 0x70, true); }

    // ---- config ---------------------------------------------------------------------------------
    int32_t ConfigInt(const std::string& name, int32_t def)    // FUN_10196AB0
    {
        const int32_t* v = AscConfig::Int(name.c_str());
        return v ? *v : def;
    }
    bool ConfigBool(const std::string& name, bool def)         // FUN_10196990
    {
        const bool* v = AscConfig::Bool(name.c_str());
        return v ? *v : def;
    }

    // FUN_102E9CC0: a class-fusion character (only slots 1..15 exist).
    bool Fusion()
    {
        const uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        if (!ConfigBool("CONFIG_CLASS_FUSION_ENABLED", false))
            return false;
        if (ClassOf(player) != 10)
            return false;
        return (AscGameMode::Mode() & 0x1DEF) == 0;
    }
    bool SlotValid(uint32_t slot0) { return slot0 < 0x11 && (!Fusion() || slot0 - 1 < 0xF); }
}

// ---- the manager ---------------------------------------------------------------------------------
namespace
{
    struct Pair { uint32_t slot; uint32_t value; };   // slot is 1-based
    struct InspectData
    {
        uint64_t progress = 0;
        uint32_t level = 0;
        std::vector<uint32_t> applied, known;
    };
    struct ScrollEntry   // 0x30
    {
        uint32_t guid = 0, entry = 0;
        std::string name;
        uint8_t bag = 0, slot = 0;
        uint32_t quality = 0, spell = 0;
        bool known = false;
    };

    class EnchantContainer : public AscFilter::Container<Row>
    {
    protected:
        std::vector<Row> DoPopulate() override;
        bool ArgMatch(uint32_t f, const Row& r) override;
        uint32_t ArgGroup(uint32_t f) override;
        bool TextMatch(const std::string& lowerText, const Row& r) override;
        std::string FilterKey(const Row& r) override;
    public:
        bool MatchArg(uint32_t f, Row r) { return ArgMatch(f, r); }
        std::string lastRawText;   // what TextMatch compares (the original passes the un-lowered text)
    };

    struct Mgr
    {
        std::vector<uint32_t> known;                    // +0x00
        std::vector<uint32_t> slots, slotsAux;          // +0x0C / +0x18
        uint64_t progress = 0;                          // +0x28
        uint32_t level = 0;                             // +0x30
        std::vector<Pair> pendingReforge;               // +0x34
        std::vector<Pair> pendingApply;                 // +0x40
        uint32_t inspectKey = 0;                        // +0x4C
        std::unordered_map<uint32_t, InspectData> inspect;   // +0x50
        bool altarOpen = false;                         // +0x70
        EnchantContainer container;                     // +0x74
        std::vector<ScrollEntry> scrolls;               // +0xF0
        __time64_t altarCheck = 0;                      // DAT_10BE3920
        std::vector<uint8_t*> nearby;                   // DAT_10BE3914
    };
    Mgr& M() { static Mgr m; return m; }

    bool Known(uint32_t spell)   // FUN_102E96B0
    {
        const auto& k = M().known;
        return std::find(k.begin(), k.end(), spell) != k.end();
    }
    // FUN_102E3FC0
    uint32_t SlotAt(uint32_t slot0)
    {
        if (slot0 < 0x11 && (!Fusion() || slot0 - 1 < 0xF) && slot0 < M().slots.size())
            return M().slots[slot0];
        return 0;
    }

    // FUN_102E96E0: a mystic altar (a game object whose entry FUN_102EA0C0 lists) within 10 yards.
    bool IsAltar(uint32_t entry)
    {
        static const uint32_t kAltars[] = {0x30, 0x13914, 0x2B18A, 0x2B18B, 0x2B18D, 0x3BD0E, 0x57390, 0xC354F,
                                           0xF428F, 0x1D0B98, 0x1D0B99, 0x3174B8, 0x3182BE, 0x3183CC, 0x3183CE,
                                           0x3183ED, 0x31869E, 0x3188F0, 0x31956C, 0x31A0B9, 7100000, 0x7A1233,
                                           0x7A1234, 0x843F77};
        for (uint32_t a : kAltars)
            if (a == entry)
                return true;
        return false;
    }
    int __cdecl CollectGameObject(uint64_t guid, void*)   // LAB_102DD3B0
    {
        if (uint8_t* o = static_cast<uint8_t*>(ObjectPtr(guid, 0x20)))
            M().nearby.push_back(o);
        return 1;
    }
    void Position(uint8_t* object, float* xyz)
    {
        typedef void(__thiscall* GetPosition_t)(void*, float*);
        (*reinterpret_cast<GetPosition_t**>(object))[0x2C / 4](object, xyz);
    }
    bool AltarNearby()
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return false;
        float p[3];
        Position(player, p);
        M().nearby.clear();
        reinterpret_cast<int(__cdecl*)(int(__cdecl*)(uint64_t, void*), void*)>(0x4D4B30)(&CollectGameObject, nullptr);
        for (uint8_t* o : M().nearby)
        {
            float q[3];
            Position(o, q);
            const float dz = static_cast<float>(std::pow(static_cast<double>(q[2] - p[2]), 2.0));
            const float dx = static_cast<float>(std::pow(static_cast<double>(q[0] - p[0]), 2.0));
            const float dy = static_cast<float>(std::pow(static_cast<double>(q[1] - p[1]), 2.0));
            const float d = static_cast<float>(std::sqrt(static_cast<double>(dy + dx + dz)));
            if (d <= 10.0f && *reinterpret_cast<const uint32_t*>(o + 0x14) == 5 && IsAltar(EntryOf(o)))
                return true;
        }
        return false;
    }

    // ---- progress --------------------------------------------------------------------------------
    // FUN_102E51A0: the progress a level needs.
    uint64_t LevelProgress(uint32_t level)
    {
        if (level == 0)
            return 1;
        if (level > 0xF9)
            return static_cast<uint32_t>(level * 0x1001 - 0x72038);
        const double l = static_cast<double>(level);
        return static_cast<uint64_t>(std::floor(l * 7.5 * l + static_cast<double>(static_cast<int32_t>(level * 0x162))));
    }
    // GetProgress / SMSG 0x619: (progress - need(level - 1)) / (need(level) - need(level - 1)) * 100.
    double ProgressPercent(uint64_t progress, uint32_t level)
    {
        const uint32_t prev = level ? level - 1 : 0;
        // FUN_10AE6420: the differences convert as UNSIGNED 64-bit (level 0 gives +inf, not -inf).
        const double span = static_cast<double>(LevelProgress(level) - LevelProgress(prev));
        const double into = static_cast<double>(progress - LevelProgress(prev));
        return into / span * 100.0;
    }
    // FUN_102E6530: the same ratio (a float, not scaled) for an inspect result.
    float InspectProgress(const InspectData& d)
    {
        if (d.level == 0)
            return 0.0f;
        const uint64_t a = LevelProgress((d.level > 1 ? d.level : 1) - 1);
        const uint64_t b = LevelProgress(d.level);
        if (b == a)
            return 0.0f;
        return static_cast<float>(d.progress - a) / static_cast<float>(b - a);   // FUN_10AE64C0: unsigned
    }

    // ---- the projected slot maps FUN_102E6620 / FUN_102E6DF0 / FUN_102E7390 -------------------------
    // Slot -> enchant for the first `count` (at most 17) slots, then (with `overlay`) the pending applies and
    // collection reforges, then `slot` <- `spell` when a spell is given.
    std::unordered_map<uint32_t, uint32_t> Projected(bool overlay, uint32_t slot0, uint32_t spell, uint32_t count)
    {
        std::unordered_map<uint32_t, uint32_t> m;
        if (count > 0x11)
            count = 0x11;
        for (uint32_t i = 0; i < count; ++i)
            m[i] = SlotAt(i);
        if (overlay)
        {
            for (const Pair& p : M().pendingApply)
                if (uint8_t* item = ItemObject(p.value))
                    if (Row r = ByItem(EntryOf(item)))
                        m[p.slot - 1] = U32(r, 4);
            for (const Pair& p : M().pendingReforge)
                m[p.slot - 1] = p.value;
        }
        if (spell != 0)
            m[slot0] = spell;
        return m;
    }
    uint32_t CountSpell(uint32_t which, bool overlay, uint32_t slot0, uint32_t spell, uint32_t count)   // FUN_102E6DF0
    {
        uint32_t n = 0;
        for (const auto& kv : Projected(overlay, slot0, spell, count))
            n += kv.second == which;
        return n;
    }
    uint32_t CountQuality(uint32_t q, bool overlay, uint32_t slot0, uint32_t spell, uint32_t count)   // FUN_102E7390
    {
        uint32_t n = 0;
        for (const auto& kv : Projected(overlay, slot0, spell, count))
            if (Row r = BySpell(kv.second))
                n += QualityOf(r) == q;
        return n;
    }
    uint32_t CountRareWorldforged(bool overlay, uint32_t slot0, uint32_t spell, uint32_t count)   // FUN_102E6620
    {
        uint32_t n = 0;
        for (const auto& kv : Projected(overlay, slot0, spell, count))
            if (Row r = BySpell(kv.second))
                n += RareWorldforged(r);
        return n;
    }

    // FUN_102E0790: how many of the first `n` "<fmt>{i}" unlock levels the player has reached, capped.
    uint32_t UnlockedCount(uint32_t cap, uint32_t n, const char* fmt)
    {
        uint32_t c = 0;
        const uint8_t* player = ActivePlayer();
        const uint32_t level = player ? UnitLevel(player) : 0;
        for (uint32_t i = 1; i <= n; ++i)
            if (static_cast<uint32_t>(ConfigInt(fmt + std::to_string(i), 0)) <= level)
                ++c;
        return cap < c ? cap : c;
    }
    // The per-quality unlock loop of FUN_102E5250 (the "REBORN" keys for a stock-class player).
    uint32_t UnlockLoop(uint32_t max, const char* normal, const char* reborn)
    {
        for (uint32_t i = 0;;)
        {
            const uint8_t* player = ActivePlayer();
            const uint32_t level = player ? UnitLevel(player) : 0;
            const char* fmt = IsStockClass(ClassOf(player)) ? reborn : normal;
            const uint32_t need = static_cast<uint32_t>(ConfigInt(fmt + std::to_string(i + 1), 0));
            if (level < need)
                return i;
            ++i;
            if (max <= i)
                return max;
        }
    }
    // FUN_102E5250: how many enchants of quality q may be equipped.
    uint32_t QualityCap(uint32_t q)
    {
        if (!Fusion())
        {
            const bool stock = IsStockClass(ClassOf(ActivePlayer()));
            uint32_t max;
            switch (q)
            {
            case 2:
                return 0x11;
            case 3:
                max = static_cast<uint32_t>(ConfigInt(stock ? "CONFIG_MAX_RARE_RANDOM_ENCHANTS_REBORN" : "CONFIG_MAX_RARE_RANDOM_ENCHANTS", 0x11));
                return max ? UnlockLoop(max, "CONFIG_RARE_RANDOM_ENCHANT_UNLOCK_LEVEL_", "CONFIG_RARE_RANDOM_ENCHANT_REBORN_UNLOCK_LEVEL_") : 0;
            case 4:
                max = static_cast<uint32_t>(stock ? ConfigInt("CONFIG_MAX_EPIC_RANDOM_ENCHANTS_REBORN", 5) : ConfigInt("CONFIG_MAX_EPIC_RANDOM_ENCHANTS", 3));
                return max ? UnlockLoop(max, "CONFIG_EPIC_RANDOM_ENCHANT_UNLOCK_LEVEL_", "CONFIG_EPIC_RANDOM_ENCHANT_REBORN_UNLOCK_LEVEL_") : 0;
            case 5:
                max = static_cast<uint32_t>(ConfigInt(stock ? "CONFIG_MAX_LEGENDARY_RANDOM_ENCHANTS_REBORN" : "CONFIG_MAX_LEGENDARY_RANDOM_ENCHANTS", 1));
                return max ? UnlockLoop(max, "CONFIG_LEGENDARY_RANDOM_ENCHANT_UNLOCK_LEVEL_", "CONFIG_LEGENDARY_RANDOM_ENCHANT_REBORN_UNLOCK_LEVEL_") : 0;
            case 6:
                max = static_cast<uint32_t>(ConfigInt(stock ? "CONFIG_MAX_ARTIFACT_RANDOM_ENCHANTS_REBORN" : "CONFIG_MAX_ARTIFACT_RANDOM_ENCHANTS", 1));
                return max ? UnlockLoop(max, "CONFIG_ARTIFACT_RANDOM_ENCHANT_UNLOCK_LEVEL_", "CONFIG_ARTIFACT_RANDOM_ENCHANT_REBORN_UNLOCK_LEVEL_") : 0;
            default:
                return 0;
            }
        }
        switch (q)
        {
        case 2:
            return UnlockedCount(static_cast<uint32_t>(ConfigInt("CONFIG_MAX_UNCOMMON_RANDOM_ENCHANTS", 6)), 6, "CONFIG_UNCOMMON_RANDOM_ENCHANT_UNLOCK_LEVEL_");
        case 3:
            return UnlockedCount(static_cast<uint32_t>(ConfigInt("CONFIG_MAX_RARE_RANDOM_ENCHANTS", 5)), 5, "CONFIG_RARE_RANDOM_ENCHANT_UNLOCK_LEVEL_");
        case 4:
            return UnlockedCount(static_cast<uint32_t>(ConfigInt("CONFIG_MAX_EPIC_RANDOM_ENCHANTS", 4)), 4, "CONFIG_EPIC_RANDOM_ENCHANT_UNLOCK_LEVEL_");
        default:
            return 0;
        }
    }

    // ---- costs ------------------------------------------------------------------------------------
    // FUN_102E4000(row, money, slotVariant): the collection-reforge cost; -1 for "no token price". An
    // unexpected quality returns the quality itself (the original returns the switch value).
    uint32_t CollectionReforgeCost(Row r, bool money, bool slot)
    {
        const uint32_t q = QualityOf(r);
        const char* name = nullptr;
        int32_t def = 0;
        switch (q)
        {
        case 2:
            if (!slot) { if (!money) return 0xFFFFFFFF; name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ITEM_UNCOMMON_MONEY_COST"; def = 300000; }
            else if (money) { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_UNCOMMON_MONEY_COST"; def = 300000; }
            else { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_UNCOMMON_TOKEN_COST"; def = 0x4B0; }
            break;
        case 3:
            if (!slot) { if (!money) return 0xFFFFFFFF; name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ITEM_RARE_MONEY_COST"; def = 600000; }
            else if (money) { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_RARE_MONEY_COST"; def = 600000; }
            else { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_RARE_TOKEN_COST"; def = 0x960; }
            break;
        case 4:
            if (!slot) { if (!money) return 0xFFFFFFFF; name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ITEM_EPIC_MONEY_COST"; def = 1000000; }
            else if (money) { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_EPIC_MONEY_COST"; def = 1000000; }
            else { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_EPIC_TOKEN_COST"; def = 4000; }
            break;
        case 5:
            if (!slot) { if (!money) return 0xFFFFFFFF; name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ITEM_LEGENDARY_MONEY_COST"; def = 2500000; }
            else if (money) { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_LEGENDARY_MONEY_COST"; def = 2500000; }
            else { name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_LEGENDARY_TOKEN_COST"; def = 10000; }
            break;
        case 6:
            if (slot) { name = money ? "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ARTIFACT_MONEY_COST" : "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ARTIFACT_TOKEN_COST"; def = money ? 2500000 : 10000; }
            else { if (!money) return 0xFFFFFFFF; name = "CONFIG_MYSTIC_ENCHANT_COLLECTION_REFORGE_ITEM_ARTIFACT_MONEY_COST"; def = 2500000; }
            break;
        default:
            return q;
        }
        return static_cast<uint32_t>(ConfigInt(name, def));
    }
    // FUN_102E4AA0: the extract (disenchant) token cost.
    uint32_t ExtractCost(Row r)
    {
        const uint32_t q = QualityOf(r);
        switch (q)
        {
        case 2: return static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_EXTRACT_UNCOMMON_TOKEN_COST", 0x9C4));
        case 3: return static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_EXTRACT_RARE_TOKEN_COST", 5000));
        case 4: return static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_EXTRACT_EPIC_TOKEN_COST", 10000));
        case 5: return static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_EXTRACT_LEGENDARY_TOKEN_COST", 20000));
        case 6: return static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_EXTRACT_ARTIFACT_TOKEN_COST", 20000));
        default: return q;
        }
    }
    // FUN_102E6C50
    uint32_t ReforgeCost(bool money)
    {
        return money ? static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_REFORGE_MONEY_COST", 25000))
                     : static_cast<uint32_t>(ConfigInt("CONFIG_MYSTIC_ENCHANT_REFORGE_TOKEN_COST", 0xFA));
    }
    // The extract purchase (FUN_102E2CC0 and the bindings): level * 200 + 1000, at most 5000.
    uint32_t ExtractPurchaseCost()
    {
        const uint32_t v = M().level * 200 + 1000;
        return v > 5000 ? 5000 : v;
    }

    uint32_t SatAdd(uint32_t a, uint32_t b) { return b > ~a ? 0xFFFFFFFF : a + b; }

    // The cost object FUN_102E0B20 accumulates into: money plus {item, count} pairs.
    struct Cost
    {
        uint32_t money = 0;
        std::vector<std::pair<uint32_t, uint32_t>> items;
        uint32_t Count(uint32_t item) const
        {
            for (const auto& p : items)
                if (p.first == item)
                    return p.second;
            return 0;
        }
        void Add(uint32_t item, uint32_t n)
        {
            for (auto& p : items)
                if (p.first == item)
                {
                    p.second = SatAdd(p.second, n);
                    return;
                }
            items.emplace_back(item, n);
        }
    };
    // The shared walk of FUN_102E0BA0 (affordable?) and FUN_102E48C0 (totals): each row pays in Marks of
    // Ascension when the player holds enough (cumulatively), else in money if that still fits.
    bool Tally(const std::vector<Row>& rows, bool slot, Cost& c, bool strict)
    {
        const uint8_t* player = ActivePlayer();
        for (Row r : rows)
        {
            if (!r)   // CanApplyItem with no scroll row: the original dereferences null here
                continue;
            const uint32_t tok = CollectionReforgeCost(r, false, slot);
            const uint32_t money = CollectionReforgeCost(r, true, slot);
            const bool tokOk = tok != 0 && tok != 0xFFFFFFFF;
            const bool moneyOk = money != 0 && money != 0xFFFFFFFF;
            if (tokOk && HasItems(player, kMarkOfAscension, SatAdd(c.Count(kMarkOfAscension), tok)))
            {
                c.Add(kMarkOfAscension, tok);
                continue;
            }
            if (!moneyOk)
            {
                if (strict)
                    return false;
                continue;
            }
            if (Coinage(player) < SatAdd(money, c.money))
            {
                if (strict)
                    return false;
                continue;
            }
            c.money = SatAdd(money, c.money);
        }
        return !strict || c.money <= Coinage(player);
    }
    bool Affordable(const std::vector<Row>& rows, bool slot)   // FUN_102E0BA0
    {
        Cost c;
        return ActivePlayer() && Tally(rows, slot, c, true);
    }
    void Totals(const std::vector<Row>& rows, bool slot, uint32_t& money, uint32_t& tokens)   // FUN_102E48C0
    {
        Cost c;
        if (ActivePlayer())
            Tally(rows, slot, c, false);
        money = c.money;
        tokens = c.Count(kMarkOfAscension);
    }
    // FUN_102E0D60: the plain reforge is affordable.
    bool ReforgeAffordable()
    {
        const uint8_t* player = ActivePlayer();
        const uint32_t tok = ReforgeCost(false), money = ReforgeCost(true);
        if (tok != 0 && HasItems(player, kMarkOfAscension, tok))
            return true;
        if (money == 0)
            return false;
        return money <= Coinage(player);
    }

    std::vector<Row> PendingReforgeRows()
    {
        std::vector<Row> rows;
        for (const Pair& p : M().pendingReforge)
            if (Row r = BySpell(p.value))
                rows.push_back(r);
        return rows;
    }

    // ---- validators (each returns the original's error vector; the bindings use the first) -------------
    // FUN_102E0F00: the apply gates every apply check shares.
    void ApplyGates(std::vector<uint32_t>& e)
    {
        if (DraftSpec())
            e.push_back(0xE);
        const uint8_t* player = ActivePlayer();
        if (player && Casting(player))
            e.push_back(0xF);
        if (WildcardMode())
            e.push_back(0x13);
    }
    // FUN_102E1500: can `spell` go into slot0 (stack, quality, level, class, realm).
    void ApplyEnchantChecks(uint32_t slot0, uint32_t spell, std::vector<uint32_t>& e)
    {
        Row r = BySpell(spell);
        if (!r)
        {
            e.push_back(0xD);
            return;
        }
        uint8_t rec[0x2A8];
        const uint32_t maxStacks = FetchSpell(spell, rec) ? std::max<uint32_t>(1, *reinterpret_cast<uint32_t*>(rec + 0xC4)) : 0;
        if (maxStacks < CountSpell(spell, true, slot0, spell, 0xFFFFFFFF))
            e.push_back(6);
        const uint32_t q = QualityOf(r);
        if (QualityCap(q) < CountQuality(q, true, slot0, spell, 0xFFFFFFFF))
        {
            switch (q)
            {
            case 2: e.push_back(7); break;
            case 3: e.push_back(8); break;
            case 4: e.push_back(9); break;
            case 5: e.push_back(10); break;
            case 6: e.push_back(0xB); break;
            default: e.push_back(0xC);
            }
        }
        if (RareWorldforged(r) && CountRareWorldforged(true, slot0, spell, 0xFFFFFFFF) > 3)
            e.push_back(0x14);
        if (!AEMet(r))
            e.push_back(0x15);
        if (!TEMet(r))
            e.push_back(0x16);
        const uint8_t* player = ActivePlayer();
        if (player && UnitLevel(player) < U32(r, 0x14))
            e.push_back(0x17);
        if (player && !ClassAllowed(r, ClassOf(player)))
            e.push_back(0x11);
        if (!RealmAllows(r))
            e.push_back(0x12);
        else if (WildcardMode())
            e.push_back(0x13);
    }
    // FUN_102E1360: CanApplySlot(slot0, item).
    std::vector<uint32_t> CheckApplySlot(uint32_t slot0, uint32_t itemLow)
    {
        std::vector<uint32_t> e;
        if (!SlotValid(slot0))
            e.push_back(4);
        uint8_t* item = ItemObject(itemLow);
        if (!item)
            e.push_back(2);
        else if (Row r = ByItem(EntryOf(item)))
            ApplyEnchantChecks(slot0, U32(r, 4), e);
        else
            e.push_back(3);
        ApplyGates(e);
        for (const Pair& p : M().pendingApply)
            if (p.value == itemLow && p.slot - 1 != slot0)
            {
                e.push_back(5);
                break;
            }
        return e;
    }
    // FUN_102E0FE0: CanApplyItem(slot0, item).
    std::vector<uint32_t> CheckApplyItem(uint32_t slot0, uint32_t itemLow)
    {
        std::vector<uint32_t> e;
        if (!SlotValid(slot0))
            e.push_back(4);
        uint8_t* item = ItemObject(itemLow);
        if (!item)
            e.push_back(2);
        else
        {
            Row scroll = ByItem(EntryOf(item));
            if (!scroll)
                e.push_back(3);
            if (Row cur = BySpell(SlotAt(slot0)))
            {
                Row r = BySpell(U32(cur, 4));
                if (!r)
                    e.push_back(0xD);
                else if (!RealmAllows(r))
                    e.push_back(0x12);
                else if (WildcardMode())
                    e.push_back(0x13);
                if (!Affordable(std::vector<Row>{scroll}, false))
                    e.push_back(0x10);
            }
        }
        ApplyGates(e);
        if (Fusion() && !AltarNearby())
            e.push_back(0x19);
        return e;
    }
    // FUN_102E18F0: the collection-reforge checks shared by the item and slot forms.
    void ReforgeTargetChecks(uint32_t current, uint32_t spell, std::vector<uint32_t>& e)
    {
        if (current == spell)
            e.push_back(9);
        if (Row r = BySpell(spell))
        {
            if (!Known(U32(r, 4)))
                e.push_back(0xB);
            if (!RealmAllows(r))
                e.push_back(0x18);
            else if (WildcardMode())
                e.push_back(0x19);
        }
        else
            e.push_back(10);
        if (!AltarNearby())
            e.push_back(0x16);
        const uint8_t* player = ActivePlayer();
        if (player && Casting(player))
            e.push_back(0x17);
    }
    struct ReforgeCosts { uint32_t moneyDelta = 0, tokenDelta = 0, money = 0, tokens = 0; };
    // FUN_102E1AB0: CanCollectionReforgeItem(item, spell).
    std::vector<uint32_t> CheckCollectionReforgeItem(uint32_t itemLow, uint32_t spell, ReforgeCosts& out)
    {
        std::vector<uint32_t> e;
        out = ReforgeCosts();
        uint32_t current = 0;
        uint8_t* item = ItemObject(itemLow);
        if (!item)
            e.push_back(2);
        else if (EntryOf(item) != kMysticScroll)
        {
            if (Row s = ByItem(EntryOf(item)))
            {
                if (U32(s, 0x1C))
                    e.push_back(4);
                current = U32(s, 4);
            }
            else
                e.push_back(3);
        }
        ReforgeTargetChecks(current, spell, e);
        Row r = BySpell(spell);
        if (!r)
            return e;
        std::vector<Row> rows = PendingReforgeRows();
        uint32_t money0, tokens0;
        Totals(rows, false, money0, tokens0);
        rows.push_back(r);
        if (!Affordable(rows, false))
            e.push_back(0xC);
        if (U32(r, 0x1C))
            e.push_back(5);
        Totals(rows, false, out.money, out.tokens);
        out.moneyDelta = money0 < out.money ? out.money - money0 : 0;
        out.tokenDelta = tokens0 < out.tokens ? out.tokens - tokens0 : 0;
        return e;
    }
    // FUN_102E1E70: CanCollectionReforgeSlot(slot0, spell); `pending` = the target is already in the
    // pending list (the save check).
    std::vector<uint32_t> CheckCollectionReforgeSlot(uint32_t slot0, uint32_t spell, bool pending, ReforgeCosts& out)
    {
        std::vector<uint32_t> e;
        out = ReforgeCosts();
        if (!SlotValid(slot0))
            e.push_back(6);
        if (DraftSpec())
            e.push_back(7);
        ReforgeTargetChecks(SlotAt(slot0), spell, e);
        Row r = BySpell(spell);
        if (!r)
            return e;
        std::vector<Row> rows = PendingReforgeRows();
        uint32_t money0, tokens0;
        Totals(rows, true, money0, tokens0);
        if (!pending)
            rows.push_back(r);
        if (!Affordable(rows, true))
            e.push_back(0xC);
        const uint8_t* player = ActivePlayer();
        if (player && !ClassAllowed(r, ClassOf(player)))
            e.push_back(8);
        Totals(rows, true, out.money, out.tokens);
        out.moneyDelta = money0 < out.money ? out.money - money0 : 0;
        out.tokenDelta = tokens0 < out.tokens ? out.tokens - tokens0 : 0;
        if (!Known(U32(r, 4)))
            e.push_back(0xB);
        uint8_t rec[0x2A8];
        const uint32_t maxStacks = FetchSpell(U32(r, 4), rec) ? std::max<uint32_t>(1, *reinterpret_cast<uint32_t*>(rec + 0xC4)) : 0;
        if (maxStacks < CountSpell(U32(r, 4), true, slot0, spell, 0xFFFFFFFF))
            e.push_back(0xF);
        const uint32_t q = QualityOf(r);
        if (QualityCap(q) < CountQuality(q, true, slot0, spell, 0xFFFFFFFF))
        {
            switch (q)
            {
            case 2: e.push_back(0x10); break;
            case 3: e.push_back(0x11); break;
            case 4: e.push_back(0x12); break;
            case 5: e.push_back(0x13); break;
            case 6: e.push_back(0x14); break;
            default: e.push_back(0x15);
            }
        }
        if (RareWorldforged(r) && CountRareWorldforged(true, slot0, spell, 0xFFFFFFFF) > 3)
            e.push_back(0x1A);
        if (!AEMet(r))
            e.push_back(0x1B);
        if (!TEMet(r))
            e.push_back(0x1C);
        if (player && UnitLevel(player) < U32(r, 0x14))
            e.push_back(0x1D);
        return e;
    }
    // FUN_102E2750: the disenchant checks for an enchant spell (RE_DISENCHANT_*, 0 = ok).
    uint32_t CheckDisenchant(uint32_t spell)
    {
        Row r = spell ? BySpell(spell) : nullptr;
        if (!r)
            return 7;
        if (!RealmAllows(r))
            return 0xE;
        if (WildcardMode())
            return 0xF;
        if (Known(spell))
            return 8;
        if (!AltarNearby())
            return 0xC;
        const uint8_t* player = ActivePlayer();
        if (player && Casting(player))
            return 0xD;
        const uint32_t tok = ExtractCost(r);
        if (tok == 0 || tok == 0xFFFFFFFF)
            return 9;
        return player && HasItems(player, kMysticExtract, tok) ? 0 : 9;
    }
    // FUN_102E2CC0: the mystic extract purchase (RE_PURCHASE_MYSTIC_EXTRACT_*).
    uint32_t CheckExtractPurchase()
    {
        return HasTokenCount(ActivePlayer(), kMysticExtract, 1) ? 3 : 0;
    }
    // FUN_102E29F0: can `spell` be equipped (in slot0, or anywhere for -1) -- RE_EQUIP_*, 0 = ok.
    uint32_t CheckEquip(uint32_t spell, uint32_t slot0)
    {
        Row r = BySpell(spell);
        if (!r)
            return 2;
        if (slot0 != 0xFFFFFFFF && !SlotValid(slot0))
            return 0xB;
        uint8_t rec[0x2A8];
        if (!FetchSpell(U32(r, 4), rec))
            return 3;
        const uint32_t maxStacks = std::max<uint32_t>(1, *reinterpret_cast<uint32_t*>(rec + 0xC4));
        if (CountSpell(U32(r, 4), false, 0, 0, slot0) >= maxStacks)
            return 3;
        const uint8_t* player = ActivePlayer();
        if (player && !ClassAllowed(r, ClassOf(player)))
            return 10;
        if (WildcardMode())
            return 0xD;
        if (!AEMet(r))
            return 0x10;
        if (!TEMet(r))
            return 0x11;
        if (player && UnitLevel(player) < U32(r, 0x14))
            return 0x12;
        const uint32_t q = QualityOf(r);
        if (QualityCap(q) <= CountQuality(q, false, 0, 0, slot0))
        {
            switch (q)
            {
            case 2: return 4;
            case 3: return 5;
            case 4: return 6;
            case 5: return 7;
            case 6: return 8;
            default: return 9;
            }
        }
        if (RareWorldforged(r))
            return CountRareWorldforged(false, 0, 0, slot0) > 3 ? 0xE : 0;
        return 0;
    }
    // FUN_102E2FB0: the reforge gates (RE_REFORGE_*).
    void ReforgeGates(std::vector<uint32_t>& e)
    {
        if (!ReforgeAffordable())
            e.push_back(7);
        if (!AltarNearby())
            e.push_back(9);
        const uint8_t* player = ActivePlayer();
        if (player)
        {
            if (Casting(player))
                e.push_back(10);
            if (IsCoAClass(ClassOf(player)))
                e.push_back(0xB);
        }
        if (WildcardMode())
            e.push_back(0xC);
    }
    std::vector<uint32_t> CheckReforgeItem(uint32_t itemLow)   // FUN_102E30C0
    {
        std::vector<uint32_t> e;
        uint8_t* item = ItemObject(itemLow);
        if (!item)
            e.push_back(2);
        else if (EntryOf(item) != kMysticScroll)
        {
            Row r = ByItem(EntryOf(item));
            if (!r)
                e.push_back(3);
            else if (U32(r, 0x1C))
                e.push_back(4);
        }
        ReforgeGates(e);
        return e;
    }
    std::vector<uint32_t> CheckReforgeSlot(uint32_t slot0)   // FUN_102E31D0
    {
        std::vector<uint32_t> e;
        if (!SlotValid(slot0))
            e.push_back(5);
        if (DraftSpec())
            e.push_back(6);
        ReforgeGates(e);
        return e;
    }
    std::vector<uint32_t> CheckDestroy(uint32_t slot0)   // FUN_102E2570
    {
        std::vector<uint32_t> e;
        const bool valid = SlotValid(slot0);
        if (!valid)
            e.push_back(2);
        if (!(valid && slot0 < M().slots.size() && M().slots[slot0] != 0))
            e.push_back(3);
        const uint8_t* player = ActivePlayer();
        if (player)
        {
            if (Casting(player))
                e.push_back(4);
            if (IsCoAClass(ClassOf(player)))
                e.push_back(5);
        }
        if (DraftSpec())
            e.push_back(6);
        return e;
    }
    std::vector<uint32_t> CheckPurchaseScroll()   // FUN_102E2DF0
    {
        std::vector<uint32_t> e;
        const uint8_t* rec = ItemCacheRecord(kMysticScroll);
        const uint8_t* player = ActivePlayer();
        if (!rec)
            e.push_back(2);
        else
        {
            if (player && Coinage(player) < *reinterpret_cast<const uint32_t*>(rec + 0x20))
                e.push_back(3);
            if (FreeBagSlots() == 0)
                e.push_back(4);
        }
        if (!AltarNearby())
            e.push_back(5);
        if (player)
        {
            if (Casting(player))
                e.push_back(6);
            if (IsCoAClass(ClassOf(player)))
                e.push_back(7);
        }
        return e;
    }

    // ---- the enchant-info table (FUN_1009B2F0) ------------------------------------------------------
    void PushEnchantInfo(lua_State* L, Row r)
    {
        const uint32_t spell = U32(r, 4);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "SpellID");
        PushNum(L, static_cast<double>(spell));
        AscLua::lua_settable(L, -3);
        uint8_t rec[0x2A8];
        const bool hasSpell = FetchSpell(spell, rec);
        AscLua::lua_pushstring(L, "SpellName");
        if (hasSpell)
            PushStr(L, *reinterpret_cast<const char* const*>(rec + 0x220));
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "MaxStacks");
        if (hasSpell)
            PushInt(L, static_cast<int32_t>(std::max<uint32_t>(1, *reinterpret_cast<uint32_t*>(rec + 0xC4))));
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Quality");
        PushEnum(L, kReQualities, 9, QualityOf(r));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "RequiredLevel");
        PushNum(L, static_cast<double>(U32(r, 0x14)));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "ItemID");
        PushNum(L, static_cast<double>(U32(r, 0x18)));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Known");
        PushBool(L, Known(spell));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "IsWorldforged");
        PushBool(L, U32(r, 0x1C) != 0);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "IsAvailableForCurrentClass");
        const uint8_t* player = ActivePlayer();
        PushBool(L, player && ClassAllowed(r, ClassOf(player)));
        AscLua::lua_settable(L, -3);

        // ClassRequirements: the three {class, tab, AE, TE} requirements grouped by class in first-seen
        // order, skipping a "None" (1) class or tab (FUN_1009AA70 / FUN_1009DBE0 / FUN_1009DE10).
        struct TabReq { uint32_t tab, ae, te; };
        std::vector<std::pair<uint32_t, std::vector<TabReq>>> groups;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t cls = U32(r, 0x4C + i * 4), tab = U32(r, 0x58 + i * 4);
            if (cls == 1 || tab == 1)
                continue;
            const TabReq t{tab, U32(r, 0x64 + i * 4), U32(r, 0x70 + i * 4)};
            auto it = std::find_if(groups.begin(), groups.end(), [cls](const auto& g) { return g.first == cls; });
            if (it == groups.end())
                groups.emplace_back(cls, std::vector<TabReq>{t});
            else
                it->second.push_back(t);
        }
        AscLua::lua_pushstring(L, "ClassRequirements");
        AscLua::lua_createtable(L, 0, static_cast<int>(groups.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t g = 0; g < groups.size(); ++g)
        {
            PushNum(L, static_cast<double>(g + 1));
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            AscLua::lua_pushstring(L, "ClassType");
            const uint32_t cls = groups[g].first;
            PushStr(L, cls - 1 < 0x2F ? kClassTypes[cls - 1] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(cls)).c_str());
            AscLua::lua_settable(L, -3);
            AscLua::lua_pushstring(L, "TabTypes");
            AscLua::lua_createtable(L, 0, static_cast<int>(groups[g].second.size()));
            AscLua::lua_checkstack(L, 2);
            for (size_t i = 0; i < groups[g].second.size(); ++i)
            {
                const TabReq& t = groups[g].second[i];
                PushNum(L, static_cast<double>(i + 1));
                AscLua::lua_createtable(L, 0, 0);
                AscLua::lua_checkstack(L, 2);
                AscLua::lua_pushstring(L, "Tab");
                PushStr(L, t.tab - 1 < 0x5F ? kTabTypes[t.tab - 1] : ("UNEXPECTED_ENUM_VALUE_" + std::to_string(t.tab)).c_str());
                AscLua::lua_settable(L, -3);
                AscLua::lua_pushstring(L, "RequiredAE");
                PushNum(L, static_cast<double>(t.ae));
                AscLua::lua_settable(L, -3);
                AscLua::lua_pushstring(L, "RequiredTE");
                PushNum(L, static_cast<double>(t.te));
                AscLua::lua_settable(L, -3);
                AscLua::lua_settable(L, -3);
            }
            AscLua::lua_settable(L, -3);
            AscLua::lua_settable(L, -3);
        }
        AscLua::lua_settable(L, -3);

        // Spec: the four tag names mapped through the tag table; "NONE" (0) and unknown names drop out.
        std::vector<uint32_t> tags;
        for (uint32_t i = 0; i < 4; ++i)
        {
            const int t = EnumIndex(kSpecTags, 91, Str(r, 0x3C + i * 4));
            if (t > 0)
                tags.push_back(static_cast<uint32_t>(t));
        }
        AscLua::lua_pushstring(L, "Spec");
        if (tags.empty())
            AscLua::lua_pushnil(L);
        else
        {
            AscLua::lua_createtable(L, 0, static_cast<int>(tags.size()));
            AscLua::lua_checkstack(L, 2);
            for (size_t i = 0; i < tags.size(); ++i)
            {
                PushNum(L, static_cast<double>(i + 1));
                PushStr(L, kSpecTags[tags[i]]);
                AscLua::lua_settable(L, -3);
            }
        }
        AscLua::lua_settable(L, -3);
    }

    // ---- the enchant container virtuals (vtable 0x10B56504) ------------------------------------------
    // [0] FUN_102EFFD0: every row the player may see.
    std::vector<Row> EnchantContainer::DoPopulate()
    {
        std::vector<Row> v;
        AscDbc::Table& t = Dbc();
        if (!t.Loaded())
            return v;
        const uint8_t* player = ActivePlayer();
        const uint8_t cls = ClassOf(player);
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
        {
            Row r = t.Row(id);
            if (!r)
                continue;
            if (U32(r, 0x34) || U32(r, 0x38))
            {
                if (IsStockClass(cls) && (U32(r, 0x34) & 0x5FF) == 0)
                    continue;
                if (cls == 10 && (U32(r, 0x34) & 0x200) == 0)
                    continue;
            }
            uint8_t rec[0x2A8];
            if (!FetchSpell(U32(r, 4), rec))
                continue;
            if (!RealmAllows(r))
                continue;
            if (!WildcardMode())
                v.push_back(r);
        }
        return v;
    }
    // [2] FUN_102F0510
    bool EnchantContainer::ArgMatch(uint32_t f, const Row& r)
    {
        struct ClassPair { uint32_t a, b; };
        switch (f)
        {
        case 0: case 1: case 2: case 3: return true;
        case 4: return Known(U32(r, 4));
        case 5: return !Known(U32(r, 4));
        case 6: return QualityOf(r) == 2;
        case 7: return QualityOf(r) == 3;
        case 8: return QualityOf(r) == 4;
        case 9: return QualityOf(r) == 5;
        case 10: return QualityOf(r) == 6;
        case 0xB: return U32(r, 0x1C) != 0;
        case 0xC: return U32(r, 0x1C) == 0;
        case 0x16: return HasClassReq(r, 0xD, true);
        case 0x33: return HasTabReq(r, 0xD, 0x1C, false);
        default: break;
        }
        // RE_FILTER_CLASS_WARRIOR..DRUID (0x0D..0x17, HERO 0x16 above): {CA class type, its reborn type}.
        static const ClassPair kClass[11] = {{3, 0x25}, {10, 0x2C}, {2, 0x24}, {4, 0x26}, {6, 0x28}, {0xB, 0x2D},
                                             {9, 0x2B}, {5, 0x27}, {7, 0x29}, {0, 0}, {8, 0x2A}};
        if (f >= 0xD && f <= 0x17)
        {
            const ClassPair& c = kClass[f - 0xD];
            return HasClassReq(r, c.a, true) || HasClassReq(r, c.b, true);
        }
        struct Spec { uint32_t f, cls, reborn, tab; };
        static const Spec kSpecs[] = {
            {0x18, 3, 0x25, 7}, {0x19, 3, 0x25, 6}, {0x1A, 3, 0x25, 5}, {0x1B, 10, 0x2C, 7}, {0x1C, 10, 0x2C, 0xE},
            {0x1D, 10, 0x2C, 0x19}, {0x1E, 2, 0x24, 2}, {0x1F, 2, 0x24, 4}, {0x20, 2, 0x24, 3}, {0x21, 4, 0x26, 10},
            {0x22, 4, 0x26, 8}, {0x23, 4, 0x26, 9}, {0x24, 6, 0x28, 0x10}, {0x25, 6, 0x28, 0xE}, {0x26, 6, 0x28, 0xF},
            {0x27, 0xB, 0x2D, 0x1A}, {0x28, 0xB, 0x2D, 0xC}, {0x29, 0xB, 0x2D, 0x1B}, {0x2A, 9, 0x2B, 0x17},
            {0x2B, 9, 0x2B, 0x18}, {0x2C, 9, 0x2B, 0x14}, {0x2D, 5, 0x27, 0xB}, {0x2E, 5, 0x27, 0xC},
            {0x2F, 5, 0x27, 0xD}, {0x30, 7, 0x29, 0x12}, {0x31, 7, 0x29, 0x13}, {0x32, 7, 0x29, 0x11},
            {0x34, 8, 0x2A, 0x14}, {0x35, 8, 0x2A, 0x16}, {0x36, 8, 0x2A, 0x15}};
        for (const Spec& s : kSpecs)
            if (s.f == f)
                return HasTabReq(r, s.cls, s.tab, true) || HasTabReq(r, s.reborn, s.tab, true);
        return false;
    }
    // [4] FUN_102F0D50
    uint32_t EnchantContainer::ArgGroup(uint32_t f)
    {
        if (f <= 3) return 1;
        if (f <= 5) return 2;
        if (f <= 10) return 3;
        if (f <= 0xC) return 4;
        if (f <= 0x36) return 5;
        return 0;
    }
    // FUN_101BF110: ASCII case-insensitive substring; an empty needle always matches.
    bool ContainsCI(const char* hay, const std::string& needle)
    {
        if (needle.empty())
            return true;
        const size_t n = strlen(hay);
        if (needle.size() > n)
            return false;
        auto fold = [](char c) { return static_cast<uint8_t>(c + 0xBF) <= 0x19 ? static_cast<char>(c + 0x20) : c; };
        for (size_t i = 0; i + needle.size() <= n; ++i)
        {
            size_t k = 0;
            while (k < needle.size() && fold(hay[i + k]) == fold(needle[k]))
                ++k;
            if (k == needle.size())
                return true;
        }
        return false;
    }
    // [3] FUN_102F0400: the spell's name, else its description.
    bool EnchantContainer::TextMatch(const std::string&, const Row& r)
    {
        if (lastRawText.empty())
            return true;
        const uint32_t spell = U32(r, 4);
        if (ContainsCI(AscSpellText::Name(spell).c_str(), lastRawText))
            return true;
        return ContainsCI(AscSpellText::Description(spell, true, true, 0).c_str(), lastRawText);
    }
    // [8] FUN_102F0DF0: ((7 - quality) << 16 | level) << 32 | id -- best quality, then lowest level first.
    std::string EnchantContainer::FilterKey(const Row& r)
    {
        const uint8_t q = static_cast<uint8_t>(7 - static_cast<uint8_t>(QualityOf(r)));
        std::string k;
        AscFilter::AppendU32(k, (static_cast<uint32_t>(q) << 16) | U32(r, 0x14));
        AscFilter::AppendU32(k, U32(r, 0));
        return k;
    }

    // ---- mystic scrolls ------------------------------------------------------------------------------
    // FUN_102E63B0: the player's mystic scrolls (the untarnished scroll, or an item flagged 0x400000).
    std::vector<uint8_t*> ScrollItems()
    {
        std::vector<uint8_t*> v;
        for (uint8_t* item : PlayerItems())
        {
            const uint32_t entry = EntryOf(item);
            const AscItemAddon::Record* rec = AscItemAddon::Find(entry);
            if (rec && (entry == kMysticScroll || (rec->fields[1] & 0x400000)))
                v.push_back(item);
        }
        return v;
    }
    ScrollEntry MakeEntry(uint8_t* item, const std::string& name, uint32_t quality)   // FUN_102E00D0 / FUN_102DF220
    {
        ScrollEntry e;
        e.guid = GuidLow(GuidOf(item));
        e.entry = EntryOf(item);
        e.name = name;
        BagAndSlot(item, e.bag, e.slot);
        e.quality = quality;
        if (Row r = ByItem(e.entry))
        {
            e.spell = U32(r, 4);
            e.known = Known(e.spell);
        }
        return e;
    }
    // The comparators FUN_102E08C0 / FUN_102E0A00: the untarnished scroll first, then scrolls the player can
    // use (level and class), then bag, then slot.
    bool Usable(const ScrollEntry& e)
    {
        const uint8_t* player = ActivePlayer();
        Row r = ByItem(e.entry);
        return player && r && U32(r, 0x14) <= UnitLevel(player) && ClassAllowed(r, ClassOf(player));
    }
    bool ScrollLess(const ScrollEntry& a, const ScrollEntry& b)
    {
        const bool sa = a.entry == kMysticScroll, sb = b.entry == kMysticScroll;
        if (sa != sb)
            return sa;
        const bool ua = Usable(a), ub = Usable(b);
        if (ua != ub)
            return ua;
        if (a.bag != b.bag)
            return a.bag < b.bag;
        return a.slot < b.slot;
    }
    void PushScroll(lua_State* L, const ScrollEntry& e)   // FUN_1009DA50
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "Guid");
        PushInt(L, static_cast<int32_t>(e.guid));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Entry");
        PushInt(L, static_cast<int32_t>(e.entry));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Name");
        PushStr(L, e.name.c_str());
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Bag");
        PushInt(L, e.bag);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Slot");
        PushInt(L, e.slot);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Quality");
        PushInt(L, static_cast<int32_t>(e.quality));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "SpellID");
        PushInt(L, static_cast<int32_t>(e.spell));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Known");
        PushBool(L, e.known);
        AscLua::lua_settable(L, -3);
    }
    void PushScrollList(lua_State* L, const std::vector<ScrollEntry>& v)   // FUN_102DF980
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushScroll(L, v[i]);
            AscLua::lua_settable(L, -3);
        }
    }

    // FUN_102EF270: rebuild +0xF0 from the player's scrolls, the text and the RE_FILTER list.
    void FilterScrolls(std::string text, const std::vector<uint32_t>& filters)
    {
        std::vector<uint32_t> knownF, qualityF, worldforgedF, classF;
        for (uint32_t f : filters)
        {
            if (f <= 3)
                continue;
            if (f <= 5)
                knownF.push_back(f);
            else if (f <= 10)
                qualityF.push_back(f);
            else if (f <= 12)
                worldforgedF.push_back(f);
            else
                classF.push_back(f);
        }
        for (char& c : text)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        Mgr& m = M();
        m.scrolls.clear();
        bool untarnished = false;
        for (uint8_t* item : PlayerItems())
        {
            const uint32_t entry = EntryOf(item);
            const AscItemAddon::Record* rec = AscItemAddon::Find(entry);
            if (entry == kMysticScroll)
            {
                if (untarnished)
                    continue;
                untarnished = true;
            }
            else if (!((rec ? rec->fields[1] : 0) & 0x400000))
                continue;
            std::string name = rec ? rec->name : std::string();
            if (name.empty())
                name = ClientItemName(item);
            const ScrollEntry e = MakeEntry(item, name, rec ? rec->Quality() : 0);
            Row r = ByItem(e.entry);
            if (!text.empty())
            {
                std::string lower = e.name;
                for (char& c : lower)
                    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
                if (lower.find(text) == std::string::npos)
                    continue;
            }
            if (!knownF.empty())
            {
                bool ok = false;
                for (uint32_t f : knownF)
                    ok = ok || (r && ((f == 4 && e.known) || (f == 5 && !e.known)));
                if (!ok)
                    continue;
            }
            if (!qualityF.empty())
            {
                bool ok = false;
                for (uint32_t f : qualityF)
                    ok = ok || e.quality == f - 4;
                if (!ok)
                    continue;
            }
            if (!worldforgedF.empty())
            {
                bool ok = false;
                for (uint32_t f : worldforgedF)
                    ok = ok || (r && ((f == 0xB && U32(r, 0x1C) != 0) || (f == 0xC && U32(r, 0x1C) == 0)));
                if (!ok)
                    continue;
            }
            if (!classF.empty())
            {
                bool ok = false;
                for (uint32_t f : classF)
                    ok = ok || (r && m.container.MatchArg(f, r));
                if (!ok)
                    continue;
            }
            m.scrolls.push_back(e);
        }
        std::sort(m.scrolls.begin(), m.scrolls.end(), ScrollLess);   // FUN_102DEE90
    }
}

namespace
{
    // ---- Lua argument readers ----------------------------------------------------------------------
    // FUN_100BED80 + FUN_100C1370: a table of {name = boolean}; the names whose value is true.
    std::vector<std::string> TrueKeys(lua_State* L, int idx)
    {
        std::vector<std::string> out;
        if (AscLua::lua_type(L, idx) != AscScript::TABLE)
            return out;
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, idx))
        {
            if (AscLua::lua_type(L, -2) == AscScript::STRING && AscLua::lua_toboolean(L, -1))
                out.emplace_back(AscLua::lua_tolstring(L, -2, nullptr));
            AscLua::lua_settop(L, -2);
        }
        return out;
    }
    std::vector<uint32_t> FilterIndexes(const std::vector<std::string>& names)   // FUN_102DF6D0
    {
        std::vector<uint32_t> v;
        for (const std::string& n : names)
        {
            const int i = EnumIndex(kFilters, 55, n);
            if (i >= 0)
                v.push_back(static_cast<uint32_t>(i));
        }
        return v;
    }

    // ---- bindings ----------------------------------------------------------------------------------
    int ReforgeItem(lua_State* L)   // 0x102EE7D0
    {
        uint32_t itemLow;
        if (!ReadNumber(L, itemLow))
            return 0;
        uint8_t* item = ItemObject(itemLow);
        if (!item)
        {
            PushBool(L, false);
            return 1;
        }
        uint8_t bag, slot;
        WireBagSlot(item, bag, slot);
        Packet(0x603).U8(bag).U8(slot).Send();
        PushBool(L, true);
        return 1;
    }
    int CanReforgeItem(lua_State* L)   // 0x102EBD30
    {
        uint32_t itemLow;
        if (!ReadNumber(L, itemLow))
            return 0;
        const std::vector<uint32_t> e = CheckReforgeItem(itemLow);
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kReforge, 13, e[0]);
        return 2;
    }
    int ReforgeSlot(lua_State* L)   // 0x102EE960
    {
        uint32_t slot;
        if (!ReadNumber(L, slot) || slot == 0)
            return 0;
        Packet(0x60E).U32(slot - 1).Send();
        PushBool(L, true);
        return 1;
    }
    int CanReforgeSlot(lua_State* L)   // 0x102EBE30
    {
        uint32_t slot;
        if (!ReadNumber(L, slot) || slot == 0)
            return 0;
        const std::vector<uint32_t> e = CheckReforgeSlot(slot - 1);
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kReforge, 13, e[0]);
        return 2;
    }
    int GetReforgeCost(lua_State* L)   // 0x102EDA70
    {
        PushNum(L, static_cast<double>((ReforgeCost(false))));
        PushNum(L, static_cast<double>((ReforgeCost(true))));
        return 2;
    }
    int CollectionReforgeItem(lua_State* L)   // 0x102EC720
    {
        int32_t itemLow, spell;
        if (!ReadInt2(L, itemLow, spell))
            return 0;
        uint8_t* item = ItemObject(static_cast<uint32_t>(itemLow));
        if (!item)
        {
            PushBool(L, false);
            return 1;
        }
        uint8_t bag, slot;
        WireBagSlot(item, bag, slot);
        Packet(0x605).U8(bag).U8(slot).U32(static_cast<uint32_t>(spell)).Send();
        PushBool(L, true);
        return 1;
    }
    void PushReforgeResult(lua_State* L, const std::vector<uint32_t>& e, const ReforgeCosts& c)
    {
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kCollReforge, 30, e[0]);
        if (!e.empty() && e[0] == 0xC)
        {
            PushInt(L, static_cast<int32_t>(c.moneyDelta));
            PushInt(L, static_cast<int32_t>(c.tokenDelta));
            PushInt(L, static_cast<int32_t>(c.money));
            PushInt(L, static_cast<int32_t>(c.tokens));
        }
        else
            PushNils(L, 4);
    }
    int CanCollectionReforgeItem(lua_State* L)   // 0x102EAD40
    {
        int32_t itemLow, spell;
        if (!ReadInt2(L, itemLow, spell))
            return 0;
        ReforgeCosts c;
        const std::vector<uint32_t> e = CheckCollectionReforgeItem(static_cast<uint32_t>(itemLow), static_cast<uint32_t>(spell), c);
        PushReforgeResult(L, e, c);
        return 6;
    }
    void UpsertPending(std::vector<Pair>& v, uint32_t slot, uint32_t value)
    {
        for (Pair& p : v)
            if (p.slot == slot)
            {
                p.value = value;
                return;
            }
        v.push_back({slot, value});
    }
    int CollectionReforgeSlot(lua_State* L)   // 0x102EC8C0
    {
        int32_t slot, spell;
        if (ReadInt2(L, slot, spell) && slot != 0)
            UpsertPending(M().pendingReforge, static_cast<uint32_t>(slot), static_cast<uint32_t>(spell));
        return 0;
    }
    int CanCollectionReforgeSlot(lua_State* L)   // 0x102EAE70
    {
        int32_t slot, spell;
        if (!ReadInt2(L, slot, spell) || slot == 0)
            return 0;
        ReforgeCosts c;
        const std::vector<uint32_t> e = CheckCollectionReforgeSlot(static_cast<uint32_t>(slot) - 1, static_cast<uint32_t>(spell), false, c);
        PushReforgeResult(L, e, c);
        return 6;
    }
    // CanCollectionReforgeAnySlot / CanApplyAnySlot: the first slot with no error wins; otherwise
    // false and {[slot] = {error names}} in the map's own iteration order.
    template <class F>
    int AnySlot(lua_State* L, F check, const char* const* table, uint32_t count)
    {
        std::unordered_map<uint32_t, std::vector<uint32_t>> errors;
        for (uint32_t slot0 = 0;; ++slot0)
        {
            errors[slot0 + 1] = check(slot0);
            if (errors[slot0 + 1].empty())
            {
                PushBool(L, true);
                AscLua::lua_pushnil(L);
                return 2;
            }
            if (slot0 + 1 > 0x10)
                break;
        }
        PushBool(L, false);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (const auto& kv : errors)
        {
            PushInt(L, static_cast<int32_t>(kv.first));
            PushEnumList(L, table, count, kv.second);
            AscLua::lua_settable(L, -3);
        }
        return 2;
    }
    int CanCollectionReforgeAnySlot(lua_State* L)   // 0x102EAAC0
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return 0;
        return AnySlot(L, [spell](uint32_t slot0) { ReforgeCosts c; return CheckCollectionReforgeSlot(slot0, spell, false, c); },
                       kCollReforge, 30);
    }
    void PushSlotSpell(lua_State* L, uint32_t slot, uint32_t spell)   // FUN_1009E100
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "Slot");
        PushNum(L, static_cast<double>((slot)));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Enchant");
        if (Row r = BySpell(spell))
            PushEnchantInfo(L, r);
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
    }
    void PushSlotItem(lua_State* L, uint32_t slot, uint32_t itemLow)   // FUN_1009E010
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "RESlot");
        PushNum(L, static_cast<double>((slot)));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Enchant");
        Row r = nullptr;
        if (uint8_t* item = ItemObject(itemLow))
            r = ByItem(EntryOf(item));
        if (r)
            PushEnchantInfo(L, r);
        else
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);
    }
    int GetCollectionReforgeChanges(lua_State* L)   // 0x102ED1A0
    {
        const auto& v = M().pendingReforge;
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushSlotSpell(L, v[i].slot, v[i].value);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }
    // FUN_102EFEB0 / FUN_102EFE60: drop the pending entry for `slot`, or the last one for -1.
    void UndoPending(std::vector<Pair>& v, uint32_t slot)
    {
        if (slot == 0xFFFFFFFF)
        {
            if (!v.empty())
                v.pop_back();
            return;
        }
        for (auto it = v.begin(); it != v.end(); ++it)
            if (it->slot == slot)
            {
                v.erase(it);
                return;
            }
    }
    int UndoCollectionReforge(lua_State* L)   // 0x102EF200
    {
        uint32_t slot;
        if (ReadNumber(L, slot))
            UndoPending(M().pendingReforge, slot);
        return 0;
    }
    int UndoLastCollectionReforge(lua_State*)   // 0x102EF250
    {
        if (!M().pendingReforge.empty())
            M().pendingReforge.pop_back();
        return 0;
    }
    int SaveCollectionReforge(lua_State* L)   // 0x102EEC50
    {
        const auto& v = M().pendingReforge;
        if (v.empty())
        {
            PushBool(L, false);
            return 1;
        }
        Packet p(0x60F);
        p.U32(static_cast<uint32_t>(v.size()));
        for (const Pair& e : v)
            p.U32(e.slot - 1).U32(e.value);
        p.Send();
        PushBool(L, true);
        return 0;   // the original pushes true but returns 0
    }
    // The save checks collect every entry's errors into an unordered_set and report the head of its
    // list -- MSVC's insertion keeps the first error ever inserted at the head.
    int CanSaveCollectionReforge(lua_State* L)   // 0x102EC500
    {
        std::vector<uint32_t> all;
        for (const Pair& p : M().pendingReforge)
        {
            ReforgeCosts c;
            for (uint32_t e : CheckCollectionReforgeSlot(p.slot - 1, p.value, true, c))
                if (std::find(all.begin(), all.end(), e) == all.end())
                    all.push_back(e);
        }
        PushBool(L, all.empty());
        if (all.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kCollReforge, 30, all[0]);
        return 2;
    }
    int GetCollectionReforgeSlotCost(lua_State* L)   // 0x102ED330
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return 0;
        Row r = BySpell(spell);
        if (!r)
            return PushNils(L, 2);
        PushNum(L, static_cast<double>((CollectionReforgeCost(r, false, true))));
        PushNum(L, static_cast<double>((CollectionReforgeCost(r, true, true))));
        return 2;
    }
    int GetCollectionReforgeItemCost(lua_State* L)   // 0x102ED260
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return 0;
        Row r = BySpell(spell);
        if (!r)
            return PushNils(L, 2);
        PushNum(L, static_cast<double>((CollectionReforgeCost(r, false, false))));
        PushNum(L, static_cast<double>((CollectionReforgeCost(r, true, false))));
        return 2;
    }
    int GetSaveCollectionReforgeSlotCost(lua_State* L)   // 0x102EDAF0
    {
        uint32_t money, tokens;
        Totals(PendingReforgeRows(), true, money, tokens);
        PushNum(L, static_cast<double>((money)));
        PushNum(L, static_cast<double>((tokens)));
        return 2;
    }

    // FUN_100E92D0: {number, boolean}.
    bool ReadNumberBool(lua_State* L, uint32_t& n, bool& b)
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return false;
        n = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        b = AscLua::lua_toboolean(L, 2) != 0;
        return true;
    }
    int DisenchantItem(lua_State* L)   // 0x102ECA60
    {
        uint32_t itemLow;
        bool extract;
        if (!ReadNumberBool(L, itemLow, extract))
            return 0;
        uint8_t* item = ItemObject(itemLow);
        if (!item)
        {
            PushBool(L, false);
            return 1;
        }
        uint8_t bag, slot;
        WireBagSlot(item, bag, slot);
        Packet(0x607).U8(extract ? 1 : 0).U8(bag).U8(slot).Send();
        PushBool(L, true);
        return 1;
    }
    // The extract pre-check of CanDisenchantItem / CanDisenchantSlot: (false, reason, cost) when the
    // extract purchase itself is refused.
    bool ExtractRefused(lua_State* L)
    {
        const uint32_t r = CheckExtractPurchase();
        if (!r)
            return false;
        PushBool(L, false);
        PushEnum(L, kExtract, 4, r);
        PushInt(L, static_cast<int32_t>(ExtractPurchaseCost()));
        return true;
    }
    int CanDisenchantItem(lua_State* L)   // 0x102EB1A0
    {
        uint32_t itemLow;
        bool extract;
        if (!ReadNumberBool(L, itemLow, extract))
            return 0;
        if (extract && ExtractRefused(L))
            return 3;
        uint32_t err;
        if (uint8_t* item = ItemObject(itemLow))
        {
            Row r = ByItem(EntryOf(item));
            err = r ? CheckDisenchant(U32(r, 4)) : 3;
        }
        else
            err = 2;
        if (err == 0 || (extract && err == 9))
        {
            PushBool(L, true);
            AscLua::lua_pushnil(L);
            return 2;
        }
        PushBool(L, false);
        PushEnum(L, kDisenchant, 16, err);
        return 2;
    }
    int DisenchantSlot(lua_State* L)   // 0x102ECC00
    {
        uint32_t slot;
        bool extract;
        if (!ReadNumberBool(L, slot, extract) || slot == 0)
            return 0;
        Packet(0x608).U8(extract ? 1 : 0).U32(slot - 1).Send();
        PushBool(L, true);
        return 1;
    }
    int CanDisenchantSlot(lua_State* L)   // 0x102EB340
    {
        uint32_t slot;
        bool extract;
        if (!ReadNumberBool(L, slot, extract) || slot == 0)
            return 0;
        if (extract && ExtractRefused(L))
            return 3;
        const uint32_t slot0 = slot - 1;
        uint32_t err;
        if (!SlotValid(slot0))
            err = 4;
        else if (DraftSpec())
            err = 5;
        else
            err = CheckDisenchant(slot0 < M().slots.size() && SlotValid(slot0) ? M().slots[slot0] : 0);
        if (err == 0 || (extract && err == 9))
        {
            PushBool(L, true);
            AscLua::lua_pushnil(L);
            return 2;
        }
        PushBool(L, false);
        PushEnum(L, kDisenchant, 16, err);
        return 2;
    }
    int GetDisenchantCost(lua_State* L)   // 0x102ED400
    {
        uint32_t spell;
        bool extract;
        if (!ReadNumberBool(L, spell, extract))
            return 0;
        Row r = BySpell(spell);
        if (!r)
            return PushNils(L, 2);
        PushNum(L, static_cast<double>((ExtractCost(r))));
        if (extract)
            PushNum(L, static_cast<double>((ExtractPurchaseCost())));
        else
            AscLua::lua_pushnil(L);
        return 2;
    }

    int ApplyItem(lua_State* L)   // 0x102EA420
    {
        int32_t slot, itemLow;
        if (!ReadInt2(L, slot, itemLow) || slot == 0)
            return 0;
        uint8_t* item = ItemObject(static_cast<uint32_t>(itemLow));
        if (!item)
        {
            PushBool(L, false);
            return 1;
        }
        uint8_t bag, bslot;
        WireBagSlot(item, bag, bslot);
        Packet(0x60A).U32(static_cast<uint32_t>(slot) - 1).U8(bag).U8(bslot).Send();
        PushBool(L, true);
        return 1;
    }
    int CanApplyItem(lua_State* L)   // 0x102EA8E0
    {
        int32_t slot, itemLow;
        if (!ReadInt2(L, slot, itemLow) || slot == 0)
            return 0;
        const std::vector<uint32_t> e = CheckApplyItem(static_cast<uint32_t>(slot) - 1, static_cast<uint32_t>(itemLow));
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kApply, 26, e[0]);
        return 2;
    }
    int ApplySlot(lua_State* L)   // 0x102EA5B0
    {
        int32_t slot, itemLow;
        if (ReadInt2(L, slot, itemLow) && slot != 0 && ItemObject(static_cast<uint32_t>(itemLow)))
            UpsertPending(M().pendingApply, static_cast<uint32_t>(slot), static_cast<uint32_t>(itemLow));
        return 0;
    }
    int CanApplySlot(lua_State* L)   // 0x102EA9D0
    {
        int32_t slot, itemLow;
        if (!ReadInt2(L, slot, itemLow) || slot == 0)
            return 0;
        const std::vector<uint32_t> e = CheckApplySlot(static_cast<uint32_t>(slot) - 1, static_cast<uint32_t>(itemLow));
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kApply, 26, e[0]);
        return 2;
    }
    int CanApplyAnySlot(lua_State* L)   // 0x102EA670
    {
        uint32_t itemLow;
        if (!ReadNumber(L, itemLow))
            return 0;
        return AnySlot(L, [itemLow](uint32_t slot0) { return CheckApplySlot(slot0, itemLow); }, kApply, 26);
    }
    int GetApplyChanges(lua_State* L)   // 0x102ECFB0
    {
        const auto& v = M().pendingApply;
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushSlotItem(L, v[i].slot, v[i].value);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }
    int UndoApply(lua_State* L)   // 0x102EF1D0
    {
        uint32_t slot;
        if (ReadNumber(L, slot))
            UndoPending(M().pendingApply, slot);
        return 0;
    }
    int UndoLastApply(lua_State*)   // 0x102EF230
    {
        if (!M().pendingApply.empty())
            M().pendingApply.pop_back();
        return 0;
    }
    int SaveApply(lua_State* L)   // 0x102EEA70
    {
        const auto& v = M().pendingApply;
        if (v.empty())
        {
            PushBool(L, false);
            return 1;
        }
        Packet p(0x610);
        p.U32(static_cast<uint32_t>(v.size()));
        bool ok = true;
        for (const Pair& e : v)
        {
            uint8_t* item = ItemObject(e.value);
            if (!item)
            {
                ok = false;
                break;
            }
            uint8_t bag, slot;
            WireBagSlot(item, bag, slot);
            p.U32(e.slot - 1).U8(bag).U8(slot);
        }
        if (ok)
            p.Send();
        PushBool(L, ok);
        return 1;
    }
    int CanSaveApply(lua_State* L)   // 0x102EBF40
    {
        std::vector<uint32_t> all;
        auto add = [&all](uint32_t e) {
            if (std::find(all.begin(), all.end(), e) == all.end())
                all.push_back(e);
        };
        if (!Fusion())
        {
            for (const Pair& p : M().pendingApply)
                for (uint32_t e : CheckApplySlot(p.slot - 1, p.value))
                    add(e);
        }
        else
        {
            // Fusion: a slot applied twice is RE_APPLY_DUPLICATE_SLOT, then every entry's errors in order.
            std::vector<uint32_t> seen;
            for (const Pair& p : M().pendingApply)
            {
                if (std::find(seen.begin(), seen.end(), p.slot) != seen.end())
                    add(0x18);
                seen.push_back(p.slot);
                for (uint32_t e : CheckApplySlot(p.slot - 1, p.value))
                    add(e);
            }
        }
        PushBool(L, all.empty());
        if (all.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kApply, 26, all[0]);
        return 2;
    }
    int GetApplyItemCost(lua_State* L)   // 0x102ED070
    {
        uint32_t slot;
        if (!ReadNumber(L, slot))
            return 0;
        if (SlotValid(slot) && slot < M().slots.size() && M().slots[slot] != 0)
            if (Row r = BySpell(M().slots[slot]))
            {
                PushNum(L, static_cast<double>((CollectionReforgeCost(r, false, false))));
                PushNum(L, static_cast<double>((CollectionReforgeCost(r, true, false))));
                return 2;
            }
        return PushNils(L, 2);
    }

    int PurchaseMysticScroll(lua_State* L)   // 0x102EE290
    {
        Packet(0x611).Send();
        PushBool(L, true);
        return 1;
    }
    int CanPurchaseMysticScroll(lua_State* L)   // 0x102EBB90
    {
        const std::vector<uint32_t> e = CheckPurchaseScroll();
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kPurchase, 8, e[0]);
        return 2;
    }
    int GetMysticScrolls(lua_State* L)   // 0x102ED7B0
    {
        std::vector<ScrollEntry> v;
        for (uint8_t* item : ScrollItems())
        {
            const uint32_t entry = EntryOf(item);
            if (entry == kMysticScroll &&
                std::any_of(v.begin(), v.end(), [](const ScrollEntry& e) { return e.entry == kMysticScroll; }))
                continue;
            const AscItemAddon::Record* rec = AscItemAddon::Find(entry);
            v.push_back(MakeEntry(item, ClientItemName(item), rec ? rec->Quality() : 0));
        }
        std::sort(v.begin(), v.end(), ScrollLess);   // FUN_102DEB40
        PushScrollList(L, v);
        return 1;
    }
    int SetMysticScrollFilter(lua_State* L)   // 0x102EEDB0
    {
        if (!ValidateInput(L, {STRING, TABLE}))
            return 0;
        const std::string text = CheckString(L, 1);
        FilterScrolls(text, FilterIndexes(TrueKeys(L, 2)));
        return 0;
    }
    int GetNumFilteredMysticScrolls(lua_State* L)   // 0x102ED930
    {
        if (AscLua::lua_gettop(L) != 0)
            return 0;
        PushInt(L, static_cast<int32_t>(M().scrolls.size()));
        return 1;
    }
    int GetFilteredMysticScrollAtIndex(lua_State* L)   // 0x102ED5C0
    {
        uint32_t i;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        if (i - 1 < M().scrolls.size())
            PushScroll(L, M().scrolls[i - 1]);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }
    int GetFilteredMysticScrolls(lua_State* L)   // 0x102ED650 -> FUN_102E4E80
    {
        const int top = AscLua::lua_gettop(L);
        if (top > 2)
            return 0;
        uint32_t start = 0, count = 0x12;
        if (top >= 1 && AscLua::lua_type(L, 1) > 0)
            start = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        if (top > 1 && AscLua::lua_type(L, 2) > 0)
            count = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        std::vector<ScrollEntry> v;
        const auto& s = M().scrolls;
        if (start < s.size() && count != 0)
        {
            const uint32_t end = std::min<uint32_t>(start + count, static_cast<uint32_t>(s.size()));
            v.assign(s.begin() + start, s.begin() + end);
        }
        PushScrollList(L, v);
        return 1;
    }
    int GetMysticScrollCost(lua_State* L)   // 0x102ED760
    {
        if (const uint8_t* rec = ItemCacheRecord(kMysticScroll))
            PushInt(L, *reinterpret_cast<const int32_t*>(rec + 0x20));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int CanEquipItem(lua_State* L)   // 0x102EB590
    {
        uint32_t itemLow;
        if (!ReadNumber(L, itemLow))
            return 0;
        uint32_t spell = 0;
        if (uint8_t* item = ItemObject(itemLow))
            if (Row r = ByItem(EntryOf(item)))
                spell = U32(r, 4);
        const uint32_t e = CheckEquip(spell, 0xFFFFFFFF);
        PushBool(L, e == 0);
        if (e == 0)
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kEquip, 19, e);
        return 2;
    }
    int CanEquipSlot(lua_State* L)   // 0x102EB650
    {
        uint32_t slot;
        if (!ReadNumber(L, slot) || slot == 0)
            return 0;
        const uint32_t e = CheckEquip(SlotAt(slot - 1), slot - 1);
        PushBool(L, e == 0);
        if (e == 0)
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kEquip, 19, e);
        return 2;
    }
    int CanEquipEnchant(lua_State* L)   // 0x102EB520
    {
        uint32_t spell;
        if (!ReadNumber(L, spell))
            return 0;
        const uint32_t e = CheckEquip(spell, 0xFFFFFFFF);
        PushBool(L, e == 0);
        if (e == 0)
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kEquip, 19, e);
        return 2;
    }

    // QueryEnchants(pageSize, page, text, {RE_FILTER_x = true}) -> page of enchant infos, page count.
    int QueryEnchants(lua_State* L)   // 0x102EE360
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, STRING, TABLE}))
            return 0;
        const uint32_t pageSize = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint32_t page = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        const std::string text = CheckString(L, 3);
        const std::vector<uint32_t> filters = FilterIndexes(TrueKeys(L, 4));
        if (*reinterpret_cast<const uint8_t*>(0xBD0792) == 0)
        {
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            return 1;
        }
        EnchantContainer& c = M().container;
        std::string parsed = text;
        AscFilter::ParseTokens(parsed);
        c.lastRawText = parsed;
        c.ApplyFilter(text, filters, {});
        const std::vector<Row>& all = c.Filtered();
        std::vector<Row> pageRows;
        const uint32_t start = (page - 1) * pageSize;
        for (uint32_t i = start; i < start + pageSize; ++i)
            if (i < all.size() && all[i])
                pageRows.push_back(all[i]);
        const uint32_t pages = static_cast<uint32_t>(ToInt(std::ceil(static_cast<double>(static_cast<uint32_t>(all.size())) /
                                                                     static_cast<double>(pageSize))));
        AscLua::lua_createtable(L, 0, static_cast<int>(pageRows.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < pageRows.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushEnchantInfo(L, pageRows[i]);
            AscLua::lua_settable(L, -3);
        }
        PushNum(L, static_cast<double>(pages));
        return 2;
    }
    int GetEnchantInfoBySpell(lua_State* L)   // 0x102ED560
    {
        uint32_t spell;
        if (!ReadNumber(L, spell) || spell == 0)
            return 0;
        if (Row r = BySpell(spell))
            PushEnchantInfo(L, r);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }
    int GetEnchantInfoByItem(lua_State* L)   // 0x102ED500
    {
        uint32_t item;
        if (!ReadNumber(L, item) || item == 0)
            return 0;
        if (Row r = ByItem(item))
            PushEnchantInfo(L, r);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }
    int GetProgress(lua_State* L)   // 0x102ED980
    {
        PushNum(L, ProgressPercent(M().progress, M().level));
        PushNum(L, static_cast<double>((M().level)));
        return 2;
    }

    // The unit readers of GetAppliedEnchant / CanInspect / Inspect: a player guid only.
    bool PlayerGuidOf(const char* token, uint64_t& guid)
    {
        if (!UnitTokenGuid(token, guid) || guid == 0)
            return false;
        const uint32_t hi = static_cast<uint32_t>(guid >> 32);
        return !(hi >> 28 != 0 && hi >> 16 != 0);
    }
    int GetAppliedEnchant(lua_State* L)   // 0x102ECD30
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string token = CheckString(L, 1);
        const uint32_t slot = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        if (slot == 0)
            return 0;
        uint64_t guid;
        if (!PlayerGuidOf(token.c_str(), guid))
            return 0;
        const uint32_t slot0 = slot - 1;
        uint32_t spell = 0;
        if (ObjectPtr(guid, 0x10) == ActivePlayer())
            spell = SlotAt(slot0);
        else
        {
            auto it = M().inspect.find(GuidLow(guid));
            if (it != M().inspect.end() && slot0 < it->second.applied.size())
                spell = it->second.applied[slot0];
        }
        PushInt(L, static_cast<int32_t>(spell));
        return 1;
    }
    std::vector<uint32_t> CheckInspect(const uint8_t* unit)   // FUN_102E2C20
    {
        if (!unit)
            return {2};
        if (IsCoAClass(ClassOf(unit)))
            return {4};
        return {};
    }
    int CanInspect(lua_State* L)   // 0x102EB710
    {
        if (!ValidateInput(L, {STRING}))
            return 0;
        uint64_t guid;
        if (!PlayerGuidOf(CheckString(L, 1), guid))
            return 0;
        const std::vector<uint32_t> e = CheckInspect(static_cast<uint8_t*>(ObjectPtr(guid, 0x10)));
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kInspect, 5, e[0]);
        return 2;
    }
    void PushInspect(lua_State* L, const InspectData& d)   // FUN_1009BBA0
    {
        auto list = [L](const std::vector<uint32_t>& v) {   // FUN_1009A460
            AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
            AscLua::lua_checkstack(L, 2);
            for (size_t i = 0; i < v.size(); ++i)
            {
                PushNum(L, static_cast<double>(i + 1));
                PushInt(L, static_cast<int32_t>(v[i]));
                AscLua::lua_settable(L, -3);
            }
        };
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        AscLua::lua_pushstring(L, "Progress");
        PushNum(L, InspectProgress(d));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "Level");
        PushInt(L, static_cast<int32_t>(d.level));
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "AppliedEnchants");
        list(d.applied);
        AscLua::lua_settable(L, -3);
        AscLua::lua_pushstring(L, "KnownEnchants");
        list(d.known);
        AscLua::lua_settable(L, -3);
    }
    int Inspect(lua_State* L)   // 0x102EDF00
    {
        if (!ValidateInput(L, {STRING, BOOLEAN}))
            return 0;
        const std::string token = CheckString(L, 1);
        const bool refresh = AscLua::lua_toboolean(L, 2) != 0;
        uint64_t guid;
        if (UnitTokenGuid(token.c_str(), guid) && guid != 0)
        {
            if (!refresh)
            {
                auto it = M().inspect.find(GuidLow(guid));
                if (it != M().inspect.end())
                {
                    PushBool(L, true);
                    PushInspect(L, it->second);
                    return 2;
                }
            }
            const uint32_t hi = static_cast<uint32_t>(guid >> 32);
            if (hi < 0x10000000 || (hi >> 16) == 0)   // FUN_102EA1C0
            {
                Packet(0x613).U64(guid).Send();
                M().inspectKey = GuidLow(guid);
                PushBool(L, true);
                AscLua::lua_pushnil(L);
                return 1;
            }
        }
        PushBool(L, false);
        AscLua::lua_pushnil(L);
        return 1;
    }
    int HasAnyScroll(lua_State* L)   // 0x102EDDF0
    {
        PushBool(L, !ScrollItems().empty());
        return 1;
    }
    int HasAnyCollected(lua_State* L)   // 0x102EDDC0
    {
        PushBool(L, !M().known.empty());
        return 1;
    }
    int HasAnySlotEnchanted(lua_State* L)   // 0x102EDE60
    {
        uint32_t n = 0;
        for (uint32_t i = 0; i < 0x11; ++i)
            n += SlotAt(i) != 0;
        PushBool(L, n != 0);
        return 1;
    }
    int HasNearbyMysticAltar(lua_State* L)   // 0x102EDED0
    {
        PushBool(L, AltarNearby());
        return 1;
    }
    int Destroy(lua_State* L)   // 0x102EC950
    {
        uint32_t slot;
        if (!ReadNumber(L, slot) || slot == 0)
            return 0;
        Packet(0x617).U32(slot - 1).Send();
        PushBool(L, true);
        return 1;
    }
    int CanDestroy(lua_State* L)   // 0x102EAFD0
    {
        uint32_t slot;
        if (!ReadNumber(L, slot) || slot == 0)
            return 0;
        const std::vector<uint32_t> e = CheckDestroy(slot - 1);
        PushBool(L, e.empty());
        if (e.empty())
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kDestroy, 7, e[0]);
        return 2;
    }
    int PurchaseMysticExtract(lua_State* L)   // 0x102EE1C0
    {
        Packet(0x733).Send();
        PushBool(L, true);
        return 1;
    }
    int CanPurchaseMysticExtract(lua_State* L)   // 0x102EBA70
    {
        const uint32_t e = CheckExtractPurchase();
        PushBool(L, e == 0);
        if (e == 0)
            AscLua::lua_pushnil(L);
        else
            PushEnum(L, kExtract, 4, e);
        PushInt(L, static_cast<int32_t>(ExtractPurchaseCost()));
        return 3;
    }

    // ---- packets -----------------------------------------------------------------------------------
    uint32_t ReadU32(CDataStore* p) { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; }
    uint64_t ReadU64(CDataStore* p) { uint64_t v; memcpy(&v, p->m_buffer + p->m_read, 8); p->m_read += 8; return v; }
    uint8_t ReadU8(CDataStore* p) { const uint8_t v = p->m_buffer[p->m_read]; p->m_read += 1; return v; }
    std::string ReadCStr(CDataStore* p)
    {
        const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        const std::string v(s);
        p->m_read += static_cast<int32_t>(v.size() + 1);
        return v;
    }

    void __cdecl OnUpdateKnown(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5F9
    {
        const uint32_t n = ReadU32(p);
        M().known.assign(n, 0);
        for (uint32_t i = 0; i < n; ++i)
            M().known[i] = ReadU32(p);
    }
    void __cdecl OnAddKnown(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5FA
    {
        const uint32_t id = ReadU32(p);
        const uint8_t silent = ReadU8(p);
        M().known.push_back(id);
        if (!silent)
            AscRuntime::Signal("MYSTIC_ENCHANT_LEARNED", "%u", id);
    }
    void __cdecl OnRemoveKnown(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5FB
    {
        const uint32_t id = ReadU32(p);
        auto& k = M().known;
        k.erase(std::remove(k.begin(), k.end(), id), k.end());
        AscRuntime::Signal("MYSTIC_ENCHANT_UNLEARNED", "%u", id);
    }
    void __cdecl OnUpdateData(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5FC
    {
        M().progress = ReadU64(p);
        M().level = ReadU32(p);
    }
    void __cdecl OnUpdateSlots(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5FD
    {
        const uint32_t n = ReadU32(p);
        M().slots.assign(n, 0);
        M().slotsAux.assign(n, 0);
        for (uint32_t i = 0; i < n; ++i)
        {
            M().slots[i] = ReadU32(p);
            M().slotsAux[i] = 0;
        }
    }
    void __cdecl OnUpdateSlot(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5FE
    {
        const uint32_t slot = ReadU32(p);
        if (slot >= M().slots.size())
            return;
        const uint32_t before = M().slots[slot];
        M().slots[slot] = ReadU32(p);
        if (before != M().slots[slot])
            AscRuntime::Signal("MYSTIC_ENCHANT_SLOT_UPDATE", "%u", slot + 1);
    }
    // FUN_100CF140: the progress multipliers of auras 0x14B60 / 0x14B61 ((points + 1) / 100 + 1 each).
    void ScaleProgress(uint64_t& v)
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return;
        for (uint32_t aura : {0x14B60u, 0x14B61u})
        {
            uint8_t rec[0x2A8];
            if (!UnitHasAuraSpell(player, aura) || !FetchSpell(aura, rec))
                continue;
            const float mult = static_cast<float>(*reinterpret_cast<const int32_t*>(rec + 0x144) + 1) / 100.0f + 1.0f;
            v = static_cast<uint64_t>(static_cast<float>(static_cast<int64_t>(v)) * mult);
        }
    }
    void __cdecl OnReforgeResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x604
    {
        const std::string result = ReadCStr(p);
        const uint32_t spell = ReadU32(p);
        if (Row r = BySpell(spell))
        {
            uint64_t gain;
            switch (QualityIndex(Str(r, 0xC)))
            {
            case 2: gain = 0x3C; break;
            case 3: gain = 0x50; break;
            case 4: gain = 100; break;
            case 5: case 6: gain = 200; break;
            default: gain = 0;
            }
            ScaleProgress(gain);
            if (gain)
            {
                Mgr& m = M();
                m.progress += gain;
                uint32_t level = m.level;
                if (LevelProgress(level) <= m.progress)
                {
                    do
                        ++level;
                    while (LevelProgress(level) <= m.progress);
                    m.level = level;
                }
            }
        }
        AscRuntime::Signal("MYSTIC_ENCHANT_REFORGE_RESULT", "%s%u", result.c_str(), spell);
    }
    // 0x60B / 0x606: an RE_APPLY_* / RE_COLLECTION_REFORGE_* result; OK clears the pending list.
    void OnResult(CDataStore* p, const char* const* table, uint32_t count, std::vector<Pair>& pending, const char* ev)
    {
        const std::string result = ReadCStr(p);
        const int i = EnumIndex(table, count, result);
        if (i < 0)
            AscLog::Printf("Unexpected Value: %s", result.c_str());
        else if (i == 0)
            pending.clear();
        AscRuntime::Signal(ev, "%s", result.c_str());
    }
    void __cdecl OnApplyResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x60B
    {
        OnResult(p, kApply, 26, M().pendingApply, "MYSTIC_ENCHANT_APPLY_RESULT");
    }
    void __cdecl OnCollectionReforgeResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x606
    {
        OnResult(p, kCollReforge, 30, M().pendingReforge, "MYSTIC_ENCHANT_COLLECTION_REFORGE_RESULT");
    }
    void StringEvent(CDataStore* p, const char* ev)
    {
        const std::string s = ReadCStr(p);
        AscRuntime::Signal(ev, "%s", s.c_str());
    }
    void __cdecl OnDestroyResult(void*, uint32_t, uint32_t, CDataStore* p) { StringEvent(p, "MYSTIC_ENCHANT_DESTROY_RESULT"); }        // 0x618
    void __cdecl OnDisenchantResult(void*, uint32_t, uint32_t, CDataStore* p) { StringEvent(p, "MYSTIC_ENCHANT_DISENCHANT_RESULT"); }  // 0x609
    void __cdecl OnPurchaseResult(void*, uint32_t, uint32_t, CDataStore* p) { StringEvent(p, "MYSTIC_ENCHANT_PURCHASE_RESULT"); }      // 0x612
    void __cdecl OnPurchaseExtractResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x734
    {
        StringEvent(p, "MYSTIC_ENCHANT_PURCHASE_MYSTIC_EXTRACT_RESULT");
    }
    void __cdecl OnInspectResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x614
    {
        const std::string result = ReadCStr(p);
        InspectData d;
        d.progress = ReadU64(p);
        d.level = ReadU32(p);
        d.applied.resize(ReadU32(p));
        for (uint32_t& v : d.applied)
            v = ReadU32(p);
        d.known.resize(ReadU32(p));
        for (uint32_t& v : d.known)
            v = ReadU32(p);
        M().inspect.emplace(M().inspectKey, std::move(d));   // an existing key keeps its old result
        M().inspectKey = 0;
        AscRuntime::Signal("MYSTIC_ENCHANT_INSPECT_RESULT", "%s", result.c_str());
    }
    void __cdecl OnProgress(void*, uint32_t, uint32_t, CDataStore* p)   // 0x619
    {
        M().progress = ReadU64(p);
        M().level = ReadU32(p);
        AscRuntime::Signal("MYSTIC_ENCHANT_PROGRESS_UPDATE", "%f%u", ProgressPercent(M().progress, M().level), M().level);
    }
    // 0x56B SMSG_PATCH_MYSTIC_ENCHANT (FUN_102E8990): a 0x7C-byte row, then its +0x0C / +0x10 quality names
    // and its four +0x3C..+0x48 tag names; upsert, reset the container, MYSTIC_ENCHANT_PATCHED.
    void __cdecl OnPatch(void*, uint32_t, uint32_t, CDataStore* p)
    {
        std::vector<uint8_t> row(0x7C);
        memcpy(row.data(), p->m_buffer + p->m_read, 0x7C);
        p->m_read += 0x7C;
        AscDbc::Table& t = Dbc();
        const uint32_t offs[6] = {0xC, 0x10, 0x3C, 0x40, 0x44, 0x48};
        for (uint32_t off : offs)
        {
            const uint32_t s = t.AddString(ReadCStr(p).c_str());
            memcpy(row.data() + off, &s, 4);
        }
        uint32_t id;
        memcpy(&id, row.data(), 4);
        const bool existed = t.Row(id) != nullptr;
        t.Upsert(id, row);
        Idx().built = false;   // FUN_10133AC0 + the two FUN_100BD860 inserts (re-derived from the table)
        Idx().bySpell.clear();
        Idx().byItem.clear();
        if (!existed)
            M().container.Reset();
        AscRuntime::Signal("MYSTIC_ENCHANT_PATCHED");
    }

    // ---- lifecycle -----------------------------------------------------------------------------------
    void OnEnterWorld()   // 0x102EA2A0: prefetch the untarnished scroll's item record
    {
        AscAssetQuery::TryCacheItem(kMysticScroll);
    }
    void OnGlueScreen()   // 0x102E32D0
    {
        Mgr& m = M();
        m.known.clear();
        m.slots.clear();
        m.progress = 0;
        m.level = 0;
        m.pendingReforge.clear();
        m.pendingApply.clear();
        m.inspectKey = 0;
        m.inspect.clear();
        m.altarOpen = false;
        m.container.Reset();
    }
    void OnTick()   // 0x102EFF00: once a second while the altar is open, close it when out of range
    {
        Mgr& m = M();
        if (!m.altarOpen)
            return;
        const __time64_t now = _time64(nullptr);
        if (now - m.altarCheck < 1)
            return;
        m.altarCheck = now;
        if (AltarNearby())
            return;
        m.altarOpen = false;
        AscRuntime::Signal("MYSTIC_ALTAR_CLOSED");
    }
    void OnGameObjectUse(void* object)   // 0x10A42DD0 -> FUN_102EA1A0 / FUN_102EA2C0
    {
        if (!IsAltar(EntryOf(static_cast<uint8_t*>(object))))
            return;
        M().altarOpen = true;
        AscRuntime::Signal("MYSTIC_ALTAR_USED");
    }
    void OnItemUse(void* object)   // 0x10A42F30's first two branches
    {
        uint8_t* item = static_cast<uint8_t*>(object);
        const uint32_t entry = EntryOf(item);
        if (entry == kMysticScroll)
            AscRuntime::Signal("MYSTIC_SCROLL_USED", "%u", entry);   // FUN_102EA3A0
        else if (entry == kPresetUnlockToken)
            AscRuntime::Signal("MYSTIC_ENCHANT_UNLOCK_PRESET_USED");   // FUN_102EA330
    }

    void Init()   // the tail of FUN_102E9850
    {
        sDC.AddPacketHandler(0x5F9, CNetClientCustomPacket((void*)&OnUpdateKnown, nullptr));
        sDC.AddPacketHandler(0x5FA, CNetClientCustomPacket((void*)&OnAddKnown, nullptr));
        sDC.AddPacketHandler(0x5FB, CNetClientCustomPacket((void*)&OnRemoveKnown, nullptr));
        sDC.AddPacketHandler(0x5FC, CNetClientCustomPacket((void*)&OnUpdateData, nullptr));
        sDC.AddPacketHandler(0x5FD, CNetClientCustomPacket((void*)&OnUpdateSlots, nullptr));
        sDC.AddPacketHandler(0x5FE, CNetClientCustomPacket((void*)&OnUpdateSlot, nullptr));
        sDC.AddPacketHandler(0x604, CNetClientCustomPacket((void*)&OnReforgeResult, nullptr));
        sDC.AddPacketHandler(0x606, CNetClientCustomPacket((void*)&OnCollectionReforgeResult, nullptr));
        sDC.AddPacketHandler(0x609, CNetClientCustomPacket((void*)&OnDisenchantResult, nullptr));
        sDC.AddPacketHandler(0x60B, CNetClientCustomPacket((void*)&OnApplyResult, nullptr));
        sDC.AddPacketHandler(0x612, CNetClientCustomPacket((void*)&OnPurchaseResult, nullptr));
        sDC.AddPacketHandler(0x614, CNetClientCustomPacket((void*)&OnInspectResult, nullptr));
        sDC.AddPacketHandler(0x618, CNetClientCustomPacket((void*)&OnDestroyResult, nullptr));
        sDC.AddPacketHandler(0x56B, CNetClientCustomPacket((void*)&OnPatch, nullptr));
        sDC.AddPacketHandler(0x619, CNetClientCustomPacket((void*)&OnProgress, nullptr));
        sDC.AddPacketHandler(0x734, CNetClientCustomPacket((void*)&OnPurchaseExtractResult, nullptr));
        AscRuntime::OnEnterWorld(&OnEnterWorld);
        AscRuntime::OnGlueScreen(&OnGlueScreen);
        AscRuntime::OnAfter403340(&OnTick);
        AscRuntime::OnGameObjectUse(&OnGameObjectUse);
        AscRuntime::OnItemUse(&OnItemUse);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_MysticEnchant", "ReforgeItem", ReforgeItem},
        {"C_MysticEnchant", "CanReforgeItem", CanReforgeItem},
        {"C_MysticEnchant", "ReforgeSlot", ReforgeSlot},
        {"C_MysticEnchant", "CanReforgeSlot", CanReforgeSlot},
        {"C_MysticEnchant", "GetReforgeCost", GetReforgeCost},
        {"C_MysticEnchant", "CollectionReforgeItem", CollectionReforgeItem},
        {"C_MysticEnchant", "CanCollectionReforgeItem", CanCollectionReforgeItem},
        {"C_MysticEnchant", "CollectionReforgeSlot", CollectionReforgeSlot},
        {"C_MysticEnchant", "CanCollectionReforgeSlot", CanCollectionReforgeSlot},
        {"C_MysticEnchant", "CanCollectionReforgeAnySlot", CanCollectionReforgeAnySlot},
        {"C_MysticEnchant", "GetCollectionReforgeChanges", GetCollectionReforgeChanges},
        {"C_MysticEnchant", "UndoCollectionReforge", UndoCollectionReforge},
        {"C_MysticEnchant", "UndoLastCollectionReforge", UndoLastCollectionReforge},
        {"C_MysticEnchant", "SaveCollectionReforge", SaveCollectionReforge},
        {"C_MysticEnchant", "CanSaveCollectionReforge", CanSaveCollectionReforge},
        {"C_MysticEnchant", "GetCollectionReforgeSlotCost", GetCollectionReforgeSlotCost},
        {"C_MysticEnchant", "GetCollectionReforgeItemCost", GetCollectionReforgeItemCost},
        {"C_MysticEnchant", "GetSaveCollectionReforgeSlotCost", GetSaveCollectionReforgeSlotCost},
        {"C_MysticEnchant", "DisenchantItem", DisenchantItem},
        {"C_MysticEnchant", "CanDisenchantItem", CanDisenchantItem},
        {"C_MysticEnchant", "DisenchantSlot", DisenchantSlot},
        {"C_MysticEnchant", "CanDisenchantSlot", CanDisenchantSlot},
        {"C_MysticEnchant", "GetDisenchantCost", GetDisenchantCost},
        {"C_MysticEnchant", "ApplyItem", ApplyItem},
        {"C_MysticEnchant", "CanApplyItem", CanApplyItem},
        {"C_MysticEnchant", "ApplySlot", ApplySlot},
        {"C_MysticEnchant", "CanApplySlot", CanApplySlot},
        {"C_MysticEnchant", "CanApplyAnySlot", CanApplyAnySlot},
        {"C_MysticEnchant", "GetApplyChanges", GetApplyChanges},
        {"C_MysticEnchant", "UndoApply", UndoApply},
        {"C_MysticEnchant", "UndoLastApply", UndoLastApply},
        {"C_MysticEnchant", "SaveApply", SaveApply},
        {"C_MysticEnchant", "CanSaveApply", CanSaveApply},
        {"C_MysticEnchant", "GetApplyItemCost", GetApplyItemCost},
        {"C_MysticEnchant", "PurchaseMysticScroll", PurchaseMysticScroll},
        {"C_MysticEnchant", "CanPurchaseMysticScroll", CanPurchaseMysticScroll},
        {"C_MysticEnchant", "GetMysticScrolls", GetMysticScrolls},
        {"C_MysticEnchant", "SetMysticScrollFilter", SetMysticScrollFilter},
        {"C_MysticEnchant", "GetNumFilteredMysticScrolls", GetNumFilteredMysticScrolls},
        {"C_MysticEnchant", "GetFilteredMysticScrollAtIndex", GetFilteredMysticScrollAtIndex},
        {"C_MysticEnchant", "GetFilteredMysticScrolls", GetFilteredMysticScrolls},
        {"C_MysticEnchant", "GetMysticScrollCost", GetMysticScrollCost},
        {"C_MysticEnchant", "CanEquipItem", CanEquipItem},
        {"C_MysticEnchant", "CanEquipSlot", CanEquipSlot},
        {"C_MysticEnchant", "CanEquipEnchant", CanEquipEnchant},
        {"C_MysticEnchant", "QueryEnchants", QueryEnchants},
        {"C_MysticEnchant", "GetEnchantInfoBySpell", GetEnchantInfoBySpell},
        {"C_MysticEnchant", "GetEnchantInfoByItem", GetEnchantInfoByItem},
        {"C_MysticEnchant", "GetProgress", GetProgress},
        {"C_MysticEnchant", "GetAppliedEnchant", GetAppliedEnchant},
        {"C_MysticEnchant", "Inspect", Inspect},
        {"C_MysticEnchant", "CanInspect", CanInspect},
        {"C_MysticEnchant", "HasAnyScroll", HasAnyScroll},
        {"C_MysticEnchant", "HasAnyCollected", HasAnyCollected},
        {"C_MysticEnchant", "HasAnySlotEnchanted", HasAnySlotEnchanted},
        {"C_MysticEnchant", "HasNearbyMysticAltar", HasNearbyMysticAltar},
        {"C_MysticEnchant", "Destroy", Destroy},
        {"C_MysticEnchant", "CanDestroy", CanDestroy},
        {"C_MysticEnchant", "PurchaseMysticExtract", PurchaseMysticExtract},
        {"C_MysticEnchant", "CanPurchaseMysticExtract", CanPurchaseMysticExtract},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

namespace AscMysticEnchant
{
    uint32_t SlotEnchant(uint32_t slot) { return SlotAt(slot); }
    const uint32_t* Slots(uint32_t& count)
    {
        count = static_cast<uint32_t>(M().slots.size());
        return M().slots.data();
    }
    bool Slotted(uint32_t spell) { return std::find(M().slots.begin(), M().slots.end(), spell) != M().slots.end(); }
    bool Collected(uint32_t spell) { return Known(spell); }
    bool Worldforged(uint32_t spell)
    {
        Row r = BySpell(spell);
        return r && U32(r, 0x1C) != 0;
    }
}
