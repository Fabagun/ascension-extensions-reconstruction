#pragma once
// The original's network profiler (see AscProfiler.cpp), for the receive path in CNetClient.cpp.
#include <cstdint>

namespace AscProfiler
{
    // FUN_102c3ab0's bookkeeping: with opcode logging on, count a received packet and return the
    // steady-clock start in ns (0 when logging is off); pass it to Handled once the handler returns.
    int64_t Received(int opcode, uint32_t size);
    // DAT_10BE3728: the (sign-extended) opcode of the packet being dispatched, stored by FUN_102c3ab0 for
    // every packet whether or not logging is on; it keeps the last value between packets.
    int32_t CurrentOpcode();
    void Handled(int opcode, int64_t start);
    // A packet above 0x55E nothing handles: logged to the Fatal channel.
    void Unhandled(int opcode);
}
