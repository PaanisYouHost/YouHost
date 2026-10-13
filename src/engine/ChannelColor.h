#pragma once

#include "X32Colours.h"

namespace youhost
{

// A strip shows `color`. `own` means the user picked that colour. A channel
// with no colour of its own follows its group.
struct ChannelPaint
{
    int color = 0;
    bool own = false;
};

// An old session has no colorOwn attribute. A stored colour is that channel's
// own. A missing colour still follows the group.
inline bool colorIsOwnInSession(bool attributePresent, bool attributeValue, int color) noexcept
{
    if (attributePresent)
        return attributeValue && normaliseX32Colour(color) != 0;
    return normaliseX32Colour(color) != 0;
}

// Picking a swatch gives the channel its own colour. Off clears that colour,
// so a grouped channel takes the group colour again.
inline void chooseChannelColor(ChannelPaint& paint, int chosen, int groupColor, bool inGroup) noexcept
{
    const int color = normaliseX32Colour(chosen);
    if (color == 0)
    {
        paint.own = false;
        paint.color = inGroup ? normaliseX32Colour(groupColor) : 0;
        return;
    }
    paint.own = true;
    paint.color = color;
}

// Group colour, on create or later. Own colours stay. Everyone else follows.
inline void followGroupColor(ChannelPaint& paint, int groupColor) noexcept
{
    if (paint.own)
        return;
    paint.color = normaliseX32Colour(groupColor);
}

inline void followGroupColorOnMembers(ChannelPaint* paints,
                                      const int* membership,
                                      int count,
                                      int group,
                                      int groupColor) noexcept
{
    if (paints == nullptr || membership == nullptr || count < 1 || group < 0)
        return;
    for (int index = 0; index < count; ++index)
        if (membership[index] == group)
            followGroupColor(paints[index], groupColor);
}

// Leaving a group drops a borrowed colour. An own colour stays on the strip.
inline void leaveGroupColor(ChannelPaint& paint) noexcept
{
    if (! paint.own)
        paint.color = 0;
}

} // namespace youhost
