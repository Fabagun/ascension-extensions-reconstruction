// The original's crash reporter: WheatyExceptionReport, as TrinityCore ships it (its RTTI names survive in the
// DLL: WheatyExceptionReport::_GetWindowsVersionFromWMI's lambdas), linked into Extensions.dll and installed by
// a CRT static initializer (0x1007EB10, table slot 0x10B19E98) -- before DllMain, as the object below is.
//
//   Install (0x1007EB10): the unhandled-exception filter FUN_10a239f0 and the CRT invalid-parameter handler
//   FUN_10a239d0 (an access violation), keeping both previous handlers (0x10D3CDE4 / 0x10D3CDE8), which the
//   teardown (0x10B18320) puts back.
//   Filter (FUN_10a239f0), once per process: <exe dir>\Crashes\<exe>_[day-month_hour-minute-second].dmp
//   (MiniDumpWithIndirectlyReferencedMemory; an assertion 0xC0000420 adds its message as a comment stream)
//   and .txt (FUN_10a228e0: date, hardware and OS, exception, fault address, registers, an uncaught C++
//   exception's what() and its object's fields, then the call stack of this thread and every other one).
//   Then the previous filter decides; with none, EXCEPTION_EXECUTE_HANDLER.
//
// Transcribed from the decompile, bugs included (IMPROVEMENTS.md "crash reporter"). Source provenance:
// TrinityCore's WheatyExceptionReport is GPL-2; this repository is MIT.
#include <windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#include <Wbemidl.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <memory>
#include <mutex>
#include <set>
#include <stack>
#include <string>
#include <typeinfo>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

extern "C" PVOID __cdecl __RTDynamicCast(PVOID inptr, LONG VfDelta, PVOID SrcType, PVOID TargetType, BOOL isReference) noexcept(false);

namespace
{
    enum BasicType
    {
        btNoType = 0, btVoid = 1, btChar = 2, btWChar = 3, btInt = 6, btUInt = 7, btFloat = 8, btBCD = 9, btBool = 10,
        btLong = 13, btULong = 14, btCurrency = 25, btDate = 26, btVariant = 27, btComplex = 28, btBit = 29,
        btBSTR = 30, btHresult = 31,
        btStdString = 101,
    };

    // 0x10B5EDF8
    const char* const rgBaseType[] = {
        "<user defined>", "void", "char", "wchar_t*", "signed char", "unsigned char", "int", "unsigned int",
        "float", "<BCD>", "bool", "short", "unsigned short", "long", "unsigned long", "int8", "int16", "int32",
        "int64", "int128", "uint8", "uint16", "uint32", "uint64", "uint128", "<currency>", "<date>", "VARIANT",
        "<complex>", "<bit>", "BSTR", "HRESULT",
    };

    // DIA's SymTagEnum / DataKind (cvconst.h), the values SymGetTypeInfo returns.
    enum
    {
        SymTagFunction = 5, SymTagData = 7, SymTagUDT = 11, SymTagEnum = 12, SymTagPointerType = 14, SymTagArrayType = 15,
        SymTagBaseType = 16, SymTagTypedef = 17, SymTagBaseClass = 18, SymTagVTable = 25,
    };
    enum { DataIsStaticLocal = 2, DataIsGlobal = 6, DataIsStaticMember = 8 };

    const int WER_MAX_ARRAY_ELEMENTS_COUNT = 10;
    const int WER_MAX_NESTING_LEVEL = 4;
    const int WER_LARGE_BUFFER_SIZE = 1024;
    const int WER_SMALL_BUFFER_SIZE = 50;

    // 0x7C bytes: Prefix +0x00, Type +0x18, Suffix +0x30, Name +0x48, Value +0x60, Logged +0x78, HasChildren +0x79
    struct SymbolDetail
    {
        std::string Prefix, Type, Suffix, Name, Value;
        bool Logged = false;
        bool HasChildren = false;

        // FUN_10a23770
        std::string ToString()
        {
            Logged = true;
            std::string formatted = Prefix + Type + Suffix;
            if (!Name.empty())
            {
                if (!formatted.empty())
                    formatted.append(" ");
                formatted.append(Name);
            }
            if (!Value.empty())
            {
                if (Name == "passwd" || Name == "password")
                    Value = "<sensitive data>";
                formatted.append(" = " + Value);
            }
            return formatted;
        }

        bool empty() const { return Value.empty() && !HasChildren; }
    };

    // Ordered by offset, then type (the set at 0x10D3CDFC).
    struct SymbolPair
    {
        DWORD _type;
        DWORD_PTR _offset;
        bool operator<(const SymbolPair& other) const
        {
            return _offset < other._offset || (_offset == other._offset && _type < other._type);
        }
    };

    struct EnumerateSymbolsCallbackContext
    {
        LPSTACKFRAME64 sf;
        PCONTEXT context;
    };

    typedef LONG(NTAPI* pRtlGetVersion)(PRTL_OSVERSIONINFOW);

    LPTOP_LEVEL_EXCEPTION_FILTER m_previousFilter = nullptr;   // 0x10D3CDE4
    _invalid_parameter_handler m_previousCrtHandler = nullptr; // 0x10D3CDE8
    FILE* m_hReportFile = nullptr;                             // 0x10D3CDEC
    HANDLE m_hDumpFile = nullptr;                              // 0x10D3CDF0
    HANDLE m_hProcess = nullptr;                               // 0x10D3CDF4
    pRtlGetVersion RtlGetVersion = nullptr;                    // 0x10D3CDF8
    bool alreadyCrashed = false;                               // 0x10D3CCDC
    char m_szDumpFileName[MAX_PATH];                           // 0x10D3CCE0
    char m_szLogFileName[MAX_PATH];                            // 0x10D3CBD8
    std::mutex alreadyCrashedLock;                             // 0x10D3CE18
    std::set<SymbolPair> symbols;                              // 0x10D3CDFC
    std::stack<SymbolDetail> symbolDetails;                    // 0x10D3CE08
    char szExceptionBuffer[512];                               // 0x10D3CE50

    // FUN_10a23390
    int Log(const char* format, ...)
    {
        va_list args;
        va_start(args, format);
        const int r = vfprintf(m_hReportFile, format, args);
        va_end(args);
        return r;
    }

    // FUN_10a20ed0
    void ClearSymbols()
    {
        symbols.clear();
        while (!symbolDetails.empty())
            symbolDetails.pop();
    }

    // FUN_10a233c0
    void PrintSymbolDetail()
    {
        if (symbolDetails.empty())
            return;
        if (symbolDetails.top().Logged || symbolDetails.top().empty())
            return;
        for (size_t i = 0; i < symbolDetails.size(); i++)
            Log("\t");
        Log("%s\r\n", symbolDetails.top().ToString().c_str());
    }

    // FUN_10a23670
    void PushSymbolDetail()
    {
        PrintSymbolDetail();
        symbolDetails.emplace();
    }

