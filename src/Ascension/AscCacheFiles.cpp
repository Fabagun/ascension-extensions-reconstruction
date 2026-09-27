// The WDB files of the DLL's two server caches (static stores 0x10BDF360 itemstatcache.wdb, 0x10BDF3A8
// questcacheaddon.wdb; both built enabled).
//
//   Directory (FUN_101b0690): "Cache\WDB\<the string at 0xC5DE88>\<the string of the CVar *0xC79D08>",
//     created through the client's 0x461D10(path, 1).
//   Load (FUN_101b00c0, item stats only -- the quest-addon file is never read back): "<dir>/<name>" opened
//     through 0x461FA0 (GENERIC_READ, share 1, OPEN_EXISTING, 0x80), the whole file read into a heap
//     buffer the original never frees; u32 count, u32 version (the store key), then count 0xA8-byte
//     records, each inserted as a loaded entry. Runs once, on the first 0x635710 (AscAttachLua), before
//     the key check that empties a store whose key changed.
//   Prune (FUN_101afbb0, before 0x528F00 -- world leave): entries still waiting for the server dropped.
//   Save (FUN_101afb90 -> FUN_101afd10, after 0x402910 -- the client teardown): the 0x700 handler and the
//     quest-addon one (0x732) cleared; then for each store, when it holds anything, "<dir>/<name>" written
//     through 0x461FA0 (GENERIC_WRITE, share 1, CREATE_ALWAYS, 0x80) / 0x461B90: u32 count, u32 version,
//     the records (item stats: 0xA8 bytes each; quest addon: quest, points, unk); then the store emptied.
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscItemStats.hpp>
#include <Ascension/AscLog.hpp>
#include <Ascension/AscQuestAddon.hpp>
#include <Ascension/AscRuntime.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

void AscCacheFiles_LoadItemStats();   // called by AscAttachLua's 0x635710 hook

namespace
{
    typedef int(__cdecl* Open_t)(const char*, uint32_t, uint32_t, uint32_t, uint32_t);
    const Open_t Open = reinterpret_cast<Open_t>(0x461FA0);
    void Close(int h) { reinterpret_cast<void(__cdecl*)(int)>(0x461B00)(h); }

    void Directory(char (&out)[0x104])   // FUN_101b0690
    {
        const uint8_t* cvar = *reinterpret_cast<const uint8_t* const*>(0xC79D08);
        const char* sub = cvar ? *reinterpret_cast<const char* const*>(cvar + 0x28) : "";
        char path[0x104];
        sprintf_s(path, "Cache\\%s\\%s\\%s", "WDB", reinterpret_cast<const char*>(0xC5DE88), sub ? sub : "");
        strncpy_s(out, path, _TRUNCATE);
        reinterpret_cast<int(__cdecl*)(const char*, int)>(0x461D10)(out, 1);
    }

    void PathOf(const char* name, char (&out)[0x104])
    {
        char dir[0x104];
        Directory(dir);
        sprintf_s(out, "%s/%s", dir, name);
    }

    void Write(const char* name, const std::vector<uint8_t>& data)
    {
        char path[0x104];
        PathOf(name, path);
        const int h = Open(path, 0x40000000, 1, 2, 0x80);
        if (h == -1)
            return;
        uint32_t written = 0;
        reinterpret_cast<int(__cdecl*)(int, const void*, uint32_t, uint32_t*)>(0x461B90)(h, data.data(), static_cast<uint32_t>(data.size()), &written);
        Close(h);
    }

    void Put32(std::vector<uint8_t>& v, uint32_t x)
    {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&x);
        v.insert(v.end(), b, b + 4);
    }

    void Prune()   // FUN_101afbb0
    {
        AscItemStats::PruneUnloaded();
        AscQuestAddon::PruneUnloaded();
    }

    void Save()   // FUN_101afb90 -> FUN_101afd10
    {
        typedef void(__cdecl* Clear_t)(uint32_t);
        reinterpret_cast<Clear_t>(0x6B0BC0)(0x700);
        reinterpret_cast<Clear_t>(0x6B0BC0)(0x732);
        if (const size_t n = AscItemStats::Count())
        {
            std::vector<uint8_t> data;
            Put32(data, static_cast<uint32_t>(n));
            Put32(data, AscItemStats::Version());
            AscItemStats::ForEachRecord([](const uint8_t* r, void* ctx) {
                auto& v = *static_cast<std::vector<uint8_t>*>(ctx);
                v.insert(v.end(), r, r + 0xA8);
            }, &data);
            Write("itemstatcache.wdb", data);
        }
        AscItemStats::ClearStore();
        if (const size_t n = AscQuestAddon::Count())
        {
            std::vector<uint8_t> data;
            Put32(data, static_cast<uint32_t>(n));
            Put32(data, AscQuestAddon::Version());
            AscQuestAddon::ForEachRecord([](const AscQuestAddon::Record& r, void* ctx) {
                auto& v = *static_cast<std::vector<uint8_t>*>(ctx);
                Put32(v, r.quest);
                Put32(v, r.points);
                Put32(v, r.unk);
            }, &data);
            Write("questcacheaddon.wdb", data);
        }
        AscQuestAddon::ClearStore();
    }

    void Init()
    {
        AscRuntime::OnBefore528F00(&Prune);
        AscRuntime::OnAfter402910(&Save);
    }

    AscBindings::Module s_module(nullptr, 0, &Init);
}

void AscCacheFiles_LoadItemStats()   // FUN_101b00c0
{
    char path[0x104];
    PathOf("itemstatcache.wdb", path);
    const int h = Open(path, 0x80000000, 1, 3, 0x80);
    if (h == -1)
        return;
    const uint32_t size = reinterpret_cast<uint32_t(__cdecl*)(int)>(0x461BD0)(h);
    uint8_t* buffer = new uint8_t[size];   // never freed, as in the original
    uint32_t read = 0;
    reinterpret_cast<int(__cdecl*)(int, void*, uint32_t, uint32_t*)>(0x461B50)(h, buffer, size, &read);
    if (read >= 8)
    {
        uint32_t count, version;
        memcpy(&count, buffer, 4);
        memcpy(&version, buffer + 4, 4);
        AscItemStats::Version() = version;
        // The original reads count records without checking the file is long enough; ours stops at its end.
        for (uint32_t i = 0; i < count && 8 + (i + 1) * 0xA8ull <= read; ++i)
            AscItemStats::InsertLoaded(buffer + 8 + i * 0xA8);
    }
    Close(h);
}
