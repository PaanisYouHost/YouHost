#!/usr/bin/env python3
"""Insert a hook so YouHost can place bit depth between sample rate and buffer size."""

from pathlib import Path
import sys

NEEDLE = """        if (bufferSizeDropDown != nullptr)
        {
            bufferSizeDropDown->setVisible (advancedSettingsVisible);
"""

INSERT = """        if (auto* bitDepthSlot = findChildWithID ("youhost-bit-depth"))
        {
            bitDepthSlot->setVisible (advancedSettingsVisible);

            if (advancedSettingsVisible)
            {
                bitDepthSlot->setBounds (r.removeFromTop (h));
                r.removeFromTop (space);
            }
        }

        if (bufferSizeDropDown != nullptr)
        {
            bufferSizeDropDown->setVisible (advancedSettingsVisible);
"""

MARKER = 'findChildWithID ("youhost-bit-depth")'
OLD_BOUNDS = "bitDepthSlot->setBounds (r.removeFromTop (h).withX (0).withWidth (getWidth()));"
NEW_BOUNDS = "bitDepthSlot->setBounds (r.removeFromTop (h));"


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "modules/juce_audio_utils/gui/juce_AudioDeviceSelectorComponent.cpp"
    if not path.is_file():
        path = Path("modules/juce_audio_utils/gui/juce_AudioDeviceSelectorComponent.cpp")
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        if OLD_BOUNDS in text:
            path.write_text(text.replace(OLD_BOUNDS, NEW_BOUNDS, 1), encoding="utf-8")
        return 0
    if NEEDLE not in text:
        print("Could not find the buffer-size layout block", file=sys.stderr)
        return 1
    path.write_text(text.replace(NEEDLE, INSERT, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
