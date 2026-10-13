// Paint the SYNC LOST window and fail if Ä, ä, or ö have no glyph.
// Writes a PNG of the same overlay the app draws.

#include "ui/SyncLostOverlay.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <iostream>
#include <string>

namespace
{

int fail(const std::string& message)
{
    std::cerr << "sync lost glyphs failed: " << message << "\n";
    return 1;
}

bool glyphFor(const juce::GlyphArrangement& arrangement, juce::juce_wchar character)
{
    for (int index = 0; index < arrangement.getNumGlyphs(); ++index)
    {
        const auto& glyph = arrangement.getGlyph(index);
        if (glyph.getCharacter() != character)
            continue;
        if (glyph.getGlyphIndex() == 0 || glyph.getRight() - glyph.getLeft() < 0.5f)
            return false;
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    const auto text = youhost::syncLostUtf8(youhost::kSyncLostFinnish);
    const auto expected = juce::String(juce::CharPointer_UTF8("\xc3\x84\xc3\xa4nikortti kadonnut"));
    if (text != expected)
        return fail("Finnish line was not read as UTF-8");

    const auto font = youhost::syncLostFont(64.0f);
    std::cout << "sync lost typeface " << font.getTypefaceName() << "\n";
    juce::GlyphArrangement arrangement;
    arrangement.addLineOfText(font, text, 0.0f, font.getAscent());
    juce::GlyphArrangement extras;
    extras.addLineOfText(font, youhost::syncLostUtf8("\xc3\xb6"), 0.0f, font.getAscent());
    if (! glyphFor(arrangement, 0x00C4) || ! glyphFor(arrangement, 0x00E4) || ! glyphFor(extras, 0x00F6))
        return fail("Ä, ä, or ö has no glyph in " + font.getTypefaceName().toStdString());

    youhost::SyncLostState state;
    state.deviceLost(22, 10, 4);
    youhost::SyncLostOverlay overlay;
    overlay.sync(state);
    overlay.setBounds(0, 0, 720, 320);
    juce::Image image(juce::Image::ARGB, 720, 320, true);
    {
        juce::Graphics graphics(image);
        overlay.paint(graphics);
    }

    int ink = 0;
    for (int y = 180; y < 300; ++y)
    {
        for (int x = 40; x < 680; ++x)
        {
            const auto pixel = image.getPixelAt(x, y);
            if (pixel.getRed() > 180 && pixel.getGreen() > 120 && pixel.getBlue() > 120)
                ++ink;
        }
    }
    if (ink < 200)
        return fail("Finnish line did not paint");

    const auto path = argc > 1 ? std::string(argv[1]) : std::string("/tmp/sync-lost-finnish.png");
    juce::File file(path);
    file.getParentDirectory().createDirectory();
    juce::FileOutputStream stream(file);
    if (! stream.openedOk())
        return fail("could not write " + path);
    juce::PNGImageFormat png;
    if (! png.writeImageToStream(image, stream))
        return fail("png encode failed");
    std::cout << "sync lost glyphs passed, wrote " << path << "\n";
    return 0;
}
