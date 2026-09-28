// The original's nameplate module: installer FUN_102bf920 (called from the attach init FUN_10a66100),
// the "nameplateN" unit-token registry behind GetNamePlateForUnit / NAME_PLATE_UNIT_ADDED / _REMOVED,
// its twelve CVars and the eight client detours that read them.
//
//   Patches   0x72594B push 0 -> push 1; 0x7258CE 8 bytes NOP (je / cmp [eax+0x30],0 / je);
//             0x98E9F9 12 bytes NOP (the plate update's target-GUID compare); 0x98EA07 jne rel8 0x14 -> 0x1F;
//             0x715739 the plate height fadd operand 0x9FC830 -> &g_verticalOffset;
//             0x71557D / 0x715586 jne -> jmp.
//   Detours   0x98E9F0 -> FUN_102be570 (plate update: distance fade, intersect opacity)
//             0x725890 -> FUN_102be880 (plate pass: distance and facing-angle culling)
//             0x72B060 -> FUN_102c2630 (plate position: personal plate, neutral / combat filters, offset)
//             0x7256C0 -> sub_102beb70 (plate created -> registry add FUN_102c0bc0)
//             0x725840 -> FUN_102beb40 (plate released -> registry remove FUN_102c0d80)
//             0x9900E0 -> sub_102c28b0 (then client event 0x12 for the unit, 0x60BF10)
//             0x715C30 -> sub_102be530, 0x98E910 -> FUN_102c2e90 (the player's own plate is never the
//                         one under the cursor)
//             0x60ABF0 -> LAB_10a6ec00 / FUN_10a4ea40 ("nameplateN" unit tokens; installed by the attach
//                         init itself at 0x10A672E5, hook object 0x10BCCCF0)
//   Lifecycle FUN_102c29a0 after 0x403340 (queued NAME_PLATE_UNIT_ADDED, frame levels by distance);
//             LAB_102bd620 before 0x528C30 (registry cleared on leaving the world).
//
// The DLL keeps each CVar's pointer from registration (DAT_10be348c..34b8) and reads +0x2C (float) /
// +0x30 (int); we look them up by name because the world state re-registers them. Where the original
// reads a CVar without a null check, a missing one reads as 0 here instead of faulting.
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscNamePlates.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscLua.hpp>
#include <Client/CVar.hpp>
#include <Windows.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    // ---- client addresses the DLL keeps pointers to (0x10BCBBEC..0x10BCBC64, 0x10BCA100) ----------
    const uint32_t kPlatesShown = 0xAC80A8;      // DAT_10bcbbec
    const uint32_t kTargetGuid = 0xBD07B0;       // DAT_10bcbbf0
    const uint32_t kPlateList = 0xADAA88;        // DAT_10bcbc60: first plate, linked through +0x2A0
    const uint32_t kCursorPlate = 0xCA1204;      // DAT_10bcbc64
    const uint32_t kPassArgument = 0xB7436C;     // DAT_10bca100

    // ---- DLL globals ----------------------------------------------------------------------------
    float g_verticalOffset = 0.6666667f;         // DAT_10bcbbf4 (0x3F2AAAAA), read by the client via 0x715739
    int32_t g_personalPosition = 2;              // DAT_10bcbbf8
    bool g_showNeutral = true;                   // DAT_10bcbbfc

    struct PlateEntry                            // FUN_102c0bc0's 8-byte allocation
    {
        uint32_t id;
        uint8_t* unit;
    };
    std::unordered_map<uint32_t, PlateEntry*> g_plates;   // 0x10BE3454
    std::vector<uint8_t*> g_units;                        // 0x10BE3480, GetNamePlateForUnit's scratch list
    std::vector<uint32_t> g_addedIds;                     // 0x10BE34BC

    struct PlateOrder                                     // 0x18 bytes, FUN_102c0a70
    {
        float distance;
        uint32_t unused;
        uint32_t guidLo;
        uint32_t guidHi;
        void* plate;
        uint32_t unused2;
    };
    std::vector<PlateOrder> g_order;                      // 0x10BE34C8

    void Patch(uint32_t addr, const void* bytes, size_t n)
    {
        DWORD old;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old))
            return;
        memcpy(reinterpret_cast<void*>(addr), bytes, n);
        VirtualProtect(reinterpret_cast<void*>(addr), n, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    }
    template <typename T> void PatchValue(uint32_t addr, T v) { Patch(addr, &v, sizeof(T)); }

    const uint8_t* Var(const char* name) { return reinterpret_cast<const uint8_t*>(CVar::Lookup(name)); }
    float VarF(const uint8_t* v) { return *reinterpret_cast<const float*>(v + 0x2C); }
    int32_t VarI(const uint8_t* v) { return *reinterpret_cast<const int32_t*>(v + 0x30); }
    float VarF(const char* name) { const uint8_t* v = Var(name); return v ? VarF(v) : 0.0f; }
    int32_t VarI(const char* name) { const uint8_t* v = Var(name); return v ? VarI(v) : 0; }

    // ---- client calls ---------------------------------------------------------------------------
    void Position(void* object, float* out)     // vtable +0x2C
    {
        void** vt = *static_cast<void***>(object);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(vt[0x2C / 4])(object, out);
    }
    void RawPosition(void* object, float* out)  // vtable +0x20
    {
        void** vt = *static_cast<void***>(object);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(vt[0x20 / 4])(object, out);
    }
    float Facing(void* object)                  // vtable +0x34
    {
        void** vt = *static_cast<void***>(object);
        return reinterpret_cast<float(__thiscall*)(void*)>(vt[0x34 / 4])(object);
    }
    // 0x722AE0(unit, descriptor +0x10C): the unit's model height.
    float Height(uint8_t* unit)
    {
        const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
        return reinterpret_cast<float(__thiscall*)(void*, uint32_t)>(0x722AE0)(unit, *reinterpret_cast<const uint32_t*>(d + 0x10C));
    }
    uint32_t UnitFlags(const uint8_t* unit)     // descriptor +0xEC
    {
        const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
        return *reinterpret_cast<const uint32_t*>(d + 0xEC);
    }
    void SetAlpha(void* frame, uint32_t alpha) { reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x48EA10)(frame, alpha); }
    void Hide(void* frame) { reinterpret_cast<void(__thiscall*)(void*)>(0x48F620)(frame); }
    void Show(void* frame) { reinterpret_cast<void(__thiscall*)(void*)>(0x48F660)(frame); }
    bool Equals(const uint8_t* at, uint64_t guid)
    {
        return *reinterpret_cast<const uint32_t*>(at) == static_cast<uint32_t>(guid) &&
               *reinterpret_cast<const uint32_t*>(at + 4) == static_cast<uint32_t>(guid >> 32);
    }
    uint64_t TargetGuid() { return *reinterpret_cast<const uint64_t*>(kTargetGuid); }

    // The DLL's inline distance: each axis difference squared through pow (in double), summed in float
    // as dy + dx + dz, square root in double.
    float Distance(const float* a, const float* b)
    {
        const float dz = static_cast<float>(std::pow(static_cast<double>(a[2] - b[2]), 2.0));
        const float dx = static_cast<float>(std::pow(static_cast<double>(a[0] - b[0]), 2.0));
        const float dy = static_cast<float>(std::pow(static_cast<double>(a[1] - b[1]), 2.0));
        return static_cast<float>(std::sqrt(static_cast<double>(dy + dx + dz)));
    }

    std::string Token(uint32_t id) { return "nameplate" + std::to_string(id); }

    // ---- registry -------------------------------------------------------------------------------
    // FUN_102c0bc0: the lowest free id from 1 up, a new {id, unit} entry, and the id queued for
    // NAME_PLATE_UNIT_ADDED on the next tick.
    void AddPlate(uint8_t* unit)
    {
        uint32_t id = 1;
        do
        {
            if (g_plates.find(id) == g_plates.end())
                break;
            ++id;
        } while (id != 0xFFFFFFFF);
        g_plates[id] = new PlateEntry{id, unit};
        g_addedIds.push_back(id);
    }

    // FUN_102c0d80: NAME_PLATE_UNIT_REMOVED("nameplateN") for the unit's entry, then the entry is freed
    // and erased.
    void RemovePlate(uint8_t* unit)
    {
        uint32_t id = 0;
        for (auto& kv : g_plates)
            if (kv.second->unit == unit)
            {
                if (PlateEntry* e = g_plates[kv.first])
                    id = e->id;
                break;
            }
        if (id != 0)
            AscRuntime::Signal("NAME_PLATE_UNIT_REMOVED", "%s", Token(id).c_str());

        for (auto& kv : g_plates)
            if (kv.second->unit == unit)
            {
                PlateEntry* e = g_plates[kv.first];
                if (!e)
                    return;
                const uint32_t key = e->id;
                delete g_plates[key];
                g_plates.erase(key);
                return;
            }
    }

    // LAB_102bd620 (list 0x10BE28AC, before 0x528C30): the map is cleared; its entries are not freed.
    void ClearPlates() { g_plates.clear(); }

    // ---- detours --------------------------------------------------------------------------------
    // FUN_102be570 over 0x98E9F0 (__thiscall(plate, float), ret 4). With no target: fade by distance
    // from the player's head (when nameplateFadeIn is 1), and dim to nameplateIntersectOpacity when the
    // line from the player (or the camera's x/y, with nameplateIntersectUseCamera) to the unit's
    // mid-height is blocked. The target's and the player's own plates are always fully opaque.
    typedef void(__fastcall* PlateUpdate_t)(uint8_t*, void*, float);
    PlateUpdate_t g_plateUpdate = nullptr;
    void __fastcall PlateUpdateDetour(uint8_t* plate, void* edx, float elapsed)
    {
        g_plateUpdate(plate, edx, elapsed);
        const uint64_t plateGuid = *reinterpret_cast<const uint64_t*>(plate + 0x2A8);
        if (TargetGuid() == 0)
            if (uint8_t* player = ActivePlayer())
            {
                float head[3];
                Position(player, head);
                head[2] = Height(player) * 0.95f + head[2];
                float from[3] = {head[0], head[1], head[2]};
                const uint8_t* useCamera = Var("nameplateIntersectUseCamera");
                if (useCamera && VarI(useCamera) == 1)
                    if (const uint8_t* camera = reinterpret_cast<const uint8_t*(__cdecl*)()>(0x4F5960)())   // FUN_101941c0
                    {
                        from[0] = *reinterpret_cast<const float*>(camera + 8);
                        from[1] = *reinterpret_cast<const float*>(camera + 0xC);
                    }
                if (uint8_t* unit = static_cast<uint8_t*>(ObjectPtr(plateGuid, 8)))
                {
                    float at[3];
                    Position(unit, at);
                    const float height = Height(unit);
                    float hit[3] = {0.0f, 0.0f, 0.0f};
                    float fraction = 1.0f;
                    at[2] = height * 0.5f + at[2];
                    const uint8_t* fadeIn = Var("nameplateFadeIn");
                    if (fadeIn && VarI(fadeIn) == 1)
                    {
                        const float d = Distance(head, at);
                        const uint8_t* distanceVar = Var("nameplateDistance");
                        const float range = distanceVar ? VarF(distanceVar) : 43.0f;
                        const float start = range - 13.5f;
                        if (d < start)
                            SetAlpha(plate, 0xFF);
                        else
                            SetAlpha(plate, static_cast<uint8_t>(static_cast<int>((1.0f - (d - start) / (range - start)) * 255.0f)));
                    }
                    const uint8_t* opacity = Var("nameplateIntersectOpacity");
                    if (opacity && reinterpret_cast<bool(__cdecl*)(const float*, const float*, float*, float*, uint32_t, int)>(0x77F310)(
                                       from, at, hit, &fraction, 0x100070, 0))
                        SetAlpha(plate, static_cast<uint8_t>(static_cast<int>(VarF(opacity) * 255.0f)));
                }
            }
        if (Equals(plate + 0x2A8, TargetGuid()))
            SetAlpha(plate, 0xFF);
        if (Equals(plate + 0x2A8, ActivePlayerGuid()))
            SetAlpha(plate, 0xFF);
    }

    // FUN_102be880 over 0x725890 (__cdecl(a)): after the client's pass, when nameplateAngle is non-zero,
    // every plate in the client's list is hidden beyond nameplateDistance, or when further than 3 yards
    // and outside nameplateAngle of the player's facing; shown otherwise (also when its unit is gone).
    typedef int(__cdecl* PlatePass_t)(uint32_t);
    PlatePass_t g_platePass = nullptr;
    int __cdecl PlatePassDetour(uint32_t a)
    {
        const int r = g_platePass(a);
        const uint8_t* angle = Var("nameplateAngle");
        if (!angle || !(VarF(angle) != 0.0f))
            return r;
        uint8_t* player = ActivePlayer();
        if (!player)
            return r;
        float from[3] = {0.0f, 0.0f, 0.0f};
        Position(player, from);
        for (uint32_t plate = *reinterpret_cast<const uint32_t*>(kPlateList); (plate & 1) == 0 && plate != 0;
             plate = *reinterpret_cast<const uint32_t*>(plate + 0x2A0))
        {
            uint8_t* frame = reinterpret_cast<uint8_t*>(plate);
            uint8_t* unit = static_cast<uint8_t*>(ObjectPtr(*reinterpret_cast<const uint64_t*>(frame + 0x2A8), 0x18));
            if (!unit)
            {
                Show(frame);
                continue;
            }
            float at[3] = {0.0f, 0.0f, 0.0f};
            Position(unit, at);
            if (Distance(from, at) >= VarF("nameplateDistance"))
            {
                Hide(frame);
                continue;
            }
            float facing = Facing(player);
            if (facing > 3.1415f)
                facing = facing - 6.283f;
            const float bearing = reinterpret_cast<float(__cdecl*)(const float*, const float*)>(0x4F5130)(from, at);
            const float delta = bearing - facing;
            if (Distance(from, at) <= 3.0f || std::fabs(delta) <= VarF(angle))
                Show(frame);
            else
                Hide(frame);
        }
        return r;
    }

    // FUN_102c2630 over 0x72B060 (__thiscall(unit, view, out), ret 8): the plate's screen position.
    // Nothing while the client's plates are off. The player's own plate (nameplateShowPersonal = 1) is
    // placed by nameplatePersonalPosition: 1 at vtable +0x20, 2 at +0x2C raised by 0.04. Neutral units
    // out of combat are skipped unless nameplateShowNeutral or targeted; nameplateInCombatOnly skips the
    // rest out of combat. With nameplateFixedVerticalOffset, the height offset shrinks 0.025 per yard.
    typedef int(__fastcall* PlatePosition_t)(uint8_t*, void*, void*, float*);
    PlatePosition_t g_platePosition = nullptr;
    int __fastcall PlatePositionDetour(uint8_t* unit, void* edx, void* view, float* out)
    {
        if (*reinterpret_cast<const uint32_t*>(kPlatesShown) == 0)
            return 0;
        uint8_t* player = ActivePlayer();
        if (!player)
            return g_platePosition(unit, edx, view, out);
        typedef void(__thiscall* Project_t)(void*, float*, float*, int);   // 0x4F6D20
        if (VarI("nameplateShowPersonal") == 1 && unit == player)
        {
            if (g_personalPosition == 1)
            {
                RawPosition(player, out);
                reinterpret_cast<Project_t>(0x4F6D20)(view, out, out, 0);
                return 1;
            }
            if (g_personalPosition == 2)
            {
                Position(player, out);
                reinterpret_cast<Project_t>(0x4F6D20)(view, out, out, 0);
                out[1] = out[1] - 0.04f;
                return 1;
            }
            return 0;
        }
        if (!g_showNeutral && reinterpret_cast<int(__thiscall*)(void*, void*)>(0x7251C0)(player, unit) == 3 &&
            ((UnitFlags(unit) >> 0x13) & 1) == 0)
        {
            const uint8_t* d = *reinterpret_cast<uint8_t* const*>(unit + 8);
            if (!Equals(d, TargetGuid()))
                return 0;
        }
        if (VarI("nameplateInCombatOnly") == 1 && ((UnitFlags(unit) >> 0x13) & 1) == 0)
            return 0;
        const uint8_t* vertical = Var("nameplateVerticalOffset");
        const uint8_t* fixed = Var("nameplateFixedVerticalOffset");
        if (vertical && fixed && VarI(fixed) != 0)
        {
            float from[3];
            float at[3];
            Position(player, from);
            Position(unit, at);
            g_verticalOffset = VarF(vertical) - Distance(from, at) * 0.025f;
        }
        return g_platePosition(unit, edx, view, out);
    }

    // sub_102beb70 over 0x7256C0 (__thiscall(unit, a, b), ret 8): a unit that had no plate before the
    // original ran joins the registry. The original tests only the low byte of the +0xC38 pointer.
    typedef int(__fastcall* PlateCreate_t)(uint8_t*, void*, void*, void*);
    PlateCreate_t g_plateCreate = nullptr;
    int __fastcall PlateCreateDetour(uint8_t* unit, void* edx, void* a, void* b)
    {
        const uint8_t had = unit[0xC38];
        const int r = g_plateCreate(unit, edx, a, b);
        if (had == 0)
            AddPlate(unit);
        return r;
    }

    // FUN_102beb40 over 0x725840 (__thiscall(unit)): once the original has released the plate (low
    // byte of +0xC38 now 0), the unit leaves the registry.
    typedef int(__fastcall* PlateRelease_t)(uint8_t*, void*);
    PlateRelease_t g_plateRelease = nullptr;
    int __fastcall PlateReleaseDetour(uint8_t* unit, void* edx)
    {
        const int r = g_plateRelease(unit, edx);
        if (unit[0xC38] == 0)
            RemovePlate(unit);
        return r;
    }

    // sub_102c28b0 over 0x9900E0 (__cdecl, six arguments, the first two a GUID): after the original,
    // client event 0x12 is raised for every unit token naming that GUID (0x60BF10).
    typedef int(__cdecl* UnitUpdate_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    UnitUpdate_t g_unitUpdate = nullptr;
    int __cdecl UnitUpdateDetour(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6)
    {
        const int r = g_unitUpdate(a1, a2, a3, a4, a5, a6);
        reinterpret_cast<void(__cdecl*)(uint32_t*, int)>(0x60BF10)(&a1, 0x12);
        return r;
    }

    // sub_102be530 over 0x715C30 (returns the plate at 0xCA1204): not when it is the player's own.
    typedef uint8_t*(__cdecl* CursorPlate_t)();
    CursorPlate_t g_cursorPlate = nullptr;
    uint8_t* __cdecl CursorPlateDetour()
    {
        const uint8_t* plate = *reinterpret_cast<uint8_t* const*>(kCursorPlate);
        if (plate && Equals(plate + 0x2A8, ActivePlayerGuid()))
            return nullptr;
        return g_cursorPlate();
    }

    // FUN_102c2e90 over 0x98E910 (__thiscall, no stack arguments): skipped (0) when the plate at
    // 0xCA1204 is the player's own. The original dereferences that plate unchecked.
    typedef int(__fastcall* CursorPlateUse_t)(void*, void*);
    CursorPlateUse_t g_cursorPlateUse = nullptr;
    int __fastcall CursorPlateUseDetour(void* ecx, void* edx)
    {
        const uint8_t* plate = *reinterpret_cast<uint8_t* const*>(kCursorPlate);
        if (Equals(plate + 0x2A8, ActivePlayerGuid()))
            return 0;
        return g_cursorPlateUse(ecx, edx);
    }

    // ---- tick (FUN_102c29a0, after 0x403340) --------------------------------------------------------
    // FUN_102c0a70 (0x4D4B30 callback): each unit with a plate, with its distance from the player.
    int __cdecl CollectPlate(uint32_t lo, uint32_t hi, const float* from)
    {
        uint8_t* unit = static_cast<uint8_t*>(ObjectPtr((static_cast<uint64_t>(hi) << 32) | lo, 0x18));
        if (unit && *reinterpret_cast<const uint32_t*>(unit + 0xC38) != 0)
        {
            float at[3];
            Position(unit, at);
            PlateOrder o;
            o.distance = Distance(from, at);
            o.unused = 0;
            o.guidLo = lo;
            o.guidHi = hi;
            o.plate = *reinterpret_cast<void* const*>(unit + 0xC38);
            o.unused2 = 0;
            g_order.push_back(o);
        }
        return 1;
    }

    // Queued NAME_PLATE_UNIT_ADDED("nameplateN") events, then every plate's frame level (0x4910A0):
    // 10, 20, ... farthest first, the target's last (FUN_102be160's order).
    void Tick()
    {
        uint8_t* player = ActivePlayer();
        if (!player)
            return;
        float from[3];
        Position(player, from);
        const size_t queued = g_addedIds.size();
        for (size_t i = 0; i < queued && i < g_addedIds.size(); ++i)
            AscRuntime::Signal("NAME_PLATE_UNIT_ADDED", "%s", Token(g_addedIds[i]).c_str());
        g_addedIds.clear();

        reinterpret_cast<void(__cdecl*)(int(__cdecl*)(uint32_t, uint32_t, const float*), const float*)>(0x4D4B30)(&CollectPlate, from);
        if (g_order.empty())
            return;
        const uint64_t target = TargetGuid();
        const uint32_t tLo = static_cast<uint32_t>(target), tHi = static_cast<uint32_t>(target >> 32);
        std::sort(g_order.begin(), g_order.end(), [tLo, tHi](const PlateOrder& a, const PlateOrder& b) {
            const bool aTarget = a.guidLo == tLo && a.guidHi == tHi;
            const bool bTarget = b.guidLo == tLo && b.guidHi == tHi;
            return !aTarget && (bTarget || a.distance > b.distance);
        });
        int level = 10;
        for (const PlateOrder& o : g_order)
        {
            reinterpret_cast<void(__thiscall*)(void*, int, int)>(0x4910A0)(o.plate, level, 1);
            level += 10;
        }
        g_order.clear();
    }

    // ---- GetNamePlateForUnit (FUN_102bf780) -----------------------------------------------------------
    // sub_102bd5d0 (0x4D4B30 callback): every unit object.
    int __cdecl CollectUnit(uint32_t lo, uint32_t hi, void*)
    {
        if (uint8_t* unit = static_cast<uint8_t*>(ObjectPtr((static_cast<uint64_t>(hi) << 32) | lo, 0x18)))
            g_units.push_back(unit);
        return 1;
    }

    // Exactly one argument (a unit token), else nothing. The first unit whose GUID matches decides: its
    // registry entry's plate frame (its Lua object created through 0x819880 if it has none yet), or
    // nothing.
    int GetNamePlateForUnit(lua_State* L)
    {
        if (AscLua::lua_gettop(L) != 1)
            return 0;
        const std::string token = CheckString(L, 1);
        const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x60C1C0)(token.c_str());   // FUN_103086c0
        g_units.clear();
        reinterpret_cast<void(__cdecl*)(int(__cdecl*)(uint32_t, uint32_t, void*), void*)>(0x4D4B30)(&CollectUnit, nullptr);
        for (uint8_t* unit : g_units)
        {
            if (!Equals(*reinterpret_cast<uint8_t* const*>(unit + 8), guid))
                continue;
            for (auto& kv : g_plates)
                if (kv.second->unit == unit)
                {
                    PlateEntry* e = g_plates[kv.first];
                    if (!e)
                        return 0;
                    uint8_t* plate = *reinterpret_cast<uint8_t* const*>(e->unit + 0xC38);
                    if (!plate)
                        return 0;
                    if (*reinterpret_cast<const uint32_t*>(plate + 4) == 0)
                        reinterpret_cast<void(__thiscall*)(void*, int)>(0x819880)(plate, 0);
                    reinterpret_cast<void(__cdecl*)(lua_State*, int, int)>(0x84E670)(L, -10000, *reinterpret_cast<const int32_t*>(plate + 8));
                    return 1;
                }
            return 0;
        }
        return 0;
    }

    // ---- "nameplateN" unit tokens (FUN_10a4ea40, detour LAB_10a6ec00 over 0x60ABF0) ------------------
    // A token of at least 9 characters containing "nameplate" anywhere loses its first 9 characters;
    // the rest is read as std::stoul(rest, nullptr, 0) (FUN_102bf410) and looked up in the registry.
    // Deviation: stoul's invalid_argument / out_of_range would escape through the client's frames in
    // the original; here such a token is simply not a plate. An empty remainder is not a plate either.
    PlateEntry* PlateForToken(const char* token)
    {
        if (!token)
            return nullptr;
        std::string s(token);
        if (s.size() < 9 || s.find("nameplate") == std::string::npos)
            return nullptr;
        s.erase(0, 9);
        if (s.empty())
            return nullptr;
        char* end = nullptr;
        errno = 0;
        const unsigned long id = strtoul(s.c_str(), &end, 0);
        if (end == s.c_str() || errno == ERANGE)
            return nullptr;
        if (g_plates.find(static_cast<uint32_t>(id)) == g_plates.end())
            return nullptr;
        return g_plates[static_cast<uint32_t>(id)];
    }

    typedef uint32_t(__cdecl* TokenGuid_t)(const char*, uint64_t*, int);
    TokenGuid_t g_tokenGuid = nullptr;
    uint32_t __cdecl TokenGuidDetour(const char* token, uint64_t* guid, int a3)
    {
        PlateEntry* e = PlateForToken(token);
        if (e && e->unit)
        {
            *guid = *reinterpret_cast<const uint64_t*>(*reinterpret_cast<uint8_t* const*>(e->unit + 8));
            return 1;
        }
        return g_tokenGuid(token, guid, a3);
    }

    // ---- CVar callbacks -------------------------------------------------------------------------------
    int __cdecl OnVerticalOffset(void*, const char*, const char* value, void*)   // sub_102bf350
    {
        g_verticalOffset = static_cast<float>(atof(value));
        return 1;
    }
    // FUN_102bebc0: as SetNamePlateRange, 5..60 yards stored squared at 0xADAA7C.
    int __cdecl OnDistance(void*, const char*, const char* value, void*)
    {
        const float x = static_cast<float>(atof(value));
        const float lo = 5.0f <= x ? x : 5.0f;
        const float r = x <= 60.0f ? lo : 60.0f;
        PatchValue<float>(0xADAA7C, r * r);
        return 1;
    }
    int __cdecl OnAngle(void*, const char*, const char*, void*)   // sub_102beba0: rerun the plate pass
    {
        reinterpret_cast<int(__cdecl*)(uint32_t)>(0x725890)(*reinterpret_cast<const uint32_t*>(kPassArgument));
        return 1;
    }
    int __cdecl OnShowNeutral(void*, const char*, const char* value, void*)   // sub_102bf330
    {
        g_showNeutral = atoi(value) != 0;
        return 1;
    }
    // FUN_102bee60: "1" NOPs the dead-unit skip at 0x72B07A (jle 0x72B0DB); anything else restores it.
    int __cdecl OnShowDead(void*, const char*, const char* value, void*)
    {
        static const uint8_t kSkip[] = {0x7E, 0x5F};
        static const uint8_t kNop[] = {0x90, 0x90};
        Patch(0x72B07A, strcmp(value, "1") != 0 ? kSkip : kNop, 2);
        return 1;
    }
    int __cdecl OnPersonalPosition(void*, const char*, const char* value, void*)   // sub_102bee20
    {
        g_personalPosition = atoi(value);
        if (g_personalPosition < 1)
            g_personalPosition = 1;
        else if (g_personalPosition > 2)
            g_personalPosition = 2;
        return 1;
    }

    const AscNamePlates::CVarSpec kCVars[] = {
        {"nameplateVerticalOffset", "0.0", reinterpret_cast<void*>(&OnVerticalOffset)},
        {"nameplateFixedVerticalOffset", "0", nullptr},
        {"nameplateDistance", "1681", reinterpret_cast<void*>(&OnDistance)},
        {"nameplateAngle", "0.0", reinterpret_cast<void*>(&OnAngle)},
        {"nameplateIntersectOpacity", "0.5", nullptr},
        {"nameplateIntersectUseCamera", "0", nullptr},
        {"nameplateFadeIn", "1", nullptr},
        {"nameplateShowNeutral", "1", reinterpret_cast<void*>(&OnShowNeutral)},
        {"nameplateShowDead", "0", reinterpret_cast<void*>(&OnShowDead)},
        {"nameplateShowPersonal", "0", nullptr},
        {"nameplatePersonalPosition", "2", reinterpret_cast<void*>(&OnPersonalPosition)},
        {"nameplateInCombatOnly", "0", nullptr},
    };

    // ---- installer (FUN_102bf920) -------------------------------------------------------------------
    void Init()
    {
        // FUN_102bf920 -> FUN_10114540 x12: no help, flags 0x21, category 4, registered after world load.
        for (const AscNamePlates::CVarSpec& c : kCVars)
            AscClientOptions::QueueWorldCVar({c.name, c.defaultValue, 0x21, 4, c.callback});

        const uint8_t push1 = 1;
        Patch(0x72594B, &push1, 1);
        static const uint8_t kNop8[8] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        Patch(0x7258CE, kNop8, 8);

        AscRuntime::OnLeaveWorld(&ClearPlates);
        AscRuntime::OnAfter403340(&Tick);
        g_plateUpdate = reinterpret_cast<PlateUpdate_t>(AscRuntime::Detour(0x98E9F0, 6, reinterpret_cast<void*>(&PlateUpdateDetour)));
        g_platePass = reinterpret_cast<PlatePass_t>(AscRuntime::Detour(0x725890, 6, reinterpret_cast<void*>(&PlatePassDetour)));
        g_platePosition = reinterpret_cast<PlatePosition_t>(AscRuntime::Detour(0x72B060, 6, reinterpret_cast<void*>(&PlatePositionDetour)));

        static const uint8_t kNop12[12] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
        Patch(0x98E9F9, kNop12, 12);
        const uint8_t rel = 0x1F;
        Patch(0x98EA07, &rel, 1);

        g_plateCreate = reinterpret_cast<PlateCreate_t>(AscRuntime::Detour(0x7256C0, 6, reinterpret_cast<void*>(&PlateCreateDetour)));
        g_plateRelease = reinterpret_cast<PlateRelease_t>(AscRuntime::Detour(0x725840, 9, reinterpret_cast<void*>(&PlateReleaseDetour)));
        g_unitUpdate = reinterpret_cast<UnitUpdate_t>(AscRuntime::Detour(0x9900E0, 6, reinterpret_cast<void*>(&UnitUpdateDetour)));
        g_cursorPlate = reinterpret_cast<CursorPlate_t>(AscRuntime::Detour(0x715C30, 5, reinterpret_cast<void*>(&CursorPlateDetour)));
        g_cursorPlateUse = reinterpret_cast<CursorPlateUse_t>(AscRuntime::Detour(0x98E910, 5, reinterpret_cast<void*>(&CursorPlateUseDetour)));

        PatchValue<float*>(0x715739, &g_verticalOffset);
        g_tokenGuid = reinterpret_cast<TokenGuid_t>(AscRuntime::Detour(0x60ABF0, 5, reinterpret_cast<void*>(&TokenGuidDetour)));
        const uint8_t jmp = 0xEB;
        Patch(0x71557D, &jmp, 1);
        Patch(0x715586, &jmp, 1);
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "GetNamePlateForUnit", GetNamePlateForUnit},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

const AscNamePlates::CVarSpec* AscNamePlates::CVars(size_t& count)
{
    count = sizeof(kCVars) / sizeof(kCVars[0]);
    return kCVars;
}

// FUN_102bf3c0: the id of the plate showing `unit`, 0 when none does.
uint32_t AscNamePlates_IdForUnit(const void* unit)
{
    for (auto& kv : g_plates)
        if (kv.second->unit == unit)
        {
            const PlateEntry* e = g_plates[kv.first];
            return e ? e->id : 0;
        }
    return 0;
}
