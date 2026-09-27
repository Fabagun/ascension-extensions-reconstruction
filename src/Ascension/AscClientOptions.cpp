// The attach init's (FUN_10a66100) option CVars and the hooks that read them (installer FUN_10a66d40
// and the attach init itself). Callback byte strings come from running each original callback under
// tools/emulate_dll_fn.py, checked against its decompile.
//
//   0x56C500 -> sub_10a429a0   chat-bubble height over nameplates (float 0x10BCCB9C, chatBubblesNameplate)
//   0x744150 -> sub_10a44df0   selection circle turned with the unit (ObjectSelectionCircleTexture 2/3)
//   0x720330 -> sub_10a462f0   selection circle radius from both units' sizes (ObjectSelectionCircleMode)
//   0x71F990 -> FUN_10a468f0   "AbsorbGetHit" sound on hit type 10 (absorbSoundOnSpellHit)
//   0x722340 -> sub_10a468b0   melee combat-log lines off (CombatLogMelee)
//   0x606330 -> sub_10a42930   camera shake off (cameraShake)
//   0x5873E0 -> FUN_10a44c90   TRADE_REQUEST instead of the stock trade prompt (autoAcceptTrades 0)
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CVar.hpp>
#include <Windows.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace AscScript;

namespace
{
    void Patch(uint32_t addr, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(addr), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    }
    template <typename T> void PatchValue(uint32_t addr, T v) { Patch(addr, &v, sizeof(T)); }
    template <size_t N> void PatchBytes(uint32_t addr, const uint8_t (&b)[N]) { Patch(addr, b, N); }
    bool IsOne(const char* v) { return strcmp(v, "1") == 0; }

    const uint8_t* Var(const char* name) { return reinterpret_cast<const uint8_t*>(CVar::Lookup(name)); }
    float VarF(const uint8_t* v) { return *reinterpret_cast<const float*>(v + 0x2C); }
    int32_t VarI(const uint8_t* v) { return *reinterpret_cast<const int32_t*>(v + 0x30); }

    // ---- chat bubbles over nameplates --------------------------------------------------------------
    float g_bubbleHeight = 0.7f;        // DAT_10bccb9c, read by the client through 0x56C76B

