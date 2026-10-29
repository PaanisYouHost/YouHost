#!/usr/bin/env python3
"""Jump back to the message loop when a plugin crashes on that thread.

[NSApp run] is the call that is still on the stack. A SIGSEGV inside a
plugin timer longjmps here, the plugin is removed on the next timer, and
the run loop starts again. Targets that do not link the guard get the weak
stub, which just runs the loop once.
"""

from pathlib import Path
import sys

MARKER = "youhostMessageFault"

OLD = """void MessageManager::runDispatchLoop()
{
    if (quitMessagePosted)
        return;

    JUCE_AUTORELEASEPOOL
    {
        // must only be called by the message thread!
        jassert (isThisTheMessageThread());

       #if JUCE_CATCH_UNHANDLED_EXCEPTIONS
        @try
        {
            [NSApp run];
        }
        @catch (NSException* e)
        {
            // An AppKit exception will kill the app, but at least this provides a chance to log it.,
            std::runtime_error ex (std::string ("NSException: ") + [[e name] UTF8String] + ", Reason:" + [[e reason] UTF8String]);
            JUCEApplicationBase::sendUnhandledException (&ex, __FILE__, __LINE__);
        }
        @finally
        {
        }
       #else
        [NSApp run];
       #endif
    }
}
"""

NEW = """extern "C" __attribute__((weak)) int youhostMessageFaultSetjmp()
{
    return 0;
}

extern "C" __attribute__((weak)) void youhostMessageFaultRecovered()
{
}

void MessageManager::runDispatchLoop()
{
    if (quitMessagePosted)
        return;

    JUCE_AUTORELEASEPOOL
    {
        // must only be called by the message thread!
        jassert (isThisTheMessageThread());

        // youhostMessageFault: a plugin SIGSEGV on this thread jumps back
        // here instead of aborting. The run loop then starts again.
        for (;;)
        {
            if (youhostMessageFaultSetjmp() != 0)
            {
                youhostMessageFaultRecovered();
                continue;
            }

           #if JUCE_CATCH_UNHANDLED_EXCEPTIONS
            @try
            {
                [NSApp run];
            }
            @catch (NSException* e)
            {
                // An AppKit exception will kill the app, but at least this provides a chance to log it.,
                std::runtime_error ex (std::string ("NSException: ") + [[e name] UTF8String] + ", Reason:" + [[e reason] UTF8String]);
                JUCEApplicationBase::sendUnhandledException (&ex, __FILE__, __LINE__);
            }
            @finally
            {
            }
           #else
            [NSApp run];
           #endif
            break;
        }
    }
}
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "modules/juce_events/native/juce_MessageManager_mac.mm"
    if not path.is_file():
        print(f"Could not find {path}", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return 0
    if OLD not in text:
        print("Could not find MessageManager::runDispatchLoop", file=sys.stderr)
        return 1
    path.write_text(text.replace(OLD, NEW, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
