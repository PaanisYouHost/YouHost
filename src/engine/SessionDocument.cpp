#include "SessionDocument.h"
#include "MeterScale.h"
#include "SignalPath.h"
#include "X32Colours.h"

#include <algorithm>
#include <cmath>

namespace youhost
{
namespace
{

juce::XmlElement* addPlugin(juce::XmlElement& parent, const SessionSlot& slot)
{
    auto* plugin = slot.description.createXml().release();
    if (plugin == nullptr)
        return nullptr;
    parent.addChildElement(plugin);
    return plugin;
}

juce::String encodePeaks(const std::vector<WavePeak>& peaks)
{
    if (peaks.empty())
        return {};

    juce::MemoryBlock block;
    block.setSize(peaks.size() * sizeof(std::int16_t) * 2, true);
    auto* packed = static_cast<std::int16_t*>(block.getData());
    for (std::size_t index = 0; index < peaks.size(); ++index)
    {
        const auto clampSample = [](float value)
        {
            const float limited = std::max(-1.0f, std::min(1.0f, value));
            return static_cast<std::int16_t>(std::lround(limited * 32767.0f));
        };
        packed[index * 2] = clampSample(peaks[index].low);
        packed[index * 2 + 1] = clampSample(peaks[index].high);
    }
    return block.toBase64Encoding();
}

std::vector<WavePeak> decodePeaks(const juce::String& encoded)
{
    std::vector<WavePeak> peaks;
    if (encoded.isEmpty())
        return peaks;

    juce::MemoryBlock block;
    if (! block.fromBase64Encoding(encoded) || block.getSize() < sizeof(std::int16_t) * 2)
        return peaks;

    const auto count = block.getSize() / (sizeof(std::int16_t) * 2);
    const auto* packed = static_cast<const std::int16_t*>(block.getData());
    peaks.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        WavePeak peak;
        peak.low = static_cast<float>(packed[index * 2]) / 32767.0f;
        peak.high = static_cast<float>(packed[index * 2 + 1]) / 32767.0f;
        peaks.push_back(peak);
    }
    return peaks;
}

} // namespace

bool writeSessionFile(const juce::File& file, const SessionData& data)
{
    juce::XmlElement root("YouHostSession");
    root.setAttribute("version", 5);
    root.setAttribute("bits", normaliseWavBitDepth(data.wavBitDepth));
    root.setAttribute("page", data.page == 2 ? 2 : 1);
    root.setAttribute("wave", data.waveformGain);
    root.setAttribute("align", data.alignGroup == 1 ? "group" : "all");
    if (data.sampleRate > 0.0)
        root.setAttribute("rate", data.sampleRate);

    auto* meters = root.createNewChildElement("Meters");
    meters->setAttribute("peak", data.peakMeter);
    meters->setAttribute("reference", normaliseRmsReferenceDb(data.rmsReferenceDb));

    if (data.device != nullptr)
        root.createNewChildElement("Device")->addChildElement(new juce::XmlElement(*data.device));

    for (int channel = 0; channel < kMaxChannels; ++channel)
    {
        const auto& source = data.channels[static_cast<std::size_t>(channel)];
        bool anySlot = false;
        for (const auto& slot : source.slots)
            anySlot = anySlot || slot.occupied;
        const bool named = source.name.isNotEmpty();
        const bool customArm = source.listen != ChannelListen::record;
        const bool customGain = std::fabs(source.outputDb) > 0.01f;
        const bool coloured = source.color != 0;
        const bool grouped = source.group >= 0;
        if (! anySlot && ! source.excludeFromCompensation && ! named && ! customArm && ! customGain && ! coloured && ! grouped)
            continue;

        auto* element = root.createNewChildElement("Channel");
        element->setAttribute("index", channel);
        element->setAttribute("exclude", source.excludeFromCompensation);
        element->setAttribute("record", source.listen == ChannelListen::record);
        element->setAttribute("listen", channelListenName(source.listen));
        if (customGain)
            element->setAttribute("outputDb", source.outputDb);
        element->setAttribute("color", source.color);
        if (grouped)
            element->setAttribute("group", source.group);
        if (named)
            element->setAttribute("name", source.name);

        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& sourceSlot = source.slots[static_cast<std::size_t>(slot)];
            if (! sourceSlot.occupied)
                continue;

            auto* slotElement = element->createNewChildElement("Slot");
            slotElement->setAttribute("index", slot);
            slotElement->setAttribute("bypass", sourceSlot.bypassed);
            if (sourceSlot.stereoFold != 0)
                slotElement->setAttribute("fold", stereoFoldToken(stereoFoldFromInt(sourceSlot.stereoFold)));
            if (addPlugin(*slotElement, sourceSlot) == nullptr)
                continue;

            if (sourceSlot.state.getSize() > 0)
                slotElement->createNewChildElement("State")
                    ->setAttribute("data", sourceSlot.state.toBase64Encoding());
        }
    }

    for (int group = 0; group < kMaxDisplayGroups; ++group)
    {
        const auto& source = data.groups[static_cast<std::size_t>(group)];
        if (! source.used && source.name.isEmpty() && source.color == 0 && ! source.collapsed)
            continue;
        auto* element = root.createNewChildElement("Group");
        element->setAttribute("index", group);
        element->setAttribute("name", source.name);
        element->setAttribute("color", source.color);
        element->setAttribute("collapsed", source.collapsed);
    }

    for (const auto& take : data.takes)
    {
        auto* element = root.createNewChildElement("Take");
        element->setAttribute("start", juce::String(take.startSample));
        element->setAttribute("length", juce::String(take.lengthSamples));
        const auto peaks = encodePeaks(take.peaks);
        if (peaks.isNotEmpty())
            element->setAttribute("peaks", peaks);

        for (int channel = 0; channel < kMaxChannels; ++channel)
        {
            const auto& name = take.files[static_cast<std::size_t>(channel)];
            if (name.isEmpty())
                continue;
            auto* fileElement = element->createNewChildElement("File");
            fileElement->setAttribute("channel", channel);
            fileElement->setAttribute("name", name);
            const auto channelPeaks = encodePeaks(take.channelPeaks[static_cast<std::size_t>(channel)]);
            if (channelPeaks.isNotEmpty())
                fileElement->setAttribute("peaks", channelPeaks);
        }
    }

    file.getParentDirectory().createDirectory();
    return root.writeTo(file);
}

