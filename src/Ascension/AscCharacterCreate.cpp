// C_CharacterCreate, data half: the realm creation gates and the archetype / class-guide lookups over
// the CharacterCreation*.dbc tables. The creation manager (static 0x10BDB700, accessor FUN_100d5070,
// ctor FUN_100d23e0) keeps sorted indexes built on first use -- FUN_1018bd40 for archetypes (+0x2CC
// dirty), FUN_1018c240 for the class guide (+0x31C dirty) -- and the preview camera at +0x398..+0x3A0.
//
//   ArchetypeRoles        +0x48 str, +0x4C/+0x50/+0x54 loc, +0x58 order
//   ArchetypeCategories   +4 role, +0x4C/+0x50 str, +0x54/+0x58/+0x5C loc, +0x60 order
//   Archetypes            +4 category, +8 str, +0xC..+0x1C five spells, +0x20 str, +0x24/+0x30 three
//                         strings each, +0x3C..+0x48 str, +0x4C/+0x50/+0x54 loc, +0x58..+0x68 the five
//                         spells' descriptions (loc), +0x6C str, +0x70 order
//   ClassGuideRoles       +4/+8 str, +0xC/+0x10 loc, +0x14 order
//   ClassGuideSubroles    +4 role, +8/+0xC/+0x10 str, +0x14/+0x18 loc, +0x1C order
//   ClassGuideSubroleClasses +4 subrole, +8 class, +0xC order
//
// The preview half drives the glue character component (*0xB6B1A0: +0x18 race, +0x1C sex, +0x28 the
// customization handle, +0x38 its CM2Model) from three override layers of 0xB0 bytes at +0x14:
//   layer 0  rebuilt by FUN_1018c8e0 from the selected archetype (FUN_1018a330) or the previewed class
//            outfit (FUN_1018aa90) -- CharacterCreation{ArchetypeRoles,ArchetypeCategories,
//            ArchetypeDetails,ClassDetails,ShapeshiftDetails}.dbc
//   layer 1  what the bindings set (read FUN_1018b000, written FUN_1018f850)
//   layer 2  nothing in the plain code writes it; merged all the same
// FUN_1018d080 merges them in order into +0x224; FUN_1018c9d0 stores the merge, applies it to the
// model (FUN_101890d0) and fires CHARACTER_CREATE_PREVIEW_CHANGED when it differs from the last one.
// Module init FUN_1018b290 also replaces the glue's SetCharacterCreateFacing / GetCharacterCreateFacing
// (0x4E0C10 / 0x4E0BE0) and detours the character-creation calls that change the model (race 0x4E20B0,
// sex 0x4E1540, class 0x4E1740, randomize 0x4E17F0) and the camera lookup 0x4C1290.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscDbcPatch.hpp>
#include <Ascension/AscModelKit.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    AscDbc::Table& Roles()            { return AscDbc::Get("DBFilesClient\\CharacterCreationArchetypeRoles.dbc"); }
    AscDbc::Table& Categories()       { return AscDbc::Get("DBFilesClient\\CharacterCreationArchetypeCategories.dbc"); }
    AscDbc::Table& Archetypes()       { return AscDbc::Get("DBFilesClient\\CharacterCreationArchetypes.dbc"); }
    AscDbc::Table& GuideRoles()       { return AscDbc::Get("DBFilesClient\\CharacterCreationClassGuideRoles.dbc"); }
    AscDbc::Table& GuideSubroles()    { return AscDbc::Get("DBFilesClient\\CharacterCreationClassGuideSubroles.dbc"); }
    AscDbc::Table& GuideSubroleClasses() { return AscDbc::Get("DBFilesClient\\CharacterCreationClassGuideSubroleClasses.dbc"); }

    uint32_t U32(const uint8_t* r, uint32_t off) { return AscDbc::Table::U32(r, off); }

    using AscModelKit::Transform;
    const Transform kIdentity = {{0, 0, 0}, {0, 0, 0}, 1.0f};   // FUN_1018b590

    template <class T> struct Opt { T v{}; bool set = false; };
    struct Vec3 { float v[3]; };

    // One override layer (0xB0). Offsets are the layer's own; the merged field follows in brackets.
    struct Layer
    {
        Opt<uint32_t> spell;                 // +0x00 / +0x04  [+0x224] state-kit spell on the character
        Opt<uint32_t> anim;                  // +0x08 / +0x0C  [+0x228]
        Opt<uint32_t> petCreature;           // +0x10 / +0x14  [+0x22C] creature entry of the pet model
        Opt<uint32_t> shapeshift;            // +0x18 / +0x1C  [+0x230] creature entry of the shapeshift model
        Opt<uint32_t> petDisplay;            // +0x20 / +0x24  [+0x234] CreatureDisplayInfo of the pet model
        Opt<bool> weaponsDrawn;              // +0x28 / +0x29  [+0x238]
        Opt<std::vector<uint32_t>> items;    // +0x2C / +0x38  [+0x23C]
        Opt<uint32_t> unk248;                // +0x3C / +0x40  [+0x248] (see ApplyToModel)
        Opt<Transform> character;            // +0x44 / +0x60  [+0x24C / +0x250]
        Opt<Transform> shape;                // +0x64 / +0x80  [+0x26C / +0x270]
        Opt<Vec3> camera;                    // +0x84 / +0x90  [+0x28C / +0x290]
        Opt<float> facing;                   // +0x94 / +0x98  [+0x29C / +0x2A0]
        Opt<bool> undress;                   // +0x9C / +0x9D  [+0x2A4 / +0x2A5]
        Opt<uint32_t> cast;                  // +0xA0 / +0xA4  [+0x2A8]
        Opt<uint32_t> petCast;               // +0xA8 / +0xAC  [+0x2AC]
    };

    struct Merged   // +0x224 (0x8C), defaults as FUN_1018d080 builds them
    {
        uint32_t spell = 0, anim = 0, petCreature = 0, shapeshift = 0, petDisplay = 0;
        bool weaponsDrawn = false;
        std::vector<uint32_t> items;
        uint32_t unk248 = 0;
        bool hasCharacter = false;
        Transform character = kIdentity;
        bool hasShape = false;
        Transform shape = kIdentity;
        bool hasCamera = false;
        float camera[3] = {0, 0, 0};
        bool hasFacing = false;
        float facing = 0;
        bool hasUndress = false;
        bool undress = false;
        uint32_t cast = 0, petCast = 0;
    };

    struct Manager
    {
        uint8_t mode = 0;                                             // +0x05: 0 none, 1 archetype, 2 class outfit
        uint32_t selection[3] = {0, 0, 0};                            // +0x08 role, +0x0C category, +0x10 archetype
        Layer layers[3];                                              // +0x14
        Merged merged;                                                // +0x224
        uint32_t classPreview = 0;                                    // +0x2B0
        std::string classPreviewName;                                 // +0x2B4
        bool archetypesDirty = true;                                  // +0x2CC
        std::vector<uint32_t> roles;                                  // +0x2D0
        std::unordered_map<uint32_t, std::vector<uint32_t>> categoriesByRole;   // +0x2D8
        std::unordered_map<uint32_t, std::vector<uint32_t>> archetypesByCategory; // +0x2F8
        bool guideDirty = true;                                       // +0x31C
        std::vector<uint32_t> guideRoles;                             // +0x320
        std::unordered_map<uint32_t, std::vector<uint32_t>> subrolesByRole;      // +0x32C
        std::unordered_map<uint32_t, std::vector<uint32_t>> classesBySubrole;    // +0x34C
        bool classNamesDirty = true;                                  // +0x36C
        std::unordered_map<std::string, uint32_t> classByName;        // +0x370: upper-case ChrClasses filename -> id
        void* petModel = nullptr;                                     // +0x390
        void* shapeModel = nullptr;                                   // +0x394
        float camera[3] = {0, 0, 0};                                  // +0x398: the camera the glue last asked for
        float zoomOrigin[3] = {0, 0, 0};                              // +0x3A4
        Opt<uint32_t> race;                                           // +0x3B0 / +0x3B4
        Opt<uint32_t> sex;                                            // +0x3B8 / +0x3BC
        bool hadComponent = false;                                    // +0x3C0
    } g_mgr;

    // Every row id of a table in id order.
    template <class Fn> void ForEachRow(AscDbc::Table& t, Fn fn)
    {
        if (!t.Loaded())
            return;
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
                fn(id, r);
    }

    void SortBy(AscDbc::Table& t, std::vector<uint32_t>& ids, uint32_t orderOff, bool tieById)
    {
        std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) {
            const uint32_t oa = U32(t.Row(a), orderOff), ob = U32(t.Row(b), orderOff);
            if (tieById && oa == ob)
                return a < b;
            return oa < ob;
        });
    }

    void BuildArchetypes()   // FUN_1018bd40
    {
        g_mgr.roles.clear();
        g_mgr.categoriesByRole.clear();
        g_mgr.archetypesByCategory.clear();
        ForEachRow(Roles(), [](uint32_t id, const uint8_t*) { g_mgr.roles.push_back(id); });
        SortBy(Roles(), g_mgr.roles, 0x58, false);   // FUN_10187c20
        ForEachRow(Categories(), [](uint32_t id, const uint8_t* r) { g_mgr.categoriesByRole[U32(r, 4)].push_back(id); });
        for (auto& kv : g_mgr.categoriesByRole)
            SortBy(Categories(), kv.second, 0x60, false);   // FUN_10188110
        ForEachRow(Archetypes(), [](uint32_t id, const uint8_t* r) { g_mgr.archetypesByCategory[U32(r, 4)].push_back(id); });
        for (auto& kv : g_mgr.archetypesByCategory)
            SortBy(Archetypes(), kv.second, 0x70, false);   // FUN_10188600
        g_mgr.archetypesDirty = false;
    }

    void BuildClassGuide()   // FUN_1018c240
    {
        g_mgr.guideRoles.clear();
        g_mgr.subrolesByRole.clear();
        g_mgr.classesBySubrole.clear();
        ForEachRow(GuideRoles(), [](uint32_t id, const uint8_t*) { g_mgr.guideRoles.push_back(id); });
        SortBy(GuideRoles(), g_mgr.guideRoles, 0x14, true);   // FUN_10187e90
        ForEachRow(GuideSubroles(), [](uint32_t id, const uint8_t* r) { g_mgr.subrolesByRole[U32(r, 4)].push_back(id); });
        for (auto& kv : g_mgr.subrolesByRole)
            SortBy(GuideSubroles(), kv.second, 0x1C, true);   // FUN_10188380
        std::unordered_map<uint32_t, std::vector<const uint8_t*>> rows;   // subrole -> rows
        ForEachRow(GuideSubroleClasses(), [&](uint32_t, const uint8_t* r) { rows[U32(r, 4)].push_back(r); });
        for (auto& kv : rows)
        {
            std::sort(kv.second.begin(), kv.second.end(), [](const uint8_t* a, const uint8_t* b) {   // FUN_10188870
                if (U32(a, 0xC) == U32(b, 0xC))
                    return U32(a, 0) < U32(b, 0);
                return U32(a, 0xC) < U32(b, 0xC);
            });
            std::vector<uint32_t>& classes = g_mgr.classesBySubrole[kv.first];
            for (const uint8_t* r : kv.second)
                classes.push_back(U32(r, 8));
        }
        g_mgr.guideDirty = false;
    }

    void PushIds(lua_State* L, const std::vector<uint32_t>& v)   // FUN_1009a460
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            AscLua::lua_pushinteger(L, static_cast<int32_t>(v[i]));
            AscLua::lua_settable(L, -3);
        }
    }

    int PushList(lua_State* L, std::unordered_map<uint32_t, std::vector<uint32_t>>& map)
    {
        uint32_t key;
        if (!ReadNumber(L, key))
            return 0;
        auto it = map.find(key);
        PushIds(L, it == map.end() ? std::vector<uint32_t>() : it->second);
        return 1;
    }

    void PushStrs(lua_State* L, AscDbc::Table& t, const uint8_t* r, std::initializer_list<uint32_t> offs)
    {
        for (uint32_t off : offs)
            AscLua::lua_pushstring(L, t.Str(r, off));
    }

    int Nils(lua_State* L, int n)
    {
        for (int i = 0; i < n; ++i)
            AscLua::lua_pushnil(L);
        return n;
    }

    // ---- gates (RealmInfo +0x44 dev, +0x46 CoA, +0x47 wildcard) ------------------------------------------
    bool Dev()      { return RealmInfoSvc::Get().gates[4] != 0; }   // FUN_102fc640
    bool CoAGate()  { return RealmInfoSvc::Get().gates[6] != 0; }   // FUN_102fc630
    bool WCRGate()  { return RealmInfoSvc::Get().gates[7] != 0; }   // FUN_102fc6a0

    bool CoAAllowed() { return CoAGate() || Dev(); }   // FUN_1018ae90
    bool WCRAllowed() { return WCRGate() || Dev(); }   // FUN_1018aec0

    int CanCreateCoA(lua_State* L)  { PushBool(L, CoAAllowed()); return 1; }
    int CanCreateHero(lua_State* L) { PushBool(L, !CoAGate() && !WCRGate()); return 1; }
    int CanCreateWCR(lua_State* L)  { PushBool(L, WCRAllowed()); return 1; }

    // FUN_1018d550: Hero (10) where neither CoA nor wildcard is on; the 21 CoA classes where CoA is
    // allowed; the stock classes except Death Knight where wildcard is allowed.
    int CanCreateClass(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER}))
            return 0;
        const uint32_t cls = static_cast<uint32_t>(ToInt(CheckNumber(L, 1))) & 0xFF;
        bool ok = false;
        if (cls <= 0x20)
        {
            if (cls == 10 && !CoAGate() && !WCRGate())
                ok = true;
            else if (cls >= 12 && cls <= 32)
                ok = CoAAllowed();
            else if (cls >= 1 && cls <= 11 && cls != 6 && cls != 10)
                ok = WCRAllowed();
        }
        PushBool(L, ok);
        return 1;
    }

    // ---- archetypes ------------------------------------------------------------------------------------
    int GetArchetypeRoles(lua_State* L)   // FUN_1018deb0
    {
        if (g_mgr.archetypesDirty)
            BuildArchetypes();
        PushIds(L, g_mgr.roles);
        return 1;
    }

    int GetArchetypeCategories(lua_State* L)   // FUN_1018d980 (roleId)
    {
        if (g_mgr.archetypesDirty)
            BuildArchetypes();
        return PushList(L, g_mgr.categoriesByRole);
    }

    int GetArchetypes(lua_State* L)   // FUN_1018e080 (categoryId)
    {
        if (g_mgr.archetypesDirty)
            BuildArchetypes();
        return PushList(L, g_mgr.archetypesByCategory);
    }

    int GetArchetypeRoleInfo(lua_State* L)   // handler_GetArchetypeRoleInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* r = Roles().Row(id);
        if (!r)
            return Nils(L, 4);
        PushStrs(L, Roles(), r, {0x48, 0x4C, 0x50, 0x54});
        return 4;
    }

    int GetArchetypeCategoryInfo(lua_State* L)   // handler_GetArchetypeCategoryInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* r = Categories().Row(id);
        if (!r)
            return Nils(L, 5);
        PushStrs(L, Categories(), r, {0x4C, 0x50, 0x54, 0x58, 0x5C});
        return 5;
    }

    // FUN_1018dbb0: name, {spells}, primary stat, {weapon types}, {armor types}, icon, three strings, the
    // three localized texts, cinematic.
    int GetArchetypeInfo(lua_State* L)
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        AscDbc::Table& t = Archetypes();
        const uint8_t* r = t.Row(id);
        if (!r)
            return Nils(L, 13);
        AscLua::lua_pushstring(L, t.Str(r, 8));
        AscLua::lua_createtable(L, 0, 5);
        AscLua::lua_checkstack(L, 2);
        for (uint32_t i = 0; i < 5; ++i)
            if (const uint32_t spell = U32(r, 0xC + i * 4))
            {
                AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
                AscLua::lua_pushinteger(L, static_cast<int32_t>(spell));
                AscLua::lua_settable(L, -3);
            }
        AscLua::lua_pushstring(L, t.Str(r, 0x20));
        for (uint32_t base : {0x24u, 0x30u})   // FUN_100d1de0
        {
            AscLua::lua_createtable(L, 0, 3);
            AscLua::lua_checkstack(L, 2);
            for (uint32_t i = 0; i < 3; ++i)
            {
                AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
                AscLua::lua_pushstring(L, t.Str(r, base + i * 4));
                AscLua::lua_settable(L, -3);
            }
        }
        PushStrs(L, t, r, {0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x6C});
        return 13;
    }

    // FUN_1018df30: (archetypeId, spellId) -> that spell's description, nil when empty or absent.
    int GetArchetypeSpellDescription(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER}))
            return 0;
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        AscDbc::Table& t = Archetypes();
        if (const uint8_t* r = t.Row(id))
            for (uint32_t i = 0; i < 5; ++i)
                if (U32(r, 0xC + i * 4) == spell)
                {
                    const char* s = t.Str(r, 0x58 + i * 4);
                    if (*s)
                    {
                        AscLua::lua_pushstring(L, s);
                        return 1;
                    }
                    break;
                }
        AscLua::lua_pushnil(L);
        return 1;
    }

    // ---- class guide -----------------------------------------------------------------------------------
    int GetClassGuideRoles(lua_State* L)   // FUN_1018e390
    {
        if (g_mgr.guideDirty)
            BuildClassGuide();
        PushIds(L, g_mgr.guideRoles);
        return 1;
    }

    int GetClassGuideSubroles(lua_State* L)   // FUN_1018e650 (roleId)
    {
        if (g_mgr.guideDirty)
            BuildClassGuide();
        return PushList(L, g_mgr.subrolesByRole);
    }

    int GetClassGuideSubroleClasses(lua_State* L)   // FUN_1018e410 (subroleId)
    {
        if (g_mgr.guideDirty)
            BuildClassGuide();
        return PushList(L, g_mgr.classesBySubrole);
    }

    int GetClassGuideRoleInfo(lua_State* L)   // handler_GetClassGuideRoleInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* r = GuideRoles().Row(id);
        if (!r)
            return Nils(L, 4);
        PushStrs(L, GuideRoles(), r, {4, 8, 0xC, 0x10});
        return 4;
    }

    int GetClassGuideSubroleInfo(lua_State* L)   // handler_GetClassGuideSubroleInfo
    {
        uint32_t id;
        if (!ReadNumber(L, id))
            return 0;
        const uint8_t* r = GuideSubroles().Row(id);
        if (!r)
            return Nils(L, 6);
        PushInt(L, static_cast<int32_t>(U32(r, 4)));
        PushStrs(L, GuideSubroles(), r, {8, 0xC, 0x10, 0x14, 0x18});
        return 6;
    }

    int GetCameraPosition(lua_State* L)   // handler_GetCameraPosition
    {
        for (float v : g_mgr.camera)
            AscLua::lua_pushnumber(L, static_cast<double>(v));
        return 3;
    }

    // ==== preview ========================================================================================
    const float kDegToRad = 0.017453292f;          // 0x10B2CBF8
    const float kRadToDeg = 57.2957763671875f;     // 0x10B38974

    uint8_t* Component() { return *reinterpret_cast<uint8_t**>(0xB6B1A0); }   // *DAT_10bc952c
    uint32_t ComponentU32(uint8_t* c, uint32_t off) { return *reinterpret_cast<uint32_t*>(c + off); }
    void* ModelOf(uint8_t* c) { return *reinterpret_cast<void**>(c + 0x38); }
    void* Ptr(void* p, uint32_t off) { return *reinterpret_cast<void**>(static_cast<uint8_t*>(p) + off); }
    float F32(const uint8_t* r, uint32_t off) { return AscDbc::Table::F32(r, off); }

    AscDbc::Table& ArchetypeDetails()  { return AscDbc::Get("DBFilesClient\\CharacterCreationArchetypeDetails.dbc"); }
    AscDbc::Table& ClassDetails()      { return AscDbc::Get("DBFilesClient\\CharacterCreationClassDetails.dbc"); }
    AscDbc::Table& PetDetails()        { return AscDbc::Get("DBFilesClient\\CharacterCreationPetDetails.dbc"); }
    AscDbc::Table& ShapeshiftDetails() { return AscDbc::Get("DBFilesClient\\CharacterCreationShapeshiftDetails.dbc"); }

    // The client's own tables (WowClientDB containers).
    const uint8_t* ItemRow(uint32_t id)             { return ClientDbcRow(kItemDbc, id); }     // FUN_100b1870
    const uint8_t* CreatureDisplayInfo(uint32_t id) { return ClientDbcRow(0xAD34B8, id); }     // FUN_100b1420
    const uint8_t* CreatureModelData(uint32_t id)   { return ClientDbcRow(0xAD3500, id); }     // FUN_100b14b0
    const uint32_t kChrClasses = 0xAD3404;                                                     // FUN_100b13c0

    // FUN_100b8c00: the creature record's display id (+0xC), 0 when there is no such creature.
    uint32_t CreatureDisplayId(uint32_t entry)
    {
        const AscBridgeStore::CreatureRecord* r = AscBridgeStore::CreatureByEntry(entry);
        return r ? r->displayId : 0;
    }

    // FUN_101ccf70 / FUN_101cd890 / FUN_101cdbb0 / FUN_101cdda0: the first row, in id order, whose columns
    // 1.. equal the keys.
    const uint8_t* FindRow(AscDbc::Table& t, std::initializer_list<uint32_t> keys)
    {
        if (!t.Loaded())
            return nullptr;
        for (uint32_t id = t.MinId(); id <= t.MaxId(); ++id)
            if (const uint8_t* r = t.Row(id))
            {
                uint32_t off = 4;
                bool match = true;
                for (uint32_t k : keys)
                {
                    match = match && U32(r, off) == k;
                    off += 4;
                }
                if (match)
                    return r;
            }
        return nullptr;
    }

    Transform TransformAt(const uint8_t* r, uint32_t off)   // pos, rotation in degrees, scale
    {
        Transform t;
        for (int i = 0; i < 3; ++i)
        {
            t.pos[i] = F32(r, off + i * 4);
            t.rot[i] = F32(r, off + 0xC + i * 4) * kDegToRad;
        }
        t.scale = F32(r, off + 0x18);
        return t;
    }

    template <class T> void Set(Opt<T>& o, const T& v) { o.v = v; o.set = true; }

    // The five preview columns every source row carries at `base`: spell, animation, pet creature,
    // shapeshift creature, weapons-drawn flag, then twelve item ids.
    void FillCommon(Layer& l, const uint8_t* r, uint32_t base)
    {
        Set(l.spell, U32(r, base));
        Set(l.anim, U32(r, base + 4));
        Set(l.petCreature, U32(r, base + 8));
        Set(l.shapeshift, U32(r, base + 0xC));
        Set(l.weaponsDrawn, U32(r, base + 0x10) != 0);
        const uint32_t* items = reinterpret_cast<const uint32_t*>(r + base + 0x14);
        Set(l.items, std::vector<uint32_t>(items, items + 12));
    }

    // ShapeshiftDetails (creature, race): +0xC the shapeshift model's transform, +0x28 its spell, +0x2C its
    // animation; +0x30 the character's transform, +0x4C / +0x50 the character's spell / animation.
    bool ApplyShapeshift(Layer& l)
    {
        if (!l.shapeshift.set || l.shapeshift.v == 0)
            return false;
        uint8_t* c = Component();
        const uint8_t* s = FindRow(ShapeshiftDetails(), {l.shapeshift.v, c ? ComponentU32(c, 0x18) : 0});
        if (!s)
            return false;
        Set(l.spell, U32(s, 0x4C));
        Set(l.anim, U32(s, 0x50));
        Set(l.character, TransformAt(s, 0x30));
        Set(l.shape, TransformAt(s, 0xC));
        return true;
    }

    void RaceSex(uint32_t& race, uint32_t& sex)
    {
        uint8_t* c = Component();
        race = c ? ComponentU32(c, 0x18) : 0;
        sex = c ? ComponentU32(c, 0x1C) : 2;
    }

    // FUN_1018a330: archetype (details per race / sex), else category, else role.
    Layer ArchetypeLayer()
    {
        Layer l;
        const uint8_t* details = nullptr;
        if (const uint32_t archetype = g_mgr.selection[2])
        {
            if (Archetypes().Row(archetype))
            {
                uint32_t race, sex;
                RaceSex(race, sex);
                details = FindRow(ArchetypeDetails(), {archetype, race, sex});
                if (details)
                    FillCommon(l, details, 0x2C);
            }
        }
        else if (const uint32_t category = g_mgr.selection[1])
        {
            const uint8_t* r = Categories().Row(category);
            if (!r)
                return l;
            FillCommon(l, r, 8);
            Set(l.character, kIdentity);
        }
        else if (const uint32_t role = g_mgr.selection[0])
        {
            const uint8_t* r = Roles().Row(role);
            if (!r)
                return l;
            FillCommon(l, r, 4);
            Set(l.character, kIdentity);
        }
        if (ApplyShapeshift(l))
            return l;
        if (details)
            Set(l.character, TransformAt(details, 0x10));
        return l;
    }

    // FUN_1018aa90: the previewed class's ClassDetails row for this race / sex.
    Layer ClassLayer()
    {
        Layer l;
        if (g_mgr.classPreview == 0)
            return l;
        uint32_t race, sex;
        RaceSex(race, sex);
        const uint8_t* d = FindRow(ClassDetails(), {g_mgr.classPreview, race, sex});
        if (!d)
            return l;
        FillCommon(l, d, 0x2C);
        if (ApplyShapeshift(l))
            return l;
        Set(l.character, TransformAt(d, 0x10));
        return l;
    }

    void RebuildLayer0()   // FUN_1018c8e0
    {
        if (g_mgr.mode == 1)
            g_mgr.layers[0] = ArchetypeLayer();
        else if (g_mgr.mode == 2)
            g_mgr.layers[0] = ClassLayer();
        else
            g_mgr.layers[0] = Layer();
    }

    Merged Merge()   // FUN_1018d080
    {
        Merged m;
        for (const Layer& l : g_mgr.layers)
        {
            if (l.spell.set) m.spell = l.spell.v;
            if (l.anim.set) m.anim = l.anim.v;
            if (l.petCreature.set) m.petCreature = l.petCreature.v;
            if (l.shapeshift.set) m.shapeshift = l.shapeshift.v;
            if (l.petDisplay.set) m.petDisplay = l.petDisplay.v;
            if (l.weaponsDrawn.set) m.weaponsDrawn = l.weaponsDrawn.v;
            if (l.items.set) m.items = l.items.v;
            if (l.unk248.set) m.unk248 = l.unk248.v;
            if (l.character.set) { m.hasCharacter = true; m.character = l.character.v; }
            if (l.shape.set) { m.hasShape = true; m.shape = l.shape.v; }
            if (l.camera.set)
            {
                // An all-zero camera clears the override and leaves the last coordinates in place.
                const float* v = l.camera.v.v;
                if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f)
                    m.hasCamera = false;
                else
                {
                    m.hasCamera = true;
                    std::copy(v, v + 3, m.camera);
                }
            }
            if (l.facing.set) { m.hasFacing = true; m.facing = l.facing.v; }
            if (l.undress.set) { m.hasUndress = true; m.undress = l.undress.v; }
            if (l.cast.set) m.cast = l.cast.v;
            if (l.petCast.set) m.petCast = l.petCast.v;
        }
        return m;
    }

    bool SameTransform(const Transform& a, const Transform& b)   // FUN_1018a2a0
    {
        for (int i = 0; i < 3; ++i)
            if (a.pos[i] != b.pos[i] || a.rot[i] != b.rot[i])
                return false;
        return a.scale == b.scale;
    }

    bool Same(const Merged& a, const Merged& b)   // FUN_1018a0f0
    {
        if (a.spell != b.spell || a.anim != b.anim || a.petCreature != b.petCreature || a.shapeshift != b.shapeshift
            || a.petDisplay != b.petDisplay || a.weaponsDrawn != b.weaponsDrawn || a.items != b.items || a.unk248 != b.unk248)
            return false;
        if (a.hasCharacter != b.hasCharacter || (a.hasCharacter && !SameTransform(a.character, b.character)))
            return false;
        if (a.hasShape != b.hasShape || (a.hasShape && !SameTransform(a.shape, b.shape)))
            return false;
        if (a.hasCamera != b.hasCamera
            || (a.hasCamera && (a.camera[0] != b.camera[0] || a.camera[1] != b.camera[1] || a.camera[2] != b.camera[2])))   // FUN_1018a0b0
            return false;
        if (a.hasFacing != b.hasFacing || (a.hasFacing && a.facing != b.facing))
            return false;
        if (a.hasUndress != b.hasUndress || (a.hasUndress && a.undress != b.undress))
            return false;
        return a.cast == b.cast && a.petCast == b.petCast;
    }

    // FUN_1018d3a0: the character's and the shapeshift model's transforms; the facing turns the
    // shapeshift model when there is one, else the character.
    void Transforms(const Merged& m, Transform& character, Transform& shape)
    {
        character = m.hasCharacter ? m.character : kIdentity;
        shape = m.hasShape ? m.shape : kIdentity;
        if (m.hasFacing)
        {
            if (m.shapeshift != 0)
                shape.rot[2] = m.facing;
            else
                character.rot[2] = m.facing;
        }
    }

    void Release(void* model) { reinterpret_cast<void(__thiscall*)(void*)>(0x8274F0)(model); }
    void* CreateModel(void* scene, const char* path)   // CM2Scene 0x81F8F0
    {
        return reinterpret_cast<void*(__thiscall*)(void*, const char*, int)>(0x81F8F0)(scene, path, 0);
    }
    void AttachTo(void* model, void* parent, uint32_t attach)   // 0x831630
    {
        reinterpret_cast<void(__thiscall*)(void*, void*, uint32_t, int, int)>(0x831630)(model, parent, attach, 0, 0);
    }
    void DetachAll(void* model)   // FUN_103081f0: attachment points -1 .. 0x3F (0x10B5B130)
    {
        for (int32_t a = -1; a <= 0x3F; ++a)
            AscModelKit::Detach(model, static_cast<uint32_t>(a));
    }
    void PlayStand(void* model) { AscModelKit::PlayAnim(model, 0); }   // FUN_103081c0

    // FUN_10308220: the character model's attachments, then 0x4EEB30 on every one of the component's
    // 0x1D item slots (0x10B5B238).
    void ClearComponent(uint8_t* c)
    {
        DetachAll(ModelOf(c));
        for (uint32_t i = 0; i <= 0x1C; ++i)
            reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x4EEB30)(c, i);
    }

    // FUN_1018b990: the spell's precast kit hand effects on attachment points 0x15 / 0x16 and its
    // animation -- the cast animation of PlayCastAnimation / PlayPetCastAnimation.
    bool CastVisual(void* model, uint32_t spell)
    {
        if (!model || !spell)
            return false;
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return false;
        const uint8_t* v = ClientDbcRow(0xAD4AA8, U32(rec, 0x20C));   // SpellVisual, FUN_100b2620
        if (!v)
            return false;
        const uint8_t* kit = ClientDbcRow(0xAD4A3C, U32(v, 4));       // SpellVisualKit, FUN_100b2680
        if (!kit)
            return false;
        AscModelKit::Detach(model, 0x15);
        AscModelKit::Detach(model, 0x16);
        void* scene = Ptr(model, 0x28);
        if (!scene)
            return false;
        static const uint32_t kHands[][2] = {{0x18, 0x15}, {0x1C, 0x16}};   // kit column, attachment point
        for (const auto& h : kHands)
            if (const uint32_t effect = U32(kit, h[0]))
                if (const uint8_t* e = ClientDbcRow(0xAD4A18, effect))           // SpellVisualEffectName, FUN_100b2650
                    if (void* fx = CreateModel(scene, *reinterpret_cast<const char* const*>(e + 8)))
                        AttachTo(fx, model, h[1]);
        AscModelKit::PlayAnim(model, U32(kit, 8));
        return true;
    }

    // Whether a live preview model already shows this CreatureModelData file (FUN_10089a60: an exact,
    // case-sensitive compare against the model's M2 name at [+0x2C] + 0x3C).
    bool ShowsFile(void* model, const char* file)
    {
        const char* name = reinterpret_cast<const char*>(static_cast<uint8_t*>(Ptr(model, 0x2C)) + 0x3C);
        return std::string(name) == file;
    }

    // The pet (+0x390) / shapeshift (+0x394) model: kept when it already shows `modelData`'s file,
    // otherwise released and recreated in the character's scene and attached under [model + 0x48].
    // Returns the model (or nullptr); `fresh` tells whether it was just created.
    void* Refresh(void*& slot, void* scene, const uint8_t* display, const uint8_t* modelData, void* character)
    {
        const char* file = *reinterpret_cast<const char* const*>(modelData + 8);
        bool fresh = false;
        if (slot)
        {
            fresh = !ShowsFile(slot, file);
            if (fresh)
            {
                Release(slot);
                slot = nullptr;
            }
        }
        else
            fresh = true;
        if (fresh)
            slot = CreateModel(scene, file);
        if (!slot)
            return nullptr;
        reinterpret_cast<void(__cdecl*)(void*, const uint8_t*, const uint8_t*)>(0x4F20C0)(slot, display, modelData);
        if (fresh)
            reinterpret_cast<void(__thiscall*)(void*, void*, int, int, int)>(0x831630)(slot, Ptr(character, 0x48), 0, 0, 0);
        DetachAll(slot);
        PlayStand(slot);
        return slot;
    }

    void PlayDetailsKit(void* model, const uint8_t* r)   // +0x28 spell, +0x2C animation (-1 = none)
    {
        const bool anim = U32(r, 0x2C) != 0xFFFFFFFF;
        if (!AscModelKit::PlayStateKit(model, U32(r, 0x28), anim))
            AscModelKit::PlayPrecastKit(model, U32(r, 0x28), anim);
    }

    // FUN_101890d0: put the merged preview on the glue model.
    void ApplyToModel(const Merged& m)
    {
        uint8_t* c = Component();
        if (!c)
            return;
        void* model = ModelOf(c);
        if (!model)
            return;
        Transform character, shape;
        Transforms(m, character, shape);
        ClearComponent(c);
        PlayStand(model);
        if (m.spell)
        {
            const bool anim = m.anim != 0;
            if (!AscModelKit::PlayStateKit(model, m.spell, anim))
                AscModelKit::PlayPrecastKit(model, m.spell, anim);
        }
        if (m.anim)
            AscModelKit::PlayAnim(model, m.anim);
        if (m.hasCharacter || m.hasFacing)
            AscModelKit::SetTransform(model, character);

        // ---- pet: a display id outright, else a creature entry with its PetDetails placement ----
        if (m.petDisplay == 0 && m.petCreature == 0)
        {
            if (g_mgr.petModel)
            {
                Release(g_mgr.petModel);
                g_mgr.petModel = nullptr;
            }
        }
        else
        {
            const uint8_t* display = nullptr;
            const uint8_t* modelData = nullptr;
            const uint8_t* details = nullptr;
            if (m.petDisplay)
            {
                display = CreatureDisplayInfo(m.petDisplay);
                if (display)
                    modelData = CreatureModelData(U32(display, 4));
            }
            else if (const uint32_t displayId = CreatureDisplayId(m.petCreature))
            {
                display = CreatureDisplayInfo(displayId);
                if (display)
                {
                    modelData = CreatureModelData(U32(display, 4));
                    details = FindRow(PetDetails(), {m.petCreature, ComponentU32(c, 0x18)});
                }
            }
            void* scene = Ptr(model, 0x28);
            if (!display || !modelData || !scene)
            {
                if (g_mgr.petModel)
                {
                    Release(g_mgr.petModel);
                    g_mgr.petModel = nullptr;
                }
            }
            else if (void* pet = Refresh(g_mgr.petModel, scene, display, modelData, model))
            {
                if (details)
                {
                    PlayDetailsKit(pet, details);
                    AscModelKit::SetTransform(pet, TransformAt(details, 0xC));
                    if (U32(details, 0x2C) != 0xFFFFFFFF)
                        AscModelKit::PlayAnim(pet, U32(details, 0x2C));
                }
                else
                    AscModelKit::SetTransform(pet, kIdentity);
                if (m.petCast)
                    CastVisual(pet, m.petCast);
            }
        }

        // ---- items: weapons by slot, drawn into the hands when weaponsDrawn; the rest by inventory type ----
        bool mainHand = false, offHand = false;
        uint32_t slot = 0;
        for (uint32_t id : m.items)
        {
            const uint8_t* item = ItemRow(id);
            if (!item)
                continue;
            const uint32_t inv = U32(item, 0x18);
            if (U32(item, 4) != 2)
            {
                reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t)>(0x4F29C0)(c, inv, U32(item, 0x14));
                continue;
            }
            bool weapon = true;
            switch (inv)
            {
                case 0x0D: slot = (!mainHand || offHand) ? 0xF : 0x10; break;   // one-hand
                case 0x0F: case 0x1A: slot = 0x11; break;                         // ranged
                case 0x11: case 0x15: slot = 0xF; break;                          // two-hand, main hand
                case 0x16: slot = 0x10; break;                                    // off hand
                default: weapon = false; break;
            }
            const bool bow = (inv == 0x0F || inv == 0x1A) && U32(item, 8) == 2;
            if (weapon)
            {
                if (slot == 0xF)
                    mainHand = true;
                else if (slot == 0x10)
                    offHand = true;
            }
            if (!m.weaponsDrawn)
            {
                if (weapon)
                {
                    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x4EE6D0)(c, slot);
                    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int, uint32_t, int)>(0x4EF900)(c, U32(item, 0x14), 0, slot, 0);
                }
            }
            else if (weapon)
            {
                uint8_t display[0x64];   // ItemDisplayInfo copy: +4/+8 model names, +0xC/+0x10 textures
                if (reinterpret_cast<int(__thiscall*)(void*, uint32_t, void*)>(0x4CFD90)(reinterpret_cast<void*>(0xAD3DDC), U32(item, 0x14), display))
                {
                    const int32_t attach = reinterpret_cast<int32_t(__cdecl*)(uint32_t, int)>(0x4E7940)(U32(item, 0x1C), slot != 0x10);
                    if (attach != -1)
                    {
                        auto str = [&](uint32_t off) { return *reinterpret_cast<const char* const*>(display + off); };
                        const bool second = slot == 0x10 && str(8) && *str(8);   // off hand: the second model if any
                        const uint32_t base = second ? 4 : 0;
                        const char* modelName = str(base + 4);
                        if (!modelName || !*modelName)
                            modelName = str(4);
                        const char* texture = str(base + 0xC);
                        if (!texture || !*texture)
                            texture = str(0xC);
                        if (modelName && *modelName)
                        {
                            reinterpret_cast<void(__cdecl*)(void*, int32_t)>(0x4E79A0)(model, attach);
                            const std::string texPath = std::string("Item\\ObjectComponents\\Weapon\\") + (texture ? texture : "");
                            const std::string modelPath = std::string("Item\\ObjectComponents\\Weapon\\") + modelName;
                            reinterpret_cast<void(__cdecl*)(void*, int32_t, const char*, const char*, int, void*)>(0x4EAA70)(
                                model, attach, modelPath.c_str(), texPath.c_str(), 0, display);
                        }
                    }
                }
            }
            if (bow && Ptr(model, 0x58))   // bows get the string callback (0x1018A320 -> 0x4E0110)
                reinterpret_cast<void(__thiscall*)(void*, void*, int)>(0x8251B0)(Ptr(model, 0x58), reinterpret_cast<void*>(0x4E0110), 0);
        }

        // ---- shapeshift model: its creature's display, placed by the merged shapeshift transform ----
        if (m.shapeshift == 0)
        {
            if (g_mgr.shapeModel)
            {
                Release(g_mgr.shapeModel);
                g_mgr.shapeModel = nullptr;
            }
        }
        else if (const uint32_t displayId = CreatureDisplayId(m.shapeshift))
        {
            const uint8_t* display = CreatureDisplayInfo(displayId);
            const uint8_t* modelData = display ? CreatureModelData(U32(display, 4)) : nullptr;
            void* scene = Ptr(model, 0x28);
            if (display && modelData && scene)
                if (void* s = Refresh(g_mgr.shapeModel, scene, display, modelData, model))
                {
                    if (const uint8_t* details = FindRow(ShapeshiftDetails(), {m.shapeshift, ComponentU32(c, 0x18)}))
                    {
                        PlayDetailsKit(s, details);
                        AscModelKit::SetTransform(s, shape);
                        if (U32(details, 0x2C) != 0xFFFFFFFF)
                            AscModelKit::PlayAnim(s, U32(details, 0x2C));
                    }
                    else if (m.hasShape || m.hasFacing)
                        AscModelKit::SetTransform(s, shape);
                }
        }

        // unk248 goes to the model's attachment-list entries 1 and 2 ([+0x58] list, id +0x50, next +0x60)
        // through 0x7E4BF0 / 0x7E5080(x, &value, 1000). Nothing in the plain code sets it.
        if (m.unk248)
            for (uint32_t id : {1u, 2u})
            {
                uint8_t* node = static_cast<uint8_t*>(Ptr(model, 0x58));
                while (node && ComponentU32(node, 0x50) != id)
                    node = *reinterpret_cast<uint8_t**>(node + 0x60);
                if (node)
                    if (void* x = reinterpret_cast<void*(__cdecl*)(void*)>(0x7E4BF0)(node))
                        reinterpret_cast<void(__cdecl*)(void*, const uint32_t*, int)>(0x7E5080)(x, &m.unk248, 1000);
            }
        if (m.cast)
            CastVisual(model, m.cast);
        if (m.hasUndress && m.undress)
        {
            for (uint32_t s = 0; s < 0x13; ++s)
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x4EE6D0)(c, s);
        }
        else if (!m.spell && !m.anim && !m.petCreature && !m.shapeshift && !m.petDisplay && m.items.empty() && !m.unk248
                 && !m.cast && !m.petCast)
            reinterpret_cast<void(__cdecl*)()>(0x4E0FD0)();   // nothing previewed: the glue's own outfit
    }

    void PreviewChanged() { AscRuntime::Signal("CHARACTER_CREATE_PREVIEW_CHANGED"); }   // FUN_1018fa50
    void SelectionChanged()   // FUN_1018fb50
    {
        AscRuntime::Signal("CHARACTER_CREATE_SELECTION_CHANGED", "%u%u%u",
                           g_mgr.selection[0], g_mgr.selection[1], g_mgr.selection[2]);
    }

    void Update()   // FUN_1018c9d0: store the merge and apply it; announce when it changed
    {
        Merged m = Merge();
        const bool same = Same(g_mgr.merged, m);
        g_mgr.merged = std::move(m);
        ApplyToModel(g_mgr.merged);
        if (!same)
            PreviewChanged();
    }

    void UpdateIfChanged()   // the tail of FUN_1018b7c0: apply only when the merge changed
    {
        Merged m = Merge();
        const bool same = Same(g_mgr.merged, m);
        g_mgr.merged = std::move(m);
        if (!same)
        {
            ApplyToModel(g_mgr.merged);
            PreviewChanged();
        }
    }

    void SnapshotComponent()   // head of FUN_1018b7c0 / FUN_1018b8f0
    {
        uint8_t* c = Component();
        Set(g_mgr.race, c ? ComponentU32(c, 0x18) : 0u);
        Set(g_mgr.sex, c ? ComponentU32(c, 0x1C) : 2u);
        g_mgr.hadComponent = c != nullptr;
        RebuildLayer0();
    }

    void WriteLayer1(const Layer& l) { g_mgr.layers[1] = l; }   // FUN_1018f850(1, ...)

    // ---- customization cycling ----------------------------------------------------------------------
    // FUN_1018fc80: one step of customization `type` (1 skin, 2 face, 3 hair style, 4 hair colour,
    // 5 facial hair) in direction `dir` on the component.
    bool Cycle(uint8_t* c, uint32_t type, int32_t dir)
    {
        if (!c || !dir)
            return false;
        typedef char(__thiscall* Step_t)(void*, int);
        switch (type)
        {
            case 1: return reinterpret_cast<Step_t>(dir > 0 ? 0x4EB150 : 0x4EB290)(c, 0) != 0;
            case 2: return reinterpret_cast<char(__thiscall*)(void*, int, uint32_t)>(dir > 0 ? 0x4EB710 : 0x4EB990)(c, 0, ComponentU32(c, 0x28)) != 0;
            case 3: return reinterpret_cast<Step_t>(dir > 0 ? 0x4F0490 : 0x4F0630)(c, 0) != 0;
            case 4: return reinterpret_cast<Step_t>(dir > 0 ? 0x4EB500 : 0x4EB5C0)(c, 0) != 0;
            case 5: return reinterpret_cast<Step_t>(dir > 0 ? 0x4EBCA0 : 0x4EBE80)(c, 0) != 0;
            default: return false;
        }
    }

    // FUN_1032dbf0 (the vanity manager's customization-lock query): in this build it answers "not a
    // lockable option" for everything, so the skip below never steps.
    bool CustomizationLock(uint32_t, bool& lockable, bool& unlocked, uint32_t& requirement)
    {
        lockable = false;
        unlocked = true;
        requirement = 0;
        return true;
    }

    bool SkipLocked(uint32_t type, int32_t dir)   // FUN_1018aef0: step past locked options, at most 200
    {
        bool stepped = false;
        for (uint32_t n = 0;;)
        {
            bool lockable = false, unlocked = true;
            uint32_t requirement = 0;
            if (!CustomizationLock(type, lockable, unlocked, requirement) || !lockable || unlocked)
                return stepped;
            if (!Cycle(Component(), type, dir))
                return stepped;
            stepped = true;
            if (++n > 199)
                return true;
        }
    }

    void SkipAllLocked()   // FUN_1018d030
    {
        bool stepped = false;
        for (uint32_t type = 1; type <= 5; ++type)
            if (SkipLocked(type, 1))
                stepped = true;
        if (stepped)
            if (uint8_t* c = Component())
                reinterpret_cast<void(__cdecl*)(void*, int)>(0x4E6AE0)(c, 1);
    }

    // ---- detours (module init FUN_1018b290) ---------------------------------------------------------
    // LAB_1018ec80 replaces SetCharacterCreateFacing(degrees).
    int __cdecl SetCharacterCreateFacing(lua_State* L);
    // LAB_1018e250 replaces GetCharacterCreateFacing(): the facing override, else the shapeshift or
    // character transform's yaw, in degrees.
    int __cdecl GetCharacterCreateFacing(lua_State* L)
    {
        const Merged& m = g_mgr.merged;
        float yaw = 0.0f;
        if (m.hasFacing)
            yaw = m.facing;
        else if (m.shapeshift != 0 && m.hasShape)
            yaw = m.shape.rot[2];
        else if (m.hasCharacter)
            yaw = m.character.rot[2];
        AscLua::lua_pushnumber(L, static_cast<double>(yaw * kRadToDeg));
        return 1;
    }

    typedef void(__cdecl* Arg1_t)(uint32_t);
    typedef int(__cdecl* Arg1Ret_t)(uint32_t);
    typedef int(__cdecl* Arg0Ret_t)();
    typedef void(__cdecl* Camera_t)(uint32_t, int32_t, float*);
    Arg1_t g_setSex = nullptr, g_setRace = nullptr;
    Arg1Ret_t g_setClass = nullptr;
    Arg0Ret_t g_randomize = nullptr;
    Camera_t g_camera = nullptr;

    void RefreshAfterRaceSex()   // FUN_1018b8f0
    {
        SnapshotComponent();
        Update();
    }
    void RefreshIfChanged()   // FUN_1018b7c0
    {
        SnapshotComponent();
        UpdateIfChanged();
    }

    void __cdecl SetSelectedSexDetour(uint32_t v) { g_setSex(v); SkipAllLocked(); RefreshAfterRaceSex(); }      // LAB_1018f9b0
    void __cdecl SetSelectedRaceDetour(uint32_t v) { g_setRace(v); SkipAllLocked(); RefreshAfterRaceSex(); }    // LAB_1018f990
    int __cdecl SetSelectedClassDetour(uint32_t v)   // LAB_1018ae20
    {
        const int r = g_setClass(v);
        SkipAllLocked();
        RefreshIfChanged();
        return r;
    }
    int __cdecl RandomizeDetour()   // LAB_1018ae50
    {
        const int r = g_randomize();
        SkipAllLocked();
        RefreshIfChanged();
        return r;
    }

    // FUN_1018af70: the camera position lookup; id 7 is the character-creation camera. Remember what the
    // glue asked for and substitute the preview's camera when one is set.
    void __cdecl CameraDetour(uint32_t a, int32_t id, float* out)
    {
        g_camera(a, id, out);
        if (id == 7 && out)
        {
            std::copy(out, out + 3, g_mgr.camera);
            if (g_mgr.merged.hasCamera)
                std::copy(g_mgr.merged.camera, g_mgr.merged.camera + 3, out);
            std::copy(out, out + 3, g_mgr.camera);
        }
    }

    // FUN_1018b160, after every call of 0x495810: follow the component's race / sex.
    void WatchComponent()
    {
        uint8_t* c = Component();
        if (!c)
        {
            // The glue scene (and the preview models in it) is gone; forget them without releasing.
            g_mgr.petModel = nullptr;
            g_mgr.shapeModel = nullptr;
            g_mgr.race.set = false;
            g_mgr.sex.set = false;
            g_mgr.hadComponent = false;
            return;
        }
        const uint32_t race = ComponentU32(c, 0x18), sex = ComponentU32(c, 0x1C);
        if (g_mgr.hadComponent && g_mgr.race.set && g_mgr.race.v == race && g_mgr.sex.set && g_mgr.sex.v == sex)
            return;
        Set(g_mgr.race, race);
        Set(g_mgr.sex, sex);
        g_mgr.hadComponent = true;
        RebuildLayer0();
        Update();
    }

    // FUN_1018cad0 (glue screen): drop the preview models and every override; announce what was lost.
    void ResetPreview()
    {
        const bool hadSelection = g_mgr.selection[0] || g_mgr.selection[1] || g_mgr.selection[2];
        const Merged& m = g_mgr.merged;
        const bool hadPreview = m.spell || m.anim || m.petCreature || m.shapeshift || m.petDisplay || m.weaponsDrawn
            || !m.items.empty() || m.unk248 || m.hasCharacter || m.hasShape || m.hasCamera || m.hasFacing || m.hasUndress
            || m.cast || m.petCast;
        if (g_mgr.petModel)
        {
            Release(g_mgr.petModel);
            g_mgr.petModel = nullptr;
        }
        if (g_mgr.shapeModel)
        {
            Release(g_mgr.shapeModel);
            g_mgr.shapeModel = nullptr;
        }
        g_mgr.mode = 0;
        std::fill(g_mgr.selection, g_mgr.selection + 3, 0u);
        g_mgr.classPreview = 0;
        g_mgr.classPreviewName.clear();
        for (Layer& l : g_mgr.layers)
            l = Layer();
        g_mgr.merged = Merged();
        std::fill(g_mgr.camera, g_mgr.camera + 3, 0.0f);
        std::fill(g_mgr.zoomOrigin, g_mgr.zoomOrigin + 3, 0.0f);
        g_mgr.race.set = false;
        g_mgr.sex.set = false;
        g_mgr.hadComponent = false;
        if (hadSelection)
            SelectionChanged();
        if (hadPreview)
            PreviewChanged();
    }

    // FUN_1018b790, after an SMSG_PATCH_CHARACTER_CREATION_* row lands: rebuild every index and the preview.
    void OnCreationTablePatched(const uint8_t*)
    {
        g_mgr.archetypesDirty = true;
        g_mgr.guideDirty = true;
        g_mgr.classNamesDirty = true;
        RebuildLayer0();
        Update();
    }

    // FUN_1018f5a0: the facing override, put straight on the models without a full update.
    void SetFacing(float radians)
    {
        Layer l = g_mgr.layers[1];
        Set(l.facing, radians);
        WriteLayer1(l);
        g_mgr.merged.hasFacing = true;
        g_mgr.merged.facing = radians;
        uint8_t* c = Component();
        if (!c || !ModelOf(c))
            return;
        Transform character, shape;
        Transforms(g_mgr.merged, character, shape);
        AscModelKit::SetTransform(ModelOf(c), character);
        if (g_mgr.shapeModel && g_mgr.merged.shapeshift)
            AscModelKit::SetTransform(g_mgr.shapeModel, shape);
    }

    int __cdecl SetCharacterCreateFacing(lua_State* L)
    {
        const float degrees = static_cast<float>(CheckNumber(L, 1));
        SetFacing(degrees * kDegToRad);
        return 0;
    }

    void SetCamera(const float* v)   // FUN_1018f500
    {
        Layer l = g_mgr.layers[1];
        Set(l.camera, Vec3{{v[0], v[1], v[2]}});
        WriteLayer1(l);
        Update();
    }

    // ---- bindings -----------------------------------------------------------------------------------
    // Layer-1 setters share one shape: copy the layer, set one override, write it back, update.
    template <class Fn> int EditLayer1(Fn fn)
    {
        Layer l = g_mgr.layers[1];
        fn(l);
        WriteLayer1(l);
        Update();
        return 0;
    }

    int AddPet(lua_State* L)   // FUN_1018d490 (displayId)
    {
        uint32_t display;
        if (ReadNumber(L, display))
            EditLayer1([&](Layer& l) { Set(l.petDisplay, display); });
        return 0;
    }

    int PlayCastAnimation(lua_State* L)   // FUN_1018e7a0 (spell)
    {
        uint32_t spell;
        if (ReadNumber(L, spell))
            EditLayer1([&](Layer& l) { Set(l.cast, spell); });
        return 0;
    }

    int PlayPetCastAnimation(lua_State* L)   // FUN_1018e860 (spell)
    {
        uint32_t spell;
        if (ReadNumber(L, spell))
            EditLayer1([&](Layer& l) { Set(l.petCast, spell); });
        return 0;
    }

    int StopCastAnimation(lua_State*) { return EditLayer1([](Layer& l) { Set(l.cast, 0u); }); }          // FUN_1018edf0
    int StopPetCastAnimation(lua_State*) { return EditLayer1([](Layer& l) { Set(l.petCast, 0u); }); }    // FUN_1018ee90

    // FUN_1018d8d0: drop the weapons-drawn, item and undress overrides.
    int Dress(lua_State*)
    {
        return EditLayer1([](Layer& l) {
            l.items = Opt<std::vector<uint32_t>>();
            l.weaponsDrawn.set = false;
            l.undress.set = false;
        });
    }

    // FUN_1018ef70: sheathed, no items, undressed.
    int Undress(lua_State*)
    {
        return EditLayer1([](Layer& l) {
            Set(l.weaponsDrawn, false);
            Set(l.items, std::vector<uint32_t>());
            Set(l.undress, true);
        });
    }

    // Slot key of an item for TryOnItem (FUN_1018fd20): a weapon's hand slot (10001 one-hand / main,
    // 10002 off hand, 10003 two-hand, 10004 ranged, else 10000 + type); anything else its inventory type.
    int32_t TryOnKey(const uint8_t* item)
    {
        const int32_t inv = static_cast<int32_t>(U32(item, 0x18));
        if (U32(item, 4) != 2)
            return inv;
        switch (inv)
        {
            case 0x0D: case 0x15: return 0x2711;
            case 0x16: return 0x2712;
            case 0x11: return 0x2713;
            case 0x0F: case 0x1A: return 0x2714;
            default: return inv + 10000;
        }
    }

    // FUN_1018fd20: replace whatever the preview wears in the item's slot with the item; a weapon also
    // sheathes (weaponsDrawn = false).
    int TryOnItem(lua_State* L)
    {
        const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        const uint8_t* item = ItemRow(id);
        if (!item)
            return 0;
        Layer l = g_mgr.layers[1];
        std::vector<uint32_t> items = l.items.set ? l.items.v : g_mgr.merged.items;
        const int32_t key = TryOnKey(item);
        items.erase(std::remove_if(items.begin(), items.end(), [&](uint32_t other) {
                        const uint8_t* o = ItemRow(other);
                        return o && TryOnKey(o) == key;
                    }), items.end());
        items.push_back(id);
        Set(l.items, items);
        if (U32(item, 4) == 2)
            Set(l.weaponsDrawn, false);
        WriteLayer1(l);
        Update();
        return 0;
    }

    int ResetCameraPosition(lua_State*)   // FUN_1018eab0
    {
        Layer l = g_mgr.layers[1];
        Set(l.camera, Vec3{{0, 0, 0}});
        std::fill(g_mgr.zoomOrigin, g_mgr.zoomOrigin + 3, 0.0f);
        WriteLayer1(l);
        Update();
        return 0;
    }

    int SetCameraPosition(lua_State* L)   // FUN_1018eb80 (x, y, z)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, NUMBER}))
            return 0;
        const float v[3] = {static_cast<float>(CheckNumber(L, 1)), static_cast<float>(CheckNumber(L, 2)),
                            static_cast<float>(CheckNumber(L, 3))};
        SetCamera(v);
        return 0;
    }

    // The original's length: pow(d, 2) per axis rounded to float, summed y + x + z in float, then a
    // double square root.
    float Length(float x, float y, float z)
    {
        const float pz = static_cast<float>(pow(static_cast<double>(z), 2.0));
        const float px = static_cast<float>(pow(static_cast<double>(x), 2.0));
        const float py = static_cast<float>(pow(static_cast<double>(y), 2.0));
        const float sum = py + px + pz;
        return static_cast<float>(sqrt(static_cast<double>(sum)));
    }

    // FUN_1018f070 (distance): move the camera `distance` toward the character's attachment 0x14,
    // unless that would pass it. The first zoom remembers where the camera started.
    int ZoomIn(lua_State* L)
    {
        const float distance = static_cast<float>(CheckNumber(L, 1));
        float* o = g_mgr.zoomOrigin;
        if (o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f)
            std::copy(g_mgr.camera, g_mgr.camera + 3, o);
        uint8_t* c = Component();
        if (!c || !ModelOf(c))
            return 0;
        float target[3] = {0, 0, 0};
        reinterpret_cast<void(__thiscall*)(void*, float*, int)>(0x831330)(ModelOf(c), target, 0x14);
        const float* cam = g_mgr.camera;
        const float dx = cam[0] - target[0], dy = cam[1] - target[1], dz = cam[2] - target[2];
        if (distance <= Length(dx, dy, dz))
        {
            const float inv = 1.0f / static_cast<float>(sqrt(static_cast<double>(dy * dy + dx * dx + dz * dz)));
            const float v[3] = {cam[0] - dx * inv * distance, cam[1] - dy * inv * distance, cam[2] - dz * inv * distance};
            SetCamera(v);
        }
        return 0;
    }

    // FUN_1018f2e0 (distance): move the camera back toward where the zoom started, at most `distance`.
    int ZoomOut(lua_State* L)
    {
        const float distance = static_cast<float>(CheckNumber(L, 1));
        const float* o = g_mgr.zoomOrigin;
        uint8_t* c = Component();
        if ((o[0] == 0.0f && o[1] == 0.0f && o[2] == 0.0f) || !c || !ModelOf(c))
            return 0;
        const float* cam = g_mgr.camera;
        const float dx = cam[0] - o[0], dy = cam[1] - o[1], dz = cam[2] - o[2];
        float len = Length(dx, dy, dz);
        if (len == 0.0f)
            return 0;
        if (distance <= len)
            len = distance;
        const float inv = 1.0f / static_cast<float>(sqrt(static_cast<double>(dy * dy + dx * dx + dz * dz)));
        const float v[3] = {cam[0] - inv * dx * len, cam[1] - inv * dy * len, cam[2] - inv * dz * len};
        SetCamera(v);
        return 0;
    }

    // FUN_1018f9d0 via FUN_1018ecd0 (role, category, archetype).
    int SetSelectedArchetype(lua_State* L)
    {
        if (!ValidateInput(L, {NUMBER, NUMBER, NUMBER}))
            return 0;
        const uint32_t sel[3] = {static_cast<uint32_t>(ToInt(CheckNumber(L, 1))), static_cast<uint32_t>(ToInt(CheckNumber(L, 2))),
                                 static_cast<uint32_t>(ToInt(CheckNumber(L, 3)))};
        const bool changed = !std::equal(sel, sel + 3, g_mgr.selection);
        std::copy(sel, sel + 3, g_mgr.selection);
        if (sel[0] == 0 && sel[1] == 0 && sel[2] == 0)
        {
            if (g_mgr.mode == 1)
                g_mgr.mode = 0;
        }
        else
            g_mgr.mode = 1;
        RebuildLayer0();
        Update();
        if (changed)
            SelectionChanged();
        return 0;
    }

    std::string Upper(const char* s, size_t n)   // FUN_1018b5d0
    {
        std::string out(s, n);
        for (char& ch : out)
            ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        return out;
    }

    // FUN_1018e920 -> FUN_1018baf0 (classFile): preview that class's outfit ("WARRIOR", case-insensitive).
    int PreviewClassOutfit(lua_State* L)
    {
        if (!ValidateInput(L, {STRING}))
            return 0;
        const std::string name = CheckString(L, 1);
        if (name.empty())
            return 0;
        if (g_mgr.classNamesDirty)
        {
            const uint32_t minId = *reinterpret_cast<const uint32_t*>(kChrClasses + 0x10);
            const uint32_t maxId = *reinterpret_cast<const uint32_t*>(kChrClasses + 0x0C);
            for (uint32_t id = minId; id <= maxId; ++id)
                if (const uint8_t* r = ClientDbcRow(kChrClasses, id))
                {
                    const char* file = *reinterpret_cast<const char* const*>(r + 0x1C);
                    if (file && *file)
                        g_mgr.classByName[Upper(file, strlen(file))] = U32(r, 0);
                }
            g_mgr.classNamesDirty = false;
        }
        const std::string key = Upper(name.data(), name.size());
        auto it = g_mgr.classByName.find(key);
        if (it == g_mgr.classByName.end())
            return 0;
        g_mgr.classPreview = it->second;
        g_mgr.classPreviewName = key;
        g_mgr.mode = 2;
        RebuildLayer0();
        Update();
        return 0;
    }

    int ClearClassPreview(lua_State*)   // handler_ClearClassPreview
    {
        if (g_mgr.classPreview == 0 && g_mgr.classPreviewName.empty() && g_mgr.mode != 2)
            return 0;
        g_mgr.classPreview = 0;
        g_mgr.classPreviewName.clear();
        if (g_mgr.mode == 2)
            g_mgr.mode = (g_mgr.selection[0] || g_mgr.selection[1] || g_mgr.selection[2]) ? 1 : 0;
        RebuildLayer0();
        Update();
        return 0;
    }

    // FUN_1018d830 (type, direction): one step, then past locked options, then refresh.
    int CycleCustomizationSkipLocked(lua_State* L)
    {
        const uint32_t type = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        int32_t dir = static_cast<int32_t>(CheckNumber(L, 2));
        if (dir == 0)
            dir = 1;
        uint8_t* c = Component();
        if (c && Cycle(c, type, dir))
        {
            SkipLocked(type, dir);
            if (uint8_t* now = Component())
                reinterpret_cast<void(__cdecl*)(void*, int)>(0x4E6AE0)(now, 1);
            RefreshIfChanged();
        }
        return 0;
    }

    void InitPreview()   // FUN_1018b290
    {
        AscRuntime::OnGlueScreen(ResetPreview);
        AscRuntime::OnAfter495810(WatchComponent);
        AscRuntime::ReplaceFunction(0x4E0C10, reinterpret_cast<void*>(&SetCharacterCreateFacing));
        AscRuntime::ReplaceFunction(0x4E0BE0, reinterpret_cast<void*>(&GetCharacterCreateFacing));
        g_camera = reinterpret_cast<Camera_t>(AscRuntime::Detour(0x4C1290, 6, reinterpret_cast<void*>(&CameraDetour)));
        g_setSex = reinterpret_cast<Arg1_t>(AscRuntime::Detour(0x4E1540, 9, reinterpret_cast<void*>(&SetSelectedSexDetour)));
        g_setRace = reinterpret_cast<Arg1_t>(AscRuntime::Detour(0x4E20B0, 9, reinterpret_cast<void*>(&SetSelectedRaceDetour)));
        g_setClass = reinterpret_cast<Arg1Ret_t>(AscRuntime::Detour(0x4E1740, 9, reinterpret_cast<void*>(&SetSelectedClassDetour)));
        g_randomize = reinterpret_cast<Arg0Ret_t>(AscRuntime::Detour(0x4E17F0, 6, reinterpret_cast<void*>(&RandomizeDetour)));

        // SMSG_PATCH_CHARACTER_CREATION_*: struct images; Roles / Categories carry their name strings.
        AscDbcPatch::Register(0x6D4, "DBFilesClient\\CharacterCreationArchetypeRoles.dbc", 0x5C, 0xFull << 18, OnCreationTablePatched);
        AscDbcPatch::Register(0x6D5, "DBFilesClient\\CharacterCreationArchetypeCategories.dbc", 0x64, 0x1Full << 19, OnCreationTablePatched);
        AscDbcPatch::Register(0x6D7, "DBFilesClient\\CharacterCreationPetDetails.dbc", 0x30, 0, OnCreationTablePatched);
        AscDbcPatch::Register(0x6D8, "DBFilesClient\\CharacterCreationShapeshiftDetails.dbc", 0x54, 0, OnCreationTablePatched);
        AscDbcPatch::Register(0x6E3, "DBFilesClient\\CharacterCreationArchetypeDetails.dbc", 0x70, 0, OnCreationTablePatched);
        AscDbcPatch::Register(0x9CF, "DBFilesClient\\CharacterCreationClassDetails.dbc", 0x70, 0, OnCreationTablePatched);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_CharacterCreate", "CanCreateCoA", CanCreateCoA},

        {"C_CharacterCreate", "CanCreateHero", CanCreateHero},
        {"C_CharacterCreate", "CanCreateWCR", CanCreateWCR},
        {"C_CharacterCreate", "CanCreateClass", CanCreateClass},
        {"C_CharacterCreate", "GetArchetypeRoles", GetArchetypeRoles},
        {"C_CharacterCreate", "GetArchetypeCategories", GetArchetypeCategories},
        {"C_CharacterCreate", "GetArchetypes", GetArchetypes},
        {"C_CharacterCreate", "GetArchetypeRoleInfo", GetArchetypeRoleInfo},
        {"C_CharacterCreate", "GetArchetypeCategoryInfo", GetArchetypeCategoryInfo},
        {"C_CharacterCreate", "GetArchetypeInfo", GetArchetypeInfo},
        {"C_CharacterCreate", "GetArchetypeSpellDescription", GetArchetypeSpellDescription},
        {"C_CharacterCreate", "GetClassGuideRoles", GetClassGuideRoles},
        {"C_CharacterCreate", "GetClassGuideSubroles", GetClassGuideSubroles},
        {"C_CharacterCreate", "GetClassGuideSubroleClasses", GetClassGuideSubroleClasses},
        {"C_CharacterCreate", "GetClassGuideRoleInfo", GetClassGuideRoleInfo},
        {"C_CharacterCreate", "GetClassGuideSubroleInfo", GetClassGuideSubroleInfo},
        {"C_CharacterCreate", "GetCameraPosition", GetCameraPosition},
        {"C_CharacterCreate", "PlayCastAnimation", PlayCastAnimation},
        {"C_CharacterCreate", "StopCastAnimation", StopCastAnimation},
        {"C_CharacterCreate", "AddPet", AddPet},
        {"C_CharacterCreate", "PlayPetCastAnimation", PlayPetCastAnimation},
        {"C_CharacterCreate", "StopPetCastAnimation", StopPetCastAnimation},
        {"C_CharacterCreate", "SetCameraPosition", SetCameraPosition},
        {"C_CharacterCreate", "ResetCameraPosition", ResetCameraPosition},
        {"C_CharacterCreate", "ZoomIn", ZoomIn},
        {"C_CharacterCreate", "ZoomOut", ZoomOut},
        {"C_CharacterCreate", "SetSelectedArchetype", SetSelectedArchetype},
        {"C_CharacterCreate", "PreviewClassOutfit", PreviewClassOutfit},
        {"C_CharacterCreate", "ClearClassPreview", ClearClassPreview},
        {"C_CharacterCreate", "Dress", Dress},
        {"C_CharacterCreate", "Undress", Undress},
        {"C_CharacterCreate", "TryOnItem", TryOnItem},
        {"C_CharacterCreate", "CycleCustomizationSkipLocked", CycleCustomizationSkipLocked},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), InitPreview);
}

// SMSG 0x6D6 (FUN_101df460, both paths) -> FUN_100d5070 -> FUN_1018b790: the three sorted indexes marked
// dirty (+0x2CC / +0x31C / +0x36C), then FUN_1018c8e0 and FUN_1018c9d0.
void AscCharacterCreate_ArchetypesPatched()
{
    g_mgr.archetypesDirty = true;
    g_mgr.guideDirty = true;
    g_mgr.classNamesDirty = true;
    RebuildLayer0();
    Update();
}
