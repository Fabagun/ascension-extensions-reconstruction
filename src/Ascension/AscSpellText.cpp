// Spell text the original DLL builds from Spell.dbc, transcribed from the decompile.
//
//   Description  FUN_101ac910 -> FUN_101ac3d0: the client's formatter 0x57ABC0 fills a 0x400-byte buffer
//                (DAT_10bdef30) from the record, then FUN_101a8e30 rewrites the @-markup. Two caches, both
//                appended on every miss and read only with `cached`: the formatter output (DAT_10bdef24,
//                {spell, char[0x400], time}, dropped after 300 s) and the markup result (DAT_10bdef18,
//                {spell, string, time}, dropped after 20 s); the tick FUN_101ac9b0 runs after 0x403340 and
//                the glue reset FUN_101a85c0 empties both.
//   Markup       FUN_101a8e30, below.
//   C_Format.Format (FUN_101acb30) and the global GetSpellDescription (FUN_10a4dd90) sit on top.
//   Name         DAT_10bdf458: spell -> Spell +0x220 ("" when the spell is missing).
//   FormText     FUN_101c6300 / DAT_10bdf498: spell -> FUN_101c1b60.
#include <Ascension/AscSpellText.hpp>
#include <Ascension/AscBindings.hpp>
#include <Ascension/AscCAMgr.hpp>
#include <Ascension/AscClientOptions.hpp>
#include <Ascension/AscGameMode.hpp>
#include <Ascension/AscMysticEnchant.hpp>
#include <Ascension/AscRuntime.hpp>
#include <Ascension/AscScript.hpp>
#include <Client/CVar.hpp>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <vector>
#include <cstring>
#include <unordered_map>

using namespace AscScript;

namespace AscSpellText
{
namespace
{
    const uint32_t kShapeshiftForms = 0xAD49F4;   // SpellShapeshiftForm.dbc (FUN_100b25f0; min FUN_100b1100, max FUN_100b1040)

    const char* RecStr(const uint8_t* rec, uint32_t off) { return *reinterpret_cast<const char* const*>(rec + off); }
    uint32_t RecU32(const uint8_t* rec, uint32_t off) { return *reinterpret_cast<const uint32_t*>(rec + off); }

    // FUN_101c6160 (map DAT_10bdf478): the form's name, SpellShapeshiftForm +8.
    const std::string& FormName(uint32_t form)
    {
        static std::unordered_map<uint32_t, std::string> names;
        auto it = names.find(form);
        if (it != names.end())
            return it->second;
        std::string name;
        if (const uint8_t* row = ClientDbcRow(kShapeshiftForms, form))
            if (const char* s = RecStr(row, 8))
                name = s;
        return names.emplace(form, name).first->second;
    }

    // FUN_101c1b60: nothing when Spell +0x18 has bit 19 or the stance mask (+0x30/+0x34) is empty;
    // otherwise " <form> Requires <form>" for every form in the mask, in id order.
    std::string FormsOf(const uint8_t* rec)
    {
        std::string out;
        if ((RecU32(rec, 0x18) >> 19) & 1 || (RecU32(rec, 0x30) == 0 && RecU32(rec, 0x34) == 0))
            return out;
        const uint32_t lo = *reinterpret_cast<const uint32_t*>(kShapeshiftForms + 0x10);
        for (uint32_t form = lo; form <= *reinterpret_cast<const uint32_t*>(kShapeshiftForms + 0x0C); ++form)
        {
            if (form == 0 || (form - 1) >> 5 >= 2)
                continue;
            if (!(RecU32(rec, 0x30 + ((form - 1) >> 5) * 4) & (1u << ((form - 1) & 0x1F))))
                continue;
            const std::string& name = FormName(form);
            if (name.empty())
                continue;
            out += " ";
            out += name;
            out += " Requires ";
            out += name;
        }
        return out;
    }

