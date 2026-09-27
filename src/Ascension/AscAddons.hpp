#pragma once
// SecureAddonMgr (FUN_10291ed0 -> 0x10BE3044), filled by SMSG 0x94E in AscGlobalsJ.cpp.

namespace AscAddons
{
    // FUN_10312850: the secure flag of the first known addon with this exact name; false if unknown.
    bool IsSecure(const char* name);
    // FUN_10311c00 + FUN_103114f0: the name is on the server's list at all (exact match).
    bool IsKnown(const char* name);
    // 0x10BE3F60: set while the client's 0x510B30 runs (AscGlobalsJ's detour).
    bool InsideProtectedCall();
}
