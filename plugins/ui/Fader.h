#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>

// Large horizontal fader. Drag or click the track to move it.
// Hold + or - and it keeps stepping, faster the longer it is held.
// Double-click the value to type. Option-click resets. The scroll wheel does nothing.
class Fader : public juce::Component,
              public juce::SettableTooltipClient,
              private juce::Timer
{
public:
    Fader()
    {
        setMouseClickGrabsKeyboardFocus(false);
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
        editor_.setJustification(juce::Justification::centred);
        editor_.setInputRestrictions(10, "0123456789.-+");
        editor_.setFont(juce::Font(juce::FontOptions(16.0f)));
        editor_.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff1a1e28));
        editor_.setColour(juce::TextEditor::textColourId, juce::Colour(0xfff3f5f8));
        editor_.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        editor_.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff6ee0aa));
        editor_.onReturnKey = [this] { commit(); };
        editor_.onFocusLost = [this] { commit(); };
        editor_.onEscapeKey = [this]
        {
            editor_.setVisible(false);
            repaint();
        };
        addChildComponent(editor_);
    }

    ~Fader() override
    {
        stopTimer();
    }

    std::function<float()> read;
    std::function<void(float)> write;
    std::function<float()> resetTo;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.5f;
    int decimals = 1;
    juce::String suffix { " dB" };
    float textPt = 28.0f;

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}

    void mouseDown(const juce::MouseEvent& event) override
    {
        stopRepeat();
        if (read == nullptr || write == nullptr)
            return;
        if (event.mods.isAltDown())
        {
            dragging_ = false;
            if (resetTo != nullptr)
                write(snap(resetTo()));
            repaint();
            return;
        }
        if (event.getNumberOfClicks() >= 2 && valueArea().contains(event.position))
        {
            dragging_ = false;
            editor_.setFont(juce::Font(juce::FontOptions(juce::jmin(textPt, 22.0f))));
            editor_.setText(juce::String(read(), decimals), false);
            editor_.setVisible(true);
            editor_.grabKeyboardFocus();
            editor_.selectAll();
            resized();
            return;
        }
        if (minusArea().contains(event.position))
        {
            dragging_ = false;
            beginRepeat(-1);
            return;
        }
        if (plusArea().contains(event.position))
        {
            dragging_ = false;
            beginRepeat(1);
            return;
        }
        dragging_ = true;
        write(valueFromPosition(event.position.x));
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (! dragging_ || write == nullptr)
            return;
        write(valueFromPosition(event.position.x));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragging_ = false;
        stopRepeat();
    }

    void paint(juce::Graphics& graphics) override
    {
        const float value = read != nullptr ? read() : minimum;
        if (! editor_.isVisible())
        {
            auto font = juce::FontOptions(textPt).withStyle("Bold");
            graphics.setColour(juce::Colour(0xfff3f5f8));
            graphics.setFont(juce::Font(font));
            graphics.drawText(juce::String(value, decimals) + suffix, valueArea().toNearestInt(), juce::Justification::centred, false);
        }

        paintStep(graphics, minusArea(), "-");
        paintStep(graphics, plusArea(), "+");

        const auto track = sliderArea();
        graphics.setColour(juce::Colour(0xff0c0e13));
        graphics.fillRoundedRectangle(track, 8.0f);

        const float norm = normalised(value);
        const float fillWidth = juce::jmax(6.0f, track.getWidth() * norm);
        graphics.setColour(juce::Colour(0xff2f6d52));
        graphics.fillRoundedRectangle(track.withWidth(juce::jmin(fillWidth, track.getWidth())), 8.0f);

        const float thumbWidth = juce::jmin(44.0f, juce::jmax(18.0f, track.getWidth() * 0.12f));
        const float travel = juce::jmax(0.0f, track.getWidth() - thumbWidth);
        auto thumb = juce::Rectangle<float>(track.getX() + travel * norm, track.getY() + 3.0f, thumbWidth, juce::jmax(8.0f, track.getHeight() - 6.0f));
        graphics.setColour(juce::Colour(0xfff3f5f8));
        graphics.fillRoundedRectangle(thumb, 7.0f);
    }

    void resized() override
    {
        editor_.setBounds(valueArea().toNearestInt());
    }