    // FUN_10a48c10: "1" lets bubbles follow nameplates (0x720020 NOPed, 0x725737 jmp); the height read at
    // 0x56C76B always points at our float.
    int __cdecl OnChatBubblesNameplate(void*, const char*, const char* value, void*)
    {
        static const uint8_t kJne[] = {0x0F, 0x85, 0x01, 0x01, 0x00, 0x00}, kNop[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const bool on = IsOne(value);
        PatchBytes(0x720020, on ? kNop : kJne);
        PatchValue<float*>(0x56C76B, &g_bubbleHeight);
        PatchValue<uint8_t>(0x725737, on ? 0xEB : 0x74);
        return 1;
    }

    // sub_10a429a0 over 0x56C500 (__thiscall(this, a, unit), ret 8): the bubble height before the original.
    // The player's own bubble with a personal nameplate: 1.25 at position 1, else 0.7. Anyone else: the
    // nameplate offset + 1.0 while the unit has a plate, else 0.7. Option off: 0.7.
    typedef int(__fastcall* Bubble_t)(void*, void*, uint32_t, uint8_t*);
    Bubble_t g_bubble = nullptr;
    int __fastcall BubbleDetour(void* ecx, void* edx, uint32_t a, uint8_t* unit)
    {
        const uint8_t* cvar = Var("chatBubblesNameplate");
        if (cvar && VarI(cvar) == 1)
        {
            const uint8_t* personal = Var("nameplateShowPersonal");
            if (reinterpret_cast<bool(__thiscall*)(void*)>(0x4CEE50)(unit) && personal && VarI(personal) == 1)
            {
                const uint8_t* pos = Var("nameplatePersonalPosition");
                g_bubbleHeight = (pos && VarI(pos) == 1) ? 1.25f : 0.7f;
            }
            else
            {
                const uint8_t* offset = Var("nameplateVerticalOffset");
                const float o = offset ? VarF(offset) : 0.7f;
                g_bubbleHeight = *reinterpret_cast<const uint32_t*>(unit + 0xC38) != 0 ? o + 1.0f : 0.7f;
            }
        }
        else
            g_bubbleHeight = 0.7f;
        return g_bubble(ecx, edx, a, unit);
    }

    // ---- selection circle ----------------------------------------------------------------------------
    // FUN_10a46fa0: the circle texture at 0x746147 ("1" no fill, "2" arrow, "3" arrow without fill, else
    // the stock texture), then reloaded through 0x7460C0.
    const char* g_circleTexture = "Textures\\UnitSelectTexture.blp";   // PTR_DAT_10bccc20
    int __cdecl OnCircleTexture(void*, const char*, const char* value, void*)
    {
        if (IsOne(value))
            g_circleTexture = "Textures\\UnitSelectTextureNoFill.blp";
        else if (strcmp(value, "2") == 0)
            g_circleTexture = "Textures\\UnitSelectTextureArrow.blp";
        else if (strcmp(value, "3") == 0)
            g_circleTexture = "Textures\\UnitSelectTextureArrowNoFill.blp";
        else
            g_circleTexture = "Textures\\UnitSelectTexture.blp";
        PatchValue<const char*>(0x746147, g_circleTexture);
        reinterpret_cast<void(__cdecl*)()>(0x7460C0)();
        return 1;
    }

    // FUN_102bd3f0: m = Rz(angle) * m (row-major 4x4, FUN_102bcea0).
    void Rotate(float* m, float angle)
    {
        const float c = static_cast<float>(cos(static_cast<double>(angle)));
        const float s = static_cast<float>(sin(static_cast<double>(angle)));
        const float r[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float out[16];
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = r[i * 4 + 1] * m[4 + j] + r[i * 4] * m[j] + m[8 + j] * r[i * 4 + 2] + m[12 + j] * r[i * 4 + 3];
        memcpy(m, out, sizeof(out));
    }

    // sub_10a44df0 over 0x744150 (__thiscall(unit, matrix, b), ret 8): with an arrow texture (2 or 3) the
    // matrix is turned by the unit's facing (vtable +0x94) and a half turn, in place of the original.
    typedef int(__fastcall* Circle_t)(void*, void*, float*, uint32_t);
    Circle_t g_circle = nullptr;
    int __fastcall CircleDetour(void* ecx, void* edx, float* m, uint32_t b)
    {
        uint8_t* unit = static_cast<uint8_t*>(ecx);
        const uint8_t* cvar = Var("ObjectSelectionCircleTexture");
        if (*reinterpret_cast<const uint32_t*>(unit + 8) != 0 && cvar && (VarI(cvar) == 2 || VarI(cvar) == 3))
        {
            void** vt = *reinterpret_cast<void***>(unit);
            const float facing = reinterpret_cast<float(__thiscall*)(void*)>(vt[0x94 / 4])(unit);
            Rotate(m, -facing);
            Rotate(m, 3.14159274f);
            return 0;
        }
        return g_circle(ecx, edx, m, b);
    }

    // sub_10a462f0 over 0x720330 (__thiscall(unit)): unless the option is 0, the circle radius (+0xB0C) is
    // both units' sizes (descriptor +0x108) + 4/3, at least 5, in place of the original.
    typedef void(__fastcall* Radius_t)(void*, void*);
    Radius_t g_radius = nullptr;
    void __fastcall RadiusDetour(void* ecx, void* edx)
    {
        const uint8_t* cvar = Var("ObjectSelectionCircleMode");
        if (cvar && VarI(cvar) == 0)
        {
            g_radius(ecx, edx);
            return;
        }
        uint8_t* unit = static_cast<uint8_t*>(ecx);
        float r = 5.0f;
        if (uint8_t* player = ActivePlayer())
        {
            const float sum = *reinterpret_cast<const float*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0x108) +
                              *reinterpret_cast<const float*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0x108) + 1.33333337f;
            r = sum > 5.0f ? sum : 5.0f;
        }
        *reinterpret_cast<float*>(unit + 0xB0C) = r;
    }
    // sub_10a3ab20: every unit's radius recomputed through the (hooked) 0x720330.
    int __cdecl RecomputeRadius(uint32_t lo, uint32_t hi, void*)
    {
        if (void* unit = ObjectPtr((static_cast<uint64_t>(hi) << 32) | lo, 8))
            reinterpret_cast<void(__thiscall*)(void*)>(0x720330)(unit);
        return 1;
    }
    // sub_10a46f60: in world, the value is stored first so the recompute sees it.
    int __cdecl OnCircleMode(uint8_t* cvar, const char*, const char* value, void*)
    {
        if (*reinterpret_cast<const uint8_t*>(0xBD0792) != 0)
        {
            *reinterpret_cast<int32_t*>(cvar + 0x30) = atoi(value);
            reinterpret_cast<void(__cdecl*)(int(__cdecl*)(uint32_t, uint32_t, void*), void*)>(0x4D4B30)(&RecomputeRadius, nullptr);
        }
        return 1;
    }

