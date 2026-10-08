#include "engine/DropoutDetect.h"
#include "engine/LatencyMath.h"
#include "engine/MeterLayout.h"
#include "engine/MeterScale.h"
#include "engine/Passthrough.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line)
{
    if (condition)
        return;

    std::cerr << file << ":" << line << "  " << expression << "\n";
    ++failures;
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

bool near(float actual, float expected, float tolerance)
{
    return std::fabs(actual - expected) <= tolerance;
}

youhost::AudioThreadConfig configAt(double sampleRate, const youhost::Routing& routing)
{
    youhost::AudioThreadConfig config;
    config.routing = routing;
    config.meterTiming = youhost::meterTimingFor(sampleRate);
    return config;
}

void runBlocks(std::array<youhost::ChannelStrip, youhost::kMaxChannels>& strips,
               const youhost::AudioThreadConfig& config,
               const std::vector<std::vector<float>>& inputs,
               int times)
{
    std::vector<const float*> inputPointers(inputs.size());
    for (int n = 0; n < times; ++n)
    {
        for (std::size_t channel = 0; channel < inputs.size(); ++channel)
            inputPointers[channel] = inputs[channel].data();

        youhost::processPassthrough(inputPointers.data(),
                                    static_cast<int>(inputPointers.size()),
                                    nullptr,
                                    0,
                                    static_cast<int>(inputs.empty() ? 0 : inputs[0].size()),
                                    config,
                                    strips.data(),
                                    youhost::kMaxChannels);
    }
}

void testPassthroughCopiesMatchingChannels()
{
    std::array<bool, youhost::kMaxChannels> inputs {};
    std::array<bool, youhost::kMaxChannels> outputs {};
    inputs[1] = true;
    inputs[3] = true;
    outputs[1] = true;
    outputs[3] = true;
    outputs[4] = true;

    const auto routing = youhost::makeRouting(inputs, outputs);
    CHECK(routing.visibleChannels == 5);
    CHECK(routing.inputCount == 2);
    CHECK(routing.outputCount == 3);
    CHECK(routing.inputPacked[1] == 0);
    CHECK(routing.inputPacked[3] == 1);
    CHECK(routing.outputPacked[4] == 2);

    constexpr int frames = 8;
    float in0[frames];
    float in1[frames];
    float out0[frames];
    float out1[frames];
    float out2[frames];
    for (int i = 0; i < frames; ++i)
    {
        in0[i] = 0.25f;
        in1[i] = -0.5f;
        out0[i] = 7.0f;
        out1[i] = 7.0f;
        out2[i] = 7.0f;
    }

    const float* inputsPacked[] = { in0, in1 };
    float* outputsPacked[] = { out0, out1, out2 };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    const auto config = configAt(48000.0, routing);

    youhost::processPassthrough(inputsPacked, 2, outputsPacked, 3, frames, config, strips.data(), youhost::kMaxChannels);

    for (int i = 0; i < frames; ++i)
    {
        CHECK(out0[i] == 0.25f);
        CHECK(out1[i] == -0.5f);
        CHECK(out2[i] == 0.0f);
    }

    CHECK(strips[1].meter.rms.load() > 0.0f);
    CHECK(strips[0].meter.rms.load() == 0.0f);
    CHECK(strips[3].meter.clipped.load() == false);
}

void testMetersSettleClipAndClear()
{
    std::array<bool, youhost::kMaxChannels> active {};
    active[0] = true;
    const auto config = configAt(48000.0, youhost::makeRouting(active, active));
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};

    constexpr int frames = 480;
    std::vector<float> constant(static_cast<std::size_t>(frames), 0.5f);
    runBlocks(strips, config, { constant }, 200);
    CHECK(near(strips[0].meter.rms.load(), 0.5f, 0.01f));
    CHECK(near(strips[0].meter.peak.load(), 0.5f, 0.001f));
    CHECK(strips[0].meter.clipped.load() == false);

    std::vector<float> hot(static_cast<std::size_t>(frames), 0.0f);
    hot[0] = 1.0f;
    runBlocks(strips, config, { hot }, 1);
    CHECK(strips[0].meter.clipped.load() == true);
    CHECK(strips[0].meter.peak.load() >= 0.99f);

    std::vector<float> silence(static_cast<std::size_t>(frames), 0.0f);
    runBlocks(strips, config, { silence }, 100);
    CHECK(strips[0].meter.clipped.load() == true);
    CHECK(strips[0].meter.peak.load() > 0.95f);

    // Four seconds of silence must leave the clip latched. It used to time out after two.
    runBlocks(strips, config, { silence }, 300);
    CHECK(strips[0].meter.clipped.load() == true);
    CHECK(strips[0].meter.peak.load() < 0.5f);

    strips[0].meter.clearRequested.store(true);
    runBlocks(strips, config, { silence }, 1);
    CHECK(strips[0].meter.clipped.load() == false);

    runBlocks(strips, config, { silence }, 50);
    CHECK(strips[0].meter.clipped.load() == false);

    std::vector<float> almost(static_cast<std::size_t>(frames), 0.999f);
    runBlocks(strips, config, { almost }, 1);
    CHECK(strips[0].meter.clipped.load() == false);
}

