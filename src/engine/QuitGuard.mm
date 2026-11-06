#include "QuitGuard.h"

#include <juce_events/juce_events.h>

#import <AppKit/AppKit.h>

namespace youhost
{

void installCommandQuitMonitor() noexcept
{
    // The start screen is modal, so the Quit menu key equivalent never arrives.
    // Watch Command-Q before the key is delivered and quit from every screen.
    static id monitor = nil;
    if (monitor != nil)
        return;
    monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown
                                                     handler:^NSEvent* (NSEvent* event)
    {
        const auto flags = event.modifierFlags;
        const bool command = (flags & NSEventModifierFlagCommand) != 0;
        const bool shift = (flags & NSEventModifierFlagShift) != 0;
        const bool control = (flags & NSEventModifierFlagControl) != 0;
        if (! command || shift || control || event.keyCode != 12)
            return event;
        armUserQuit();
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->systemRequestedQuit();
        return nil;
    }];
}

bool macCurrentEventIsUserQuit() noexcept
{
    @autoreleasepool
    {
        NSEvent* event = [NSApp currentEvent];
        if (event == nil)
            return false;
        if (event.type == NSEventTypeKeyDown)
        {
            const bool command = (event.modifierFlags & NSEventModifierFlagCommand) != 0;
            return command && event.keyCode == 12;
        }
        if (event.type != NSEventTypeLeftMouseUp && event.type != NSEventTypeRightMouseUp)
            return false;
        NSWindow* window = event.window;
        if (window == nil)
            return false;
        NSString* name = NSStringFromClass(window.class);
        return name != nil && [name containsString:@"Menu"];
    }
}

} // namespace youhost
