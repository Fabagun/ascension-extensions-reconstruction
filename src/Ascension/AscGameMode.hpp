#pragma once
#include <cstdint>

// The original's game-mode / per-specialization manager (0x10be4138). See AscGameMode.cpp.
namespace AscGameMode
{
    uint32_t Mode();            // +0x00
    uint32_t ActiveMask();      // +0x04 (tutorial / skill-card gating)
    bool SpecBuildDraft(int spec);   // FUN_1031efd0: spec record flags bit 0; false past 19
    int ActiveSpecIndex();      // FUN_1016f580, -1 when unknown
    void OnModeChanged(void (*cb)());   // run by SMSG_GAME_MODE_CHANGED before the event (FUN_1031bd50 slot)
}
