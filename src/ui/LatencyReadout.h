#pragma once

#include "engine/LatencyMath.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace youhost
{

class LatencyReadout : public juce::Component
{
public:
    LatencyReadout();

    void setNumbers(const LatencyNumbers& numbers);
    void setAlignGroup(int perGroup);
    void setResetHandler(std::function<void()> handler);
    void setAlignHandler(std::function<void(int perGroup)> handler);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    LatencyNumbers numbers_;
    int alignGroup_ = 0;
    std::function<void(int)> onAlign_;
    juce::TextButton allButton_ { "All aligned" };
    juce::TextButton groupButton_ { "Per group" };
    juce::TextButton resetButton_ { "Reset" };
};

} // namespace youhost
