// The MemoryBridge-backed DBC stores -- see AscBridgeStore.hpp.
//
//   Run: the original hooks the client's DBC initialisation 0x634E00 (FUN_10276640 over hook object
//   0x10BCB58C): original first, then every callback in the set 0x10BE2A6C. Its DBC manager
//   (0x101CF430 -> FUN_101d3d20) is one of them; the other (0x10A3AB70) lives in the protected region.
//   Only the manager's bridged loads are reproduced here, plus its HD model tables (AscHdDbc.cpp); its
//   other DBCs are still read on demand by AscDbc.
//
//   Loader (FUN_101fd240 Creature, FUN_101fded0 Quest; store objects 0x10BE045C / 0x10BE0414, table
//   singletons FUN_100c3da0 0x10BDB468 / FUN_101dc820 0x10BE0928): WDBC with the expected field count
//   and row size -> CreateTable(record size, count) -> rows packed into a VirtualAlloc'd array (tracking
//   the largest / smallest id) -> string block via 0x68 at offset 0 -> (Creature only) record name
//   pointers made relative to the string block -> records via 0x66 -> server index 0x6A(table, 0) ->
//   temporaries freed (FUN_101dc3c0), file closed, loaded = 1. Any failure resets the store (FUN_101daee0
//   / FUN_101db1b0).
//
//   Cache (FUN_100bf700; Creature 0x10BDB4C0, Quest FUN_101dc4a0): at most 0x400 records and 0x200000
//   bytes, evicted least-recently-used first; a (field offset, key) -> record index of at most 0x1000
//   entries, trimmed oldest first (FUN_100cba60) and purged of a record when it is evicted.
#include <Ascension/AscBridgeStore.hpp>
#include <Ascension/AscItemAddon.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscMemoryBridge.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <list>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace AscAnimationCount { void Apply(); }
namespace AscHdDbc { void Apply(); }

namespace
{
    typedef int(__stdcall* SFileOpen_t)(void*, const char*, uint32_t, void**);
    typedef int(__stdcall* SFileRead_t)(void*, void*, uint32_t, uint32_t*, void*, uint32_t);
    typedef int(__stdcall* SFileClose_t)(void*);
    bool Read(void* h, void* buf, uint32_t n) { return reinterpret_cast<SFileRead_t>(0x422530)(h, buf, n, nullptr, nullptr, 0) != 0; }
    void Close(void* h) { reinterpret_cast<SFileClose_t>(0x422910)(h); }

    // FUN_101b0c80: the 16 locale slots of a locstring plus its flags; returns the slot this client reads
    // ([0xC5DE9C] through the jump table at 0x101B0D38, 15 for anything else).
    uint32_t ReadLocString(void* h)
    {
        static const uint32_t kMap[] = {0, 2, 3, 4, 6, 8, 9, 10, 11};
        const uint32_t locale = *reinterpret_cast<const uint32_t*>(0xC5DE9C);
        const uint32_t slot = locale <= 8 ? kMap[locale] : 15;
        uint32_t value = 0, skip;
        for (uint32_t i = 0; i < 0x10; ++i)
            Read(h, i == slot ? &value : &skip, 4);
        Read(h, &skip, 4);
        return value;
    }

    // ---- reply readers ------------------------------------------------------------------------------
    // FUN_100be440
    uint32_t GetU32(const std::vector<uint8_t>& b, size_t& pos)
    {
        if (b.size() < pos + 4)
            throw std::runtime_error("Buffer underflow during deserialization");
        uint32_t v;
        memcpy(&v, b.data() + pos, 4);
        pos += 4;
        return v;
    }
    // FUN_100be490
    uint8_t GetU8(const std::vector<uint8_t>& b, size_t& pos)
    {
        if (b.size() < pos + 1)
            throw std::runtime_error("Buffer underflow during deserialization");
        return b[pos++];
    }

    // FUN_100c2380 / FUN_100cd2f0: a record's string table, "MSTB" 1, u32 n, n x {u32 id, u32 field, u32,
    // u32 offset, u32 length}, u32 size, size bytes. False when the header is not MSTB 1 or an entry runs
    // past the blob; a short buffer throws.
    struct StringEntry { uint32_t id, field, unknown, offset, length; };
    struct StringTable
    {
        std::vector<StringEntry> entries;
        std::vector<char> blob;
        std::unordered_map<uint64_t, const StringEntry*> byKey;   // (id << 32 | field)
    };
    bool ParseStrings(const std::vector<uint8_t>& b, size_t& pos, StringTable& t)
    {
        t.entries.clear();
        t.blob.clear();
        t.byKey.clear();
        if (GetU32(b, pos) != 0x4254534D || GetU32(b, pos) != 1)
            return false;
        t.entries.resize(GetU32(b, pos));
        for (StringEntry& e : t.entries)
        {
            if (b.size() < pos + sizeof(StringEntry))
                throw std::runtime_error("Buffer underflow during deserialization");
            memcpy(&e, b.data() + pos, sizeof(StringEntry));
            pos += sizeof(StringEntry);
        }
        const uint32_t size = GetU32(b, pos);
        if (b.size() < pos + size)
            throw std::runtime_error("Buffer underflow during deserialization");
        t.blob.assign(b.begin() + pos, b.begin() + pos + size);
        pos += size;
        for (const StringEntry& e : t.entries)
        {
            if (e.offset > t.blob.size() || e.length > t.blob.size() - e.offset)
                return false;
            t.byKey[(static_cast<uint64_t>(e.id) << 32) | e.field] = &e;
        }
        return true;
    }

    // ---- records ------------------------------------------------------------------------------------
    using AscBridgeStore::CreatureRecord;
    struct QuestRecord { uint32_t f[0x1D]; };
    static_assert(sizeof(QuestRecord) == 0x74, "QuestRecord");
    // MythicKeystones.dbc / SpellAffect.dbc: 3 u32 read as-is (FUN_101b9760), 0xC-byte record.
    struct SmallRecord { uint32_t f[3]; };

    using ItemAddonRecord = AscItemAddon::Record;
    using AscBridgeStore::VanityRecord;

