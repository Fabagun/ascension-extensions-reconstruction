// Interface hot patches: a supported channel for server-sent interface Lua, and the 0x5BC formatter check
// that closes the route servers used before it. Not in the genuine DLL; built only with ASCENSION_HOT_PATCH.
// Design and threat model: AscensionRebirth/HOT_PATCH_DESIGN.md.
//
// SMSG 0x8F0 HOT_PATCH: sized patchId, sized targetAddon, sized source (u32 length + bytes each).
// CMSG 0x8F1 capability, after every world entry: u32 protocol version, u8 mode (0 off, 1 ask, 2 allow).
// CMSG 0x8F2 result: sized patchId, u8 outcome (Outcome below), sized detail (at most 255 bytes).
//
// A patch is source only (no ESC byte, no `loadstring`, no `string.dump`), at most 32 KB, 128 KB per
// session. It runs once per Lua session per (patchId, content hash): the hash is kept in the Lua global
// AscHotPatchApplied, which a /reload or relog clears. An empty target runs at once with no taint source,
// the attribution the 0x5BC route measured on 2026-10-17 (issecurevariable of a pushed global: secure). A
// named target runs at that addon's ADDON_LOADED (or at once if loaded) with the addon's name as the taint
// source, so it gets exactly the trust SMSG 0x94E gave that addon. The chunk may return "applied" or
// "skipped: <reason>"; otherwise "ran" or "error".
//
// The player's choice is in WTF\HotPatch.txt: "mode=0|1|2" and one "realm=<name>=<0|1>" line per answered
// realm. Without the file patches are allowed; /hotpatch off refuses them, /hotpatch ask asks once per realm,
// /hotpatch on allows them again. /hotpatch alone lists this session's patches.
//
// 0x5BC (ChallengeRequirementTypes): a formatter (fields 6-8) must be one `return <expression>` built from
// `value`, literals, operators and calls to the seven functions the shipped table uses. Anything else drops
// the row and the client keeps the one it had.
#include <PatchConfig.hpp>
#if ASCENSION_HOT_PATCH
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscDbcPatch.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscLua.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <windows.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <deque>
#include <set>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    constexpr uint32_t kProtocolVersion = 1;
    constexpr size_t kMaxPatch = 32 * 1024;
    constexpr size_t kMaxSession = 128 * 1024;
    constexpr const char* kSettingsFile = "WTF\\HotPatch.txt";

    enum Outcome : uint8_t
    {
        Ran, Applied, Deferred, Skipped, Duplicate, RefusedByPlayer, RefusedGuard, Error
    };
    const char* const kOutcomeNames[] = {
        "ran", "applied", "deferred", "skipped", "duplicate", "refused-by-player", "refused-guard", "error"
    };

    enum Mode : uint8_t { Off, Ask, Allow };

    struct Patch
    {
        std::string id, target, source;
        uint64_t hash = 0;
    };

    struct Record
    {
        std::string id, target, detail;
        size_t size = 0;
        uint64_t hash = 0;
        Outcome outcome = Ran;
    };

    std::vector<Record> g_records;
    std::deque<Patch> g_awaitingConsent;
    std::vector<Patch> g_awaitingAddon;
    size_t g_sessionBytes = 0;
    bool g_promptShown = false;

    uint64_t Hash(const std::string& s)
    {
        uint64_t h = 1469598103934665603ull;
        for (unsigned char c : s)
            h = (h ^ c) * 1099511628211ull;
        return h;
    }

    std::string HexHash(uint64_t h)
    {
        char buf[17];
        snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
        return buf;
    }


    struct Settings
    {
        Mode mode = Allow;
        std::vector<std::pair<std::string, bool>> realms;
    };

    Settings LoadSettings()
    {
        Settings s;
        FILE* f = fopen(kSettingsFile, "r");
        if (!f)
            return s;
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
                l.pop_back();
            if (l.rfind("mode=", 0) == 0)
            {
                const int m = atoi(l.c_str() + 5);
                s.mode = m <= 0 ? Off : m >= 2 ? Allow : Ask;
            }
            else if (l.rfind("realm=", 0) == 0)
            {
                const size_t eq = l.rfind('=');
                if (eq > 6)
                    s.realms.emplace_back(l.substr(6, eq - 6), l.substr(eq + 1) == "1");
            }
        }
        fclose(f);
        return s;
    }

    void SaveSettings(const Settings& s)
    {
        FILE* f = fopen(kSettingsFile, "w");
        if (!f)
            return;
        fprintf(f, "mode=%d\n", static_cast<int>(s.mode));
        for (const auto& [realm, allowed] : s.realms)
            fprintf(f, "realm=%s=%d\n", realm.c_str(), allowed ? 1 : 0);
        fclose(f);
    }


    typedef int (__cdecl* LoadBuffer_t)(lua_State*, const char*, size_t, const char*);
    typedef int (__cdecl* PCall_t)(lua_State*, int, int, int);
    const LoadBuffer_t luaL_loadbuffer = reinterpret_cast<LoadBuffer_t>(0x84F860);   // called by 0x819210
    const PCall_t lua_pcall = reinterpret_cast<PCall_t>(0x84EC50);

    // FrameScript_Execute (0x819210) keeps the running addon -- the taint source -- at 0xD4139C, sets it from
    // its third argument around the call and restores it after. A patch does the same.
    const char* Taint() { return *reinterpret_cast<const char* const*>(0xD4139C); }
    void SetTaint(const char* name)
    {
        DWORD old;
        const BOOL ok = VirtualProtect(reinterpret_cast<void*>(0xD4139C), 4, PAGE_EXECUTE_READWRITE, &old);
        *reinterpret_cast<const char**>(0xD4139C) = name;
        if (ok)
            VirtualProtect(reinterpret_cast<void*>(0xD4139C), 4, old, &old);
    }

    // Taint sources are kept by pointer in tainted values, so the names live for the whole process.
    const char* InternName(const std::string& name)
    {
        static std::set<std::string> names;
        return names.insert(name).first->c_str();
    }

    std::string CallString(const char* fn, const char* arg = nullptr)
    {
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        std::string out;
        AscLua::lua_getfield(L, LUA_GLOBALSINDEX, fn);
        if (AscLua::lua_type(L, -1) == LUA_TFUNCTION)
        {
            if (arg)
                AscLua::lua_pushstring(L, arg);
            if (lua_pcall(L, arg ? 1 : 0, 1, 0) == 0)
            {
                if (AscLua::lua_type(L, -1) == LUA_TSTRING)
                    out = AscLua::tostring(L, -1);
                else if (AscLua::lua_toboolean(L, -1))
                    out = "1";
            }
        }
        AscLua::lua_settop(L, top);
        return out;
    }

    std::string RealmName() { return CallString("GetRealmName"); }
    bool AddonLoaded(const std::string& name) { return !CallString("IsAddOnLoaded", name.c_str()).empty(); }

    // AscHotPatchApplied[id] = hash, a Lua global so it lasts exactly one Lua session.
    bool AlreadyRan(const Patch& p)
    {
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        bool ran = false;
        AscLua::lua_getfield(L, LUA_GLOBALSINDEX, "AscHotPatchApplied");
        if (AscLua::lua_type(L, -1) == LUA_TTABLE)
        {
            AscLua::lua_getfield(L, -1, p.id.c_str());
            ran = AscLua::lua_type(L, -1) == LUA_TSTRING && HexHash(p.hash) == AscLua::tostring(L, -1);
        }
        AscLua::lua_settop(L, top);
        return ran;
    }

    void MarkRan(const Patch& p)
    {
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        AscLua::lua_getfield(L, LUA_GLOBALSINDEX, "AscHotPatchApplied");
        if (AscLua::lua_type(L, -1) != LUA_TTABLE)
        {
            AscLua::lua_settop(L, top);
            AscLua::lua_createtable(L, 0, 4);
            AscLua::lua_pushvalue(L, -1);
            AscLua::lua_setfield(L, LUA_GLOBALSINDEX, "AscHotPatchApplied");
        }
        AscLua::lua_pushstring(L, HexHash(p.hash).c_str());
        AscLua::lua_setfield(L, -2, p.id.c_str());
        AscLua::lua_settop(L, top);
    }


    void PutSized(Packet& packet, const std::string& s)
    {
        packet.U32(static_cast<uint32_t>(s.size()));
        if (!s.empty())
            packet.Data(s.data(), static_cast<uint32_t>(s.size()));
    }

    void Report(const Patch& p, Outcome outcome, std::string detail)
    {
        if (detail.size() > 255)
            detail.resize(255);
        AscLog::Printf("hot patch %s target=%s size=%u hash=%s: %s%s%s", p.id.c_str(),
            p.target.empty() ? "-" : p.target.c_str(), static_cast<unsigned>(p.source.size()), HexHash(p.hash).c_str(),
            kOutcomeNames[outcome], detail.empty() ? "" : " ", detail.c_str());
        g_records.push_back(Record{p.id, p.target, detail, p.source.size(), p.hash, outcome});
        Packet packet(0x8F2);
        PutSized(packet, p.id);
        packet.U8(outcome);
        PutSized(packet, detail);
        packet.Send();
    }


    bool Guard(const Patch& p, std::string& reason)
    {
        if (p.id.empty() || p.source.empty())
            reason = "empty patch id or source";
        else if (p.source.size() > kMaxPatch)
            reason = "larger than 32 KB";
        else if (g_sessionBytes + p.source.size() > kMaxSession)
            reason = "over the 128 KB session limit";
        else if (p.source.find('\x1b') != std::string::npos)
            reason = "contains an ESC byte (bytecode)";
        else if (p.source.find("loadstring") != std::string::npos)
            reason = "calls loadstring";
        else if (p.source.find("string.dump") != std::string::npos)
            reason = "calls string.dump";
        return reason.empty();
    }


    void Run(const Patch& p)
    {
        if (AlreadyRan(p))
        {
            Report(p, Duplicate, "");
            return;
        }
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        const char* saved = Taint();
        SetTaint(p.target.empty() ? nullptr : InternName(p.target));
        const std::string chunk = "=AscHotPatch:" + p.id;
        Outcome outcome = Ran;
        std::string detail;
        if (luaL_loadbuffer(L, p.source.data(), p.source.size(), chunk.c_str()) != 0 ||
            lua_pcall(L, 0, 1, 0) != 0)
        {
            outcome = Error;
            if (const char* e = AscLua::tostring(L, -1))
                detail = e;
        }
        else if (AscLua::lua_type(L, -1) == LUA_TSTRING)
        {
            const std::string r = AscLua::tostring(L, -1);
            if (r == "applied")
                outcome = Applied;
            else if (r.rfind("skipped", 0) == 0)
            {
                outcome = Skipped;
                detail = r.size() > 9 && r[7] == ':' ? r.substr(9) : r.substr(7);
            }
            else
                detail = r;
        }
        SetTaint(saved);
        AscLua::lua_settop(L, top);
        g_sessionBytes += p.source.size();
        MarkRan(p);
        Report(p, outcome, detail);
    }

    void RunOrDefer(const Patch& p)
    {
        if (!p.target.empty() && !AddonLoaded(p.target))
        {
            g_awaitingAddon.push_back(p);
            Report(p, Deferred, "waiting for " + p.target);
            return;
        }
        Run(p);
    }

    const char* const kConsentLua = R"lua(
