#!/usr/bin/env python3
"""List the plugins inside a VST3 shell without initialising every one.

WaveShell's moduleinfo is often only the wrapper. Walking the factory and
initialising each class takes longer than the scan allows, so the shell
comes back empty. Class info is enough: the unique id is the factory class,
and that is what createPluginInstance loads.
"""

from pathlib import Path
import sys

MARKER = "youhostShellClasses"

OLD_FIND = """void VST3PluginFormatHeadless::findAllTypesForFile (OwnedArray<PluginDescription>& results, const String& fileOrIdentifier)
{
    if (! fileMightContainThisPluginType (fileOrIdentifier))
        return;

    if (const auto fast = DescriptionLister::findDescriptionsFast (File (fileOrIdentifier)); ! fast.empty())
    {
        for (const auto& d : fast)
            results.add (new PluginDescription (d));

        return;
    }
"""

NEW_FIND = """void VST3PluginFormatHeadless::findAllTypesForFile (OwnedArray<PluginDescription>& results, const String& fileOrIdentifier)
{
    if (! fileMightContainThisPluginType (fileOrIdentifier))
        return;

    const auto shellFile = fileOrIdentifier.containsIgnoreCase ("WaveShell")
                           || fileOrIdentifier.containsIgnoreCase ("Shell");
    const auto fast = DescriptionLister::findDescriptionsFast (File (fileOrIdentifier));
    size_t shellNamed = 0;

    for (const auto& d : fast)
        if (d.name.containsIgnoreCase ("WaveShell") || d.name.containsIgnoreCase ("Shell"))
            ++shellNamed;

    // youhostShellClasses: a shell moduleinfo that only names the wrapper is
    // not the plugin list. One ordinary plugin still uses the fast path.
    if (! fast.empty() && (! shellFile || fast.size() > shellNamed))
    {
        for (const auto& d : fast)
            results.add (new PluginDescription (d));

        return;
    }

    if (shellFile)
    {
        for (const auto& file : getLibraryPaths (*this, fileOrIdentifier))
        {
            auto handle = RefCountedDllHandle::getHandle (file);

            if (handle == nullptr)
                continue;

            auto pluginFactory = handle->getPluginFactory();

            if (pluginFactory == nullptr)
                continue;

            for (const auto& d : DescriptionLister::findDescriptionsLight (*pluginFactory, File (file)))
                results.add (new PluginDescription (d));
        }

        if (! results.isEmpty())
            return;
    }
"""

OLD_LISTER_END = """            if (desc.uniqueId != 0)
                result.push_back (desc);
        }

        return result;
    }
};
"""

NEW_LISTER_END = """            if (desc.uniqueId != 0)
                result.push_back (desc);
        }

        return result;
    }

    // Class ids only. Initialising every Waves plugin is what makes the scan time out.
    static std::vector<PluginDescription> findDescriptionsLight (IPluginFactory& factory, const File& file)
    {
        std::vector<PluginDescription> result;
        StringArray foundNames;
        PFactoryInfo factoryInfo;
        factory.getFactoryInfo (&factoryInfo);
        auto companyName = toString (factoryInfo.vendor).trim();
        const auto numClasses = factory.countClasses();

        for (Steinberg::int32 i = 0; i < numClasses; ++i)
        {
            PClassInfo info;
            factory.getClassInfo (i, &info);

            if (std::strcmp (info.category, kVstAudioEffectClass) != 0)
                continue;

            const String name (toString (info.name).trim());

            if (name.containsIgnoreCase ("WaveShell"))
                continue;

            if (foundNames.contains (name, true))
                continue;

            std::unique_ptr<PClassInfo2> info2;
            VSTComSmartPtr<IPluginFactory2> pf2;

            if (pf2.loadFrom (&factory))
            {
                info2.reset (new PClassInfo2());
                pf2->getClassInfo2 (i, info2.get());
            }

            foundNames.add (name);
            PluginDescription desc;
            createPluginDescription (desc, file, companyName, name, info, info2.get(), nullptr, 0, 0);

            if (desc.uniqueId != 0)
                result.push_back (desc);
        }

        return result;
    }
};
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    header = root / "modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h"
    source = root / "modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatHeadless.cpp"
    if not header.is_file() or not source.is_file():
        print(f"Could not find the VST3 shell sources under {root}", file=sys.stderr)
        return 1
    header_text = header.read_text(encoding="utf-8")
    source_text = source.read_text(encoding="utf-8")
    if MARKER in source_text and "findDescriptionsLight" in header_text:
        return 0
    if OLD_LISTER_END not in header_text:
        print("Could not find the VST3 description lister", file=sys.stderr)
        return 1
    if OLD_FIND not in source_text:
        print("Could not find VST3 findAllTypesForFile", file=sys.stderr)
        return 1
    header.write_text(header_text.replace(OLD_LISTER_END, NEW_LISTER_END, 1), encoding="utf-8")
    source.write_text(source_text.replace(OLD_FIND, NEW_FIND, 1), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
