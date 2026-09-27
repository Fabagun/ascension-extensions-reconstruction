// SMSG_UPDATE_OBJECT_ADDON 0x578 (handler_0x0578, FUN_102ce0c0) and the per-GUID field stores.
//
// Packet: u64 guid, then {u32 index, u32 value} pairs to the end. The GUID's high word picks the
// store (handler_0x0578): high < 0x10000000 or kind 0xF130/0xF140/0xF150 -> units (0x58 fields);
// 0x4000 -> items (1 field); 0xF100 / 0xF101 / 0xF110 / 0x1FC0 -> their own (empty) stores; 0x1F50
// and anything else -> ignored. Values are written only for indices inside the record; then, for
// each written index, the field's own callback and the kind's registered callback run.
//
// Lifecycle (init at 0x102CDF82): an object leaving the client's list (0x6FC0F0) drops its unit
// record (FUN_102ccfc0); on every second 0x6DF050 call every unit record except the active
// player's is dropped (FUN_102cd070 -> FUN_102cd780).
#include <Ascension/AscObjectAddon.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <map>
#include <unordered_map>

namespace
{
    typedef std::unordered_map<uint64_t, std::vector<AscObjectAddon::Field>> Store;
    Store g_units;          // 0x10BE3648, 0x58 fields
    Store g_items;          // 0x10BE3608, 1 field
    Store g_f110;           // 0x10BE3668 (0xF110 / 0x1FC0)
    Store g_f100;           // 0x10BE3688
    Store g_f101;           // 0x10BE36A8
    std::map<std::pair<uint32_t, uint32_t>, std::pair<AscObjectAddon::TypeCallback, void*>> g_typeCallbacks;   // 0x10BE36CC

    std::vector<AscObjectAddon::Field>& Record(Store& s, uint64_t guid, uint32_t fields)
    {
        auto it = s.find(guid);
        if (it == s.end())
        {
            it = s.emplace(guid, std::vector<AscObjectAddon::Field>()).first;
            it->second.resize(fields);
        }
        return it->second;
    }

    // FUN_102ce0c0's classification.
    uint32_t Kind(uint32_t high)
    {
        const uint32_t k = high >> 16;
        if ((high >> 28) == 0)
            return 4;
        switch (k)
        {
        case 0xF100: return 6;
        case 0xF101: return 7;
        case 0xF110: case 0x1FC0: return 5;
        case 0x4000: return 1;
        case 0xF130: case 0xF140: case 0xF150: return 3;
        default: return 0;
        }
    }

    void __cdecl OnObjectAddon(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint64_t guid;
        memcpy(&guid, p->m_buffer + p->m_read, 8);
        p->m_read += 8;
        const uint32_t lo = static_cast<uint32_t>(guid), hi = static_cast<uint32_t>(guid >> 32);
        const uint32_t k = hi >> 16;
        std::vector<AscObjectAddon::Field>* rec = nullptr;
        if (hi < 0x10000000 || k == 0 || k == 0xF130 || k == 0xF140 || k == 0xF150)
            rec = &AscObjectAddon::Unit(guid);
        else if (k == 0x4000)
            rec = &AscObjectAddon::Item(guid);
        else if (k == 0xF100)
            rec = &Record(g_f100, guid, 0);
        else if (k == 0xF101)
            rec = &Record(g_f101, guid, 0);
        else if (k == 0xF110 || k == 0x1FC0)
            rec = &Record(g_f110, guid, 0);
        else
            return;

        std::vector<uint32_t> changed;
        while (p->m_read != p->m_size)
        {
            uint32_t index, value;
            memcpy(&index, p->m_buffer + p->m_read, 4);
            memcpy(&value, p->m_buffer + p->m_read + 4, 4);
            p->m_read += 8;
            if (index < rec->size())
            {
                (*rec)[index].value = value;
                changed.push_back(index);
            }
        }
        const uint32_t kind = Kind(hi);
        for (uint32_t index : changed)
        {
            AscObjectAddon::Field& f = (*rec)[index];
            if (f.callback)
                f.callback(lo, hi, index, f.arg, &f);
            const auto cb = g_typeCallbacks.find({kind, index});
            if (cb != g_typeCallbacks.end() && cb->second.first)
                cb->second.first(lo, hi, index, cb->second.second, &f);
        }
    }

    // FUN_102ccfc0: the object's GUID is the first qword of its descriptor block (object +8).
    void OnObjectFree(void* object)
    {
        const uint64_t guid = **reinterpret_cast<const uint64_t* const*>(static_cast<uint8_t*>(object) + 8);
        g_units.erase(guid);
    }

    // FUN_102cd780: keep only the active player's unit record.
    void KeepActivePlayer()
    {
        const uint64_t player = AscScript::ActivePlayerGuid();
        std::vector<AscObjectAddon::Field> kept;
        bool had = false;
        if (player)
        {
            const auto it = g_units.find(player);
            if (it != g_units.end())
            {
                kept = it->second;
                had = true;
            }
        }
        g_units.clear();
        if (had)
            g_units.emplace(player, kept);
    }

    void Init()
    {
        sDC.AddPacketHandler(0x578, CNetClientCustomPacket((void*)&OnObjectAddon, nullptr));
        AscRuntime::OnObjectFree(&OnObjectFree);
        AscRuntime::OnAlternate6DF050(&KeepActivePlayer);
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

std::vector<AscObjectAddon::Field>& AscObjectAddon::Unit(uint64_t guid) { return Record(g_units, guid, 0x58); }
std::vector<AscObjectAddon::Field>& AscObjectAddon::Item(uint64_t guid) { return Record(g_items, guid, 1); }

void AscObjectAddon::RegisterTypeCallback(uint32_t kind, uint32_t index, TypeCallback fn, void* arg)
{
    g_typeCallbacks[{kind, index}] = {fn, arg};
}