    // What a cached record costs against the 2 MB budget, and how it is freed.
    uint32_t Len(const char* s) { return s ? static_cast<uint32_t>(strlen(s)) + 1 : 0; }
    uint32_t Cost(const CreatureRecord& r) { return 0x1C + Len(r.name); }                     // FUN_100c5c40
    uint32_t Cost(const QuestRecord&) { return 0x74; }                                         // FUN_101e6da0
    uint32_t Cost(const SmallRecord&) { return 0xC; }                                          // FUN_101e6c20
    uint32_t Cost(const ItemAddonRecord& r) { return 0x40 + Len(r.name) + Len(r.description); }   // FUN_101a59b0
    uint32_t Cost(const VanityRecord& r) { return 0xB4 + Len(r.Str(3)) + Len(r.Str(0x2A)) + Len(r.Str(0x2B)); }   // FUN_1032e760
    void Release(CreatureRecord& r) { free(const_cast<char*>(r.name)); }
    void Release(QuestRecord&) {}
    void Release(SmallRecord&) {}
    void Release(ItemAddonRecord& r)
    {
        free(const_cast<char*>(r.name));
        free(const_cast<char*>(r.description));
    }
    uint32_t RecordId(const CreatureRecord& r) { return r.id; }
    uint32_t RecordId(const QuestRecord& r) { return r.f[0]; }
    uint32_t RecordId(const SmallRecord& r) { return r.f[0]; }
    uint32_t RecordId(const ItemAddonRecord& r) { return r.rowId; }
    uint32_t RecordId(const VanityRecord& r) { return r.f[0]; }
    void Release(VanityRecord& r)
    {
        for (uint32_t i : {3u, 0x2Au, 0x2Bu})
            free(const_cast<char*>(r.Str(i)));
    }

    // The string fields a record's reply carries (DAT_10BDB458 FUN_100c1740 / DAT_10BDEDCC FUN_101a37b0).
    const std::vector<uint32_t>& StringFields(const CreatureRecord*) { static const std::vector<uint32_t> f = {8}; return f; }
    const std::vector<uint32_t>& StringFields(const ItemAddonRecord*) { static const std::vector<uint32_t> f = {8, 0xC}; return f; }
    const std::vector<uint32_t>& StringFields(const VanityRecord*) { static const std::vector<uint32_t> f = {0xC, 0xA8, 0xAC}; return f; }   // DAT_10D3C154 (FUN_1032bd50)
    const std::vector<uint32_t>& StringFields(const QuestRecord*) { static const std::vector<uint32_t> f; return f; }                          // DAT_10BE0794, never filled
    const std::vector<uint32_t>& StringFields(const SmallRecord*) { static const std::vector<uint32_t> f; return f; }                          // DAT_10BE07A0 / DAT_10BE0788, never filled

    // ---- the cache (FUN_100bf700 and friends) ---------------------------------------------------------
    template <class Record> class RecordCache
    {
    public:
        // FUN_100c3910 / FUN_102129e0
        Record* Get(uint32_t index)
        {
            auto it = m_records.find(index);
            if (it == m_records.end())
            {
                ++m_misses;
                return nullptr;
            }
            ++m_hits;
            m_lru.splice(m_lru.begin(), m_lru, it->second.lru);
            return it->second.record;
        }

        // FUN_100c5c40 / FUN_101e6da0: takes ownership; nullptr when the record was evicted straight away.
        Record* Insert(uint32_t index, Record* record)
        {
            if (!record)
                return nullptr;
            EvictIndex(index);
            const uint32_t cost = Cost(*record);
            m_lru.push_front(index);
            m_bytes += cost;
            m_records[index] = Slot{record, cost, m_lru.begin()};
            ++m_inserts;
            while ((kMaxRecords < m_records.size() || kMaxBytes < m_bytes) && !m_lru.empty())
            {
                auto victim = m_records.find(m_lru.back());
                if (victim == m_records.end())
                    m_lru.pop_back();
                else
                {
                    Evict(victim);
                    ++m_evictions;
                }
            }
            auto it = m_records.find(index);
            return it == m_records.end() ? nullptr : it->second.record;
        }

        // FUN_100c19c0 / FUN_101dac80: only for a cached record; a key already present moves to the front.
        void Index(uint32_t field, uint32_t key, uint32_t index)
        {
            if (m_records.find(index) == m_records.end())
                return;
            const uint64_t k = IndexKey(field, key);
            auto old = m_index.find(k);
            if (old != m_index.end())
                Unindex(old);
            m_indexLru.push_front(k);
            m_index[k] = IndexSlot{index, m_indexLru.begin()};
            while (kMaxIndex < m_index.size() && !m_indexLru.empty())   // FUN_100cba60
            {
                auto it = m_index.find(m_indexLru.back());
                if (it == m_index.end())
                    m_indexLru.pop_back();
                else
                    Unindex(it);
            }
        }

        // The index probe at the top of FUN_100b8cc0 / FUN_101d0bd0: a live hit counts as an index hit, a
        // stale key is dropped (FUN_100c2780), and anything else counts as an index miss.
        Record* FindIndexed(uint32_t field, uint32_t key)
        {
            auto it = m_index.find(IndexKey(field, key));
            if (it != m_index.end())
            {
                m_indexLru.splice(m_indexLru.begin(), m_indexLru, it->second.lru);
                if (Record* r = Get(it->second.index))
                {
                    ++m_indexHits;
                    return r;
                }
                Unindex(it);
            }
            ++m_indexMisses;
            return nullptr;
        }

        // FUN_101dbdc0
        void EvictIndex(uint32_t index)
        {
            auto it = m_records.find(index);
            if (it != m_records.end())
                Evict(it);
        }

        // FUN_10209860's walk: the first cached record whose id (+0) is `id`.
        void EvictId(uint32_t id)
        {
            for (auto it = m_records.begin(); it != m_records.end(); ++it)
                if (RecordId(*it->second.record) == id)
                {
                    Evict(it);
                    return;
                }
        }

        void Clear()
        {
            while (!m_records.empty())
                Evict(m_records.begin());
            m_lru.clear();
        }

    private:
        static const size_t kMaxRecords = 0x400, kMaxBytes = 0x200000, kMaxIndex = 0x1000;
        struct Slot { Record* record; uint32_t cost; std::list<uint32_t>::iterator lru; };
        struct IndexSlot { uint32_t index; std::list<uint64_t>::iterator lru; };
        typedef typename std::unordered_map<uint32_t, Slot>::iterator RecordIt;
        typedef typename std::unordered_map<uint64_t, IndexSlot>::iterator IndexIt;

        static uint64_t IndexKey(uint32_t field, uint32_t key) { return (static_cast<uint64_t>(key) << 32) | field; }

        void Unindex(IndexIt it)
        {
            m_indexLru.erase(it->second.lru);
            m_index.erase(it);
        }

        // FUN_100c25c0 / FUN_101dc000
        void Evict(RecordIt it)
        {
            const uint32_t index = it->first;
            Release(*it->second.record);
            delete it->second.record;
            m_bytes -= it->second.cost < m_bytes ? it->second.cost : m_bytes;
            m_lru.erase(it->second.lru);
            m_records.erase(it);
            for (auto i = m_index.begin(); i != m_index.end();)
            {
                if (i->second.index == index)
                {
                    m_indexLru.erase(i->second.lru);
                    i = m_index.erase(i);
                }
                else
                    ++i;
            }
        }

        std::unordered_map<uint32_t, Slot> m_records;
        std::list<uint32_t> m_lru;
        std::unordered_map<uint64_t, IndexSlot> m_index;
        std::list<uint64_t> m_indexLru;
        size_t m_bytes = 0;
        uint64_t m_hits = 0, m_misses = 0, m_indexHits = 0, m_indexMisses = 0, m_inserts = 0, m_evictions = 0;
    };

