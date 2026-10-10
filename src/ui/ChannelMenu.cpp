#include "ChannelMenu.h"
#include "engine/WindowCatalog.h"
#include "Theme.h"
#include "X32Look.h"

#include <memory>
#include <vector>

namespace youhost
{
namespace
{

void focusEditorSoon(juce::TextEditor* editor)
{
    if (editor == nullptr)
        return;
    editor->setSelectAllWhenFocused(true);
    editor->grabKeyboardFocus();
    editor->selectAll();
    juce::Component::SafePointer<juce::TextEditor> safe(editor);
    juce::MessageManager::callAsync([safe]
    {
        if (safe != nullptr)
        {
            safe->grabKeyboardFocus();
            safe->selectAll();
        }
    });
}

void renameWithPrompt(const juce::String& title, const juce::String& message, const juce::String& current, std::function<void(juce::String)> apply)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::QuestionIcon);
    window->addTextEditor("name", current, "Name");
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    focusEditorSoon(window->getTextEditor("name"));
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create([window, apply = std::move(apply)](int result)
                            {
                                if (result == 1 && apply != nullptr)
                                    apply(window->getTextEditorContents("name"));
                            }),
                            true);
}

class GroupRenameContent : public juce::Component
{
public:
    GroupRenameContent(AudioEngine& engine, int group, std::function<void(bool)> done)
        : engine_(engine),
          group_(group),
          originalColor_(engine.groupColor(group)),
          done_(std::move(done))
    {
        addAndMakeVisible(editor_);
        addAndMakeVisible(ok_);
        addAndMakeVisible(cancel_);
        editor_.setText(engine.groupName(group), false);
        editor_.setSelectAllWhenFocused(true);
        editor_.setFont(juce::Font(juce::FontOptions(15.0f)));
        editor_.onReturnKey = [this] { commit(); };
        editor_.onEscapeKey = [this] { cancel(); };
        ok_.onClick = [this] { commit(); };
        cancel_.onClick = [this] { cancel(); };
        ok_.setMouseClickGrabsKeyboardFocus(false);
        cancel_.setMouseClickGrabsKeyboardFocus(false);
        ok_.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
        cancel_.addShortcut(juce::KeyPress(juce::KeyPress::escapeKey));
        for (int index = 0; index < kX32ColourCount; ++index)
        {
            auto swatch = std::make_unique<Swatch>(*this, index);
            addAndMakeVisible(*swatch);
            swatches_.push_back(std::move(swatch));
        }
    }

    ~GroupRenameContent() override
    {
        if (! finished_)
            engine_.setGroupColor(group_, originalColor_);
    }

    void focusName()
    {
        focusEditorSoon(&editor_);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16, 12);
        editor_.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);
        auto swatchRow = area.removeFromTop(28);
        const int count = static_cast<int>(swatches_.size());
        const int gap = 4;
        const int width = count > 0 ? std::max(16, (swatchRow.getWidth() - gap * (count - 1)) / count) : 16;
        for (int index = 0; index < count; ++index)
        {
            swatches_[static_cast<std::size_t>(index)]->setBounds(swatchRow.removeFromLeft(width));
            if (index + 1 < count)
                swatchRow.removeFromLeft(gap);
        }
        area.removeFromTop(12);
        auto buttons = area.removeFromTop(28);
        cancel_.setBounds(buttons.removeFromRight(88));
        buttons.removeFromRight(8);
        ok_.setBounds(buttons.removeFromRight(88));
    }

