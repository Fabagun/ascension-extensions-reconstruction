#include <Ascension/AscDbc.hpp>
#include <Ascension/AscLog.hpp>
#include <map>
#include <cstring>

namespace AscDbc
{
namespace
{
    typedef int(__stdcall* SFileOpen_t)(void*, const char*, uint32_t, void**);
    typedef int(__stdcall* SFileRead_t)(void*, void*, uint32_t, uint32_t*, void*, uint32_t);
    typedef int(__stdcall* SFileClose_t)(void*);

    // tools/dbc_layouts.py: tables whose row parser collapses localized strings.
    struct Layout { const char* path; const char* spec; };
    const Layout kLayouts[] = {
#include <Ascension/AscDbcLayouts.generated.inc>
    };

    // Separators compare equal: a table keeps whichever spelling first asked for it, and the layout list
    // spells each name the way the DLL's container table does. A plain == left every table requested
    // with the other spelling unconverted (raw file rows), so its second localized field read a locale
    // column of the first.
    const char* LayoutFor(const std::string& path)
    {
        auto same = [](const std::string& a, const char* b) {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i)
            {
                const char x = a[i] == '/' ? '\\' : a[i], y = b[i] == '/' ? '\\' : b[i];
                if (x != y)
                    return false;
            }
            return i == a.size() && !b[i];
        };
        for (const Layout& l : kLayouts)
            if (same(path, l.path))
                return l.spec;
        return nullptr;
    }

    // FUN_101b0c80: which of the 16 locale columns this client reads ([0xC5DE9C], jump table 0x101b0d38).
    uint32_t LocaleColumn()
    {
        static const uint32_t kMap[] = {0, 2, 3, 4, 6, 8, 9, 10, 11};
        const uint32_t v = *reinterpret_cast<const uint32_t*>(0xC5DE9C);
        return v <= 8 ? kMap[v] : 15;
    }

    bool Read(void* h, void* buf, uint32_t n)
    {
        return n == 0 || reinterpret_cast<SFileRead_t>(0x422530)(h, buf, n, nullptr, nullptr, 0) != 0;
    }
}

Table::Table(const char* path) : m_path(path) {}

void Table::Load()
{
    if (m_tried)
        return;
    m_tried = true;

    void* h = nullptr;
    if (!reinterpret_cast<SFileOpen_t>(0x424B50)(nullptr, m_path.c_str(), 0x20000, &h) || !h)
    {
        // Get() shares one table between both spellings, so the first caller's separator is the one
        // tried. The archives do not treat '/' and '\' alike (GlobalStrings.dbc opens only as
        // "DBFilesClient\GlobalStrings.dbc"), so the other spelling is tried before giving up.
        std::string other(m_path);
        for (char& c : other)
            c = c == '/' ? '\\' : c == '\\' ? '/' : c;
        h = nullptr;
        if (!reinterpret_cast<SFileOpen_t>(0x424B50)(nullptr, other.c_str(), 0x20000, &h) || !h)
        {
            // Not cached as a failure: some DBCs live in archives mounted after attach (GlobalStrings.dbc
            // is asked for at the first glue registration), so a later call tries again.
            if (!m_openLogged)
                AscLog::Printf("Unable to open %s (will retry)", m_path.c_str());
            m_openLogged = true;
            m_tried = false;
            return;
        }
    }

    uint32_t sig = 0, cols = 0, strSize = 0;
    bool ok = Read(h, &sig, 4) && sig == 0x43424457
           && Read(h, &m_count, 4) && Read(h, &cols, 4) && Read(h, &m_rowSize, 4) && Read(h, &strSize, 4);
    if (ok && m_count == 0)
        AscLog::Printf("%s has no records", m_path.c_str());
    if (ok && m_count)
    {
        m_rows.resize(static_cast<size_t>(m_count) * m_rowSize);
        m_strings.resize(strSize + 1, '\0');
        ok = Read(h, m_rows.data(), static_cast<uint32_t>(m_rows.size())) && Read(h, m_strings.data(), strSize);
    }
    reinterpret_cast<SFileClose_t>(0x422910)(h);
    if (!ok || !m_count || m_rowSize < 4)
    {
        if (!ok)
            AscLog::Printf("%s: malformed WDBC", m_path.c_str());
        return;
    }

    // The DLL's struct is the file row with each localized block (16 locale offsets + flags) reduced
    // to the offset for this client's locale. Byte fields ('b') are packed in the file but the struct
    // keeps natural alignment, so a dword after them starts on the next 4-byte boundary.
    if (const char* spec = LayoutFor(m_path))
    {
        uint32_t structSize = 0;
        for (const char* c = spec; *c; ++c)
            structSize = (*c == 'b') ? structSize + 1 : ((structSize + 3) & ~3u) + 4;
        structSize = (structSize + 3) & ~3u;
        std::vector<uint8_t> converted(static_cast<size_t>(m_count) * structSize);
        const uint32_t loc = LocaleColumn();
        for (uint32_t i = 0; i < m_count; ++i)
        {
            const uint8_t* src = &m_rows[static_cast<size_t>(i) * m_rowSize];
            uint8_t* row = &converted[static_cast<size_t>(i) * structSize];
            uint32_t at = 0;
            for (const char* c = spec; *c; ++c)
            {
                if (*c == 'b') { row[at++] = *src++; continue; }
                at = (at + 3) & ~3u;
                if (*c == 'L') { memcpy(row + at, src + loc * 4, 4); src += 68; }
                else           { memcpy(row + at, src, 4); src += 4; }
                at += 4;
            }
        }
        m_rows.swap(converted);
        m_rowSize = structSize;
    }

    // FUN_101feb50: maxId starts at 0, minId at 0x0FFFFFFF.
    m_max = 0;
    m_min = 0x0FFFFFFF;
    for (uint32_t i = 0; i < m_count; ++i)
    {
        const uint32_t id = U32(&m_rows[static_cast<size_t>(i) * m_rowSize], 0);
        if (id > m_max) m_max = id;
        if (id < m_min) m_min = id;
    }
    m_index.assign(static_cast<size_t>(m_max - m_min) + 1, nullptr);
    for (uint32_t i = 0; i < m_count; ++i)
    {
        const uint8_t* row = &m_rows[static_cast<size_t>(i) * m_rowSize];
        m_index[U32(row, 0) - m_min] = row;
    }
    m_ok = true;
    AscLog::Printf("AscDbc: %s %u rows, %u cols, ids %u..%u", m_path.c_str(), m_count, cols, m_min, m_max);
}

