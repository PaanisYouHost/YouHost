#include "ChannelMenu.h"
#include "X32Look.h"

namespace youhost
{
namespace
{

void renameWithPrompt(const juce::String& title, const juce::String& message, const juce::String& current, std::function<void(juce::String)> apply)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::QuestionIcon);
    window->addTextEditor("name", current, "Name");
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create([window, apply = std::move(apply)](int result)
                            {
                                if (result == 1 && apply != nullptr)
                                    apply(window->getTextEditorContents("name"));
                            }),
                            true);
}

juce::PopupMenu colorMenu(int ticked)
{
    juce::PopupMenu menu;
    for (int index = 0; index < kX32ColourCount; ++index)
    {
        juce::PopupMenu::Item item;
        item.itemID = 200 + index;
        item.text = kX32Colours[index].name;
        item.colour = x32Fill(index);
        item.isTicked = index == ticked;
        if (x32Fill(index).getPerceivedBrightness() < 0.08f)
            item.colour = juce::Colour(0xff9aa3b5);
        menu.addItem(item);
    }
    return menu;
}

int sharedColor(const AudioEngine& engine, const std::vector<int>& channels)
{
    if (channels.empty())
        return -1;
    const int first = engine.channelColor(channels.front());
    for (int channel : channels)
        if (engine.channelColor(channel) != first)
            return -1;
    return first;
}

int sharedGroup(const AudioEngine& engine, const std::vector<int>& channels)
{
    if (channels.empty())
        return -2;
    const int first = engine.channelGroup(channels.front());
    for (int channel : channels)
        if (engine.channelGroup(channel) != first)
            return -2;
    return first;
}

} // namespace

void showChannelMenu(AudioEngine& engine,
                     juce::Component& target,
                     int channel,
                     std::function<void(int)> beginRename)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    if (! engine.isChannelSelected(channel))
        engine.selectChannel(channel, false);

    const auto channels = engine.selectedChannels();
    const int oneColor = channels.size() == 1 ? sharedColor(engine, channels) : sharedColor(engine, channels);
    const int oneGroup = sharedGroup(engine, channels);

    juce::PopupMenu menu;
    if (channels.size() == 1)
        menu.addItem(300, "Rename channel");
    else
        menu.addItem(0, juce::String(channels.size()) + " channels", false, false);

    menu.addSubMenu("Color", colorMenu(oneColor));

    juce::PopupMenu groups;
    groups.addItem(1, "No group", true, oneGroup == -1);
    groups.addSeparator();
    for (int group = 0; group < kMaxDisplayGroups; ++group)
        groups.addItem(20 + group, engine.groupName(group), true, oneGroup == group);
    menu.addSubMenu("Group", groups);

    bool allSafe = ! channels.empty();
    for (int chosen : channels)
        allSafe = allSafe && engine.channelSafe(chosen);
    menu.addItem(4, "Scene safe", true, allSafe);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [&engine, channels, allSafe, beginRename = std::move(beginRename)](int result)
                       {
                           if (result == 300 && channels.size() == 1)
                           {
                               const int chosen = channels.front();
                               if (beginRename != nullptr)
                                   beginRename(chosen);
                               else
                                   renameWithPrompt("Rename channel " + juce::String(chosen + 1),
                                                    "The name is shown on the channel and used in the WAV file name.",
                                                    engine.channelName(chosen),
                                                    [&engine, chosen](juce::String name) { engine.setChannelName(chosen, name); });
                           }
                           else if (result == 1)
                           {
                               engine.assignChannelsToGroup(channels, -1);
                           }
                           else if (result >= 20 && result < 20 + kMaxDisplayGroups)
                           {
                               engine.assignChannelsToGroup(channels, result - 20);
                           }
                           else if (result == 4)
                           {
                               const bool next = ! allSafe;
                               for (int chosen : channels)
                                   engine.setChannelSafe(chosen, next);
                           }
                           else if (result >= 200 && result < 200 + kX32ColourCount)
                           {
                               const int color = result - 200;
                               for (int chosen : channels)
                                   engine.setChannelColor(chosen, color);
                           }
                       });
}

void renameGroup(AudioEngine& engine, int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;
    renameWithPrompt("Rename " + engine.groupName(group),
                     "The group name is only a label. It does not change the audio.",
                     engine.groupName(group),
                     [&engine, group](juce::String name) { engine.setGroupName(group, name); });
}

void showGroupMenu(AudioEngine& engine, juce::Component& target, int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;

    juce::PopupMenu menu;
    menu.addItem(300, "Rename group");
    menu.addSubMenu("Color", colorMenu(engine.groupColor(group)));
    menu.addItem(2, "Remove group");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [&engine, group](int result)
                       {
                           if (result == 300)
                               renameGroup(engine, group);
                           else if (result == 2)
                           {
                               engine.clearGroup(group);
                           }
                           else if (result >= 200 && result < 200 + kX32ColourCount)
                           {
                               engine.setGroupColor(group, result - 200);
                           }
                       });
}

} // namespace youhost