    typedef void(__cdecl* FormatSpellFn)(const uint8_t* rec, char* out, uint32_t size, uint32_t, uint32_t, uint32_t,
                                         uint32_t, uint32_t, uint32_t);
    const FormatSpellFn FormatSpell = reinterpret_cast<FormatSpellFn>(0x57ABC0);
}

const std::string& Name(uint32_t spellId)
{
    static std::unordered_map<uint32_t, std::string> names;
    auto it = names.find(spellId);
    if (it != names.end())
        return it->second;
    uint8_t rec[0x2A8];
    std::string name;
    if (FetchSpell(spellId, rec) && RecStr(rec, 0x220))
        name = RecStr(rec, 0x220);
    return names.emplace(spellId, name).first->second;
}

const std::string& FormText(uint32_t spellId)
{
    static std::unordered_map<uint32_t, std::string> forms;
    auto it = forms.find(spellId);
    if (it != forms.end())
        return it->second;
    uint8_t rec[0x2A8];
    std::string text;
    if (FetchSpell(spellId, rec))
        text = FormsOf(rec);
    return forms.emplace(spellId, text).first->second;
}

}

namespace
{
    using AscSpellText::RecStr;
    using AscSpellText::RecU32;
    using AscSpellText::FormatSpell;

    const char* const kQuestionMark = "Interface\\Icons\\INV_Misc_QuestionMark";
    const uint32_t kSpellIcons = 0xAD488C;     // SpellIcon.dbc (FUN_100b2470)
    const uint32_t kSpellRanges = 0xAD4988;    // SpellRange.dbc (FUN_100b2590)
    const uint32_t kSpellCastTimes = 0xAD4748; // SpellCastTimes.dbc (FUN_100b22c0)

    int g_depth = 0;   // the thread-local recursion counter (TLS +8)

    struct FormattedEntry { uint32_t spell; char text[0x400]; __time64_t time; };   // DAT_10bdef24
    struct MarkupEntry { uint32_t spell; std::string text; __time64_t time; };      // DAT_10bdef18
    std::vector<FormattedEntry> g_formatted;
    std::vector<MarkupEntry> g_markup;

    // The two cvars FUN_101ac910 registers (DAT_10bdef10 / DAT_10bdef14); the markup tests their integer
    // value (+0x30) == 1.
    bool CVarOn(const char* name)
    {
        CVar* v = CVar::Lookup(name);
        return v && v->m_padding[12] == 1;
    }

    bool KeyDown(uint32_t key) { return (reinterpret_cast<uint32_t(__cdecl*)(uint32_t)>(0x47D230)(key) & 0xFF) != 0; }

    // Riding (skill line 762): 0x6DC1C0 finds the skill slot, 0x51A250 reads its value (both __thiscall).
    int32_t RidingSkill(uint8_t* player)
    {
        typedef int(__thiscall * SkillSlot_t)(void*, uint32_t);
        typedef int32_t(__thiscall * SkillValue_t)(void*, int);
        return reinterpret_cast<SkillValue_t>(0x51A250)(player, reinterpret_cast<SkillSlot_t>(0x6DC1C0)(player, 0x2FA));
    }

