// Shared helpers for the plugin's tests. Test-only; nothing here ships.

#pragma once

#include "XerumAudioProcessor.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace xerum::plugin::tests
{

inline model::Value number (const double value)
{
    model::Value out;
    out.type = model::Value::Type::Float;
    out.real = value;
    return out;
}

inline model::Value text (std::string value)
{
    model::Value out;
    out.type = model::Value::Type::String;
    out.text = std::move (value);
    return out;
}

/// Render `blocks` blocks of `blockSize`, collecting channel 0. `midi` is called per block so a test can
/// play a note, release it, or automate something part-way through a render.
inline std::vector<float> render (XerumAudioProcessor& processor, const int blocks, const int blockSize,
                                  const std::function<void (juce::MidiBuffer&, int)>& midi = {},
                                  const int channels = 2)
{
    juce::AudioBuffer<float> buffer (channels, blockSize);
    std::vector<float> collected;
    collected.reserve (static_cast<std::size_t> (blocks) * static_cast<std::size_t> (blockSize));

    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        juce::MidiBuffer events;

        if (midi)
            midi (events, block);

        processor.processBlock (buffer, events);

        for (int i = 0; i < blockSize; ++i)
            collected.push_back (buffer.getSample (0, i));
    }

    return collected;
}

inline double peakOf (const std::vector<float>& samples)
{
    double peak = 0.0;

    for (const auto sample : samples)
        peak = std::max (peak, std::abs (static_cast<double> (sample)));

    return peak;
}

/// Render `blocks` blocks of `blockSize` and collect **both** channels — `{left, right}` — for the cases
/// that measure the difference between them (ticket 41's unison spread). `midi` is called per block.
inline std::pair<std::vector<float>, std::vector<float>> renderStereo (
    XerumAudioProcessor& processor, const int blocks, const int blockSize,
    const std::function<void (juce::MidiBuffer&, int)>& midi = {})
{
    juce::AudioBuffer<float> buffer (2, blockSize);
    std::pair<std::vector<float>, std::vector<float>> collected;

    for (int block = 0; block < blocks; ++block)
    {
        buffer.clear();
        juce::MidiBuffer events;

        if (midi)
            midi (events, block);

        processor.processBlock (buffer, events);

        for (int i = 0; i < blockSize; ++i)
        {
            collected.first.push_back (buffer.getSample (0, i));
            collected.second.push_back (buffer.getSample (1, i));
        }
    }

    return collected;
}

/// The largest absolute difference between two equal-length channel vectors.
inline double channelDifference (const std::vector<float>& left, const std::vector<float>& right)
{
    double difference = 0.0;

    for (std::size_t i = 0; i < left.size() && i < right.size(); ++i)
        difference = std::max (difference, std::abs (static_cast<double> (left[i] - right[i])));

    return difference;
}

inline double rmsOf (const std::vector<float>& samples)
{
    if (samples.empty())
        return 0.0;

    double sum = 0.0;

    for (const auto sample : samples)
        sum += static_cast<double> (sample) * static_cast<double> (sample);

    return std::sqrt (sum / static_cast<double> (samples.size()));
}

/// A note on at the start of the render and off in the middle, which is what every audio test wants.
inline std::function<void (juce::MidiBuffer&, int)> noteThenRelease (const int note, const int releaseBlock)
{
    return [note, releaseBlock] (juce::MidiBuffer& events, const int block)
    {
        if (block == 0)
            events.addEvent (juce::MidiMessage::noteOn (1, note, static_cast<juce::uint8> (100)), 0);

        if (block == releaseBlock)
            events.addEvent (juce::MidiMessage::noteOff (1, note), 0);
    };
}

inline juce::String corpusRoot()
{
    return "/Library/Audio/Presets/Xfer Records/Serum 2 Presets";
}

} // namespace xerum::plugin::tests