    // ---- stores + the generic loader ---------------------------------------------------------------
    // Store object (+4 loaded, +8 count, +0xC largest id, +0x10 smallest id, +0x14 string block, +0x1C
    // records) + its table singleton (+0 table, +0xC string size) + its cache.
    template <class Record> struct Store
    {
        uint32_t loaded = 0, count = 0, maxId = 0xFFFFFFFF, minId = 0xFFFFFFF;
        char* strings = nullptr;
        Record* records = nullptr;
        uint32_t table = 0, stringSize = 0;
        RecordCache<Record> cache;
    };
    Store<CreatureRecord> g_creature;
    Store<QuestRecord> g_quest;
    Store<ItemAddonRecord> g_itemAddon;   // store object 0x10BE0438, table singleton 0x10BDEDE0
    Store<VanityRecord> g_vanity;         // store object 0x10BE0480, table singleton 0x10BE08D0
    Store<SmallRecord> g_keystone;        // MythicKeystones: store object 0x10BE03F0, table singleton 0x10BE0A40
    Store<SmallRecord> g_spellAffect;     // SpellAffect: store object 0x10BE04A4, table singleton 0x10BE07B8

    template <class Record> void FreeTemporaries(Store<Record>& s)   // FUN_101dc3c0
    {
        if (s.records)
        {
            VirtualFree(s.records, 0, MEM_RELEASE);
            s.records = nullptr;
        }
        if (s.strings)
        {
            VirtualFree(s.strings, 0, MEM_RELEASE);
            s.strings = nullptr;
        }
    }

    template <class Record> void Reset(Store<Record>& s, void* file)   // FUN_101daee0 / FUN_101db1b0
    {
        FreeTemporaries(s);
        if (s.table)
        {
            AscMemoryBridge::DestroyTable(s.table);
            s.table = 0;
        }
        s.stringSize = 0;
        s.cache.Clear();
        if (file)
            Close(file);
        s.loaded = 0;
        s.count = 0;
        s.maxId = 0xFFFFFFFF;
        s.minId = 0xFFFFFFF;
    }

    // FUN_101cf330
    void ReadRow(void* h, CreatureRecord& r)
    {
        Read(h, &r.id, 4);
        Read(h, &r.entry, 4);
        const uint32_t name = ReadLocString(h);
        Read(h, &r.displayId, 4);
        Read(h, &r.f10, 4);
        Read(h, &r.f14, 4);
        Read(h, &r.f18, 4);
        r.name = reinterpret_cast<const char*>(static_cast<uintptr_t>(name));   // a string-block offset
    }
    // FUN_102137b0: 29 u32 in order.
    void ReadRow(void* h, QuestRecord& r)
    {
        for (uint32_t& f : r.f)
            Read(h, &f, 4);
    }
    // FUN_1020e270: row id, item, name locstring, description locstring, 12 u32.
    void ReadRow(void* h, ItemAddonRecord& r)
    {
        Read(h, &r.rowId, 4);
        Read(h, &r.id, 4);
        const uint32_t name = ReadLocString(h);
        const uint32_t description = ReadLocString(h);
        for (uint32_t& f : r.fields)
            Read(h, &f, 4);
        r.name = reinterpret_cast<const char*>(static_cast<uintptr_t>(name));
        r.description = reinterpret_cast<const char*>(static_cast<uintptr_t>(description));
    }
    // FUN_1021a230: cols 0..2, the col-3 string, cols 4..12, cols 13..14 as one 8-byte read, cols 15..17,
    // cols 18..41 as one 0x60-byte read, two locstrings (cols 42..58, 59..75), col 76.
    void ReadRow(void* h, VanityRecord& r)
    {
        uint32_t artwork;
        Read(h, &r.f[0], 4);
        Read(h, &r.f[1], 4);
        Read(h, &r.f[2], 4);
        Read(h, &artwork, 4);
        for (uint32_t i = 4; i <= 0xC; ++i)
            Read(h, &r.f[i], 4);
        Read(h, &r.f[0xD], 8);
        for (uint32_t i = 0xF; i <= 0x11; ++i)
            Read(h, &r.f[i], 4);
        Read(h, &r.f[0x12], 0x60);
        const uint32_t description = ReadLocString(h);
        const uint32_t additional = ReadLocString(h);
        Read(h, &r.f[0x2C], 4);
        r.f[3] = artwork;
        r.f[0x2A] = description;
        r.f[0x2B] = additional;
    }
    // FUN_101b9760
    void ReadRow(void* h, SmallRecord& r)
    {
        for (uint32_t& f : r.f)
            Read(h, &f, 4);
    }
    void ClearName(SmallRecord&) {}
    void ClearName(VanityRecord& r) { r.f[3] = r.f[0x2A] = r.f[0x2B] = 0; }
    void ClearName(CreatureRecord& r) { r.name = nullptr; }
    void ClearName(QuestRecord&) {}
    void ClearName(ItemAddonRecord& r)
    {
        r.name = nullptr;
        r.description = nullptr;
    }

