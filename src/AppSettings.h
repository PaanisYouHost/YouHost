#pragma once

#include "engine/MeterScale.h"

#include <juce_data_structures/juce_data_structures.h>

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

    juce::String loadWindowState()
    {
        if (auto* settings = properties_.getUserSettings())
            return settings->getValue("windowState");
        return {};
    }

    void saveWindowState(const juce::String& state)
    {
        if (auto* settings = properties_.getUserSettings())
        {
            settings->setValue("windowState", state);
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