private:
    class Swatch : public juce::Component
    {
    public:
        Swatch(GroupRenameContent& owner, int color)
            : owner_(owner),
              color_(color)
        {
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat().reduced(2.0f);
            graphics.setColour(x32Fill(color_));
            graphics.fillRoundedRectangle(bounds, 3.0f);
            if (x32Fill(color_).getPerceivedBrightness() < 0.08f)
            {
                graphics.setColour(juce::Colour(0xff9aa3b5));
                graphics.drawRoundedRectangle(bounds, 3.0f, 1.0f);
            }
            if (owner_.currentColor() == color_)
            {
                graphics.setColour(theme::text);
                graphics.drawRoundedRectangle(bounds.reduced(1.0f), 3.0f, 2.0f);
            }
        }

        void mouseDown(const juce::MouseEvent&) override
        {
            owner_.chooseColor(color_);
        }

    private:
        GroupRenameContent& owner_;
        int color_ = 0;
    };

    int currentColor() const
    {
        return engine_.groupColor(group_);
    }

    void chooseColor(int color)
    {
        engine_.setGroupColor(group_, color);
        repaint();
    }

    void commit()
    {
        if (finished_)
            return;
        finished_ = true;
        engine_.setGroupName(group_, editor_.getText());
        if (done_ != nullptr)
            done_(true);
    }

    void cancel()
    {
        if (finished_)
            return;
        finished_ = true;
        engine_.setGroupColor(group_, originalColor_);
        if (done_ != nullptr)
            done_(false);
    }

    AudioEngine& engine_;
    int group_ = 0;
    int originalColor_ = 0;
    std::function<void(bool)> done_;
    juce::TextEditor editor_;
    juce::TextButton ok_ { "OK" };
    juce::TextButton cancel_ { "Cancel" };
    std::vector<std::unique_ptr<Swatch>> swatches_;
    bool finished_ = false;
};

class NameNewGroupContent : public juce::Component
{
public:
    NameNewGroupContent(AudioEngine& engine, int freeSlot, std::function<void(bool)> done)
        : engine_(engine),
          freeSlot_(freeSlot),
          done_(std::move(done))
    {
        addAndMakeVisible(editor_);
        addAndMakeVisible(ok_);
        addAndMakeVisible(cancel_);
        editor_.setText("Group " + juce::String(freeSlot_ + 1), false);
        editor_.setSelectAllWhenFocused(true);
        editor_.setFont(juce::Font(juce::FontOptions(15.0f)));
        editor_.onReturnKey = [this] { commit(); };
        editor_.onEscapeKey = [this] { dismiss(); };
        ok_.onClick = [this] { commit(); };
        cancel_.onClick = [this] { dismiss(); };
        ok_.setMouseClickGrabsKeyboardFocus(false);
        cancel_.setMouseClickGrabsKeyboardFocus(false);
        ok_.addShortcut(juce::KeyPress(juce::KeyPress::returnKey));
        cancel_.addShortcut(juce::KeyPress(juce::KeyPress::escapeKey));
        for (int index = 0; index < kX32ColourCount; ++index)
        {
            auto swatch = std::make_unique<Swatch>(*this, index);
            addAndMakeVisible(*swatch);
            swatches_.push_back(std::move(swatch));
        }
    }

    void focusName()
    {
        focusEditorSoon(&editor_);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(theme::background);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16, 12);
        editor_.setBounds(area.removeFromTop(28));
        area.removeFromTop(8);
        auto swatchRow = area.removeFromTop(28);
        const int count = static_cast<int>(swatches_.size());
        const int gap = 4;
        const int width = count > 0 ? std::max(16, (swatchRow.getWidth() - gap * (count - 1)) / count) : 16;
        for (int index = 0; index < count; ++index)
        {
            swatches_[static_cast<std::size_t>(index)]->setBounds(swatchRow.removeFromLeft(width));
            if (index + 1 < count)
                swatchRow.removeFromLeft(gap);
        }
        area.removeFromTop(12);
        auto buttons = area.removeFromTop(28);
        cancel_.setBounds(buttons.removeFromRight(88));
        buttons.removeFromRight(8);
        ok_.setBounds(buttons.removeFromRight(88));
    }

