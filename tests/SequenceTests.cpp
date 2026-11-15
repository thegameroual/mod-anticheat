/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#include "TestHarness.h"
#include "core/AnticheatConfig.h"
#include "detectors/MovementDetectors.h"

#include <algorithm>
#include <iomanip>

namespace
{
using namespace anticheat;

struct Replay
{
    AnticheatConfig config;
    MovementState movement;
    ScoreState score;
    AlertState alerts;
    EnforcementState enforcement;
    DetectionContext context;
    DetectionBatch detections;
    AlertDecision alert;
    EnforcementDecision decision;
    std::uint64_t firstAlert = 0, firstAction = 0, reports = 0, alertCount = 0;
    bool protectedTransition = false;

    Replay()
    {
        context.nowMs = context.clientTimeMs = 1000;
        context.mapId = 1;
        detections = EvaluateMovement(context, movement, config.detectors);
        ViolationScorer::Observe(score, detections, context.nowMs, config.scoring);
    }

    void Step(std::uint32_t elapsed, float speed = 7.0f)
    {
        Distance(elapsed, speed * static_cast<float>(elapsed) / 1000.0f);
    }

    void Distance(std::uint32_t elapsed, float distance)
    {
        context.serverPosition = movement.previousPosition;
        context.nowMs += elapsed;
        context.clientTimeMs += elapsed;
        context.position.x += distance;
        detections = EvaluateMovement(context, movement, config.detectors);
        auto update = ViolationScorer::Observe(score, detections, context.nowMs, config.scoring);
        reports += update.count;
        alert = AlertPolicy::Evaluate(score, alerts, context.nowMs, config.enforcement.mode, config.alerts);
        decision = EnforcementManager::Decide(score, enforcement, context.nowMs, context.latencyMs,
            protectedTransition, config.enforcement, config.scoring);
        if (alert.emit)
        {
            ++alertCount;
            if (!firstAlert) firstAlert = context.nowMs - 1000;
        }
        if (decision.action != Action::None && !firstAction) firstAction = context.nowMs - 1000;
    }

    void Run(std::uint32_t duration, std::uint32_t interval, float speed)
    {
        CHECK(duration % interval == 0);
        for (std::uint32_t t = 0; t < duration; t += interval) Step(interval, speed);
    }

    DetectorEpisode const& Speed() const { return score.episodes[Index(DetectionType::Speed)]; }

    void Trace(char const* scenario) const
    {
        constexpr char const* phases[] = {"dormant", "open", "persistent", "interrupted", "resolved", "expired"};
        std::cout << std::fixed << std::setprecision(2) << "trace scenario=" << scenario
            << " server_ms=" << context.nowMs << " observed=" << Speed().observed
            << " budget=" << Speed().allowed << " incident=" << Speed().id
            << " phase=" << phases[static_cast<unsigned>(Speed().phase)]
            << " covered_ms=" << Speed().coveredMs << " windows=" << Speed().confirmedWindows
            << " risk=" << score.score << " alert=" << alert.emit
            << " action=" << static_cast<unsigned>(decision.action)
            << " first_alert_elapsed_ms=" << firstAlert << " first_action_elapsed_ms=" << firstAction << '\n';
    }
};
}

