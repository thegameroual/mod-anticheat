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

enum class EvaluationStatus : std::uint8_t { NotEvaluated, Normal, Anomalous, Exempt };

struct DetectorEvaluation
{
    EvaluationStatus status = EvaluationStatus::NotEvaluated;
    std::uint64_t beginMs = 0;
    std::uint64_t endMs = 0;
    bool reliable = false;
};

struct DetectionBatch
{
    std::array<DetectionResult, DetectionCount> results{};
    // Absence of a result is not a normal evaluation. Only explicit intervals
    // can contribute to observed persistence; zero-length samples add no time.
    std::array<DetectorEvaluation, DetectionCount> evaluations{};
    std::size_t size = 0;

    void Add(DetectionResult const& result)
    {
        if (size < results.size())
            results[size++] = result;
    }
};
}
