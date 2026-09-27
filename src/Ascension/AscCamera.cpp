// Action camera, camera target focus and camera M2 collision (module init FUN_1019c510).
//
// CVars (queued like the attach init's, see AscClientOptions; category 4) -- callbacks compare with "1":
//   ActionCam "0" (FUN_10199420)            "1": 0x5FFD34 = 7A
//   cameraTargetFocusInteractEnable "0"
//   cameraTargetFocusEnemyEnable "0" (FUN_1019a9e0)   jne/jmp at 0x72E870 / 0x72E5FE / 0x72E6AE / 0x5FBC80,
//                                           and the call at 0x7569B6 NOPed while on
//   cameraActionDist "1.25", cameraActionAngle "4.0", cameraActionZ "0.05"
//   cameraActionHeadBobs "0" (FUN_10199da0)  0x5FFD34 = EB on / 7A off
//   cameraTargetFocusTurnSpeed "3.15" (FUN_1019bcb0)  the client's own writers of 0xCA11DC NOPed (0x727520,
//                                           0x731720); 0xCA11DC = clamp(atof(value), 0.5, 16.5)
//   cameraM2Collision "1", flags 0x21 (FUN_1019a190)  the store at 0x791E60 and flag 0x100171 at 0x605E13
//                                           (0x170 without the M2 bit) follow the value
//   cameraM2CollisionAlpha "0.5", flags 0x21
//   cameraZoomFactor "1.0"
//   autoTarget "1" (FUN_10199650)           jne at 0x75697C / 0x806AA2 on, jmp off
// Hooks:
//   0x601D60 -> sub_101992c0  camera target position: + {cos, sin} (unit facing + angle) * dist, + Z
//   0x605D60 -> sub_10199050  camera collision: M2s between the player's head and the camera fade to
//                             cameraM2CollisionAlpha; with ActionCam the camera also shifts sideways
//   0x7A25F0 -> FUN_1019c2f0  REPLACED: the collision callback records each hit M2 and its alpha
//   0x401480 / 0x5FF950 / 0x5FFA60 -> the hit M2s' alphas are restored first (0x401480) / pass-through
//   0x5241B0 -> sub_101993b0  focus the player's camera on the target (0x7272C0) when the target clears
// Callbacks: after a target change (AscRuntime, FUN_10278330) FUN_1019c420 turns the camera to a hostile
// (enemy focus) or talkative (interact focus) target; before 0x6ECF80 (FUN_10278660) FUN_10198e60
// restores the faded M2s. The Lua natives InteractNearest / SetCursorPosition / GetScreenPosition
// (FUN_1019cfb0) live in AscGlobalsI / AscGlobalsF.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Client/CVar.hpp>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>
#include <windows.h>

