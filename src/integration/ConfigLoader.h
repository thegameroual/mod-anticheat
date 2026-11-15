/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once
#include "core/AnticheatConfig.h"

namespace anticheat
{
bool LoadAnticheatConfig(AnticheatConfig& candidate, std::vector<std::string>& errors);
}
