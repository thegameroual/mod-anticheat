/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#include "MovementDetectors.h"

#include <algorithm>
#include <cmath>

namespace anticheat
{
namespace
{
double DistanceXY(Vec3 const& a, Vec3 const& b)
{
    return std::hypot(static_cast<double>(a.x) - b.x, static_cast<double>(a.y) - b.y);
}

double Distance3D(Vec3 const& a, Vec3 const& b)
{
    return std::hypot(DistanceXY(a, b), static_cast<double>(a.z) - b.z);
}

void Add(DetectionBatch& out, DetectorConfig const& config, DetectionType type,
    float confidence, bool actionable, double observed, double allowed, char const* reason)
{
    if (config.enabled[Index(type)])
        out.Add({type, confidence, actionable, static_cast<float>(observed), static_cast<float>(allowed), reason});
}

void StorePrevious(DetectionContext const& c, MovementState& s)
{
    s.previousPosition = c.position;
    s.previousTimeMs = c.nowMs;
    s.previousClientTimeMs = c.clientTimeMs;
    s.previousSpeed = c.allowedSpeed;
    s.mapId = c.mapId;
    s.previousExempt = c.exempt;
    s.previousServerTransport = c.serverTransport;
    s.previousFlightAllowed = c.flightAllowed;
    s.previousWaterWalkAllowed = c.waterWalkAllowed;
    s.previousSlowFallAllowed = c.slowFallAllowed;
    s.previousRooted = c.rooted;
    s.previousForcedMovement = c.forcedMovement;
    s.initialized = true;
}

void Rebase(DetectionContext const& c, MovementState& s)
{
    auto grace = s.graceUntilMs;
    auto speedGrace = s.speedGraceUntilMs;
    auto speedGraceMax = s.speedGraceMaxSpeed;
    auto speedGracePeak = s.speedGracePeakSpeed;
    auto flightGrace = s.flightGraceUntilMs;
    auto waterGrace = s.waterWalkGraceUntilMs;
    auto fallGrace = s.slowFallGraceUntilMs;
    auto rootGrace = s.rootGraceUntilMs;
    s.Reset();
    s.graceUntilMs = grace;
    s.speedGraceUntilMs = speedGrace;
    s.speedGraceMaxSpeed = speedGraceMax;
    s.speedGracePeakSpeed = speedGracePeak;
    s.flightGraceUntilMs = flightGrace;
    s.waterWalkGraceUntilMs = waterGrace;
    s.slowFallGraceUntilMs = fallGrace;
    s.rootGraceUntilMs = rootGrace;
    s.speedWindowStartMs = c.nowMs;
    s.speedWindowPeak = c.allowedSpeed;
    s.clockWindowStartMs = c.nowMs;
    s.clockWindowClientMs = c.clientTimeMs;
    StorePrevious(c, s);
}

bool GroundIsKnown(DetectionContext const& c)
{
    return c.groundKnown && std::isfinite(c.groundZ) && c.groundZ > -100000.0f;
}

double IntervalSpeedAllowance(DetectionContext const& c, MovementState const& s, std::uint64_t elapsed)
{
    float baseSpeed = std::max(c.allowedSpeed, s.previousSpeed);
    float transitionSpeed = std::max(baseSpeed, s.speedGraceMaxSpeed);
    auto transitionElapsed = s.previousTimeMs < s.speedGraceUntilMs
        ? std::min(elapsed, s.speedGraceUntilMs - s.previousTimeMs) : 0;
    return baseSpeed * (static_cast<double>(elapsed) / 1000.0) +
        (transitionSpeed - baseSpeed) * (static_cast<double>(transitionElapsed) / 1000.0);
}

float IntervalPeakSpeed(DetectionContext const& c, MovementState const& s)
{
    float peak = std::max(c.allowedSpeed, s.previousSpeed);
    if (s.previousTimeMs < s.speedGraceUntilMs || c.nowMs < s.speedGraceUntilMs)
        peak = std::max({peak, s.speedGraceMaxSpeed, s.speedGracePeakSpeed});
    return peak;
}

void CheckSpeed(DetectionContext const& c, MovementState& s, DetectorConfig const& cfg,
    std::uint64_t elapsed, DetectionBatch& out)
{
    // Ground movement uses XY: jumping/falling cannot waive horizontal speed checks.
    // Flying/swimming uses 3D only when the server authorizes that movement mode.
    double distance = c.flightAllowed || c.swimmingAllowed
        ? Distance3D(c.position, s.previousPosition) : DistanceXY(c.position, s.previousPosition);
    s.speedDistance += distance;
    s.speedAllowance += IntervalSpeedAllowance(c, s, elapsed);
    s.speedWindowPeak = std::max(s.speedWindowPeak, IntervalPeakSpeed(c, s));
    s.speedWindowLatencyMs = std::max(s.speedWindowLatencyMs, std::min(c.latencyMs, cfg.latencyCapMs));
    // One latency allowance per observation window, never one per packet.
    double allowed = s.speedAllowance * cfg.speedTolerance +
        s.speedWindowPeak * (static_cast<double>(s.speedWindowLatencyMs) / 1000.0) + 1.0;
    if (c.nowMs - s.speedWindowStartMs < cfg.speedWindowMs)
    {
        // A burst can stop before another packet closes the time window. Give it
        // the full window's budget, then report immediately if even that is exceeded.
        double fullWindowBudget = s.speedWindowPeak *
            (static_cast<double>(cfg.speedWindowMs) / 1000.0) * cfg.speedTolerance +
            s.speedWindowPeak * (static_cast<double>(s.speedWindowLatencyMs) / 1000.0) + 1.0;
        if (s.speedDistance <= std::max(allowed, fullWindowBudget))
            return;
        allowed = std::max(allowed, fullWindowBudget);
    }
    if (s.speedDistance > allowed && s.speedDistance > 5.0)
        Add(out, cfg, DetectionType::Speed, 0.9f, true, s.speedDistance, allowed,
            "distance exceeds server-time movement budget");
    s.speedWindowStartMs = c.nowMs;
    s.speedDistance = 0.0;
    s.speedAllowance = 0.0;
    s.speedWindowPeak = c.allowedSpeed;
    s.speedWindowLatencyMs = 0;
}

void CheckTeleport(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg,
    std::uint64_t elapsed, DetectionBatch& out)
{
    double distance = Distance3D(c.position, c.serverPosition);
    double allowedTravel = (IntervalSpeedAllowance(c, s, elapsed) + IntervalPeakSpeed(c, s) *
        (static_cast<double>(std::min(c.latencyMs, cfg.latencyCapMs)) / 1000.0)) * cfg.speedTolerance;
    double allowed = std::max<double>(cfg.teleportDistance, allowedTravel + 5.0);
    // Falling vertical distance is not evidence of teleportation. Horizontal movement still is.
    if (c.falling && !c.flightAllowed)
        distance = DistanceXY(c.position, c.serverPosition);
    if (distance > allowed)
        Add(out, cfg, DetectionType::Teleport, 0.9f, true, distance, allowed,
            "position discontinuity from preceding server position");
}

void CheckFly(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if ((c.flying || c.canFly) && !c.flightAllowed && c.nowMs >= s.flightGraceUntilMs)
        Add(out, cfg, DetectionType::Fly, 0.9f, true, 1.0, 0.0,
            "flight flags without server flight authority");
}

void CheckWaterwalk(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (c.waterWalking && !c.waterWalkAllowed && c.nowMs >= s.waterWalkGraceUntilMs)
        Add(out, cfg, DetectionType::Waterwalk, 0.85f, true, 1.0, 0.0,
            "water-walk flag without server authority");
    // A server water-state lookup can precede relocation; land-swim is not reliable evidence.
}

void CheckNoFall(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (c.disableGravity && !c.flightAllowed && !c.slowFallAllowed && c.nowMs >= s.slowFallGraceUntilMs)
        Add(out, cfg, DetectionType::NoFall, 0.55f, false, 1.0, 0.0,
            "gravity-disabled flag; does not establish fall-damage suppression");
}

void CheckControl(DetectionContext const& c, MovementState& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (!c.rooted || c.nowMs < s.rootGraceUntilMs)
    {
        s.rootAnchorValid = false;
        s.rootDistanceBase = 0.0;
        return;
    }
    if (!s.rootAnchorValid)
    {
        s.rootAnchor = c.position;
        s.rootAnchorValid = true;
        return;
    }
    double distance = s.rootDistanceBase + DistanceXY(c.position, s.rootAnchor);
    if (distance > 1.5)
        Add(out, cfg, DetectionType::IgnoreControl, 0.85f, true, distance, 1.5,
            "horizontal displacement from stable server-root anchor");
}

void CheckClimb(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (c.serverTransport || c.flightAllowed || c.swimmingAllowed || c.slowFallAllowed || c.falling || c.jump || s.jumping)
        return;
    double dz = static_cast<double>(c.position.z) - s.previousPosition.z;
    if (dz < 2.0)
        return;
    double angle = std::atan2(dz, DistanceXY(c.position, s.previousPosition)) * 180.0 / 3.14159265358979323846;
    if (angle > cfg.climbAngleDegrees)
        Add(out, cfg, DetectionType::Climb, 0.5f, false, angle, cfg.climbAngleDegrees,
            "steep ascent; stairs and collision geometry require review");
}

void CheckJumpAndGravity(DetectionContext const& c, MovementState& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    bool groundKnown = GroundIsKnown(c);
    double height = static_cast<double>(c.position.z) - c.groundZ;
    bool grounded = groundKnown && std::abs(height) < 0.75 && !c.falling;
    if (c.serverTransport || c.flightAllowed || c.swimmingAllowed || c.slowFallAllowed || c.landed || grounded)
    {
        s.jumping = false;
        s.falling = false;
    }
    if (c.serverTransport || c.flightAllowed || c.swimmingAllowed || c.slowFallAllowed)
        return;

    if (s.jumping && c.nowMs - s.jumpStartedMs > 10000)
        s.jumping = false; // A missing landing opcode must not leave a permanent state.
    if (c.jump)
    {
        if (s.jumping && groundKnown && height > 3.0)
            Add(out, cfg, DetectionType::Jump, 0.65f, false, height, 3.0,
                "repeated jump while above sampled terrain");
        if (!s.jumping)
            s.jumpStartedMs = c.nowMs;
        s.jumping = true;
    }

    if (c.falling && !s.falling)
    {
        s.falling = true;
        s.fallPeakZ = c.position.z;
        s.fallPeakMs = c.nowMs;
    }
    else if (c.falling && s.falling)
    {
        if (c.position.z > s.fallPeakZ)
        {
            s.fallPeakZ = c.position.z;
            s.fallPeakMs = c.nowMs;
        }
        double duration = static_cast<double>(c.nowMs - s.fallPeakMs) / 1000.0;
        if (duration >= 1.5 && groundKnown && height > 3.0)
        {
            // Conservative diagnostic only: receipt time is not the client's physics clock.
            constexpr double gravity = 19.291105;
            constexpr double terminalVelocity = 60.148003;
            constexpr double terminalTime = terminalVelocity / gravity;
            double accelerated = std::min(duration, terminalTime);
            double expected = 0.5 * gravity * accelerated * accelerated +
                terminalVelocity * std::max(0.0, duration - terminalTime);
            double actual = static_cast<double>(s.fallPeakZ) - c.position.z;
            if (expected > 10.0 && actual >= 0.0 && actual < expected * 0.25)
                Add(out, cfg, DetectionType::Gravity, 0.45f, false, actual, expected * 0.25,
                    "sustained airborne fall below conservative gravity envelope");
        }
    }
    else if (!c.falling)
        s.falling = false;
}

void CheckClock(DetectionContext const& c, MovementState& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    auto interval = c.nowMs - s.clockWindowStartMs;
    if (interval < cfg.speedWindowMs)
        return;
    // Modular unsigned subtraction handles ordinary uint32 rollover. Backwards clocks are
    // diagnostics, never extra movement allowance and never automatic sanction evidence.
    std::uint32_t clientInterval = c.clientTimeMs - s.clockWindowClientMs;
    double drift = std::abs(static_cast<double>(clientInterval) - static_cast<double>(interval));
    if (drift > cfg.maxClockDriftMs)
        Add(out, cfg, DetectionType::FakePing, 0.35f, false, drift, cfg.maxClockDriftMs,
            "client/server clock-window disagreement; network or clock anomaly");
    s.clockWindowStartMs = c.nowMs;
    s.clockWindowClientMs = c.clientTimeMs;
}

void CheckNoClip(DetectionContext const& c, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (c.geometrySampled && c.lineBlocked)
        Add(out, cfg, DetectionType::NoClip, 0.4f, false, 1.0, 0.0,
            "blocked line of sight; not a player-collision proof");
}
}

bool ShouldSampleGround(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg)
{
    bool enabled = cfg.enabled[Index(DetectionType::Jump)] || cfg.enabled[Index(DetectionType::Gravity)];
    return enabled && s.initialized && c.position.IsFinite() && !c.exempt && !c.serverTransport && !c.teleport && !c.forcedMovement &&
        !c.flightAllowed && !c.swimmingAllowed && !c.slowFallAllowed && c.nowMs >= s.graceUntilMs &&
        (c.jump || c.falling || s.jumping) &&
        (!s.groundSampleSeen || (c.nowMs >= s.lastGroundSampleMs && c.nowMs - s.lastGroundSampleMs >= cfg.geometryIntervalMs));
}

bool ShouldSampleGeometry(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg)
{
    return cfg.enabled[Index(DetectionType::NoClip)] && s.initialized && c.mapId == s.mapId &&
        !c.transportFrameChanged && c.serverTransport == s.previousServerTransport &&
        Distance3D(c.serverPosition, s.previousPosition) <= 0.5 &&
        c.position.IsFinite() && !c.exempt && !c.serverTransport && !c.teleport && !c.forcedMovement &&
        !c.flightAllowed && !c.swimmingAllowed && !c.falling && !c.jump && !s.jumping &&
        c.nowMs >= s.graceUntilMs && DistanceXY(c.position, s.previousPosition) > 1.5 &&
        (!s.geometrySampleSeen || (c.nowMs >= s.lastGeometrySampleMs && c.nowMs - s.lastGeometrySampleMs >= cfg.geometryIntervalMs));
}

DetectionBatch EvaluateMovement(DetectionContext const& c, MovementState& s, DetectorConfig const& cfg)
{
    DetectionBatch out;
    if (!c.position.IsFinite() || !c.serverPosition.IsFinite() || !std::isfinite(c.allowedSpeed) || c.allowedSpeed < 0.0f)
    {
        Add(out, cfg, DetectionType::Teleport, 1.0f, false, 0.0, 0.0,
            "invalid non-finite movement snapshot rejected");
        return out; // Do not contaminate tracking with invalid positions.
    }
    if (s.initialized && c.nowMs < s.previousTimeMs)
    {
        s.Reset();
        s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    if (!s.initialized || c.mapId != s.mapId)
    {
        if (s.initialized)
            s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    if (c.transportFrameChanged || c.serverTransport != s.previousServerTransport)
    {
        // A coordinate change does not erase travelled distance, clock observations
        // or control evidence. Preserve the established rooted displacement magnitude
        // because the old and new frames can have different orientations.
        if (s.rootAnchorValid)
        {
            s.rootDistanceBase += DistanceXY(s.previousPosition, s.rootAnchor);
            s.rootAnchor = c.serverPosition;
        }
        s.previousPosition = c.serverPosition;
        s.previousServerTransport = c.serverTransport;
        s.jumping = false;
        s.falling = false;
    }
    auto elapsed = c.nowMs - s.previousTimeMs;
    if (c.exempt || c.teleport || c.forcedMovement || s.previousExempt || s.previousForcedMovement)
    {
        s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    if (elapsed > cfg.maxSampleGapMs)
    {
        double distance = DistanceXY(c.position, c.serverPosition);
        double allowed = std::max<double>(cfg.teleportDistance,
            std::max(c.allowedSpeed, s.previousSpeed) * static_cast<double>(elapsed) / 1000.0 * cfg.speedTolerance + 5.0);
        if (distance > allowed)
            Add(out, cfg, DetectionType::Teleport, 0.6f, false, distance, allowed,
                "position discontinuity after long sample gap; interrupted observation");
        s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    if (s.previousFlightAllowed && !c.flightAllowed)
        s.flightGraceUntilMs = c.nowMs + cfg.transitionGraceMs;
    if (s.previousWaterWalkAllowed && !c.waterWalkAllowed)
        s.waterWalkGraceUntilMs = c.nowMs + cfg.transitionGraceMs;
    if (s.previousSlowFallAllowed && !c.slowFallAllowed)
        s.slowFallGraceUntilMs = c.nowMs + cfg.transitionGraceMs;
    if (!s.previousRooted && c.rooted)
        s.rootGraceUntilMs = c.nowMs + cfg.transitionGraceMs + std::min(c.latencyMs, cfg.latencyCapMs);

    if (c.nowMs < s.graceUntilMs)
    {
        Rebase(c, s);
        return out;
    }
    if (!c.onTransport && !c.serverTransport && Distance3D(c.serverPosition, s.previousPosition) > 0.5)
    {
        // A completed server spline can move the actor between two client packets.
        // Anchor at that authoritative position, but still inspect this packet: a
        // server correction cannot authorize an arbitrary client displacement.
        // Transport offsets are client-influenced even after core relocation;
        // their local/world discrepancy cannot establish a trusted correction.
        DetectionContext baseline = c;
        baseline.position = c.serverPosition;
        Rebase(baseline, s);
        elapsed = 0;
    }
    if (c.groundSampled)
    {
        s.groundSampleSeen = true;
        s.lastGroundSampleMs = c.nowMs;
    }
    if (c.geometrySampled)
    {
        s.geometrySampleSeen = true;
        s.lastGeometrySampleMs = c.nowMs;
    }
    CheckSpeed(c, s, cfg, elapsed, out);
    CheckTeleport(c, s, cfg, elapsed, out);
    CheckFly(c, s, cfg, out);
    CheckClimb(c, s, cfg, out);
    CheckJumpAndGravity(c, s, cfg, out);
    CheckWaterwalk(c, s, cfg, out);
    CheckNoFall(c, s, cfg, out);
    CheckControl(c, s, cfg, out);
    CheckClock(c, s, cfg, out);
    CheckNoClip(c, cfg, out);
    if (c.transportInvalid)
        Add(out, cfg, DetectionType::NoClip, 0.65f, false, 1.0, 0.0,
            "transport claim inconsistent with validated server vessel frame");
    StorePrevious(c, s);
    return out;
}
}
