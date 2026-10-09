#pragma once

#include "Theme.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace youhost
{

// Raised buttons and inset fields are cached images. A repaint blits the
// image. The image is built once per size and colour, on the message thread.
// Meters do not use this path. Nothing here runs on the audio thread.
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
        setColour(juce::TextEditor::focusedOutlineColourId, theme::text);
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
        blit(graphics, bounds, face, highlighted, down, FaceKind::raised);
    }

    void drawComboBox(juce::Graphics& graphics, int width, int height, bool isButtonDown,
                      int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override
    {
        auto bounds = juce::Rectangle<int>(0, 0, width, height);
        const auto face = box.findColour(juce::ComboBox::backgroundColourId);
        blit(graphics, bounds, face, false, isButtonDown, FaceKind::inset);

        juce::Path arrow;
        const float cx = static_cast<float>(buttonX) + static_cast<float>(buttonW) * 0.5f;
        const float cy = static_cast<float>(buttonY) + static_cast<float>(buttonH) * 0.5f;
        arrow.addTriangle(cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
        graphics.setColour(box.findColour(juce::ComboBox::arrowColourId));
        graphics.fillPath(arrow);
    }

    void fillTextEditorBackground(juce::Graphics& graphics, int width, int height, juce::TextEditor& editor) override
    {
        const auto face = editor.findColour(juce::TextEditor::backgroundColourId);
        blit(graphics, juce::Rectangle<int>(0, 0, width, height), face, false, false, FaceKind::inset);
    }

    void drawTextEditorOutline(juce::Graphics& graphics, int width, int height, juce::TextEditor& editor) override
    {
        if (! editor.hasKeyboardFocus(true))
            return;
        graphics.setColour(editor.findColour(juce::TextEditor::focusedOutlineColourId));
        graphics.drawRoundedRectangle(juce::Rectangle<float>(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f), 3.0f, 1.0f);
    }

private:
    enum class FaceKind : std::uint8_t
    {
        raised,
        inset
    };

    struct Face
    {
        int width = 0;
        int height = 0;
        std::uint32_t colour = 0;
        bool highlighted = false;
        bool down = false;
        FaceKind kind = FaceKind::raised;
        juce::Image image;
    };

    static void paintRaised(juce::Graphics& graphics, juce::Rectangle<float> bounds, juce::Colour face, bool highlighted, bool down)
    {
        auto top = face.brighter(highlighted ? 0.30f : 0.20f);
        auto bottom = face.darker(0.38f);
        if (down)
            std::swap(top, bottom);
        auto plate = bounds.reduced(0.5f);
        juce::ColourGradient gradient(top, plate.getX(), plate.getY(), bottom, plate.getX(), plate.getBottom(), false);
        graphics.setGradientFill(gradient);
        graphics.fillRoundedRectangle(plate, 3.0f);
        graphics.setColour(juce::Colours::black.withAlpha(down ? 0.55f : 0.45f));
        graphics.drawRoundedRectangle(plate, 3.0f, 1.0f);
        graphics.setColour(juce::Colours::white.withAlpha(down ? 0.06f : 0.28f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getY() + 1.0f, plate.getRight() - 2.0f, plate.getY() + 1.0f, 1.0f);
        graphics.setColour(juce::Colours::black.withAlpha(down ? 0.12f : 0.40f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getBottom() - 1.5f, plate.getRight() - 2.0f, plate.getBottom() - 1.5f, 1.0f);
    }

    static void paintInset(juce::Graphics& graphics, juce::Rectangle<float> bounds, juce::Colour face, bool down)
    {
        auto top = face.darker(down ? 0.40f : 0.24f);
        auto bottom = face.brighter(0.05f);
        auto plate = bounds.reduced(0.5f);
        juce::ColourGradient gradient(top, plate.getX(), plate.getY(), bottom, plate.getX(), plate.getBottom(), false);
        graphics.setGradientFill(gradient);
        graphics.fillRoundedRectangle(plate, 3.0f);
        graphics.setColour(juce::Colours::black.withAlpha(0.50f));
        graphics.drawRoundedRectangle(plate, 3.0f, 1.0f);
        graphics.setColour(juce::Colours::black.withAlpha(0.28f));
        graphics.drawLine(plate.getX() + 2.0f, plate.getY() + 1.0f, plate.getRight() - 2.0f, plate.getY() + 1.0f, 1.0f);
    }

    static void paintFlat(juce::Graphics& graphics, juce::Rectangle<float> bounds, juce::Colour face)
    {
        graphics.setColour(face);
        graphics.fillRect(bounds);
        graphics.setColour(juce::Colours::black.withAlpha(0.45f));
        graphics.drawRect(bounds, 1.0f);
    }

    void blit(juce::Graphics& graphics, juce::Rectangle<int> bounds, juce::Colour face, bool highlighted, bool down, FaceKind kind)
    {
        const int width = bounds.getWidth();
        const int height = bounds.getHeight();
        if (width < 2 || height < 2)
            return;
        auto area = bounds.toFloat();
        if (width > 640 || height > 160)
        {
            paintFlat(graphics, area, face);
            return;
        }

        const auto image = cachedFace(width, height, face, highlighted, down, kind);
        graphics.drawImageAt(image, bounds.getX(), bounds.getY());
    }

    juce::Image cachedFace(int width, int height, juce::Colour face, bool highlighted, bool down, FaceKind kind)
    {
        const auto colour = face.getARGB();
        for (const auto& cached : faces_)
            if (cached.width == width && cached.height == height && cached.colour == colour
                && cached.highlighted == highlighted && cached.down == down && cached.kind == kind)
                return cached.image;

        if (faces_.size() > 96)
            faces_.clear();

        Face created;
        created.width = width;
        created.height = height;
        created.colour = colour;
        created.highlighted = highlighted;
        created.down = down;
        created.kind = kind;
        created.image = juce::Image(juce::Image::ARGB, width, height, true);
        juce::Graphics imageGraphics(created.image);
        const auto plate = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
        if (kind == FaceKind::inset)
            paintInset(imageGraphics, plate, face, down);
        else
            paintRaised(imageGraphics, plate, face, highlighted, down);
        faces_.push_back(created);
        return created.image;
    }

    std::vector<Face> faces_;
};

} // namespace youhost
