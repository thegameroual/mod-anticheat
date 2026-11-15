/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once

#include "scoring/ViolationScorer.h"

namespace anticheat
{
enum class EnforcementMode : std::uint8_t { Observation = 0, Alert = 1, Automatic = 2 };
enum class Action : std::uint8_t { None = 0, Kick = 1, Ban = 2 };

struct EnforcementConfig
{
    EnforcementMode mode = EnforcementMode::Observation;
    float kickScore = 100;
    float banScore = 150;
    std::uint32_t minEvents = 5;
    std::uint32_t minFamilies = 2;
    std::uint64_t minSpanMs = 30000;
    std::uint32_t maxLatencyMs = 750;
    std::uint32_t banSeconds = 86400;
};

struct EnforcementState { bool applied = false; };

struct EnforcementDecision
{
    Action action = Action::None;
    float score = 0;
    std::uint32_t evidenceCount = 0;
    std::uint32_t familyMask = 0;
    std::uint64_t spanMs = 0;
};

class EnforcementManager
{
public:
    // The caller marks applied only after successfully recording/queuing the
    // action. This method is a pure policy decision, with no Player or SQL access.
    static EnforcementDecision Decide(ScoreState const& state, EnforcementState const& enforcement,
        std::uint64_t now, std::uint32_t latencyMs, bool protectedTransition,
        EnforcementConfig const& config, ScoreConfig const& scoreConfig);
};
}
