#include "NetPatch.hpp"
#include "AscLog.hpp"
#include <winsock2.h>
#include <windows.h>
#include <cstring>

namespace
{
    using send_t = int(WINAPI*)(SOCKET, const char*, int, int);
    using WSASend_t = int(WINAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

    // The build-field rewrite below is DISABLED by default: reporting 12344 on the wire gets the
    // client rejected outright (LOGIN_BADVERSION). The socket hooks are still useful on their own
    // for diagnostics, so the rewrite is gated rather than the whole patch.
    bool g_patchBuild = false;

    send_t g_realSend = nullptr;
    WSASend_t g_realWSASend = nullptr;

    // AUTH_LOGON_CHALLENGE wire layout: opcode(1)=0x00, error(1), size(2 LE), "WoW\0"(4),
    // major(1) minor(1) revision(1), build(2 LE), ... . The build field is what the authserver
    // compares against realmlist.gamebuild when it decides whether to flag a realm "currently
    // down" for this session -- rewrite it in place, once, right before the bytes leave the process.
    void PatchBuildField(char* buf, int len)
    {
        if (!g_patchBuild) return;
        if (len < 13 || !buf) return;
        if (static_cast<unsigned char>(buf[0]) != 0x00) return;
        if (buf[4] != 'W' || buf[5] != 'o' || buf[6] != 'W' || buf[7] != 0) return;
        unsigned short build = static_cast<unsigned char>(buf[11]) | (static_cast<unsigned char>(buf[12]) << 8);
        if (build != 12340) return;
        buf[11] = 0x38; // 12344 LE low byte
        buf[12] = 0x30; // 12344 LE high byte
        AscLog::Printf("NetPatch: rewrote outgoing AUTH_LOGON_CHALLENGE build 12340 -> 12344");
    }

    using closesocket_t = int(WINAPI*)(SOCKET);
    using shutdown_t = int(WINAPI*)(SOCKET, int);

    closesocket_t g_realClosesocket = nullptr;
    shutdown_t g_realShutdown = nullptr;

    // The session bounces back to character select ~10s after world entry, and the client processes
    // SMSG_LOGOUT_RESPONSE/COMPLETE just before it. Two possibilities that look alike from the Lua
    // side: the client asked to log out, or the socket died and the server tore the session down.
    // World headers are encrypted outbound, so the opcode is unreadable -- but a header-only CMSG
    // is exactly 6 bytes on the wire, and a teardown shows up as closesocket/shutdown. Log both.
    void LogSmallFrame(const char* buf, int len)
    {
        if (!buf || len <= 0 || len > 16) return;
        char hex[64] = {0};
        for (int i = 0; i < len && i < 16; ++i)
            wsprintfA(hex + i * 3, "%02X ", static_cast<unsigned char>(buf[i]));
        AscLog::Printf("NetPatch: out %d bytes: %s", len, hex);
    }

    int WINAPI Hook_closesocket(SOCKET s)
    {
        AscLog::Printf("NetPatch: closesocket(%u)", static_cast<unsigned>(s));
        return g_realClosesocket(s);
    }

    int WINAPI Hook_shutdown(SOCKET s, int how)
    {
        AscLog::Printf("NetPatch: shutdown(%u, %d)", static_cast<unsigned>(s), how);
        return g_realShutdown(s, how);
    }

    int WINAPI Hook_send(SOCKET s, const char* buf, int len, int flags)
    {
        PatchBuildField(const_cast<char*>(buf), len);
        LogSmallFrame(buf, len);
        return g_realSend(s, buf, len, flags);
    }

    int WINAPI Hook_WSASend(SOCKET s, LPWSABUF bufs, DWORD n, LPDWORD sent, DWORD flags,
        LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cr)
    {
        if (n >= 1 && bufs && bufs[0].buf)
        {
            PatchBuildField(bufs[0].buf, static_cast<int>(bufs[0].len));
            LogSmallFrame(bufs[0].buf, static_cast<int>(bufs[0].len));
        }
        return g_realWSASend(s, bufs, n, sent, flags, ov, cr);
    }

    // WS2_32.dll is imported by ORDINAL in this exe (measured 2026-09-22 -- named lookup found
    // nothing), so match IAT slots by their currently-resolved FUNCTION ADDRESS against
    // GetProcAddress instead of trying to reconstruct the right ordinal number. Works regardless
    // of whether a given entry was imported by name or ordinal.
    bool PatchImportByAddress(HMODULE hModule, const char* moduleName, void* targetAddr, void* newFunc, void** oldFunc)
    {
        auto base = reinterpret_cast<BYTE*>(hModule);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (importDir.VirtualAddress == 0) return false;
        auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);
        for (; desc->Name; ++desc)
        {
            const char* name = reinterpret_cast<const char*>(base + desc->Name);
            if (_stricmp(name, moduleName) != 0) continue;
            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            for (; thunk->u1.Function; ++thunk)
            {
                if (reinterpret_cast<void*>(thunk->u1.Function) != targetAddr) continue;
                void** slot = reinterpret_cast<void**>(&thunk->u1.Function);
                DWORD oldProtect;
                if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
                if (oldFunc) *oldFunc = *slot;
                *slot = newFunc;
                VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
                return true;
            }
        }
        return false;
    }
}

void NetPatch::InstallDiagnostics()
{
    g_patchBuild = false;
    Install();
}

void NetPatch::Install()
{
    HMODULE exe = GetModuleHandleA(nullptr);
    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) { AscLog::Printf("NetPatch: ws2_32.dll not loaded yet, skipping"); return; }
    void* realSendAddr = reinterpret_cast<void*>(GetProcAddress(ws2, "send"));
    void* realWSASendAddr = reinterpret_cast<void*>(GetProcAddress(ws2, "WSASend"));
    AscLog::Printf("NetPatch: ws2_32=%p send=%p WSASend=%p", (void*)ws2, realSendAddr, realWSASendAddr);
    bool okSend = realSendAddr && PatchImportByAddress(exe, "ws2_32.dll", realSendAddr,
        reinterpret_cast<void*>(Hook_send), reinterpret_cast<void**>(&g_realSend));
    bool okWSASend = realWSASendAddr && PatchImportByAddress(exe, "ws2_32.dll", realWSASendAddr,
        reinterpret_cast<void*>(Hook_WSASend), reinterpret_cast<void**>(&g_realWSASend));
    AscLog::Printf("NetPatch: send hook %s, WSASend hook %s", okSend ? "installed" : "FAILED", okWSASend ? "installed" : "FAILED");

    void* realCloseAddr = reinterpret_cast<void*>(GetProcAddress(ws2, "closesocket"));
    void* realShutdownAddr = reinterpret_cast<void*>(GetProcAddress(ws2, "shutdown"));
    bool okClose = realCloseAddr && PatchImportByAddress(exe, "ws2_32.dll", realCloseAddr,
        reinterpret_cast<void*>(Hook_closesocket), reinterpret_cast<void**>(&g_realClosesocket));
    bool okShutdown = realShutdownAddr && PatchImportByAddress(exe, "ws2_32.dll", realShutdownAddr,
        reinterpret_cast<void*>(Hook_shutdown), reinterpret_cast<void**>(&g_realShutdown));
    AscLog::Printf("NetPatch: closesocket hook %s, shutdown hook %s", okClose ? "installed" : "FAILED", okShutdown ? "installed" : "FAILED");
}