    // FUN_101fd240 / FUN_101fded0
    template <class Record> void Load(Store<Record>& s, const char* path, uint32_t fields, uint32_t rowSize)
    {
        void* h = nullptr;
        if (!reinterpret_cast<SFileOpen_t>(0x424B50)(nullptr, path, 0x20000, &h))
            return;
        uint32_t magic = 0, fileFields = 0, fileRowSize = 0, stringSize = 0;
        if (!Read(h, &magic, 4) || magic != 0x43424457 || !Read(h, &s.count, 4) || s.count == 0 || !Read(h, &fileFields, 4) ||
            fileFields != fields || !Read(h, &fileRowSize, 4) || fileRowSize != rowSize || !Read(h, &stringSize, 4))
        {
            Reset(s, h);
            return;
        }
        s.table = AscMemoryBridge::CreateTable(sizeof(Record), s.count);
        if (!s.table)
        {
            Reset(s, h);
            return;
        }
        s.stringSize = stringSize;
        s.strings = stringSize ? static_cast<char*>(VirtualAlloc(nullptr, stringSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)) : nullptr;
        if (stringSize && !s.strings)
        {
            Reset(s, h);
            return;
        }
        s.maxId = 0;
        s.minId = 0xFFFFFFF;
        if (s.count > 0xFFFFFFFFu / sizeof(Record) ||
            !(s.records = static_cast<Record*>(VirtualAlloc(nullptr, s.count * sizeof(Record), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))))
        {
            Reset(s, h);
            return;
        }
        for (uint32_t i = 0; i < s.count; ++i)
        {
            Record& r = s.records[i];
            ReadRow(h, r);
            if (RecordId(r) > s.maxId)
                s.maxId = RecordId(r);
            if (RecordId(r) < s.minId)
                s.minId = RecordId(r);
        }
        if (stringSize && (!Read(h, s.strings, stringSize) || !AscMemoryBridge::Command68(s.table, 0, s.strings, stringSize)))
        {
            Reset(s, h);
            return;
        }
        // The name fields already hold string-block offsets, which is what the original's pointer
        // subtraction produces (and a zero offset when there is no string block).
        if (!s.strings)
            for (uint32_t i = 0; i < s.count; ++i)
                ClearName(s.records[i]);
        if (!AscMemoryBridge::Command66(s.table, 0, s.records, s.count * static_cast<uint32_t>(sizeof(Record))))
        {
            Reset(s, h);
            return;
        }
        for (uint32_t field : {0u})
            AscMemoryBridge::Command6A(s.table, field);
        FreeTemporaries(s);
        Close(h);
        s.loaded = 1;
    }

    // ---- keyed lookup with string fields (Creature, ItemAddon) --------------------------------------
    const char*& StringSlot(void* record, uint32_t field) { return *reinterpret_cast<const char**>(static_cast<uint8_t*>(record) + field); }

