/*
 * mod-true-companion — Licensed under the GNU GPL v2 or later. See LICENSE.
 */

#include "TrueCompanionTransfer.h"

#include <algorithm>

namespace truecompanion
{
    namespace
    {

        Json PageHeader(Json const* full, size_t index)
        {
            Json p = Json::MakeObject();
            if (full)
                for (auto const& [k, v] : full->AsObject())
                    if (k != "memories")
                        p.Set(k, v);
            p.Set("format", kExportFormat);
            p.Set("page", int64_t(index));
            p.Set("pages", int64_t(0));          // patched once the page count is known
            p.Set("memories", Json::MakeArray());
            return p;
        }

        // Dump size of `page` with `pages` written as a number of up to 4 digits (stable estimate).
        size_t PageSize(Json const& page) { return page.Dump().size() + 4; }
    }

    std::vector<std::string> SplitExportPages(Json const& full, size_t maxPageBytes)
    {
        std::vector<Json> pages;
        pages.push_back(PageHeader(&full, 0));
        size_t current = PageSize(pages.back());
        for (Json const& m : full["memories"].AsArray())
        {
            size_t add = m.Dump().size() + 1;
            if (current + add > maxPageBytes && !pages.back()["memories"].AsArray().empty())
            {
                pages.push_back(PageHeader(nullptr, pages.size()));
                current = PageSize(pages.back());
            }
            pages.back().Set("memories", [&] { Json a = pages.back()["memories"]; a.Push(m); return a; }());
            current += add;
        }
        std::vector<std::string> out;
        for (auto& p : pages)
        {
            p.Set("pages", int64_t(pages.size()));
            out.push_back(p.Dump());
        }
        return out;
    }

    std::optional<Json> JoinExportPages(std::vector<std::string> const& pages)
    {
        if (pages.empty())
            return std::nullopt;
        Json full = Json::MakeObject();
        Json memories = Json::MakeArray();
        for (size_t i = 0; i < pages.size(); ++i)
        {
            auto page = Json::Parse(pages[i], nullptr, JsonParseLimits{ kExportPageBytes * 4, 12 });
            if (!page || !page->IsObject() || !IsExportFormat(page->GetString("format")) || page->GetInt("page", -1) != int64_t(i) ||
                page->GetInt("pages", -1) != int64_t(pages.size()) || !(*page)["memories"].IsArray())
                return std::nullopt;
            if (i == 0)
                for (auto const& [k, v] : page->AsObject())
                    if (k != "memories" && k != "page" && k != "pages")
                        full.Set(k, v);
            for (Json const& m : (*page)["memories"].AsArray())
                memories.Push(m);
        }
        full.Set("memories", memories);
        return full;
    }

    std::string ImportAssembler::Begin(size_t parts, TimePoint now)
    {
        Reset();
        if (parts == 0 || parts > kMaxImportParts)
            return "import_too_large";
        _parts = parts;
        _data.assign(parts, std::nullopt);
        _last = now;
        return "";
    }

    std::string ImportAssembler::Add(size_t index, std::string const& data, TimePoint now)
    {
        if (!Active(now))
        {
            Reset();
            return "import_not_started";
        }
        if (index >= _parts)
            return "import_bad_part";
        if (_data[index])
            _bytes -= _data[index]->size();   // resent part replaces the old copy
        if (_bytes + data.size() > kMaxImportBytes)
        {
            Reset();
            return "import_too_large";
        }
        _bytes += data.size();
        _data[index] = data;
        _last = now;
        return "";
    }

    size_t ImportAssembler::Received() const
    {
        return size_t(std::count_if(_data.begin(), _data.end(), [](auto const& d) { return d.has_value(); }));
    }

    std::optional<Json> ImportAssembler::Finish(std::string& error, TimePoint now)
    {
        if (_parts == 0)
        {
            error = "import_not_started";
            return std::nullopt;
        }
        if (!Active(now))
        {
            Reset();
            error = "import_expired";   // parts that waited too long are not applied late
            return std::nullopt;
        }
        if (Received() != _parts)
        {
            error = "import_incomplete";
            return std::nullopt;
        }
        std::vector<std::string> pages;
        for (auto& d : _data)
            pages.push_back(*d);
        Reset();
        auto full = JoinExportPages(pages);
        if (!full)
            error = "invalid_json";
        return full;
    }

    void ImportAssembler::Reset()
    {
        _parts = 0;
        _bytes = 0;
        _data.clear();
    }

    ExportSnapshots::Page ExportSnapshots::Get(uint64_t companion, size_t part, std::string const& snapshotId,
                                               std::function<std::vector<std::string>()> const& build, TimePoint now)
    {
        for (auto it = _byCompanion.begin(); it != _byCompanion.end();)
            it = now - it->second.lastUse > kExportSnapshotTtlSec ? _byCompanion.erase(it) : std::next(it);
        Page r;
        if (part == 0)
        {
            Snap s;
            s.pages = build();
            if (s.pages.empty())
            {
                _byCompanion.erase(companion);
                r.error = "export_disabled";
                return r;
            }
            s.id = std::to_string(++_next);
            s.lastUse = now;
            _byCompanion[companion] = std::move(s);
        }
        auto it = _byCompanion.find(companion);
        if (it == _byCompanion.end() || (part > 0 && it->second.id != snapshotId))
        {
            r.error = "snapshot_expired";
            return r;
        }
        Snap& s = it->second;
        if (part >= s.pages.size())
        {
            r.error = "bad_part";
            return r;
        }
        s.lastUse = now;
        r.data = s.pages[part];
        r.parts = s.pages.size();
        r.snapshot = s.id;
        if (part + 1 == s.pages.size())
            _byCompanion.erase(it);   // the last page: done
        return r;
    }
}
