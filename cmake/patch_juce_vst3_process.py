#!/usr/bin/env python3
"""Make VST3 process wait the way an Audio Unit render does: not at all.

The audio thread was off-CPU for more than half of each VST3 process. Two
host calls did that, and the Audio Unit path has neither.

performEdit and the output-parameter listener ran on the audio thread. They
take the parameter lock and call the edit controller, which waits on the
message thread. An Audio Unit render does not. The value is queued and the
message thread applies it.

ProcessContext was only the sample rate. An Audio Unit render is already
inside the device interval and has the HAL clock. A VST3 that needs that
clock waits. Fill system time, the running sample count, and playing.
"""

from pathlib import Path
import sys

MARKER = "youhostContinuousSamples"

OLD_DISPATCHER = """    void start (Vst::IEditController& controllerIn)
    {
        controller = &controllerIn;
        cache = CachedParamValues { getAllParamIDs (controllerIn) };
        startTimerHz (60);
    }

    void flush()
    {
        cache.ifSet ([this] (Steinberg::int32 index, float value)
        {
            controller->setParamNormalized (cache.getParamID (index), value);
        });
    }

private:
    void timerCallback() override
    {
        flush();
    }

    CachedParamValues cache;
    Vst::IEditController* controller = nullptr;
};
"""

NEW_DISPATCHER = """    using FlushCallback = void (*) (void*, Vst::ParamID, float);

    void start (Vst::IEditController& controllerIn, void* contextIn = nullptr, FlushCallback callbackIn = nullptr)
    {
        controller = &controllerIn;
        flushContext = contextIn;
        flushCallback = callbackIn;
        cache = CachedParamValues { getAllParamIDs (controllerIn) };
        startTimerHz (60);
    }

    void flush()
    {
        cache.ifSet ([this] (Steinberg::int32 index, float value)
        {
            const auto id = cache.getParamID (index);
            controller->setParamNormalized (id, value);

            // Listeners run on the message thread. process() only queues.
            if (flushCallback != nullptr)
                flushCallback (flushContext, id, value);
        });
    }

private:
    void timerCallback() override
    {
        flush();
    }

    CachedParamValues cache;
    Vst::IEditController* controller = nullptr;
    void* flushContext = nullptr;
    FlushCallback flushCallback = nullptr;
};
"""

OLD_CACHE = """        void setValueWithoutUpdatingProcessor (float newValue)
        {
            if (! exactlyEqual (pluginInstance.cachedParamValues.exchangeWithoutNotifying (vstParamIndex, newValue), newValue))
                sendValueChangedMessageToListeners (newValue);
        }
"""

NEW_CACHE = """        void cacheFromAudioThread (float newValue)
        {
            pluginInstance.cachedParamValues.exchangeWithoutNotifying (vstParamIndex, newValue);
        }

        void notifyHostFromMessageThread (float newValue)
        {
            sendValueChangedMessageToListeners (newValue);
        }

        void setValueWithoutUpdatingProcessor (float newValue)
        {
            if (exactlyEqual (pluginInstance.cachedParamValues.exchangeWithoutNotifying (vstParamIndex, newValue), newValue))
                return;

            // process() is not the message thread. Notifying here takes
            // listenerLock and waits on the message thread.
            auto* manager = MessageManager::getInstanceWithoutCreating();

            if (manager != nullptr && manager->isThisTheMessageThread())
                sendValueChangedMessageToListeners (newValue);
        }
"""

OLD_START = """        parameterDispatcher.start (*editController);
"""

NEW_START = """        parameterDispatcher.start (*editController, this, &VST3PluginInstanceHeadless::youhostFlushParameter);
"""

OLD_BEGIN = """tresult VST3HostContextHeadless::beginEdit (Vst::ParamID paramID)
{
    if (plugin == nullptr)
        return kResultTrue;

    if (auto* param = plugin->getParameterForID (paramID))
"""

NEW_BEGIN = """tresult VST3HostContextHeadless::beginEdit (Vst::ParamID paramID)
{
    if (plugin == nullptr)
        return kResultTrue;

    // A gesture takes listenerLock. process() must not wait on it.
    if (auto* manager = MessageManager::getInstanceWithoutCreating())
        if (! manager->isThisTheMessageThread())
            return kResultTrue;

    if (auto* param = plugin->getParameterForID (paramID))
"""

OLD_PERFORM = """tresult VST3HostContextHeadless::performEdit (Vst::ParamID paramID, Vst::ParamValue valueNormalised)
{
    if (plugin == nullptr)
        return kResultTrue;

    if (auto* param = plugin->getParameterForID (paramID))
    {
        param->setValueFromPerformEdit ((float) valueNormalised);

        // did the plug-in already update the parameter internally
        if (! approximatelyEqual (plugin->editController->getParamNormalized (paramID), valueNormalised))
            return plugin->editController->setParamNormalized (paramID, valueNormalised);

        return kResultTrue;
    }

    return kResultFalse;
}
"""