    std::string U(uint32_t v) { return std::to_string(v); }
    // std::format's "{}" for a float: the shortest round-trip form.
    std::string F(float v)
    {
        char buf[64];
        const auto r = std::to_chars(buf, buf + sizeof(buf), v);
        return std::string(buf, r.ptr);
    }
    // FUN_101ae730: "|cff{color}{text}|r".
    std::string Colored(const char* color, const std::string& text) { return std::string("|cff") + color + text + "|r"; }
    std::string IconTag(const std::string& icon, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
    {
        return "|T" + icon + ":" + U(a) + ":" + U(b) + ":" + U(c) + ":" + U(d) + "|t";
    }

    // FUN_101acf80: the icon, unless it names no file the client can open (".blp" is appended for the test
    // when neither ".blp" nor ".BLP" appears) -- then the question mark.
    std::string ValidIcon(const std::string& icon)
    {
        if (icon.empty() || icon == kQuestionMark)
            return icon;
        std::string path = icon;
        if ((path.size() < 4 || path.find(".blp") == std::string::npos) &&
            (path.size() < 4 || path.find(".BLP") == std::string::npos))
            path += ".blp";
        typedef int(__stdcall * SFileOpen_t)(void*, const char*, uint32_t, void**);
        typedef int(__stdcall * SFileClose_t)(void*);
        void* h = nullptr;
        if (!reinterpret_cast<SFileOpen_t>(0x424B50)(nullptr, path.c_str(), 0x20000, &h))
            return kQuestionMark;
        reinterpret_cast<SFileClose_t>(0x422910)(h);
        return icon;
    }
    std::string SpellIconPath(const uint8_t* rec)
    {
        std::string icon = kQuestionMark;
        if (const uint8_t* row = ClientDbcRow(kSpellIcons, RecU32(rec, 0x214)))
            if (const char* p = RecStr(row, 4))
                icon = p;
        return ValidIcon(icon);
    }

    // The label and icon every spell-naming @tag@ builds: "Unknown Spell (<id>)" or the spell's name
    // (plus " (<rank>)" when asked and present), and its icon at 20x20.
    void SpellLabel(uint32_t id, bool withRank, std::string& label, std::string& icon)
    {
        label = "Unknown Spell (" + U(id) + ")";
        std::string rank;
        icon = kQuestionMark;
        uint8_t rec[0x2A8];
        if (FetchSpell(id, rec))
        {
            label = RecStr(rec, 0x220) ? RecStr(rec, 0x220) : "";
            if (withRank && RecStr(rec, 0x224))
                rank = RecStr(rec, 0x224);
            if (const uint8_t* row = ClientDbcRow(kSpellIcons, RecU32(rec, 0x214)))
                if (const char* p = RecStr(row, 4))
                    icon = p;
        }
        icon = IconTag(ValidIcon(icon), 20, 20, 0, 0);
        if (withRank && !rank.empty())
            label = label + " (" + rank + ")";
    }

    // ---- the tooltip lines FUN_101adc90 can add ----------------------------------------------------
    std::string RangeLine(const uint8_t* rec)   // FUN_101a8d10
    {
        const uint8_t* row = ClientDbcRow(kSpellRanges, RecU32(rec, 0xB8));
        if (!row)
            return "|cffFF0000UNKNOWN yd range|r";
        return "|cffFFFFFF" + F(*reinterpret_cast<const float*>(row + 0xC)) + " yd range|r";
    }
    std::string CooldownLine(const uint8_t* rec)   // FUN_101a8720
    {
        const uint32_t v = std::max(RecU32(rec, 0x74), RecU32(rec, 0x78));
        const float f = v < 60000 ? static_cast<float>(static_cast<double>(v)) / 1000.0f
                                  : static_cast<float>(static_cast<double>(v)) / 60000.0f;
        return "|cffFFFFFF" + F(f) + " " + (v < 60000 ? "sec" : "min") + " cooldown|r";
    }
    std::string CastLine(const uint8_t* rec)   // FUN_101a8600
    {
        const uint8_t* row = ClientDbcRow(kSpellCastTimes, RecU32(rec, 0x70));
        if (!row)
            return "|cffFF0000UNKNOWN sec cast|r";
        return "|cffFFFFFF" + F(static_cast<float>(*reinterpret_cast<const int32_t*>(row + 4)) / 1000.0f) + " sec cast|r";
    }
    std::string PowerLine(const uint8_t* rec)   // FUN_101a88a0
    {
        static const std::unordered_map<uint32_t, uint32_t> kInsanity = {{0xC48FC, 0x1E}};   // DAT_10bdf330
        uint32_t cost = RecU32(rec, 0xA8);
        if (cost == 0)
        {
            auto it = kInsanity.find(RecU32(rec, 0));
            if (it != kInsanity.end())
                return "|cffa54cffDrains " + U(it->second) + " Insanity|r";
        }
        if (const uint32_t pct = RecU32(rec, 0x230))   // a percentage of UNIT_FIELD_BASE_MANA
        {
            const uint8_t* player = ActivePlayer();
            cost = player ? (*reinterpret_cast<const uint32_t*>(*reinterpret_cast<uint8_t* const*>(player + 8) + 0x1E0) * pct) / 100 : 0;
        }
        std::string name = "UNKNOWN POWER";
        switch (static_cast<int32_t>(RecU32(rec, 0xA4)))
        {
        case 0: name = "Mana"; break;
        case 1:
            name = "Rage";
            cost = static_cast<uint32_t>(static_cast<float>(static_cast<double>(cost)) * 0.1f);
            break;
        case 2: name = "Focus"; break;
        case 3: name = "Energy"; break;
        case 4: name = "Happiness"; break;
        case 5: name = "Rune"; break;
        case 6: name = "Runic Power"; break;
        case -2: name = "Health"; break;
        default: break;
        }
        return "|cffFFFFFF" + U(cost) + " " + name + "|r";
    }

    // FUN_101adc90 (@s:) / FUN_101ad1f0 (@re:): byte-identical apart from the error text.
    std::string Render(const char* kind, uint32_t id, bool nameOnly, bool descOnly, bool cached, bool divider,
                       bool range, bool cooldown, bool cast, bool power)
    {
        std::string out;
        uint8_t rec[0x2A8];
        if (!FetchSpell(id, rec))
            out = std::string("|cffFF0000<UNKNOWN ") + kind + ": " + U(id) + ". ENSURE YOUR GAME IS UP TO DATE>|r";
        else
        {
            const std::string head = IconTag(SpellIconPath(rec), 20, 20, 0, 0) + " |cffFFFFFF" +
                                     (RecStr(rec, 0x220) ? RecStr(rec, 0x220) : "") + "|r";
            if (nameOnly)
                out = head;
            else
            {
                const std::string desc = AscSpellText::Description(id, false, cached, 0);
                if (descOnly)
                    out = desc;
                else
                {
                    out = head;
                    if (range)
                        out = out + "\n" + RangeLine(rec);
                    if (cooldown)
                        out = out + "\n" + CooldownLine(rec);
                    if (power)
                        out = out + "\n" + PowerLine(rec);
                    if (cast)
                        out = out + "\n" + CastLine(rec);
                    out = out + "\n" + desc;
                }
            }
        }
        if (divider)
            out = IconTag("Interface\\Common\\ui-tooltipdivider", 12, 180, 0, 0) + "\n" + out;
        return out;
    }

    // ---- token scanning -------------------------------------------------------------------------------
    // FUN_101acd90: does the tag at the next '@' (from pos) open with `open`? On a match start = the opener,
    // end = the first `close` after it, true. Otherwise false, and pos moves to that '@' -- or to the end
    // when no further '@' follows.
    bool FindTag(const std::string& t, size_t& pos, size_t& start, size_t& end, const char* open, const char* close)
    {
        const size_t len = t.size();
        const size_t at = pos < len ? t.find('@', pos) : std::string::npos;
        if (at == std::string::npos)
        {
            pos = len;
            return false;
        }
        const size_t ol = strlen(open), cl = strlen(close);
        const size_t o = len - at >= ol ? t.find(open, at) : std::string::npos;
        start = o == std::string::npos ? len : o;
        if (o == at)
        {
            const size_t from = o + ol;
            const size_t c = len - from >= cl ? t.find(close, from) : std::string::npos;
            end = c == std::string::npos ? len : c;
            if (c != std::string::npos)
                return true;
            if ((len > from ? t.find('@', from) : std::string::npos) == std::string::npos)
                pos = len;
            return false;
        }
        if ((len > at + 1 ? t.find('@', at + 1) : std::string::npos) != std::string::npos)
            pos = at;
        else
            pos = len;
        return false;
    }

    // FUN_101a8240 (@ext:) / FUN_101a8410 (@sim:): the body split at its first top-level '~' (nested tags of
    // the same kind are stepped over). @ext: without a '~' is all first part; @sim: then does not match.
    bool SplitTag(const std::string& t, size_t& pos, size_t& s, size_t& e, const char* open, const char* close,
                  bool needTilde, std::string& first, std::string& second)
    {
        size_t os = 0, oe = 0;
        if (!FindTag(t, pos, os, oe, open, close))
            return false;
        const size_t n = oe - (os + 5);
        size_t tilde = std::string::npos;
        int nest = 0;
        for (size_t i = 0; i < n;)
        {
            const size_t avail = std::min<size_t>(5, n - i);
            const char* p = t.data() + os + 5 + i;
            if (avail == 5 && memcmp(p, open, 5) == 0)
            {
                i += 5;
                ++nest;
            }
            else if (avail == 5 && memcmp(p, close, 5) == 0)
            {
                i += 5;
                if (nest >= 1)
                    --nest;
            }
            else
            {
                if (*p == '~' && nest == 0 && tilde == std::string::npos)
                    tilde = i;
                ++i;
            }
        }
        if (tilde == std::string::npos)
        {
            if (needTilde)
                return false;
            first = t.substr(os + 5, n);
            second.clear();
        }
        else
        {
            first = t.substr(os + 5, std::min(tilde, n));
            second = t.substr(os + 5 + tilde + 1, n - (tilde + 1));
        }
        s = os;
        e = oe + 5;
        pos = s;
        return true;
    }

    // FUN_101a7e90 / FUN_101a7f00: the tag read with sscanf from its opener; on success [s, e) covers it
    // through the close delimiter and pos = s.
    template <class... A>
    bool ScanTag(const std::string& t, size_t& pos, size_t& s, size_t& e, const char* open, const char* close,
                 const char* fmt, int want, A*... out)
    {
        size_t ts = 0, te = 0;
        if (!FindTag(t, pos, ts, te, open, close))
            return false;
        if (sscanf(t.c_str() + ts, fmt, out...) != want)
            return false;
        s = ts;
        e = te + strlen(close);
        pos = s;
        return true;
    }
}

namespace AscSpellText
{
bool Markup(std::string& t, bool a, bool cached, int stripQuotes, uint32_t spell, std::vector<std::string>* collected,
            bool collect, bool noRequirements)
{
    bool changed = false;
    if (++g_depth < 11)
    {
        size_t pos = 0, s = 0, e = 0;
        const bool shift = a || KeyDown(0) || CVarOn("extendedTooltips");
        bool hint = false;
        auto eraseLines = [&t](size_t at, size_t end) {
            t.erase(at, end - at);
            while (at < t.size() && (t[at] == '\n' || t[at] == '\r'))
                t.erase(at, 1);
        };
        auto put = [&](const std::string& text) {   // collect mode moves the text out instead of inlining it
            if (!collect)
                t.replace(s, e - s, text);
            else
            {
                collected->push_back(text);
                t.erase(s, e - s);
            }
        };
        auto known = [](uint32_t id) { return ActivePlayer() && AscCA::SpellKnown(id, true); };
        for (int iter = 0; iter < 100; ++iter)
        {
            if (t.empty() || t.size() <= pos || !FindTag(t, pos, s, e, "@", "@"))
                break;
            std::string first, second, label, icon;
            uint32_t id = 0;
            int32_t flags = 0;
            char buf[256] = {};
            if (SplitTag(t, pos, s, e, "@ext:", ":ext@", false, first, second))
            {
                std::vector<std::string> sub;
                Markup(first, a, cached, stripQuotes, spell, &sub, false, false);
                Markup(second, a, cached, stripQuotes, spell, &sub, false, false);
                if (shift)
                    t.replace(s, e - s, first);
                else if (second.empty())
                    eraseLines(s, e);
                else
                    t.replace(s, e - s, second);
                hint = true;
            }
            else if (SplitTag(t, pos, s, e, "@sim:", ":sim@", true, first, second))
            {
                std::vector<std::string> sub;
                Markup(first, a, cached, stripQuotes, spell, &sub, false, false);
                Markup(second, a, cached, stripQuotes, spell, &sub, false, false);
                t.replace(s, e - s, CVarOn("simplifiedTooltips") ? second : first);
            }
            else if (ScanTag(t, pos, s, e, "@ifknown:", ":ifknown@", "@ifknown:%u:%255[^:]:ifknown@", 2, &id, buf))
            {
                if (a || (ActivePlayer() && (AscCA::SpellKnown(id, true) || AscMysticEnchant::Slotted(id))))
                    t.replace(s, e - s, buf);
                else
                    eraseLines(s, e);
            }
            else if (ScanTag(t, pos, s, e, "@ifnotknown:", ":ifnotknown@", "@ifnotknown:%255[^:]:%u:ifnotknown@", 2, buf, &id))
            {
                if (a || (ActivePlayer() && !AscCA::SpellKnown(id, true) && !AscMysticEnchant::Slotted(id)))
                    t.replace(s, e - s, buf);
                else
                    eraseLines(s, e);
            }
            else if (ScanTag(t, pos, s, e, "@learns:", "@", "@learns:%u@", 1, &id))
            {
                if ((((AscGameMode::Mode() >> 6) & 1) && AscCA::IsCASpell(id)) || known(id))
                    eraseLines(s, e);
                else
                {
                    SpellLabel(id, false, label, icon);
                    t.replace(s, e - s, Colored("FFFFFF", "Unlocks " + icon + " " + label));
                }
            }
            else if (ScanTag(t, pos, s, e, "@nousewith:", "@", "@nousewith:%u@", 1, &id))
            {
                SpellLabel(id, false, label, icon);
                t.replace(s, e - s, Colored("FF1A1A", "Not usable with " + icon + " " + label));
            }
            else if (ScanTag(t, pos, s, e, "@re:", "@", "@re:%u:%d@", 2, &id, &flags))
            {
                const uint32_t f = flags < 0 ? static_cast<uint32_t>(-flags) : 0;
                if ((((f >> 2) & 1) && !shift) || (((f >> 3) & 1) && shift))
                {
                    t.erase(s, e - s);
                    hint = true;
                }
                else
                    put(Render("ENCHANT", id, f & 1, (f >> 1) & 1, cached, true, (f >> 4) & 1, (f >> 5) & 1,
                               (f >> 6) & 1, (f >> 7) & 1));
            }
            else if (ScanTag(t, pos, s, e, "@replaces:", "@", "@replaces:%u@", 1, &id))
            {
                const bool k = known(id);
                SpellLabel(id, true, label, icon);
                t.replace(s, e - s, Colored(k ? "1AFF1A" : "F83600", "Replaces " + icon + " " + label));
            }
            else if (ScanTag(t, pos, s, e, "@req:", "@", "@req:%u@", 1, &id))
            {
                if (noRequirements)
                    t.erase(s, e - s);
                else
                {
                    const bool k = known(id);
                    SpellLabel(id, true, label, icon);
                    t.replace(s, e - s, Colored(k ? "FFFFFF" : "FF1A1A", "Requires " + icon + " " + label));
                }
            }
            else if (ScanTag(t, pos, s, e, "@unlockby:", "@", "@unlockby:%u@", 1, &id))
            {
                if (noRequirements)
                    t.erase(s, e - s);
                else
                {
                    const bool k = known(id);
                    SpellLabel(id, true, label, icon);
                    t.replace(s, e - s, Colored(k ? "1AFF1A" : "F83600", "Unlocked by " + icon + " " + label));
                }
            }
            else if (ScanTag(t, pos, s, e, "@usewith:", "@", "@usewith:%u@", 1, &id))
            {
                SpellLabel(id, false, label, icon);
                t.replace(s, e - s, Colored("FFFFFF", "Usable with " + icon + " " + label));
            }
            else if (ScanTag(t, pos, s, e, "@wflocation:", "@", "@wflocation:%255[^@]@", 1, buf))
            {
                const bool worldforged = AscMysticEnchant::Worldforged(spell);
                const std::string where = Colored("FC8A00", buf);
                const std::string note = Colored("00CCFF", "Worldforged Mystic Enchants are soulbound and must be discovered throughout the world!");
                if (!a && (!worldforged || AscMysticEnchant::Collected(spell)))
                    put("\n" + where);
                else
                    put("\n" + where + "\n" + note);
            }
            else if (ScanTag(t, pos, s, e, "@s:", "@", "@s:%u:%d@", 2, &id, &flags))
            {
                const uint32_t f = flags < 0 ? static_cast<uint32_t>(-flags) : 0;
                if ((((f >> 2) & 1) && !shift) || (((f >> 3) & 1) && shift))
                {
                    t.erase(s, e - s);
                    hint = true;
                }
                else
                    put(Render("SPELL", id, f & 1, (f >> 1) & 1, cached, true, (f >> 4) & 1, (f >> 5) & 1,
                               (f >> 6) & 1, (f >> 7) & 1));
            }
            else if (t.find("@mountspeed@", pos) != std::string::npos)   // FUN_10169050: anywhere after pos
            {
                s = t.find("@mountspeed@", pos);
                pos = s;
                std::string v = "0";
                if (uint8_t* player = ActivePlayer())
                {
                    const int32_t skill = RidingSkill(player);
                    if (skill == 150 || skill == 225 || skill == 300)
                        v = "100";
                    else if (skill == 0)
                        v = "40";
                    else if (skill == 75)
                        v = "60";
                }
                t.replace(s, 12, v);
            }
            else if (t.find("@flyingmountspeed@", pos) != std::string::npos)
            {
                s = t.find("@flyingmountspeed@", pos);
                pos = s;
                std::string v = "0";
                if (uint8_t* player = ActivePlayer())
                {
                    const int32_t skill = RidingSkill(player);
                    if (skill == 225)
                        v = "150";
                    else if (skill == 300)
                    {
                        typedef char(__thiscall * Knows_t)(void*, uint32_t);
                        uint8_t rec[0x2A8];
                        if (reinterpret_cast<Knows_t>(0x7260E0)(player, 0x1667E))
                            v = "310";
                        else if (spell != 0 && FetchSpell(spell, rec))
                        {
                            v = "280";
                            for (uint32_t i = 0; i < 3; ++i)   // SPELL_AURA_MOD_INCREASE_MOUNTED_FLIGHT_SPEED
                                if (RecU32(rec, 0x17C + i * 4) == 0xCF)
                                {
                                    const int32_t speed = static_cast<int32_t>(RecU32(rec, 0x128 + i * 4) + RecU32(rec, 0x140 + i * 4));
                                    if (speed > 0x118)
                                        v = std::to_string(speed);
                                    break;
                                }
                        }
                    }
                }
                t.replace(s, 18, v);
            }
            else
                break;   // an '@...@' nothing recognises ends the pass
            changed = true;
        }
        for (int i = 0; i < 100; ++i)
        {
            const size_t at = t.size() >= 3 ? t.find("\n\n\n") : std::string::npos;
            if (at == std::string::npos)
                break;
            t.replace(at, 3, "\n\n");
            changed = true;
        }
        auto blank = [](char c) { return c == '\n' || c == '\r' || c == ' '; };
        while (!t.empty() && blank(t.front()))
        {
            t.erase(0, 1);
            changed = true;
        }
        while (!t.empty() && blank(t.back()))
        {
            t.pop_back();
            changed = true;
        }
        if (stripQuotes != 0 && !t.empty() && t.front() == '"' && t.back() == '"')
        {
            t.erase(0, 1);
            if (!t.empty())
                t.pop_back();
            changed = true;
        }
        if (hint && !shift)
            t += "\n\n|cff00DDFFHold SHIFT for more information|r";
    }
    else
    {
        t = "|cffFF0000<Error: Maximum recursion depth exceeded>|r";
        changed = true;
    }
    --g_depth;
    return changed;
}

std::string Description(uint32_t spellId, bool a, bool cached, int c)
{
    uint8_t rec[0x2A8];
    const char* desc = FetchSpell(spellId, rec) ? RecStr(rec, 0x228) : nullptr;
    if (!desc || !*desc)
        return std::string();
    static char buffer[0x400];   // DAT_10bdef30
    bool hit = false;
    if (cached)
        for (const FormattedEntry& f : g_formatted)
            if (f.spell == spellId)
            {
                memcpy(buffer, f.text, sizeof(buffer));
                hit = true;
                break;
            }
    if (!hit)
    {
        FormatSpell(rec, buffer, sizeof(buffer), 0, 0, 0, 0, 1, 0);
        FormattedEntry f;
        f.spell = spellId;
        memcpy(f.text, buffer, sizeof(buffer));
        f.time = _time64(nullptr);
        g_formatted.push_back(f);
    }
    std::string text = buffer;
    if (cached)
        for (const MarkupEntry& m : g_markup)
            if (m.spell == spellId)
                return m.text;
    std::vector<std::string> collected;
    Markup(text, a, cached, 0, spellId, &collected, false, c != 0);
    g_markup.push_back({spellId, text, _time64(nullptr)});
    return text;
}
}

namespace
{
    // FUN_101ac9b0 (after 0x403340): markup results live 20 s, formatter output 300 s.
    void Tick()
    {
        for (size_t i = 0; i < g_markup.size();)
        {
            if (_time64(nullptr) - g_markup[i].time > 0x14)
                g_markup.erase(g_markup.begin() + i);
            else
                ++i;
        }
        for (size_t i = 0; i < g_formatted.size();)
        {
            if (_time64(nullptr) - g_formatted[i].time > 0x12C)
                g_formatted.erase(g_formatted.begin() + i);
            else
                ++i;
        }
    }
    void Reset()   // FUN_101a85c0
    {
        g_markup.clear();
        g_formatted.clear();
    }

