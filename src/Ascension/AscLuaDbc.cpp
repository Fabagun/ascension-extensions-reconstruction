// LuaDBC -- scripts describe a custom DBC's columns and read it from the MPQs:
//   local dbc = CreateDBC("Foo.dbc")          -- userdata, metatable "LuaDBC" from the registry
//   SetInt32(dbc, "A") SetUInt32 SetFloat SetString SetLocalizedString   -- column types i u f s l
//   Load(dbc)  GetRecord(dbc, id)  GetRecordByColumn(dbc, col, v)  GetRecordsByColumn(dbc, col, v)
// Only CreateDBC is global; the rest are methods of the "LuaDBC" metatable (FUN_1030efe0).
// Transcribed from FUN_1030c720 (CreateDBC), FUN_1030e730/eb20/e830/ec20/ea20 (Set*), handler_Load,
// FUN_1030ca70 (GetRecord), FUN_1030cdd0 / handler_GetRecordsByColumn. Schemas live in a map keyed by
// name (FUN_1030c320) and are never freed; CreateDBC on a known name resets its columns.
//
// Kept from the original: a localized column stores 16 values but a record's values are indexed by
// column number, so a column after an "l" reads the wrong slots; duplicate record ids append their
// values to the first record. Record iteration is file order (the original walks its hash list).
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace AscScript;

namespace
{
    constexpr int kRegistry = -10000;
    typedef void* (__cdecl* NewUserdata_t)(lua_State*, size_t);
    typedef void* (__cdecl* ToUserdata_t)(lua_State*, int);
    typedef void  (__cdecl* RawGet_t)(lua_State*, int);
    typedef int   (__cdecl* SetMetatable_t)(lua_State*, int);
    const NewUserdata_t  lua_newuserdata  = reinterpret_cast<NewUserdata_t>(0x84F0F0);
    const ToUserdata_t   lua_touserdata   = reinterpret_cast<ToUserdata_t>(0x84E1C0);
    const RawGet_t       lua_rawget       = reinterpret_cast<RawGet_t>(0x84E600);
    const SetMetatable_t lua_setmetatable = reinterpret_cast<SetMetatable_t>(0x84EA90);

    typedef int(__stdcall* SFileOpen_t)(void*, const char*, uint32_t, void**);
    typedef int(__stdcall* SFileRead_t)(void*, void*, uint32_t, uint32_t*, void*, uint32_t);
    typedef int(__stdcall* SFileClose_t)(void*);

    // DAT_10b5b670; the 16th slot is NULL and keys as "".
    const char* const kLocales[16] = {"enUS", "enGB", "koKR", "frFR", "deDE", "enCN", "zhCN", "enTW",
                                      "esES", "esMX", "ruRU", "ptPT", "ptBR", "itIT", "Unk", ""};

    struct Column { std::string name, type; };
    struct Record { uint32_t id = 0; std::vector<uint32_t> values; };
    struct Schema
    {
        bool created = false;                       // +0x00
        std::string path;                           // +0x04
        std::vector<Column> columns;                // +0x1C
        std::vector<Record> records;                // +0x28 (hash map)
        std::unordered_map<uint32_t, size_t> index;
        std::vector<char> strings;                  // +0x48
        uint32_t columnCount = 0;                   // +0x4C: values per record after the id
    };
    std::unordered_map<std::string, Schema*>& Schemas() { static std::unordered_map<std::string, Schema*> m; return m; }

    // The original dereferences without a check; a non-userdata self returns nothing here instead.
    Schema* Self(lua_State* L)
    {
        Schema** ud = static_cast<Schema**>(lua_touserdata(L, 1));
        return ud ? *ud : nullptr;
    }

    uint32_t Value(const Record& r, size_t i) { return i < r.values.size() ? r.values[i] : 0; }
    const char* Str(const Schema* s, uint32_t off) { return off < s->strings.size() ? s->strings.data() + off : ""; }

    int CreateDBC(lua_State* L)
    {
        const std::string name = CheckString(L, 1);
        Schema*& s = Schemas()[name];
        if (!s)
            s = new Schema();
        if (s->created)
        {
            s->created = false;
            s->columns.clear();
            s->columnCount = 0;
            s->strings.clear();
        }
        s->path = "DBFilesClient\\" + name;
        s->created = true;
        *static_cast<Schema**>(lua_newuserdata(L, 4)) = s;
        AscLua::lua_pushstring(L, "LuaDBC");
        lua_rawget(L, kRegistry);
        if (AscLua::lua_type(L, -1) == 5)
            lua_setmetatable(L, -2);
        else
        {
            AscLua::lua_settop(L, -3);
            AscLua::lua_pushnil(L);
        }
        return 1;
    }

