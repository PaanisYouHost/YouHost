#!/usr/bin/env python3
"""Keep the real-time callback off the device lock, and copy Core Audio in blocks.

A pthread mutex held for the whole plugin call lets the message thread invert
the audio thread. Core Audio's per-sample walk is the same cost again on a wide
interface. The HAL also gets the whole buffer period, so a plugin that fits the
buffer is not reported as an overload.
"""

from pathlib import Path
import sys

MARKER = "youhostCallbackInFlight"


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    manager = root / "modules/juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp"
    core = root / "modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp"
    if not manager.is_file():
        manager = Path("modules/juce_audio_devices/audio_io/juce_AudioDeviceManager.cpp")
    if not core.is_file():
        core = Path("modules/juce_audio_devices/native/juce_CoreAudio_mac.cpp")

    manager_text = manager.read_text(encoding="utf-8")
    core_text = core.read_text(encoding="utf-8")
    if MARKER in manager_text and "youhostCopyDeviceChannels" in core_text and "kAudioDevicePropertyIOCycleUsage" in core_text:
        return 0

    manager_needle = """void AudioDeviceManager::removeAudioCallback (AudioIODeviceCallback* callbackToRemove)
{
    if (callbackToRemove != nullptr)
    {
        bool needsDeinitialising = currentAudioDevice != nullptr;

        {
            const ScopedLock sl (audioCallbackLock);

            needsDeinitialising = needsDeinitialising && callbacks.contains (callbackToRemove);
            callbacks.removeFirstMatchingValue (callbackToRemove);
        }

        if (needsDeinitialising)
            callbackToRemove->audioDeviceStopped();
    }
}
"""
    manager_insert = """namespace
{
// Raised around the client callback after the device lock is dropped.
// remove/stop observe it, then take the lock only when it is zero, so the
// callback can finish without waiting on the message thread.
std::atomic<int> youhostCallbackInFlight { 0 };
}

void AudioDeviceManager::removeAudioCallback (AudioIODeviceCallback* callbackToRemove)
{
    if (callbackToRemove != nullptr)
    {
        bool needsDeinitialising = false;

        for (;;)
        {
            {
                const ScopedLock sl (audioCallbackLock);

                if (youhostCallbackInFlight.load (std::memory_order_acquire) == 0)
                {
                    needsDeinitialising = currentAudioDevice != nullptr
                                       && callbacks.contains (callbackToRemove);
                    callbacks.removeFirstMatchingValue (callbackToRemove);
                    break;
                }
            }

            Thread::yield();
        }

        if (needsDeinitialising)
            callbackToRemove->audioDeviceStopped();
    }
}
"""

    callback_needle = """void AudioDeviceManager::audioDeviceIOCallbackInt (const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const AudioIODeviceCallbackContext& context)
{
    const ScopedLock sl (audioCallbackLock);

    inputLevelGetter->updateLevel (inputChannelData, numInputChannels, numSamples);

    if (callbacks.size() > 0)
    {
        AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasurer, numSamples);

        tempBuffer.setSize (jmax (1, numOutputChannels), jmax (1, numSamples), false, false, true);

        callbacks.getUnchecked (0)->audioDeviceIOCallbackWithContext (inputChannelData,
                                                                      numInputChannels,
                                                                      outputChannelData,
                                                                      numOutputChannels,
                                                                      numSamples,
                                                                      context);

        auto* const* tempChans = tempBuffer.getArrayOfWritePointers();

        for (int i = callbacks.size(); --i > 0;)
        {
            callbacks.getUnchecked (i)->audioDeviceIOCallbackWithContext (inputChannelData,
                                                                          numInputChannels,
                                                                          tempChans,
                                                                          numOutputChannels,
                                                                          numSamples,
                                                                          context);

            for (int chan = 0; chan < numOutputChannels; ++chan)
            {
                if (auto* src = tempChans [chan])
                    if (auto* dst = outputChannelData [chan])
                        for (int j = 0; j < numSamples; ++j)
                            dst[j] += src[j];
            }
        }
    }
    else
    {
        for (int i = 0; i < numOutputChannels; ++i)
            zeromem (outputChannelData[i], (size_t) numSamples * sizeof (float));
    }

    if (testSound != nullptr)
    {
        auto numSamps = jmin (numSamples, testSound->getNumSamples() - testSoundPosition);
        auto* src = testSound->getReadPointer (0, testSoundPosition);

        for (int i = 0; i < numOutputChannels; ++i)
            if (auto* dst = outputChannelData [i])
                for (int j = 0; j < numSamps; ++j)
                    dst[j] += src[j];

        testSoundPosition += numSamps;

        if (testSoundPosition >= testSound->getNumSamples())
            testSound.reset();
    }

    outputLevelGetter->updateLevel (outputChannelData, numOutputChannels, numSamples);
}
"""
    callback_insert = """void AudioDeviceManager::audioDeviceIOCallbackInt (const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const AudioIODeviceCallbackContext& context)
{
    constexpr int youhostMaxCallbacks = 8;
    AudioIODeviceCallback* youhostCallbacks[youhostMaxCallbacks] {};
    int youhostCallbackCount = 0;

    struct YouHostInFlight
    {
        bool armed = false;

        ~YouHostInFlight()
        {
            if (armed)
                youhostCallbackInFlight.fetch_sub (1, std::memory_order_release);
        }
    };

    YouHostInFlight youhostInFlight;

    {
        const ScopedLock sl (audioCallbackLock);
        youhostCallbackCount = jmin (callbacks.size(), youhostMaxCallbacks);

        for (int i = 0; i < youhostCallbackCount; ++i)
            youhostCallbacks[i] = callbacks.getUnchecked (i);

        if (youhostCallbackCount > 0)
        {
            youhostCallbackInFlight.fetch_add (1, std::memory_order_release);
            youhostInFlight.armed = true;
        }
    }

    inputLevelGetter->updateLevel (inputChannelData, numInputChannels, numSamples);

    if (youhostCallbackCount > 0)
    {
        AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasurer, numSamples);

        // One client writes the device buffer. The temp buffer is only for a mix.
        if (youhostCallbackCount > 1)
            tempBuffer.setSize (jmax (1, numOutputChannels), jmax (1, numSamples), false, false, true);

        youhostCallbacks[0]->audioDeviceIOCallbackWithContext (inputChannelData,
                                                               numInputChannels,
                                                               outputChannelData,
                                                               numOutputChannels,
                                                               numSamples,
                                                               context);

        if (youhostCallbackCount > 1)
        {
            auto* const* tempChans = tempBuffer.getArrayOfWritePointers();

            for (int i = youhostCallbackCount; --i > 0;)
            {
                youhostCallbacks[i]->audioDeviceIOCallbackWithContext (inputChannelData,
                                                                       numInputChannels,
                                                                       tempChans,
                                                                       numOutputChannels,
                                                                       numSamples,
                                                                       context);

                for (int chan = 0; chan < numOutputChannels; ++chan)
                {
                    if (auto* src = tempChans [chan])
                        if (auto* dst = outputChannelData [chan])
                            for (int j = 0; j < numSamples; ++j)
                                dst[j] += src[j];
                }
            }
        }
    }
    else
    {
        for (int i = 0; i < numOutputChannels; ++i)
            zeromem (outputChannelData[i], (size_t) numSamples * sizeof (float));
    }

    if (const ScopedTryLock youhostTry (audioCallbackLock); youhostTry.isLocked() && testSound != nullptr)
    {
        auto numSamps = jmin (numSamples, testSound->getNumSamples() - testSoundPosition);
        auto* src = testSound->getReadPointer (0, testSoundPosition);

        for (int i = 0; i < numOutputChannels; ++i)
            if (auto* dst = outputChannelData [i])
                for (int j = 0; j < numSamps; ++j)
                    dst[j] += src[j];

        testSoundPosition += numSamps;

        if (testSoundPosition >= testSound->getNumSamples())
            testSound.reset();
    }

    outputLevelGetter->updateLevel (outputChannelData, numOutputChannels, numSamples);
}
"""

    wait_needle = """        const ScopedLock sl (audioCallbackLock);

        for (int i = callbacks.size(); --i >= 0;)
            callbacks.getUnchecked (i)->audioDeviceAboutToStart (device);
"""
    wait_insert = """        const ScopedLock sl (audioCallbackLock);

        while (youhostCallbackInFlight.load (std::memory_order_acquire) != 0)
            Thread::yield();

        for (int i = callbacks.size(); --i >= 0;)
            callbacks.getUnchecked (i)->audioDeviceAboutToStart (device);
"""
    stopped_needle = """    const ScopedLock sl (audioCallbackLock);

    loadMeasurer.reset();

    for (int i = callbacks.size(); --i >= 0;)
        callbacks.getUnchecked (i)->audioDeviceStopped();
"""
    stopped_insert = """    const ScopedLock sl (audioCallbackLock);

    while (youhostCallbackInFlight.load (std::memory_order_acquire) != 0)
        Thread::yield();

    loadMeasurer.reset();

    for (int i = callbacks.size(); --i >= 0;)
        callbacks.getUnchecked (i)->audioDeviceStopped();
"""
    error_needle = """    const ScopedLock sl (audioCallbackLock);

    for (int i = callbacks.size(); --i >= 0;)
        callbacks.getUnchecked (i)->audioDeviceError (message);
"""
    error_insert = """    const ScopedLock sl (audioCallbackLock);

    while (youhostCallbackInFlight.load (std::memory_order_acquire) != 0)
        Thread::yield();

    for (int i = callbacks.size(); --i >= 0;)
        callbacks.getUnchecked (i)->audioDeviceError (message);
"""

    if manager_needle not in manager_text or callback_needle not in manager_text:
        print("Could not find the device-callback lock", file=sys.stderr)
        return 1
    if wait_needle not in manager_text or stopped_needle not in manager_text or error_needle not in manager_text:
        print("Could not find the device-callback wait sites", file=sys.stderr)
        return 1

    manager_text = manager_text.replace(manager_needle, manager_insert, 1)
    manager_text = manager_text.replace(callback_needle, callback_insert, 1)
    manager_text = manager_text.replace(wait_needle, wait_insert, 1)
    manager_text = manager_text.replace(stopped_needle, stopped_insert, 1)
    manager_text = manager_text.replace(error_needle, error_insert, 1)
    manager.write_text(manager_text, encoding="utf-8")

    copy_needle = """    template <typename Fn>
    static void forEachActiveSample (const AudioBufferList* buffers,
                                     const ChannelMap& activeChannelMap,
                                     int numSamples,
                                     Fn sampleCallback)
    {
        if (buffers == nullptr)
            return;

        auto deviceChannel = 0;

        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
        {
            const auto& buffer = buffers->mBuffers[i];
            const auto numChannels = (size_t) buffer.mNumberChannels;
            auto* bufferData = (float*) buffer.mData;

            for (size_t bufferChannel = 0; bufferChannel < numChannels; ++bufferChannel, ++deviceChannel)
            {
                const auto activeChannel = activeChannelMap.get (deviceChannel);

                if (! activeChannel)
                   continue;

                auto* data = bufferData + bufferChannel;

                for (auto index = 0; index < numSamples; ++index, data += numChannels)
                    sampleCallback (*activeChannel, index, *data);
            }
        }
    }
"""
    copy_insert = """    // Non-interleaved device buffers are one memcpy per channel. An interleaved
    // buffer stays a tight stride copy. The per-sample lambda was a call per sample.
    static void youhostCopyDeviceChannels (const AudioBufferList* buffers,
                                           const ChannelMap& activeChannelMap,
                                           int numSamples,
                                           float* const* planes,
                                           bool toDevice)
    {
        if (buffers == nullptr || planes == nullptr || numSamples <= 0)
            return;

        auto deviceChannel = 0;

        for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i)
        {
            const auto& buffer = buffers->mBuffers[i];
            const auto numChannels = (size_t) buffer.mNumberChannels;
            auto* bufferData = static_cast<float*> (buffer.mData);

            for (size_t bufferChannel = 0; bufferChannel < numChannels; ++bufferChannel, ++deviceChannel)
            {
                const auto activeChannel = activeChannelMap.get (deviceChannel);

                if (! activeChannel || bufferData == nullptr)
                    continue;

                float* plane = planes[*activeChannel];

                if (plane == nullptr)
                    continue;

                if (numChannels == 1)
                {
                    const auto bytes = (size_t) numSamples * sizeof (float);

                    if (toDevice)
                        memcpy (bufferData, plane, bytes);
                    else
                        memcpy (plane, bufferData, bytes);

                    continue;
                }

                auto* data = bufferData + bufferChannel;

                if (toDevice)
                {
                    for (int index = 0; index < numSamples; ++index, data += (int) numChannels)
                        *data = plane[index];
                }
                else
                {
                    for (int index = 0; index < numSamples; ++index, data += (int) numChannels)
                        plane[index] = *data;
                }
            }
        }
    }
"""
    input_needle = """        forEachActiveSample (inInputData,
                             activeChannelMap[toUnderlyingType (Direction::input)],
                             numSamples,
                             [&] (auto channel, auto sample, auto value)
        {
            inputSamples[channel][sample] = value;
        });
"""
    input_insert = """        youhostCopyDeviceChannels (inInputData,
                                   activeChannelMap[toUnderlyingType (Direction::input)],
                                   numSamples,
                                   inputSamples,
                                   false);
"""
    output_needle = """        forEachActiveSample (outOutputData,
                             activeChannelMap[toUnderlyingType (Direction::output)],
                             numSamples,
                             [&] (auto channel, auto sample, auto& value)
        {
            value = outputSamples[channel][sample];
        });
"""
    output_insert = """        youhostCopyDeviceChannels (outOutputData,
                                   activeChannelMap[toUnderlyingType (Direction::output)],
                                   numSamples,
                                   const_cast<float* const*> (outputSamples),
                                   true);
"""
    start_needle = """    bool start()
    {
        numOverloads = 0;
        return created
            && device.isAlive()
            && tryMultiple ([&] { return checkStatus (AudioDeviceStart (device.getId(), processorId)); }, 3);
    }
"""
    start_insert = """    bool start()
    {
        numOverloads = 0;
        // The whole period belongs to this callback. A smaller IOCycleUsage makes
        // the HAL report an overload while the plugin is still inside the buffer.
        const Float32 wholeCycle = 1.0f;
        device.setProperty (kAudioDevicePropertyIOCycleUsage, wholeCycle);
        return created
            && device.isAlive()
            && tryMultiple ([&] { return checkStatus (AudioDeviceStart (device.getId(), processorId)); }, 3);
    }
"""

    if copy_needle not in core_text or input_needle not in core_text or output_needle not in core_text or start_needle not in core_text:
        print("Could not find the Core Audio sample copy", file=sys.stderr)
        return 1

    core_text = core_text.replace(copy_needle, copy_insert, 1)
    core_text = core_text.replace(input_needle, input_insert, 1)
    core_text = core_text.replace(output_needle, output_insert, 1)
    core_text = core_text.replace(start_needle, start_insert, 1)
    core.write_text(core_text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
