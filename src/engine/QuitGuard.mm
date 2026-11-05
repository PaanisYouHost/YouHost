#include "QuitGuard.h"

#import <AppKit/AppKit.h>

namespace youhost
{

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
