#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <Misc/Util.hpp>
#include <Ascension/AscProfiler.hpp>
#include <Ascension/AscRuntime.hpp>

#include <unordered_set>

// The packet dispatch of the original's installer FUN_102c3540, transcribed (2026-09-27; until then the upstream
// scaffold patched the call sites 0x6324CA / 0x714AFC / 0x716A79 / 0x6B0B9E with a 0x51F split instead):
//   0x631FA0 SetMessageHandler  -> FUN_102c4400: opcode > 0x55E into the DLL's maps, else the client's table
//   0x631FE0 ProcessMessage     -> FUN_102c3ab0: opcode < 0x55F to the client, else the DLL's map
//   0x63200D                       ProcessMessage's bound `cmp esi, 0x51F` -> 0x9D4
//   0x632A40 NetClient ctor     -> FUN_102c3a90: keeps the object (0x10BE3564) for the DLL registrar FUN_102c4590
// The DLL's own handlers (sDC's map) are all above 0x55E, as the original's are.
// (0x631FC0, the clear, is AscProfiler's.)

namespace
{
    void* g_netClient = nullptr;   // DAT_10be3564

    // DAT_10be34d8: the "Skipped packet" set FUN_102c3ab0 consults first. Only its static initialiser
    // (FUN_10078380) touches it -- it is always empty -- kept for the exact branch.
    std::unordered_set<uint32_t> g_skipped;

    typedef void(__fastcall* SetHandler_t)(void*, void*, uint32_t, void*, void*);
    SetHandler_t g_setHandler = nullptr;
    typedef int(__fastcall* Process_t)(void*, void*, uint32_t, CDataStore*, uint32_t);
    Process_t g_process = nullptr;
    typedef void*(__fastcall* Ctor_t)(void*, void*);
    Ctor_t g_ctor = nullptr;

    void __fastcall SetMessageHandlerDetour(void* net, void* edx, uint32_t opcode, void* handler, void* param)   // FUN_102c4400
    {
        if (static_cast<int32_t>(opcode) > 0x55E)
        {
            sDC.GetPacketHandlerMap().insert_or_assign(opcode, CNetClientCustomPacket(handler, param));
            return;
        }
        g_setHandler(net, edx, opcode, handler, param);
    }

    int __fastcall ProcessMessageDetour(void* net, void* edx, uint32_t a2, CDataStore* store, uint32_t a4)   // FUN_102c3ab0
    {
        const int16_t op16 = *reinterpret_cast<const int16_t*>(store->m_buffer + store->m_read);
        store->m_read += 2;
        const int32_t opcode = op16;
        const int64_t start = AscProfiler::Received(opcode, static_cast<uint32_t>(store->m_size));
        if (g_skipped.count(static_cast<uint32_t>(opcode)))
            return 1;   // "Skipped packet: %s (%u)"
        if (opcode < 0x55F)
        {
            store->m_read = 0;
            const int r = g_process(net, edx, a2, store, a4);
            AscProfiler::Handled(opcode, start);
            return r;
        }
        auto& map = sDC.GetPacketHandlerMap();
        const auto it = map.find(static_cast<uint32_t>(opcode));
        if (it == map.end())
        {
            AscProfiler::Unhandled(opcode);   // "Unhandled packet: %s (%u)", and nothing read
            return 1;
        }
        const int r = reinterpret_cast<int(__cdecl*)(void*, uint32_t, uint32_t, CDataStore*)>(it->second.m_handler)(
            it->second.m_param, static_cast<uint32_t>(opcode), a2, store);
        AscProfiler::Handled(opcode, start);
        return r;
    }

    void* __fastcall NetClientCtorDetour(void* self, void* edx)   // FUN_102c3a90
    {
        g_netClient = g_ctor(self, edx);
        return g_netClient;
    }
}

void CNetClient::ApplyPatches()
{
    g_setHandler = reinterpret_cast<SetHandler_t>(AscRuntime::Detour(0x631FA0, 6, reinterpret_cast<void*>(&SetMessageHandlerDetour)));
    g_process = reinterpret_cast<Process_t>(AscRuntime::Detour(0x631FE0, 10, reinterpret_cast<void*>(&ProcessMessageDetour)));
    const uint8_t bound[2] = {0xD4, 0x09};   // HOOKED 0x63200D: cmp esi, 0x51F -> 0x9D4
    Util::OverwriteBytesAtAddress(0x63200D, const_cast<uint8_t*>(bound), sizeof(bound));
    g_ctor = reinterpret_cast<Ctor_t>(AscRuntime::Detour(0x632A40, 5, reinterpret_cast<void*>(&NetClientCtorDetour)));
}

// The DLL registrar FUN_102c4590: FUN_102c4400 with the kept NetClient.
void CNetClient::RegisterHandler(uint32_t opcode, void* handler, void* param)
{
    SetMessageHandlerDetour(g_netClient, nullptr, opcode, handler, param);
}

void CNetClient::Packet_MSG_SET_ACTION_BUTTON(uint32_t slotID, bool p1, bool p2)
{
    reinterpret_cast<void (__cdecl*)(uint32_t, bool, bool)>(0x5AA390)(slotID, p1, p2);
}
