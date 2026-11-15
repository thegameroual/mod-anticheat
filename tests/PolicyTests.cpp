#include "TestHarness.h"
#include "core/AnticheatConfig.h"
#include <limits>
using namespace anticheat;
namespace
{
DetectionBatch Evaluation(std::uint64_t begin, std::uint64_t end, bool anomaly = true,
    DetectionType type = DetectionType::Speed, bool actionable = true)
{
    DetectionBatch b;
    b.evaluations[Index(type)] = {anomaly ? EvaluationStatus::Anomalous : EvaluationStatus::Normal, begin, end, true};
    if (anomaly) b.Add({type, 0.9f, actionable, 60, 27.25f, "test interval"});
    return b;
}
}
void RunPolicyTests()
{
    {
        // Later point samples update diagnostic reports without inventing duration.
        ScoreState points;
        ScoreConfig policy;
        auto sample = Evaluation(1000, 1000, true, DetectionType::NoClip, false);
        CHECK(ViolationScorer::Observe(points, sample, 1000, policy).count == 1);
        auto id = points.episodes[Index(DetectionType::NoClip)].id;
        sample = Evaluation(16000, 16000, true, DetectionType::NoClip, false);
        CHECK(ViolationScorer::Observe(points, sample, 16000, policy).count == 1);
        auto const& incident = points.episodes[Index(DetectionType::NoClip)];
        CHECK(incident.id == id && incident.lastAnomalyMs == 16000);
        CHECK(incident.coveredMs == 0 && points.score == 0);
    }
    ScoreConfig c;
    ScoreState s;
    auto first = ViolationScorer::Observe(s, Evaluation(0,3000), 3000, c);
    CHECK(first.count == 1 && first.incidents[0].opened);
    auto id = s.episodes[0].id;
    for (std::uint64_t now = 6000; now <= 60000; now += 3000)
        ViolationScorer::Observe(s, Evaluation(now-3000,now), now, c);
    CHECK(s.score > 50); // Previously failed: ongoing Speed risk decayed to zero.
    CHECK(s.totalAccepted == 1 && s.episodes[0].id == id);
    CHECK(s.episodes[0].confirmedWindows == 20);
    CHECK(s.episodes[0].coveredMs == 60000);
    auto saved = s.episodes[0].coveredMs;
    for (unsigned i=0; i<10000; ++i)
        ViolationScorer::Observe(s, Evaluation(57000,60000), 60000, c);
    CHECK(s.episodes[0].coveredMs == saved);
    CHECK(s.totalAccepted == 1);
    DetectionBatch unknown;
    ViolationScorer::Observe(s, unknown, 63000, c);
    CHECK(s.episodes[0].normalMs == 0);
    auto tick = ViolationScorer::Tick(s, 67000, c);
    CHECK(tick.count == 1 && s.episodes[0].phase == IncidentPhase::Interrupted);
    CHECK(s.episodes[0].qualifiedMs == 0);
    CHECK(s.episodes[0].coveredMs == saved);
    ViolationScorer::Observe(s, Evaluation(67000,70000), 70000, c);
    CHECK(s.episodes[0].id == id); // Interrupted observation is not a new incident.
    for (std::uint64_t now=73000; now<=82000; now+=3000)
        ViolationScorer::Observe(s, Evaluation(now-3000,now,false), now, c);
    CHECK(s.episodes[0].phase == IncidentPhase::Resolved);
    ViolationScorer::Observe(s, Evaluation(82000,85000),85000,c);
    CHECK(s.episodes[0].id != id && s.totalAccepted == 2);
    ViolationScorer::Tick(s, 300000,c);
    CHECK(s.episodes[0].phase == IncidentPhase::Expired);
    CHECK(s.score == 0);

    ScoreState correlated, speedOnly, diagnostics;
    auto badWeights = c;
    badWeights.weights.fill(1000);
    for (std::uint64_t now=3000; now<=90000; now+=3000)
    {
        auto b = Evaluation(now-3000,now);
        ViolationScorer::Observe(speedOnly,b,now,c);
        b.Add({DetectionType::Teleport,.9f,true,60,27.25f,"same displacement"});
        b.evaluations[Index(DetectionType::Teleport)] = b.evaluations[0];
        ViolationScorer::Observe(correlated,b,now,c);
        ViolationScorer::Observe(diagnostics,Evaluation(now-3000,now,true,DetectionType::NoClip,true),now,badWeights);
    }
    CHECK(correlated.score <= speedOnly.score + 10); // Maximum within family, never sum.
    CHECK(diagnostics.score == 0);
    EnforcementConfig policy;
    EnforcementState latch;
    policy.singleFamilyEnabled = true;
    CHECK(EnforcementManager::Decide(speedOnly,latch,90000,50,false,policy,c).action == Action::None);
    policy.mode = EnforcementMode::Alert;
    CHECK(EnforcementManager::Decide(speedOnly,latch,90000,50,false,policy,c).action == Action::None);
    policy.mode = EnforcementMode::Automatic;
    CHECK(EnforcementManager::Decide(speedOnly,latch,90000,50,false,policy,c).action == Action::Kick);
    CHECK(EnforcementManager::Decide(speedOnly,latch,90000,900,false,policy,c).action == Action::None);
    CHECK(EnforcementManager::Decide(speedOnly,latch,90000,50,true,policy,c).action == Action::None);
    latch.applied = true;
    CHECK(EnforcementManager::Decide(speedOnly,latch,90000,50,false,policy,c).action == Action::None);
    latch.applied = false;
    policy.singleFamilyEnabled = false;
    CHECK(EnforcementManager::Decide(correlated,latch,90000,50,false,policy,c).action == Action::None);
    policy.singleFamilyEnabled = true;
    CHECK(EnforcementManager::Decide(diagnostics,latch,90000,50,false,policy,c).action == Action::None);
    CHECK(EnforcementManager::Decide(speedOnly,latch,97000,50,false,policy,c).action == Action::None);
    AlertState alerts;
    AlertConfig alertConfig;
    CHECK(!AlertPolicy::Evaluate(speedOnly,alerts,90000,EnforcementMode::Observation,alertConfig).emit);
    CHECK(AlertPolicy::Evaluate(speedOnly,alerts,90000,EnforcementMode::Alert,alertConfig).emit);
    CHECK(!AlertPolicy::Evaluate(speedOnly,alerts,90001,EnforcementMode::Alert,alertConfig).emit);

    ScoreState unknownOnly;
    DetectionResult signal{DetectionType::Speed,.9f,true,60,27,"no evaluated interval"};
    for (std::uint64_t now=0; now<=120000; now+=3000)
        ViolationScorer::Observe(unknownOnly,&signal,1,now,c);
    CHECK(unknownOnly.totalAccepted == 1);
    CHECK(unknownOnly.score == 0 && unknownOnly.episodes[0].coveredMs == 0);
    ScoreState invalid;
    signal.confidence = std::numeric_limits<float>::quiet_NaN();
    CHECK(ViolationScorer::Observe(invalid,&signal,1,0,c).count == 0);

    // A weak early window must not veto all later, strongly measured windows.
    ScoreState improving;
    for (std::uint64_t now=3000; now<=180000; now+=3000)
    {
        auto b = Evaluation(now-3000,now);
        b.results[0].allowed = 10;
        b.results[0].observed = now == 3000 ? 11 : 20;
        ViolationScorer::Observe(improving,b,now,c);
    }
    CHECK(EnforcementManager::Decide(improving,{},180000,50,false,policy,c).action == Action::Kick);
    auto indeterminate = Evaluation(180000,180100,false);
    indeterminate.evaluations[0].reliable = false;
    ViolationScorer::Observe(improving,indeterminate,180100,c);
    CHECK(EnforcementManager::Decide(improving,{},180100,50,false,policy,c).action == Action::None);
    CHECK(improving.episodes[0].qualifiedMs == 0);

    // A diagnostic portion of a bucket cannot later become qualified evidence.
    ScoreState mixed;
    ViolationScorer::Observe(mixed,Evaluation(0,2000,true,DetectionType::Speed,false),2000,c);
    ViolationScorer::Observe(mixed,Evaluation(2000,3000),3000,c);
    CHECK(mixed.episodes[0].qualifiedMs == 1000);
    CHECK(mixed.episodes[0].confirmedWindows == 0);

    // A weak fragment taints its entire confirmed window, even if the last
    // fragment is strong; only a later uninterrupted strong suffix can qualify.
    ScoreState fragments;
    for (std::uint64_t now=3000; now<=57000; now+=3000)
        ViolationScorer::Observe(fragments,Evaluation(now-3000,now),now,c);
    auto weakFragment = Evaluation(57000,58000);
    weakFragment.results[0].observed = 30; // 30 / 27.25 is below the single-family minimum.
    ViolationScorer::Observe(fragments,weakFragment,58000,c);
    ViolationScorer::Observe(fragments,Evaluation(58000,60000),60000,c);
    CHECK(EnforcementManager::Decide(fragments,{},60000,50,false,policy,c).action == Action::None);
    for (std::uint64_t now=63000; now<=120000; now+=3000)
        ViolationScorer::Observe(fragments,Evaluation(now-3000,now),now,c);
    CHECK(EnforcementManager::Decide(fragments,{},120000,50,false,policy,c).action == Action::Kick);
    auto qualification = fragments.episodes[0].qualifiedMs;
    auto freshness = fragments.episodes[0].lastAnomalyMs;
    ViolationScorer::Observe(fragments,Evaluation(120001,120001),120001,c);
    CHECK(fragments.episodes[0].qualifiedMs == qualification);
    CHECK(fragments.episodes[0].lastAnomalyMs == freshness);
    CHECK(EnforcementManager::Decide(fragments,{},120001,50,false,policy,c).action == Action::Kick);

    // Alerts require the configured number of current, uninterrupted windows.
    AlertConfig manyWindows;
    manyWindows.minWindows = 10;
    AlertState reminder;
    ViolationScorer::Tick(fragments,130000,c);
    for (std::uint64_t now=133000; now<=139000; now+=3000)
        ViolationScorer::Observe(fragments,Evaluation(now-3000,now),now,c);
    CHECK(!AlertPolicy::Evaluate(fragments,reminder,139000,EnforcementMode::Alert,manyWindows).emit);
}
