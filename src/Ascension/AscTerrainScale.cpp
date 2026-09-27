// Terrain texture scale (installer FUN_1033be70): after 0x7D0050 (__cdecl(chunk, a, b)) builds a map chunk's
// texture layers, each layer whose flags (+4 of its 16-byte entry, array at chunk data +0x12C) carry a
// scale in bits 11..13 -- a signed 3-bit exponent e: 1..5, 6 = -2, 7 = -1 -- has its transforms in the
// table 0xD250A0 (16 bytes per layer) scaled by 1 / 2^e: +0x120 / +0x124 always, +0xD0 / +0xD4 when the
// chunk's layer record (+0x34, 0x14 bytes each) has 0x40 (then zeroed when the layer flags lack 0x38).
// For e = 4 / 5 the chunk's position (data +0x28 / +0x24) modulo 2 / 4, times -0.5 / 0.25, is added to
// +0xD0 / +0xD4 so the larger texture tiles across chunks. Then 0x408210(0, 0, table, 0x25).
// (FUN_1033bef0)
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cmath>
#include <cstdint>

namespace
{
    typedef int(__cdecl* Fn3_t)(uint8_t*, uint32_t, uint32_t);
    Fn3_t g_7D0050 = nullptr;

    float& Table(uint32_t layer, uint32_t off) { return *reinterpret_cast<float*>(0xD250A0 + layer * 0x10 + off); }

    int __cdecl Hook7D0050(uint8_t* chunk, uint32_t a, uint32_t b)
    {
        const int r = g_7D0050(chunk, a, b);
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(chunk + 0x10);
        const uint8_t* layers = *reinterpret_cast<uint8_t* const*>(data + 0x12C);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(layers - 4) >> 4;
        for (uint32_t i = 0; i < count; ++i)
        {
            data = *reinterpret_cast<uint8_t* const*>(chunk + 0x10);
            layers = *reinterpret_cast<uint8_t* const*>(data + 0x12C);
            const uint32_t flags = *reinterpret_cast<const uint32_t*>(layers + i * 0x10 + 4);
            const uint32_t n = (flags >> 11) & 7;
            if (!n)
                continue;
            const int32_t e = n < 6 ? static_cast<int32_t>(n) : static_cast<int32_t>(n | 0xFFFFFFF8);
            const float scale = 1.0f / static_cast<float>(pow(2.0, static_cast<double>(static_cast<float>(e))));
            Table(i, 0x120) = scale * Table(i, 0x120);
            Table(i, 0x124) = scale * Table(i, 0x124);
            if (chunk[0x34 + i * 0x14] & 0x40)
            {
                Table(i, 0xD0) = scale * Table(i, 0xD0);
                Table(i, 0xD4) = scale * Table(i, 0xD4);
                if (!(flags & 0x38))
                {
                    Table(i, 0xD0) = 0.0f;
                    Table(i, 0xD4) = 0.0f;
                }
            }
            float du = 0.0f, dv = 0.0f;
            if (e == 4 || e == 5)
            {
                const uint32_t period = e != 4 ? 4 : 2;
                float factor = 1.0f;
                for (int32_t k = e; k > 3; --k)
                    factor *= -0.5f;
                du = static_cast<float>(static_cast<double>(*reinterpret_cast<const uint32_t*>(data + 0x28) % period)) * factor;
                dv = static_cast<float>(static_cast<double>(*reinterpret_cast<const uint32_t*>(data + 0x24) % period)) * factor;
            }
            Table(i, 0xD0) = du + Table(i, 0xD0);
            Table(i, 0xD4) = dv + Table(i, 0xD4);
        }
        reinterpret_cast<void(__cdecl*)(int, int, void*, int)>(0x408210)(0, 0, reinterpret_cast<void*>(0xD250A0), 0x25);
        return r;
    }

    void Init()   // FUN_1033be70
    {
        g_7D0050 = reinterpret_cast<Fn3_t>(AscRuntime::Detour(0x7D0050, 9, reinterpret_cast<void*>(&Hook7D0050)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
