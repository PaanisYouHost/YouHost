#pragma once

namespace youhost
{

// One label for the title, the window, help, and About. The numeric version
// is CMake's project VERSION. Tests build without that define and still see
// Beta 0.9.2.
inline const char* versionNumber() noexcept
{
#ifdef YOUHOST_VERSION
    return YOUHOST_VERSION;
#else
    return "0.9.2";
#endif
}

inline const char* versionLabel() noexcept
{
#ifdef YOUHOST_VERSION_LABEL
    return YOUHOST_VERSION_LABEL;
#else
    return "YouHost Beta 0.9.2";
#endif
}

} // namespace youhost
