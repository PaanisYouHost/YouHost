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

private:
    juce::ApplicationProperties properties_;
};

} // namespace youhost
