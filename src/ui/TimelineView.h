#pragma once

#include "engine/Recorder.h"
#include "engine/SessionFormat.h"
#include "engine/TimelineLanes.h"
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

    using LaneProvider = std::function<void(const std::function<void(const std::vector<TimelineLaneView>&)>&)>;

    void setTransport(const TransportView& view);
    void setLaneProvider(LaneProvider provider);
    void setLocateHandler(std::function<void(std::int64_t)> handler);
    void setHeightHandler(std::function<void(int)> handler);
    void zoomIn();
    void zoomOut();
    void fitAll();
    void verticalZoomIn();
    void verticalZoomOut();
    void setWaveformGain(float gain);
    void setWaveformGainHandler(std::function<void(float)> handler);
    void setViewState(const SessionTimelineState& state);
    SessionTimelineState viewState() const;

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& event) override;
    void mouseDrag(const juce::MouseEvent& event) override;
    void mouseUp(const juce::MouseEvent& event) override;
    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override;

private:
    void scrollBarMoved(juce::ScrollBar* bar, double newRangeStart) override;
    std::int64_t fullSpan() const;
    std::int64_t visibleSamples() const;
    void zoomBy(int delta);
    void nudgeWaveformGain(int direction);
    std::int64_t zoomAnchorSample() const;
    void verticalZoomBy(int delta);
    void syncScroll();
    juce::Rectangle<float> waveformArea() const;
    std::int64_t sampleAt(float x) const;
    void locateAt(float x);
    int laneCount() const;
    int lanesShown() const;
    void clampLaneScroll();

    TransportView view_;
    LaneProvider laneProvider_;
    std::function<void(std::int64_t)> onLocate_;
    std::function<void(int)> onHeight_;
    int knownLanes_ = 0;
    int zoomStep_ = 0;
    int verticalStep_ = 0;
    int laneScroll_ = 0;
    std::int64_t viewStart_ = 0;
    bool holdTimeScroll_ = false;
    bool pendingView_ = false;
    bool updatingScroll_ = false;
    SessionTimelineState pendingViewState_ {};
    bool draggingHeight_ = false;
    int dragStartY_ = 0;
    int dragStartHeight_ = 0;
    juce::ScrollBar scroll_ { false };
    juce::ScrollBar laneScrollBar_ { true };
    juce::TextButton zoomOutButton_ { "-" };
    juce::TextButton zoomInButton_ { "+" };
    juce::TextButton verticalOutButton_ { "v-" };
    juce::TextButton verticalInButton_ { "v+" };
    juce::TextButton fitButton_ { "Fit" };
    juce::TextButton waveOutButton_ { "W-" };
    juce::TextButton waveInButton_ { "W+" };
    float waveformGain_ = 1.0f;
    float laneWheel_ = 0.0f;
    double timeWheel_ = 0.0;
    float gainWheel_ = 0.0f;
    std::function<void(float)> onWaveformGain_;
};

} // namespace youhost
