#include "SessionDocument.h"
#include "MeterScale.h"

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

} // namespace

bool writeSessionFile(const juce::File& file, const SessionData& data)
{
    juce::XmlElement root("YouHostSession");
    root.setAttribute("version", 1);

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
        if (! anySlot && ! source.excludeFromCompensation)
            continue;

        auto* element = root.createNewChildElement("Channel");
        element->setAttribute("index", channel);
        element->setAttribute("exclude", source.excludeFromCompensation);

        for (int slot = 0; slot < kSlotsPerChannel; ++slot)
        {
            const auto& sourceSlot = source.slots[static_cast<std::size_t>(slot)];
            if (! sourceSlot.occupied)
                continue;

            auto* slotElement = element->createNewChildElement("Slot");
            slotElement->setAttribute("index", slot);
            slotElement->setAttribute("bypass", sourceSlot.bypassed);
            if (addPlugin(*slotElement, sourceSlot) == nullptr)
                continue;

            if (sourceSlot.state.getSize() > 0)
                slotElement->createNewChildElement("State")
                    ->setAttribute("data", sourceSlot.state.toBase64Encoding());
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
            if (auto* state = slot->getChildByName("State"))
                destinationSlot.state.fromBase64Encoding(state->getStringAttribute("data"));
        }
    }

    return true;
}

} // namespace youhost
