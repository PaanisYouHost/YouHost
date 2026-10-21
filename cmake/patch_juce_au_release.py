#!/usr/bin/env python3
"""Dispose an Audio Unit off the message thread, and do not wait for it.

JUCE posts AudioComponentInstanceDispose to the message thread and waits.
Slate dispatch_syncs that same thread from inside the dispose, so removing
the plugin never returns. The caller is already off the message thread.
"""

from pathlib import Path
import sys

MARKER = "youhostUnlock"

OLD = """            struct AUDeleter final : public CallbackMessage
            {
                AUDeleter (AudioUnitPluginInstanceHeadless& inInstance, WaitableEvent& inEvent)
                    : auInstance (inInstance), completionSignal (inEvent)
                {}

                void messageCallback() override
                {
                    auInstance.cleanup();
                    completionSignal.signal();
                }

                AudioUnitPluginInstanceHeadless& auInstance;
                WaitableEvent& completionSignal;
            };

            if (MessageManager::getInstance()->isThisTheMessageThread())
            {
                cleanup();
            }
            else
            {
                WaitableEvent completionEvent;
                (new AUDeleter (*this, completionEvent))->post();
                completionEvent.wait();
            }
"""

NEW = """            if (MessageManager::getInstance()->isThisTheMessageThread())
            {
                cleanup();
            }
            else
            {
                // YouHost: AudioComponentInstanceDispose runs on this thread.
                // Slate dispatch_syncs the main queue inside that dispose.
                // Posting it back to the message thread and waiting never
                // returns. The caller is already off that thread and does not wait.
                const ScopedUnlock youhostUnlock (lock);
                cleanup();
            }
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h"
    if not path.is_file():
        print(f"Could not find {path}", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return 0
    if OLD not in text:
        print("Could not find the Audio Unit destructor wait", file=sys.stderr)
        return 1
    path.write_text(text.replace(OLD, NEW, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
