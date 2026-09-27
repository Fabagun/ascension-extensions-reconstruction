// Realm data hot-swap -- the original's per-realm data directory (module init FUN_102f8930).
//
// A realm can run on its own data: Data\<path>\listarchive names MPQs (one per line) that are opened
// from Data\<path>\ on top of the base archives at priorities 0x200, 0x201, ... (Ascension ships
// Data\area-52\listarchive -> patch-D.MPQ). The path comes from SMSG_REALM_INFO's first string
// (FUN_102fc6c0), from SetDataPath(path) (glue), from the "realmdata <path>" console command, or
// from -realmdata on the command line (initial path only).
//
// Switching (FUN_102fac20 -> tick FUN_102fc250, run after 0x403340 and after 0x495810):
//   0..3 s    the AscensionRealmHotSwapOverlay progress bar fills to 80 %, then 99 %;
//   then      the realm archives are closed (0x421720 each, 0x423D70), the path is set, the new ones
//             are opened, 0x401B00 runs, every known client DBC container's +4 is cleared so it
//             reloads, the WDB cache is flushed (0x634E00), the current map is reloaded when a
//             player exists, the realm list is re-read (0x6B0BF0(1, path)); the bar shows 100 %;
//   +250 ms   the overlay hides and the switch ends (FUN_102fb780).
// Opening the realm archives (FUN_102f8de0) also LoadLibraryA's every *.dll under <base>Data\<path>
// (recursively) and, when <base>Data\<path>buildinfo exists (no separator: the original's path),
// reads a build number from it (default 12340) and patches it into the client: the build string
// 0x9F5200, and the immediates 0x8CA5F0 and 0x4648B3.
//
// Client hooks installed by the init:
//   0x405DD0 (open the base archives)   FUN_102fb1c0: the original, then open the realm archives.
//   0x421950 (SFileOpenArchive, stdcall) FUN_102fa4c0: -hd 0 refuses the eight HD archives;
//                                        Data\patch-Q.MPQ sets the HD flag.
//   0x5F8F50 (scan Interface\AddOns)    FUN_102fb130: the original, then each realm archive's
//                                        Interface\AddOns\ (0x424FA0 with 0x4031C0 / 0x5F8E80) and
//                                        -addonspath <dir> (0x462000 with 0x5F8F30).
//   0x667EF9 (WDB cache directory)      FUN_102fb510: "Cache/WDB/<locale>/<realm>" instead of
//                                        "Cache/WDB/<locale>".
//   0x4041BD                            the call to 0x635520 is NOPed.
// FUN_102faa20 over 0x454070 is in AscSmallHooks.
//
// Reconstruction notes: the original writes the ADDRESS of a stack buffer over the build string at
// 0x9F5200; we write the address of a static buffer holding the same digits, which leaves the same
// kind of bytes there without a dangling pointer.
#include <Ascension/AscRealmData.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace AscScript;

namespace
{
    const uint32_t kClientDbcs[] = {
#include <Ascension/AscClientDbcs.generated.inc>
    };

    std::string g_path;                 // 0x10BCC0A8
    std::string g_prefix;               // 0x10BCC0C0: "Data\" + path + "\", its c_str in 0xAB6160
    std::string g_pending;              // 0x10BCC0F0
    bool g_active = false;              // 0x10BCC0D8
    bool g_finished = false;            // 0x10BCC0D9
    bool g_overlayCreated = false;      // 0x10BCC0DA
    bool g_swapDue = false;             // 0x10BCC0DB
    int64_t g_startNs = 0;              // 0x10BCC0E0
    int64_t g_finishNs = 0;             // 0x10BCC0E8
    std::vector<void*> g_archives;      // 0x10BE3B0C
    bool g_hdPatch = false;             // 0x10BE357D
    char g_buildDigits[8] = {};         // see the reconstruction note above

    int64_t NowNs()   // FUN_100b8230
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void WriteCode(uint32_t address, const void* bytes, size_t n)
    {
        DWORD old;
        if (VirtualProtect(reinterpret_cast<void*>(address), n, PAGE_EXECUTE_READWRITE, &old))
        {
            memcpy(reinterpret_cast<void*>(address), bytes, n);
            VirtualProtect(reinterpret_cast<void*>(address), n, old, &old);
        }
    }

    void RunLua(const char* code)   // DAT_10bcc15c: FrameScript_Execute 0x819210(code, 0, 0)
    {
        reinterpret_cast<void(__cdecl*)(const char*, int, int)>(0x819210)(code, 0, 0);
    }

    // FUN_102fb830: a Lua string literal -- "..." with \n, \r, \" and \\ escaped.
    std::string Quote(const std::string& s)
    {
        std::string out = "\"";
        for (char c : s)
        {
            switch (c)
            {
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                default: out += c; break;
            }
        }
        return out + "\"";
    }

