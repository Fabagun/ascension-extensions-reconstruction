// Auto-quest pop-ups -- the original's AutoQuest object (accessor FUN_100e6190, static 0x10bdbc18):
// a vector of { u32 type, u32 questId } with type 0 = "OFFER", 1 = "COMPLETE". Lua (AddAutoQuestPopUp)
// and SMSG 0x927 (FUN_100e6220) fill it.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    struct PopUp { uint32_t type; uint32_t questId; };
    std::vector<PopUp> g_popUps;

    const char* const kTypes[] = {"OFFER", "COMPLETE"};   // PTR_s_OFFER_10b2cf74

    int32_t ArgInt(lua_State* L, int idx) { return ToInt(AscLua::lua_tonumber(L, idx)); }

    // (questId, "OFFER" | "COMPLETE") -> true; nothing for an unknown type.
    int AddAutoQuestPopUp(lua_State* L)
    {
        const uint32_t questId = static_cast<uint32_t>(ArgInt(L, 1));
        const std::string type = CheckString(L, 2);
        for (uint32_t i = 0; i < 2; ++i)
            if (type == kTypes[i])
            {
                g_popUps.push_back({i, questId});
                AscLua::lua_pushboolean(L, 1);
                return 1;
            }
        return 0;
    }

    // n (1-based) -> questId, type name.
    int GetAutoQuestPopUp(lua_State* L)
    {
        const uint32_t n = static_cast<uint32_t>(ArgInt(L, 1));
        if (g_popUps.size() < n || n == 0)
            return 0;
        const PopUp& p = g_popUps[n - 1];
        AscLua::lua_pushinteger(L, static_cast<int>(p.questId));
        const std::string name = p.type < 2 ? kTypes[p.type] : "UNEXPECTED_ENUM_VALUE_" + std::to_string(p.type);
        AscLua::lua_pushstring(L, name.c_str());
        return 2;
    }

    int GetNumAutoQuestPopUps(lua_State* L)
    {
        const bool inWorld = *reinterpret_cast<const uint8_t*>(0xBD0792) != 0;
        AscLua::lua_pushinteger(L, inWorld ? static_cast<int>(g_popUps.size()) : 0);
        return 1;
    }

    int RemoveAutoQuestPopUp(lua_State* L)
    {
        const uint32_t questId = static_cast<uint32_t>(ArgInt(L, 1));
        g_popUps.erase(std::remove_if(g_popUps.begin(), g_popUps.end(),
                                      [questId](const PopUp& p) { return p.questId == questId; }),
                       g_popUps.end());
        return 0;
    }

    // Pop-up n -> CMSG 0x542 { u32 questId }.
    int ShowQuestComplete(lua_State* L)
    {
        const uint32_t n = static_cast<uint32_t>(ArgInt(L, 1));
        if (n <= g_popUps.size() && n != 0)
            Packet(0x542).U32(g_popUps[n - 1].questId).Send();
        return 0;
    }

    // CMSG_QUESTGIVER_ACCEPT_QUEST (0x189) with no quest giver: { u64 0, u32 questId, u32 0 }.
    int Custom_SendAutoQuestAccept(lua_State* L)
    {
        const uint32_t questId = static_cast<uint32_t>(ArgInt(L, 1));
        Packet(0x189).U64(0).U32(questId).U32(0).Send();
        return 0;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "AddAutoQuestPopUp", AddAutoQuestPopUp},
        {nullptr, "Custom_SendAutoQuestAccept", Custom_SendAutoQuestAccept},
        {nullptr, "GetAutoQuestPopUp", GetAutoQuestPopUp},
        {nullptr, "GetNumAutoQuestPopUps", GetNumAutoQuestPopUps},
        {nullptr, "RemoveAutoQuestPopUp", RemoveAutoQuestPopUp},
        {nullptr, "ShowQuestComplete", ShowQuestComplete},
    };
    // SMSG 0x927 (FUN_100e6220): u32 quest, u32 type. A listed quest only moves UP (a type not above its
    // current one ends here, without an event); an unlisted one is appended. Then type 0 fires
    // QUEST_AUTO_OFFER("%u", quest) and type 1 QUEST_AUTOCOMPLETE("%u", quest).
    void __cdecl OnAutoQuest(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint32_t quest, type;
        memcpy(&quest, p->m_buffer + p->m_read, 4);
        memcpy(&type, p->m_buffer + p->m_read + 4, 4);
        p->m_read += 8;
        auto it = std::find_if(g_popUps.begin(), g_popUps.end(), [&](const PopUp& u) { return u.questId == quest; });
        if (it != g_popUps.end())
        {
            if (static_cast<int32_t>(it->type) >= static_cast<int32_t>(type))
                return;
            it->type = type;
        }
        else
            g_popUps.push_back({type, quest});
        if (type == 0)
            AscRuntime::Signal("QUEST_AUTO_OFFER", "%u", quest);
        else if (type == 1)
            AscRuntime::Signal("QUEST_AUTOCOMPLETE", "%u", quest);
    }

    void Init() { sDC.AddPacketHandler(0x927, CNetClientCustomPacket((void*)&OnAutoQuest, nullptr)); }

    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

// FUN_100e6210 on the AutoQuest object: the pop-up list emptied (the attach hook on 0x403DE0).
void AscAutoQuest_Clear() { g_popUps.clear(); }
