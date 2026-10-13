#pragma once

#include "engine/Shortcuts.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

inline bool componentIsTextInput(const juce::Component* component)
{
    if (component == nullptr)
        return false;
    if (dynamic_cast<const juce::TextEditor*>(component) != nullptr)
        return true;
    if (component->findParentComponentOfClass<juce::TextEditor>() != nullptr)
        return true;
    return false;
}

inline RawKey rawKeyFromPress(const juce::KeyPress& key)
{
    RawKey raw;
    raw.shift = key.getModifiers().isShiftDown();
    raw.command = key.getModifiers().isCommandDown();
    raw.alt = key.getModifiers().isAltDown();
    raw.textCharacter = key.getTextCharacter();
    raw.keyCode = key.getKeyCode();
    if (key.isKeyCode(juce::KeyPress::spaceKey))
        raw.token = RawToken::space;
    else if (key.isKeyCode(juce::KeyPress::leftKey))
        raw.token = RawToken::left;
    else if (key.isKeyCode(juce::KeyPress::rightKey))
        raw.token = RawToken::right;
    else if (key.isKeyCode(juce::KeyPress::backspaceKey))
        raw.token = RawToken::backspace;
    return raw;
}

// Listeners on the focused component run before that component can swallow
// the key. Tool windows bind the same listener to every child.
inline void bindToolKeys(juce::Component& component, juce::KeyListener& keys)
{
    component.addKeyListener(&keys);
    for (int index = 0; index < component.getNumChildComponents(); ++index)
        if (auto* child = component.getChildComponent(index))
            bindToolKeys(*child, keys);
}

inline void unbindToolKeys(juce::Component& component, juce::KeyListener& keys)
{
    component.removeKeyListener(&keys);
    for (int index = 0; index < component.getNumChildComponents(); ++index)
        if (auto* child = component.getChildComponent(index))
            unbindToolKeys(*child, keys);
}

} // namespace youhost
