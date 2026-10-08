#pragma once

#include "engine/MeterScale.h"

#include <juce_data_structures/juce_data_structures.h>

#include <algorithm>
#include <memory>

namespace youhost
{

class AppSettings
{
public:
    AppSettings()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = "YouHost";
        options.filenameSuffix = ".xml";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName = "Ambient Audio/YouHost";
        options.storageFormat = juce::PropertiesFile::storeAsXML;
        properties_.setStorageParameters(options);
    }

    std::unique_ptr<juce::XmlElement> loadAudioSetup()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getXmlValue("audioSetup");
        return {};
    }

    void saveAudioSetup(const juce::XmlElement* xml)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            if (xml != nullptr)
                settings->setValue("audioSetup", xml);
            else
                settings->removeValue("audioSetup");
            settings->saveIfNeeded();
        }
    }

    bool loadPeakMeter()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getBoolValue("peakMeter", false);
        return false;
    }

    void savePeakMeter(bool peak)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("peakMeter", peak);
            settings->saveIfNeeded();
        }
    }

    int loadRmsReferenceDb()
    {
        if (auto* settings = properties_.getUserSettings())
            return normaliseRmsReferenceDb(settings->getIntValue("rmsReferenceDb", kDefaultRmsReferenceDb));
        return kDefaultRmsReferenceDb;
    }

    void saveRmsReferenceDb(int db)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("rmsReferenceDb", normaliseRmsReferenceDb(db));
            settings->saveIfNeeded();
        }
    }

    int loadWavBitDepth()
    {
        if (auto* settings = properties_.getUserSettings())
            return normaliseWavBitDepth(settings->getIntValue("wavBitDepth", kDefaultWavBitDepth));
        return kDefaultWavBitDepth;
    }

    void saveWavBitDepth(int bits)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("wavBitDepth", normaliseWavBitDepth(bits));
            settings->saveIfNeeded();
        }
    }

    juce::String loadWindowState()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getValue("windowState");
        return {};
    }

    void saveWindowState(const juce::String& state)
    {
        saveNamedWindow("windowState", state);
    }

    juce::String loadNamedWindow(const juce::String& key)
    {
        if (key.isEmpty())
            return {};
        if (auto* settings = properties_.getUserSettings())
            return settings->getValue(key);
        return {};
    }

    void saveNamedWindow(const juce::String& key, const juce::String& state)
    {
        if (key.isEmpty())
            return;
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue(key, state);
            settings->saveIfNeeded();
        }
    }

    void save()
    {
        if (auto* settings = properties_.getUserSettings())
            settings->saveIfNeeded();
    }

    std::unique_ptr<juce::XmlElement> loadKnownPlugins()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getXmlValue("knownPlugins");
        return {};
    }

    void saveKnownPlugins(const juce::XmlElement* xml)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            if (xml != nullptr)
                settings->setValue("knownPlugins", xml);
            else
                settings->removeValue("knownPlugins");
            settings->saveIfNeeded();
        }
    }

    std::unique_ptr<juce::XmlElement> loadScanFailures()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getXmlValue("scanFailures");
        return {};
    }

    void saveScanFailures(const juce::XmlElement* xml)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            if (xml != nullptr)
                settings->setValue("scanFailures", xml);
            else
                settings->removeValue("scanFailures");
            settings->saveIfNeeded();
        }
    }

    bool loadScanWavesShells()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getBoolValue("scanWavesShells", false);
        return false;
    }

    void saveScanWavesShells(bool enabled)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("scanWavesShells", enabled);
            settings->saveIfNeeded();
        }
    }

    bool loadShowAppleInserts()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getBoolValue("showAppleInserts", false);
        return false;
    }

    void saveShowAppleInserts(bool enabled)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("showAppleInserts", enabled);
            settings->saveIfNeeded();
        }
    }

    bool loadShowInstrumentInserts()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getBoolValue("showInstrumentInserts", false);
        return false;
    }

    void saveShowInstrumentInserts(bool enabled)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("showInstrumentInserts", enabled);
            settings->saveIfNeeded();
        }
    }

    juce::StringArray loadRecentSessions()
    {
        juce::StringArray lines;
        if (auto* settings = properties_.getUserSettings())
            lines.addLines(settings->getValue("recentSessions"));
        lines.removeEmptyStrings();
        while (lines.size() > 10)
            lines.remove(lines.size() - 1);
        return lines;
    }

    void rememberRecentSession(const juce::String& folder)
    {
        if (folder.isEmpty())
            return;
        if (auto* settings = properties_.getUserSettings())
        {
            auto lines = loadRecentSessions();
            lines.removeString(folder);
            lines.insert(0, folder);
            while (lines.size() > 10)
                lines.remove(lines.size() - 1);
            settings->setValue("recentSessions", lines.joinIntoString("\n"));
            settings->saveIfNeeded();
        }
    }

    juce::String loadLastSessionFolder()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getValue("lastSessionFolder");
        return {};
    }

    void saveLastSessionFolder(const juce::String& folder)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("lastSessionFolder", folder);
            settings->saveIfNeeded();
        }
    }

    // Parent folder of the last session the user actually chose (not the internal fallback).
    juce::String loadSessionParentFolder()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getValue("sessionParentFolder");
        return {};
    }

    void saveSessionParentFolder(const juce::String& folder)
    {
        if (folder.isEmpty())
            return;
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("sessionParentFolder", folder);
            settings->saveIfNeeded();
        }
    }

    int loadTimelineHeight()
    {
        if (auto* settings = properties_.getUserSettings())
            return std::max(0, settings->getIntValue("timelineHeight", 0));
        return 0;
    }

    void saveTimelineHeight(int height)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("timelineHeight", std::max(0, height));
            settings->saveIfNeeded();
        }
    }

    juce::StringArray loadSuspiciousPlugins()
    {
        juce::StringArray lines;
        if (auto* settings = properties_.getUserSettings())
            lines.addLines(settings->getValue("suspiciousPlugins"));
        lines.removeEmptyStrings();
        return lines;
    }

    void addSuspiciousPlugin(const juce::String& identifier)
    {
        if (identifier.isEmpty())
            return;
        auto lines = loadSuspiciousPlugins();
        if (! lines.contains(identifier))
            lines.add(identifier);
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("suspiciousPlugins", lines.joinIntoString("\n"));
            settings->saveIfNeeded();
        }
    }

    juce::File supportDirectory()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getFile().getParentDirectory();

        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("Ambient Audio")
            .getChildFile("YouHost");
    }

private:
    juce::ApplicationProperties properties_;
};

} // namespace youhost