if not StaticPopupDialogs.ASC_HOT_PATCH_CONSENT then
    StaticPopupDialogs.ASC_HOT_PATCH_CONSENT = {
        text = "%s wants to change your interface with code it sends (%s).\n\nAllow interface patches from this realm? You can change this later with /hotpatch.",
        button1 = "Allow",
        button2 = "Deny",
        OnAccept = function() C_HotPatch.SetRealmConsent(true) end,
        OnCancel = function(_, _, reason) if reason ~= "override" then C_HotPatch.SetRealmConsent(false) end end,
        timeout = 0,
        whileDead = true,
        hideOnEscape = false,
        preferredIndex = 3,
    }
end
)lua";

    void Prompt()
    {
        if (g_promptShown)
            return;
        g_promptShown = true;
        std::string ids;
        for (const Patch& p : g_awaitingConsent)
            if (ids.find(p.id) == std::string::npos)
                ids += (ids.empty() ? "" : ", ") + p.id;
        AscLua::Execute(kConsentLua, "AscHotPatch", nullptr);
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        AscLua::lua_getfield(L, LUA_GLOBALSINDEX, "StaticPopup_Show");
        if (AscLua::lua_type(L, -1) == LUA_TFUNCTION)
        {
            AscLua::lua_pushstring(L, "ASC_HOT_PATCH_CONSENT");
            AscLua::lua_pushstring(L, RealmName().c_str());
            AscLua::lua_pushstring(L, ids.c_str());
            lua_pcall(L, 3, 0, 0);
        }
        AscLua::lua_settop(L, top);
    }

    void Handle(Patch p)
    {
        std::string reason;
        if (!Guard(p, reason))
        {
            Report(p, RefusedGuard, reason);
            return;
        }
        const Settings s = LoadSettings();
        if (s.mode == Off)
        {
            Report(p, RefusedByPlayer, "hot patches are off");
            return;
        }
        if (s.mode == Ask)
        {
            const std::string realm = RealmName();
            for (const auto& [name, allowed] : s.realms)
                if (name == realm)
                {
                    if (!allowed)
                    {
                        Report(p, RefusedByPlayer, "denied for this realm");
                        return;
                    }
                    RunOrDefer(p);
                    return;
                }
            g_awaitingConsent.push_back(std::move(p));
            Prompt();
            return;
        }
        RunOrDefer(p);
    }

    void __cdecl OnHotPatch(void*, uint32_t, uint32_t, CDataStore* pkt)
    {
        auto sized = [pkt](std::string& out) {
            if (pkt->m_read + 4 > pkt->m_size)
                return false;
            uint32_t n;
            memcpy(&n, pkt->m_buffer + pkt->m_read, 4);
            if (n > static_cast<uint32_t>(pkt->m_size - pkt->m_read - 4))
                return false;
            out.assign(reinterpret_cast<const char*>(pkt->m_buffer + pkt->m_read + 4), n);
            pkt->m_read += static_cast<int32_t>(4 + n);
            return true;
        };
        Patch p;
        if (!sized(p.id) || !sized(p.target) || !sized(p.source))
        {
            AscLog::Printf("hot patch: malformed SMSG 0x8F0");
            return;
        }
        p.hash = Hash(p.source);
        Handle(std::move(p));
    }


    int SetRealmConsent(lua_State* L)
    {
        const bool allowed = AscLua::lua_toboolean(L, 1) != 0;
        Settings s = LoadSettings();
        const std::string realm = RealmName();
        bool found = false;
        for (auto& [name, value] : s.realms)
            if (name == realm)
            {
                value = allowed;
                found = true;
            }
        if (!found)
            s.realms.emplace_back(realm, allowed);
        SaveSettings(s);
        g_promptShown = false;
        std::deque<Patch> waiting;
        waiting.swap(g_awaitingConsent);
        for (const Patch& p : waiting)
            if (allowed)
                RunOrDefer(p);
            else
                Report(p, RefusedByPlayer, "denied for this realm");
        return 0;
    }

    int OnAddonLoaded(lua_State* L)
    {
        const std::string addon = CheckString(L, 1);
        std::vector<Patch> waiting;
        for (auto it = g_awaitingAddon.begin(); it != g_awaitingAddon.end();)
            if (it->target == addon)
            {
                waiting.push_back(*it);
                it = g_awaitingAddon.erase(it);
            }
            else
                ++it;
        for (const Patch& p : waiting)
            Run(p);
        return 0;
    }

    int GetPatches(lua_State* L)
    {
        AscLua::lua_createtable(L, static_cast<int>(g_records.size()), 0);
        int i = 0;
        for (const Record& r : g_records)
        {
            AscLua::lua_createtable(L, 0, 6);
            AscLua::lua_pushstring(L, r.id.c_str());
            AscLua::lua_setfield(L, -2, "id");
            AscLua::lua_pushstring(L, r.target.c_str());
            AscLua::lua_setfield(L, -2, "target");
            AscLua::lua_pushinteger(L, static_cast<int>(r.size));
            AscLua::lua_setfield(L, -2, "size");
            AscLua::lua_pushstring(L, HexHash(r.hash).c_str());
            AscLua::lua_setfield(L, -2, "hash");
            AscLua::lua_pushstring(L, kOutcomeNames[r.outcome]);
            AscLua::lua_setfield(L, -2, "outcome");
            AscLua::lua_pushstring(L, r.detail.c_str());
            AscLua::lua_setfield(L, -2, "detail");
            AscLua::lua_rawseti(L, -2, ++i);
        }
        return 1;
    }

    int GetMode(lua_State* L)
    {
        static const char* const names[] = {"off", "ask", "on"};
        AscLua::lua_pushstring(L, names[LoadSettings().mode]);
        return 1;
    }

    void SendCapability();

    int SetMode(lua_State* L)
    {
        const std::string m = CheckString(L, 1);
        Settings s = LoadSettings();
        s.mode = m == "off" ? Off : m == "on" ? Allow : Ask;
        SaveSettings(s);
        SendCapability();
        return 0;
    }

    const char* const kWorldLua = R"lua(
