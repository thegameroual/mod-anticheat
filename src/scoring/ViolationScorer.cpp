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
void Recalculate(ScoreState& s)
{
    std::array<float, 3> families{};
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        auto family = EvidenceFamily(static_cast<DetectionType>(i));
        if (family) families[family == 1 ? 0 : family == 2 ? 1 : 2] =
            std::max(families[family == 1 ? 0 : family == 2 ? 1 : 2], s.episodes[i].risk);
    }
    s.score = families[0] + families[1] + families[2];
}
void Emit(ScoreUpdate& update, DetectorEpisode& e, DetectionType type, std::uint64_t now,
    char const* reason, bool opened = false)
{
    if (update.count == DetectionCount) return;
    auto index = update.count++;
    update.accepted[index] = {type, e.confidence, e.actionable, e.observed, e.allowed, reason};
    update.incidents[index] = ViolationScorer::Snapshot(e);
    update.incidents[index].opened = opened;
    e.lastReportMs = now;
}
void ClearQualification(ScoreState& s, std::size_t index)
{
    auto& e = s.episodes[index];
    e.qualifiedMs = e.qualifiedPendingMs = 0;
    e.confirmedWindows = 0;
    e.qualifiedPendingSeverity = 100;
    e.actionable = false;
    if (index == Index(DetectionType::Speed))
        s.speedWindowSize = s.speedWindowNext = 0;
}
void Interrupt(ScoreState& s, std::size_t index)
{
    auto& e = s.episodes[index];
    ClearQualification(s, index);
    e.pendingMs = e.normalMs = e.streakMs = 0;
    e.streakWindows = 0;
    e.evaluated = false;
    e.actionable = false;
    if (e.id && e.phase != IncidentPhase::Resolved && e.phase != IncidentPhase::Expired)
        e.phase = IncidentPhase::Interrupted;
}
}
IncidentSnapshot ViolationScorer::Snapshot(DetectorEpisode const& e)
{
    return {e.id, e.firstMs, e.lastAnomalyMs, e.coveredMs, e.abnormalWindows,
        e.normalWindows, e.observed, e.allowed, e.severity, e.phase, false};
}
void ViolationScorer::Decay(ScoreState& state, std::uint64_t now, ScoreConfig const& config)
{
    double decay = state.clockStarted && now >= state.lastDecay ?
        static_cast<double>(now - state.lastDecay) * config.decayPerSecond / 1000.0 : 0;
    for (auto& episode : state.episodes)
        episode.risk = static_cast<float>(std::max(0.0, episode.risk - decay));
    state.clockStarted = true;
    state.lastDecay = now;
    Recalculate(state);
}
ScoreUpdate ViolationScorer::Tick(ScoreState& s, std::uint64_t now, ScoreConfig const& c)
{
    Decay(s, now, c);
    ScoreUpdate out;
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        auto& e = s.episodes[i];
        if (!e.id || e.phase == IncidentPhase::Resolved || e.phase == IncidentPhase::Expired) continue;
        auto before = e.phase;
        if (now < e.lastEvaluationMs || now - e.lastEvaluationMs > c.maxObservationGapMs) Interrupt(s, i);
        if (now >= e.lastAnomalyMs && now - e.lastAnomalyMs > c.evidenceWindowMs)
            e.phase = IncidentPhase::Expired;
        if (before != e.phase) Emit(out, e, static_cast<DetectionType>(i), now, "observation interrupted or expired");
    }
    return out;
}
ScoreUpdate ViolationScorer::Observe(ScoreState& s, DetectionResult const* results,
    std::size_t count, std::uint64_t now, ScoreConfig const& c)
{
    DetectionBatch batch;
    for (std::size_t i = 0; i < count; ++i) batch.Add(results[i]);
    return Observe(s, batch, now, c);
}
ScoreUpdate ViolationScorer::Observe(ScoreState& s, DetectionBatch const& batch,
    std::uint64_t now, ScoreConfig const& c)
{
    Decay(s, now, c);
    ScoreUpdate out;
    std::array<DetectionResult const*, DetectionCount> found{};
    for (std::size_t i = 0; i < batch.size; ++i)
    {
        auto const& r = batch.results[i];
        if (Index(r.type) >= DetectionCount || !std::isfinite(r.confidence) ||
            r.confidence < c.minObservationConfidence || r.confidence > 1 ||
            !std::isfinite(r.observed) || !std::isfinite(r.allowed)) continue;
        if (s.rawObservations != std::numeric_limits<std::uint64_t>::max()) ++s.rawObservations;
        if (!found[Index(r.type)] || r.confidence > found[Index(r.type)]->confidence) found[Index(r.type)] = &r;
    }
    float initialRemaining = c.maxScorePerObservation;
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        auto& e = s.episodes[i];
        auto type = static_cast<DetectionType>(i);
        auto const& v = batch.evaluations[i];
        auto r = found[i];
        auto before = e.phase;
        // A same-time packet supplies no new time or data freshness. Preserve
        // existing qualification so a client cannot reset it with zero-time bursts.
        bool const zeroInterval = v.status != EvaluationStatus::NotEvaluated &&
            v.status != EvaluationStatus::Exempt && v.beginMs == now && v.endMs == now;
        if (zeroInterval && e.id && EvidenceFamily(type))
            continue;
        bool gap = e.evaluated && (now < e.lastEvaluationMs || now - e.lastEvaluationMs > c.maxObservationGapMs);
        if (gap || v.status == EvaluationStatus::Exempt) Interrupt(s, i);
        bool valid = v.reliable && v.endMs == now && v.endMs > v.beginMs &&
            v.endMs - v.beginMs <= c.maxObservationGapMs;
        if (v.status != EvaluationStatus::NotEvaluated && !valid && !zeroInterval)
            Interrupt(s, i);
        auto begin = e.evaluated ? std::max(e.lastEvaluationMs, v.beginMs) : v.beginMs;
        auto duration = valid && v.endMs > begin ? v.endMs - begin : 0;
        if (e.evaluated && v.beginMs > e.lastEvaluationMs) { Interrupt(s, i); duration = 0; }
        if (r)
        {
            bool opening = !e.id || e.phase == IncidentPhase::Resolved || e.phase == IncidentPhase::Expired;
            if (opening)
            {
                float residual = e.risk;
                e = {};
                e.risk = residual;
                e.id = ++s.nextIncident;
                e.firstMs = now;
                e.phase = IncidentPhase::Open;
                ++s.totalAccepted;
                ++s.counts[i];
            }
            e.lastAnomalyMs = now;
            e.observed = r->observed;
            e.allowed = r->allowed;
            e.confidence = r->confidence;
            float severity = r->allowed > 0 ? r->observed / r->allowed : 1.0f;
            e.severity = std::clamp(severity, 0.0f, 100.0f);
            e.actionable = r->actionable && r->confidence >= c.minConfidence && EvidenceFamily(type) &&
                valid && v.status == EvaluationStatus::Anomalous;
            e.normalMs = 0;
            if (!e.actionable)
                ClearQualification(s, i);
            if (valid && v.status == EvaluationStatus::Anomalous)
            {
                e.streakMs = std::min(c.evidenceWindowMs, e.streakMs + duration);
                e.streakWindows = static_cast<std::uint32_t>(e.streakMs / c.confirmationWindowMs);
                e.pendingMs += duration;
                // Every bucket represents newly evaluated, non-overlapping time, never packet count.
                auto windows = e.pendingMs / c.confirmationWindowMs;
                e.pendingMs %= c.confirmationWindowMs;
                if (windows)
                {
                    auto covered = windows * c.confirmationWindowMs;
                    e.coveredMs = std::min(c.evidenceWindowMs, e.coveredMs + covered);
                    e.abnormalWindows = std::min<std::uint64_t>(1000000, e.abnormalWindows + windows);
                    e.phase = IncidentPhase::Persistent;
                }
                // Qualification has its own accumulator: diagnostic fragments
                // can never become actionable when a later fragment is reliable.
                if (e.actionable)
                {
                    auto remaining = duration;
                    while (remaining)
                    {
                        auto take = std::min(remaining, c.confirmationWindowMs - e.qualifiedPendingMs);
                        e.qualifiedMs = std::min(c.evidenceWindowMs, e.qualifiedMs + take);
                        e.qualifiedPendingMs += take;
                        e.qualifiedPendingSeverity = std::min(e.qualifiedPendingSeverity, e.severity);
                        remaining -= take;
                        if (e.qualifiedPendingMs == c.confirmationWindowMs)
                        {
                            if (type == DetectionType::Speed)
                            {
                                s.speedWindows[s.speedWindowNext] = {now - remaining,
                                    static_cast<std::uint32_t>(c.confirmationWindowMs), e.qualifiedPendingSeverity};
                                s.speedWindowNext = (s.speedWindowNext + 1) % s.speedWindows.size();
                                s.speedWindowSize = std::min(s.speedWindowSize + 1, s.speedWindows.size());
                            }
                            float floor = c.weights[i] * e.confidence + c.persistencePerSecond *
                                (static_cast<float>(e.qualifiedMs) / 1000) * std::min(e.qualifiedPendingSeverity, 2.0f);
                            e.risk = std::max(e.risk, std::min(c.maxIncidentRisk, floor));
                            e.qualifiedPendingMs = 0;
                            e.qualifiedPendingSeverity = 100;
                        }
                    }
                    e.confirmedWindows = static_cast<std::uint32_t>(e.qualifiedMs / c.confirmationWindowMs);
                }
            }
            else { ClearQualification(s, i); e.pendingMs = e.streakMs = 0; e.streakWindows = 0; }
            if (opening && e.actionable)
            {
                auto added = std::min(initialRemaining, c.weights[i] * e.confidence);
                initialRemaining -= added;
                e.risk = std::max(e.risk, added);
            }
            if (opening || now - e.lastReportMs >= c.reportIntervalMs)
                Emit(out, e, type, now, r->reason, opening);
        }
        else if (v.status == EvaluationStatus::Normal && valid)
        {
            ClearQualification(s, i);
            e.pendingMs = e.streakMs = 0;
            e.streakWindows = 0;
            auto previousNormal = e.normalMs / c.confirmationWindowMs;
            e.normalMs = std::min(c.rearmMs, e.normalMs + duration);
            e.normalWindows = static_cast<std::uint32_t>(std::min<std::uint64_t>(1000000,
                e.normalWindows + e.normalMs / c.confirmationWindowMs - previousNormal));
            if (e.id && e.normalMs >= c.rearmMs) e.phase = IncidentPhase::Resolved;
        }
        else if (v.status == EvaluationStatus::Anomalous && !r)
            Interrupt(s, i); // Missing/invalid confidence is not retained as reliable evidence.
        if (v.status != EvaluationStatus::NotEvaluated)
        {
            e.lastEvaluationMs = now;
            e.evaluated = valid;
        }
        if (!r && e.phase != before && e.id) Emit(out, e, type, now, "incident observation state changed");
    }
    Recalculate(s);
    for (std::size_t i = 0; i < out.count; ++i)
        if (out.incidents[i].opened)
        {
            s.scoreSum += s.score;
            ++s.scoreSamples;
        }
    return out;
}
}
