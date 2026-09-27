// The client-DBC SMSG_PATCH_* handlers of the original's installer FUN_10228d30 / FUN_10229156 /
// FUN_1022958a (registered through the map-backed registrar 0x102CFEB0). Each patches one of the CLIENT's
// WowClientDB tables: the wire record is read item by item, the row with that id (the getter,
// AscClientDbc::Row) is overwritten in place, or a new row is allocated and inserted (AscClientDbc::Insert).
//
// The table (AscClientDbcPatch.generated.inc, tools/gen_client_patch.py) was recovered by running every
// handler in Unicorn (tools/emulate_patch_handler.py): the wire program and which wire value lands in which
// row dword are measured, not read by eye. Strings are copied into their own allocation (FUN_10ae66d0 +
// strcpy_s) and the row keeps the pointer; an overwritten row's old strings are leaked, as in the original.
// Handlers with client refresh calls or DLL side effects are written out by hand below the table.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>
#include <windows.h>

void AscAppearance_RebuildOutfits();
void AscSpellApi_SkillLineAbilityUpdate(uint32_t spell);
void AscSpellApi_SkillLineAbilityErase(uint32_t spell);

namespace
{
    struct Spec
    {
        uint32_t opcode;
        uint32_t container;
        uint32_t rowSize;
        const char* program;   // b h d q x s z, see the generator
        const char* map;       // per row dword: 1 + value index, or 0
    };
    const Spec kSpecs[] = {
#include <Ascension/AscClientDbcPatch.generated.inc>
        // By hand (the emulator cannot run their std::string handling), from the disassembly:
        // 0x967 ITEM_RANDOM_SUFFIX (FUN_1022ded0): id, 8 skipped bytes, 40 bytes of fields, two sized strings;
        // row = id, name, internal name, the fields.
        // 0x932 (FUN_1022d960): 8 dwords, container of getter FUN_100b1870 -- see On932.
        {0x932, 0xAD3D4C, 0x20, "dddddddd", "\001\002\003\004\005\006\007\010"},
        // 0x935 SPELL_ITEM_ENCHANTMENT (FUN_10232360): 14 dwords, a 16-locale string (slot 0 kept), 4 skipped, 7 dwords.
        {0x935, 0xAD48B0, 0x58, "ddddddddddddddL____ddddddd", "\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017\020\021\022\023\024\025\026"},
        // 0x937 ACHIEVEMENT_CRITERIA (FUN_10229b40): 9 dwords, a 16-locale string (slot 0), 5 dwords.
        {0x937, 0xAD3080, 0x3C, "dddddddddLddddd", "\001\002\003\004\005\006\007\010\011\012\013\014\015\016\017"},
        // 0x938 ACHIEVEMENT_CATEGORY (FUN_10229850): 2 dwords, a 16-locale string (slot 0), 4 skipped, 1 dword.
        {0x938, 0xAD30A4, 0x10, "ddL____d", "\001\002\003\004"},
        {0x967, 0xAD3ED8, 0x34, "d________xxqzz", "\001\014\015\002\003\004\005\006\007\010\011\012\013"},
    };

    std::map<uint32_t, const Spec*>& ByOpcode() { static std::map<uint32_t, const Spec*> m; return m; }

    char* CopyString(const char* s, size_t n)
    {
        char* p = static_cast<char*>(malloc(n + 1));
        memcpy(p, s, n);
        p[n] = 0;
        return p;
    }

    // The wire record as the handler reads it: numeric items split into dwords, strings as pointers.
    std::vector<uint32_t> ReadValues(const char* program, CDataStore* p)
    {
        std::vector<uint32_t> v;
        uint32_t packed = 0, bytePos = 0;
        auto take = [&](uint32_t n) { uint32_t x = 0; memcpy(&x, p->m_buffer + p->m_read, n); p->m_read += static_cast<int32_t>(n); return x; };
        for (const char* c = program; *c; ++c)
            switch (*c)
            {
            case 'b': v.push_back(take(1)); break;
            case 'h': v.push_back(take(2)); break;
            case 'd': v.push_back(take(4)); break;
            case 'q': v.push_back(take(4)); v.push_back(take(4)); break;
            case 'x': for (int i = 0; i < 4; ++i) v.push_back(take(4)); break;
            case '_': p->m_read += 1; break;   // a byte the handler skips (e.g. an in-row string slot)
            case 'L':                            // a 16-locale string, each u32 length + bytes; slot 0 is kept
            {
                uint32_t first = 0;
                for (int i = 0; i < 16; ++i)
                {
                    const uint32_t n = take(4);
                    const char* str = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
                    if (i == 0)
                        first = reinterpret_cast<uint32_t>(CopyString(str, n));
                    p->m_read += static_cast<int32_t>(n);
                }
                v.push_back(first);
                break;
            }
            case 'p':                            // a u8 packed into the current value ('|' closes it)
            {
                if (bytePos == 0)
                    v.push_back(0);
                v.back() |= take(1) << (8 * bytePos);
                ++bytePos;
                break;
            }
            case '|': bytePos = 0; break;
            case 'B':                            // a u32 of which only the low byte is kept; four pack into one value
            {
                const uint32_t b = take(4) & 0xFF;
                if (packed == 0)
                    v.push_back(0);
                v.back() |= b << (8 * packed);
                packed = (packed + 1) & 3;
                break;
            }
            case 's':
            {
                const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
                const size_t n = strlen(s);
                p->m_read += static_cast<int32_t>(n + 1);
                v.push_back(reinterpret_cast<uint32_t>(CopyString(s, n)));
                break;
            }
            case 'z':
            {
                const uint32_t n = take(4);
                const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
                p->m_read += static_cast<int32_t>(n);
                v.push_back(reinterpret_cast<uint32_t>(CopyString(s, n)));
                break;
            }
            }
        return v;
    }

