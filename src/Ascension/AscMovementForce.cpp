// Server movement forces (module init FUN_102b9830): pushes and pulls the server applies to a unit's
// movement, integrated on the client, plus the smoothing of other units' position updates while a
// force acts on them.
//
//   SMSG 0x761 (FUN_102b8fe0)  apply to a unit we control: packed guid, u32 counter, force; ack 0x763
//   SMSG 0x762 (FUN_102b9300)  remove from a unit we control: packed guid, u32 counter, u32 id; ack 0x764
//   SMSG 0x765 (FUN_102b9140)  apply to another unit: packed guid, movement info, force
//   SMSG 0x766 (FUN_102b9530)  remove from another unit: packed guid, movement info, u32 id
//   force (FUN_102ba040): u32 id, float x, y, z, u32 filter, float magnitude, u32 type (1 = a point
//   the unit is pulled towards, anything else a direction)
//
// Hooks (hook manager FUN_100010f0):
//   0x6F09F0  FUN_102b8be0  remembers the time of the last player move while a force pushes it
//   0x987D00  FUN_102b8cb0  adds the forces' displacement to the move step
//   0x6EB730  FUN_102b8da0  another unit's position update: kept and interpolated (FUN_102ba180)
//   0x762E00  FUN_102b8c30  runs with the unit flagged as moving (flags |= 1) while it stands still
//   0x403B70  FUN_102b9b90  resets everything, then the original
//   0x6F1490  FUN_102b9c00  per-frame step: the player's forces, then every other forced unit
// The reset FUN_102b8f70 also runs on leaving the world, before 0x528F00 and after 0x402910.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace
{
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    struct Force        // map node +0x0C..+0x28
    {
        uint32_t id;
        float x, y, z;
        uint32_t filter;      // 0, or the movement's +8 it applies to
        uint32_t type;        // 1 = point
        float magnitude;      // yards per second
        uint32_t counter;     // 0x761's counter; 0 from 0x765
    };

    struct Smooth       // map node +0x10..+0x57
    {
        float pos[3], facing[3];         // +0x10 / +0x1C: where the update put the unit
        float dpos[3], dfacing[3];       // +0x28 / +0x34: the jump it made
        int32_t start;                   // +0x40
        uint32_t interval;               // +0x44: ms to spread the jump over
        int32_t lastLocal;               // +0x48
        int32_t lastServer;              // +0x4C
        float progress;                  // +0x50
        bool active;                     // +0x54
    };

    std::unordered_map<uint64_t, std::unordered_map<uint32_t, Force>> g_forces;   // 0x10be33c4
    std::unordered_map<uint64_t, Smooth> g_smooth;                                // 0x10be33f0
    std::unordered_set<uint64_t> g_stopped;                                       // 0x10be3410
    bool g_stepIdle = false;       // 0x10be33e4
    uint64_t g_stepGuid = 0;       // 0x10be33e8
    int32_t g_lastPlayerMove = 0;  // 0x10be3430
    int32_t g_lastHeartbeat = 0;   // 0x10be3434
    int32_t g_lastFrame = 0;       // 0x10be3438

    const uint32_t kMoving = 0xC0100F, kMovingOrFalling = 0xC0180F;

    uint64_t PlayerGuid() { return *reinterpret_cast<const uint64_t*>(0xCA1238); }
    int32_t Now() { return reinterpret_cast<int32_t(__cdecl*)()>(0x86AE20)(); }
    uint8_t* MovementOf(uint64_t guid)   // FUN_102b8fc0: object (units and players) +0xD8
    {
        const uint8_t* o = reinterpret_cast<uint8_t*(__cdecl*)(uint32_t, uint32_t, uint32_t)>(0x4D4DB0)(
            static_cast<uint32_t>(guid), static_cast<uint32_t>(guid >> 32), 0x18);
        return o ? *reinterpret_cast<uint8_t* const*>(o + 0xD8) : nullptr;
    }
    uint32_t& Flags(uint8_t* m) { return *reinterpret_cast<uint32_t*>(m + 0x44); }
    float* Pos(uint8_t* m) { return reinterpret_cast<float*>(m + 0x10); }
    float* Facing(uint8_t* m) { return reinterpret_cast<float*>(m + 0x4C); }
    uint8_t* Controller(uint8_t* m) { return *reinterpret_cast<uint8_t**>(m + 0x144); }
    uint64_t ControllerGuid(uint8_t* c) { return **reinterpret_cast<uint64_t**>(c + 8); }

    // Client calls (the original's pointer table 0x10bcbb84..0x10bcbbdc).
    void ClearStep(uint8_t* m) { reinterpret_cast<void(__thiscall*)(uint8_t*, int)>(0x9881D0)(m, 0); }
    void Advance(uint8_t* m, int32_t now, uint32_t dt) { reinterpret_cast<void(__thiscall*)(uint8_t*, int32_t, uint32_t)>(0x6EAC40)(m, now, dt); }
    void StartMove(uint8_t* c, int32_t now) { reinterpret_cast<void(__thiscall*)(uint8_t*, int32_t, int, int)>(0x73AB20)(c, now, 1, 0); }
    void StopMove(uint8_t* c) { reinterpret_cast<void(__thiscall*)(uint8_t*, int, int)>(0x73AC30)(c, 0, -1); }

    // FUN_102ba0f0: whose control the movement answers to -- the step in progress, the unit's
    // controller when it is of kind 3 or 4, or the player when it is the player's own movement or
    // the player controls it.
    uint64_t ControlGuid(uint8_t* m)
    {
        if (g_stepGuid)
            return g_stepGuid;
        if (!m)
            return 0;
        uint8_t* c = Controller(m);
        if (c)
        {
            const uint32_t kind = *reinterpret_cast<uint32_t*>(c + 0x14);
            if ((kind == 3 || kind == 4) && ControllerGuid(c))
                return ControllerGuid(c);
        }
        const uint64_t player = PlayerGuid();
        if (player)
        {
            if (MovementOf(player) == m)
                return player;
            if (Controller(m) && reinterpret_cast<bool(__thiscall*)(uint8_t*)>(0x4D43C0)(Controller(m)))
                return player;
        }
        return 0;
    }

    bool Forced(uint8_t* m)   // FUN_102b97c0
    {
        if (g_forces.empty())
            return false;
        const uint64_t guid = ControlGuid(m);
        if (!guid)
            return false;
        auto it = g_forces.find(guid);
        return it != g_forces.end() && !it->second.empty();
    }

    // FUN_102b8800: the forces' displacement over dtMs, added to out.
    void Displacement(uint8_t* m, int32_t dtMs, float* out)
    {
        if (!m || !out || dtMs <= 0)
            return;
        const uint64_t guid = ControlGuid(m);
        if (!guid)
            return;
        auto it = g_forces.find(guid);
        const float seconds = static_cast<float>(dtMs) * 0.001f;
        if (it == g_forces.end() || !(seconds > 0.0f))
            return;
        float sx = 0.0f, sy = 0.0f, sz = 0.0f;
        for (const auto& kv : it->second)
        {
            const Force& f = kv.second;
            if (!(f.magnitude > 0.0f) || (f.filter != 0 && f.filter != *reinterpret_cast<uint32_t*>(m + 8)))
                continue;
            float dx = f.x, dy = f.y, dz = f.z;
            if (f.type == 1)
            {
                dx -= Pos(m)[0];
                dy -= Pos(m)[1];
                dz -= Pos(m)[2];
            }
            const float len2 = dy * dy + dx * dx + dz * dz;
            if (len2 < 1e-06f)
                continue;
            const float len = static_cast<float>(sqrt(static_cast<double>(len2)));
            float step = f.magnitude * seconds;
            if (f.type == 1 && step > len)
                step = len;   // a point is reached, never overshot
            const float k = step / len;
            sz = k * dz + sz;
            sx = k * dx + sx;
            sy = k * dy + sy;
        }
        if (sx != 0.0f || sy != 0.0f || sz != 0.0f)
        {
            out[0] = sx + out[0];
            out[1] = sy + out[1];
            out[2] = sz + out[2];
        }
    }

    // FUN_102ba610: a unit the player controls (not the player) was stepped; track whether it stood
    // still, telling the controller to stop when that changes.
    void TrackStopped(uint8_t* m, uint64_t guid, bool idle)
    {
        if (!m || guid == PlayerGuid())
            return;
        uint8_t* c = Controller(m);
        if (!c || ControllerGuid(c) != guid)
            return;
        const bool changed = idle ? g_stopped.insert(guid).second : g_stopped.erase(guid) != 0;
        if (changed)
            StopMove(c);
    }

    // FUN_102ba4a0: advance the movement by dt with the forces applied (0x987D00's hook sees
    // g_stepIdle and replaces the step by the displacement).
    void Step(uint8_t* m, uint64_t guid, int32_t now, uint32_t dt)
    {
        if (!m || !dt)
            return;
        const bool isPlayer = guid && guid == PlayerGuid();
        ClearStep(m);
        const uint32_t saved = Flags(m);
        const bool idle = (saved & kMoving) == 0;
        g_stepIdle = idle;
        g_stepGuid = guid;
        if (idle)
        {
            Flags(m) = saved | 1;
            Advance(m, now, dt);
            if (Controller(m) && !isPlayer)
                StartMove(Controller(m), now);
        }
        else
            Advance(m, now, dt);
        g_stepGuid = 0;
        g_stepIdle = false;
        Flags(m) = saved;
        TrackStopped(m, guid, idle);
    }

    // FUN_102ba590: the player's own movement, when the player's controller is the player.
    void TrackPlayerStopped(uint8_t* m, uint64_t guid, bool idle)
    {
        if (!m || guid != PlayerGuid())
            return;
        uint8_t* c = Controller(m);
        if (!c || ControllerGuid(c) != guid)
            return;
        const bool changed = idle ? g_stopped.insert(guid).second : g_stopped.erase(guid) != 0;
        if (changed)
            StopMove(c);
    }

    // FUN_102ba180: a position update just moved the unit; spread the jump over the time since the
    // last one (server time when it is newer, else local; 50..1000 ms). False when the jump is too
    // small or too far to smooth.
    bool BeginSmooth(uint64_t guid, uint8_t* m, const float* oldPos, const float* oldFacing, int32_t serverTime)
    {
        if (!m || !guid)
            return false;
        const int32_t now = Now();
        if (now < 1)
            return false;
        Smooth& s = g_smooth[guid];   // FUN_102b8000
        uint32_t interval = 0x80;
        bool clamp = true;
        if (s.lastServer >= 1 && serverTime > s.lastServer)
            interval = static_cast<uint32_t>(serverTime - s.lastServer);
        else if (s.lastLocal >= 1 && now > s.lastLocal)
            interval = static_cast<uint32_t>(now - s.lastLocal);
        else
            clamp = false;
        if (clamp)
            interval = interval < 0x32 ? 0x32 : interval > 1000 ? 1000 : interval;
        s.start = now;
        s.interval = interval;
        s.lastLocal = now;
        s.progress = 0.0f;
        if (serverTime > 0)
            s.lastServer = serverTime;
        memcpy(s.pos, Pos(m), 12);
        memcpy(s.facing, Facing(m), 12);
        for (int i = 0; i < 3; ++i)
        {
            s.dpos[i] = Pos(m)[i] - oldPos[i];
            s.dfacing[i] = Facing(m)[i] - oldFacing[i];
        }
        const float dist2 = s.dpos[1] * s.dpos[1] + s.dpos[0] * s.dpos[0] + s.dpos[2] * s.dpos[2];
        s.active = true;
        const float reach = static_cast<float>(static_cast<double>(interval)) * 0.001f * 60.0f;
        if (0.0025f < dist2 && dist2 <= reach * reach + 9.536743e-07f)
            return true;
        s.active = false;
        return false;
    }

    // FUN_102b8a40: move the unit along the kept jump to where it should be by now.
    void ApplySmooth(uint8_t* m, Smooth& s, int32_t now)
    {
        if (!m || !s.active || static_cast<int32_t>(s.interval) <= 0)
            return;
        const int32_t elapsed = now - s.start < 0 ? 0 : now - s.start;
        auto add = [&](float k) {
            for (int i = 0; i < 3; ++i)
            {
                Pos(m)[i] = k * s.dpos[i] + Pos(m)[i];
                Facing(m)[i] = k * s.dfacing[i] + Facing(m)[i];
            }
        };
        if (static_cast<int32_t>(s.interval) <= elapsed)
        {
            const float rest = 1.0f - s.progress;
            if (0.0f < rest)
                add(rest);
            s.active = false;
            s.progress = 0.0f;
            memcpy(Pos(m), s.pos, 12);
            memcpy(Facing(m), s.facing, 12);
            return;
        }
        float t = static_cast<float>(elapsed) / static_cast<float>(static_cast<int32_t>(s.interval));
        t = t < 0.0f ? 0.0f : t;
        t = t > 1.0f ? 1.0f : t;
        float step = t - s.progress;
        step = step < 0.0f ? 0.0f : step;
        step = step > 1.0f ? 1.0f : step;
        if (0.0f < step)
            add(step);
        s.progress = t;
    }

    void Reset()   // FUN_102b8f70
    {
        g_forces.clear();
        g_smooth.clear();
        g_stopped.clear();
        g_stepIdle = false;
        g_stepGuid = 0;
        g_lastHeartbeat = 0;
        g_lastFrame = 0;
        g_lastPlayerMove = 0;
    }

    // ---- packets ------------------------------------------------------------------------------------
    uint64_t ReadGuid(CDataStore* p)   // 0x76DC20
    {
        uint64_t guid = 0;
        reinterpret_cast<void(__cdecl*)(CDataStore*, uint64_t*)>(0x76DC20)(p, &guid);
        return guid;
    }

    Force ReadForce(CDataStore* p)   // FUN_102ba040
    {
        Force f{};
        f.id = Read<uint32_t>(p);
        f.x = Read<float>(p);
        f.y = Read<float>(p);
        f.z = Read<float>(p);
        f.filter = Read<uint32_t>(p);
        f.magnitude = Read<float>(p);
        f.type = Read<uint32_t>(p) == 1 ? 1 : 0;
        return f;
    }

    // FUN_102ba320: {packed guid, u32 counter} and the unit's movement block (0x7164B0), stamped with
    // the movement clock (0x6E9230, else the tick count).
    void SendAck(uint32_t opcode, uint64_t guid, uint32_t counter)
    {
        uint8_t* m = MovementOf(guid);
        if (!m)
            return;
        AscScript::Packet pkt(opcode);
        pkt.PackedGuid(guid).U32(counter);
        int32_t time = reinterpret_cast<int32_t(__cdecl*)()>(0x6E9230)();
        if (time == 0)
            time = Now();
        reinterpret_cast<void(__thiscall*)(uint8_t*, uint32_t, int32_t, CDataStore*)>(0x7164B0)(m, opcode, time, pkt.Store());
        pkt.Send();
    }

    // The movement info 0x765 / 0x766 carry (0x4F4C50 init, 0x4F4D40 read), applied through the
    // original 0x6EB730 (not the hook) when the unit exists.
    typedef int(__fastcall* Update_t)(uint8_t* m, void* edx, int32_t now, void* info, void* out, int a, int b);
    Update_t g_updateOriginal = nullptr;

    uint8_t* ReadAndApplyMovement(CDataStore* p, uint64_t guid)
    {
        uint8_t* m = (guid && guid == PlayerGuid()) ? nullptr : MovementOf(guid);
        uint8_t info[0x60];
        memset(info, 0, sizeof(info));
        reinterpret_cast<void(__thiscall*)(uint8_t*)>(0x4F4C50)(info);
        reinterpret_cast<void(__cdecl*)(CDataStore*, uint8_t*)>(0x4F4D40)(p, info);
        if (m)
        {
            uint32_t out = 0;
            g_updateOriginal(m, nullptr, Now(), info, &out, 0, 0);
        }
        return m;
    }

    void __cdecl OnApply(void*, uint32_t, uint32_t, CDataStore* p)   // 0x761
    {
        const uint64_t guid = ReadGuid(p);
        const uint32_t counter = Read<uint32_t>(p);
        Force f = ReadForce(p);
        f.counter = counter;
        g_forces[guid][f.id] = f;
        if (guid == PlayerGuid())
            if (uint8_t* m = MovementOf(guid))
                ClearStep(m);
        SendAck(0x763, guid, counter);
    }

    void __cdecl OnApplyOther(void*, uint32_t, uint32_t, CDataStore* p)   // 0x765
    {
        const uint64_t guid = ReadGuid(p);
        uint8_t* m = ReadAndApplyMovement(p, guid);
        Force f = ReadForce(p);
        f.counter = 0;
        g_forces[guid][f.id] = f;
        if (m)
            ClearStep(m);
    }

    // 0x762: the stop the controller would get is gated on a second erase of the same guid from
    // g_stopped right after the first, so it never fires (IMPROVEMENTS.md).
    void __cdecl OnRemove(void*, uint32_t, uint32_t, CDataStore* p)   // 0x762
    {
        const uint64_t guid = ReadGuid(p);
        const uint32_t counter = Read<uint32_t>(p);
        const uint32_t id = Read<uint32_t>(p);
        auto it = g_forces.find(guid);
        if (it == g_forces.end())
        {
            SendAck(0x764, guid, counter);
            return;
        }
        it->second.erase(id);
        if (it->second.empty())
        {
            const bool isPlayer = guid == PlayerGuid();
            g_forces.erase(it);
            g_smooth.erase(guid);
            if (isPlayer)
                g_stopped.erase(guid);
            uint8_t* m = MovementOf(guid);
            if (m && (isPlayer || g_stopped.erase(guid)))
            {
                uint8_t* c = Controller(m);
                if (c && ControllerGuid(c) == guid && g_stopped.erase(guid))
                    StopMove(c);
            }
        }
        if (guid == PlayerGuid())
            if (uint8_t* m = MovementOf(guid))
                ClearStep(m);
        SendAck(0x764, guid, counter);
    }

    void __cdecl OnRemoveOther(void*, uint32_t, uint32_t, CDataStore* p)   // 0x766
    {
        const uint64_t guid = ReadGuid(p);
        uint8_t* m = ReadAndApplyMovement(p, guid);
        const uint32_t id = Read<uint32_t>(p);
        auto it = g_forces.find(guid);
        if (it == g_forces.end())
            return;
        it->second.erase(id);
        if (!it->second.empty())
            return;
        const bool isPlayer = guid == PlayerGuid();
        g_forces.erase(it);
        g_smooth.erase(guid);
        if (isPlayer)
            g_stopped.erase(guid);
        if (!m)
        {
            if (guid)
                m = MovementOf(guid);
            if (!m)
                return;
        }
        if (!isPlayer && !g_stopped.erase(guid))
            return;
        uint8_t* c = Controller(m);
        if (!c || ControllerGuid(c) != guid)
            return;
        if (g_stopped.erase(guid))   // never true: erased just above (IMPROVEMENTS.md)
            StopMove(c);
    }

    // ---- hooks --------------------------------------------------------------------------------------
    typedef int(__fastcall* Fn2_t)(uint8_t*, void*, uint32_t, uint32_t);
    typedef int(__fastcall* Fn5_t)(uint8_t*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    Fn2_t g_6F09F0 = nullptr, g_987D00 = nullptr;
    Fn5_t g_762E00 = nullptr;
    typedef int(__cdecl* Fn403B70_t)(uint32_t, uint32_t);
    Fn403B70_t g_403B70 = nullptr;

    int __fastcall Hook6F09F0(uint8_t* m, void* edx, uint32_t time, uint32_t b)   // FUN_102b8be0
    {
        const int r = g_6F09F0(m, edx, time, b);
        if (m)
        {
            const uint64_t guid = ControlGuid(m);
            if (guid && guid == PlayerGuid() && (Flags(m) & kMoving))
                g_lastPlayerMove = static_cast<int32_t>(time);
        }
        return r;
    }

    int __fastcall Hook987D00(uint8_t* m, void* edx, uint32_t dt, uint32_t outPtr)   // FUN_102b8cb0
    {
        const int r = g_987D00(m, edx, dt, outPtr);
        float* out = reinterpret_cast<float*>(outPtr);
        if (!m || !Forced(m) || !out || static_cast<int32_t>(dt) <= 0 || (Flags(m) & 0x800))
            return r;
        float d[3] = {0.0f, 0.0f, 0.0f};
        Displacement(m, static_cast<int32_t>(dt), d);
        if (d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f)
        {
            if (!g_stepIdle)
            {
                d[0] += out[0];
                d[1] += out[1];
                d[2] += out[2];
            }
            out[2] = d[2];
            out[1] = d[1];
            out[0] = d[0];
        }
        return r ? r : 1;
    }

    int __fastcall Hook6EB730(uint8_t* m, void* edx, int32_t now, void* info, void* out, int a, int b)   // FUN_102b8da0
    {
        bool keep = false;
        uint64_t guid = 0;
        float oldPos[3] = {}, oldFacing[3] = {};
        if (m && info && Forced(m))
        {
            guid = ControlGuid(m);
            if (guid && guid != PlayerGuid())
            {
                if ((*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(info) + 0x10) & kMovingOrFalling) == 0)
                {
                    memcpy(oldPos, Pos(m), 12);
                    memcpy(oldFacing, Facing(m), 12);
                    keep = true;
                }
                else
                {
                    auto it = g_smooth.find(guid);   // FUN_102bb070
                    if (it != g_smooth.end())
                        it->second.active = false;
                }
            }
        }
        const int r = g_updateOriginal(m, edx, now, info, out, a, b);
        if (keep && m && guid && BeginSmooth(guid, m, oldPos, oldFacing, *static_cast<int32_t*>(info)))
        {
            memcpy(Pos(m), oldPos, 12);
            memcpy(Facing(m), oldFacing, 12);
        }
        return r;
    }

    int __fastcall Hook762E00(uint8_t* m, void* edx, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e)   // FUN_102b8c30
    {
        bool flagged = false;
        uint32_t saved = 0;
        if (m && Forced(m) && (Flags(m) & kMovingOrFalling) == 0)
        {
            flagged = true;
            saved = Flags(m);
            Flags(m) = saved | 1;
        }
        const int r = g_762E00(m, edx, a, b, c, d, e);
        if (flagged)
            Flags(m) = saved;
        return r;
    }

    int __cdecl Hook403B70(uint32_t a, uint32_t b)   // FUN_102b9b90
    {
        Reset();
        return g_403B70(a, b);
    }

    // 0x6F1490 begins push esi / push edi / call 0x86AE20 -- the call is re-made here, then 0x6F1497.
    __declspec(naked) int __cdecl Original6F1490()
    {
        __asm
        {
            push esi
            push edi
            mov eax, 0x86AE20
            call eax
            push 0x6F1497
            ret
        }
    }

    int __cdecl Hook6F1490()   // FUN_102b9c00
    {
        const int r = Original6F1490();
        const int32_t now = Now();
        if (now <= 0)
            return r;
        uint32_t dt = 0;
        if (g_lastFrame > 0 && now > g_lastFrame)
            dt = static_cast<uint32_t>(now - g_lastFrame);
        g_lastFrame = now;
        if (dt > 100)
            dt = 100;
        else if (dt == 0)
            return r;

        const uint64_t player = PlayerGuid();
        if (player)
            if (uint8_t* m = MovementOf(player))
            {
                if (!Forced(m))
                {
                    uint8_t* c = Controller(m);
                    if (player == PlayerGuid() && c && ControllerGuid(c) == player && g_stopped.erase(player))
                        StopMove(c);
                }
                else
                {
                    const uint32_t flags = Flags(m);
                    TrackPlayerStopped(m, player, (flags & kMovingOrFalling) == 0);
                    if ((flags & kMovingOrFalling) == 0 && g_lastPlayerMove != now)
                    {
                        float d[3] = {0.0f, 0.0f, 0.0f};
                        Displacement(m, static_cast<int32_t>(dt), d);
                        Step(m, player, now, dt);
                        if ((d[0] != 0.0f || d[1] != 0.0f || d[2] != 0.0f) && now - g_lastHeartbeat >= 0x80 && Controller(m))
                        {
                            // MSG_MOVE_HEARTBEAT (0xEE) through the controller.
                            reinterpret_cast<void(__thiscall*)(uint8_t*, int32_t, uint32_t, int, int, int, int, int)>(0x71F0C0)(
                                Controller(m), now, 0xEE, 0, 0, 0, 0, 0xFF);
                            g_lastHeartbeat = now;
                        }
                    }
                }
            }

        if (g_forces.empty())
            return r;
        for (auto it = g_forces.begin(); it != g_forces.end();)
        {
            const uint64_t guid = it->first;
            if (!guid || it->second.empty())
            {
                g_smooth.erase(guid);
                g_stopped.erase(guid);
                it = g_forces.erase(it);   // FUN_102b8610
                continue;
            }
            if (guid == player)
            {
                ++it;
                continue;
            }
            uint8_t* m = MovementOf(guid);
            if (!m)
            {
                g_smooth.erase(guid);
                g_stopped.erase(guid);
                it = g_forces.erase(it);
                continue;
            }
            if (Flags(m) & 0x800)
            {
                ++it;
                continue;
            }
            const uint32_t moving = Flags(m) & kMoving;
            uint8_t* c = Controller(m);
            if (guid == PlayerGuid() || !c || ControllerGuid(c) != guid)
            {
                if (!moving)
                    Step(m, guid, now, dt);
            }
            else if (!moving)
            {
                if (g_stopped.insert(guid).second)
                    StopMove(c);
                Step(m, guid, now, dt);
            }
            else if (g_stopped.erase(guid))
                StopMove(c);
            auto s = g_smooth.find(guid);
            if (s != g_smooth.end())
                ApplySmooth(m, s->second, now);
            ++it;
        }
        return r;
    }

    void Init()   // FUN_102b9830
    {
        AscRuntime::OnLeaveWorld(Reset);
        AscRuntime::OnBefore528F00(Reset);
        AscRuntime::OnAfter402910(Reset);
        sDC.AddPacketHandler(0x761, CNetClientCustomPacket((void*)&OnApply, nullptr));
        sDC.AddPacketHandler(0x762, CNetClientCustomPacket((void*)&OnRemove, nullptr));
        sDC.AddPacketHandler(0x765, CNetClientCustomPacket((void*)&OnApplyOther, nullptr));
        sDC.AddPacketHandler(0x766, CNetClientCustomPacket((void*)&OnRemoveOther, nullptr));
        g_6F09F0 = reinterpret_cast<Fn2_t>(AscRuntime::Detour(0x6F09F0, 6, reinterpret_cast<void*>(&Hook6F09F0)));
        g_987D00 = reinterpret_cast<Fn2_t>(AscRuntime::Detour(0x987D00, 6, reinterpret_cast<void*>(&Hook987D00)));
        g_updateOriginal = reinterpret_cast<Update_t>(AscRuntime::Detour(0x6EB730, 5, reinterpret_cast<void*>(&Hook6EB730)));
        g_762E00 = reinterpret_cast<Fn5_t>(AscRuntime::Detour(0x762E00, 6, reinterpret_cast<void*>(&Hook762E00)));
        g_403B70 = reinterpret_cast<Fn403B70_t>(AscRuntime::Detour(0x403B70, 6, reinterpret_cast<void*>(&Hook403B70)));
        AscRuntime::ReplaceFunction(0x6F1490, reinterpret_cast<void*>(&Hook6F1490));
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}
