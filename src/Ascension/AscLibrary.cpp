// Addon-file helpers: LoadLibrary / IsLibraryLoaded -- XML libraries under Interface\LibraryXML\,
// loaded once each (list DAT_10d3d7dc..e0) -- and SaveSavedVariables.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <algorithm>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    std::vector<std::string> g_loaded;   // DAT_10d3d7dc..e0

    // The list names libraries loaded into the CURRENT Lua state. A /reload or a world re-entry builds a new
    // state with none of them, so the list starts empty with each new state (the original empties it at the
    // top of FUN_10a66100; how often that runs is not visible in the plain code, but ElvUI survives /reload
    // on the original, so it must be per state). Kept across states, IsLibraryLoaded answers true
    // for a library the new state lacks, LibStub skips LoadLibrary, and every addon that asks for it fails
    // ("Cannot find a library instance", ElvUI after /reload, 2026-09-28).
    void ForgetLibraries() { g_loaded.clear(); }
    void Init()
    {
        AscBindings::OnGlueRegistered(&ForgetLibraries);
        AscBindings::OnWorldRegistered(&ForgetLibraries);
    }

    bool Loaded(const std::string& name) { return std::find(g_loaded.begin(), g_loaded.end(), name) != g_loaded.end(); }

    // FUN_10a6fcc0: "Interface\LibraryXML\<name>.toc" through the client's TOC loader (0x814340, with
    // *0xAF5718 and an out buffer). The taint source 0xD4139C is cleared for the load when
    // 0xD413A0 is set and 0xD413A4 is not, then restored.
    int LoadLibraryXml(lua_State* L)   // not "LoadLibrary": that is a Windows.h macro
    {
        const std::string name = CheckString(L, 1);
        if (Loaded(name))
            return 0;
        g_loaded.push_back(name);
        const std::string path = "Interface\\LibraryXML\\" + name + ".toc";
        uint32_t* taint = reinterpret_cast<uint32_t*>(0xD4139C);
        const uint32_t saved = *taint;
        auto untaint = []() { return *reinterpret_cast<const uint32_t*>(0xD413A0) != 0 && *reinterpret_cast<const uint8_t*>(0xD413A4) == 0; };
        if (untaint())
            *taint = 0;
        uint8_t out[0x100] = {};
        reinterpret_cast<void(__cdecl*)(const char*, const char*, void*, uint32_t)>(0x814340)(path.c_str(), name.c_str(), out, 0xAF5718);
        if (untaint())
            *taint = saved;
        return 0;
    }

    int IsLibraryLoaded(lua_State* L)
    {
        AscLua::lua_pushboolean(L, Loaded(CheckString(L, 1)));
        return 1;
    }

    // FUN_10a70bf0: write only one addon's SavedVariables. Every other addon whose saved-variables
    // record (0x55F4D0 on 0xC2491C) has its +0x18 "dirty" flag set is cleared, 0x5F5620 saves, then
    // those flags are restored and the named addon's is set.
    int SaveSavedVariables(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        typedef uint8_t* (__thiscall* Record_t)(void*, const char*);
        const Record_t record = reinterpret_cast<Record_t>(0x55F4D0);
        void* table = reinterpret_cast<void*>(0xC2491C);
        std::vector<std::string> cleared;
        const auto count = reinterpret_cast<uint32_t(__cdecl*)()>(0x5F4FF0);
        for (uint32_t i = 0; i < count(); ++i)
        {
            const char* addon = reinterpret_cast<const char*(__cdecl*)(uint32_t)>(0x5F5000)(i);
            if (name == addon)
                continue;
            uint8_t* rec = record(table, addon);
            if (rec && rec[0x18])
            {
                rec[0x18] = 0;
                cleared.push_back(addon);
            }
        }
        reinterpret_cast<void(__cdecl*)()>(0x5F5620)();
        for (const std::string& a : cleared)
            if (uint8_t* rec = record(table, a.c_str()))
                rec[0x18] = 1;
        if (uint8_t* rec = record(table, name.c_str()))
            rec[0x18] = 1;
        return 0;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "IsLibraryLoaded", IsLibraryLoaded},
        {nullptr, "LoadLibrary", LoadLibraryXml},
        {nullptr, "SaveSavedVariables", SaveSavedVariables},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
