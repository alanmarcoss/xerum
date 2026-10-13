#include "value.h"

namespace xerum::model
{

std::string_view toString (Value::Type type)
{
    switch (type)
    {
        case Value::Type::Null:   return "null";
        case Value::Type::Bool:   return "bool";
        case Value::Type::Int:    return "int";
        case Value::Type::Float:  return "float";
        case Value::Type::String: return "string";
        case Value::Type::Bytes:  return "bytes";
        case Value::Type::Array:  return "array";
        case Value::Type::Map:    return "map";
    }
    return "unknown";
}

const Value* Value::find (std::string_view key) const
{
    for (const auto& [k, v] : entries)
        if (k == key)
            return &v;

    return nullptr;
}

std::string Value::stringOr (std::string_view key, std::string fallback) const
{
    const auto* member = find (key);

    if (member != nullptr && member->isText())
        return member->text;

    return fallback;
}

bool Value::numberAsDouble (double& out) const
{
    switch (type)
    {
        case Type::Int:   out = static_cast<double> (integer); return true;
        case Type::Float: out = real;                          return true;
        default:          return false;
    }
}

bool operator== (const Value& a, const Value& b)
{
    if (a.type != b.type)
        return false;

    switch (a.type)
    {
        case Value::Type::Null:   return true;
        case Value::Type::Bool:   return a.boolean == b.boolean;
        case Value::Type::Int:    return a.integer == b.integer;
        case Value::Type::Float:  return a.real == b.real;
        case Value::Type::String:
        case Value::Type::Bytes:  return a.text == b.text;
        case Value::Type::Array:  return a.items == b.items;
        case Value::Type::Map:    return a.entries == b.entries;
    }

    return false;
}

bool operator!= (const Value& a, const Value& b) { return ! (a == b); }

} // namespace xerum::model
