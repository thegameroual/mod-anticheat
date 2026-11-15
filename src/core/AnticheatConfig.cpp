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
    range(scoring.cooldownMs, 1000, 300000, "Score.CooldownMs");
    range(scoring.rearmMs, 1000, 300000, "Score.RearmMs");
    range(scoring.globalCooldownMs, 250, 60000, "Score.GlobalCooldownMs");
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
    range(alertFrequencyMs, 1000, 3600000, "GM.AlertFrequencyMs");
    return errors;
}
}
