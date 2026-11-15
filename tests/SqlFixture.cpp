#include "persistence/ReportQueue.h"
#include <iostream>
#include <locale>

namespace
{
struct CommaDecimal : std::numpunct<char>
{
    char do_decimal_point() const override { return ','; }
};
}

int main()
{
    using namespace anticheat;
    std::locale::global(std::locale(std::locale(), new CommaDecimal));
    for (unsigned i = 1; i <= 7; ++i)
    {
        ReportRecord report;
        report.eventKey = "sql-test-" + std::to_string(i);
        report.incidentKey = "boot-session-detector-incident";
        report.kind = i == 6 ? ReportKind::Lifecycle : i == 7 ? ReportKind::Alert :
            i == 4 ? ReportKind::Decision : ReportKind::Opened;
        report.guid = i == 5 ? 9 : 7;
        report.account = 8;
        report.name = "O'Brien";
        report.details = "quote' backslash\\ newline\n utf8:\xc3\xa9; DROP TABLE never;";
        if (i == 5)
            report.details = std::string("bad:") + static_cast<char>(0xff);
        report.mapId = 571;
        report.x = 1.5f;
        report.confidence = 0.95f;
        report.score = i == 1 ? 10 : i == 2 ? 30 : i == 3 ? 50 : i == 4 ? 999 : 0;
        report.action = i == 4 ? Action::Kick : Action::None;
        report.createdAt = 86400ull * (i == 3 ? 20001 : 20000);
        std::cout << BuildReportInsert(report) << ";\n";
    }
}
