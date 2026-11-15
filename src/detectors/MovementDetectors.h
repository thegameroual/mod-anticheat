/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#pragma once

#include "../core/DetectorConfig.h"
#include "../core/DetectionContext.h"
#include "../core/DetectionResult.h"
#include "../core/MovementState.h"

namespace anticheat
{
bool ShouldSampleGround(DetectionContext const& context, MovementState const& state, DetectorConfig const& config);
bool ShouldSampleGeometry(DetectionContext const& context, MovementState const& state, DetectorConfig const& config);
DetectionBatch EvaluateMovement(DetectionContext const& context, MovementState& state, DetectorConfig const& config);
}
