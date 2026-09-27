// Chat checks -- GetChatCheck (0x10192C40) over the Chat manager (static 0x10BDEB8C, FUN_101928b0): per chat
// type, a sorted set of phrases the server sends.
//   SMSG_CHAT_CHECK_ADD    0x928 (FUN_101929e0): u32 type, u32 length, bytes -- the last byte is dropped
//                                              (the sender's NUL); inserted into the type's set.
//   SMSG_CHAT_CHECK_REMOVE 0x929 (FUN_10192ac0): the same layout; erased.
// GetChatCheck(chatType, text): the chat type NAME maps to its id through a static table (FUN_1006c3b0's
// initializer list 0x10BDEB68; unknown names give 0), and the result is true when the text contains any of
// that type's phrases (FUN_10192720 copies the set; each phrase is compared equal, then searched for).
// The table lists CHAT_MSG_ADDON twice, first as 1 and then as -1; the map keeps the first, so
// "CHAT_MSG_ADDON" is 1 -- kept.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <map>
#include <set>
#include <string>
#include <unordered_map>

using namespace AscScript;

namespace
{
    const std::pair<const char*, int8_t> kChatTypes[] = {
        {"CHAT_MSG_ADDON", 1}, {"CHAT_MSG_ADDON", -1}, {"CHAT_MSG_SYSTEM", 0}, {"CHAT_MSG_SAY", 1},
        {"CHAT_MSG_PARTY", 2}, {"CHAT_MSG_RAID", 3}, {"CHAT_MSG_GUILD", 4}, {"CHAT_MSG_OFFICER", 5},
        {"CHAT_MSG_YELL", 6}, {"CHAT_MSG_WHISPER", 7}, {"CHAT_MSG_WHISPER_FOREIGN", 8}, {"CHAT_MSG_WHISPER_INFORM", 9},
        {"CHAT_MSG_EMOTE", 10}, {"CHAT_MSG_TEXT_EMOTE", 11}, {"CHAT_MSG_MONSTER_SAY", 12}, {"CHAT_MSG_MONSTER_PARTY", 13},
        {"CHAT_MSG_MONSTER_YELL", 14}, {"CHAT_MSG_MONSTER_WHISPER", 15}, {"CHAT_MSG_MONSTER_EMOTE", 16},
        {"CHAT_MSG_CHANNEL", 17}, {"CHAT_MSG_CHANNEL_JOIN", 18}, {"CHAT_MSG_CHANNEL_LEAVE", 19},
        {"CHAT_MSG_CHANNEL_LIST", 20}, {"CHAT_MSG_CHANNEL_NOTICE", 21}, {"CHAT_MSG_CHANNEL_NOTICE_USER", 22},
        {"CHAT_MSG_AFK", 23}, {"CHAT_MSG_DND", 24}, {"CHAT_MSG_IGNORED", 25}, {"CHAT_MSG_SKILL", 26},
        {"CHAT_MSG_LOOT", 27}, {"CHAT_MSG_MONEY", 28}, {"CHAT_MSG_OPENING", 29}, {"CHAT_MSG_TRADESKILLS", 30},
        {"CHAT_MSG_PET_INFO", 31}, {"CHAT_MSG_COMBAT_MISC_INFO", 32}, {"CHAT_MSG_COMBAT_XP_GAIN", 33},
        {"CHAT_MSG_COMBAT_HONOR_GAIN", 34}, {"CHAT_MSG_COMBAT_FACTION_CHANGE", 35}, {"CHAT_MSG_BG_SYSTEM_NEUTRAL", 36},
        {"CHAT_MSG_BG_SYSTEM_ALLIANCE", 37}, {"CHAT_MSG_BG_SYSTEM_HORDE", 38}, {"CHAT_MSG_RAID_LEADER", 39},
        {"CHAT_MSG_RAID_WARNING", 40}, {"CHAT_MSG_RAID_BOSS_EMOTE", 41}, {"CHAT_MSG_RAID_BOSS_WHISPER", 42},
        {"CHAT_MSG_FILTERED", 43}, {"CHAT_MSG_BATTLEGROUND", 44}, {"CHAT_MSG_BATTLEGROUND_LEADER", 45},
        {"CHAT_MSG_RESTRICTED", 46}, {"CHAT_MSG_BATTLENET", 47}, {"CHAT_MSG_ACHIEVEMENT", 48},
        {"CHAT_MSG_GUILD_ACHIEVEMENT", 49}, {"CHAT_MSG_ARENA_POINTS", 50}, {"CHAT_MSG_PARTY_LEADER", 51},
    };

    const std::unordered_map<std::string, int8_t>& ChatTypeIds()
    {
        static std::unordered_map<std::string, int8_t> ids;
        if (ids.empty())
            for (const auto& t : kChatTypes)
                ids.emplace(t.first, t.second);   // first wins
        return ids;
    }

    std::unordered_map<uint32_t, std::set<std::string>> g_checks;   // Chat manager +0x04

    // u32 type, then FUN_100d3680's u32-length string with its last byte dropped.
    bool ReadCheck(CDataStore* p, uint32_t& type, std::string& phrase)
    {
        int32_t t = 0, len = 0;
        CDataStore::GetInt32(p, &t);
        CDataStore::GetInt32(p, &len);
        type = static_cast<uint32_t>(t);
        if (len <= 0 || p->m_read + len > p->m_size)
            return len == 0;
        phrase.assign(reinterpret_cast<const char*>(p->m_buffer + p->m_read), static_cast<size_t>(len - 1));
        p->m_read += len;
        return true;
    }

    void __cdecl OnChatCheckAdd(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_101929e0
    {
        uint32_t type = 0;
        std::string phrase;
        if (ReadCheck(p, type, phrase))
            g_checks[type].insert(phrase);
    }

    void __cdecl OnChatCheckRemove(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10192ac0
    {
        uint32_t type = 0;
        std::string phrase;
        if (ReadCheck(p, type, phrase))
            g_checks[type].erase(phrase);
    }

    int GetChatCheck(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        const std::string text = CheckString(L, 2);
        const auto& ids = ChatTypeIds();
        const auto id = ids.find(name);
        const uint32_t type = id == ids.end() ? 0u : static_cast<uint32_t>(static_cast<int32_t>(id->second));
        const auto set = g_checks.find(type);
        bool found = false;
        if (set != g_checks.end())
            for (const std::string& phrase : set->second)
                if (phrase == text || (phrase.size() <= text.size() && text.find(phrase) != std::string::npos))
                {
                    found = true;
                    break;
                }
        PushBool(L, found);
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x928, CNetClientCustomPacket((void*)&OnChatCheckAdd, nullptr));
        sDC.AddPacketHandler(0x929, CNetClientCustomPacket((void*)&OnChatCheckRemove, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetChatCheck", GetChatCheck},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

// FUN_101929d0 on the Chat manager: every check dropped (the attach hook on 0x403DE0).
void AscChatCheck_Clear() { g_checks.clear(); }
