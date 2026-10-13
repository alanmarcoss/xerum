#include "cbor.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>

namespace xerum::model
{

std::string_view toString (CborValue::Type type)
{
    switch (type)
    {
        case CborValue::Type::Null:   return "null";
        case CborValue::Type::Bool:   return "bool";
        case CborValue::Type::Int:    return "int";
        case CborValue::Type::Float:  return "float";
        case CborValue::Type::String: return "string";
        case CborValue::Type::Bytes:  return "bytes";
        case CborValue::Type::Array:  return "array";
        case CborValue::Type::Map:    return "map";
    }
    return "unknown";
}

const CborValue* CborValue::find (std::string_view key) const
{
    for (const auto& [k, v] : entries)
        if (k == key)
            return &v;

    return nullptr;
}

bool CborValue::numberAsDouble (double& out) const
{
    switch (type)
    {
        case Type::Int:   out = static_cast<double> (integer); return true;
        case Type::Float: out = real;                          return true;
        default:          return false;
    }
}

namespace
{

/// The reader. A struct rather than a set of free functions so the position and the error travel
/// together, and so no call site can forget to check either.
struct Reader
{
    std::span<const std::byte> input;
    std::size_t position { 0 };
    std::string error;

    bool fail (std::string message)
    {
        if (error.empty())
            error = std::move (message);

        return false;
    }

    bool atEnd() const { return position >= input.size(); }

    bool readByte (std::uint8_t& out)
    {
        if (atEnd())
            return fail ("unexpected end of payload");

        out = static_cast<std::uint8_t> (input[position++]);
        return true;
    }

    bool readBytes (std::size_t count, std::span<const std::byte>& out)
    {
        if (count > input.size() - position)
            return fail ("declared length runs past the end of the payload");

        out = input.subspan (position, count);
        position += count;
        return true;
    }

    bool readUnsigned (std::size_t count, std::uint64_t& out)
    {
        out = 0;

        for (std::size_t i = 0; i < count; ++i)
        {
            std::uint8_t byte = 0;

            if (! readByte (byte))
                return false;

            out = (out << 8) | byte;
        }

        return true;
    }

    /// The argument that follows a major-type byte: either literal, or in the following 1/2/4/8 bytes.
    /// `indefinite` distinguishes the "length follows until a break" encoding, which this format uses.
    bool readArgument (std::uint8_t additional, std::optional<std::uint64_t>& value, bool& indefinite)
    {
        indefinite = false;

        if (additional < 24)
        {
            value = additional;
            return true;
        }

        if (additional == 31)
        {
            indefinite = true;
            value = std::nullopt;
            return true;
        }

        static constexpr std::size_t widths[] = { 1, 2, 4, 8 };

        if (additional > 27)
            return fail ("reserved additional-information value");

        std::uint64_t raw = 0;

        if (! readUnsigned (widths[additional - 24], raw))
            return false;

        value = raw;
        return true;
    }

