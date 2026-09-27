// The cursor module (installer FUN_10a1e950): cursor sizes and a software cursor.
//
//   CVar cursorSizePreferred (FUN_101145c0 -> glue vector, CVar.cpp; flags 1, "0", category 1; callback
//     FUN_10a1e320): "1" / "2" / "3" / "4" pick 48 / 64 / 96 / 128 px, anything else 32. The size is kept
//     as width size / (float)*0xC7D2C8 (0x10D3CBCC) and height 0x6C0AC0(size) (0x10D3CBD0); the texture
//     variant (0x10D3CBD4) becomes 2 for "1".."3" and 4 for "4" only when Data\Patch-Q.mpq opens (the HD
//     module), and 0 for 32 px. Without Patch-Q a non-zero size keeps the previous variant.
//   The cursor table 0x10D3C7A8: five slots per stock cursor index (1..0x34, names from 0xAD2808); on
//     every glue screen (FUN_10a1e140) empty slots 0 / 2 / 4 are loaded from Interface\Cursor\<name> and
//     <name>2 / <name>4. SetAttackCursor (AscGlobalsJ.cpp) writes indices 4 / 30 of the same table.
//     A second glue-screen callback (FUN_10a1e250) sets the cursor index *0xC26DE8 to 1.
//   0x7695E0 (gxCursor's callback, __cdecl 4 arguments; FUN_10a1e260): "1" (hardware) sets 0x10BCC87D,
//     calls 0x493C80(*0xB7A9DC) and releases the software cursor's render callback; anything else adds the
//     render callback FUN_10a1eb80 through 0x4A8BB0 and clears 0x10BCC87D. Then the original.
//   0x616830 (SetCursor(name); FUN_10a1e610): in software mode the texture is loaded here instead of by
//     the original -- a name containing "Directions" with a size variant loses its last four characters
//     and gains the variant number -- into 0x10D3C7A4, and the cursor index becomes 0x35.
//   0x683640 (__thiscall + 1 argument; FUN_10a205f0): unless hardware / *0xC25DE0, the argument (show
//     cursor) is kept in 0x10BCC87C for the software cursor and the original gets 0.
//   0x6162C0 (FUN_10a20530): after the original, 0x68E750(device, 1) while *0xC25DE0 is set.
//   The render callback (FUN_10a1eb80) draws the current cursor texture as a quad at the mouse position.
//   The world-registered callback FUN_10a20560 sets SetAttackCursor / SetRangedAttackTexture /
//     SetAttackTexture as globals; those are in AscGlobalsJ.cpp's binding table.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <share.h>
#include <string>

void* g_ascCursorTable[0x35 * 5] = {};   // 0x10D3C7A8

namespace
{
    void* g_custom = nullptr;          // 0x10D3C7A4
    void* g_renderHandle = nullptr;    // 0x10D3C7A0
    float g_width = 0.0f;              // 0x10D3CBCC
    float g_height = 0.0f;             // 0x10D3CBD0
    uint32_t g_variant = 0;            // 0x10D3CBD4
    bool g_show = true;                // 0x10BCC87C
    bool g_hardware = true;            // 0x10BCC87D

    typedef void*(__cdecl* Load_t)(const char*, uint32_t, void*, int);
    void* Load(const char* path) { return reinterpret_cast<Load_t>(0x4B9760)(path, 0x201, reinterpret_cast<void*>(0xAF5718), 1); }

    void SetSize(float px, int32_t size)
    {
        const uint32_t screen = *reinterpret_cast<const uint32_t*>(0xC7D2C8);
        g_width = px / static_cast<float>(static_cast<double>(screen));
        g_height = reinterpret_cast<float(__cdecl*)(int32_t)>(0x6C0AC0)(size);
    }