namespace
{
    void WriteCode(uint32_t at, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(at), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(at), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(at), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), n);
    }
    void WriteByte(uint32_t at, uint8_t v) { WriteCode(at, &v, 1); }
    bool IsOne(const char* value) { return value && strcmp(value, "1") == 0; }

    // The CVars (the original's slots 0x10BDECF0 ..), found after registration.
    CVar* Var(const char* name)
    {
        return CVar::Lookup(name);
    }
    int32_t Int(const char* name)
    {
        CVar* v = Var(name);
        return v ? *reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(v) + 0x30) : 0;
    }
    float Float(const char* name)
    {
        CVar* v = Var(name);
        return v ? *reinterpret_cast<const float*>(reinterpret_cast<uint8_t*>(v) + 0x2C) : 0.0f;
    }
    bool ActionCam() { return Int("ActionCam") != 0; }

    uint8_t* ActivePlayer() { return reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)(); }
    uint8_t* ObjectByGuid(uint32_t lo, uint32_t hi, uint32_t mask)
    {
        return reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(lo, hi, mask);
    }
    float Facing(uint8_t* unit)   // vtable +0x34
    {
        return (*reinterpret_cast<float(__thiscall**)(void*)>(*reinterpret_cast<uint8_t**>(unit) + 0x34))(unit);
    }

    // ---- CVar callbacks ------------------------------------------------------------------------------------
    int __cdecl OnActionCam(void*, const char*, const char* value, void*)   // FUN_10199420
    {
        if (IsOne(value))
            WriteByte(0x5FFD34, 0x7A);
        return 1;
    }
    int __cdecl OnHeadBobs(void*, const char*, const char* value, void*)   // FUN_10199da0
    {
        WriteByte(0x5FFD34, IsOne(value) ? 0xEB : 0x7A);
        return 1;
    }
    int __cdecl OnEnemyFocus(void*, const char*, const char* value, void*)   // FUN_1019a9e0
    {
        const bool on = IsOne(value);
        for (uint32_t at : {0x72E870u, 0x72E5FEu, 0x72E6AEu, 0x5FBC80u})
            WriteByte(at, on ? 0xEB : 0x75);
        static const uint8_t kCall[5] = {0xE8, 0x05, 0x09, 0xFD, 0xFF}, kNop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        WriteCode(0x7569B6, on ? kNop5 : kCall, 5);
        return 1;
    }
    int __cdecl OnTurnSpeed(void*, const char*, const char* value, void*)   // FUN_1019bcb0
    {
        uint8_t nops[32];
        memset(nops, 0x90, sizeof(nops));
        WriteCode(0x727520, nops, 32);
        WriteCode(0x731720, nops, 6);
        const float v = static_cast<float>(atof(value));
        float r = v < 0.5f ? 0.5f : v;   // cmovbe pair: NaN passes through, as in the original
        if (v > 16.5f)
            r = 16.5f;
        *reinterpret_cast<float*>(0xCA11DC) = r;
        return 1;
    }
    int __cdecl OnM2Collision(void*, const char*, const char* value, void*)   // FUN_1019a190
    {
        static const uint8_t kStore[6] = {0xD9, 0x98, 0x78, 0x01, 0x00, 0x00}, kNop6[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const bool on = IsOne(value);
        WriteCode(0x791E60, on ? kStore : kNop6, 6);
        const uint32_t flags = on ? 0x100171u : 0x170u;
        WriteCode(0x605E13, &flags, 4);
        return 1;
    }
    int __cdecl OnAutoTarget(void*, const char*, const char* value, void*)   // FUN_10199650
    {
        const uint8_t b = IsOne(value) ? 0x75 : 0xEB;
        WriteByte(0x75697C, b);
        WriteByte(0x806AA2, b);
        return 1;
    }

    // ---- the faded M2s (0x10BDED20 .. 0x10BDED28): {model, alpha before the fade} ---------------------------
    std::vector<std::pair<uint8_t*, float>> g_hits;
    bool InWorld() { return *reinterpret_cast<const uint8_t*>(0xBD0792) != 0; }   // DAT_10BCA098

    void RestoreAlphas()   // FUN_1019c3d0
    {
        if (InWorld())
            for (auto& h : g_hits)
                *reinterpret_cast<float*>(h.first + 0x17C) = h.second;
        g_hits.clear();
    }

    int __cdecl RecordHit(uint32_t, uint32_t, uint8_t* model)   // FUN_1019c2f0, replaces 0x7A25F0
    {
        const float alpha = *reinterpret_cast<const float*>(model + 0x17C);
        g_hits.emplace_back(model, alpha);
        int bits;
        memcpy(&bits, &alpha, 4);
        return bits;
    }

    // ---- hooks ---------------------------------------------------------------------------------------------
    typedef int(__cdecl* Fn401480_t)(uint32_t, uint32_t, uint32_t, uint32_t);
    Fn401480_t g_401480 = nullptr;
    int __cdecl Detour401480(uint32_t a, uint32_t b, uint32_t c, uint32_t d)   // sub_1019c970
    {
        RestoreAlphas();
        g_401480(a, b, c, d);
        return 1;
    }

    typedef int(__fastcall* This3_t)(void*, void*, uint32_t, uint32_t, uint32_t);
    This3_t g_5FF950 = nullptr, g_5FFA60 = nullptr;
    int __fastcall Detour5FF950(void* s, void* e, uint32_t a, uint32_t b, uint32_t c) { return g_5FF950(s, e, a, b, c); }   // sub_10198ff0
    int __fastcall Detour5FFA60(void* s, void* e, uint32_t a, uint32_t b, uint32_t c) { return g_5FFA60(s, e, a, b, c); }   // sub_10199020

    typedef int(__cdecl* Fn5241B0_t)(uint32_t, uint32_t, uint32_t);
    Fn5241B0_t g_5241B0 = nullptr;
    int __cdecl Detour5241B0(uint32_t lo, uint32_t hi, uint32_t c)   // sub_101993b0
    {
        const uint32_t* target = reinterpret_cast<const uint32_t*>(0xBD07B0);   // DAT_10BCA094
        if (ActionCam() && Int("cameraTargetFocusEnemyEnable") != 0 && lo == target[0] && hi == target[1])
            if (uint8_t* player = ActivePlayer())
                reinterpret_cast<void(__thiscall*)(void*, int, int)>(0x7272C0)(player, 0, 1);
        return g_5241B0(lo, hi, c);
    }

    typedef float*(__fastcall* Fn601D60_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn601D60_t g_601D60 = nullptr;
    float* __fastcall Detour601D60(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d)   // sub_101992c0
    {
        float* r = g_601D60(self, edx, a, b, c, d);
        if (ActionCam())
        {
            const uint8_t* cam = static_cast<uint8_t*>(self);
            if (uint8_t* unit = ObjectByGuid(*reinterpret_cast<const uint32_t*>(cam + 0x88), *reinterpret_cast<const uint32_t*>(cam + 0x8C), 8))
            {
                const float angleVar = Float("cameraActionAngle");
                const double angle = static_cast<double>(static_cast<long double>(Facing(unit)) + angleVar);
                r[0] = static_cast<float>(cos(angle)) * Float("cameraActionDist") + r[0];
                r[1] = static_cast<float>(sin(angle)) * Float("cameraActionDist") + r[1];
                r[2] = Float("cameraActionZ") + r[2];
            }
        }
        return r;
    }

    typedef uint32_t(__fastcall* Fn605D60_t)(void*, void*, float*, float*, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn605D60_t g_605D60 = nullptr;
    uint32_t __fastcall Detour605D60(void* self, void* edx, float* pos, float* dist, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)   // sub_10199050
    {
        uint8_t* cam = static_cast<uint8_t*>(self);
        RestoreAlphas();
        if (Int("cameraM2Collision") == 0)
            if (uint8_t* player = ActivePlayer())
            {
                float from[3] = {pos[0], pos[1], pos[2]};
                float zero[3] = {0.0f, 0.0f, 0.0f};
                float one = 1.0f;
                uint8_t scratch[0x780];
                const float height = (*reinterpret_cast<float(__thiscall**)(void*)>(*reinterpret_cast<uint8_t**>(player) + 0x3C))(player);
                from[2] = static_cast<float>(height * 1.5L + from[2]);
                if (reinterpret_cast<bool(__cdecl*)(void*, float*, float*, float*, uint32_t, void*)>(0x77F310)(cam + 8, from, zero, &one, 0x100171, scratch))
                {
                    const float alpha = Float("cameraM2CollisionAlpha");
                    for (auto& h : g_hits)
                    {
                        uint32_t& flags = *reinterpret_cast<uint32_t*>(h.first + 0x10);
                        const bool shared = *reinterpret_cast<const uint32_t*>(h.first + 0x48) != 0;
                        flags |= shared ? 0x80u : 8u;
                        flags |= shared ? 0x20000u : 0x10000u;
                        *reinterpret_cast<float*>(h.first + 0x17C) = alpha;
                    }
                }
            }
        const uint32_t r = g_605D60(self, edx, pos, dist, a3, a4, a5, a6);
        if ((r & 0x10000) && ActionCam())
            if (uint8_t* unit = ObjectByGuid(*reinterpret_cast<const uint32_t*>(cam + 0x88), *reinterpret_cast<const uint32_t*>(cam + 0x8C), 8))
            {
                const float angleVar = Float("cameraActionAngle");
                const float facing = static_cast<float>(Facing(unit) + static_cast<long double>(angleVar));
                const float d = Float("cameraActionDist");
                float shift = d - (dist[0] - 0.11111111f);
                if (shift > d)
                    shift = d;
                shift = 0.0f > shift ? 0.0f : shift;
                pos[0] -= static_cast<float>(cos(static_cast<double>(facing))) * shift;
                pos[1] -= static_cast<float>(sin(static_cast<double>(facing))) * shift;
            }
        return r;
    }

    // ---- lifecycle callbacks ---------------------------------------------------------------------------------
    void AfterTargetChange()   // FUN_1019c420
    {
        uint32_t guid[2] = {*reinterpret_cast<const uint32_t*>(0xBD07B0), *reinterpret_cast<const uint32_t*>(0xBD07B4)};
        uint8_t* player = ActivePlayer();
        if (!player)
            return;
        uint8_t* unit = ObjectByGuid(guid[0], guid[1], 8);
        if (!unit || *reinterpret_cast<const uint32_t*>(unit + 0x14) == 4)
            return;
        bool focus = false;
        if (reinterpret_cast<bool(__thiscall*)(void*, void*)>(0x729A70)(player, unit) && ActionCam() &&
            Int("cameraTargetFocusEnemyEnable") != 0)   // FUN_1019c940
            focus = true;
        else if (reinterpret_cast<int(__thiscall*)(void*, void*)>(0x7251C0)(player, unit) > 3 && ActionCam() &&
                 Int("cameraTargetFocusInteractEnable") != 0)
            focus = true;
        if (focus)
            reinterpret_cast<void(__thiscall*)(void*, uint32_t*, int)>(0x72B4A0)(player, guid, 1);
    }

    // sub_10198e60: `mov ecx, [esp+4]; call 0x4D43C0` -- a __thiscall on the list's argument (the object's
    // +0x144). Transcribed as a no-argument __cdecl until 2026-09-27, which left ECX to chance and crashed
    // on release-to-graveyard (0x4D43C3 reading [null]).
    void Before6ECF80(uint32_t object)
    {
        if (reinterpret_cast<bool(__thiscall*)(void*)>(0x4D43C0)(reinterpret_cast<void*>(object)))
            RestoreAlphas();
    }

    void Init()
    {
        AscRuntime::OnAfterTargetChange(&AfterTargetChange);
        AscRuntime::OnBefore6ECF80(&Before6ECF80);
        g_601D60 = reinterpret_cast<Fn601D60_t>(AscRuntime::Detour(0x601D60, 6, reinterpret_cast<void*>(&Detour601D60)));
        g_605D60 = reinterpret_cast<Fn605D60_t>(AscRuntime::Detour(0x605D60, 6, reinterpret_cast<void*>(&Detour605D60)));
        g_5241B0 = reinterpret_cast<Fn5241B0_t>(AscRuntime::Detour(0x5241B0, 6, reinterpret_cast<void*>(&Detour5241B0)));
        AscRuntime::ReplaceFunction(0x7A25F0, reinterpret_cast<void*>(&RecordHit));
        g_5FF950 = reinterpret_cast<This3_t>(AscRuntime::Detour(0x5FF950, 6, reinterpret_cast<void*>(&Detour5FF950)));
        g_5FFA60 = reinterpret_cast<This3_t>(AscRuntime::Detour(0x5FFA60, 6, reinterpret_cast<void*>(&Detour5FFA60)));
        g_401480 = reinterpret_cast<Fn401480_t>(AscRuntime::Detour(0x401480, 5, reinterpret_cast<void*>(&Detour401480)));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}

namespace AscCamera
{
    // FUN_1019c510's FUN_10114540 queue entries, in its order.
    const AscClientOptions::CVarSpec kCVars[] = {
        {"ActionCam", "0", 1, 4, reinterpret_cast<void*>(&OnActionCam)},
        {"cameraTargetFocusInteractEnable", "0", 1, 4, nullptr},
        {"cameraTargetFocusEnemyEnable", "0", 1, 4, reinterpret_cast<void*>(&OnEnemyFocus)},
        {"cameraActionDist", "1.25", 1, 4, nullptr},
        {"cameraActionAngle", "4.0", 1, 4, nullptr},
        {"cameraActionZ", "0.05", 1, 4, nullptr},
        {"cameraActionHeadBobs", "0", 1, 4, reinterpret_cast<void*>(&OnHeadBobs)},
        {"cameraTargetFocusTurnSpeed", "3.15", 1, 4, reinterpret_cast<void*>(&OnTurnSpeed)},
        {"cameraM2Collision", "1", 0x21, 4, reinterpret_cast<void*>(&OnM2Collision)},
        {"cameraM2CollisionAlpha", "0.5", 0x21, 4, nullptr},
        {"cameraZoomFactor", "1.0", 1, 4, nullptr},
        {"autoTarget", "1", 1, 4, reinterpret_cast<void*>(&OnAutoTarget)},
    };
    const AscClientOptions::CVarSpec* CVars(size_t& count)
    {
        count = sizeof(kCVars) / sizeof(kCVars[0]);
        return kCVars;
    }
}
