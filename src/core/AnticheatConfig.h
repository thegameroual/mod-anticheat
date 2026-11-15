/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once

#include "DetectorConfig.h"
#include "enforcement/EnforcementManager.h"
#include "persistence/ReportQueue.h"
#include <string>
#include <vector>

namespace anticheat
{
struct AnticheatConfig
{
    bool enabled = true;
    DetectorConfig detectors;
    ScoreConfig scoring;
    EnforcementConfig enforcement;
    AlertConfig alerts;
    RepositoryConfig repository;
    std::uint32_t gmLevel = 1;
    std::uint64_t wardenCooldownMs = 10000;
    bool loginMessage = true;
    bool instrumentation = false;

    std::vector<std::string> Validate() const;
};
}
