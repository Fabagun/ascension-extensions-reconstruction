#pragma once
// The original's ChallengeCreator ("trial") manager (FUN_10124310 -> static 0x10BDD210, ctor
// FUN_10120690). C_TrialCreator is AscTrialCreator.cpp; this is the part C_Challenge reads.

namespace AscTrialCreator
{
    // FUN_10122410 (+0xC): the community trial the player is in, as its id string; C_Challenge refuses
    // start / stop while it is non-empty (codes 0xF / 4).
    bool HasActiveTrial();
}
