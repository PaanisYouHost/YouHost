#include "GigWindow.h"

#include "Theme.h"
#include "WindowMemory.h"
#include "engine/GigMode.h"
#include "engine/WindowCatalog.h"

#include <atomic>
#include <memory>
#include <string>

namespace youhost
{
namespace
{

std::string runCommand(const juce::String& command)
{
    juce::ChildProcess process;
    if (! process.start(command))
        return {};
    if (! process.waitForProcessToFinish(1500))
    {
        process.kill();
        return {};
    }
    return process.readAllProcessOutput().toStdString();
}

struct GigSnapshot
{
    bool cardOpen = false;
    GigLoad spotlight = GigLoad::unknown;
    GigLoad timeMachine = GigLoad::unknown;
    GigLoad updates = GigLoad::unknown;
    GigLoad wifi = GigLoad::unknown;
    GigLoad bluetooth = GigLoad::unknown;
};

GigSnapshot readGigSnapshot(bool cardOpen)
{
    GigSnapshot snapshot;
    snapshot.cardOpen = cardOpen;
    const auto processes = runCommand("ps -axo comm=");
    snapshot.spotlight = spotlightLoad(processes);
    snapshot.timeMachine = timeMachineLoad(runCommand("tmutil status"));
    const auto download = runCommand("defaults read /Library/Preferences/com.apple.SoftwareUpdate AutomaticDownload");
    const auto install = runCommand("defaults read /Library/Preferences/com.apple.SoftwareUpdate AutomaticallyInstallMacOSUpdates");
    std::string updates;
    if (download.find('1') != std::string::npos)
        updates += "AutomaticDownload = 1\n";
    if (install.find('1') != std::string::npos)
        updates += "AutomaticallyInstallMacOSUpdates = 1\n";
    if (processes.find("\ninstalld\n") != std::string::npos)
        updates += "\ninstalld\n";
    if (download.empty() && install.empty())
        snapshot.updates = GigLoad::unknown;
    else
        snapshot.updates = updatesLoad(updates.empty() ? std::string("none") : updates);
    snapshot.wifi = wifiLoad(runCommand("networksetup -getairportpower en0"));
    if (snapshot.wifi == GigLoad::unknown)
        snapshot.wifi = wifiLoad(runCommand("networksetup -getairportpower en1"));
    const auto bluetooth = runCommand("defaults read /Library/Preferences/com.apple.Bluetooth ControllerPowerState");
    if (bluetooth.find('1') != std::string::npos && bluetooth.find('0') == std::string::npos)
        snapshot.bluetooth = bluetoothLoad("ControllerPowerState = 1");
    else if (bluetooth.find('0') != std::string::npos)
        snapshot.bluetooth = bluetoothLoad("ControllerPowerState = 0");
    else
        snapshot.bluetooth = GigLoad::unknown;
    return snapshot;
}

} // namespace

class GigWindow::Content : public juce::Component
{
public:
    explicit Content(AudioEngine& engine)
        : engine_(engine)
    {
        setSize(gigWindowWidth(), gigWindowContentHeight());
    }

    ~Content() override
    {
        alive_->store(false);
    }

    void refresh()
    {
        cardOpen_ = engine_.deviceIsOpen() && ! engine_.offlineTemplate();
        repaint();
        startCheck();
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
        auto area = getLocalBounds().reduced(16, 14);
        graphics.setColour(theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(18.0f).withStyle("Bold")));
        graphics.drawText("Gig mode", area.removeFromTop(22), juce::Justification::centredLeft, false);
        area.removeFromTop(8);
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        const auto claim = cardOpen_
                               ? juce::String(kGigClaimLine)
                               : juce::String("Open the audio card. Gig mode applies while that card is running. ")
                                     + juce::String(kGigClaimLine);
        graphics.drawFittedText(claim, area.removeFromTop(64), juce::Justification::topLeft, 4);
        area.removeFromTop(8);

        drawRow(graphics, area.removeFromTop(52), "Spotlight", spotlight_, kGigSpotlightHint);
        drawRow(graphics, area.removeFromTop(52), "Time Machine", timeMachine_, kGigTimeMachineHint);
        drawRow(graphics, area.removeFromTop(52), "Updates", updates_, kGigUpdatesHint);
        drawRow(graphics, area.removeFromTop(52), "Wi-Fi", wifi_, kGigWifiHint);
        drawRow(graphics, area.removeFromTop(52), "Bluetooth", bluetooth_, kGigBluetoothHint);
    }

private:
    void drawRow(juce::Graphics& graphics, juce::Rectangle<int> row, const char* name, GigLoad load, const char* hint)
    {
        graphics.setColour(load == GigLoad::busy ? juce::Colour(0xffffb020) : theme::text);
        graphics.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));
        graphics.drawText(juce::String(name) + ": " + gigLoadWord(load),
                          row.removeFromTop(18),
                          juce::Justification::centredLeft,
                          false);
        graphics.setColour(theme::dim);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        juce::String line = load == GigLoad::unknown ? juce::String(kGigCouldNotCheck) + " " : juce::String();
        line += hint;
        graphics.drawFittedText(line, row, juce::Justification::topLeft, 2);
    }

    void startCheck()
    {
        if (checking_.exchange(true))
            return;
        auto alive = alive_;
        auto* self = this;
        juce::Thread::launch([alive, self, open = cardOpen_]
        {
            const auto snapshot = readGigSnapshot(open);
            juce::MessageManager::callAsync([alive, self, snapshot]
            {
                if (! alive->load())
                    return;
                self->apply(snapshot);
            });
        });
    }

    void apply(const GigSnapshot& snapshot)
    {
        checking_.store(false);
        cardOpen_ = engine_.deviceIsOpen() && ! engine_.offlineTemplate();
        spotlight_ = snapshot.spotlight;
        timeMachine_ = snapshot.timeMachine;
        updates_ = snapshot.updates;
        wifi_ = snapshot.wifi;
        bluetooth_ = snapshot.bluetooth;
        repaint();
    }

    AudioEngine& engine_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    std::atomic<bool> checking_ { false };
    bool cardOpen_ = false;
    GigLoad spotlight_ = GigLoad::unknown;
    GigLoad timeMachine_ = GigLoad::unknown;
    GigLoad updates_ = GigLoad::unknown;
    GigLoad wifi_ = GigLoad::unknown;
    GigLoad bluetooth_ = GigLoad::unknown;
};

GigWindow::GigWindow(AudioEngine& engine, AppSettings& settings)
    : juce::DocumentWindow("GIG", theme::panel, juce::DocumentWindow::closeButton),
      settings_(settings)
{
    auto content = std::make_unique<Content>(engine_);
    content_ = content.get();
    setUsingNativeTitleBar(true);
    setContentOwned(content.release(), true);
    prepareRememberedWindow(*this, settings_, "windowGig", gigWindowWidth(), gigWindowHeight(), 480, gigWindowContentHeight());
    setVisible(false);
}

GigWindow::~GigWindow()
{
    saveRememberedWindow(*this, settings_, "windowGig", gigWindowWidth(), gigWindowHeight());
}

void GigWindow::toggle()
{
    setVisible(! isVisible());
    if (isVisible())
    {
        if (content_ != nullptr)
            content_->refresh();
        toFront(true);
    }
}

void GigWindow::closeButtonPressed()
{
    saveRememberedWindow(*this, settings_, "windowGig", gigWindowWidth(), gigWindowHeight());
    setVisible(false);
}

} // namespace youhost
