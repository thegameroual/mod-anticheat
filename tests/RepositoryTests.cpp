#include "TestHarness.h"
#include "persistence/ReportQueue.h"
#include <limits>
#include <locale>

using namespace anticheat;

namespace
{
ReportRecord Record(unsigned id, Action action = Action::None)
{
    ReportRecord record;
    record.guid = 1;
    record.account = 2;
    record.name = "Tester";
    record.details = "bounded evidence";
    record.eventKey = "test-" + std::to_string(id);
    record.action = action;
    record.createdAt = 1730000000;
    return record;
}

struct CommaDecimal : std::numpunct<char>
{
    char do_decimal_point() const override { return ','; }
};
}

void RunRepositoryTests()
{
    ReportQueue queue;
    RepositoryConfig config;
    config.capacity = 4;
    config.reservedActions = 1;
    config.batchSize = 2;
    queue.Configure(config);
    CHECK(queue.Enqueue(Record(1)));
    CHECK(queue.Enqueue(Record(2)));
    CHECK(queue.Enqueue(Record(3)));
    CHECK(!queue.Enqueue(Record(4)));
    CHECK(queue.Enqueue(Record(5, Action::Kick)));
    CHECK(!queue.Enqueue(Record(6, Action::Ban)));
    CHECK(queue.Metrics().rejected == 2);
    CHECK(queue.Metrics().rejectedActions == 1);
    auto first = queue.BeginBatch();
    CHECK(first.size() == 2);
    CHECK(queue.Metrics().queued == 2 && queue.Metrics().inFlight == 2);
    CHECK(!queue.Enqueue(Record(7, Action::Kick)));
    queue.Complete(false);
    CHECK(queue.Metrics().failedBatches == 1);
    CHECK(!queue.Metrics().healthy);
    CHECK(queue.BeginBatch()[0].eventKey == first[0].eventKey);
    CHECK(queue.Metrics().committed == 0);
    queue.Complete(true);
    CHECK(queue.Metrics().committed == 2);
    CHECK(queue.Metrics().healthy);
    CHECK(queue.BeginBatch().size() == 2);
    queue.Complete(true);
    CHECK(queue.Empty());
    CHECK(queue.Metrics().committed == 4);
    CHECK(queue.Enqueue(Record(8)));
    CHECK(queue.SnapshotAll().size() == 1);

    // Shrinking configuration never discards already accepted reports.
    config.capacity = 1;
    queue.Configure(config);
    CHECK(!queue.Enqueue(Record(9, Action::Kick)));
    CHECK(queue.BeginBatch()[0].eventKey == "test-8");
    queue.Complete(true);
    auto invalid = Record(10);
    invalid.x = std::numeric_limits<float>::infinity();
    CHECK(!queue.Enqueue(invalid));
    CHECK(queue.Empty());

    CHECK(SqlText("O'Brien\\\n", 100) == "CONVERT(X'4f27427269656e5c0a' USING utf8mb4)");
    CHECK(SqlText("", 100) == "CONVERT(X'' USING utf8mb4)");
    CHECK(SqlText("abc", 2) == "CONVERT(X'6162' USING utf8mb4)");
    CHECK(SqlText(std::string("\xc3\xa9"), 1) == "CONVERT(X'' USING utf8mb4)");
    CHECK(SqlText(std::string("\xc3\xa9"), 2) == "CONVERT(X'c3a9' USING utf8mb4)");
    CHECK(SqlText(std::string("\xff"), 10) == "CONVERT(X'3f' USING utf8mb4)");
    CHECK(SqlText(std::string("\xed\xa0\x80"), 10) == "CONVERT(X'3f3f3f' USING utf8mb4)");

    auto report = Record(11);
    report.name = "'); DROP TABLE test; --";
    report.details = "'; UPDATE x SET y=1; --\\";
    report.x = 1.5;
    auto oldLocale = std::locale();
    std::locale::global(std::locale(oldLocale, new CommaDecimal));
    auto query = BuildReportInsert(report);
    std::locale::global(oldLocale);
    CHECK(query.find("DROP TABLE") == std::string::npos);
    CHECK(query.find("UPDATE x") == std::string::npos);
    CHECK(query.find(",1.5,") != std::string::npos);
    CHECK(query.find("ON DUPLICATE KEY UPDATE") != std::string::npos);
    bool threw = false;
    try { BuildReportInsert(invalid); }
    catch (std::invalid_argument const&) { threw = true; }
    CHECK(threw);
}
