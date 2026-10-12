// Focuses a tool window and sends keys through ComponentPeer::handleKeyPress.
// Space, M, and the number keys must reach the transport listener. A marker
// name editor keeps Space, and a selected marker starts playback at its sample.

#include "engine/Markers.h"
#include "ui/ToolKeyBind.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <string>

namespace
{

int fail(const std::string& message)
{
    std::cerr << "tool window keys failed: " << message << "\n";
    return 1;
}

class Swallow : public juce::Component
{
public:
    Swallow()
    {
        setWantsKeyboardFocus(true);
    }

    bool keyPressed(const juce::KeyPress&) override
    {
        ++swallowed;
        return true;
    }

    int swallowed = 0;
};

class Holder : public juce::Component
{
public:
    Holder()
    {
        addAndMakeVisible(swallow);
        addAndMakeVisible(name);
        name.setText("Marker 1", juce::dontSendNotification);
        name.setEditable(false, true, false);
    }

    void resized() override
    {
        swallow.setBounds(8, 8, 140, 28);
        name.setBounds(156, 8, 220, 28);
    }

    Swallow swallow;
    juce::Label name;
};

class Probe : public juce::KeyListener
{
public:
    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override
    {
        const bool typing = youhost::componentIsTextInput(originating)
                            || youhost::componentIsTextInput(juce::Component::getCurrentlyFocusedComponent());
        if (! youhost::toolWindowForwardsTransportKeys(typing))
            return false;

        const auto matched = youhost::dispatchRawKey(youhost::rawKeyFromPress(key), 1, false);
        if (! matched.has_value())
            return false;

        if (*matched == youhost::ShortcutId::playOrStop)
        {
            ++plays;
            if (const auto sample = youhost::playSampleFromSelectedMarker(true, false, rowSelected, markerSample, true))
                playedFrom = *sample;
        }
        else if (*matched == youhost::ShortcutId::marker)
            ++markerKey;
        else if (*matched == youhost::ShortcutId::markers)
            ++markersKey;
        else if (*matched == youhost::ShortcutId::recPage
                 || *matched == youhost::ShortcutId::hostPage
                 || *matched == youhost::ShortcutId::dropouts
                 || *matched == youhost::ShortcutId::cpu
                 || *matched == youhost::ShortcutId::latency)
            ++numbers;
        return true;
    }

    bool rowSelected = false;
    std::int64_t markerSample = 48000;
    int plays = 0;
    int markerKey = 0;
    int markersKey = 0;
    int numbers = 0;
    std::int64_t playedFrom = -1;
};

bool send(juce::Component& window, const juce::KeyPress& key)
{
    auto* peer = window.getPeer();
    if (peer == nullptr)
        return false;
    return peer->handleKeyPress(key);
}

juce::KeyPress spaceKey()
{
    return { juce::KeyPress::spaceKey, {}, ' ' };
}

int exercise(const juce::String& title, char numberKey, bool fromMarker)
{
    Probe probe;
    probe.rowSelected = fromMarker;
    juce::DocumentWindow window(title, juce::Colours::black, juce::DocumentWindow::closeButton);
    auto* holder = new Holder();
    window.setUsingNativeTitleBar(false);
    window.setContentOwned(holder, true);
    window.setSize(420, 240);
    window.setVisible(true);
    window.toFront(true);
    youhost::bindToolKeys(window, probe);

    if (window.getPeer() == nullptr)
        return fail(title.toStdString() + " has no peer");

    holder->swallow.grabKeyboardFocus();
    if (! holder->swallow.hasKeyboardFocus(false))
        return fail(title.toStdString() + " did not focus the selected row");

    const int swallowedBefore = holder->swallow.swallowed;
    if (! send(window, spaceKey()))
        return fail(title.toStdString() + " did not dispatch Space");
    if (! send(window, juce::KeyPress('m')))
        return fail(title.toStdString() + " did not dispatch M");
    if (! send(window, juce::KeyPress(numberKey)))
        return fail(title.toStdString() + " did not dispatch the number key");

    if (probe.plays != 1)
        return fail(title.toStdString() + " Space did not start playback");
    if (fromMarker && probe.playedFrom != 48000)
        return fail(title.toStdString() + " Space did not play from the selected marker");
    if (! fromMarker && probe.playedFrom != -1)
        return fail(title.toStdString() + " Space located without a selected marker");
    if (probe.markerKey != 1)
        return fail(title.toStdString() + " M did not drop a marker");
    const bool numberReached = numberKey == '6' ? probe.markersKey == 1 : probe.numbers == 1;
    if (! numberReached)
        return fail(title.toStdString() + " number key did not reach the transport");
    if (holder->swallow.swallowed != swallowedBefore)
        return fail(title.toStdString() + " focused control swallowed a transport key");

    std::cout << title << " focused keys: Space"
              << (fromMarker ? " from marker" : "")
              << ", M, " << numberKey << "\n";
    return 0;
}

int editingKeepsSpace()
{
    Probe probe;
    probe.rowSelected = true;
    juce::DocumentWindow window("MARKERS", juce::Colours::black, juce::DocumentWindow::closeButton);
    auto* holder = new Holder();
    window.setUsingNativeTitleBar(false);
    window.setContentOwned(holder, true);
    window.setSize(420, 240);
    window.setVisible(true);
    youhost::bindToolKeys(window, probe);
    holder->name.showEditor();

    auto* editor = holder->name.getCurrentTextEditor();
    if (editor == nullptr)
        return fail("marker name editor did not open");
    editor->grabKeyboardFocus();
    if (! editor->hasKeyboardFocus(true))
        return fail("marker name editor did not take focus");

    const auto before = editor->getText();
    send(window, spaceKey());
    if (probe.plays != 0)
        return fail("Space started playback while the marker name was being edited");
    if (editor->getText() == before)
        return fail("Space did not type into the marker name");

    std::cout << "MARKERS name editor kept Space\n";
    return 0;
}

} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juce;

    if (const int status = exercise("MARKERS", '6', true))
        return status;
    if (const int status = exercise("CPU", '4', false))
        return status;
    if (const int status = exercise("DROPOUTS", '3', false))
        return status;
    if (const int status = exercise("LATENCY", '5', false))
        return status;
    if (const int status = editingKeepsSpace())
        return status;

    std::cout << "tool window keys passed\n";
    return 0;
}
