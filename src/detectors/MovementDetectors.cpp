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

void Evaluate(DetectionBatch& out, DetectorConfig const& config, DetectionType type,
    std::uint64_t begin, std::uint64_t end, bool reliable = true, bool exempt = false)
{
    if (config.enabled[Index(type)])
        out.evaluations[Index(type)] = {exempt ? EvaluationStatus::Exempt : EvaluationStatus::Normal,
            std::min(begin, end), end, reliable && !exempt};
}

void ExemptAll(DetectionBatch& out, DetectorConfig const& config, std::uint64_t begin, std::uint64_t end)
{
    for (std::size_t i = 0; i < DetectionCount; ++i)
        if (static_cast<DetectionType>(i) != DetectionType::Warden)
            Evaluate(out, config, static_cast<DetectionType>(i), begin, end, false, true);
}

void Add(DetectionBatch& out, DetectorConfig const& config, DetectionType type,
    float confidence, bool actionable, double observed, double allowed, char const* reason)
{
    if (config.enabled[Index(type)])
    {
        out.Add({type, confidence, actionable, static_cast<float>(observed), static_cast<float>(allowed), reason});
        if (out.evaluations[Index(type)].status == EvaluationStatus::Normal)
            out.evaluations[Index(type)].status = EvaluationStatus::Anomalous;
    }
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
    s.previousSwimmingAllowed = c.swimmingAllowed;
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
    // Keep quality for the entire window, including transitions that ended before
    // its closing packet. A larger authorized allowance still detects gross
    // excess, but does not establish sustained unauthorised movement.
    float baseSpeed = std::max(c.allowedSpeed, s.previousSpeed);
    bool transition = c.speedTransition || std::abs(c.allowedSpeed - s.previousSpeed) > 0.001f ||
        c.flightAllowed != s.previousFlightAllowed ||
        c.swimmingAllowed != s.previousSwimmingAllowed ||
        (s.previousTimeMs < s.speedGraceUntilMs &&
            std::max(s.speedGraceMaxSpeed, s.speedGracePeakSpeed) > baseSpeed + 0.001f);
    if (transition)
    {
        s.speedWindowReliable = false;
        Evaluate(out, cfg, DetectionType::Speed, s.previousTimeMs, c.nowMs, false, true);
    }
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
    bool earlyBurst = c.nowMs - s.speedWindowStartMs < cfg.speedWindowMs;
    if (earlyBurst)
    {
        // A burst can stop before another packet closes the time window. Give it
        // the full window's budget, then report immediately if even that is exceeded.
        double fullWindowBudget = s.speedWindowPeak *
            (static_cast<double>(cfg.speedWindowMs) / 1000.0) * cfg.speedTolerance +
            s.speedWindowPeak * (static_cast<double>(s.speedWindowLatencyMs) / 1000.0) + 1.0;
        if (s.speedDistance <= std::max(allowed, fullWindowBudget))
            return;
        // The conservative full-window gate decides whether to emit early.
        // Severity still compares distance with its actual elapsed-time budget,
        // otherwise early closure would disguise sustained excess as a near miss.
    }
    Evaluate(out, cfg, DetectionType::Speed, s.speedWindowStartMs, c.nowMs,
        s.speedWindowReliable, !s.speedWindowReliable);
    if (s.speedDistance > allowed && s.speedDistance > 5.0)
        Add(out, cfg, DetectionType::Speed, 0.9f, true, s.speedDistance, allowed,
            earlyBurst ? "distance exceeds full-window burst guard; reported budget covers elapsed time" :
                "distance exceeds server-time movement budget");
    s.speedWindowStartMs = c.nowMs;
    s.speedDistance = 0.0;
    s.speedAllowance = 0.0;
    s.speedWindowPeak = c.allowedSpeed;
    s.speedWindowLatencyMs = 0;
    s.speedWindowReliable = true;
}

void CheckTeleport(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg,
    std::uint64_t elapsed, DetectionBatch& out)
{
    bool transition = c.speedTransition || std::abs(c.allowedSpeed - s.previousSpeed) > 0.001f ||
        (s.previousTimeMs < s.speedGraceUntilMs &&
            std::max(s.speedGraceMaxSpeed, s.speedGracePeakSpeed) >
                std::max(c.allowedSpeed, s.previousSpeed) + 0.001f);
    Evaluate(out, cfg, DetectionType::Teleport, s.previousTimeMs, c.nowMs, !transition, transition);
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
    Evaluate(out, cfg, DetectionType::Fly, s.previousTimeMs, c.nowMs, true,
        c.flightAllowed || s.previousTimeMs < s.flightGraceUntilMs);
    if ((c.flying || c.canFly) && !c.flightAllowed && c.nowMs >= s.flightGraceUntilMs)
        Add(out, cfg, DetectionType::Fly, 0.9f, true, 1.0, 0.0,
            "flight flags without server flight authority");
}

void CheckWaterwalk(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    Evaluate(out, cfg, DetectionType::Waterwalk, s.previousTimeMs, c.nowMs, true,
        c.waterWalkAllowed || s.previousTimeMs < s.waterWalkGraceUntilMs);
    if (c.waterWalking && !c.waterWalkAllowed && c.nowMs >= s.waterWalkGraceUntilMs)
        Add(out, cfg, DetectionType::Waterwalk, 0.85f, true, 1.0, 0.0,
            "water-walk flag without server authority");
    // A server water-state lookup can precede relocation; land-swim is not reliable evidence.
}

