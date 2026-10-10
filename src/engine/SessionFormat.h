#pragma once

#include "ChannelListen.h"
#include "DisplayLayout.h"
#include "HostLimits.h"
#include "MeterScale.h"
#include "SessionChannels.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace youhost
{

// Session files are XML. The version attribute is informational.
// A newer YouHost opens any older file and fills missing fields with defaults.
// An older YouHost opens a newer file by ignoring attributes and elements it
// does not know. It must not fail, and a save must keep those unknown fields.
inline constexpr int kSessionFormatVersion = 7;

struct SessionTimelineState
{
    int zoom = 0;
    int vertical = 0;
    std::int64_t scroll = 0;
    int laneScroll = 0;
    int height = 0;
};

struct SessionNode
{
    std::string name;
    std::string text;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<SessionNode> children;
};

inline const std::string* sessionAttribute(const SessionNode& node, std::string_view key)
{
    for (const auto& attribute : node.attributes)
        if (attribute.first == key)
            return &attribute.second;
    return nullptr;
}

inline bool sessionHasAttribute(const SessionNode& node, std::string_view key)
{
    return sessionAttribute(node, key) != nullptr;
}

inline int sessionAttributeInt(const SessionNode& node, const char* key, int fallback)
{
    const auto* value = sessionAttribute(node, key);
    if (value == nullptr || value->empty())
        return fallback;
    char* end = nullptr;
    const long parsed = std::strtol(value->c_str(), &end, 10);
    if (end == value->c_str())
        return fallback;
    return static_cast<int>(parsed);
}

inline std::int64_t sessionAttributeInt64(const SessionNode& node, const char* key, std::int64_t fallback)
{
    const auto* value = sessionAttribute(node, key);
    if (value == nullptr || value->empty())
        return fallback;
    char* end = nullptr;
    const long long parsed = std::strtoll(value->c_str(), &end, 10);
    if (end == value->c_str())
        return fallback;
    return static_cast<std::int64_t>(parsed);
}

inline double sessionAttributeDouble(const SessionNode& node, const char* key, double fallback)
{
    const auto* value = sessionAttribute(node, key);
    if (value == nullptr || value->empty())
        return fallback;
    char* end = nullptr;
    const double parsed = std::strtod(value->c_str(), &end);
    if (end == value->c_str())
        return fallback;
    return parsed;
}

inline bool sessionAttributeBool(const SessionNode& node, const char* key, bool fallback)
{
    const auto* value = sessionAttribute(node, key);
    if (value == nullptr)
        return fallback;
    if (*value == "1" || *value == "true" || *value == "True" || *value == "yes")
        return true;
    if (*value == "0" || *value == "false" || *value == "False" || *value == "no")
        return false;
    return fallback;
}

inline const SessionNode* sessionChild(const SessionNode& node, std::string_view name)
{
    for (const auto& child : node.children)
        if (child.name == name)
            return &child;
    return nullptr;
}

inline SessionTimelineState timelineFromNode(const SessionNode& root)
{
    SessionTimelineState state;
    const auto* timeline = sessionChild(root, "Timeline");
    if (timeline == nullptr)
        return state;
    state.zoom = std::max(0, sessionAttributeInt(*timeline, "zoom", 0));
    state.vertical = std::max(0, sessionAttributeInt(*timeline, "vertical", 0));
    state.scroll = std::max<std::int64_t>(0, sessionAttributeInt64(*timeline, "scroll", 0));
    state.laneScroll = std::max(0, sessionAttributeInt(*timeline, "lanes", 0));
    state.height = std::max(0, sessionAttributeInt(*timeline, "height", 0));
    return state;
}

inline bool knownSessionAttribute(std::string_view element, std::string_view attribute) noexcept
{
    auto listed = [&](std::string_view name, std::initializer_list<std::string_view> keys)
    {
        if (element != name)
            return false;
        for (const auto key : keys)
            if (key == attribute)
                return true;
        return false;
    };

    if (listed("YouHostSession", { "version", "bits", "page", "wave", "align", "rate" }))
        return true;
    if (listed("Meters", { "peak", "reference" }))
        return true;
    if (listed("Channel", { "index", "exclude", "record", "listen", "outputDb", "color", "group", "name" }))
        return true;
    if (listed("Slot", { "index", "bypass", "fold" }))
        return true;
    if (listed("Group", { "index", "name", "color", "collapsed" }))
        return true;
    if (listed("Take", { "start", "length", "peaks" }))
        return true;
    if (listed("File", { "channel", "name", "peaks" }))
        return true;
    if (listed("Timeline", { "zoom", "vertical", "scroll", "lanes", "height" }))
        return true;
    if (listed("State", { "data" }))
        return true;
    return false;
}

inline bool knownSessionTag(std::string_view name) noexcept
{
    return name == "YouHostSession" || name == "Meters" || name == "Device" || name == "Channel"
           || name == "Group" || name == "Take" || name == "Slot" || name == "File" || name == "PLUGIN"
           || name == "State" || name == "Timeline";
}

inline int sessionMatchIndex(const SessionNode& written, const SessionNode& original, int takeOrdinal)
{
    int seenTakes = 0;
    for (int index = 0; index < static_cast<int>(written.children.size()); ++index)
    {
        const auto& child = written.children[static_cast<std::size_t>(index)];
        if (child.name != original.name)
            continue;
        if (original.name == "Take")
        {
            if (seenTakes == takeOrdinal)
                return index;
            ++seenTakes;
            continue;
        }
        if (original.name == "PLUGIN" || original.name == "State" || original.name == "Meters"
            || original.name == "Device" || original.name == "Timeline")
            return index;

        const char* key = original.name == "File" ? "channel" : "index";
        const auto* left = sessionAttribute(child, key);
        const auto* right = sessionAttribute(original, key);
        if (left != nullptr && right != nullptr && *left == *right)
            return index;
    }
    return -1;
}

// Copy unknown attributes onto `written`, and append unknown child elements.
// Known fields stay as `written` left them, including a known field that was
// omitted because it is back at its default.
inline void mergeSessionNodes(const SessionNode& original, SessionNode& written)
{
    for (const auto& attribute : original.attributes)
    {
        if (knownSessionAttribute(written.name, attribute.first))
            continue;
        if (! sessionHasAttribute(written, attribute.first))
            written.attributes.push_back(attribute);
    }
    if (written.text.empty())
        written.text = original.text;

    const bool deviceElement = written.name == "Device";
    std::vector<SessionNode> extras;
    int takeOrdinal = 0;
    for (const auto& child : original.children)
    {
        if (child.name.empty())
        {
            if (! deviceElement)
                extras.push_back(child);
            continue;
        }

        const int ordinal = child.name == "Take" ? takeOrdinal++ : 0;
        if (knownSessionTag(child.name))
        {
            const int match = sessionMatchIndex(written, child, ordinal);
            if (match >= 0)
                mergeSessionNodes(child, written.children[static_cast<std::size_t>(match)]);
            continue;
        }

        if (deviceElement)
        {
            bool alreadyWritten = false;
            for (const auto& existing : written.children)
                if (existing.name == child.name)
                    alreadyWritten = true;
            if (alreadyWritten)
                continue;
        }
        extras.push_back(child);
    }
    for (auto& extra : extras)
        written.children.push_back(std::move(extra));
}

struct SessionSlotRecord
{
    int index = 0;
    bool bypassed = false;
    std::string fold = "L";
    std::string pluginName;
    std::string state;
    bool occupied = false;
};

struct SessionChannelRecord
{
    int index = 0;
    std::string name;
    int group = -1;
    std::string listen = "rec";
    bool exclude = false;
    int color = 0;
    double outputDb = 0.0;
    std::vector<SessionSlotRecord> slots;
};

struct SessionGroupRecord
{
    int index = 0;
    std::string name;
    int color = 0;
    bool collapsed = false;
};

struct SessionFileRecord
{
    int channel = 0;
    std::string name;
};

struct SessionTakeRecord
{
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::vector<SessionFileRecord> files;
};

struct SessionDocumentModel
{
    int version = 0;
    int bits = kDefaultWavBitDepth;
    int page = 1;
    double wave = 1.0;
    std::string align = "all";
    double rate = 0.0;
    int buffer = 0;
    int cardChannels = 0;
    std::string inputDevice;
    std::string outputDevice;
    bool explicitOffline = false;
    int channelCount = kMaxChannels;
    bool peak = false;
    int reference = kDefaultRmsReferenceDb;
    bool hasMeters = false;
    SessionTimelineState timeline {};
    bool hasTimeline = false;
    std::vector<SessionChannelRecord> channels;
    std::vector<SessionGroupRecord> groups;
    std::vector<SessionTakeRecord> takes;
    SessionNode source;
    bool hasSource = false;
};

inline bool readSessionModel(const SessionNode& root, SessionDocumentModel& model, bool readTimeline)
{
    if (root.name != "YouHostSession")
        return false;

    model = {};
    model.hasSource = true;
    model.source = root;
    model.version = std::max(0, sessionAttributeInt(root, "version", 0));
    model.bits = normaliseWavBitDepth(sessionAttributeInt(root, "bits", kDefaultWavBitDepth));
    model.page = sessionAttributeInt(root, "page", 1) == 2 ? 2 : 1;
    model.wave = sessionAttributeDouble(root, "wave", 1.0);
    if (const auto* align = sessionAttribute(root, "align"))
        model.align = *align == "group" ? "group" : "all";
    model.rate = sessionAttributeDouble(root, "rate", 0.0);
    model.buffer = sessionAttributeInt(root, "buffer", 0);
    model.cardChannels = sessionAttributeInt(root, "cardChannels", 0);
    if (const auto* input = sessionAttribute(root, "input"))
        model.inputDevice = *input;
    if (const auto* output = sessionAttribute(root, "output"))
        model.outputDevice = *output;
    model.explicitOffline = sessionAttributeBool(root, "offline", false);
    // A stored channels attribute is ignored. The session always holds 128.
    model.channelCount = kMaxChannels;

    if (const auto* meters = sessionChild(root, "Meters"))
    {
        model.hasMeters = true;
        model.peak = sessionAttributeBool(*meters, "peak", false);
        model.reference = normaliseRmsReferenceDb(sessionAttributeInt(*meters, "reference", kDefaultRmsReferenceDb));
    }

    if (readTimeline && sessionChild(root, "Timeline") != nullptr)
    {
        model.hasTimeline = true;
        model.timeline = timelineFromNode(root);
    }

    for (const auto& channel : root.children)
    {
        if (channel.name != "Channel")
            continue;
        const int index = sessionAttributeInt(channel, "index", -1);
        if (index < 0 || index >= kMaxChannels)
            continue;

        SessionChannelRecord record;
        record.index = index;
        if (const auto* name = sessionAttribute(channel, "name"))
            record.name = *name;
        record.group = sessionAttributeInt(channel, "group", -1);
        if (record.group < 0 || record.group >= kMaxDisplayGroups)
            record.group = -1;
        record.exclude = sessionAttributeBool(channel, "exclude", false);
        record.color = sessionAttributeInt(channel, "color", 0);
        record.outputDb = sessionAttributeDouble(channel, "outputDb", 0.0);
        if (const auto* listen = sessionAttribute(channel, "listen"))
            record.listen = channelListenName(channelListenFromName(*listen));
        else if (sessionHasAttribute(channel, "record") && ! sessionAttributeBool(channel, "record", true))
            record.listen = "off";
        else
            record.listen = "rec";

        for (const auto& slot : channel.children)
        {
            if (slot.name != "Slot")
                continue;
            const auto* plugin = sessionChild(slot, "PLUGIN");
            if (plugin == nullptr)
                continue;
            const int slotIndex = sessionAttributeInt(slot, "index", -1);
            if (slotIndex < 0 || slotIndex >= kSlotsPerChannel)
                continue;

            SessionSlotRecord stored;
            stored.occupied = true;
            stored.index = slotIndex;
            stored.bypassed = sessionAttributeBool(slot, "bypass", false);
            if (const auto* fold = sessionAttribute(slot, "fold"))
                stored.fold = *fold;
            if (const auto* pluginName = sessionAttribute(*plugin, "name"))
                stored.pluginName = *pluginName;
            if (const auto* state = sessionChild(slot, "State"))
                if (const auto* data = sessionAttribute(*state, "data"))
                    stored.state = *data;
            record.slots.push_back(std::move(stored));
        }
        model.channels.push_back(std::move(record));
    }

    for (const auto& group : root.children)
    {
        if (group.name != "Group")
            continue;
        const int index = sessionAttributeInt(group, "index", -1);
        if (index < 0 || index >= kMaxDisplayGroups)
            continue;
        SessionGroupRecord record;
        record.index = index;
        if (const auto* name = sessionAttribute(group, "name"))
            record.name = *name;
        record.color = sessionAttributeInt(group, "color", 0);
        record.collapsed = sessionAttributeBool(group, "collapsed", false);
        model.groups.push_back(record);
    }

    for (const auto& take : root.children)
    {
        if (take.name != "Take")
            continue;
        SessionTakeRecord record;
        record.start = sessionAttributeInt64(take, "start", 0);
        record.length = sessionAttributeInt64(take, "length", 0);
        if (record.length <= 0)
            continue;
        for (const auto& file : take.children)
        {
            if (file.name != "File")
                continue;
            const int channel = sessionAttributeInt(file, "channel", -1);
            if (channel < 0 || channel >= kMaxChannels)
                continue;
            SessionFileRecord stored;
            stored.channel = channel;
            if (const auto* name = sessionAttribute(file, "name"))
                stored.name = *name;
            record.files.push_back(std::move(stored));
        }
        model.takes.push_back(std::move(record));
    }

    return true;
}

inline bool readSessionModel(const SessionNode& root, SessionDocumentModel& model)
{
    return readSessionModel(root, model, true);
}

// What an older YouHost does: the same core fields, and every unknown element
// is skipped. A newer version number is not an error.
inline bool readSessionModelLegacy(const SessionNode& root, SessionDocumentModel& model)
{
    return readSessionModel(root, model, false);
}

inline void sessionSetAttribute(SessionNode& node, std::string key, std::string value)
{
    for (auto& attribute : node.attributes)
    {
        if (attribute.first == key)
        {
            attribute.second = std::move(value);
            return;
        }
    }
    node.attributes.emplace_back(std::move(key), std::move(value));
}

inline SessionNode writeSessionModel(const SessionDocumentModel& model)
{
    SessionNode root;
    root.name = "YouHostSession";
    sessionSetAttribute(root, "version", std::to_string(kSessionFormatVersion));
    sessionSetAttribute(root, "bits", std::to_string(normaliseWavBitDepth(model.bits)));
    sessionSetAttribute(root, "page", model.page == 2 ? "2" : "1");
    sessionSetAttribute(root, "wave", std::to_string(model.wave));
    sessionSetAttribute(root, "align", model.align == "group" ? "group" : "all");
    if (model.rate > 0.0)
        sessionSetAttribute(root, "rate", std::to_string(model.rate));
    if (model.buffer >= 16)
        sessionSetAttribute(root, "buffer", std::to_string(model.buffer));
    if (model.cardChannels > 0)
        sessionSetAttribute(root, "cardChannels", std::to_string(model.cardChannels));
    if (! model.inputDevice.empty())
        sessionSetAttribute(root, "input", model.inputDevice);
    if (! model.outputDevice.empty())
        sessionSetAttribute(root, "output", model.outputDevice);
    if (model.explicitOffline)
        sessionSetAttribute(root, "offline", "1");

    SessionNode meters;
    meters.name = "Meters";
    sessionSetAttribute(meters, "peak", model.peak ? "1" : "0");
    sessionSetAttribute(meters, "reference", std::to_string(normaliseRmsReferenceDb(model.reference)));
    root.children.push_back(std::move(meters));

    for (const auto& channel : model.channels)
    {
        if (channel.index < 0 || channel.index >= kMaxChannels)
            continue;
        SessionNode element;
        element.name = "Channel";
        sessionSetAttribute(element, "index", std::to_string(channel.index));
        sessionSetAttribute(element, "exclude", channel.exclude ? "1" : "0");
        const auto listen = channelListenFromName(channel.listen);
        sessionSetAttribute(element, "record", listen == ChannelListen::record ? "1" : "0");
        sessionSetAttribute(element, "listen", channelListenName(listen));
        if (std::abs(channel.outputDb) > 0.01)
            sessionSetAttribute(element, "outputDb", std::to_string(channel.outputDb));
        sessionSetAttribute(element, "color", std::to_string(channel.color));
        if (channel.group >= 0)
            sessionSetAttribute(element, "group", std::to_string(channel.group));
        if (! channel.name.empty())
            sessionSetAttribute(element, "name", channel.name);

        for (const auto& slot : channel.slots)
        {
            if (! slot.occupied || slot.index < 0 || slot.index >= kSlotsPerChannel)
                continue;
            SessionNode slotNode;
            slotNode.name = "Slot";
            sessionSetAttribute(slotNode, "index", std::to_string(slot.index));
            sessionSetAttribute(slotNode, "bypass", slot.bypassed ? "1" : "0");
            if (! slot.fold.empty() && slot.fold != "L")
                sessionSetAttribute(slotNode, "fold", slot.fold);
            SessionNode plugin;
            plugin.name = "PLUGIN";
            sessionSetAttribute(plugin, "name", slot.pluginName);
            slotNode.children.push_back(std::move(plugin));
            if (! slot.state.empty())
            {
                SessionNode state;
                state.name = "State";
                sessionSetAttribute(state, "data", slot.state);
                slotNode.children.push_back(std::move(state));
            }
            element.children.push_back(std::move(slotNode));
        }
        root.children.push_back(std::move(element));
    }

    for (const auto& group : model.groups)
    {
        SessionNode element;
        element.name = "Group";
        sessionSetAttribute(element, "index", std::to_string(group.index));
        sessionSetAttribute(element, "name", group.name);
        sessionSetAttribute(element, "color", std::to_string(group.color));
        sessionSetAttribute(element, "collapsed", group.collapsed ? "1" : "0");
        root.children.push_back(std::move(element));
    }

    for (const auto& take : model.takes)
    {
        if (take.length <= 0)
            continue;
        SessionNode element;
        element.name = "Take";
        sessionSetAttribute(element, "start", std::to_string(take.start));
        sessionSetAttribute(element, "length", std::to_string(take.length));
        for (const auto& file : take.files)
        {
            if (file.name.empty())
                continue;
            SessionNode fileNode;
            fileNode.name = "File";
            sessionSetAttribute(fileNode, "channel", std::to_string(file.channel));
            sessionSetAttribute(fileNode, "name", file.name);
            element.children.push_back(std::move(fileNode));
        }
        root.children.push_back(std::move(element));
    }

    SessionNode timeline;
    timeline.name = "Timeline";
    sessionSetAttribute(timeline, "zoom", std::to_string(std::max(0, model.timeline.zoom)));
    sessionSetAttribute(timeline, "vertical", std::to_string(std::max(0, model.timeline.vertical)));
    sessionSetAttribute(timeline, "scroll", std::to_string(std::max<std::int64_t>(0, model.timeline.scroll)));
    sessionSetAttribute(timeline, "lanes", std::to_string(std::max(0, model.timeline.laneScroll)));
    sessionSetAttribute(timeline, "height", std::to_string(std::max(0, model.timeline.height)));
    root.children.push_back(std::move(timeline));

    if (model.hasSource)
        mergeSessionNodes(model.source, root);
    return root;
}

inline void sessionAppendEscaped(std::string& out, std::string_view text)
{
    for (const char character : text)
    {
        if (character == '&')
            out += "&amp;";
        else if (character == '<')
            out += "&lt;";
        else if (character == '>')
            out += "&gt;";
        else if (character == '"')
            out += "&quot;";
        else
            out.push_back(character);
    }
}

inline void sessionWriteNode(std::string& out, const SessionNode& node)
{
    if (node.name.empty())
    {
        sessionAppendEscaped(out, node.text);
        return;
    }

    out.push_back('<');
    out += node.name;
    for (const auto& attribute : node.attributes)
    {
        out.push_back(' ');
        out += attribute.first;
        out += "=\"";
        sessionAppendEscaped(out, attribute.second);
        out.push_back('"');
    }
    if (node.children.empty() && node.text.empty())
    {
        out += "/>";
        return;
    }
    out.push_back('>');
    sessionAppendEscaped(out, node.text);
    for (const auto& child : node.children)
        sessionWriteNode(out, child);
    out += "</";
    out += node.name;
    out.push_back('>');
}

inline std::string writeSessionXml(const SessionNode& root)
{
    std::string out;
    sessionWriteNode(out, root);
    return out;
}

namespace session_xml
{

struct Parser
{
    std::string_view in;
    std::size_t index = 0;
    bool ok = true;

    bool empty() const
    {
        return index >= in.size();
    }

    void skipSpace()
    {
        while (! empty() && std::isspace(static_cast<unsigned char>(in[index])) != 0)
            ++index;
    }

    bool startsWith(std::string_view token) const
    {
        return in.substr(index).starts_with(token);
    }
};

inline std::string decodeEntities(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] != '&')
        {
            out.push_back(text[index]);
            continue;
        }
        const auto rest = text.substr(index);
        if (rest.starts_with("&amp;"))
        {
            out.push_back('&');
            index += 4;
        }
        else if (rest.starts_with("&lt;"))
        {
            out.push_back('<');
            index += 3;
        }
        else if (rest.starts_with("&gt;"))
        {
            out.push_back('>');
            index += 3;
        }
        else if (rest.starts_with("&quot;"))
        {
            out.push_back('"');
            index += 5;
        }
        else if (rest.starts_with("&apos;"))
        {
            out.push_back('\'');
            index += 5;
        }
        else
            out.push_back('&');
    }
    return out;
}