    int AddColumn(lua_State* L, const char* type, uint32_t width, bool rejectId)
    {
        Schema* s = Self(L);
        if (!s)
            return 0;
        const std::string name = CheckString(L, 2);
        if (rejectId && name == "ID")
        {
            const std::string msg = s->path + " has duplicate column name ID";
            AscLua::luaL_error(L, msg.c_str());
            return 0;
        }
        s->columns.push_back({name, type});
        s->columnCount += width;
        return 0;
    }
    int SetInt32(lua_State* L)           { return AddColumn(L, "i", 1, true); }
    int SetUInt32(lua_State* L)          { return AddColumn(L, "u", 1, true); }
    int SetFloat(lua_State* L)           { return AddColumn(L, "f", 1, false); }
    int SetString(lua_State* L)          { return AddColumn(L, "s", 1, false); }
    int SetLocalizedString(lua_State* L) { return AddColumn(L, "l", 17, false); }

    int LoadError(lua_State* L, const char* fmt, const std::string& path)
    {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, path.c_str());
        AscLua::luaL_error(L, buf);
        return 0;
    }

    // handler_Load: WDBC header, then per record an id and each column's values (one u32, or 16 + a
    // discarded flags word for "l"), then the string table. Errors are raised with luaL_error.
    int Load(lua_State* L)
    {
        Schema* s = Self(L);
        if (!s)
            return 0;
        s->records.clear();
        s->index.clear();
        void* h = nullptr;
        if (!reinterpret_cast<SFileOpen_t>(0x424B50)(nullptr, s->path.c_str(), 0x20000, &h) || !h)
            return LoadError(L, "Unable to open %s", s->path);
        auto read = [h](void* p, uint32_t n) { return reinterpret_cast<SFileRead_t>(0x422530)(h, p, n, nullptr, nullptr, 0) != 0; };
        const auto close = [h]() { reinterpret_cast<SFileClose_t>(0x422910)(h); };
        uint32_t sig = 0, count = 0, cols = 0, rowSize = 0, strSize = 0;
        if (!read(&sig, 4))
            return close(), LoadError(L, "Unable to read signature from %s", s->path);
        if (sig != 0x43424457)
            return close(), LoadError(L, "Invalid signature to open %s", s->path);
        if (!read(&count, 4))
            return close(), LoadError(L, "Unable to read record count from %s", s->path);
        if (count == 0)
            return close(), 0;
        if (!read(&cols, 4))
            return close(), LoadError(L, "Unable to read column count from %s", s->path);
        if (cols != s->columnCount + 1)
        {
            char buf[512];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s has wrong number of columns (found %i, expected %i)",
                        s->path.c_str(), static_cast<int>(cols), static_cast<int>(s->columnCount + 1));
            close();
            AscLua::luaL_error(L, buf);
            return 0;
        }
        if (!read(&rowSize, 4))
            return close(), LoadError(L, "Unable to read row size from %s", s->path);
        if (!read(&strSize, 4))
            return close(), LoadError(L, "Unable to string size from %s", s->path);
        for (uint32_t r = 0; r < count; ++r)
        {
            uint32_t id = 0;
            read(&id, 4);
            auto it = s->index.find(id);
            if (it == s->index.end())
            {
                it = s->index.emplace(id, s->records.size()).first;
                s->records.push_back(Record{id, {}});
            }
            Record& rec = s->records[it->second];
            for (const Column& c : s->columns)
            {
                const char t = c.type.size() == 1 ? c.type[0] : 0;
                if (t == 'i' || t == 'u' || t == 'f' || t == 's')
                {
                    uint32_t v = 0;
                    read(&v, 4);
                    rec.values.push_back(v);
                }
                else if (t == 'l')
                {
                    for (int j = 0; j < 16; ++j)
                    {
                        uint32_t v = 0;
                        read(&v, 4);
                        rec.values.push_back(v);
                    }
                    uint32_t flags = 0;
                    read(&flags, 4);
                }
            }
        }
        s->strings.assign(strSize + 1, '\0');
        const bool ok = read(s->strings.data(), strSize);
        close();
        if (!ok)
            return LoadError(L, "%s: Cannot read string table", s->path);
        return 0;
    }

    // { ID = id, [column] = value, ... } -- FUN_1030ca70's record table, shared by the three getters.
    void PushRecord(lua_State* L, const Schema* s, const Record& r)
    {
        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_pushstring(L, "ID");
        AscLua::lua_pushinteger(L, static_cast<int>(r.id));
        AscLua::lua_settable(L, -3);
        for (size_t i = 0; i < s->columns.size(); ++i)
        {
            const Column& c = s->columns[i];
            PushStr(L, c.name.c_str());
            switch (c.type.size() == 1 ? c.type[0] : 0)
            {
            case 'i':
            case 'u':
                AscLua::lua_pushinteger(L, static_cast<int>(Value(r, i)));
                break;
            case 'f':
            {
                const uint32_t bits = Value(r, i);
                float f;
                memcpy(&f, &bits, 4);
                AscLua::lua_pushnumber(L, f);
                break;
            }
            case 's':
                AscLua::lua_pushstring(L, Str(s, Value(r, i)));
                break;
            case 'l':
                AscLua::lua_createtable(L, 0, 0);
                for (size_t j = 0; j < 16; ++j)
                {
                    AscLua::lua_pushstring(L, kLocales[j]);
                    AscLua::lua_pushstring(L, Str(s, Value(r, i + j)));
                    AscLua::lua_settable(L, -3);
                }
                break;
            default:
                AscLua::lua_pushnil(L);
                break;
            }
            AscLua::lua_settable(L, -3);
        }
    }

    int GetRecord(lua_State* L)
    {
        Schema* s = Self(L);
        if (!s)
            return 0;
        const uint32_t id = static_cast<uint32_t>(ToInt(AscLua::lua_tonumber(L, 2)));
        auto it = s->index.find(id);
        if (it == s->index.end())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        PushRecord(L, s, s->records[it->second]);
        return 1;
    }

    // Matching records for (column, value): numbers compare by the column's type ("i" as int,
    // "u" against the truncated number, "f" as float) on the first column of that name; strings
    // compare the string-table text on the last column of that name.
    std::vector<const Record*> Matches(lua_State* L, const Schema* s, bool firstOnly)
    {
        std::vector<const Record*> out;
        const std::string col = CheckString(L, 2);
        if (AscLua::lua_isnumber(L, 3))
        {
            const double num = AscLua::lua_tonumber(L, 3);
            const int32_t asU = ToInt(num);
            size_t c = 0;
            while (c < s->columns.size() && s->columns[c].name != col)
                ++c;
            if (c == s->columns.size())
                return out;
            const std::string& type = s->columns[c].type;
            for (const Record& r : s->records)
            {
                const uint32_t v = Value(r, c);
                float f;
                memcpy(&f, &v, 4);
                const bool hit = (type == "i" && static_cast<int32_t>(v) == static_cast<int32_t>(num))
                              || (type == "u" && static_cast<int32_t>(v) == asU)
                              || (type == "f" && f == static_cast<float>(num));
                if (hit)
                {
                    out.push_back(&r);
                    if (firstOnly)
                        break;
                }
            }
        }
        else if (AscLua::lua_isstring(L, 3))
        {
            const std::string value = CheckString(L, 3);
            size_t c = static_cast<size_t>(-1);
            for (size_t i = 0; i < s->columns.size(); ++i)
                if (s->columns[i].name == col)
                    c = i;
            if (c == static_cast<size_t>(-1))
                return out;
            for (const Record& r : s->records)
                if (value == Str(s, Value(r, c)))
                {
                    out.push_back(&r);
                    if (firstOnly)
                        break;
                }
        }
        return out;
    }

    int GetRecordByColumn(lua_State* L)
    {
        Schema* s = Self(L);
        if (!s)
            return 0;
        const std::vector<const Record*> m = Matches(L, s, true);
        if (m.empty())
            AscLua::lua_pushnil(L);
        else
            PushRecord(L, s, *m[0]);
        return 1;
    }

    int GetRecordsByColumn(lua_State* L)
    {
        Schema* s = Self(L);
        if (!s)
            return 0;
        const std::vector<const Record*> m = Matches(L, s, false);
        if (m.empty())
        {
            AscLua::lua_pushnil(L);
            return 1;
        }
        for (const Record* r : m)
            PushRecord(L, s, *r);
        return static_cast<int>(m.size());
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "CreateDBC", CreateDBC},
        {"@LuaDBC", "GetRecord", GetRecord},
        {"@LuaDBC", "GetRecordByColumn", GetRecordByColumn},
        {"@LuaDBC", "GetRecordsByColumn", GetRecordsByColumn},
        {"@LuaDBC", "Load", Load},
        {"@LuaDBC", "SetFloat", SetFloat},
        {"@LuaDBC", "SetInt32", SetInt32},
        {"@LuaDBC", "SetLocalizedString", SetLocalizedString},
        {"@LuaDBC", "SetString", SetString},
        {"@LuaDBC", "SetUInt32", SetUInt32},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
