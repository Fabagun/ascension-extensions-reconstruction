#pragma once
// The original's FilterableContainerBase<T, NullFilterType, NullSortType> (engine FUN_10a169f0 and
// its instantiations; populate FUN_100c5910, token parse FUN_100c2840, reset FUN_101016d0). About 18
// managers use it (tutorials, keywords, mentors, skill cards, challenges, appearances, outfits,
// recovery services, build creator, custom store, GM tickets, ...).
//
// Layout it models: +0x04 all entries, +0x10 filtered entries (what the Lua getters index), +0x1C the
// last filter text, token/argument caches, a sort-key map, +0x78 "dirty" (repopulate on next filter).
//
// Per-type virtuals, by original vtable slot:
//   [0] Populate          [1] TokenMatch(type, value, e) -- default false (any $token empties the list)
//   [3] TextMatch(lowercase text, e)                        [6] OnReset (no-op in every instantiation seen)
//   [7] PopulateKey(e)    [8] FilterKey(e)                  -- default all-zero keys
// Slots [2] [4] [5] [9] [10] (argument groups) keep their base defaults in every instantiation
// transcribed so far, where they contribute nothing; they are not modelled until one overrides them.
//
// Sort keys are the original's boost::multiprecision integers built by shift-and-or of fixed-width
// fields. Numeric order of such a value equals lexicographic order of the fields written big-endian,
// so a key here is that byte string (AppendU32 / AppendByte), compared with std::string's operator<.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace AscFilter
{
    struct Token { char type; int32_t value; };

    inline void AppendU32(std::string& k, uint32_t v)
    {
        for (int s = 24; s >= 0; s -= 8)
            k.push_back(static_cast<char>((v >> s) & 0xFF));
    }
    inline void AppendByte(std::string& k, uint8_t v) { k.push_back(static_cast<char>(v)); }
    // A key from a signed 64-bit value, as the fixed-width unsigned integer the original builds: a
    // negative value wraps (two's complement), so it orders after every non-negative one.
    inline void AppendU64(std::string& k, uint64_t v)
    {
        AppendU32(k, static_cast<uint32_t>(v >> 32));
        AppendU32(k, static_cast<uint32_t>(v));
    }

    inline std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // FUN_100c2840: strip every "$<type><digits>" word into a token (a word whose tail is not all
    // digits is removed without producing a token), then trim both ends and collapse double spaces.
    inline std::vector<Token> ParseTokens(std::string& text)
    {
        std::vector<Token> tokens;
        size_t pos = 0;
        while (pos < text.size() && (pos = text.find('$', pos)) != std::string::npos)
        {
            size_t end = text.find(' ', pos);
            if (end == std::string::npos)
                end = text.size();
            if (pos + 2 < end)
            {
                const std::string digits = text.substr(pos + 2, end - pos - 2);
                bool allDigits = true;
                for (char c : digits)
                    allDigits = allDigits && isdigit(static_cast<unsigned char>(c));
                if (allDigits)
                {
                    uint32_t v = 0;
                    bool overflow = false;
                    for (char c : digits)
                    {
                        const uint32_t d = static_cast<uint32_t>(c - '0');
                        if (v < 0x0CCCCCCC || (v == 0x0CCCCCCC && d <= 7))
                            v = v * 10 + d;
                        else
                            overflow = true;
                    }
                    tokens.push_back({text[pos + 1], overflow ? 0 : static_cast<int32_t>(v)});
                }
            }
            text.erase(pos, end - pos);
        }
        size_t a = 0;
        while (a < text.size() && isspace(static_cast<unsigned char>(text[a])))
            ++a;
        text.erase(0, a);
        while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
            text.pop_back();
        std::string out;
        for (char c : text)
            if (!(c == ' ' && !out.empty() && out.back() == ' '))
                out.push_back(c);
        text.swap(out);
        return tokens;
    }

    template <class T>
    class Container
    {
    public:
        virtual ~Container() = default;

        const std::vector<T>& Filtered() const { return m_filtered; }
        size_t Size() const { return m_filtered.size(); }
        T At(size_t i) const { return i < m_filtered.size() ? m_filtered[i] : T(); }

        // FUN_101016d0
        void Reset()
        {
            m_all.clear();
            m_filtered.clear();
            m_lastText.clear();
            m_lastTokens.clear();
            m_dirty = true;
            OnReset();
        }

        // FUN_10a169f0 (param_4, "incremental", only ever narrows an already-filtered list whose result
        // equals refiltering from scratch, so it is not modelled separately).
        void ApplyFilter(const std::string& filter)
        {
            std::string text = filter;
            if (m_all.empty())
                Populate();
            if (m_all.empty())
            {
                m_dirty = false;
                return;
            }
            const std::vector<Token> tokens = ParseTokens(text);
            const std::string lower = Lower(text);

            m_filtered.clear();
            for (const T& e : m_all)
            {
                bool keep = true;
                for (const Token& t : tokens)
                    keep = keep && TokenMatch(t.type, t.value, e);
                if (keep && TextMatch(lower, e))
                    m_filtered.push_back(e);
            }
            m_lastText = text;
            m_lastTokens = tokens;

            std::vector<std::pair<std::string, T>> keyed;
            keyed.reserve(m_filtered.size());
            for (const T& e : m_filtered)
                keyed.emplace_back(FilterKey(e), e);
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keyed.size(); ++i)
                m_filtered[i] = keyed[i].second;
            m_dirty = false;
        }

        // What the original's own refilters pass (FUN_10a169f0 on +0x1C): the cached text AFTER token
        // extraction, so a category/spec token in the last filter is dropped on a refilter.
        void Reapply() { const std::string t = m_lastText; ApplyFilter(t); }

        // FUN_100fca40, the engine with argument groups and sort selections (Build Creator). Every
        // token must match; every group of arguments (ArgGroup) needs one argument that matches, except
        // that group 0 fails outright on its first argument that does not; then the text. The result is
        // std::sort-ed (FUN_100bce30 / comparator FUN_100c1460) on PopulateKey, then each selected
        // sort's SortKey in the order given, then FilterKey.
        void ApplyFilter(const std::string& filter, const std::vector<uint32_t>& args, const std::vector<uint32_t>& sorts)
        {
            BeforeFilter(filter, args, sorts);
            std::string text = filter;
            if (m_all.empty())
                Populate();
            if (m_all.empty())
            {
                m_dirty = false;
                return;
            }
            std::vector<std::pair<uint32_t, std::vector<uint32_t>>> groups;
            for (uint32_t a : args)
            {
                const uint32_t g = ArgGroup(a);
                auto it = std::find_if(groups.begin(), groups.end(), [g](const auto& p) { return p.first == g; });
                if (it == groups.end())
                    groups.emplace_back(g, std::vector<uint32_t>{a});
                else
                    it->second.push_back(a);
            }
            const std::vector<Token> tokens = ParseTokens(text);
            const std::string lower = Lower(text);

            m_filtered.clear();
            for (const T& e : m_all)
            {
                bool keep = true;
                for (const Token& t : tokens)
                    if (keep)
                        keep = TokenMatch(t.type, t.value, e);
                for (const auto& g : groups)
                {
                    if (!keep)
                        break;
                    bool any = false;
                    for (uint32_t a : g.second)
                    {
                        if (!keep || any)
                            break;
                        if (ArgMatch(a, e))
                            any = true;
                        else if (g.first == 0)
                            keep = false;
                    }
                    if (!any)
                        keep = false;
                }
                if (keep && TextMatch(lower, e))
                    m_filtered.push_back(e);
            }
            m_lastText = text;
            m_lastTokens = tokens;

            struct Keyed
            {
                std::string populate, filter;
                std::vector<std::string> sort;
                T e;
            };
            std::vector<Keyed> keyed;
            keyed.reserve(m_filtered.size());
            for (const T& e : m_filtered)
            {
                Keyed k{PopulateKey(e), FilterKey(e), {}, e};
                for (uint32_t s : sorts)
                    k.sort.push_back(SortKey(s, e));
                keyed.push_back(std::move(k));
            }
            std::sort(keyed.begin(), keyed.end(), [](const Keyed& a, const Keyed& b) {
                if (a.populate != b.populate)
                    return a.populate < b.populate;
                for (size_t i = 0; i < a.sort.size(); ++i)
                    if (a.sort[i] != b.sort[i])
                        return a.sort[i] < b.sort[i];
                return a.filter < b.filter;
            });
            for (size_t i = 0; i < keyed.size(); ++i)
                m_filtered[i] = keyed[i].e;
            m_dirty = false;
        }

    protected:
        virtual std::vector<T> DoPopulate() = 0;
        virtual bool TokenMatch(char, int32_t, const T&) { return false; }
        // Argument groups (slots [2] [4]) and sort selections (slot [9]); base: no match, group 0, no key.
        virtual bool ArgMatch(uint32_t, const T&) { return false; }
        virtual uint32_t ArgGroup(uint32_t) { return 0; }
        virtual std::string SortKey(uint32_t, const T&) { return std::string(); }
        virtual bool TextMatch(const std::string& lowerText, const T& e) = 0;
        virtual void OnReset() {}
        virtual std::string PopulateKey(const T&) { return std::string(); }
        virtual std::string FilterKey(const T&) { return std::string(); }
        // A derived query override's own work before the base query (e.g. FUN_101b41b0 -> FUN_101b4a70).
        virtual void BeforeFilter(const std::string&, const std::vector<uint32_t>&, const std::vector<uint32_t>&) {}
        // What a derived override sees at +4 / +8 (FUN_101b47b0 populates when empty).
        const std::vector<T>& All()
        {
            if (m_all.empty())
                Populate();
            return m_all;
        }

    private:
        // FUN_100c5910: fetch, order by PopulateKey (ascending insertion sort), filtered = all.
        void Populate()
        {
            m_all = DoPopulate();
            std::vector<std::pair<std::string, T>> keyed;
            for (const T& e : m_all)
                keyed.emplace_back(PopulateKey(e), e);
            std::stable_sort(keyed.begin(), keyed.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (size_t i = 0; i < keyed.size(); ++i)
                m_all[i] = keyed[i].second;
            m_filtered = m_all;
            m_dirty = true;
        }

        std::vector<T> m_all, m_filtered;
        std::string m_lastText;
        std::vector<Token> m_lastTokens;
        bool m_dirty = true;
    };
}
