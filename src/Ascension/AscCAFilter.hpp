#pragma once
// The character-advancement browser filters (klib filterable indexes over CharacterAdvancement.dbc):
// CharacterAdvancementMgr +0x28 / +0x190 / +0x2F8 and BuildCreator +0x228. See AscCAFilter.cpp.

#include <Ascension/AscCAMgr.hpp>
#include <string>

namespace AscCAFilter
{
    bool RowMatchesQuery(AscCA::Row r, const std::string& lowerQuery);   // FUN_101c1ce0 (gated)
    int RowQualityIndex(AscCA::Row r);                                    // FUN_1014e100(row +0x60), -1 none

    // CharacterAdvancementMgr::Update when the pending build's version moved (FUN_10182780): on the
    // mgr +0x190 container, FUN_101ca1d0 (adds to +0x120), FUN_101c4360 (clears +0x140) and
    // FUN_101ca500 (refills +0x140).
    void RebuildRecentSets();

    // FUN_101c9df0 / FUN_101c9f90 on one container (SMSG 0x64A inserting a row).
    void ResetEditor();                  // BuildCreator +0x228
    void ResetEntries();                 // mgr +0x2F8
    void ResetCategories(bool classes);  // mgr +0x190 when classes, else +0x28
}
