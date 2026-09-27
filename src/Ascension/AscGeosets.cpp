// Creature geoset overrides (installer FUN_101a1570): CreatureDisplayInfoGeosetData.dbc picks, per
// display id, one geoset in any of the 20 geoset groups of the unit's model.
//
// Map (FUN_101ceee0 into 0x10BDF4D4, cleared first by FUN_101cf290): the table's records in file order,
// display (+0xC) -> group (+4) -> geoset (+8, 1..100); the first record for a display/group wins. The
// original builds it with the other indexes at load (0x101DA041) and again after every SMSG 0x6FC.
// FUN_101a1510(model): for each group 0..19 with an entry for the current display (0x10BDED60), the
// group's geosets i*100..i*100+99 are hidden and i*100+geoset shown (0x82C7C0).
//
// The current display is set around the client paths that build a unit's model:
//   0x73D5D0 (descriptor +0x114), 0x73E410 / 0x73FCC0 (+0x10C; 0x73E410's argument forced to 1 when
//   called from 0x740309), 0x4E3CD0 (the selected character-select entry, 0xB6B240 + index x 0x198:
//   display +0x164, model +0x18C), 0x597700 (its argument +0x24, on the last model 0x81F8F0 made).
// 0x4E7790 applies it to the model it gets. 0x82C7C0 (set geosets min..max visible) is REPLACED for
// loaded models (+0x10 bit 0): each geoset in range whose flag (+0x9C) changes, then 0x825D70.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <intrin.h>
#include <cstdint>
#include <unordered_map>

namespace
{
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint32_t>> g_geosets;   // 0x10BDF4D4
    bool g_built = false;
    uint32_t g_display = 0;       // 0x10BDED60
    void* g_lastModel = nullptr;  // 0x10BDED64