    const char* const kOverlay =
        "\nif UIParent and not AscensionRealmHotSwapOverlay then\n"
        "    local f = CreateFrame(\"Frame\", \"AscensionRealmHotSwapOverlay\", UIParent)\n"
        "    f:SetFrameStrata(\"FULLSCREEN_DIALOG\")\n"
        "    f:SetAllPoints(UIParent)\n"
        "    f:EnableMouse(true)\n"
        "    f:Hide()\n\n"
        "    f.bg = f:CreateTexture(nil, \"BACKGROUND\")\n"
        "    f.bg:SetAllPoints(f)\n"
        "    f.bg:SetTexture(0, 0, 0, 1)\n\n"
        "    f.bar_bg = f:CreateTexture(nil, \"ARTWORK\")\n"
        "    f.bar_bg:SetPoint(\"CENTER\", f, \"CENTER\", 0, -70)\n"
        "    f.bar_bg:SetWidth(360)\n"
        "    f.bar_bg:SetHeight(18)\n"
        "    f.bar_bg:SetTexture(0.08, 0.08, 0.08, 1)\n\n"
        "    f.bar = f:CreateTexture(nil, \"OVERLAY\")\n"
        "    f.bar:SetPoint(\"LEFT\", f.bar_bg, \"LEFT\", 0, 0)\n"
        "    f.bar:SetHeight(18)\n"
        "    f.bar:SetWidth(1)\n"
        "    f.bar:SetTexture(0.85, 0.78, 0.45, 1)\n\n"
        "    f.text = f:CreateFontString(nil, \"OVERLAY\", \"GameFontNormalLarge\")\n"
        "    f.text:SetPoint(\"BOTTOM\", f.bar_bg, \"TOP\", 0, 14)\n\n"
        "    f.percent = f:CreateFontString(nil, \"OVERLAY\", \"GameFontHighlightSmall\")\n"
        "    f.percent:SetPoint(\"TOP\", f.bar_bg, \"BOTTOM\", 0, -10)\n"
        "end\n";

    // FUN_102fbac0: create the overlay once, then show `progress` (clamped to 0..1).
    void ShowOverlay(float progress)
    {
        if (!g_overlayCreated)
        {
            RunLua(kOverlay);
            g_overlayCreated = true;
        }
        const float p = progress < 0.0f ? 0.0f : (progress > 1.0f ? 1.0f : progress);
        float width = p * 360.0f;
        if (width <= 1.0f)
            width = 1.0f;
        const int percent = static_cast<int>(p * 100.0f + 0.5f);
        std::string text = "Switching realm data";
        if (!g_pending.empty())
            text += ": " + g_pending;
        char widthText[32];
        snprintf(widthText, sizeof(widthText), "%g", width);   // ostream << float
        const std::string code = std::string("if AscensionRealmHotSwapOverlay then ")
            + "local f = AscensionRealmHotSwapOverlay; " + "f:Show(); " + "f.bar:SetWidth(" + widthText + "); "
            + "f.percent:SetText(" + Quote(std::to_string(percent) + "%") + "); "
            + "f.text:SetText(" + Quote(text) + "); " + "end";
        RunLua(code.c_str());
    }

    // FUN_102fb780: hide the overlay and end the switch.
    void EndSwitch()
    {
        if (g_overlayCreated)
            RunLua("if AscensionRealmHotSwapOverlay then AscensionRealmHotSwapOverlay:Hide() end");
        g_active = false;
        g_swapDue = false;
        g_pending.clear();
    }

    // FUN_102fa650: the path, and the client's data prefix 0xAB6160 -> "Data\<path>\".
    void SetPath(const std::string& path)
    {
        g_path = path;
        g_prefix = "Data\\" + path + "\\";
        const char* prefix = g_prefix.c_str();
        WriteCode(0xAB6160, &prefix, 4);
    }

    typedef void(__stdcall* CloseArchive_t)(void*);
    typedef int(__stdcall* OpenArchive_t)(const char*, int, int, void**);

    // FUN_102f87d0: close every realm archive, then 0x423D70.
    void CloseArchives()
    {
        for (void* a : g_archives)
            reinterpret_cast<CloseArchive_t>(0x421720)(a);
        g_archives.clear();
        reinterpret_cast<void(__cdecl*)()>(0x423D70)();
    }

    // <client base> + the client's data directory (0x421880 copies 0xB32360; 0x4218B0 returns 0xB32258).
    std::string BaseDir()
    {
        char base[0x104] = {};
        reinterpret_cast<int(__stdcall*)(char*, int)>(0x421880)(base, 0x104);
        return std::string(base) + reinterpret_cast<const char*(__cdecl*)()>(0x4218B0)();
    }