    void PopSymbolDetail()
    {
        PrintSymbolDetail();
        symbolDetails.pop();
    }

    bool StoreSymbol(DWORD type, DWORD_PTR offset)
    {
        return symbols.insert(SymbolPair{type, offset}).second;
    }

    // FUN_10a22dd0
    const char* GetExceptionString(DWORD dwCode)
    {
        switch (dwCode)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
        case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
        case EXCEPTION_SINGLE_STEP: return "SINGLE_STEP";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT: return "FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW: return "FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK: return "FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW: return "FLT_UNDERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
        case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
        case EXCEPTION_GUARD_PAGE: return "GUARD_PAGE";
        case EXCEPTION_INVALID_HANDLE: return "INVALID_HANDLE";
        case 0xE06D7363: return "Unhandled C++ exception";
        }
        FormatMessageA(FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE, GetModuleHandleA("NTDLL.DLL"), dwCode, 0,
                       szExceptionBuffer, sizeof(szExceptionBuffer), nullptr);
        return szExceptionBuffer;
    }

    // FUN_10a22400: the message is LocalAlloc'd and never freed.
    char* ErrorMessage(DWORD dw)
    {
        LPSTR lpMsgBuf;
        if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM, nullptr, dw,
                           MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPSTR>(&lpMsgBuf), 0, nullptr))
            return lpMsgBuf;
        char* msgBuf = static_cast<char*>(LocalAlloc(LPTR, 30));
        sprintf(msgBuf, "Unknown error: %u", static_cast<unsigned>(dw));
        return msgBuf;
    }

    // FUN_10a232f0
    bool GetLogicalAddress(PVOID addr, PTSTR szModule, DWORD len, DWORD& section, DWORD_PTR& offset)
    {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
            return false;
        const DWORD_PTR hMod = reinterpret_cast<DWORD_PTR>(mbi.AllocationBase);
        if (!hMod)
            return false;
        if (!GetModuleFileNameA(reinterpret_cast<HMODULE>(hMod), szModule, len))
            return false;
        PIMAGE_DOS_HEADER pDosHdr = reinterpret_cast<PIMAGE_DOS_HEADER>(hMod);
        PIMAGE_NT_HEADERS pNtHdr = reinterpret_cast<PIMAGE_NT_HEADERS>(hMod + pDosHdr->e_lfanew);
        PIMAGE_SECTION_HEADER pSection = IMAGE_FIRST_SECTION(pNtHdr);
        const DWORD_PTR rva = reinterpret_cast<DWORD_PTR>(addr) - hMod;
        for (unsigned i = 0; i < pNtHdr->FileHeader.NumberOfSections; i++, pSection++)
        {
            const DWORD sectionStart = pSection->VirtualAddress;
            const DWORD sectionEnd = sectionStart + (pSection->SizeOfRawData > pSection->Misc.VirtualSize ? pSection->SizeOfRawData : pSection->Misc.VirtualSize);
            if ((rva >= sectionStart) && (rva <= sectionEnd))
            {
                section = i + 1;
                offset = rva - sectionStart;
                return true;
            }
        }
        return false;
    }

    // FUN_10a24430
    BOOL GetWindowsVersionFromWMI(char* szVersion, DWORD cntMax)
    {
        static const CLSID kWbemLocator = {0x4590F811, 0x1D3A, 0x11D0, {0x89, 0x1F, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};
        static const IID kIWbemLocator = {0xDC12A687, 0x737F, 0x11CF, {0x88, 0x4D, 0x00, 0xAA, 0x00, 0x4B, 0x2E, 0x24}};

        HRESULT hres = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hres))
            return FALSE;
        std::shared_ptr<void> com(nullptr, [](void*) { CoUninitialize(); });

        hres = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                                    nullptr, EOAC_NONE, nullptr);
        if (FAILED(hres))
            return FALSE;

        std::shared_ptr<IWbemLocator> loc = []() -> std::shared_ptr<IWbemLocator> {
            IWbemLocator* tmp = nullptr;
            if (FAILED(CoCreateInstance(kWbemLocator, nullptr, CLSCTX_INPROC_SERVER, kIWbemLocator, reinterpret_cast<LPVOID*>(&tmp))))
                return nullptr;
            return {tmp, [](IWbemLocator* ptr) { if (ptr) ptr->Release(); }};
        }();
        if (!loc)
            return FALSE;

        std::shared_ptr<IWbemServices> svc = [loc]() -> std::shared_ptr<IWbemServices> {
            IWbemServices* tmp = nullptr;
            BSTR ns = SysAllocString(L"ROOT\\CIMV2");
            const HRESULT h = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &tmp);
            SysFreeString(ns);
            if (FAILED(h))
                return nullptr;
            return {tmp, [](IWbemServices* ptr) { if (ptr) ptr->Release(); }};
        }();
        if (!svc)
            return FALSE;

        hres = CoSetProxyBlanket(svc.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                 RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        if (FAILED(hres))
            return FALSE;

        std::shared_ptr<IEnumWbemClassObject> queryResult = [svc]() -> std::shared_ptr<IEnumWbemClassObject> {
            IEnumWbemClassObject* tmp = nullptr;
            BSTR language = SysAllocString(L"WQL");
            BSTR query = SysAllocString(L"SELECT Caption, CSDVersion FROM Win32_OperatingSystem");
            const HRESULT h = svc->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &tmp);
            SysFreeString(query);
            SysFreeString(language);
            if (FAILED(h))
                return nullptr;
            return {tmp, [](IEnumWbemClassObject* ptr) { if (ptr) ptr->Release(); }};
        }();

        BOOL result = FALSE;
        if (queryResult)
        {
            IWbemClassObject* fields = nullptr;
            ULONG rows = 0;
            queryResult->Next(WBEM_INFINITE, 1, &fields, &rows);
            while (rows)
            {
                VARIANT field;
                VariantInit(&field);
                fields->Get(L"Caption", 0, &field, nullptr, nullptr);
                char buf[256];
                memset(buf, 0, sizeof(buf));
                wcstombs_s(nullptr, buf, sizeof(buf), field.bstrVal, sizeof(buf));   // no vt check, as the original
                strncat(szVersion, buf, cntMax);
                VariantClear(&field);
                fields->Get(L"CSDVersion", 0, &field, nullptr, nullptr);
                if (field.vt == VT_BSTR)
                {
                    strncat(szVersion, " ", cntMax);
                    memset(buf, 0, sizeof(buf));
                    wcstombs_s(nullptr, buf, sizeof(buf), field.bstrVal, sizeof(buf));
                    if (strlen(buf))
                        strncat(szVersion, buf, cntMax);
                }
                VariantClear(&field);
                fields->Release();
                fields = nullptr;
                rows = 0;
                result = TRUE;
                queryResult->Next(WBEM_INFINITE, 1, &fields, &rows);
            }
        }
        return result;
    }

    // FUN_10a23ff0. A second RtlGetVersion that SUCCEEDS returns false (the original's `!RtlGetVersion`).
    BOOL GetWindowsVersion(char* szVersion, DWORD cntMax)
    {
        *szVersion = '\0';
        if (GetWindowsVersionFromWMI(szVersion, cntMax))
            return TRUE;

        RTL_OSVERSIONINFOEXW osvi = {};
        osvi.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOEXW);
        const LONG bVersionEx = RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi));
        if (bVersionEx < 0)
        {
            osvi.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOW);
            if (!RtlGetVersion(reinterpret_cast<PRTL_OSVERSIONINFOW>(&osvi)))
                return FALSE;
        }

        char szCSDVersion[256];
        wcstombs_s(nullptr, szCSDVersion, sizeof(szCSDVersion), osvi.szCSDVersion, 256);
        char wszTmp[128];
        if (osvi.dwPlatformId != VER_PLATFORM_WIN32_NT)
        {
            sprintf(wszTmp, "%s (Version %d.%d, Build %d)", szCSDVersion, osvi.dwMajorVersion, osvi.dwMinorVersion,
                    osvi.dwBuildNumber & 0xFFFF);
            strncat(szVersion, wszTmp, cntMax);
            return TRUE;
        }

        const WORD suiteMask = osvi.wSuiteMask;
        const BYTE productType = osvi.wProductType;
        const char* name = nullptr;
        if (osvi.dwMajorVersion == 10)
            name = productType == VER_NT_WORKSTATION ? "Windows 10 " : "Windows Server 2016 ";
        else if (osvi.dwMajorVersion == 6)
        {
            if (productType == VER_NT_WORKSTATION)
                name = osvi.dwMinorVersion == 3 ? "Windows 8.1 " : osvi.dwMinorVersion == 2 ? "Windows 8 "
                     : osvi.dwMinorVersion == 1 ? "Windows 7 " : "Windows Vista ";
            else
                name = osvi.dwMinorVersion == 3 ? "Windows Server 2012 R2 " : osvi.dwMinorVersion == 2 ? "Windows Server 2012 "
                     : osvi.dwMinorVersion == 1 ? "Windows Server 2008 R2 " : "Windows Server 2008 ";
        }
        else if (osvi.dwMajorVersion == 5)
        {
            if (osvi.dwMinorVersion == 2)
                name = "Microsoft Windows Server 2003 ";
            else if (osvi.dwMinorVersion == 1)
                name = "Microsoft Windows XP ";
            else if (osvi.dwMinorVersion == 0)
                name = "Microsoft Windows 2000 ";
        }
        else if (osvi.dwMajorVersion < 5)
            name = "Microsoft Windows NT ";
        if (name)
            strncat(szVersion, name, cntMax);

        // The edition, only from the extended structure.
        if (bVersionEx >= 0)
        {
            const char* edition = nullptr;
            if (productType == VER_NT_WORKSTATION)
            {
                if (osvi.dwMajorVersion == 4)
                    edition = "Workstation 4.0 ";
                else if (suiteMask & VER_SUITE_PERSONAL)
                    edition = "Home Edition ";
                else if (suiteMask & VER_SUITE_EMBEDDEDNT)
                    edition = "Embedded ";
                else
                    edition = "Professional ";
            }
            else if (productType == VER_NT_SERVER)
            {
                if (osvi.dwMajorVersion == 6 || osvi.dwMajorVersion == 10)
                {
                    if (suiteMask & VER_SUITE_SMALLBUSINESS_RESTRICTED)
                        edition = "Essentials ";
                    else if (suiteMask & VER_SUITE_DATACENTER)
                        edition = "Datacenter ";
                    else if (suiteMask & VER_SUITE_ENTERPRISE)
                        edition = "Enterprise ";
                    else
                        edition = "Standard ";
                }
                else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 2)
                {
                    if (suiteMask & VER_SUITE_DATACENTER)
                        edition = "Datacenter Edition ";
                    else if (suiteMask & VER_SUITE_ENTERPRISE)
                        edition = "Enterprise Edition ";
                    else if (suiteMask == VER_SUITE_BLADE)
                        edition = "Web Edition ";
                    else
                        edition = "Standard Edition ";
                }
                else if (osvi.dwMajorVersion == 5 && osvi.dwMinorVersion == 0)
                {
                    if (suiteMask & VER_SUITE_DATACENTER)
                        edition = "Datacenter Server ";
                    else if (suiteMask & VER_SUITE_ENTERPRISE)
                        edition = "Advanced Server ";
                    else
                        edition = "Server ";
                }
                else
                    edition = (suiteMask & VER_SUITE_ENTERPRISE) ? "Server 4.0, Enterprise Edition " : "Server 4.0 ";
            }
            if (edition)
                strncat(szVersion, edition, cntMax);
        }

        if (osvi.dwMajorVersion == 4 && _stricmp(szCSDVersion, "Service Pack 6") == 0)
        {
            HKEY hKey;
            const LONG lRet = RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Hotfix\\Q246009",
                                            0, KEY_QUERY_VALUE, &hKey);
            if (lRet == ERROR_SUCCESS)
                sprintf(wszTmp, "Service Pack 6a (Version %d.%d, Build %d)", osvi.dwMajorVersion, osvi.dwMinorVersion,
                        osvi.dwBuildNumber & 0xFFFF);
            else
                sprintf(wszTmp, "%s (Version %d.%d, Build %d)", szCSDVersion, osvi.dwMajorVersion, osvi.dwMinorVersion,
                        osvi.dwBuildNumber & 0xFFFF);
            strncat(szVersion, wszTmp, cntMax);
            RegCloseKey(hKey);
        }
        else
        {
            if (strlen(szCSDVersion) == 0)
                sprintf(wszTmp, "(Version %d.%d, Build %d)", osvi.dwMajorVersion, osvi.dwMinorVersion, osvi.dwBuildNumber & 0xFFFF);
            else
                sprintf(wszTmp, "%s (Version %d.%d, Build %d)", szCSDVersion, osvi.dwMajorVersion, osvi.dwMinorVersion,
                        osvi.dwBuildNumber & 0xFFFF);
            strncat(szVersion, wszTmp, cntMax);
        }
        return TRUE;
    }

    // FUN_10a234d0. When the value query fails the key stays open.
    void PrintSystemInfo()
    {
        SYSTEM_INFO SystemInfo;
        GetSystemInfo(&SystemInfo);
        MEMORYSTATUS MemoryStatus;
        MemoryStatus.dwLength = sizeof(MEMORYSTATUS);
        GlobalMemoryStatus(&MemoryStatus);
        char sString[1024];
        Log("//=====================================================\r\n");
        HKEY hKey;
        BYTE processorName[2048];
        DWORD size = sizeof(processorName);
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS &&
            RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr, processorName, &size) == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            const char* p = reinterpret_cast<const char*>(processorName);
            sString[0] = '\0';
            while (iswspace(static_cast<wint_t>(static_cast<short>(*p))))
                ++p;
            strncpy(sString, p, sizeof(sString));
            Log("*** Hardware ***\r\nProcessor: %s\r\nNumber Of Processors: %d\r\nPhysical Memory: %d KB (Available: %d KB)\r\nCommit Charge Limit: %d KB\r\n",
                sString, SystemInfo.dwNumberOfProcessors, MemoryStatus.dwTotalPhys / 0x400, MemoryStatus.dwAvailPhys / 0x400,
                MemoryStatus.dwTotalPageFile / 0x400);
        }
        else
            Log("*** Hardware ***\r\nProcessor: <unknown>\r\nNumber Of Processors: %d\r\nPhysical Memory: %d KB (Available: %d KB)\r\nCommit Charge Limit: %d KB\r\n",
                SystemInfo.dwNumberOfProcessors, MemoryStatus.dwTotalPhys / 0x400, MemoryStatus.dwAvailPhys / 0x400,
                MemoryStatus.dwTotalPageFile / 0x400);

        if (GetWindowsVersion(sString, sizeof(sString)))
            Log("\r\n*** Operation System ***\r\n%s\r\n", sString);
        else
            Log("\r\n*** Operation System:\r\n<unknown>\r\n");
    }

    // FUN_10a22d50
    BasicType GetBasicType(DWORD typeIndex, DWORD64 modBase)
    {
        BasicType basicType;
        if (SymGetTypeInfo(m_hProcess, modBase, typeIndex, TI_GET_BASETYPE, &basicType))
            return basicType;
        DWORD typeId;
        if (SymGetTypeInfo(m_hProcess, modBase, typeIndex, TI_GET_TYPEID, &typeId))
            if (SymGetTypeInfo(m_hProcess, modBase, typeId, TI_GET_BASETYPE, &basicType))
                return basicType;
        return btNoType;
    }

    // FUN_10a23030: the value of a CodeView x86 register (CV_REG_AL .. CV_REG_EDI, CV_REG_EIP).
    bool GetIntegerRegisterValue(PCONTEXT context, ULONG registerId, DWORD_PTR& value)
    {
        const uint8_t* c = reinterpret_cast<const uint8_t*>(context);
        auto b = [&](uint32_t off) { value = c[off]; return true; };
        auto w = [&](uint32_t off) { value = *reinterpret_cast<const WORD*>(c + off); return true; };
        auto d = [&](uint32_t off) { value = *reinterpret_cast<const DWORD*>(c + off); return true; };
        switch (registerId)
        {
        case 1: return b(0xB0);    case 2: return b(0xAC);    case 3: return b(0xA8);    case 4: return b(0xA4);
        case 5: return b(0xB1);    case 6: return b(0xAD);    case 7: return b(0xA9);    case 8: return b(0xA5);
        case 9: return w(0xB0);    case 10: return w(0xAC);   case 11: return w(0xA8);   case 12: return w(0xA4);
        case 13: return w(0xC4);   case 14: return w(0xB4);   case 15: return w(0xA0);   case 16: return w(0x9C);
        case 17: return d(0xB0);   case 18: return d(0xAC);   case 19: return d(0xA8);   case 20: return d(0xA4);
        case 21: return d(0xC4);   case 22: return d(0xB4);   case 23: return d(0xA0);   case 24: return d(0x9C);
        case 33: return d(0xB8);
        }
        return false;
    }

    // FUN_10a22450
    void FormatOutputValue(char* pszCurrBuffer, BasicType basicType, DWORD64 length, PVOID pAddress, size_t bufferSize,
                           size_t countOverride = 0)
    {
        __try
        {
            switch (basicType)
            {
            case btChar:
            {
                if (countOverride != 0)
                    length = countOverride;
                else
                    length = strlen(static_cast<char*>(pAddress));
                if (length > bufferSize - 6)
                    sprintf(pszCurrBuffer, "\"%.*s...\"", static_cast<DWORD>(bufferSize - 6), static_cast<char*>(pAddress));
                else
                    sprintf(pszCurrBuffer, "\"%.*s\"", static_cast<DWORD>(length), static_cast<char*>(pAddress));
                break;
            }
            case btStdString:
            {
                std::string* value = static_cast<std::string*>(pAddress);
                if (value->length() > bufferSize - 6)
                    sprintf(pszCurrBuffer, "\"%.*s...\"", static_cast<DWORD>(bufferSize - 6), value->c_str());
                else
                    sprintf(pszCurrBuffer, "\"%s\"", value->c_str());
                break;
            }
            default:
                if (length == 1)
                    sprintf(pszCurrBuffer, "0x%X", *static_cast<PBYTE>(pAddress));
                else if (length == 2)
                    sprintf(pszCurrBuffer, "0x%X", *static_cast<PWORD>(pAddress));
                else if (length == 4)
                {
                    if (basicType == btFloat)
                        sprintf(pszCurrBuffer, "%f", *static_cast<PFLOAT>(pAddress));
                    else
                        sprintf(pszCurrBuffer, "0x%X", *static_cast<PDWORD>(pAddress));
                }
                else if (length == 8)
                {
                    if (basicType == btFloat)
                        sprintf(pszCurrBuffer, "%f", *static_cast<double*>(pAddress));
                    else
                        sprintf(pszCurrBuffer, "0x%I64X", *static_cast<DWORD64*>(pAddress));
                }
                else
                    sprintf(pszCurrBuffer, "0x%X", static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(pAddress)));
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            sprintf(pszCurrBuffer, "0x%X <Unable to read memory>", static_cast<DWORD>(reinterpret_cast<DWORD_PTR>(pAddress)));
        }
    }

    // FUN_10a20f70
    DWORD_PTR DereferenceUnsafePointer(DWORD_PTR address)
    {
        __try
        {
            return *reinterpret_cast<PDWORD_PTR>(address);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return static_cast<DWORD_PTR>(-1);
        }
    }

    // FUN_10a20ff0
    void DumpTypeIndex(DWORD64 modBase, DWORD dwTypeIndex, DWORD_PTR offset, bool& bHandled, const char* Name,
                       char* /*suffix*/, bool newSymbol, bool logChildren)
    {
        bHandled = false;
        if (newSymbol)
            PushSymbolDetail();

        DWORD typeTag;
        if (!SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_SYMTAG, &typeTag))
            return;

        WCHAR* pwszTypeName;
        if (SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_SYMNAME, &pwszTypeName))
        {
            if (wcscmp(pwszTypeName, L"std::basic_string<char,std::char_traits<char>,std::allocator<char> >") == 0)
            {
                LocalFree(pwszTypeName);
                symbolDetails.top().Type = "std::string";
                char buffer[50];
                FormatOutputValue(buffer, btStdString, 0, reinterpret_cast<PVOID>(offset), sizeof(buffer));
                symbolDetails.top().Value = buffer;
                if (Name != nullptr && Name[0] != '\0')
                    symbolDetails.top().Name = Name;
                bHandled = true;
                return;
            }
            char buffer[WER_LARGE_BUFFER_SIZE];
            wcstombs(buffer, pwszTypeName, sizeof(buffer));
            buffer[WER_LARGE_BUFFER_SIZE - 1] = '\0';
            if (Name != nullptr && Name[0] != '\0')
            {
                symbolDetails.top().Type = buffer;
                symbolDetails.top().Name = Name;
            }
            else if (buffer[0] != '\0')
                symbolDetails.top().Name = buffer;
            LocalFree(pwszTypeName);
        }
        else if (Name != nullptr && Name[0] != '\0')
            symbolDetails.top().Name = Name;

        if (!StoreSymbol(dwTypeIndex, offset))
        {
            if (typeTag == SymTagBaseClass)
                bHandled = true;
            return;
        }

        DWORD innerTypeID;
        switch (typeTag)
        {
        case SymTagPointerType:
            if (SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_TYPEID, &innerTypeID))
            {
                if (Name != nullptr && Name[0] != '\0')
                    symbolDetails.top().Name = Name;
                BOOL isReference;
                SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_IS_REFERENCE, &isReference);
                char addressStr[40];
                memset(addressStr, 0, sizeof(addressStr));
                symbolDetails.top().Suffix += isReference ? "&" : "*";
                const DWORD_PTR address = DereferenceUnsafePointer(offset);
                char buffer[WER_LARGE_BUFFER_SIZE];
                FormatOutputValue(buffer, btVoid, sizeof(PVOID), reinterpret_cast<PVOID>(offset), sizeof(buffer));
                symbolDetails.top().Value = buffer;
                if (symbolDetails.size() >= WER_MAX_NESTING_LEVEL)
                    logChildren = false;
                if (address == NULL || address == static_cast<DWORD_PTR>(-1))
                    logChildren = false;
                DumpTypeIndex(modBase, innerTypeID, address, bHandled, Name, addressStr, false, logChildren);
                if (!bHandled)
                {
                    const BasicType basicType = GetBasicType(dwTypeIndex, modBase);
                    if (symbolDetails.top().Type.empty())
                        symbolDetails.top().Type = rgBaseType[basicType];
                    if (address == NULL)
                        symbolDetails.top().Value = "NULL";
                    else if (address == static_cast<DWORD_PTR>(-1))
                        symbolDetails.top().Value = "<Unable to read memory>";
                    else
                    {
                        ULONG64 length;
                        SymGetTypeInfo(m_hProcess, modBase, innerTypeID, TI_GET_LENGTH, &length);
                        char buffer2[50];
                        FormatOutputValue(buffer2, basicType, length, reinterpret_cast<PVOID>(address), sizeof(buffer2));
                        symbolDetails.top().Value = buffer2;
                    }
                    bHandled = true;
                    return;
                }
                else if (address == NULL)
                    symbolDetails.top().Value = "NULL";
                else if (address == static_cast<DWORD_PTR>(-1))
                {
                    symbolDetails.top().Value = "<Unable to read memory>";
                    bHandled = true;
                    return;
                }
            }
            break;
        case SymTagData:
            if (SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_TYPEID, &innerTypeID))
            {
                DWORD innerTypeTag;
                if (!SymGetTypeInfo(m_hProcess, modBase, innerTypeID, TI_GET_SYMTAG, &innerTypeTag))
                    break;
                switch (innerTypeTag)
                {
                case SymTagUDT:
                    if (symbolDetails.size() >= WER_MAX_NESTING_LEVEL)
                        logChildren = false;
                    DumpTypeIndex(modBase, innerTypeID, offset, bHandled, symbolDetails.top().Name.c_str(), const_cast<char*>(""), false, logChildren);
                    break;
                case SymTagPointerType:
                    if (Name != nullptr && Name[0] != '\0')
                        symbolDetails.top().Name = Name;
                    DumpTypeIndex(modBase, innerTypeID, offset, bHandled, symbolDetails.top().Name.c_str(), const_cast<char*>(""), false, logChildren);
                    break;
                case SymTagArrayType:
                    DumpTypeIndex(modBase, innerTypeID, offset, bHandled, symbolDetails.top().Name.c_str(), const_cast<char*>(""), false, logChildren);
                    break;
                default:
                    break;
                }
            }
            break;
        case SymTagArrayType:
            if (SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_TYPEID, &innerTypeID))
            {
                symbolDetails.top().HasChildren = true;
                BasicType basicType = btNoType;
                DumpTypeIndex(modBase, innerTypeID, offset, bHandled, Name, const_cast<char*>(""), false, false);
                // The array itself has no value, only its elements.
                std::string firstElementValue = symbolDetails.top().Value;
                symbolDetails.top().Value.clear();
                DWORD elementsCount;
                if (SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_COUNT, &elementsCount))
                    symbolDetails.top().Suffix += "[" + std::to_string(elementsCount) + "]";
                else
                    symbolDetails.top().Suffix += "[<unknown count>]";
                if (!bHandled)
                {
                    basicType = GetBasicType(dwTypeIndex, modBase);
                    if (symbolDetails.top().Type.empty())
                        symbolDetails.top().Type = rgBaseType[basicType];
                    bHandled = true;
                }
                ULONG64 length;
                SymGetTypeInfo(m_hProcess, modBase, innerTypeID, TI_GET_LENGTH, &length);
                char buffer[50];
                switch (basicType)
                {
                case btChar:
                case btStdString:
                    FormatOutputValue(buffer, basicType, length, reinterpret_cast<PVOID>(offset), sizeof(buffer), elementsCount);
                    symbolDetails.top().Value = buffer;
                    break;
                default:
                    for (DWORD index = 0; index < elementsCount && index < WER_MAX_ARRAY_ELEMENTS_COUNT; index++)
                    {
                        bool elementHandled = false;
                        PushSymbolDetail();
                        if (index == 0)
                        {
                            if (firstElementValue.empty())
                            {
                                FormatOutputValue(buffer, basicType, length, reinterpret_cast<PVOID>(offset + length * index), sizeof(buffer));
                                firstElementValue = buffer;
                            }
                            symbolDetails.top().Value = firstElementValue;
                        }
                        else
                        {
                            DumpTypeIndex(modBase, innerTypeID, static_cast<DWORD_PTR>(offset + length * index), elementHandled, "",
                                          const_cast<char*>(""), false, false);
                            if (!elementHandled)
                            {
                                FormatOutputValue(buffer, basicType, length, reinterpret_cast<PVOID>(offset + length * index), sizeof(buffer));
                                symbolDetails.top().Value = buffer;
                            }
                        }
                        symbolDetails.top().Prefix.clear();
                        symbolDetails.top().Type.clear();
                        symbolDetails.top().Suffix = "[" + std::to_string(index) + "]";
                        symbolDetails.top().Name.clear();
                        PopSymbolDetail();
                    }
                    break;
                }
                return;
            }
            break;
        case SymTagBaseType:
            break;
        case SymTagEnum:
            return;
        default:
            break;
        }

        DWORD dwChildrenCount = 0;
        SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_GET_CHILDRENCOUNT, &dwChildrenCount);
        if (!dwChildrenCount)
            return;

        struct FINDCHILDREN : TI_FINDCHILDREN_PARAMS
        {
            ULONG MoreChildIds[1024 * 2];
            FINDCHILDREN() { Count = sizeof(MoreChildIds) / sizeof(MoreChildIds[0]); }
        } children;
        children.Count = dwChildrenCount;
        children.Start = 0;
        if (!SymGetTypeInfo(m_hProcess, modBase, dwTypeIndex, TI_FINDCHILDREN, &children))
            return;

        for (unsigned i = 0; i < dwChildrenCount; i++)
        {
            DWORD symTag;
            SymGetTypeInfo(m_hProcess, modBase, children.ChildId[i], TI_GET_SYMTAG, &symTag);
            if (symTag == SymTagFunction || symTag == SymTagEnum || symTag == SymTagTypedef || symTag == SymTagVTable)
                continue;
            DWORD dataKind;
            SymGetTypeInfo(m_hProcess, modBase, children.ChildId[i], TI_GET_DATAKIND, &dataKind);
            if (dataKind == DataIsStaticLocal || dataKind == DataIsGlobal || dataKind == DataIsStaticMember)
                continue;

            symbolDetails.top().HasChildren = true;
            if (!logChildren)
            {
                bHandled = false;
                return;
            }

            bool bHandled2;
            const BasicType basicType = GetBasicType(children.ChildId[i], modBase);
            DWORD dwMemberOffset;
            SymGetTypeInfo(m_hProcess, modBase, children.ChildId[i], TI_GET_OFFSET, &dwMemberOffset);
            const DWORD_PTR dwFinalOffset = offset + dwMemberOffset;
            DumpTypeIndex(modBase, children.ChildId[i], dwFinalOffset, bHandled2, "", const_cast<char*>(""), true, true);
            if (!bHandled2)
            {
                if (symbolDetails.top().Type.empty())
                    symbolDetails.top().Type = rgBaseType[basicType];
                DWORD typeId;
                SymGetTypeInfo(m_hProcess, modBase, children.ChildId[i], TI_GET_TYPEID, &typeId);
                ULONG64 length;
                SymGetTypeInfo(m_hProcess, modBase, typeId, TI_GET_LENGTH, &length);
                char buffer[50];
                FormatOutputValue(buffer, basicType, length, reinterpret_cast<PVOID>(dwFinalOffset), sizeof(buffer));
                symbolDetails.top().Value = buffer;
            }
            PopSymbolDetail();
        }
        bHandled = true;
    }

    // FUN_10a22660
    bool FormatSymbolValue(PSYMBOL_INFO pSym, EnumerateSymbolsCallbackContext* pCtx)
    {
        if (pSym->Tag == SymTagFunction)
            return false;

        DWORD_PTR pVariable = 0;
        if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_REGRELATIVE)
        {
            if (pSym->Register == 0x7536)   // CV_ALLREG_VFRAME
                pVariable = static_cast<DWORD_PTR>(pCtx->sf->AddrFrame.Offset);
            else
            {
                DWORD_PTR registerValue;
                if (!GetIntegerRegisterValue(pCtx->context, pSym->Register, registerValue))
                    return false;
                pVariable = registerValue;
            }
            pVariable += static_cast<DWORD_PTR>(pSym->Address);
        }
        else if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_REGISTER)
        {
            DWORD_PTR registerValue;
            if (!GetIntegerRegisterValue(pCtx->context, pSym->Register, registerValue))
                return false;
            pVariable = registerValue;
        }
        else if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_FRAMERELATIVE)
        {
            pVariable = static_cast<DWORD_PTR>(pCtx->sf->AddrFrame.Offset);
            pVariable += static_cast<DWORD_PTR>(pSym->Address);
        }
        else
            pVariable = static_cast<DWORD_PTR>(pSym->Address);

        PushSymbolDetail();
        if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_PARAMETER)
            symbolDetails.top().Prefix = "Parameter ";
        else if (pSym->Flags & IMAGEHLP_SYMBOL_INFO_LOCAL)
            symbolDetails.top().Prefix = "Local ";

        bool bHandled;
        DumpTypeIndex(pSym->ModBase, pSym->TypeIndex, pVariable, bHandled, pSym->Name, const_cast<char*>(""), false, true);
        if (!bHandled)
        {
            const BasicType basicType = GetBasicType(pSym->TypeIndex, pSym->ModBase);
            if (symbolDetails.top().Type.empty())
                symbolDetails.top().Type = rgBaseType[basicType];
            if (pSym->Name[0] != '\0')
                symbolDetails.top().Name = pSym->Name;
            char buffer[50];
            FormatOutputValue(buffer, basicType, pSym->Size, reinterpret_cast<PVOID>(pVariable), sizeof(buffer));
            symbolDetails.top().Value = buffer;
        }
        PopSymbolDetail();
        return true;
    }

    // FUN_10a22370
    BOOL CALLBACK EnumerateSymbolsCallback(PSYMBOL_INFO pSymInfo, ULONG /*SymbolSize*/, PVOID UserContext)
    {
        __try
        {
            ClearSymbols();
            FormatSymbolValue(pSymInfo, static_cast<EnumerateSymbolsCallbackContext*>(UserContext));
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("punting on symbol %s, partial output:\r\n", pSymInfo->Name);
        }
        return TRUE;
    }

    // FUN_10a23cc0. Both callers pass bWriteVariables = false. On x86 "%08X  %08X  " takes the 64-bit PC as
    // its two words, so the second column is the PC's high half (0), not the frame.
    void WriteStackDetails(PCONTEXT pContext, bool bWriteVariables, HANDLE pThreadHandle)
    {
        Log("\r\nCall stack:\r\n");
        Log("Address   Frame     Function      SourceFile\r\n");

        STACKFRAME64 sf;
        memset(&sf, 0, sizeof(sf));
        sf.AddrPC.Offset = pContext->Eip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrStack.Offset = pContext->Esp;
        sf.AddrStack.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = pContext->Ebp;
        sf.AddrFrame.Mode = AddrModeFlat;

        while (true)
        {
            if (!StackWalk64(IMAGE_FILE_MACHINE_I386, m_hProcess, pThreadHandle != nullptr ? pThreadHandle : GetCurrentThread(), &sf,
                             pContext, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                break;
            if (sf.AddrFrame.Offset == 0)
                break;

            Log("%08X  %08X  ", sf.AddrPC.Offset, sf.AddrFrame.Offset);

            DWORD64 symDisplacement = 0;
            struct : SYMBOL_INFO
            {
                char NameBuffer[2000];
            } sip;
            sip.SizeOfStruct = sizeof(SYMBOL_INFO);
            sip.MaxNameLen = 2001;
            if (SymFromAddr(m_hProcess, sf.AddrPC.Offset, &symDisplacement, &sip))
                Log("%hs+%I64X", sip.Name, symDisplacement);
            else
            {
                char szModule[MAX_PATH] = "";
                DWORD section = 0;
                DWORD_PTR offset = 0;
                GetLogicalAddress(reinterpret_cast<PVOID>(static_cast<DWORD_PTR>(sf.AddrPC.Offset)), szModule, sizeof(szModule), section, offset);
                Log("%04X:%08X %s", section, offset, szModule);
            }

            IMAGEHLP_LINE64 lineInfo = {sizeof(IMAGEHLP_LINE64)};
            DWORD dwLineDisplacement;
            if (SymGetLineFromAddr64(m_hProcess, sf.AddrPC.Offset, &dwLineDisplacement, &lineInfo))
                Log("  %s line %u", lineInfo.FileName, lineInfo.LineNumber);
            Log("\r\n");

            if (bWriteVariables)
            {
                IMAGEHLP_STACK_FRAME imagehlpStackFrame;
                imagehlpStackFrame.InstructionOffset = sf.AddrPC.Offset;
                SymSetContext(m_hProcess, &imagehlpStackFrame, nullptr);
                EnumerateSymbolsCallbackContext ctx;
                ctx.sf = &sf;
                ctx.context = pContext;
                SymEnumSymbols(m_hProcess, 0, nullptr, EnumerateSymbolsCallback, &ctx);
                Log("\r\n");
            }
        }
    }

    // FUN_10a24ad0: every other thread of the process, no heading between them.
    void PrintThreadsCallstack(bool bWriteVariables)
    {
        const DWORD dwOwnerPID = GetCurrentProcessId();
        const DWORD dwCurrentTID = GetCurrentThreadId();
        m_hProcess = GetCurrentProcess();
        HANDLE hThreadSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (hThreadSnap == INVALID_HANDLE_VALUE)
            return;
        THREADENTRY32 te32;
        te32.dwSize = sizeof(THREADENTRY32);
        BOOL ok = Thread32First(hThreadSnap, &te32);
        while (ok)
        {
            if (te32.th32OwnerProcessID == dwOwnerPID && te32.th32ThreadID != dwCurrentTID)
            {
                CONTEXT context;
                context.ContextFlags = 0xffffffff;
                HANDLE threadHandle = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, false, te32.th32ThreadID);
                if (threadHandle)
                {
                    if (GetThreadContext(threadHandle, &context))
                        WriteStackDetails(&context, bWriteVariables, threadHandle);
                    CloseHandle(threadHandle);
                }
            }
            ok = Thread32Next(hThreadSnap, &te32);
        }
        CloseHandle(hThreadSnap);
    }

    // The C++ runtime's own records, as an uncaught throw (0xE06D7363) carries them.
    struct TypeDescriptor { const void* pVFTable; void* spare; char name[1]; };
    struct CatchableType { unsigned properties; TypeDescriptor* pType; };
    struct CatchableTypeArray { int nCatchableTypes; CatchableType* arrayOfCatchableTypes[1]; };
    struct ThrowInfo { unsigned attributes; void* pmfnUnwind; void* pForwardCompat; CatchableTypeArray* pCatchableTypeArray; };

    // The inner part of FUN_10a228e0 (its body, under the SEH frame below).
    void GenerateExceptionReportBody(PEXCEPTION_POINTERS pExceptionInfo)
    {
        SYSTEMTIME systime;
        GetLocalTime(&systime);
        Log("Date %u:%u:%u. Time %u:%u \r\n", systime.wDay, systime.wMonth, systime.wYear, systime.wHour, systime.wMinute);
        PEXCEPTION_RECORD pExceptionRecord = pExceptionInfo->ExceptionRecord;

        PrintSystemInfo();
        Log("\r\n//=====================================================\r\n");
        Log("Exception code: %08X %s\r\n", pExceptionRecord->ExceptionCode, GetExceptionString(pExceptionRecord->ExceptionCode));
        if (pExceptionRecord->ExceptionCode == 0xC0000420 && pExceptionRecord->NumberParameters >= 2)
        {
            pExceptionRecord->ExceptionAddress = reinterpret_cast<PVOID>(pExceptionRecord->ExceptionInformation[1]);
            Log("Assertion message: %s\r\n", reinterpret_cast<const char*>(pExceptionRecord->ExceptionInformation[0]));
        }

        char szFaultingModule[MAX_PATH];
        DWORD section = 0;
        DWORD_PTR offset = 0;
        GetLogicalAddress(pExceptionRecord->ExceptionAddress, szFaultingModule, sizeof(szFaultingModule), section, offset);
        Log("Fault address:  %08X %02X:%08X %s\r\n", pExceptionRecord->ExceptionAddress, section, offset, szFaultingModule);

        PCONTEXT pCtx = pExceptionInfo->ContextRecord;
        Log("\r\nRegisters:\r\n");
        Log("EAX:%08X\r\nEBX:%08X\r\nECX:%08X\r\nEDX:%08X\r\nESI:%08X\r\nEDI:%08X\r\n", pCtx->Eax, pCtx->Ebx, pCtx->Ecx, pCtx->Edx,
            pCtx->Esi, pCtx->Edi);
        Log("CS:EIP:%04X:%08X\r\n", pCtx->SegCs, pCtx->Eip);
        Log("SS:ESP:%04X:%08X  EBP:%08X\r\n", pCtx->SegSs, pCtx->Esp, pCtx->Ebp);
        Log("DS:%04X  ES:%04X  FS:%04X  GS:%04X\r\n", pCtx->SegDs, pCtx->SegEs, pCtx->SegFs, pCtx->SegGs);
        Log("Flags:%08X\r\n", pCtx->EFlags);

        SymSetOptions(SYMOPT_DEFERRED_LOADS);
        if (!SymInitialize(GetCurrentProcess(), nullptr, TRUE))
        {
            Log("\r\n");
            Log("----\r\n");
            Log("SYMBOL HANDLER ERROR (THIS IS NOT THE CRASH ERROR)\r\n\r\n");
            Log("Couldn't initialize symbol handler for process when generating crash report\r\n");
            Log("Error: %s\r\n", ErrorMessage(GetLastError()));
            Log("THE BELOW CALL STACKS MIGHT HAVE MISSING OR INACCURATE FILE/FUNCTION NAMES\r\n\r\n");
            Log("----\r\n");
        }

        if (pExceptionRecord->ExceptionCode == 0xE06D7363 && pExceptionRecord->NumberParameters >= 2)
        {
            PVOID exceptionObject = reinterpret_cast<PVOID>(pExceptionRecord->ExceptionInformation[1]);
            const ThrowInfo* throwInfo = reinterpret_cast<const ThrowInfo*>(pExceptionRecord->ExceptionInformation[2]);
            CatchableTypeArray* catchables = throwInfo->pCatchableTypeArray;
            TypeDescriptor* thrownType;
            if (catchables->nCatchableTypes && catchables->arrayOfCatchableTypes[0] &&
                (thrownType = catchables->arrayOfCatchableTypes[0]->pType) != nullptr)
            {
                // FUN_10a20a00 / FUN_10a20c60: typeid(std::exception), then __RTDynamicCast from the thrown type.
                TypeDescriptor* stdExceptionTypeInfo = reinterpret_cast<TypeDescriptor*>(const_cast<std::type_info*>(&typeid(std::exception)));
                std::exception* exceptionPtr = static_cast<std::exception*>(
                    __RTDynamicCast(exceptionObject, 0, thrownType, stdExceptionTypeInfo, FALSE));
                if (!exceptionPtr && thrownType == stdExceptionTypeInfo)
                    exceptionPtr = static_cast<std::exception*>(exceptionObject);
                Log("\r\nUncaught C++ exception info:");
                if (exceptionPtr)
                    Log(" %s", exceptionPtr->what());
                Log("\r\n");

                char undName[2000];
                memset(undName, 0, sizeof(undName));
                if (UnDecorateSymbolName(&thrownType->name[1], undName, 2000,
                                         UNDNAME_32_BIT_DECODE | UNDNAME_NAME_ONLY | UNDNAME_NO_ARGUMENTS))
                {
                    struct : SYMBOL_INFO
                    {
                        char NameBuffer[2000];
                    } sym;
                    memset(&sym, 0, sizeof(sym));
                    sym.SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym.MaxNameLen = 2000;
                    if (SymGetTypeFromName(m_hProcess, static_cast<ULONG64>(reinterpret_cast<intptr_t>(GetModuleHandleA(nullptr))), undName, &sym))
                    {
                        sym.Address = reinterpret_cast<DWORD_PTR>(exceptionObject);
                        sym.Flags = 0;
                        strcpy(sym.Name, "uncaught_exception");
                        FormatSymbolValue(&sym, nullptr);
                    }
                }
            }
        }

        CONTEXT trashableContext = *pCtx;
        WriteStackDetails(&trashableContext, false, nullptr);
        PrintThreadsCallstack(false);

        SymCleanup(GetCurrentProcess());
        Log("\r\n");
    }

    // FUN_10a228e0
    void GenerateExceptionReport(PEXCEPTION_POINTERS pExceptionInfo)
    {
        __try
        {
            GenerateExceptionReportBody(pExceptionInfo);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("Error writing the crash log\r\n");
        }
    }

    // FUN_10a239f0
    LONG WINAPI WheatyUnhandledExceptionFilter(PEXCEPTION_POINTERS pExceptionInfo)
    {
        std::unique_lock<std::mutex> guard(alreadyCrashedLock);
        if (alreadyCrashed)
            return EXCEPTION_EXECUTE_HANDLER;
        alreadyCrashed = true;

        char module_folder_name[MAX_PATH];
        GetModuleFileNameA(nullptr, module_folder_name, MAX_PATH);
        char* pos = strrchr(module_folder_name, '\\');
        if (!pos)
            return 0;
        pos[0] = '\0';
        ++pos;

        char crash_folder_path[MAX_PATH];
        sprintf_s(crash_folder_path, "%s\\%s", module_folder_name, "Crashes");
        if (!CreateDirectoryA(crash_folder_path, nullptr))
            if (GetLastError() != ERROR_ALREADY_EXISTS)
                return 0;

        SYSTEMTIME systime;
        GetLocalTime(&systime);
        sprintf(m_szDumpFileName, "%s\\%s_[%u-%u_%u-%u-%u].dmp", crash_folder_path, pos, systime.wDay, systime.wMonth,
                systime.wHour, systime.wMinute, systime.wSecond);
        sprintf(m_szLogFileName, "%s\\%s_[%u-%u_%u-%u-%u].txt", crash_folder_path, pos, systime.wDay, systime.wMonth,
                systime.wHour, systime.wMinute, systime.wSecond);

        // Compared with NULL, not INVALID_HANDLE_VALUE, as the original.
        m_hDumpFile = CreateFileA(m_szDumpFileName, GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_FLAG_WRITE_THROUGH, nullptr);
        if (m_hDumpFile)
        {
            MINIDUMP_EXCEPTION_INFORMATION info;
            info.ClientPointers = FALSE;
            info.ExceptionPointers = pExceptionInfo;
            info.ThreadId = GetCurrentThreadId();

            MINIDUMP_USER_STREAM additionalStream = {};
            MINIDUMP_USER_STREAM_INFORMATION additionalStreamInfo = {};
            if (pExceptionInfo->ExceptionRecord->ExceptionCode == 0xC0000420 && pExceptionInfo->ExceptionRecord->NumberParameters > 0)
            {
                additionalStream.Type = CommentStreamA;
                additionalStream.Buffer = reinterpret_cast<PVOID>(pExceptionInfo->ExceptionRecord->ExceptionInformation[0]);
                additionalStream.BufferSize = static_cast<ULONG>(strlen(reinterpret_cast<const char*>(pExceptionInfo->ExceptionRecord->ExceptionInformation[0])) + 1);
                additionalStreamInfo.UserStreamArray = &additionalStream;
                additionalStreamInfo.UserStreamCount = 1;
            }
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), m_hDumpFile, MiniDumpWithIndirectlyReferencedMemory, &info,
                              &additionalStreamInfo, nullptr);
            CloseHandle(m_hDumpFile);
        }

        m_hReportFile = fopen(m_szLogFileName, "wb");
        if (m_hReportFile)
        {
            GenerateExceptionReport(pExceptionInfo);
            fclose(m_hReportFile);
            m_hReportFile = nullptr;
        }

        if (m_previousFilter)
            return m_previousFilter(pExceptionInfo);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // FUN_10a239d0
    void __cdecl WheatyCrtHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
    {
        RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    }

    // 0x1007EB10 (constructor) and 0x10B18320 (the atexit teardown).
    struct WheatyExceptionReport
    {
        WheatyExceptionReport()
        {
            m_previousFilter = SetUnhandledExceptionFilter(WheatyUnhandledExceptionFilter);
            m_previousCrtHandler = _set_invalid_parameter_handler(WheatyCrtHandler);
            m_hProcess = GetCurrentProcess();
            alreadyCrashed = false;
            RtlGetVersion = reinterpret_cast<pRtlGetVersion>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion"));
            IsDebuggerPresent();   // the release build's leftover of a _CrtSetReportMode block
        }
        ~WheatyExceptionReport()
        {
            if (m_previousFilter)
                SetUnhandledExceptionFilter(m_previousFilter);
            if (m_previousCrtHandler)
                _set_invalid_parameter_handler(m_previousCrtHandler);
            ClearSymbols();
        }
    } g_WheatyExceptionReport;
}
