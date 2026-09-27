#pragma once

#include <cstdint>

struct CDataStore;

struct CNetClientCustomPacket
{
    void* m_handler;
    void* m_param;

    CNetClientCustomPacket(void* handler, void* param) : m_handler(handler), m_param(param)
    {
    }
};

class CNetClient
{
public:
    // The original's packet-dispatch detours (installer FUN_102c3540); see CNetClient.cpp.
    static void ApplyPatches();
    // FUN_102c4590: register a handler the way the original's DLL does.
    static void RegisterHandler(uint32_t opcode, void* handler, void* param);

    // original packet wrappers
    static void Packet_MSG_SET_ACTION_BUTTON(uint32_t slotID, bool p1, bool p2);

private:
    CNetClient() = delete;
    ~CNetClient() = delete;
};
