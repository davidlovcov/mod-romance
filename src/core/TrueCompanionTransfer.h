/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 *
 * Paged export / import of a companion (personality, settings, memories).
 *
 * A full export can be far larger than one addon message (the server accepts at
 * most 16 KiB per message, the addon 64 KiB). Exports are therefore sent as
 * pages: page 0 carries everything except most memories, every page carries a
 * slice of the memories, and the addon joins them. Imports travel the same way
 * in the other direction (import_begin / import_part / import_end) and are only
 * applied when every part arrived.
 *
 * Engine independent; covered by the unit tests.
 */

#ifndef MOD_TRUE_COMPANION_TRANSFER_H
#define MOD_TRUE_COMPANION_TRANSFER_H

#include "TrueCompanionJson.h"
#include "TrueCompanionTypes.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace truecompanion
{
    // Server export pages are at most this many bytes of JSON text each.
    constexpr size_t kExportPageBytes = 8 * 1024;
    // Most parts one import may have, and most bytes it may buffer.
    constexpr size_t kMaxImportParts = 128;
    constexpr size_t kMaxImportBytes = 1024 * 1024;
    // An unfinished import is dropped this long after its last part.
    constexpr int64_t kImportTimeoutSec = 60;

    // The "format" of an export. Exports made before the module was renamed (mod-romance) are still accepted.
    constexpr char const* kExportFormat = "mod-true-companion-export";
    inline bool IsExportFormat(std::string const& f) { return f == kExportFormat || f == "mod-romance-export"; }

    // Splits a full export ({format, ..., memories[]}) into pages of at most `maxPageBytes` JSON each.
    // Every page is {"format":"mod-true-companion-export", "page":i, "pages":n, ...}; page 0 also has every
    // other field of the export. Always returns at least one page.
    std::vector<std::string> SplitExportPages(Json const& full, size_t maxPageBytes = kExportPageBytes);

    // Joins pages (in order) back into one export; nullopt if a page is missing, malformed or out of order.
    std::optional<Json> JoinExportPages(std::vector<std::string> const& pages);

    // An unfinished export snapshot is dropped this long after its last page request.
    constexpr int64_t kExportSnapshotTtlSec = 60;

    // One frozen export per companion while the addon fetches its pages: page 0 builds and freezes it, later pages
    // come from that snapshot, so memories that change in between (conversation, gifts, cleanup) can't mix two
    // states. The addon sends the snapshot id with every later page; a different or expired id is refused.
    class ExportSnapshots
    {
    public:
        struct Page
        {
            std::string error;      // export_disabled | snapshot_expired | bad_part
            std::string data;
            size_t parts = 0;
            std::string snapshot;
        };
        Page Get(uint64_t companion, size_t part, std::string const& snapshotId,
                 std::function<std::vector<std::string>()> const& build, TimePoint now);
        size_t Count() const { return _byCompanion.size(); }

    private:
        struct Snap
        {
            std::string id;
            std::vector<std::string> pages;
            TimePoint lastUse = 0;
        };
        std::map<uint64_t, Snap> _byCompanion;
        uint64_t _next = 0;
    };

    // Collects the parts of one import on the server.
    class ImportAssembler
    {
    public:
        // Starts (or restarts) an import with `parts` parts. Returns an error code or "".
        std::string Begin(size_t parts, TimePoint now);
        // Adds part `index` (JSON text of one page). Returns an error code or "".
        std::string Add(size_t index, std::string const& data, TimePoint now);
        // All parts there (and the import not timed out): the joined export. Otherwise an error code in `error`.
        std::optional<Json> Finish(std::string& error, TimePoint now);
        bool Active(TimePoint now) const { return _parts > 0 && now - _last <= kImportTimeoutSec; }
        size_t Received() const;
        size_t Expected() const { return _parts; }
        void Reset();

    private:
        size_t _parts = 0;
        size_t _bytes = 0;
        TimePoint _last = 0;
        std::vector<std::optional<std::string>> _data;
    };
}

#endif
