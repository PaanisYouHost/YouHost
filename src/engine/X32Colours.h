#pragma once

namespace youhost
{

// Behringer X32 scribble-strip colours, in the console's order:
// OFF, RD, GN, YE, BL, MG, CY, WH, then the inverted row OFFi … WHi.
// Inverted colours are the same hue drawn as coloured text on a dark strip
// (OFFi is a light strip with dark text).
struct X32Colour
{
    const char* name;
    const char* code;
    int red;
    int green;
    int blue;
    bool inverted;
};

inline constexpr X32Colour kX32Colours[] = {
    { "Off", "OFF", 0, 0, 0, false },
    { "Red", "RD", 230, 36, 36, false },
    { "Green", "GN", 36, 196, 72, false },
    { "Yellow", "YE", 240, 206, 32, false },
    { "Blue", "BL", 36, 92, 220, false },
    { "Magenta", "MG", 214, 40, 186, false },
    { "Cyan", "CY", 28, 196, 214, false },
    { "White", "WH", 236, 236, 236, false },
    { "Off inverted", "OFFi", 232, 232, 232, true },
    { "Red inverted", "RDi", 230, 36, 36, true },
    { "Green inverted", "GNi", 36, 196, 72, true },
    { "Yellow inverted", "YEi", 240, 206, 32, true },
    { "Blue inverted", "BLi", 36, 92, 220, true },
    { "Magenta inverted", "MGi", 214, 40, 186, true },
    { "Cyan inverted", "CYi", 28, 196, 214, true },
    { "White inverted", "WHi", 236, 236, 236, true },
};

inline constexpr int kX32ColourCount = 16;

inline int normaliseX32Colour(int value) noexcept
{
    if (value < 0 || value >= kX32ColourCount)
        return 0;
    return value;
}

} // namespace youhost
