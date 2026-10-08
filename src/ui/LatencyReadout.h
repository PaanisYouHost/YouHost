#pragma once

#include "engine/LatencyMath.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class LatencyReadout : public juce::Component
{
public:
    LatencyReadout();

    void setNumbers(const LatencyNumbers& numbers);

    void paint(juce::Graphics& graphics) override;

private:
    LatencyNumbers numbers_;
};

} // namespace youhost