    // ---- absorb sound (FUN_10a468f0 over 0x71F990, __thiscall(unit, a, type, c, d), ret 0x10) ----------
    typedef int(__fastcall* Hit_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
    Hit_t g_hit = nullptr;
    int __fastcall HitDetour(void* ecx, void* edx, uint32_t a, uint32_t type, uint32_t c, uint32_t d)
    {
        const int r = g_hit(ecx, edx, a, type, c, d);
        const uint8_t* cvar = Var("absorbSoundOnSpellHit");
        if (type == 10 && cvar && VarI(cvar) == 1)
        {
            float pos[3];
            void** vt = *static_cast<void***>(ecx);
            reinterpret_cast<void(__thiscall*)(void*, float*)>(vt[0x2C / 4])(ecx, pos);
            pos[2] = static_cast<float>(static_cast<double>(pos[2]) + 2.0);
            reinterpret_cast<void(__cdecl*)(const char*, const float*, int, int)>(0x4C74A0)("(DONOTRENAME)AbsorbGetHit", pos, 0, 0);
        }
        return r;
    }

    // ---- melee combat log (sub_10a468b0 over 0x722340, __thiscall + 6 arguments, ret 0x18) --------------
    typedef int(__fastcall* Log_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Log_t g_log = nullptr;
    int __fastcall LogDetour(void* ecx, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f)
    {
        const uint8_t* cvar = Var("CombatLogMelee");
        if (c != 0 || (cvar && VarI(cvar) != 0))
            return g_log(ecx, edx, a, b, c, d, e, f);
        return 0;
    }

    // ---- camera shake (sub_10a42930 over 0x606330, __thiscall + 8 arguments, ret 0x20) -------------------
    typedef int(__fastcall* Shake_t)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Shake_t g_shake = nullptr;
    int __fastcall ShakeDetour(void* ecx, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h)
    {
        const uint8_t* cvar = Var("cameraShake");
        if (!cvar || VarI(cvar) != 0)
            return g_shake(ecx, edx, a, b, c, d, e, f, g, h);
        return 0;
    }

    // ---- trade prompt (FUN_10a44c90 over 0x5873E0, __cdecl(state, b, c, d)) ------------------------------
    // State 1 with autoAcceptTrades off: TRADE_REQUEST(name of the trader at 0xCA0FE8, vtable +0xD8)
    // replaces the stock prompt; no trader leaves it to the original.
    typedef int(__cdecl* Trade_t)(int32_t, uint32_t, uint32_t, uint32_t);
    Trade_t g_trade = nullptr;
    int __cdecl TradeDetour(int32_t state, uint32_t b, uint32_t c, uint32_t d)
    {
        const uint8_t* cvar = Var("autoAcceptTrades");
        if (state == 1 && cvar && VarI(cvar) != 1)
            if (void* trader = ObjectPtr(*reinterpret_cast<const uint64_t*>(0xCA0FE8), 0x10))
            {
                void** vt = *static_cast<void***>(trader);
                const char* name = reinterpret_cast<const char*(__thiscall*)(void*)>(vt[0xD8 / 4])(trader);
                AscRuntime::Signal("TRADE_REQUEST", "%s", name);
                return 0;
            }
        return g_trade(state, b, c, d);
    }

    // ---- callback-only options -----------------------------------------------------------------------
    // FUN_10a47b70: combat-text sizes (11 styles at 0xAF4748, +0x10/+0x14 per 0x1C, and 0xAF487C) are the
    // stock values times the scale clamped to 0.25..3.
    const float kTextSize[11][2] = {
        {0.018333f, 0.018333f}, {0.018333f, 0.018333f}, {0.0f, 0.0275f}, {0.018333f, 0.018333f}, {0.018333f, 0.018333f},
        {0.018333f, 0.018333f}, {0.018333f, 0.018333f}, {0.0f, 0.0275f}, {0.0275f, 0.0275f}, {0.0275f, 0.0275f}, {0.018333f, 0.018333f}};
    const float kTextOther[11] = {0.002f, 0.002f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f};
    int __cdecl OnWorldTextScale(void*, const char*, const char* value, void*)
    {
        const float x = static_cast<float>(atof(value));
        const float lo = 0.25f <= x ? x : 0.25f;
        const float s = x <= 3.0f ? lo : 3.0f;
        for (uint32_t i = 0; i < 11; ++i)
        {
            PatchValue<float>(0xAF4748 + 0x10 + i * 0x1C, s * kTextSize[i][0]);
            PatchValue<float>(0xAF4748 + 0x14 + i * 0x1C, s * kTextSize[i][1]);
            PatchValue<float>(0xAF487C + i * 4, s * kTextOther[i]);
        }
        return 1;
    }
    // FUN_10a47c30: "1" casts through assist targets. Quirk kept: other values write 0x89 at 0x80C91E where
    // the stock byte is 0x80.
    int __cdecl OnAutoAssistCast(void*, const char*, const char* value, void*)
    {
        static const uint8_t kA[] = {0x74, 0x6F}, kB[] = {0x74, 0x07}, kC[] = {0x0D, 0x89, 0x00, 0x00, 0x00};
        static const uint8_t kNop2[] = {0x90, 0x90}, kNop5[] = {0x90, 0x90, 0x90, 0x90, 0x90};
        const bool on = IsOne(value);
        PatchBytes(0x80C8C1, on ? kNop2 : kA);
        PatchBytes(0x80D7C1, on ? kNop2 : kB);
        PatchBytes(0x80C91D, on ? kNop5 : kC);
        return 1;
    }
    // FUN_10a49850: "1" keeps the stock "??" level for dangerous enemies.
    int __cdecl OnHideDangerousEnemyLevel(void*, const char*, const char* value, void*)
    {
        static const uint8_t kA[] = {0x7E, 0x0B}, kB[] = {0x0F, 0x8E, 0xDD, 0x00, 0x00, 0x00};
        static const uint8_t kNop2[] = {0x90, 0x90}, kNop6[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        const bool on = IsOne(value);
        PatchBytes(0x60FAB2, on ? kA : kNop2);
        PatchBytes(0x621766, on ? kB : kNop6);
        return 1;
    }
    // FUN_10a4a200: frame 4 of the client's list (0x6F75B0) gets the scale clamped to 0..5 at +0x10.
    int __cdecl OnLootArtScale(void*, const char*, const char* value, void*)
    {
        const float x = static_cast<float>(atof(value));
        const float lo = 0.0f <= x ? x : 0.0f;
        const float s = x <= 5.0f ? lo : 5.0f;
        if (uint8_t* frame = reinterpret_cast<uint8_t*(__cdecl*)(int)>(0x6F75B0)(4))
            *reinterpret_cast<float*>(frame + 0x10) = s;
        return 1;
    }
    int __cdecl OnTargetName(void*, const char*, const char* value, void*)   // FUN_10a4a270
    {
        static const uint8_t kJe[] = {0x74, 0x42}, kNop[] = {0x90, 0x90};
        PatchBytes(0x729D3D, IsOne(value) ? kJe : kNop);
        return 1;
    }
    int __cdecl OnSoundEnableFall(void*, const char*, const char* value, void*)   // FUN_10a47780
    {
        PatchValue<uint8_t>(0x73D46A, IsOne(value) ? 0x7E : 0xEB);
        return 1;
    }

    template <class F> void* Cb(F f) { return reinterpret_cast<void*>(f); }
    // In the original's registration order (FUN_10a66100's call sites 0x10A67607 .. 0x10A6B338).
    const AscClientOptions::CVarSpec kCVars[] = {
        {"ObjectSelectionCircleTexture", "2", 1, 4, Cb(&OnCircleTexture)},
        {"ObjectSelectionCircleMode", "0", 1, 4, Cb(&OnCircleMode)},
        {"flashWindow", "0", 1, 4, nullptr},             // DAT_10D3D72C
        {"absorbSoundOnSpellHit", "0", 1, 3, nullptr},
        {"CombatLogMelee", "1", 0x21, 3, nullptr},
        {"chatBubblesNameplate", "1", 1, 4, Cb(&OnChatBubblesNameplate)},
        {"holdToCast", "0", 1, 3, nullptr},              // DAT_10D3D9FC
        {"hideDangerousEnemyLevel", "1", 0x21, 4, Cb(&OnHideDangerousEnemyLevel)},
        {"lootArtScale", "1.0", 1, 4, Cb(&OnLootArtScale)},
        {"targetName", "1", 1, 4, Cb(&OnTargetName)},
        {"WorldTextScale", "1.0", 1, 4, Cb(&OnWorldTextScale)},
        {"autoAssistCast", "0", 1, 3, Cb(&OnAutoAssistCast)},
        {"cameraShake", "1", 1, 4, nullptr},
        {"UnitNameGO", "0", 1, 4, nullptr},              // DAT_10D3DA24 (AscAttachFixes)
        {"UnitNameInteractiveNPC", "0", 1, 4, nullptr},  // DAT_10D3DA2C
        {"UnitNameQuestNPC", "1", 1, 4, nullptr},        // DAT_10D3DC30
        {"UnitNameHostileNPC", "1", 1, 4, nullptr},      // DAT_10D3DC34
        {"UnitNameAllNPC", "0", 1, 4, nullptr},          // DAT_10D3DC38
        {"Sound_EnableFall", "1", 1, 7, Cb(&OnSoundEnableFall)},
        {"autoAcceptTrades", "1", 1, 4, nullptr},
    };

    // The client's world CVar registration (0x51D9B0, push ebp / mov ebp,esp / sub esp,0x80: nine bytes);
    // the original drains its queue after it (0x10114090, hook object 0x10BC9900), as here.
    typedef void(__cdecl* WorldCVars_t)();
    WorldCVars_t g_worldCVars = nullptr;
    void __cdecl WorldCVarsDetour()
    {
        g_worldCVars();
        CVar::RegisterWorldQueue();
    }

    // FUN_10a6b890 (after the glue natives, list 0x10BE284C): the client's own UI-scale registration 0x5238A0
    // (uiScale, default patched to "0.85" by AscAttachPatches, and useUiScale "0") runs at glue too.
    void RegisterUiScaleCVars() { reinterpret_cast<void(__cdecl*)()>(0x5238A0)(); }

    void Init()
    {
        AscBindings::OnGlueRegistered(&RegisterUiScaleCVars);
        typedef void* V;
        g_worldCVars = reinterpret_cast<WorldCVars_t>(AscRuntime::Detour(0x51D9B0, 9, reinterpret_cast<V>(&WorldCVarsDetour)));
        g_bubble = reinterpret_cast<Bubble_t>(AscRuntime::Detour(0x56C500, 6, reinterpret_cast<V>(&BubbleDetour)));
        g_circle = reinterpret_cast<Circle_t>(AscRuntime::Detour(0x744150, 6, reinterpret_cast<V>(&CircleDetour)));
        g_radius = reinterpret_cast<Radius_t>(AscRuntime::Detour(0x720330, 6, reinterpret_cast<V>(&RadiusDetour)));
        g_hit = reinterpret_cast<Hit_t>(AscRuntime::Detour(0x71F990, 7, reinterpret_cast<V>(&HitDetour)));
        g_log = reinterpret_cast<Log_t>(AscRuntime::Detour(0x722340, 6, reinterpret_cast<V>(&LogDetour)));
        g_shake = reinterpret_cast<Shake_t>(AscRuntime::Detour(0x606330, 5, reinterpret_cast<V>(&ShakeDetour)));
        g_trade = reinterpret_cast<Trade_t>(AscRuntime::Detour(0x5873E0, 6, reinterpret_cast<V>(&TradeDetour)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

const AscClientOptions::CVarSpec* AscClientOptions::CVars(size_t& count)
{
    count = sizeof(kCVars) / sizeof(kCVars[0]);
    return kCVars;
}

namespace
{
    std::vector<AscClientOptions::CVarSpec>& Queued() { static std::vector<AscClientOptions::CVarSpec> v; return v; }
}

void AscClientOptions::QueueWorldCVar(const CVarSpec& spec) { Queued().push_back(spec); }

const AscClientOptions::CVarSpec* AscClientOptions::QueuedWorldCVars(size_t& count)
{
    count = Queued().size();
    return Queued().data();
}