void RunSequenceTests()
{
    using namespace anticheat;
    {
        Replay normal;
        normal.config.enforcement.mode = EnforcementMode::Automatic;
        normal.config.enforcement.singleFamilyEnabled = true;
        normal.Trace("legitimate");
        normal.Run(180000, 100, 7);
        normal.Trace("legitimate");
        CHECK(normal.score.totalAccepted == 0);
        CHECK(normal.score.score == 0 && normal.firstAlert == 0 && normal.firstAction == 0);
    }
    {
        Replay isolated;
        isolated.config.enforcement.mode = EnforcementMode::Alert;
        isolated.Run(3000, 100, 20);
        isolated.Trace("isolated");
        // Two 1.4-second burst evaluations do not yet fill a 3-second confirmation bucket.
        CHECK(isolated.Speed().id != 0 && isolated.Speed().confirmedWindows == 0);
        isolated.Run(30000, 100, 7);
        isolated.Trace("isolated");
        CHECK(isolated.score.totalAccepted == 1);
        CHECK(isolated.Speed().phase == IncidentPhase::Resolved);
        CHECK(isolated.score.score == 0 && isolated.firstAlert == 0 && isolated.firstAction == 0);
    }
    for (auto mode : {EnforcementMode::Observation, EnforcementMode::Alert, EnforcementMode::Automatic})
    {
        Replay persistent;
        persistent.config.enforcement.mode = mode;
        persistent.config.enforcement.singleFamilyEnabled = true;
        CHECK(persistent.config.scoring.decayPerSecond == 1.0f);
        persistent.Trace(mode == EnforcementMode::Automatic ? "speed-single-opt-in" : "speed-observation-alert");
        for (unsigned t = 100; t <= 90000; t += 100)
        {
            persistent.Step(100, 20);
            if (t == 3000 || t == 9000 || t == 30000 || t == 60000 || t == 90000)
                persistent.Trace(mode == EnforcementMode::Automatic ? "speed-single-opt-in" : "speed-observation-alert");
        }
        CHECK(persistent.score.counts[Index(DetectionType::Speed)] == 1);
        CHECK(persistent.Speed().coveredMs >= 87000 && persistent.Speed().coveredMs <= 90000);
        CHECK(persistent.Speed().confirmedWindows == persistent.Speed().coveredMs / 3000);
        CHECK(persistent.score.score >= 100);
        CHECK(persistent.reports <= 8); // Open plus bounded updates, never a row per packet.
        if (mode == EnforcementMode::Observation) CHECK(persistent.alertCount == 0);
        else
        {
            CHECK(persistent.firstAlert >= 9000 && persistent.firstAlert <= 12000);
            CHECK(persistent.alertCount == 2); // Initial alert and a 60-second reminder.
        }
        if (mode != EnforcementMode::Automatic) CHECK(persistent.firstAction == 0);
        else
        {
            CHECK(persistent.firstAction >= 60000 && persistent.firstAction <= 63000);
            CHECK(persistent.decision.action == Action::Kick && persistent.decision.singleFamily);
            CHECK(persistent.decision.spanMs >= 60000 && persistent.decision.evidenceCount >= 20);
            CHECK(persistent.decision.incidentId == persistent.Speed().id);
            persistent.enforcement.applied = true;
            persistent.Step(100, 20);
            CHECK(persistent.decision.action == Action::None);
        }
    }
    {
        Replay disabled;
        disabled.config.enforcement.mode = EnforcementMode::Automatic;
        disabled.Run(120000, 100, 20);
        CHECK(disabled.Speed().confirmedWindows >= 20);
        CHECK(disabled.firstAction == 0); // One-family route requires its own opt-in.
        Replay corroborated;
        corroborated.config.enforcement.mode = EnforcementMode::Automatic;
        corroborated.context.flying = true;
        corroborated.Run(60000, 100, 20);
        corroborated.Trace("speed-and-flight");
        CHECK(corroborated.firstAction >= 30000 && corroborated.firstAction <= 33000);
        CHECK(!corroborated.decision.singleFamily && corroborated.decision.familyMask == 3);
        CHECK(corroborated.config.scoring.decayPerSecond == 1.0f);
    }
    {
        float previousRisk = -1;
        for (std::uint32_t interval : {50u, 100u, 250u})
        {
            Replay rate;
            rate.config.enforcement.mode = EnforcementMode::Automatic;
            rate.config.enforcement.singleFamilyEnabled = true;
            rate.Run(63000, interval, 20);
            CHECK(rate.firstAlert >= 9000 && rate.firstAlert <= 12000);
            CHECK(rate.firstAction >= 60000 && rate.firstAction <= 63000);
            CHECK(rate.Speed().coveredMs >= 60000 && rate.Speed().coveredMs <= 63000);
            CHECK(rate.Speed().confirmedWindows == rate.Speed().coveredMs / 3000);
            CHECK(rate.score.counts[Index(DetectionType::Speed)] == 1);
            // Closing a burst at the next received sample shifts decay by <=250 ms.
            if (previousRisk >= 0) CHECK_NEAR(rate.score.score, previousRisk, 0.3);
            previousRisk = rate.score.score;
        }
    }
    {
        Replay silent;
        silent.config.enforcement.mode = EnforcementMode::Automatic;
        silent.config.enforcement.singleFamilyEnabled = true;
        silent.Run(30000, 100, 20);
        auto covered = silent.Speed().coveredMs;
        auto id = silent.Speed().id;
        auto beforeRisk = silent.score.score;
        silent.context.nowMs += 10000;
        auto updates = ViolationScorer::Tick(silent.score, silent.context.nowMs, silent.config.scoring);
        CHECK(updates.count > 0);
        CHECK(silent.Speed().coveredMs == covered && silent.Speed().id == id);
        CHECK(silent.Speed().phase == IncidentPhase::Interrupted);
        CHECK(silent.Speed().qualifiedMs == 0 && silent.Speed().confirmedWindows == 0);
        CHECK(silent.score.score < beforeRisk);
        CHECK(!AlertPolicy::Evaluate(silent.score, silent.alerts, silent.context.nowMs,
            silent.config.enforcement.mode, silent.config.alerts).emit);
        CHECK(EnforcementManager::Decide(silent.score, silent.enforcement, silent.context.nowMs,
            0, false, silent.config.enforcement, silent.config.scoring).action == Action::None);
        silent.Step(100, 20); // Engine sees the gap and rebases; no invented normal interval.
        CHECK(silent.Speed().normalMs == 0);
        CHECK(silent.Speed().id == id && silent.Speed().coveredMs == covered);
    }
    {
        Replay sparse;
        sparse.Run(4200, 100, 20);
        auto id = sparse.Speed().id;
        sparse.Step(100, 20);
        CHECK(sparse.detections.evaluations[Index(DetectionType::Speed)].status == EvaluationStatus::NotEvaluated);
        CHECK(sparse.Speed().id == id && sparse.Speed().normalMs == 0);
        CHECK(sparse.Speed().confirmedWindows == 1);
        sparse.Run(2700, 100, 20);
        CHECK(sparse.Speed().id == id && sparse.Speed().confirmedWindows == 2);
    }
    {
        Replay jitter;
        jitter.config.enforcement.mode = EnforcementMode::Automatic;
        jitter.config.enforcement.singleFamilyEnabled = true;
        constexpr std::uint32_t intervals[] = {50, 150, 400, 0, 200, 100, 600, 500, 1000};
        for (unsigned window = 0; window < 21; ++window)
            for (auto dt : intervals) jitter.Step(dt, 20);
        CHECK(jitter.context.nowMs == 64000);
        CHECK(jitter.Speed().coveredMs >= 60000 && jitter.firstAction >= 60000 && jitter.firstAction <= 63000);
        Replay batched;
        batched.config.enforcement.mode = EnforcementMode::Automatic;
        batched.config.enforcement.singleFamilyEnabled = true;
        for (unsigned seconds = 0; seconds < 63; ++seconds)
        {
            batched.Step(1000, 20);
            for (unsigned duplicate = 0; duplicate < 20; ++duplicate) batched.Step(0, 0);
        }
        CHECK(batched.Speed().coveredMs >= 60000 && batched.Speed().coveredMs <= 63000);
        CHECK(batched.Speed().confirmedWindows >= 20 && batched.Speed().confirmedWindows <= 21);
        batched.Trace("batched");
        jitter.Trace("jitter");
        // At the risk cap, one confirmation bucket's different closing time leaves
        // up to three seconds of decay (3 points), not extra independent evidence.
        CHECK_NEAR(batched.score.score, jitter.score.score, 3.1);
        CHECK(batched.firstAction >= 60000 && batched.firstAction <= 63000);
    }
    {
        // Effective 3-second budget: 7 * 3 * 1.25 + 1 = 27.25 yards.
        Replay boundary;
        boundary.Run(3000, 100, 9.0f);
        CHECK(boundary.Speed().id == 0);
        Replay marginal;
        marginal.config.enforcement.mode = EnforcementMode::Automatic;
        marginal.config.enforcement.singleFamilyEnabled = true;
        marginal.Run(90000, 100, 9.2f);
        CHECK(marginal.Speed().id != 0);
        CHECK_NEAR(marginal.Speed().allowed, 27.25, 0.01);
        CHECK(marginal.Speed().severity < marginal.config.alerts.minSeverity);
        CHECK(marginal.firstAlert == 0 && marginal.firstAction == 0);
        // The latency allowance is paid once per window: +7*.5 = 3.5 yards.
        Replay latency;
        latency.context.latencyMs = 500;
        latency.Run(3000, 100, 10.2f);
        CHECK(latency.Speed().id == 0);
        latency.Run(3000, 100, 10.4f);
        CHECK(latency.Speed().id != 0);
        CHECK_NEAR(latency.Speed().allowed, 30.75, 0.01);
    }
    {
        Replay burst;
        burst.config.enforcement.mode = EnforcementMode::Automatic;
        burst.config.enforcement.singleFamilyEnabled = true;
        for (unsigned i = 0; i < 1000; ++i) burst.Distance(0, 100);
        CHECK(burst.score.counts[Index(DetectionType::Speed)] == 1);
        CHECK(burst.Speed().coveredMs == 0 && burst.Speed().confirmedWindows == 0);
        CHECK(burst.firstAlert == 0 && burst.firstAction == 0);
        CHECK(burst.score.score == 0);
        CHECK(burst.score.counts[Index(DetectionType::Teleport)] == 1);
        CHECK(burst.decision.familyMask == 0);
    }
    {
        Replay diagnostic;
        diagnostic.config.enforcement.mode = EnforcementMode::Automatic;
        diagnostic.config.enforcement.singleFamilyEnabled = true;
        diagnostic.config.scoring.weights.fill(1000);
        diagnostic.context.disableGravity = true;
        diagnostic.context.geometrySampled = true;
        diagnostic.context.lineBlocked = true;
        diagnostic.Run(90000, 100, 7);
        CHECK(diagnostic.score.rawObservations > 100);
        CHECK(diagnostic.score.score == 0 && diagnostic.firstAction == 0);
    }
    {
        Replay correlated;
        correlated.config.enforcement.mode = EnforcementMode::Automatic;
        // Both detectors see the same actual displacement, with real nonzero coverage.
        for (unsigned window = 0; window < 20; ++window) correlated.Distance(3000, 200);
        auto const& teleport = correlated.score.episodes[Index(DetectionType::Teleport)];
        CHECK(correlated.Speed().confirmedWindows == 20 && teleport.confirmedWindows == 20);
        CHECK(correlated.score.counts[Index(DetectionType::Speed)] == 1);
        CHECK(correlated.score.counts[Index(DetectionType::Teleport)] == 1);
        CHECK_NEAR(correlated.score.score, std::max(correlated.Speed().risk, teleport.risk), 0.001);
        CHECK(correlated.decision.familyMask == 1 && correlated.firstAction == 0);
    }
    {
        Replay rooted;
        rooted.config.enforcement.mode = EnforcementMode::Automatic;
        rooted.context.rooted = true;
        rooted.Run(1600, 100, 0); // Root acknowledgement grace, then a stable anchor.
        rooted.Distance(100, 2);
        auto const& control = rooted.score.episodes[Index(DetectionType::IgnoreControl)];
        CHECK(control.id != 0);
        rooted.Run(30000, 100, 0); // Remaining at an old offset is not continued movement.
        CHECK(rooted.score.counts[Index(DetectionType::IgnoreControl)] == 1);
        CHECK(control.coveredMs == 0 && control.confirmedWindows == 0);
        CHECK(control.phase == IncidentPhase::Resolved);
        CHECK(rooted.score.score == 0 && rooted.firstAlert == 0 && rooted.firstAction == 0);
    }
    {
        Replay authorized;
        authorized.config.enforcement.mode = EnforcementMode::Automatic;
        authorized.config.enforcement.singleFamilyEnabled = true;
        authorized.context.allowedSpeed = 14;
        authorized.context.speedTransition = true;
        authorized.Run(1500, 100, 14);
        authorized.context.speedTransition = false;
        authorized.Run(60000, 100, 14);
        CHECK(authorized.score.score == 0 && authorized.firstAlert == 0 && authorized.firstAction == 0);
        Replay transport;
        transport.config.enforcement.mode = EnforcementMode::Automatic;
        transport.config.enforcement.singleFamilyEnabled = true;
        transport.context.onTransport = transport.context.serverTransport = true;
        transport.Run(90000, 100, 7);
        CHECK(transport.score.score == 0 && transport.firstAlert == 0 && transport.firstAction == 0);
    }
    {
        Replay transition;
        transition.config.enforcement.mode = EnforcementMode::Automatic;
        transition.config.enforcement.singleFamilyEnabled = true;
        transition.Run(30000, 100, 20);
        auto covered = transition.Speed().coveredMs;
        transition.context.teleport = true;
        transition.Distance(100, 1000);
        CHECK(transition.Speed().qualifiedMs == 0);
        CHECK(transition.Speed().coveredMs == covered);
        CHECK(transition.decision.action == Action::None);
        transition.context.teleport = false;
        transition.Run(1500, 100, 7);
        CHECK(transition.Speed().coveredMs == covered);
        transition.context.forcedMovement = true;
        transition.Run(10000, 100, 50);
        CHECK(transition.Speed().coveredMs == covered && transition.firstAction == 0);
        // A fresh session has no authority, evidence, alerts or action from the prior one.
        Replay reconnected;
        CHECK(reconnected.score.totalAccepted == 0 && reconnected.Speed().id == 0);
        CHECK(!reconnected.enforcement.applied && reconnected.firstAlert == 0);
    }
    {
        Replay quality;
        quality.config.enforcement.mode = EnforcementMode::Automatic;
        quality.config.enforcement.singleFamilyEnabled = true;
        quality.Run(60000, 100, 20);
        quality.protectedTransition = true; // Adapter uses the same guard for queue/authority degradation.
        quality.Step(100, 20);
        CHECK(quality.decision.action == Action::None);
        quality.protectedTransition = false;
        quality.context.latencyMs = quality.config.enforcement.maxLatencyMs + 1;
        quality.Step(100, 20);
        CHECK(quality.decision.action == Action::None);
    }
    {
        Replay frames;
        frames.config.enforcement.mode = EnforcementMode::Alert;
        frames.context.flying = true;
        frames.context.waterWalking = true;
        for (unsigned i = 0; i < 300; ++i)
        {
            frames.context.onTransport = frames.context.serverTransport = i % 2 == 0;
            frames.Step(100, 20);
        }
        auto const& flight = frames.score.episodes[Index(DetectionType::Fly)];
        auto const& water = frames.score.episodes[Index(DetectionType::Waterwalk)];
        CHECK(flight.confirmedWindows == 10 && flight.qualifiedMs == 30000);
        CHECK(water.confirmedWindows == 10 && water.qualifiedMs == 30000);
        CHECK(frames.Speed().qualifiedMs == 0 && frames.Speed().confirmedWindows == 0);
        CHECK(frames.firstAlert >= 9000 && frames.firstAlert <= 12000);
        CHECK(frames.firstAction == 0);
    }
}
