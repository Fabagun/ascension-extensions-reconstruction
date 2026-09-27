#include <Ascension/CharSelectOpcodes.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>

#define SMSG_KNOWN_ADDON_LIST                0x094E
#define SMSG_ACCOUNT_INFO                    0x09BB
#define SMSG_CHARACTER_LIST_INFO             0x075E
#define SMSG_CHARACTER_SELECTION_SORT_ORDER  0x076F
#define SMSG_CHARACTER_SELECTION_MAIL        0x0770
#define SMSG_CHARACTER_SELECTION_GAME_MODE   0x0771
#define SMSG_CHARACTER_CUSTOMIZATION_UNLOCKS 0x09D0
#define SMSG_CHAT_INFRACTION                 0x06E5
#define SMSG_GAME_MODE_STATE                 0x090B
#define SMSG_CHARACTER_ADVANCEMENT_CREDITS   0x0926
// These five share ONE client handler (0x1022FC70, decompiled) that unconditionally reads a fixed
// 0x58 = 88-byte record regardless of any leading value -- the "count"/"value" field the reference
// doc's single-sample capture suggested for each is misleading (that sample happened to have a
// degenerate/zero record). Consuming the confirmed 88 bytes is the safe behavior; the field meanings
// are NOT decoded, so no event is fired with them.
#define SMSG_UPDATE_KNOWN_RANDOM_ENCHANTS    0x05F9
#define SMSG_PENDING_SKILL_CARD_LIST         0x0653
#define SMSG_PLAYER_DATA                     0x0672
#define SMSG_INITIAL_ITEM_COOLDOWNS          0x064F
#define SMSG_HONORABLE_COMBAT_ZONES          0x0768

using namespace AscLua;

namespace CharSelectSvc
{
    static AccountInfo g_account;
    static CharListInfo g_charList;
    AccountInfo& GetAccountInfo() { return g_account; }
    CharListInfo& GetCharListInfo() { return g_charList; }

    static std::string GetStr(CDataStore* pkt, size_t maxLen = 256)
    {
        std::vector<char> buf(maxLen, 0);
        CDataStore::GetCString(pkt, buf.data(), (int32_t)maxLen);
        return std::string(buf.data());
    }

    // ---- 0x09BB SMSG_ACCOUNT_INFO ------------------------------------------------------------
    void __cdecl Handle_AccountInfo(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        AccountInfo a;
        CDataStore::GetInt32(pkt, (int32_t*)&a.accountId);
        int32_t zero32 = 0; CDataStore::GetInt32(pkt, &zero32);
        int16_t zero16 = 0; CDataStore::GetInt16(pkt, &zero16);
        CDataStore::GetInt32(pkt, (int32_t*)&a.characterCount);
        a.characters.reserve(a.characterCount);
        for (uint32_t i = 0; i < a.characterCount; ++i)
        {
            AccountCharIdentity c;
            c.name = GetStr(pkt);
            c.gender = GetStr(pkt, 32);
            CDataStore::GetInt32(pkt, (int32_t*)&c.level);
            c.classToken = GetStr(pkt, 64);
            c.raceToken = GetStr(pkt, 64);
            c.faction = GetStr(pkt, 32);
            a.characters.push_back(c);
        }
        a.have = true;
        g_account = a;
        AscLog::Printf("SMSG_ACCOUNT_INFO(0x%X): account=%u characters=%u", opcode, a.accountId, a.characterCount);
    }

    // ---- 0x075E SMSG_CHARACTER_LIST_INFO -------------------------------------------------------
    void __cdecl Handle_CharacterListInfo(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        CharListInfo c;
        CDataStore::GetInt32(pkt, (int32_t*)&c.maxActive);
        CDataStore::GetInt32(pkt, (int32_t*)&c.total);
        CDataStore::GetInt32(pkt, (int32_t*)&c.active);
        CDataStore::GetInt32(pkt, (int32_t*)&c.inactive);
        c.entries.reserve(c.active);
        for (uint32_t i = 0; i < c.active; ++i)
        {
            CharListEntry e;
            CDataStore::GetInt32(pkt, (int32_t*)&e.guid);
            CDataStore::GetInt8(pkt, (int8_t*)&e.active);
            CDataStore::GetInt8(pkt, (int8_t*)&e.online);
            CDataStore::GetInt8(pkt, (int8_t*)&e.level);
            CDataStore::GetInt8(pkt, (int8_t*)&e.race);
            CDataStore::GetInt8(pkt, (int8_t*)&e.klass);
            CDataStore::GetInt8(pkt, (int8_t*)&e.gender);
            CDataStore::GetInt32(pkt, (int32_t*)&e.zone);
            e.name = GetStr(pkt);
            c.entries.push_back(e);
        }
        c.have = true;
        g_charList = c;
        AscLog::Printf("SMSG_CHARACTER_LIST_INFO(0x%X): max=%u total=%u active=%u inactive=%u",
            opcode, c.maxActive, c.total, c.active, c.inactive);
    }

