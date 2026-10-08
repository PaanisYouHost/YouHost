#pragma once

#include "engine/Recorder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace youhost
{

inline juce::String timecodeText(std::int64_t samples, double sampleRate)
{
    const Timecode code = timecodeFromSamples(samples, sampleRate);
    return juce::String::formatted("%02d:%02d:%02d", code.hours, code.minutes, code.seconds);
}

class TimelineView : public juce::Component
{
public:
    void setTransport(const TransportView& view);
    void setLocateHandler(std::function<void(std::int64_t)> handler);

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;

private:
    std::int64_t spanSamples() const;
    std::int64_t sampleAt(float x) const;
    void locateAt(float x);

    TransportView view_;
    std::function<void(std::int64_t)> onLocate_;
};

} // namespace youhost
