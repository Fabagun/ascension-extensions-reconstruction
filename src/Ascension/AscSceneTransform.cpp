// Installer FUN_101aeac0 (with the MovieFrame:SetVolume registrar hook, see AscWidget.cpp):
//
//   0x95EED0  (__thiscall, no arguments) FUN_101ae900: 0x489230 on this+0x20 gives four floats q; the
//             DLL keeps { 0x47C060(q0), 0x47C050(q1), 0x47C060(q2), 0x47C050(q3) } (0x10BCA734..0x10BCA740),
//             saves the device's matrix at +0xF88 and the one 0x4080E0 reads, runs the original, then
//             restores them (0x6A9B40 / 0x6A9E00)
//   0x681F60  (__cdecl, six floats) FUN_101aed40: when called from inside 0x95EED0 (return 0x95EF4A) its
//             first four arguments are replaced by the kept values, in the order 738, 740, 734, 73C
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <intrin.h>
#include <cstdint>
#include <cstring>

namespace
{
    float g_kept[4] = {};   // 0x10BCA734, 738, 73C, 740

    typedef float(__cdecl* Fn1f_t)(float);
    float F1(float v) { return reinterpret_cast<Fn1f_t>(0x47C060)(v); }
    float F2(float v) { return reinterpret_cast<Fn1f_t>(0x47C050)(v); }

    void Identity(float* m)   // FUN_100d2360
    {
        memset(m, 0, 64);
        m[0] = m[5] = m[10] = m[15] = 1.0f;
    }

    typedef int(__fastcall* Fn95EED0_t)(uint8_t*, void*);
    Fn95EED0_t g_95EED0 = nullptr;
    int __fastcall Hook95EED0(uint8_t* self, void* edx)
    {
        float q[4];
        reinterpret_cast<void(__thiscall*)(uint8_t*, float*)>(0x489230)(self + 0x20, q);
        g_kept[0] = F1(q[0]);
        g_kept[1] = F2(q[1]);
        g_kept[2] = F1(q[2]);
        g_kept[3] = F2(q[3]);
        float view[16], world[16];
        Identity(view);
        Identity(world);
        reinterpret_cast<void(__cdecl*)(float*)>(0x4080E0)(view);
        uint8_t* device = *reinterpret_cast<uint8_t**>(0xC5DF88);
        memcpy(world, device + 0xF88, 64);
        const int r = g_95EED0(self, edx);
        device = *reinterpret_cast<uint8_t**>(0xC5DF88);
        reinterpret_cast<void(__thiscall*)(uint8_t*, float*)>(0x6A9B40)(device, world);
        device = *reinterpret_cast<uint8_t**>(0xC5DF88);
        reinterpret_cast<void(__thiscall*)(uint8_t*, float*)>(0x6A9E00)(device, view);
        return r;
    }

    typedef int(__cdecl* Fn681F60_t)(float, float, float, float, float, float);
    Fn681F60_t g_681F60 = nullptr;
    int __cdecl Hook681F60(float a, float b, float c, float d, float e, float f)
    {
        if (reinterpret_cast<uint32_t>(_ReturnAddress()) == 0x95EF4A)
        {
            a = g_kept[1];
            b = g_kept[3];
            c = g_kept[0];
            d = g_kept[2];
        }
        return g_681F60(a, b, c, d, e, f);
    }

    void Init()   // FUN_101aeac0 (0x970610's registrar detour is in AscWidget)
    {
        g_95EED0 = reinterpret_cast<Fn95EED0_t>(AscRuntime::Detour(0x95EED0, 9, reinterpret_cast<void*>(&Hook95EED0)));
        g_681F60 = reinterpret_cast<Fn681F60_t>(AscRuntime::Detour(0x681F60, 5, reinterpret_cast<void*>(&Hook681F60)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
