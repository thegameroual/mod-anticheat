/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence
 */
#pragma once
#include "DetectionResult.h"

namespace anticheat
{
struct DetectorConfig
{
    std::array<bool, DetectionCount> enabled = {true, true, true, true, true, true,
        true, true, true, true, true, true};
    float speedTolerance = 1.25f;
    float teleportDistance = 50.0f;
    float climbAngleDegrees = 80.0f;
    std::uint32_t maxClockDriftMs = 2000;
    std::uint32_t speedWindowMs = 3000;
    std::uint32_t maxSampleGapMs = 5000;
    std::uint32_t transitionGraceMs = 1500;
    std::uint32_t latencyCapMs = 500;
    std::uint32_t geometryIntervalMs = 500;
};
}
