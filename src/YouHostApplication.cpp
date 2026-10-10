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
        setResizeLimits(960, 640, 4000, 2400);

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
        worker_->startFromCommandLine(commandLine);
        workerMode_ = true;
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
    if (mainWindow_ != nullptr || engine_ == nullptr || settings_ == nullptr)
        return;

    if (engine_->takeUncleanShutdown())
    {
        juce::AlertWindow::showOkCancelBox(
            juce::AlertWindow::WarningIcon,
            "A plugin may have crashed YouHost",
            engine_->uncleanPluginMessage(),
            "Leave them off",
            "Load them anyway",
            nullptr,
            juce::ModalCallbackFunction::create([this](int result)
            {
                if (engine_ != nullptr)
                    engine_->acceptCrashChoice(result == 1);
                if (mainWindow_ == nullptr && engine_ != nullptr && settings_ != nullptr)
                    mainWindow_ = std::make_unique<MainWindow>(*engine_, *settings_);
            }));
        return;
    }

    mainWindow_ = std::make_unique<MainWindow>(*engine_, *settings_);
}

void YouHostApplication::shutdown()
{
    if (workerMode_)
    {
        worker_.reset();
        return;
    }

    if (engine_ != nullptr)
        engine_->prepareForQuit();
    mainWindow_.reset();
    engine_.reset();
    settings_.reset();
    juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    lookAndFeel_.reset();
}

void YouHostApplication::systemRequestedQuit()
{
    if (mainWindow_ != nullptr)
    {
        if (auto* main = dynamic_cast<MainComponent*>(mainWindow_->getContentComponent()))
        {
            main->requestApplicationQuit([this]
            {
                if (engine_ != nullptr)
                    engine_->prepareForQuit();
                mainWindow_.reset();
                quit();
            });
            return;
        }
    }
    if (engine_ != nullptr)
        engine_->prepareForQuit();
    mainWindow_.reset();
    quit();
}

} // namespace youhost
