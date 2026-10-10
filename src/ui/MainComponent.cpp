#include "MainComponent.h"
#include "ChannelMenu.h"
#include "Theme.h"
#include "WindowMemory.h"
#include "engine/HostLimits.h"
#include "engine/LatencyMath.h"
#include "engine/MeterScale.h"
#include "engine/SessionFiles.h"
#include "engine/Shortcuts.h"

#include <vector>

namespace youhost
{

class BitDepthSlot : public juce::Component
{
public:
    BitDepthSlot()
    {
        setComponentID("youhost-bit-depth");
        addAndMakeVisible(box_);
        // Same attachment as JUCE's "Sample rate:" row: the label sits to the
        // left of the combo, and the combo itself uses the standard row bounds.
        label_.setText("Bit depth:", juce::dontSendNotification);
        label_.attachToComponent(this, true);
        box_.addItem("16-bit", 16);
        box_.addItem("24-bit", 24);
        box_.addItem("32-bit float", 32);
        box_.setTooltip("Bit depth for the next take. 24-bit is the default. 32-bit is float. A take that is already recording keeps its depth.");
    }

    void resized() override
    {
        box_.setBounds(getLocalBounds());
    }

    int selectedId() const { return box_.getSelectedId(); }

    void setSelectedId(int id)
    {
        if (box_.getSelectedId() != id)
            box_.setSelectedId(id, juce::dontSendNotification);
    }

    std::function<void(int)> onChange;

private:
    void changeCallback()
    {
        if (onChange != nullptr && box_.getSelectedId() > 0)
            onChange(box_.getSelectedId());
    }

    juce::Label label_;
    juce::ComboBox box_;

public:
    void bind()
    {
        box_.onChange = [this] { changeCallback(); };
    }
};

namespace
{


void hideTestButtons(juce::Component& component)
{
    for (auto* child : component.getChildren())
    {
        if (child == nullptr)
            continue;
        if (auto* button = dynamic_cast<juce::TextButton*>(child))
        {
            if (button->getButtonText() == "Test")
            {
                button->setVisible(false);
                button->setEnabled(false);
                button->onClick = nullptr;
            }
        }
        hideTestButtons(*child);
    }
}

int referenceIdFor(int db)
{
    if (db == -14)
        return 1;
    if (db == -18)
        return 2;
    return 3;
}

int referenceDbFor(int id)
{
    if (id == 1)
        return -14;
    if (id == 2)
        return -18;
    return kDefaultRmsReferenceDb;
}

void showRadio(juce::TextButton& rms, juce::TextButton& peak, bool showPeak)
{
    rms.setToggleState(! showPeak, juce::dontSendNotification);
    peak.setToggleState(showPeak, juce::dontSendNotification);
}

void quiet(juce::Button& button)
{
    button.setMouseClickGrabsKeyboardFocus(false);
    button.setWantsKeyboardFocus(false);
}

class FloatWindow : public juce::DocumentWindow
{
public:
    FloatWindow(const juce::String& title,
                juce::Component& content,
                AppSettings& settings,
                const juce::String& key,
                int width,
                int height,
                int minWidth,
                int minHeight)
        : juce::DocumentWindow(title, theme::panel, juce::DocumentWindow::closeButton),
          settings_(settings),
          key_(key)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content, false);
        prepareRememberedWindow(*this, settings_, key_, width, height, minWidth, minHeight);
        setVisible(false);
    }

    ~FloatWindow() override
    {
        saveRememberedWindow(*this, settings_, key_);
    }

    void closeButtonPressed() override
    {
        saveRememberedWindow(*this, settings_, key_);
        setVisible(false);
    }

private:
    AppSettings& settings_;
    juce::String key_;
};

void attachBitDepthSlot(juce::AudioDeviceSelectorComponent& selector, BitDepthSlot& slot)
{
    juce::Component* panel = nullptr;
    std::function<void(juce::Component&)> walk;
    walk = [&](juce::Component& node)
    {
        for (auto* child : node.getChildren())
        {
            if (child == nullptr || child == &slot)
                continue;
            if (auto* label = dynamic_cast<juce::Label*>(child))
                if (label->getText() == "Sample rate:")
                    panel = label->getParentComponent();
            if (panel == nullptr)
                walk(*child);
        }
    };
    walk(selector);
    if (panel == nullptr)
        return;
    if (slot.getParentComponent() != panel)
    {
        if (auto* old = slot.getParentComponent())
            old->removeChildComponent(&slot);
        panel->addAndMakeVisible(&slot);
        panel->resized();
    }
}

class SetupWindow : public juce::DocumentWindow
{
public:
    SetupWindow(juce::AudioDeviceSelectorComponent& selector, AppSettings& settings)
        : juce::DocumentWindow("Audio setup", theme::background, juce::DocumentWindow::closeButton),
          settings_(settings),
          content_(selector)
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content_, false);
        prepareRememberedWindow(*this, settings_, "windowSetup", 720, 420, 560, 320);
        setVisible(false);
    }

    ~SetupWindow() override
    {
        saveRememberedWindow(*this, settings_, "windowSetup");
    }

    void closeButtonPressed() override
    {
        saveRememberedWindow(*this, settings_, "windowSetup");
        setVisible(false);
    }

private:
    class Content : public juce::Component
    {
    public:
        explicit Content(juce::AudioDeviceSelectorComponent& selector)
            : selector_(selector)
        {
            addAndMakeVisible(viewport_);
            viewport_.setViewedComponent(&selector_, false);
            viewport_.setScrollBarsShown(true, true);
        }

        ~Content() override
        {
            viewport_.setViewedComponent(nullptr, false);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(12, 8);
            viewport_.setBounds(area);
            const int width = std::max(520, viewport_.getMaximumVisibleWidth());
            selector_.setSize(width, std::max(selector_.getHeight(), 160));
        }

    private:
        juce::AudioDeviceSelectorComponent& selector_;
        juce::Viewport viewport_;
    };

    AppSettings& settings_;
    Content content_;
};

juce::String sessionNameProblem(const juce::File& parent, const juce::String& rawName, juce::File& folderOut)
{
    if (! parent.isDirectory())
        return "That location is not available. The drive may be missing or unmounted.";
    if (! parent.hasWriteAccess())
        return "That location is not writable.";

    const auto clean = sanitiseSessionName(rawName.toStdString());
    folderOut = parent.getChildFile(juce::String(clean));
    if (folderOut.existsAsFile())
        return folderOut.getFullPathName() + " is a file. Choose another name.";
    if (folderOut.getChildFile(kSessionFileName).existsAsFile())
        return "That folder already has a session. Open it, or choose another name.";
    return {};
}

class SessionPlaceWindow : public juce::DocumentWindow
{
public:
    SessionPlaceWindow(const juce::String& title,
                       juce::File parent,
                       juce::String parentNote,
                       std::function<void(juce::File folder, bool internalDisk)> done)
        : juce::DocumentWindow(title, theme::background, juce::DocumentWindow::closeButton),
          content_(std::move(parent), std::move(parentNote), std::move(done))
    {
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content_, false);
        setResizable(true, false);
        setResizeLimits(480, 260, 900, 520);
        centreWithSize(560, 300);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        content_.cancel();
    }

    ~SessionPlaceWindow() override
    {
        if (isCurrentlyModal())
            exitModalState(0);
    }

private:
    class Content : public juce::Component
    {
    public:
        Content(juce::File parent, juce::String parentNote, std::function<void(juce::File, bool)> done)
            : parent_(std::move(parent)),
              done_(std::move(done))
        {
            addAndMakeVisible(nameLabel_);
            addAndMakeVisible(name_);
            addAndMakeVisible(locationLabel_);
            addAndMakeVisible(location_);
            addAndMakeVisible(note_);
            addAndMakeVisible(browse_);
            addAndMakeVisible(create_);
            addAndMakeVisible(internal_);
            addAndMakeVisible(cancel_);
            nameLabel_.setText("Session name", juce::dontSendNotification);
            locationLabel_.setText("Location", juce::dontSendNotification);
            name_.setText(suggestedNewSessionName(parent_), juce::dontSendNotification);
            name_.setSelectAllWhenFocused(true);
            location_.setJustificationType(juce::Justification::centredLeft);
            note_.setJustificationType(juce::Justification::centredLeft);
            note_.setColour(juce::Label::textColourId, theme::amber);
            note_.setText(parentNote, juce::dontSendNotification);
            refreshLocation();
            browse_.onClick = [this] { browse(); };
            create_.onClick = [this] { create(); };
            internal_.onClick = [this] { confirmInternal(); };
            cancel_.onClick = [this] { cancel(); };
            for (auto* button : { &browse_, &create_, &internal_, &cancel_ })
            {
                button->setMouseClickGrabsKeyboardFocus(false);
                button->setWantsKeyboardFocus(false);
            }
        }

        void cancel()
        {
            if (done_ == nullptr)
                return;
            auto done = std::move(done_);
            done(juce::File(), false);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16, 14);
            auto row = area.removeFromTop(28);
            nameLabel_.setBounds(row.removeFromLeft(110));
            name_.setBounds(row);
            area.removeFromTop(8);
            row = area.removeFromTop(28);
            locationLabel_.setBounds(row.removeFromLeft(110));
            browse_.setBounds(row.removeFromRight(96).reduced(4, 0));
            location_.setBounds(row);
            area.removeFromTop(4);
            note_.setBounds(area.removeFromTop(36));
            area.removeFromTop(8);
            row = area.removeFromTop(32);
            create_.setBounds(row.removeFromLeft(150).reduced(0, 2));
            row.removeFromLeft(8);
            internal_.setBounds(row.removeFromLeft(140).reduced(0, 2));
            cancel_.setBounds(row.removeFromRight(100).reduced(0, 2));
        }

    private:
        void refreshLocation()
        {
            location_.setText(parent_.getFullPathName(), juce::dontSendNotification);
        }

        void browse()
        {
            if (chooser_ != nullptr)
                return;
            chooser_ = std::make_unique<juce::FileChooser>("Choose a Drive or Folder", parent_, juce::String(), true);
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe = juce::Component::SafePointer<Content>(this)](const juce::FileChooser& chooser)
                                  {
                                      const auto chosen = chooser.getResult();
                                      juce::MessageManager::callAsync([safe, chosen]
                                      {
                                          if (safe == nullptr)
                                              return;
                                          safe->chooser_.reset();
                                          if (chosen.isDirectory())
                                          {
                                              safe->parent_ = chosen;
                                              safe->note_.setText({}, juce::dontSendNotification);
                                              safe->refreshLocation();
                                          }
                                      });
                                  });
        }

        void create()
        {
            juce::File folder;
            const auto problem = sessionNameProblem(parent_, name_.getText(), folder);
            if (problem.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Cannot create the session",
                                                       problem);
                return;
            }
            if (done_ == nullptr)
                return;
            auto done = std::move(done_);
            done(folder, false);
        }

        void confirmInternal()
        {
            juce::AlertWindow::showOkCancelBox(
                juce::MessageBoxIconType::WarningIcon,
                "Session on the internal disk?",
                "This stores the session in the Music folder on the internal disk. Recordings usually belong on an external drive.",
                "Use internal disk",
                "Cancel",
                nullptr,
                juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<Content>(this)](int result)
                {
                    if (result != 1 || safe == nullptr || safe->done_ == nullptr)
                        return;
                    auto done = std::move(safe->done_);
                    done(juce::File(), true);
                }));
        }

        juce::File parent_;
        std::function<void(juce::File, bool)> done_;
        std::unique_ptr<juce::FileChooser> chooser_;
        juce::Label nameLabel_;
        juce::TextEditor name_;
        juce::Label locationLabel_;
        juce::Label location_;
        juce::Label note_;
        juce::TextButton browse_ { "Browse..." };
        juce::TextButton create_ { "Create session" };
        juce::TextButton internal_ { "Internal disk..." };
        juce::TextButton cancel_ { "Cancel" };
    };

    Content content_;
};

