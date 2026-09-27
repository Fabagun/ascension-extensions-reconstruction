#pragma once
// Realm data hot-swap (see AscRealmData.cpp): the per-realm data directory under Data\.
#include <string>

namespace AscRealmData
{
    const std::string& CurrentPath();   // FUN_102f8920: 0x10BCC0A8
    // FUN_102fac20: start switching to `path` (no-op when it is already current or pending).
    void Switch(const std::string& path);
    bool HdPatchLoaded();               // DAT_10be357d: Data\patch-Q.MPQ was opened
}
