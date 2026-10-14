#!/usr/bin/env python3
"""Log every standard VST3 channel-context key before JUCE keeps only name and colour."""

import pathlib
import sys

MARKER = "YouMix channel context log"

SNIPPET = r"""
                // YouMix channel context log. JUCE otherwise keeps only the name and colour.
                {
                    juce::String youMixReport = "vst3 setChannelContextInfos";
                    if (instance != nullptr)
                        youMixReport = instance->getName() + " " + youMixReport;

                    auto youMixNoteString = [&] (const char* key)
                    {
                        Vst::String128 value {};
                        if (list->getString (key, value, sizeof (value)) == kResultTrue)
                            youMixReport += juce::String (" | ") + key + "=\"" + toString (value) + "\"";
                        else
                            youMixReport += juce::String (" | ") + key + "=<absent>";
                    };
                    auto youMixNoteInt = [&] (const char* key)
                    {
                        Steinberg::int64 value = 0;
                        if (list->getInt (key, value) == kResultTrue)
                            youMixReport += juce::String (" | ") + key + "=" + juce::String (static_cast<juce::int64> (value));
                        else
                            youMixReport += juce::String (" | ") + key + "=<absent>";
                    };
                    youMixNoteString (Vst::ChannelContext::kChannelNameKey);
                    youMixNoteInt (Vst::ChannelContext::kChannelNameLengthKey);
                    youMixNoteString (Vst::ChannelContext::kChannelUIDKey);
                    youMixNoteInt (Vst::ChannelContext::kChannelRuntimeIDKey);
                    youMixNoteInt (Vst::ChannelContext::kChannelIndexKey);
                    youMixNoteInt (Vst::ChannelContext::kChannelColorKey);
                    youMixNoteString (Vst::ChannelContext::kChannelIndexNamespaceKey);
                    youMixNoteInt (Vst::ChannelContext::kChannelIndexNamespaceOrderKey);
                    youMixNoteInt (Vst::ChannelContext::kChannelPluginLocationKey);
                    youMixNoteString ("track name");
                    youMixNoteString ("Track Name");
                    youMixNoteString ("trackName");
                    const auto youMixLogDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                                  .getChildFile ("Library/Logs/YouMix");
                    youMixLogDir.createDirectory();
                    youMixLogDir.getChildFile ("host-track.log")
                        .appendText (juce::Time::getCurrentTime().toISO8601 (true) + " " + youMixReport + "\n");
                }

"""

ANCHOR = """            if (list != nullptr)
            {
                AudioProcessor::TrackProperties trackProperties;
"""

REPLACEMENT = """            if (list != nullptr)
            {
""" + SNIPPET + """                AudioProcessor::TrackProperties trackProperties;
"""


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: log_vst3_channel_context.py <juce_audio_plugin_client_VST3.cpp>", file=sys.stderr)
        return 2
    path = pathlib.Path(sys.argv[1])
    text = path.read_text()
    if MARKER in text:
        print("already patched", path)
        return 0
    if ANCHOR not in text:
        print("anchor not found in", path, file=sys.stderr)
        return 1
    path.write_text(text.replace(ANCHOR, REPLACEMENT, 1))
    print("patched", path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
