// C_BuildCreator, transcribed from the original Extensions.dll.
//
// BuildCreatorEntryContainer (static 0x10BDCDF0, accessor FUN_10101370, ctor FUN_100f7580, reset
// FUN_100fc070):
//   +0x000  u32 category of the last QueryAllBuilds (enum 0x10B1C288)
//   +0x004  vector<CBuildCreatorEntry> queried / bookmarked builds (SMSG 0x631, BookmarkBuild)
//   +0x010  CBuildCreatorEntry pending (the editor's build)
//   +0x148  vector<CBuildCreatorEntry> the category page (SMSG 0x62F)
//   +0x154  FilterableContainerBase<CBuildCreatorEntry> (reset FUN_101016d0; +0x164 filtered)
//   +0x1D0  vector<{string id, u8, u8}> per-spec active builds (SMSG 0x632 / 0x633)
//   +0x1DC  vector<string> owned build ids (SMSG 0x634)     +0x1E8 upvoted ids (SMSG 0x635)
//   +0x1F4  string id of the last RateBuild, +0x20C its up/down flag; +0x210 id of the last DeleteBuild
//
// CBuildCreatorEntry (0x138, ctor FUN_100f7bc0): see Entry below. Wire form FUN_100f9330 (read) /
// FUN_100f9c30 (write); after every read or edit FUN_10107e00 sorts the four lists.
#include <Ascension/AscBuildCreator.hpp>
#include <Ascension/AscAccount.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscFilterContainer.hpp>
#include <Ascension/AscGameEvents.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscMysticEnchant.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/AscSpellRank.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AscScript;

namespace AscBuildCreator
{
namespace
{
#include <Ascension/AscCAEnums.generated.inc>

    const uint32_t kRoleValues[6] = {0, 1, 2, 4, 8, 15};   // 0x10B21FDC

    // The enum lookups' "not found" return is the stack word after their by-value string argument
    // (FUN_100efb90 / FUN_100effa0 return in_stack_0000001c): caller garbage that matches no value.
    const uint32_t kNoValue = 0xFFFFFFFF;

    // ---- the entry ---------------------------------------------------------------------------------
    struct Spell         // 0x28
    {
        uint32_t spell = 0;         // +0x00
        uint32_t level = 0;         // +0x04
        uint8_t core = 0;           // +0x08
        uint8_t optimal = 0;        // +0x09
        uint8_t empowering = 0;     // +0x0A
        uint8_t synergistic = 0;    // +0x0B
        uint32_t flags = 0;         // +0x0C
        std::string comment;        // +0x10
    };

    struct Enchant       // 0x28
    {
        uint32_t id = 0;            // +0x00 (a MysticEnchant spell)
        uint32_t stacks = 0;        // +0x04
        uint32_t level = 0;         // +0x08
        uint32_t flags = 0;         // +0x0C
        std::string comment;        // +0x10
    };

    struct Typed         // 0x1C: a weapon or armour subclass
    {
        uint32_t type = 0;          // +0x00
        std::string comment;        // +0x04
    };

    struct Entry         // 0x138
    {
        std::string id;             // +0x00
        std::string levelingId;     // +0x18
        std::string pveId;          // +0x30
        std::string pvpId;          // +0x48
        uint32_t cls = 0;           // +0x60
        std::string author;         // +0x64
        std::string name;           // +0x7C
        std::string subtext;        // +0x94
        std::string description;    // +0xAC
        std::string icon;           // +0xC4
        uint32_t upvotes = 0;       // +0xDC
        uint64_t created = 0;       // +0xE0
        uint64_t updated = 0;       // +0xE8
        uint32_t category = 0;      // +0xF0
        uint32_t roles = 8;         // +0xF4
        uint32_t primaryStat = 0;   // +0xF8
        std::vector<Spell> spells;          // +0xFC
        std::vector<Enchant> enchants;      // +0x108
        std::vector<Typed> weapons;         // +0x114
        std::vector<Typed> armor;           // +0x120
        uint8_t needsRepairs = 0;   // +0x12C
        uint32_t flags = 0;         // +0x130
        uint32_t difficulty = 0;    // +0x134
    };

    struct SpecRecord    // 0x1C
    {
        std::string id;
        uint8_t a = 0, b = 0;       // +0x18 / +0x19
    };

    struct Container
    {
        uint32_t category = 0;
        std::vector<Entry> queried;
        Entry pending;
        std::vector<Entry> page;
        std::vector<SpecRecord> specs;
        std::vector<std::string> owned, upvoted;
        std::string ratedId;
        bool rateUp = false;
        std::string deletedId;
    } g;

    // BuildCreatorEntryContainer's filter (vtable 0x10B2E6A4 over the base 0x10B2E674): the category
    // page, text in name / description / author, argument groups and sort selections from UpdateFilter.
    class BuildFilter : public AscFilter::Container<const Entry*>
    {
    protected:
        std::vector<const Entry*> DoPopulate() override   // [0] FUN_10107ef0
        {
            std::vector<const Entry*> v;
            for (const Entry& e : g.page)
                v.push_back(&e);
            return v;
        }

        bool TextMatch(const std::string& lower, const Entry* const& e) override   // [3] FUN_101081f0
        {
            if (lower.empty())
                return true;
            return AscFilter::Lower(e->name).find(lower) != std::string::npos
                || AscFilter::Lower(e->description).find(lower) != std::string::npos
                || AscFilter::Lower(e->author).find(lower) != std::string::npos;
        }

        uint32_t ArgGroup(uint32_t a) override   // [4] FUN_101087a0 (jump table 0x1010880C)
        {
            if (a == 1) return 1;
            if (a >= 2 && a <= 6) return 2;
            if (a == 7) return 3;
            if (a >= 8 && a <= 0xB) return 4;
            if (a >= 0xC && a <= 0xE) return 5;
            if (a >= 0x10 && a <= 0x2F) return 6;
            return 0;
        }

        bool ArgMatch(uint32_t a, const Entry* const& e) override;   // [2] FUN_101083b0
        std::string SortKey(uint32_t s, const Entry* const& e) override;   // [9] FUN_10108a00
    } g_filter;

    // ---- small helpers -----------------------------------------------------------------------------
    template <class T> T Read(CDataStore* p)
    {
        T v;
        memcpy(&v, p->m_buffer + p->m_read, sizeof(T));
        p->m_read += sizeof(T);
        return v;
    }

    std::string ReadCString(CDataStore* p)
    {
        const char* s = reinterpret_cast<const char*>(p->m_buffer + p->m_read);
        std::string out(s);
        p->m_read += static_cast<uint32_t>(out.size()) + 1;
        return out;
    }

    // Unit type 3/4: UNIT_FIELD_BYTES_0 byte 1 (descriptor +0x5C), else 0.
    uint32_t PlayerClass()
    {
        const uint8_t* unit = ActivePlayer();
        if (!unit)
            return 0;
        const uint32_t type = *reinterpret_cast<const uint32_t*>(unit + 0x14);
        if (type != 3 && type != 4)
            return 0;
        return (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(unit + 8) + 0x5C) >> 8) & 0xFF;
    }

    bool StockClass(uint32_t c) { return (c >= 1 && c <= 9) || c == 11; }   // FUN_100c6380

    std::string Unexpected(uint32_t v) { return "UNEXPECTED_ENUM_VALUE_" + std::to_string(v); }

    template <size_t N>
    std::string EnumName(const char* const (&table)[N], uint32_t v)
    {
        return v < N ? std::string(table[v]) : Unexpected(v);
    }

    template <size_t N>
    bool EnumIndex(const char* const (&table)[N], const std::string& s, uint32_t& out)
    {
        for (uint32_t i = 0; i < N; ++i)
            if (s == table[i])
            {
                out = i;
                return true;
            }
        return false;
    }

    // FUN_100efb90 and its siblings: name -> value, logging "Unexpected Value: {}" otherwise.
    template <size_t N>
    uint32_t EnumOrLog(const char* const (&table)[N], const std::string& s, uint32_t fallback)
    {
        uint32_t v = 0;
        if (EnumIndex(table, s, v))
            return v;
        AscLog::Printf("Unexpected Value: %s", s.c_str());
        return fallback;
    }

    // FUN_100fe540 & co: FNV-hashed id -> MysticEnchant.dbc row (map 0x10BE04C8, keyed by the row's
    // +4 spell). FUN_102e65d0: the RE_QUALITY_* at +0x10 for a stock-class player, else +0xC.
    AscDbc::Table& MysticEnchants() { return AscDbc::Get("DBFilesClient\\MysticEnchant.dbc"); }

    const uint8_t* EnchantRecord(uint32_t spell)
    {
        static std::unordered_map<uint32_t, const uint8_t*> map;
        static bool built = false;
        if (!built)
        {
            built = true;
            AscDbc::Table& t = MysticEnchants();
            for (uint32_t i = 0; i < t.Count(); ++i)
                if (const uint8_t* r = t.RowAt(i))
                    map.emplace(AscDbc::Table::U32(r, 4), r);
        }
        auto it = map.find(spell);
        return it == map.end() ? nullptr : it->second;
    }

    uint32_t EnchantQuality(const uint8_t* rec)
    {
        const char* s = MysticEnchants().Str(rec, ActivePlayer() && StockClass(PlayerClass()) ? 0x10 : 0xC);
        uint32_t q = 0;
        if (!EnumIndex(kReQualities, s, q))
            AscLog::Printf("Unexpected %s", s);   // FUN_102df780
        return q;
    }

    // FUN_10107e00: spells by level (FUN_100eefc0), enchants by level then quality (FUN_100eec40),
    // weapon and armour types by type (FUN_100ee930). The original's std::sort, not a stable sort.
    void SortEntry(Entry& e)
    {
        std::sort(e.spells.begin(), e.spells.end(),
                  [](const Spell& a, const Spell& b) { return a.level < b.level; });
        std::sort(e.enchants.begin(), e.enchants.end(), [](const Enchant& a, const Enchant& b) {
            if (a.level != b.level)
                return a.level < b.level;
            const uint8_t* ra = EnchantRecord(a.id);
            const uint8_t* rb = EnchantRecord(b.id);
            if (!ra || !rb)
                return false;
            return EnchantQuality(ra) < EnchantQuality(rb);
        });
        auto byType = [](const Typed& a, const Typed& b) { return a.type < b.type; };
        std::sort(e.weapons.begin(), e.weapons.end(), byType);
        std::sort(e.armor.begin(), e.armor.end(), byType);
    }

    // FUN_100f7bc0 + the class the editor stamps on a fresh build (EditBuild / DiscardPendingBuild).
    void ResetPending()
    {
        g.pending = Entry();
        g.pending.cls = PlayerClass();
    }

