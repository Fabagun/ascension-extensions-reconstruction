#include <Ascension/AscensionCA.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>

#define SMSG_CA_ESSENCE_BUDGET 0x0722
#define SMSG_CA_ACTIVE_SPEC    0x0725
#define SMSG_CA_KNOWN_ENTRIES  0x0726

using namespace AscLua;

namespace AscensionCASvc
{
    static State g_state;
    State& Get() { return g_state; }

    void __cdecl Handle_ActiveSpec(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        State& s = g_state;
        CDataStore::GetInt32(pkt, (int32_t*)&s.activeSpecIndex);
        CDataStore::GetInt32(pkt, (int32_t*)&s.specSlotCount);
        s.haveActiveSpec = true;
        AscLog::Printf("SMSG_CA_ACTIVE_SPEC(0x%X): activeSpecIndex=%u specSlotCount=%u", opcode, s.activeSpecIndex, s.specSlotCount);
    }

    void __cdecl Handle_EssenceBudget(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        EssenceRow r{};
        CDataStore::GetInt32(pkt, (int32_t*)&r.id);
        CDataStore::GetInt32(pkt, (int32_t*)&r.level);
        CDataStore::GetInt32(pkt, (int32_t*)&r.key);
        CDataStore::GetInt32(pkt, (int32_t*)&r.flagA);
        CDataStore::GetInt32(pkt, (int32_t*)&r.flagB);
        CDataStore::GetInt32(pkt, (int32_t*)&r.flagC);
        CDataStore::GetInt32(pkt, (int32_t*)&r.flagD);
        CDataStore::GetInt32(pkt, (int32_t*)&r.abilityEssence);
        CDataStore::GetInt32(pkt, (int32_t*)&r.talentEssence);
        g_state.essence.push_back(r);
        if (g_state.essence.size() <= 3 || g_state.essence.size() % 200 == 0)
            AscLog::Printf("SMSG_CA_ESSENCE_BUDGET(0x%X): row %u id=%u level=%u key=%u", opcode, (unsigned)g_state.essence.size(), r.id, r.level, r.key);
    }

    void __cdecl Handle_KnownEntries(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        int32_t count = 0;
        CDataStore::GetInt32(pkt, &count);
        g_state.known.clear();
        g_state.known.reserve(count > 0 ? (size_t)count : 0);
        for (int32_t i = 0; i < count; ++i)
        {
            KnownEntry k{};
            CDataStore::GetInt32(pkt, (int32_t*)&k.entry);
            CDataStore::GetInt32(pkt, (int32_t*)&k.rank);
            CDataStore::GetInt32(pkt, (int32_t*)&k.unk0c);
            CDataStore::GetInt8(pkt, (int8_t*)&k.flag);
            CDataStore::GetInt32(pkt, (int32_t*)&k.unk18);
            CDataStore::GetInt32(pkt, (int32_t*)&k.unk1c);
            g_state.known.push_back(k);
        }
        g_state.haveKnown = true;
        AscLog::Printf("SMSG_CA_KNOWN_ENTRIES(0x%X): %d record(s)", opcode, count);
    }

    void RegisterPacketHandlers()
    {
        sDC.AddPacketHandler(SMSG_CA_ACTIVE_SPEC, CNetClientCustomPacket((void*)&Handle_ActiveSpec, nullptr));
        sDC.AddPacketHandler(SMSG_CA_ESSENCE_BUDGET, CNetClientCustomPacket((void*)&Handle_EssenceBudget, nullptr));
        sDC.AddPacketHandler(SMSG_CA_KNOWN_ENTRIES, CNetClientCustomPacket((void*)&Handle_KnownEntries, nullptr));
    }
}
