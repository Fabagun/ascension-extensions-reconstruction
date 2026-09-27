#include <Ascension/AscQuestAddon.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CDataStore.hpp>
#include <Client/CNetClient.hpp>
#include <Misc/DataContainer.hpp>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace AscScript;

namespace
{
    struct Entry
    {
        AscQuestAddon::Record record{};
        std::vector<std::pair<AscQuestAddon::Callback, void*>> waiting;   // +0x1C
        bool loaded = false;                                              // +0x24
    };
    std::unordered_map<uint32_t, Entry> g_entries;

    void __cdecl OnQuestAddon(void*, uint32_t, uint32_t, CDataStore* p)   // 0x732
    {
        uint32_t v[3];
        memcpy(v, p->m_buffer + p->m_read, sizeof(v));
        p->m_read += sizeof(v);
        auto it = g_entries.find(v[0]);
        if (it == g_entries.end())
            return;
        Entry& e = it->second;
        e.record = {v[0], v[1], v[2], v[0]};
        e.loaded = true;
        const auto waiting = std::move(e.waiting);
        e.waiting.clear();
        for (const auto& w : waiting)
            w.first(v[0], w.second);
    }

    void Init()
    {
        sDC.AddPacketHandler(0x732, CNetClientCustomPacket((void*)&OnQuestAddon, nullptr));
    }

    AscBindings::Module s_module(nullptr, 0, Init);
}

namespace AscQuestAddon
{
    const Record* Get(uint32_t quest, Callback cb, void* arg)
    {
        if (!quest)
            return nullptr;
        auto it = g_entries.find(quest);
        if (it == g_entries.end())
        {
            Entry& e = g_entries[quest];
            if (cb)
                e.waiting.emplace_back(cb, arg);
            Packet(0x731).U32(quest).Send();
            return nullptr;
        }
        if (it->second.loaded)
            return &it->second.record;
        if (cb)
            it->second.waiting.emplace_back(cb, arg);
        return nullptr;
    }
}

void AscQuestAddon::ClearStore() { g_entries.clear(); }

namespace
{
    uint32_t g_version = 0;   // 0x10BDF3CC
}
uint32_t& AscQuestAddon::Version() { return g_version; }
size_t AscQuestAddon::Count() { return g_entries.size(); }
void AscQuestAddon::ForEachRecord(void (*cb)(const Record&, void*), void* ctx)
{
    for (auto& kv : g_entries)
        cb(kv.second.record, ctx);
}
void AscQuestAddon::PruneUnloaded()
{
    for (auto it = g_entries.begin(); it != g_entries.end();)
        it = it->second.loaded ? std::next(it) : g_entries.erase(it);
}
