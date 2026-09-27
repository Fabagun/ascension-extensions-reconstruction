// Loading-screen variations and a start-up gate (installer FUN_10290630).
//
//   0x409ED0  first: the map being loaded (*0xAB63BC) gets one of its LoadingScreenVariation.dbc
//             entries (+4..+0x40, the non-zero ones) at random as its Map.dbc loading screen (+0x24)
//             (FUN_10290810 / FUN_10210a60: std::mt19937 seeded once from std::random_device, uniform)
//   0x409550  after it the gate 0x10BCB8C8 opens; 0x40AB70 closes it after it runs
//   0x86B280  runs only while the gate is open
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>
#include <random>
#include <vector>

namespace
{
    bool g_open = false;   // 0x10BCB8C8

    uint32_t PickVariation(const uint8_t* row)   // FUN_10210a60
    {
        std::vector<uint32_t> ids;
        for (uint32_t i = 0; i < 0x10; ++i)
            if (const uint32_t id = *reinterpret_cast<const uint32_t*>(row + 4 + i * 4))
                ids.push_back(id);
        if (ids.empty())
            return 0;
        static std::mt19937 rng(std::random_device{}());   // 0x10BE0CA8
        std::uniform_int_distribution<int> pick(0, static_cast<int>(ids.size()) - 1);
        return ids[pick(rng)];
    }

    void __cdecl ApplyVariation()   // FUN_10290810, before the original
    {
        const uint32_t map = *reinterpret_cast<const uint32_t*>(0xAB63BC);
        const uint8_t* variation = AscDbc::Get("DBFilesClient\\LoadingScreenVariation.dbc").Row(map);
        if (!variation)
            return;
        uint8_t* row = const_cast<uint8_t*>(AscClientDbc::Row(0xAD4160, map));   // Map.dbc
        if (!row)
            return;
        if (const uint32_t screen = PickVariation(variation))
            *reinterpret_cast<uint32_t*>(row + 0x24) = screen;
    }

    void* g_409ED0 = nullptr;
    __declspec(naked) void Hook409ED0()   // the original is entered with the caller's stack untouched
    {
        __asm
        {
            pushad
            pushfd
            call ApplyVariation
            popfd
            popad
            jmp dword ptr [g_409ED0]
        }
    }

    typedef int(__cdecl* Fn0_t)();
    typedef int(__cdecl* Fn1_t)(uint32_t);
    typedef int(__cdecl* Fn2_t)(uint32_t, uint32_t);
    Fn0_t g_409550 = nullptr;
    Fn2_t g_40AB70 = nullptr;
    Fn1_t g_86B280 = nullptr;

    int __cdecl Hook409550()   // FUN_10290870
    {
        const int r = g_409550();
        g_open = true;
        return r;
    }

    int __cdecl Hook40AB70(uint32_t a, uint32_t b)   // FUN_10290890
    {
        const int r = g_40AB70(a, b);
        g_open = false;
        return r;
    }

    int __cdecl Hook86B280(uint32_t a)   // FUN_102908b0
    {
        return g_open ? g_86B280(a) : 0;
    }

    void Init()   // FUN_10290630
    {
        g_409ED0 = AscRuntime::Detour(0x409ED0, 6, reinterpret_cast<void*>(&Hook409ED0));
        g_40AB70 = reinterpret_cast<Fn2_t>(AscRuntime::Detour(0x40AB70, 6, reinterpret_cast<void*>(&Hook40AB70)));
        g_409550 = reinterpret_cast<Fn0_t>(AscRuntime::Detour(0x409550, 6, reinterpret_cast<void*>(&Hook409550)));
        g_86B280 = reinterpret_cast<Fn1_t>(AscRuntime::Detour(0x86B280, 6, reinterpret_cast<void*>(&Hook86B280)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
