/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace anticheat
{
enum class DetectionType : std::uint8_t
{
    Speed, Fly, Teleport, Climb, Jump, Gravity, Waterwalk, NoFall,
    IgnoreControl, FakePing, NoClip, Warden, Count
};

inline constexpr std::size_t DetectionCount = static_cast<std::size_t>(DetectionType::Count);
inline constexpr std::array<char const*, DetectionCount> DetectionNames = {
    "Speed", "Unauthorized flight", "Teleport", "Climb", "Jump", "Gravity",
    "Waterwalk", "Gravity flag", "Ignore control", "Clock anomaly", "Geometry", "Warden"
};

constexpr std::size_t Index(DetectionType type) { return static_cast<std::size_t>(type); }

struct DetectionResult
{
    DetectionType type = DetectionType::Speed;
    float confidence = 0.0f;
    bool actionable = false;
    float observed = 0.0f;
    float allowed = 0.0f;
    char const* reason = ""; // Static detector text; no allocation on the packet path.
};

struct DetectionBatch
{
    std::array<DetectionResult, DetectionCount> results{};
    std::size_t size = 0;

    void Add(DetectionResult const& result)
    {
        if (size < results.size())
            results[size++] = result;
    }
};
}