    // FUN_100c18f0 -> FUN_100b8440 (Creature) / FUN_101a3940 -> FUN_101a2120 + FUN_101a2290 (ItemAddon):
    // each string field in turn, from the reply's string table (FUN_101a3840: entry (index, field)), else
    // fetched from the bridge (0x6C, keyed by the string-block offset the field holds). Every string is a
    // malloc'd copy. The first failed fetch frees the copies made so far, clears that field and fails
    // the record; later fields are not touched.
    template <class Record> bool ResolveStrings(Record& r, const StringTable* strings, uint32_t index, uint32_t table)
    {
        std::vector<char*> made;
        for (uint32_t field : StringFields(static_cast<Record*>(nullptr)))
        {
            const char*& slot = StringSlot(&r, field);
            if (strings)
            {
                auto it = strings->byKey.find((static_cast<uint64_t>(index) << 32) | field);
                if (it != strings->byKey.end())
                {
                    const StringEntry& e = *it->second;
                    const char* src = strings->blob.data() + e.offset;
                    const bool terminated = e.length != 0 && src[e.length - 1] == '\0';
                    uint32_t size = e.length + (terminated ? 0 : 1);
                    if (size == 0)
                        size = 1;
                    char* copy = static_cast<char*>(malloc(size));
                    if (e.length)
                        memcpy(copy, src, e.length);
                    if (!terminated)
                        copy[size - 1] = '\0';
                    made.push_back(copy);
                    slot = copy;
                    continue;
                }
            }
            std::string text;
            if (!AscMemoryBridge::Command6C(table, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(slot)), text))
            {
                for (char* m : made)
                    free(m);
                slot = nullptr;
                return false;
            }
            char* copy = static_cast<char*>(malloc(text.size()));
            memcpy(copy, text.data(), text.size());
            made.push_back(copy);
            slot = copy;
        }
        return true;
    }

    // FUN_100b8cc0 / FUN_101a2690 / FUN_103298f0 (key field +4) and FUN_101d0bd0 / FUN_101d0500 /
    // FUN_101d1290 / FUN_10329f60 (id, field 0): the record whose `field` is `key`. 0x6D on that field with
    // the record's string fields as extras; the reply is total, key, u8 found, u32 index (< count), then
    // -- unless the index is cached -- the record and (with string fields) its MSTB string table, consumed
    // exactly. On a mismatch the strings already resolved are not freed, as in the original.
    template <class Record> const Record* Lookup(Store<Record>& s, uint32_t field, uint32_t key)
    {
        if (!s.table)
            return nullptr;
        RecordCache<Record>& cache = s.cache;
        if (Record* r = cache.FindIndexed(field, key))
            return r;
        const std::vector<uint32_t>& stringFields = StringFields(static_cast<Record*>(nullptr));
        std::vector<uint8_t> out;
        if (!AscMemoryBridge::Command6D(s.table, field, std::vector<uint32_t>{key}, out, stringFields))
            return nullptr;
        size_t pos = 0;
        if (GetU32(out, pos) == 0)
            return nullptr;
        const uint32_t replyKey = GetU32(out, pos);
        const uint8_t found = GetU8(out, pos);
        if (!found || replyKey != key)
            return nullptr;
        const uint32_t index = GetU32(out, pos);
        if (index >= s.count)
            return nullptr;
        if (Record* r = cache.Get(index))
        {
            cache.Index(field, key, index);
            cache.Index(0, RecordId(*r), index);
            return r;
        }
        if (out.size() < pos + sizeof(Record))
            return nullptr;
        Record r;
        memcpy(&r, out.data() + pos, sizeof(r));
        pos += sizeof(r);
        if (!stringFields.empty())
        {
            StringTable strings;
            if (!ParseStrings(out, pos, strings) || !ResolveStrings(r, &strings, index, s.table))
                return nullptr;
        }
        if (pos != out.size())
            return nullptr;
        Record* stored = cache.Insert(index, new Record(r));
        if (stored)
        {
            cache.Index(0, RecordId(*stored), index);
            cache.Index(field, key, index);
        }
        return stored;
    }
    template <class Record> const Record* LookupByKey(Store<Record>& s, uint32_t key) { return Lookup(s, 4, key); }
    template <class Record> const Record* LookupById(Store<Record>& s, uint32_t id) { return Lookup(s, 0, id); }

    // ---- Quest ---------------------------------------------------------------------------------------
    // Quest queries carry no string fields: DAT_10BE0794 is never filled, so no MSTB table follows a record.

    // FUN_102121e0: the fields a quest group can be named in.
    const uint32_t kQuestGroupFields[] = {8, 0xC, 0x10};

    // FUN_102127c0: every record index whose group field holds `group`, sorted and unique.
    bool QuestGroupIndexes(Store<QuestRecord>& s, uint32_t group, std::vector<uint32_t>& out)
    {
        out.clear();
        if (!s.table)
            return true;
        for (uint32_t field : kQuestGroupFields)
        {
            std::vector<uint32_t> hits;
            if (!AscMemoryBridge::Command6B(s.table, field, group, hits))
                return false;
            out.insert(out.end(), hits.begin(), hits.end());
        }
        if (out.size() > 1)
        {
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
        }
        return true;
    }

    // FUN_10212cd0 (Quest) / FUN_1032ef40 (Vanity): make sure every index is cached. Indexes at or past
    // the count are skipped, cached ones are marked found, and the rest go out as one 0x6F batch with the
    // record's string fields as extras. The reply is u32 n, then n x {u32 index, u8 found, and when found
    // the record, then (with string fields) its MSTB string table}; it must be consumed exactly. `found`
    // is then refreshed from the cache for every entry.
    template <class Record> bool FetchBatch(Store<Record>& s, std::vector<std::pair<uint32_t, bool>>& entries)
    {
        if (entries.empty() || s.count == 0 || !s.table)
            return true;
        std::vector<uint32_t> missing;
        for (auto& e : entries)
            if (e.first < s.count)
            {
                if (s.cache.Get(e.first))
                    e.second = true;
                else
                    missing.push_back(e.first);
            }
        if (!missing.empty())
        {
            const std::vector<uint32_t>& stringFields = StringFields(static_cast<Record*>(nullptr));
            std::vector<uint8_t> out;
            if (!AscMemoryBridge::Command6F(s.table, missing, out, stringFields))
                return false;
            size_t pos = 0;
            const uint32_t n = GetU32(out, pos);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t index = GetU32(out, pos);
                if (!GetU8(out, pos))
                    continue;
                if (out.size() < pos + sizeof(Record))
                    return false;
                Record r;
                memcpy(&r, out.data() + pos, sizeof(Record));
                pos += sizeof(Record);
                if (!stringFields.empty())
                {
                    StringTable strings;
                    if (!ParseStrings(out, pos, strings) || !ResolveStrings(r, &strings, index, s.table))
                        return false;
                }
                if (Record* stored = s.cache.Insert(index, new Record(r)))   // FUN_1020ac50 / FUN_1032e760
                    s.cache.Index(0, RecordId(*stored), index);
            }
            if (pos != out.size())
                return false;
        }
        for (auto& e : entries)
            if (!e.second)
                e.second = e.first < s.count && s.cache.Get(e.first) != nullptr;
        return true;
    }

    bool FetchQuests(Store<QuestRecord>& s, const std::vector<uint32_t>& indexes)
    {
        std::vector<std::pair<uint32_t, bool>> entries;
        for (uint32_t index : indexes)
            entries.emplace_back(index, false);
        return FetchBatch(s, entries);
    }

    // FUN_101cf4c0 / FUN_101cf580 / FUN_101cf700 / FUN_101cf7c0 / FUN_10329610: the table index of the
    // record whose id (+0) is `id` -- the first hit of 0x6B on field 0.
    template <class Record> bool IndexOf(Store<Record>& s, uint32_t id, uint32_t& index)
    {
        std::vector<uint32_t> hits;
        if (!s.table || !AscMemoryBridge::Command6B(s.table, 0, id, hits) || hits.empty())
            return false;
        index = hits[0];
        return true;
    }

    // FUN_10209ae0 / FUN_10330970 / the head of FUN_102094b0: the string block starts with one NUL byte;
    // write it first when the block is empty.
    template <class Record> bool EnsureStringBlock(Store<Record>& s)
    {
        if (s.stringSize != 0)
            return true;
        const char nul = 0;
        if (!AscMemoryBridge::Command68(s.table, 0, &nul, 1))
            return false;
        s.stringSize = 1;
        return true;
    }

    // FUN_101d3190 (ItemAddon) / FUN_1032bb10 (Vanity) / FUN_102094b0 (Creature): append each non-null
    // string field, in order, to the table's string block (0x68 at its end) and replace the pointer by its
    // offset. A null string stays 0. The first failure leaves the rest alone and fails the patch.
    template <class Record> bool AppendStrings(Store<Record>& s, Record& r)
    {
        for (uint32_t field : StringFields(static_cast<Record*>(nullptr)))
        {
            const char*& slot = StringSlot(&r, field);
            if (!slot)
                continue;
            if (!EnsureStringBlock(s))
                return false;
            const uint32_t len = static_cast<uint32_t>(strlen(slot)) + 1;
            if (len > ~s.stringSize)
                return false;
            const uint32_t offset = s.stringSize;
            if (!AscMemoryBridge::Command68(s.table, offset, slot, len))
                return false;
            s.stringSize += len;
            slot = reinterpret_cast<const char*>(static_cast<uintptr_t>(offset));
        }
        return true;
    }

    // FUN_102094b0 (Creature) / FUN_10209860 (Quest) / FUN_102096d0 (ItemAddon) / FUN_102099a0
    // (SpellAffect) / FUN_1032eae0 (Vanity): overwrite `existing`'s table row with `r`, or append `r`
    // when there is none (refused when its id is already in the table). Strings go to the string block,
    // then 0x73 writes the record; on success the cached record at that index is dropped, the count grows
    // for an append, a changed id drops the old id's cached record, and the id range widens.
    template <class Record> bool PatchRecord(Store<Record>& s, const Record* existing, const Record& r)
    {
        if (!s.table)
            return false;
        uint32_t index = 0, oldId = 0;
        if (!existing)
        {
            if (IndexOf(s, RecordId(r), index))
                return false;
            index = s.count;
        }
        else
        {
            oldId = RecordId(*existing);
            if (!IndexOf(s, oldId, index))
                return false;
        }
        Record row = r;
        if (!AppendStrings(s, row) || !AscMemoryBridge::Command73(s.table, index, &row, sizeof(row)))
            return false;
        s.cache.EvictIndex(index);
        if (!existing)
            ++s.count;
        else if (oldId != RecordId(r))
            s.cache.EvictId(oldId);
        if (RecordId(r) < s.minId)
            s.minId = RecordId(r);
        if (RecordId(r) > s.maxId)
            s.maxId = RecordId(r);
        return true;
    }

    // SMSG 0x56F (FUN_101e2a30): one whole Quest.dbc row.
    void __cdecl OnPatchQuest(void*, uint32_t, uint32_t, CDataStore* p)
    {
        uint8_t row[0x74];
        memcpy(row, p->m_buffer + p->m_read, sizeof(row));
        p->m_read += sizeof(row);
        AscBridgeStore::PatchQuest(row);
    }

    // ---- ItemAddon patch (SMSG 0x569) -------------------------------------------------------------

    uint32_t ReadU32(CDataStore* p)
    {
        uint32_t v;
        memcpy(&v, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        return v;
    }
    std::string ReadSized(CDataStore* p)   // FUN_100d3680
    {
        const uint32_t n = ReadU32(p);
        std::string text(reinterpret_cast<const char*>(p->m_buffer + p->m_read), n);
        p->m_read += static_cast<int32_t>(n);
        return text;
    }

    // SMSG 0x569 (FUN_101e1800): u32 item, 8 bytes not read, 12 u32 (+0x10..+0x3C), u32-length name,
    // u32-length description. The record keeps the existing row id, or takes the largest id + 1.
    void __cdecl OnPatchItemAddon(void*, uint32_t, uint32_t, CDataStore* p)
    {
        ItemAddonRecord r = {};
        r.id = ReadU32(p);
        p->m_read += 8;
        for (uint32_t& f : r.fields)
            f = ReadU32(p);
        const std::string name = ReadSized(p), description = ReadSized(p);
        char* nameCopy = _strdup(name.c_str());
        char* descriptionCopy = _strdup(description.c_str());
        r.name = nameCopy;
        r.description = descriptionCopy;
        const ItemAddonRecord* existing = LookupByKey(g_itemAddon, r.id);
        r.rowId = existing ? existing->rowId : g_itemAddon.maxId + 1;
        const bool patched = PatchRecord(g_itemAddon, existing, r);
        free(nameCopy);
        free(descriptionCopy);
        // On success the original refreshes FUN_100c5eb0, FUN_100c65c0, FUN_1032e9b0 and FUN_10330960
        // (not yet transcribed). With the shipped MMgr64.exe 0x73 is refused, so this never succeeds
        // (IMPROVEMENTS.md).
        (void)patched;
    }

    // SMSG 0x567 (FUN_101e14f0): u32 entry, 4 bytes not read, 4 u32 (+0xC..+0x18), u32-length name. A known
    // entry is overwritten in its CACHED record only -- the new name replaces the old one without freeing
    // it, and the id (+0) becomes 0. An unknown entry becomes a record with the next id (largest + 1)
    // appended through FUN_101da550 -> FUN_102094b0 (with the shipped MMgr64.exe 0x73 is refused).
    void __cdecl OnPatchCreature(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t entry = ReadU32(p);
        p->m_read += 4;
        uint32_t f[4];
        for (uint32_t& v : f)
            v = ReadU32(p);
        const std::string text = ReadSized(p);
        char* name = _strdup(text.c_str());
        if (CreatureRecord* r = const_cast<CreatureRecord*>(LookupByKey(g_creature, entry)))
        {
            r->name = name;
            r->displayId = f[0];
            r->f10 = f[1];
            r->f14 = f[2];
            r->id = 0;
            r->entry = entry;
            r->f18 = f[3];
            return;
        }
        CreatureRecord r = {g_creature.maxId + 1, entry, name, f[0], f[1], f[2], f[3]};
        PatchRecord(g_creature, static_cast<const CreatureRecord*>(nullptr), r);
        free(name);
    }

    // SMSG 0x574 (FUN_101e3790): 3 u32; patches the SpellAffect record with that id, else appends it.
    void __cdecl OnPatchSpellAffect(void*, uint32_t, uint32_t, CDataStore* p)
    {
        SmallRecord r;
        for (uint32_t& v : r.f)
            v = ReadU32(p);
        const SmallRecord* found = LookupById(g_spellAffect, r.f[0]);
        SmallRecord existing;
        if (found)
            existing = *found;
        PatchRecord(g_spellAffect, found ? &existing : nullptr, r);
    }

    // ---- the DBC-initialisation hook (0x634E00 -> FUN_10276640) --------------------------------------
    typedef void(__cdecl* DbcInit_t)();
    DbcInit_t g_dbcInit = nullptr;
    void __cdecl DbcInitDetour()
    {
        g_dbcInit();
        AscAnimationCount::Apply();   // 0x10A3AB70 -> FUN_10a51180, the set's other callback
        // FUN_101d3d20, bridged part (0x101D9101): Creature, Quest, ItemAddon, VanityCollection (all
        // yet reproduced).
        // FUN_101d3d20 loads these two first (0x101D3D7A, 0x101D4338), so they get the first table handles.
        Load(g_keystone, "DBFilesClient\\MythicKeystones.dbc", 3, 0xC);
        Load(g_spellAffect, "DBFilesClient\\SpellAffect.dbc", 3, 0xC);
        Load(g_creature, "DBFilesClient\\Creature.dbc", 0x17, 0x5C);
        Load(g_quest, "DBFilesClient\\Quest.dbc", 0x1D, 0x74);
        Load(g_itemAddon, "DBFilesClient\\ItemAddon.dbc", 0x30, 0xC0);
        Load(g_vanity, "DBFilesClient\\VanityCollection.dbc", 0x4C, 0x134);
        AscHdDbc::Apply();            // 0x101D9251..0x101D9BA3, see AscHdDbc.cpp
        // Later in FUN_101d3d20 (0x101D9BCD..0x101D9D59): extra server indexes, {4} on the tables of
        // singletons 0x10BDEDE0 (ItemAddon) and 0x10BE08D0 (VanityCollection), then {4} (entry) on Creature's -- the
        // key FUN_100b8cc0 queries by. Sent even when the table is 0, as the original.
        for (uint32_t field : {4u})
            AscMemoryBridge::Command6A(g_itemAddon.table, field);   // 0x101D9C28
        for (uint32_t field : {4u})
            AscMemoryBridge::Command6A(g_vanity.table, field);      // 0x101D9CC2
        for (uint32_t field : {4u})
            AscMemoryBridge::Command6A(g_creature.table, field);    // 0x101D9D59
        // 0x101D9FB9 -> FUN_10215a50: {4} on SpellAffect's table, while it exists.
        if (g_spellAffect.table)
            for (uint32_t field : {4u})
                AscMemoryBridge::Command6A(g_spellAffect.table, field);
        // 0x101D9FBE -> FUN_10213b80: the quest group fields, while the table exists.
        if (g_quest.table)
            for (uint32_t field : kQuestGroupFields)
                AscMemoryBridge::Command6A(g_quest.table, field);
    }

    void Init()
    {
        g_dbcInit = reinterpret_cast<DbcInit_t>(AscRuntime::Detour(0x634E00, 5, reinterpret_cast<void*>(&DbcInitDetour)));
        sDC.AddPacketHandler(0x56F, CNetClientCustomPacket((void*)&OnPatchQuest, nullptr));
        sDC.AddPacketHandler(0x569, CNetClientCustomPacket((void*)&OnPatchItemAddon, nullptr));
        sDC.AddPacketHandler(0x567, CNetClientCustomPacket((void*)&OnPatchCreature, nullptr));
        sDC.AddPacketHandler(0x574, CNetClientCustomPacket((void*)&OnPatchSpellAffect, nullptr));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}

const AscBridgeStore::CreatureRecord* AscBridgeStore::CreatureByEntry(uint32_t entry)
{
    return LookupByKey(g_creature, entry);
}

const AscItemAddon::Record* AscItemAddon::Find(uint32_t itemId)
{
    return LookupByKey(g_itemAddon, itemId);
}

const uint8_t* AscBridgeStore::QuestById(uint32_t id)
{
    return reinterpret_cast<const uint8_t*>(LookupById(g_quest, id));
}

// FUN_102123e0: the group's records in ascending index order; empty when any query fails.
std::vector<const uint8_t*> AscBridgeStore::QuestsInGroup(uint32_t group)
{
    std::vector<const uint8_t*> out;
    if (!g_quest.table)
        return out;
    std::vector<uint32_t> indexes;
    if (!QuestGroupIndexes(g_quest, group, indexes) || !FetchQuests(g_quest, indexes))
        return out;
    for (uint32_t index : indexes)
        if (const QuestRecord* r = g_quest.cache.Get(index))
            out.push_back(reinterpret_cast<const uint8_t*>(r));
    return out;
}

// FUN_10209860 (with FUN_101e2a30's lookup of the current record): overwrite the quest's record, or
// append a new one at the end of the table. A new id that already exists in the table is refused.
bool AscBridgeStore::PatchQuest(const uint8_t* row)
{
    Store<QuestRecord>& s = g_quest;
    uint32_t id;
    memcpy(&id, row, 4);
    QuestRecord r;
    memcpy(&r, row, sizeof(r));
    return PatchRecord(s, LookupById(s, id), r);
}

uint32_t AscBridgeStore::VanityCount()
{
    return g_vanity.count;   // store object +8, DAT_10BE0488
}

// FUN_1032dc10: {index, found, record copy} for each index, in order; false when the batch fails.
bool AscBridgeStore::VanityRecords(const uint32_t* indexes, uint32_t n, std::vector<VanityEntry>& out)
{
    out.clear();
    std::vector<std::pair<uint32_t, bool>> entries;
    for (uint32_t i = 0; i < n; ++i)
        entries.emplace_back(indexes[i], false);
    if (!FetchBatch(g_vanity, entries))
        return false;
    for (const auto& e : entries)
    {
        VanityEntry v = {};
        v.index = e.first;
        if (e.second)
            if (const VanityRecord* r = g_vanity.cache.Get(e.first))
            {
                v.record = *r;
                v.found = true;
            }
        out.push_back(v);
    }
    return true;
}

// FUN_1032e090: the record at `index` while it still holds `itemId`, else the record for `itemId`
// (FUN_103298f0, 0x6D on field 4).
const AscBridgeStore::VanityRecord* AscBridgeStore::VanityAt(uint32_t index, uint32_t itemId)
{
    Store<VanityRecord>& s = g_vanity;
    if (index < s.count && s.table)
    {
        const VanityRecord* r = s.cache.Get(index);
        if (!r)
        {
            std::vector<std::pair<uint32_t, bool>> one = {{index, false}};
            if (FetchBatch(s, one))
                r = s.cache.Get(index);
        }
        if (r && r->f[1] == itemId)
            return r;
    }
    return LookupByKey(s, itemId);
}

// FUN_101d0500 on the MythicKeystones table.
bool AscBridgeStore::KeystoneById(uint32_t id, uint32_t out[3])
{
    const SmallRecord* r = LookupById(g_keystone, id);
    if (!r)
        return false;
    memcpy(out, r->f, sizeof(r->f));
    return true;
}

// SMSG 0x56D (FUN_101e24d0): a keystone the store already answers for is overwritten in its CACHED record
// only (the table keeps the old row; eviction brings it back). An unknown key is appended through
// FUN_101da590: refused when field 0 already holds the key (0x6B, FUN_101cf640), else 0x73 at the end of
// the table -- which the shipped MMgr64.exe rejects (IMPROVEMENTS.md).
void AscBridgeStore::PatchKeystone(const uint32_t rec[3])
{
    Store<SmallRecord>& s = g_keystone;
    if (SmallRecord* r = const_cast<SmallRecord*>(LookupById(s, rec[0])))
    {
        memcpy(r->f, rec, sizeof(r->f));
        return;
    }
    if (!s.table)
        return;
    std::vector<uint32_t> hits;
    if (AscMemoryBridge::Command6B(s.table, 0, rec[0], hits) && !hits.empty())
        return;
    if (!AscMemoryBridge::Command73(s.table, s.count, rec, 0xC))
        return;
    s.cache.EvictIndex(s.count);
    ++s.count;
    if (rec[0] < s.minId)
        s.minId = rec[0];
    if (rec[0] > s.maxId)
        s.maxId = rec[0];
}

namespace
{
    // FUN_1020d820: 0x6D on the ItemAddon table by item (key field 4, no string fields). The reply is
    // u32 n, then n x {u32 item, u8 found, and when found u32 index (below the count, else the whole read
    // fails) + the 0x40-byte record}. The record's string fields still hold string-block offsets.
    bool ReadItemAddonRecords(const std::vector<uint32_t>& items, std::unordered_map<uint32_t, ItemAddonRecord>& out)
    {
        out.clear();
        if (items.empty())
            return true;
        Store<ItemAddonRecord>& s = g_itemAddon;
        if (!s.table)
            return false;
        std::vector<uint8_t> reply;
        if (!AscMemoryBridge::Command6D(s.table, 4, items, reply, std::vector<uint32_t>()))
            return false;
        size_t pos = 0;
        const uint32_t n = GetU32(reply, pos);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t item = GetU32(reply, pos);
            if (!GetU8(reply, pos))
                continue;
            if (GetU32(reply, pos) >= s.count || reply.size() < pos + sizeof(ItemAddonRecord))
                return false;
            memcpy(&out[item], reply.data() + pos, sizeof(ItemAddonRecord));
            pos += sizeof(ItemAddonRecord);
        }
        return true;
    }

    // FUN_1020e490: 0x6E (CString batch read) over string-block offsets. The reply is u32 n, then n x
    // {u32 offset, u8 found, and when found u32 length + the bytes}.
    bool ReadItemAddonStrings(const std::vector<uint32_t>& offsets, std::unordered_map<uint32_t, std::string>& out)
    {
        out.clear();
        if (offsets.empty())
            return true;
        if (!g_itemAddon.table)
            return false;
        std::vector<uint8_t> reply;
        if (!AscMemoryBridge::Command6E(g_itemAddon.table, offsets, reply))
            return false;
        size_t pos = 0;
        const uint32_t n = GetU32(reply, pos);
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t offset = GetU32(reply, pos);
            if (!GetU8(reply, pos))
                continue;
            const uint32_t len = GetU32(reply, pos);
            if (reply.size() < pos + len)
                throw std::runtime_error("Buffer underflow during deserialization");
            out[offset].assign(reinterpret_cast<const char*>(reply.data() + pos), len);
            pos += len;
        }
        return true;
    }

    uint32_t Offset(const char* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
}

