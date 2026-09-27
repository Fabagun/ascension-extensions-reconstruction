#include <Ascension/RealmInfo.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRealmData.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>

#define SMSG_REALM_INFO 0x9BC

using namespace AscLua;

namespace RealmInfoSvc
{
    static RealmInfo g_info;
    RealmInfo& Get() { return g_info; }

    static float GetFloat(CDataStore* pkt)
    {
        int32_t bits = 0;
        CDataStore::GetInt32(pkt, &bits);
        float f;
        memcpy(&f, &bits, 4);
        return f;
    }

    // SMSG_REALM_INFO 0x9BC -- FUN_102fc6c0 (registered at 0x102FC5EA). Realm object FUN_1008e2d0: +4 id
    // (0xB also sets the sticky +0x49 Area-52 flag), +8 ruleset, +0xC..+0x14 rates, +0x18 maintenance
    // (bool), +0x1C/+0x20 rates, +0x24 max deposit, +0x40..+0x47 gates, data path, realm name, +0x48 flag,
    // +0x64 an optional trailing u32.
    void __cdecl Handle_SMSG_REALM_INFO(void* /*param*/, uint32_t opcode, uint32_t /*a2*/, CDataStore* pkt)
    {
        RealmInfo r;
        CDataStore::GetInt32(pkt, (int32_t*)&r.field0);
        CDataStore::GetInt32(pkt, (int32_t*)&r.ruleset);
        r.rateXpQuest = GetFloat(pkt);
        r.rate1 = GetFloat(pkt);
        r.rate2 = GetFloat(pkt);
        CDataStore::GetInt32(pkt, (int32_t*)&r.flag14);
        r.rate3 = GetFloat(pkt);
        r.rateAuctionDepositVanity = GetFloat(pkt);
        CDataStore::GetInt32(pkt, (int32_t*)&r.maxAuctionDepositVanity);
        for (int i = 0; i < 8; ++i) CDataStore::GetInt8(pkt, (int8_t*)&r.gates[i]);
        CDataStore::GetCString(pkt, r.dataPath, sizeof(r.dataPath));
        CDataStore::GetCString(pkt, r.realmName, sizeof(r.realmName));
        CDataStore::GetInt8(pkt, (int8_t*)&r.trailingFlag);
        // FUN_102fc6c0 reads the last u32 only when bytes remain (+0x64 keeps 0 otherwise).
        if (pkt->m_read != pkt->m_size)
            CDataStore::GetInt32(pkt, (int32_t*)&r.field64);
        r.visitedArea52 = g_info.visitedArea52 || r.field0 == 0xB;
        r.received = 1;
        g_info = r;
        // FUN_102fc6c0's tail, in its order: a non-empty data path that differs from the current one starts
        // the realm data hot-swap (FUN_102fac20, AscRealmData.cpp); then a set maintenance flag fires
        // REALM_ALERT_MAINTENANCE.
        if (r.dataPath[0] && AscRealmData::CurrentPath() != r.dataPath)
            AscRealmData::Switch(r.dataPath);
        if (r.flag14)
            AscRuntime::Signal("REALM_ALERT_MAINTENANCE");
        AscLog::Printf("SMSG_REALM_INFO(0x%X): field0=%u ruleset=%u rates=%.4f/%.4f/%.4f/%.4f auctionDeposit=%.4f(max %u) "
            "gates=%d%d%d%d%d%d%d%d realm='%s' trailingFlag=%d field64=%u",
            opcode, r.field0, r.ruleset, r.rateXpQuest, r.rate1, r.rate2, r.rate3, r.rateAuctionDepositVanity,
            r.maxAuctionDepositVanity, r.gates[0], r.gates[1], r.gates[2], r.gates[3], r.gates[4], r.gates[5],
            r.gates[6], r.gates[7], r.realmName, r.trailingFlag, r.field64);
    }

    void RegisterPacketHandlers()
    {
        sDC.AddPacketHandler(SMSG_REALM_INFO, CNetClientCustomPacket((void*)&Handle_SMSG_REALM_INFO, nullptr));
    }

    // C_Realm is transcribed in AscAccount.cpp (FUN_102fc640..6b0 over gates[0..4]).

}
