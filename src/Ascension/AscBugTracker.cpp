// C_BugTracker (12) + the global CreateBugReport, transcribed from the original's bug report composer
// (static at 0x10BC9650, E:\oniwow-artifacts\ghidra-ext\out\all\100e*.c; registrar FUN_100e8260).
//
//   +0x00 u32  realm-info +0x64 (taken on every clear)     +0x0C str title        (1..0x80)
//   +0x04 u32  category (< 10, clear -> 9)                  +0x24 str description  (1..0x1000)
//   +0x08 u32  priority (< 4, clear -> 0)                   +0x3C u8  public
//   +0x40 time64 last submit (0 = never)
//
// Every setter fires BUG_REPORT_MODIFIED while in the world (0xBD0792). The report is cleared at
// load (FUN_100060b0), by ClearReport, and by SMSG_CREATE_BUG_REPORT_SUCCESS.
//   CMSG 0x562 (FUN_100e71c0): u32 realm field, u32 category, u32 priority, u8 public,
//        u32 len + title bytes, u32 len + description bytes (no terminators). Stamps the time.
//   SMSG 0x565 CREATE_BUG_REPORT_ERROR (FUN_100e7340): u32 len + bytes -> event("%s").
//   SMSG 0x566 CREATE_BUG_REPORT_SUCCESS (FUN_100e7540): u32 id -> clear, event("%u").
// SubmitReport refuses within 2.0 s (0x10B2CC08) of the last send; CreateBugReport does not check.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Ascension/RealmInfo.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <ctime>
#include <string>

using namespace AscScript;

namespace
{
    struct Report
    {
        uint32_t realmField = 0;
        uint32_t category = 9;
        uint32_t priority = 0;
        std::string title, description;
        bool isPublic = false;
        __time64_t lastSubmit = 0;
    };
    Report g_report;

    bool Gate() { return reinterpret_cast<int(__cdecl*)(uint32_t)>(0x5191C0)(0x11) != 0; }   // FUN_10111ce0
    bool InWorld() { return *reinterpret_cast<const uint8_t*>(0xBD0792) != 0; }

    void Modified()
    {
        if (InWorld())
            AscRuntime::Signal("BUG_REPORT_MODIFIED");
    }

    void Clear()   // FUN_100e76f0
    {
        g_report.realmField = RealmInfoSvc::Get().field64;
        g_report.category = 9;
        g_report.priority = 0;
        g_report.title.clear();
        g_report.description.clear();
        g_report.isPublic = false;
        g_report.lastSubmit = 0;
        Modified();
    }

    void SetCategory(uint32_t v) { g_report.category = v; Modified(); }                 // FUN_100e7fe0
    void SetPriority(uint32_t v) { g_report.priority = v; Modified(); }                 // FUN_100e80e0
    void SetTitle(const std::string& v) { g_report.title = v; Modified(); }             // FUN_100e81e0
    void SetDescription(const std::string& v) { g_report.description = v; Modified(); } // FUN_100e8060
    void SetPublic(bool v) { g_report.isPublic = v; Modified(); }                       // FUN_100e8160

    void Send()   // FUN_100e71c0
    {
        Packet p(0x562);
        p.U32(g_report.realmField).U32(g_report.category).U32(g_report.priority).U8(g_report.isPublic ? 1 : 0);
        p.U32(static_cast<uint32_t>(g_report.title.size()));
        if (!g_report.title.empty())
            p.Data(g_report.title.data(), static_cast<uint32_t>(g_report.title.size()));
        p.U32(static_cast<uint32_t>(g_report.description.size()));
        if (!g_report.description.empty())
            p.Data(g_report.description.data(), static_cast<uint32_t>(g_report.description.size()));
        p.Send();
        g_report.lastSubmit = _time64(nullptr);
    }

    bool ValidTitle(const std::string& s) { return !s.empty() && s.size() <= 0x80; }
    bool ValidDescription(const std::string& s) { return !s.empty() && s.size() <= 0x1000; }

    int ClearReport(lua_State*)
    {
        if (Gate())
            Clear();
        return 0;
    }

    // CreateBugReport(category, priority, title, description, public) -- FUN_100e7890 (argument parser
    // FUN_100e6f50). A bare global: the original registers it on its own before the C_BugTracker table.
    int CreateBugReport(lua_State* L)
    {
        if (!Gate())
            return 0;
        bool ok = ValidateInput(L, {NUMBER, NUMBER, STRING, STRING, BOOLEAN});
        uint32_t category = 0, priority = 0;
        std::string title, description;
        bool isPublic = false;
        if (ok)
        {
            category = static_cast<uint32_t>(ToInt(CheckNumber(L, 1)));
            priority = static_cast<uint32_t>(ToInt(CheckNumber(L, 2)));
            title = CheckString(L, 3);
            description = CheckString(L, 4);
            isPublic = AscLua::lua_toboolean(L, 5) != 0;
            ok = category <= 9 && priority <= 3 && ValidTitle(title) && ValidDescription(description);
        }
        if (ok)
        {
            SetCategory(category);
            SetPriority(priority);
            SetTitle(title);
            SetDescription(description);
            SetPublic(isPublic);
            Send();
        }
        PushBool(L, ok);
        return 1;
    }

