/*
 * mod-romance — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "RomanceMemory.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_set>

namespace romance
{
    namespace
    {
        std::unordered_set<std::string> const& StopWords()
        {
            static std::unordered_set<std::string> const words = {
                // English
                "the", "a", "an", "and", "or", "but", "is", "are", "was", "were", "be", "been", "am", "i", "you", "he",
                "she", "it", "we", "they", "me", "him", "her", "us", "them", "my", "your", "his", "its", "our", "their",
                "to", "of", "in", "on", "at", "for", "with", "about", "as", "by", "from", "that", "this", "these",
                "those", "what", "which", "who", "how", "do", "does", "did", "have", "has", "had", "so", "just", "very",
                "really", "can", "could", "would", "should", "will", "not", "no", "yes", "too", "there", "here", "then",
                "than", "if", "when", "like", "into", "up", "out", "some", "any", "all", "one", "get", "got", "know",
                "think", "hey", "hi", "hello", "oh", "ok", "okay", "well", "also", "still", "again", "now", "today",
                // German
                "der", "die", "das", "den", "dem", "des", "ein", "eine", "einen", "einem", "einer", "und", "oder",
                "aber", "ist", "sind", "war", "waren", "bin", "bist", "ich", "du", "er", "sie", "es", "wir", "ihr",
                "mich", "dich", "mir", "dir", "uns", "euch", "mein", "meine", "dein", "deine", "sein", "seine", "zu",
                "von", "mit", "auf", "für", "fuer", "an", "im", "in", "am", "nicht", "kein", "keine", "ja", "nein",
                "auch", "noch", "wie", "was", "wer", "wo", "wenn", "dann", "so", "sehr", "hab", "habe", "hast", "hat",
                "haben", "wird", "werden", "kann", "kannst", "doch", "mal", "schon", "hallo", "hey", "na", "denn" };
            return words;
        }

        std::string Stem(std::string w)
        {
            if (w.size() > 4 && w.compare(w.size() - 2, 2, "'s") == 0)
                w.resize(w.size() - 2);
            if (w.size() > 5 && w.compare(w.size() - 3, 3, "ies") == 0)
                w = w.substr(0, w.size() - 3) + "y";
            else if (w.size() > 4 && w.back() == 's' && w[w.size() - 2] != 's')
                w.pop_back();
            return w;
        }

        bool SimilarToken(std::string const& a, std::string const& b)
        {
            if (a == b)
                return true;
            // prefix match for inflections (necklace / necklaces, Halskette / Halsketten)
            size_t n = std::min(a.size(), b.size());
            return n >= 5 && a.compare(0, 5, b, 0, 5) == 0 && std::max(a.size(), b.size()) - n <= 3;
        }

        double SourceWeight(MemorySource s)
        {
            switch (s)
            {
                case MemorySource::Observed: return 1.0;
                case MemorySource::UserEdited: return 1.0;
                case MemorySource::Profile: return 0.9;
                case MemorySource::PlayerStated: return 0.8;
                case MemorySource::ModelSummary: return 0.6;
                case MemorySource::Interpretation: return 0.4;
            }
            return 0.5;
        }
    }

    std::vector<std::string> Tokenize(std::string const& text)
    {
        std::vector<std::string> out;
        std::string lower = ToLower(text);
        std::string cur;
        auto flush = [&]()
        {
            if (cur.size() >= 2 && !StopWords().count(cur))
                out.push_back(Stem(cur));
            cur.clear();
        };
        for (size_t i = 0; i < lower.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(lower[i]);
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80 || c == '\'')
                cur.push_back(char(c));
            else
                flush();
        }
        flush();
        return out;
    }

    double TokenJaccard(std::vector<std::string> const& a, std::vector<std::string> const& b)
    {
        if (a.empty() && b.empty())
            return 1.0;
        std::set<std::string> sa(a.begin(), a.end()), sb(b.begin(), b.end());
        size_t inter = 0;
        for (auto const& x : sa)
            for (auto const& y : sb)
                if (SimilarToken(x, y))
                {
                    ++inter;
                    break;
                }
        size_t uni = sa.size() + sb.size() - inter;
        return uni ? double(inter) / double(uni) : 0.0;
    }

    void MemoryStore::Load(std::vector<MemoryEntry> entries)
    {
        _entries = std::move(entries);
        _nextId = 1;
        for (auto const& e : _entries)
            _nextId = std::max(_nextId, e.id + 1);
    }

    MemoryEntry const* MemoryStore::Find(uint32_t id) const
    {
        for (auto const& e : _entries)
            if (e.id == id)
                return &e;
        return nullptr;
    }

    int MemoryStore::SourceCertainty(MemorySource s) { return int(SourceWeight(s) * 100); }

    std::string MemoryStore::PromptTag(MemorySource s)
    {
        switch (s)
        {
            case MemorySource::Observed: return "witnessed";
            case MemorySource::PlayerStated: return "they told you";
            case MemorySource::ModelSummary: return "your recollection";
            case MemorySource::Interpretation: return "your impression, unsure";
            case MemorySource::UserEdited: return "certain";
            case MemorySource::Profile: return "shared history";
        }
        return "memory";
    }

    MemoryStore::AddResult MemoryStore::Add(MemoryKind kind, MemorySource source, int importance, std::string const& textIn,
                                            TimePoint now)
    {
        AddResult r;
        std::string text = LimitUtf8(CollapseWhitespace(Trim(textIn)), 300);
        text.erase(std::remove(text.begin(), text.end(), '|'), text.end());
        if (text.size() < 4 || !IsValidUtf8(text))
            return r;
        importance = std::max(0, std::min(100, importance));

        std::vector<std::string> tokens = Tokenize(text);
        if (tokens.empty())
            return r;

        // Merge with a near-duplicate of a compatible kind instead of storing twice.
        for (auto& e : _entries)
        {
            bool compatibleKind = e.kind == kind || (e.kind == MemoryKind::Fact && kind == MemoryKind::Preference) ||
                                  (e.kind == MemoryKind::Preference && kind == MemoryKind::Fact);
            if (!compatibleKind)
                continue;
            if (TokenJaccard(tokens, Tokenize(e.text)) >= _cfg.dedupeThreshold)
            {
                e.importance = std::min(100, std::max(e.importance, importance) + 3);
                e.updatedAt = now;
                // Prefer the more reliable source and its wording.
                if (SourceWeight(source) > SourceWeight(e.source) && e.source != MemorySource::UserEdited)
                {
                    e.source = source;
                    e.text = text;
                }
                r.stored = true;
                r.merged = true;
                r.entry = e;
                return r;
            }
        }

        MemoryEntry m;
        m.id = _nextId++;
        m.kind = kind;
        m.source = source;
        m.importance = importance;
        m.text = text;
        m.createdAt = now;
        m.updatedAt = now;
        _entries.push_back(m);
        r.stored = true;
        r.entry = m;
        return r;
    }

    bool MemoryStore::Remove(uint32_t id)
    {
        auto it = std::find_if(_entries.begin(), _entries.end(), [id](MemoryEntry const& e) { return e.id == id; });
        if (it == _entries.end())
            return false;
        _entries.erase(it);
        return true;
    }

    std::optional<MemoryEntry> MemoryStore::Edit(uint32_t id, std::string const& textIn, std::optional<int> importance, TimePoint now)
    {
        std::string text = LimitUtf8(CollapseWhitespace(Trim(textIn)), 300);
        text.erase(std::remove(text.begin(), text.end(), '|'), text.end());
        if (text.size() < 2 || !IsValidUtf8(text))
            return std::nullopt;
        for (auto& e : _entries)
            if (e.id == id)
            {
                e.text = text;
                e.source = MemorySource::UserEdited;   // a human confirmed/corrected it
                if (importance)
                    e.importance = std::max(0, std::min(100, *importance));
                e.updatedAt = now;
                return e;
            }
        return std::nullopt;
    }

    std::optional<MemoryEntry> MemoryStore::SetPinned(uint32_t id, bool pinned)
    {
        for (auto& e : _entries)
            if (e.id == id)
            {
                e.pinned = pinned;
                return e;
            }
        return std::nullopt;
    }

    std::vector<uint32_t> MemoryStore::RemoveKind(std::optional<MemoryKind> kind)
    {
        std::vector<uint32_t> removed;
        std::vector<MemoryEntry> keep;
        for (auto const& e : _entries)
        {
            if (!kind || e.kind == *kind)
                removed.push_back(e.id);
            else
                keep.push_back(e);
        }
        _entries.swap(keep);
        return removed;
    }

    std::vector<MemoryEntry> MemoryStore::Retrieve(std::string const& query, TimePoint now, size_t maxCount, size_t charBudget)
    {
        std::vector<MemoryEntry> result;
        if (_entries.empty() || maxCount == 0)
            return result;

        std::vector<std::string> q = Tokenize(query);

        // Inverse document frequency over the store so common words count less.
        std::map<std::string, double> idf;
        {
            std::map<std::string, int> df;
            std::vector<std::vector<std::string>> docs;
            for (auto const& e : _entries)
            {
                auto toks = Tokenize(e.text);
                std::set<std::string> uniq(toks.begin(), toks.end());
                for (auto const& t : uniq)
                    ++df[t];
            }
            double n = double(_entries.size());
            for (auto const& t : q)
            {
                int d = 0;
                for (auto const& [term, count] : df)
                    if (SimilarToken(term, t))
                        d = std::max(d, count);
                idf[t] = std::log(1.0 + n / (1.0 + d));
            }
        }
        double qWeight = 0;
        for (auto const& t : q)
            qWeight += idf[t];

        struct Scored { size_t idx; double score; bool core; };
        std::vector<Scored> scored;
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            MemoryEntry const& e = _entries[i];
            double rel = 0;
            if (qWeight > 0)
            {
                auto toks = Tokenize(e.text);
                double hit = 0;
                for (auto const& t : q)
                    for (auto const& x : toks)
                        if (SimilarToken(t, x))
                        {
                            hit += idf[t];
                            break;
                        }
                rel = std::min(1.0, hit / qWeight);
            }
            double ageDays = std::max(0.0, double(now - e.createdAt) / 86400.0);
            double recency = std::exp(-ageDays / 20.0);
            double s = 0.5 * rel + 0.25 * (e.importance / 100.0) + 0.15 * recency + 0.1 * SourceWeight(e.source);
            if (e.pinned)
                s += 0.3;
            bool core = e.pinned || (e.importance >= 85 && (e.kind == MemoryKind::Relationship || e.kind == MemoryKind::Fact));
            scored.push_back({ i, s, core });
        }
        // Core memories first (they define the relationship), then by score.
        std::sort(scored.begin(), scored.end(), [](Scored const& a, Scored const& b)
        {
            if (a.core != b.core)
                return a.core;
            return a.score > b.score;
        });

        size_t used = 0;
        size_t coreTaken = 0;
        for (auto const& s : scored)
        {
            if (result.size() >= maxCount)
                break;
            if (s.core && ++coreTaken > std::max<size_t>(2, maxCount / 3))
                continue; // keep room for relevant memories
            MemoryEntry& e = _entries[s.idx];
            if (used + e.text.size() > charBudget)
                continue;
            used += e.text.size();
            e.lastRecalledAt = now;
            ++e.recallCount;
            result.push_back(e);
        }
        // Present oldest first so the model reads them as a timeline.
        std::sort(result.begin(), result.end(), [](MemoryEntry const& a, MemoryEntry const& b) { return a.createdAt < b.createdAt; });
        return result;
    }

    bool MemoryStore::IsProtected(MemoryEntry const& m) const
    {
        return m.pinned || (m.kind == MemoryKind::Relationship && m.source == MemorySource::Observed) ||
               m.source == MemorySource::UserEdited;
    }

    double MemoryStore::RetentionScore(MemoryEntry const& m, TimePoint now) const
    {
        double ageDays = std::max(0.0, double(now - m.updatedAt) / 86400.0);
        return m.importance / 100.0 * 0.6 + std::exp(-ageDays / 60.0) * 0.25 + std::min(1.0, m.recallCount / 10.0) * 0.15 +
               SourceWeight(m.source) * 0.1;
    }

    std::vector<uint32_t> MemoryStore::Prune(TimePoint now)
    {
        std::vector<uint32_t> removed;
        if (_cfg.memoryRetentionDays > 0)
        {
            int64_t limit = int64_t(_cfg.memoryRetentionDays) * 86400;
            for (auto const& e : _entries)
                if (!IsProtected(e) && e.importance < 70 && now - e.updatedAt > limit)
                    removed.push_back(e.id);
        }
        for (uint32_t id : removed)
            Remove(id);

        if (_entries.size() > _cfg.maxMemoriesPerCompanion)
        {
            std::vector<std::pair<double, uint32_t>> cand;
            for (auto const& e : _entries)
                if (!IsProtected(e))
                    cand.push_back({ RetentionScore(e, now), e.id });
            std::sort(cand.begin(), cand.end());
            size_t excess = _entries.size() - _cfg.maxMemoriesPerCompanion;
            for (size_t i = 0; i < excess && i < cand.size(); ++i)
            {
                Remove(cand[i].second);
                removed.push_back(cand[i].second);
            }
        }
        return removed;
    }

    std::vector<MemoryEntry> MemoryStore::Filter(std::optional<MemoryKind> kind, std::string const& search, size_t offset,
                                                 size_t limit, size_t* total) const
    {
        std::vector<MemoryEntry> matched;
        std::string needle = ToLower(Trim(search));
        for (auto const& e : _entries)
        {
            if (kind && e.kind != *kind)
                continue;
            if (!needle.empty() && ToLower(e.text).find(needle) == std::string::npos)
                continue;
            matched.push_back(e);
        }
        std::sort(matched.begin(), matched.end(), [](MemoryEntry const& a, MemoryEntry const& b) { return a.createdAt > b.createdAt || (a.createdAt == b.createdAt && a.id > b.id); });
        if (total)
            *total = matched.size();
        std::vector<MemoryEntry> page;
        for (size_t i = offset; i < matched.size() && page.size() < limit; ++i)
            page.push_back(matched[i]);
        return page;
    }
}