    uint32_t U32(const void* p, uint32_t off) { return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + off); }

    void Build()   // FUN_101ceee0
    {
        g_built = true;
        g_geosets.clear();
        AscDbc::Table& t = AscDbc::Get("DBFilesClient\\CreatureDisplayInfoGeosetData.dbc");
        for (uint32_t i = 0; i < t.Count(); ++i)
        {
            const uint8_t* r = t.RowAt(i);
            const uint32_t display = U32(r, 0xC), group = U32(r, 4), geoset = U32(r, 8);
            auto d = g_geosets.find(display);
            if (d != g_geosets.end() && d->second.count(group))
                continue;
            if (geoset - 1 < 100)
                g_geosets[display][group] = geoset;
        }
    }

    int32_t Lookup(uint32_t display, uint32_t group)   // FUN_101ced80
    {
        if (!g_built)
            Build();
        auto d = g_geosets.find(display);
        if (d == g_geosets.end())
            return -1;
        auto g = d->second.find(group);
        return g == d->second.end() ? -1 : static_cast<int32_t>(g->second);
    }

    typedef int(__fastcall* SetGeosets_t)(void*, void*, uint32_t, uint32_t, uint32_t);
    SetGeosets_t g_82C7C0 = nullptr;

    void Apply(void* model)   // FUN_101a1510
    {
        if (!g_display)
            return;
        for (uint32_t group = 0; group < 0x14; ++group)
        {
            const int32_t geoset = Lookup(g_display, group);
            if (geoset == -1)
                continue;
            const uint32_t base = group * 100;
            reinterpret_cast<int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(0x82C7C0)(model, base, base + 99, 0);
            reinterpret_cast<int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(0x82C7C0)(model, base + geoset, base + geoset, 1);
        }
    }

    int __fastcall Replace82C7C0(uint8_t* model, void* edx, uint32_t min, uint32_t max, uint32_t visible)   // 0x101A1440
    {
        if (!(model[0x10] & 1))
            return g_82C7C0(model, edx, min, max, visible);
        const uint8_t* instance = *reinterpret_cast<uint8_t* const*>(model + 0x2C);
        if (!instance)
            return 0;
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(instance + 0x170);
        const uint32_t count = U32(data, 0x1C);
        if (!count)
            return 0;
        const uint8_t* geosets = *reinterpret_cast<uint8_t* const*>(data + 0x20);
        uint32_t* flags = *reinterpret_cast<uint32_t**>(model + 0x9C);
        bool changed = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t id = *reinterpret_cast<const uint16_t*>(geosets + i * 0x30);
            if (min <= id && id <= max && flags[i] != visible)
            {
                flags[i] = visible;
                changed = true;
            }
        }
        if (changed)
            reinterpret_cast<void(__thiscall*)(uint8_t*)>(0x825D70)(model);
        return 0;
    }

    typedef int(__cdecl* Fn0_t)();
    typedef int(__cdecl* Fn2_t)(void*, uint32_t);
    typedef int(__fastcall* This1_t)(uint8_t*, void*, uint32_t);
    typedef int(__fastcall* This2_t)(uint8_t*, void*, uint32_t, uint32_t);
    typedef int(__fastcall* This3_t)(uint8_t*, void*, uint32_t, uint32_t, uint32_t);
    Fn0_t g_4E3CD0 = nullptr;
    Fn2_t g_4E7790 = nullptr;
    This1_t g_597700 = nullptr, g_73E410 = nullptr;
    This2_t g_73D5D0 = nullptr;
    This3_t g_73FCC0 = nullptr;

    uint32_t Descriptor(uint8_t* unit, uint32_t off) { return U32(*reinterpret_cast<uint8_t**>(unit + 8), off); }

    int __cdecl Hook4E3CD0()   // 0x101A12F0
    {
        const int r = g_4E3CD0();
        const uint32_t index = *reinterpret_cast<const uint32_t*>(0xAC436C);
        const uint8_t* entry = reinterpret_cast<const uint8_t*>(index * 0x198 + *reinterpret_cast<const uint32_t*>(0xB6B240));
        if (entry)
            if (void* model = *reinterpret_cast<void* const*>(entry + 0x18C))
            {
                g_display = U32(entry, 0x164);
                Apply(model);
                g_display = 0;
            }
        return r;
    }

    int __cdecl Hook4E7790(void* model, uint32_t b)   // 0x101A12D0
    {
        const int r = g_4E7790(model, b);
        Apply(model);
        return r;
    }

    int __fastcall Hook597700(uint8_t* self, void* edx, uint32_t a)   // 0x101A1340
    {
        const int r = g_597700(self, edx, a);
        if (g_lastModel)
        {
            g_display = U32(reinterpret_cast<void*>(a), 0x24);
            Apply(g_lastModel);
            g_display = 0;
        }
        return r;
    }

    int __fastcall Hook73D5D0(uint8_t* unit, void* edx, uint32_t a, uint32_t b)   // 0x101A1390
    {
        g_display = Descriptor(unit, 0x114);
        const int r = g_73D5D0(unit, edx, a, b);
        g_display = 0;
        return r;
    }

    int __fastcall Hook73E410(uint8_t* unit, void* edx, uint32_t a)   // 0x101A1400
    {
        g_display = Descriptor(unit, 0x10C);
        if (reinterpret_cast<uint32_t>(_ReturnAddress()) == 0x74030E)
            a = 1;
        const int r = g_73E410(unit, edx, a);
        g_display = 0;
        return r;
    }

    int __fastcall Hook73FCC0(uint8_t* unit, void* edx, uint32_t a, uint32_t b, uint32_t c)   // 0x101A13C0
    {
        g_display = Descriptor(unit, 0x10C);
        const int r = g_73FCC0(unit, edx, a, b, c);
        g_display = 0;
        return r;
    }

    template <class T> T Hook(uint32_t target, uint32_t prologue, void* fn)
    {
        return reinterpret_cast<T>(AscRuntime::Detour(target, prologue, fn));
    }

    void Init()   // FUN_101a1570, in its order (0x81F8F0's capture rides AscItemCollections' detour)
    {
        g_82C7C0 = Hook<SetGeosets_t>(0x82C7C0, 6, reinterpret_cast<void*>(&Replace82C7C0));   // hooked 0x82C7C0
        g_73FCC0 = Hook<This3_t>(0x73FCC0, 6, reinterpret_cast<void*>(&Hook73FCC0));          // hooked 0x73FCC0
        g_73D5D0 = Hook<This2_t>(0x73D5D0, 6, reinterpret_cast<void*>(&Hook73D5D0));          // hooked 0x73D5D0
        g_73E410 = Hook<This1_t>(0x73E410, 6, reinterpret_cast<void*>(&Hook73E410));          // hooked 0x73E410
        g_4E7790 = Hook<Fn2_t>(0x4E7790, 6, reinterpret_cast<void*>(&Hook4E7790));            // hooked 0x4E7790
        g_4E3CD0 = Hook<Fn0_t>(0x4E3CD0, 9, reinterpret_cast<void*>(&Hook4E3CD0));            // hooked 0x4E3CD0
        g_597700 = Hook<This1_t>(0x597700, 7, reinterpret_cast<void*>(&Hook597700));          // hooked 0x597700
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

// FUN_101a14f0 (hook object 0x10BCA4DC on 0x81F8F0): the model just made.
void AscGeosets_ModelCreated(void* model) { g_lastModel = model; }

// SMSG 0x6FC (FUN_101e1440) rebuilds the map after every row.
void AscGeosets_Rebuild(const uint8_t*) { Build(); }
