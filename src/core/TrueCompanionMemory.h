/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Long-term memory store for one (player, companion) pair.
 *  - every entry records its kind, its source (observed / player said / model
 *    summary / interpretation / user edited) and an importance score
 *  - near-duplicates are merged instead of stored twice
 *  - retrieval ranks by relevance to the current message, importance,
 *    recency and certainty, within a character budget
 *  - pruning keeps the store bounded without touching pinned or milestone entries
 */

#ifndef MOD_TRUE_COMPANION_MEMORY_H
#define MOD_TRUE_COMPANION_MEMORY_H

#include "TrueCompanionConfig.h"
#include "TrueCompanionTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace truecompanion
{
    // Lower-cased word tokens with stop words removed and light stemming (EN + DE).
    std::vector<std::string> Tokenize(std::string const& text);
    double TokenContainment(std::vector<std::string> const& a, std::vector<std::string> const& b);
    // "currently", "right now", "they just ..." - situation statements that should not become long-term memories.
    bool IsTransientMemory(std::string const& text);
    double TokenJaccard(std::vector<std::string> const& a, std::vector<std::string> const& b);

    class MemoryStore
    {
    public:
        explicit MemoryStore(TrueCompanionConfig const& cfg) : _cfg(cfg) { }

        void Load(std::vector<MemoryEntry> entries);
        void Clear() { _entries.clear(); }

        struct AddResult
        {
            bool stored = false;     // false if rejected (empty/invalid)
            bool merged = false;     // merged into an existing entry
            MemoryEntry entry;       // the new or updated entry (persist this)
        };
        AddResult Add(MemoryKind kind, MemorySource source, int importance, std::string const& text, TimePoint now);

        bool Remove(uint32_t id);
        std::optional<MemoryEntry> Edit(uint32_t id, std::string const& text, std::optional<int> importance, TimePoint now);
        std::optional<MemoryEntry> SetPinned(uint32_t id, bool pinned);
        std::vector<uint32_t> RemoveKind(std::optional<MemoryKind> kind);

        // Rank memories for a prompt. Updates recall statistics of returned entries.
        std::vector<MemoryEntry> Retrieve(std::string const& query, TimePoint now, size_t maxCount, size_t charBudget);

        // Enforce the per-companion limit and retention period. Returns removed ids.
        std::vector<uint32_t> Prune(TimePoint now);

        // Tidies stored memories (e.g. from older versions): merges duplicates and drops the companion's own
        // impressions of a momentary situation. Pinned and player-written memories are never removed.
        struct CompactResult
        {
            std::vector<uint32_t> removed;
            std::vector<MemoryEntry> changed;
        };
        CompactResult Compact(TimePoint now);

        std::vector<MemoryEntry> Filter(std::optional<MemoryKind> kind, std::string const& search, size_t offset,
                                        size_t limit, size_t* total = nullptr) const;

        std::vector<MemoryEntry> const& All() const { return _entries; }
        MemoryEntry const* Find(uint32_t id) const;
        uint32_t NextId() const { return _nextId; }
        size_t Size() const { return _entries.size(); }
        // Entries Prune() may remove (not pinned, not player-written, not an observed milestone).
        size_t PrunableCount() const;

        static int SourceCertainty(MemorySource s); // 0..100, how much the companion can rely on it
        static std::string PromptTag(MemorySource s);

    private:
        double RetentionScore(MemoryEntry const& m, TimePoint now) const;
        bool IsProtected(MemoryEntry const& m) const;

        TrueCompanionConfig const& _cfg;
        std::vector<MemoryEntry> _entries;
        uint32_t _nextId = 1;
    };
}

#endif