class StartupWindow : public juce::DocumentWindow
{
public:
    StartupWindow(AudioEngine& engine, std::function<void()> onDone)
        : juce::DocumentWindow("Start session", theme::background, juce::DocumentWindow::closeButton),
          content_(engine, [this] { finish(); })
    {
        onDone_ = std::move(onDone);
        setUsingNativeTitleBar(true);
        setContentNonOwned(&content_, false);
        setResizable(true, false);
        setResizeLimits(640, 520, 1400, 1100);
        centreWithSize(780, 640);
        setVisible(true);
    }

    ~StartupWindow() override
    {
        if (isCurrentlyModal())
            exitModalState(0);
    }

    void closeButtonPressed() override
    {
        finish();
    }

private:
    void finish()
    {
        if (finished_)
            return;
        finished_ = true;
        if (onDone_)
            onDone_();
    }

    class Content : public juce::Component,
                    private juce::Timer
    {
    public:
        Content(AudioEngine& engine, std::function<void()> onDone)
            : engine_(engine),
              onDone_(std::move(onDone)),
              selector_(engine.deviceManager(), 0, 0, 0, 0, false, false, false, false),
              parent_(engine.defaultSessionParent())
        {
            addAndMakeVisible(intro_);
            addAndMakeVisible(deviceViewport_);
            addAndMakeVisible(nameLabel_);
            addAndMakeVisible(name_);
            addAndMakeVisible(locationLabel_);
            addAndMakeVisible(location_);
            addAndMakeVisible(note_);
            addAndMakeVisible(browse_);
            addAndMakeVisible(create_);
            addAndMakeVisible(open_);
            addAndMakeVisible(internal_);
            addAndMakeVisible(recentLabel_);
            addAndMakeVisible(recentViewport_);
            intro_.setText("Choose the interface, sample rate, and buffer size, then create a session or open one. "
                           "Recordings usually go on an external drive. The interface opens with all channels. "
                           "Channel use is chosen only with the REC, INPUT, and OFF buttons.",
                           juce::dontSendNotification);
            intro_.setJustificationType(juce::Justification::topLeft);
            nameLabel_.setText("Session name", juce::dontSendNotification);
            locationLabel_.setText("Location", juce::dontSendNotification);
            recentLabel_.setText("Open recent", juce::dontSendNotification);
            name_.setText(suggestedNewSessionName(parent_), juce::dontSendNotification);
            location_.setJustificationType(juce::Justification::centredLeft);
            note_.setColour(juce::Label::textColourId, theme::amber);
            note_.setText(engine_.missingSessionParentNote(), juce::dontSendNotification);
            refreshLocation();
            deviceViewport_.setViewedComponent(&selector_, false);
            deviceViewport_.setScrollBarsShown(true, false);
            selector_.setItemHeight(22);
            recentViewport_.setViewedComponent(&recent_, false);
            recentViewport_.setScrollBarsShown(true, false);
            browse_.onClick = [this] { browse(); };
            create_.onClick = [this] { create(); };
            open_.onClick = [this] { openExisting(); };
            internal_.onClick = [this] { confirmInternal(); };
            rebuildRecent();
            hideTestButtons(selector_);
            bitSlot_.bind();
            bitSlot_.setSelectedId(engine_.wavBitDepth());
            bitSlot_.onChange = [this](int bits) { engine_.setWavBitDepth(bits, true); };
            attachBitDepthSlot(selector_, bitSlot_);
            startTimerHz(4);
        }

        ~Content() override
        {
            stopTimer();
            deviceViewport_.setViewedComponent(nullptr, false);
            recentViewport_.setViewedComponent(nullptr, false);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced(16, 12);
            intro_.setBounds(area.removeFromTop(68));
            area.removeFromTop(6);
            deviceViewport_.setBounds(area.removeFromTop(168));
            const int width = std::max(520, deviceViewport_.getMaximumVisibleWidth());
            selector_.setSize(width, std::max(selector_.getHeight(), 140));
            area.removeFromTop(10);
            auto row = area.removeFromTop(28);
            nameLabel_.setBounds(row.removeFromLeft(110));
            name_.setBounds(row);
            area.removeFromTop(6);
            row = area.removeFromTop(28);
            locationLabel_.setBounds(row.removeFromLeft(110));
            browse_.setBounds(row.removeFromRight(96).reduced(4, 0));
            location_.setBounds(row);
            note_.setBounds(area.removeFromTop(22));
            area.removeFromTop(6);
            row = area.removeFromTop(32);
            create_.setBounds(row.removeFromLeft(150).reduced(0, 2));
            row.removeFromLeft(8);
            open_.setBounds(row.removeFromLeft(140).reduced(0, 2));
            row.removeFromLeft(8);
            internal_.setBounds(row.removeFromLeft(140).reduced(0, 2));
            area.removeFromTop(8);
            recentLabel_.setBounds(area.removeFromTop(20));
            recentViewport_.setBounds(area);
            layoutRecent();
        }

    private:
        void timerCallback() override
        {
            hideTestButtons(selector_);
            attachBitDepthSlot(selector_, bitSlot_);
            bitSlot_.setSelectedId(engine_.wavBitDepth());
            auto note = engine_.missingSessionParentNote();
            const auto deviceNote = engine_.startupDeviceNote();
            if (deviceNote.isNotEmpty())
            {
                if (note.isNotEmpty())
                    note << " " << deviceNote;
                else
                    note = deviceNote;
            }
            note_.setText(note, juce::dontSendNotification);
        }

        void refreshLocation()
        {
            location_.setText(parent_.getFullPathName(), juce::dontSendNotification);
        }

        void layoutRecent()
        {
            const int width = std::max(200, recentViewport_.getMaximumVisibleWidth());
            int y = 0;
            for (auto& button : recentButtons_)
            {
                button->setBounds(0, y, width, 26);
                y += 28;
            }
            if (recentButtons_.empty())
            {
                emptyRecent_.setBounds(0, 0, width, 28);
                y = 28;
            }
            recent_.setSize(width, std::max(y, recentViewport_.getMaximumVisibleHeight()));
        }

        void rebuildRecent()
        {
            recentButtons_.clear();
            emptyRecent_.setText("No recent sessions.", juce::dontSendNotification);
            emptyRecent_.setColour(juce::Label::textColourId, theme::dim);
            const auto sessions = engine_.recentSessions();
            if (sessions.isEmpty())
            {
                recent_.addAndMakeVisible(emptyRecent_);
                return;
            }
            for (int index = 0; index < sessions.size() && index < 10; ++index)
            {
                const auto file = juce::File(sessions[index]);
                auto button = std::make_unique<juce::TextButton>(file.getFileName());
                button->setTooltip(file.getFullPathName());
                button->setMouseClickGrabsKeyboardFocus(false);
                button->onClick = [this, file]
                {
                    if (engine_.loadSessionFrom(file))
                        finish();
                    else
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                               "Could not open that session",
                                                               engine_.sessionMessage());
                };
                recent_.addAndMakeVisible(*button);
                recentButtons_.push_back(std::move(button));
            }
        }

