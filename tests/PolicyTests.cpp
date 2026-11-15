#include "TestHarness.h"
#include "scoring/ViolationScorer.h"
#include "enforcement/EnforcementManager.h"
#include <limits>

using namespace anticheat;

namespace
{
DetectionResult Signal(DetectionType type, float confidence = 1, bool actionable = true)
{
    return {type, confidence, actionable, 30, 7, "test"};
}

ScoreState RepeatedEvidence(ScoreConfig const& config, DetectionType second = DetectionType::Fly)
{
    ScoreState state;
    DetectionResult results[] = {Signal(DetectionType::Speed), Signal(second)};
    for (std::uint64_t time = 1000; time <= 81000; time += 20000)
    {
        ViolationScorer::Observe(state, results, 2, time, config);
        ViolationScorer::Observe(state, nullptr, 0, time + 1, config);
    }
    return state;
}
}

void RunPolicyTests()
{
    ScoreConfig config;
    config.decayPerSecond = 0;
    auto speed = Signal(DetectionType::Speed);
    ScoreState state;
    CHECK(ViolationScorer::Observe(state, nullptr, 0, 0, config).count == 0);
    CHECK(ViolationScorer::Observe(state, &speed, 1, 0, config).count == 1);
    for (std::uint64_t time = 1; time <= 10000; ++time)
        CHECK(ViolationScorer::Observe(state, &speed, 1, time, config).count == 0);
    CHECK(state.totalAccepted == 1);
    CHECK(state.rawObservations == 10001);
    CHECK_NEAR(state.score, 5, 0.001f);

    // No packets during a long outage cannot manufacture a clean observation.
    CHECK(ViolationScorer::Observe(state, &speed, 1, 120000, config).count == 0);
    ViolationScorer::Observe(state, nullptr, 0, 120001, config);
    CHECK(ViolationScorer::Observe(state, &speed, 1, 130002, config).count == 1);
    CHECK(state.counts[Index(DetectionType::Speed)] == 2);
    CHECK_NEAR(state.scoreSum / state.scoreSamples, 7.5, 0.001);

    // Windowed speed output is absent between evaluations, not a new incident.
    ScoreState sparse;
    for (std::uint64_t time = 0; time <= 120000; time += 100)
    {
        bool anomaly = time % 3000 == 0;
        ViolationScorer::Observe(sparse, anomaly ? &speed : nullptr, anomaly ? 1 : 0, time, config);
    }
    CHECK(sparse.totalAccepted == 1);

    // A cooldown rejection consumes that episode too; it cannot turn into a
    // fresh incident automatically at the cooldown boundary.
    ScoreState cooling;
    auto longCooldown = config;
    longCooldown.cooldownMs = 60000;
    CHECK(ViolationScorer::Observe(cooling, &speed, 1, 0, longCooldown).count == 1);
    ViolationScorer::Observe(cooling, nullptr, 0, 1, longCooldown);
    CHECK(ViolationScorer::Observe(cooling, &speed, 1, 20000, longCooldown).count == 0);
    CHECK(ViolationScorer::Observe(cooling, &speed, 1, 60000, longCooldown).count == 0);
    ViolationScorer::Observe(cooling, nullptr, 0, 60001, longCooldown);
    CHECK(ViolationScorer::Observe(cooling, &speed, 1, 70002, longCooldown).count == 1);

    ScoreState decay;
    ViolationScorer::Observe(decay, &speed, 1, 0, config);
    auto decaying = config;
    decaying.decayPerSecond = 1;
    ViolationScorer::Decay(decay, 1500, decaying);
    CHECK_NEAR(decay.score, 3.5f, 0.001f);
    ViolationScorer::Decay(decay, 10000, decaying);
    CHECK(decay.score == 0);

    ScoreState diagnostic;
    auto geometry = Signal(DetectionType::NoClip, 0.5f, false);
    CHECK(ViolationScorer::Observe(diagnostic, &geometry, 1, 0, config).count == 1);
    CHECK(diagnostic.score == 0);
    CHECK(ViolationScorer::Observe(diagnostic, &speed, 1, 1, config).count == 1);
    auto warden = Signal(DetectionType::Warden, 1, true);
    CHECK(ViolationScorer::Observe(diagnostic, &warden, 1, 2, config).count == 1);
    CHECK_NEAR(diagnostic.score, 5, 0.001f);
    auto flight = Signal(DetectionType::Fly);
    auto duringCooldown = ViolationScorer::Observe(diagnostic, &flight, 1, 3, config);
    CHECK(duringCooldown.count == 1);
    CHECK(!duringCooldown.accepted[0].actionable);
    CHECK_NEAR(diagnostic.score, 5, 0.001f);

    ScoreState lowConfidence;
    auto uncertain = Signal(DetectionType::Speed, 0.5f, true);
    auto accepted = ViolationScorer::Observe(lowConfidence, &uncertain, 1, 0, config);
    CHECK(accepted.count == 1);
    CHECK(!accepted.accepted[0].actionable);
    CHECK(lowConfidence.score == 0);
    auto invalid = Signal(DetectionType::Fly, std::numeric_limits<float>::quiet_NaN());
    CHECK(ViolationScorer::Observe(lowConfidence, &invalid, 1, 0, config).count == 0);

    ScoreState burst;
    DetectionResult all[] = {Signal(DetectionType::Speed), Signal(DetectionType::Fly),
        Signal(DetectionType::Teleport), Signal(DetectionType::Waterwalk), Signal(DetectionType::IgnoreControl)};
    CHECK(ViolationScorer::Observe(burst, all, 5, 0, config).count == 5);
    CHECK_NEAR(burst.score, config.maxScorePerObservation, 0.001f);

    EnforcementConfig policy;
    EnforcementState enforcement;
    auto evidence = RepeatedEvidence(config);
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 50, false, policy, config).action == Action::None);
    policy.mode = EnforcementMode::Alert;
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 50, false, policy, config).action == Action::None);
    policy.mode = EnforcementMode::Automatic;
    auto decision = EnforcementManager::Decide(evidence, enforcement, 81001, 50, false, policy, config);
    CHECK(decision.action == Action::Kick);
    CHECK(decision.evidenceCount == 5);
    CHECK(decision.familyMask == 3);
    CHECK(decision.spanMs == 80000);
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 1000, false, policy, config).action == Action::None);
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 50, true, policy, config).action == Action::None);
    policy.banScore = 90;
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 50, false, policy, config).action == Action::Ban);
    policy.banSeconds = 0;
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 50, false, policy, config).action == Action::Kick);
    enforcement.applied = true;
    CHECK(EnforcementManager::Decide(evidence, enforcement, 81001, 50, false, policy, config).action == Action::None);
    enforcement.applied = false;
    CHECK(EnforcementManager::Decide(evidence, enforcement, 300000, 50, false, policy, config).action == Action::None);

    // Speed and teleport are correlated displacement evidence, one family.
    auto correlated = RepeatedEvidence(config, DetectionType::Teleport);
    CHECK(EnforcementManager::Decide(correlated, enforcement, 81001, 50, false, policy, config).action == Action::None);
    // One packet with five detector flags remains only one episode.
    policy.kickScore = 1;
    CHECK(EnforcementManager::Decide(burst, enforcement, 30000, 50, false, policy, config).action == Action::None);
    ScoreState reconnect;
    CHECK(reconnect.score == 0 && reconnect.totalAccepted == 0);
}