    int SubmitReport(lua_State* L)
    {
        if (!Gate())
            return 0;
        bool ok = g_report.category < 10 && g_report.priority < 4 && ValidTitle(g_report.title)
                  && ValidDescription(g_report.description);
        if (ok && g_report.lastSubmit != 0 && _difftime64(_time64(nullptr), g_report.lastSubmit) < 2.0)
            ok = false;
        if (ok)
            Send();
        PushBool(L, ok);
        return 1;
    }

    int GetReportCategory(lua_State* L)
    {
        if (!Gate()) return 0;
        PushInt(L, static_cast<int32_t>(g_report.category));
        return 1;
    }
    int GetReportPriority(lua_State* L)
    {
        if (!Gate()) return 0;
        PushInt(L, static_cast<int32_t>(g_report.priority));
        return 1;
    }
    int GetReportTitle(lua_State* L)
    {
        if (!Gate()) return 0;
        PushStr(L, g_report.title.c_str());
        return 1;
    }
    int GetReportDescription(lua_State* L)
    {
        if (!Gate()) return 0;
        PushStr(L, g_report.description.c_str());
        return 1;
    }
    int IsReportPublic(lua_State* L)
    {
        if (!Gate()) return 0;
        PushBool(L, g_report.isPublic);
        return 1;
    }

    int SetReportCategory(lua_State* L)
    {
        if (!Gate()) return 0;
        uint32_t v;
        const bool ok = ReadNumber(L, v) && v < 10;
        if (ok) SetCategory(v);
        PushBool(L, ok);
        return 1;
    }
    int SetReportPriority(lua_State* L)
    {
        if (!Gate()) return 0;
        uint32_t v;
        const bool ok = ReadNumber(L, v) && v < 4;
        if (ok) SetPriority(v);
        PushBool(L, ok);
        return 1;
    }
    int SetReportTitle(lua_State* L)
    {
        if (!Gate()) return 0;
        std::string v;
        const bool ok = ReadString(L, v) && ValidTitle(v);
        if (ok) SetTitle(v);
        PushBool(L, ok);
        return 1;
    }
    int SetReportDescription(lua_State* L)
    {
        if (!Gate()) return 0;
        std::string v;
        const bool ok = ReadString(L, v) && ValidDescription(v);
        if (ok) SetDescription(v);
        PushBool(L, ok);
        return 1;
    }
    int SetReportPublic(lua_State* L)
    {
        if (!Gate()) return 0;
        const bool ok = ValidateInput(L, {BOOLEAN});
        if (ok) SetPublic(AscLua::lua_toboolean(L, 1) != 0);
        PushBool(L, ok);
        return 1;
    }

    void __cdecl OnReportError(void*, uint32_t, uint32_t, CDataStore* p)   // 0x565
    {
        uint32_t len;
        memcpy(&len, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        const std::string text(reinterpret_cast<const char*>(p->m_buffer + p->m_read), len);
        p->m_read += static_cast<int32_t>(len);
        AscRuntime::Signal("CREATE_BUG_REPORT_ERROR", "%s", text.c_str());
    }

    void __cdecl OnReportSuccess(void*, uint32_t, uint32_t, CDataStore* p)   // 0x566
    {
        uint32_t id;
        memcpy(&id, p->m_buffer + p->m_read, 4);
        p->m_read += 4;
        Clear();
        AscRuntime::Signal("CREATE_BUG_REPORT_SUCCESS", "%u", id);
    }

    void Init()
    {
        g_report.lastSubmit = 0;
        sDC.AddPacketHandler(0x565, CNetClientCustomPacket((void*)&OnReportError, nullptr));
        sDC.AddPacketHandler(0x566, CNetClientCustomPacket((void*)&OnReportSuccess, nullptr));
        // FUN_100e71b0 (`mov ecx, report; jmp FUN_100e76f0`): the report is cleared on every glue screen
        // and at every world entry.
        AscRuntime::OnGlueScreen(&Clear);
        AscRuntime::OnEnterWorld(&Clear);
    }

    const AscBindings::Binding kBindings[] = {
        {"C_BugTracker", "ClearReport", ClearReport},
        {nullptr, "CreateBugReport", CreateBugReport},
        {"C_BugTracker", "GetReportCategory", GetReportCategory},
        {"C_BugTracker", "GetReportDescription", GetReportDescription},
        {"C_BugTracker", "GetReportPriority", GetReportPriority},
        {"C_BugTracker", "GetReportTitle", GetReportTitle},
        {"C_BugTracker", "IsReportPublic", IsReportPublic},
        {"C_BugTracker", "SetReportCategory", SetReportCategory},
        {"C_BugTracker", "SetReportDescription", SetReportDescription},
        {"C_BugTracker", "SetReportPriority", SetReportPriority},
        {"C_BugTracker", "SetReportPublic", SetReportPublic},
        {"C_BugTracker", "SetReportTitle", SetReportTitle},
        {"C_BugTracker", "SubmitReport", SubmitReport},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