    void LoadTable()   // FUN_10a1e140
    {
        const char* const* names = reinterpret_cast<const char* const*>(0xAD2808);
        char path[260];
        for (uint32_t i = 1; i < 0x35; ++i)
        {
            void** row = &g_ascCursorTable[i * 5];
            if (!row[0])
            {
                sprintf_s(path, "Interface\\Cursor\\%s", names[i]);
                row[0] = Load(path);
            }
            for (uint32_t n : {2u, 4u})
            {
                if (row[n])
                    continue;
                sprintf_s(path, "Interface\\Cursor\\%s%u", names[i], n);
                row[n] = Load(path);
            }
        }
    }

    void ResetIndex()   // FUN_10a1e250
    {
        *reinterpret_cast<uint32_t*>(0xC26DE8) = 1;
    }

    void __cdecl Draw()   // FUN_10a1eb80
    {
        if (!g_show)
            return;
        const uint32_t index = *reinterpret_cast<const uint32_t*>(0xC26DE8);
        void* texture = index == 0x35 ? g_custom : g_ascCursorTable[g_variant + (index ? index : 1) * 5];
        void* gx = reinterpret_cast<void*(__cdecl*)(void*, int, int)>(0x4B6CB0)(texture, 0, 0);
        if (!gx)
            return;
        uint32_t x = 0, y = 0;
        reinterpret_cast<void(__cdecl*)(uint32_t*, uint32_t*)>(0x86A0D0)(&x, &y);
        void* device = *reinterpret_cast<void**>(0xC5DF88);
        float rect[4];
        reinterpret_cast<void(__thiscall*)(void*, float*)>(0x6A5A00)(device, rect);
        if (!x || !y)
            return;
        if (!(rect[3] > static_cast<float>(static_cast<double>(x))) || !(rect[2] > static_cast<float>(static_cast<double>(y))))
            return;
        float px = 0.0f, py = 0.0f;
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, float*, float*)>(0x47FC90)(x, y, &px, &py);
        const float top = py - g_height;
        const float right = g_width + px;
        uint32_t colour = 0xFFFFFFFF;
        float quad[12] = {px, top, 0.0f, right, top, 0.0f, px, py, 0.0f, right, py, 0.0f};
        uint32_t indices[2] = {0x10000, 0x30002};
        reinterpret_cast<void(__thiscall*)(void*)>(0x409670)(device);
        reinterpret_cast<void(__cdecl*)(uint32_t, void*)>(0x408240)(0x15, gx);
        reinterpret_cast<void(__cdecl*)(void*, int, int)>(0x681450)(gx, 0, 0);
        typedef void(__cdecl* State_t)(uint32_t, uint32_t);
        reinterpret_cast<State_t>(0x408BF0)(0xB, 0);
        reinterpret_cast<State_t>(0x408BF0)(0xC, 0);
        reinterpret_cast<State_t>(0x408BF0)(6, 2);
        reinterpret_cast<State_t>(0x408BF0)(7, 1);
        reinterpret_cast<void(__cdecl*)(uint32_t, float*, uint32_t, void*, void*, uint32_t*, void*, void*, void*, uint32_t, uint32_t, void*, void*)>(0x6828C0)(
            4, quad, 0xC, nullptr, nullptr, &colour, nullptr, nullptr, nullptr, 0xAB6400, 8, nullptr, nullptr);
        reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t*)>(0x682340)(4, 4, indices);
        *reinterpret_cast<uint32_t*>(0xC5DF84) = 0;
        reinterpret_cast<void(__thiscall*)(void*)>(0x685FB0)(device);
    }

    typedef int(__cdecl* CVarCallback_t)(void*, const char*, const char*, void*);
    CVarCallback_t g_7695E0 = nullptr;
    int __cdecl Hook7695E0(void* cvar, const char* previous, const char* value, void* arg)   // FUN_10a1e260
    {
        if (strcmp(value, "1") == 0)
        {
            g_hardware = true;
            reinterpret_cast<void(__stdcall*)(uint32_t)>(0x493C80)(*reinterpret_cast<const uint32_t*>(0xB7A9DC));
            if (g_renderHandle)
            {
                reinterpret_cast<void(__cdecl*)(void*)>(0x47BF30)(g_renderHandle);
                g_renderHandle = nullptr;
            }
        }
        else
        {
            g_hardware = false;
            reinterpret_cast<void(__cdecl*)(int, float, int, int, void(__cdecl*)(), void**)>(0x4A8BB0)(0, 2.0f, 3, 0, &Draw, &g_renderHandle);
        }
        return g_7695E0(cvar, previous, value, arg);
    }

    typedef int(__cdecl* SetCursor_t)(const char*);
    SetCursor_t g_616830 = nullptr;
    int __cdecl Hook616830(const char* name)   // FUN_10a1e610
    {
        if (g_hardware)
            return g_616830(name);
        std::string s(name);
        if (g_variant != 0 && s.size() >= 10 && s.find("Directions") != std::string::npos)
        {
            s = s.substr(0, s.size() - 4);
            s += std::to_string(g_variant);
            g_custom = Load(s.c_str());
        }
        else
            g_custom = Load(name);
        *reinterpret_cast<uint32_t*>(0xC26DE8) = 0x35;
        return 0;
    }

    typedef int(__fastcall* Fn683640_t)(void*, void*, uint32_t);
    Fn683640_t g_683640 = nullptr;
    int __fastcall Hook683640(void* self, void* edx, uint32_t show)   // FUN_10a205f0
    {
        const bool stock = g_hardware || *reinterpret_cast<const uint32_t*>(0xC25DE0) != 0;
        g_show = (stock ? 0 : show) != 0;
        return g_683640(self, edx, stock ? show : 0);
    }

    typedef void(__cdecl* Fn0_t)();
    Fn0_t g_6162C0 = nullptr;
    void __cdecl Hook6162C0()   // FUN_10a20530
    {
        g_6162C0();
        if (*reinterpret_cast<const uint32_t*>(0xC25DE0) != 0)
            reinterpret_cast<void(__thiscall*)(void*, int)>(0x68E750)(*reinterpret_cast<void**>(0xC5DF88), 1);
    }

    void Init()   // FUN_10a1e950 (the CVar is registered from CVar.cpp's glue vector)
    {
        g_7695E0 = reinterpret_cast<CVarCallback_t>(AscRuntime::Detour(0x7695E0, 6, reinterpret_cast<void*>(&Hook7695E0)));
        g_6162C0 = reinterpret_cast<Fn0_t>(AscRuntime::Detour(0x6162C0, 6, reinterpret_cast<void*>(&Hook6162C0)));
        g_616830 = reinterpret_cast<SetCursor_t>(AscRuntime::Detour(0x616830, 6, reinterpret_cast<void*>(&Hook616830)));
        g_683640 = reinterpret_cast<Fn683640_t>(AscRuntime::Detour(0x683640, 6, reinterpret_cast<void*>(&Hook683640)));
        AscRuntime::OnGlueScreen(&LoadTable);
        AscRuntime::OnGlueScreen(&ResetIndex);
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

// FUN_10a1e320: cursorSizePreferred's callback.
int __cdecl AscCursor_SizePreferred(void*, const char*, const char* value, void*)
{
    std::ifstream probe("Data\\Patch-Q.mpq", std::ios::in, _SH_DENYNO);
    const bool hd = probe.is_open();
    probe.close();
    if (strcmp(value, "0") != 0)
    {
        if (strcmp(value, "1") == 0 || strcmp(value, "2") == 0 || strcmp(value, "3") == 0)
        {
            const bool one = value[0] == '1', two = value[0] == '2';
            SetSize(one ? 48.0f : two ? 64.0f : 96.0f, one ? 0x30 : two ? 0x40 : 0x60);
            if (hd)
                g_variant = 2;
            return 1;
        }
        if (strcmp(value, "4") == 0)
        {
            SetSize(128.0f, 0x80);
            if (hd)
                g_variant = 4;
            return 1;
        }
    }
    SetSize(32.0f, 0x20);
    g_variant = 0;
    return 1;
}
