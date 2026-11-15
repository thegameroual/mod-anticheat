/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#pragma once

#include "DetectionContext.h"

#include <algorithm>
#include <cstdint>

namespace anticheat
{
struct MovementState
{
    bool initialized = false;
    Vec3 previousPosition;
    std::uint64_t previousTimeMs = 0;
    std::uint32_t previousClientTimeMs = 0;
    std::uint32_t mapId = 0;
    float previousSpeed = 7.0f;
    bool previousExempt = false;
    bool previousServerTransport = false;
    bool previousFlightAllowed = false;
    bool previousWaterWalkAllowed = false;
    bool previousSlowFallAllowed = false;
    bool previousRooted = false;
    bool previousForcedMovement = false;

    std::uint64_t graceUntilMs = 0;
    std::uint64_t speedGraceUntilMs = 0;
    float speedGraceMaxSpeed = 0.0f;
    float speedGracePeakSpeed = 0.0f;
    std::uint64_t flightGraceUntilMs = 0;
    std::uint64_t waterWalkGraceUntilMs = 0;
    std::uint64_t slowFallGraceUntilMs = 0;
    std::uint64_t rootGraceUntilMs = 0;

    std::uint64_t speedWindowStartMs = 0;
    double speedDistance = 0.0;
    double speedAllowance = 0.0;
    float speedWindowPeak = 7.0f;
    std::uint32_t speedWindowLatencyMs = 0;

    bool jumping = false;
    std::uint64_t jumpStartedMs = 0;
    bool falling = false;
    std::uint64_t fallPeakMs = 0;
    float fallPeakZ = 0.0f;
    Vec3 rootAnchor;
    bool rootAnchorValid = false;
    double rootDistanceBase = 0.0;

    std::uint64_t clockWindowStartMs = 0;
    std::uint32_t clockWindowClientMs = 0;
    bool groundSampleSeen = false;
    std::uint64_t lastGroundSampleMs = 0;
    bool geometrySampleSeen = false;
    std::uint64_t lastGeometrySampleMs = 0;

    void Reset() { *this = MovementState{}; }
    void GrantGrace(std::uint64_t nowMs, std::uint32_t durationMs)
    {
        graceUntilMs = std::max(graceUntilMs, nowMs + durationMs);
    }
};
}