        void browse()
        {
            if (chooser_ != nullptr)
                return;
            chooser_ = std::make_unique<juce::FileChooser>("Choose a Drive or Folder", parent_, juce::String(), true);
            chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe = juce::Component::SafePointer<Content>(this)](const juce::FileChooser& chooser)
                                  {
                                      const auto chosen = chooser.getResult();
                                      juce::MessageManager::callAsync([safe, chosen]
                                      {
                                          if (safe == nullptr)
                                              return;
                                          safe->chooser_.reset();
                                          if (chosen.isDirectory())
                                          {
                                              safe->parent_ = chosen;
                                              safe->note_.setText({}, juce::dontSendNotification);
                                              safe->refreshLocation();
                                          }
                                      });
                                  });
        }

        void create()
        {
            juce::File folder;
            const auto problem = sessionNameProblem(parent_, name_.getText(), folder);
            if (problem.isNotEmpty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Cannot create the session",
                                                       problem);
                return;
            }
            if (engine_.placeNewSession(folder, false))
                finish();
            else
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                       "Cannot create the session",
                                                       engine_.sessionMessage());
        }

        void openExisting()
        {
            if (chooser_ != nullptr)
                return;
            chooser_ = std::make_unique<juce::FileChooser>("Open a Session",
                                                           engine_.suggestedSessionFolder(),
                                                           "*.youhost",
                                                           true);
            chooser_->launchAsync(juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::canSelectDirectories,
                                  [safe = juce::Component::SafePointer<Content>(this)](const juce::FileChooser& chooser)
                                  {
                                      const auto chosen = chooser.getResult();
                                      juce::MessageManager::callAsync([safe, chosen]
                                      {
                                          if (safe == nullptr)
                                              return;
                                          safe->chooser_.reset();
                                          if (safe == nullptr || chosen.getFullPathName().isEmpty())
                                              return;
                                          if (safe->engine_.loadSessionFrom(chosen))
                                              safe->finish();
                                          else
                                              juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                                                     "Could not open that session",
                                                                                     safe->engine_.sessionMessage());
                                      });
                                  });
        }

        void confirmInternal()
        {
            juce::AlertWindow::showOkCancelBox(
                juce::MessageBoxIconType::WarningIcon,
                "Session on the internal disk?",
                "This stores the session in the Music folder on the internal disk. Recordings usually belong on an external drive.",
                "Use internal disk",
                "Cancel",
                nullptr,
                juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<Content>(this)](int result)
                {
                    if (result != 1 || safe == nullptr)
                        return;
                    if (safe->engine_.createInternalSession())
                        safe->finish();
                    else
                        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                                               "Cannot create the session",
                                                               safe->engine_.sessionMessage());
                }));
        }

        void finish()
        {
            if (onDone_)
                onDone_();
        }

        AudioEngine& engine_;
        std::function<void()> onDone_;
        juce::AudioDeviceSelectorComponent selector_;
        BitDepthSlot bitSlot_;
        juce::File parent_;
        std::unique_ptr<juce::FileChooser> chooser_;
        juce::Label intro_;
        juce::Viewport deviceViewport_;
        juce::Label nameLabel_;
        juce::TextEditor name_;
        juce::Label locationLabel_;
        juce::Label location_;
        juce::Label note_;
        juce::TextButton browse_ { "Browse..." };
        juce::TextButton create_ { "Create session" };
        juce::TextButton open_ { "Open existing..." };
        juce::TextButton internal_ { "Internal disk..." };
        juce::Label recentLabel_;
        juce::Viewport recentViewport_;
        juce::Component recent_;
        juce::Label emptyRecent_;
        std::vector<std::unique_ptr<juce::TextButton>> recentButtons_;
    };

    std::function<void()> onDone_;
    bool finished_ = false;
    Content content_;
};

} // namespace

struct MainComponent::KeyProxy : juce::KeyListener
{
    explicit KeyProxy(MainComponent& ownerIn)
        : owner(ownerIn)
    {
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component* originating) override
    {
        return owner.handleKey(key, originating);
    }

    MainComponent& owner;
};

class MainComponent::FileMenu : public juce::MenuBarModel
{
public:
    explicit FileMenu(MainComponent& ownerIn)
        : owner(ownerIn)
    {
    }

    juce::StringArray getMenuBarNames() override
    {
        return { "File" };
    }

    juce::PopupMenu getMenuForIndex(int, const juce::String&) override
    {
        juce::PopupMenu menu;
        menu.addItem(1, "New");
        menu.addItem(2, "Open...");

        juce::PopupMenu recent;
        const auto sessions = owner.engine_.recentSessions();
        for (int index = 0; index < sessions.size() && index < 10; ++index)
        {
            juce::PopupMenu::Item item;
            item.itemID = 100 + index;
            item.text = juce::File(sessions[index]).getFileName();
            recent.addItem(item);
        }
        menu.addSubMenu("Open Recent", recent, ! sessions.isEmpty());
        menu.addItem(5, "Import Recording Folder...");
        menu.addSeparator();
        menu.addCommandItem(&owner.commandManager_, MainComponent::saveCommand);
        menu.addCommandItem(&owner.commandManager_, MainComponent::saveAsCommand);
        menu.addSeparator();
        menu.addItem(6, "Clear Timeline...");
        return menu;
    }

    void menuItemSelected(int id, int) override
    {
        if (id == MainComponent::saveCommand || id == MainComponent::saveAsCommand)
            return;
        if (id == 1)
            owner.newSession();
        else if (id == 2)
            owner.openSession();
        else if (id == 5)
            owner.importRecordings();
        else if (id == 6)
            owner.confirmClearTimeline();
        else if (id >= 100 && id < 110)
            owner.openRecent(id - 100);
        owner.refresh();
    }

private:
    MainComponent& owner;
};

class MainComponent::BrandMark : public juce::Component,
                                 public juce::SettableTooltipClient
{
public:
    BrandMark()
    {
        setTooltip("Email Ambient Audio");
        setMouseCursor(juce::MouseCursor::PointingHandCursor);
    }

    void setLogo(const juce::Image& image)
    {
        logo_ = image;
        repaint();
    }

    void paint(juce::Graphics& graphics) override
    {
        if (logo_.isValid())
        {
            graphics.setOpacity(0.45f);
            graphics.drawImageWithin(logo_, 0, 0, 120, getHeight(),
                                     juce::RectanglePlacement::xLeft | juce::RectanglePlacement::centred | juce::RectanglePlacement::onlyReduceInSize);
            return;
        }
        graphics.setColour(theme::text.withAlpha(0.38f));
        graphics.setFont(juce::Font(juce::FontOptions(11.0f)));
        graphics.drawText("Ambient Audio Oy", getLocalBounds(), juce::Justification::centredLeft, true);
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        if (! event.mouseWasClicked())
            return;
        juce::URL("mailto:mika.paananen@ambientaudio.fi?subject=YouHost").launchInDefaultBrowser();
    }

private:
    juce::Image logo_;
};