private:
    class Swatch : public juce::Component
    {
    public:
        Swatch(NameNewGroupContent& owner, int color)
            : owner_(owner),
              color_(color)
        {
        }

        void paint(juce::Graphics& graphics) override
        {
            auto bounds = getLocalBounds().toFloat().reduced(2.0f);
            graphics.setColour(x32Fill(color_));
            graphics.fillRoundedRectangle(bounds, 3.0f);
            if (x32Fill(color_).getPerceivedBrightness() < 0.08f)
            {
                graphics.setColour(juce::Colour(0xff9aa3b5));
                graphics.drawRoundedRectangle(bounds, 3.0f, 1.0f);
            }
            if (owner_.color_ == color_)
            {
                graphics.setColour(theme::text);
                graphics.drawRoundedRectangle(bounds.reduced(1.0f), 3.0f, 2.0f);
            }
        }

        void mouseDown(const juce::MouseEvent&) override
        {
            owner_.color_ = color_;
            owner_.repaint();
        }

    private:
        NameNewGroupContent& owner_;
        int color_ = 0;
    };

    void commit()
    {
        if (finished_)
            return;
        const auto selected = engine_.selectedChannels();
        const auto plan = planNewGroup(! selected.empty(),
                                       freeSlot_,
                                       editor_.getText().toStdString(),
                                       normaliseX32Colour(color_));
        if (! plan.created)
        {
            dismiss();
            return;
        }
        finished_ = true;
        engine_.setGroupColor(plan.group, plan.color);
        engine_.setGroupName(plan.group, juce::String::fromUTF8(plan.name.c_str()));
        engine_.assignChannelsToGroup(selected, plan.group);
        if (done_ != nullptr)
            done_(true);
    }

    void dismiss()
    {
        if (finished_)
            return;
        finished_ = true;
        if (done_ != nullptr)
            done_(false);
    }

    AudioEngine& engine_;
    int freeSlot_ = 0;
    int color_ = 0;
    std::function<void(bool)> done_;
    juce::TextEditor editor_;
    juce::TextButton ok_ { "OK" };
    juce::TextButton cancel_ { "Cancel" };
    std::vector<std::unique_ptr<Swatch>> swatches_;
    bool finished_ = false;
};

int firstFreeGroupSlot(const AudioEngine& engine)
{
    int membership[kMaxChannels];
    for (int channel = 0; channel < kMaxChannels; ++channel)
        membership[channel] = engine.channelGroup(channel);
    return firstUnusedGroup(membership, kMaxChannels, kMaxDisplayGroups);
}

juce::PopupMenu colorMenu(int ticked)
{
    juce::PopupMenu menu;
    for (int index = 0; index < kX32ColourCount; ++index)
    {
        juce::PopupMenu::Item item;
        item.itemID = 200 + index;
        item.text = kX32Colours[index].name;
        item.colour = x32Fill(index);
        item.isTicked = index == ticked;
        if (x32Fill(index).getPerceivedBrightness() < 0.08f)
            item.colour = juce::Colour(0xff9aa3b5);
        menu.addItem(item);
    }
    return menu;
}

int sharedColor(const AudioEngine& engine, const std::vector<int>& channels)
{
    if (channels.empty())
        return -1;
    const int first = engine.channelColor(channels.front());
    for (int channel : channels)
        if (engine.channelColor(channel) != first)
            return -1;
    return first;
}

int sharedGroup(const AudioEngine& engine, const std::vector<int>& channels)
{
    if (channels.empty())
        return -2;
    const int first = engine.channelGroup(channels.front());
    for (int channel : channels)
        if (engine.channelGroup(channel) != first)
            return -2;
    return first;
}

} // namespace

