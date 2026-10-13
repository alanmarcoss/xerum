#include "json.h"

#include <cmath>
#include <cstdlib>

namespace xerum::model
{

const JsonValue* JsonValue::find (std::string_view key) const
{
    for (const auto& [k, v] : members)
        if (k == key)
            return &v;

    return nullptr;
}

std::string JsonValue::stringOr (std::string_view key, std::string fallback) const
{
    const auto* member = find (key);

    if (member != nullptr && member->isString())
        return member->text;

    return fallback;
}

namespace
{

constexpr int kMaxDepth = 64;

/// Append a code point as UTF-8. Surrogate pairs arrive here already combined.
void appendUtf8 (std::string& out, std::uint32_t codePoint)
{
    if (codePoint <= 0x7F)
    {
        out.push_back (static_cast<char> (codePoint));
    }
    else if (codePoint <= 0x7FF)
    {
        out.push_back (static_cast<char> (0xC0 | (codePoint >> 6)));
        out.push_back (static_cast<char> (0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0xFFFF)
    {
        out.push_back (static_cast<char> (0xE0 | (codePoint >> 12)));
        out.push_back (static_cast<char> (0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back (static_cast<char> (0x80 | (codePoint & 0x3F)));
    }
    else
    {
        out.push_back (static_cast<char> (0xF0 | (codePoint >> 18)));
        out.push_back (static_cast<char> (0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back (static_cast<char> (0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back (static_cast<char> (0x80 | (codePoint & 0x3F)));
    }
}

struct Reader
{
    std::string_view input;
    std::size_t position { 0 };
    std::string error;

    bool fail (std::string message)
    {
        if (error.empty())
            error = std::move (message);

        return false;
    }

    bool atEnd() const { return position >= input.size(); }
    char peek() const { return input[position]; }

    void skipWhitespace()
    {
        while (! atEnd())
        {
            const char c = input[position];

            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\0')
                ++position;
            else
                break;
        }
    }

    bool literal (std::string_view word)
    {
        if (input.substr (position, word.size()) != word)
            return fail ("expected " + std::string (word));

        position += word.size();
        return true;
    }

    bool readString (std::string& out)
    {
        if (atEnd() || peek() != '"')
            return fail ("expected a string");

        ++position;

        while (true)
        {
            if (atEnd())
                return fail ("unterminated string");

            const char c = input[position++];

            if (c == '"')
                return true;

            if (c != '\\')
            {
                out.push_back (c);
                continue;
            }

            if (atEnd())
                return fail ("unterminated escape");

            const char escape = input[position++];

            switch (escape)
            {
                case '"':  out.push_back ('"');  break;
                case '\\': out.push_back ('\\'); break;
                case '/':  out.push_back ('/');  break;
                case 'b':  out.push_back ('\b'); break;
                case 'f':  out.push_back ('\f'); break;
                case 'n':  out.push_back ('\n'); break;
                case 'r':  out.push_back ('\r'); break;
                case 't':  out.push_back ('\t'); break;

                case 'u':
                {
                    auto hex4 = [&] (std::uint32_t& value) -> bool
                    {
                        if (position + 4 > input.size())
                            return fail ("truncated \\u escape");

                        value = 0;

                        for (int i = 0; i < 4; ++i)
                        {
                            const char h = input[position++];
                            value <<= 4;

                            if (h >= '0' && h <= '9')      value |= static_cast<std::uint32_t> (h - '0');
                            else if (h >= 'a' && h <= 'f')  value |= static_cast<std::uint32_t> (h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F')  value |= static_cast<std::uint32_t> (h - 'A' + 10);
                            else                            return fail ("bad hex in \\u escape");
                        }

                        return true;
                    };

                    std::uint32_t code = 0;

                    if (! hex4 (code))
                        return false;

                    // Surrogate pairs: preset names contain emoji, so a reader that ignores them
                    // produces mojibake rather than an error.
                    if (code >= 0xD800 && code <= 0xDBFF
                        && position + 1 < input.size()
                        && input[position] == '\\' && input[position + 1] == 'u')
                    {
                        position += 2;

                        std::uint32_t low = 0;

                        if (! hex4 (low))
                            return false;

                        if (low >= 0xDC00 && low <= 0xDFFF)
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }

                    appendUtf8 (out, code);
                    break;
                }

                default:
                    return fail ("unknown escape");
            }
        }
    }

    bool readValue (JsonValue& out, int depth)
    {
        if (depth > kMaxDepth)
            return fail ("nesting deeper than the reader allows");

        skipWhitespace();

        if (atEnd())
            return fail ("unexpected end of input");

        const char c = peek();

        if (c == '{')
        {
            ++position;
            out.type = JsonValue::Type::Object;
            skipWhitespace();

            if (! atEnd() && peek() == '}')
            {
                ++position;
                return true;
            }

            while (true)
            {
                skipWhitespace();

                std::string key;

                if (! readString (key))
                    return false;

                skipWhitespace();

                if (atEnd() || peek() != ':')
                    return fail ("expected ':' in object");

                ++position;

                out.members.emplace_back (std::move (key), JsonValue {});

                if (! readValue (out.members.back().second, depth + 1))
                    return false;

                skipWhitespace();

                if (atEnd())
                    return fail ("unterminated object");

                if (peek() == ',')
                {
                    ++position;
                    continue;
                }

                if (peek() == '}')
                {
                    ++position;
                    return true;
                }

                return fail ("expected ',' or '}' in object");
            }
        }

        if (c == '[')
        {
            ++position;
            out.type = JsonValue::Type::Array;
            skipWhitespace();

            if (! atEnd() && peek() == ']')
            {
                ++position;
                return true;
            }

            while (true)
            {
                out.items.emplace_back();

                if (! readValue (out.items.back(), depth + 1))
                    return false;

                skipWhitespace();

                if (atEnd())
                    return fail ("unterminated array");

                if (peek() == ',')
                {
                    ++position;
                    continue;
                }

                if (peek() == ']')
                {
                    ++position;
                    return true;
                }

                return fail ("expected ',' or ']' in array");
            }
        }

        if (c == '"')
        {
            out.type = JsonValue::Type::String;
            return readString (out.text);
        }

        if (c == 't') { out.type = JsonValue::Type::Bool; out.boolean = true;  return literal ("true"); }
        if (c == 'f') { out.type = JsonValue::Type::Bool; out.boolean = false; return literal ("false"); }
        if (c == 'n') { out.type = JsonValue::Type::Null;                     return literal ("null"); }

        if (c == '-' || (c >= '0' && c <= '9'))
        {
            const auto start = position;

            if (peek() == '-')
                ++position;

            while (! atEnd() && ((peek() >= '0' && peek() <= '9') || peek() == '.'
                                 || peek() == 'e' || peek() == 'E' || peek() == '+' || peek() == '-'))
                ++position;

            const std::string token (input.substr (start, position - start));

            if (token.empty() || token == "-")
                return fail ("malformed number");

            out.type = JsonValue::Type::Number;
            out.number = std::strtod (token.c_str(), nullptr);
            return true;
        }

        return fail ("unexpected character");
    }
};

} // namespace

JsonResult decodeJson (std::string_view input)
{
    JsonResult result;
    Reader reader { input, 0, {} };

    if (! reader.readValue (result.value, 0))
    {
        result.error = reader.error.empty() ? "malformed JSON" : reader.error;
        result.value = JsonValue {};
        return result;
    }

    result.bytesRead = reader.position;
    result.ok = true;
    return result;
}

} // namespace xerum::model