MainComponent::MainComponent(AudioEngine& engine, AppSettings& settings)
    : engine_(engine),
      settings_(settings),
      pluginPage_(engine, settings),
      scanner_(engine, settings),
      dropouts_(engine, settings),
      cpu_(engine, settings),
      deviceSelector_(engine.deviceManager(), 0, 0, 0, 0, false, false, false, false)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    keys_ = std::make_unique<KeyProxy>(*this);
    showPeak_ = false;
    rmsReferenceDb_ = settings_.loadRmsReferenceDb();
    engine_.setSessionMeters(false, rmsReferenceDb_);
    engine_.setMeterRestoreHandler([this](bool peak, int reference)
    {
        rmsReferenceDb_ = normaliseRmsReferenceDb(reference);
        referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
        setPeakMode(peak, false);
    });
    engine_.setPageRestoreHandler([this](int page) { showPage(page); });
    engine_.setTimelineStateProvider([this]
    {
        auto state = timeline_.viewState();
        state.height = timelineHeight_;
        return state;
    });
    engine_.setTimelineStateHandler([this](const SessionTimelineState& state)
    {
        timeline_.setViewState(state);
        if (state.height <= 0)
            return;
        timelineHeight_ = state.height;
        settings_.saveTimelineHeight(state.height);
        resized();
    });
    engine_.setPluginSlotHandler([this](int channel) { onPluginSlot(channel); });

    addAndMakeVisible(meterViewport_);
    addAndMakeVisible(leftScale_);
    addAndMakeVisible(rightScale_);
    addAndMakeVisible(timeline_);
    addAndMakeVisible(pluginPage_);
    addAndMakeVisible(recorderButton_);
    addAndMakeVisible(pluginsButton_);
    addAndMakeVisible(scannerButton_);
    addAndMakeVisible(cpuButton_);
    addAndMakeVisible(prevButton_);
    addAndMakeVisible(nextButton_);
    addAndMakeVisible(stopButton_);
    addAndMakeVisible(playButton_);
    addAndMakeVisible(recButton_);
    addAndMakeVisible(timeLabel_);
    addAndMakeVisible(modeLabel_);
    addAndMakeVisible(helpButton_);
    brand_ = std::make_unique<BrandMark>();
    addAndMakeVisible(*brand_);
    addAndMakeVisible(rmsButton_);
    addAndMakeVisible(peakButton_);
    addAndMakeVisible(referenceLabel_);
    addAndMakeVisible(referenceBox_);
    addAndMakeVisible(clearClipsButton_);
    addAndMakeVisible(newButton_);
    addAndMakeVisible(openButton_);
    addAndMakeVisible(saveButton_);
    addAndMakeVisible(saveAsButton_);
    addAndMakeVisible(fileButton_);
    addAndMakeVisible(dropoutsButton_);
    addAndMakeVisible(setupButton_);
    addAndMakeVisible(latencyButton_);
    addAndMakeVisible(retryButton_);
    addAndMakeVisible(groupButton_);
    addAndMakeVisible(allButton_);
    addAndMakeVisible(hideButton_);
    addAndMakeVisible(latencyLabel_);
    meterViewport_.setViewedComponent(&meterGrid_, false);
    meterViewport_.setScrollBarsShown(false, true);

    latencyWindow_ = std::make_unique<FloatWindow>("LATENCY", latencyReadout_, settings_, "windowLatency", 520, 420, 420, 320);
    if (keys_ != nullptr)
    {
        scanner_.addKeyListener(keys_.get());
        dropouts_.addKeyListener(keys_.get());
        cpu_.addKeyListener(keys_.get());
        latencyWindow_->addKeyListener(keys_.get());
    }
    setupWindow_ = std::make_unique<SetupWindow>(deviceSelector_, settings_);
    commandManager_.registerAllCommandsForTarget(this);
    commandManager_.setFirstCommandTarget(this);
    engine_.setGlobalKeyListener(commandManager_.getKeyMappings());
    latencyReadout_.setResetHandler([this] { engine_.resetDropouts(); });
    latencyReadout_.setGraphHandler([this] { toggleDropouts(); });
    latencyReadout_.setAlignHandler([this](int mode) { engine_.setAlignGroup(mode); });
    timeline_.setWaveformGainHandler([this](float gain) { engine_.setWaveformGain(gain); });
    fileMenu_ = std::make_unique<FileMenu>(*this);
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(fileMenu_.get());
#endif

    for (auto* button : { &recorderButton_, &pluginsButton_, &scannerButton_, &prevButton_, &nextButton_,
                          &stopButton_, &playButton_, &recButton_, &helpButton_, &rmsButton_, &peakButton_,
                          &clearClipsButton_, &newButton_, &openButton_, &saveButton_, &saveAsButton_, &fileButton_,
                          &dropoutsButton_, &cpuButton_, &setupButton_, &latencyButton_, &retryButton_, &groupButton_,
                          &allButton_, &hideButton_ })
        quiet(*button);

    for (auto* button : { &recorderButton_, &pluginsButton_, &scannerButton_, &dropoutsButton_, &cpuButton_, &latencyButton_,
                          &setupButton_, &groupButton_, &allButton_, &hideButton_ })
        button->setColour(juce::TextButton::buttonOnColourId, theme::buttonOn);

    recorderButton_.onClick = [this] { showPage(1); };
    pluginsButton_.onClick = [this] { showPage(2); };
    scannerButton_.onClick = [this] { toggleScanner(); };
    scannerButton_.setTooltip("SCAN (" + juce::String(shortcutChord(ShortcutId::scanner))
                              + "). Used when new plugins are installed.");
    cpuButton_.onClick = [this] { toggleCpu(); };
    cpuButton_.setTooltip("CPU (" + juce::String(shortcutChord(ShortcutId::cpu)) + "). Per-core usage and the audio callback.");
    prevButton_.onClick = [this] { engine_.transportJump(-1); };
    nextButton_.onClick = [this] { engine_.transportJump(1); };
    stopButton_.onClick = [this] { engine_.transportStop(); refresh(); };
    playButton_.onClick = [this]
    {
        if (engine_.transportView().mode != TransportMode::stopped)
            return;
        if (engine_.isRecordReady())
            requestRecord();
        else
            engine_.transportPlay();
    };
    recButton_.onClick = [this] { engine_.toggleRecordReady(); refresh(); };
    helpButton_.onClick = [this] { showHelp(); };
    newButton_.onClick = [this] { newSession(); };
    openButton_.onClick = [this] { openSession(); };
    saveButton_.onClick = [this] { saveSession(); };
    saveAsButton_.onClick = [this] { saveSessionAs(); };
    fileButton_.onClick = [this] { showFileMenu(); };
    dropoutsButton_.onClick = [this] { toggleDropouts(); };
    recorderButton_.setTooltip("Recorder page (" + juce::String(shortcutChord(ShortcutId::recPage)) + ").");
    pluginsButton_.setTooltip("Plugins page (" + juce::String(shortcutChord(ShortcutId::hostPage)) + ").");
    dropoutsButton_.setTooltip("Open or close the dropout timeline (" + juce::String(shortcutChord(ShortcutId::dropouts)) + ").");
    latencyButton_.setTooltip("Open or close the latency card (" + juce::String(shortcutChord(ShortcutId::latency)) + ").");
    fileButton_.setTooltip("New, Open, Open Recent, Save As, Import Recording Folder, and Clear Timeline.");
    groupButton_.setTooltip("Rename or recolor a group, or make a group from the selection.");
    groupButton_.onClick = [this] { showGroupsMenu(); };
    allButton_.setTooltip("Show every channel. Opens every group.");
    hideButton_.setTooltip("Fold every channel that belongs to a group. Channels with no group stay visible.");
    saveButton_.setTooltip("Save session.youhost (" + juce::String(shortcutChord(ShortcutId::save)) + ").");
    saveAsButton_.setTooltip("Copy the whole session, including audio, then continue in the new folder ("
                             + juce::String(shortcutChord(ShortcutId::saveAs)) + "). Not while recording.");
    latencyButton_.onClick = [this] { toggleLatency(); };

    prevButton_.setTooltip("Previous take (Left).");
    nextButton_.setTooltip("Next take (Right).");
    stopButton_.setTooltip("Stop (Space).");
    playButton_.setTooltip("Play the recorded takes (Space). If Record is armed, Play starts the take.");
    recButton_.setTooltip("Arm recording. The button blinks red. Press Play to start, or Cmd+Space to record immediately.");
    helpButton_.setTooltip(juce::String(shortcutHelpText()));
    allButton_.onClick = [this] { engine_.expandAllGroups(); refresh(); };
    hideButton_.onClick = [this] { engine_.hideGroupedChannels(); refresh(); };
    recButton_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff8d2430));
    recButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xffff3344));
    recButton_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    recButton_.setColour(juce::TextButton::textColourOnId, juce::Colours::white);
    playButton_.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff1f7a4a));
    playButton_.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff2fbf6e));
    playButton_.setColour(juce::TextButton::textColourOffId, juce::Colours::white);
    playButton_.setColour(juce::TextButton::textColourOnId, juce::Colours::white);

    timeLabel_.setFont(juce::Font(juce::FontOptions(28.0f).withStyle("Bold")));
    timeLabel_.setJustificationType(juce::Justification::centred);
    timeLabel_.setText("00:00:00", juce::dontSendNotification);
    modeLabel_.setFont(juce::Font(juce::FontOptions(13.0f).withStyle("Bold")));
    modeLabel_.setJustificationType(juce::Justification::centredLeft);
    latencyLabel_.setFont(juce::Font(juce::FontOptions(12.0f)));
    latencyLabel_.setJustificationType(juce::Justification::centredRight);
    latencyLabel_.setColour(juce::Label::textColourId, theme::dim);

    deviceSelector_.setItemHeight(22);

    rmsButton_.setRadioGroupId(1);
    peakButton_.setRadioGroupId(1);
    rmsButton_.setClickingTogglesState(true);
    peakButton_.setClickingTogglesState(true);
    showRadio(rmsButton_, peakButton_, false);
    referenceLabel_.setJustificationType(juce::Justification::centredRight);
    referenceLabel_.setFont(juce::Font(juce::FontOptions(13.0f)));
    referenceBox_.addItem("-14 dBFS", 1);
    referenceBox_.addItem("-18 dBFS", 2);
    referenceBox_.addItem("-20 dBFS", 3);
    referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
    referenceBox_.setTooltip("Line level for the RMS scale. 0 VU sits at this many dBFS. Meters are green below this point and yellow above it.");
    bitDepthSlot_ = std::make_unique<BitDepthSlot>();
    bitDepthSlot_->bind();
    bitDepthSlot_->setSelectedId(engine_.wavBitDepth());
    bitDepthSlot_->onChange = [this](int chosen)
    {
        if (chosen > 0)
            engine_.setWavBitDepth(chosen, true);
    };
    attachBitDepthSlot(deviceSelector_, *bitDepthSlot_);
    timelineHeight_ = settings_.loadTimelineHeight();

    rmsButton_.onClick = [this] { setPeakMode(false, true); };
    peakButton_.onClick = [this] { setPeakMode(true, true); };
    referenceBox_.onChange = [this] { setRmsReference(referenceDbFor(referenceBox_.getSelectedId()), true); };
    clearClipsButton_.onClick = [this] { engine_.requestClipClearAll(); };
    clearClipsButton_.setTooltip("Clear every latched clip mark");
    setupButton_.onClick = [this] { toggleSetup(); };
    setupButton_.setTooltip("Device, sample rate, and buffer size. The interface opens with all channels. REC, INPUT, and OFF choose what each channel does.");

    meterGrid_.setClearHandler([this](int channel)
    {
        if (engine_.clipFor(channel))
            engine_.requestClipClear(channel);
    });
    meterGrid_.setRecordHandler([this](int channel)
    {
        engine_.cycleChannelListen(channel);
    });
    meterGrid_.setChannelMenuHandler([this](int channel)
    {
        showChannelMenu(engine_, meterGrid_, channel, [this](int chosen) { meterGrid_.beginNameEdit(chosen); });
    });
    meterGrid_.setNameCommitHandler([this](int channel, juce::String name) { engine_.setChannelName(channel, name); });
    meterGrid_.setNameStepHandler([this](int channel, int direction)
    {
        const auto strips = engine_.displayStrips(engine_.visibleChannels());
        return adjacentVisibleChannel(strips.data(), static_cast<int>(strips.size()), channel, direction);
    });
    meterGrid_.setGroupMenuHandler([this](int group) { showGroupMenu(engine_, meterGrid_, group); });
    meterGrid_.setGroupRenameHandler([this](int group) { renameGroup(engine_, group); });
    meterGrid_.setGroupToggleHandler([this](int group) { engine_.toggleGroupCollapsed(group); });
    meterGrid_.setSelectHandler([this](int channel, bool extend, bool toggle)
    {
        engine_.selectChannel(channel, extend, toggle);
    });
    timeline_.setLocateHandler([this](std::int64_t sample) { engine_.transportLocate(sample); });
    timeline_.setLaneProvider([this](const std::function<void(const std::vector<TimelineLaneView>&)>& paint)
    {
        engine_.visitTimelineLanes(paint);
    });
    timeline_.setHeightHandler([this](int height)
    {
        const int clamped = juce::jlimit(72, std::max(160, getHeight() / 2), height);
        if (clamped == timelineHeight_)
            return;
        timelineHeight_ = clamped;
        settings_.saveTimelineHeight(clamped);
        resized();
    });

    retryButton_.onClick = [this]
    {
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
                                          [this](bool granted)
                                          {
                                              juce::MessageManager::callAsync([this, granted]
                                              {
                                                  engine_.start(granted);
                                                  resized();
                                                  refresh();
                                              });
                                          });
    };

    startTimerHz(30);
    hideDeviceTestTone();
    engine_.pluginCatalogue().startIfEmpty();
    showPage(1);
    refresh();
    juce::Component::SafePointer<MainComponent> safe(this);
    juce::MessageManager::callAsync([safe]
    {
        if (safe != nullptr)
            safe->openStartup();
    });
}

