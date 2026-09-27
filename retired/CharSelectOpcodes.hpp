#pragma once
// The character-select cluster: opcodes the server sends between AUTH_OK and the player picking a
// character. Layouts are byte-for-byte from ascension-opcode-dbc-reference.md's [layout]-status
// entries, which were decoded from a real captured live-realm session (not guessed, not replayed
// synthetically) -- see FINDINGS.md 2026-09-21 for how that reference relates to the atlas/decompile.
#include <cstdint>
#include <vector>
#include <string>
struct CDataStore;

namespace CharSelectSvc
{
    // ---- 0x09BB SMSG_ACCOUNT_INFO ---------------------------------------------------------------
    struct AccountCharIdentity { std::string name, gender, classToken, raceToken, faction; uint32_t level = 0; };
    struct AccountInfo { uint32_t accountId = 0, characterCount = 0; std::vector<AccountCharIdentity> characters; bool have = false; };

    // ---- 0x075E SMSG_CHARACTER_LIST_INFO ---------------------------------------------------------
    struct CharListEntry { uint32_t guid = 0, zone = 0; uint8_t active = 0, online = 0, level = 0, race = 0, klass = 0, gender = 0; std::string name; };
    struct CharListInfo { uint32_t maxActive = 0, total = 0, active = 0, inactive = 0; std::vector<CharListEntry> entries; bool have = false; };

    AccountInfo& GetAccountInfo();
    CharListInfo& GetCharListInfo();

    void RegisterPacketHandlers();
}
