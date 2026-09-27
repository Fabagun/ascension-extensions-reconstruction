// Spell charges (map 0x10BE43E4: SpellChargesCategory id -> {+0xC used, +0x10 next recharge (ms),
// +0x14 recharge start (ms), +0x18 recharge duration}), transcribed from ghidra-ext/out/all/10320*.c.
// Times are the client's 0x86AE20 millisecond clock.
//
//   Module init FUN_10320060: SMSG 0x9C2 clear all, 0x9C3 {u32 category} remove, 0x9C4 the whole list,
//   0x9C5 one update; registers SPELL_UPDATE_CHARGES; on world entry the map is cleared (0x1031FD70);
//   after 0x403340 the tick FUN_10320680 recharges.
//   Every handler fires SPELL_UPDATE_CHARGES while in world (0xBD0792); 0x9C5 only when it applied.
//
//   GetSpellCharges(spell): SpellCharges.dbc row (+4 category) -> SpellChargesCategory.dbc row
//   (+4 max charges, +8 recharge ms). Pushes charges, max, start, duration (seconds); 0,0,0,0 when
//   the spell has no charges.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <unordered_map>

using namespace AscScript;

namespace
{
    struct Charges { uint32_t used = 0, next = 0, start = 0, duration = 0; };
    std::unordered_map<uint32_t, Charges> g_charges;

    uint32_t Now() { return reinterpret_cast<uint32_t(__cdecl*)()>(0x86AE20)(); }
    bool InWorld() { return *reinterpret_cast<const uint8_t*>(0xBD0792) != 0; }
    void Updated()
    {
        if (InWorld())
            AscRuntime::Signal("SPELL_UPDATE_CHARGES");
    }

    const uint8_t* CategoryRow(uint32_t id) { return AscDbc::Get("DBFilesClient\\SpellChargesCategory.dbc").Row(id); }
    uint32_t U32(const uint8_t* r, uint32_t off) { return AscDbc::Table::U32(r, off); }

    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    void __cdecl OnClear(void*, uint32_t, uint32_t, CDataStore*)   // handler_0x09c2
    {
        g_charges.clear();
        Updated();
    }

    void __cdecl OnRemove(void*, uint32_t, uint32_t, CDataStore* p)   // FUN_10320160
    {
        g_charges.erase(Read<uint32_t>(p));
        Updated();
    }

    // FUN_10320230: u32 n x {u32 category, i32 remaining ms, u8 used}; an entry is kept only for a known
    // category with a max-charges column and a non-zero remaining time.
    void __cdecl OnList(void*, uint32_t, uint32_t, CDataStore* p)
    {
        for (uint32_t n = Read<uint32_t>(p); n; --n)
        {
            const uint32_t category = Read<uint32_t>(p);
            const int32_t remaining = Read<int32_t>(p);
            const uint8_t used = Read<uint8_t>(p);
            const uint8_t* row = CategoryRow(category);
            if (!row || remaining == 0 || U32(row, 4) == 0)
                continue;
            const uint32_t next = Now() + remaining;
            Charges& c = g_charges[category];
            c.used = used;
            c.next = next;
            c.start = Now();
            c.duration = U32(row, 8);
        }
        Updated();
    }

    // FUN_10320350: {u32 category, u32 ms, u32 used}. used 0 removes; a new category starts now.
    void __cdecl OnUpdate(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t category = Read<uint32_t>(p);
        const uint32_t ms = Read<uint32_t>(p);
        const uint32_t used = Read<uint32_t>(p);
        const uint8_t* row = CategoryRow(category);
        if (!row || ms == 0 || U32(row, 4) == 0)
            return;
        if (used == 0)
            g_charges.erase(category);
        else
        {
            auto it = g_charges.find(category);
            if (it == g_charges.end())
            {
                Charges c;                                   // FUN_10320030
                c.used = used;
                c.next = Now() + ms;
                c.start = Now();
                c.duration = U32(row, 8);
                g_charges[category] = c;
            }
            else
            {
                it->second.used = used;
                it->second.next = Now() + ms;
            }
        }
        Updated();
    }

    // FUN_10320680: a charge returns each time its recharge ends; the last one removes the entry.
    void Tick()
    {
        const uint32_t now = Now();
        for (auto it = g_charges.begin(); it != g_charges.end();)
        {
            if (now < it->second.next)
            {
                ++it;
                continue;
            }
            if (--it->second.used == 0)
                it = g_charges.erase(it);
            else
            {
                it->second.start = Now();
                it->second.next = it->second.duration + it->second.start;
                ++it;
            }
            Updated();
        }
    }

    void OnEnterWorld() { g_charges.clear(); }   // 0x1031FD70

    int GetSpellCharges(lua_State* L)
    {
        if (AscLua::lua_isnumber(L, 1))
        {
            const uint32_t spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            if (const uint8_t* charges = AscDbc::Get("DBFilesClient\\SpellCharges.dbc").Row(spell))
            {
                const uint32_t category = U32(charges, 4);
                if (const uint8_t* row = CategoryRow(category))
                {
                    const int32_t max = static_cast<int32_t>(U32(row, 4));
                    auto it = g_charges.find(category);
                    int32_t duration;
                    if (it == g_charges.end())
                    {
                        PushInt(L, max);
                        PushInt(L, max);
                        PushNum(L, 0.0);
                        duration = static_cast<int32_t>(U32(row, 8));
                    }
                    else
                    {
                        PushInt(L, max - static_cast<int32_t>(it->second.used));
                        PushInt(L, max);
                        PushNum(L, static_cast<double>(it->second.start) * 0.001);
                        duration = static_cast<int32_t>(it->second.next - it->second.start);
                    }
                    PushNum(L, static_cast<double>(duration) * 0.001);
                    return 4;
                }
            }
        }
        for (int i = 0; i < 4; ++i)
            PushInt(L, 0);
        return 4;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x9C2, CNetClientCustomPacket((void*)&OnClear, nullptr));
        sDC.AddPacketHandler(0x9C3, CNetClientCustomPacket((void*)&OnRemove, nullptr));
        sDC.AddPacketHandler(0x9C4, CNetClientCustomPacket((void*)&OnList, nullptr));
        sDC.AddPacketHandler(0x9C5, CNetClientCustomPacket((void*)&OnUpdate, nullptr));
        AscRuntime::OnEnterWorld(OnEnterWorld);
        AscRuntime::OnAfter403340(Tick);
    }

    const AscBindings::Binding kBindings[] = {   // registrar 0x10320B00
        {nullptr, "GetSpellCharges", GetSpellCharges},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
