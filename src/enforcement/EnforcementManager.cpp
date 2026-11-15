/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "EnforcementManager.h"
#include <algorithm>

namespace anticheat
{
EnforcementDecision EnforcementManager::Decide(ScoreState const& s, EnforcementState const& latch,
    std::uint64_t now, std::uint32_t latency, bool protectedTransition,
    EnforcementConfig const& c, ScoreConfig const& scoring)
{
    EnforcementDecision d;
    d.score = s.score;
    if (c.mode != EnforcementMode::Automatic || latch.applied || protectedTransition || latency > c.maxLatencyMs)
        return d;
    unsigned families = 0;
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        auto const& e = s.episodes[i];
        auto family = EvidenceFamily(static_cast<DetectionType>(i));
        if (!family || !e.actionable || !e.evaluated || e.phase != IncidentPhase::Persistent || now < e.lastAnomalyMs ||
            now - e.lastAnomalyMs > std::min(c.freshnessMs, scoring.maxObservationGapMs)) continue;
        if (e.confirmedWindows >= std::max(3u, c.minEvents) &&
            e.qualifiedMs >= std::max<std::uint64_t>(10000, c.minSpanMs))
        {
            families |= family;
            d.evidenceCount = std::max(d.evidenceCount, e.confirmedWindows);
            d.spanMs = d.spanMs ? std::min(d.spanMs, e.qualifiedMs) : e.qualifiedMs;
        }
    }
    d.familyMask = families;
    unsigned count = 0;
    for (auto bits = families; bits; bits >>= 1) count += bits & 1;
    bool multi = count >= std::max(2u, c.minFamilies);
    // Only speed exposes a measured integrated travel budget suitable for this route.
    auto const& speed = s.episodes[Index(DetectionType::Speed)];
    std::uint64_t strongDuration = 0;
    std::uint32_t strongWindows = 0;
    float strongSeverity = 100;
    std::uint64_t expectedEnd = 0;
    float const requiredSeverity = std::max(1.25f, c.singleMinSeverity);
    for (std::size_t offset = 0; offset < s.speedWindowSize; ++offset)
    {
        auto index = (s.speedWindowNext + s.speedWindows.size() - 1 - offset) % s.speedWindows.size();
        auto const& window = s.speedWindows[index];
        if (!window.durationMs || window.endMs > now || now - window.endMs > scoring.evidenceWindowMs ||
            window.severity < requiredSeverity || (expectedEnd && window.endMs != expectedEnd))
            break;
        strongDuration += window.durationMs;
        ++strongWindows;
        strongSeverity = std::min(strongSeverity, window.severity);
        expectedEnd = window.endMs - window.durationMs;
    }
    bool single = c.singleFamilyEnabled && speed.actionable && speed.evaluated && speed.phase == IncidentPhase::Persistent &&
        now >= speed.lastAnomalyMs && now - speed.lastAnomalyMs <= std::min(c.freshnessMs, scoring.maxObservationGapMs) &&
        strongDuration >= std::max<std::uint64_t>(30000, c.singleMinDurationMs) &&
        strongWindows >= std::max(10u, c.singleMinWindows) &&
        speed.severity >= requiredSeverity && speed.qualifiedPendingSeverity >= requiredSeverity;
    if (!multi && !single) return d;
    if (!multi)
    {
        d.singleFamily = true;
        d.incidentId = speed.id;
        d.severity = strongSeverity;
        d.spanMs = strongDuration;
        d.evidenceCount = strongWindows;
        d.familyMask = 1;
    }
    if (c.banScore > 0 && s.score >= c.banScore && c.banSeconds > 0 && c.banSeconds <= 2592000 &&
        (multi || c.singleFamilyBan)) d.action = Action::Ban;
    else if (c.kickScore > 0 && s.score >= c.kickScore) d.action = Action::Kick;
    return d;
}

AlertDecision AlertPolicy::Evaluate(ScoreState const& s, AlertState& sent, std::uint64_t now,
    EnforcementMode mode, AlertConfig const& c)
{
    if (mode == EnforcementMode::Observation) return {};
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        auto const& e = s.episodes[i];
        if (!e.id || e.phase != IncidentPhase::Persistent || !e.evaluated || e.confidence < c.minConfidence ||
            now < e.lastAnomalyMs || now - e.lastAnomalyMs > c.freshnessMs) continue;
        bool persistent = e.streakWindows >= c.minWindows && e.streakMs >= c.minDurationMs &&
            (e.allowed <= 0 || e.severity >= c.minSeverity);
        bool severe = e.streakMs >= 6000 && e.severity >= c.severeSeverity;
        if (!persistent && !severe) continue;
        if (sent.incident[i] == e.id && (now < sent.lastAlert[i] || now - sent.lastAlert[i] < c.repeatMs)) continue;
        sent.incident[i] = e.id;
        sent.lastAlert[i] = now;
        sent.severity[i] = e.severity;
        return {true, static_cast<DetectionType>(i), ViolationScorer::Snapshot(e),
            severe ? "large repeated measured anomaly" : "persistent evaluated anomaly"};
    }
    return {};
}
}
