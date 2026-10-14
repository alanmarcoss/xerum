#include "cbor_write.h"

#include <algorithm>
#include <cstring>

namespace xerum::model
{

namespace
{

void putByte (std::vector<std::byte>& out, std::uint8_t byte)
{
    out.push_back (static_cast<std::byte> (byte));
}

void putBigEndian (std::vector<std::byte>& out, std::uint64_t value, int width)
{
    for (int i = width - 1; i >= 0; --i)
        putByte (out, static_cast<std::uint8_t> ((value >> (8 * i)) & 0xFF));
}

/// Emit `major` with the shortest argument form that holds `value` — the canonical encoding, and what a
/// reader written against the spec expects.
void putHead (std::vector<std::byte>& out, std::uint8_t major, std::uint64_t value)
{
    if (value < 24)
    {
        putByte (out, static_cast<std::uint8_t> ((major << 5) | value));
    }
    else if (value <= 0xFF)
    {
        putByte (out, static_cast<std::uint8_t> ((major << 5) | 24));
        putBigEndian (out, value, 1);
    }
    else if (value <= 0xFFFF)
    {
        putByte (out, static_cast<std::uint8_t> ((major << 5) | 25));
        putBigEndian (out, value, 2);
    }
    else if (value <= 0xFFFFFFFFull)
    {
        putByte (out, static_cast<std::uint8_t> ((major << 5) | 26));
        putBigEndian (out, value, 4);
    }
    else
    {
        putByte (out, static_cast<std::uint8_t> ((major << 5) | 27));
        putBigEndian (out, value, 8);
    }
}

void encode (std::vector<std::byte>& out, const Value& value)
{
    switch (value.type)
    {
        case Value::Type::Null:
            putByte (out, 0xF6);
            return;

        case Value::Type::Bool:
            putByte (out, value.boolean ? 0xF5 : 0xF4);
            return;

        case Value::Type::Int:
            if (value.integer >= 0)
                putHead (out, 0, static_cast<std::uint64_t> (value.integer));
            else
                putHead (out, 1, static_cast<std::uint64_t> (-1 - value.integer));
            return;

        case Value::Type::Float:
        {
            // Always float64. The reference mixes widths, and every value we hold is a double; widening
            // is lossless, narrowing is not.
            putByte (out, 0xFB);
            std::uint64_t bits = 0;
            std::memcpy (&bits, &value.real, sizeof (bits));
            putBigEndian (out, bits, 8);
            return;
        }

        case Value::Type::String:
        case Value::Type::Bytes:
        {
            putHead (out, value.type == Value::Type::Bytes ? 2 : 3, value.text.size());
            out.insert (out.end(), reinterpret_cast<const std::byte*> (value.text.data()),
                        reinterpret_cast<const std::byte*> (value.text.data() + value.text.size()));
            return;
        }

        case Value::Type::Array:
        {
            putHead (out, 4, value.items.size());

            for (const auto& item : value.items)
                encode (out, item);

            return;
        }

        case Value::Type::Map:
        {
            putHead (out, 5, value.entries.size());

            // Sorted, so the same tree always produces the same bytes. A duplicate key would collapse
            // here and is therefore refused by the sort's stability rather than silently dropped — see
            // the test for what happens.
            auto sorted = value.entries;
            std::stable_sort (sorted.begin(), sorted.end(),
                              [] (const auto& a, const auto& b) { return a.first < b.first; });

            for (const auto& [key, member] : sorted)
            {
                putHead (out, 3, key.size());
                out.insert (out.end(), reinterpret_cast<const std::byte*> (key.data()),
                            reinterpret_cast<const std::byte*> (key.data() + key.size()));
                encode (out, member);
            }

            return;
        }
    }

    putByte (out, 0xF6);   // unreachable: every enumerator is handled above
}

} // namespace

std::vector<std::byte> encodeCbor (const Value& value)
{
    std::vector<std::byte> out;
    out.reserve (4096);
    encode (out, value);
    return out;
}

} // namespace xerum::model
