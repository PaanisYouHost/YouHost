#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace youhost
{

void showChannelMenu(AudioEngine& engine,
                     juce::Component& target,
                     int channel,
                     std::function<void(int channel)> beginRename = nullptr);
void showGroupMenu(AudioEngine& engine, juce::Component& target, int group);
void renameGroup(AudioEngine& engine, int group);
void showMakeGroupDialog(AudioEngine& engine);

} // namespace youhost