bool AscItemAddon::FetchInfo(const uint32_t* items, uint32_t n, std::unordered_map<uint32_t, Info>& out)
{
    out.clear();   // FUN_101b71c0
    if (n == 0)
        return true;
    std::vector<uint32_t> unique;
    std::unordered_set<uint32_t> seen;
    for (uint32_t i = 0; i < n; ++i)
        if (seen.insert(items[i]).second)
            unique.push_back(items[i]);
    std::unordered_map<uint32_t, ItemAddonRecord> records;
    if (!ReadItemAddonRecords(unique, records))
        return false;
    std::vector<uint32_t> offsets;
    std::unordered_set<uint32_t> seenOffsets;
    for (const auto& r : records)
        for (const char* p : {r.second.name, r.second.description})
            if (seenOffsets.insert(Offset(p)).second)
                offsets.push_back(Offset(p));
    std::unordered_map<uint32_t, std::string> strings;
    if (!ReadItemAddonStrings(offsets, strings))
    {
        out.clear();
        return false;
    }
    for (const auto& r : records)
    {
        Info& info = out[r.first];   // FUN_1020df00
        auto name = strings.find(Offset(r.second.name));
        auto description = strings.find(Offset(r.second.description));
        info.name = name != strings.end() ? name->second : std::string();
        info.description = description != strings.end() ? description->second : std::string();
        info.quality = r.second.fields[0];
        info.flags = r.second.fields[1];
    }
    return true;
}

