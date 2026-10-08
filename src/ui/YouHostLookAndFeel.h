#pragma once

#include "Theme.h"

namespace youhost
{

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
    }
};

} // namespace youhost