NEW_PERFORM = """tresult VST3HostContextHeadless::performEdit (Vst::ParamID paramID, Vst::ParamValue valueNormalised)
{
    if (plugin == nullptr)
        return kResultTrue;

    if (auto* param = plugin->getParameterForID (paramID))
    {
        // process() calls this every block for meters. The edit controller
        // then waits on the message thread, which is the off-CPU half of the
        // VST3 call. An Audio Unit render does not call back into the view.
        if (auto* manager = MessageManager::getInstanceWithoutCreating())
        {
            if (! manager->isThisTheMessageThread())
            {
                param->cacheFromAudioThread ((float) valueNormalised);
                plugin->parameterDispatcher.push (param->getVstParamIndex(), (float) valueNormalised);
                return kResultTrue;
            }
        }

        param->setValueFromPerformEdit ((float) valueNormalised);

        // did the plug-in already update the parameter internally
        if (plugin->editController != nullptr
            && ! approximatelyEqual (plugin->editController->getParamNormalized (paramID), valueNormalised))
            return plugin->editController->setParamNormalized (paramID, valueNormalised);

        return kResultTrue;
    }

    return kResultFalse;
}
"""

OLD_END = """tresult VST3HostContextHeadless::endEdit (Vst::ParamID paramID)
{
    if (plugin == nullptr)
        return kResultTrue;

    if (auto* param = plugin->getParameterForID (paramID))
"""

NEW_END = """tresult VST3HostContextHeadless::endEdit (Vst::ParamID paramID)
{
    if (plugin == nullptr)
        return kResultTrue;

    if (auto* manager = MessageManager::getInstanceWithoutCreating())
        if (! manager->isThisTheMessageThread())
            return kResultTrue;

    if (auto* param = plugin->getParameterForID (paramID))
"""

OLD_TIMING_MEMBER = """    Vst::ProcessContext timingInfo; //< Only use this in processBlock()!
"""

NEW_TIMING_MEMBER = """    Vst::ProcessContext timingInfo; //< Only use this in processBlock()!
    Steinberg::int64 youhostContinuousSamples = 0;

    static void youhostFlushParameter (void* context, Vst::ParamID id, float value)
    {
        auto* self = static_cast<VST3PluginInstanceHeadless*> (context);

        if (self == nullptr)
            return;

        if (auto* param = self->getParameterForID (id))
            param->notifyHostFromMessageThread (value);
    }
"""

OLD_TIMING = """    void updateTimingInformation (Vst::ProcessData& destination, double processSampleRate)
    {
        toProcessContext (timingInfo, getPlayHead(), processSampleRate);
        destination.processContext = &timingInfo;
    }
"""

NEW_TIMING = """    void updateTimingInformation (Vst::ProcessData& destination, double processSampleRate)
    {
        toProcessContext (timingInfo, getPlayHead(), processSampleRate);

        // An Audio Unit render is already on the device clock. VST3 only has
        // this struct. With no play head it was just the sample rate, and the
        // plugin waited inside process().
        if ((timingInfo.state & Vst::ProcessContext::kSystemTimeValid) == 0)
        {
            const auto seconds = Time::highResolutionTicksToSeconds (Time::getHighResolutionTicks());
            timingInfo.systemTime = (Steinberg::int64) (seconds * 1000000000.0);
            timingInfo.state |= Vst::ProcessContext::kSystemTimeValid;
        }

        if ((timingInfo.state & Vst::ProcessContext::kContTimeValid) == 0)
        {
            timingInfo.continousTimeSamples = youhostContinuousSamples;
            timingInfo.state |= Vst::ProcessContext::kContTimeValid;
        }

        if (timingInfo.projectTimeSamples == 0)
            timingInfo.projectTimeSamples = youhostContinuousSamples;

        timingInfo.state |= Vst::ProcessContext::kPlaying;

        if (destination.numSamples > 0)
            youhostContinuousSamples += destination.numSamples;

        destination.processContext = &timingInfo;
    }
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "modules/juce_audio_processors_headless/format_types/juce_VST3PluginFormatImpl.h"
    if not path.is_file():
        print(f"Could not find {path}", file=sys.stderr)
        return 1
    text = path.read_text(encoding="utf-8")
    if MARKER in text:
        return 0
    replacements = [
        (OLD_DISPATCHER, NEW_DISPATCHER, "parameter dispatcher"),
        (OLD_CACHE, NEW_CACHE, "parameter cache"),
        (OLD_START, NEW_START, "dispatcher start"),
        (OLD_BEGIN, NEW_BEGIN, "beginEdit"),
        (OLD_PERFORM, NEW_PERFORM, "performEdit"),
        (OLD_END, NEW_END, "endEdit"),
        (OLD_TIMING_MEMBER, NEW_TIMING_MEMBER, "timing member"),
        (OLD_TIMING, NEW_TIMING, "process context"),
    ]
    for old, new, name in replacements:
        if old not in text:
            print(f"Could not find the VST3 {name}", file=sys.stderr)
            return 1
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
