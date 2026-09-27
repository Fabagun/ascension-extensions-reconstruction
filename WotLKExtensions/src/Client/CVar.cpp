#include <Client/CVar.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscNamePlates.hpp>
#include <Ascension/AscGraphics.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <Data/Enums.hpp>
#include <Misc/DataContainer.hpp>
#include <Misc/Util.hpp>

#include <PatchConfig.hpp>

// The original's detour on the client's glue CVar registration 0x401B60 (FUN_10114180, installer
// FUN_101141e0; 6 bytes: push ebp / mov ebp,esp / sub esp,0x20). Until 2026-09-27 the scaffold rewrote the
// call at 0x404147 instead.
static int32_t (__cdecl* g_401B60)() = nullptr;

void CVar::ApplyPatches()
{
#if GLUEMGREXTENSION
    g_401B60 = reinterpret_cast<int32_t (__cdecl*)()>(AscRuntime::Detour(0x401B60, 6, reinterpret_cast<void*>(&RegisterGlueCVarsCustom)));

    FillCustomGlueCVarVector();
#endif
}

namespace AscCamera { const AscClientOptions::CVarSpec* CVars(size_t& count); }

void CVar::RegisterWorldQueue()
{
    {
        size_t camCount = 0;   // FUN_1019c510's camera CVars
        const AscClientOptions::CVarSpec* cams = AscCamera::CVars(camCount);
        for (size_t i = 0; i < camCount; ++i)
            Register(cams[i].name, nullptr, cams[i].flags, cams[i].defaultValue, cams[i].callback, cams[i].category, false, 0, false);
    }
    size_t optCount = 0;
    const AscClientOptions::CVarSpec* opts = AscClientOptions::CVars(optCount);
    for (size_t i = 0; i < optCount; ++i)
        Register(opts[i].name, nullptr, opts[i].flags, opts[i].defaultValue, opts[i].callback, opts[i].category, false, 0, false);
    size_t queuedCount = 0;
    const AscClientOptions::CVarSpec* queued = AscClientOptions::QueuedWorldCVars(queuedCount);
    for (size_t i = 0; i < queuedCount; ++i)
        Register(queued[i].name, queued[i].help, queued[i].flags, queued[i].defaultValue, queued[i].callback, queued[i].category, queued[i].a7, 0, false);
}

// (aoeRadiusIndicator* / showQuestUnitCircles are the unit-select module's, queued exactly by AscUnitSelect.cpp.)
// FUN_10114180: the client's glue CVars first, then every queued one (0x10BDD1AC..) as
// 0x767FC0(name, help, flags, default, callback, category, a7, 0, 0).
int32_t CVar::RegisterGlueCVarsCustom()
{
    const int32_t r = g_401B60();

    auto& customCVars = sDC.GetGlueCVarVector();
    for (auto& it : customCVars)
        Register(it.m_name, it.m_description, it.m_size, it.m_defaultValue, it.m_callback, it.m_flags, it.m_a7, 0, false);

    return r;
}

int32_t CVar::Register(const char* name, const char* description, uint32_t size, const char* defaultVal, void* callback, uint32_t flags, bool a7, int32_t a8, bool a9)
{
    return reinterpret_cast<int32_t (__cdecl*)(const char*, const char*, uint32_t, const char*, void*, uint32_t, bool, int32_t, bool)>(0x767FC0)(name, description, size, defaultVal, callback, flags, a7, a8, a9);
}

void CVar::Set(CVar* cVar, int32_t value, bool a3, bool a4, bool a5, bool a6)
{
    reinterpret_cast<void (__thiscall*)(void*, int32_t, bool, bool, bool, bool)>(0x766940)(cVar, value, a3, a4, a5, a6);
}

CVar* CVar::Lookup(const char* name)
{
    return reinterpret_cast<CVar* (__cdecl*)(const char*)>(0x767440)(name);
}

int __cdecl AscCursor_SizePreferred(void*, const char*, const char*, void*);   // AscCursor.cpp
void* AscRaidVideo_Callback();                                                // AscRaidVideo.cpp
const char* AscRaidVideo_Name(size_t i, const char** defaultValue);

static int __cdecl AcceptAnyValue() { return 1; }   // 0x102A0990

