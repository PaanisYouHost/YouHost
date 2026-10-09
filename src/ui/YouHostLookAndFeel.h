#pragma once

#include "Theme.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace youhost
{

// Button faces are cached images. Paint only blits them, and only on the
// message thread when a button is shown or its state changes. Nothing here
// runs on the audio thread.
class YouHostLookAndFeel : public juce::LookAndFeel_V4
{
public:
    YouHostLookAndFeel()
    {
        setColourScheme(juce::LookAndFeel_V4::getDarkColourScheme());

        setColour(juce::ResizableWindow::backgroundColourId, theme::background);
        setColour(juce::DocumentWindow::backgroundColourId, theme::background);
        setColour(juce::Label::textColourId, theme::text);
        setColour(juce::TextButton::buttonColourId, theme::button);
        setColour(juce::TextButton::buttonOnColourId, theme::buttonOn);
        setColour(juce::TextButton::textColourOffId, theme::text);
        setColour(juce::TextButton::textColourOnId, theme::text);
        setColour(juce::ComboBox::backgroundColourId, theme::panel);
        setColour(juce::ComboBox::textColourId, theme::text);
        setColour(juce::ComboBox::outlineColourId, theme::panelEdge);
        setColour(juce::ComboBox::arrowColourId, theme::dim);
        setColour(juce::PopupMenu::backgroundColourId, theme::panel);
        setColour(juce::PopupMenu::textColourId, theme::text);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, theme::buttonOn);
        setColour(juce::PopupMenu::highlightedTextColourId, theme::text);
        setColour(juce::ListBox::backgroundColourId, theme::background);
        setColour(juce::ListBox::textColourId, theme::text);
        setColour(juce::ListBox::outlineColourId, theme::panelEdge);
        setColour(juce::ScrollBar::thumbColourId, theme::panelEdge);
        setColour(juce::ScrollBar::trackColourId, theme::background);
        setColour(juce::ToggleButton::textColourId, theme::text);
        setColour(juce::ToggleButton::tickColourId, theme::green);
        setColour(juce::ToggleButton::tickDisabledColourId, theme::fainter);
        setColour(juce::TextEditor::backgroundColourId, theme::background);
        setColour(juce::TextEditor::textColourId, theme::text);
        setColour(juce::TextEditor::outlineColourId, theme::panelEdge);
        setColour(juce::TextEditor::highlightColourId, theme::buttonOn);
        setColour(juce::CaretComponent::caretColourId, theme::text);
        setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    }

    void drawButtonBackground(juce::Graphics& graphics,
                              juce::Button& button,
                              const juce::Colour& backgroundColour,
                              bool highlighted,
                              bool down) override
    {
        auto bounds = button.getLocalBounds();
        const int width = bounds.getWidth();
        const int height = bounds.getHeight();
        if (width < 2 || height < 2)
            return;

        const bool on = button.getToggleState();
        const auto face = on ? button.findColour(juce::TextButton::buttonOnColourId, true) : backgroundColour;
        if (width > 640 || height > 160)
        {
            paintFace(graphics, bounds.toFloat(), face, highlighted, down);
            return;
        }

        const auto image = cachedFace(width, height, face, highlighted, down);
        graphics.drawImageAt(image, bounds.getX(), bounds.getY());
    }

private:
    struct Face
    {
        int width = 0;
        int height = 0;
        std::uint32_t colour = 0;
        bool highlighted = false;
        bool down = false;
        juce::Image image;
    };

    static void paintFace(juce::Graphics& graphics, juce::Rectangle<float> bounds, juce::Colour face, bool highlighted, bool down)
    {
        auto top = face.brighter(highlighted ? 0.22f : 0.14f);
        auto bottom = face.darker(0.28f);
        if (down)
            std::swap(top, bottom);
        juce::ColourGradient gradient(top, bounds.getX(), bounds.getY(), bottom, bounds.getX(), bounds.getBottom(), false);
        graphics.setGradientFill(gradient);
        graphics.fillRoundedRectangle(bounds.reduced(0.5f), 3.0f);
        graphics.setColour(juce::Colours::black.withAlpha(down ? 0.45f : 0.35f));
        graphics.drawRoundedRectangle(bounds.reduced(0.5f), 3.0f, 1.0f);
        graphics.setColour(top.withAlpha(0.7f));
        graphics.drawLine(bounds.getX() + 2.0f, bounds.getY() + 1.0f, bounds.getRight() - 2.0f, bounds.getY() + 1.0f, 1.0f);
    }

    juce::Image cachedFace(int width, int height, juce::Colour face, bool highlighted, bool down)
    {
        const auto colour = face.getARGB();
        for (const auto& cached : faces_)
            if (cached.width == width && cached.height == height && cached.colour == colour
                && cached.highlighted == highlighted && cached.down == down)
                return cached.image;

        if (faces_.size() > 48)
            faces_.clear();

        Face created;
        created.width = width;
        created.height = height;
        created.colour = colour;
        created.highlighted = highlighted;
        created.down = down;
        created.image = juce::Image(juce::Image::ARGB, width, height, true);
        juce::Graphics imageGraphics(created.image);
        paintFace(imageGraphics, juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)), face, highlighted, down);
        faces_.push_back(created);
        return created.image;
    }

    std::vector<Face> faces_;
};

} // namespace youhost
