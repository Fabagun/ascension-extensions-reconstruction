#pragma once
#include <cstdint>

// Row lookup into DBFilesClient\CharacterAdvancement.dbc, which AscCharacterAdvancement.cpp already
// loads for C_CharacterAdvancement.GetAllEntries. Exposed so the C_CharacterAdvancement bindings can
// do real essence accounting -- AE/TE costs come from the same rows the tree UI draws from.
struct AscCAEntry
{
    uint32_t id = 0;
    uint32_t aeCost = 0;
    uint32_t teCost = 0;
    uint32_t level = 0;
    uint32_t group = 0;
    uint32_t classType = 0;
    uint32_t tab = 0;
    uint32_t spells[5] = {0, 0, 0, 0, 0};
    // Prerequisite / connection links. MEASURED 2026-09-23 at byte offsets 415, 419, 423, 427, 431
    // (3 mod 4 -- the record is a packed mixed-width structure, not a dword array, which is also why
    // the header claims 179 fields for a 173-dword row). Across every tree, 100% of the non-zero
    // values are ids of nodes IN THE SAME TREE: 3602/3604 and 1553/1555 for the first two.
    // The dword columns 2..4 previously used for this were wrong -- col 2 was only 64% in-tree,
    // col 3 9%, col 4 entirely empty -- so the connection graph and every prerequisite check built
    // on them were operating on bad data.
    uint32_t connected[5] = {0, 0, 0, 0, 0};
    uint32_t required[3] = {0, 0, 0};

    // Grid layout, MEASURED 2026-09-23. These are FLOATS at byte offsets 398/402/406/410 inside the
    // 692-byte record -- note 2 mod 4, i.e. NOT dword aligned, which is why a dword-indexed sweep of
    // the table never found them (col 100 read as an int is 16256 = 0x3F80, the upper half of 1.0f).
    // Validated across all 153 trees: every one yields small integral coordinates. Nodes may SHARE a
    // cell (choice nodes), so a uniqueness test is the wrong check -- that mistake is what produced
    // the earlier, wrong conclusion that the layout had to come from the server.
    float posX = 0.0f;
    float posY = 0.0f;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
};

bool AscCA_FindEntry(uint32_t id, AscCAEntry& out);
uint32_t AscCA_EntryCount();

// Every row for one class and tab, in row order -- the set the talent tree draws.
// tab < 0 means "any tab".
#include <vector>
void AscCA_CollectByClassTab(uint32_t classType, int tab, std::vector<AscCAEntry>& out);

// MEASURED 2026-09-23: the talent UI calls GetEntriesByClass with NAMES, not ids --
// CoATreeViewMixin:OnLoad resolves UnitClass("player") to a class file, turns it into "Necromancer"
// and calls SetClassTab(classDBC, "Class"). These resolve those names through the two tables that
// sit beside the main one, both shaped [0]=id, [1]=name.
//   CharacterAdvancementClassTypes.dbc : "Necromancer" -> 25   (matches col 32)
//   CharacterAdvancementTabTypes.dbc   : "Class"       -> 87   (matches col 33)
// Return 0 when the name is unknown.
uint32_t AscCA_ClassIdByName(const char* name);
uint32_t AscCA_TabIdByName(const char* name);

// CharacterAdvancementEssence.dbc -- the per-level essence budget. Same 9-column shape as the
// SMSG_CA_ESSENCE_BUDGET (0x722) packet, which our server never sends, so this is where the budget
// has to come from: [1] level, [2] classType, [7] abilityEssence, [8] talentEssence. Necromancer
// (class 25) at level 80 is AE 36 / TE 35.
bool AscCA_EssenceForLevel(uint32_t classType, uint32_t level, uint32_t& ae, uint32_t& te);

// Tab id -> tab name, the inverse of AscCA_TabIdByName. Needed to derive the active specialization
// from the entries the character has actually learned.
const char* AscCA_TabNameById(uint32_t id);
