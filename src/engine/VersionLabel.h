#pragma once

namespace youhost
{

// One label for the title, the window, help, and About. The numeric version
// is CMake's project VERSION. Tests build without that define and still see
// YouHost 1.0.13. Session files stay at format 7.
inline const char* versionNumber() noexcept
{
#ifdef YOUHOST_VERSION
    return YOUHOST_VERSION;
#else
    return "1.0.13";
#endif
}

inline const char* versionLabel() noexcept
{
#ifdef YOUHOST_VERSION_LABEL
    return YOUHOST_VERSION_LABEL;
#else
    return "YouHost 1.0.13";
#endif
}

} // namespace youhost