    // FUN_100fd750: by id in the category page, then in the queried list.
    Entry* FindBuild(const std::string& id)
    {
        for (Entry& e : g.page)
            if (e.id == id)
                return &e;
        for (Entry& e : g.queried)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    bool Contains(const std::vector<std::string>& v, const std::string& s)
    {
        return std::find(v.begin(), v.end(), s) != v.end();
    }

    void Erase(std::vector<std::string>& v, const std::string& s)
    {
        auto it = std::find(v.begin(), v.end(), s);
        if (it != v.end())
            v.erase(it);
    }

    // FUN_101016d0 on +0x154: drop the filter's lists so the next UpdateFilter repopulates.
    void ResetFilter() { g_filter.Reset(); }

    AscCA::BuildSeed SeedOf(const Entry& e)
    {
        AscCA::BuildSeed s;
        s.classKey = e.cls;
        s.category = e.category;
        s.primaryStat = e.primaryStat;
        s.id = e.id;
        for (const Spell& sp : e.spells)
            s.spells.emplace_back(sp.spell, sp.level);
        return s;
    }

    // ---- wire form ---------------------------------------------------------------------------------
    Entry ReadEntry(CDataStore* p)   // FUN_100f9330
    {
        Entry e;
        e.id = ReadCString(p);
        e.levelingId = ReadCString(p);
        e.pveId = ReadCString(p);
        e.pvpId = ReadCString(p);
        e.cls = Read<uint32_t>(p);
        e.author = ReadCString(p);
        e.name = ReadCString(p);
        e.subtext = ReadCString(p);
        e.description = ReadCString(p);
        e.icon = ReadCString(p);
        e.upvotes = Read<uint32_t>(p);
        e.created = Read<uint64_t>(p);
        e.updated = Read<uint64_t>(p);
        e.category = Read<uint32_t>(p);
        e.roles = Read<uint32_t>(p);
        e.primaryStat = Read<uint32_t>(p);
        for (uint32_t n = Read<uint32_t>(p), i = 0; i < n; ++i)   // FUN_100ea6d0 / FUN_100f6180
        {
            Spell s;
            s.spell = Read<uint32_t>(p);
            s.level = Read<uint32_t>(p);
            s.core = Read<uint8_t>(p);
            s.optimal = Read<uint8_t>(p);
            s.empowering = Read<uint8_t>(p);
            s.synergistic = Read<uint8_t>(p);
            s.flags = Read<uint32_t>(p);
            s.comment = ReadCString(p);
            e.spells.push_back(s);
        }
        for (uint32_t n = Read<uint32_t>(p), i = 0; i < n; ++i)   // FUN_100ea200
        {
            Enchant c;
            c.id = Read<uint32_t>(p);
            c.stacks = Read<uint32_t>(p);
            c.level = Read<uint32_t>(p);
            c.flags = Read<uint32_t>(p);
            c.comment = ReadCString(p);
            e.enchants.push_back(c);
        }
        for (std::vector<Typed>* list : {&e.weapons, &e.armor})   // FUN_100eab00 / FUN_100e9d50
            for (uint32_t n = Read<uint32_t>(p), i = 0; i < n; ++i)
            {
                Typed t;
                t.type = Read<uint32_t>(p);
                t.comment = ReadCString(p);
                list->push_back(t);
            }
        e.needsRepairs = Read<uint8_t>(p);
        e.flags = Read<uint32_t>(p);
        e.difficulty = Read<uint32_t>(p);
        return e;
    }

    void WriteEntry(Packet& pk, const Entry& e)   // FUN_100f9c30
    {
        pk.Str(e.id.c_str()).Str(e.levelingId.c_str()).Str(e.pveId.c_str()).Str(e.pvpId.c_str());
        pk.U32(e.cls);
        pk.Str(e.author.c_str()).Str(e.name.c_str()).Str(e.subtext.c_str()).Str(e.description.c_str());
        pk.Str(e.icon.c_str());
        pk.U32(e.upvotes).U64(e.created).U64(e.updated).U32(e.category).U32(e.roles).U32(e.primaryStat);
        pk.U32(static_cast<uint32_t>(e.spells.size()));   // FUN_100eb3c0
        for (const Spell& s : e.spells)
            pk.U32(s.spell).U32(s.level).U8(s.core).U8(s.optimal).U8(s.empowering).U8(s.synergistic)
                .U32(s.flags).Str(s.comment.c_str());
        pk.U32(static_cast<uint32_t>(e.enchants.size()));   // FUN_100eb1f0
        for (const Enchant& c : e.enchants)
            pk.U32(c.id).U32(c.stacks).U32(c.level).U32(c.flags).Str(c.comment.c_str());
        for (const std::vector<Typed>* list : {&e.weapons, &e.armor})   // FUN_100eb070
        {
            pk.U32(static_cast<uint32_t>(list->size()));
            for (const Typed& t : *list)
                pk.U32(t.type).Str(t.comment.c_str());
        }
        pk.U8(e.needsRepairs).U32(e.flags).U32(e.difficulty);
    }


    // ---- build links (ExportBuild / ImportBuild) ----------------------------------------------------
    // The original compresses with Boost.Beast's zlib port (raw deflate, level 6, window 15, memLevel
    // 8) and inflates with its inflate_stream. The client's own zlib 1.2.2 runs the same algorithm, so
    // the link is built with it: deflateInit2_ / deflate / deflateEnd and inflateInit2_ / inflate /
    // inflateEnd, the functions compress2 (0x866680) and uncompress (0x866770) call.
    struct ZStream   // z_stream, 0x38
    {
        const uint8_t* next_in;
        uint32_t avail_in, total_in;
        uint8_t* next_out;
        uint32_t avail_out, total_out;
        const char* msg;
        void* state;
        void* zalloc;
        void* zfree;
        void* opaque;
        int data_type;
        uint32_t adler, reserved;
    };
    static_assert(sizeof(ZStream) == 0x38, "z_stream");
    const char* const kZlibVersion = reinterpret_cast<const char*>(0xA3B558);   // "1.2.2"
    typedef int(__cdecl* DeflateInit2Fn)(ZStream*, int, int, int, int, int, const char*, int);
    typedef int(__cdecl* InflateInit2Fn)(ZStream*, int, const char*, int);
    typedef int(__cdecl* ZFlushFn)(ZStream*, int);
    typedef int(__cdecl* ZEndFn)(ZStream*);
    const DeflateInit2Fn deflateInit2_ = reinterpret_cast<DeflateInit2Fn>(0x864DC0);
    const ZFlushFn deflate_ = reinterpret_cast<ZFlushFn>(0x863A40);
    const ZEndFn deflateEnd_ = reinterpret_cast<ZEndFn>(0x863E50);
    const InflateInit2Fn inflateInit2_ = reinterpret_cast<InflateInit2Fn>(0x865080);
    const ZFlushFn inflate_ = reinterpret_cast<ZFlushFn>(0x865270);
    const ZEndFn inflateEnd_ = reinterpret_cast<ZEndFn>(0x866660);

    // FUN_101094a0, first half: one Z_FINISH pass into a buffer the size of the input; whatever does
    // not fit is dropped (only tiny inputs come out larger than they went in).
    std::string Deflate(const std::string& in)
    {
        if (in.empty())
            return std::string();
        std::string out(in.size(), '\0');
        ZStream z = {};
        z.next_in = reinterpret_cast<const uint8_t*>(in.data());
        z.avail_in = static_cast<uint32_t>(in.size());
        z.next_out = reinterpret_cast<uint8_t*>(&out[0]);
        z.avail_out = static_cast<uint32_t>(out.size());
        if (deflateInit2_(&z, 6, 8, -15, 8, 0, kZlibVersion, sizeof(ZStream)) != 0)
            return std::string();
        deflate_(&z, 4);
        out.resize(out.size() - z.avail_out);
        deflateEnd_(&z);
        return out;
    }

    // FUN_10109ba0, second half: into ten times the input; a stream error yields "".
    std::string Inflate(const std::string& in)
    {
        if (in.empty())
            return std::string();
        std::string out(in.size() * 10, '\0');
        ZStream z = {};
        z.next_in = reinterpret_cast<const uint8_t*>(in.data());
        z.avail_in = static_cast<uint32_t>(in.size());
        z.next_out = reinterpret_cast<uint8_t*>(&out[0]);
        z.avail_out = static_cast<uint32_t>(out.size());
        if (inflateInit2_(&z, -15, kZlibVersion, sizeof(ZStream)) != 0)
            return std::string();
        const int r = inflate_(&z, 2);   // Z_SYNC_FLUSH
        inflateEnd_(&z);
        if (r != 0 && r != 1 && r != -5)   // Z_OK, Z_STREAM_END, Z_BUF_ERROR
            return std::string();
        out.resize(out.size() - z.avail_out);
        return out;
    }

    const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string Base64Encode(const std::string& in)   // FUN_101094a0, second half
    {
        std::string out;
        const uint8_t* p = reinterpret_cast<const uint8_t*>(in.data());
        size_t n = in.size();
        for (; n >= 3; n -= 3, p += 3)
        {
            out += kBase64[p[0] >> 2];
            out += kBase64[(p[0] & 3) * 0x10 + (p[1] >> 4)];
            out += kBase64[(p[1] & 0xF) * 4 + (p[2] >> 6)];
            out += kBase64[p[2] & 0x3F];
        }
        if (n == 1)
        {
            out += kBase64[p[0] >> 2];
            out += kBase64[(p[0] & 3) * 0x10];
            out += "==";
        }
        else if (n == 2)
        {
            out += kBase64[p[0] >> 2];
            out += kBase64[(p[0] & 3) * 0x10 + (p[1] >> 4)];
            out += kBase64[(p[1] & 0xF) * 4];
            out += '=';
        }
        return out;
    }

    // FUN_10109ba0, first half (table 0x10B2E518): stops at '=' or the first byte outside the alphabet;
    // a trailing partial group yields its whole bytes.
    std::string Base64Decode(const std::string& in)
    {
        auto value = [](uint8_t c) -> int {
            const char* f = strchr(kBase64, c);
            return (c && f) ? static_cast<int>(f - kBase64) : -1;
        };
        std::string out;
        uint8_t q[4] = {};
        int k = 0;
        for (uint8_t c : in)
        {
            if (c == '=' || value(c) < 0)
                break;
            q[k++] = static_cast<uint8_t>(value(c));
            if (k == 4)
            {
                out += static_cast<char>((q[0] << 2) | ((q[1] >> 4) & 3));
                out += static_cast<char>((q[1] << 4) | ((q[2] >> 2) & 0xF));
                out += static_cast<char>((q[2] << 6) + q[3]);
                k = 0;
            }
        }
        if (k)
        {
            for (int i = k; i < 4; ++i)
                q[i] = 0;
            const char b[3] = {static_cast<char>((q[0] << 2) | ((q[1] >> 4) & 3)),
                               static_cast<char>((q[1] << 4) | ((q[2] >> 2) & 0xF)),
                               static_cast<char>((q[2] << 6) + q[3])};
            out.append(b, k - 1);
        }
        return out;
    }

    // FUN_1010faf0: RFC 3986 unreserved characters pass, everything else becomes %XX (uppercase hex).
    std::string UrlEncode(const std::string& in)
    {
        static const char hex[] = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : in)
            if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~')
                out += static_cast<char>(c);
            else
            {
                out += '%';
                out += hex[c >> 4];
                out += hex[c & 0xF];
            }
        return out;
    }

    // FUN_1010f5e0: %XX (read with istringstream >> hex) when two characters follow; '+' stays.
    std::string UrlDecodeImpl(const std::string& in)
    {
        std::string out;
        for (size_t i = 0; i < in.size(); ++i)
            if (in[i] == '%' && i + 2 < in.size())
            {
                std::istringstream is(in.substr(i + 1, 2));
                int v = 0;
                is >> std::hex >> v;
                out += static_cast<char>(v);
                i += 2;
            }
            else
                out += in[i];
        return out;
    }

    uint32_t RankSpell(AscCA::Row r, uint32_t rank)   // FUN_101c62e0: ranks 1..9 at +0x14
    {
        return (rank && rank - 1 < 9) ? AscCA::RowU32(r, 0x14 + (rank - 1) * 4) : 0;
    }

    bool ClassAllowed(AscCA::Row r)   // FUN_101c6af0
    {
        const uint8_t* player = ActivePlayer();
        return player && AscCA::ClassTypeAdmits(r, AscCA::UnitClassOf(player));
    }

    void UpsertSpell(Entry& e, const Spell& s)   // FUN_100fa550
    {
        auto it = std::find_if(e.spells.begin(), e.spells.end(), [&](const Spell& o) { return o.spell == s.spell; });
        if (it == e.spells.end())
            e.spells.push_back(s);
        else
            *it = s;
        SortEntry(e);
    }

    void UpsertEnchant(Entry& e, const Enchant& c)   // FUN_100fa430
    {
        auto it = std::find_if(e.enchants.begin(), e.enchants.end(), [&](const Enchant& o) { return o.id == c.id; });
        if (it == e.enchants.end())
            e.enchants.push_back(c);
        else
            *it = c;
        SortEntry(e);
    }

    // FUN_100fd8f0: ":<first rank>" per ability chain and ":<row>t<rank>" per talent / trait /
    // talent-ability chain (highest rank held), "::", then ":<enchant>e<stacks>" each, then ":".
    // Chains are visited in MSVC unordered_map order, which this std::unordered_map reproduces.
    std::string ExportLink(const Entry& e)
    {
        std::unordered_map<uint32_t, uint32_t> chains;
        for (const Spell& s : e.spells)
        {
            const uint32_t first = AscSpellRank::FirstRank(s.spell);
            auto it = chains.find(first);
            if (it == chains.end())
                chains.emplace(first, AscSpellRank::RankNumber(s.spell));
            else
                it->second = std::max(it->second, AscSpellRank::RankNumber(s.spell));
        }
        std::ostringstream os;
        for (const auto& kv : chains)
            if (AscCA::Row r = AscCA::RowBySpell(kv.first))
            {
                const int t = AscCA::RowType(r);
                if (t == 1 || t == 2 || t == 3 || t == 4)
                {
                    os << ':';
                    if (t == 1)
                        os << kv.first;
                    else
                        os << AscCA::RowU32(r, 0) << 't' << kv.second;
                }
            }
        os << ':';
        for (const Enchant& c : e.enchants)
            os << ':' << c.id << 'e' << c.stacks;
        os << ':';
        return UrlEncode(Base64Encode(Deflate(os.str())));
    }

