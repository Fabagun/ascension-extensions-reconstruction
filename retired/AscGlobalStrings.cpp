// Ascension keeps its GlobalStrings in a DBC, not only in Interface\FrameXML\GlobalStrings.lua.
//
// Measured 2026-09-22 from FrameXML.log: MELEE_DAMAGER and TUTORIAL_REWARD_PENDING are absent from
// GlobalStrings.lua entirely, yet present in DBFilesClient\GlobalStrings.dbc (14,358 rows). Others
// such as ITEM_REQ_ARENA_RATING and ERR_LEARN_SPELL_S are in the .lua and still read nil in world.
// So the real Extensions.dll defines these globals from the DBC at runtime, and we must too --
// executing the .lua alone leaves a long tail of nil-string errors.
//
// Layout (20 fields), from the survey in OniWoW/server/tools/p7_13_globalstrings_dbc.py:
//   [0] ID   [1] flags   [2] BaseTag (the global's name)   [3..18] 16 locale strings   [19] locale flags
// Every field is an offset into the trailing string block. We take the tag from [2] and the first
// non-empty locale from [3..18], exactly as that script does.
//
// Read through SFile (the client's own archive API) rather than a loose file, so this always
// reflects the archives the client actually loaded -- the client patches itself often.

#include <Ascension/AscLua.hpp>
#include <Ascension/AscLog.hpp>
#include <Client/SFile.hpp>

#include <Windows.h>
#include <cstdint>
#include <cstdlib>

using namespace AscLua;

namespace
{
    const uint32_t WDBC_MAGIC = 0x43424457;     // 'WDBC' little-endian
    const int TAG_FIELD = 2;
    const int FIRST_LOCALE_FIELD = 3;
    const int LAST_LOCALE_FIELD = 18;
}

void LoadGlobalStringsDbc(lua_State* L)
{
    if (!L)
        return;

    HANDLE file = nullptr;
    if (!SFile::OpenFileEx(nullptr, "DBFilesClient\\GlobalStrings.dbc", 0x20000, &file) || !file)
    {
        AscLog::Printf("GlobalStrings.dbc: not present in the archives; skipping");
        return;
    }

    uint32_t header[5] = {};   // magic, recordCount, fieldCount, recordSize, stringBlockSize
    if (!SFile::ReadFile(file, header, sizeof(header), nullptr, nullptr, 0) || header[0] != WDBC_MAGIC)
    {
        AscLog::Printf("GlobalStrings.dbc: bad header (magic 0x%08X)", header[0]);
        SFile::CloseFile(file);
        return;
    }

    const uint32_t records = header[1], fields = header[2], recordSize = header[3], stringSize = header[4];
    if (!records || fields <= (uint32_t)LAST_LOCALE_FIELD || recordSize < fields * 4 || !stringSize)
    {
        AscLog::Printf("GlobalStrings.dbc: unexpected shape (%u rows x %u fields, rowSize %u, strings %u) -- "
                       "layout assumption is ID/flags/BaseTag/16 locales/flags; NOT loaded",
                       records, fields, recordSize, stringSize);
        SFile::CloseFile(file);
        return;
    }

    uint8_t* rows = (uint8_t*)malloc((size_t)records * recordSize);
    char* strings = (char*)malloc(stringSize);
    bool ok = rows && strings
        && SFile::ReadFile(file, rows, records * recordSize, nullptr, nullptr, 0)
        && SFile::ReadFile(file, strings, stringSize, nullptr, nullptr, 0);
    SFile::CloseFile(file);

    if (!ok)
    {
        AscLog::Printf("GlobalStrings.dbc: read failed");
        free(rows); free(strings);
        return;
    }
    strings[stringSize - 1] = 0;        // the block is NUL-terminated, but do not trust the file

    uint32_t defined = 0;
    for (uint32_t r = 0; r < records; ++r)
    {
        const uint32_t* rec = (const uint32_t*)(rows + (size_t)r * recordSize);

        const uint32_t tagOff = rec[TAG_FIELD];
        if (tagOff >= stringSize)
            continue;
        const char* tag = strings + tagOff;
        if (!*tag)
            continue;

        const char* text = "";
        for (int i = FIRST_LOCALE_FIELD; i <= LAST_LOCALE_FIELD; ++i)
        {
            const uint32_t off = rec[i];
            if (off && off < stringSize && strings[off])
            {
                text = strings + off;
                break;
            }
        }

        lua_pushstring(L, text);
        lua_setfield(L, LUA_GLOBALSINDEX, tag);
        ++defined;
    }

    free(rows);
    free(strings);
    AscLog::Printf("GlobalStrings.dbc: defined %u globals (of %u rows)", defined, records);
}
