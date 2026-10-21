#!/usr/bin/env python3
"""Keep a macOS VST3 view in logical points, the same unit as an Audio Unit.

JUCE 9 folds the Retina backing scale into IPlugView sizes and calls
setContentScaleFactor with it. On macOS those sizes are already points.
Vastaus then opens at a different size from its Audio Unit. Windows still
receives the backing scale, because that platform reports physical pixels.
"""

from pathlib import Path
import sys

MARKER = "r->nativeScaleFactor = 1.0f"

OLD_SCALE = """    void setContentScaleFactor()
    {
        if (scaleInterface != nullptr)
        {
            [[maybe_unused]] const auto result = scaleInterface->setContentScaleFactor ((IPlugViewContentScaleSupport::ScaleFactor) getEffectiveScale());

           #if ! JUCE_MAC
            [[maybe_unused]] const auto warning = warnOnFailure (result);
           #endif
        }
    }
"""

NEW_SCALE = """    void setContentScaleFactor()
    {
       #if JUCE_MAC
        // Logical points already include the screen. Passing the backing
        // scale, or passing 1 on a Retina display, makes Vastaus redraw at
        // the wrong size. The Audio Unit view does not get this call.
        return;
       #else
        if (scaleInterface != nullptr)
        {
            [[maybe_unused]] const auto result = scaleInterface->setContentScaleFactor ((IPlugViewContentScaleSupport::ScaleFactor) getEffectiveScale());

            [[maybe_unused]] const auto warning = warnOnFailure (result);
        }
       #endif
    }
"""

OLD_NOTIFY = """                if (auto* r = ref.getComponent())
                {
                    r->nativeScaleFactor = platformScale;
                    r->setContentScaleFactor();
                    r->resizeToFit();
"""

NEW_NOTIFY = """                if (auto* r = ref.getComponent())
                {
                   #if JUCE_MAC
                    // macOS IPlugView getSize, onSize, and checkSizeConstraint
                    // are logical points, the same unit as an Audio Unit NSView.
                    // AppKit applies the Retina backing scale. Folding that
                    // scale in here divides the view by two. Vastaus VST3 then
                    // opens at the wrong size while its Audio Unit stays correct.
                    ignoreUnused (platformScale);
                    r->nativeScaleFactor = 1.0f;
                   #else
                    r->nativeScaleFactor = platformScale;
                    r->setContentScaleFactor();
                   #endif
                    r->resizeToFit();
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "modules/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp"
    if not path.is_file():
        print(f"Could not find {path}", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return 0
    if OLD_SCALE not in text or OLD_NOTIFY not in text:
        print("Could not find the VST3 scale block", file=sys.stderr)
        return 1
    text = text.replace(OLD_SCALE, NEW_SCALE, 1).replace(OLD_NOTIFY, NEW_NOTIFY, 1)
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
