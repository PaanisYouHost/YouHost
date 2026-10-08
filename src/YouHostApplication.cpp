#include "YouHostApplication.h"
#include "engine/PluginCatalogue.h"
#include "ui/MainComponent.h"
#include "ui/Theme.h"

namespace youhost
{

class YouHostApplication::MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(AudioEngine& engine, AppSettings& settings)
        : DocumentWindow("YouHost", theme::background, DocumentWindow::allButtons),
          settings_(settings)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(engine, settings), true);
        setResizable(true, false);
        setResizeLimits(960, 720, 2600, 1800);

        const auto stored = settings_.loadWindowState();
        if (stored.isNotEmpty())
            restoreWindowStateFromString(stored);
        else
            centreWithSize(1280, 900);

        setVisible(true);
    }

    ~MainWindow() override
    {
        settings_.saveWindowState(getWindowStateAsString());
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

private:
    AppSettings& settings_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

YouHostApplication::YouHostApplication() = default;

YouHostApplication::~YouHostApplication() = default;

const juce::String YouHostApplication::getApplicationName()
{
    return "YouHost";
}

const juce::String YouHostApplication::getApplicationVersion()
{
    return YOUHOST_VERSION;
}

bool YouHostApplication::moreThanOneInstanceAllowed()
{
    // The scanner is a second copy of this executable. The single-instance
    // guard runs before initialise, so the child has to be allowed through.
    const auto arguments = juce::JUCEApplicationBase::getCommandLineParameterArray();
    for (const auto& argument : arguments)
        if (argument.contains("YouHostScan:"))
            return true;
    return false;
}

void YouHostApplication::initialise(const juce::String& commandLine)
{
    if (isScanWorkerCommandLine(commandLine))
    {
        worker_ = std::make_unique<ScanWorker>();
        if (worker_->initialiseFromCommandLine(commandLine, "YouHostScan", 15000))
        {
            workerMode_ = true;
            return;
        }

        worker_.reset();
        quit();
        return;
    }

    lookAndFeel_ = std::make_unique<YouHostLookAndFeel>();
    juce::LookAndFeel::setDefaultLookAndFeel(lookAndFeel_.get());
    settings_ = std::make_unique<AppSettings>();
    engine_ = std::make_unique<AudioEngine>(*settings_);

    juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                      [this](bool granted)
                                      {
                                          juce::MessageManager::callAsync([this, granted]
                                          {
                                              if (engine_ == nullptr)
                                                  return;
                                              engine_->start(granted);
                                              openWindow();
                                          });
                                      });
}

void YouHostApplication::openWindow()
{
    if (mainWindow_ == nullptr && engine_ != nullptr && settings_ != nullptr)
        mainWindow_ = std::make_unique<MainWindow>(*engine_, *settings_);
}

void YouHostApplication::shutdown()
{
    if (workerMode_)
    {
        worker_.reset();
        return;
    }

    mainWindow_.reset();
    engine_.reset();
    settings_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    lookAndFeel_.reset();
}

void YouHostApplication::systemRequestedQuit()
{
    quit();
}

} // namespace youhost
