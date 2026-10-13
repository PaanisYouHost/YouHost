#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <cmath>
#include <functional>

// Plate that matches the YouHost output-trim control: drag in 0.5-style steps,
// option-click reset, double-click to type, scroll wheel ignored.
class DragValue : public juce::Component
{
public:
    DragValue()
    {
        setMouseClickGrabsKeyboardFocus(false);
        editor_.setJustification(juce::Justification::centred);
        editor_.setInputRestrictions(10, "0123456789.-+");
        editor_.setFont(juce::Font(juce::FontOptions(12.0f)));
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

    std::function<float()> read;
    std::function<void(float)> write;
    std::function<float()> resetTo;
    float minimum = -60.0f;
    float maximum = 0.0f;
    float step = 0.5f;
    int decimals = 1;
    juce::String suffix { " dB" };
    float textPt = 12.0f;
    bool bold = false;

    void setHero(bool hero)
    {
        textPt = hero ? 28.0f : 12.0f;
        bold = hero;
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}

    void mouseDown(const juce::MouseEvent& event) override
    {
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
        if (event.getNumberOfClicks() >= 2)
        {
            dragging_ = false;
            editor_.setFont(juce::Font(juce::FontOptions(juce::jmin(textPt, 16.0f))));
            editor_.setText(juce::String(read(), decimals), false);
            editor_.setVisible(true);
            editor_.grabKeyboardFocus();
            editor_.selectAll();
            return;
        }
        dragging_ = true;
        start_ = read();
    }

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (! dragging_ || write == nullptr)
            return;
        const float raw = start_ - static_cast<float>(event.getDistanceFromDragStartY()) * (step / 10.0f);
        write(snap(raw));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        dragging_ = false;
    }

    void paint(juce::Graphics& graphics) override
    {
        if (editor_.isVisible())
            return;

        const float value = read != nullptr ? read() : 0.0f;
        const bool away = resetTo != nullptr && std::fabs(value - resetTo()) > step * 0.49f;
        auto plate = getLocalBounds().toFloat().reduced(1.0f);
        graphics.setColour(away ? juce::Colour(0xff8a6a22) : juce::Colour(0xff262b38));
        graphics.fillRoundedRectangle(plate, 3.0f);
        graphics.setColour(juce::Colours::white.withAlpha(0.18f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getY() + 1.0f, plate.getRight() - 2.0f, plate.getY() + 1.0f, 1.0f);
        graphics.setColour(juce::Colours::black.withAlpha(0.35f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getBottom() - 1.5f, plate.getRight() - 2.0f, plate.getBottom() - 1.5f, 1.0f);

        auto font = juce::FontOptions(textPt);
        if (bold || away)
            font = font.withStyle("Bold");
        graphics.setColour(juce::Colour(0xfff3f5f8));
        graphics.setFont(juce::Font(font));
        graphics.drawText(juce::String(value, decimals) + suffix, getLocalBounds(), juce::Justification::centred, false);
    }

    void resized() override
    {
        editor_.setBounds(getLocalBounds().reduced(1));
    }

private:
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
    float start_ = 0.0f;
    bool dragging_ = false;
};
