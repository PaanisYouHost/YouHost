#pragma once

#include "engine/PluginNotice.h"
#include "ui/SyncLostOverlay.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace youhost
{

class PluginNoticeOverlay : public juce::Component
{
public:
    std::function<void(int)> onAction;

    PluginNoticeOverlay()
    {
        setOpaque(true);
        setAlwaysOnTop(true);
        setVisible(false);
        for (auto* button : { &ok_, &cancel_, &retry_, &bypass_ })
        {
            addChildComponent(button);
            button->onClick = [this, button]
            {
                if (onAction == nullptr)
                    return;
                if (button == &ok_)
                    onAction(1);
                else if (button == &cancel_)
                    onAction(2);
                else if (button == &retry_)
                    onAction(3);
                else if (button == &bypass_)
                    onAction(4);
            };
        }
        ok_.setButtonText("OK");
        cancel_.setButtonText("Cancel");
        retry_.setButtonText("Retry");
        bypass_.setButtonText("Bypass plugin");
    }

    PluginNoticeKind kind() const noexcept { return model_.kind; }
    bool detailsOpen() const noexcept { return model_.detailsOpen; }

    bool sync(const PluginNoticeModel& model)
    {
        const bool changed = model.kind != model_.kind || model.channel != model_.channel || model.pluginName != model_.pluginName
                             || model.detailsOpen != model_.detailsOpen || model.channelName != model_.channelName
                             || std::abs(model.mutedSeconds - model_.mutedSeconds) > 0.05;
        model_ = model;
        const bool corner = model_.kind == PluginNoticeKind::corner && ! model_.detailsOpen;
        ok_.setVisible(! corner && model_.kind != PluginNoticeKind::crashed && model_.kind != PluginNoticeKind::none);
        cancel_.setVisible(model_.kind == PluginNoticeKind::protectedAsk);
        retry_.setVisible(model_.kind == PluginNoticeKind::protectedAsk || model_.kind == PluginNoticeKind::notRestored);
        bypass_.setVisible(model_.kind == PluginNoticeKind::notRestored);
        if (changed)
            repaint();
        return changed;
    }

    bool setBlinkPhase(bool lit)
    {
        if (! pluginNoticeBlinks(model_.kind) || lit == lit_)
            return false;
        lit_ = lit;
        repaint();
        return true;
    }

    void paint(juce::Graphics& graphics) override
    {
        const bool corner = model_.kind == PluginNoticeKind::corner && ! model_.detailsOpen;
        juce::Colour fill(0xff8d1424);
        if (model_.kind == PluginNoticeKind::crashed)
            fill = lit_ ? juce::Colour(0xffc41818) : juce::Colour(0xff7a1010);
        else if (model_.kind == PluginNoticeKind::corner || model_.kind == PluginNoticeKind::restored)
            fill = lit_ || model_.kind == PluginNoticeKind::restored ? juce::Colour(0xff1d7a32) : juce::Colour(0xff0e4a1c);
        else if (model_.kind == PluginNoticeKind::notRestored)
            fill = juce::Colour(0xff4a1014);
        else if (model_.kind == PluginNoticeKind::protectedAsk)
            fill = juce::Colour(0xff1a4f86);
        graphics.fillAll(fill);
        graphics.setColour(juce::Colours::white);
        if (corner)
        {
            graphics.setFont(syncLostFont(14.0f));
            graphics.drawFittedText(syncLostUtf8(pluginRestoredCornerLabel(model_.channel).c_str()),
                                    getLocalBounds().reduced(8, 2), juce::Justification::centred, 1);
            return;
        }

        auto area = getLocalBounds().reduced(24, 16);
        area.removeFromBottom(56);
        graphics.setFont(syncLostFont(28.0f));
        graphics.drawFittedText(syncLostUtf8(pluginNoticeTitle(shownKind())), area.removeFromTop(40), juce::Justification::centred, 1);
        graphics.setFont(syncLostFont(16.0f));
        graphics.drawFittedText(syncLostUtf8(body().c_str()), area, juce::Justification::centred, 12);
    }

    void resized() override
    {
        auto row = getLocalBounds().reduced(16).removeFromBottom(40);
        const bool corner = model_.kind == PluginNoticeKind::corner && ! model_.detailsOpen;
        if (corner)
            return;
        if (model_.kind == PluginNoticeKind::protectedAsk)
        {
            retry_.setBounds(row.removeFromLeft(row.getWidth() / 3).reduced(4, 4));
            cancel_.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(4, 4));
            ok_.setBounds(row.reduced(4, 4));
            return;
        }
        if (model_.kind == PluginNoticeKind::notRestored)
        {
            retry_.setBounds(row.removeFromLeft(row.getWidth() / 2).reduced(4, 4));
            bypass_.setBounds(row.reduced(4, 4));
            return;
        }
        ok_.setBounds(row.withSizeKeepingCentre(88, 32));
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        if (model_.kind != PluginNoticeKind::corner)
            return;
        model_.detailsOpen = true;
        if (onAction != nullptr)
            onAction(0);
        repaint();
    }

    PluginNoticeModel model() const { return model_; }

private:
    PluginNoticeKind shownKind() const noexcept
    {
        if (model_.kind == PluginNoticeKind::corner && model_.detailsOpen)
            return PluginNoticeKind::restored;
        return model_.kind;
    }

    std::string body() const
    {
        if (model_.kind == PluginNoticeKind::crashed)
            return pluginCrashedLine(model_.channel, model_.channelName, model_.pluginName);
        if (model_.kind == PluginNoticeKind::protectedAsk)
            return pluginProtectedLine(model_.pluginName);
        if (model_.kind == PluginNoticeKind::notRestored)
            return pluginNotRestoredLine();
        if (shownKind() == PluginNoticeKind::restored && (model_.detailsOpen || model_.kind == PluginNoticeKind::restored))
        {
            if (model_.detailsOpen || model_.kind == PluginNoticeKind::corner)
                return pluginRestoredDetails(model_.channel, model_.channelName, model_.pluginName, model_.mutedSeconds);
            return pluginRestoredLine();
        }
        return pluginRestoredLine();
    }

    PluginNoticeModel model_ {};
    bool lit_ = true;
    juce::TextButton ok_, cancel_, retry_, bypass_;
};

} // namespace youhost
