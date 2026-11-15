/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#include "TestHarness.h"
#include "detectors/MovementDetectors.h"

#include <limits>

namespace
{
using namespace anticheat;

bool Has(DetectionBatch const& batch, DetectionType type)
{
    for (std::size_t i = 0; i < batch.size; ++i)
        if (batch.results[i].type == type)
            return true;
    return false;
}

bool HasActionable(DetectionBatch const& batch)
{
    for (std::size_t i = 0; i < batch.size; ++i)
        if (batch.results[i].actionable)
            return true;
    return false;
}

struct Movement
{
    DetectorConfig cfg;
    MovementState state;
    DetectionContext context;

    Movement()
    {
        context.nowMs = 1000;
        context.clientTimeMs = 1000;
        context.mapId = 1;
        CHECK(EvaluateMovement(context, state, cfg).size == 0);
    }

    DetectionBatch Step(std::uint32_t elapsed, float dx = 0.0f, float dz = 0.0f)
    {
        context.serverPosition = state.previousPosition;
        context.position.x += dx;
        context.position.z += dz;
        context.nowMs += elapsed;
        context.clientTimeMs += elapsed;
        return EvaluateMovement(context, state, cfg);
    }
};
}

void RunMovementTests()
{
    using namespace anticheat;
    {
        Movement m;
        m.context.position.x = 3.0f;
        m.context.nowMs += 1000;
        CHECK(ShouldSampleGeometry(m.context, m.state, m.cfg));
        m.context.transportFrameChanged = true;
        CHECK(!ShouldSampleGeometry(m.context, m.state, m.cfg));
        m.context.transportFrameChanged = false;
        m.context.serverPosition.x = 2.0f; // A server spline crossed the old segment.
        CHECK(!ShouldSampleGeometry(m.context, m.state, m.cfg));
    }
    {
        Movement slope;
        CHECK(!Has(slope.Step(1000, 3.0f, 1.0f), DetectionType::Climb));
        Movement wall;
        auto result = wall.Step(1000, 0.25f, 5.0f);
        CHECK(Has(result, DetectionType::Climb));
        CHECK(!HasActionable(result));
        Movement vertical;
        CHECK(Has(vertical.Step(1000, 0.0f, 5.0f), DetectionType::Climb));
        Movement flight;
        flight.context.flightAllowed = true;
        CHECK(!Has(flight.Step(1000, 0.25f, 5.0f), DetectionType::Climb));
    }
    {
        Movement m;
        for (unsigned i = 0; i < 120; ++i)
            CHECK(!HasActionable(m.Step(100, 0.7f)));
    }
    {
        Movement m;
        bool detected = false;
        for (unsigned i = 0; i < 60; ++i)
            detected = Has(m.Step(100, 2.0f), DetectionType::Speed) || detected;
        CHECK(detected);
    }
    {
        Movement m;
        // The old >3x-speed/10-yard exception must never grant spell authority.
        m.Step(100, 20.0f);
        CHECK(Has(m.Step(2900, 40.0f), DetectionType::Speed));
    }
    {
        Movement m;
        // Small or identical client timestamp intervals do not bypass speed accumulation.
        bool detected = false;
        for (unsigned i = 0; i < 100; ++i)
        {
            m.context.clientTimeMs = 1;
            detected = Has(m.Step(0, 0.8f), DetectionType::Speed) || detected;
        }
        CHECK(detected); // No later movement packet is needed to close an oversized burst.
    }
    {
        Movement m;
        m.context.falling = true;
        m.context.onTransport = true;
        m.context.flying = true;
        m.context.canFly = true;
        bool detected = false;
        for (unsigned i = 0; i < 30; ++i)
            detected = Has(m.Step(100, 2.0f), DetectionType::Speed) || detected;
        CHECK(detected); // Client flags alone never waive the horizontal budget.
    }
    {
        Movement m;
        CHECK(Has(m.Step(0, 100.0f), DetectionType::Teleport));
    }
    {
        Movement m;
        m.context.teleport = true; // Server Blink/teleport authorization.
        CHECK(m.Step(100, 1000.0f).size == 0);
        m.context.teleport = false;
        CHECK(m.Step(500, 1.0f).size == 0);
        m.Step(1500);
        CHECK(Has(m.Step(10, 100.0f), DetectionType::Teleport));
    }
    {
        Movement m;
        m.context.forcedMovement = true; // Charge/knockback server spline.
        CHECK(m.Step(100, 80.0f).size == 0);
        m.context.forcedMovement = false;
        CHECK(m.Step(100, 10.0f).size == 0);
        CHECK(m.Step(500, 1.0f).size == 0);
        m.Step(1600);
        CHECK(Has(m.Step(100, 80.0f), DetectionType::Teleport));
    }
    {
        Movement m;
        m.context.mapId = 2;
        CHECK(m.Step(100, 1000.0f).size == 0);
        CHECK_NEAR(m.state.speedDistance, 0.0, 0.001);
        CHECK(!m.state.jumping);
        CHECK(!m.state.falling);
        m.Step(1600);
        CHECK(!HasActionable(m.Step(100, 0.7f)));
    }
    {
        Movement m;
        m.context.exempt = true; // Server transport, vehicle and taxi share this path.
        CHECK(m.Step(100, 1000.0f).size == 0);
        m.context.exempt = false;
        CHECK(m.Step(100, 100.0f).size == 0);
        m.Step(1600);
        CHECK(!HasActionable(m.Step(100, 0.7f)));
        CHECK(Has(m.Step(100, 100.0f), DetectionType::Teleport));
    }
    {
        Movement m;
        m.context.flightAllowed = true;
        m.context.flying = true;
        m.context.canFly = true;
        m.context.allowedSpeed = 42.0f;
        for (unsigned i = 0; i < 60; ++i)
            CHECK(!HasActionable(m.Step(100, 4.0f, 1.0f)));
        m.context.flightAllowed = false;
        CHECK(!Has(m.Step(100), DetectionType::Fly));
        CHECK(!Has(m.Step(1000), DetectionType::Fly));
        CHECK(Has(m.Step(500), DetectionType::Fly));
    }
    {
        Movement m;
        m.context.flying = true;
        m.state.GrantGrace(m.context.nowMs, 1500); // Bounded server ACK transition.
        CHECK(m.Step(1000).size == 0);
        CHECK(Has(m.Step(500), DetectionType::Fly));
    }
    {
        Movement m;
        m.context.swimmingAllowed = true;
        m.context.swimming = true;
        m.context.allowedSpeed = 4.7f;
        for (unsigned i = 0; i < 40; ++i)
            CHECK(!HasActionable(m.Step(100, 0.3f, 0.2f)));
        m.context.waterWalking = true;
        m.context.waterWalkAllowed = true;
        CHECK(!Has(m.Step(100), DetectionType::Waterwalk));
        m.context.waterWalkAllowed = false;
        CHECK(!Has(m.Step(100), DetectionType::Waterwalk));
        CHECK(Has(m.Step(1500), DetectionType::Waterwalk));
    }
    {
        Movement m;
        m.context.swimming = true; // Wet-state transition alone is not a cheating proof.
        CHECK(!Has(m.Step(100), DetectionType::Waterwalk));
    }
    {
        Movement m;
        m.context.rooted = true; // Adapter combines server root and stun states.
        CHECK(!Has(m.Step(100, 0.5f), DetectionType::IgnoreControl));
        CHECK(!Has(m.Step(1500, 0.5f), DetectionType::IgnoreControl));
        for (unsigned i = 0; i < 3; ++i)
            CHECK(!Has(m.Step(100, 0.5f), DetectionType::IgnoreControl));
        CHECK(Has(m.Step(100, 0.5f), DetectionType::IgnoreControl));
        m.context.rooted = false;
        CHECK(!Has(m.Step(100, 0.5f), DetectionType::IgnoreControl));
    }
    {
        Movement m;
        m.context.jump = true;
        m.context.falling = true;
        m.context.groundSampled = true;
        m.context.groundKnown = true;
        CHECK(!Has(m.Step(100, 0.5f, 3.0f), DetectionType::Jump));
        m.context.jump = false;
        CHECK(!Has(m.Step(100, 0.5f, 1.0f), DetectionType::Jump));
        m.context.jump = true;
        auto result = m.Step(100, 0.5f, 1.0f);
        CHECK(Has(result, DetectionType::Jump));
        CHECK(!HasActionable(result));
        m.context.jump = false;
        m.context.falling = false;
        m.context.landed = true;
        m.Step(500, 0.5f, -5.0f);
        CHECK(!m.state.jumping);
        CHECK(!m.state.falling);
        m.context.landed = false;
        m.context.jump = true;
        CHECK(!Has(m.Step(100, 0.5f, 2.0f), DetectionType::Jump));
    }
    {
        Movement m;
        m.cfg.enabled[Index(DetectionType::Gravity)] = false;
        m.context.jump = true;
        m.context.falling = true;
        m.Step(100, 0.0f, 5.0f);
        m.context.jump = false;
        m.context.falling = false;
        m.context.landed = true;
        m.Step(1000, 0.0f, -5.0f);
        CHECK(!m.state.jumping); // State transitions are independent of enabled detectors.
    }
    {
        Movement m;
        m.context.jump = true;
        m.Step(100);
        m.context.jump = false;
        for (unsigned i = 0; i < 11; ++i)
            m.Step(1000);
        CHECK(!m.state.jumping); // Lost land packet does not stick forever.
    }
    {
        Movement m;
        m.context.position.z = 100.0f;
        m.context.falling = true;
        m.context.groundSampled = true;
        m.context.groundKnown = true;
        m.Step(100);
        CHECK(!Has(m.Step(1000, 0.0f, -9.65f), DetectionType::Gravity));
        CHECK(!Has(m.Step(1000, 0.0f, -28.93f), DetectionType::Gravity));
    }
    {
        Movement m;
        m.context.position.z = 100.0f;
        m.context.falling = true;
        m.context.groundSampled = true;
        m.context.groundKnown = true;
        m.Step(100);
        auto result = m.Step(2000, 0.0f, -1.0f);
        CHECK(Has(result, DetectionType::Gravity));
        CHECK(!HasActionable(result));
        m.context.slowFallAllowed = true;
        CHECK(!Has(m.Step(2000, 0.0f, -1.0f), DetectionType::Gravity));
        CHECK(!m.state.falling);
    }
    {
        Movement m;
        m.context.jump = true;
        m.context.falling = true;
        m.context.groundSampled = true;
        m.context.groundKnown = false;
        m.context.groundZ = -200000.0f;
        m.Step(100, 0.0f, 5.0f);
        CHECK(!Has(m.Step(2000, 0.0f, 1.0f), DetectionType::Jump));
        CHECK(!Has(m.Step(2000), DetectionType::Gravity));
        m.context.groundKnown = true; // Defense in depth against accidentally forwarded sentinel.
        CHECK(!Has(m.Step(100), DetectionType::Jump));
    }
    {
        Movement m;
        m.context.disableGravity = true;
        auto result = m.Step(100);
        CHECK(Has(result, DetectionType::NoFall));
        CHECK(!HasActionable(result));
        m.context.slowFallAllowed = true;
        CHECK(!Has(m.Step(100), DetectionType::NoFall));
        m.context.slowFallAllowed = false;
        CHECK(!Has(m.Step(100), DetectionType::NoFall));
        CHECK(Has(m.Step(1500), DetectionType::NoFall));
    }
    {
        Movement m;
        m.context.latencyMs = 2000;
        for (unsigned window = 0; window < 4; ++window)
        {
            CHECK(!HasActionable(m.Step(1000, 7.0f)));
            for (unsigned packet = 0; packet < 10; ++packet)
                CHECK(!HasActionable(m.Step(100, 0.7f)));
            CHECK(!HasActionable(m.Step(1000, 7.0f)));
        }
        m.context.latencyMs = 1;
        CHECK(!HasActionable(m.Step(100, 0.7f)));
        m.context.latencyMs = 4000;
        CHECK(!HasActionable(m.Step(100, 0.7f)));
    }
    {
        Movement m;
        // The clock rollover must behave like any other modular timestamp interval.
        m.context.clientTimeMs = std::numeric_limits<std::uint32_t>::max() - 15;
        m.state.Reset();
        EvaluateMovement(m.context, m.state, m.cfg);
        for (unsigned i = 0; i < 30; ++i)
            CHECK(!Has(m.Step(100, 0.7f), DetectionType::FakePing));
    }
    {
        Movement m;
        m.context.clientTimeMs = 0; // Backwards jump is diagnostic; no movement allowance.
        auto result = m.Step(3000, 50.0f);
        CHECK(Has(result, DetectionType::Speed));
        m.context.clientTimeMs -= 50000;
        result = m.Step(3000);
        CHECK(Has(result, DetectionType::FakePing));
        CHECK(!HasActionable(result));
    }
    {
        Movement m;
        m.context.position.x = 3.0f;
        CHECK(ShouldSampleGeometry(m.context, m.state, m.cfg));
        m.context.geometrySampled = true;
        m.context.lineBlocked = true;
        auto result = m.Step(100);
        CHECK(Has(result, DetectionType::NoClip));
        CHECK(!HasActionable(result));
        m.context.serverPosition = m.state.previousPosition;
        m.context.position.x += 3.0f;
        m.context.nowMs += 100;
        CHECK(!ShouldSampleGeometry(m.context, m.state, m.cfg));
        m.context.nowMs += 500;
        CHECK(ShouldSampleGeometry(m.context, m.state, m.cfg));
    }
    {
        Movement m;
        m.context.jump = true;
        CHECK(ShouldSampleGround(m.context, m.state, m.cfg));
        m.context.groundSampled = true;
        m.context.groundKnown = false;
        m.Step(100);
        CHECK(!ShouldSampleGround(m.context, m.state, m.cfg));
        m.context.nowMs += 500;
        CHECK(ShouldSampleGround(m.context, m.state, m.cfg));
    }
    {
        Movement m;
        m.context.position.x = std::numeric_limits<float>::quiet_NaN();
        auto result = EvaluateMovement(m.context, m.state, m.cfg);
        CHECK(result.size == 1);
        CHECK(!HasActionable(result));
        CHECK(m.state.previousPosition.IsFinite());
        m.context.position = m.state.previousPosition;
        CHECK(!HasActionable(m.Step(100, 0.7f)));
    }
    {
        Movement m;
        m.state.GrantGrace(m.context.nowMs, 5000);
        m.state.jumping = true;
        m.state.falling = true;
        m.state.Reset(); // Session destruction/reconnection must not retain exemptions.
        CHECK(!m.state.initialized);
        CHECK(m.state.graceUntilMs == 0);
        CHECK(!m.state.jumping && !m.state.falling);
        CHECK(m.Step(100).size == 0);
        m.context.flying = true;
        CHECK(Has(m.Step(100), DetectionType::Fly));
    }
    {
        Movement m;
        m.cfg.enabled.fill(false);
        m.context.flying = true;
        m.context.disableGravity = true;
        m.context.waterWalking = true;
        CHECK(m.Step(100, 1000.0f).size == 0);
    }
    {
        Movement m;
        auto result = m.Step(6000, 500.0f);
        CHECK(Has(result, DetectionType::Teleport));
        CHECK(!HasActionable(result));
        CHECK_NEAR(m.state.speedDistance, 0.0, 0.001);
    }
    {
        Movement m;
        // Completed Charge moved the server actor without intermediate movement packets.
        m.context.serverPosition.x = 40.0f;
        m.context.position.x = 40.7f;
        m.context.nowMs += 100;
        m.context.clientTimeMs += 100;
        CHECK(!HasActionable(EvaluateMovement(m.context, m.state, m.cfg)));
        CHECK_NEAR(m.state.speedDistance, 0.7, 0.001);
        for (unsigned i = 0; i < 30; ++i)
            CHECK(!HasActionable(m.Step(100, 0.7f)));
        m.context.serverPosition.x += 40.0f;
        m.context.position.x += 140.0f;
        CHECK(Has(EvaluateMovement(m.context, m.state, m.cfg), DetectionType::Teleport));
    }
    {
        Movement m;
        // Validated vessel motion is removed by transport-local coordinates.
        m.context.serverTransport = true;
        CHECK(m.Step(100).size == 0);
        m.Step(1600);
        for (unsigned i = 0; i < 30; ++i)
            CHECK(!HasActionable(m.Step(100, 0.7f)));
        CHECK(!ShouldSampleGround(m.context, m.state, m.cfg));
        m.context.position.x += 5.0f;
        CHECK(!ShouldSampleGeometry(m.context, m.state, m.cfg));
        bool detected = false;
        for (unsigned i = 0; i < 30; ++i)
            detected = Has(m.Step(100, 3.0f), DetectionType::Speed) || detected;
        CHECK(detected); // A transport cannot grant unlimited local movement.
        m.context.serverTransport = false;
        m.context.serverPosition = {1000.0f, 0.0f, 0.0f};
        m.context.position = {1000.7f, 0.0f, 0.0f};
        m.context.nowMs += 100;
        CHECK(!HasActionable(EvaluateMovement(m.context, m.state, m.cfg)));
        m.context.serverTransport = true;
        m.context.serverPosition = {1.0f, 0.0f, 0.0f};
        m.context.position = {101.0f, 0.0f, 0.0f};
        CHECK(Has(EvaluateMovement(m.context, m.state, m.cfg), DetectionType::Teleport));
    }
    {
        Movement m;
        m.context.transportInvalid = true;
        m.context.onTransport = true;
        auto result = m.Step(100, 100.0f);
        CHECK(Has(result, DetectionType::NoClip));
        CHECK(Has(result, DetectionType::Teleport));
    }
    {
        Movement m;
        bool speedDetected = false;
        bool clockDetected = false;
        for (unsigned i = 0; i < 30; ++i)
        {
            // A valid transport claim alternates with the world frame every packet.
            // Each packet is below the instantaneous teleport threshold.
            m.context.serverTransport = !m.context.serverTransport;
            m.context.serverPosition = {m.context.serverTransport ? 0.0f : 1000.0f, 0.0f, 0.0f};
            m.context.position = {m.context.serverPosition.x + 20.0f, 0.0f, 0.0f};
            m.context.nowMs += 100;
            auto result = EvaluateMovement(m.context, m.state, m.cfg);
            speedDetected = Has(result, DetectionType::Speed) || speedDetected;
            clockDetected = Has(result, DetectionType::FakePing) || clockDetected;
        }
        CHECK(speedDetected);
        CHECK(clockDetected); // Coordinate changes cannot reset the clock observation window.
    }
    {
        Movement m;
        m.context.serverTransport = true;
        m.Step(100);
        bool detected = false;
        for (unsigned i = 0; i < 30; ++i)
        {
            m.context.transportFrameChanged = true; // Switching between distinct valid vessels.
            m.context.serverPosition = {static_cast<float>(i), 0.0f, 0.0f};
            m.context.position = {m.context.serverPosition.x + 20.0f, 0.0f, 0.0f};
            m.context.nowMs += 100;
            m.context.clientTimeMs += 100;
            detected = Has(EvaluateMovement(m.context, m.state, m.cfg), DetectionType::Speed) || detected;
        }
        CHECK(detected);
    }
    {
        Movement m;
        m.context.rooted = true;
        m.Step(100);
        m.Step(1500);
        bool detected = false;
        for (unsigned i = 0; i < 5; ++i)
        {
            m.context.serverTransport = !m.context.serverTransport;
            m.context.serverPosition = {m.context.serverTransport ? 0.0f : 1000.0f, 0.0f, 0.0f};
            m.context.position = {m.context.serverPosition.x + 0.5f, 0.0f, 0.0f};
            m.context.nowMs += 100;
            m.context.clientTimeMs += 100;
            detected = Has(EvaluateMovement(m.context, m.state, m.cfg), DetectionType::IgnoreControl) || detected;
        }
        CHECK(detected); // Changing reference frames does not grant a fresh root allowance.
        CHECK(m.state.rootDistanceBase >= 1.5);
        m.context.rooted = false;
        m.context.transportFrameChanged = false;
        m.Step(100);
        CHECK_NEAR(m.state.rootDistanceBase, 0.0, 0.001);
    }
    {
        Movement m;
        m.context.flying = true;
        m.context.waterWalking = true;
        bool speedDetected = false;
        for (unsigned i = 0; i < 30; ++i)
        {
            // Repeated real waterwalk/featherfall transitions only waive their own flags.
            m.state.waterWalkGraceUntilMs = m.context.nowMs + 1500;
            m.state.slowFallGraceUntilMs = m.context.nowMs + 1500;
            auto result = m.Step(100, 2.0f);
            CHECK(!Has(result, DetectionType::Waterwalk));
            CHECK(Has(result, DetectionType::Fly));
            speedDetected = Has(result, DetectionType::Speed) || speedDetected;
        }
        CHECK(speedDetected);
    }
    {
        Movement m;
        m.context.onTransport = true;
        m.context.serverTransport = true;
        m.context.rooted = true;
        m.Step(100);
        m.Step(1500);
        bool speedDetected = false;
        bool controlDetected = false;
        for (unsigned i = 0; i < 30; ++i)
        {
            // Core relocation/transport updates may use different client-provided
            // world/local positions within the adapter's validation tolerance.
            m.context.serverPosition = m.state.previousPosition;
            m.context.serverPosition.x += 1.0f;
            m.context.position.x += (i / 5) % 2 ? -20.0f : 20.0f;
            m.context.nowMs += 100;
            m.context.clientTimeMs += 100;
            auto result = EvaluateMovement(m.context, m.state, m.cfg);
            speedDetected = Has(result, DetectionType::Speed) || speedDetected;
            controlDetected = Has(result, DetectionType::IgnoreControl) || controlDetected;
        }
        CHECK(speedDetected);
        CHECK(controlDetected);
        CHECK(m.state.clockWindowStartMs > 1000);
    }
    {
        Movement m;
        m.state.speedGraceUntilMs = m.context.nowMs + 1500;
        m.state.speedGraceMaxSpeed = 14.0f;
        m.context.flying = true;
        auto result = m.Step(100, 100.0f);
        CHECK(Has(result, DetectionType::Speed)); // Transition allowance is bounded, never immunity.
        CHECK(Has(result, DetectionType::Fly));
        CHECK(Has(result, DetectionType::Teleport));
    }
    {
        Movement m;
        m.state.speedGraceUntilMs = m.context.nowMs + 1500;
        m.state.speedGraceMaxSpeed = 14.0f; // Recently reduced server speed: 14 -> 7.
        for (unsigned i = 0; i < 15; ++i)
            CHECK(!Has(m.Step(100, 1.4f), DetectionType::Speed));
        bool detected = false;
        for (unsigned i = 0; i < 60; ++i)
            detected = Has(m.Step(100, 1.4f), DetectionType::Speed) || detected;
        CHECK(detected); // Continuing at the old speed beyond expiry is checked normally.
    }
    {
        Movement m;
        // A mounted speed reduction may arrive before the client's delayed movement.
        // Both detectors must use the same bounded server transition allowance.
        m.state.speedGraceUntilMs = m.context.nowMs + 1500;
        m.state.speedGraceMaxSpeed = 42.0f;
        CHECK(!HasActionable(m.Step(100, 4.2f)));
        CHECK(!HasActionable(m.Step(1400, 58.8f)));
        CHECK(Has(m.Step(1400, 58.8f), DetectionType::Teleport));
    }
    {
        Movement m;
        // An interval straddles expiry: average speed determines travelled distance,
        // while the authorized peak bounds the extra latency allowance.
        m.context.latencyMs = 500;
        m.state.speedGraceUntilMs = m.context.nowMs + 1000;
        m.state.speedGraceMaxSpeed = 16.3f; // (100 * .1 + 7 * .9) / 1 second.
        m.state.speedGracePeakSpeed = 100.0f;
        CHECK(!HasActionable(m.Step(1000, 60.0f)));
        CHECK(Has(m.Step(1000, 60.0f), DetectionType::Teleport));
    }
    {
        Movement m;
        bool detected = false;
        for (unsigned i = 0; i < 30; ++i)
        {
            m.context.allowedSpeed = i % 2 ? 7.0f : 14.0f;
            m.state.speedGraceUntilMs = m.context.nowMs + 1500;
            m.state.speedGraceMaxSpeed = 14.0f;
            detected = Has(m.Step(100, 20.0f), DetectionType::Speed) || detected;
        }
        CHECK(detected); // Legitimate repeated speed toggles cannot erase accumulated travel.
    }
    {
        Movement m;
        m.state.speedGraceUntilMs = m.context.nowMs + 5000;
        m.context.rooted = true;
        m.Step(100);
        m.Step(1500);
        CHECK(Has(m.Step(100, 2.0f), DetectionType::IgnoreControl));
        m.context.serverPosition.x += 20.0f;
        m.context.position.x = m.context.serverPosition.x;
        EvaluateMovement(m.context, m.state, m.cfg);
        CHECK(m.state.speedGraceUntilMs == 6000); // Trusted server rebase preserves bounded order.
        m.state.Reset();
        CHECK(m.state.speedGraceUntilMs == 0); // Session lifecycle discards every order.
    }
    {
        Movement m;
        m.state.GrantGrace(m.context.nowMs, 50000);
        m.context.nowMs = 10;
        EvaluateMovement(m.context, m.state, m.cfg);
        CHECK(m.state.graceUntilMs == 10 + m.cfg.transitionGraceMs);
        m.context.flying = true;
        CHECK(Has(m.Step(m.cfg.transitionGraceMs), DetectionType::Fly));
    }
    {
        // Deterministic varying speeds/receipt intervals, including zero-time packets.
        // The simulated trajectory never exceeds the currently authorized velocity.
        Movement m;
        std::uint32_t seed = 0x714fe3a9;
        for (unsigned i = 0; i < 10000; ++i)
        {
            seed = seed * 1664525U + 1013904223U;
            auto elapsed = seed % 201;
            m.context.allowedSpeed = 1.0f + static_cast<float>((seed >> 8) % 42);
            m.context.latencyMs = (seed >> 16) % 4000;
            auto result = m.Step(elapsed, m.context.allowedSpeed * static_cast<float>(elapsed) / 1000.0f);
            CHECK(!Has(result, DetectionType::Speed));
            CHECK(!Has(result, DetectionType::Teleport));
        }
    }
}
