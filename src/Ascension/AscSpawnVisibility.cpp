// FUN_101a2040 (a glue-screen callback of the same module) zeroes 0x10BDEDA0, whose only accessor
// (0x101A2070) has no caller in the plain code -- not reproduced.
// FUN_101a2020: spell 0x16E59 is only applied (list 0x10BE2B6C, before 0x724820; callback 0x101A1FA0) to
// an object that ObjectSpawnVisibilitySettings.dbc does NOT show (FUN_10211e10 false).
//
// ObjectSpawnVisibilitySettings.dbc: +0 id, +4 group, +8 TYPEID_* name, +0xC entry, +0x10 GUID counter,
// +0x14 an id that must be in the client's list at *(0xACFDEC + 8), +0x18 one that must be in the list at
// *(0xACFDB0 + 8) (FUN_103087e0). FUN_10211e10(type, entry, counter): rows of the object's type are grouped
// by +4 twice -- those naming its entry, those naming its counter. Each set passes when it is empty or one
// of its groups has every row passing FUN_10211db0; the object is shown when both pass.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstring>
#include <map>
#include <vector>

namespace
{
    // FUN_10211aa0: a TYPEID_* name -> its index (0x10B3C7A8); anything else is logged and gives `fallback`.
    uint32_t TypeIdByName(const char* name, uint32_t fallback)
    {
        static const char* const kNames[] = {"TYPEID_OBJECT", "TYPEID_ITEM", "TYPEID_CONTAINER", "TYPEID_UNIT",
                                             "TYPEID_PLAYER", "TYPEID_GAMEOBJECT", "TYPEID_DYNAMICOBJECT", "TYPEID_CORPSE"};
        for (uint32_t i = 0; i < 8; ++i)
            if (strcmp(name, kNames[i]) == 0)
                return i;
        AscLog::Printf("Unexpected Value: %s", name);
        return fallback;
    }

    // The client's tagged singly-linked id lists (node +4 next, +8 id; a set low bit ends the list).
    bool InClientList(uint32_t listBase, uint32_t id)
    {
        uint32_t node = *reinterpret_cast<const uint32_t*>(listBase + 8);
        if (node & 1)
            node = 0;
        while (node)
        {
            if (*reinterpret_cast<const uint32_t*>(node + 8) == id)
                return true;
            const uint32_t next = *reinterpret_cast<const uint32_t*>(node + 4);
            node = (next & 1) ? 0 : next;
        }
        return false;
    }

    bool RowPasses(const uint8_t* row)   // FUN_10211db0
    {
        const uint32_t a = AscDbc::Table::U32(row, 0x14);
        if (a != 0 && !InClientList(0xACFDEC, a))
            return false;
        const uint32_t b = AscDbc::Table::U32(row, 0x18);
        if (b != 0)
            return InClientList(0xACFDB0, b);
        return true;
    }

    bool SetPasses(const std::map<uint32_t, std::vector<const uint8_t*>>& groups)
    {
        if (groups.empty())
            return true;
        for (const auto& g : groups)
        {
            bool all = true;
            for (const uint8_t* row : g.second)
                if (!RowPasses(row))
                {
                    all = false;
                    break;
                }
            if (all)
                return true;
        }
        return false;
    }

    bool Visible(uint32_t type, uint32_t entry, uint32_t counter)   // FUN_10211e10
    {
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\ObjectSpawnVisibilitySettings.dbc");
        std::map<uint32_t, std::vector<const uint8_t*>> byEntry, byCounter;
        for (uint32_t id = t.MinId(); t.Loaded() && id <= t.MaxId(); ++id)
        {
            const uint8_t* row = t.Row(id);
            if (!row || TypeIdByName(t.Str(row, 8), 0) != type)
                continue;
            const uint32_t e = AscDbc::Table::U32(row, 0xC);
            if (e != 0 && e == entry)
                byEntry[AscDbc::Table::U32(row, 4)].push_back(row);
            const uint32_t c = AscDbc::Table::U32(row, 0x10);
            if (c != 0 && c == counter)
                byCounter[AscDbc::Table::U32(row, 4)].push_back(row);
        }
        return SetPasses(byEntry) && SetPasses(byCounter);
    }

    // 0x101A1FA0
    bool __cdecl BeforeAuraApply(void* object, uint32_t, uint32_t rec)
    {
        if (*reinterpret_cast<const uint32_t*>(rec) != 0x16E59)
            return true;
        const uint8_t* o = static_cast<const uint8_t*>(object);
        const uint32_t* desc = *reinterpret_cast<uint32_t* const*>(o + 8);
        const uint32_t lo = desc[0], hi = desc[1];
        uint32_t counter = lo;
        if (hi >= 0x10000000)
            switch (hi >> 16)
            {
            case 0xF101: case 0x1FC0: case 0x1F40: case 0x1F50: case 0x4000: case 0xF100: break;
            default: counter = lo & 0xFFFFFF;
            }
        return !Visible(*reinterpret_cast<const uint32_t*>(o + 0x14), desc[3], counter);
    }

    void Init() { AscRuntime::OnBefore724820(&BeforeAuraApply, 0x101A2025); }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
