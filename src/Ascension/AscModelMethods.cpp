// Model widget methods the original appends to the client's Model table (hook on 0x9603D0,
// FUN_10a467e0): SetSpell, SetSpellVisual, GetSpell, SetUnitGUID, SetSheathe. See AscWidget.cpp.
//
// Every method acts on the frame's CM2Model (frame +0x2A0) and does nothing without one.
//
// SetSpell / SetSpellVisual drive the DLL's own spell-visual kit player (0x10308E40..0x10309DB1):
//   FUN_10309880 PlayKit(model, kit, noAnim, loopEffects): each SpellVisualKitModelAttach row of the
//     kit, then the kit's twelve effect columns at fixed attachment points, attached through
//     FUN_10309560; then the kit's animation. True when anything was attached or played.
//   FUN_10309450 PlayVisual(model, spell, visual): instant spells play precast + cast + impact and
//     reset on the animation's end (0x103080F0); cast-time spells play precast, then cast + impact
//     when its animation ends (0x10309D50).
//   FUN_10309690 PlayMissile: the missile state at 0x10BCCD68 (FUN_10112860 / launch FUN_10112660 /
//     tick FUN_10112890).
// The original's missile tick (0x10A76860) has no caller in plain code -- the VM-protected region
// drives it -- so a 16 ms timer runs it here, and launch waits until the model's M2 data is loaded.
// The two exe bytes 0x5971C1 / 0x59714D (conditional jumps in the model frame's update, 0x5971B0 /
// 0x597140) are flipped between jne (0x75) and jmp (0xEB) exactly as the original does, through
// NtProtectVirtualMemory there and VirtualProtect here.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscModelKit.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <windows.h>
#include <cstring>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    typedef void  (__cdecl* RawGetI_t)(lua_State*, int, int);
    typedef void* (__cdecl* ToUserdata_t)(lua_State*, int);
    const RawGetI_t    lua_rawgeti    = reinterpret_cast<RawGetI_t>(0x84E670);
    const ToUserdata_t lua_touserdata = reinterpret_cast<ToUserdata_t>(0x84E1C0);

    void* Self(lua_State* L)   // self[0]
    {
        lua_rawgeti(L, 1, 0);
        void* obj = lua_touserdata(L, -1);
        AscLua::lua_settop(L, -2);
        return obj;
    }
    void* ModelOf(lua_State* L)   // frame +0x2A0
    {
        uint8_t* frame = static_cast<uint8_t*>(Self(L));
        return frame ? *reinterpret_cast<void**>(frame + 0x2A0) : nullptr;
    }

    // ---- client CM2Model calls ------------------------------------------------------------------------
    // The client calls it with the model first (0x832229: model +4, +8, +0x10 anim, +0x14, &+0x18, +0x24 arg, ...);
    // the original's callbacks read the model from their first argument.
    typedef void(__cdecl* AnimCallback)(void* model, void*, uint32_t anim, void*, void*, uint32_t arg);
    void SetCallback(void* m, AnimCallback fn, uint32_t arg)   // 0x823FE0
    {
        reinterpret_cast<void(__thiscall*)(void*, AnimCallback, uint32_t, int)>(0x823FE0)(m, fn, arg, 0);
    }
    void PlayAnim(void* m, uint32_t anim, float speed)   // 0x832AB0
    {
        uint32_t nanBits = 0x7FC00000;
        float nan;
        memcpy(&nan, &nanBits, 4);
        reinterpret_cast<void(__thiscall*)(void*, int, uint32_t, float, int, float, int, int)>(0x832AB0)(m, -1, anim, nan, 0, speed, 1, 1);
    }
    void Detach(void* m, uint32_t attach)   // 0x827560
    {
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x827560)(m, attach);
    }
    bool HasAnim(void* m, uint32_t anim)   // 0x825EE0
    {
        return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x825EE0)(m, anim) != 0;
    }
    uint32_t Now()   // 0x86AE20
    {
        return reinterpret_cast<uint32_t(__cdecl*)()>(0x86AE20)();
    }

    // ---- client DBC rows ------------------------------------------------------------------------------
    const uint8_t* SpellVisual(uint32_t id)         { return ClientDbcRow(0xAD4AA8, id); }   // FUN_100b2620
    const uint8_t* SpellVisualKit(uint32_t id)      { return ClientDbcRow(0xAD4A3C, id); }   // FUN_100b2680
    const uint8_t* SpellVisualEffectName(uint32_t id) { return ClientDbcRow(0xAD4A18, id); } // FUN_100b2650
    const uint8_t* SpellCastTimes(uint32_t id)      { return ClientDbcRow(0xAD4748, id); }   // FUN_100b22c0
    uint32_t U32(const uint8_t* p, uint32_t off) { uint32_t v; memcpy(&v, p + off, 4); return v; }
    float F32(const uint8_t* p, uint32_t off)    { float v; memcpy(&v, p + off, 4); return v; }

    // ---- 4x4 row-major matrix (FUN_102bd230 / 310 / 3f0 / 4d0 / 530) ----------------------------------
    struct Mat { float m[16]; };
    Mat Mul(const Mat& a, const Mat& b)   // FUN_102bcea0
    {
        Mat o;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                o.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j] + a.m[i * 4 + 3] * b.m[12 + j];
        return o;
    }
    void Rotate(Mat& t, int axis, float a)   // t = R * t
    {
        const float c = cosf(a), s = sinf(a);
        Mat r = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        if (axis == 0)      { r.m[5] = c; r.m[6] = s; r.m[9] = -s; r.m[10] = c; }
        else if (axis == 1) { r.m[0] = c; r.m[2] = -s; r.m[8] = s; r.m[10] = c; }
        else                { r.m[0] = c; r.m[1] = s; r.m[4] = -s; r.m[5] = c; }
        t = Mul(r, t);
    }
    void Translate(Mat& t, const float* v)
    {
        for (int j = 0; j < 3; ++j)
            t.m[12 + j] += v[0] * t.m[j] + v[1] * t.m[4 + j] + v[2] * t.m[8 + j];
    }
    void Scale(Mat& t, float s)
    {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                t.m[i * 4 + j] *= s;
    }

    // ---- kit player -----------------------------------------------------------------------------------
    // 0x10309DE0: an attached effect's animation ended -- loop its "stand" (0x9F) or replay.
    void __cdecl OnEffectAnimEnd(void* model, void*, uint32_t anim, void*, void*, uint32_t)
    {
        SetCallback(model, nullptr, 0);
        if (HasAnim(model, 0x9F))
            PlayAnim(model, 0x9F, 0.0f);
        else
            PlayAnim(model, anim, 1.0f);
    }

    // FUN_10309560: create the effect model and attach it with offset / rotation / effect scale.
    bool AttachEffect(void* model, uint32_t effectName, uint32_t attach, bool loop, const float* offset, const float* rot)
    {
        const uint8_t* e = SpellVisualEffectName(effectName);
        if (!e)
            return false;
        void* scene = *reinterpret_cast<void**>(static_cast<uint8_t*>(model) + 0x28);
        void* fx = reinterpret_cast<void*(__thiscall*)(void*, const char*, int)>(0x81F8F0)(
            scene, *reinterpret_cast<const char* const*>(e + 8), 0);
        if (!fx)
            return false;
        if (loop)
            SetCallback(fx, OnEffectAnimEnd, 0);
        reinterpret_cast<void(__thiscall*)(void*, void*, uint32_t, int, int)>(0x831630)(fx, model, attach, 0, 0);
        Mat t = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
        reinterpret_cast<void(__thiscall*)(Mat*)>(0x407F40)(&t);
        if (offset)
            Translate(t, offset);
        if (rot)
        {
            Rotate(t, 0, rot[0]);
            Rotate(t, 1, rot[1]);
            Rotate(t, 2, rot[2]);
        }
        Scale(t, F32(e, 0x10));
        reinterpret_cast<void(__thiscall*)(void*, Mat*)>(0x4D8630)(fx, &t);
        return true;
    }

    // FUN_10309880
    bool PlayKit(void* model, uint32_t kitId, bool noAnim, bool loop)
    {
        const uint8_t* kit = SpellVisualKit(kitId);
        if (!kit)
            return false;
        std::vector<uint32_t> attached;
        // SpellVisualKitModelAttach (container 0xAD4A84, ids min..max): {+4 kit, +8 effect, +0xC
        // attachment, +0x10 offset xyz, +0x1C yaw/pitch/roll}.
        const uint32_t first = *reinterpret_cast<const uint32_t*>(0xAD4A94);
        for (uint32_t id = first; id <= *reinterpret_cast<const uint32_t*>(0xAD4A90); ++id)
        {
            const uint8_t* a = ClientDbcRow(0xAD4A84, id);
            if (!a || U32(a, 4) != U32(kit, 0))
                continue;
            float offset[3], rot[3];
            memcpy(offset, a + 0x10, 12);
            memcpy(rot, a + 0x1C, 12);
            if (AttachEffect(model, U32(a, 8), U32(a, 0xC), loop, offset, rot))
                attached.push_back(U32(a, 0xC));
        }
        static const uint32_t kColumns[][2] = {   // kit column offset, attachment point
            {0x0C, 0x14}, {0x10, 0x22}, {0x14, 0x13}, {0x18, 0x15}, {0x1C, 0x16}, {0x20, 0x11},
            {0x24, 0x20}, {0x28, 0x21}, {0x2C, 0x17}, {0x30, 0x18}, {0x34, 0x19}, {0x38, 0x00},
        };
        for (const auto& c : kColumns)
            if (AttachEffect(model, U32(kit, c[0]), c[1], loop, nullptr, nullptr))
                attached.push_back(c[1]);
        const uint32_t anim = U32(kit, 8);
        if (!noAnim && anim != 0xFFFFFFFF)
        {
            PlayAnim(model, anim, 1.0f);
            noAnim = false;
        }
        return !(attached.empty() && (noAnim || anim == 0xFFFFFFFF));
    }

    // 0x103080F0: the cast finished -- back to stand, every effect point cleared.
    void __cdecl OnCastEnd(void* model, void*, uint32_t, void*, void*, uint32_t visual)
    {
        if (visual == 0 || !SpellVisual(visual))
            return;
        SetCallback(model, nullptr, 0);
        PlayAnim(model, 0, 1.0f);
        for (uint32_t a : {0x14u, 0x22u, 0x13u, 0x15u, 0x16u, 0x11u, 0x20u, 0x21u, 0x17u, 0x18u, 0x19u})
            Detach(model, a);
    }

    // 0x10309D50: the precast animation ended -- play cast + impact.
    void __cdecl OnPrecastEnd(void* model, void*, uint32_t anim, void*, void*, uint32_t visual)
    {
        if (visual == 0)
            return;
        const uint8_t* v = SpellVisual(visual);
        const uint8_t* precast = v ? SpellVisualKit(U32(v, 4)) : nullptr;
        if (!precast || anim != U32(precast, 8))
            return;
        SetCallback(model, nullptr, 0);
        if (!PlayKit(model, U32(v, 8), false, false))
            return;
        PlayKit(model, U32(v, 0xC), false, false);
        if (const uint8_t* cast = SpellVisualKit(U32(v, 8)))
            SetCallback(model, OnCastEnd, U32(cast, 8));
    }

    // FUN_10309450
    bool PlayVisual(void* model, uint32_t spell, uint32_t visualOverride)
    {
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return true;
        const uint8_t* v = SpellVisual(visualOverride ? visualOverride : U32(rec, 0x20C));
        if (!v || !SpellVisualKit(U32(v, 4)))
            return false;
        const uint8_t* castTime = SpellCastTimes(U32(rec, 0x70));
        if (!castTime)
            return false;
        if (U32(castTime, 4) != 0)
        {
            if (!PlayKit(model, U32(v, 4), false, false))
                return false;
            SetCallback(model, OnPrecastEnd, U32(v, 0));
            return true;
        }
        PlayKit(model, U32(v, 4), false, false);
        PlayKit(model, U32(v, 8), false, false);
        PlayKit(model, U32(v, 0xC), false, false);
        SetCallback(model, OnCastEnd, U32(v, 0));
        return true;
    }

    // ---- missile (0x10BCCD68) -------------------------------------------------------------------------
    struct Missile
    {
        void* model = nullptr;      // +0x00
        uint32_t spell = 0;         // +0x04
        uint32_t start = 0;         // +0x08
        void* fx = nullptr;         // +0x0C
        float pos[3] = {};          // +0x10
        float height = 0;           // +0x1C
        float speed = 0;            // +0x20
        bool loop = false;          // +0x24
    } g_missile;
    uint32_t g_missileTimer = 0;

    void MissileLaunch(Missile& m)   // FUN_10112660
    {
        m.start = 0;
        uint8_t rec[0x2A8];
        if (!FetchSpell(m.spell, rec))
            return;
        const uint8_t* v = SpellVisual(U32(rec, 0x20C));
        if (!v || U32(v, 0x1C) == 0)
            return;
        const uint8_t* e = SpellVisualEffectName(U32(v, 0x20));
        if (!e)
            return;
        auto hasAttach = [&](uint32_t a) { return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x8273D0)(m.model, a) != 0; };
        int32_t attach = static_cast<int32_t>(U32(v, 0x40));
        if (!hasAttach(static_cast<uint32_t>(attach)))
        {
            if (!hasAttach(0xF))
                attach = 0xF;
            else
                attach = hasAttach(0x13) ? 0x13 : -1;
        }
        void* scene = *reinterpret_cast<void**>(static_cast<uint8_t*>(m.model) + 0x28);
        m.fx = reinterpret_cast<void*(__thiscall*)(void*, const char*, int)>(0x81F8F0)(scene, *reinterpret_cast<const char* const*>(e + 8), 0);
        if (!m.fx)
            return;
        reinterpret_cast<void(__thiscall*)(void*, void*, int32_t, int, int)>(0x831630)(m.fx, m.model, attach, 0, 0);
        auto hasTag = [&](uint32_t tag) { return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x8275F0)(m.model, tag) != 0; };
        auto attachPosRaw = reinterpret_cast<void(__thiscall*)(void*, float*, int32_t)>(0x831330);
        // Deviation: 0x831330 faults on an attachment the model lacks (0x4C21BC), and the fallback
        // above picks 0xF exactly when the model has no 0xF (verified in the disassembly at
        // 0x101126E2), or -1. Query only attachments that exist; the others stay at zero.
        auto attachPos = [&](float* out, int32_t a) {
            if (a >= 0 && hasAttach(static_cast<uint32_t>(a)))
                attachPosRaw(m.model, out, a);
        };
        float hand[3] = {}, origin[3] = {};
        if (hasTag(0x4C534324))        // "$CSL"
            attachPos(hand, 0x15);
        else if (hasTag(0x52534324))   // "$CSR"
            attachPos(hand, 0x16);
        else if (hasTag(0x54534324))   // "$CST"
            attachPos(hand, 0x22);
        attachPos(origin, attach);
        // Gnome models are measured 0.75 lower (DLL 0x10B2F5D4).
        const char* file = reinterpret_cast<const char*>(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(m.model) + 0x2C) + 0x3C);
        float z = origin[2];
        if (strcmp(file, "character\\gnome\\male\\gnomemale.m2") == 0 || strcmp(file, "character\\gnome\\female\\gnomefemale.m2") == 0)
            z -= 0.75f;
        m.pos[0] = hand[0] - origin[0];
        m.pos[1] = hand[1] - origin[1];
        m.pos[2] = hand[2] - z;
        m.height = hasAttach(0x15) ? reinterpret_cast<float(__thiscall*)(void*, uint32_t)>(0x831550)(m.model, 0x15) : 0.0f;   // same guard
        m.speed = F32(rec, 0xBC);
    }

    int __cdecl MissileTick(void*)   // FUN_10112890 (0x10A76860)
    {
        Missile& m = g_missile;
        // Deviation: our timer can run before a freshly set model has its M2 data (+0x2C); the
        // attachment calls fault on it (0x8273D0), so launch waits for it.
        if (m.start != 0 && Now() >= m.start && *reinterpret_cast<void**>(static_cast<uint8_t*>(m.model) + 0x2C))
            MissileLaunch(m);
        if (m.fx && m.loop)
        {
            m.pos[0] += m.speed * 0.001f;
            if (m.pos[0] >= 25.0f)
            {
                reinterpret_cast<void(__thiscall*)(void*)>(0x8274F0)(m.fx);
                m.fx = nullptr;
            }
            else
                reinterpret_cast<void(__thiscall*)(void*, float*, int, float)>(0x8251D0)(m.fx, m.pos, 0, m.height);
        }
        g_missileTimer = AscRuntime::Schedule(16, MissileTick, nullptr);
        return 0;
    }

    // FUN_10309690 + FUN_10112860
    bool PlayMissile(void* model, uint32_t spell, uint32_t visualOverride, uint32_t start, bool clear, bool loop)
    {
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return false;
        const uint8_t* v = SpellVisual(visualOverride ? visualOverride : U32(rec, 0x20C));
        if (!v || U32(v, 0x1C) == 0)
            return false;
        g_missile.model = model;
        g_missile.spell = spell;
        g_missile.start = start ? start : Now();
        g_missile.loop = loop;
        if (clear)
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(model) + 0x17C) = 0;
        if (!g_missileTimer)
            g_missileTimer = AscRuntime::Schedule(16, MissileTick, nullptr);
        return true;
    }

    // ---- the two exe jump bytes -----------------------------------------------------------------------
    void PatchByte(uint32_t address, uint8_t value)
    {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(address), 1, PAGE_EXECUTE_READWRITE, &old))
        {
            *reinterpret_cast<volatile uint8_t*>(address) = value;
            VirtualProtect(reinterpret_cast<void*>(address), 1, old, &old);
        }
    }
    void SetFrameJumps(uint8_t op)
    {
        PatchByte(0x5971C1, op);
        PatchByte(0x59714D, op);
    }

    uint32_t g_spell = 0;    // 0x10D3DA04
    uint32_t g_visual = 0;   // 0x10D3DA08

    // Shared body of handler_SetSpell / handler_SetSpellVisual (visual 0 = the spell's own).
    int ApplySpell(void* model, uint32_t spell, uint32_t visual, bool explicitVisual)
    {
        if (spell == 0)
        {
            SetFrameJumps(0x75);
            Detach(model, 0x15);
            Detach(model, 0x16);
            PlayAnim(model, 0, 1.0f);
        }
        uint8_t rec[0x2A8];
        if (!FetchSpell(spell, rec))
            return 0;
        const uint8_t* v = SpellVisual(explicitVisual ? visual : U32(rec, 0x20C));
        if (!v)
            return 0;
        if (U32(v, 0x10) != 0)
            PlayKit(model, U32(v, 0x10), false, true);
        else if (U32(v, 0x1C) != 0)
            PlayMissile(model, spell, 0, 1, false, true);
        else if (U32(v, 8) != 0)
        {
            SetFrameJumps(0xEB);
            PlayVisual(model, spell, explicitVisual ? visual : 0);
        }
        return 0;
    }

    // handler_SetSpell (0x10a6c1c0): (spell)
    int SetSpell(lua_State* L)
    {
        void* model = ModelOf(L);
        if (!model)
            return 0;
        g_spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        return ApplySpell(model, g_spell, 0, false);
    }

    // handler_SetSpellVisual (0x10a6ca30): (spell, visual)
    int SetSpellVisual(lua_State* L)
    {
        void* model = ModelOf(L);
        if (!model)
            return 0;
        g_spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        g_visual = static_cast<uint32_t>(ToInt(CheckNumber(L, 3)));
        return ApplySpell(model, g_spell, g_visual, true);
    }

    // 0x10a6be90: the last spell given to any model.
    int GetSpell(lua_State* L)
    {
        AscLua::lua_pushinteger(L, static_cast<int32_t>(g_spell));
        return 1;
    }

    // FUN_10a6beb0: sheathe both hands' weapons (the exe's item array 0xC0E4F8) on the model.
    int SetSheathe(lua_State* L)
    {
        void* model = ModelOf(L);
        if (!model)
            return 0;
        const uint32_t* items = reinterpret_cast<const uint32_t*>(0xC0E4F8);
        for (uint32_t i = 0; i < 2; ++i)
        {
            const uint8_t* item = static_cast<const uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, uint32_t, void*, void*, void*, int)>(0x67CA30)(
                reinterpret_cast<void*>(0xC5D828), items[i], nullptr, nullptr, nullptr, 0));
            if (!item)
                continue;
            uint8_t display[0x64];
            if (!reinterpret_cast<int(__thiscall*)(void*, uint32_t, void*)>(0x4CFD90)(reinterpret_cast<void*>(0xAD3DDC), U32(item, 0x10), display))
                continue;
            const char* texture = *reinterpret_cast<const char* const*>(display + 0xC);
            const std::string path = std::string("Item\\ObjectComponents\\Weapon\\") + (texture ? texture : "") + ".blp";
            const uint32_t slot = reinterpret_cast<uint32_t(__cdecl*)(uint32_t, int)>(0x4E7940)(U32(item, 0x198), i == 0);
            reinterpret_cast<void(__cdecl*)(void*, uint32_t, const char*, const char*, int, void*)>(0x4EAA70)(
                model, slot, *reinterpret_cast<const char* const*>(display + 4), path.c_str(), 0, display);
        }
        reinterpret_cast<void(__cdecl*)(void*, int)>(0x4E79A0)(model, 1);
        reinterpret_cast<void(__cdecl*)(void*, int)>(0x4E79A0)(model, 2);
        return 0;
    }

    // FUN_10a6d2c0: parse the GUID string (0x74D120) and hand it to the model (0x5977E0). Returns 1
    // without pushing, as the original does.
    int SetUnitGUID(lua_State* L)
    {
        void* model = Self(L);
        if (!AscLua::lua_isstring(L, 2))
            AscLua::luaL_error(L, "Usage: SetUnitGUID(\"GUID\")");
        const std::string s = CheckString(L, 2);
        const uint64_t guid = reinterpret_cast<uint64_t(__cdecl*)(const char*)>(0x74D120)(s.c_str());
        if (guid == 0)
            AscLua::luaL_error(L, "Usage: SetUnitGUID(\"GUID\")");
        reinterpret_cast<void(__thiscall*)(void*, uint64_t)>(0x5977E0)(model, guid);
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {"#Model", "SetSpell", SetSpell},
        {"#Model", "SetSpellVisual", SetSpellVisual},
        {"#Model", "GetSpell", GetSpell},
        {"#Model", "SetUnitGUID", SetUnitGUID},
        {"#Model", "SetSheathe", SetSheathe},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}

