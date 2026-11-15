/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <type_traits>

namespace anticheat
{
// ConfigMgr's typed getters replace malformed input with defaults. A reload
// needs to reject that candidate instead, so parse the original value strictly.
template<class T>
bool ParseConfigValue(std::string const& text, T& value)
{
    auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return false;
    auto last = text.find_last_not_of(" \t\r\n");
    auto token = text.substr(first, last - first + 1);
    if constexpr (std::is_same_v<T, bool>)
    {
        if (token == "1" || token == "true")
            value = true;
        else if (token == "0" || token == "false")
            value = false;
        else
            return false;
        return true;
    }
    else
    {
        if constexpr (std::is_unsigned_v<T>)
            if (token.front() == '-')
                return false;
        std::istringstream input(token);
        input.imbue(std::locale::classic());
        T candidate{};
        input >> candidate;
        if (input.fail())
            return false;
        input >> std::ws;
        if (!input.eof())
            return false;
        if constexpr (std::is_floating_point_v<T>)
            if (!std::isfinite(candidate))
                return false;
        value = candidate;
        return true;
    }
}
}
