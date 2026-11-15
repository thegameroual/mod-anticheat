/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once

#include "ReportQueue.h"
#include <memory>

namespace anticheat
{
class ReportRepository
{
public:
    ReportRepository();
    ~ReportRepository();
    ReportRepository(ReportRepository const&) = delete;
    ReportRepository& operator=(ReportRepository const&) = delete;
    void Configure(RepositoryConfig const& config);
    bool Enqueue(ReportRecord record);
    void Update(std::uint64_t now);
    // Call before CharacterDatabase closes. False means records could not be
    // confirmed within the configured deadline; they are logged explicitly.
    bool Shutdown();
    RepositoryMetrics Metrics() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};
}
