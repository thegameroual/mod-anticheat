/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "ReportQueue.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace anticheat
{
void ReportQueue::Configure(RepositoryConfig const& config)
{
    _config = config;
    _config.capacity = std::max<std::size_t>(1, _config.capacity);
    _config.reservedActions = std::min(_config.reservedActions, _config.capacity - 1);
    _config.batchSize = std::clamp<std::size_t>(_config.batchSize, 1, _config.capacity);
}

bool ReportQueue::Enqueue(ReportRecord record)
{
    bool action = record.action != Action::None;
    std::size_t limit = action ? _config.capacity : _config.capacity - _config.reservedActions;
    if (_pending.size() + _inFlight.size() >= limit || Index(record.type) >= DetectionCount ||
        static_cast<unsigned>(record.action) > static_cast<unsigned>(Action::Ban) ||
        static_cast<unsigned>(record.kind) > static_cast<unsigned>(ReportKind::Warden) ||
        !std::isfinite(record.x) || !std::isfinite(record.y) || !std::isfinite(record.z) ||
        !std::isfinite(record.confidence) || !std::isfinite(record.score) ||
        record.confidence < 0 || record.confidence > 1 || record.score < 0)
    {
        ++_metrics.rejected;
        if (action)
            ++_metrics.rejectedActions;
        return false;
    }
    // Bound storage before queueing, including externally supplied Warden text.
    record.name.resize(std::min<std::size_t>(record.name.size(), 48));
    record.details.resize(std::min<std::size_t>(record.details.size(), 1024));
    record.eventKey.resize(std::min<std::size_t>(record.eventKey.size(), 64));
    record.incidentKey.resize(std::min<std::size_t>(record.incidentKey.size(), 128));
    _pending.push_back(std::move(record));
    ++_metrics.accepted;
    return true;
}

std::vector<ReportRecord> const& ReportQueue::BeginBatch()
{
    if (!_inFlight.empty())
        return _inFlight;
    _inFlight.reserve(_config.batchSize);
    while (!_pending.empty() && _inFlight.size() < _config.batchSize)
    {
        _inFlight.push_back(std::move(_pending.front()));
        _pending.pop_front();
    }
    return _inFlight;
}

void ReportQueue::Complete(bool success)
{
    if (success)
    {
        _metrics.healthy = true;
        _metrics.committed += _inFlight.size();
        _inFlight.clear();
    }
    else
    {
        _metrics.healthy = false;
        ++_metrics.failedBatches;
    }
}

RepositoryMetrics ReportQueue::Metrics() const
{
    auto result = _metrics;
    result.queued = _pending.size();
    result.inFlight = _inFlight.size();
    return result;
}

std::vector<ReportRecord> ReportQueue::SnapshotAll() const
{
    std::vector<ReportRecord> result = _inFlight;
    result.insert(result.end(), _pending.begin(), _pending.end());
    return result;
}
}
