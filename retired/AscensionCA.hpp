#pragma once
// Client-side receive for the Character Advancement (CA) establishment burst our own server sends
// at CMSG_SET_ACTIVE_MOVER (see server-patches/ascension-ca/AscensionCA.{h,cpp} -- this is the
// client-side mirror of that exact wire contract, confirmed byte-for-byte against that source, not
// guessed from the client decompile alone):
//   SMSG_CA_ACTIVE_SPEC   (0x725,  8 B): u32 activeSpecIndex, u32 specSlotCount
//   SMSG_CA_ESSENCE_BUDGET(0x722, 36 B): one CharacterAdvancementEssence.dbc row per packet
//                                         (9 x u32: id, level, key, flagA, flagB, flagC, flagD,
//                                          abilityEssence, talentEssence)
//   SMSG_CA_KNOWN_ENTRIES (0x726, var ): u32 count, then count x 21 B records
//                                         (u32 entry, u32 rank, u32 unk0c, u8 flag, u32 unk18, u32 unk1c)
//
// What this does NOT do: replicate the real Extensions.dll's proprietary per-GUID CA container (the
// in-memory backing store the 133 C_CharacterAdvancement.* natives read from). That is Ascension-
// specific logic living inside the DLL we are replacing, not something the stock exe exposes to call
// into -- reimplementing it is future work. This handler's job for now is narrower but still real:
// consume every byte exactly so the connection never desyncs, and keep the parsed data in a small
// side table (queryable from C_CharacterAdvancement stubs later) instead of silently discarding it.
#include <cstdint>
#include <vector>
struct CDataStore;

namespace AscensionCASvc
{
    struct EssenceRow { uint32_t id, level, key, flagA, flagB, flagC, flagD, abilityEssence, talentEssence; };
    struct KnownEntry { uint32_t entry, rank, unk0c; uint8_t flag; uint32_t unk18, unk1c; };

    struct State
    {
        uint32_t activeSpecIndex = 0, specSlotCount = 0;
        bool haveActiveSpec = false;
        std::vector<EssenceRow> essence;
        std::vector<KnownEntry> known;
        bool haveKnown = false;
    };

    State& Get();
    void RegisterPacketHandlers();

    // Transitional: AscCAMgr owns 0x725/0x726 now and replays each packet through these so the
    // hand-built C_CharacterAdvancement bindings still reading State keep working until replaced.
    void __cdecl Handle_ActiveSpec(void*, uint32_t opcode, uint32_t, CDataStore* pkt);
    void __cdecl Handle_KnownEntries(void*, uint32_t opcode, uint32_t, CDataStore* pkt);
}
