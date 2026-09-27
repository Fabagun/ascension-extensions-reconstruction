#include <Ascension/AscOpcodeParse.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/CDataStore.hpp>

#include <cstring>

namespace
{
    // Bounds-checked cursor over the CDataStore's own buffer. We read the buffer directly rather
    // than through CDataStore::GetIntN because the layouts need f32, length-prefixed strings and
    // fixed byte runs, and because a malformed layout must fail cleanly instead of walking off
    // the end of a network packet.
    struct Cursor
    {
        const uint8_t* base;
        int32_t        size;
        int32_t        pos;
        bool           overflow;

        bool take(int32_t n, const uint8_t** out)
        {
            if (n < 0 || pos + n > size) { overflow = true; return false; }
            if (out) *out = base + pos;
            pos += n;
            return true;
        }
        bool u32(uint32_t* v)
        {
            const uint8_t* p;
            if (!take(4, &p)) return false;
            memcpy(v, p, 4);
            return true;
        }
    };

    // Count fields resolved by name for repeat blocks. Layouts are small; linear scan is fine.
    struct Counts
    {
        static const int MAX = 16;
        const char* names[MAX];
        uint32_t    values[MAX];
        int         n;

        void put(const char* name, uint32_t v)
        {
            if (!name || n >= MAX) return;
            names[n] = name; values[n] = v; ++n;
        }
        bool get(const char* name, uint32_t* out) const
        {
            if (!name) return false;
            for (int i = 0; i < n; ++i)
                if (strcmp(names[i], name) == 0) { *out = values[i]; return true; }
            return false;
        }
    };

    // Reads one scalar/blob field. Returns false on overflow or an unparseable type.
    bool ReadField(const AscField& f, Cursor& c, Counts& counts)
    {
        const uint8_t* p = nullptr;
        switch (f.type)
        {
        case FT_U8:  { if (!c.take(1, &p)) return false; counts.put(f.name, *p); return true; }
        case FT_U16: { if (!c.take(2, &p)) return false; uint16_t v; memcpy(&v, p, 2); counts.put(f.name, v); return true; }
        case FT_U32:
        case FT_I32: { uint32_t v; if (!c.u32(&v)) return false; counts.put(f.name, v); return true; }
        case FT_U64: return c.take(8, nullptr);
        case FT_F32: return c.take(4, nullptr);
        case FT_BYTES: return c.take((int32_t)f.size, nullptr);
        case FT_LPSTRING:
        {
            uint32_t len;
            if (!c.u32(&len)) return false;
            if (len > (uint32_t)c.size) { c.overflow = true; return false; }   // absurd length
            return c.take((int32_t)len, nullptr);
        }
        case FT_CSTRING:
        {
            while (true)
            {
                if (!c.take(1, &p)) return false;
                if (*p == 0) return true;
            }
        }
        case FT_PACKED_GUID:
        {
            if (!c.take(1, &p)) return false;
            uint8_t mask = *p, extra = 0;
            for (int i = 0; i < 8; ++i) if (mask & (1 << i)) ++extra;
            return c.take(extra, nullptr);
        }
        case FT_BYTES_VAR: return c.take(c.size - c.pos, nullptr);   // consumes the remainder
        case FT_UNKNOWN:   return false;
        default:           return false;
        }
    }
}

void AscOpcodeParse(const AscOpcodeDesc& desc, CDataStore* ds)
{
    // The dispatcher has already consumed the 2-byte opcode, so m_read is the payload start.
    Cursor c{ reinterpret_cast<const uint8_t*>(ds->m_buffer), ds->m_size, ds->m_read, false };
    const int32_t payloadStart = c.pos;
    Counts counts{};
    counts.n = 0;

    bool ok = true;
    bool truncated = false;

    for (uint16_t i = 0; i < desc.fieldCount && ok; ++i)
    {
        const AscField& f = desc.fields[i];

        if (f.type == FT_REPEAT_END)
            continue;

        if (f.type == FT_REPEAT_BEGIN)
        {
            // find the matching REPEAT_END (repeat blocks are not nested in the current atlas,
            // but handle nesting defensively by depth counting)
            uint16_t depth = 1, end = i + 1;
            for (; end < desc.fieldCount && depth; ++end)
            {
                if (desc.fields[end].type == FT_REPEAT_BEGIN) ++depth;
                else if (desc.fields[end].type == FT_REPEAT_END) --depth;
            }
            --end;      // index of the matching REPEAT_END

            uint32_t times = 0;
            if (!f.repeatCount)
                times = f.size;                 // literal iteration count
            else if (!counts.get(f.repeatCount, &times))
            {
                AscLog::Printf("AscOpcodes: %s (0x%03X) repeat count %s not found; stopping",
                    desc.name, desc.opcode, f.repeatCount ? f.repeatCount : "(null)");
                ok = false;
                break;
            }
            for (uint32_t r = 0; r < times && ok; ++r)
                for (uint16_t j = i + 1; j < end && ok; ++j)
                {
                    if (desc.fields[j].type == FT_UNKNOWN) { ok = false; truncated = true; break; }
                    ok = ReadField(desc.fields[j], c, counts);
                }
            i = end;
            continue;
        }

        if (f.type == FT_UNKNOWN) { truncated = true; ok = false; break; }
        ok = ReadField(f, c, counts);
    }

    const int32_t payloadLen = ds->m_size - payloadStart;
    const int32_t consumed = c.pos - payloadStart;

    if (!ok && c.overflow)
        AscLog::Printf("AscOpcodes: %s (0x%03X) layout=%s OVERRAN payload (%d bytes) -- atlas layout is wrong",
            desc.name, desc.opcode, desc.layoutState, payloadLen);
    else if (truncated)
        AscLog::Printf("AscOpcodes: %s (0x%03X) layout=%s incomplete (unknown field), %d/%d bytes read",
            desc.name, desc.opcode, desc.layoutState, consumed, payloadLen);
    else if (!ok)
        AscLog::Printf("AscOpcodes: %s (0x%03X) layout=%s parse failed at %d/%d bytes",
            desc.name, desc.opcode, desc.layoutState, consumed, payloadLen);
    else if (consumed != payloadLen)
        AscLog::Printf("AscOpcodes: %s (0x%03X) layout=%s MISMATCH consumed %d of %d bytes",
            desc.name, desc.opcode, desc.layoutState, consumed, payloadLen);
    else
        AscLog::Printf("AscOpcodes: %s (0x%03X) layout=%s VERIFIED %d bytes",
            desc.name, desc.opcode, desc.layoutState, payloadLen);

    // Always finish the packet exactly once, exactly like the native no-handler path.
    CDataStore::IsRead(ds);
}