private:
    void timerCallback() override
    {
        if (direction_ == 0)
            return;
        ++repeats_;
        int multiplier = 1;
        if (repeats_ > 14)
            multiplier = 4;
        else if (repeats_ > 7)
            multiplier = 2;
        stepBy(direction_ * multiplier);
        int interval = 140;
        if (repeats_ > 14)
            interval = 40;
        else if (repeats_ > 8)
            interval = 70;
        else if (repeats_ > 3)
            interval = 100;
        startTimer(interval);
    }

    void beginRepeat(int direction)
    {
        direction_ = direction;
        repeats_ = 0;
        stepBy(direction);
        startTimer(380);
    }

    void stopRepeat()
    {
        direction_ = 0;
        repeats_ = 0;
        stopTimer();
    }

    void stepBy(int steps)
    {
        if (read == nullptr || write == nullptr || steps == 0 || step <= 0.0f)
            return;
        write(snap(read() + step * static_cast<float>(steps)));
        repaint();
    }

    static void paintStep(juce::Graphics& graphics, juce::Rectangle<float> area, const char* text)
    {
        graphics.setColour(juce::Colour(0xff262b38));
        graphics.fillRoundedRectangle(area, 8.0f);
        graphics.setColour(juce::Colour(0xfff3f5f8));
        const float point = juce::jlimit(16.0f, 28.0f, area.getHeight() * 0.55f);
        graphics.setFont(juce::Font(juce::FontOptions(point).withStyle("Bold")));
        graphics.drawText(text, area.toNearestInt(), juce::Justification::centred, false);
    }

    float rowHeight() const
    {
        return getHeight() > 78 ? 36.0f : 24.0f;
    }

    juce::Rectangle<float> valueArea() const
    {
        return getLocalBounds().toFloat().removeFromTop(rowHeight());
    }

    juce::Rectangle<float> controlRow() const
    {
        auto area = getLocalBounds().toFloat();
        area.removeFromTop(rowHeight());
        return area.reduced(0.0f, 3.0f);
    }

    float buttonWidth() const
    {
        const auto row = controlRow();
        float width = juce::jmin(46.0f, juce::jmax(28.0f, row.getHeight()));
        if (row.getWidth() < 160.0f)
            width = juce::jmin(width, juce::jmax(22.0f, row.getWidth() * 0.22f));
        return width;
    }

    juce::Rectangle<float> minusArea() const
    {
        auto row = controlRow();
        return row.removeFromLeft(buttonWidth());
    }

    juce::Rectangle<float> plusArea() const
    {
        auto row = controlRow();
        return row.removeFromRight(buttonWidth());
    }

    juce::Rectangle<float> sliderArea() const
    {
        auto row = controlRow();
        row.removeFromLeft(buttonWidth() + 4.0f);
        row.removeFromRight(buttonWidth() + 4.0f);
        return row;
    }

    float normalised(float value) const
    {
        const float span = maximum - minimum;
        if (span <= 0.0f)
            return 0.0f;
        float norm = (value - minimum) / span;
        if (norm < 0.0f)
            norm = 0.0f;
        if (norm > 1.0f)
            norm = 1.0f;
        return norm;
    }

    float valueFromPosition(float x) const
    {
        const auto track = sliderArea();
        const float width = track.getWidth();
        float norm = width > 1.0f ? (x - track.getX()) / width : 0.0f;
        if (norm < 0.0f)
            norm = 0.0f;
        if (norm > 1.0f)
            norm = 1.0f;
        return snap(minimum + norm * (maximum - minimum));
    }

    float snap(float value) const
    {
        float snapped = value;
        if (step > 0.0f)
            snapped = std::round(value / step) * step;
        if (snapped < minimum)
            snapped = minimum;
        if (snapped > maximum)
            snapped = maximum;
        return snapped;
    }

    void commit()
    {
        if (! editor_.isVisible() || write == nullptr)
            return;
        write(snap(editor_.getText().getFloatValue()));
        editor_.setVisible(false);
        repaint();
    }

    juce::TextEditor editor_;
    bool dragging_ = false;
    int direction_ = 0;
    int repeats_ = 0;
};
