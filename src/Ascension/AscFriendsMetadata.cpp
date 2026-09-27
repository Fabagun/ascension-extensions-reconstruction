// Friends metadata -- a name -> string map (unordered_map at 0x10be2520) the server fills with
// SMSG_FRIEND_ADDON_LIST (0x9C0, FUN_102388c0):
//   u32 count, then count x { u32 len + name bytes, u32 len + value bytes }
// A value that differs from the stored one replaces it and fires FRIEND_METADATA_CHANGED(name, value).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <string>
#include <unordered_map>

using namespace AscScript;

namespace
{
    std::unordered_map<std::string, std::string> g_meta;

    uint32_t ReadU32(CDataStore* p) { uint32_t v = *reinterpret_cast<const uint32_t*>(p->m_buffer + p->m_read); p->m_read += 4; return v; }
    std::string ReadLenStr(CDataStore* p)
    {
        const uint32_t n = ReadU32(p);
        std::string s(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += static_cast<int32_t>(n);
        return s;
    }

    void __cdecl OnFriendAddonList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t count = ReadU32(p);
        for (uint32_t i = 0; i < count; ++i)
        {
            const std::string name = ReadLenStr(p);
            const std::string value = ReadLenStr(p);
            auto it = g_meta.find(name);
            if (it != g_meta.end() && it->second == value)
                continue;
            g_meta[name] = value;
            AscRuntime::Signal("FRIEND_METADATA_CHANGED", "%s%s", name.c_str(), value.c_str());
        }
    }

    // FUN_10238590: nil for an empty or unknown name.
    int GetFriendsMetadata(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        auto it = name.empty() ? g_meta.end() : g_meta.find(name);
        if (it == g_meta.end())
            AscLua::lua_pushnil(L);
        else
            AscLua::lua_pushstring(L, it->second.c_str());
        return 1;
    }

    // FUN_10238e50: CMSG 0x556 { u32 len + name, u32 len + value }; returns 1 without pushing.
    int SetFriendsMetadata(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        const std::string value = CheckString(L, 2);
        Packet pk(0x556);
        pk.U32(static_cast<uint32_t>(name.size()));
        if (!name.empty())
            pk.Data(name.data(), static_cast<uint32_t>(name.size()));
        pk.U32(static_cast<uint32_t>(value.size()));
        if (!value.empty())
            pk.Data(value.data(), static_cast<uint32_t>(value.size()));
        pk.Send();
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x9C0, CNetClientCustomPacket((void*)&OnFriendAddonList, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetFriendsMetadata", GetFriendsMetadata},
        {nullptr, "SetFriendsMetadata", SetFriendsMetadata},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