inline void skipNoise(Parser& parser)
{
    for (;;)
    {
        parser.skipSpace();
        if (parser.startsWith("<?"))
        {
            const auto end = parser.in.find("?>", parser.index);
            if (end == std::string_view::npos)
            {
                parser.ok = false;
                return;
            }
            parser.index = end + 2;
            continue;
        }
        if (parser.startsWith("<!--"))
        {
            const auto end = parser.in.find("-->", parser.index);
            if (end == std::string_view::npos)
            {
                parser.ok = false;
                return;
            }
            parser.index = end + 3;
            continue;
        }
        if (parser.startsWith("<!"))
        {
            const auto end = parser.in.find('>', parser.index);
            if (end == std::string_view::npos)
            {
                parser.ok = false;
                return;
            }
            parser.index = end + 1;
            continue;
        }
        return;
    }
}

inline bool readName(Parser& parser, std::string& name)
{
    if (parser.empty())
        return false;
    const auto start = parser.index;
    const auto first = static_cast<unsigned char>(parser.in[parser.index]);
    if (std::isalpha(first) == 0 && parser.in[parser.index] != '_')
        return false;
    ++parser.index;
    while (! parser.empty())
    {
        const auto character = static_cast<unsigned char>(parser.in[parser.index]);
        if (std::isalnum(character) == 0 && parser.in[parser.index] != '_' && parser.in[parser.index] != '-'
            && parser.in[parser.index] != ':' && parser.in[parser.index] != '.')
            break;
        ++parser.index;
    }
    name.assign(parser.in.substr(start, parser.index - start));
    return true;
}

