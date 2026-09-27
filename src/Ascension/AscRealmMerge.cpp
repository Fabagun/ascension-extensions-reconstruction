// C_RealmMerge (FUN_102fd660 / 960 / da40 / dc60 / dea0).
//
// Realm ids and names are the DLL's static enum (0x10B580C8 ids, 0x10B58120 names, 21 entries).
// SMSG 0x5F5 (handler_0x05f5) sets the target realm id (0x10BE3B24); SMSG 0x5F7 (FUN_102fd410):
// u32 len + string -> REALM_MERGE_SET_TARGET_RESULT("%s"). SetTargetRealm sends CMSG 0x5F6 u32 id.
// Availability comes from the config store: CONFIG_REALM_MERGE_ENABLED (bool) and
// CONFIG_REALM_MERGE_AVAILABLE_REALMS (int vector).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscConfig.hpp>
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
    struct Realm { int32_t id; const char* name; };
    const Realm kRealms[21] = {
        {0, "None"}, {1, "AscensionRealm"}, {2, "Proudmoore"}, {3, "GrizzlyHills"}, {4, "League"},
        {5, "Bloodhoof"}, {6, "Daggerspine"}, {7, "Moroes"}, {8, "Bronzebeard"}, {9, "TwistingNether"},
        {10, "CharacterArchive"}, {11, "Area52"}, {12, "ThunderbluffQA"}, {13, "CoA"}, {14, "Fizzcrank"},
        {15, "Archimonde"}, {16, "WarcraftReborn"}, {17, "Uther"}, {26, "WildhammerQA"}, {27, "Rexxar"},
        {28, "FreepickLiveQA"},
    };

    int32_t g_target = 0;

    const char* RealmName(int32_t id)
    {
        for (const Realm& r : kRealms)
            if (r.id == id)
                return r.name;
        return nullptr;
    }

    const std::vector<int32_t>* Available() { return AscConfig::IntVector("CONFIG_REALM_MERGE_AVAILABLE_REALMS"); }
    bool Enabled()
    {
        const bool* b = AscConfig::Bool("CONFIG_REALM_MERGE_ENABLED");
        return b && *b;
    }

    void __cdecl OnSetTarget(void*, uint32_t, uint32_t, CDataStore* p)
    {
        memcpy(&g_target, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
    }

    void __cdecl OnSetTargetResult(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t len;
        memcpy(&len, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        const std::string result(reinterpret_cast<const char*>(p->m_buffer + p->m_read), len);
        p->m_read += static_cast<int32_t>(len);
        AscRuntime::Signal("REALM_MERGE_SET_TARGET_RESULT", "%s", result.c_str());
    }

    // Names of the available realm ids that are in the enum, in config order.
    int GetAvailableOptions(lua_State* L)
    {
        std::vector<const char*> names;
        if (const std::vector<int32_t>* ids = Available())
            for (int32_t id : *ids)
                if (const char* n = RealmName(id))
                    names.push_back(n);
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < names.size(); ++i)
        {
            PushNum(L, i + 1);
            PushStr(L, names[i]);
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    int GetTargetRealm(lua_State* L)
    {
        if (const char* n = RealmName(g_target))
            PushStr(L, n);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int IsMergeAvailable(lua_State* L)
    {
        const std::vector<int32_t>* ids = Available();
        PushBool(L, Enabled() && ids && !ids->empty());
        return 1;
    }

    int NeedsSetTargetRealm(lua_State* L)
    {
        const std::vector<int32_t>* ids = Available();
        PushBool(L, Enabled() && ids && !ids->empty()
            && std::find(ids->begin(), ids->end(), g_target) == ids->end());
        return 1;
    }

    // FUN_102fcfa0: exact name -> id; an unknown name resolves to 0 ("None"), which is refused.
    int SetTargetRealm(lua_State* L)
    {
        if (!ValidateInput(L, {STRING}))
            return 0;
        const std::string name = CheckString(L, 1);
        int32_t id = 0;
        for (const Realm& r : kRealms)
            if (name == r.name)
            {
                id = r.id;
                break;
            }
        if (id == 0)
        {
            PushBool(L, false);
            return 1;
        }
        Packet(0x5F6).U32(static_cast<uint32_t>(id)).Send();
        PushBool(L, true);
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x5F5, CNetClientCustomPacket((void*)&OnSetTarget, nullptr));
        sDC.AddPacketHandler(0x5F7, CNetClientCustomPacket((void*)&OnSetTargetResult, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_RealmMerge", "GetAvailableOptions", GetAvailableOptions},
        {"C_RealmMerge", "GetTargetRealm", GetTargetRealm},
        {"C_RealmMerge", "IsMergeAvailable", IsMergeAvailable},
        {"C_RealmMerge", "NeedsSetTargetRealm", NeedsSetTargetRealm},
        {"C_RealmMerge", "SetTargetRealm", SetTargetRealm},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