    // C_Format.Format(text [, full [, stripQuotes [, spellId]]]) -> text, {collected}   (FUN_101acb30)
    int Format(lua_State* L)
    {
        const int t2 = AscLua::lua_type(L, 2), t3 = AscLua::lua_type(L, 3), t4 = AscLua::lua_type(L, 4);
        const int types[4] = {STRING, t2 > 0 ? BOOLEAN : NIL, t3 > 0 ? NUMBER : NIL, t4 > 0 ? NUMBER : NIL};
        if (!ValidateInput(L, types, 4))
            return 0;
        std::string text = CheckString(L, 1);
        const bool full = t2 > 0 && AscLua::lua_toboolean(L, 2) != 0;
        const int stripQuotes = t3 > 0 ? ToInt(CheckNumber(L, 3)) : 0;
        const uint32_t spell = t4 > 0 ? static_cast<uint32_t>(ToInt(CheckNumber(L, 4))) : 0;
        std::vector<std::string> collected;
        AscSpellText::Markup(text, full, false, stripQuotes, spell, &collected, true, false);
        PushStr(L, text.c_str());
        AscLua::lua_createtable(L, 0, static_cast<int>(collected.size()));   // FUN_100beb90
        AscLua::lua_checkstack(L, 2);
        for (size_t i = 0; i < collected.size(); ++i)
        {
            PushNum(L, static_cast<double>(i + 1));
            PushStr(L, collected[i].c_str());
            AscLua::lua_settable(L, -3);
        }
        return 2;
    }

