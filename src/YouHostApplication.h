#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"
#include "engine/PluginCatalogue.h"
#include "ui/YouHostLookAndFeel.h"

#include <memory>

namespace youhost
{

class YouHostApplication : public juce::JUCEApplication
{
public:
    YouHostApplication();
    ~YouHostApplication() override;

    const juce::String getApplicationName() override;
    const juce::String getApplicationVersion() override;
    bool moreThanOneInstanceAllowed() override;

    void initialise(const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted(const juce::String& commandLine) override;

private:
    class MainWindow;

    void openWindow();

    std::unique_ptr<YouHostLookAndFeel> lookAndFeel_;
    std::unique_ptr<AppSettings> settings_;
    std::unique_ptr<AudioEngine> engine_;
    std::unique_ptr<MainWindow> mainWindow_;
    std::unique_ptr<ScanWorker> worker_;
    bool workerMode_ = false;
};

} // namespace youhost
