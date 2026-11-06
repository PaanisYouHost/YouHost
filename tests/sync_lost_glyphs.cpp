// Paint the SYNC LOST window and fail if the English subtitle does not draw.
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

} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    const auto text = youhost::syncLostUtf8(youhost::kSyncLostSubtitle);
    if (text != "Audio card lost")
        return fail("subtitle was not the English line");

    const auto font = youhost::syncLostFont(64.0f);
    std::cout << "sync lost typeface " << font.getTypefaceName() << "\n";
    juce::GlyphArrangement arrangement;
    arrangement.addLineOfText(font, text, 0.0f, font.getAscent());
    if (arrangement.getNumGlyphs() < 10)
        return fail("subtitle produced no glyphs in " + font.getTypefaceName().toStdString());

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
        return fail("English subtitle did not paint");

    const auto path = argc > 1 ? std::string(argv[1]) : std::string("/tmp/sync-lost-english.png");
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