    // GetSpellDescription([spellId [, full]]) (FUN_10a4dd90 -> FUN_101ac910(spell, full, 0, 0))
    int GetSpellDescription(lua_State* L)
    {
        const uint32_t spell = AscLua::lua_gettop(L) >= 1 ? static_cast<uint32_t>(ToInt(CheckNumber(L, 1))) : 0;
        const bool full = AscLua::lua_gettop(L) >= 2 && AscLua::lua_toboolean(L, 2) != 0;
        PushStr(L, AscSpellText::Description(spell, full, false, 0).c_str());
        return 1;
    }

    void Init()
    {
        AscRuntime::OnAfter403340(&Tick);
        AscRuntime::OnGlueScreen(&Reset);
        // FUN_10114540 x2 (storage 0x10BDEF10 / 0x10BDEF14): no help, flags 1, "0", no callback, category 3.
        AscClientOptions::QueueWorldCVar({"extendedTooltips", "0", 1, 3, nullptr});
        AscClientOptions::QueueWorldCVar({"simplifiedTooltips", "0", 1, 3, nullptr});
    }

    const AscBindings::Binding kBindings[] = {
        {"C_Format", "Format", Format},
        {nullptr, "GetSpellDescription", GetSpellDescription},
    };
    AscBindings::Module s_module(kBindings, sizeof(kBindings) / sizeof(kBindings[0]), &Init);
}
