#pragma once
#include <cstdint>
#include <string>

// The rapid-rolling session of the WildcardMgr (FUN_10a349d0), AscWildcardRapid.cpp.
namespace AscWildcardRapid
{
    bool Active();              // FUN_10a34ce0: phase 1..5
    bool InFlight();            // FUN_10a34cb0: a cancel pending or phase 1..3
    void MarkRerollPending();   // FUN_10a2f910: an unlearn request (CMSG 0x61D) is outstanding
    bool TakeRerollPending();   // read and clear it (SMSG 0x61E, the glue reset)
    // FUN_10a33b10 (world entry): the session back to its default (stop code STOP_RAPID_ROLLING_NONE) and
    // the cancel phase (+0x270) to 0. The record at +0x274 it also resets is not modelled here.
    void ResetSession();

    // The session's part of AscWildcardRolls.cpp's handlers: true when the session consumed it.
    bool OnUnlearnResult(const std::string& result);   // FUN_10a35c20 (SMSG 0x61E)
    bool OnRollResult(const std::string& result);      // FUN_10a359c0 (SMSG 0x644)
    void OnEntryLearned(uint32_t entry, uint32_t newRank, uint32_t preRoll, const std::string& stop);   // FUN_10a355d0
    void OnRevealDone(uint32_t entry);                 // FUN_10a35870
}

// The mode manager's roll-ready poll (AscWildcardRolls.cpp).
namespace AscWildcardRolls
{
    void ArmPoll();   // FUN_10a31760: 50 ms polls for a second, then 1 s; WILDCARD_ROLL_READY on the edge
}
