import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    index = opening + 1
    while depth:
        depth += (source[index] == '{') - (source[index] == '}')
        index += 1
    return source[start:index]


HARNESS = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#define __cdecl

struct CDataStore
{
    std::vector<uint8_t> m_buffer;
    uint32_t m_read = 0;
};

template<class T> T Read(CDataStore* p)
{
    T value;
    memcpy(&value, p->m_buffer.data() + p->m_read, sizeof(T));
    p->m_read += sizeof(T);
    return value;
}

template<class T> void Append(CDataStore& packet, T value)
{
    auto bytes = reinterpret_cast<uint8_t const*>(&value);
    packet.m_buffer.insert(packet.m_buffer.end(), bytes, bytes + sizeof(T));
}

struct Entry
{
    uint32_t id = 0, rank = 0, u0c = 0, u18 = 0, u1c = 0;
    uint8_t locked = 0;
};

struct Build
{
    uint64_t guid = 0;
    int32_t spec = -1;
    uint32_t classKey = 0, level = 0, u2c = 0, u64 = 0, u65 = 0;
    bool draft = false, wildcard = false, specDraft = false, qualities = false;
    bool hero = false, coa = false, stock = false, pendingHooks = false;
    std::string name;
    std::vector<Entry> entries;
    uint32_t preparedClass = 0;
    Build() = default;
    Build(Build const& other, bool) : Build(other) { }
    void Prepare() { preparedClass = classKey; }
    void UpdatePointers(bool) { }
    void Reorder(bool) { }
    bool Has(uint32_t id) const
    {
        return std::any_of(entries.begin(), entries.end(), [id](auto const& e) { return e.id == id; });
    }
    uint32_t RankOf(uint32_t id) const
    {
        for (auto const& e : entries)
            if (e.id == id)
                return e.rank;
        return 0;
    }
    bool LockedOf(uint32_t id) const
    {
        for (auto const& e : entries)
            if (e.id == id)
                return e.locked != 0;
        return false;
    }
};

struct Player
{
    uint32_t spec = 0, count = 0;
    std::map<uint32_t, Build> builds;
    Build* Get(uint32_t slot)
    {
        auto found = builds.find(slot);
        return found == builds.end() ? nullptr : &found->second;
    }
    void Ensure(uint32_t slot) { builds.try_emplace(slot); }
};

struct Manager
{
    Player* player = nullptr;
    Build* pending = nullptr;
} g_mgr;

std::array<uint8_t, 0x100> descriptor{};
std::array<uint8_t, 0x20> unit{};
bool playerAvailable = true;
uint8_t* ActivePlayer() { return playerAvailable ? unit.data() : nullptr; }
uint64_t ActivePlayerGuid() { return 123; }
uint8_t UnitClass(uint8_t const* object)
{
    auto desc = *reinterpret_cast<uint8_t* const*>(object + 8);
    return static_cast<uint8_t>(*reinterpret_cast<uint32_t const*>(desc + 0x5C) >> 8);
}
bool StockClass(uint8_t value) { return (value >= 1 && value <= 9) || value == 11; }
int ActiveSpecIndex() { return static_cast<int>(g_mgr.player->spec); }
Build defaultBuild;
Build* ActiveBuild()
{
    if (g_mgr.player)
        if (auto build = g_mgr.player->Get(g_mgr.player->spec))
            return build;
    return &defaultBuild;
}
uint64_t g_pendingVersion = 0;
void EnsurePlayer()
{
    if (!g_mgr.player)
        g_mgr.player = new Player;
}
void RefreshCredits() { }
std::vector<int> g_onSpecChanged, g_onLearned, g_onRankChanged, g_onUnlearned;
void Fire(std::vector<int> const&, uint32_t, uint32_t) { }
namespace AscGameMode
{
uint32_t Mode() { return 0; }
bool SpecBuildDraft(int32_t) { return false; }
}
namespace AscConfig
{
bool const* Bool(char const*) { return nullptr; }
}
namespace AscObjectAddon
{
struct Slot { uint32_t value = 0; };
std::array<Slot, 3>& Unit(uint64_t)
{
    static std::array<Slot, 3> slots;
    return slots;
}
}
std::vector<std::pair<uint32_t, uint32_t>> eventParameters;
namespace AscRuntime
{
template<class... Args> void Signal(char const* event, Args...)
{
    if (std::string(event) == "ASCENSION_KNOWN_ENTRIES_UPDATED")
        eventParameters.emplace_back(ActiveBuild()->classKey, ActiveBuild()->level);
}
}

@PARAMS@
@SYNC@
@RESET@
@ACTIVE@
@KNOWN@