void showChannelMenu(AudioEngine& engine,
                     juce::Component& target,
                     int channel,
                     std::function<void(int)> beginRename)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    if (! engine.isChannelSelected(channel))
        engine.selectChannel(channel, false);

    const auto channels = engine.selectedChannels();
    const int oneColor = channels.size() == 1 ? sharedColor(engine, channels) : sharedColor(engine, channels);
    const int oneGroup = sharedGroup(engine, channels);

    juce::PopupMenu menu;
    if (channels.size() == 1)
        menu.addItem(300, "Rename channel");
    else
        menu.addItem(0, juce::String(channels.size()) + " channels", false, false);

    menu.addSubMenu("Color", colorMenu(oneColor));
    menu.addItem(400, "Make group from selection...");

    juce::PopupMenu groups;
    groups.addItem(1, "No group", true, oneGroup == -1);
    groups.addSeparator();
    for (int group = 0; group < kMaxDisplayGroups; ++group)
        groups.addItem(20 + group, engine.groupName(group), true, oneGroup == group);
    menu.addSubMenu("Group", groups);

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [&engine, channels, beginRename = std::move(beginRename)](int result)
                       {
                           if (result == 300 && channels.size() == 1)
                           {
                               const int chosen = channels.front();
                               if (beginRename != nullptr)
                                   beginRename(chosen);
                               else
                                   renameWithPrompt("Rename channel " + juce::String(chosen + 1),
                                                    "The name is shown on the channel and used in the WAV file name.",
                                                    engine.channelName(chosen),
                                                    [&engine, chosen](juce::String name) { engine.setChannelName(chosen, name); });
                           }
                           else if (result == 400)
                           {
                               showMakeGroupDialog(engine);
                           }
                           else if (result == 1)
                           {
                               engine.assignChannelsToGroup(channels, -1);
                           }
                           else if (result >= 20 && result < 20 + kMaxDisplayGroups)
                           {
                               engine.assignChannelsToGroup(channels, result - 20);
                           }
                           else if (result >= 200 && result < 200 + kX32ColourCount)
                           {
                               const int color = result - 200;
                               for (int chosen : channels)
                                   engine.setChannelColor(chosen, color);
                           }
                       });
}

void renameGroup(AudioEngine& engine, int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;

    auto* window = new juce::DialogWindow("Rename " + engine.groupName(group), theme::background, true, true);
    auto* content = new GroupRenameContent(engine, group, [window](bool)
    {
        juce::MessageManager::callAsync([window] { window->exitModalState(0); });
    });
    window->setContentOwned(content, true);
    window->centreWithSize(groupRenameWindowWidth(), groupRenameWindowHeight());
    window->setResizable(false, false);
    window->setUsingNativeTitleBar(true);
    window->enterModalState(true, nullptr, true);
    content->focusName();
}

void showMakeGroupDialog(AudioEngine& engine)
{
    if (engine.selectedChannels().empty())
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "New group",
                                               "Select one or more channels first.");
        return;
    }

    const int freeSlot = firstFreeGroupSlot(engine);
    if (freeSlot < 0)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon,
                                               "New group",
                                               "All 10 groups are already in use.");
        return;
    }

    auto* window = new juce::DialogWindow("New group", theme::background, true, true);
    auto* content = new NameNewGroupContent(engine, freeSlot, [window](bool)
    {
        juce::MessageManager::callAsync([window] { window->exitModalState(0); });
    });
    window->setContentOwned(content, true);
    window->centreWithSize(groupRenameWindowWidth(), groupRenameWindowHeight());
    window->setResizable(false, false);
    window->setUsingNativeTitleBar(true);
    window->enterModalState(true, nullptr, true);
    content->focusName();
}

void showGroupMenu(AudioEngine& engine, juce::Component& target, int group)
{
    if (group < 0 || group >= kMaxDisplayGroups)
        return;

    juce::PopupMenu menu;
    menu.addItem(300, "Rename group");
    menu.addSubMenu("Color", colorMenu(engine.groupColor(group)));
    menu.addItem(2, "Remove group");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target),
                       [&engine, group](int result)
                       {
                           if (result == 300)
                               renameGroup(engine, group);
                           else if (result == 2)
                           {
                               engine.clearGroup(group);
                           }
                           else if (result >= 200 && result < 200 + kX32ColourCount)
                           {
                               engine.setGroupColor(group, result - 200);
                           }
                       });
}

} // namespace youhost
