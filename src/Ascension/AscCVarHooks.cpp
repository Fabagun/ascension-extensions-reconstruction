// The CVar module's client hooks (installer FUN_101141e0; the glue registration 0x401B60 lives in
// WotLKExtensions' CVar.cpp).
//
//   0x4E4610  first loads "WTF\Account\<account>\config-cache.wtf" (0x766530) when an account name is
//             known (0x6B1010); wrapped by FUN_101952e0's "-character" select (a second hook object)
//   0x512890  SaveGameCVars(a, file, flags): a file handle of -1 is refused with a Fatal log line; after
//             a save with flag 0x10, lastCharacterIndex is written as the character-select index
//             (0xAC436C) -- the one name on the DLL's list 0x10BDD188
//   0x766640  writing a CVar line: names on that list are skipped (1)
//   0x76A220  the two scale globals 0xAB63B4 / 0xAB63B8 follow the resolution string before the client
//             applies it (FUN_101130c0)
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLogger.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Windows.h>
#include <cstdio>
#include <cstring>

namespace
{
    const char* const kSavedNames[] = {"lastCharacterIndex"};   // 0x10BDD188 (static init 0x10007630)

    bool Listed(const char* name)
    {
        for (const char* n : kSavedNames)
            if (strcmp(name, n) == 0)
                return true;
        return false;
    }

    typedef int(__cdecl* Write_t)(const char*, const char*, uint32_t);
    typedef int(__cdecl* Save_t)(uint32_t, int32_t, uint32_t);
    typedef int(__cdecl* Fn4_t)(uint32_t, uint32_t, const char*, uint32_t);
    Write_t g_766640 = nullptr;
    Save_t g_512890 = nullptr;
    Fn4_t g_76A220 = nullptr;

    // 0x4E4610 begins push esi / xor esi,esi / push esi / call 0x5F7840: the call is re-made, then 0x4E4619.
    __declspec(naked) int __cdecl Original4E4610()
    {
        __asm
        {
            push esi
            xor esi, esi
            push esi
            mov eax, 0x5F7840
            call eax
            push 0x4E4619
            ret
        }
    }
    int __cdecl ConfigCache4E4610()   // FUN_10113040
    {
        const char* account = reinterpret_cast<const char*(__cdecl*)()>(0x6B1010)();
        if (account && *account)
        {
            char path[0x104] = {};
            _snprintf_s(path, sizeof(path), _TRUNCATE, "WTF\\Account\\%s\\config-cache.wtf", account);
            reinterpret_cast<void(__cdecl*)(const char*)>(0x766530)(path);
        }
        return Original4E4610();
    }

    // LAB_10194a40 (0x10194A40; hook object 0x10BCA078, installed by FUN_101952e0) on the same target. The live
    // original's 0x4E4610 jumps here first, so it wraps FUN_10113040. On the first call only (flag
    // 0x10BDEC0C, set whether or not the argument is given): "-character name" picks the first character
    // in the list 0xB6B238 (+4 count, +8 records, name at +8) with exactly that name, makes it the
    // selection 0xAC436C and enters the world (0x4D9BD0). The original steps the records by 0x188; the
    // client's are 0x198 (0x4D9BF7), so only the first is read correctly (IMPROVEMENTS.md).
    bool g_characterDone = false;   // 0x10BDEC0C
    int __cdecl Hook4E4610()
    {
        const int r = ConfigCache4E4610();
        if (g_characterDone)
            return r;
        g_characterDone = true;
        const char* name = AscScript::CommandLineArg("character");
        if (!name)
            return r;
        const uint8_t* list = reinterpret_cast<const uint8_t*>(0xB6B238);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(list + 4);
        const uint8_t* records = *reinterpret_cast<const uint8_t* const*>(list + 8);
        for (uint32_t i = 0; i < count; ++i)
            if (strcmp(reinterpret_cast<const char*>(records + i * 0x188 + 8), name) == 0)
            {
                *reinterpret_cast<uint32_t*>(0xAC436C) = i;
                reinterpret_cast<void(__cdecl*)()>(0x4D9BD0)();
                break;
            }
        return r;
    }

    int __cdecl Hook766640(const char* name, const char* value, uint32_t file)   // FUN_101140f0
    {
        if (Listed(name))
            return 1;
        return g_766640(name, value, file);
    }

    int __cdecl Hook512890(uint32_t a, int32_t file, uint32_t flags)   // FUN_10114640
    {
        if (file == -1)
        {
            AscLogger::Write(5, "SaveGameCVarsCallBack_: handle is -1 (invalid). Skipping save to prevent "
                                "System_File__write_overlapped access violation crash.");
            return 0;
        }
        const int r = g_512890(a, file, flags);
        if (flags & 0x10)
            for (const char* name : kSavedNames)
            {
                const uint8_t* cvar = reinterpret_cast<const uint8_t*(__cdecl*)(const char*)>(0x767440)(name);
                if (!cvar)
                    continue;
                const char* value = *reinterpret_cast<const char* const*>(cvar + 0x28);
                char index[16];
                if (strcmp(name, "lastCharacterIndex") == 0)
                {
                    snprintf(index, sizeof(index), "%d", *reinterpret_cast<const int32_t*>(0xAC436C));
                    value = index;
                }
                else if (!value)
                    value = "0";
                g_766640(name, value, static_cast<uint32_t>(file));
            }
        return r;
    }

    void WriteFloat(uint32_t at, uint32_t bits)   // through NtProtectVirtualMemory in the original
    {
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(at), 4, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<uint32_t*>(at) = bits;
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(at), 4, old, &old);
    }

    int __cdecl Hook76A220(uint32_t a, uint32_t b, const char* resolution, uint32_t d)   // FUN_101130c0
    {
        static const char* const kWide[] = {"1920x1080", "1600x900", "1360x768", "1366x768", "1280x720", "1176x664"};
        bool wide = false;
        for (const char* w : kWide)
            wide = wide || strcmp(resolution, w) == 0;
        uint32_t first = 0x3F800000, second;
        if (wide)
            second = 0x3FAAAAA8;
        else if (strcmp(resolution, "1768x992") == 0)
            second = 0x3FAB851F;
        else if (strcmp(resolution, "1280x768") == 0 || strcmp(resolution, "1600x1024") == 0)
            second = 0x3FAA3D71;
        else
        {
            first = 0x3FAAAAA8;
            second = 0x3FE38E32;
        }
        WriteFloat(0xAB63B4, first);
        WriteFloat(0xAB63B8, second);
        return g_76A220(a, b, resolution, d);
    }

    void Init()   // FUN_101141e0 (its CVar queue and Lua registration live elsewhere)
    {
        g_766640 = reinterpret_cast<Write_t>(AscRuntime::Detour(0x766640, 9, reinterpret_cast<void*>(&Hook766640)));
        AscRuntime::ReplaceFunction(0x4E4610, reinterpret_cast<void*>(&Hook4E4610));
        g_512890 = reinterpret_cast<Save_t>(AscRuntime::Detour(0x512890, 7, reinterpret_cast<void*>(&Hook512890)));
        g_76A220 = reinterpret_cast<Fn4_t>(AscRuntime::Detour(0x76A220, 9, reinterpret_cast<void*>(&Hook76A220)));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
