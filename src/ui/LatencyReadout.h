#pragma once

#include "engine/LatencyCompensation.h"
#include "engine/LatencyMath.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>

namespace youhost
{

class LatencyReadout : public juce::Component
{
public:
    LatencyReadout();

    void setNumbers(const LatencyNumbers& numbers);
    void setAlignGroup(int perGroup);
    void setGroupLines(const GroupLatencyLine* lines, int count);
    void setResetHandler(std::function<void()> handler);
    void setAlignHandler(std::function<void(int perGroup)> handler);

    void paint(juce::Graphics& graphics) override;
    void resized() override;

private:
    LatencyNumbers numbers_;
    int alignGroup_ = 0;
    int groupCount_ = 0;
    std::array<GroupLatencyLine, kMaxDisplayGroups> groupLines_ {};
    std::function<void(int)> onAlign_;
    juce::TextButton allButton_ { "Global" };
    juce::TextButton groupButton_ { "Per group" };
    juce::TextButton resetButton_ { "Reset" };
};

} // namespace youhost
