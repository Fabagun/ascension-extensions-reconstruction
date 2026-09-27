// Raid video settings (installer FUN_102dc960): a second set of eleven graphics CVars applied inside
// raids and battlegrounds (Map.dbc +8 == 2 / 3) while useRaidVideoSettings is on.
//
//   CVars (FUN_101145c0 -> glue vector, CVar.cpp): useRaidVideoSettings "0" and the eleven <name>_raid,
//     all flags 1, category 1, callback FUN_102dcdb0.
//   Callback (FUN_102dcdb0): only with useRaidVideoSettings on and the player in a raid / battleground.
//     For a <name>_raid CVar the new value goes straight to <name>; for useRaidVideoSettings itself every
//     _raid value is applied.
//   Every other 0x6DF050 call (FUN_102dd130): with useRaidVideoSettings on, in a raid / battleground the
//     flag 0x10BE38E0 is set and each base CVar's current string POINTER (+0x28) is kept (0x10BE38B4..DC)
//     before the _raid value is set; elsewhere the flag is cleared and the restore below runs.
//   Restore (FUN_102dcb90; also every glue screen): each base CVar set back from its kept pointer.
//   0x766640 (the Config.wtf line writer, __cdecl(name, value, c); FUN_102dc5f0, chained on the same
//     address as AscCVarHooks): with the flag set, a base CVar's line is written with the kept value.
// The base CVars are reached through the client's own CVar pointers (0xCD8580 ...) for the first eight
// and by name for fogDistance / entityShadows / animateClouds, as in the original.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Client/CVar.hpp>
#include <cstdint>
#include <cstring>
#include <string>

namespace
{
    struct Setting
    {
        const char* base;
        uint32_t clientPtr;       // the client's CVar* global, 0 = looked up by name
        const char* raidName;
        const char* raidDefault;
    };
    const Setting kSettings[11] = {
        {"farclip", 0xCD8580, "farclip_raid", "507"},
        {"particleDensity", 0xCD8594, "particleDensity_raid", "0.4"},
        {"extShadowQuality", 0xCD85DC, "extShadowQuality_raid", "0"},
        {"environmentDetail", 0xCD85D4, "environmentDetail_raid", "0.75"},
        {"groundEffectDensity", 0xCD85BC, "groundEffectDensity_raid", "24"},
        {"groundEffectDist", 0xCD85C0, "groundEffectDist_raid", "80.0"},
        {"textureFilteringMode", 0xB2FA00, "textureFilteringMode_raid", "1"},
        {"projectedTextures", 0xCD85E0, "projectedTextures_raid", "1"},
        {"fogDistance", 0, "fogDistance_raid", "548"},
        {"entityShadows", 0, "entityShadows_raid", "0"},
        {"animateClouds", 0, "animateClouds_raid", "0"},
    };
    const char* g_kept[11] = {};   // 0x10BE38B4 .. 0x10BE38DC
    bool g_inRaid = false;         // 0x10BE38E0

    const uint8_t* Base(size_t i)
    {
        if (kSettings[i].clientPtr)
            return *reinterpret_cast<const uint8_t* const*>(kSettings[i].clientPtr);   // FUN_10114490
        return reinterpret_cast<const uint8_t*>(CVar::Lookup(kSettings[i].base));
    }
    const char* Str(const uint8_t* cvar) { return *reinterpret_cast<const char* const*>(cvar + 0x28); }

    // FUN_10114c40: the length assert, then the client's CVar::Set 0x7668C0(value, 1, 0, 0, 0).
    void Set(const uint8_t* cvar, const char* value)
    {
        const char* key = *reinterpret_cast<const char* const*>(cvar + 0x14);
        if (strlen(value) + strlen(key) > 0xF9)
            return;
        reinterpret_cast<void(__thiscall*)(const void*, const char*, int, int, int, int)>(0x7668C0)(cvar, value, 1, 0, 0, 0);
    }

    bool Enabled()
    {
        const CVar* use = CVar::Lookup("useRaidVideoSettings");   // 0x10BE3884
        return !use || *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(use) + 0x30) != 0;
    }
    bool InRaidMap()
    {
        const uint8_t* map = AscClientDbc::Row(0xAD4160, *reinterpret_cast<const uint32_t*>(0xBD088C));
        if (!map)
            return false;
        const uint32_t type = *reinterpret_cast<const uint32_t*>(map + 8);
        return type == 2 || type == 3;
    }
    // A missing map row ends the tick without touching the flag.
    bool HaveMap() { return AscClientDbc::Row(0xAD4160, *reinterpret_cast<const uint32_t*>(0xBD088C)) != nullptr; }

    void Restore()   // FUN_102dcb90
    {
        if (!Enabled())
            return;
        for (size_t i = 0; i < 11; ++i)
            if (g_kept[i])
                if (const uint8_t* base = Base(i))
                    Set(base, g_kept[i]);
    }

    void Tick()   // FUN_102dd130
    {
        if (!Enabled() || !HaveMap())
            return;
        if (!InRaidMap())
        {
            g_inRaid = false;
            Restore();
            return;
        }
        g_inRaid = true;
        for (size_t i = 0; i < 11; ++i)
        {
            const uint8_t* base = Base(i);
            if (!base)
                continue;
            g_kept[i] = Str(base);
            Set(base, Str(reinterpret_cast<const uint8_t*>(CVar::Lookup(kSettings[i].raidName))));
        }
    }

    int __cdecl OnChanged(void* cvar, const char*, const char* value, void*)   // FUN_102dcdb0
    {
        if (!Enabled() || !HaveMap() || !InRaidMap())
            return 1;
        std::string name = *reinterpret_cast<const char* const*>(reinterpret_cast<const uint8_t*>(cvar) + 0x14);
        const size_t at = name.size() >= 5 ? name.find("_raid") : std::string::npos;
        if (at == std::string::npos)
        {
            for (size_t i = 0; i < 11; ++i)
                if (const uint8_t* base = Base(i))
                    Set(base, Str(reinterpret_cast<const uint8_t*>(CVar::Lookup(kSettings[i].raidName))));
            return 1;
        }
        name.erase(at, 5);
        if (const CVar* base = CVar::Lookup(name.c_str()))
            Set(reinterpret_cast<const uint8_t*>(base), value);
        return 1;
    }

    typedef int(__cdecl* Write_t)(const char*, const char*, uint32_t);
    Write_t g_766640 = nullptr;
    int __cdecl Hook766640(const char* name, const char* value, uint32_t c)   // FUN_102dc5f0
    {
        if (g_inRaid)
            for (size_t i = 0; i < 11; ++i)
                if (strcmp(name, kSettings[i].base) == 0)
                    return g_766640(name, g_kept[i], c);
        return g_766640(name, value, c);
    }

    void Init()   // FUN_102dc960 (the CVars are registered from CVar.cpp's glue vector)
    {
        AscRuntime::OnAlternate6DF050(&Tick);
        AscRuntime::OnGlueScreen(&Restore);
        g_766640 = reinterpret_cast<Write_t>(AscRuntime::Detour(0x766640, 9, reinterpret_cast<void*>(&Hook766640)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

void* AscRaidVideo_Callback() { return reinterpret_cast<void*>(&OnChanged); }
const char* AscRaidVideo_Name(size_t i, const char** defaultValue)
{
    if (i >= 11)
        return nullptr;
    *defaultValue = kSettings[i].raidDefault;
    return kSettings[i].raidName;
}