void testClipClearWithoutAnOpenInput()
{
    youhost::Routing routing;
    auto config = configAt(48000.0, routing);
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    strips[2].meterState.clipped = true;
    strips[2].meter.clipped.store(true);
    strips[2].meter.clearRequested.store(true);

    float frame[4] = {};
    const float* inputs[] = { frame };
    youhost::processPassthrough(inputs, 1, nullptr, 0, 4, config, strips.data(), youhost::kMaxChannels);
    CHECK(strips[2].meter.clipped.load() == false);
    CHECK(strips[2].meter.clearRequested.load() == false);
}

void testUnwrittenOutputsAreCleared()
{
    youhost::Routing routing;
    float out[4] = { 3.0f, 3.0f, 3.0f, 3.0f };
    float* outputs[] = { out };
    std::array<youhost::ChannelStrip, youhost::kMaxChannels> strips {};
    youhost::processPassthrough(nullptr, 0, outputs, 1, 4, configAt(48000.0, routing), strips.data(), youhost::kMaxChannels);
    for (float sample : out)
        CHECK(sample == 0.0f);

    youhost::processPassthrough(nullptr, 0, nullptr, 0, 0, configAt(48000.0, routing), strips.data(), youhost::kMaxChannels);
}

void testLatencyFormulas()
{
    using youhost::RoundTripFormula;
    CHECK(youhost::formulaForDeviceType("CoreAudio") == RoundTripFormula::coreAudioSubtractOneBuffer);
    CHECK(youhost::formulaForDeviceType("ALSA") == RoundTripFormula::alsaAddOneBuffer);
    CHECK(youhost::formulaForDeviceType("Windows Audio") == RoundTripFormula::driverSum);

    CHECK(youhost::roundTripSamples(210, 180, 128, 0, RoundTripFormula::coreAudioSubtractOneBuffer) == 262);
    CHECK(youhost::roundTripSamples(384, 384, 128, 0, RoundTripFormula::alsaAddOneBuffer) == 896);
    CHECK(youhost::roundTripSamples(100, 80, 64, 32, RoundTripFormula::driverSum) == 212);
    CHECK(youhost::roundTripSamples(10, 10, 128, 0, RoundTripFormula::coreAudioSubtractOneBuffer) == 0);
    CHECK(near(static_cast<float>(youhost::samplesToMilliseconds(48, 48000.0)), 1.0f, 0.0001f));
    CHECK(youhost::samplesToMilliseconds(48, 0.0) == 0.0);
}

void testMeterLayoutScales()
{
    const auto stereo = youhost::layoutMeters(2, 800.0f, 300.0f);
    CHECK(stereo.columns == 2);
    CHECK(stereo.rows == 1);
    CHECK(stereo.cellWidth <= 42.0f);

    const auto desk = youhost::layoutMeters(32, 1100.0f, 400.0f);
    CHECK(desk.columns == 32);
    CHECK(desk.rows == 1);

    const auto full = youhost::layoutMeters(128, 1100.0f, 400.0f);
    CHECK(full.columns * full.rows >= 128);
    CHECK(full.rows >= 2);
    CHECK(full.cellWidth >= 16.0f);

    const auto none = youhost::layoutMeters(0, 400.0f, 200.0f);
    CHECK(none.cellWidth == 0.0f);

    std::array<bool, youhost::kMaxChannels> all {};
    all.fill(true);
    const auto routing = youhost::makeRouting(all, all);
    CHECK(routing.inputCount == 128);
    CHECK(routing.outputCount == 128);
    CHECK(routing.visibleChannels == 128);
    CHECK(routing.inputPacked[127] == 127);
}

