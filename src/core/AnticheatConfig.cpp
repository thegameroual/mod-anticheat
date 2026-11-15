/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "AnticheatConfig.h"
#include <cmath>

namespace anticheat
{
std::vector<std::string> AnticheatConfig::Validate() const
{
    std::vector<std::string> errors;
    auto range = [&](auto value, auto low, auto high, char const* name)
    {
        double number = static_cast<double>(value);
        if (!std::isfinite(number) || number < static_cast<double>(low) || number > static_cast<double>(high))
            errors.emplace_back(std::string(name) + " is outside its supported range");
    };
    range(detectors.speedTolerance, 1.0, 3.0, "SpeedHack.Tolerance");
    range(detectors.teleportDistance, 10, 500, "TeleportHack.DistanceThreshold");
    range(detectors.climbAngleDegrees, 45, 89.9, "ClimbHack.AngleThreshold");
    range(detectors.maxClockDriftMs, 500, 60000, "FakePing.MaxDesync");
    range(detectors.speedWindowMs, 1000, 10000, "Movement.SpeedWindowMs");
    range(detectors.maxSampleGapMs, 1000, 30000, "Movement.MaxSampleGapMs");
    range(detectors.transitionGraceMs, 250, 5000, "Movement.TransitionGraceMs");
    range(detectors.latencyCapMs, 0, 2000, "Movement.LatencyCapMs");
    range(detectors.geometryIntervalMs, 100, 10000, "Movement.GeometryIntervalMs");
    range(scoring.rearmMs, 1000, 300000, "Score.RearmMs");
    range(scoring.evidenceWindowMs, 10000, 3600000, "Score.EvidenceWindowMs");
    range(scoring.minConfidence, 0.5, 1.0, "Score.MinConfidence");
    range(scoring.minObservationConfidence, 0.0, 1.0, "Score.MinObservationConfidence");
    range(scoring.maxScorePerObservation, 1, 100, "Score.MaxPerObservation");
    range(scoring.decayPerSecond, 0, 100, "Score.DecayPerSecond");
    for (auto weight : scoring.weights)
        range(weight, 0, 1000, "Detection weight");
    if (scoring.rearmMs < 2ull * detectors.speedWindowMs + detectors.transitionGraceMs)
        errors.emplace_back("Score.RearmMs must cover two speed windows plus transition grace");
    if (scoring.minObservationConfidence > scoring.minConfidence)
        errors.emplace_back("Observation confidence cannot exceed scoring confidence");
    range(static_cast<unsigned>(enforcement.mode), 0, 2, "Mode");
    range(enforcement.kickScore, 0, 100000, "Enforcement.KickScore");
    range(enforcement.banScore, 0, 100000, "Enforcement.BanScore");
    range(enforcement.minEvents, 3, 64, "Enforcement.MinEvents");
    range(enforcement.minFamilies, 2, 3, "Enforcement.MinFamilies");
    range(enforcement.minSpanMs, 10000, 3600000, "Enforcement.MinSpanMs");
    range(enforcement.maxLatencyMs, 0, 5000, "Enforcement.MaxLatencyMs");
    range(enforcement.banSeconds, 1, 2592000, "Enforcement.BanSeconds");
    if (enforcement.minSpanMs >= scoring.evidenceWindowMs)
        errors.emplace_back("Enforcement.MinSpanMs must be shorter than Score.EvidenceWindowMs");
    if (enforcement.banScore > 0 && enforcement.kickScore > enforcement.banScore)
        errors.emplace_back("Enforcement.BanScore must be >= KickScore (or zero to disable bans)");
    range(repository.capacity, 128, 100000, "Persistence.Capacity");
    range(repository.reservedActions, 1, 10000, "Persistence.ReservedActions");
    range(repository.batchSize, 1, 1000, "Persistence.BatchSize");
    range(repository.retryMs, 100, 60000, "Persistence.RetryMs");
    range(repository.shutdownTimeoutMs, 1000, 60000, "Persistence.ShutdownTimeoutMs");
    if (repository.reservedActions >= repository.capacity || repository.batchSize > repository.capacity)
        errors.emplace_back("Persistence reserve/batch must fit inside capacity");
    range(gmLevel, 1, 3, "GM.AnnounceLevel");
    range(scoring.confirmationWindowMs, 1000, 10000, "Score.ConfirmationWindowMs");
    range(scoring.maxObservationGapMs, 1000, 30000, "Score.MaxObservationGapMs");
    range(scoring.reportIntervalMs, 3000, 300000, "Score.ReportIntervalMs");
    range(scoring.persistencePerSecond, 0.1, 10, "Score.PersistencePerSecond");
    range(scoring.maxIncidentRisk, 20, 1000, "Score.MaxIncidentRisk");
    range(alerts.minDurationMs, 3000, 120000, "Alert.MinDurationMs");
    range(alerts.repeatMs, 10000, 3600000, "Alert.RepeatMs");
    range(alerts.freshnessMs, 1000, 30000, "Alert.FreshnessMs");
    range(alerts.minWindows, 2, 100, "Alert.MinWindows");
    range(alerts.minSeverity, 1, 10, "Alert.MinSeverity");
    range(alerts.severeSeverity, 1.1, 100, "Alert.SevereSeverity");
    range(alerts.minConfidence, 0.3, 1, "Alert.MinConfidence");
    range(enforcement.singleMinDurationMs, 30000, 3600000, "Enforcement.SingleMinDurationMs");
    range(enforcement.singleMinWindows, 10, 64, "Enforcement.SingleMinWindows");
    range(enforcement.singleMinSeverity, 1.25, 10, "Enforcement.SingleMinSeverity");
    range(enforcement.freshnessMs, 1000, 30000, "Enforcement.FreshnessMs");
    range(wardenCooldownMs, 1000, 300000, "Warden.CooldownMs");
    if (scoring.confirmationWindowMs < detectors.speedWindowMs ||
        scoring.maxObservationGapMs < 2 * detectors.speedWindowMs ||
        scoring.maxObservationGapMs > scoring.evidenceWindowMs ||
        scoring.reportIntervalMs < scoring.confirmationWindowMs)
        errors.emplace_back("Observation/confirmation/report windows are inconsistent");
    if (enforcement.singleMinDurationMs >= scoring.evidenceWindowMs ||
        enforcement.singleMinDurationMs > 64 * scoring.confirmationWindowMs ||
        enforcement.singleMinWindows * scoring.confirmationWindowMs > scoring.evidenceWindowMs ||
        enforcement.minEvents * scoring.confirmationWindowMs > scoring.evidenceWindowMs ||
        alerts.minWindows * scoring.confirmationWindowMs > scoring.evidenceWindowMs ||
        alerts.minDurationMs > scoring.evidenceWindowMs ||
        alerts.freshnessMs > scoring.maxObservationGapMs || enforcement.freshnessMs > scoring.maxObservationGapMs ||
        alerts.severeSeverity < alerts.minSeverity)
        errors.emplace_back("Alert/enforcement evidence requirements exceed observation bounds");
    if (enforcement.kickScore > 3 * scoring.maxIncidentRisk ||
        enforcement.banScore > 3 * scoring.maxIncidentRisk ||
        (enforcement.singleFamilyEnabled && enforcement.kickScore > scoring.maxIncidentRisk) ||
        (enforcement.singleFamilyBan && (!enforcement.singleFamilyEnabled ||
            enforcement.banScore > scoring.maxIncidentRisk)))
        errors.emplace_back("Configured sanction thresholds are unreachable within bounded family risk");
    return errors;
}
}