if not AscHotPatchFrame then
    AscHotPatchFrame = CreateFrame("Frame")
    AscHotPatchFrame:RegisterEvent("ADDON_LOADED")
    AscHotPatchFrame:SetScript("OnEvent", function(_, _, addon) C_HotPatch.OnAddonLoaded(addon) end)
end
SLASH_ASCHOTPATCH1 = "/hotpatch"
SlashCmdList.ASCHOTPATCH = function(msg)
    msg = strlower(strtrim(msg or ""))
    if msg == "on" or msg == "off" or msg == "ask" then
        C_HotPatch.SetMode(msg)
    end
    local patches = C_HotPatch.GetPatches()
    DEFAULT_CHAT_FRAME:AddMessage(format("Interface patches: %s (/hotpatch on|ask|off). %d this session.", C_HotPatch.GetMode(), #patches))
    for _, p in ipairs(patches) do
        DEFAULT_CHAT_FRAME:AddMessage(format("  %s%s: %s%s (%d bytes, %s)", p.id, p.target ~= "" and (" -> " .. p.target) or "",
            p.outcome, p.detail ~= "" and (" - " .. p.detail) or "", p.size, p.hash))
    end
end
)lua";

    void SendCapability()
    {
        Packet packet(0x8F1);
        packet.U32(kProtocolVersion);
        packet.U8(LoadSettings().mode);
        packet.Send();
    }

    bool NewLuaSession()
    {
        lua_State* L = AscLua::GetState();
        const int top = AscLua::lua_gettop(L);
        AscLua::lua_getfield(L, LUA_GLOBALSINDEX, "AscHotPatchFrame");
        const bool fresh = AscLua::lua_type(L, -1) == LUA_TNIL;
        AscLua::lua_settop(L, top);
        return fresh;
    }

    void OnEnteredWorld()
    {
        if (NewLuaSession())
        {
            g_awaitingConsent.clear();
            g_awaitingAddon.clear();
            g_sessionBytes = 0;
            g_promptShown = false;
        }
        AscLua::Execute(kWorldLua, "AscHotPatch", nullptr);
        SendCapability();
    }


    bool FormatterAllowed(const std::string& text, std::string& reason)
    {
        static const std::set<std::string> kCalls = {
            "C_Quest.GetTitleByID", "SecondsToTime", "GetCreatureNameByID", "GetMoneyString",
            "C_Item.GetColoredItemName", "GetColoredReputationName", "GetFactionInfoByID",
        };
        static const std::set<std::string> kWords = {"value", "and", "or", "not", "nil", "true", "false"};
        size_t i = 0;
        const size_t n = text.size();
        auto skipSpace = [&] { while (i < n && isspace(static_cast<unsigned char>(text[i]))) ++i; };
        skipSpace();
        if (text.compare(i, 6, "return") != 0 || (i + 6 < n && (isalnum(static_cast<unsigned char>(text[i + 6])) || text[i + 6] == '_')))
        {
            reason = "does not start with return";
            return i == n;
        }
        i += 6;
        int depth = 0;
        bool any = false;
        while (true)
        {
            skipSpace();
            if (i >= n)
                break;
            const char c = text[i];
            any = true;
            if (isalpha(static_cast<unsigned char>(c)) || c == '_')
            {
                std::string name;
                while (true)
                {
                    const size_t start = i;
                    while (i < n && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'))
                        ++i;
                    name.append(text, start, i - start);
                    if (i + 1 < n && text[i] == '.' && text[i + 1] != '.' &&
                        (isalpha(static_cast<unsigned char>(text[i + 1])) || text[i + 1] == '_'))
                    {
                        name += '.';
                        ++i;
                        continue;
                    }
                    break;
                }
                if (!kCalls.count(name) && !kWords.count(name))
                {
                    reason = "uses " + name;
                    return false;
                }
            }
            else if (isdigit(static_cast<unsigned char>(c)))
            {
                while (i < n && (isalnum(static_cast<unsigned char>(text[i])) || text[i] == '.'))
                    ++i;
            }
            else if (c == '"' || c == '\'')
            {
                ++i;
                while (i < n && text[i] != c)
                {
                    if (text[i] == '\n')
                        break;
                    i += text[i] == '\\' ? 2 : 1;
                }
                if (i >= n || text[i] != c)
                {
                    reason = "unterminated string";
                    return false;
                }
                ++i;
            }
            else if (c == '(')
            {
                ++depth;
                ++i;
            }
            else if (c == ')')
            {
                if (--depth < 0)
                {
                    reason = "unbalanced )";
                    return false;
                }
                ++i;
            }
            else if (c == '-' && i + 1 < n && text[i + 1] == '-')
            {
                reason = "contains a comment";
                return false;
            }
            else if (strchr("+-*/%^#,", c))
                ++i;
            else if (c == '.' && i + 1 < n && text[i + 1] == '.')
                i += 2;
            else if ((c == '=' || c == '~' || c == '<' || c == '>') && i + 1 < n && text[i + 1] == '=')
                i += 2;
            else if (c == '<' || c == '>')
                ++i;
            else
            {
                reason = std::string("uses '") + c + "'";
                return false;
            }
        }
        if (depth != 0)
        {
            reason = "unbalanced (";
            return false;
        }
        if (!any)
        {
            reason = "returns nothing";
            return false;
        }
        return true;
    }

    bool ValidateRequirementType(uint32_t opcode, const std::map<uint32_t, std::string>& strings)
    {
        for (uint32_t field : {6u, 7u, 8u})
        {
            auto it = strings.find(field);
            if (it == strings.end() || it->second.empty())
                continue;
            std::string reason;
            if (!FormatterAllowed(it->second, reason))
            {
                auto token = strings.find(1);
                AscLog::Printf("SMSG 0x%X: refused requirement type %s, formatter %u %s", opcode,
                    token != strings.end() ? token->second.c_str() : "?", field - 5, reason.c_str());
                return false;
            }
        }
        return true;
    }

    void Init()
    {
        sDC.AddPacketHandler(0x8F0, CNetClientCustomPacket((void*)&OnHotPatch, nullptr));
        AscDbcPatch::SetValidator(0x5BC, &ValidateRequirementType);
        AscRuntime::OnAfterEnterWorld(&OnEnteredWorld);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_HotPatch", "SetRealmConsent", SetRealmConsent},
        {"C_HotPatch", "OnAddonLoaded", OnAddonLoaded},
        {"C_HotPatch", "GetPatches", GetPatches},
        {"C_HotPatch", "GetMode", GetMode},
        {"C_HotPatch", "SetMode", SetMode},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
#endif
