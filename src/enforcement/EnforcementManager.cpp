/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "EnforcementManager.h"
#include <algorithm>

namespace anticheat
{
namespace
{
std::uint32_t Family(DetectionType type)
{
    switch (type)
    {
        case DetectionType::Speed:
        case DetectionType::Teleport: return 1;
        case DetectionType::Fly:
        case DetectionType::Waterwalk: return 2;
        case DetectionType::IgnoreControl: return 4;
        // Geometry, jump/physics and timing are corroboration for human review.
        // Warden owns its own sanction and never feeds this policy.
        default: return 0;
    }
}

std::uint32_t Popcount(std::uint32_t mask)
{
    std::uint32_t count = 0;
    for (; mask; mask >>= 1)
        count += mask & 1;
    return count;
}
}

EnforcementDecision EnforcementManager::Decide(ScoreState const& state,
    EnforcementState const& enforcement, std::uint64_t now, std::uint32_t latencyMs,
    bool protectedTransition, EnforcementConfig const& config, ScoreConfig const& scoreConfig)
{
    EnforcementDecision decision;
    decision.score = state.score;
    if (config.mode != EnforcementMode::Automatic || enforcement.applied ||
        protectedTransition || latencyMs > config.maxLatencyMs)
        return decision;
    bool kickThreshold = config.kickScore > 0 && state.score >= config.kickScore;
    bool banThreshold = config.banScore > 0 && config.banSeconds > 0 && state.score >= config.banScore;
    if (!kickThreshold && !banThreshold)
        return decision;

    std::array<std::uint64_t, 64> episodes{};
    std::size_t episodeCount = 0;
    std::uint64_t first = now;
    std::uint64_t last = 0;
    for (std::size_t i = 0; i < state.evidenceSize; ++i)
    {
        auto const& evidence = state.evidence[i];
        std::uint32_t family = Family(evidence.type);
        if (!evidence.actionable || !family || evidence.time > now ||
            now - evidence.time > scoreConfig.evidenceWindowMs)
            continue;
        decision.familyMask |= family;
        first = std::min(first, evidence.time);
        last = std::max(last, evidence.time);
        // Several detector flags in the same movement observation are one
        // episode. Increasing packet frequency never increases this count.
        bool known = false;
        for (std::size_t j = 0; j < episodeCount; ++j)
            known = known || episodes[j] == evidence.time;
        if (!known)
            episodes[episodeCount++] = evidence.time;
    }
    decision.evidenceCount = static_cast<std::uint32_t>(episodeCount);
    decision.spanMs = last >= first ? last - first : 0;
    if (episodeCount < std::max(3u, config.minEvents) ||
        Popcount(decision.familyMask) < std::max(2u, config.minFamilies) ||
        decision.spanMs < std::max<std::uint64_t>(10000, config.minSpanMs))
        return decision;

    if (config.banScore > 0 && state.score >= config.banScore && config.banSeconds > 0)
        decision.action = Action::Ban;
    else if (config.kickScore > 0 && state.score >= config.kickScore)
        decision.action = Action::Kick;
    return decision;
}
}