void CVar::FillCustomGlueCVarVector()
{
    // CVars Ascension's SharedXML options panels expect (OptionsPanelTemplates.lua:391 "Couldn't find CVar").
    // Stock 12340 does not register these at glue time. Category/size args mirror stock registrations.
    // The raid video settings module (FUN_102dc960 -> FUN_101145c0 x12, AscRaidVideo.cpp): no help, flags 1,
    // category 1, callback FUN_102dcdb0; useRaidVideoSettings "0" then the eleven <name>_raid.
    AddToGlueCVarVector("useRaidVideoSettings", nullptr, 1, "0", AscRaidVideo_Callback(), 1, false, 0, false);
    {
        const char* def = nullptr;
        for (size_t i = 0; const char* name = AscRaidVideo_Name(i, &def); ++i)
            AddToGlueCVarVector(name, nullptr, 1, def, AscRaidVideo_Callback(), 1, false, 0, false);
    }
    // The AddOn-taint module (FUN_10312150 -> FUN_101145c0, storage 0x10BE3F7C): no help, flags 1,
    // default "1", no callback, category 5. At 0 the addon-load hook (AscAddonTaint, 0x5F7E90) refuses
    // addons the server does not list.
    AddToGlueCVarVector("loadUnknownAddOns", nullptr, 1, "1", nullptr, 5, false, 0, false);
    // C_SuperTrack's visited-node list (0x10334FB0 -> FUN_10114540): no help, flags 0x21, default "",
    // an always-accept callback (0x102A0990), category 4.
    AddToGlueCVarVector("VisitedSuperTracks", nullptr, 0x21, "", reinterpret_cast<void*>(&AcceptAnyValue), 4, false, 0, false);
    // C_Manastorm's module init (0x102A4540 -> FUN_10114540, storage 0x10BE3190): no help, flags 1,
    // default "0", the same always-accept callback, category 4. Nothing in the plain code reads it.
    AddToGlueCVarVector("manastormObjectiveIconCulling", nullptr, 1, "0", reinterpret_cast<void*>(&AcceptAnyValue), 4, false, 0, false);
    // The nameplate module's twelve (FUN_102bf920 -> FUN_10114540): no help, flags 0x21, category 4,
    // with the original's callbacks. They replace seven placeholders the panel sweep had registered.
    size_t plateCount = 0;
    const AscNamePlates::CVarSpec* plates = AscNamePlates::CVars(plateCount);
    for (size_t i = 0; i < plateCount; ++i)
        AddToGlueCVarVector(plates[i].name, nullptr, 0x21, plates[i].defaultValue, plates[i].callback, 4, false, 0, false);
    // The render module's nineteen (FUN_10265d20 -> FUN_10114540 / FUN_101145c0): no help, flags 1,
    // category 1, with the original's callbacks. They replace three placeholders registered earlier.
    size_t gfxCount = 0;
    const AscGraphics::CVarSpec* gfx = AscGraphics::CVars(gfxCount);
    for (size_t i = 0; i < gfxCount; ++i)
        AddToGlueCVarVector(gfx[i].name, nullptr, 1, gfx[i].defaultValue, gfx[i].callback, 1, gfx[i].a7, 0, false);
    // The attach init's option CVars (FUN_10a66100 -> FUN_10114540), with the original's flags, defaults,
    // categories and callbacks. They replace the placeholders the sweeps had registered for them.
    // (Registered by RegisterWorldQueue, after 0x51D9B0, like the original.)
    // C_Logger's level gates (FUN_101141e0): no help, flags 1, default "1", no callback, category 5.
    AddToGlueCVarVector("logDebug", nullptr, 1, "1", nullptr, 5, false, 0, false);
    AddToGlueCVarVector("logInfo", nullptr, 1, "1", nullptr, 5, false, 0, false);
    // The cursor module's size (FUN_10a1e950 -> FUN_101145c0, storage 0x10D3C79C): no help, flags 1,
    // default "0", category 1, callback FUN_10a1e320 (AscCursor.cpp).
    AddToGlueCVarVector("cursorSizePreferred", nullptr, 1, "0", reinterpret_cast<void*>(&AscCursor_SizePreferred), 1, false, 0, false);
    // Nothing else. The 2026-09-22/23 "sweeps" (154 names the UI reads, mostly with invented "0" defaults) and
    // tutorialLevel were removed on 2026-09-27: 59 of them do not exist in the original client at all, and the
    // rest are created there by the UI's own Lua or from saved Config.wtf lines, not by the DLL (live audit,
    // tools/live_audit.py; the DLL's 93 are the exact registrations above).
}

void CVar::AddToGlueCVarVector(const char* name, const char* description, uint32_t size, const char* defaultVal, void* callback, uint32_t flags, bool a7, int32_t a8, bool a9)
{
    CustomCVar temp{ name, description, size, defaultVal, callback, flags, a8, a7, a9 };

    sDC.GetGlueCVarVector().push_back(temp);
}
