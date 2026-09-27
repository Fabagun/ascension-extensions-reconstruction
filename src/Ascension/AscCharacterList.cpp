// C_CharacterList over CharacterListMgr (FUN_10191230 -> static 0x10BDEB48): +4/+8/+0xC/+0x10 counts,
// +0x14 vector of 0x28-byte entries.
// SMSG_CHARACTER_LIST_INFO 0x75E (FUN_10190d60): u32 (capped at 0x80) -> +4, u32 n -> +8, u32 -> +0xC,
// u32 inactive -> +0x10, then n x {u32 guid, u8 active, u8 online, u8 level, u8 race, u8 class,
// u8 gender, u32 zone, str name}; fires CHARACTER_LIST_UPDATED.
// Refresh sends CMSG 0x37 (CMSG_CHAR_ENUM); RequestActivate / RequestDeactivate send CMSG 0x72E /
// 0x72F {u32 guid}.
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
    struct Entry { uint32_t guid = 0; std::string name; uint8_t race = 0, cls = 0, gender = 0, level = 0; uint32_t zone = 0; bool active = false, online = false; };
    struct
    {
        uint32_t counts[4] = {};
        std::vector<Entry> entries;
    } g_list;

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    void __cdecl OnCharacterList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        g_list.counts[0] = std::min<uint32_t>(Read<uint32_t>(p), 0x80);
        g_list.counts[1] = Read<uint32_t>(p);
        g_list.counts[2] = Read<uint32_t>(p);
        g_list.counts[3] = Read<uint32_t>(p);
        g_list.entries.clear();
        for (uint32_t i = 0; i < g_list.counts[1]; ++i)
        {
            Entry e;
            e.guid = Read<uint32_t>(p);
            e.active = Read<uint8_t>(p) != 0;
            e.online = Read<uint8_t>(p) != 0;
            e.level = Read<uint8_t>(p);
            e.race = Read<uint8_t>(p);
            e.cls = Read<uint8_t>(p);
            e.gender = Read<uint8_t>(p);
            e.zone = Read<uint32_t>(p);
            e.name = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
            p->m_read += static_cast<int32_t>(e.name.size() + 1);
            g_list.entries.push_back(e);
        }
        AscRuntime::Signal("CHARACTER_LIST_UPDATED");
    }

    int GetCounts(lua_State* L)
    {
        for (uint32_t c : g_list.counts)
            PushInt(L, static_cast<int32_t>(c));
        return 4;
    }

    void SetInt(lua_State* L, const char* k, int32_t v) { AscLua::lua_pushstring(L, k); PushInt(L, v); AscLua::lua_settable(L, -3); }
    void SetBool(lua_State* L, const char* k, bool v) { AscLua::lua_pushstring(L, k); PushBool(L, v); AscLua::lua_settable(L, -3); }

    int GetEntries(lua_State* L)
    {
        AscLua::lua_createtable(L, static_cast<int>(g_list.entries.size()), 0);
        AscLua::lua_checkstack(L, 2);
        int i = 1;
        for (const Entry& e : g_list.entries)
        {
            AscLua::lua_createtable(L, 0, 9);
            SetInt(L, "guid", static_cast<int32_t>(e.guid));
            AscLua::lua_pushstring(L, "name");
            PushStr(L, e.name.c_str());
            AscLua::lua_settable(L, -3);
            SetBool(L, "active", e.active);
            SetBool(L, "online", e.online);
            SetInt(L, "level", e.level);
            SetInt(L, "race", e.race);
            SetInt(L, "class", e.cls);
            SetInt(L, "gender", e.gender);
            SetInt(L, "zoneId", static_cast<int32_t>(e.zone));
            AscLua::lua_rawseti(L, -2, i++);
        }
        return 1;
    }

    int HasInactiveCharacters(lua_State* L)
    {
        PushBool(L, g_list.counts[3] != 0);
        return 1;
    }

    int Refresh(lua_State* L)
    {
        Packet(0x37).Send();
        PushBool(L, true);
        return 1;
    }

    int SendGuid(lua_State* L, uint32_t opcode)
    {
        uint32_t guid;
        if (!ReadNumber(L, guid))
            return 0;
        Packet(opcode).U32(guid).Send();
        PushBool(L, true);
        return 1;
    }
    int RequestActivate(lua_State* L) { return SendGuid(L, 0x72E); }
    int RequestDeactivate(lua_State* L) { return SendGuid(L, 0x72F); }

    void Init()
    {
        sDC.AddPacketHandler(0x75E, CNetClientCustomPacket((void*)&OnCharacterList, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_CharacterList", "GetCounts", GetCounts},
        {"C_CharacterList", "GetEntries", GetEntries},
        {"C_CharacterList", "HasInactiveCharacters", HasInactiveCharacters},
        {"C_CharacterList", "Refresh", Refresh},
        {"C_CharacterList", "RequestActivate", RequestActivate},
        {"C_CharacterList", "RequestDeactivate", RequestDeactivate},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