MainComponent::~MainComponent()
{
    engine_.setPluginSlotHandler(nullptr);
    stopTimer();
    copyWindow_.reset();
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    commandManager_.setFirstCommandTarget(nullptr);
    engine_.setGlobalKeyListener(nullptr);
    if (startupWindow_ != nullptr && startupWindow_->isCurrentlyModal())
        startupWindow_->exitModalState(0);
    startupWindow_.reset();
    if (placeWindow_ != nullptr && placeWindow_->isCurrentlyModal())
        placeWindow_->exitModalState(0);
    placeWindow_.reset();
    setupWindow_.reset();
    fileMenu_.reset();
    if (keyTarget_ != nullptr)
    {
        if (keys_ != nullptr)
            keyTarget_->removeKeyListener(keys_.get());
        keyTarget_->removeKeyListener(commandManager_.getKeyMappings());
    }
    if (keys_ != nullptr)
    {
        scanner_.removeKeyListener(keys_.get());
        dropouts_.removeKeyListener(keys_.get());
        if (latencyWindow_ != nullptr)
            latencyWindow_->removeKeyListener(keys_.get());
    }
    engine_.setMeterRestoreHandler(nullptr);
    engine_.setPageRestoreHandler(nullptr);
    engine_.setTimelineStateProvider(nullptr);
    engine_.setTimelineStateHandler(nullptr);
    latencyWindow_.reset();
}

void MainComponent::parentHierarchyChanged()
{
    auto* top = getTopLevelComponent();
    if (top == keyTarget_)
        return;
    if (keyTarget_ != nullptr)
    {
        if (keys_ != nullptr)
            keyTarget_->removeKeyListener(keys_.get());
        keyTarget_->removeKeyListener(commandManager_.getKeyMappings());
    }
    keyTarget_ = top;
    if (keyTarget_ != nullptr)
    {
        if (keys_ != nullptr)
            keyTarget_->addKeyListener(keys_.get());
        keyTarget_->addKeyListener(commandManager_.getKeyMappings());
    }
    grabKeyboardFocus();
}

bool textEditing(juce::Component* component)
{
    if (component == nullptr)
        return false;
    if (dynamic_cast<juce::TextEditor*>(component) != nullptr)
        return true;
    if (component->findParentComponentOfClass<juce::TextEditor>() != nullptr)
        return true;
    if (dynamic_cast<juce::AudioProcessorEditor*>(component) != nullptr)
        return true;
    if (component->findParentComponentOfClass<juce::AudioProcessorEditor>() != nullptr)
        return true;
    return false;
}

bool MainComponent::shortcutBlocked(juce::Component* originating) const
{
    return textEditing(originating) || textEditing(juce::Component::getCurrentlyFocusedComponent());
}

bool MainComponent::handleKey(const juce::KeyPress& key, juce::Component* originating)
{
    if (shortcutBlocked(originating))
        return false;

    KeyQuery query;
    query.shift = key.getModifiers().isShiftDown();
    query.command = key.getModifiers().isCommandDown();
    query.alt = key.getModifiers().isAltDown();
    query.character = static_cast<char>(juce::CharacterFunctions::toLowerCase(key.getTextCharacter()));
    const auto code = key.getKeyCode();
    if (key.isKeyCode(juce::KeyPress::spaceKey))
        query.kind = KeyKind::space;
    else if (key.isKeyCode(juce::KeyPress::leftKey))
        query.kind = KeyKind::left;
    else if (key.isKeyCode(juce::KeyPress::rightKey))
        query.kind = KeyKind::right;
    else if (code == static_cast<int>('['))
        query.kind = KeyKind::bracketLeft;
    else if (code == static_cast<int>(']'))
        query.kind = KeyKind::bracketRight;
    else if (code == static_cast<int>('R') || code == static_cast<int>('r') || query.character == 'r')
        query.kind = KeyKind::letterR;
    else
        query.kind = KeyKind::character;

    const auto matched = matchShortcut(query);
    if (! matched.has_value())
        return false;

    switch (*matched)
    {
        case ShortcutId::recPage:
            showPage(1);
            return true;
        case ShortcutId::hostPage:
            showPage(2);
            return true;
        case ShortcutId::dropouts:
            toggleDropouts();
            return true;
        case ShortcutId::cpu:
            toggleCpu();
            return true;
        case ShortcutId::latency:
            toggleLatency();
            return true;
        case ShortcutId::scanner:
            toggleScanner();
            return true;
        case ShortcutId::zoomIn:
            timeline_.zoomIn();
            return true;
        case ShortcutId::zoomOut:
            timeline_.zoomOut();
            return true;
        case ShortcutId::fit:
            timeline_.fitAll();
            return true;
        case ShortcutId::lanesTaller:
            timeline_.verticalZoomIn();
            return true;
        case ShortcutId::lanesShorter:
            timeline_.verticalZoomOut();
            return true;
        case ShortcutId::recordNow:
            requestRecord();
            return true;
        case ShortcutId::playOrStop:
        {
            const auto mode = engine_.transportView().mode;
            if (mode == TransportMode::stopped)
            {
                if (engine_.isRecordReady())
                    requestRecord();
                else
                    engine_.transportPlay();
            }
            else
                engine_.transportStop();
            return true;
        }
        case ShortcutId::previousTake:
            engine_.transportJump(-1);
            return true;
        case ShortcutId::nextTake:
            engine_.transportJump(1);
            return true;
        case ShortcutId::nudgeBack:
            engine_.transportNudge(-5.0);
            return true;
        case ShortcutId::nudgeForward:
            engine_.transportNudge(5.0);
            return true;
        case ShortcutId::save:
            saveSession();
            return true;
        case ShortcutId::saveAs:
            saveSessionAs();
            return true;
        case ShortcutId::count:
            break;
    }
    return false;
}

juce::ApplicationCommandTarget* MainComponent::getNextCommandTarget()
{
    return nullptr;
}

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& commands)
{
    commands.add(saveCommand);
    commands.add(saveAsCommand);
}

void MainComponent::getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result)
{
    const auto addKeys = [&result](ShortcutId id)
    {
        for (const auto& binding : kBindings)
        {
            if (binding.id != id || ! binding.commandBinding)
                continue;
            int mods = 0;
            if (binding.command == 1)
                mods |= juce::ModifierKeys::commandModifier;
            if (binding.shift == 1)
                mods |= juce::ModifierKeys::shiftModifier;
            if (binding.alt == 1)
                mods |= juce::ModifierKeys::altModifier;
            result.addDefaultKeypress(binding.commandCharacter, mods);
        }
    };

    if (commandID == saveCommand)
    {
        result.setInfo("Save", shortcutMeaning(ShortcutId::save), "File", 0);
        addKeys(ShortcutId::save);
    }
    else if (commandID == saveAsCommand)
    {
        result.setInfo("Save As...", shortcutMeaning(ShortcutId::saveAs), "File", 0);
        addKeys(ShortcutId::saveAs);
    }
}

bool MainComponent::perform(const juce::ApplicationCommandTarget::InvocationInfo& info)
{
    if (startupWindow_ != nullptr || placeWindow_ != nullptr)
        return true;

    if (info.commandID == saveCommand)
    {
        saveSession();
        return true;
    }
    if (info.commandID == saveAsCommand)
    {
        saveSessionAs();
        return true;
    }
    return false;
}

void MainComponent::openStartup()
{
    if (startupWindow_ != nullptr)
        return;
    juce::Component::SafePointer<MainComponent> safe(this);
    startupWindow_ = std::make_unique<StartupWindow>(engine_, [safe]
    {
        juce::MessageManager::callAsync([safe]
        {
            if (safe != nullptr)
                safe->dismissStartup();
        });
    });
    startupWindow_->enterModalState(true, nullptr, false);
}

void MainComponent::dismissStartup()
{
    if (startupWindow_ == nullptr)
        return;
    if (startupWindow_->isCurrentlyModal())
        startupWindow_->exitModalState(0);
    startupWindow_.reset();
    refresh();
    grabKeyboardFocus();
}

void MainComponent::promptForSession(const juce::String& title, std::function<void(bool placed)> then)
{
    if (placeWindow_ != nullptr || startupWindow_ != nullptr)
        return;

    juce::Component::SafePointer<MainComponent> safe(this);
    placeWindow_ = std::make_unique<SessionPlaceWindow>(
        title,
        engine_.defaultSessionParent(),
        engine_.missingSessionParentNote(),
        [safe, then](juce::File chosen, bool internalDisk)
        {
            juce::MessageManager::callAsync([safe, then, chosen, internalDisk]
            {
                if (safe == nullptr)
                    return;
                if (safe->placeWindow_ != nullptr && safe->placeWindow_->isCurrentlyModal())
                    safe->placeWindow_->exitModalState(0);
                safe->placeWindow_.reset();

                bool placed = false;
                if (internalDisk)
                    placed = safe->engine_.createInternalSession();
                else if (chosen.getFullPathName().isNotEmpty())
                    placed = safe->engine_.placeNewSession(chosen, false);

                safe->refresh();
                if (then != nullptr)
                    then(placed);
            });
        });
    placeWindow_->enterModalState(true, nullptr, false);
}

