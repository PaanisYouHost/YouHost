#pragma once

#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

void showChannelMenu(AudioEngine& engine, juce::Component& target, int channel);
void showGroupMenu(AudioEngine& engine, juce::Component& target, int group);
void renameGroup(AudioEngine& engine, int group);

} // namespace youhost