    void ImportLog(const char* what, uint32_t v)
    {
        AscLog::Printf("BuildCreatorEntry::ImportBuildURL: %s: %u", what, v);
    }

    // FUN_101001e0 (+ FUN_10100e30): replaces the entry's spells and random enchants with the link's.
    // A malformed number throws out of the original's std::stoul; here it ends the import likewise.
    void ImportLink(Entry& e, const std::string& url)
    {
        e.spells.clear();
        e.enchants.clear();
        const std::string text = Inflate(Base64Decode(UrlDecodeImpl(url)));
        std::istringstream is(text);
        std::string tok;
        try
        {
            while (std::getline(is, tok, ':'))
            {
                if (tok.empty())
                    continue;
                const size_t t = tok.find('t');
                const size_t ep = tok.find('e');
                if (t != std::string::npos)
                {
                    const uint32_t rowId = static_cast<uint32_t>(std::stoul(tok.substr(0, t)));
                    const uint32_t rank = static_cast<uint32_t>(std::stoul(tok.substr(t + 1)));
                    const AscCA::Row r = AscCA::FindRow(rowId);
                    if (!r)
                        continue;
                    if (!AscCA::RowVisible(r))
                        ImportLog("Attempted to add talent from incorrect realm", rowId);
                    else if (!ClassAllowed(r))
                        ImportLog("Attempted to add talent from incorrect class", rowId);
                    else if (AscCA::RowType(r) != 2 && AscCA::RowType(r) != 4)
                        ImportLog("Attempted to add talent that is not a talent", rowId);
                    else
                        for (uint32_t k = 0; k < rank; ++k)
                        {
                            Spell s;
                            s.spell = RankSpell(r, k + 1);
                            s.level = AscGameEvents::ServerMaxLevel();
                            UpsertSpell(e, s);
                        }
                }
                else if (ep == std::string::npos)
                {
                    const uint32_t spell = static_cast<uint32_t>(std::stoul(tok));
                    const AscCA::Row r = AscCA::RowBySpell(spell);
                    if (!r)
                        continue;
                    if (!AscCA::RowVisible(r))
                        ImportLog("Attempted to add spell from incorrect realm", spell);
                    else if (!ClassAllowed(r))
                        ImportLog("Attempted to add spell from incorrect class", spell);
                    else if (AscCA::RowType(r) != 1)
                        ImportLog("Attempted to add spell that is not an ability", spell);
                    else
                    {
                        Spell s;
                        s.spell = spell;
                        s.level = AscGameEvents::ServerMaxLevel();
                        UpsertSpell(e, s);
                    }
                }
                else
                {
                    const uint32_t id = static_cast<uint32_t>(std::stoul(tok.substr(0, ep)));
                    const uint32_t stacks = static_cast<uint32_t>(std::stoul(tok.substr(ep + 1)));
                    const uint8_t* rec = EnchantRecord(id);
                    if (!rec)
                        continue;
                    // Realm flags +0x20..+0x30 against the realm object's +0x40..+0x44.
                    const uint8_t* gates = RealmInfoSvc::Get().gates;
                    bool ok = false;
                    for (uint32_t i = 0; i < 5 && !ok; ++i)
                        ok = AscDbc::Table::U32(rec, 0x20 + i * 4) && gates[i];
                    if (!ok)
                    {
                        ImportLog("Attempted to add enchantment from incorrect realm", id);
                        continue;
                    }
                    Enchant c;
                    c.id = id;
                    c.stacks = stacks;
                    c.level = AscGameEvents::ServerMaxLevel();
                    UpsertEnchant(e, c);
                }
            }
        }
        catch (const std::exception& ex)
        {
            AscLog::Printf("BuildCreatorEntry::ImportBuildURL: %s", ex.what());
        }
    }

    // FUN_100fc240: an entry holding every rank of the build's entries and, when asked, the equipped
    // mystic enchants counted into stacks.
    Entry EntryFromBuild(const AscCA::Build& b, bool enchants)
    {
        Entry e;
        for (const AscCA::Entry& ce : b.entries)
            if (AscCA::Row r = AscCA::FindRow(ce.id))
                for (uint32_t k = 0; k < ce.rank; ++k)
                {
                    Spell s;
                    s.spell = RankSpell(r, k + 1);
                    s.level = AscGameEvents::ServerMaxLevel();
                    UpsertSpell(e, s);
                }
        if (!enchants)
            return e;
        // FUN_102e64e0 counts the non-empty slots of 17, then slots 0..count-1 are read.
        uint32_t count = 0;
        for (uint32_t i = 0; i < 0x11; ++i)
            count += AscMysticEnchant::SlotEnchant(i) != 0;
        std::unordered_map<uint32_t, uint32_t> stacks;
        for (uint32_t i = 0; i < count; ++i)
            if (const uint32_t spell = AscMysticEnchant::SlotEnchant(i))
                ++stacks[spell];
        for (const auto& kv : stacks)
        {
            Enchant c;
            c.id = kv.first;
            c.stacks = kv.second;
            c.level = AscGameEvents::ServerMaxLevel();
            UpsertEnchant(e, c);
        }
        return e;
    }

    // ---- Lua serialisers ---------------------------------------------------------------------------
    void SetStr(lua_State* L, const char* k, const std::string& v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushstring(L, v.c_str());
        AscLua::lua_settable(L, -3);
    }
    void SetInt(lua_State* L, const char* k, int32_t v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushinteger(L, v);
        AscLua::lua_settable(L, -3);
    }
    void SetBool(lua_State* L, const char* k, bool v)
    {
        AscLua::lua_pushstring(L, k);
        AscLua::lua_pushboolean(L, v ? 1 : 0);
        AscLua::lua_settable(L, -3);
    }

