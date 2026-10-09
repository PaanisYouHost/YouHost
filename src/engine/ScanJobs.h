#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace youhost
{

inline char foldChar(unsigned char character)
{
    return static_cast<char>(std::tolower(character));
}

inline bool equalsFold(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (foldChar(static_cast<unsigned char>(left[index])) != foldChar(static_cast<unsigned char>(right[index])))
            return false;
    return true;
}

inline bool containsFold(std::string_view text, std::string_view needle)
{
    if (needle.empty() || text.size() < needle.size())
        return false;
    for (std::size_t start = 0; start + needle.size() <= text.size(); ++start)
    {
        bool match = true;
        for (std::size_t index = 0; index < needle.size(); ++index)
        {
            if (foldChar(static_cast<unsigned char>(text[start + index]))
                != foldChar(static_cast<unsigned char>(needle[index])))
            {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

// Waves shells show up as WaveShell*.vst3 or as an AudioUnit id whose
// manufacturer code is ksWV. One file then enumerates hundreds of plugins.
inline bool isWavesIdentifier(std::string_view identifier)
{
    return containsFold(identifier, "waveshell")
           || containsFold(identifier, "kswv")
           || containsFold(identifier, "/waves/")
           || containsFold(identifier, "\\waves\\");
}

inline bool isShellIdentifier(std::string_view identifier)
{
    return isWavesIdentifier(identifier) || containsFold(identifier, "shell");
}

inline bool isAppleBuiltIn(std::string_view manufacturer, std::string_view identifier)
{
    if (equalsFold(manufacturer, "apple"))
        return true;
    return containsFold(identifier, ",appl")
           || containsFold(identifier, "/system/library/")
           || containsFold(identifier, "\\system\\library\\");
}

// A VST3 scan from moduleinfo stores 0 inputs for every plugin, including effects.
// Channel count is not a substitute for "this is an instrument".
inline bool categoryTokenIsInstrument(std::string_view category)
{
    std::size_t index = 0;
    while (index <= category.size())
    {
        const auto separator = category.find_first_of("|,", index);
        const auto tokenEnd = separator == std::string_view::npos ? category.size() : separator;
        auto token = category.substr(index, tokenEnd - index);
        while (! token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (! token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        if (equalsFold(token, "instrument") || equalsFold(token, "generator") || equalsFold(token, "synth"))
            return true;
        if (separator == std::string_view::npos)
            break;
        index = separator + 1;
    }
    return false;
}

inline bool isAudioUnitInstrumentOrGenerator(std::string_view identifier)
{
    return containsFold(identifier, "audiounit:generators/")
           || containsFold(identifier, "audiounit:synths/");
}

// Every scanned effect stays in the insert list, including Apple Audio Units and
// VST3 plugins that reported no channel count. Instruments and generators are the
// only plugins left out, and only while showInstruments is off.
inline bool showInInsertList(std::string_view identifier,
                             std::string_view category,
                             bool instrument,
                             bool showInstruments)
{
    if (showInstruments)
        return true;
    return ! instrument
           && ! categoryTokenIsInstrument(category)
           && ! isAudioUnitInstrumentOrGenerator(identifier);
}

struct ScanCandidate
{
    std::string format;
    std::string identifier;
    bool shell = false;
    bool waves = false;
};

inline ScanCandidate makeScanCandidate(std::string format, std::string identifier)
{
    ScanCandidate candidate;
    candidate.format = std::move(format);
    candidate.identifier = std::move(identifier);
    candidate.waves = isWavesIdentifier(candidate.identifier);
    candidate.shell = isShellIdentifier(candidate.identifier);
    return candidate;
}

inline bool skipBecauseWaves(const ScanCandidate& candidate, bool scanWavesShells)
{
    return candidate.waves && ! scanWavesShells;
}

// Non-shell plugins first so a slow WaveShell cannot block De-Feedback and the rest.
inline void orderScanCandidates(std::vector<ScanCandidate>& jobs)
{
    std::stable_sort(jobs.begin(), jobs.end(), [](const ScanCandidate& left, const ScanCandidate& right)
    {
        return ! left.shell && right.shell;
    });
}

} // namespace youhost
