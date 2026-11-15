/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "ViolationScorer.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace anticheat
{
namespace
{
void Increment(std::uint64_t& value)
{
    if (value != std::numeric_limits<std::uint64_t>::max())
        ++value;
}

bool ActionableType(DetectionType type)
{
    return type == DetectionType::Speed || type == DetectionType::Teleport ||
        type == DetectionType::Fly || type == DetectionType::Waterwalk ||
        type == DetectionType::IgnoreControl;
}
}

void ViolationScorer::Decay(ScoreState& state, std::uint64_t now, ScoreConfig const& config)
{
    if (state.clockStarted && now >= state.lastDecay)
    {
        double decay = static_cast<double>(now - state.lastDecay) * config.decayPerSecond / 1000.0;
        state.score = static_cast<float>(std::max(0.0, static_cast<double>(state.score) - decay));
    }
    state.clockStarted = true;
    state.lastDecay = now;
}

ScoreUpdate ViolationScorer::Observe(ScoreState& state, DetectionResult const* results,
    std::size_t count, std::uint64_t now, ScoreConfig const& config)
{
    Decay(state, now, config);
    ScoreUpdate update;
    std::array<DetectionResult const*, DetectionCount> present{};
    for (std::size_t i = 0; i < count; ++i)
    {
        auto const& result = results[i];
        if (Index(result.type) >= DetectionCount || !std::isfinite(result.confidence) ||
            result.confidence < config.minObservationConfidence || result.confidence > 1.0f)
            continue;
        Increment(state.rawObservations);
        auto*& selected = present[Index(result.type)];
        if (!selected || result.confidence > selected->confidence)
            selected = &result;
    }

    bool canAccept = !state.globalAccepted ||
        (now >= state.lastGlobalAccepted && now - state.lastGlobalAccepted >= config.globalCooldownMs);
    bool scored = false;
    float remainingScore = std::max(0.0f, config.maxScorePerObservation);
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        auto& episode = state.episodes[i];
        if (!present[i])
        {
            if (episode.active && !episode.cleaning)
            {
                episode.cleaning = true;
                episode.cleanSince = now;
            }
            continue;
        }

        if (episode.cleaning && now >= episode.cleanSince && now - episode.cleanSince >= config.rearmMs)
            episode.active = false;
        episode.cleaning = false;
        if (episode.active)
            continue;
        // Suppressed incidents are consumed too: a continuous incident cannot
        // become a new event just because a cooldown elapsed.
        episode.active = true;
        auto result = *present[i];
        result.actionable = result.actionable && result.confidence >= config.minConfidence && ActionableType(result.type);
        // Keep a new detector's evidence during the shared score cooldown, but
        // do not let that packet contribute score or an enforcement episode.
        if (!canAccept)
            result.actionable = false;
        if (episode.accepted &&
            (now < episode.lastAccepted || now - episode.lastAccepted < config.cooldownMs))
            continue;

        episode.accepted = true;
        episode.lastAccepted = now;
        update.accepted[update.count++] = result;
        float weight = config.weights[i];
        if (!std::isfinite(weight) || weight < 0)
            weight = 0;
        // Uncertain geometry, clocks and Warden events are retained but must not
        // contribute to a movement sanction, even with an accidental weight.
        if (result.actionable)
        {
            float added = std::min(remainingScore, weight * result.confidence);
            remainingScore -= added;
            state.score = std::min(1000000.0f, state.score + added);
            scored = true;
        }
        Increment(state.totalAccepted);
        Increment(state.counts[i]);
        state.evidence[state.evidenceNext] = {now, result.type, result.actionable};
        state.evidenceNext = (state.evidenceNext + 1) % state.evidence.size();
        state.evidenceSize = std::min(state.evidenceSize + 1, state.evidence.size());
    }
    if (scored)
    {
        state.globalAccepted = true;
        state.lastGlobalAccepted = now;
    }
    // The integration records the final score of the complete observation for
    // every accepted result; use the same definition for the live sample mean.
    state.scoreSum += static_cast<double>(state.score) * update.count;
    for (std::size_t i = 0; i < update.count; ++i)
        Increment(state.scoreSamples);
    return update;
}
}
