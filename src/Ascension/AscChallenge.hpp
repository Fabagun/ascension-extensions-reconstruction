#pragma once
// ChallengeMgr pieces other subsystems use (C_TrialCreator). See AscChallenge.cpp.
#include <cstdint>
#include <set>
#include <utility>
#include <vector>

struct lua_State;

namespace AscChallenge
{
    // FUN_10133dc0 / FUN_10134a80 with an ignore set; true when no (unignored) reason is found.
    bool CanActivate(uint32_t id, uint32_t level, const std::set<uint32_t>& ignore);
    bool CanDeactivate(uint32_t id, const std::set<uint32_t>& ignore);
    std::vector<uint32_t> ActiveIds();        // manager +0x2C list order

    const uint8_t* ChallengeRowOf(uint32_t id);   // Challenge.dbc row
    int Difficulty(const uint8_t* row);       // FUN_1011ef10 on +0x2C (CHALLENGE_DIFFICULTY_*, default 0)
    bool HasSoloRule(uint32_t id, uint32_t level);   // a rule of type CHALLENGE_RULES_TYPE_NO_GROUP
    // FUN_1013b350: some active challenge's current level carries a rule of this CHALLENGE_RULES_TYPE
    // index (an unknown name logs and counts as 0).
    bool ActiveHasRule(uint32_t type);

    // The trial serializer's merged level fields (FUN_101243b0 after its Challenges list): IsCompleted
    // (always false), Rewards (every level's computed rewards, concatenated), then Spells, Conditions,
    // Rules and Requirements each merged, sorted by row address and deduplicated. Into the table on top.
    void PushMergedLevels(lua_State* L, const std::vector<std::pair<uint32_t, uint32_t>>& levels, bool sortConditions);
}