int failures = 0;
void Check(bool condition, char const* name)
{
    failures += !condition;
    std::printf("%s: %s\n", condition ? "PASS" : "FAIL", name);
}
void SetUnit(uint32_t cls, uint32_t level)
{
    auto desc = descriptor.data();
    memcpy(unit.data() + 8, &desc, sizeof(desc));
    uint64_t guid = ActivePlayerGuid();
    memcpy(desc, &guid, sizeof(guid));
    uint32_t bytes = cls << 8;
    memcpy(desc + 0x5C, &bytes, sizeof(bytes));
    memcpy(desc + 0xD8, &level, sizeof(level));
}
CDataStore Entries(uint32_t id, uint32_t rank)
{
    CDataStore packet;
    Append<uint32_t>(packet, 1);
    Append(packet, id);
    Append(packet, rank);
    Append<uint32_t>(packet, 0);
    Append<uint8_t>(packet, 0);
    Append<uint32_t>(packet, 0);
    Append<uint32_t>(packet, 0);
    return packet;
}
int main()
{
    SetUnit(20, 80);
    CDataStore slot;
    Append<uint32_t>(slot, 0);
    Append<uint32_t>(slot, 1);
    OnActiveSpec(nullptr, 0x725, 0, &slot);
    auto known = Entries(4025, 1);
    OnKnownEntries(nullptr, 0x726, 0, &known);
    Check(known.m_read == 25, "native known-entry packet consumes its unchanged 21-byte record");
    Check(ActiveBuild()->classKey == 20 && ActiveBuild()->level == 80 && ActiveBuild()->coa,
          "first native known-entries packet initializes the Bloodmage build immediately");
    Check(ActiveBuild()->preparedClass == 20, "rules are prepared with the actual class");
    Check(g_mgr.pending->classKey == 20 && g_mgr.pending->level == 80 && g_mgr.pending->Has(4025),
          "pending build receives the same parameters and Sanguine identity");
    Check(eventParameters.size() == 1 && eventParameters[0] == std::make_pair(20u, 80u),
          "known-entry listeners see initialized class and level");
    SetUnit(20, 81);
    known = Entries(4025, 2);
    OnKnownEntries(nullptr, 0x726, 0, &known);
    Check(g_mgr.pending->level == 81 && g_mgr.pending->RankOf(4025) == 2,
          "subsequent native packets update level before resetting pending ranks");
    g_mgr.player->spec = 1;
    known = Entries(9905, 1);
    OnKnownEntries(nullptr, 0x726, 0, &known);
    Check(ActiveBuild()->spec == 1 && g_mgr.pending->classKey == 20 && g_mgr.pending->Has(9905),
          "a newly established preset uses its native slot and current class");
    playerAvailable = false;
    known = Entries(9905, 2);
    OnKnownEntries(nullptr, 0x726, 0, &known);
    Check(g_mgr.pending->RankOf(9905) == 2, "a missing local unit leaves packet handling safe");
    delete g_mgr.pending;
    delete g_mgr.player;
    return failures ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description='Compile and exercise the native CA packet handlers with client doubles.')
    parser.add_argument('--source-ref')
    args = parser.parse_args()
    name = 'src/Ascension/AscCAMgr.cpp'
    if args.source_ref:
        source = subprocess.check_output(['git', 'show', f'{args.source_ref}:{name}'], cwd=ROOT, text=True)
    else:
        source = (ROOT / name).read_text(encoding='utf-8')
    harness = HARNESS
    for marker, signature in [
        ('PARAMS', 'void ParamsFromUnit(Build& out'),
        ('SYNC', 'bool SyncParams(Build& b'),
        ('RESET', 'void ResetPendingBuild()'),
        ('ACTIVE', 'void __cdecl OnActiveSpec('),
        ('KNOWN', 'void __cdecl OnKnownEntries('),
    ]:
        definitions = source.replace('void ParamsFromUnit(Build& out, const uint8_t* unit, int32_t spec);', '')
        definitions = definitions.replace('bool SyncParams(Build& b, const Build& p);', '')
        harness = harness.replace(f'@{marker}@', function(definitions, signature))
    compiler = shutil.which(os.environ.get('CXX', 'cl.exe' if os.name == 'nt' else 'c++'))
    if not compiler:
        raise SystemExit('A C++20 compiler is required.')
    with tempfile.TemporaryDirectory(prefix='native-ca-packets-') as directory:
        out = Path(directory)
        cpp = out / 'packets.cpp'
        cpp.write_text(harness, encoding='utf-8')
        executable = out / ('packets.exe' if os.name == 'nt' else 'packets')
        if Path(compiler).stem.lower() == 'cl':
            flags = ['/nologo', '/std:c++20', '/EHsc', str(cpp), '/Fe' + str(executable)]
        else:
            flags = ['-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                     str(cpp), '-o', str(executable)]
        subprocess.run([compiler, *flags], cwd=out, check=True)
        return subprocess.run([str(executable)], cwd=out).returncode


if __name__ == '__main__':
    raise SystemExit(main())