inline bool parseNode(Parser& parser, SessionNode& node);

inline bool parseChildren(Parser& parser, SessionNode& node, const std::string& name)
{
    std::string text;
    while (parser.ok && ! parser.empty())
    {
        if (parser.startsWith("</"))
        {
            parser.index += 2;
            std::string endName;
            if (! readName(parser, endName) || endName != name)
            {
                parser.ok = false;
                return false;
            }
            parser.skipSpace();
            if (parser.empty() || parser.in[parser.index] != '>')
            {
                parser.ok = false;
                return false;
            }
            ++parser.index;
            node.text = decodeEntities(text);
            return true;
        }
        if (parser.startsWith("<"))
        {
            if (! text.empty())
            {
                SessionNode chunk;
                chunk.text = decodeEntities(text);
                node.children.push_back(std::move(chunk));
                text.clear();
            }
            SessionNode child;
            if (! parseNode(parser, child))
                return false;
            node.children.push_back(std::move(child));
            continue;
        }
        text.push_back(parser.in[parser.index]);
        ++parser.index;
    }
    parser.ok = false;
    return false;
}

inline bool parseNode(Parser& parser, SessionNode& node)
{
    skipNoise(parser);
    if (! parser.ok || parser.empty() || parser.in[parser.index] != '<')
    {
        parser.ok = false;
        return false;
    }
    ++parser.index;
    if (! readName(parser, node.name))
    {
        parser.ok = false;
        return false;
    }

    for (;;)
    {
        parser.skipSpace();
        if (parser.empty())
        {
            parser.ok = false;
            return false;
        }
        if (parser.startsWith("/>"))
        {
            parser.index += 2;
            return true;
        }
        if (parser.in[parser.index] == '>')
        {
            ++parser.index;
            return parseChildren(parser, node, node.name);
        }
        std::string key;
        if (! readName(parser, key))
        {
            parser.ok = false;
            return false;
        }
        parser.skipSpace();
        if (parser.empty() || parser.in[parser.index] != '=')
        {
            parser.ok = false;
            return false;
        }
        ++parser.index;
        parser.skipSpace();
        if (parser.empty() || (parser.in[parser.index] != '"' && parser.in[parser.index] != '\''))
        {
            parser.ok = false;
            return false;
        }
        const char quote = parser.in[parser.index];
        ++parser.index;
        const auto start = parser.index;
        const auto end = parser.in.find(quote, parser.index);
        if (end == std::string_view::npos)
        {
            parser.ok = false;
            return false;
        }
        node.attributes.emplace_back(std::move(key), decodeEntities(parser.in.substr(start, end - start)));
        parser.index = end + 1;
    }
}

} // namespace session_xml

inline bool parseSessionXml(std::string_view text, SessionNode& root)
{
    session_xml::Parser parser;
    parser.in = text;
    root = {};
    if (! session_xml::parseNode(parser, root))
        return false;
    session_xml::skipNoise(parser);
    return parser.ok && parser.empty();
}

} // namespace youhost
