// Zone lighting + the second moon -- five hooks from the attach init (installer FUN_10a66d40).
//
//   0x7F2790 -> FUN_10a4b030   sky init: original, then the second moon is (re)initialised and the zone
//                              table is rebuilt from ZoneLight.dbc + ZoneLightPoint.dbc
//   0x77EED0 -> FUN_10a4b880   per-position light update: original, then every zone the position is near
//                              blends its light in (0x7ED150)
//   0x7EECC0 -> sub_10a4baa0   original, then the second moon copies the first moon's position / fade
//   0x7F3230 -> sub_10a4ba70   original, then both moons take the value at 0xD38E48 +0xC
//   0x7F0870 -> sub_10a4ba30   original, then (while [0xD38CCC]) the second moon is drawn
//   SMSG 0x680 (FUN_1021a4d0)  one ZoneLight row: u32 id, C-string name, u32 map, light, transition, zMin,
//                              zMax -- overwritten in place or inserted; used from the next sky init on
//
// The second moon (DAT_10D3D714) is a zeroed 0xB0-byte block (static init FUN_10086b40) that becomes a
// client sky object of class 0xA41C2C (the moon at 0xD38E68 is one): 0x9AD000 loads its texture, then
// the vtable and fields are set.
//
// Zones (DAT_10D3D9DC, an unordered_map<u32, Zone> keyed by ZoneLight id): { id, map, light, transition,
// zMin, zMax, polygon, bounding box }. Polygon points and query positions live in the flipped plane
// (17066.666 - y, 17066.666 - x); the box is grown by the transition width.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    float Bits(uint32_t b)
    {
        float f;
        memcpy(&f, &b, 4);
        return f;
    }
    float F32(const uint8_t* row, uint32_t off)
    {
        float f;
        memcpy(&f, row + off, 4);
        return f;
    }
    uint32_t U32(const uint8_t* row, uint32_t off) { return AscDbc::Table::U32(row, off); }

    const float kHalfWorld = Bits(0x46855555);   // 17066.666, DAT_10B62D04
    uint8_t* const kMoon = reinterpret_cast<uint8_t*>(0xD38E68);        // DAT_10BCD208
    const uint8_t* const kSkyValue = reinterpret_cast<const uint8_t*>(0xD38E48);   // DAT_10BCD204

    alignas(16) uint8_t g_moon2[0xB0] = {};   // DAT_10D3D714 (FUN_10086b40: 0xB0 zeroed bytes)

    struct Zone   // the map value (node +0xC)
    {
        uint32_t id = 0, map = 0, light = 0;
        float transition = Bits(0x42480000);   // 50.0   (FUN_10a3c6f0)
        float zMin = Bits(0xC77A0000);         // -64000
        float zMax = Bits(0x477A0000);         // 64000
        std::vector<std::pair<float, float>> points;
        float minA = Bits(0x46855555), minB = Bits(0x46855555), maxA = 0.0f, maxB = 0.0f;
    };
    std::unordered_map<uint32_t, Zone> g_zones;   // 0x10D3D9DC (FUN_10086c40: 8 buckets, load factor 1)

    struct Point { int32_t id; float a, b; int32_t order; };

    template <class T> void Set(uint8_t* p, uint32_t off, T v) { memcpy(p + off, &v, sizeof(T)); }

    // ---- 0x7F2790 (__cdecl, 1 argument) -> FUN_10a4b030 ---------------------------------------------------
    typedef int(__cdecl* Fn7F2790_t)(uint32_t);
    Fn7F2790_t g_7F2790 = nullptr;
    int __cdecl Detour7F2790(uint32_t a)
    {
        const int r = g_7F2790(a);
        Set<uint32_t>(kMoon, 0x1C, 0x3F800000);
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(0x9AD000)(g_moon2, "Textures\\moon02Glare.blp");
        Set<uint32_t>(g_moon2, 0x00, 0xA41C2C);
        Set<uint32_t>(g_moon2, 0x20, 0x40000000);
        Set<uint32_t>(g_moon2, 0xA8, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(kMoon)));
        Set<uint32_t>(g_moon2, 0x2C, 0x3FC1F07C);
        Set<uint32_t>(g_moon2, 0x08, 1);
        Set<uint32_t>(g_moon2, 0x38, 1);
        Set<uint32_t>(g_moon2, 0x28, 0x4041F07C);
        Set<uint32_t>(g_moon2, 0x9C, 0x3DCCCCCD);
        Set<uint32_t>(g_moon2, 0xA0, 0x3F800000);
        Set<uint32_t>(g_moon2, 0x90, 0x3F800000);
        Set<uint32_t>(g_moon2, 0x94, 0x3F800000);
        Set<uint32_t>(g_moon2, 0x98, 0x3F333333);
        Set<uint32_t>(g_moon2, 0x70, 0x3DAAAAAB);
        Set<uint32_t>(g_moon2, 0x74, 0x3F800000);
        Set<uint32_t>(g_moon2, 0x78, 0x3E0AAAAB);
        Set<uint32_t>(g_moon2, 0x7C, 0);
        Set<uint32_t>(g_moon2, 0x80, 0x3F72AAAB);
        Set<uint32_t>(g_moon2, 0x84, 0);
        Set<uint32_t>(g_moon2, 0x88, 0x3F7FD27E);
        Set<uint32_t>(g_moon2, 0x8C, 0x3F800000);
        Set<uint32_t>(g_moon2, 0xA4, 0);

        g_zones.clear();   // FUN_10a77e30

        // ZoneLight.dbc {id, name, map, light, transition, zMin, zMax}, in id order.
        AscDbc::Table& zl = AscDbc::Get("DBFilesClient\\ZoneLight.dbc");
        if (zl.Loaded())
            for (uint32_t id = zl.MinId(); id < zl.MaxId() + 1; ++id)
                if (const uint8_t* row = zl.Row(id))
                {
                    Zone& z = g_zones[U32(row, 0)];
                    z.id = U32(row, 0);
                    z.map = U32(row, 8);
                    z.light = U32(row, 0xC);
                    const float t = F32(row, 0x10), lo = F32(row, 0x14), hi = F32(row, 0x18);
                    z.transition = t <= 1.0f ? 1.0f : t;
                    z.zMin = lo <= -64000.0f ? -64000.0f : lo;
                    z.zMax = 64000.0f <= hi ? 64000.0f : hi;
                    if (z.zMax < z.zMin)
                        std::swap(z.zMin, z.zMax);
                }

        // ZoneLightPoint.dbc {id, zone, x, y, order}, grouped by zone.
        std::unordered_map<uint32_t, std::vector<Point>> groups;
        AscDbc::Table& zp = AscDbc::Get("DBFilesClient\\ZoneLightPoint.dbc");
        if (zp.Loaded())
            for (uint32_t id = zp.MinId(); id < zp.MaxId() + 1; ++id)
                if (const uint8_t* row = zp.Row(id))
                    groups.try_emplace(U32(row, 4)).first->second.push_back(
                        {static_cast<int32_t>(U32(row, 0)), kHalfWorld - F32(row, 0xC), kHalfWorld - F32(row, 8),
                         static_cast<int32_t>(U32(row, 0x10))});

        // Each group: sorted by (order, id), one point per order, appended to its zone's polygon.
        for (auto& g : groups)
        {
            std::vector<Point>& v = g.second;
            std::sort(v.begin(), v.end(), [](const Point& x, const Point& y) {   // FUN_10a3c0f0
                return x.order < y.order || (x.order == y.order && x.id < y.id);
            });
            v.erase(std::unique(v.begin(), v.end(), [](const Point& x, const Point& y) { return x.order == y.order; }), v.end());
            Zone& z = g_zones[g.first];
            for (const Point& p : v)
            {
                z.points.emplace_back(p.a, p.b);
                if (p.a < z.minA)
                    z.minA = p.a;
                if (p.b < z.minB)
                    z.minB = p.b;
                if (z.maxA < p.a)
                    z.maxA = p.a;
                if (z.maxB < p.b)
                    z.maxB = p.b;
            }
            z.minA -= z.transition;
            z.minB -= z.transition;
            z.maxA += z.transition;
            z.maxB += z.transition;
        }
        return r;
    }

    // ---- 0x77EED0 (__cdecl(position)) -> FUN_10a4b880 -------------------------------------------------------
    typedef int(__cdecl* Fn77EED0_t)(const float*);
    Fn77EED0_t g_77EED0 = nullptr;
    int __cdecl Detour77EED0(const float* pos)
    {
        const int r = g_77EED0(pos);
        const float qa = -(pos[1] - kHalfWorld);   // xmm6
        const float qb = -(pos[0] - kHalfWorld);   // xmm4
        const uint32_t map = *reinterpret_cast<const uint32_t*>(0xADFBC4);   // *DAT_10BCCA20
        for (auto& e : g_zones)
        {
            const Zone& z = e.second;
            if (z.map != map)
                continue;
            const float t = z.transition, zPos = pos[2];
            if (z.zMin - t > zPos || zPos > z.zMax + t)
                continue;
            float margin = z.zMax - zPos;
            const float below = zPos - z.zMin;
            margin = (margin < below ? margin : below) + t;   // minss
            if (qa < z.minA || qb < z.minB || z.maxA < qa || z.maxB < qb)
                continue;
            const float pt[2] = {qa, qb};
            float dist = 0.0f;
            typedef int(__cdecl* Poly_t)(int, const void*, const float*, float*);
            if (reinterpret_cast<Poly_t>(0x7F9C90)(static_cast<int>(z.points.size()), z.points.data(), pt, &dist))
                dist = -dist;
            if (!(t > dist))
                continue;
            const float inside = -(dist - t);
            const float blend = (margin < inside ? margin : inside) / (t + t) * 100.0f;
            void* lights = reinterpret_cast<void*(__cdecl*)()>(0x7ECEF0)();
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, float)>(0x7ED150)(lights, z.light, blend);
        }
        return r;
    }

    // ---- 0x7EECC0 / 0x7F3230 / 0x7F0870 (no stack arguments) --------------------------------------------------
    typedef int(__cdecl* Fn_t)();
    Fn_t g_7EECC0 = nullptr, g_7F3230 = nullptr, g_7F0870 = nullptr;

    int __cdecl Detour7EECC0()   // sub_10a4baa0
    {
        const int r = g_7EECC0();
        Set<uint32_t>(g_moon2, 0x90, U32(kMoon, 0x14));
        memcpy(g_moon2 + 0xC, kMoon, 8);
        Set<uint32_t>(g_moon2, 0x14, U32(kMoon, 8));
        Set<uint32_t>(g_moon2, 0x94, U32(kMoon, 0x14));
        return r;
    }
    int __cdecl Detour7F3230()   // sub_10a4ba70
    {
        const int r = g_7F3230();
        Set<uint32_t>(kMoon, 0xC, U32(kSkyValue, 0xC));
        Set<uint32_t>(g_moon2, 0x18, U32(kSkyValue, 0xC));
        return r;
    }
    int __cdecl Detour7F0870()   // sub_10a4ba30
    {
        const int r = g_7F0870();
        if (*reinterpret_cast<const uint32_t*>(0xD38CCC) == 0)
            return r;
        reinterpret_cast<void(__thiscall*)(void*, float)>(0x7EF6E0)(g_moon2, *reinterpret_cast<const float*>(0xD38B48));
        return reinterpret_cast<int(__thiscall*)(void*)>(0x9AC400)(g_moon2);
    }

    // ---- SMSG 0x680 (FUN_1021a4d0) ------------------------------------------------------------------------
    void __cdecl OnPatchZoneLight(void*, uint32_t, uint32_t, CDataStore* p)
    {
        auto u32 = [p]() { uint32_t v; memcpy(&v, p->m_buffer + p->m_read, 4); p->m_read += 4; return v; };
        const uint32_t id = u32();
        const char* name = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        const size_t len = strlen(name);
        p->m_read += static_cast<int32_t>(len + 1);
        AscDbc::Table& t = AscDbc::Get("DBFilesClient/ZoneLight.dbc");
        uint32_t f[7] = {id, t.AddString(name), 0, 0, 0, 0, 0};
        for (uint32_t i = 2; i < 7; ++i)
            f[i] = u32();
        if (const uint8_t* live = t.Row(id))
        {
            memcpy(const_cast<uint8_t*>(live), f, sizeof(f));
            return;
        }
        t.Upsert(id, std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(f), reinterpret_cast<const uint8_t*>(f) + sizeof(f)));
    }

    void Init()
    {
        g_7F2790 = reinterpret_cast<Fn7F2790_t>(AscRuntime::Detour(0x7F2790, 6, reinterpret_cast<void*>(&Detour7F2790)));
        g_77EED0 = reinterpret_cast<Fn77EED0_t>(AscRuntime::Detour(0x77EED0, 6, reinterpret_cast<void*>(&Detour77EED0)));
        g_7EECC0 = reinterpret_cast<Fn_t>(AscRuntime::Detour(0x7EECC0, 8, reinterpret_cast<void*>(&Detour7EECC0)));
        g_7F3230 = reinterpret_cast<Fn_t>(AscRuntime::Detour(0x7F3230, 9, reinterpret_cast<void*>(&Detour7F3230)));
        g_7F0870 = reinterpret_cast<Fn_t>(AscRuntime::Detour(0x7F0870, 7, reinterpret_cast<void*>(&Detour7F0870)));
        sDC.AddPacketHandler(0x680, CNetClientCustomPacket((void*)&OnPatchZoneLight, nullptr));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
