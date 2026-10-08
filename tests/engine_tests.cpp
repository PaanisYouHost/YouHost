#include "engine/LatencyMath.h"
#include "engine/MeterLayout.h"
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

    strips[0].meter.clearRequested.store(true);
    runBlocks(strips, config, { silence }, 1);
    CHECK(strips[0].meter.clipped.load() == false);

    std::vector<float> almost(static_cast<std::size_t>(frames), 0.999f);
    runBlocks(strips, config, { almost }, 1);
    CHECK(strips[0].meter.clipped.load() == false);
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