    // Returns the row written; `inserted` tells a new row from an overwritten one (the hand-written
    // handlers' follow-ups depend on it).
    uint8_t* Apply(const Spec& s, CDataStore* p, bool* inserted = nullptr)
    {
        const std::vector<uint32_t> v = ReadValues(s.program, p);
        const uint32_t dwords = s.rowSize / 4;
        auto value = [&](uint32_t d) { const uint8_t k = static_cast<uint8_t>(s.map[d]); return k ? v[k - 1] : 0u; };
        const uint32_t id = value(0);
        if (uint8_t* row = AscClientDbc::Row(s.container, id))
        {
            for (uint32_t d = 0; d < dwords; ++d)
                if (s.map[d])
                    reinterpret_cast<uint32_t*>(row)[d] = value(d);
            if (inserted)
                *inserted = false;
            return row;
        }
        uint8_t* row = static_cast<uint8_t*>(operator new(s.rowSize));
        for (uint32_t d = 0; d < dwords; ++d)
            reinterpret_cast<uint32_t*>(row)[d] = value(d);
        AscClientDbc::Insert(s.container, row);
        if (inserted)
            *inserted = true;
        return row;
    }

    // Follow-ups the original runs after some patches (installed over the generic handler below).
    typedef void (*After)(uint8_t* row, bool inserted);
    std::map<uint32_t, After>& Afters() { static std::map<uint32_t, After> m; return m; }

    int __cdecl OnPatch(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        auto it = ByOpcode().find(opcode);
        if (it == ByOpcode().end())
            return 1;
        bool inserted = false;
        uint8_t* row = Apply(*it->second, p, &inserted);
        auto after = Afters().find(opcode);
        if (after != Afters().end())
            after->second(row, inserted);
        return 1;
    }