    // FUN_102f8de0: open the realm's archives, load its DLLs, apply its build number, then 0x423D70.
    void OpenArchives()
    {
        if (!g_path.empty())
        {
            const std::string dir = BaseDir() + g_path;
            std::ifstream list(dir + "\\" + "listarchive");
            std::string line;
            int priority = 0x200;
            while (std::getline(list, line))
            {
                void* handle = nullptr;
                const std::string name = "Data\\" + g_path + "\\" + line;
                if (reinterpret_cast<OpenArchive_t>(0x421950)(name.c_str(), priority++, 0xC00, &handle))
                    g_archives.push_back(handle);
            }

            std::error_code ec;
            if (std::filesystem::exists(dir, ec))
                for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
                     !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
                    if (it->path().extension() == L".dll")
                        LoadLibraryA(it->path().string().c_str());

            unsigned build = 0x3034;
            std::ifstream info(dir + "buildinfo");
            if (info)
                info >> build;
            const std::string digits = std::to_string(build);
            if (digits.size() <= 5)
                memcpy(g_buildDigits, digits.c_str(), digits.size());
            const char* digitsPtr = g_buildDigits;
            WriteCode(0x9F5200, &digitsPtr, 4);
            const uint16_t build16 = static_cast<uint16_t>(build);
            WriteCode(0x8CA5F0, &build16, 2);
            WriteCode(0x4648B3, &build16, 2);
        }
        reinterpret_cast<void(__cdecl*)()>(0x423D70)();
    }

    void ReloadMapIfInWorld()
    {
        if (ActivePlayer())
            if (const uint8_t* map = ClientDbcRow(0xAD4160, *reinterpret_cast<const uint32_t*>(0xBD088C)))
                reinterpret_cast<void(__cdecl*)(const char*, int)>(0x7BFCE0)(*reinterpret_cast<const char* const*>(map + 4), 1);
    }

    void ReloadRealmList()   // 0x6B0BF0(1, path)
    {
        reinterpret_cast<void(__cdecl*)(int, const char*)>(0x6B0BF0)(1, g_path.c_str());
    }

    // FUN_102fc250: the switch, one frame at a time.
    void __cdecl Tick()
    {
        if (!g_active)
            return;
        const int64_t now = NowNs();
        if (g_finished)
        {
            if (now - g_finishNs >= 250000000)
                EndSwitch();
            return;
        }
        const int64_t elapsedMs = (now - g_startNs) / 1000000;
        if (g_swapDue)
        {
            const std::string path = g_pending;
            CloseArchives();
            SetPath(path);
            OpenArchives();
            reinterpret_cast<int(__cdecl*)()>(0x401B00)();
            g_overlayCreated = false;
            for (uint32_t db : kClientDbcs)
                *reinterpret_cast<uint32_t*>(db + 4) = 0;
            reinterpret_cast<void(__cdecl*)()>(0x634E00)();
            ReloadMapIfInWorld();
            ReloadRealmList();
            AscLog::Printf("AscRealmData: switched to '%s', %u realm archive(s) open", g_path.c_str(),
                           static_cast<unsigned>(g_archives.size()));
            if (g_active)
            {
                g_finished = true;
                g_finishNs = NowNs();
                ShowOverlay(1.0f);
            }
            return;
        }
        if (elapsedMs < 3000)
        {
            ShowOverlay(static_cast<float>(elapsedMs) * 0.8f / 3000.0f);
            return;
        }
        ShowOverlay(0.99f);
        g_swapDue = true;
    }

    // ---- hooks ----------------------------------------------------------------------------------------
    typedef int(__cdecl* OpenBase_t)();
    OpenBase_t g_openBase = nullptr;
    int __cdecl OpenBaseDetour()   // FUN_102fb1c0
    {
        const int r = g_openBase();
        OpenArchives();
        return r;
    }

    OpenArchive_t g_openArchive = nullptr;
    int __stdcall OpenArchiveDetour(const char* name, int priority, int flags, void** handle)   // FUN_102fa4c0
    {
        typedef int(__stdcall* StrCmpI_t)(const char*, const char*, int);
        const StrCmpI_t strcmpi = reinterpret_cast<StrCmpI_t>(0x76E780);
        static const char* const kHdArchives[] = {"Data\\patch-4.MPQ", "Data\\patch-5.MPQ", "Data\\patch-X.MPQ",
            "Data\\patch-U.MPQ", "Data\\patch-Z.MPQ", "Data\\patch-Q.MPQ", "Data\\patch-R.MPQ", "Data\\patch-V.MPQ"};
        const char* hd = CommandLineArg("hd");
        if (hd && strcmp(hd, "0") == 0)
            for (const char* a : kHdArchives)
                if (strcmpi(name, a, 0x7FFFFFFF) == 0)
                    return 0;
        if (strcmpi(name, "Data\\patch-Q.MPQ", 0x7FFFFFFF) == 0)
            g_hdPatch = true;
        return g_openArchive(name, priority, flags, handle);
    }

