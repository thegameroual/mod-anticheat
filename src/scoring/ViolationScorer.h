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
    std::uint64_t cooldownMs = 10000;
    std::uint64_t rearmMs = 10000;
    std::uint64_t globalCooldownMs = 1000;
    std::uint64_t evidenceWindowMs = 120000;
    float minConfidence = 0.8f;
    float minObservationConfidence = 0.3f;
    float maxScorePerObservation = 20.0f;
    float decayPerSecond = 1.0f;
    std::array<float, DetectionCount> weights = {5, 20, 15, 5, 5, 5, 10, 0, 10, 0, 0, 0};
};

struct DetectorEpisode
{
    bool active = false;
    bool cleaning = false;
    bool accepted = false;
    std::uint64_t cleanSince = 0;
    std::uint64_t lastAccepted = 0;
};

struct Evidence
{
    std::uint64_t time = 0;
    DetectionType type = DetectionType::Speed;
    bool actionable = false;
};

struct ScoreState
{
    float score = 0;
    std::uint64_t totalAccepted = 0;
    std::array<std::uint64_t, DetectionCount> counts{};
    std::uint64_t rawObservations = 0;
    double scoreSum = 0;
    std::uint64_t scoreSamples = 0;
    std::array<DetectorEpisode, DetectionCount> episodes{};
    std::array<Evidence, 64> evidence{};
    std::size_t evidenceSize = 0;
    std::size_t evidenceNext = 0;
    bool clockStarted = false;
    std::uint64_t lastDecay = 0;
    bool globalAccepted = false;
    std::uint64_t lastGlobalAccepted = 0;
};

struct ScoreUpdate
{
    std::array<DetectionResult, DetectionCount> accepted{};
    std::size_t count = 0;
};

class ViolationScorer
{
public:
    static ScoreUpdate Observe(ScoreState& state, DetectionResult const* results,
        std::size_t count, std::uint64_t now, ScoreConfig const& config);
    static ScoreUpdate Observe(ScoreState& state, DetectionBatch const& batch,
        std::uint64_t now, ScoreConfig const& config)
    {
        return Observe(state, batch.results.data(), batch.size, now, config);
    }
    static void Decay(ScoreState& state, std::uint64_t now, ScoreConfig const& config);
};
}
