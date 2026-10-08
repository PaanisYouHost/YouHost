#include "YouHostApplication.h"
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
        setResizeLimits(880, 640, 2600, 1700);

        const auto stored = settings_.loadWindowState();
        if (stored.isNotEmpty())
            restoreWindowStateFromString(stored);
        else
            centreWithSize(1180, 820);

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
    return false;
}

void YouHostApplication::initialise(const juce::String& commandLine)
{
    juce::ignoreUnused(commandLine);

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
