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
    void setResetHandler(std::function<void()> handler);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    struct CardLayout
    {
        juce::Rectangle<float> hero;
        juce::Rectangle<float> bufferRow;
        juce::Rectangle<float> inputRow;
        juce::Rectangle<float> outputRow;
        juce::Rectangle<float> compensationRow;
        juce::Rectangle<float> dropoutRow;
        juce::Rectangle<float> note;
    };

    static CardLayout layoutCard(juce::Rectangle<float> bounds);

    LatencyNumbers numbers_;
    juce::TextButton resetButton_ { "Reset" };
};

} // namespace youhost
