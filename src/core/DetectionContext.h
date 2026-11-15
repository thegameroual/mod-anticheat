/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#pragma once

#include <cmath>
#include <cstdint>

namespace anticheat
{
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    bool IsFinite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
};

// A value snapshot: no Player, Map, Session or other engine-owned pointer escapes the hook.
// The adapter must derive server authority from the server, never from the incoming flags.
struct DetectionContext
{
    Vec3 position;
    Vec3 serverPosition;
    std::uint64_t nowMs = 0;
    std::uint32_t clientTimeMs = 0;
    std::uint32_t mapId = 0;
    std::uint32_t latencyMs = 0;
    float allowedSpeed = 7.0f;

    bool exempt = false; // GM, taxi or server-owned vehicle.
    bool serverTransport = false; // Adapter validated and converted to transport-local coordinates.
    bool transportFrameChanged = false; // Includes switching between two vessels.
    bool transportInvalid = false;
    bool teleport = false;
    bool forcedMovement = false;
    bool flightAllowed = false;
    bool waterWalkAllowed = false;
    bool slowFallAllowed = false;
    bool rooted = false;
    bool swimmingAllowed = false;

    bool falling = false;
    bool flying = false;
    bool canFly = false;
    bool swimming = false;
    bool waterWalking = false;
    bool disableGravity = false;
    bool onTransport = false; // Untrusted: deliberately never a global exemption.
    bool jump = false;
    bool landed = false;

    bool groundSampled = false;
    bool groundKnown = false; // Adapter rejects invalid terrain height sentinels.
    float groundZ = 0.0f;
    bool geometrySampled = false;
    bool lineBlocked = false;
};
}
