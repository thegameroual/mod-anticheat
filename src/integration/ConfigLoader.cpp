/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "ConfigLoader.h"
#include "core/ConfigValue.h"
#include "Config.h"
#include <iomanip>
#include <limits>

namespace anticheat
{
bool LoadAnticheatConfig(AnticheatConfig& c, std::vector<std::string>& errors)
{
    errors.clear();
    auto Read = [&](char const* key, auto& value)
    {
        std::string fullKey = std::string("Anticheat.") + key;
        std::ostringstream fallback;
        fallback.imbue(std::locale::classic());
        fallback << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
        auto text = sConfigMgr->GetOption<std::string>(fullKey, fallback.str());
        if (!ParseConfigValue(text, value))
            errors.emplace_back(fullKey + " contains a malformed value");
    };
    c = AnticheatConfig{};
    Read("Enable", c.enabled);
    unsigned mode = static_cast<unsigned>(c.enforcement.mode);
    Read("Mode", mode);
    // Validate before narrowing: 256 must not wrap into observation mode.
    if (mode > 2)
    {
        errors.emplace_back("Anticheat.Mode must be 0 (observation), 1 (alerts), or 2 (automatic)");
        return false;
    }
    c.enforcement.mode = static_cast<EnforcementMode>(mode);
    Read("GM.AnnounceLevel", c.gmLevel);
    Read("GM.AlertFrequencyMs", c.alertFrequencyMs);
    Read("LoginMessage", c.loginMessage);
    Read("Instrumentation", c.instrumentation);
    constexpr char const* toggles[] = {"DetectSpeedHack", "DetectFlyHack", "DetectTeleportHack", "DetectClimbHack",
        "DetectJumpHack", "DetectGravityHack", "DetectWaterwalkHack", "DetectNofallHack", "DetectIgnoreControl",
        "DetectFakePing", "DetectNoClipHack", "DetectWarden"};
    constexpr char const* weights[] = {"SpeedHack", "FlyHack", "Teleport", "Climb", "Jump", "Gravity",
        "Waterwalk", "Nofall", "IgnoreControl", "FakePing", "NoClip", "Warden"};
    for (std::size_t i = 0; i < DetectionCount; ++i)
    {
        Read(toggles[i], c.detectors.enabled[i]);
        auto key = std::string("Weight.") + weights[i];
        Read(key.c_str(), c.scoring.weights[i]);
    }
    Read("SpeedHack.Tolerance", c.detectors.speedTolerance);
    Read("TeleportHack.DistanceThreshold", c.detectors.teleportDistance);
    Read("ClimbHack.AngleThreshold", c.detectors.climbAngleDegrees);
    Read("FakePing.MaxDesync", c.detectors.maxClockDriftMs);
    Read("Movement.SpeedWindowMs", c.detectors.speedWindowMs);
    Read("Movement.MaxSampleGapMs", c.detectors.maxSampleGapMs);
    Read("Movement.TransitionGraceMs", c.detectors.transitionGraceMs);
    Read("Movement.LatencyCapMs", c.detectors.latencyCapMs);
    Read("Movement.GeometryIntervalMs", c.detectors.geometryIntervalMs);
    Read("Score.CooldownMs", c.scoring.cooldownMs);
    Read("Score.RearmMs", c.scoring.rearmMs);
    Read("Score.GlobalCooldownMs", c.scoring.globalCooldownMs);
    Read("Score.EvidenceWindowMs", c.scoring.evidenceWindowMs);
    Read("Score.MinConfidence", c.scoring.minConfidence);
    Read("Score.MinObservationConfidence", c.scoring.minObservationConfidence);
    Read("Score.MaxPerObservation", c.scoring.maxScorePerObservation);
    Read("Score.DecayPerSecond", c.scoring.decayPerSecond);
    Read("Enforcement.KickScore", c.enforcement.kickScore);
    Read("Enforcement.BanScore", c.enforcement.banScore);
    Read("Enforcement.MinEvents", c.enforcement.minEvents);
    Read("Enforcement.MinFamilies", c.enforcement.minFamilies);
    Read("Enforcement.MinSpanMs", c.enforcement.minSpanMs);
    Read("Enforcement.MaxLatencyMs", c.enforcement.maxLatencyMs);
    Read("Enforcement.BanSeconds", c.enforcement.banSeconds);
    Read("Persistence.Capacity", c.repository.capacity);
    Read("Persistence.ReservedActions", c.repository.reservedActions);
    Read("Persistence.BatchSize", c.repository.batchSize);
    Read("Persistence.RetryMs", c.repository.retryMs);
    Read("Persistence.ShutdownTimeoutMs", c.repository.shutdownTimeoutMs);
    auto ranges = c.Validate();
    errors.insert(errors.end(), ranges.begin(), ranges.end());
    return errors.empty();
}
}