void testMeterScales()
{
    CHECK(youhost::normaliseRmsReferenceDb(-20) == -20);
    CHECK(youhost::normaliseRmsReferenceDb(-19) == -20);
    CHECK(youhost::normaliseRmsReferenceDb(-15) == -14);
    CHECK(youhost::normaliseRmsReferenceDb(0) == -14);

    youhost::MeterTick ticks[9];
    CHECK(youhost::rmsTicks(-20, ticks, 9) == 9);
    CHECK(ticks[0].label == 20);
    CHECK(ticks[0].dbFs == 0.0f);
    CHECK(ticks[3].label == 0);
    CHECK(ticks[3].dbFs == -20.0f);
    CHECK(ticks[8].label == -40);
    CHECK(ticks[8].dbFs == -60.0f);

    CHECK(youhost::rmsTicks(-18, ticks, 9) == 9);
    CHECK(ticks[0].dbFs == 2.0f);
    CHECK(ticks[3].dbFs == -18.0f);

    CHECK(youhost::rmsTicks(-14, ticks, 9) == 9);
    CHECK(ticks[0].dbFs == 6.0f);
    CHECK(ticks[3].dbFs == -14.0f);
    CHECK(ticks[8].dbFs == -54.0f);

    CHECK(youhost::peakTicks(ticks, 9) == 8);
    CHECK(ticks[0].label == 0);
    CHECK(ticks[0].dbFs == 0.0f);
    CHECK(ticks[2].label == -6);
    CHECK(ticks[7].label == -60);
    CHECK(ticks[7].dbFs == -60.0f);

    const float fullScale = 1.0f;
    const float line = 0.1f; // -20 dBFS
    const float floor = 0.001f; // -60 dBFS
    CHECK(near(youhost::meterNormal(fullScale, false, -20), 1.0f, 0.001f));
    CHECK(near(youhost::meterNormal(line, false, -20), 40.0f / 60.0f, 0.001f));
    CHECK(near(youhost::meterNormal(floor, false, -20), 0.0f, 0.001f));
    // -14 dBFS reference: full scale is only +14 VU, short of the +20 tick.
    CHECK(near(youhost::meterNormal(fullScale, false, -14), 54.0f / 60.0f, 0.001f));
    CHECK(near(youhost::meterNormal(fullScale, true, -14), 1.0f, 0.001f));
    CHECK(near(youhost::meterNormal(floor, true, -20), 0.0f, 0.001f));
}

void testDropoutDecisions()
{
    const int64_t period = youhost::expectedPeriodNs(48000.0, 128);
    CHECK(period == 2666667);
    CHECK(youhost::expectedPeriodNs(0.0, 128) == 0);
    CHECK(youhost::dropoutGapCount(0, period, period, false) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period, period, true) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period, period, false) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period + 1000000, period, false) == 0);
    CHECK(youhost::dropoutGapCount(1000, 1000 + period * 2, period, false) == 1);
    CHECK(youhost::dropoutOverrunCount(period, period) == 0);
    CHECK(youhost::dropoutOverrunCount(period + 1, period) == 1);
    CHECK(youhost::dropoutOverrunCount(period, 0) == 0);
}

void testRaiseUnit()
{
    CHECK(near(youhost::raiseUnit(0.5f, 4), 0.0625f, 0.00001f));
    CHECK(youhost::raiseUnit(0.5f, 0) == 1.0f);
    CHECK(youhost::raiseUnit(0.0f, 3) == 0.0f);
}

} // namespace

int main()
{
    testRaiseUnit();
    testPassthroughCopiesMatchingChannels();
    testMetersSettleClipAndClear();
    testClipClearWithoutAnOpenInput();
    testMeterScales();
    testDropoutDecisions();
    testUnwrittenOutputsAreCleared();
    testLatencyFormulas();
    testMeterLayoutScales();

    if (failures != 0)
    {
        std::cerr << failures << " check(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "YouHost engine tests passed\n";
    return EXIT_SUCCESS;
}
