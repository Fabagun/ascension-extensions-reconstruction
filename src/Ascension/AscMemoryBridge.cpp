// The original's MemoryBridgeClient (0x102B0000..0x102B7FFF), talking to Ascension's MMgr64.exe as-is.
//
//   Client object (FUN_102b0b60, instance 0x10BE3354): +0 client PID, +4 server PID, +8 state (0 stopped,
//   1 starting, 2 running, 3 stopping), +0xC/+0x10 request/response mappings, +0x14/+0x18 their views,
//   +0x1C/+0x20 request/response events, +0x24 server process, +0x28 thread id, +0x2C token, +0x44 request
//   buffer.
//   Objects: "Local\MemoryBridge_{clientPID}_{token}_{Req|Res|ReqEvent|ResEvent}", 1 MB views.
//   Message (FUN_102b0be0, 0x48 bytes) = u32 cmd, u32 a, u64 b, u32 c, blob, entries1, entries2, u32 protocol
//   (3), u32 result, u32 clientPID, u32 serverPID; serialized by FUN_102b6dd0, parsed by FUN_102b53f0.
//   Log: FUN_102b7780 / FUN_102b7820, "[LEVEL] text" to stdout and MemoryBridge.log (truncated at start).
#include <Ascension/AscCrashContext.hpp>
#include <Ascension/AscMemoryBridge.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // ---- log (FUN_102b7530 / FUN_102b7780 / FUN_102b7820) -------------------------------------------
    enum Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };
    struct Logger
    {
        std::mutex mutex;
        std::ofstream file;
        Logger() : file("MemoryBridge.log", std::ios::out | std::ios::trunc) {}
    };
    // Deviation: never destroyed. The original's logger is a function-local static, so it is destroyed
    // before the client object whose destructor still logs through it at exit.
    Logger& Log()
    {
        static Logger* logger = new Logger;
        return *logger;
    }
    void Write(Level level, const std::string& text)
    {
        Logger& l = Log();
        std::lock_guard<std::mutex> lock(l.mutex);
        std::string tag;
        switch (level)
        {
        case kDebug: tag = "DEBUG"; break;
        case kInfo: tag = "INFO"; break;
        case kWarn: tag = "WARN"; break;
        case kError: tag = "ERROR"; break;
        }
        const std::string line = "[" + tag + "] " + text;
        std::cout << line << std::endl;
        if (l.file.is_open())
            l.file << line << std::endl;
    }
    std::string U(uint32_t v) { return std::to_string(v); }

    const char* StateName(int32_t s)   // FUN_102b6470
    {
        switch (s)
        {
        case 0: return "stopped";
        case 1: return "starting";
        case 2: return "running";
        case 3: return "stopping";
        default: return "unknown";
        }
    }
    const char* ResultName(uint32_t r)   // FUN_102b6800
    {
        switch (r)
        {
        case 0: return "success";
        case 1: return "failed";
        case 2: return "invalid_message";
        case 3: return "protocol_version_mismatch";
        case 4: return "process_mismatch";
        case 5: return "unavailable";
        case 6: return "shutting_down";
        case 7: return "timeout";
        case 8: return "oversized_message";
        case 9: return "invalid_argument";
        case 10: return "unknown_command";
        default: return "unknown_result";
        }
    }

    // ---- message --------------------------------------------------------------------------------
    struct Entry1 { uint32_t a = 0, b = 0, c = 0; uint64_t d = 0; uint32_t e = 0; std::vector<uint8_t> data; };
    struct Entry2 { uint32_t a = 0, b = 0, c = 0; uint64_t d = 0; std::vector<uint8_t> data; };
    struct Message
    {
        uint32_t cmd = 0, a = 0;
        uint64_t b = 0;
        uint32_t c = 0;
        std::vector<uint8_t> blob;
        std::vector<Entry1> e1;
        std::vector<Entry2> e2;
        uint32_t protocol = 3, result = 0, clientPid = 0, serverPid = 0;
    };

    template <class T> void Put(std::vector<uint8_t>& out, const T& v)
    {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
        out.insert(out.end(), p, p + sizeof(T));
    }
    void PutBlob(std::vector<uint8_t>& out, const std::vector<uint8_t>& b)
    {
        Put<uint32_t>(out, static_cast<uint32_t>(b.size()));
        out.insert(out.end(), b.begin(), b.end());
    }
    // FUN_102b6dd0
    void Serialize(const Message& m, std::vector<uint8_t>& out)
    {
        Put(out, m.cmd);
        Put(out, m.a);
        Put(out, m.b);
        Put(out, m.c);
        PutBlob(out, m.blob);
        Put<uint32_t>(out, static_cast<uint32_t>(m.e1.size()));
        for (const Entry1& e : m.e1)
        {
            Put(out, e.a);
            Put(out, e.b);
            Put(out, e.c);
            Put(out, e.d);
            Put(out, e.e);
            PutBlob(out, e.data);
        }
        Put<uint32_t>(out, static_cast<uint32_t>(m.e2.size()));
        for (const Entry2& e : m.e2)
        {
            Put(out, e.a);
            Put(out, e.b);
            Put(out, e.c);
            Put(out, e.d);
            PutBlob(out, e.data);
        }
        Put(out, m.protocol);
        Put(out, m.result);
        Put(out, m.clientPid);
        Put(out, m.serverPid);
    }

    // FUN_102b53f0: underflow in a fixed field throws; a blob running past the end returns false; the four
    // trailing fields are each read only if they fit.
    struct Reader
    {
        const uint8_t* p;
        size_t size, at = 0;
        template <class T> T Get()
        {
            if (size < at + sizeof(T))
                throw std::runtime_error("Buffer underflow during deserialization");
            T v;
            memcpy(&v, p + at, sizeof(T));
            at += sizeof(T);
            return v;
        }
        bool Blob(std::vector<uint8_t>& out)
        {
            const uint32_t n = Get<uint32_t>();
            if (size < at + n)
                return false;
            out.assign(p + at, p + at + n);
            at += n;
            return true;
        }
        bool Optional(uint32_t& v)
        {
            if (size < at + 4)
                return false;
            memcpy(&v, p + at, 4);
            at += 4;
            return true;
        }
    };
    bool Deserialize(Message& m, const void* data, size_t size)
    {
        Reader r{static_cast<const uint8_t*>(data), size};
        m.cmd = r.Get<uint32_t>();
        m.a = r.Get<uint32_t>();
        m.b = r.Get<uint64_t>();
        m.c = r.Get<uint32_t>();
        if (!r.Blob(m.blob))
            return false;
        m.e1.resize(r.Get<uint32_t>());
        for (Entry1& e : m.e1)
        {
            e.a = r.Get<uint32_t>();
            e.b = r.Get<uint32_t>();
            e.c = r.Get<uint32_t>();
            e.d = r.Get<uint64_t>();
            e.e = r.Get<uint32_t>();
            if (!r.Blob(e.data))
                return false;
        }
        m.e2.resize(r.Get<uint32_t>());
        for (Entry2& e : m.e2)
        {
            e.a = r.Get<uint32_t>();
            e.b = r.Get<uint32_t>();
            e.c = r.Get<uint32_t>();
            e.d = r.Get<uint64_t>();
            if (!r.Blob(e.data))
                return false;
        }
        if (r.Optional(m.protocol) && r.Optional(m.result) && r.Optional(m.clientPid))
            r.Optional(m.serverPid);
        return true;
    }

    // ---- client object --------------------------------------------------------------------------
    struct Client
    {
        DWORD clientPid = GetCurrentProcessId();   // FUN_102b0b60
        DWORD serverPid = 0;
        std::atomic<int32_t> state{0};
        HANDLE reqMapping = nullptr, resMapping = nullptr;
        void* reqView = nullptr;
        void* resView = nullptr;
        HANDLE reqEvent = nullptr, resEvent = nullptr, process = nullptr;
        std::thread::id thread;
        std::wstring token;
        std::vector<uint8_t> request;
        ~Client();   // FUN_102b0da0, registered at exit by 0x10B166C0
    };
    Client& Instance()
    {
        static Client client;
        return client;
    }
    void ShutdownClient(Client& c);

    bool ProcessAlive(HANDLE process) { return process == nullptr || WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
    bool Connected(const Client& c)
    {
        return c.reqView && c.resView && c.reqEvent && c.resEvent && ProcessAlive(c.process);
    }

    // FUN_102b5bd0: two 64-bit values from std::random_device, mixed with the process / thread ids, the
    // steady clock and a stack address, as 32 upper-case hex digits.
    std::wstring GenerateToken()
    {
        uint32_t v[4];
        std::random_device rd;
        for (int i = 0; i < 4; i += 2)
        {
            const uint32_t first = rd();
            v[i] = rd();
            v[i + 1] = first;
        }
        const uint64_t now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        v[1] ^= GetCurrentProcessId();
        const uint32_t lo0 = v[0] ^ GetCurrentThreadId();
        const uint32_t hi1 = static_cast<uint32_t>(now >> 32) ^ v[3];
        const uint32_t lo1 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v)) ^ static_cast<uint32_t>(now) ^ v[2];
        wchar_t buf[40];
        swprintf(buf, 40, L"%016llX%016llX", (static_cast<uint64_t>(v[1]) << 32) | lo0, (static_cast<uint64_t>(hi1) << 32) | lo1);
        return buf;
    }
    bool ValidToken(const std::wstring& t)   // FUN_102b6430
    {
        if (t.size() != 0x20)
            return false;
        for (wchar_t ch : t)
            if (!((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') || (ch >= L'A' && ch <= L'F')))
                return false;
        return true;
    }
    std::wstring ObjectName(const Client& c, const wchar_t* suffix)   // FUN_102b64c0
    {
        return L"Local\\MemoryBridge_" + std::to_wstring(c.clientPid) + L"_" + c.token + L"_" + suffix;
    }
    std::string Utf8(const std::wstring& w)
    {
        if (w.empty())
            return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    // FUN_102b2050
    bool Launch(Client& c)
    {
        if (!ValidToken(c.token))
        {
            Write(kError, "Refusing to launch MMgr64.exe without a valid object token.");
            return false;
        }
        if (c.process)
        {
            CloseHandle(c.process);
            c.process = nullptr;
            c.serverPid = 0;
        }
        wchar_t path[0x104] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, 0x104);
        if (n == 0 || n == 0x104)
        {
            Write(kError, "Error getting module file name. Error code: " + U(GetLastError()));
            return false;
        }
        const std::wstring module(path);
        const size_t slash = module.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
        {
            Write(kError, "Error: Could not determine the client directory.");
            return false;
        }
        const std::wstring dir = module.substr(0, slash + 1);
        const std::wstring exe = dir + L"MMgr64.exe";
        if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Write(kError, "MMgr64.exe not found at: " + Utf8(exe));
            return false;
        }
        std::wstring cmd = L"\"" + exe + L"\" " + std::to_wstring(c.clientPid) + L" " + c.token;
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        {
            Write(kError, "CreateProcess failed with error code: " + U(GetLastError()));
            return false;
        }
        c.serverPid = pi.dwProcessId;
        c.process = pi.hProcess;
        CloseHandle(pi.hThread);
        return true;
    }

    // FUN_102afa90 / 102afca0 / 102afeb0 / 102b00c0: up to 50 tries 100 ms apart, abandoned early when the
    // server has exited.
    template <class Open> bool OpenWait(Client& c, const std::wstring& name, const char* label, HANDLE& out, Open open)
    {
        for (int tries = 0;;)
        {
            out = open(name.c_str());
            if (out)
                return true;
            const DWORD err = GetLastError();
            if (c.process && WaitForSingleObject(c.process, 0) != WAIT_TIMEOUT)
            {
                Write(kError, std::string("MemoryBridge server exited before ") + label + " opened. Last error: " + U(err));
                return false;
            }
            Sleep(100);
            if (++tries > 0x31)
            {
                Write(kError, std::string("Client failed to open ") + label + ": " + U(GetLastError()));
                return false;
            }
        }
    }

    // FUN_102b6890: send `req`, wait for the response.
    Message Exchange(Client& c, const Message& req)
    {
        const int32_t state = c.state.load();
        if (state != 2 && !(state == 1 && req.cmd == 7) && !(state == 3 && req.cmd == 8))
            throw std::runtime_error(std::string("MemoryBridgeClient is ") + StateName(c.state.load()) + ".");
        if (!Connected(c))
            throw std::runtime_error("MemoryBridgeClient is not connected.");

        Message m = req;
        m.protocol = 3;
        m.result = 0;
        m.clientPid = c.clientPid;
        m.serverPid = c.serverPid;
        c.request.clear();
        Serialize(m, c.request);
        if (c.request.size() > 0xFF000)
        {
            Write(kError, "Request message too large: " + std::to_string(c.request.size()) + " bytes");
            throw std::runtime_error("Request message too large.");
        }
        memset(c.reqView, 0, 0x100000);
        memcpy(c.reqView, c.request.data(), c.request.size());
        if (!SetEvent(c.reqEvent))
            throw std::runtime_error("Failed to signal request event.");

        DWORD timeout = 5000;
        switch (req.cmd)
        {
        case 2: case 8: timeout = 1000; break;
        case 5: case 100: case 0x66: case 0x67: case 0x68: case 0x69: case 0x6A: case 0x6B: case 0x6C:
        case 0x6D: case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: timeout = 10000; break;
        case 0x65: timeout = 2000; break;
        }
        if (WaitForSingleObject(c.resEvent, timeout) != WAIT_OBJECT_0)
        {
            Write(kError, "Timed out waiting " + U(timeout) + " ms for MemoryBridge command " + U(req.cmd) + ".");
            throw std::runtime_error("Timed out waiting for response.");
        }
        Message res;
        if (!Deserialize(res, c.resView, 0x100000))
            throw std::runtime_error("Failed to deserialize response message.");
        if (res.protocol != 3)
            throw std::runtime_error("MemoryBridge protocol mismatch. client=3 server=" + U(res.protocol));
        if (res.cmd == 0xFFFFFFFF && res.result == 0)
            res.result = 1;
        if (res.result != 0)
            Write(kDebug, "MemoryBridge command " + U(req.cmd) + " returned " + ResultName(res.result) + ". clientPID=" +
                              U(res.clientPid) + " serverPID=" + U(res.serverPid));
        return res;
    }

    // FUN_102b6580
    bool Handshake(Client& c)
    {
        try
        {
            Message req;
            req.cmd = 7;
            const Message res = Exchange(c, req);
            if (res.cmd == 0 && res.result == 0)
            {
                if (res.clientPid == c.clientPid && res.serverPid != 0)
                {
                    c.serverPid = res.serverPid;
                    return true;
                }
                Write(kError, "MemoryBridge handshake PID mismatch. client expected=" + U(c.clientPid) + " actual=" + U(res.clientPid) +
                                  " server=" + U(res.serverPid));
            }
            else
                Write(kError, std::string("MemoryBridge handshake rejected: ") + ResultName(res.result));
        }
        catch (const std::exception& e)
        {
            Write(kError, std::string("MemoryBridge handshake failed: ") + e.what());
        }
        catch (...)
        {
            Write(kError, "MemoryBridge handshake failed: unknown error");
        }
        return false;
    }

    // FUN_102b7cf0: operation statistics (count, total, largest) for sizes of at least 128.
    struct Stats { uint64_t count = 0, total = 0, largest = 0; uint32_t largestKind = 0; } g_stats;
    void Stat(uint32_t kind, uint64_t size)
    {
        if (size < 0x80)
            return;
        ++g_stats.count;
        g_stats.total += size;
        if (size > g_stats.largest)
        {
            g_stats.largest = size;
            g_stats.largestKind = kind;
        }
    }

    // Chunked transfer (FUN_102b2f60 read, FUN_102b42a0 write, FUN_102b47b0 commands 0x66 / 0x68).
    bool Chunked(uint32_t cmd, uint32_t a, uint32_t offset, uint8_t* dst, const uint8_t* src, uint32_t size)
    {
        if (size == 0)
            return true;
        if ((dst == nullptr && src == nullptr) || offset + size < offset)
            return false;
        Client& c = Instance();
        for (uint32_t done = 0; done < size;)
        {
            const uint32_t chunk = size - done < 0xFEFD0 ? size - done : 0xFEFD0;
            Message req;
            req.cmd = cmd;
            req.a = a;
            req.b = chunk;
            req.c = offset + done;
            if (src)
                req.blob.assign(src + done, src + done + chunk);
            const Message res = Exchange(c, req);
            if (res.cmd != 0 || (dst && res.blob.size() != chunk))
                return false;
            if (dst)
                memcpy(dst + done, res.blob.data(), chunk);
            done += chunk;
        }
        return true;
    }

    void PutU32s(std::vector<uint8_t>& out, const std::vector<uint32_t>& v)   // FUN_102b02d0
    {
        Put<uint32_t>(out, static_cast<uint32_t>(v.size()));
        for (uint32_t x : v)
            Put(out, x);
    }

    // FUN_102b35a0: the keys go out in batches of at most 0x3FBF3. A refused batch is retried at half its
    // size (at least 1) and fails the whole query only at size 1; after each accepted batch the size goes
    // back to the maximum. Blob = u32 n + n keys, then, when `extra` is not empty, "MSTR" (0x10B45FE4), 1
    // (0x10B45FE8), u32 n + n extra. Each reply blob is u32 count + payload (FUN_102b52f0: fewer than 4
    // bytes throws, a count that would overflow the total fails).
    bool BatchedQuery(uint32_t cmd, uint32_t a, uint32_t c, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out,
                      const std::vector<uint32_t>& extra)
    {
        out.clear();
        if (keys.empty())
        {
            Put<uint32_t>(out, 0);
            return true;
        }
        const size_t kMax = 0x3FBF3;
        uint32_t total = 0;
        std::vector<uint8_t> payload;
        size_t done = 0;
        size_t batch = keys.size() < kMax ? keys.size() : kMax;
        while (done < keys.size())
        {
            const size_t take = keys.size() - done < batch ? keys.size() - done : batch;
            Message req;
            req.cmd = cmd;
            req.a = a;
            req.c = c;
            PutU32s(req.blob, std::vector<uint32_t>(keys.begin() + done, keys.begin() + done + take));
            if (!extra.empty())
            {
                Put<uint32_t>(req.blob, 0x5254534D);
                Put<uint32_t>(req.blob, 1);
                PutU32s(req.blob, extra);
            }
            req.b = req.blob.size();
            const Message res = Exchange(Instance(), req);
            if (res.cmd != 0)
            {
                if (take == 1)
                    return false;
                batch = take >> 1 > 1 ? take >> 1 : 1;
                continue;
            }
            if (res.blob.size() < 4)
                throw std::runtime_error("Buffer underflow during deserialization");
            uint32_t n;
            memcpy(&n, res.blob.data(), 4);
            if (n > ~total)
                return false;
            total += n;
            payload.insert(payload.end(), res.blob.begin() + 4, res.blob.end());
            done += take;
            batch = keys.size() - done < kMax ? keys.size() - done : kMax;
        }
        Put(out, total);
        out.insert(out.end(), payload.begin(), payload.end());
        return true;
    }

    // ---- start-up (FUN_10a4e380 over 0x4067F0) ----------------------------------------------------------
    // After the client's own start-up step: initialise the bridge and run the communication test; any
    // failure is the client's fatal error (FUN_10112f70 -> 0x8C51D0).
    void Fatal(const char* text) { AscCrashContext::Assert(text, nullptr, 0); }
    typedef int(__cdecl* Startup_t)();
    Startup_t g_startup = nullptr;
    int __cdecl StartupDetour()
    {
        const int r = g_startup();
        if (!AscMemoryBridge::Initialize())
            Fatal("Failed to initialize memory bridge");
        const uint32_t handle = AscMemoryBridge::AllocateMemory(0x400, 0);
        if (handle == 0)
            Fatal("Allocation failed: received handle 0.");
        std::vector<uint8_t> out(0x400, 0xAA);
        if (!AscMemoryBridge::WriteMemory(handle, 0, out.data(), 0x400))
            Fatal("WriteMemory failed.");
        std::vector<uint8_t> in(0x400, 0);
        if (!AscMemoryBridge::ReadMemory(handle, 0, in.data(), static_cast<uint32_t>(in.size())))
            Fatal("ReadMemory failed.");
        for (uint32_t i = 0; i < 0x400; ++i)
            if (out[i] != in[i])
                Fatal(("Data mismatch at index " + std::to_string(i)).c_str());
        if (!AscMemoryBridge::FreeMemory(handle))
            Fatal("FreeMemory failed.");
        if (AscMemoryBridge::ReadMemory(handle, 0, in.data(), static_cast<uint32_t>(in.size())))
            Fatal("ReadMemory succeeded on freed memory, which is unexpected.");
        Write(kInfo, "MemoryBridge communication test passed.");
        return r;
    }

    void Init()
    {
        g_startup = reinterpret_cast<Startup_t>(AscRuntime::Detour(0x4067F0, 9, reinterpret_cast<void*>(&StartupDetour)));
    }
    AscBindings::Module s_module(nullptr, 0, &Init);
}

// FUN_102b5dd0
bool AscMemoryBridge::Initialize()
{
    Client& c = Instance();
    if (c.state.load() == 2)
        return true;
    c.state.store(1);
    c.thread = std::this_thread::get_id();
    c.token = GenerateToken();
    if (!ValidToken(c.token))
    {
        Write(kError, "Failed to generate a valid MemoryBridge object token.");
        return false;
    }
    if (!Launch(c))
    {
        Write(kError, "Failed to launch MMgr64.exe server process.");
        c.state.store(0);
        return false;
    }
    const std::wstring reqMap = ObjectName(c, L"Req"), resMap = ObjectName(c, L"Res");
    const std::wstring reqEvt = ObjectName(c, L"ReqEvent"), resEvt = ObjectName(c, L"ResEvent");
    if (!OpenWait(c, reqMap, "request mapping", c.reqMapping, [](const wchar_t* n) { return OpenFileMappingW(FILE_MAP_WRITE, FALSE, n); }))
    {
        Shutdown();
        return false;
    }
    c.reqView = MapViewOfFile(c.reqMapping, FILE_MAP_WRITE, 0, 0, 0x100000);
    if (!c.reqView)
    {
        Write(kError, "Client failed to map request view: " + U(GetLastError()));
        Shutdown();
        return false;
    }
    if (!OpenWait(c, resMap, "response mapping", c.resMapping, [](const wchar_t* n) { return OpenFileMappingW(FILE_MAP_READ, FALSE, n); }))
    {
        Shutdown();
        return false;
    }
    c.resView = MapViewOfFile(c.resMapping, FILE_MAP_READ, 0, 0, 0x100000);
    if (!c.resView)
    {
        Write(kError, "Client failed to map response view: " + U(GetLastError()));
        Shutdown();
        return false;
    }
    if (!OpenWait(c, reqEvt, "request event", c.reqEvent, [](const wchar_t* n) { return OpenEventW(EVENT_MODIFY_STATE, FALSE, n); }) ||
        !OpenWait(c, resEvt, "response event", c.resEvent, [](const wchar_t* n) { return OpenEventW(SYNCHRONIZE, FALSE, n); }) ||
        !Handshake(c))
    {
        Shutdown();
        return false;
    }
    c.state.store(2);
    Write(kInfo, "MemoryBridgeClient initialized. clientPID=" + U(c.clientPid) + " serverPID=" + U(c.serverPid) + " protocol=3");
    return true;
}

// FUN_102b7130
void AscMemoryBridge::Shutdown() { ShutdownClient(Instance()); }

namespace
{
    void ShutdownClient(Client& c)
    {
        const int32_t previous = c.state.exchange(3);
        if (previous == 0)
            return;
        if (previous == 2 && Connected(c))
        {
            try
            {
                Message req;
                req.cmd = 8;
                const Message res = Exchange(c, req);
                if (res.cmd != 0)
                    Write(kDebug, std::string("MemoryBridge shutdown request failed: ") + ResultName(res.result));
            }
            catch (const std::exception& e)
            {
                Write(kDebug, std::string("MemoryBridge shutdown request skipped: ") + e.what());
            }
            catch (...)
            {
                Write(kDebug, "MemoryBridge shutdown request skipped: unknown error");
            }
        }
        if (c.reqView) { UnmapViewOfFile(c.reqView); c.reqView = nullptr; }
        if (c.reqMapping) { CloseHandle(c.reqMapping); c.reqMapping = nullptr; }
        if (c.resView) { UnmapViewOfFile(c.resView); c.resView = nullptr; }
        if (c.resMapping) { CloseHandle(c.resMapping); c.resMapping = nullptr; }
        if (c.reqEvent) { CloseHandle(c.reqEvent); c.reqEvent = nullptr; }
        if (c.resEvent) { CloseHandle(c.resEvent); c.resEvent = nullptr; }
        if (c.process) { CloseHandle(c.process); c.process = nullptr; }
        c.serverPid = 0;
        c.token.clear();
        c.state.store(0);
        Write(kInfo, "MemoryBridgeClient shutdown.");
    }

    Client::~Client() { ShutdownClient(*this); }
}

// FUN_102b1b60
uint32_t AscMemoryBridge::AllocateMemory(uint32_t sizeLo, uint32_t sizeHi)
{
    Message req;
    req.cmd = 1;
    req.b = (static_cast<uint64_t>(sizeHi) << 32) | sizeLo;
    const Message res = Exchange(Instance(), req);
    if (res.cmd != 0)
        throw std::runtime_error("AllocateMemory failed.");
    return res.a;
}

// FUN_102b1f10: not while disconnected.
bool AscMemoryBridge::FreeMemory(uint32_t handle)
{
    Client& c = Instance();
    if (!Connected(c))
        return false;
    Message req;
    req.cmd = 2;
    req.a = handle;
    return Exchange(c, req).cmd == 0;
}

bool AscMemoryBridge::ReadMemory(uint32_t handle, uint32_t offset, void* dst, uint32_t size)
{
    if (size != 0 && dst == nullptr)
        return false;
    return Chunked(3, handle, offset, static_cast<uint8_t*>(dst), nullptr, size);
}
bool AscMemoryBridge::WriteMemory(uint32_t handle, uint32_t offset, const void* src, uint32_t size)
{
    if (size != 0 && src == nullptr)
        return false;
    return Chunked(4, handle, offset, nullptr, static_cast<const uint8_t*>(src), size);
}

// 0x102B7AB0 -> FUN_102b1ca0: 0 when refused.
uint32_t AscMemoryBridge::CreateTable(uint32_t a, uint32_t b)
{
    Stat(2, b);
    Message req;
    req.cmd = 100;
    req.b = a;
    req.c = b;
    const Message res = Exchange(Instance(), req);
    return res.cmd != 0 ? 0 : res.a;
}

// 0x102B7AE0 (catches everything) -> FUN_102b1d40: not for table 0 or while disconnected. A failed exchange
// shuts the bridge down and is logged.
bool AscMemoryBridge::DestroyTable(uint32_t table)
{
    try
    {
        Client& c = Instance();
        if (table == 0 || !Connected(c))
            return false;
        try
        {
            Message req;
            req.cmd = 0x65;
            req.a = table;
            return Exchange(c, req).cmd == 0;
        }
        catch (const std::exception& e)
        {
            ShutdownClient(c);
            Write(kDebug, "DestroyTable(" + U(table) + ") skipped: " + e.what());
        }
        catch (...)
        {
            ShutdownClient(c);
            Write(kDebug, "DestroyTable(" + U(table) + ") skipped: unknown error");
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

bool AscMemoryBridge::Command66(uint32_t a, uint32_t offset, const void* src, uint32_t size)
{
    Stat(4, size);
    if (size != 0 && src == nullptr)
        return false;
    return Chunked(0x66, a, offset, nullptr, static_cast<const uint8_t*>(src), size);
}
bool AscMemoryBridge::Command68(uint32_t a, uint32_t offset, const void* src, uint32_t size)
{
    Stat(6, size);
    if (size != 0 && src == nullptr)
        return false;
    return Chunked(0x68, a, offset, nullptr, static_cast<const uint8_t*>(src), size);
}

// FUN_102b1c10
bool AscMemoryBridge::Command6A(uint32_t a, uint32_t b)
{
    Message req;
    req.cmd = 0x6A;
    req.a = a;
    req.c = b;
    return Exchange(Instance(), req).cmd == 0;
}

// 0x102B7B70 -> FUN_102b2d10: the reply blob is u32 n + n u32.
bool AscMemoryBridge::Command6B(uint32_t a, uint32_t b, uint32_t c, std::vector<uint32_t>& out)
{
    out.clear();
    Message req;
    req.cmd = 0x6B;
    req.a = a;
    req.b = c;
    req.c = b;
    const Message res = Exchange(Instance(), req);
    if (res.cmd != 0)
        return false;
    if (res.blob.size() < 4)
        throw std::runtime_error("Buffer underflow during deserialization");
    uint32_t n;
    memcpy(&n, res.blob.data(), 4);
    out.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        if (res.blob.size() < 4u * (i + 2))
            throw std::runtime_error("Buffer underflow during deserialization");
        memcpy(&out[i], res.blob.data() + 4 * (i + 1), 4);
    }
    Stat(7, n);
    return true;
}

// 0x102B7C00 -> FUN_102b3420: true when the reply is a NUL-terminated string.
bool AscMemoryBridge::Command6C(uint32_t a, uint32_t b, std::string& out)
{
    out.clear();
    Message req;
    req.cmd = 0x6C;
    req.a = a;
    req.c = b;
    const Message res = Exchange(Instance(), req);
    if (res.cmd != 0 || res.blob.empty())
        return false;
    out.assign(res.blob.begin(), res.blob.end());
    if (out.back() != '\0')
        return false;
    Stat(8, out.size());
    return true;
}

// 0x102B7BB0 -> FUN_102b2e70. Deviation: with a zero table, no data or no size the original returns an
// uninitialised stack byte; here false.
bool AscMemoryBridge::Command73(uint32_t a, uint32_t b, const void* src, uint32_t size)
{
    Stat(4, size);
    if (a == 0 || src == nullptr || size == 0)
        return false;
    Message req;
    req.cmd = 0x73;
    req.a = a;
    req.b = size;
    req.c = b;
    const uint8_t* p = static_cast<const uint8_t*>(src);
    req.blob.assign(p, p + size);
    return Exchange(Instance(), req).cmd == 0;
}

// 0x102B7CB0 -> FUN_102b4280
bool AscMemoryBridge::Command6D(uint32_t a, uint32_t c, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out,
                                const std::vector<uint32_t>& extra)
{
    Stat(9, keys.size());
    return BatchedQuery(0x6D, a, c, keys, out, extra);
}

// 0x102B7C40 -> FUN_102b34f0: no extra keys.
bool AscMemoryBridge::Command6E(uint32_t a, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out)
{
    Stat(10, keys.size());
    return BatchedQuery(0x6E, a, 0, keys, out, std::vector<uint32_t>());
}

// 0x102B7C70 -> FUN_102b4260
bool AscMemoryBridge::Command6F(uint32_t a, const std::vector<uint32_t>& keys, std::vector<uint8_t>& out,
                                const std::vector<uint32_t>& extra)
{
    Stat(11, keys.size());
    return BatchedQuery(0x6F, a, 0, keys, out, extra);
}
