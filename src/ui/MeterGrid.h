#pragma once

#include "engine/DisplayLayout.h"
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
    bool recordArmed = true;
    bool recordLive = false;
    std::array<SlotMark, kSlotsPerChannel> slots {};
};

struct BridgeCell
{
    bool header = false;
    int channel = -1;
    int group = -1;
    int color = 0;
    bool collapsed = false;
    bool selected = false;
    juce::String title;
    MeterReading reading {};
    bool anyPlugin = false;
    int membersOn = 0;
    int memberCount = 0;
};

struct MeterHit
{
    int channel = -1;
    int group = -1;
    bool header = false;
    bool clip = false;
    bool record = false;
};

class MeterScaleRail : public juce::Component
{
public:
    void setScale(bool peak, int referenceDb, bool alignRight);
    void paint(juce::Graphics& graphics) override;

private:
    bool peak_ = false;
    int referenceDb_ = -20;
    bool alignRight_ = false;
};

class MeterGrid : public juce::Component
{
public:
    MeterGrid();

    void setCells(std::vector<BridgeCell> cells, bool showPeak, int rmsReferenceDb);
    void setFitWidth(int viewportWidth);
    int preferredWidth(int viewportWidth) const;
    int naturalContentWidth(int viewportWidth) const;

    void setClearHandler(std::function<void(int channel)> handler);
    void setRecordHandler(std::function<void(int channel)> handler);
    void setChannelMenuHandler(std::function<void(int channel)> handler);
    void setGroupToggleHandler(std::function<void(int group)> handler);
    void setGroupMenuHandler(std::function<void(int group)> handler);
    void setGroupRenameHandler(std::function<void(int group)> handler);
    void setSelectHandler(std::function<void(int channel, bool extend)> handler);

    void paint(juce::Graphics& graphics) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    BridgeMetrics metricsFor(int viewportWidth) const;
    MeterHit hitAt(juce::Point<float> position) const;

    std::vector<BridgeCell> cells_;
    bool showPeak_ = false;
    int rmsReferenceDb_ = -20;
    int fitWidth_ = 0;
    std::function<void(int)> onClearClip_;
    std::function<void(int)> onRecord_;
    std::function<void(int)> onChannelMenu_;
    std::function<void(int)> onGroupToggle_;
    std::function<void(int)> onGroupMenu_;
    std::function<void(int)> onGroupRename_;
    std::function<void(int, bool)> onSelect_;
    BridgeMetrics metrics_ {};
};

} // namespace youhost