    // ---- 0x076F SMSG_CHARACTER_SELECTION_SORT_ORDER: one cstring (space-separated 1-based order) --
    void __cdecl Handle_SortOrder(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        std::string order = GetStr(pkt, 64);
        AscLog::Printf("SMSG_CHARACTER_SELECTION_SORT_ORDER(0x%X): payload='%s'", opcode, order.c_str());
    }

    // ---- 0x09D0 SMSG_CHARACTER_CUSTOMIZATION_UNLOCKS: u32, always 0, handler ignores it -----------
    void __cdecl Handle_CustomizationUnlocks(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        int32_t zero = 0;
        CDataStore::GetInt32(pkt, &zero);
        AscLog::Printf("SMSG_CHARACTER_CUSTOMIZATION_UNLOCKS(0x%X): %d", opcode, zero);
    }

    // ---- 0x06E5 SMSG_CHAT_INFRACTION: u32, cstring, u32 -------------------------------------------
    void __cdecl Handle_ChatInfraction(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        int32_t a = 0, b = 0;
        CDataStore::GetInt32(pkt, &a);
        std::string name = GetStr(pkt, 64);
        CDataStore::GetInt32(pkt, &b);
        AscLog::Printf("SMSG_CHAT_INFRACTION(0x%X): %d name='%s' %d", opcode, a, name.c_str(), b);
    }

    // ---- 0x090B SMSG_GAME_MODE_STATE: single u32 mode mask -----------------------------------------
    void __cdecl Handle_GameModeState(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        int32_t mask = 0;
        CDataStore::GetInt32(pkt, &mask);
        AscLog::Printf("SMSG_GAME_MODE_STATE(0x%X): mask=0x%08X", opcode, mask);
    }

    // ---- 0x0926 SMSG_CHARACTER_ADVANCEMENT_CREDITS: u8 kind, u32 amount ---------------------------
    void __cdecl Handle_CACredits(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        int8_t kind = 0; int32_t amount = 0;
        CDataStore::GetInt8(pkt, &kind);
        CDataStore::GetInt32(pkt, &amount);
        AscLog::Printf("SMSG_CHARACTER_ADVANCEMENT_CREDITS(0x%X): kind=%d amount=%d", opcode, kind, amount);
    }

    // ---- shared 88-byte consume group (see header comment) ----------------------------------------
    void __cdecl Handle_FixedUnknown88(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        uint8_t buf[88];
        for (int i = 0; i < 88; ++i) CDataStore::GetInt8(pkt, (int8_t*)&buf[i]);
        AscLog::Printf("0x%X: consumed 88-byte record (fields not decoded)", opcode);
    }

    void RegisterPacketHandlers()
    {
        // SMSG_KNOWN_ADDON_LIST: AscGlobalsJ.cpp keeps the list (FUN_10312b90).
        sDC.AddPacketHandler(SMSG_ACCOUNT_INFO, CNetClientCustomPacket((void*)&Handle_AccountInfo, nullptr));
        sDC.AddPacketHandler(SMSG_CHARACTER_LIST_INFO, CNetClientCustomPacket((void*)&Handle_CharacterListInfo, nullptr));
        sDC.AddPacketHandler(SMSG_CHARACTER_SELECTION_SORT_ORDER, CNetClientCustomPacket((void*)&Handle_SortOrder, nullptr));
        // SMSG_CHARACTER_SELECTION_MAIL: AscGlobalsJ.cpp keeps its state (handler_0x0770 / FUN_100d3910).
        // SMSG_CHARACTER_SELECTION_GAME_MODE: AscGlobalsJ.cpp keeps its state (handler_0x0770 / FUN_100d3910).
        sDC.AddPacketHandler(SMSG_CHARACTER_CUSTOMIZATION_UNLOCKS, CNetClientCustomPacket((void*)&Handle_CustomizationUnlocks, nullptr));
        sDC.AddPacketHandler(SMSG_CHAT_INFRACTION, CNetClientCustomPacket((void*)&Handle_ChatInfraction, nullptr));
        sDC.AddPacketHandler(SMSG_GAME_MODE_STATE, CNetClientCustomPacket((void*)&Handle_GameModeState, nullptr));
        sDC.AddPacketHandler(SMSG_CHARACTER_ADVANCEMENT_CREDITS, CNetClientCustomPacket((void*)&Handle_CACredits, nullptr));
        sDC.AddPacketHandler(SMSG_UPDATE_KNOWN_RANDOM_ENCHANTS, CNetClientCustomPacket((void*)&Handle_FixedUnknown88, nullptr));
        sDC.AddPacketHandler(SMSG_PENDING_SKILL_CARD_LIST, CNetClientCustomPacket((void*)&Handle_FixedUnknown88, nullptr));
        sDC.AddPacketHandler(SMSG_PLAYER_DATA, CNetClientCustomPacket((void*)&Handle_FixedUnknown88, nullptr));
        sDC.AddPacketHandler(SMSG_INITIAL_ITEM_COOLDOWNS, CNetClientCustomPacket((void*)&Handle_FixedUnknown88, nullptr));
        sDC.AddPacketHandler(SMSG_HONORABLE_COMBAT_ZONES, CNetClientCustomPacket((void*)&Handle_FixedUnknown88, nullptr));
    }
}
