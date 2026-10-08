#pragma once

#include "engine/Recorder.h"
#include "engine/TimelineZoom.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace youhost
{

inline juce::String timecodeText(std::int64_t samples, double sampleRate)
{
    const Timecode code = timecodeFromSamples(samples, sampleRate);
    return juce::String::formatted("%02d:%02d:%02d", code.hours, code.minutes, code.seconds);
}

class TimelineView : public juce::Component,
                     private juce::ScrollBar::Listener
{
public:
    TimelineView();
    ~TimelineView() override;

    void setTransport(const TransportView& view);
    void setLocateHandler(std::function<void(std::int64_t)> handler);
    void zoomIn();
    void zoomOut();

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;

private:
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;
    std::int64_t fullSpan() const;
    std::int64_t visibleSamples() const;
    void zoomBy(int delta);
    void syncScroll();
    juce::Rectangle<float> waveformArea() const;
    std::int64_t sampleAt(float x) const;
    void locateAt(float x);

    TransportView view_;
    std::function<void(std::int64_t)> onLocate_;
    int zoomStep_ = 0;
    std::int64_t viewStart_ = 0;
    bool updatingScroll_ = false;
    juce::ScrollBar scroll_ { false };
    juce::TextButton zoomOutButton_ { "-" };
    juce::TextButton zoomInButton_ { "+" };
};

} // namespace youhost
