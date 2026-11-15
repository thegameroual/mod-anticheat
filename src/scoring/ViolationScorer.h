/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once
#include "core/DetectionResult.h"
#include <array>
#include <cstdint>

namespace anticheat
{
struct ScoreConfig
{
    std::uint64_t rearmMs = 10000;
    std::uint64_t evidenceWindowMs = 120000, confirmationWindowMs = 3000, maxObservationGapMs = 6000;
    std::uint64_t reportIntervalMs = 15000;
    float minConfidence = 0.8f, minObservationConfidence = 0.3f;
    float maxScorePerObservation = 20.0f, decayPerSecond = 1.0f;
    float persistencePerSecond = 2.0f, maxIncidentRisk = 200.0f;
    std::array<float, DetectionCount> weights = {5,20,15,5,5,5,10,0,10,0,0,0};
};
enum class IncidentPhase : std::uint8_t { Dormant, Open, Persistent, Interrupted, Resolved, Expired };
struct IncidentSnapshot
{
    std::uint64_t id = 0, firstMs = 0, lastMs = 0, coveredMs = 0;
    std::uint32_t abnormalWindows = 0, normalWindows = 0;
    float observed = 0, allowed = 0, severity = 0;
    IncidentPhase phase = IncidentPhase::Dormant;
    bool opened = false;
};
struct DetectorEpisode
{
    std::uint64_t id = 0, firstMs = 0, lastAnomalyMs = 0, lastEvaluationMs = 0;
    std::uint64_t coveredMs = 0, qualifiedMs = 0, pendingMs = 0, normalMs = 0, lastReportMs = 0;
    std::uint64_t streakMs = 0, qualifiedPendingMs = 0;
    std::uint32_t abnormalWindows = 0, normalWindows = 0, confirmedWindows = 0, streakWindows = 0;
    float observed = 0, allowed = 0, severity = 0, confidence = 0, risk = 0;
    float qualifiedPendingSeverity = 100;
    bool actionable = false, evaluated = false;
    IncidentPhase phase = IncidentPhase::Dormant;
};
struct SpeedWindow
{
    std::uint64_t endMs = 0;
    std::uint32_t durationMs = 0;
    float severity = 0;
};
struct ScoreState
{
    float score = 0;
    std::uint64_t totalAccepted = 0, nextIncident = 0;
    std::array<std::uint64_t, DetectionCount> counts{};
    std::uint64_t rawObservations = 0;
    double scoreSum = 0;
    std::uint64_t scoreSamples = 0;
    std::array<DetectorEpisode, DetectionCount> episodes{};
    std::array<SpeedWindow, 64> speedWindows{};
    std::size_t speedWindowSize = 0, speedWindowNext = 0;
    bool clockStarted = false;
    std::uint64_t lastDecay = 0;
};
struct ScoreUpdate
{
    std::array<DetectionResult, DetectionCount> accepted{};
    std::array<IncidentSnapshot, DetectionCount> incidents{};
    std::size_t count = 0;
};
constexpr unsigned EvidenceFamily(DetectionType type)
{
    return type == DetectionType::Speed || type == DetectionType::Teleport ? 1 :
        type == DetectionType::Fly || type == DetectionType::Waterwalk ? 2 :
        type == DetectionType::IgnoreControl ? 4 : 0;
}
class ViolationScorer
{
public:
    // Signal-only input has unknown coverage. It can open reports, never prove persistence or normality.
    static ScoreUpdate Observe(ScoreState&, DetectionResult const*, std::size_t, std::uint64_t, ScoreConfig const&);
    static ScoreUpdate Observe(ScoreState&, DetectionBatch const&, std::uint64_t, ScoreConfig const&);
    static void Decay(ScoreState&, std::uint64_t, ScoreConfig const&);
    static ScoreUpdate Tick(ScoreState&, std::uint64_t, ScoreConfig const&);
    static IncidentSnapshot Snapshot(DetectorEpisode const&);
};
}
