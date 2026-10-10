#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"
#include "ui/CpuWindow.h"
#include "ui/DropoutWindow.h"
#include "ui/LatencyReadout.h"
#include "ui/MeterGrid.h"
#include "ui/PluginPage.h"
#include "ui/ScannerWindow.h"
#include "ui/TimelineView.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>

namespace youhost
{

class BitDepthSlot;
class RecordLockButton;

class MainComponent : public juce::Component,
                      private juce::Timer,
                      private juce::ApplicationCommandTarget
{
public:
    MainComponent(AudioEngine& engine, AppSettings& settings);
    ~MainComponent() override;

    void requestApplicationQuit(std::function<void()> quit);

    void paint(juce::Graphics& graphics) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    bool handleKey(const juce::KeyPress& key, juce::Component* originating);
    void refresh();
    void publishMeters(bool repaintLevels);
    void onPluginSlot(int channel);
    void setPeakMode(bool peak, bool fromUser);
    void setRmsReference(int db, bool fromUser);
    void hideDeviceTestTone();
    void toggleSetup();
    void showPage(int page);
    void toggleScanner();
    void toggleDropouts();
    void toggleCpu();
    void toggleLatency();
    void ensureLogo();
    class BrandMark;
    void openSession();
    void saveSession();
    void saveSessionAs();
    void importRecordings();
    void confirmClearTimeline();
    void openRecent(int index);
    void showFileMenu();
    void showHelp();
    void showGroupsMenu();
    void layoutMeters();
    void openStartup();
    void dismissStartup();
    void applyGlobalListen(ChannelListen mode);
    void promptForSession(const juce::String& title, std::function<void(bool placed)> then, bool clean = false);
    void runAfterUnsavedCheck(std::function<void()> action);
    void newSession();
    void requestRecord();
    void startRecordingIfReady();
    bool shortcutBlocked(juce::Component* originating) const;

    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
    bool perform(const juce::ApplicationCommandTarget::InvocationInfo& info) override;

    enum CommandIDs
    {
        saveCommand = 0x2101,
        saveAsCommand = 0x2102
    };

    AudioEngine& engine_;
    AppSettings& settings_;
    LatencyReadout latencyReadout_;
    MeterGrid meterGrid_;
    TimelineView timeline_;
    PluginPage pluginPage_;
    ScannerWindow scanner_;
    DropoutWindow dropouts_;
    CpuWindow cpu_;
    class FileMenu;
    std::unique_ptr<FileMenu> fileMenu_;
    juce::TooltipWindow tooltipWindow_ { this, 700 };

    juce::TextButton recorderButton_ { "1 REC" };
    juce::TextButton pluginsButton_ { "2 HOST" };
    juce::TextButton scannerButton_ { "SCAN" };
    juce::TextButton prevButton_ { "Prev" };
    juce::TextButton nextButton_ { "Next" };
    juce::TextButton stopButton_ { "Stop" };
    juce::TextButton playButton_ { "Play" };
    juce::TextButton recButton_ { "Record" };
    juce::Label timeLabel_;
    juce::Label modeLabel_;
    juce::TextButton helpButton_ { "?" };
    juce::TextButton rmsButton_ { "RMS" };
    juce::TextButton peakButton_ { "Peak" };
    juce::Label referenceLabel_ { {}, "RMS 0" };
    juce::ComboBox referenceBox_;
    juce::TextButton clearClipsButton_ { "Clear clips" };
    juce::TextButton newButton_ { "New" };
    juce::TextButton openButton_ { "Open" };
    juce::TextButton saveButton_ { "Save" };
    juce::TextButton saveAsButton_ { "Save As" };
    juce::TextButton fileButton_ { "File" };
    juce::TextButton dropoutsButton_ { "3 DROPOUTS" };
    juce::TextButton cpuButton_ { "4 CPU" };
    juce::TextButton setupButton_ { "Audio setup" };
    juce::TextButton latencyButton_ { "5 LATENCY" };
    juce::TextButton retryButton_ { "Retry" };
    juce::TextButton groupButton_ { "Group" };
    juce::TextButton allButton_ { "All" };
    juce::TextButton hideButton_ { "Hide" };
    juce::TextButton globalRecButton_ { "ALL REC" };
    juce::TextButton globalInputButton_ { "ALL INPUT" };
    juce::TextButton globalOffButton_ { "ALL OFF" };
    std::unique_ptr<RecordLockButton> recordLock_;
    std::function<void(bool)> afterCopy_;
    bool copyWasRunning_ = false;
    bool copyFailedSeen_ = false;
    bool lockLayout_ = false;
    juce::Label latencyLabel_;
    juce::Viewport meterViewport_;
    MeterScaleRail leftScale_;
    MeterScaleRail rightScale_;
    juce::AudioDeviceSelectorComponent deviceSelector_;
    std::unique_ptr<BitDepthSlot> bitDepthSlot_;
    std::unique_ptr<juce::DocumentWindow> setupWindow_;
    std::unique_ptr<juce::DocumentWindow> latencyWindow_;
    std::unique_ptr<juce::DocumentWindow> helpWindow_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    struct KeyProxy;
    std::unique_ptr<KeyProxy> keys_;
    juce::Component* keyTarget_ = nullptr;
    juce::ApplicationCommandManager commandManager_;
    std::unique_ptr<juce::DocumentWindow> startupWindow_;
    std::unique_ptr<juce::DocumentWindow> placeWindow_;
    std::unique_ptr<juce::DocumentWindow> copyWindow_;
    double copyProgressValue_ = 0.0;
    void chooseSaveAsDestination(std::function<void(bool saved)> then = {});
    void syncCopyProgress();

    bool heavyPaintSuspended_ = false;
    bool heavyPaintHeld_ = false;
    bool showPeak_ = false;
    int rmsReferenceDb_ = kDefaultRmsReferenceDb;
    int page_ = 1;
    int hintPage_ = -1;
    int pollDivider_ = 0;
    bool logoTried_ = false;
    juce::Image logo_;
    std::unique_ptr<BrandMark> brand_;
    juce::Rectangle<int> fileRule_;
    juce::Rectangle<int> transportRule_;
    juce::Rectangle<int> scanTick_;

    juce::Rectangle<int> titleArea_;
    juce::Rectangle<int> statusArea_;
    juce::Rectangle<int> bannerArea_;
    juce::Rectangle<int> deviceLostArea_;
    juce::Rectangle<int> recordLockArea_;
    int timelineHeight_ = 0;
    juce::Rectangle<int> hintArea_;
    juce::Rectangle<int> bridgeArea_;
    juce::Rectangle<int> laidOutBridge_;
    int laidOutNatural_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace youhost