namespace AscModelKit
{
void SetTransform(void* model, const Transform& x)
{
    Mat t = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    reinterpret_cast<void(__thiscall*)(Mat*)>(0x407F40)(&t);
    Translate(t, x.pos);
    Rotate(t, 0, x.rot[0]);
    Rotate(t, 1, x.rot[1]);
    Rotate(t, 2, x.rot[2]);
    Scale(t, x.scale);
    reinterpret_cast<void(__thiscall*)(void*, Mat*)>(0x4D8630)(model, &t);
}

bool PlayStateKit(void* model, uint32_t spell, bool noAnim)
{
    uint8_t rec[0x2A8];
    if (!FetchSpell(spell, rec))
        return false;
    const uint8_t* v = SpellVisual(U32(rec, 0x20C));
    return v && ::PlayKit(model, U32(v, 0x10), noAnim, true);
}

bool PlayPrecastKit(void* model, uint32_t spell, bool noAnim)
{
    uint8_t rec[0x2A8];
    if (!FetchSpell(spell, rec))
        return false;
    const uint8_t* v = SpellVisual(U32(rec, 0x20C));
    return v && ::PlayKit(model, U32(v, 4), noAnim, false);
}

void PlayAnim(void* model, uint32_t anim) { ::PlayAnim(model, anim, 1.0f); }
void Detach(void* model, uint32_t attach) { ::Detach(model, attach); }
}
