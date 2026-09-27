// Spell shadows -- the cone indicator for directional ground-target spells (installer FUN_103216a0).
//
//   CVar SpellShadowShowBad (flags 1, "1", category 4; 0x10BE442C) -- registered, never read.
//   World entry (FUN_10320b60): Interface\SpellShadow\Spell-Shadow-Cone.blp and -Unacceptable.blp are
//     (re)loaded into 0x10BE4418 / 0x10BE441C (the old ones released through 0x47BF30; each load gets a
//     fresh 8-byte status {0x9F4250, 8} that is never freed). Only the cone is ever used.
//   0x616800 (the cursor mode, __cdecl(mode); FUN_10320c20): mode 1 puts the stock texture back into the
//     ground-target code (the pointer operand at 0x4F8A4B = 0xB74350), state 0; mode 0x1C with a pending
//     spell (*0xD3F4E4 +0x18) whose SpellCustomAttr +0x10 has 0x200000 / 0x400000 points it at the cone
//     (&0x10BE4418), state 2 / 3.
//   0x4F8A40 (the ground-target draw, __cdecl(a); FUN_10321240): the operand at 0x4F8AD4 is 0xA4040C while
//     *0xAC79A4 is set, else 0xB74370; in state 2 / 3 the target point (0xB74380) is saved at 0x10BE4420
//     and replaced by the player's position, *0xAC79A4 = 0 and *0xB74370 = 0x8019C0().
//   0x7E4370 (__cdecl, seven arguments; FUN_10321830): called from 0x4F8CF0 in state 2 (angle from the
//     player to the saved point, 0x4F5130) or 3 (the player's facing), the matrix 0x10BCC428 becomes a
//     rotation by -angle, both points of argument 1 move 7.5 yd along the angle, and arguments 2 / 3
//     become 0xAD2D40 / that matrix; called from 0x725A8F, argument 1's z values move 2.5 apart.
// Code operands are written through NtProtect in the original.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Windows.h>
#include <intrin.h>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace
{
    void* g_cone = nullptr;           // 0x10BE4418
    void* g_unacceptable = nullptr;   // 0x10BE441C
    int32_t g_state = 0;              // 0x10BE4414
    uint8_t g_savedTarget[12] = {};   // 0x10BE4420 (x, y) / 0x10BE4428 (z)
    float g_rotation[16] = {};        // 0x10BCC428

    void WriteCode32(uint32_t at, uint32_t value)
    {
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint32_t*>(at) = value;
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(at), 4, old, &old);
    }

    void LoadTextures()   // FUN_10320b60
    {
        typedef void(__cdecl* Release_t)(void*);
        if (g_cone)
            reinterpret_cast<Release_t>(0x47BF30)(g_cone);
        if (g_unacceptable)
            reinterpret_cast<Release_t>(0x47BF30)(g_unacceptable);
        uint32_t* status = new uint32_t[2]{0x9F4250, 8};
        typedef void*(__cdecl* Load_t)(const char*, uint32_t, void*, int);
        g_cone = reinterpret_cast<Load_t>(0x4B9760)("Interface\\SpellShadow\\Spell-Shadow-Cone.blp", 0x201, status, 1);
        g_unacceptable = reinterpret_cast<Load_t>(0x4B9760)("Interface\\SpellShadow\\Spell-Shadow-Unacceptable.blp", 0x201, status, 1);
    }

    bool CustomAttr10(uint32_t spell, uint32_t mask)   // FUN_103249d0
    {
        const uint8_t* row = AscCA::SpellCustomAttrRow(spell);
        return row && (*reinterpret_cast<const uint32_t*>(row + 0x10) & mask) != 0;
    }

    typedef int(__cdecl* Fn1_t)(uint32_t);
    Fn1_t g_616800 = nullptr, g_4F8A40 = nullptr;

    int __cdecl Hook616800(uint32_t mode)   // FUN_10320c20
    {
        if (mode == 1)
        {
            WriteCode32(0x4F8A4B, 0xB74350);
            g_state = 0;
        }
        else if (mode == 0x1C)
        {
            const uint8_t* pending = *reinterpret_cast<uint8_t* const*>(0xD3F4E4);
            if (pending && CustomAttr10(*reinterpret_cast<const uint32_t*>(pending + 0x18), 0x200000))
            {
                WriteCode32(0x4F8A4B, reinterpret_cast<uint32_t>(&g_cone));
                g_state = 2;
            }
            else if (pending && CustomAttr10(*reinterpret_cast<const uint32_t*>(pending + 0x18), 0x400000))
            {
                WriteCode32(0x4F8A4B, reinterpret_cast<uint32_t>(&g_cone));
                g_state = 3;
            }
        }
        return g_616800(mode);
    }

    int __cdecl Hook4F8A40(uint32_t a)   // FUN_10321240
    {
        WriteCode32(0x4F8AD4, *reinterpret_cast<const uint32_t*>(0xAC79A4) != 0 ? 0xA4040C : 0xB74370);
        uint8_t* player = AscScript::ActivePlayer();
        if (player && (g_state == 2 || g_state == 3))
        {
            memcpy(g_savedTarget, reinterpret_cast<const void*>(0xB74380), 12);
            float position[3];
            reinterpret_cast<void(__thiscall*)(uint8_t*, float*)>((*reinterpret_cast<void***>(player))[0x2C / 4])(player, position);
            memcpy(reinterpret_cast<void*>(0xB74380), position, 12);
            *reinterpret_cast<uint32_t*>(0xAC79A4) = 0;
            *reinterpret_cast<float*>(0xB74370) = reinterpret_cast<float(__cdecl*)()>(0x8019C0)();
        }
        return g_4F8A40(a);
    }

    // FUN_102bd3f0: this = RotZ(angle) x this (row-major 4x4; FUN_102bcea0 is out = a x b).
    void RotateBy(float* m, float angle)
    {
        const float c = static_cast<float>(cos(static_cast<double>(angle)));
        const float s = static_cast<float>(sin(static_cast<double>(angle)));
        const float r[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float out[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = r[i * 4 + 1] * m[4 + j] + r[i * 4] * m[j] + r[i * 4 + 2] * m[8 + j] + r[i * 4 + 3] * m[12 + j];
        memcpy(m, out, sizeof(out));
    }

    typedef int(__cdecl* Fn7_t)(float*, uint32_t, uint32_t, float, uint32_t, uint32_t, float);
    Fn7_t g_7E4370 = nullptr;
    int __cdecl Hook7E4370(float* pos, uint32_t b, uint32_t c, float d, uint32_t e, uint32_t f, float g)   // FUN_10321830
    {
        const uint32_t caller = reinterpret_cast<uint32_t>(_ReturnAddress());
        if (caller == 0x4F8CF0)
        {
            uint8_t* player = AscScript::ActivePlayer();
            if (player && (g_state == 2 || g_state == 3))
            {
                void** vtable = *reinterpret_cast<void***>(player);
                float angle;
                if (g_state == 2)
                {
                    float position[3];
                    reinterpret_cast<void(__thiscall*)(uint8_t*, float*)>(vtable[0x2C / 4])(player, position);
                    angle = reinterpret_cast<float(__cdecl*)(const float*, const void*)>(0x4F5130)(position, g_savedTarget);
                }
                else
                    angle = reinterpret_cast<float(__thiscall*)(uint8_t*)>(vtable[0x34 / 4])(player);
                reinterpret_cast<void(__thiscall*)(float*)>(0x407F40)(g_rotation);
                RotateBy(g_rotation, -angle);
                const float dx = static_cast<float>(cos(static_cast<double>(angle))) * 7.5f;
                pos[3] = dx + pos[3];
                const float dy = static_cast<float>(sin(static_cast<double>(angle))) * 7.5f;
                pos[4] = dy + pos[4];
                pos[5] = pos[5] + 0.0f;
                pos[0] = dx + pos[0];
                pos[1] = dy + pos[1];
                pos[2] = pos[2] + 0.0f;
                b = 0xAD2D40;
                c = reinterpret_cast<uint32_t>(g_rotation);
            }
        }
        else if (caller == 0x725A8F)
        {
            pos[5] = pos[5] + 2.5f;
            pos[2] = pos[2] - 2.5f;
        }
        return g_7E4370(pos, b, c, d, e, f, g);
    }

    void Init()   // FUN_103216a0
    {
        AscRuntime::OnEnterWorld(&LoadTextures);
        g_616800 = reinterpret_cast<Fn1_t>(AscRuntime::Detour(0x616800, 6, reinterpret_cast<void*>(&Hook616800)));
        g_7E4370 = reinterpret_cast<Fn7_t>(AscRuntime::Detour(0x7E4370, 9, reinterpret_cast<void*>(&Hook7E4370)));
        g_4F8A40 = reinterpret_cast<Fn1_t>(AscRuntime::Detour(0x4F8A40, 8, reinterpret_cast<void*>(&Hook4F8A40)));
        AscClientOptions::QueueWorldCVar({"SpellShadowShowBad", "1", 1, 4, nullptr});
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

void AscSpellShadow_RotateBy(float* m, float angle) { RotateBy(m, angle); }