void MainComponent::newSession()
{
    promptForSession("New Session", nullptr);
}

void MainComponent::requestRecord()
{
    if (startupWindow_ != nullptr || placeWindow_ != nullptr)
        return;
    if (! engine_.hasSession())
    {
        promptForSession("Choose Where to Record", [this](bool placed)
        {
            if (placed)
                startRecordingIfReady();
        });
        return;
    }
    startRecordingIfReady();
}

void MainComponent::startRecordingIfReady()
{
    const auto problem = engine_.sessionRecordProblem();
    if (problem.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Cannot record",
                                               problem);
        refresh();
        return;
    }
    engine_.transportRecord();
    refresh();
}

void MainComponent::showGroupsMenu()
{
    juce::PopupMenu menu;
    menu.addItem(50, "Make group from selection...", ! engine_.selectedChannels().empty());
    menu.addSeparator();
    for (int group = 0; group < kMaxDisplayGroups; ++group)
        menu.addItem(group + 1, engine_.groupName(group));
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&groupButton_),
                       [this](int result)
                       {
                           if (result == 50)
                               showMakeGroupDialog(engine_);
                           else if (result > 0)
                               showGroupMenu(engine_, groupButton_, result - 1);
                       });
}

void MainComponent::layoutMeters()
{
    if (page_ != 1 || bridgeArea_.isEmpty())
        return;

    constexpr int scaleW = 36;
    const int guess = std::max(1, bridgeArea_.getWidth() - scaleW * 2);
    const int natural = meterGrid_.naturalContentWidth(guess);
    if (natural == laidOutNatural_ && bridgeArea_ == laidOutBridge_ && meterViewport_.getWidth() > 0)
        return;

    laidOutNatural_ = natural;
    laidOutBridge_ = bridgeArea_;

    juce::Rectangle<int> view;
    if (natural + scaleW * 2 <= bridgeArea_.getWidth())
    {
        const int block = natural + scaleW * 2;
        const int x = bridgeArea_.getX() + (bridgeArea_.getWidth() - block) / 2;
        leftScale_.setBounds(x, bridgeArea_.getY(), scaleW, bridgeArea_.getHeight());
        view = { x + scaleW, bridgeArea_.getY(), natural, bridgeArea_.getHeight() };
        rightScale_.setBounds(x + scaleW + natural, bridgeArea_.getY(), scaleW, bridgeArea_.getHeight());
    }
    else
    {
        leftScale_.setBounds(bridgeArea_.getX(), bridgeArea_.getY(), scaleW, bridgeArea_.getHeight());
        rightScale_.setBounds(bridgeArea_.getRight() - scaleW, bridgeArea_.getY(), scaleW, bridgeArea_.getHeight());
        view = bridgeArea_.withTrimmedLeft(scaleW).withTrimmedRight(scaleW);
    }

    meterViewport_.setBounds(view);
    meterGrid_.setFitWidth(view.getWidth());
    const int width = std::max(view.getWidth(), meterGrid_.preferredWidth(view.getWidth()));
    const int height = std::max(1, meterViewport_.getMaximumVisibleHeight());
    meterGrid_.setSize(width, height);
}

void MainComponent::showPage(int page)
{
    page_ = page == 2 ? 2 : 1;
    engine_.setSessionPage(page_);
    recorderButton_.setToggleState(page_ == 1, juce::dontSendNotification);
    pluginsButton_.setToggleState(page_ == 2, juce::dontSendNotification);
    resized();
}

void MainComponent::toggleScanner()
{
    scanner_.toggle();
    scannerButton_.setToggleState(scanner_.isVisible(), juce::dontSendNotification);
}

void MainComponent::toggleDropouts()
{
    dropouts_.toggle();
    dropoutsButton_.setToggleState(dropouts_.isVisible(), juce::dontSendNotification);
}

void MainComponent::toggleCpu()
{
    cpu_.toggle();
    cpuButton_.setToggleState(cpu_.isVisible(), juce::dontSendNotification);
}

void MainComponent::toggleLatency()
{
    if (latencyWindow_ == nullptr)
        return;
    const bool show = ! latencyWindow_->isVisible();
    latencyWindow_->setVisible(show);
    latencyButton_.setToggleState(show, juce::dontSendNotification);
    if (show)
        latencyWindow_->toFront(true);
}

void MainComponent::saveSession()
{
    if (! engine_.hasSession())
    {
        promptForSession("Choose Where to Save This Session", nullptr);
        return;
    }
    engine_.saveSession();
    refresh();
}

void MainComponent::saveSessionAs()
{
    if (! engine_.hasSession())
    {
        saveSession();
        return;
    }
    if (engine_.transportView().mode == TransportMode::recording)
    {
        juce::AlertWindow::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon,
            "Stop recording first",
            "Save As copies the whole session, including the audio files. Recording has to stop before the copy starts.",
            "Stop recording",
            "Cancel",
            nullptr,
            juce::ModalCallbackFunction::create([this](int result)
            {
                if (result == 0)
                    return;
                engine_.transportStop();
                chooseSaveAsDestination();
            }));
        return;
    }
    chooseSaveAsDestination();
}

void MainComponent::chooseSaveAsDestination()
{
    if (fileChooser_ != nullptr)
        return;

    fileChooser_ = std::make_unique<juce::FileChooser>("Save a Copy of This Session",
                                                       engine_.suggestedSessionFolder().getParentDirectory(),
                                                       juce::String(),
                                                       true);
    fileChooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories
                                  | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      fileChooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.beginSessionCopy(chosen);
                                      refresh();
                                  });
                              });
}

void MainComponent::importRecordings()
{
    if (! engine_.hasSession())
    {
        promptForSession("Choose Where to Save This Session", [this](bool placed)
        {
            if (placed)
                importRecordings();
        });
        return;
    }
    if (fileChooser_ != nullptr)
        return;

    fileChooser_ = std::make_unique<juce::FileChooser>("Import a Recording Folder",
                                                       engine_.suggestedSessionFolder(),
                                                       juce::String(),
                                                       true);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      fileChooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.importRecordingFolder(chosen);
                                      refresh();
                                  });
                              });
}

void MainComponent::confirmClearTimeline()
{
    juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::WarningIcon,
                                       "Clear the Timeline?",
                                       "This removes the takes from the timeline. The WAV files stay in the audio folder.",
                                       "Clear Timeline",
                                       "Cancel",
                                       nullptr,
                                       juce::ModalCallbackFunction::create([this](int result)
                                       {
                                           if (result == 1)
                                           {
                                               engine_.clearTimeline();
                                               refresh();
                                           }
                                       }));
}

void MainComponent::openRecent(int index)
{
    const auto sessions = engine_.recentSessions();
    if (index < 0 || index >= sessions.size())
        return;
    if (engine_.hasSession())
        engine_.saveSession();
    engine_.loadSessionFrom(juce::File(sessions[index]));
    refresh();
}

void MainComponent::showFileMenu()
{
    if (fileMenu_ == nullptr)
        return;
    auto menu = fileMenu_->getMenuForIndex(0, "File");
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&fileButton_),
                       [this](int result)
                       {
                           if (fileMenu_ != nullptr)
                               fileMenu_->menuItemSelected(result, 0);
                       });
}

class CopyProgressWindow : public juce::DocumentWindow,
                           private juce::Timer
{
public:
    explicit CopyProgressWindow(double& value)
        : juce::DocumentWindow("Saving a copy", theme::background, 0),
          value_(value),
          bar_(value_)
    {
        setUsingNativeTitleBar(true);
        auto* content = new juce::Component();
        content->addAndMakeVisible(label_);
        content->addAndMakeVisible(bar_);
        label_.setJustificationType(juce::Justification::centredLeft);
        label_.setText("Saving a copy", juce::dontSendNotification);
        setContentOwned(content, false);
        centreWithSize(380, 110);
        startTimerHz(8);
        setVisible(true);
        setAlwaysOnTop(true);
        resized();
    }

    void closeButtonPressed() override
    {
    }

    void resized() override
    {
        juce::DocumentWindow::resized();
        if (auto* content = getContentComponent())
        {
            auto area = content->getLocalBounds().reduced(16, 14);
            label_.setBounds(area.removeFromTop(22));
            area.removeFromTop(8);
            bar_.setBounds(area.removeFromTop(18));
        }
    }

private:
    void timerCallback() override
    {
        label_.setText("Saving a copy   " + juce::String(juce::roundToInt(value_ * 100.0)) + "%",
                       juce::dontSendNotification);
    }

    double& value_;
    juce::Label label_;
    juce::ProgressBar bar_;
};

void MainComponent::syncCopyProgress()
{
    const auto failure = engine_.takeCopyFailure();
    if (failure.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon,
                                               "Could not save the copy",
                                               failure);
    }

    const float progress = engine_.sessionCopyProgress();
    if (progress < 0.0f)
    {
        copyWindow_.reset();
        return;
    }

    copyProgressValue_ = static_cast<double>(progress);
    if (copyWindow_ == nullptr)
        copyWindow_ = std::make_unique<CopyProgressWindow>(copyProgressValue_);
}

void MainComponent::showHelp()
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                           "YouHost shortcuts",
                                           juce::String(shortcutHelpText()));
}

void MainComponent::setPeakMode(bool peak, bool fromUser)
{
    showPeak_ = peak;
    showRadio(rmsButton_, peakButton_, showPeak_);
    referenceBox_.setEnabled(! showPeak_);
    referenceLabel_.setEnabled(! showPeak_);
    if (fromUser)
    {
        engine_.setSessionMeters(showPeak_, rmsReferenceDb_);
        engine_.noteSessionEdit();
    }
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    refresh();
}

