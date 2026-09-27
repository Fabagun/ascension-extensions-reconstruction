// C_BuildDraft (namespace registered by FUN_10111b10) over the build-draft state (FUN_10111410 ->
// static 0x10BC9848): +0x00 completed builds, +0x0C draftable builds (vector<string>), +0x18 drafted
// build, +0x30 awaiting-role-prompt flag. All strings on the wire are NUL-terminated.
//   SMSG 0x636 DRAFTABLES: list replaced (u32 n x str); DRAFTABLE_BUILDS_UPDATE if it differs.
//   SMSG 0x637 DRAFTABLE:  u32 index, str; the list grows to fit; DRAFTABLE_BUILD_UPDATE("%u", index+1)
//                           if the slot changed.
//   SMSG 0x638 DRAFTED:    str; DRAFTED_BUILD_UPDATE.
//   SMSG 0x639 ROLE_PROMPT: BUILD_DRAFT_ROLE_PROMPT, then the flag is set.
//   SMSG 0x63B SELECT_ROLE_RESULT: str; BUILD_DRAFT_SELECT_ROLE_OK clears the flag;
//                           BUILD_DRAFT_SELECT_ROLE_RESULT("%s").
//   SMSG 0x63C COMPLETED_BUILDS: list replaced (u32 n x str), no event.
//   SMSG 0x63D COMPLETED_BUILD:  str appended; COMPLETED_BUILD_UPDATE("%s").
//   SelectRole(role): CMSG 0x63A {str}.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    struct
    {
        std::vector<std::string> completed, draftable;
        std::string drafted;
        bool awaitingRole = false;
    } g_build;

    uint32_t ReadU32(CDataStore* p)
    {
        uint32_t v;
        memcpy(&v, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        return v;
    }
    std::string ReadStr(CDataStore* p)
    {
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read));
        p->m_read += static_cast<int32_t>(s.size() + 1);
        return s;
    }

    void __cdecl OnDraftables(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::vector<std::string> old = g_build.draftable;
        g_build.draftable.clear();
        for (uint32_t n = ReadU32(p); n; --n)
            g_build.draftable.push_back(ReadStr(p));
        if (old != g_build.draftable)
            AscRuntime::Signal("DRAFTABLE_BUILDS_UPDATE");
    }

    void __cdecl OnDraftable(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t index = ReadU32(p);
        const std::string old = index < g_build.draftable.size() ? g_build.draftable[index] : std::string();
        if (index >= g_build.draftable.size())
            g_build.draftable.resize(index + 1);
        g_build.draftable[index] = ReadStr(p);
        if (g_build.draftable[index] != old)
            AscRuntime::Signal("DRAFTABLE_BUILD_UPDATE", "%u", index + 1);
    }

    void __cdecl OnDrafted(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_build.drafted = ReadStr(p);
        AscRuntime::Signal("DRAFTED_BUILD_UPDATE");
    }

    void __cdecl OnRolePrompt(void*, uint32_t, uint32_t, CDataStore*)
    {
        AscRuntime::Signal("BUILD_DRAFT_ROLE_PROMPT");
        g_build.awaitingRole = true;
    }

    void __cdecl OnSelectRoleResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadStr(p);
        if (result == "BUILD_DRAFT_SELECT_ROLE_OK")
            g_build.awaitingRole = false;
        AscRuntime::Signal("BUILD_DRAFT_SELECT_ROLE_RESULT", "%s", result.c_str());
    }

    void __cdecl OnCompletedBuilds(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_build.completed.clear();
        for (uint32_t n = ReadU32(p); n; --n)
            g_build.completed.push_back(ReadStr(p));
    }

    void __cdecl OnCompletedBuild(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string name = ReadStr(p);
        g_build.completed.push_back(name);
        AscRuntime::Signal("COMPLETED_BUILD_UPDATE", "%s", name.c_str());
    }

    bool Contains(const std::vector<std::string>& v, const std::string& s)
    {
        return std::find(v.begin(), v.end(), s) != v.end();
    }

    int IsCompletedBuild(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        PushBool(L, Contains(g_build.completed, s));
        return 1;
    }

    int IsDraftableBuild(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        PushBool(L, Contains(g_build.draftable, s));
        return 1;
    }

    int IsDraftedBuild(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        PushBool(L, g_build.drafted == s);
        return 1;
    }

    int GetNumDraftableBuilds(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g_build.draftable.size()));
        return 1;
    }

    // FUN_10111470: 1-based; out of range pushes "" (the static empty string 0x10BC9830).
    int GetDraftableBuild(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const int32_t i = ToInt(CheckNumber(L, 1));
        if (i == 0)
            return 0;
        const uint32_t idx = static_cast<uint32_t>(i - 1);
        PushStr(L, idx < g_build.draftable.size() ? g_build.draftable[idx].c_str() : "");
        return 1;
    }

    int GetDraftedBuild(lua_State* L)
    {
        PushStr(L, g_build.drafted.c_str());
        return 1;
    }

    int IsAwaitingRolePrompt(lua_State* L)
    {
        PushBool(L, g_build.awaitingRole);
        return 1;
    }

    int SelectRole(lua_State* L)
    {
        std::string role;
        if (!ReadString(L, role))
            return 0;
        Packet(0x63A).Data(role.c_str(), static_cast<uint32_t>(role.size() + 1)).Send();
        PushBool(L, true);
        return 1;
    }

    void Reset()   // FUN_10110250 (every glue screen): both lists, the drafted build and the flag cleared
    {
        g_build.completed.clear();
        g_build.draftable.clear();
        g_build.drafted.clear();
        g_build.awaitingRole = false;
    }

    void Init()
    {
        AscRuntime::OnGlueScreen(&Reset);
        sDC.AddPacketHandler(0x636, CNetClientCustomPacket((void*)&OnDraftables, nullptr));
        sDC.AddPacketHandler(0x637, CNetClientCustomPacket((void*)&OnDraftable, nullptr));
        sDC.AddPacketHandler(0x638, CNetClientCustomPacket((void*)&OnDrafted, nullptr));
        sDC.AddPacketHandler(0x639, CNetClientCustomPacket((void*)&OnRolePrompt, nullptr));
        sDC.AddPacketHandler(0x63B, CNetClientCustomPacket((void*)&OnSelectRoleResult, nullptr));
        sDC.AddPacketHandler(0x63C, CNetClientCustomPacket((void*)&OnCompletedBuilds, nullptr));
        sDC.AddPacketHandler(0x63D, CNetClientCustomPacket((void*)&OnCompletedBuild, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_BuildDraft", "IsCompletedBuild", IsCompletedBuild},
        {"C_BuildDraft", "IsDraftableBuild", IsDraftableBuild},
        {"C_BuildDraft", "IsDraftedBuild", IsDraftedBuild},
        {"C_BuildDraft", "GetNumDraftableBuilds", GetNumDraftableBuilds},
        {"C_BuildDraft", "GetDraftableBuild", GetDraftableBuild},
        {"C_BuildDraft", "GetDraftedBuild", GetDraftedBuild},
        {"C_BuildDraft", "SelectRole", SelectRole},
        {"C_BuildDraft", "IsAwaitingRolePrompt", IsAwaitingRolePrompt},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