    // FUN_1008e3d0(player, unused): every inventory slot's item (0..0x26) is looked up (0x4D4DB0, type 2; the
    // results are not used), then for each equipped bag (GUIDs at 0xC23540) that exists, the bag's container
    // (vtable +0x24) gets 0x754390(i) for each of its slots (descriptor +0x100).
    void RefreshInventory(uint8_t* player)
    {
        typedef void*(__cdecl* ByGuid_t)(uint32_t, uint32_t, uint32_t);
        const ByGuid_t ByGuid = reinterpret_cast<ByGuid_t>(0x4D4DB0);
        for (uint32_t i = 0; i < 0x27; ++i)
        {
            const uint8_t* desc = *reinterpret_cast<uint8_t* const*>(player + 8);
            ByGuid(*reinterpret_cast<const uint32_t*>(desc + 0x510 + i * 8), *reinterpret_cast<const uint32_t*>(desc + 0x514 + i * 8), 2);
        }
        for (uint8_t slot = 0x13; slot < 0x17; ++slot)
        {
            const uint32_t* guid = reinterpret_cast<const uint32_t*>(0xC23540 + (slot - 0x13) * 8);   // DAT_10BC91E0 holds 0xC23540
            uint8_t* bag = static_cast<uint8_t*>(ByGuid(guid[0], guid[1], 4));
            if (!bag)
                continue;
            void* container = (*reinterpret_cast<void*(__thiscall**)(void*)>(*reinterpret_cast<uint8_t**>(bag) + 0x24))(bag);
            if (!container)
                continue;
            for (uint32_t i = 0; i < *reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t**>(bag + 8) + 0x100); ++i)
                reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x754390)(container, i);
        }
    }

    // 0x932 (FUN_1022d960): 8 dwords; id 0 is refused (returns 0, nothing stored); an OVERWRITTEN row is
    // followed by RefreshInventory(player).
    int __cdecl On932(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        uint32_t id;
        memcpy(&id, p->m_buffer + p->m_read, 4);
        if (id == 0)
        {
            p->m_read += 0x20;
            return 0;
        }
        bool inserted = false;
        Apply(*ByOpcode()[opcode], p, &inserted);
        if (!inserted)
            if (uint8_t* player = reinterpret_cast<uint8_t*(__cdecl*)()>(0x4038F0)())
                RefreshInventory(player);
        return 1;
    }

    uint8_t g_spellbookDirty = 0;   // DAT_10BE24E4: only ever written (0x934 / 0x92A)

    // 0x934 SKILL_LINE_ABILITY (FUN_10230100). Overwrite: the OLD spell leaves the spell -> row map
    // (FUN_103266c0) before the row is overwritten. Both paths then map the new spell (FUN_10328a30),
    // set DAT_10BE24E4 while in world (0xBD0792) and the client byte 0xD3F60C; an overwrite also runs
    // 0x53B4E0(spell, 0) and, unless that returned -1, 0x53FAD0(spell, 0) and 0x542030(spell, 1, 0).
    int __cdecl On934(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        const Spec& s = *ByOpcode()[opcode];
        uint32_t id;
        memcpy(&id, p->m_buffer + p->m_read, 4);
        const uint8_t* old = AscClientDbc::Row(s.container, id);
        if (old)
            AscSpellApi_SkillLineAbilityErase(*reinterpret_cast<const uint32_t*>(old + 8));
        bool inserted = false;
        const uint8_t* row = Apply(s, p, &inserted);
        const uint32_t spell = *reinterpret_cast<const uint32_t*>(row + 8);
        AscSpellApi_SkillLineAbilityUpdate(spell);
        if (*reinterpret_cast<const uint8_t*>(0xBD0792))
            g_spellbookDirty = 1;
        *reinterpret_cast<uint8_t*>(0xD3F60C) = 1;
        if (inserted)
            return 1;
        if (reinterpret_cast<int(__cdecl*)(uint32_t, int)>(0x53B4E0)(spell, 0) != -1)
        {
            reinterpret_cast<void(__cdecl*)(uint32_t, int)>(0x53FAD0)(spell, 0);
            reinterpret_cast<void(__cdecl*)(uint32_t, int, int)>(0x542030)(spell, 1, 0);
        }
        return 1;
    }

    uint8_t g_spellDirty2 = 0;   // DAT_10BE24E5: only ever written (0x92A)

    // 0x92A SPELL (FUN_10231de0): the 0x2A8-byte Spell.dbc image, then four sized strings stored at +0x220,
    // +0x228, +0x224, +0x22C (in that order). Spell modifiers the client cannot hold -- an effect applying
    // aura 0x6B / 0x6C (add flat / pct modifier) with misc value > 30 -- become aura 4 (dummy). The row is
    // copied through the client's 0x95D980 (src, size, dst) while its spell-cache flag 0xC5DEA0 is set, else
    // plainly. An overwrite in world (0xBD0792) sets DAT_10BE24E4 and DAT_10BE24E5; a new row is inserted
    // with the generic inserter (FUN_100ad860).
    int __cdecl On92A(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint8_t tmp[0x2A8];
        memcpy(tmp, p->m_buffer + p->m_read, sizeof tmp);
        p->m_read += sizeof tmp;
        static const uint32_t kStringField[4] = {0x220, 0x228, 0x224, 0x22C};
        const std::vector<uint32_t> strings = ReadValues("zzzz", p);
        for (int i = 0; i < 4; ++i)
            memcpy(tmp + kStringField[i], &strings[i], 4);
        for (int i = 0; i < 3; ++i)
        {
            int32_t& aura = *reinterpret_cast<int32_t*>(tmp + 0x17C + i * 4);
            const int32_t misc = *reinterpret_cast<const int32_t*>(tmp + 0x1B8 + i * 4);
            if ((aura == 0x6B || aura == 0x6C) && misc > 0x1E)
                aura = 4;
        }
        const bool compressed = *reinterpret_cast<const uint8_t*>(0xC5DEA0) != 0;
        typedef void(__cdecl* Pack_t)(const void*, uint32_t, void*);
        const uint32_t id = *reinterpret_cast<const uint32_t*>(tmp);
        if (uint8_t* row = AscClientDbc::Row(0xAD49D0, id))
        {
            if (compressed)
                reinterpret_cast<Pack_t>(0x95D980)(tmp, sizeof tmp, row);
            else
                memcpy(row, tmp, sizeof tmp);
            if (*reinterpret_cast<const uint8_t*>(0xBD0792))
            {
                g_spellbookDirty = 1;
                g_spellDirty2 = 1;
            }
            return 1;
        }
        uint8_t* row = static_cast<uint8_t*>(operator new(sizeof tmp));
        memcpy(row, tmp, sizeof tmp);
        if (compressed)
            reinterpret_cast<Pack_t>(0x95D980)(tmp, sizeof tmp, row);
        AscClientDbc::Insert(0xAD49D0, row);
        return 1;
    }

    // 0x95A (FUN_10233540): the record (id, 8 skipped bytes, two sized strings) is read and allocated, but
    // its getter (FUN_100b2710) always misses and its inserter (FUN_1008d890) is an empty stub -- a disabled
    // table. Nothing is stored (the original leaks the row).
    int __cdecl OnDisabled95A(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::vector<uint32_t> v = ReadValues("d________zz", p);
        (void)v;   // the strings leak as in the original
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x95A, CNetClientCustomPacket((void*)&OnDisabled95A, nullptr));
        sDC.AddPacketHandler(0x92A, CNetClientCustomPacket((void*)&On92A, nullptr));
        // 0x966 ITEM_SET (FUN_1022e0b0): a NEW row also runs the appearance manager's FUN_100c6750.
        Afters()[0x966] = [](uint8_t*, bool inserted) { if (inserted) AscAppearance_RebuildOutfits(); };
        // 0x930 / 0x931 (FUN_10232830 / FUN_10232920), either path: the client's 0x758CD0() and 0x758C50(0).
        for (uint32_t op : {0x930u, 0x931u})
            Afters()[op] = [](uint8_t*, bool)
            {
                reinterpret_cast<void(__cdecl*)()>(0x758CD0)();
                reinterpret_cast<void(__cdecl*)(int)>(0x758C50)(0);
            };
        // 0x92D (FUN_102333b0): an OVERWRITTEN row, unless flag 0x10BE3740 (FUN_102cfea0; nothing in plain code
        // sets it) -> 0x4C6230(1), Sleep(200), 0x4C82E0(1).
        Afters()[0x92D] = [](uint8_t*, bool inserted)
        {
            if (inserted)
                return;
            reinterpret_cast<void(__cdecl*)(int)>(0x4C6230)(1);
            Sleep(200);
            reinterpret_cast<void(__cdecl*)(int)>(0x4C82E0)(1);
        };
        // 0x92E (FUN_10233480), either path: on a dev realm (RealmInfo +0x44, FUN_102fc640) the client's 0x7FBE90().
        Afters()[0x92E] = [](uint8_t*, bool)
        {
            if (RealmInfoSvc::Get().gates[4])
                reinterpret_cast<void(__cdecl*)()>(0x7FBE90)();
        };
        // 0x936 ACHIEVEMENT (FUN_10229fb0): an OVERWRITTEN row -> the client's 0x5B4F00() and 0x5B6DF0(), then
        // FUN_1020ae50 rebuilds the DLL map 0x10BE05F8 (row +0x18 -> rows). Nothing in the plain code reads
        // that map, so its rebuild is not reproduced.
        Afters()[0x936] = [](uint8_t*, bool inserted)
        {
            if (inserted)
                return;
            reinterpret_cast<void(__cdecl*)()>(0x5B4F00)();
            reinterpret_cast<void(__cdecl*)()>(0x5B6DF0)();
        };
        // 0x937 / 0x938: an OVERWRITTEN row -> the client's 0x5B4F00() and 0x5B6DF0(), as 0x936.
        for (uint32_t op : {0x937u, 0x938u})
            Afters()[op] = Afters()[0x936];
        // 0x98A / 0x98B (FUN_10230900 / FUN_10230ac0), either path: 0x4C6230(1), Sleep(200), 0x4C82E0(1).
        for (uint32_t op : {0x98Au, 0x98Bu})
            Afters()[op] = [](uint8_t*, bool)
            {
                reinterpret_cast<void(__cdecl*)(int)>(0x4C6230)(1);
                Sleep(200);
                reinterpret_cast<void(__cdecl*)(int)>(0x4C82E0)(1);
            };
        for (const Spec& s : kSpecs)
        {
            ByOpcode()[s.opcode] = &s;
            sDC.AddPacketHandler(s.opcode, CNetClientCustomPacket(s.opcode == 0x932 ? (void*)&On932 : s.opcode == 0x934 ? (void*)&On934 : (void*)&OnPatch, nullptr));
        }
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
