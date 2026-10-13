#include "legacy.h"

#include <cstring>

namespace xerum::model
{

namespace
{

constexpr int kMaxDepth = 64;

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

    bool byte (std::uint8_t& out)
    {
        if (atEnd())
            return fail ("unexpected end of payload");

        out = static_cast<std::uint8_t> (input[position++]);
        return true;
    }

    bool peek (std::uint8_t& out) const
    {
        if (atEnd())
            return false;

        out = static_cast<std::uint8_t> (input[position]);
        return true;
    }

    /// A key, or the body of an `S` string: `i`, one length byte, then that many bytes.
    bool readName (std::string& out)
    {
        std::uint8_t tag = 0;

        if (! byte (tag))
            return false;

        if (tag != 'i')
            return fail ("expected a length-prefixed name");

        std::uint8_t length = 0;

        if (! byte (length))
            return false;

        if (position + length > input.size())
            return fail ("name runs past the end of the payload");

        out.assign (reinterpret_cast<const char*> (input.data() + position), length);
        position += length;
        return true;
    }

    bool readValue (Value& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail ("nesting deeper than the reader allows");

        std::uint8_t tag = 0;

        if (! byte (tag))
            return false;

        switch (tag)
        {
            case '{':
            {
                out.type = Value::Type::Map;

                while (true)
                {
                    std::uint8_t next = 0;

                    if (! peek (next))
                        return fail ("unterminated map");

                    if (next == '}')
                    {
                        ++position;
                        return true;
                    }

                    std::string key;

                    if (! readName (key))
                        return false;

                    out.entries.emplace_back (std::move (key), Value {});

                    if (! readValue (out.entries.back().second, depth + 1))
                        return false;
                }
            }

            case '[':
            {
                out.type = Value::Type::Array;

                while (true)
                {
                    std::uint8_t next = 0;

                    if (! peek (next))
                        return fail ("unterminated array");

                    if (next == ']')
                    {
                        ++position;
                        return true;
                    }

                    out.items.emplace_back();

                    if (! readValue (out.items.back(), depth + 1))
                        return false;
                }
            }

            case 'i':   // a value-position integer in one byte
            {
                std::uint8_t value = 0;

                if (! byte (value))
                    return false;

                out.type = Value::Type::Int;
                out.integer = value;
                return true;
            }

            case 'I':   // a two-byte big-endian integer
            {
                std::uint8_t high = 0, low = 0;

                if (! byte (high) || ! byte (low))
                    return false;

                out.type = Value::Type::Int;
                out.integer = (static_cast<std::int64_t> (high) << 8) | low;
                return true;
            }

            case 'S':   // a string value, spelled like a key
            {
                out.type = Value::Type::String;
                return readName (out.text);
            }

            case 'D':   // big-endian double. Little-endian here would produce plausible nonsense.
            {
                if (position + 8 > input.size())
                    return fail ("double runs past the end of the payload");

                std::uint64_t raw = 0;

                for (int i = 0; i < 8; ++i)
                    raw = (raw << 8) | static_cast<std::uint8_t> (input[position + static_cast<std::size_t> (i)]);

                position += 8;

                double decoded = 0.0;
                std::memcpy (&decoded, &raw, sizeof (decoded));

                out.type = Value::Type::Float;
                out.real = decoded;
                return true;
            }

            case 'T':   // a bare true
            {
                out.type = Value::Type::Bool;
                out.boolean = true;
                return true;
            }

            case 'F':   // a bare false
            {
                // Not `F` followed by a byte. `Init.XferPath` writes `isOpen`, then `F`, then the next
                // key immediately — so a reader that consumes a byte after `F` swallows that key and
                // then fails on a file that is perfectly well-formed. The tracker's first pass recorded
                // `F <byte>`; eleven of the twelve legacy files do not contain `F` at all, which is why
                // the mistake survived, and this is the twelfth.
                out.type = Value::Type::Bool;
                out.boolean = false;
                return true;
            }

            default:
                return fail ("unknown typecode " + std::to_string (tag)
                             + " at offset " + std::to_string (position - 1));
        }
    }
};

} // namespace

ValueResult decodeLegacy (std::span<const std::byte> input)
{
    ValueResult result;
    Reader reader { input, 0, {} };

    if (! reader.readValue (result.value, 0))
    {
        result.error = reader.error.empty() ? "malformed legacy stream" : reader.error;
        result.value = Value {};
        return result;
    }

    result.bytesRead = reader.position;
    result.ok = true;
    return result;
}

} // namespace xerum::model
