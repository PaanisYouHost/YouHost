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
RECT_MARKER = "host window stays the plugin view size"

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


OLD_TO_RECT = """    ViewRect componentToVST3Rect (Rectangle<int> r) const
    {
        const auto combinedScale = nativeScaleFactor * getDesktopScaleFactor();
        const auto physical = (localAreaToGlobal (r.toFloat()) * combinedScale).toNearestInt();
        return { 0, 0, physical.getWidth(), physical.getHeight() };
    }
"""

NEW_TO_RECT = """    ViewRect componentToVST3Rect (Rectangle<int> r) const
    {
       #if JUCE_MAC
        // macOS IPlugView sizes are logical points. localAreaToGlobal folds in
        // the Retina backing scale, and the reverse conversion reads that
        // rectangle as a screen rect. The host window then fills the display
        // while the plugin view stays in the corner. Resize requests stay
        // in the same points. The host window stays the plugin view size.
        return { 0, 0, r.getWidth(), r.getHeight() };
       #else
        const auto combinedScale = nativeScaleFactor * getDesktopScaleFactor();
        const auto physical = (localAreaToGlobal (r.toFloat()) * combinedScale).toNearestInt();
        return { 0, 0, physical.getWidth(), physical.getHeight() };
       #endif
    }
"""

OLD_FROM_RECT = """    Rectangle<int> vst3ToComponentRect (const ViewRect& vr) const
    {
        const auto combinedScale = nativeScaleFactor * getDesktopScaleFactor();
        const auto floatRect = Rectangle { (float) vr.right, (float) vr.bottom } / combinedScale;
        return getLocalArea (nullptr, floatRect).toNearestInt();
    }
"""

NEW_FROM_RECT = """    Rectangle<int> vst3ToComponentRect (const ViewRect& vr) const
    {
       #if JUCE_MAC
        const auto width = vr.right - vr.left;
        const auto height = vr.bottom - vr.top;
        return { 0, 0, width > 0 ? width : vr.right, height > 0 ? height : vr.bottom };
       #else
        const auto combinedScale = nativeScaleFactor * getDesktopScaleFactor();
        const auto floatRect = Rectangle { (float) vr.right, (float) vr.bottom } / combinedScale;
        return getLocalArea (nullptr, floatRect).toNearestInt();
       #endif
    }
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "modules/juce_audio_processors/format_types/juce_VST3PluginFormat.cpp"
    if not path.is_file():
        print(f"Could not find {path}", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    changed = False
    if MARKER not in text:
        if OLD_SCALE not in text or OLD_NOTIFY not in text:
            print("Could not find the VST3 scale block", file=sys.stderr)
            return 1
        text = text.replace(OLD_SCALE, NEW_SCALE, 1).replace(OLD_NOTIFY, NEW_NOTIFY, 1)
        changed = True
    if RECT_MARKER not in text:
        if OLD_TO_RECT not in text or OLD_FROM_RECT not in text:
            print("Could not find the VST3 size conversion", file=sys.stderr)
            return 1
        text = text.replace(OLD_TO_RECT, NEW_TO_RECT, 1).replace(OLD_FROM_RECT, NEW_FROM_RECT, 1)
        changed = True
    if changed:
        path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
