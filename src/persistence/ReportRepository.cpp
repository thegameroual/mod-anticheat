/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "ReportRepository.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Transaction.h"
#include <chrono>
#include <future>
#include <iomanip>
#include <optional>
#include <mutex>
#include <random>
#include <sstream>
#include <utility>

namespace anticheat
{
class ReportRepository::Impl
{
public:
    ReportQueue queue;
    mutable std::mutex mutex;
    std::optional<TransactionCallback> transaction;
    std::uint64_t retryAt = 0;
    std::uint64_t sequence = 0;
    std::string boot;
    std::uint64_t reportedRejected = 0;
    std::uint64_t lastRejectionLog = 0;

    Impl()
    {
        std::random_device random;
        std::ostringstream prefix;
        prefix << std::hex << std::setfill('0');
        for (unsigned i = 0; i < 4; ++i)
            prefix << std::setw(8) << random();
        boot = prefix.str();
    }

    void Start()
    {
        std::vector<ReportRecord> batch;
        {
            std::lock_guard<std::mutex> lock(mutex);
            batch = queue.BeginBatch();
        }
        if (batch.empty())
            return;
        auto tx = CharacterDatabase.BeginTransaction();
        for (auto const& report : batch)
            tx->Append(BuildReportInsert(report));
        transaction.emplace(CharacterDatabase.AsyncCommitTransaction(tx));
    }

    bool Finish()
    {
        bool success = false;
        try
        {
            success = transaction->m_future.get();
        }
        catch (std::future_error const& exception)
        {
            LOG_ERROR("module.anticheat", "Report transaction future failed: {}", exception.what());
        }
        transaction.reset();
        std::size_t remaining;
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.Complete(success);
            remaining = queue.Metrics().inFlight;
        }
        if (!success)
            LOG_ERROR("module.anticheat", "Report transaction failed; retaining {} events for retry",
                remaining);
        return success;
    }

    RepositoryConfig Config() const
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.Config();
    }
};

ReportRepository::ReportRepository() : _impl(std::make_unique<Impl>()) { }
ReportRepository::~ReportRepository() = default;
void ReportRepository::Configure(RepositoryConfig const& config)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    _impl->queue.Configure(config);
}
RepositoryMetrics ReportRepository::Metrics() const
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->queue.Metrics();
}

bool ReportRepository::Enqueue(ReportRecord record)
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    record.eventKey = _impl->boot + '-' + std::to_string(++_impl->sequence);
    if (!record.createdAt)
        record.createdAt = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    return _impl->queue.Enqueue(std::move(record));
}

void ReportRepository::Update(std::uint64_t now)
{
    auto metrics = Metrics();
    if (metrics.rejected != _impl->reportedRejected &&
        (!_impl->reportedRejected || now - _impl->lastRejectionLog >= 5000))
    {
        LOG_ERROR("module.anticheat", "Report queue overload/invalid input: {} rejected ({} actions), {} queued, {} in flight",
            metrics.rejected, metrics.rejectedActions, metrics.queued, metrics.inFlight);
        _impl->reportedRejected = metrics.rejected;
        _impl->lastRejectionLog = now;
    }
    if (_impl->transaction)
    {
        if (_impl->transaction->m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
        if (!_impl->Finish())
            _impl->retryAt = now + _impl->Config().retryMs;
    }
    metrics = Metrics();
    if (now >= _impl->retryAt && (metrics.queued || metrics.inFlight))
        _impl->Start();
}

bool ReportRepository::Shutdown()
{
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(_impl->Config().shutdownTimeoutMs);
    unsigned failures = 0;
    while (std::chrono::steady_clock::now() < deadline)
    {
        auto metrics = Metrics();
        if (!metrics.queued && !metrics.inFlight)
            return true;
        if (!_impl->transaction)
            _impl->Start();
        if (_impl->transaction->m_future.wait_until(deadline) != std::future_status::ready)
            break;
        if (!_impl->Finish() && ++failures >= 2)
            break;
    }
    auto metrics = Metrics();
    if (!metrics.queued && !metrics.inFlight)
        return true;
    LOG_ERROR("module.anticheat", "Shutdown flush incomplete: {} queued and {} unconfirmed events; database unavailable or deadline reached",
        metrics.queued, metrics.inFlight);
    // Preserve individual evidence in the server log when SQL cannot be
    // confirmed. The in-flight batch may already be committed; keys allow review.
    std::vector<ReportRecord> unconfirmed;
    {
        std::lock_guard<std::mutex> lock(_impl->mutex);
        unconfirmed = _impl->queue.SnapshotAll();
    }
    for (auto const& report : unconfirmed)
        LOG_ERROR("module.anticheat", "Unconfirmed report key={} guid={} account={} type={} action={} score={} details={}",
            report.eventKey, report.guid, report.account, Index(report.type),
            static_cast<unsigned>(report.action), report.score, report.details);
    return false;
}
}