void MainComponent::setRmsReference(int db, bool fromUser)
{
    rmsReferenceDb_ = normaliseRmsReferenceDb(db);
    settings_.saveRmsReferenceDb(rmsReferenceDb_);
    referenceBox_.setSelectedId(referenceIdFor(rmsReferenceDb_), juce::dontSendNotification);
    if (fromUser)
    {
        engine_.setSessionMeters(showPeak_, rmsReferenceDb_);
        engine_.noteSessionEdit();
    }
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    refresh();
}

void MainComponent::openSession()
{
    if (fileChooser_ != nullptr)
        return;

    if (engine_.hasSession())
        engine_.saveSession();
    fileChooser_ = std::make_unique<juce::FileChooser>("Open a Session",
                                                       engine_.suggestedSessionFolder(),
                                                       "*.youhost",
                                                       true);
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this](const juce::FileChooser& chooser)
                              {
                                  const auto chosen = chooser.getResult();
                                  juce::MessageManager::callAsync([this, chosen]
                                  {
                                      fileChooser_.reset();
                                      if (chosen.getFullPathName().isNotEmpty())
                                          engine_.loadSessionFrom(chosen);
                                      refresh();
                                  });
                              });
}

void MainComponent::timerCallback()
{
    if (++pollDivider_ >= 6)
    {
        pollDivider_ = 0;
        engine_.pollDeviceStats();
        engine_.maintainSession();
    }
    syncCopyProgress();

    if (bitDepthSlot_ != nullptr)
        attachBitDepthSlot(deviceSelector_, *bitDepthSlot_);

    if (setupWindow_ != nullptr && setupWindow_->isVisible())
    {
        hideDeviceTestTone();
    }
    refresh();
}

void MainComponent::onPluginSlot(int channel)
{
    if (channel < 0)
    {
        refresh();
    }
    else
    {
        pluginPage_.refreshChannel(channel);
        publishMeters(false);
        repaint(statusArea_);
        repaint(hintArea_);
    }
    // Paint the dirty rows before the next plugin is created. A later
    // instantiate must not walk a stale full-window repaint.
    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();
}

void MainComponent::publishMeters(bool repaintLevels)
{
    auto transport = engine_.transportView();
    const bool recording = transport.mode == TransportMode::recording;
    const int channels = engine_.visibleChannels();
    const auto strips = engine_.displayStrips(channels);
    std::vector<BridgeCell> cells;
    cells.reserve(strips.size());
    for (const auto& item : strips)
    {
        BridgeCell cell;
        if (item.kind == StripKind::groupHeader)
        {
            cell.header = true;
            cell.group = item.group;
            cell.color = engine_.groupColor(item.group);
            cell.collapsed = engine_.groupCollapsed(item.group);
            cell.title = engine_.groupName(item.group);
            for (int channel = 0; channel < channels; ++channel)
            {
                if (engine_.channelGroup(channel) != item.group)
                    continue;
                ++cell.memberCount;
                const auto listen = engine_.channelListen(channel);
                if (listen == ChannelListen::record)
                    ++cell.membersRecord;
                else if (listen == ChannelListen::input)
                    ++cell.membersInput;
                if (channelListenAudible(listen))
                    ++cell.membersOn;
                const float level = showPeak_ ? engine_.peakFor(channel) : engine_.rmsFor(channel);
                if (showPeak_)
                    cell.reading.peak = std::max(cell.reading.peak, level);
                else
                    cell.reading.rms = std::max(cell.reading.rms, level);
                cell.reading.clipped = cell.reading.clipped || engine_.clipFor(channel);
                if (! cell.anyPlugin)
                {
                    const auto snap = engine_.channelSnapshot(channel);
                    for (const auto& slot : snap.slots)
                        cell.anyPlugin = cell.anyPlugin || slot.occupied;
                }
            }
        }
        else
        {
            cell.channel = item.channel;
            cell.color = engine_.channelColor(item.channel);
            cell.selected = engine_.isChannelSelected(item.channel);
            cell.title = engine_.channelName(item.channel);
            cell.reading.rms = engine_.rmsFor(item.channel);
            cell.reading.peak = engine_.peakFor(item.channel);
            cell.reading.clipped = engine_.clipFor(item.channel);
            cell.reading.hasInput = engine_.inputActive(item.channel);
            const auto listen = engine_.channelListen(item.channel);
            cell.reading.listen = static_cast<int>(listen);
            cell.reading.recordArmed = channelListenRecords(listen);
            cell.reading.recordLive = recording && cell.reading.recordArmed && cell.reading.hasInput;
            cell.pdc = engine_.channelPdcText(item.channel);
        }
        cells.push_back(std::move(cell));
    }
    meterGrid_.setCells(std::move(cells), showPeak_, rmsReferenceDb_, repaintLevels);
}

void MainComponent::refresh()
{
    const bool loading = engine_.isLoadingPlugins();
    if (loading != heavyPaintSuspended_)
    {
        heavyPaintSuspended_ = loading;
        meterGrid_.setBufferedToImage(loading);
        pluginPage_.setBufferedToImage(loading);
        timeline_.setBufferedToImage(loading);
        heavyPaintHeld_ = false;
    }
    if (loading && heavyPaintHeld_)
    {
        repaint(statusArea_);
        repaint(hintArea_);
        return;
    }

    latencyReadout_.setNumbers(engine_.latencyNumbers());
    const auto numbers = engine_.latencyNumbers();
    if (numbers.deviceOpen)
    {
        latencyLabel_.setText("Delay " + juce::String(samplesToMilliseconds(numbers.compensationSamples, numbers.sampleRate), 1)
                                  + " ms   USB " + juce::String(samplesToMilliseconds(numbers.roundTripSamples, numbers.sampleRate), 1)
                                  + " ms",
                              juce::dontSendNotification);
    }
    else
    {
        latencyLabel_.setText("No device", juce::dontSendNotification);
    }

    auto transport = engine_.transportView();
    if (transport.naturalEnd || transport.failed)
    {
        engine_.transportStop();
        transport = engine_.transportView();
    }
    if (transport.mode == TransportMode::recording)
        engine_.touchSession();
    timeline_.setWaveformGain(engine_.waveformGain());
    timeline_.setTransport(transport);
    timeLabel_.setText(timecodeText(transport.position, transport.sampleRate), juce::dontSendNotification);
    const bool recording = transport.mode == TransportMode::recording;
    const bool playing = transport.mode == TransportMode::playing;
    timeLabel_.setColour(juce::Label::textColourId, recording ? juce::Colours::white : theme::text);
    timeLabel_.setColour(juce::Label::backgroundColourId, recording ? juce::Colour(0xff8d2430) : theme::panel);
    modeLabel_.setText(playing ? "PLAYBACK" : recording ? "RECORD" : juce::String(), juce::dontSendNotification);
    modeLabel_.setColour(juce::Label::textColourId, playing ? theme::amber : theme::red);
    const bool blink = engine_.isRecordReady() && ! recording && ((juce::Time::getMillisecondCounter() / 450u) % 2u) == 0u;
    recButton_.setToggleState(recording || blink, juce::dontSendNotification);
    playButton_.setToggleState(playing, juce::dontSendNotification);
    scannerButton_.setToggleState(scanner_.isVisible(), juce::dontSendNotification);
    dropoutsButton_.setToggleState(dropouts_.isVisible(), juce::dontSendNotification);
    cpuButton_.setToggleState(cpu_.isVisible(), juce::dontSendNotification);
    latencyButton_.setToggleState(latencyWindow_ != nullptr && latencyWindow_->isVisible(), juce::dontSendNotification);
    latencyReadout_.setAlignGroup(engine_.alignGroup());
    setupButton_.setToggleState(setupWindow_ != nullptr && setupWindow_->isVisible(), juce::dontSendNotification);
    if (bitDepthSlot_ != nullptr)
        bitDepthSlot_->setSelectedId(engine_.wavBitDepth());
    allButton_.setToggleState(engine_.groupsAreExpanded(), juce::dontSendNotification);
    hideButton_.setToggleState(engine_.groupsAreHidden(), juce::dontSendNotification);
    leftScale_.setScale(showPeak_, rmsReferenceDb_, false);
    rightScale_.setScale(showPeak_, rmsReferenceDb_, true);

    publishMeters(true);
    layoutMeters();
    pluginPage_.setMeterMode(showPeak_, rmsReferenceDb_);
    pluginPage_.refresh();
    // Status text changes with the CPU readout. Buttons, meters, and the
    // timeline repaint themselves when their own state changes.
    repaint(statusArea_);
    if (pollDivider_ == 0 || page_ != hintPage_)
    {
        hintPage_ = page_;
        repaint(hintArea_);
        if (! bannerArea_.isEmpty())
            repaint(bannerArea_);
        if (! deviceLostArea_.isEmpty())
            repaint(deviceLostArea_);
    }
    if (loading)
        heavyPaintHeld_ = true;
}

void MainComponent::hideDeviceTestTone()
{
    hideTestButtons(deviceSelector_);
}

void MainComponent::toggleSetup()
{
    if (setupWindow_ == nullptr)
        return;
    const bool show = ! setupWindow_->isVisible();
    setupWindow_->setVisible(show);
    if (show)
    {
        setupWindow_->toFront(true);
        hideDeviceTestTone();
    }
}

