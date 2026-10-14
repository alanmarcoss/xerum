#include "wavetable.h"

#include <cstring>

namespace xerum::dsp
{

std::string_view toString (FrameInterpolation interpolation)
{
    switch (interpolation)
    {
        case FrameInterpolation::None:          return "none";
        case FrameInterpolation::Linear:        return "linear";
        case FrameInterpolation::SpectralMorph: return "spectral morph";
        case FrameInterpolation::Unknown:       return "unknown";
    }
    return "unknown";
}

float Wavetable::interpolated (double position, std::size_t index) const
{
    if (frameCount == 0)
        return 0.0f;

    if (frameCount == 1)
        return at (0, index);

    if (position <= 0.0)
        return at (0, index);

    if (position >= static_cast<double> (frameCount - 1))
        return at (frameCount - 1, index);

    const auto lower = static_cast<std::size_t> (position);
    const auto fraction = static_cast<float> (position - static_cast<double> (lower));

    if (interpolation == FrameInterpolation::None)
        return at (fraction < 0.5f ? lower : lower + 1, index);

    // Linear crossfade — and for a table whose file declared `clm ` mode 2, 3 or 4, that is not a
    // fallback: ticket 37 measured the reference rendering all four modes as exactly this crossfade, so
    // reading a `FrameInterpolation::SpectralMorph` table this way *is* the reference's behaviour. (The
    // spectral morph that does exist in the reference is a **preset** field,
    // `WTOsc{N}/interpolateAfterLoad`, and it applies to an *embedded* table only — see
    // `spectral_morph.h`, and ticket 41, which wires it at bind.)
    const auto a = at (lower, index);
    const auto b = at (lower + 1, index);

    return a + (b - a) * fraction;
}

namespace
{

std::uint32_t readU32 (std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy (&value, bytes.data() + offset, sizeof (value));
    return value;
}

std::uint16_t readU16 (std::span<const std::byte> bytes, std::size_t offset)
{
    std::uint16_t value = 0;
    std::memcpy (&value, bytes.data() + offset, sizeof (value));
    return value;
}

std::string chunkId (std::span<const std::byte> bytes, std::size_t offset)
{
    return std::string (reinterpret_cast<const char*> (bytes.data() + offset), 4);
}

/// The `clm ` payload: `<!>2048 BC000000 wavetable (…)`. The number is the frame size; the first two hex
/// digits of the field after it are the interpolation mode and the factory flag.
struct ClmInfo
{
    std::size_t frameSize { 0 };
    FrameInterpolation interpolation { FrameInterpolation::Unknown };
    bool factory { false };
    bool present { false };
};

ClmInfo parseClm (std::string_view text)
{
    ClmInfo info;
    info.present = true;

    const auto marker = text.find ("<!>");
    std::size_t at = marker == std::string_view::npos ? 0 : marker + 3;

    // Frame size, digits only.
    std::size_t numberStart = at;

    while (numberStart < text.size() && ! std::isdigit (static_cast<unsigned char> (text[numberStart])))
        ++numberStart;

    std::size_t numberEnd = numberStart;

    while (numberEnd < text.size() && std::isdigit (static_cast<unsigned char> (text[numberEnd])))
        ++numberEnd;

    if (numberEnd > numberStart)
        info.frameSize = static_cast<std::size_t> (std::stoul (std::string (text.substr (numberStart, numberEnd - numberStart))));

    // The eight hex digits after it.
    std::size_t hexStart = numberEnd;

    while (hexStart < text.size() && text[hexStart] == ' ')
        ++hexStart;

    auto hexValue = [] (char c) -> int
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    if (hexStart + 2 <= text.size())
    {
        const int mode = hexValue (text[hexStart]);
        const int factory = hexValue (text[hexStart + 1]);

        if (mode >= 0)
        {
            switch (mode)
            {
                case 0: info.interpolation = FrameInterpolation::None; break;
                case 1: info.interpolation = FrameInterpolation::Linear; break;
                case 2:
                case 3:
                case 4: info.interpolation = FrameInterpolation::SpectralMorph; break;
                default: info.interpolation = FrameInterpolation::Unknown; break;
            }
        }

        info.factory = factory == 1;
    }

    return info;
}

} // namespace

WavetableResult parseWavetable (std::span<const std::byte> bytes)
{
    WavetableResult result;

    if (bytes.size() < 12)
    {
        result.error = "file is too short to be a RIFF container";
        return result;
    }

    if (chunkId (bytes, 0) != "RIFF" || chunkId (bytes, 8) != "WAVE")
    {
        result.error = "not a RIFF/WAVE file";
        return result;
    }

    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    ClmInfo clm;
    std::span<const std::byte> data;

    // Walk the chunk list. Sizes are padded to even boundaries, which matters for the `clm ` chunk
    // because its length varies — 40 bytes in some files and 48 in others (ticket 02).
    std::size_t offset = 12;

    while (offset + 8 <= bytes.size())
    {
        const auto id = chunkId (bytes, offset);
        const auto size = static_cast<std::size_t> (readU32 (bytes, offset + 4));
        const auto body = offset + 8;

        if (body + size > bytes.size())
        {
            // The reference's 16-bit writer declares `data` using the **32-bit** byte count — twice the
            // bytes actually present — and inflates the RIFF size field to match. Fourteen factory tables
            // are written that way, and a strict reader refuses all fourteen with "runs past the end".
            //
            // Accept only that arithmetic, and only for 16-bit data. Anything else is a truncated file,
            // and a truncated file must still be refused: tolerating every overrun would hide real
            // corruption.
            const auto available = bytes.size() - body;

            if (id != "data" || bitsPerSample != 16 || size != available * 2)
            {
                result.error = "chunk \"" + id + "\" runs past the end of the file";
                return result;
            }
        }

        if (id == "fmt ")
        {
            if (size < 16)
            {
                result.error = "fmt chunk is too short";
                return result;
            }

            formatTag = readU16 (bytes, body);
            channels = readU16 (bytes, body + 2);
            sampleRate = readU32 (bytes, body + 4);
            bitsPerSample = readU16 (bytes, body + 14);
        }
        else if (id == "clm ")
        {
            clm = parseClm (std::string_view (reinterpret_cast<const char*> (bytes.data() + body), size));
        }
        else if (id == "data")
        {
            // Data is conventionally last; take what is present and stop.
            const auto available = bytes.size() - body;
            data = bytes.subspan (body, size < available ? size : available);
            break;
        }

        offset = body + size + (size % 2);   // chunks are word-aligned
    }

    if (formatTag == 0)
    {
        result.error = "no fmt chunk";
        return result;
    }

    if (data.empty())
    {
        result.error = "no data chunk";
        return result;
    }

    const bool sixteenBit = formatTag == 1 && bitsPerSample == 16;
    const bool thirtyTwoBitFloat = formatTag == 3 && bitsPerSample == 32;

    if (! sixteenBit && ! thirtyTwoBitFloat)
    {
        result.error = "unsupported sample format: tag " + std::to_string (formatTag) + ", "
                     + std::to_string (bitsPerSample) + " bits";
        return result;
    }

    const std::size_t bytesPerSample = sixteenBit ? 2 : 4;

    // The frame size comes from the file, and the frame *count is computed* — never assumed to be 256.
    const std::size_t frameSize = clm.frameSize != 0 ? clm.frameSize : kDefaultFrameSize;

    if (frameSize == 0 || data.size() < frameSize * bytesPerSample)
    {
        result.error = "data is smaller than a single frame";
        return result;
    }

    const std::size_t frameCount = data.size() / (frameSize * bytesPerSample);

    if (frameCount == 0)
    {
        result.error = "no complete frames in the data";
        return result;
    }

    auto& table = result.table;
    table.frameSize = frameSize;
    table.frameCount = frameCount;
    table.sixteenBit = sixteenBit;
    table.factory = clm.factory;
    table.interpolation = clm.present ? clm.interpolation : FrameInterpolation::None;
    table.sampleRate = sampleRate != 0 ? static_cast<double> (sampleRate) : 44100.0;
    table.samples.resize (frameCount * frameSize);

    if (thirtyTwoBitFloat)
    {
        std::memcpy (table.samples.data(), data.data(), table.samples.size() * sizeof (float));
    }
    else
    {
        // 16-bit PCM: scale to the same -1..1 range the float tables use, so nothing downstream has to
        // know which kind it got.
        for (std::size_t i = 0; i < table.samples.size(); ++i)
        {
            std::int16_t value = 0;
            std::memcpy (&value, data.data() + i * 2, sizeof (value));
            table.samples[i] = static_cast<float> (value) / 32768.0f;
        }
    }

    (void) channels;   // every factory table is mono; a stereo one would need a decision, not a guess

    result.ok = true;
    return result;
}

} // namespace xerum::dsp