    bool readValue (CborValue& out, int depth);
    bool readInto (CborValue& out, int depth);
};

/// Guard against a malformed payload claiming a nesting depth that would exhaust the stack. A preset is
/// a shallow document; anything deeper is corrupt or hostile.
constexpr int kMaxDepth = 64;

bool Reader::readInto (CborValue& out, int depth)
{
    if (depth > kMaxDepth)
        return fail ("nesting deeper than the reader allows");

    std::uint8_t initial = 0;

    if (! readByte (initial))
        return false;

    const auto major = static_cast<std::uint8_t> (initial >> 5);
    const auto additional = static_cast<std::uint8_t> (initial & 0x1F);

    std::optional<std::uint64_t> argument;
    bool indefinite = false;

    // Major type 7 is the exception: its "argument" bits describe the value itself (or how many bytes
    // the float uses), and for the float forms those bytes ARE the payload. Reading them as an argument
    // would consume the float before its own case could read it — which is exactly the bug this line
    // fixes.
    if (major != 7)
    {
        if (! readArgument (additional, argument, indefinite))
            return false;
    }

    switch (major)
    {
        case 0:  // unsigned integer
        {
            if (! argument.has_value())
                return fail ("indefinite length on an integer");

            out.type = CborValue::Type::Int;
            out.integer = static_cast<std::int64_t> (*argument);
            return true;
        }

        case 1:  // negative integer
        {
            if (! argument.has_value())
                return fail ("indefinite length on an integer");

            out.type = CborValue::Type::Int;
            out.integer = -1 - static_cast<std::int64_t> (*argument);
            return true;
        }

        case 2:  // byte string
        case 3:  // text string
        {
            out.type = major == 2 ? CborValue::Type::Bytes : CborValue::Type::String;

            if (indefinite)
            {
                // Chunked strings: concatenate until the break. The format uses this.
                while (true)
                {
                    if (atEnd())
                        return fail ("unterminated indefinite-length string");

                    if (static_cast<std::uint8_t> (input[position]) == 0xFF)
                    {
                        ++position;
                        return true;
                    }

                    CborValue chunk;

                    if (! readInto (chunk, depth + 1))
                        return false;

                    if (chunk.type != out.type)
                        return fail ("chunked string mixed its chunk types");

                    out.text += chunk.text;
                }
            }

            std::span<const std::byte> bytes;

            if (! readBytes (static_cast<std::size_t> (*argument), bytes))
                return false;

            out.text.assign (reinterpret_cast<const char*> (bytes.data()), bytes.size());
            return true;
        }

        case 4:  // array
        {
            out.type = CborValue::Type::Array;

            if (indefinite)
            {
                while (true)
                {
                    if (atEnd())
                        return fail ("unterminated indefinite-length array");

                    if (static_cast<std::uint8_t> (input[position]) == 0xFF)
                    {
                        ++position;
                        return true;
                    }

                    out.items.emplace_back();

                    if (! readInto (out.items.back(), depth + 1))
                        return false;
                }
            }

            out.items.reserve (static_cast<std::size_t> (*argument));

            for (std::uint64_t i = 0; i < *argument; ++i)
            {
                out.items.emplace_back();

                if (! readInto (out.items.back(), depth + 1))
                    return false;
            }

            return true;
        }

        case 5:  // map
        {
            out.type = CborValue::Type::Map;

            auto readPair = [&]() -> bool
            {
                CborValue key;

                if (! readInto (key, depth + 1))
                    return false;

                // Every key in this format is a string. Anything else is accepted but flattened to its
                // textual form, because dropping it silently would lose information.
                std::string name;

                if (key.type == CborValue::Type::String)
                    name = std::move (key.text);
                else if (key.type == CborValue::Type::Int)
                    name = std::to_string (key.integer);
                else
                    name = "<" + std::string (toString (key.type)) + ">";

                out.entries.emplace_back (std::move (name), CborValue {});

                return readInto (out.entries.back().second, depth + 1);
            };

            if (indefinite)
            {
                while (true)
                {
                    if (atEnd())
                        return fail ("unterminated indefinite-length map");

                    if (static_cast<std::uint8_t> (input[position]) == 0xFF)
                    {
                        ++position;
                        return true;
                    }

                    if (! readPair())
                        return false;
                }
            }

            out.entries.reserve (static_cast<std::size_t> (*argument));

            for (std::uint64_t i = 0; i < *argument; ++i)
                if (! readPair())
                    return false;

            return true;
        }

        case 6:  // tag — recorded but not interpreted; tags carry meaning we do not need yet
        {
            CborValue inner;

            if (! readInto (inner, depth + 1))
                return false;

            out = std::move (inner);
            return true;
        }

        case 7:  // simple values and floats
        {
            switch (additional)
            {
                case 20: out.type = CborValue::Type::Bool; out.boolean = false; return true;
                case 21: out.type = CborValue::Type::Bool; out.boolean = true;  return true;
                case 22: out.type = CborValue::Type::Null;                      return true;

                case 25:  // half precision
                {
                    std::uint64_t raw = 0;

                    if (! readUnsigned (2, raw))
                        return false;

                    // Half floats appear in principle; decode them rather than refusing, so an unusual
                    // preset is not mistaken for a corrupt one.
                    const auto sign = static_cast<double> ((raw >> 15) & 1 ? -1 : 1);
                    const auto exponent = static_cast<int> ((raw >> 10) & 0x1F);
                    const auto mantissa = static_cast<double> (raw & 0x3FF);

                    out.type = CborValue::Type::Float;

                    if (exponent == 0)
                        out.real = sign * std::ldexp (mantissa, -24);
                    else if (exponent == 31)
                        out.real = mantissa == 0 ? std::numeric_limits<double>::infinity()
                                                 : std::numeric_limits<double>::quiet_NaN();
                    else
                        out.real = sign * std::ldexp (1.0 + mantissa / 1024.0, exponent - 15);

                    return true;
                }

                case 26:  // float32
                {
                    std::uint64_t raw = 0;

                    if (! readUnsigned (4, raw))
                        return false;

                    const auto bits = static_cast<std::uint32_t> (raw);
                    float f = 0.0f;
                    std::memcpy (&f, &bits, sizeof (f));
                    out.type = CborValue::Type::Float;
                    out.real = f;
                    return true;
                }

                case 27:  // float64 — the format uses this where a reader might expect float32
                {
                    std::uint64_t raw = 0;

                    if (! readUnsigned (8, raw))
                        return false;

                    double d = 0.0;
                    std::memcpy (&d, &raw, sizeof (d));
                    out.type = CborValue::Type::Float;
                    out.real = d;
                    return true;
                }

                case 31:
                    return fail ("unexpected break");

                default:
                    return fail ("unsupported simple value " + std::to_string (additional));
            }
        }

        default:
            break;
    }

    return fail ("unsupported major type " + std::to_string (major));
}

bool Reader::readValue (CborValue& out, int depth) { return readInto (out, depth); }

} // namespace

CborResult decodeCbor (std::span<const std::byte> input)
{
    CborResult result;
    Reader reader { input, 0, {} };

    if (! reader.readValue (result.value, 0))
    {
        result.error = reader.error.empty() ? "malformed CBOR" : reader.error;

        // A half-built document must not escape: a caller that ignored `ok` would otherwise read a map
        // that looks plausible and is missing everything after the point of failure.
        result.value = CborValue {};
        return result;
    }

    result.ok = true;
    result.bytesRead = reader.position;
    return result;
}

} // namespace xerum::model
