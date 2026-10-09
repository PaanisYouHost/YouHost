#pragma once

#include "ScanJobs.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

struct CatalogPlugin
{
    std::string name;
    std::string manufacturer;
    std::string format;
};

struct InsertPluginRow
{
    int source = -1;
    std::string label;
};

struct InsertGroup
{
    std::string manufacturer;
    std::vector<InsertPluginRow> plugins;
};

enum class InsertFormatFilter
{
    all,
    audioUnit,
    vst3
};

inline constexpr std::string_view kInstrumentsHiddenNote = "Instruments and generators are hidden.";

inline bool textIsBlank(std::string_view text)
{
    for (char character : text)
    {
        const auto byte = static_cast<unsigned char>(character);
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r')
            return false;
    }
    return true;
}

inline std::string shortPluginFormat(std::string_view format)
{
    if (containsFold(format, "audiounit") || equalsFold(format, "au"))
        return "AU";
    if (containsFold(format, "vst3"))
        return "VST3";
    return std::string(format);
}

inline bool matchesInsertFormat(std::string_view format, InsertFormatFilter filter)
{
    if (filter == InsertFormatFilter::all)
        return true;
    const auto shortName = shortPluginFormat(format);
    if (filter == InsertFormatFilter::audioUnit)
        return shortName == "AU";
    return shortName == "VST3";
}

inline std::string foldedKey(std::string_view text)
{
    std::string folded;
    folded.reserve(text.size());
    for (char character : text)
        folded.push_back(foldChar(static_cast<unsigned char>(character)));
    return folded;
}

inline bool queryTokensMatch(std::string_view haystack, std::string_view query)
{
    std::size_t index = 0;
    bool any = false;
    while (index < query.size())
    {
        while (index < query.size() && (query[index] == ' ' || query[index] == '\t'))
            ++index;
        if (index >= query.size())
            break;
        const std::size_t start = index;
        while (index < query.size() && query[index] != ' ' && query[index] != '\t')
            ++index;
        any = true;
        if (! containsFold(haystack, query.substr(start, index - start)))
            return false;
    }
    return any || textIsBlank(query);
}

inline bool samePluginIdentity(const CatalogPlugin& left, const CatalogPlugin& right)
{
    const auto leftMaker = textIsBlank(left.manufacturer) ? std::string_view("unknown") : std::string_view(left.manufacturer);
    const auto rightMaker = textIsBlank(right.manufacturer) ? std::string_view("unknown") : std::string_view(right.manufacturer);
    return equalsFold(leftMaker, rightMaker) && equalsFold(left.name, right.name);
}

inline bool pluginShowsFormat(const std::vector<CatalogPlugin>& plugins, std::size_t index)
{
    if (index >= plugins.size())
        return false;
    const auto& plugin = plugins[index];
    const auto format = shortPluginFormat(plugin.format);
    for (std::size_t other = 0; other < plugins.size(); ++other)
    {
        if (other == index || ! samePluginIdentity(plugin, plugins[other]))
            continue;
        if (shortPluginFormat(plugins[other].format) != format)
            return true;
    }
    return false;
}

// Manufacturers and plugins are alphabetical. When the full catalogue has the same
// plugin as both AU and VST3, each row is tagged "(AU)" or "(VST3)". An empty query
// returns every plugin that matches the format filter. Each word of a query must
// appear in the maker, name, or format.
inline std::vector<InsertGroup> groupInsertPlugins(const std::vector<CatalogPlugin>& plugins,
                                                   std::string_view query,
                                                   InsertFormatFilter formatFilter = InsertFormatFilter::all)
{
    struct Pending
    {
        std::string manufacturer;
        std::string sortKey;
        std::vector<int> sources;
    };

    std::vector<Pending> pending;
    for (int index = 0; index < static_cast<int>(plugins.size()); ++index)
    {
        const auto& plugin = plugins[static_cast<std::size_t>(index)];
        if (! matchesInsertFormat(plugin.format, formatFilter))
            continue;
        const std::string haystack = plugin.manufacturer + " " + plugin.name + " " + plugin.format;
        if (! textIsBlank(query) && ! queryTokensMatch(haystack, query))
            continue;

        const bool unknown = textIsBlank(plugin.manufacturer);
        const std::string manufacturer = unknown ? "Unknown" : plugin.manufacturer;
        const std::string sortKey = foldedKey(manufacturer);
        Pending* group = nullptr;
        for (auto& candidate : pending)
        {
            if (candidate.sortKey == sortKey)
            {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr)
        {
            pending.push_back(Pending { manufacturer, sortKey, {} });
            group = &pending.back();
        }
        group->sources.push_back(index);
    }

    std::sort(pending.begin(), pending.end(), [](const Pending& left, const Pending& right)
    {
        return left.sortKey < right.sortKey;
    });

    std::vector<InsertGroup> groups;
    groups.reserve(pending.size());
    for (auto& group : pending)
    {
        std::sort(group.sources.begin(), group.sources.end(), [&plugins](int left, int right)
        {
            const auto& a = plugins[static_cast<std::size_t>(left)];
            const auto& b = plugins[static_cast<std::size_t>(right)];
            const auto nameOrder = foldedKey(a.name).compare(foldedKey(b.name));
            if (nameOrder != 0)
                return nameOrder < 0;
            const auto formatOrder = foldedKey(shortPluginFormat(a.format)).compare(foldedKey(shortPluginFormat(b.format)));
            if (formatOrder != 0)
                return formatOrder < 0;
            return left < right;
        });

        InsertGroup built;
        built.manufacturer = group.manufacturer;
        for (int source : group.sources)
        {
            const auto& plugin = plugins[static_cast<std::size_t>(source)];
            InsertPluginRow row;
            row.source = source;
            row.label = plugin.name;
            if (pluginShowsFormat(plugins, static_cast<std::size_t>(source)))
                row.label += " (" + shortPluginFormat(plugin.format) + ")";
            built.plugins.push_back(std::move(row));
        }
        groups.push_back(std::move(built));
    }
    return groups;
}

} // namespace youhost