bool readSessionFile(const juce::File& file, SessionData& data)
{
    const std::unique_ptr<juce::XmlElement> root = juce::XmlDocument::parse(file);
    if (root == nullptr || ! root->hasTagName("YouHostSession"))
        return false;

    data = {};
    data.page = root->getIntAttribute("page", 1) == 2 ? 2 : 1;
    data.sampleRate = root->getDoubleAttribute("rate", 0.0);
    data.waveformGain = static_cast<float>(root->getDoubleAttribute("wave", 1.0));
    data.alignGroup = root->getStringAttribute("align") == "group" ? 1 : 0;
    data.wavBitDepth = normaliseWavBitDepth(root->getIntAttribute("bits", kDefaultWavBitDepth));
    if (auto* meters = root->getChildByName("Meters"))
    {
        data.peakMeter = meters->getBoolAttribute("peak", false);
        data.rmsReferenceDb = normaliseRmsReferenceDb(meters->getIntAttribute("reference", kDefaultRmsReferenceDb));
    }

    if (auto* device = root->getChildByName("Device"))
        if (auto* inner = device->getFirstChildElement())
            data.device = std::make_unique<juce::XmlElement>(*inner);

    for (auto* channel = root->getChildByName("Channel"); channel != nullptr; channel = channel->getNextElementWithTagName("Channel"))
    {
        const int index = channel->getIntAttribute("index", -1);
        if (index < 0 || index >= kMaxChannels)
            continue;

        auto& destination = data.channels[static_cast<std::size_t>(index)];
        destination.excludeFromCompensation = channel->getBoolAttribute("exclude", false);
        destination.outputDb = static_cast<float>(channel->getDoubleAttribute("outputDb", 0.0));
        if (channel->hasAttribute("listen"))
            destination.listen = channelListenFromName(channel->getStringAttribute("listen").toStdString());
        else
            destination.listen = channel->getBoolAttribute("record", true) ? ChannelListen::record : ChannelListen::off;
        destination.recordEnabled = destination.listen == ChannelListen::record;
        destination.color = normaliseX32Colour(channel->getIntAttribute("color", 0));
        destination.group = channel->getIntAttribute("group", -1);
        if (destination.group < 0 || destination.group >= kMaxDisplayGroups)
            destination.group = -1;
        destination.name = channel->getStringAttribute("name");

        for (auto* slot = channel->getChildByName("Slot"); slot != nullptr; slot = slot->getNextElementWithTagName("Slot"))
        {
            const int slotIndex = slot->getIntAttribute("index", -1);
            if (slotIndex < 0 || slotIndex >= kSlotsPerChannel)
                continue;

            auto* plugin = slot->getChildByName("PLUGIN");
            if (plugin == nullptr)
                continue;

            auto& destinationSlot = destination.slots[static_cast<std::size_t>(slotIndex)];
            if (! destinationSlot.description.loadFromXml(*plugin))
                continue;

            destinationSlot.occupied = true;
            destinationSlot.bypassed = slot->getBoolAttribute("bypass", false);
            destinationSlot.stereoFold = static_cast<int>(stereoFoldFromToken(slot->getStringAttribute("fold", "L").toRawUTF8()));
            if (auto* state = slot->getChildByName("State"))
                destinationSlot.state.fromBase64Encoding(state->getStringAttribute("data"));
        }
    }

    for (auto* group = root->getChildByName("Group"); group != nullptr; group = group->getNextElementWithTagName("Group"))
    {
        const int index = group->getIntAttribute("index", -1);
        if (index < 0 || index >= kMaxDisplayGroups)
            continue;
        auto& destination = data.groups[static_cast<std::size_t>(index)];
        destination.used = true;
        destination.name = group->getStringAttribute("name");
        destination.color = normaliseX32Colour(group->getIntAttribute("color", 0));
        destination.collapsed = group->getBoolAttribute("collapsed", false);
    }

    for (auto* take = root->getChildByName("Take"); take != nullptr; take = take->getNextElementWithTagName("Take"))
    {
        SessionTake stored;
        stored.startSample = take->getStringAttribute("start").getLargeIntValue();
        stored.lengthSamples = take->getStringAttribute("length").getLargeIntValue();
        stored.peaks = decodePeaks(take->getStringAttribute("peaks"));
        for (auto* fileElement = take->getChildByName("File"); fileElement != nullptr; fileElement = fileElement->getNextElementWithTagName("File"))
        {
            const int channel = fileElement->getIntAttribute("channel", -1);
            if (channel < 0 || channel >= kMaxChannels)
                continue;
            stored.files[static_cast<std::size_t>(channel)] = fileElement->getStringAttribute("name");
            stored.channelPeaks[static_cast<std::size_t>(channel)] = decodePeaks(fileElement->getStringAttribute("peaks"));
        }
        if (stored.lengthSamples > 0)
            data.takes.push_back(std::move(stored));
    }

    return true;
}

} // namespace youhost
