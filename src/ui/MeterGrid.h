#pragma once

#include "engine/HostLimits.h"
#include "engine/MeterLayout.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <vector>

namespace youhost
{

struct SlotMark
{
    bool occupied = false;
    bool bypassed = false;
    bool selected = false;
    bool loading = false;
};

struct MeterReading
{
    float rms = 0.0f;
    float peak = 0.0f;
    bool clipped = false;
    bool hasInput = false;
    std::array<SlotMark, kSlotsPerChannel> slots {};
};

struct MeterHit
{
    int channel = -1;
    bool clip = false;
    int slot = -1;
};

class MeterGrid : public juce::Component
{
public:
    MeterGrid();

    void setReadings(std::vector<MeterReading> readings, bool showPeak, int rmsReferenceDb);
    void setClearHandler(std::function<void(int channel)> handler);
    void setSlotHandler(std::function<void(int channel, int slot)> handler);

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    MeterLayout layoutFor(int count) const;
    MeterHit meterAt(juce::Point<float> position) const;

    std::vector<MeterReading> readings_;
    bool showPeak_ = false;
    int rmsReferenceDb_ = -20;
    std::function<void(int)> onClearClip_;
    std::function<void(int, int)> onSlot_;
    MeterLayout layout_ {};
};

} // namespace youhost