const uint8_t* Table::Row(uint32_t id)
{
    Load();
    if (!m_ok || id < m_min || id > m_max)
        return nullptr;
    return m_index[id - m_min];
}

uint32_t Table::Count()
{
    Load();
    return m_ok ? m_count : 0;
}

const uint8_t* Table::RowAt(uint32_t i)
{
    Load();
    return (m_ok && i < m_count) ? &m_rows[static_cast<size_t>(i) * m_rowSize] : nullptr;
}

const char* Table::Str(const uint8_t* row, uint32_t off)
{
    const uint32_t o = U32(row, off);
    return (o < m_strings.size()) ? &m_strings[o] : "";
}

uint32_t Table::AddString(const char* s)
{
    Load();
    const uint32_t off = static_cast<uint32_t>(m_strings.size());
    m_strings.insert(m_strings.end(), s, s + strlen(s) + 1);
    return off;
}

// The original's patch handlers copy over an existing row where it lies (so file-order walks such as
// FUN_101ceee0 see the change) and insert a new one through FUN_10133ac0, which only touches the index.
// Growing the index there copies ids min..max-1: the row that held the old maximum is dropped
// (IMPROVEMENTS.md), kept here.
void Table::Upsert(uint32_t id, const std::vector<uint8_t>& row)
{
    Load();
    if (const uint8_t* existing = Row(id))
    {
        size_t n = row.size();
        for (const std::vector<uint8_t>* p : m_patched)
            if (p->data() == existing)
                n = n < p->size() ? n : p->size();
        if (existing >= m_rows.data() && existing < m_rows.data() + m_rows.size())
            n = n < m_rowSize ? n : m_rowSize;
        if (n < row.size())
            AscLog::Printf("AscDbc: %s row %u patched in place, %u of %u bytes fit", m_path.c_str(), id,
                           static_cast<unsigned>(n), static_cast<unsigned>(row.size()));
        memcpy(const_cast<uint8_t*>(existing), row.data(), n);
        return;
    }
    auto* copy = new std::vector<uint8_t>(row);
    m_patched.push_back(copy);
    if (!m_ok)
    {
        m_ok = true;
        m_min = m_max = id;
        m_index.assign(1, nullptr);
    }
    if (id < m_min || id > m_max)
    {
        const uint32_t lo = id < m_min ? id : m_min, hi = id > m_max ? id : m_max;
        std::vector<const uint8_t*> grown(static_cast<size_t>(hi - lo) + 1, nullptr);
        for (uint32_t i = m_min; i < m_max; ++i)
            grown[i - lo] = m_index[i - m_min];
        m_index.swap(grown);
        m_min = lo;
        m_max = hi;
    }
    m_index[id - m_min] = copy->data();
}

Table& Get(const char* path)
{
    // One instance per file: "DBFilesClient/X.dbc" and "DBFilesClient\\X.dbc" are the same table
    // (the original has one static container per file, whatever spelling its callers use).
    static std::map<std::string, Table*> tables;
    std::string key(path);
    for (char& c : key)
        if (c == '/')
            c = '\\';
    Table*& t = tables[key];
    if (!t)
        t = new Table(path);
    return *t;
}
}
