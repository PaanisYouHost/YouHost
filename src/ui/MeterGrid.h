#pragma once

#include "engine/MeterLayout.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace youhost
{

struct MeterReading
{
    float rms = 0.0f;
    float peak = 0.0f;
    bool clipped = false;
    bool hasInput = false;
};

class MeterGrid : public juce::Component
{
public:
    MeterGrid();

    void setReadings(std::vector<MeterReading> readings, bool showPeak, int rmsReferenceDb);
    void setClearHandler(std::function<void(int channel)> handler);

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    MeterLayout layoutFor(int count) const;
    int channelAt(juce::Point<float> position) const;

    std::vector<MeterReading> readings_;
    bool showPeak_ = false;
    int rmsReferenceDb_ = -20;
    std::function<void(int)> onClearClip_;
    MeterLayout layout_ {};
};

} // namespace youhost
