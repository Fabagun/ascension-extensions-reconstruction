#pragma once
// SMSG_REALM_INFO (0x9BC): the packet that tells the glue what kind of realm it is on and which
// character-creation flavours exist.
//
// WIRE LAYOUT (57 B, corrected 2026-09-21 against ascension-opcode-dbc-reference.md, which sourced
// it byte-for-byte from the CoA fork's own SendAscensionRealmInfo -- highest available confidence.
// The EARLIER version of this struct conflated the wire format with the client's in-memory
// realm-info OBJECT layout read by the C_Realm/C_CharacterCreate getters; they are different things
// that happen to overlap only at the 8-byte "gates" block below):
//   0x00 u32   field0            (20 in the reference sample; unidentified)
//   0x04 u32   ruleset           (0 = classic/level 60 cap)
//   0x08 f32   rateXpQuest
//   0x0C f32   rate1
//   0x10 f32   rate2
//   0x14 u32   flag              (client stores (value != 0) at realm-object +0x18)
//   0x18 f32   rate3
//   0x1C f32   rateAuctionDepositVanity
//   0x20 u32   maxAuctionDepositVanity
//   0x24 u8[8] gates             copied verbatim to realm-object +0x40..+0x47 -- THIS is what the
//                                 C_Realm.Is*/C_CharacterCreate.CanCreate* getters (0x102FC620 +0x48,
//                                 0x102FC630 +0x46, 0x102FC640 +0x44, 0x102FC650 +0x42, 0x102FC660
//                                 +0x40, 0x102FC670 +0x43, 0x102FC6A0 +0x47, 0x102FC6B0 +0x41) read.
//   var  cstring dataPath        the realm data directory under Data (e.g. "area-52"); empty = stock data
//   var  cstring realmName       NUL-terminated (e.g. "voljin")
//   +0   u8      trailingFlag    realm-object +0x48 (the getter at 0x102FC620)
//   +1   u32     field           realm-object +0x64
#include <cstdint>
struct lua_State;
struct CDataStore;

struct RealmInfo
{
    uint32_t field0 = 0;
    uint32_t ruleset = 0;
    float    rateXpQuest = 0, rate1 = 0, rate2 = 0;
    uint32_t flag14 = 0;
    float    rate3 = 0, rateAuctionDepositVanity = 0;
    uint32_t maxAuctionDepositVanity = 0;
    uint8_t  gates[8] = {};   // realm-object +0x40..+0x47
    char     dataPath[64] = {};
    char     realmName[128] = {};
    uint8_t  trailingFlag = 0; // realm-object +0x48
    uint32_t field64 = 0;
    uint8_t  visitedArea52 = 0;   // realm-object +0x49: sticky once a packet names realm 11
    uint8_t  received = 0;    // not on the wire; set once a packet has actually been parsed

    // FUN_102fc660 / 6b0 / 650 / 670 / 640 (C_Realm.IsLive / IsSeasonal / IsLeague / IsPTR / IsDevelopment)
    bool Live() const        { return gates[0] != 0; }
    bool Seasonal() const    { return gates[1] != 0; }
    bool League() const      { return gates[2] != 0; }
    bool PTR() const         { return gates[3] != 0; }
    bool Dev() const         { return gates[4] != 0; }
    bool CoA() const         { return gates[6] != 0 || gates[4] != 0; }
    bool Hero() const        { return !(gates[6] != 0 || gates[7] != 0); }
    bool Wildcard() const    { return gates[7] != 0; }
};

namespace RealmInfoSvc
{
    RealmInfo& Get();
    void __cdecl Handle_SMSG_REALM_INFO(void* param, uint32_t opcode, uint32_t a2, CDataStore* pkt);
    void RegisterPacketHandlers();
    // Lua namespaces backed by this state
}