    typedef void(__cdecl* ScanAddOns_t)();
    ScanAddOns_t g_scanAddOns = nullptr;
    void __cdecl ScanAddOnsDetour()   // FUN_102fb130
    {
        g_scanAddOns();
        struct { const char* prefix; uint32_t length; uint32_t callback; uint32_t zero; } ctx = {"Interface\\AddOns\\", 0x11, 0x5F8E80, 0};
        for (void* a : g_archives)
            reinterpret_cast<void(__stdcall*)(void*, uint32_t, void*)>(0x424FA0)(a, 0x4031C0, &ctx);
        if (const char* dir = CommandLineArg("addonspath"))
            reinterpret_cast<void(__cdecl*)(const char*, const char*, uint32_t, int, int)>(0x462000)(dir, "*", 0x5F8F30, 0, 0);
    }

    // FUN_102fb520: "Cache/WDB/<locale 0xC5DE88>/<realm name *(*0xC79D08 + 0x28)>".
    void __cdecl BuildCachePath(char* out)
    {
        const uint8_t* realm = *reinterpret_cast<const uint8_t* const*>(0xC79D08);
        char path[260];
        snprintf(path, sizeof(path), "Cache/%s/%s/%s", "WDB", reinterpret_cast<const char*>(0xC5DE88),
                 *reinterpret_cast<const char* const*>(realm + 0x28));
        strncpy(out, path, 0x104);
    }

    // FUN_102fb510, jumped to from 0x667EF9 with the destination buffer in esi.
    __declspec(naked) void CachePathHook()
    {
        __asm {
            push esi
            call BuildCachePath
            add esp, 4
            mov eax, 0x667F16
            jmp eax
        }
    }

    // ---- entry points ---------------------------------------------------------------------------------
    // FUN_102f8820: "realmdata" with no argument returns to the stock data at once; with one, switches.
    int __cdecl RealmDataCommand(const char*, const char* args)
    {
        if (!*args)
        {
            CloseArchives();
            SetPath("");
            reinterpret_cast<int(__cdecl*)()>(0x401B00)();
            ReloadMapIfInWorld();
            ReloadRealmList();
            return 1;
        }
        AscRealmData::Switch(args);
        return 1;
    }

    // FUN_102fa570: SetDataPath(path).
    int SetDataPath(lua_State* L)
    {
        AscRealmData::Switch(CheckString(L, 1));
        return 0;
    }

    void Init()
    {
        static const uint8_t kNop5[5] = {0x90, 0x90, 0x90, 0x90, 0x90};
        WriteCode(0x4041BD, kNop5, 5);
        const char* initial = CommandLineArg("realmdata");
        SetPath(initial ? initial : "");
        g_openBase = reinterpret_cast<OpenBase_t>(AscRuntime::Detour(0x405DD0, 9, reinterpret_cast<void*>(&OpenBaseDetour)));
        g_openArchive = reinterpret_cast<OpenArchive_t>(AscRuntime::Detour(0x421950, 5, reinterpret_cast<void*>(&OpenArchiveDetour)));
        g_scanAddOns = reinterpret_cast<ScanAddOns_t>(AscRuntime::Detour(0x5F8F50, 6, reinterpret_cast<void*>(&ScanAddOnsDetour)));
        AscRuntime::ReplaceFunction(0x667EF9, reinterpret_cast<void*>(&CachePathHook));
        AscRuntime::OnAfter403340(&Tick);
        AscRuntime::OnAfter495810(&Tick);
        reinterpret_cast<void(__cdecl*)(const char*, int(__cdecl*)(const char*, const char*), int, int)>(0x769100)(
            "realmdata", &RealmDataCommand, 1, 0);
    }

    const AscBindings::Binding kBindings[] = {
        {nullptr, "SetDataPath", SetDataPath},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}

const std::string& AscRealmData::CurrentPath() { return g_path; }
bool AscRealmData::HdPatchLoaded() { return g_hdPatch; }

// FUN_102fac20: already on `path`, or already switching to it: nothing. Otherwise start a switch.
void AscRealmData::Switch(const std::string& path)
{
    if (path == g_path)
        return;
    if (g_active && !g_finished && path == g_pending)
        return;
    g_active = true;
    g_finished = false;
    g_swapDue = false;
    g_startNs = NowNs();
    g_pending = path;
    ShowOverlay(0.0f);
}
