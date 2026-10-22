#pragma once

#include "AppSettings.h"
#include "engine/AudioEngine.h"
#include "ui/CpuWindow.h"
#include "ui/DropoutWindow.h"
#include "ui/LatencyReadout.h"
#include "ui/MeterGrid.h"
#include "ui/PluginPage.h"
#include "ui/ScannerWindow.h"
#include "ui/SyncLostOverlay.h"
#include "ui/TimelineView.h"
#include "engine/Shortcuts.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace youhost
{

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
    void mouseDown(const juce::MouseEvent& event) override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    bool handleKey(const juce::KeyPress& key, juce::Component* originating);
    bool runShortcut(ShortcutId id);
    void refresh();
    void placeSyncOverlay();
    void placeSyncDialog();
    void showSyncRestoredDialog();
    void publishMeters(bool repaintLevels);
    void onPluginSlot(int channel);
    void setPeakMode(bool peak, bool fromUser);
    void setRmsReference(int db, bool fromUser);
    void toggleSetup();
    void showPage(int page);
    void toggleScanner();
    void toggleDropouts();
    void toggleCpu();
    void toggleLatency();
    void toggleMarkers();
    enum class MonitorWindow
    {
        dropouts,
        cpu,
        latency
    };
    void raiseMonitorWindows(MonitorWindow front);
    void keepMonitorWindowsOpen(MonitorWindow front);
    void ensureLogo();
    class BrandMark;
    void openSession();
    void saveSession();
    void saveSessionAs();
    void importRecordings();
    void openRecent(int index);
    void showOpenMenu();
    void showHelp();
    void layoutMeters();
    void openStartup();
    void dismissStartup();
    void applyGlobalListen(ChannelListen mode);
    void requestChannelListenCycle(int channel);
    void confirmListenChange(const juce::String& headline, std::function<void()> apply);
    void promptForSession(const juce::String& title, std::function<void(bool placed)> then, bool clean = false);
    void runAfterUnsavedCheck(std::function<void()> action);
    void newSession();
    void requestRecord();
    void startRecordingIfReady();
    bool shortcutBlocked(juce::Component* originating) const;
    std::optional<std::int64_t> markerPlaySample() const;
    void selectMarker(int index);
    void deleteMarkerIndex(int index);

    juce::ApplicationCommandTarget* getNextCommandTarget() override;
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
    bool perform(const juce::ApplicationCommandTarget::InvocationInfo& info) override;

    enum CommandIDs
    {
        saveCommand = kSaveCommandId,
        saveAsCommand = kSaveAsCommandId,
        selectAllCommand = kSelectAllCommandId,
        makeGroupCommand = kMakeGroupCommandId,
        undoCommand = kUndoCommandId,
        redoCommand = kRedoCommandId
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

    juce::TextButton recorderButton_ { "Rec 1" };
    juce::TextButton pluginsButton_ { "Host 2" };
    juce::TextButton scannerButton_ { "Scan" };
    juce::TextButton prevButton_ { "Back" };
    juce::TextButton nextButton_ { "Ahead" };
    juce::TextButton stopButton_ { "Halt" };
    juce::TextButton playButton_ { "Roll" };
    juce::TextButton recButton_ { "Arm" };
    juce::Label timeLabel_;
    juce::Label modeLabel_;
    juce::TextButton helpButton_ { "?" };
    juce::TextButton rmsButton_ { "Avg" };
    juce::TextButton peakButton_ { "Hold" };
    juce::Label referenceLabel_ { {}, "Line" };
    juce::ComboBox referenceBox_;
    juce::TextButton clearClipsButton_ { "Unclip" };
    juce::TextButton newButton_ { "Blank" };
    juce::TextButton openButton_ { "Load" };
    juce::TextButton saveButton_ { "Keep" };
    juce::TextButton saveAsButton_ { "Copy as" };
    juce::TextButton dropoutsButton_ { "Dropouts 3" };
    juce::TextButton cpuButton_ { "Cpu 4" };
    juce::TextButton setupButton_ { "Card" };
    juce::TextButton latencyButton_ { "Latency 5" };
    juce::TextButton markersButton_ { "Markers 6" };
    juce::TextButton markerButton_ { "Cue" };
    SyncLostOverlay syncOverlay_;
    SyncRestoredDialog syncDialog_;
    juce::TextButton retryButton_ { "Ask mic" };
    juce::TextButton groupButton_ { "Gang" };
    juce::TextButton allButton_ { "Full" };
    juce::TextButton hideButton_ { "Fold" };
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
    std::unique_ptr<juce::DocumentWindow> setupWindow_;
    std::unique_ptr<juce::DocumentWindow> latencyWindow_;
    std::unique_ptr<juce::DocumentWindow> helpWindow_;
    std::unique_ptr<juce::DocumentWindow> markersWindow_;
    int markerRevisionSeen_ = -1;
    int selectedMarkerIndex_ = -1;
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
    void layoutStatusLine();

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
    juce::Rectangle<int> cpuStatusArea_;
    juce::Font statusFont_ { juce::FontOptions(13.0f) };
    int lastCpuPercent_ = -1;
    std::string lastStatusWithoutCpu_;
    juce::Rectangle<int> bannerArea_;
    juce::Rectangle<int> recordLockArea_;
    juce::Rectangle<int> recordArmArea_;
    bool armHintVisible_ = false;
    int timelineHeight_ = 0;
    juce::Rectangle<int> hintArea_;
    juce::Rectangle<int> bridgeArea_;
    juce::Rectangle<int> laidOutBridge_;
    int laidOutNatural_ = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace youhost
