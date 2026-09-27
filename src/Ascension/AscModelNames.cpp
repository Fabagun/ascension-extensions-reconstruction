// Model name cache -- PopulateModelNameCache (FUN_10a701b0) and GetModelNameCompletions (FUN_10a6ef50), the
// original's model-path autocomplete.
//
// PopulateModelNameCache() rebuilds the path list (0x10D3D73C) from four tables, each path once (deduped on
// the path as stored), with '\' turned into '/':
//   client container 0xAD3500 +0x08, 0xAD3938 +0x04, 0xAD4A18 +0x08 -- ids 0 .. max - 1 (the original's
//     loops stop BEFORE max; kept),
//   the DLL's ItemDisplayInfoCollections.dbc +0x10 -- ids min .. max.
// Returns 1 with nothing pushed.
//
// GetModelNameCompletions(query, limit): the query splits on single spaces; tokens shorter than 3 characters
// are ignored, a token in double quotes ("...", at least 3 characters with the quotes) is a quoted term, the
// rest are plain terms. Each cached path is scored by how many terms it contains (case-sensitive substring);
// when there are quoted terms at least one of them must match, and the path is kept when anything matched.
// The kept paths are sorted by score, highest first (std::sort, not stable), cut to `limit`, and returned
// as an array.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbc.hpp>
#include <Ascension/AscScript.hpp>
#include <algorithm>
#include <string>
#include <unordered_set>
#include <vector>

using namespace AscScript;

namespace
{
    std::vector<std::string> g_paths;   // 0x10D3D73C

    int PopulateModelNameCache(lua_State*)
    {
        g_paths.clear();
        std::unordered_set<std::string> seen;
        auto add = [&](const char* path) {
            if (!path)
                return;
            if (!seen.insert(path).second)
                return;
            std::string normal = path;
            std::replace(normal.begin(), normal.end(), '\\', '/');
            g_paths.push_back(normal);
        };
        struct { uint32_t container, field; } const kClient[] = {{0xAD3500, 8}, {0xAD3938, 4}, {0xAD4A18, 8}};
        for (const auto& t : kClient)
        {
            const uint32_t max = *reinterpret_cast<const uint32_t*>(t.container + 0xC);
            for (uint32_t id = 0; id < max; ++id)
                if (const uint8_t* row = ClientDbcRow(t.container, id))
                    add(*reinterpret_cast<const char* const*>(row + t.field));
        }
        AscDbc::Table& collections = AscDbc::Get("DBFilesClient\\ItemDisplayInfoCollections.dbc");
        const uint32_t max = collections.MaxId();
        for (uint32_t id = collections.MinId(); id < max + 1; ++id)
            if (const uint8_t* row = collections.Row(id))
                add(collections.Str(row, 0x10));
        return 1;
    }

    int GetModelNameCompletions(lua_State* L)
    {
        const std::string query = CheckString(L, 1);
        const uint32_t limit = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
        std::vector<std::string> tokens;
        std::string current;
        for (char c : query)
        {
            if (c == ' ')
            {
                tokens.push_back(current);
                current.clear();
            }
            else
                current += c;
        }
        if (!current.empty())
            tokens.push_back(current);

        std::vector<std::string> quoted, plain;
        for (const std::string& t : tokens)
        {
            if (t.size() < 3)
                continue;
            if (t.front() == '"' && t.back() == '"')
                quoted.push_back(t.substr(1, t.size() - 2));
            else
                plain.push_back(t);
        }

        struct Match { std::string path; uint32_t score; };
        std::vector<Match> matches;
        auto contains = [](const std::string& path, const std::string& term) {
            return term.size() <= path.size() && (term.empty() || path.find(term) != std::string::npos);
        };
        for (const std::string& path : g_paths)
        {
            uint32_t score = 0;
            bool any = false;
            for (const std::string& q : quoted)
                if (contains(path, q))
                {
                    ++score;
                    any = true;
                }
            if (!quoted.empty() && !any)
                continue;
            for (const std::string& p : plain)
                if (contains(path, p))
                {
                    ++score;
                    any = true;
                }
            if (any)
                matches.push_back({path, score});
        }
        std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
        if (limit < matches.size())
            matches.resize(limit);

        AscLua::lua_createtable(L, 0, 0);
        AscLua::lua_checkstack(L, 2);
        int32_t index = 1;
        for (const Match& m : matches)
        {
            PushInt(L, index++);
            PushStr(L, m.path.c_str());
            AscLua::lua_settable(L, -3);
        }
        return 1;
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "PopulateModelNameCache", PopulateModelNameCache},
        {nullptr, "GetModelNameCompletions", GetModelNameCompletions},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]));
}