    void PushSpell(lua_State* L, const Spell& s)   // FUN_1009d6f0
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetInt(L, "Spell", static_cast<int32_t>(s.spell));
        SetInt(L, "Level", static_cast<int32_t>(s.level));
        SetBool(L, "IsCoreAbility", s.core != 0);
        SetBool(L, "IsOptimalAbility", s.optimal != 0);
        SetBool(L, "IsEmpoweringAbility", s.empowering != 0);
        SetBool(L, "IsSynergisticAbility", s.synergistic != 0);
        SetInt(L, "Flags", static_cast<int32_t>(s.flags));
        SetStr(L, "Comment", s.comment);
    }

    template <class T, class Fn>
    void PushList(lua_State* L, const std::vector<T>& v, Fn element)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(v.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < v.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            element(v[i]);
            AscLua::lua_settable(L, -3);
        }
    }

    // FUN_1009c1f0 (the entry passed by value).
    void PushEntry(lua_State* L, const Entry& e)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        SetStr(L, "ID", e.id);
        SetStr(L, "LevelingID", e.levelingId);
        SetStr(L, "EndGameIDPVE", e.pveId);
        SetStr(L, "EndGameIDPVP", e.pvpId);
        if (e.author.empty())
        {
            uint8_t* player = ActivePlayer();
            const char* n = "Unknown";
            if (player)
            {
                // CGObject vtable +0xD8: the object's name.
                auto fn = *reinterpret_cast<const char*(__thiscall**)(void*)>(*reinterpret_cast<uint8_t**>(player) + 0xD8);
                n = fn(player);
                if (!n)
                    n = "";
            }
            SetStr(L, "AuthorName", n);
        }
        else
            SetStr(L, "AuthorName", e.author);
        SetStr(L, "Name", e.name);
        SetStr(L, "Subtext", e.subtext);
        SetStr(L, "Description", e.description);
        SetStr(L, "Icon", e.icon);
        SetInt(L, "CreatedTime", static_cast<int32_t>(e.created));
        SetInt(L, "UpdatedTime", static_cast<int32_t>(e.updated));
        SetStr(L, "Category", EnumName(kBcCategories, e.category));
        std::string role = Unexpected(e.roles);
        for (int i = 0; i < 6; ++i)
            if (kRoleValues[i] == e.roles)
            {
                role = kPlayerRoles[i];
                break;
            }
        SetStr(L, "Roles", role);
        SetStr(L, "PrimaryStat", EnumName(kStats, e.primaryStat));   // FUN_10099f80

        AscLua::lua_pushstring(L, "Spells");
        PushList(L, e.spells, [&](const Spell& s) { PushSpell(L, s); });
        AscLua::lua_settable(L, -3);

        AscLua::lua_pushstring(L, "RandomEnchants");   // FUN_1009a680
        PushList(L, e.enchants, [&](const Enchant& c) {
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetInt(L, "Enchant", static_cast<int32_t>(c.id));
            SetInt(L, "Stacks", static_cast<int32_t>(c.stacks));
            SetInt(L, "Level", static_cast<int32_t>(c.level));
            SetInt(L, "Flags", static_cast<int32_t>(c.flags));
            SetStr(L, "Comment", c.comment);
        });
        AscLua::lua_settable(L, -3);

        AscLua::lua_pushstring(L, "WeaponTypes");   // FUN_1009a8f0 / FUN_1009d890
        PushList(L, e.weapons, [&](const Typed& t) {
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetStr(L, "Type", EnumName(kWeaponTypes, t.type));
            SetStr(L, "Comment", t.comment);
        });
        AscLua::lua_settable(L, -3);

        AscLua::lua_pushstring(L, "ArmorTypes");   // FUN_1009a500 / FUN_1009c030
        PushList(L, e.armor, [&](const Typed& t) {
            AscLua::lua_createtable(L, 0, 0);
            AscLua::lua_checkstack(L, 2);
            SetStr(L, "Type", EnumName(kArmorTypes, t.type));
            SetStr(L, "Comment", t.comment);
        });
        AscLua::lua_settable(L, -3);

        SetBool(L, "NeedsRepairs", e.needsRepairs != 0);
        SetInt(L, "Flags", static_cast<int32_t>(e.flags));
        SetInt(L, "Upvotes", static_cast<int32_t>(e.upvotes));
        SetStr(L, "DifficultyRating", e.difficulty <= 4 ? std::string(kDifficulty[e.difficulty]) : Unexpected(e.difficulty));

        // The first random enchant whose MysticEnchant record is legendary (quality 5): its spell.
        AscLua::lua_pushstring(L, "LegendaryEnchant");
        bool found = false;
        for (const Enchant& c : e.enchants)
            if (const uint8_t* rec = EnchantRecord(c.id))
                if (EnchantQuality(rec) == 5)
                {
                    AscLua::lua_pushnumber(L, static_cast<double>(static_cast<int32_t>(AscDbc::Table::U32(rec, 4))));
                    found = true;
                    break;
                }
        if (!found)
            AscLua::lua_pushnil(L);
        AscLua::lua_settable(L, -3);

        // Essence and costs come from a build of the PENDING entry (FUN_100fde60 is container +0x10),
        // whichever entry is being serialised.
        AscCA::Build b(SeedOf(g.pending), AscGameEvents::ServerMaxLevel(), false);
        SetInt(L, "AbilityEssence", static_cast<int32_t>(b.GlobalAE(0)));
        SetInt(L, "TalentEssence", static_cast<int32_t>(b.GlobalTE(0)));
        SetInt(L, "CommonCost", static_cast<int32_t>(AscCA::QualitySumOf(b, 1)));
        SetInt(L, "UncommonCost", static_cast<int32_t>(AscCA::QualitySumOf(b, 2)));
        SetInt(L, "RareCost", static_cast<int32_t>(AscCA::QualitySumOf(b, 3)));
        SetInt(L, "EpicCost", static_cast<int32_t>(AscCA::QualitySumOf(b, 4)));
        SetInt(L, "LegendaryCost", static_cast<int32_t>(AscCA::QualitySumOf(b, 5)));

        // Rows by the first flag each spell carries (core > optimal > empowering > synergistic). The
        // original indexes CharacterAdvancement.dbc by the spell value itself here (DAT_10bdf65c).
        std::unordered_set<AscCA::Row> sets[4];
        for (const Spell& s : e.spells)
            if (AscCA::Row r = AscCA::FindRow(s.spell))
            {
                if (s.core)
                    sets[0].insert(r);
                else if (s.optimal)
                    sets[1].insert(r);
                else if (s.empowering)
                    sets[2].insert(r);
                else if (s.synergistic)
                    sets[3].insert(r);
            }
        AscCA::Build* active = AscCA::ActiveBuild();
        int32_t abilities[4] = {}, knownAbilities[4] = {}, talents[4] = {}, knownTalents[4] = {};
        for (int i = 0; i < 4; ++i)
            for (AscCA::Row r : sets[i])
            {
                const int t = AscCA::RowType(r);
                const bool known = active->Has(AscCA::RowU32(r, 0));
                if (t == 1 || t == 4)   // FUN_101c6770
                {
                    ++abilities[i];
                    knownAbilities[i] += known;
                }
                if (t == 2)   // FUN_101c6e80
                {
                    ++talents[i];
                    knownTalents[i] += known;
                }
            }
        const char* kinds[4] = {"Core", "Optimal", "Empowering", "Synergistic"};
        for (int i = 0; i < 4; ++i)
        {
            const std::string k = kinds[i];
            SetInt(L, ("Num" + k + "Spells").c_str(), abilities[i] + talents[i]);
            SetInt(L, ("NumKnown" + k + "Spells").c_str(), knownAbilities[i] + knownTalents[i]);
            SetInt(L, ("Num" + k + "Abilities").c_str(), abilities[i]);
            SetInt(L, ("NumKnown" + k + "Abilities").c_str(), knownAbilities[i]);
            SetInt(L, ("Num" + k + "Talents").c_str(), talents[i]);
            SetInt(L, ("NumKnown" + k + "Talents").c_str(), knownTalents[i]);
        }
    }

    // FUN_100f1660 & siblings: { [1] = NAME, ... }; FUN_100f2c20 & siblings: the first NAME alone.
    template <size_t N>
    void PushErrorList(lua_State* L, const char* const (&table)[N], const std::vector<uint32_t>& errors)
    {
        AscLua::lua_createtable(L, 0, static_cast<int>(errors.size()));
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < errors.size(); ++i)
        {
            AscLua::lua_pushnumber(L, static_cast<double>(i + 1));
            PushStr(L, EnumName(table, errors[i]).c_str());
            AscLua::lua_settable(L, -3);
        }
    }

    // The Can* shape: (ok, nil) or (false, {names}).
    template <size_t N>
    int PushCan(lua_State* L, const char* const (&table)[N], const std::vector<uint32_t>& errors)
    {
        PushBool(L, errors.empty());
        if (errors.empty())
            AscLua::lua_pushnil(L);
        else
            PushErrorList(L, table, errors);
        return 2;
    }

    // The mutator shape: (false, NAME) on the first error, else (true, nil).
    template <size_t N>
    int PushDone(lua_State* L, const char* const (&table)[N], const std::vector<uint32_t>& errors)
    {
        if (!errors.empty())
        {
            PushBool(L, false);
            PushStr(L, EnumName(table, errors[0]).c_str());
            return 2;
        }
        PushBool(L, true);
        AscLua::lua_pushnil(L);
        return 2;
    }

    int PushOk(lua_State* L)
    {
        PushBool(L, true);
        AscLua::lua_pushnil(L);
        return 2;
    }

    // ---- Lua table arguments (FrameScript::lua_getmapvalue, FUN_100f05c0 / FUN_100f1310) -----------
    void MissingMember(const char* type, const char* key)
    {
        AscLog::Printf("lua_tovalue<%s>: Missing member '%s' in table at index %d", type, key, 1);
    }

    void FieldU32(lua_State* L, const char* type, const char* key, uint32_t& out)
    {
        AscLua::lua_getfield(L, 1, key);
        if (AscLua::lua_type(L, -1) == NUMBER)
            out = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, -1)));
        else
            MissingMember(type, key);
        AscLua::lua_settop(L, -2);
    }

    void FieldBool(lua_State* L, const char* type, const char* key, uint8_t& out)
    {
        AscLua::lua_getfield(L, 1, key);
        if (AscLua::lua_type(L, -1) == BOOLEAN)
            out = AscLua::lua_toboolean(L, -1) ? 1 : 0;
        else
            MissingMember(type, key);
        AscLua::lua_settop(L, -2);
    }

    void FieldStr(lua_State* L, const char* type, const char* key, std::string& out, bool& found)
    {
        AscLua::lua_getfield(L, 1, key);
        found = AscLua::lua_type(L, -1) == STRING;
        if (found)
            out = AscLua::lua_tolstring(L, -1, nullptr);
        else
            MissingMember(type, key);
        AscLua::lua_settop(L, -2);
    }

    bool ReadSpellArg(lua_State* L, Spell& s)   // FUN_100e9540 -> FUN_100f4b20 / FUN_100f6310
    {
        if (!ValidateInput(L, {TABLE}))
            return false;
        const char* t = "BuildCreatorSpell";
        FieldU32(L, t, "Spell", s.spell);
        FieldU32(L, t, "Level", s.level);
        FieldBool(L, t, "IsCoreAbility", s.core);
        FieldBool(L, t, "IsOptimalAbility", s.optimal);
        FieldBool(L, t, "IsEmpoweringAbility", s.empowering);
        FieldBool(L, t, "IsSynergisticAbility", s.synergistic);
        FieldU32(L, t, "Flags", s.flags);
        bool found = false;
        FieldStr(L, t, "Comment", s.comment, found);
        return true;
    }

    bool ReadEnchantArg(lua_State* L, Enchant& c)   // FUN_100e9460 -> FUN_100f4800 / FUN_100f5c50
    {
        if (!ValidateInput(L, {TABLE}))
            return false;
        const char* t = "BuildCreatorRandomEnchant";
        FieldU32(L, t, "Enchant", c.id);
        FieldU32(L, t, "Stacks", c.stacks);
        FieldU32(L, t, "Level", c.level);
        FieldU32(L, t, "Flags", c.flags);
        bool found = false;
        FieldStr(L, t, "Comment", c.comment, found);
        return true;
    }

    // FUN_100e9600 / FUN_100e9390 -> FUN_100f4e50 / FUN_100f4420: {Type = "ITEM_SUBCLASS_*", Comment}.
    template <size_t N>
    bool ReadTypedArg(lua_State* L, const char* const (&table)[N], Typed& out)
    {
        if (!ValidateInput(L, {TABLE}))
            return false;
        const char* t = "BuildCreatorItemType";
        std::string name;
        bool found = false;
        FieldStr(L, t, "Type", name, found);
        if (found)
            out.type = EnumOrLog(table, name, out.type);
        FieldStr(L, t, "Comment", out.comment, found);
        return true;
    }

    // ---- validators --------------------------------------------------------------------------------
    // FUN_100fde10: the highest rank number among the pending spells of this spell's chain (0 if none).
    uint32_t HighestPendingRank(uint32_t spell)
    {
        const uint32_t first = AscSpellRank::FirstRank(spell);
        uint32_t best = 0;
        for (const Spell& s : g.pending.spells)
            if (AscSpellRank::FirstRank(s.spell) == first)
                best = std::max(best, AscSpellRank::RankNumber(s.spell));
        return best;
    }

    Spell* PendingSpell(uint32_t spell)
    {
        for (Spell& s : g.pending.spells)
            if (s.spell == spell)
                return &s;
        return nullptr;
    }

    // FUN_100fac40
    std::vector<uint32_t> CanAddSpell(const Spell& s)
    {
        std::vector<uint32_t> errors;
        const AscCA::Row row = AscCA::RowBySpell(s.spell);
        if (!row)
            errors.push_back(1);
        else
        {
            AscCA::Build b(SeedOf(g.pending), s.level, false);
            const uint32_t id = AscCA::RowU32(row, 0);
            if (b.ValidateLearn(nullptr, id, {0x26}, {}))
                errors.push_back(4);
            if (b.ValidateLearn(nullptr, id, {0x27}, {}))
                errors.push_back(5);
            if (s.level < AscCA::RequiredLevelOf(b, row))
                errors.push_back(7);
        }
        if (PendingSpell(s.spell))
            errors.push_back(2);
        if (AscSpellRank::RankNumber(s.spell) - 1 != HighestPendingRank(s.spell))
            errors.push_back(3);
        if (s.level > AscGameEvents::ServerMaxLevel())
            errors.push_back(6);
        return errors;
    }

    // FUN_100fb9b0
    std::vector<uint32_t> CanRemoveSpell(uint32_t spell)
    {
        std::vector<uint32_t> errors;
        if (!PendingSpell(spell))
        {
            errors.push_back(1);
            return errors;
        }
        if (HighestPendingRank(spell) != AscSpellRank::RankNumber(spell))
            errors.push_back(2);
        if (const AscCA::Row row = AscCA::RowBySpell(spell))
        {
            AscCA::Build b(SeedOf(g.pending), 0, false);
            const uint32_t id = AscCA::RowU32(row, 0);
            if (b.ValidateUnlearn(nullptr, id, {0x11}, {}))
                errors.push_back(3);
            if (b.ValidateUnlearn(nullptr, id, {0x12}, {}))
                errors.push_back(4);
        }
        return errors;
    }

    // FUN_100fbd20
    std::vector<uint32_t> CanSetSpellLevel(uint32_t spell, uint32_t level)
    {
        std::vector<uint32_t> errors;
        if (const Spell* s = PendingSpell(spell))
        {
            if (s->level == level)
                errors.push_back(1);
        }
        else
            errors.push_back(2);
        const AscCA::Row row = AscCA::RowBySpell(spell);
        if (!row)
            errors.push_back(4);
        else
        {
            AscCA::Build b(SeedOf(g.pending), level, true);
            if (level < AscCA::RequiredLevelOf(b, row))
                errors.push_back(3);
        }
        const uint32_t max = AscGameEvents::ServerMaxLevel();
        if (max < level)
            errors.push_back(5);
        // The lower ranks present must not sit above this level, nor the higher ranks below it.
        const std::vector<uint32_t>& chain = AscSpellRank::Chain(AscSpellRank::FirstRank(spell));
        uint32_t prev = 0;
        for (uint32_t c : chain)
            if (const Spell* p = PendingSpell(c))
            {
                if (p->spell == spell)
                {
                    if (level < prev)
                        errors.push_back(6);
                    break;
                }
                prev = p->level;
            }
        uint32_t next = max + 1;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if (const Spell* p = PendingSpell(*it))
            {
                if (p->spell == spell)
                {
                    if (next < level)
                        errors.push_back(6);
                    break;
                }
                next = p->level;
            }
        return errors;
    }

    // FUN_100fb090: 0 or a PUBLISH_BUILD_* index.
    uint32_t ValidatePublish(bool isNew)
    {
        const Entry& e = g.pending;
        const bool player = ActivePlayer() != nullptr;
        if (player && (e.category == 8 || e.category == 10 || e.category == 11) && AscAccount::GmLevel() == 0)
            return 0x15;
        if (e.spells.empty())
            return 3;
        if (e.weapons.empty())
            return 4;
        if (e.armor.empty())
            return 5;
        if (e.name.empty())
            return 6;
        if (e.name == "Build Name")
            return 7;
        if (e.name.size() > 0x80)
            return 8;
        if (e.icon.empty())
            return 9;
        if (e.icon == "inv_misc_questionmark")
            return 0xA;
        if (e.icon.size() > 0x80)
            return 0xB;
        if (e.description.empty())
            return 0xC;
        if (e.description == "There is no description yet.")
            return 0xD;
        if (e.description.size() > 0x4000)
            return 0xE;
        const Entry* existing = FindBuild(e.id);
        if (!isNew)
        {
            if (!existing)
                return 0x24;
            if (player && PlayerClass() != existing->cls)
                return 0x26;
        }
        else if (existing)
            return 0x10;

        uint32_t id = 0, rank = 0, result = 0;
        uint32_t top = 0;
        for (const Spell& s : e.spells)
            top = std::max(top, s.level);
        for (uint32_t lvl = 1; lvl <= top; ++lvl)
        {
            AscCA::Build b(SeedOf(e), lvl, true);
            b.Reorder(true);
            if (!b.ValidateAll(true, {5, 0x1C}, {}, false, id, rank, result))
                return 0xF;
        }

        AscCA::Build b(SeedOf(e), AscGameEvents::ServerMaxLevel(), true);
        b.Reorder(true);
        const uint32_t quality[4][2] = {{0x15, 0x11}, {0x16, 0x12}, {0x17, 0x13}, {0x18, 0x14}};
        for (const auto& q : quality)
            if (!b.ValidateAll(true, {q[0]}, {}, false, id, rank, result))
                return q[1];
        switch (e.category)
        {
        case 2: case 3: case 4:
            if (b.GlobalAE(0) < 30)
                return 0x16;
            if (b.GlobalTE(0) < 30)
                return 0x17;
            break;
        case 5: case 6: case 7:
            if (b.GlobalAE(0) < 35)
                return 0x16;
            if (b.GlobalTE(0) < 35)
                return 0x17;
            break;
        }
        for (const Spell& s : e.spells)
        {
            if (s.comment.size() > 0x80)
                return 0x18;
            if ((s.synergistic != 0) + (s.empowering != 0) + (s.optimal != 0) + (s.core != 0) > 1)
                return 0x22;
        }
        for (const Enchant& c : e.enchants)
            if (c.comment.size() > 0x80)
                return 0x19;
        for (const Typed& t : e.weapons)
            if (t.comment.size() > 0x80)
                return 0x1A;
        for (const Typed& t : e.armor)
            if (t.comment.size() > 0x80)
                return 0x1B;
        if (!b.ValidateAll(true, {}, {0x2C}, false, id, rank, result))
            return 0x23;
        return 0;
    }

    // ---- SMSG handlers -----------------------------------------------------------------------------
    // SMSG 0x631 (FUN_100ff200): cstring, one entry. A hidden (+0x12C) build a non-GM does not own is
    // dropped; otherwise it replaces the queried copy, is erased when it has no spells, or is appended.
    void __cdecl OnBuild(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string text = ReadCString(p);
        Entry e = ReadEntry(p);
        SortEntry(e);
        if (e.needsRepairs && AscAccount::GmLevel() == 0 && !Contains(g.owned, e.id))
            return;
        auto it = std::find_if(g.queried.begin(), g.queried.end(), [&](const Entry& q) { return q.id == e.id; });
        if (it == g.queried.end())
            g.queried.push_back(e);
        else if (e.spells.empty())
            g.queried.erase(it);
        else
            *it = e;
        AscRuntime::Signal("BUILD_CREATOR_BUILD_RESULT", "%s%s", text.c_str(), e.id.c_str());
    }

    // SMSG 0x62F (FUN_100ff530): cstring, u32 page, u32 pages, u32 n, n entries into the category page
    // (page 0 clears it); BUILD_CREATOR_CATEGORY_RESULT after the last page.
    void __cdecl OnCategoryPage(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string text = ReadCString(p);
        const uint32_t pageNo = Read<uint32_t>(p);
        const uint32_t pages = Read<uint32_t>(p);
        uint32_t n = Read<uint32_t>(p);
        if (pageNo == 0)
        {
            g.page.clear();
            ResetFilter();
        }
        const Entry* before = g.page.data();
        for (; n; --n)
        {
            Entry e = ReadEntry(p);
            SortEntry(e);
            if (e.needsRepairs && AscAccount::GmLevel() == 0 && !Contains(g.owned, e.id))
                continue;
            auto it = std::find_if(g.page.begin(), g.page.end(), [&](const Entry& q) { return q.id == e.id; });
            if (it != g.page.end())
            {
                g.page.erase(it);
                if (!e.spells.empty())
                    g.page.push_back(e);
            }
            else
                g.page.push_back(e);
        }
        // The filter holds pointers into the page; the original only resets it on page 0 and dangles
        // when a later page reallocates. Reset then too.
        if (g.page.data() != before)
            ResetFilter();
        if (pageNo == pages - 1)
            AscRuntime::Signal("BUILD_CREATOR_CATEGORY_RESULT", "%s%s", EnumName(kBcCategories, g.category).c_str(),
                               text.c_str());
    }

    // FUN_100fe1d0 (0x627) / FUN_100ffaf0 (0x629): the result names PUBLISH_BUILD_*, reported with its
    // prefix swapped for CREATE_ / SAVE_.
    std::string Renamed(const std::string& result, const char* prefix)
    {
        if (result.size() > 7 && result.compare(0, 8, "PUBLISH_") == 0)
            return prefix + result.substr(8);
        return result;
    }

    uint32_t PublishResult(const std::string& s) { return EnumOrLog(kPublishResults, s, kNoValue); }

    // SMSG 0x627: on success the created build becomes the pending one, joins the queried list, the
    // category page (when it is the queried category, or none was) and the owned ids.
    void __cdecl OnCreate(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadCString(p);
        const std::string shown = Renamed(result, "CREATE_");
        std::string id;
        if (PublishResult(result) == 0)
        {
            g.pending = ReadEntry(p);
            SortEntry(g.pending);
            g.queried.push_back(g.pending);
            if (g.category == 0 || g.category == g.pending.category)
            {
                g.page.push_back(g.pending);
                ResetFilter();
            }
            g.owned.push_back(g.pending.id);
            id = g.pending.id;
        }
        AscRuntime::Signal("BUILD_CREATOR_CREATE_RESULT", "%s%s", shown.c_str(), id.c_str());
    }

    // SMSG 0x629: on success the saved build becomes the pending one and replaces its queried (or is
    // appended) and category-page copies; on failure a cstring id follows.
    void __cdecl OnSave(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadCString(p);
        const std::string shown = Renamed(result, "SAVE_");
        std::string id;
        if (PublishResult(result) == 0)
        {
            g.pending = ReadEntry(p);
            SortEntry(g.pending);
            auto it = std::find_if(g.queried.begin(), g.queried.end(), [&](const Entry& q) { return q.id == g.pending.id; });
            if (it == g.queried.end())
                g.queried.push_back(g.pending);
            else
                *it = g.pending;
            auto pt = std::find_if(g.page.begin(), g.page.end(), [&](const Entry& q) { return q.id == g.pending.id; });
            if (pt != g.page.end())
                *pt = g.pending;
            id = g.pending.id;
        }
        else
            id = ReadCString(p);
        AscRuntime::Signal("BUILD_CREATOR_SAVE_RESULT", "%s%s", shown.c_str(), id.c_str());
    }

    // SMSG 0x62B (FUN_100fedc0): RATE_BUILD_OK moves the rated id in or out of the upvoted list and
    // adjusts the build's Upvotes.
    void __cdecl OnRate(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadCString(p);
        uint32_t index = 0;
        if (!EnumIndex(kRateResults, result, index))
            AscLog::Printf("Unexpected Value: %s", result.c_str());
        else if (index == 0)
        {
            if (!g.rateUp)
            {
                Erase(g.upvoted, g.ratedId);
                if (Entry* e = FindBuild(g.ratedId))
                    --e->upvotes;
            }
            else
            {
                g.upvoted.push_back(g.ratedId);
                if (Entry* e = FindBuild(g.ratedId))
                    ++e->upvotes;
            }
        }
        const Entry* e = FindBuild(g.ratedId);
        AscRuntime::Signal("BUILD_CREATOR_RATE_RESULT", "%s%s%u", result.c_str(), g.ratedId.c_str(),
                           e ? e->upvotes : 0u);
    }

    // SMSG 0x62D (FUN_100fe6a0): DELETE_BUILD_OK drops the deleted id everywhere (a pending copy of it
    // becomes a fresh build).
    void __cdecl OnDelete(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const std::string result = ReadCString(p);
        uint32_t index = 0;
        if (!EnumIndex(kDeleteResults, result, index))
            AscLog::Printf("Unexpected Value: %s", result.c_str());
        else if (index == 0)
        {
            auto it = std::find_if(g.queried.begin(), g.queried.end(), [&](const Entry& q) { return q.id == g.deletedId; });
            if (it != g.queried.end())
                g.queried.erase(it);
            if (g.pending.id == g.deletedId)
                ResetPending();
            ResetFilter();
            auto pt = std::find_if(g.page.begin(), g.page.end(), [&](const Entry& q) { return q.id == g.deletedId; });
            if (pt != g.page.end())
                g.page.erase(pt);
            Erase(g.owned, g.deletedId);
            Erase(g.upvoted, g.deletedId);
        }
        AscRuntime::Signal("BUILD_CREATOR_DELETE_RESULT", "%s", result.c_str());
    }

    SpecRecord ReadSpecRecord(CDataStore* p)   // FUN_100fc950
    {
        SpecRecord r;
        r.id = ReadCString(p);
        r.a = Read<uint8_t>(p);
        r.b = Read<uint8_t>(p);
        return r;
    }

    // SMSG 0x632 (FUN_100fdfd0): u32 spec, that spec's record.
    void __cdecl OnActiveBuild(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t spec = Read<uint32_t>(p);
        if (g.specs.size() <= spec)
            g.specs.resize(spec + 1);
        g.specs[spec] = ReadSpecRecord(p);
        AscRuntime::Signal("BUILD_CREATOR_ACTIVE_BUILD_UPDATE", "%u", spec);
    }

    // SMSG 0x633 (FUN_100fe150): u32 n, n records.
    void __cdecl OnActiveBuilds(void*, uint32_t, uint32_t, CDataStore* p)
    {
        const uint32_t n = Read<uint32_t>(p);
        g.specs.clear();
        g.specs.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            g.specs[i] = ReadSpecRecord(p);
    }

    void ReadIds(CDataStore* p, std::vector<std::string>& out)
    {
        const uint32_t n = Read<uint32_t>(p);
        out.clear();
        out.resize(n);
        for (uint32_t i = 0; i < n; ++i)
            out[i] = ReadCString(p);
    }

    void __cdecl OnOwned(void*, uint32_t, uint32_t, CDataStore* p) { ReadIds(p, g.owned); }       // 0x634
    void __cdecl OnUpvoted(void*, uint32_t, uint32_t, CDataStore* p) { ReadIds(p, g.upvoted); }   // 0x635

    void __cdecl OnActivateResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5C3 (FUN_100fde70)
    {
        AscRuntime::Signal("BUILD_CREATOR_ACTIVATE_RESULT", "%s", ReadCString(p).c_str());
    }

    void __cdecl OnDeactivateResult(void*, uint32_t, uint32_t, CDataStore* p)   // 0x5C5 (FUN_100fe540)
    {
        AscRuntime::Signal("BUILD_CREATOR_DEACTIVATE_RESULT", "%s", ReadCString(p).c_str());
    }

    // ---- getters -----------------------------------------------------------------------------------
    int GetNumBuilds(lua_State* L)   // +0x164 filtered
    {
        PushInt(L, static_cast<int32_t>(g_filter.Size()));
        return 1;
    }

    int GetBuildAtIndex(lua_State* L)
    {
        uint32_t i = 0;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        if (i - 1 < g_filter.Size() && g_filter.At(i - 1))
        {
            PushEntry(L, *g_filter.At(i - 1));   // FUN_100f2bb0
            return 1;
        }
        AscLua::lua_pushnil(L);
        return 1;
    }

    int GetBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        if (const Entry* e = FindBuild(id))
            PushEntry(L, Entry(*e));
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    int GetPendingBuild(lua_State* L)
    {
        PushEntry(L, Entry(g.pending));
        return 1;
    }

    int GetActiveBuild(lua_State* L)   // the id recorded for spec (1-based), "" past the end
    {
        uint32_t i = 0;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        PushStr(L, i - 1 < g.specs.size() ? g.specs[i - 1].id.c_str() : "");
        return 1;
    }

    int GetActiveSpecForBuild(lua_State* L)   // the spec index holding this id, -1 if none
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        int32_t index = -1;
        for (size_t i = 0; i < g.specs.size(); ++i)
            if (g.specs[i].id == id)
            {
                index = static_cast<int32_t>(i);
                break;
            }
        PushInt(L, index);
        return 1;
    }

    int GetNumBookmarkedBuilds(lua_State* L)
    {
        PushInt(L, static_cast<int32_t>(g.queried.size()));
        return 1;
    }

    int GetBookmarkedBuildAtIndex(lua_State* L)   // the id, "" past the end
    {
        uint32_t i = 0;
        if (!ReadNumber(L, i) || i == 0)
            return 0;
        PushStr(L, i - 1 < g.queried.size() ? g.queried[i - 1].id.c_str() : "");
        return 1;
    }

    int IsOwnedBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        PushBool(L, Contains(g.owned, id));
        return 1;
    }

    int IsUpvotedBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        PushBool(L, Contains(g.upvoted, id));
        return 1;
    }

    int DoesBuildHaveSpellID(lua_State* L)
    {
        uint32_t spell = 0;
        if (!ReadNumber(L, spell))
            return 0;
        PushBool(L, PendingSpell(spell) != nullptr);
        return 1;
    }

    int DoesBuildHaveEnchant(lua_State* L)
    {
        uint32_t id = 0;
        if (!ReadNumber(L, id))
            return 0;
        bool found = false;
        for (const Enchant& c : g.pending.enchants)
            found = found || c.id == id;
        PushBool(L, found);
        return 1;
    }

    int GetSpell(lua_State* L)   // (buildId, spell)
    {
        if (!ValidateInput(L, {STRING, NUMBER}))
            return 0;
        const std::string id = CheckString(L, 1);
        const int32_t spell = static_cast<int32_t>(CheckNumber(L, 2));   // FUN_10ae6370
        if (const Entry* e = FindBuild(id))
            for (const Spell& s : e->spells)
                if (s.spell == static_cast<uint32_t>(spell))
                {
                    PushSpell(L, s);   // FUN_1009af40
                    return 1;
                }
        AscLua::lua_pushnil(L);
        return 1;
    }

    int GetSpellByID(lua_State* L)
    {
        uint32_t spell = 0;
        if (!ReadNumber(L, spell))
            return 0;
        if (const Spell* s = PendingSpell(spell))
            PushSpell(L, *s);
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // Level argument or the server's max level (FUN_1008ba00 fails -> FUN_102fc4d0).
    uint32_t LevelArg(lua_State* L)
    {
        uint32_t lvl = 0;
        if (!ReadNumber(L, lvl))
            lvl = AscGameEvents::ServerMaxLevel();
        return lvl;
    }

    int GetEssenceForLevel(lua_State* L)
    {
        AscCA::Build b(SeedOf(g.pending), LevelArg(L), false);
        PushInt(L, static_cast<int32_t>(b.GlobalAE(0)));
        PushInt(L, static_cast<int32_t>(b.RemainingAE()));
        PushInt(L, static_cast<int32_t>(b.GlobalTE(0)));
        PushInt(L, static_cast<int32_t>(b.RemainingTE()));
        return 4;
    }

    int GetQualityInfo(lua_State* L)   // (spell[, level]) -> quality name, cost
    {
        int32_t spell = 0, lvl = 0;
        if (!ReadInt2(L, spell, lvl))
        {
            uint32_t s = 0;
            if (!ReadNumber(L, s))
                return 0;
            spell = static_cast<int32_t>(s);
            lvl = static_cast<int32_t>(AscGameEvents::ServerMaxLevel());
        }
        const AscCA::Row row = AscCA::RowBySpell(static_cast<uint32_t>(spell));
        if (!row)
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        AscCA::Build b(SeedOf(g.pending), static_cast<uint32_t>(lvl), true);
        const uint32_t q = static_cast<uint32_t>(AscCA::QualityIndexOf(b, row));
        PushStr(L, (q < 9 ? std::string(kQualities[q]) : Unexpected(q)).c_str());
        PushInt(L, static_cast<int32_t>(AscCA::QualityCostOf(b, row)));
        return 2;
    }

    int GetQualityInfoForLevel(lua_State* L)
    {
        AscCA::Build b(SeedOf(g.pending), LevelArg(L), false);
        for (int q = 1; q <= 5; ++q)
            PushInt(L, static_cast<int32_t>(AscCA::QualitySumOf(b, q)));
        return 5;
    }

    // ---- editing the pending build -------------------------------------------------------------------
    int EditBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        if (const Entry* e = FindBuild(id))
            g.pending = Entry(*e);
        else
            ResetPending();
        return 0;
    }

    int DiscardPendingBuild(lua_State*)
    {
        ResetPending();
        return 0;
    }

    int SetName(lua_State* L)        { std::string s; if (ReadString(L, s)) g.pending.name = s; return 0; }
    int SetIcon(lua_State* L)        { std::string s; if (ReadString(L, s)) g.pending.icon = s; return 0; }
    int SetDescription(lua_State* L) { std::string s; if (ReadString(L, s)) g.pending.description = s; return 0; }

    int SetCategory(lua_State* L)
    {
        std::string s;
        if (ReadString(L, s))
            g.pending.category = EnumOrLog(kBcCategories, s, kNoValue);
        return 0;
    }

    int SetRoles(lua_State* L)
    {
        std::string s;
        if (!ReadString(L, s))
            return 0;
        uint32_t index = 0;
        uint32_t value = 0;
        if (EnumIndex(kPlayerRoles, s, index))
            value = kRoleValues[index];
        else
            AscLog::Printf("Unexpected Value: %s", s.c_str());
        g.pending.roles = value;
        return 0;
    }

    int SetPrimaryStat(lua_State* L)
    {
        std::string s;
        if (ReadString(L, s))
            g.pending.primaryStat = EnumOrLog(kStats, s, 0);
        return 0;
    }

    int SetDifficultyRating(lua_State* L)
    {
        std::string s;
        if (ReadString(L, s))
            g.pending.difficulty = EnumOrLog(kDifficulty, s, 0);
        return 0;
    }

    // (spellOrEnchant, comment): an enchant with that id, else every spell of that chain; or
    // (typeName, comment): the weapon type, else the armour type of that name.
    int SetComment(lua_State* L)
    {
        if (ValidateInput(L, {NUMBER, STRING}))
        {
            const uint32_t id = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            const std::string comment = CheckString(L, 2);
            bool enchant = false;
            for (Enchant& c : g.pending.enchants)
                if (c.id == id)
                {
                    c.comment = comment;
                    enchant = true;
                    break;
                }
            if (!enchant)
            {
                const uint32_t first = AscSpellRank::FirstRank(id);
                for (const Spell& s : g.pending.spells)
                    if (AscSpellRank::FirstRank(s.spell) == first)
                        PendingSpell(s.spell)->comment = comment;
            }
            return 0;
        }
        if (!ValidateInput(L, {STRING, STRING}))
            return 0;
        const std::string name = CheckString(L, 1);
        const std::string comment = CheckString(L, 2);
        uint32_t type = 0;
        if (EnumIndex(kWeaponTypes, name, type))   // FUN_100eff20
        {
            for (Typed& t : g.pending.weapons)
                if (t.type == type)
                {
                    t.comment = comment;
                    break;
                }
        }
        else if (EnumIndex(kArmorTypes, name, type))   // FUN_100efd20
        {
            for (Typed& t : g.pending.armor)
                if (t.type == type)
                {
                    t.comment = comment;
                    break;
                }
        }
        return 0;
    }

    // Spells.
    int AddSpell(lua_State* L)
    {
        Spell s;
        if (!ReadSpellArg(L, s))
            return 0;
        const std::vector<uint32_t> errors = CanAddSpell(s);
        if (errors.empty())
        {
            if (Spell* old = PendingSpell(s.spell))   // FUN_100fa550
                *old = s;
            else
                g.pending.spells.push_back(s);
            SortEntry(g.pending);
        }
        return PushDone(L, kAddSpellResults, errors);
    }

    int CanAddSpellL(lua_State* L)
    {
        Spell s;
        if (!ReadSpellArg(L, s))
            return 0;
        return PushCan(L, kAddSpellResults, CanAddSpell(s));
    }

    int RemoveSpell(lua_State* L)
    {
        uint32_t spell = 0;
        if (!ReadNumber(L, spell))
            return 0;
        const std::vector<uint32_t> errors = CanRemoveSpell(spell);
        if (errors.empty())
        {
            auto& v = g.pending.spells;
            auto it = std::find_if(v.begin(), v.end(), [&](const Spell& s) { return s.spell == spell; });
            if (it != v.end())
            {
                v.erase(it);
                SortEntry(g.pending);
            }
        }
        return PushDone(L, kRemoveSpellResults, errors);
    }

    int CanRemoveSpellL(lua_State* L)   // (ok, nil) or (false, NAME) -- a single name here
    {
        uint32_t spell = 0;
        if (!ReadNumber(L, spell))
            return 0;
        return PushDone(L, kRemoveSpellResults, CanRemoveSpell(spell));
    }

    int SetSpellLevel(lua_State* L)
    {
        int32_t spell = 0, level = 0;
        if (!ReadInt2(L, spell, level))
            return 0;
        const std::vector<uint32_t> errors = CanSetSpellLevel(static_cast<uint32_t>(spell), static_cast<uint32_t>(level));
        if (errors.empty())
            if (Spell* s = PendingSpell(static_cast<uint32_t>(spell)))
                s->level = static_cast<uint32_t>(level);
        return PushDone(L, kSetSpellLevelResults, errors);
    }

    int CanSetSpellLevelL(lua_State* L)
    {
        int32_t spell = 0, level = 0;
        if (!ReadInt2(L, spell, level))
            return 0;
        return PushCan(L, kSetSpellLevelResults,
                       CanSetSpellLevel(static_cast<uint32_t>(spell), static_cast<uint32_t>(level)));
    }

    int SetSpellFlags(lua_State* L)   // every spell of the chain
    {
        int32_t spell = 0, flags = 0;
        if (!ReadInt2(L, spell, flags))
            return 0;
        const uint32_t first = AscSpellRank::FirstRank(static_cast<uint32_t>(spell));
        for (const Spell& s : g.pending.spells)
            if (AscSpellRank::FirstRank(s.spell) == first)
                PendingSpell(s.spell)->flags = static_cast<uint32_t>(flags);
        return 0;
    }

    // FUN_10102b80: CanSetSpellFlags, CanSetEnchantLevel, CanSetEnchantFlags, CanSetRandomEnchantStacks.
    int CanSetInt2(lua_State* L)
    {
        int32_t a = 0, b = 0;
        if (!ReadInt2(L, a, b))
            return 0;
        return PushOk(L);
    }

    bool ReadSpellBool(lua_State* L, uint32_t& spell, bool& on)   // FUN_100e92d0
    {
        if (!ValidateInput(L, {NUMBER, BOOLEAN}))
            return false;
        spell = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
        on = AscLua::lua_toboolean(L, 2) != 0;
        return true;
    }

    // SetIs*Ability: the flag on every spell of the chain; setting it clears the other three.
    int SetAbilityKind(lua_State* L, uint8_t Spell::*flag)
    {
        uint32_t spell = 0;
        bool on = false;
        if (!ReadSpellBool(L, spell, on))
            return 0;
        uint8_t Spell::*const all[4] = {&Spell::core, &Spell::optimal, &Spell::empowering, &Spell::synergistic};
        const uint32_t first = AscSpellRank::FirstRank(spell);
        for (const Spell& s : g.pending.spells)
            if (AscSpellRank::FirstRank(s.spell) == first)
            {
                Spell* t = PendingSpell(s.spell);
                t->*flag = on ? 1 : 0;
                if (on)
                    for (uint8_t Spell::*other : all)
                        if (other != flag)
                            t->*other = 0;
            }
        return 0;
    }

    template <size_t N>
    int CanSetAbilityKind(lua_State* L, uint8_t Spell::*flag, const char* const (&table)[N])
    {
        uint32_t spell = 0;
        bool on = false;
        if (!ReadSpellBool(L, spell, on))
            return 0;
        std::vector<uint32_t> errors;
        if (const Spell* s = PendingSpell(spell))
        {
            if ((s->*flag != 0) == on)
                errors.push_back(1);
        }
        else
            errors.push_back(2);
        return PushCan(L, table, errors);
    }

    int SetIsCoreAbility(lua_State* L)        { return SetAbilityKind(L, &Spell::core); }
    int SetIsOptimalAbility(lua_State* L)     { return SetAbilityKind(L, &Spell::optimal); }
    int SetIsEmpoweringAbility(lua_State* L)  { return SetAbilityKind(L, &Spell::empowering); }
    int SetIsSynergisticAbility(lua_State* L) { return SetAbilityKind(L, &Spell::synergistic); }
    int CanSetIsCoreAbility(lua_State* L)        { return CanSetAbilityKind(L, &Spell::core, kSetCoreResults); }
    int CanSetIsOptimalAbility(lua_State* L)     { return CanSetAbilityKind(L, &Spell::optimal, kSetOptimalResults); }
    int CanSetIsEmpoweringAbility(lua_State* L)  { return CanSetAbilityKind(L, &Spell::empowering, kSetEmpoweringResults); }
    int CanSetIsSynergisticAbility(lua_State* L) { return CanSetAbilityKind(L, &Spell::synergistic, kSetSynergisticResults); }

    // (id, comment) with the comment at most 0x80.
    template <size_t N>
    int CanSetNumberComment(lua_State* L, const char* const (&table)[N])   // FUN_100e9180
    {
        if (!ValidateInput(L, {NUMBER, STRING}))
            return 0;
        const std::string comment = CheckString(L, 2);
        std::vector<uint32_t> errors;
        if (comment.size() > 0x80)
            errors.push_back(1);
        return PushCan(L, table, errors);
    }

    int CanSetSpellComment(lua_State* L)         { return CanSetNumberComment(L, kSetSpellCommentResults); }
    int CanSetRandomEnchantComment(lua_State* L) { return CanSetNumberComment(L, kSetEnchantCommentResults); }

    // Random enchants.
    int AddRandomEnchant(lua_State* L)
    {
        Enchant c;
        if (!ReadEnchantArg(L, c))
            return 0;
        auto& v = g.pending.enchants;   // FUN_100fa430
        auto it = std::find_if(v.begin(), v.end(), [&](const Enchant& e) { return e.id == c.id; });
        if (it == v.end())
            v.push_back(c);
        else
            *it = c;
        SortEntry(g.pending);
        return PushOk(L);
    }

    int CanAddRandomEnchant(lua_State* L)
    {
        Enchant c;
        if (!ReadEnchantArg(L, c))
            return 0;
        return PushOk(L);
    }

    int RemoveRandomEnchant(lua_State* L)
    {
        uint32_t id = 0;
        if (!ReadNumber(L, id))
            return 0;
        auto& v = g.pending.enchants;
        auto it = std::find_if(v.begin(), v.end(), [&](const Enchant& e) { return e.id == id; });
        if (it != v.end())
        {
            v.erase(it);
            SortEntry(g.pending);
        }
        return PushOk(L);
    }

    int CanRemoveRandomEnchant(lua_State* L)
    {
        uint32_t id = 0;
        if (!ReadNumber(L, id))
            return 0;
        return PushOk(L);
    }

    int SetEnchantField(lua_State* L, uint32_t Enchant::*field)
    {
        int32_t id = 0, value = 0;
        if (!ReadInt2(L, id, value))
            return 0;
        for (Enchant& c : g.pending.enchants)
            if (c.id == static_cast<uint32_t>(id))
            {
                c.*field = static_cast<uint32_t>(value);
                break;
            }
        return PushOk(L);
    }

    int SetEnchantLevel(lua_State* L) { return SetEnchantField(L, &Enchant::level); }
    int SetEnchantFlags(lua_State* L) { return SetEnchantField(L, &Enchant::flags); }

    // SetEnchantStacks (0x10106db0): sets +0x04 on the first match and, unlike the two above, returns
    // nothing.
    int SetEnchantStacks(lua_State* L)
    {
        int32_t id = 0, value = 0;
        if (!ReadInt2(L, id, value))
            return 0;
        for (Enchant& c : g.pending.enchants)
            if (c.id == static_cast<uint32_t>(id))
            {
                c.stacks = static_cast<uint32_t>(value);
                break;
            }
        return 0;
    }

    // Weapon and armour types.
    template <size_t N>
    int AddTyped(lua_State* L, const char* const (&table)[N], uint32_t defaultType, std::vector<Typed>& v)
    {
        Typed t;
        t.type = defaultType;
        if (!ReadTypedArg(L, table, t))
            return 0;
        auto it = std::find_if(v.begin(), v.end(), [&](const Typed& o) { return o.type == t.type; });   // FUN_100fa650
        if (it == v.end())
            v.push_back(t);
        else
            *it = t;
        SortEntry(g.pending);
        return PushOk(L);
    }

    template <size_t N>
    int CanAddTyped(lua_State* L, const char* const (&table)[N], uint32_t defaultType)
    {
        Typed t;
        t.type = defaultType;
        if (!ReadTypedArg(L, table, t))
            return 0;
        return PushOk(L);
    }

    template <size_t N>
    int RemoveTyped(lua_State* L, const char* const (&table)[N], std::vector<Typed>& v)
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        const uint32_t type = EnumOrLog(table, name, kNoValue);   // FUN_100effa0 / FUN_100efda0
        auto it = std::find_if(v.begin(), v.end(), [&](const Typed& o) { return o.type == type; });
        if (it != v.end())
        {
            v.erase(it);
            SortEntry(g.pending);
        }
        return PushOk(L);
    }

    template <size_t N>
    int CanRemoveTyped(lua_State* L, const char* const (&table)[N])
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        EnumOrLog(table, name, kNoValue);
        return PushOk(L);
    }

    template <size_t N, size_t M>
    int CanSetTypedComment(lua_State* L, const char* const (&types)[N], const char* const (&results)[M])
    {
        if (!ValidateInput(L, {STRING, STRING}))   // FUN_100d0770
            return 0;
        EnumOrLog(types, std::string(CheckString(L, 1)), kNoValue);
        const std::string comment = CheckString(L, 2);
        std::vector<uint32_t> errors;
        if (comment.size() > 0x80)
            errors.push_back(1);
        return PushCan(L, results, errors);
    }

    int AddWeaponType(lua_State* L)       { return AddTyped(L, kWeaponTypes, 0x15, g.pending.weapons); }
    int AddArmorType(lua_State* L)        { return AddTyped(L, kArmorTypes, 0xB, g.pending.armor); }
    int CanAddWeaponType(lua_State* L)    { return CanAddTyped(L, kWeaponTypes, 0x15); }
    int CanAddArmorType(lua_State* L)     { return CanAddTyped(L, kArmorTypes, 0xB); }
    int RemoveWeaponType(lua_State* L)    { return RemoveTyped(L, kWeaponTypes, g.pending.weapons); }
    int RemoveArmorType(lua_State* L)     { return RemoveTyped(L, kArmorTypes, g.pending.armor); }
    int CanRemoveWeaponType(lua_State* L) { return CanRemoveTyped(L, kWeaponTypes); }
    int CanRemoveArmorType(lua_State* L)  { return CanRemoveTyped(L, kArmorTypes); }
    int CanSetWeaponTypeComment(lua_State* L) { return CanSetTypedComment(L, kWeaponTypes, kSetWeaponCommentResults); }
    int CanSetArmorTypeComment(lua_State* L)  { return CanSetTypedComment(L, kArmorTypes, kSetArmorCommentResults); }

    // ---- server round-trips ------------------------------------------------------------------------
    int CanPublishBuild(lua_State* L)
    {
        const uint32_t code = ValidatePublish(g.pending.id.empty());
        std::vector<uint32_t> errors;
        if (code)
            errors.push_back(code);
        return PushCan(L, kPublishResults, errors);
    }

    // CMSG 0x626 create (no id yet) or 0x628 save, carrying the pending entry. Not validated here.
    int PublishBuild(lua_State* L)
    {
        Packet pk(g.pending.id.empty() ? 0x626 : 0x628);
        WriteEntry(pk, g.pending);
        pk.Send();
        PushBool(L, true);
        return 1;
    }

    int ActivateBuild(lua_State* L)   // CMSG 0x5C2: id, u8, u8
    {
        if (!ValidateInput(L, {STRING, BOOLEAN, BOOLEAN}))
            return 0;
        const std::string id = CheckString(L, 1);
        const uint8_t a = AscLua::lua_toboolean(L, 2) != 0;
        const uint8_t b = AscLua::lua_toboolean(L, 3) != 0;
        Packet(0x5C2).Str(id.c_str()).U8(a).U8(b).Send();
        PushBool(L, true);
        return 1;
    }

    int DeactivateBuild(lua_State* L)   // CMSG 0x5C4: id
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        Packet(0x5C4).Str(id.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    // CanActivateBuild / CanDeactivateBuild (FUN_10101f30): the id is known.
    int CanActivateBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        PushBool(L, FindBuild(id) != nullptr);
        AscLua::lua_pushnil(L);
        return 2;
    }

    int RateBuild(lua_State* L)   // CMSG 0x62A: id, u8 up; remembered for SMSG 0x62B
    {
        if (!ValidateInput(L, {STRING, BOOLEAN}))
            return 0;
        const std::string id = CheckString(L, 1);
        const bool up = AscLua::lua_toboolean(L, 2) != 0;
        g.ratedId = id;
        g.rateUp = up;
        Packet(0x62A).Str(id.c_str()).U8(up ? 1 : 0).Send();
        PushBool(L, true);
        return 1;
    }

    int DeleteBuild(lua_State* L)   // CMSG 0x62C: id; remembered for SMSG 0x62D
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        g.deletedId = id;
        Packet(0x62C).Str(id.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    int QueryAllBuilds(lua_State* L)   // CMSG 0x62E: the category name, remembered as the enum
    {
        std::string name;
        if (!ReadString(L, name))
            return 0;
        g.category = EnumOrLog(kBcCategories, name, kNoValue);
        Packet(0x62E).Str(name.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    int QueryBuild(lua_State* L)   // CMSG 0x630: id
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        Packet(0x630).Str(id.c_str()).Send();
        PushBool(L, true);
        return 1;
    }

    // FUN_100fa7f0: a queried build moves to the front; one only on the category page is copied there.
    int BookmarkBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        auto it = std::find_if(g.queried.begin(), g.queried.end(), [&](const Entry& e) { return e.id == id; });
        if (it != g.queried.end())
        {
            Entry copy = *it;
            g.queried.erase(it);
            g.queried.insert(g.queried.begin(), copy);
        }
        else
        {
            auto pt = std::find_if(g.page.begin(), g.page.end(), [&](const Entry& e) { return e.id == id; });
            if (pt != g.page.end())
                g.queried.insert(g.queried.begin(), *pt);
        }
        return 0;
    }

    bool BuildFilter::ArgMatch(uint32_t a, const Entry* const& e)
    {
        switch (a)
        {
        case 1: return (e->flags & 1) != 0;
        case 2: case 3: case 4: case 5: case 6: return e->difficulty == a - 2;
        case 7: return Contains(g.owned, e->id);   // FUN_10101690
        case 8: return e->primaryStat == 0;
        case 9: return e->primaryStat == 1;
        case 0xA: return e->primaryStat == 3;
        case 0xB: return e->primaryStat == 4;
        case 0xC: return (e->roles >> 1) & 1;
        case 0xD: return (e->roles >> 2) & 1;
        case 0xE: return (e->roles >> 3) & 1;
        case 0xF:   // Ascension's own builds, or any updated in the last 30 days
            return e->author == "Ascension" || e->updated >= static_cast<uint64_t>(_time64(nullptr) - 0x278D00);
        default:
            return a >= 0x10 && a <= 0x2F && e->cls == a - 0xF;
        }
    }

    // Upvotes ascending / descending, updated time ascending / descending, as the original's
    // fixed-width unsigned keys (the descending forms are ~upvotes and -updated - 1).
    std::string BuildFilter::SortKey(uint32_t s, const Entry* const& e)
    {
        uint64_t v = 0;
        switch (s)
        {
        case 1: v = e->upvotes; break;
        case 2: v = static_cast<uint32_t>(~e->upvotes); break;
        case 3: v = e->updated; break;
        case 4: v = static_cast<uint64_t>(-static_cast<int64_t>(e->updated) - 1); break;
        }
        std::string k;
        AscFilter::AppendU64(k, v);
        return k;
    }

    // FUN_100f36c0 / FUN_100f3d70 (FrameScript::lua_totable): {[number] = boolean} in lua_next order;
    // anything else logs and yields an empty map.
    std::vector<uint32_t> TrueKeys(lua_State* L, int idx)
    {
        std::unordered_map<uint32_t, bool> map;
        AscLua::lua_pushnil(L);
        while (AscLua::lua_next(L, idx))
        {
            if (AscLua::lua_type(L, -2) != NUMBER)
            {
                AscLog::Printf("FrameScript::lua_totable Invalid lua key type");
                AscLua::lua_settop(L, -3);
                return {};
            }
            if (AscLua::lua_type(L, -1) != BOOLEAN)
            {
                AscLog::Printf("FrameScript::lua_totable type");
                AscLua::lua_settop(L, -3);
                return {};
            }
            map.emplace(static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, -2))), AscLua::lua_toboolean(L, -1) != 0);
            AscLua::lua_settop(L, -2);
        }
        // UpdateFilter walks the map: MSVC's unordered_map, same FNV hash and bucket policy and the same
        // insertions, visits in the original's order.
        std::vector<uint32_t> keys;
        for (const auto& kv : map)
            if (kv.second)
                keys.push_back(kv.first);
        return keys;
    }

    // C_BuildCreator.ExportBuild (FUN_10103e40): the link of a known build.
    int ExportBuild(lua_State* L)
    {
        std::string id;
        if (!ReadString(L, id))
            return 0;
        if (const Entry* e = FindBuild(id))
            PushStr(L, ExportLink(*e).c_str());
        else
            AscLua::lua_pushnil(L);
        return 1;
    }

    // C_BuildEditor.ExportPendingBuild (0x10103f50, no arguments): the pending entry's link.
    int ExportPendingBuild(lua_State* L)
    {
        PushStr(L, ExportLink(g.pending).c_str());
        return 1;
    }

    int ImportBuild(lua_State* L)   // the link into the pending build
    {
        std::string url;
        if (ReadString(L, url))
            ImportLink(g.pending, url);
        return 0;
    }

    // The active CA build (with the equipped mystic enchants) round-tripped through a link into the
    // pending build.
    int ImportCurrentBuild(lua_State*)
    {
        const Entry current = EntryFromBuild(*AscCA::ActiveBuild(), true);
        ImportLink(g.pending, ExportLink(current));
        return 0;
    }

    int UpdateFilter(lua_State* L)   // (text, {[arg] = bool}, {[sort] = bool})
    {
        if (!ValidateInput(L, {STRING, TABLE, TABLE}))   // FUN_100e9a50
            return 0;
        const std::string text = CheckString(L, 1);
        const std::vector<uint32_t> args = TrueKeys(L, 2);
        const std::vector<uint32_t> sorts = TrueKeys(L, 3);
        g_filter.ApplyFilter(text, args, sorts);
        return 0;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x5C3, CNetClientCustomPacket((void*)&OnActivateResult, nullptr));
        sDC.AddPacketHandler(0x5C5, CNetClientCustomPacket((void*)&OnDeactivateResult, nullptr));
        sDC.AddPacketHandler(0x627, CNetClientCustomPacket((void*)&OnCreate, nullptr));
        sDC.AddPacketHandler(0x629, CNetClientCustomPacket((void*)&OnSave, nullptr));
        sDC.AddPacketHandler(0x62B, CNetClientCustomPacket((void*)&OnRate, nullptr));
        sDC.AddPacketHandler(0x62D, CNetClientCustomPacket((void*)&OnDelete, nullptr));
        sDC.AddPacketHandler(0x62F, CNetClientCustomPacket((void*)&OnCategoryPage, nullptr));
        sDC.AddPacketHandler(0x631, CNetClientCustomPacket((void*)&OnBuild, nullptr));
        sDC.AddPacketHandler(0x632, CNetClientCustomPacket((void*)&OnActiveBuild, nullptr));
        sDC.AddPacketHandler(0x633, CNetClientCustomPacket((void*)&OnActiveBuilds, nullptr));
        sDC.AddPacketHandler(0x634, CNetClientCustomPacket((void*)&OnOwned, nullptr));
        sDC.AddPacketHandler(0x635, CNetClientCustomPacket((void*)&OnUpvoted, nullptr));
    }

    const AscBindings::Binding kBindings[] = {
        {"C_BuildCreator", "UpdateFilter", UpdateFilter},
        {"C_BuildCreator", "ExportBuild", ExportBuild},
        {"C_BuildEditor", "ExportPendingBuild", ExportPendingBuild},
        {"C_BuildEditor", "ImportBuild", ImportBuild},
        {"C_BuildEditor", "ImportCurrentBuild", ImportCurrentBuild},
        {"C_BuildCreator", "GetNumBuilds", GetNumBuilds},
        {"C_BuildCreator", "GetBuildAtIndex", GetBuildAtIndex},
        {"C_BuildCreator", "GetBuild", GetBuild},
        {"C_BuildEditor", "GetPendingBuild", GetPendingBuild},
        {"C_BuildCreator", "GetActiveBuild", GetActiveBuild},
        {"C_BuildCreator", "GetActiveSpecForBuild", GetActiveSpecForBuild},
        {"C_BuildCreator", "GetNumBookmarkedBuilds", GetNumBookmarkedBuilds},
        {"C_BuildCreator", "GetBookmarkedBuildAtIndex", GetBookmarkedBuildAtIndex},
        {"C_BuildCreator", "IsOwnedBuild", IsOwnedBuild},
        {"C_BuildCreator", "IsUpvotedBuild", IsUpvotedBuild},
        {"C_BuildEditor", "DoesBuildHaveSpellID", DoesBuildHaveSpellID},
        {"C_BuildEditor", "DoesBuildHaveEnchant", DoesBuildHaveEnchant},
        {"C_BuildCreator", "GetSpell", GetSpell},
        {"C_BuildEditor", "GetSpellByID", GetSpellByID},
        {"C_BuildEditor", "GetEssenceForLevel", GetEssenceForLevel},
        {"C_BuildEditor", "GetQualityInfo", GetQualityInfo},
        {"C_BuildEditor", "GetQualityInfoForLevel", GetQualityInfoForLevel},
        {"C_BuildEditor", "EditBuild", EditBuild},
        {"C_BuildEditor", "DiscardPendingBuild", DiscardPendingBuild},
        {"C_BuildEditor", "SetName", SetName},
        {"C_BuildEditor", "SetIcon", SetIcon},
        {"C_BuildEditor", "SetDescription", SetDescription},
        {"C_BuildEditor", "SetCategory", SetCategory},
        {"C_BuildEditor", "SetRoles", SetRoles},
        {"C_BuildEditor", "SetPrimaryStat", SetPrimaryStat},
        {"C_BuildEditor", "SetDifficultyRating", SetDifficultyRating},
        {"C_BuildEditor", "SetComment", SetComment},
        {"C_BuildEditor", "AddSpell", AddSpell},
        {"C_BuildEditor", "CanAddSpell", CanAddSpellL},
        {"C_BuildEditor", "RemoveSpell", RemoveSpell},
        {"C_BuildEditor", "CanRemoveSpell", CanRemoveSpellL},
        {"C_BuildEditor", "SetSpellLevel", SetSpellLevel},
        {"C_BuildEditor", "CanSetSpellLevel", CanSetSpellLevelL},
        {"C_BuildEditor", "SetSpellFlags", SetSpellFlags},
        {"C_BuildEditor", "CanSetSpellFlags", CanSetInt2},
        {"C_BuildEditor", "SetIsCoreAbility", SetIsCoreAbility},
        {"C_BuildEditor", "SetIsOptimalAbility", SetIsOptimalAbility},
        {"C_BuildEditor", "SetIsEmpoweringAbility", SetIsEmpoweringAbility},
        {"C_BuildEditor", "SetIsSynergisticAbility", SetIsSynergisticAbility},
        {"C_BuildEditor", "CanSetIsCoreAbility", CanSetIsCoreAbility},
        {"C_BuildEditor", "CanSetIsOptimalAbility", CanSetIsOptimalAbility},
        {"C_BuildEditor", "CanSetIsEmpoweringAbility", CanSetIsEmpoweringAbility},
        {"C_BuildEditor", "CanSetIsSynergisticAbility", CanSetIsSynergisticAbility},
        {"C_BuildEditor", "CanSetSpellComment", CanSetSpellComment},
        {"C_BuildEditor", "AddRandomEnchant", AddRandomEnchant},
        {"C_BuildEditor", "CanAddRandomEnchant", CanAddRandomEnchant},
        {"C_BuildEditor", "RemoveRandomEnchant", RemoveRandomEnchant},
        {"C_BuildEditor", "CanRemoveRandomEnchant", CanRemoveRandomEnchant},
        {"C_BuildEditor", "SetEnchantLevel", SetEnchantLevel},
        {"C_BuildEditor", "CanSetEnchantLevel", CanSetInt2},
        {"C_BuildEditor", "SetEnchantFlags", SetEnchantFlags},
        {"C_BuildEditor", "SetEnchantStacks", SetEnchantStacks},
        {"C_BuildEditor", "CanSetEnchantFlags", CanSetInt2},
        {"C_BuildEditor", "CanSetRandomEnchantStacks", CanSetInt2},
        {"C_BuildEditor", "CanSetRandomEnchantComment", CanSetRandomEnchantComment},
        {"C_BuildEditor", "AddWeaponType", AddWeaponType},
        {"C_BuildEditor", "CanAddWeaponType", CanAddWeaponType},
        {"C_BuildEditor", "RemoveWeaponType", RemoveWeaponType},
        {"C_BuildEditor", "CanRemoveWeaponType", CanRemoveWeaponType},
        {"C_BuildEditor", "CanSetWeaponTypeComment", CanSetWeaponTypeComment},
        {"C_BuildEditor", "AddArmorType", AddArmorType},
        {"C_BuildEditor", "CanAddArmorType", CanAddArmorType},
        {"C_BuildEditor", "RemoveArmorType", RemoveArmorType},
        {"C_BuildEditor", "CanRemoveArmorType", CanRemoveArmorType},
        {"C_BuildEditor", "CanSetArmorTypeComment", CanSetArmorTypeComment},
        {"C_BuildEditor", "CanPublishBuild", CanPublishBuild},
        {"C_BuildEditor", "PublishBuild", PublishBuild},
        {"C_BuildCreator", "ActivateBuild", ActivateBuild},
        {"C_BuildCreator", "CanActivateBuild", CanActivateBuild},
        {"C_BuildCreator", "DeactivateBuild", DeactivateBuild},
        {"C_BuildCreator", "CanDeactivateBuild", CanActivateBuild},
        {"C_BuildCreator", "RateBuild", RateBuild},
        {"C_BuildCreator", "DeleteBuild", DeleteBuild},
        {"C_BuildCreator", "QueryAllBuilds", QueryAllBuilds},
        {"C_BuildCreator", "QueryBuild", QueryBuild},
        {"C_BuildCreator", "BookmarkBuild", BookmarkBuild},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

std::string UrlDecode(const std::string& in) { return UrlDecodeImpl(in); }
std::string DecodeLink(const std::string& url) { return Inflate(Base64Decode(UrlDecodeImpl(url))); }

AscCA::BuildSeed SeedFromLink(const std::string& url)
{
    Entry e;
    ImportLink(e, url);
    return SeedOf(e);
}

bool SeedOfBuild(const std::string& id, AscCA::BuildSeed& out)
{
    const Entry* e = FindBuild(id);
    if (!e)
        return false;
    out = SeedOf(*e);
    return true;
}

bool SpecHasActiveBuild(int spec)
{
    return spec >= 0 && static_cast<size_t>(spec) < g.specs.size() && !g.specs[spec].id.empty();
}

std::string ExportBuildOf(const AscCA::Build& b, bool enchants) { return ExportLink(EntryFromBuild(b, enchants)); }

bool PendingCanAddSpell(uint32_t spell)
{
    Spell s;
    s.spell = spell;
    s.level = AscGameEvents::ServerMaxLevel();
    return CanAddSpell(s).empty();
}

bool PendingCanRemoveSpell(uint32_t spell) { return CanRemoveSpell(spell).empty(); }

bool PendingHasSpell(uint32_t spell) { return PendingSpell(spell) != nullptr; }
}
