// xerum::dsp — wavetables.
//
// A wavetable is a stack of single-cycle frames, not a recording. Reading one is where the reference's
// asset format meets the engine, and ticket 02's findings shape the parser more than the obvious reading
// did:
//
//   * The `clm ` chunk carries a **frame-interpolation mode** and a factory flag, as eight hex digits.
//   * The **frame count is computed** from the data length divided by frame size and sample width — it is
//     not fixed at 256, and observed tables range from 1 to 256 frames.
//   * **Fourteen tables are 16-bit PCM**, not float32. A loader that assumes float works on most of the
//     corpus and fails quietly on the rest.
//
// Pure: no filesystem access here, so the parser is testable against bytes. Loading a table *from* a
// content root is the library's job, not this one.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xerum::dsp
{

/// How a table's frames are blended when the oscillator sits between two of them.
enum class FrameInterpolation
{
    None,          ///< snap to the nearest frame
    Linear,        ///< crossfade the two neighbouring frames
    SpectralMorph, ///< morph in the frequency domain (modes 2, 3 and 4 on disk)
    Unknown
};

std::string_view toString (FrameInterpolation interpolation);

struct Wavetable
{
    std::vector<float> samples;    ///< frame-major: frame f sample i lives at f * frameSize + i
    std::size_t frameSize { 0 };
    std::size_t frameCount { 0 };
    FrameInterpolation interpolation { FrameInterpolation::Unknown };
    bool factory { false };
    bool sixteenBit { false };
    double sampleRate { 44100.0 };

    bool empty() const { return frameCount == 0 || frameSize == 0; }
    std::size_t totalSamples() const { return samples.size(); }

    /// One sample of one frame, without bounds checks — the oscillator calls this in its inner loop.
    float at (std::size_t frame, std::size_t index) const { return samples[frame * frameSize + index]; }

    /// One frame's worth of samples, interpolated across the *frame* axis only.
    ///
    /// `position` is in frames and may be fractional. `index` is the sample within the frame.
    float interpolated (double position, std::size_t index) const;
};

struct WavetableResult
{
    bool ok { false };
    std::string error;
    Wavetable table;
};

/// Parse a RIFF/WAVE wavetable. Never throws.
WavetableResult parseWavetable (std::span<const std::byte> bytes);

/// The frame size every factory table uses. Read from the `clm` chunk in practice; this is the fallback
/// and the value the format is built around.
inline constexpr std::size_t kDefaultFrameSize = 2048;

} // namespace xerum::dsp
