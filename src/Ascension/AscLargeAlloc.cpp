// The LargeAlloc tracer (installer FUN_102a85f0): loads report their size to Logs\LargeAlloc.txt (channel 9)
// past a threshold.
//
//   0x4B9760  texture create (__cdecl(name, flags, status, a)): the name becomes the crash report's "Last
//             TextureCreate" (0x10BCB94C) and the current BLP (0x10BCB964, cleared after); "BLP: <name> (Size:
//             N MB)" over 1 000 000 bytes                                                   (FUN_102a8760)
//   0x83CF00  M2 load (__thiscall(model)): the current M2 (0x10BCB988) is model +0x3C while it runs; "M2: ..."
//             over 2 000 000 bytes                                                          (FUN_102a82f0)
// Nothing in the original ever adds to the BLP / M2 byte counters (0x10BE3260 / 0x10BE3264), so those two
// lines are never written; kept as they are.
//
//   0x6348B0  WowClientDB load thunk (__thiscall(db, a, b), `mov eax,[ecx]; jmp [eax+4]`, 5 bytes): FUN_102a8440
//             looks the object up in the DLL's table list (0x10BE3238), calls the original and reports
//             "DBC: <name> (Size: N MB)" over 5 000 000 bytes of 0x10BE07AC. Nothing ever fills that list and the
//             counter is only ever zeroed, so the detour always just calls through -- as ours does.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCrashContext.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
    std::string g_currentBlp;   // 0x10BCB964
    std::string g_currentM2;    // 0x10BCB988
    uint32_t g_blpBytes = 0;    // 0x10BE3260
    uint32_t g_m2Bytes = 0;     // 0x10BE3264

    void Report(const char* kind, const std::string& name, uint32_t bytes)
    {
        char line[400];
        snprintf(line, sizeof(line), "%s: %s (Size: %u MB)", kind, name.c_str(), bytes / 1000000);
        AscLogger::Write(9, line);
    }

    typedef int(__cdecl* Texture_t)(const char*, uint32_t, void*, int);
    Texture_t g_4B9760 = nullptr;
    int __cdecl Hook4B9760(const char* name, uint32_t flags, void* status, int a)
    {
        g_currentBlp = name;
        AscCrashContext::g_textureCreate = name;
        const int r = g_4B9760(name, flags, status, a);
        if (g_blpBytes > 1000000)
            Report("BLP", g_currentBlp, g_blpBytes);
        g_currentBlp.clear();
        g_blpBytes = 0;
        return r;
    }

    typedef int(__fastcall* M2_t)(uint8_t*, void*);
    M2_t g_83CF00 = nullptr;
    int __fastcall Hook83CF00(uint8_t* model, void* edx)
    {
        g_currentM2 = reinterpret_cast<const char*>(model + 0x3C);
        const int r = g_83CF00(model, edx);
        if (g_m2Bytes > 2000000)
            Report("M2", g_currentM2, g_m2Bytes);
        g_currentM2.clear();
        g_m2Bytes = 0;
        return r;
    }

    typedef int(__fastcall* DbLoad_t)(void*, void*, uint32_t, uint32_t);
    DbLoad_t g_6348B0 = nullptr;
    int __fastcall Hook6348B0(void* db, void* edx, uint32_t a, uint32_t b)   // FUN_102a8440
    {
        return g_6348B0(db, edx, a, b);
    }

    void Init()
    {
        g_4B9760 = reinterpret_cast<Texture_t>(AscRuntime::Detour(0x4B9760, 9, reinterpret_cast<void*>(&Hook4B9760)));
        g_83CF00 = reinterpret_cast<M2_t>(AscRuntime::Detour(0x83CF00, 7, reinterpret_cast<void*>(&Hook83CF00)));
        g_6348B0 = reinterpret_cast<DbLoad_t>(AscRuntime::Detour(0x6348B0, 5, reinterpret_cast<void*>(&Hook6348B0)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
