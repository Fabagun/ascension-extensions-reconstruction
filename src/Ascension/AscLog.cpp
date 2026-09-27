#include <Ascension/AscLog.hpp>
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace AscLog
{
    static FILE* g_file = nullptr;
    static bool g_on = false;

    // "-extlog <n>" (or /extlog, --extlog), value parsed like -autologin / -renderdebug: atoi(n) != 0.
    // Parsed from GetCommandLineA directly: Init runs at attach, where CommandLineToArgvW (shell32) is
    // better avoided.
    static bool Requested()
    {
        const char* p = GetCommandLineA();
        char token[256];
        bool want = false;   // the previous token was the switch
        while (p && *p)
        {
            while (*p == ' ' || *p == '\t')
                ++p;
            if (!*p)
                break;
            size_t n = 0;
            bool quoted = false;
            for (; *p && (quoted || (*p != ' ' && *p != '\t')); ++p)
            {
                if (*p == '"')
                    quoted = !quoted;
                else if (n + 1 < sizeof(token))
                    token[n++] = *p;
            }
            token[n] = 0;
            if (want)
                return atoi(token) != 0;
            const char* t = token;
            if (*t == '-' || *t == '/')
            {
                ++t;
                if (*t == '-')
                    ++t;
                want = _stricmp(t, "extlog") == 0;
            }
        }
        return false;
    }

    void Init()
    {
        g_on = Requested();
        if (!g_on)
            return;
        CreateDirectoryA("Logs", nullptr);
        g_file = fopen("Logs\\Extensions.log", "a");
        Printf("=== Extensions.dll (AscensionRebirth reconstruction) attached, pid %lu ===", GetCurrentProcessId());
    }

    void Printf(const char* fmt, ...)
    {
        if (!g_on)
            return;
        char buf[9216];
        va_list ap;
        va_start(ap, fmt);
        int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
        va_end(ap);
        if (n < 0) n = (int)sizeof(buf) - 2;
        buf[n] = 0;
        char line[2100];
        time_t t = time(nullptr);
        struct tm* tm = localtime(&t);
        _snprintf(line, sizeof(line), "%02d:%02d:%02d [Ext] %s\n", tm->tm_hour, tm->tm_min, tm->tm_sec, buf);
        OutputDebugStringA(line);
        if (g_file) { fputs(line, g_file); fflush(g_file); }
    }
}
