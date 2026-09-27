#pragma once
// The reconstruction's own developer log: Logs\Extensions.log beside the exe + OutputDebugString. Not part of
// the original, so OFF by default; the client command line switch `-extlog 1` turns it on (Init reads it).
namespace AscLog
{
    void Init();
    void Printf(const char* fmt, ...);
}
