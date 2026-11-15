/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once

#include "core/DetectionResult.h"
#include "enforcement/EnforcementManager.h"
#include <deque>
#include <string>
#include <vector>

namespace anticheat
{
enum class ReportKind : std::uint8_t { Opened = 0, Lifecycle = 1, Alert = 2, Decision = 3, Warden = 4 };

struct RepositoryConfig
{
    std::size_t capacity = 4096;
    std::size_t reservedActions = 128;
    std::size_t batchSize = 128;
    std::uint64_t retryMs = 5000;
    std::uint32_t shutdownTimeoutMs = 30000;
};

struct ReportRecord
{
    std::uint32_t guid = 0;
    std::uint32_t account = 0;
    std::uint32_t mapId = 0;
    std::string name;
    DetectionType type = DetectionType::Speed;
    float x = 0;
    float y = 0;
    float z = 0;
    float confidence = 0;
    float score = 0;
    bool actionable = false;
    std::string details;
    Action action = Action::None;
    // Assigned by the repository once. Retries keep the same idempotency key.
    std::string eventKey;
    // Stable within an incident, unlike the event key of each lifecycle snapshot.
    std::string incidentKey;
    ReportKind kind = ReportKind::Opened;
    std::uint64_t createdAt = 0;
};

struct RepositoryMetrics
{
    std::uint64_t accepted = 0;
    std::uint64_t committed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t rejectedActions = 0;
    std::uint64_t failedBatches = 0;
    bool healthy = true;
    std::size_t queued = 0;
    std::size_t inFlight = 0;
};

// Single world-thread owner. The manager passes only detached values here.
class ReportQueue
{
public:
    void Configure(RepositoryConfig const& config);
    bool Enqueue(ReportRecord record);
    std::vector<ReportRecord> const& BeginBatch();
    void Complete(bool success);
    RepositoryMetrics Metrics() const;
    std::vector<ReportRecord> SnapshotAll() const;
    bool Empty() const { return _pending.empty() && _inFlight.empty(); }
    RepositoryConfig const& Config() const { return _config; }

private:
    RepositoryConfig _config;
    RepositoryMetrics _metrics;
    std::deque<ReportRecord> _pending;
    std::vector<ReportRecord> _inFlight;
};

// The query has a fixed shape; all text is a UTF-8 hex literal and all numeric
// values are validated. It is independent of SQL backslash-escape modes.
std::string BuildReportInsert(ReportRecord const& record);
std::string SqlText(std::string const& value, std::size_t maxBytes);
}
