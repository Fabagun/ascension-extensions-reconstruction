#pragma once
#include <cstdint>

// GameEventMgr (FUN_100d5110 -> static, FUN_10256d50 ctor): std::map<u16 eventId, 0x34-byte record>
// filled by SMSG_GAME_EVENT_INFO 0x9BD. FUN_10256e20 is the query every other subsystem uses.
namespace AscGameEvents
{
    bool IsActive(uint16_t eventId);
    uint32_t ServerMaxLevel();    // FUN_102fc4d0 (realm info + level-cap events)
}
