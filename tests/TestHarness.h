#pragma once
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

namespace anticheat::test
{
inline unsigned checks = 0;
inline void Check(bool passed, char const* expression, char const* file, int line)
{
    ++checks;
    if (!passed)
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + expression);
}
}
#define CHECK(...) ::anticheat::test::Check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, epsilon) CHECK(std::abs((a) - (b)) <= (epsilon))
