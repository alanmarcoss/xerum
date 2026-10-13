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

} // namespace xerum::model