void MainComponent::paint(juce::Graphics& graphics)
{
    graphics.fillAll(theme::background);
    graphics.setColour(juce::Colour(0xff5c677c));
    graphics.fillRect(fileRule_);
    graphics.fillRect(transportRule_);
    graphics.fillRect(scanTick_);
    graphics.setColour(theme::text);
    graphics.setFont(juce::Font(juce::FontOptions(18.0f)));
    graphics.drawText("YouHost", titleArea_, juce::Justification::centredLeft, false);

    const auto numbers = engine_.latencyNumbers();
    juce::String status = engine_.deviceName();
    if (numbers.deviceOpen && numbers.sampleRate > 0.0)
    {
        status << "   " << engine_.inputCount() << " in / " << engine_.outputCount() << " out"
               << "   " << juce::String(numbers.sampleRate / 1000.0, 1) << " kHz";
    }
    status << "   " << engine_.wavBitDepthLabel();
    status << "   CPU " << juce::String(juce::roundToInt(engine_.cpuUsage() * 100.0f)) << "%";
    if (! engine_.hasSession())
        status << "   No session";
    else
        status << "   " << engine_.sessionName();
    if (engine_.sessionIsOnInternalDisk())
        status << " (internal disk)";
    const auto backup = engine_.backupStatusText();
    if (backup.isNotEmpty())
        status << "   " << backup;
    const auto loadingPlugins = engine_.pluginLoadProgress();
    if (loadingPlugins.isNotEmpty())
        status << "   " << loadingPlugins;
    const auto transport = engine_.transportView();
    if (transport.status.isNotEmpty())
        status << "   " << transport.status;

    graphics.setColour(theme::dim);
    graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
    graphics.drawText(status, statusArea_, juce::Justification::centredRight, true);

    if (! engine_.microphoneGranted())
    {
        graphics.setColour(juce::Colour(0xff3a2a22));
        graphics.fillRoundedRectangle(bannerArea_.toFloat(), 8.0f);
        graphics.setColour(theme::amber);
        graphics.setFont(juce::Font(juce::FontOptions(13.0f)));
        graphics.drawFittedText("Microphone access is off, so inputs stay silent. Allow YouHost under "
                                "System Settings, Privacy & Security, Microphone, then press Retry.",
                                bannerArea_.reduced(12, 4).withTrimmedRight(96),
                                juce::Justification::centredLeft,
                                2);
    }

    if (engine_.deviceLost())
    {
        graphics.setColour(juce::Colour(0xff5a1e28));
        graphics.fillRoundedRectangle(deviceLostArea_.toFloat(), 8.0f);
        graphics.setColour(theme::red);
        graphics.setFont(juce::Font(juce::FontOptions(14.0f).withStyle("Bold")));
        graphics.drawFittedText("Audio device lost. YouHost will reopen it when it comes back. The take was saved.",
                                deviceLostArea_.reduced(12, 4),
                                juce::Justification::centredLeft,
                                2);
    }

    juce::String hint = engine_.pluginLoadProgress();
    if (hint.isEmpty())
        hint = engine_.openError().isNotEmpty() ? engine_.openError() : juce::String();
    if (hint.isEmpty())
        hint = engine_.rateWarning();
    if (hint.isEmpty() && page_ == 1)
        hint = "The channel number stays visible. Record blinks when armed. Play starts the take. Cmd+Space records immediately. T and R zoom the timeline. Live sound still passes through.";
    if (hint.isEmpty())
        hint = "The channel number stays visible. Drag a slot to move that plugin. Option-drag copies it. A drag is ignored while a plugin on that channel is still loading.";

    graphics.setColour(engine_.openError().isNotEmpty() || engine_.rateWarning().isNotEmpty() ? theme::red : theme::fainter);
    graphics.setFont(juce::Font(juce::FontOptions(12.0f)));
    auto hintBounds = hintArea_;
    hintBounds.removeFromLeft(148);
    graphics.drawFittedText(hint, hintBounds, juce::Justification::centredLeft, 2);

    ensureLogo();
}

void MainComponent::ensureLogo()
{
    if (logoTried_)
        return;
    logoTried_ = true;

    juce::Array<juce::File> candidates;
#ifdef YOUHOST_RESOURCE_DIR
    candidates.add(juce::File(YOUHOST_RESOURCE_DIR).getChildFile("logo.png"));
#endif
    candidates.add(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("logo.png"));
    candidates.add(juce::File::getSpecialLocation(juce::File::currentApplicationFile).getChildFile("Contents/Resources/logo.png"));
    for (const auto& file : candidates)
    {
        if (! file.existsAsFile())
            continue;
        logo_ = juce::ImageFileFormat::loadFrom(file);
        if (logo_.isValid())
        {
            if (brand_ != nullptr)
                brand_->setLogo(logo_);
            return;
        }
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(12);
    auto title = area.removeFromTop(22);
    titleArea_ = title.removeFromLeft(90);
    statusArea_ = title;
    area.removeFromTop(6);

    auto views = area.removeFromTop(34);
    newButton_.setBounds(views.removeFromLeft(46).reduced(0, 2));
    views.removeFromLeft(4);
    openButton_.setBounds(views.removeFromLeft(50).reduced(0, 2));
    views.removeFromLeft(4);
    saveButton_.setBounds(views.removeFromLeft(46).reduced(0, 2));
    views.removeFromLeft(4);
    saveAsButton_.setBounds(views.removeFromLeft(72).reduced(0, 2));
    views.removeFromLeft(4);
    setupButton_.setBounds(views.removeFromLeft(96).reduced(0, 2));
    views.removeFromLeft(4);
    fileButton_.setBounds(views.removeFromLeft(46).reduced(0, 2));
    views.removeFromLeft(8);
    groupButton_.setBounds(views.removeFromLeft(56).reduced(0, 2));
    views.removeFromLeft(4);
    allButton_.setBounds(views.removeFromLeft(40).reduced(0, 2));
    views.removeFromLeft(4);
    hideButton_.setBounds(views.removeFromLeft(48).reduced(0, 2));
    latencyButton_.setBounds(views.removeFromRight(108).reduced(0, 2));
    views.removeFromRight(4);
    cpuButton_.setBounds(views.removeFromRight(76).reduced(0, 2));
    views.removeFromRight(4);
    dropoutsButton_.setBounds(views.removeFromRight(124).reduced(0, 2));
    views.removeFromRight(4);
    pluginsButton_.setBounds(views.removeFromRight(88).reduced(0, 2));
    views.removeFromRight(4);
    recorderButton_.setBounds(views.removeFromRight(76).reduced(0, 2));
    auto scanGap = views.removeFromRight(28);
    scanTick_ = juce::Rectangle<int>(scanGap.getCentreX(), scanGap.getY() + 6, 1, std::max(8, scanGap.getHeight() - 12));
    scannerButton_.setBounds(views.removeFromRight(68).reduced(0, 2));

    auto fileGap = area.removeFromTop(4);
    fileRule_ = fileGap.withSizeKeepingCentre(fileGap.getWidth(), 1);
    auto transport = area.removeFromTop(40);
    constexpr int transportWidth = 72 + 64 + 64 + 72 + 84 + 8 + 148 + 96;
    auto cluster = transport.withSizeKeepingCentre(transportWidth, transport.getHeight());
    prevButton_.setBounds(cluster.removeFromLeft(72).reduced(2, 4));
    nextButton_.setBounds(cluster.removeFromLeft(64).reduced(2, 4));
    stopButton_.setBounds(cluster.removeFromLeft(64).reduced(2, 4));
    playButton_.setBounds(cluster.removeFromLeft(72).reduced(2, 4));
    recButton_.setBounds(cluster.removeFromLeft(84).reduced(2, 4));
    cluster.removeFromLeft(8);
    timeLabel_.setBounds(cluster.removeFromLeft(148).reduced(0, 2));
    modeLabel_.setBounds(cluster.reduced(4, 8));

    auto transportGap = area.removeFromTop(4);
    transportRule_ = transportGap.withSizeKeepingCentre(transportGap.getWidth(), 1);
    auto tools = area.removeFromTop(32);
    rmsButton_.setBounds(tools.removeFromLeft(52).reduced(0, 2));
    peakButton_.setBounds(tools.removeFromLeft(58).reduced(0, 2));
    tools.removeFromLeft(8);
    referenceLabel_.setBounds(tools.removeFromLeft(52));
    referenceBox_.setBounds(tools.removeFromLeft(110).reduced(0, 2));
    tools.removeFromLeft(8);
    clearClipsButton_.setBounds(tools.removeFromLeft(96).reduced(0, 2));
    latencyLabel_.setBounds(tools.reduced(8, 0));

    const bool showBanner = ! engine_.microphoneGranted();
    retryButton_.setVisible(showBanner);
    if (showBanner)
    {
        area.removeFromTop(8);
        bannerArea_ = area.removeFromTop(48);
        retryButton_.setBounds(bannerArea_.removeFromRight(88).reduced(8, 10));
    }
    else
    {
        bannerArea_ = {};
    }

    if (engine_.deviceLost())
    {
        area.removeFromTop(8);
        deviceLostArea_ = area.removeFromTop(44);
    }
    else
    {
        deviceLostArea_ = {};
    }

    area.removeFromTop(8);
    hintArea_ = area.removeFromBottom(32);
    if (brand_ != nullptr)
        brand_->setBounds(hintArea_.getX(), hintArea_.getY(), 148, hintArea_.getHeight());
    helpButton_.setBounds(getWidth() - 40, getHeight() - 36, 28, 28);
    hintArea_.removeFromRight(36);
    area.removeFromBottom(6);

    const bool recorder = page_ == 1;
    timeline_.setVisible(true);
    meterViewport_.setVisible(recorder);
    leftScale_.setVisible(recorder);
    rightScale_.setVisible(recorder);
    pluginPage_.setVisible(! recorder);

    const int automatic = recorder ? juce::jlimit(96, 180, getHeight() / 7)
                                   : juce::jlimit(88, 120, getHeight() / 10);
    const int timelineHeight = timelineHeight_ > 0 ? juce::jlimit(72, std::max(160, getHeight() / 2), timelineHeight_)
                                                   : automatic;
    timeline_.setBounds(area.removeFromTop(timelineHeight));
    area.removeFromTop(8);
    if (recorder)
    {
        bridgeArea_ = area;
        laidOutNatural_ = -1;
        layoutMeters();
    }
    else
    {
        bridgeArea_ = {};
        pluginPage_.setBounds(area);
    }
    repaint();
}

} // namespace youhost
