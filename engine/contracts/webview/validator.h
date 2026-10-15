#pragma once

#include "schema.h"
#include <boost/json.hpp>
#include <cmath>
#include <string_view>

namespace metasequoia::webview
{
namespace detail
{
inline bool Matches(const boost::json::value &value, const boost::json::object &rule)
{
    if (const auto *branches = rule.if_contains("anyOf"))
    {
        for (const auto &branch : branches->as_array())
            if (Matches(value, branch.as_object()))
                return true;
        return false;
    }
    if (const auto *type = rule.if_contains("type"))
    {
        const auto &name = type->as_string();
        if (name == "array")
        {
            if (!value.is_array())
                return false;
            for (const auto &item : value.as_array())
                if (!Matches(item, rule.at("items").as_object()))
                    return false;
        }
        else if (name == "object")
        {
            if (!value.is_object())
                return false;
            const auto &object = value.as_object();
            const auto &properties = rule.at("properties").as_object();
            for (const auto &key : rule.at("required").as_array())
                if (!object.contains(key.as_string()))
                    return false;
            for (const auto &item : object)
            {
                if (const auto *property = properties.if_contains(item.key()))
                {
                    if (!Matches(item.value(), property->as_object()))
                        return false;
                }
                else if (!rule.at("additionalProperties").as_bool())
                    return false;
            }
        }
        else if (name == "string" && !value.is_string())
            return false;
        else if (name == "boolean" && !value.is_bool())
            return false;
        else if (name == "null" && !value.is_null())
            return false;
        else if (name == "number" || name == "integer")
        {
            if (!value.is_number())
                return false;
            const double number = value.to_number<double>();
            if (!std::isfinite(number))
                return false;
            if (name == "integer" && (std::floor(number) != number || std::abs(number) > 9007199254740991.0))
                return false;
        }
    }
    if (const auto *values = rule.if_contains("enum"))
    {
        bool found = false;
        for (const auto &allowed : values->as_array())
            found = found || value == allowed;
        if (!found)
            return false;
    }
    if (const auto *minimum = rule.if_contains("minimum"))
        if (value.to_number<double>() < minimum->to_number<double>())
            return false;
    if (const auto *maximum = rule.if_contains("maximum"))
        if (value.to_number<double>() > maximum->to_number<double>())
            return false;
    return true;
}
} // namespace detail

// Missing version is the legacy v1 skin envelope; an explicit unknown version is never dispatched.
inline bool Validate(const boost::json::value &message, std::string_view direction, std::string_view surface = {})
{
    static const auto schema = boost::json::parse(SchemaJson);
    if (!message.is_object())
        return false;
    const auto &object = message.as_object();
    const auto *type = object.if_contains("type");
    if (!type || !type->is_string())
        return false;
    if (const auto *version = object.if_contains("protocolVersion"))
        if (!version->is_number() || version->to_number<double>() != Version)
            return false;
    const auto *rules = schema.as_object().if_contains(direction);
    if (!rules)
        return false;
    const auto *entry = rules->as_object().if_contains(type->as_string());
    if (!entry)
        return false;
    const auto &spec = entry->as_object();
    if (const auto *surfaces = spec.if_contains("surfaces"))
    {
        bool found = false;
        for (const auto &allowed : surfaces->as_array())
            found = found || allowed.as_string() == surface;
        if (!found)
            return false;
    }
    const auto *dataRule = spec.if_contains("data");
    const auto *data = object.if_contains("data");
    if (dataRule && (!data || !detail::Matches(*data, dataRule->as_object())))
        return false;
    if (!dataRule && data)
        return false;
    for (const auto &item : object)
    {
        if (item.key() == "type" || item.key() == "protocolVersion" || item.key() == "data")
            continue;
        const auto *properties = spec.if_contains("properties");
        const auto *property = properties ? properties->as_object().if_contains(item.key()) : nullptr;
        if (property)
        {
            if (!detail::Matches(item.value(), property->as_object()))
                return false;
        }
        else
        {
            const auto *additional = spec.if_contains("additionalProperties");
            if (!additional || !additional->as_bool())
                return false;
        }
    }
    if (const auto *required = spec.if_contains("required"))
        for (const auto &key : required->as_array())
            if (!object.contains(key.as_string()))
                return false;
    return true;
}
} // namespace metasequoia::webview