void CheckNoFall(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    Evaluate(out, cfg, DetectionType::NoFall, s.previousTimeMs, c.nowMs, false,
        c.flightAllowed || c.slowFallAllowed || s.previousTimeMs < s.slowFallGraceUntilMs);
    if (c.disableGravity && !c.flightAllowed && !c.slowFallAllowed && c.nowMs >= s.slowFallGraceUntilMs)
        Add(out, cfg, DetectionType::NoFall, 0.55f, false, 1.0, 0.0,
            "gravity-disabled flag; does not establish fall-damage suppression");
}

void CheckControl(DetectionContext const& c, MovementState& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (!c.rooted || c.nowMs < s.rootGraceUntilMs)
    {
        Evaluate(out, cfg, DetectionType::IgnoreControl, s.previousTimeMs, c.nowMs, false, true);
        s.rootAnchorValid = false;
        s.rootDistanceBase = 0.0;
        return;
    }
    if (!s.rootAnchorValid)
    {
        Evaluate(out, cfg, DetectionType::IgnoreControl, s.previousTimeMs, c.nowMs, false, true);
        s.rootAnchor = c.position;
        s.rootAnchorValid = true;
        return;
    }
    Evaluate(out, cfg, DetectionType::IgnoreControl, s.previousTimeMs, c.nowMs, true,
        s.previousTimeMs < s.rootGraceUntilMs);
    double distance = s.rootDistanceBase + DistanceXY(c.position, s.rootAnchor);
    // The anchor catches sub-yard steps. Once a step has been reported, merely
    // standing at that offset is a normal interval, not new movement evidence.
    if (distance > 1.5 && DistanceXY(c.position, s.previousPosition) > 0.0)
        Add(out, cfg, DetectionType::IgnoreControl, 0.85f, true, distance, 1.5,
            "horizontal displacement from stable server-root anchor");
}

void CheckClimb(DetectionContext const& c, MovementState const& s, DetectorConfig const& cfg, DetectionBatch& out)
{
    if (c.serverTransport || c.flightAllowed || c.swimmingAllowed || c.slowFallAllowed || c.falling || c.jump || s.jumping)
    {
        Evaluate(out, cfg, DetectionType::Climb, s.previousTimeMs, c.nowMs, false, true);
        return;
    }
    Evaluate(out, cfg, DetectionType::Climb, s.previousTimeMs, c.nowMs, false);
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
    {
        Evaluate(out, cfg, DetectionType::Jump, s.previousTimeMs, c.nowMs, false, true);
        Evaluate(out, cfg, DetectionType::Gravity, s.previousTimeMs, c.nowMs, false, true);
        return;
    }

    if (s.jumping && c.nowMs - s.jumpStartedMs > 10000)
        s.jumping = false; // A missing landing opcode must not leave a permanent state.
    if (c.jump)
    {
        if (c.groundSampled && groundKnown)
            Evaluate(out, cfg, DetectionType::Jump, c.nowMs, c.nowMs, false);
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
            if (c.groundSampled)
                Evaluate(out, cfg, DetectionType::Gravity, c.nowMs, c.nowMs, false);
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
    Evaluate(out, cfg, DetectionType::FakePing, s.clockWindowStartMs, c.nowMs, false);
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
    if (c.geometrySampled)
        Evaluate(out, cfg, DetectionType::NoClip, c.nowMs, c.nowMs, false);
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
        ExemptAll(out, cfg, c.nowMs, c.nowMs);
        s.Reset();
        s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    if (!s.initialized || c.mapId != s.mapId)
    {
        ExemptAll(out, cfg, c.nowMs, c.nowMs);
        if (s.initialized)
            s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    bool disruptedFrame = c.transportFrameChanged || c.serverTransport != s.previousServerTransport;
    if (disruptedFrame)
    {
        s.speedWindowReliable = false;
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
        ExemptAll(out, cfg, s.previousTimeMs, c.nowMs);
        s.GrantGrace(c.nowMs, cfg.transitionGraceMs);
        Rebase(c, s);
        return out;
    }
    if (elapsed > cfg.maxSampleGapMs)
    {
        ExemptAll(out, cfg, s.previousTimeMs, c.nowMs);
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
        ExemptAll(out, cfg, s.previousTimeMs, c.nowMs);
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
        disruptedFrame = true;
        s.speedWindowReliable = false;
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
    {
        Evaluate(out, cfg, DetectionType::NoClip, c.nowMs, c.nowMs, false);
        Add(out, cfg, DetectionType::NoClip, 0.65f, false, 1.0, 0.0,
            "transport claim inconsistent with validated server vessel frame");
    }
    if (disruptedFrame)
    {
        // Client-influenced transport changes make spatial intervals uncertain,
        // but do not change server flight/water-walk authority or client clocks.
        for (auto type : {DetectionType::Speed, DetectionType::Teleport, DetectionType::Climb,
            DetectionType::Jump, DetectionType::Gravity, DetectionType::IgnoreControl, DetectionType::NoClip})
            Evaluate(out, cfg, type, s.previousTimeMs, c.nowMs, false, true);
    }
    StorePrevious(c, s);
    return out;
}
}
