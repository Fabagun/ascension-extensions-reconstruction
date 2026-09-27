// SMSG_PATCH_* -- the server hot-patching the DLL's DBC tables (module init FUN_101e5880, 66 handlers).
//
// Wire format, identical across handlers: the raw struct image (sizeof(struct) bytes; string-pointer
// fields carry garbage) followed by one C string per string field in struct order. The row with the
// struct's first dword as id is overwritten, or inserted (FUN_10133ac0). Pure patches come from
// AscDbcPatch.generated.inc (tools/gen_dbc_patch.py); handlers with side effects register themselves
// through AscDbcPatch::Register with a callback run after the row is stored.
#include <Ascension/AscDbcPatch.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace AscDbcPatch
{
namespace
{
    struct Spec
    {
        const char* table;
        uint32_t structSize;
        uint64_t stringMask;
        AfterPatch after;
        bool sizedStrings;   // FUN_100d3680: u32 length + bytes instead of a C string
        bool afterOnInsert = false;   // `after` only when the row did not exist yet
    };

    struct Generated { uint32_t opcode; const char* table; uint32_t size; uint64_t mask; };
    const Generated kGenerated[] = {
#include <Ascension/AscDbcPatch.generated.inc>
    };

    std::map<uint32_t, Spec>& Specs() { static std::map<uint32_t, Spec> m; return m; }

    void __cdecl OnPatch(void*, uint32_t opcode, uint32_t, CDataStore* pkt)
    {
        auto it = Specs().find(opcode);
        if (it == Specs().end())
            return;
        const Spec& s = it->second;
        if (pkt->m_read + static_cast<int32_t>(s.structSize) > pkt->m_size)
            return;
        std::vector<uint8_t> row(pkt->m_buffer + pkt->m_read, pkt->m_buffer + pkt->m_read + s.structSize);
        pkt->m_read += s.structSize;
        AscDbc::Table& t = AscDbc::Get(s.table);
        for (uint32_t f = 0; f < s.structSize / 4 && f < 64; ++f)
        {
            if (!(s.stringMask & (1ull << f)))
                continue;
            size_t len;
            const char* str;
            if (s.sizedStrings)
            {
                uint32_t n;
                memcpy(&n, pkt->m_buffer + pkt->m_read, 4);
                str = reinterpret_cast<const char*>(pkt->m_buffer + pkt->m_read + 4);
                len = n;
                pkt->m_read += static_cast<int32_t>(4 + n);
            }
            else
            {
                str = reinterpret_cast<const char*>(pkt->m_buffer + pkt->m_read);
                len = strnlen(str, static_cast<size_t>(pkt->m_size - pkt->m_read));
                pkt->m_read += static_cast<int32_t>(len + 1);
            }
            const uint32_t off = t.AddString(std::string(str, len).c_str());
            memcpy(&row[f * 4], &off, 4);
        }
        uint32_t id;
        memcpy(&id, row.data(), 4);
        const bool existed = t.Row(id) != nullptr;
        t.Upsert(id, row);
        if (s.after && !(s.afterOnInsert && existed))
            s.after(t.Row(id));
    }

    void Init()
    {
        for (const Generated& g : kGenerated)
            Register(g.opcode, g.table, g.size, g.mask, nullptr);
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

void Register(uint32_t opcode, const char* table, uint32_t structSize, uint64_t stringMask, AfterPatch after)
{
    Specs()[opcode] = Spec{table, structSize, stringMask, after, false};
    sDC.AddPacketHandler(opcode, CNetClientCustomPacket((void*)&OnPatch, nullptr));
}

void RegisterSized(uint32_t opcode, const char* table, uint32_t structSize, uint64_t stringMask, AfterPatch after)
{
    Specs()[opcode] = Spec{table, structSize, stringMask, after, true};
    sDC.AddPacketHandler(opcode, CNetClientCustomPacket((void*)&OnPatch, nullptr));
}
}

void AscDbcPatch::RegisterInsertHook(uint32_t opcode, const char* table, uint32_t structSize, uint64_t stringMask, AfterPatch after)
{
    Register(opcode, table, structSize, stringMask, after);
    Specs()[opcode].afterOnInsert = true;
}
