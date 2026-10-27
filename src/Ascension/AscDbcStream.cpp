// Server-streamed rows for three client WowClientDB tables the original never patches: GameObjectDisplayInfo,
// Map and MapDifficulty. Not in the genuine DLL; built only with ASCENSION_DBC_STREAM.
//
// One packet carries a batch: u32 count, then `count` records. A record is the table's in-memory row, field by
// field, in order: 'd' a u32 (floats as their bits), 'z' a string as u32 length + bytes (no terminator). The
// row a record lands in is the same as AscClientDbcPatch's: an existing id is overwritten in place, a new id
// gets its own row in the container's index.
//
//   SMSG 0x8F3 GameObjectDisplayInfo  0x4C  id, ModelName, Sound[10], GeoBoxMin xyz, GeoBoxMax xyz, ObjectEffectPackageID
//   SMSG 0x8F4 Map                    0x48  id, Directory, InstanceType, Flags, PVP, MapName, AreaTableID,
//                                           MapDescription0, MapDescription1, LoadingScreenID, MinimapIconScale,
//                                           CorpseMapID, CorpseX, CorpseY, TimeOfDayOverride, ExpansionID,
//                                           RaidOffset, MaxPlayers
//   SMSG 0x8F5 MapDifficulty          0x1C  id, MapID, Difficulty, Message, RaidDuration, MaxPlayers, Difficultystring
//
// Row sizes are the client's own (the struct size each table's Load checks, Ascension.exe 0x63FE70 / 0x648510
// / 0x648770); the field order is the client's row reader (AscDbcReaders.generated.inc for Map/MapDifficulty).
//
// Written for streams of unknown size (the vanity-price lesson, client-ext 21f00ea / a5946de): a record costs a
// copy and its strings, nothing else -- no refresh, no per-row allocation of the index. AscClientDbc::Insert
// reallocates the whole index for every id past its end (GameObjectDisplayInfo's is ~4 MB), so a batch grows
// each index ONCE, with headroom above the top so a following batch usually fits without a copy. A whole packet
// is validated before anything is stored. Strings this file allocated are freed when their row is overwritten
// again (the original's patches leak them); the client's own string block is never touched.
//
// Not refreshed: game objects already spawned keep the display they were built with until the client creates
// them again; a Map row already used by the loading map takes effect at the next load. Rows added past a
// table's record block are reachable by id (every getter), not by the client's by-position record walks --
// the same as the original's own inserted rows.
#include <PatchConfig.hpp>

#if ASCENSION_DBC_STREAM
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscClientDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_set>
#include <vector>

namespace
{
    struct Table
    {
        uint32_t opcode;
        uint32_t container;
        uint32_t rowSize;
        const char* program;   // one char per row dword: d u32, z sized string
        const char* name;
    };
    const Table kTables[] = {
        {0x8F3, 0xAD3938, 0x4C, "dzddddddddddddddddd", "GameObjectDisplayInfo"},
        {0x8F4, 0xAD4160, 0x48, "dzdddzdzzddddddddd", "Map"},
        {0x8F5, 0xAD4184, 0x1C, "dddzddz", "MapDifficulty"},
    };

    // An index spanning more ids than this is refused (16 MB of pointers); GameObjectDisplayInfo's is ~1.08M.
    const uint32_t kMaxSpan = 1u << 22;

    uint32_t& Max(uint32_t c) { return *reinterpret_cast<uint32_t*>(c + 0xC); }
    uint32_t& Min(uint32_t c) { return *reinterpret_cast<uint32_t*>(c + 0x10); }
    uint8_t**& Index(uint32_t c) { return *reinterpret_cast<uint8_t***>(c + 0x20); }

    // The index this file last allocated per container and how many slots it holds. The client replaces the
    // index when it reloads the table (realm-data hot swap), which this detects by the pointer.
    struct Grown
    {
        uint8_t** index;
        uint32_t capacity;
    };
    std::map<uint32_t, Grown>& GrownIndexes() { static std::map<uint32_t, Grown> m; return m; }

    // Strings this file allocated; only these are freed when a row is overwritten.
    std::unordered_set<uint32_t>& OwnStrings() { static std::unordered_set<uint32_t> s; return s; }

    // Makes [lo, hi] addressable: in place when the current index has room above its top, else one new index
    // (SMemAlloc 0x76E540 / SMemFree 0x76E5A0, as the client's own) with a quarter's headroom above.
    void Reserve(uint32_t c, uint32_t lo, uint32_t hi)
    {
        uint8_t** const old = Index(c);
        const uint32_t oldMin = old ? Min(c) : lo, oldMax = old ? Max(c) : hi;
        const uint32_t newMin = lo < oldMin ? lo : oldMin;
        const uint32_t newMax = hi > oldMax ? hi : oldMax;
        if (old && newMin == oldMin && newMax == oldMax)
            return;
        const uint32_t need = newMax - newMin + 1;
        auto grown = GrownIndexes().find(c);
        if (old && newMin == oldMin && grown != GrownIndexes().end() && grown->second.index == old && need <= grown->second.capacity)
        {
            Max(c) = newMax;   // the slots above the old top were zeroed when this index was allocated
            return;
        }
        uint32_t capacity = need + need / 4 + 1024;
        if (capacity > kMaxSpan)
            capacity = need;
        uint8_t** index = static_cast<uint8_t**>(
            reinterpret_cast<void*(__stdcall*)(uint32_t, const char*, uint32_t, uint32_t)>(0x76E540)(capacity * 4, "AscDbcStream.cpp", __LINE__, 0));
        memset(index, 0, capacity * 4);
        if (old)
        {
            memcpy(index + (oldMin - newMin), old, (oldMax - oldMin + 1) * 4);
            reinterpret_cast<void(__stdcall*)(void*, const char*, uint32_t, uint32_t)>(0x76E5A0)(old, nullptr, 0, 0);
        }
        Index(c) = index;
        Min(c) = newMin;
        Max(c) = newMax;
        GrownIndexes()[c] = {index, capacity};
    }