AscItemAddon::Info AscItemAddon::GetInfo(uint32_t itemId)
{
    std::unordered_map<uint32_t, Info> batch;
    if (FetchInfo(&itemId, 1, batch))
    {
        auto it = batch.find(itemId);
        if (it != batch.end())
            return it->second;
    }
    Info info;
    if (const Record* r = Find(itemId))
    {
        info.name = r->name ? r->name : "";
        info.description = r->description ? r->description : "";
        info.quality = r->fields[0];
        info.flags = r->fields[1];
    }
    return info;
}

// FUN_1032ec60's store part: the record with this id (FUN_10329f60), then FUN_1032eae0.
bool AscBridgeStore::PatchVanity(const VanityRecord& r)
{
    const VanityRecord* found = LookupById(g_vanity, r.f[0]);
    VanityRecord existing;
    if (found)
        existing = *found;
    return PatchRecord(g_vanity, found ? &existing : nullptr, r);
}

// FUN_103234d0: FUN_103238b0 (0x6B on each field of FUN_102158d0's list {4} for `spell`, sorted and
// unique when there is more than one), FUN_10325c20 (0x6F for the uncached ones), then FUN_10324420
// per index (the cache, counted as a hit or a miss). Empty on any failure or without a table.
std::vector<std::array<uint32_t, 3>> AscBridgeStore::SpellAffectsFor(uint32_t spell)
{
    std::vector<std::array<uint32_t, 3>> out;
    if (!g_spellAffect.table)
        return out;
    std::vector<uint32_t> indexes;
    for (uint32_t field : {4u})
    {
        std::vector<uint32_t> hits;
        if (!AscMemoryBridge::Command6B(g_spellAffect.table, field, spell, hits))
            return out;
        indexes.insert(indexes.end(), hits.begin(), hits.end());
    }
    if (indexes.size() > 1)
    {
        std::sort(indexes.begin(), indexes.end());
        indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    }
    std::vector<std::pair<uint32_t, bool>> entries;
    for (uint32_t index : indexes)
        entries.emplace_back(index, false);
    if (!FetchBatch(g_spellAffect, entries))
        return out;
    for (uint32_t index : indexes)
        if (const SmallRecord* r = g_spellAffect.cache.Get(index))
            out.push_back({r->f[0], r->f[1], r->f[2]});
    return out;
}