    // A record as read: dwords, with each string as an offset/length into the packet until it is stored.
    struct Field
    {
        uint32_t value;
        uint32_t length;   // strings only
    };

    int __cdecl OnStream(void*, uint32_t opcode, uint32_t, CDataStore* p)
    {
        const Table* t = nullptr;
        for (const Table& x : kTables)
            if (x.opcode == opcode)
                t = &x;
        if (!t)
            return 1;
        const uint32_t dwords = t->rowSize / 4;

        // Validate and read the whole batch before storing any of it.
        auto left = [&]() { return static_cast<uint32_t>(p->m_size - p->m_read); };
        if (left() < 4)
        {
            AscLog::Printf("dbc stream: malformed SMSG 0x%X (no count)", opcode);
            return 1;
        }
        uint32_t count;
        memcpy(&count, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        if (count > left() / (dwords * 4))   // every field is at least 4 bytes
        {
            AscLog::Printf("dbc stream: malformed SMSG 0x%X (count %u)", opcode, count);
            p->m_read = p->m_size;
            return 1;
        }
        std::vector<Field> fields(static_cast<size_t>(count) * dwords);
        for (uint32_t r = 0; r < count; ++r)
            for (uint32_t d = 0; d < dwords; ++d)
            {
                Field& f = fields[static_cast<size_t>(r) * dwords + d];
                if (left() < 4)
                {
                    AscLog::Printf("dbc stream: malformed SMSG 0x%X (record %u)", opcode, r);
                    p->m_read = p->m_size;
                    return 1;
                }
                memcpy(&f.value, p->m_buffer + p->m_read, 4);
                p->m_read += 4;
                f.length = 0;
                if (t->program[d] != 'z')
                    continue;
                if (f.value > left())
                {
                    AscLog::Printf("dbc stream: malformed SMSG 0x%X (record %u string)", opcode, r);
                    p->m_read = p->m_size;
                    return 1;
                }
                f.length = f.value;
                f.value = static_cast<uint32_t>(p->m_read);
                p->m_read += static_cast<int32_t>(f.length);
            }

        // One index growth for every new id in the batch; ids that would stretch it past kMaxSpan are refused.
        uint8_t** index = Index(t->container);
        uint32_t lo = index ? Min(t->container) : 0xFFFFFFFF, hi = index ? Max(t->container) : 0;
        bool grow = false;
        std::vector<bool> refused(count);
        for (uint32_t r = 0; r < count; ++r)
        {
            const uint32_t id = fields[static_cast<size_t>(r) * dwords].value;
            if (index && id >= Min(t->container) && id <= Max(t->container))
                continue;
            const uint32_t nlo = id < lo ? id : lo, nhi = id > hi ? id : hi;
            if (nhi - nlo >= kMaxSpan)
            {
                AscLog::Printf("dbc stream: %s id %u refused (index span over %u)", t->name, id, kMaxSpan);
                refused[r] = true;
                continue;
            }
            lo = nlo;
            hi = nhi;
            grow = true;
        }
        if (grow)
            Reserve(t->container, lo, hi);

        std::unordered_set<uint32_t>& own = OwnStrings();
        for (uint32_t r = 0; r < count; ++r)
        {
            if (refused[r])
                continue;
            const Field* f = &fields[static_cast<size_t>(r) * dwords];
            uint8_t*& slot = Index(t->container)[f[0].value - Min(t->container)];
            uint8_t* row = slot;
            if (!row)
            {
                row = static_cast<uint8_t*>(operator new(t->rowSize));
                memset(row, 0, t->rowSize);
            }
            uint32_t* out = reinterpret_cast<uint32_t*>(row);
            for (uint32_t d = 0; d < dwords; ++d)
            {
                if (t->program[d] != 'z')
                {
                    out[d] = f[d].value;
                    continue;
                }
                if (own.erase(out[d]))
                    free(reinterpret_cast<void*>(out[d]));
                char* s = static_cast<char*>(malloc(f[d].length + 1));
                memcpy(s, p->m_buffer + f[d].value, f[d].length);
                s[f[d].length] = 0;
                out[d] = reinterpret_cast<uint32_t>(s);
                own.insert(out[d]);
            }
            slot = row;
        }
        return 1;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x8F3, CNetClientCustomPacket((void*)&OnStream, nullptr));
        sDC.AddPacketHandler(0x8F4, CNetClientCustomPacket((void*)&OnStream, nullptr));
        sDC.AddPacketHandler(0x8F5, CNetClientCustomPacket((void*)&OnStream, nullptr));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}
#endif
